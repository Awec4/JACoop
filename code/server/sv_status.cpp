/*
===========================================================================
sv_status.cpp -- coop: publish this host's status to a web API.

When sv_statusURL and sv_statusToken are set, a co-op host POSTs a small JSON
snapshot of itself every sv_statusInterval seconds:

    {"token":..., "hostname":..., "map":..., "maxplayers":N,
     "lobby":0/1, "join":"ip:port", "protocol":N,
     "players":[{"name":"...","score":N,"ping":N}]}

plus an "Authorization: Bearer <token>" header, so the same request works
whether the endpoint checks the header or the body field (the AdvJKSite
worker uses the Bearer SYNC_SECRET convention).

so the website's co-op page can show which map is being played and give
visitors a one-click join. The request runs off the game thread (a worker
thread on Windows, a detached curl child elsewhere): a slow or dead endpoint
can never stall a frame.

  sv_statusURL       endpoint ("" = off); defaults to the AdvJKSite push URL
  sv_statusToken     shared secret the endpoint checks ("" = off); the built-in
                     default is the site's COOP_STATUS_TOKEN, so a stock install
                     reports to advjksite.koevetmc.workers.dev out of the box —
                     it only guards a cosmetic status endpoint, and can be
                     rotated (seta sv_statusToken "<new>") if it is abused
  sv_statusInterval  seconds between pushes, clamped to 5..300 (default 20)
  sv_statusJoin      public join address advertised on the page, "ip[:port]"
                     or a host name; defaults to the host's public address,
                     and the site also has its own fallback ("" = rely on that)
===========================================================================
*/

#include "server.h"

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#include <winhttp.h>
	#include <process.h>
#else
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/wait.h>
#endif

static cvar_t	*sv_statusURL;
static cvar_t	*sv_statusToken;
static cvar_t	*sv_statusInterval;
static cvar_t	*sv_statusJoin;

static char		statusLastMap[128];	// map the server ran before the current one
static int		statusMapStart;		// Sys_Milliseconds() when the current map started

#if defined(_WIN32)
static volatile long	statusBusy;	// nonzero while a worker push is in flight
#endif

typedef struct statusPost_s statusPost_t;
static void StatusSendPost( statusPost_t *post );

void SV_StatusInit( void ) {
	sv_statusURL      = Cvar_Get( "sv_statusURL", "https://advjksite.koevetmc.workers.dev/api/coop/ping", CVAR_ARCHIVE );
	sv_statusToken    = Cvar_Get( "sv_statusToken", "93e58253f46639614166c16147311d71dc209bbfa1c2100e", CVAR_ARCHIVE );
	sv_statusInterval = Cvar_Get( "sv_statusInterval", "20", CVAR_ARCHIVE );
	sv_statusJoin     = Cvar_Get( "sv_statusJoin", "76.176.218.251:29070", CVAR_ARCHIVE );
	// game -> engine death report channel: the game writes
	// "seq|victim|killer" on each player death; StatusCheckDeath polls it.
	Cvar_Get( "sv_coopDeath", "", CVAR_TEMP );
}

/*
================
StatusJsonEscape

Escapes " and \ and drops control chars so a cvar value is safe inside a
JSON string. Writes at most dstSize-1 chars plus the NUL.
================
*/
static void StatusJsonEscape( char *dst, size_t dstSize, const char *src ) {
	size_t n = 0;
	for ( ; src && *src && n + 2 < dstSize; src++ ) {
		unsigned char c = (unsigned char)*src;
		if ( c == '"' || c == '\\' ) {
			dst[n++] = '\\';
			dst[n++] = (char)c;
		} else if ( c >= 32 ) {
			dst[n++] = (char)c;
		}
	}
	dst[n] = 0;
}

/*
================
StatusHeaderToken

Copies the token for HTTP header use: printable chars only, no spaces or
quotes, so it can go inside "Authorization: Bearer <token>".
================
*/
static void StatusHeaderToken( char *dst, size_t dstSize, const char *src ) {
	size_t n = 0;
	for ( ; src && *src && n + 1 < dstSize; src++ ) {
		unsigned char c = (unsigned char)*src;
		if ( c > 32 && c < 127 && c != '"' ) {
			dst[n++] = (char)c;
		}
	}
	dst[n] = 0;
}

typedef struct statusPost_s {
	char	url[512];
	char	token[256];
	char	body[4096];
} statusPost_t;

/*
================
StatusBuildRoster

Fills dst with [{"name":...,"score":N,"ping":N}, ...] for the site's
player list — same fields the getstatus reply in sv_main.cpp reports.
================
*/
/*
================
StatusAccountFields

Returns `"account":"x","key":"y"` (with the leading comma) when the
client set advjk_account + advjk_key — the site verifies the pair to
credit runs to real accounts instead of display names. "" when unset.
================
*/
static const char *StatusAccountFields( client_t *cl ) {
	static char	buf[160];
	const char	*acct, *key;
	char		escA[64], escK[64];

	if ( !cl->userinfo[0] ) {
		return "";
	}
	acct = Info_ValueForKey( cl->userinfo, "advjk_account" );
	key  = Info_ValueForKey( cl->userinfo, "advjk_key" );
	if ( !acct[0] || !key[0] ) {
		return "";
	}
	StatusJsonEscape( escA, sizeof( escA ), acct );
	StatusJsonEscape( escK, sizeof( escK ), key );
	Com_sprintf( buf, sizeof( buf ), ",\"account\":\"%s\",\"key\":\"%s\"", escA, escK );
	return buf;
}

static void StatusBuildRoster( char *dst, size_t dstSize, qboolean withKeys ) {
	int		i, off = 0;

	dst[0] = 0;
	if ( !svs.clients ) {
		return;
	}
	for ( i = 0; i < MAX_CLIENTS && off < (int)dstSize - 96; i++ ) {
		client_t	*cl = &svs.clients[i];
		char		escName[MAX_NAME_LENGTH * 2];
		int			score = 0;

		if ( cl->state < CS_CONNECTED ) {
			continue;
		}
		if ( cl->gentity && cl->gentity->client ) {
			score = cl->gentity->client->persistant[PERS_SCORE];
		}
		StatusJsonEscape( escName, sizeof( escName ), cl->name );
		// keys only travel in the authenticated run/death posts — the
		// public status roster must never leak them.
		off += Com_sprintf( dst + off, dstSize - off,
			"%s{\"name\":\"%s\",\"score\":%i,\"ping\":%i%s}",
			off ? "," : "", escName, score, cl->ping,
			withKeys ? StatusAccountFields( cl ) : "" );
	}
	dst[dstSize - 1] = 0;
}

/*
================
StatusTimeURL

Derives the map-time endpoint next to sv_statusURL:
.../api/coop/ping or .../api/coop/status -> .../api/coop/time.
A custom URL that ends in neither is used as-is.
================
*/
static void StatusEndpointURL( char *dst, size_t dstSize, const char *src, const char *endpoint ) {
	const char *slash = strrchr( src, '/' );
	if ( slash && ( !Q_stricmp( slash, "/ping" ) || !Q_stricmp( slash, "/status" ) ) ) {
		Com_sprintf( dst, dstSize, "%.*s/%s", (int)( slash - src ), src, endpoint );
	} else {
		Q_strncpyz( dst, src, dstSize );
	}
}

/*
================
StatusFire

Builds and sends one JSON post to a coop endpoint ("time", "attempt",
"death"). `fields` is everything after the token key, e.g.
\"map\":\"yavin1\",\"seconds\":743. Off-thread like the status push.
================
*/
static void StatusFire( const char *endpoint, const char *fields ) {
	char	token[256], url[512];
	statusPost_t	*post;

	StatusJsonEscape( token, sizeof( token ), sv_statusToken->string );
	StatusEndpointURL( url, sizeof( url ), sv_statusURL->string, endpoint );

	post = (statusPost_t *)malloc( sizeof( *post ) );
	if ( !post ) {
		return;
	}
	Q_strncpyz( post->url, url, sizeof( post->url ) );
	StatusHeaderToken( post->token, sizeof( post->token ), sv_statusToken->string );
	Com_sprintf( post->body, sizeof( post->body ), "{\"token\":\"%s\",%s}", token, fields );
	StatusSendPost( post );
}

/*
================
StatusBuildPost

Runs on the game thread: snapshots every cvar and the client list into a
self-contained request the worker can send without touching engine state.
================
*/
static statusPost_t *StatusBuildPost( void ) {
	char	token[256], hostname[128], map[128], join[256];
	char	playersJson[1600];
	int		maxcl, lobby, elapsed = 0;
	statusPost_t	*post;

	StatusJsonEscape( token, sizeof( token ), sv_statusToken->string );
	StatusJsonEscape( hostname, sizeof( hostname ), sv_hostname ? sv_hostname->string : "" );
	StatusJsonEscape( map, sizeof( map ), sv_mapname ? sv_mapname->string : "" );
	StatusJsonEscape( join, sizeof( join ), sv_statusJoin->string );

	StatusBuildRoster( playersJson, sizeof( playersJson ), qfalse );
	if ( statusMapStart > 0 ) {
		elapsed = ( Sys_Milliseconds() - statusMapStart ) / 1000;
	}
	maxcl = sv_maxclients ? sv_maxclients->integer : MAX_CLIENTS;
	lobby = Cvar_VariableIntegerValue( "g_coopLobby" ) ? 1 : 0;

	post = (statusPost_t *)malloc( sizeof( *post ) );
	if ( !post ) {
		return NULL;
	}
	Q_strncpyz( post->url, sv_statusURL->string, sizeof( post->url ) );
	StatusHeaderToken( post->token, sizeof( post->token ), sv_statusToken->string );
	Com_sprintf( post->body, sizeof( post->body ),
		"{\"token\":\"%s\",\"hostname\":\"%s\",\"map\":\"%s\","
		"\"players\":[%s],\"maxplayers\":%i,\"lobby\":%i,\"elapsed\":%i,"
		"\"needpass\":%i,\"join\":\"%s\",\"protocol\":%i}",
		token, hostname, map, playersJson, maxcl, lobby, elapsed,
		( sv_password && sv_password->string[0] ) ? 1 : 0, join, PROTOCOL_VERSION );
	return post;
}

/*
================
StatusPostMapTime

Reports a completed map's run time to the .../time endpoint. Called when
the server leaves statusLastMap for a different map; same-map reloads
(death restarts) never reach here. Sent off-thread like the status push.
================
*/
static void StatusPostMapTime( const char *mapname, int seconds ) {
	char	hostname[128], map[128], playersJson[1600], fields[2048];

	if ( seconds < 10 ) {
		return;		// map hops shorter than this are console/dev hops, not runs
	}

	StatusJsonEscape( hostname, sizeof( hostname ), sv_hostname ? sv_hostname->string : "" );
	StatusJsonEscape( map, sizeof( map ), mapname );
	StatusBuildRoster( playersJson, sizeof( playersJson ), qtrue );

	Com_sprintf( fields, sizeof( fields ),
		"\"map\":\"%s\",\"seconds\":%i,\"hostname\":\"%s\",\"players\":[%s]",
		map, seconds, hostname, playersJson );
	StatusFire( "time", fields );
}

/*
================
StatusPostAttempt

Counts a map start as a run attempt — same-map restarts included, since a
death reload IS a fresh try. Feeds the site's attempts-vs-clears stat.
================
*/
static void StatusPostAttempt( const char *mapname ) {
	char	map[128], fields[256];

	StatusJsonEscape( map, sizeof( map ), mapname );
	Com_sprintf( fields, sizeof( fields ), "\"map\":\"%s\"", map );
	StatusFire( "attempt", fields );
}

/*
================
StatusCheckDeath

The game reports each player death through the sv_coopDeath cvar
("seq|victim|killer" — pipes can't appear in names). A change means a
fresh death: report it. Called every frame before the interval gate so
deaths post promptly instead of waiting for the next status push.
================
*/
static void StatusCheckDeath( void ) {
	static char	lastDeath[192];
	char		death[192], victim[128], killer[128], map[128],
			account[128], vkey[128], fields[640];
	char		*v, *k, *a, *kk;
	const char	*d = Cvar_VariableString( "sv_coopDeath" );

	if ( !d[0] || !strcmp( d, lastDeath ) ) {
		return;
	}
	Q_strncpyz( lastDeath, d, sizeof( lastDeath ) );

	Q_strncpyz( death, d, sizeof( death ) );
	v = strchr( death, '|' );
	if ( !v ) return;
	*v++ = 0;
	k = strchr( v, '|' );
	if ( !k ) return;
	*k++ = 0;
	// death now holds the seq (unused), v = victim, k = killer;
	// optional "|account|key" tail links the victim to a site account
	account[0] = vkey[0] = 0;
	a = strchr( k, '|' );
	if ( a ) {
		*a++ = 0;
		kk = strchr( a, '|' );
		if ( kk ) {
			*kk++ = 0;
			Q_strncpyz( vkey, kk, sizeof( vkey ) );
		}
		Q_strncpyz( account, a, sizeof( account ) );
	}

	StatusJsonEscape( victim, sizeof( victim ), v );
	StatusJsonEscape( killer, sizeof( killer ), k );
	StatusJsonEscape( map, sizeof( map ), sv_mapname ? sv_mapname->string : "" );
	{
		char	escA[128], escK[128], acctFields[320];
		acctFields[0] = 0;
		if ( account[0] && vkey[0] ) {
			StatusJsonEscape( escA, sizeof( escA ), account );
			StatusJsonEscape( escK, sizeof( escK ), vkey );
			Com_sprintf( acctFields, sizeof( acctFields ),
				",\"account\":\"%s\",\"key\":\"%s\"", escA, escK );
		}
		Com_sprintf( fields, sizeof( fields ),
			"\"victim\":\"%s\",\"killer\":\"%s\",\"map\":\"%s\"%s",
			victim, killer, map, acctFields );
	}
	StatusFire( "death", fields );
}

/*
================
SV_StatusMapStarted

Called from SV_SpawnServer once the new map name is known. A change to a
different map reports the previous map's elapsed; a same-map restart just
restarts the clock.
================
*/
void SV_StatusMapStarted( const char *mapname ) {
	if ( !com_sv_running || !com_sv_running->integer || !Cvar_VariableIntegerValue( "net_enabled" ) ) {
		return;		// solo hosts never report
	}
	if ( statusMapStart > 0 && statusLastMap[0] && Q_stricmp( statusLastMap, mapname ) != 0 ) {
		StatusPostMapTime( statusLastMap, ( Sys_Milliseconds() - statusMapStart ) / 1000 );
	}
	Q_strncpyz( statusLastMap, mapname, sizeof( statusLastMap ) );
	statusMapStart = Sys_Milliseconds();
	StatusPostAttempt( mapname );
}

#if defined(_WIN32)

static void StatusWide( wchar_t *dst, size_t dstLen, const char *src ) {
	int n = MultiByteToWideChar( CP_UTF8, 0, src, -1, NULL, 0 );
	if ( n <= 0 || (size_t)n > dstLen ) {
		dst[0] = 0;
		return;
	}
	MultiByteToWideChar( CP_UTF8, 0, src, -1, dst, (int)dstLen );
}

/*
================
StatusPostThread

Detached worker: one WinHTTP session per push, ~4s timeouts, response ignored.
Clears statusBusy right before it exits.
================
*/
static unsigned int __stdcall StatusPostThread( void *arg ) {
	statusPost_t	*post = (statusPost_t *)arg;
	wchar_t			wurl[sizeof( post->url )];
	wchar_t			host[256], path[1024];
	URL_COMPONENTSW	uc;
	HINTERNET		session = NULL, connect = NULL, request = NULL;
	DWORD			flags;

	StatusWide( wurl, sizeof( wurl ) / sizeof( wurl[0] ), post->url );
	memset( &uc, 0, sizeof( uc ) );
	uc.dwStructSize = sizeof( uc );
	uc.lpszHostName = host;  uc.dwHostNameLength = sizeof( host ) / sizeof( host[0] );
	uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = sizeof( path ) / sizeof( path[0] );
	if ( !WinHttpCrackUrl( wurl, 0, 0, &uc ) ) {
		goto done;
	}
	if ( !uc.dwUrlPathLength ) {
		path[0] = L'/'; path[1] = 0;
	}
	flags = WINHTTP_FLAG_SECURE;
	if ( !_strnicmp( post->url, "http:", 5 ) ) {
		flags = 0;
	} else if ( _strnicmp( post->url, "https:", 6 ) ) {
		goto done;	// only http(s) endpoints make sense
	}

	session = WinHttpOpen( L"AdvancedJK-Coop", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( !session ) {
		goto done;
	}
	connect = WinHttpConnect( session, host, uc.nPort, 0 );
	if ( !connect ) {
		goto done;
	}
	request = WinHttpOpenRequest( connect, L"POST", path, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags );
	if ( !request ) {
		goto done;
	}
	WinHttpSetTimeouts( request, 4000, 4000, 4000, 4000 );
	{
		wchar_t headers[768], wtoken[sizeof( post->token )];
		StatusWide( wtoken, sizeof( wtoken ) / sizeof( wtoken[0] ), post->token );
		swprintf( headers, sizeof( headers ) / sizeof( headers[0] ),
			L"Content-Type: application/json\r\nAuthorization: Bearer %s\r\n",
			wtoken );
		WinHttpSendRequest( request, headers, (DWORD)-1L,
			post->body, (DWORD)strlen( post->body ), (DWORD)strlen( post->body ), 0 );
	}
	WinHttpReceiveResponse( request, NULL );

done:
	if ( request ) WinHttpCloseHandle( request );
	if ( connect ) WinHttpCloseHandle( connect );
	if ( session ) WinHttpCloseHandle( session );
	free( post );
	InterlockedExchange( &statusBusy, 0 );
	return 0;
}

#endif

/*
================
StatusSendPost

Fires a prepared post off the game thread — WinHTTP worker on Windows,
detached curl child elsewhere. Frees post in every case; drops the
request if a push is still in flight (Windows).
================
*/
static void StatusSendPost( statusPost_t *post ) {
#if defined(_WIN32)
	if ( InterlockedCompareExchange( &statusBusy, 1, 0 ) != 0 ) {
		free( post );	// previous push still in flight, skip this one
		return;
	}
	{
		uintptr_t h = _beginthreadex( NULL, 0, StatusPostThread, post, 0, NULL );
		if ( !h ) {
			free( post );
			InterlockedExchange( &statusBusy, 0 );
		} else {
			CloseHandle( (HANDLE)h );
		}
	}
#else
	// no HTTP stack in the tree on POSIX: hand the request to curl and let it
	// die on its own (-m bounds it). Double-fork so no zombie piles up.
	{
		pid_t pid = fork();
		if ( pid == 0 ) {
			if ( fork() > 0 ) {
				_exit( 0 );
			}
			{
				char authHdr[320];
				snprintf( authHdr, sizeof( authHdr ),
					"Authorization: Bearer %s", post->token );
				execlp( "curl", "curl", "-s", "-o", "/dev/null", "-m", "5",
					"-X", "POST", "-H", "Content-Type: application/json",
					"-H", authHdr,
					"--data-binary", post->body, post->url, (char *)NULL );
			}
			_exit( 0 );
		}
		if ( pid > 0 ) {
			waitpid( pid, NULL, 0 );	// first child exits immediately
		}
		free( post );
	}
#endif
}

/*
================
SV_StatusFrame

Called from SV_Frame on every engine frame, so it keeps ticking while the
host is in the lobby, watching a cutscene or paused by cl_paused.
================
*/
void SV_StatusFrame( void ) {
	static int		lastPush = 0;
	int				now, interval;
	statusPost_t	*post;

	if ( !com_sv_running || !com_sv_running->integer ) {
		return;
	}
	if ( !Cvar_VariableIntegerValue( "net_enabled" ) ) {
		return;		// solo run: nothing to advertise, nobody could join anyway
	}
	if ( !sv_statusURL || !sv_statusURL->string[0] || !sv_statusToken || !sv_statusToken->string[0] ) {
		return;
	}

	StatusCheckDeath();

	interval = sv_statusInterval->integer;
	if ( interval < 5 )   interval = 5;
	if ( interval > 300 ) interval = 300;
	now = Sys_Milliseconds();
	if ( now - lastPush < interval * 1000 ) {
		return;
	}
	lastPush = now;

	post = StatusBuildPost();
	if ( !post ) {
		return;
	}
	StatusSendPost( post );
}
