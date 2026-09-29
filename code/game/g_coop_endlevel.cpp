/*
===========================================================================
Copyright (C) 2026, JACoop contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// g_coop_endlevel.cpp -- end of a co-op mission, seen by everybody.
//
// Stock: a script ends the level through G_ChangeMap with a "+menuname" and
// the host alone gets the debrief, the galaxy map, the force and the weapon
// screens (g_utils.cpp, ui/ingamemissionselect*.menu). A joiner saw nothing:
// the world kept running behind the host's fullscreen menu, and if the level
// script had faded the screen out it stayed black until the map changed.
//
// Here the host takes over that "+menu" when at least one other player is
// connected and drives four phases for everybody at once, over the existing
// "coopmenu <name>" server command:
//
//   D  debrief   ui/coopdebrief.menu   the mission that just ended + its stats
//   V  vote      ui/coopvote.menu      one vote per player on the next mission
//   L  loadout   ui/cooploadout.menu   force points, weapons, then PRET
//   W  wait      ui/coopdebrief.menu   no mission to choose here: the host is
//                                      on the stock screens, the others wait
//
// The state everybody reads is published in CS_COOP_ENDLEVEL:
//   "<phase>\t<title>\t<stats>\t<info>|<map>\t<label>\t<votes>\t<voters>|..."
// The first record is the header, the next ones are the missions to vote for
// (feeder 0x1c, CL_CoopEndRows in client/cl_main.cpp). The players answer with
// the client commands coop_endnext / coop_endvote / coop_endwpn / coop_endready,
// and the host may force each step (coop_endgo).
//
// Solo, or a host with nobody connected, never enters any of this: G_ChangeMap
// opens the stock menu exactly as before.

#include "../cgame/cg_local.h"	// cgi_SP_GetStringTextString: the missions' own titles, in the game's language
#include "g_local.h"
#include "g_items.h"
#include "wp_saber.h"

extern void G_AddWeaponModels( gentity_t *ent );

#define COOPEND_MAX_MISSIONS	8
#define COOPEND_DEBRIEF_TIMEOUT	120000	// ms: nobody must ever be stuck in front of a panel...
#define COOPEND_VOTE_TIMEOUT	60000	// ms: ...a player who never answers is simply ignored
#define COOPEND_START_DELAY		100		// ms: target_level_change sets tier_storyinfo and the stats AFTER calling G_ChangeMap

typedef struct coopEndMission_s {
	char		map[MAX_QPATH];
	char		label[64];
	qboolean	story;	// "back to the academy": no loadout, straight there
} coopEndMission_t;

// the campaign's three hubs, in the order of ui/ingamemissionselect?.menu
typedef struct coopEndTierEntry_s {
	const char	*map;
	const char	*ref;	// string reference of the title, MENUS package
} coopEndTierEntry_t;

static const coopEndTierEntry_t coopEndTier1[] = {
	{ "t1_sour",		"MENUS_T1_SOUR_TITLE" },
	{ "t1_surprise",	"MENUS_T1_SURPRISE_TITLE" },
	{ "t1_fatal",		"MENUS_T1_FATAL_TITLE" },
	{ "t1_danger",		"MENUS_T1_DANGER_TITLE" },
	{ "t1_rail",		"MENUS_T1_RAIL_TITLE" },
	{ NULL, NULL }
};
static const coopEndTierEntry_t coopEndTier2[] = {
	{ "t2_rancor",		"MENUS_T2_RANCOR_TITLE" },
	{ "t2_trip",		"MENUS_T2_TRIP_TITLE" },
	{ "t2_wedge",		"MENUS_T2_WEDGE_TITLE" },
	{ "t2_rogue",		"MENUS_T2_ROGUE_TITLE" },
	{ "t2_dpred",		"MENUS_T2_DPRED_TITLE" },
	{ NULL, NULL }
};
static const coopEndTierEntry_t coopEndTier3[] = {
	{ "t3_rift",		"MENUS_T3_RIFT_TITLE" },
	{ "t3_stamp",		"MENUS_T3_STAMPEDE_TITLE" },
	{ "t3_hevil",		"MENUS_T3_HEVIL_TITLE" },
	{ "t3_byss",		"MENUS_T3_BYSS_TITLE" },
	{ "t3_bounty",		"MENUS_T3_BOUNTY_TITLE" },
	{ NULL, NULL }
};

// what ui/ingamewpnselect.menu hands out with each weapon (addweaponselection
// "<weapon>" "<ammo>" "<amount>"): the host's own screen does it through the
// playerState, a joiner only sends us its three choices.
typedef struct coopEndWeapon_s {
	int	weapon;
	int	ammoIndex;
	int	ammoAmount;
} coopEndWeapon_t;

static const coopEndWeapon_t coopEndWeapons[] = {
	{ WP_BLASTER,			AMMO_BLASTER,		300 },
	{ WP_DISRUPTOR,			AMMO_POWERCELL,		300 },
	{ WP_BOWCASTER,			AMMO_POWERCELL,		300 },
	{ WP_DEMP2,				AMMO_POWERCELL,		300 },
	{ WP_REPEATER,			AMMO_METAL_BOLTS,	400 },
	{ WP_FLECHETTE,			AMMO_METAL_BOLTS,	400 },
	{ WP_CONCUSSION,		AMMO_METAL_BOLTS,	400 },
	{ WP_ROCKET_LAUNCHER,	AMMO_ROCKETS,		10 },
	{ WP_THERMAL,			AMMO_THERMAL,		10 },
	{ WP_TRIP_MINE,			AMMO_TRIPMINE,		5 },
	{ WP_DET_PACK,			AMMO_DETPACK,		5 },
	{ WP_NONE, 0, 0 }
};

static coopEndPhase_t		coopEndPhase = COOPEND_NONE;
static char					coopEndStockMenu[64];		// the "+menu" the stock game would have opened
static int					coopEndStartTime;			// level.time the flow starts (0 = nothing pending)
static int					coopEndPhaseTime;			// level.time the current phase began
static qboolean				coopEndLaunched;			// the map command went out
static qboolean				coopEndWaitAcked;			// the host pressed PASSER once in the wait phase
static char					coopEndMap[MAX_QPATH];		// the mission finally chosen
static char					coopEndTitle[64];			// mission that ended, then the one chosen
static char					coopEndStats[160];
static char					coopEndInfo[128];
static coopEndMission_t		coopEndMissions[COOPEND_MAX_MISSIONS];
static int					coopEndNumMissions;
static qboolean				coopEndNextDone[MAX_CLIENTS];	// left the debrief
static int					coopEndVotes[MAX_CLIENTS];		// -1 = has not voted
static qboolean				coopEndReady[MAX_CLIENTS];
static qboolean				coopEndGodmode[MAX_CLIENTS];	// we set FL_GODMODE on that player

/*
==============================================================================
Helpers
==============================================================================
*/
qboolean G_CoopEndLevelActive( void )
{
	return (qboolean)( coopEndPhase != COOPEND_NONE );
}

// the flow owns the screens: PRET / choix d'equipement instead of the lobby's ready flag
int G_CoopEndLevelReadyState( int slot )
{
	if ( coopEndPhase != COOPEND_LOADOUT || slot < 0 || slot >= MAX_CLIENTS )
	{
		return -1;
	}
	return coopEndReady[slot] ? 1 : 0;
}

// title of a campaign mission, in the game's own language ("Sources of Power"),
// or the map name when the string package does not know it
static void G_CoopEndMissionTitle( const char *map, char *out, int outSize )
{
	const coopEndTierEntry_t *tiers[3] = { coopEndTier1, coopEndTier2, coopEndTier3 };

	Q_strncpyz( out, map, outSize );
	for ( int t = 0; t < 3; t++ )
	{
		for ( const coopEndTierEntry_t *e = tiers[t]; e->map; e++ )
		{
			if ( !Q_stricmp( e->map, map ) )
			{
				Com_sprintf( out, outSize, "@%s", e->ref );	// coop: each client reads it in its language
				return;
			}
		}
	}
}

// What the stock screens do after the "+menu" a level asked for, read from
// the menu name and tier_storyinfo exactly like the retail menus do:
//
//   ingameMissionSelect (debrief after a hub mission, tier_storyinfo already ++'d)
//     2 4        choose a hub 1 mission        3   t1_inter (interlude)
//     5          the last hub 1 mission, or back to the academy (academy2)
//     6          academy2 (hub 1 all done)
//     8 9 10     choose a hub 2 mission        11  last one, or academy4
//     12         academy4 -> vjun1             (all five done)
//     14 15 16   choose a hub 3 mission        17  last one, or academy6
//     18         academy6 -> taspir1
//   ingameGotoTier       1 / 7 / 13: open hub 1 / 2 / 3 (yavin2, academy3, academy5)
//   ingameMissionSelectN hub N (t1_inter ends there)
//   ingameWpnSelect      academy2/4/6: weapons, then hoth2 / vjun1 / taspir1
//
// Anything else (the saber menus of academy3/5...) stays the host's stock menu.
// The first version only looked at tier_storyinfo ranges: 12 fell in hub 3's
// and the vote skipped academy4 and the whole of Vjun (a player's review, 29/09),
// 3 fell in hub 1's and skipped t1_inter.
typedef enum
{
	COOPNEXT_STOCK,		// not ours: the host's stock menu, the others wait
	COOPNEXT_CHOOSE,	// vote on the hub missions left, then the loadout
	COOPNEXT_STORY,		// straight to a story map, no loadout
	COOPNEXT_ARMORY		// the loadout (weapons), then a story map
} coopEndNext_t;

static coopEndNext_t			coopEndNextKind = COOPNEXT_STOCK;
static const coopEndTierEntry_t	*coopEndNextTier;		// COOPNEXT_CHOOSE: the hub
static const char				*coopEndNextStory;		// the story map: STORY / ARMORY, or the "academy" choice of CHOOSE
static qboolean					coopEndForcePoint;		// this loadout follows a mission choice: one force point to spend

static coopEndNext_t G_CoopEndNextStep( void )
{
	static const coopEndTierEntry_t *hubs[3] = { coopEndTier1, coopEndTier2, coopEndTier3 };
	static const char *academies[3] = { "academy2", "academy4", "academy6" };
	const int story = gi.Cvar_VariableIntegerValue( "tier_storyinfo" );

	coopEndNextTier = NULL;
	coopEndNextStory = NULL;

	if ( !Q_stricmp( coopEndStockMenu, "ingameMissionSelect" ) )
	{
		if ( story == 3 )
		{
			coopEndNextStory = "t1_inter";
			return COOPNEXT_STORY;
		}
		for ( int h = 0; h < 3; h++ )
		{
			const int base = h * 6;		// 2..6, 8..12, 14..18
			if ( story < base + 2 || story > base + 6 )
			{
				continue;
			}
			if ( story == base + 6 )
			{	// the five missions are done
				coopEndNextStory = academies[h];
				return COOPNEXT_STORY;
			}
			coopEndNextTier = hubs[h];
			if ( story == base + 5 )
			{	// four done: the fifth, or the academy
				coopEndNextStory = academies[h];
			}
			return COOPNEXT_CHOOSE;
		}
		return COOPNEXT_STOCK;
	}
	if ( !Q_stricmp( coopEndStockMenu, "ingameGotoTier" ) )
	{	// same ranges as UI_LoadMissionSelectMenu (ui/ui_main.cpp)
		if ( story > 0 && story < 5 )
		{
			coopEndNextTier = coopEndTier1;
		}
		else if ( story > 6 && story < 10 )
		{
			coopEndNextTier = coopEndTier2;
		}
		else if ( story > 11 && story < 15 )
		{
			coopEndNextTier = coopEndTier3;
		}
		return coopEndNextTier ? COOPNEXT_CHOOSE : COOPNEXT_STOCK;
	}
	if ( !Q_stricmpn( coopEndStockMenu, "ingameMissionSelect", 19 ) && coopEndStockMenu[19] >= '1' && coopEndStockMenu[19] <= '3' )
	{
		coopEndNextTier = hubs[coopEndStockMenu[19] - '1'];
		return COOPNEXT_CHOOSE;
	}
	if ( !Q_stricmp( coopEndStockMenu, "ingameWpnSelect" ) )
	{	// ui/ingamewpnselect.menu: weapon_menu 1/2/3 begins hoth2 / vjun1 / taspir1
		static const char *next[3] = { "hoth2", "vjun1", "taspir1" };
		const int wm = gi.Cvar_VariableIntegerValue( "weapon_menu" );
		if ( wm >= 1 && wm <= 3 )
		{
			coopEndNextStory = next[wm - 1];
			return COOPNEXT_ARMORY;
		}
	}
	return COOPNEXT_STOCK;
}

// the missions of the current hub that are not done yet (cvar tiers_complete
// holds the maps already played, exactly like the galaxy menu's disablecvar),
// plus "back to the academy" where the stock debrief offers it
static void G_CoopEndBuildMissions( const coopEndTierEntry_t *tier, const char *academy )
{
	char done[MAX_STRING_CHARS];

	coopEndNumMissions = 0;
	if ( !tier )
	{
		return;
	}
	gi.Cvar_VariableStringBuffer( "tiers_complete", done, sizeof( done ) );
	for ( const coopEndTierEntry_t *e = tier; e->map && coopEndNumMissions < COOPEND_MAX_MISSIONS; e++ )
	{
		if ( done[0] && strstr( done, e->map ) )
		{
			continue;	// already played
		}
		coopEndMission_t *m = &coopEndMissions[coopEndNumMissions++];
		Q_strncpyz( m->map, e->map, sizeof( m->map ) );
		G_CoopEndMissionTitle( e->map, m->label, sizeof( m->label ) );
		m->story = qfalse;
	}
	if ( academy && coopEndNumMissions < COOPEND_MAX_MISSIONS )
	{
		coopEndMission_t *m = &coopEndMissions[coopEndNumMissions++];
		Q_strncpyz( m->map, academy, sizeof( m->map ) );
		Q_strncpyz( m->label, "@MENUS_RETURN_TO_ACADEMY", sizeof( m->label ) );
		m->story = qtrue;
	}
}

// open the same menu on every machine (the host included) and make sure
// nobody is left under a black screen the level script faded in
static void G_CoopEndOpenMenu( const char *menu )
{
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = G_CoopPlayerSlot( i );
		if ( !ent )
		{
			continue;
		}
		G_CoopFadeInPlayer( ent, 500 );
		gi.SendServerCommand( i, "coopmenu %s", menu );
	}
}

// the world keeps running under the menus: nobody may die while choosing
static void G_CoopEndProtectPlayers( qboolean on )
{
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = G_CoopPlayerSlot( i );
		if ( !ent )
		{
			coopEndGodmode[i] = qfalse;
			continue;
		}
		if ( on && !coopEndGodmode[i] )
		{
			coopEndGodmode[i] = qtrue;
			ent->flags |= ( FL_GODMODE | FL_NOTARGET );
		}
		else if ( !on && coopEndGodmode[i] )
		{
			coopEndGodmode[i] = qfalse;
			ent->flags &= ~( FL_GODMODE | FL_NOTARGET );
		}
	}
}

/*
==============================================================================
CS_COOP_ENDLEVEL
==============================================================================
*/
static char G_CoopEndPhaseChar( void )
{
	switch ( coopEndPhase )
	{
	case COOPEND_DEBRIEF:	return 'D';
	case COOPEND_VOTE:		return 'V';
	case COOPEND_LOADOUT:	return 'L';
	case COOPEND_WAIT:		return 'W';
	default:				return 'N';
	}
}

static void G_CoopEndPublish( void )
{
	static char	last[MAX_STRING_CHARS];
	char		now[MAX_STRING_CHARS];

	// 5th field: 1 when this loadout gives a force point (a mission was chosen;
	// not before the academy's story maps, where the stock game only arms you)
	Com_sprintf( now, sizeof( now ), "%c\t%s\t%s\t%s\t%i", G_CoopEndPhaseChar(), coopEndTitle, coopEndStats, coopEndInfo,
		( coopEndPhase == COOPEND_LOADOUT && coopEndForcePoint ) ? 1 : 0 );
	if ( coopEndPhase == COOPEND_VOTE )
	{
		for ( int m = 0; m < coopEndNumMissions; m++ )
		{
			char voters[MAX_STRING_CHARS];
			int	 count = 0;

			voters[0] = '\0';
			for ( int i = 0; i < MAX_CLIENTS; i++ )
			{
				const gentity_t *ent = G_CoopPlayerSlot( i );
				if ( !ent || coopEndVotes[i] != m )
				{
					continue;
				}
				count++;
				Q_strcat( voters, sizeof( voters ), va( "%s%.12s", voters[0] ? ", " : "", ent->client->pers.netname ) );	// coop: 16 voters in one configstring
			}
			Q_strcat( now, sizeof( now ), va( "|%s\t%s\t%i\t%s", coopEndMissions[m].map, coopEndMissions[m].label, count, voters ) );
		}
	}
	if ( strcmp( now, last ) )
	{
		Q_strncpyz( last, now, sizeof( last ) );
		gi.SetConfigstring( CS_COOP_ENDLEVEL, now );
	}
}

/*
==============================================================================
Phases
==============================================================================
*/
static void G_CoopEndLaunch( void )
{
	if ( coopEndLaunched )
	{
		return;
	}
	coopEndLaunched = qtrue;
	G_CoopEndProtectPlayers( qfalse );
	G_CoopStoreAll();			// each joiner's loadout follows it into the next map
	coopEndPhase = COOPEND_NONE;
	gi.SetConfigstring( CS_COOP_ENDLEVEL, "" );
	gi.SendServerCommand( -1, "coopmenu closeall" );
	gi.Printf( "coop: end of mission, everybody is ready -> %s\n", coopEndMap );
	gi.cvar_set( "tier_mapname", va( "maptransition %s", coopEndMap ) );
	gi.SendConsoleCommand( va( "maptransition %s\n", coopEndMap ) );
}

static void G_CoopEndStartLoadout( const char *map, const char *label, qboolean story )
{
	Q_strncpyz( coopEndMap, map, sizeof( coopEndMap ) );
	Q_strncpyz( coopEndTitle, label, sizeof( coopEndTitle ) );
	if ( story )
	{	// a story map (academy, interlude): the stock screens go there directly
		gi.Printf( "coop: next is the story map %s\n", coopEndMap );
		G_CoopEndLaunch();
		return;
	}
	memset( coopEndReady, 0, sizeof( coopEndReady ) );
	if ( coopEndForcePoint )
	{	// the stock ingameForceSelect of this step: one point each, joiners
		// included, whether or not the host has spent its own yet
		G_CoopForceGrantPoint();
	}
	coopEndPhase = COOPEND_LOADOUT;
	coopEndPhaseTime = level.time;
	gi.Printf( "coop: next mission is %s (%s)\n", coopEndMap, coopEndTitle );
	// "@@REF" : the mission title in the language of whoever reads it (CG_Print_f)
	G_CoopPrintTr( -1, "print", "^2Prochaine mission : %s%s\n", "^2Next mission: %s%s\n", coopEndTitle[0] == '@' ? "@" : "", coopEndTitle );
	G_CoopEndOpenMenu( "coopLoadout" );
}

// majority wins; a tie goes to the mission the host voted for, else to the
// first of the tied ones (the galaxy order)
static void G_CoopEndTally( void )
{
	int counts[COOPEND_MAX_MISSIONS];

	memset( counts, 0, sizeof( counts ) );
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( G_CoopPlayerSlot( i ) && coopEndVotes[i] >= 0 && coopEndVotes[i] < coopEndNumMissions )
		{
			counts[coopEndVotes[i]]++;
		}
	}
	int best = -1;
	for ( int m = 0; m < coopEndNumMissions; m++ )
	{
		if ( best < 0 || counts[m] > counts[best] )
		{
			best = m;
		}
	}
	if ( best < 0 )
	{
		best = 0;	// nobody voted at all: the galaxy order decides
	}
	else if ( coopEndVotes[0] >= 0 && coopEndVotes[0] < coopEndNumMissions && counts[coopEndVotes[0]] == counts[best] )
	{
		best = coopEndVotes[0];	// tie: the host's vote
	}
	coopEndForcePoint = (qboolean)!coopEndMissions[best].story;
	G_CoopEndStartLoadout( coopEndMissions[best].map, coopEndMissions[best].label, coopEndMissions[best].story );
}

// everybody left the debrief: vote, straight to the loadout when there is
// nothing to choose, straight to a story map, or hand the host back its stock
// screens
static void G_CoopEndAfterDebrief( void )
{
	if ( coopEndNumMissions == 0 && coopEndNextStory )
	{	// no choice here (academy2/4/6 when a hub is over, t1_inter, hoth2/vjun1/taspir1)
		char label[64];
		G_CoopEndMissionTitle( coopEndNextStory, label, sizeof( label ) );
		coopEndForcePoint = qfalse;
		G_CoopEndStartLoadout( coopEndNextStory, label, (qboolean)( coopEndNextKind == COOPNEXT_STORY ) );
		return;
	}
	if ( coopEndNumMissions >= 2 )
	{
		for ( int i = 0; i < MAX_CLIENTS; i++ )
		{
			coopEndVotes[i] = -1;
		}
		coopEndPhase = COOPEND_VOTE;
		coopEndPhaseTime = level.time;
		G_CoopEndOpenMenu( "coopVote" );
		return;
	}
	if ( coopEndNumMissions == 1 )
	{
		coopEndForcePoint = (qboolean)!coopEndMissions[0].story;
		G_CoopEndStartLoadout( coopEndMissions[0].map, coopEndMissions[0].label, coopEndMissions[0].story );
		return;
	}
	// the campaign does not offer a choice here (academy, hub boss, cinematic
	// chain...): the host drives the stock menus, the others wait in front of
	// the debrief instead of a black screen
	coopEndPhase = COOPEND_WAIT;
	coopEndWaitAcked = qfalse;
	coopEndPhaseTime = level.time;
	Q_strncpyz( coopEndInfo, "#W", sizeof( coopEndInfo ) );	// "the host is choosing" (CL_CoopEndLocalize)
	G_CoopEndPublish();
	gi.Printf( "coop: no mission to vote for here, the host gets the stock menu %s\n", coopEndStockMenu );
	gi.SendConsoleCommand( va( "uimenu %s\n", coopEndStockMenu ) );
}

static void G_CoopEndStart( void )
{
	char killed[64], secrets[64];

	coopEndStartTime = 0;
	coopEndLaunched = qfalse;
	coopEndMap[0] = '\0';
	memset( coopEndNextDone, 0, sizeof( coopEndNextDone ) );
	memset( coopEndReady, 0, sizeof( coopEndReady ) );
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		coopEndVotes[i] = -1;
	}
	G_CoopEndMissionTitle( level.mapname, coopEndTitle, sizeof( coopEndTitle ) );
	gi.Cvar_VariableStringBuffer( "ui_stats_enemieskilled", killed, sizeof( killed ) );
	gi.Cvar_VariableStringBuffer( "ui_stats_secretsfound", secrets, sizeof( secrets ) );
	if ( !killed[0] )
	{
		Q_strncpyz( killed, "0", sizeof( killed ) );
	}
	if ( atoi( secrets ) > 0 )
	{
		Com_sprintf( coopEndStats, sizeof( coopEndStats ), "#S %s %s", killed, secrets );
	}
	else
	{
		Com_sprintf( coopEndStats, sizeof( coopEndStats ), "#S %s", killed );
	}
	coopEndInfo[0] = '\0';
	coopEndForcePoint = qfalse;
	coopEndNextKind = G_CoopEndNextStep();
	G_CoopEndBuildMissions( coopEndNextKind == COOPNEXT_CHOOSE ? coopEndNextTier : NULL, coopEndNextKind == COOPNEXT_CHOOSE ? coopEndNextStory : NULL );
	coopEndPhase = COOPEND_DEBRIEF;
	coopEndPhaseTime = level.time;
	G_CoopEndProtectPlayers( qtrue );
	gi.cvar_set( "timescale", "1" );
	gi.Printf( "coop: end of mission %s (menu %s, tier_storyinfo %i): %i mission(s) to choose from, story map %s\n",
		level.mapname, coopEndStockMenu, gi.Cvar_VariableIntegerValue( "tier_storyinfo" ), coopEndNumMissions,
		coopEndNextStory ? coopEndNextStory : "-" );
	G_CoopEndOpenMenu( "coopDebrief" );
}

/*
==============================================================================
Entry points
==============================================================================
*/
// called at level start: nothing of the previous mission survives
void G_CoopEndLevelInit( void )
{
	coopEndPhase = COOPEND_NONE;
	coopEndStartTime = coopEndPhaseTime = 0;
	coopEndLaunched = qfalse;
	coopEndNumMissions = 0;
	coopEndNextKind = COOPNEXT_STOCK;
	coopEndNextTier = NULL;
	coopEndNextStory = NULL;
	coopEndForcePoint = qfalse;
	coopEndStockMenu[0] = coopEndMap[0] = coopEndTitle[0] = coopEndStats[0] = coopEndInfo[0] = '\0';
	memset( coopEndGodmode, 0, sizeof( coopEndGodmode ) );
	memset( coopEndNextDone, 0, sizeof( coopEndNextDone ) );
	memset( coopEndReady, 0, sizeof( coopEndReady ) );
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		coopEndVotes[i] = -1;
	}
}

// G_ChangeMap got a "+menuname": qtrue when co-op takes the screens over.
// The flow only starts on the next frame, because target_level_change sets
// tier_storyinfo, tiers_complete and the mission stats *after* calling us.
qboolean G_CoopEndLevelMenu( const char *menuName )
{
	if ( G_CoopNumPlayers() < 2 || G_CoopIsLobby() || coopEndPhase != COOPEND_NONE || coopEndStartTime )
	{
		return qfalse;
	}
	Q_strncpyz( coopEndStockMenu, menuName, sizeof( coopEndStockMenu ) );
	coopEndStartTime = level.time + COOPEND_START_DELAY;
	return qtrue;
}

// once per server frame, from G_CoopLobbyFrame
void G_CoopEndLevelFrame( void )
{
	if ( coopEndStartTime && level.time >= coopEndStartTime )
	{
		if ( G_CoopNumPlayers() < 2 )
		{	// the other player left between G_ChangeMap and here: stock behaviour
			coopEndStartTime = 0;
			gi.SendConsoleCommand( va( "uimenu %s\n", coopEndStockMenu ) );
			return;
		}
		G_CoopEndStart();
		return;
	}
	if ( coopEndPhase == COOPEND_NONE )
	{
		return;
	}
	if ( G_CoopNumPlayers() < 2 )
	{	// everybody else left: the host must not stay stuck on a shared panel
		if ( coopEndMap[0] )
		{
			G_CoopEndLaunch();
		}
		else
		{
			G_CoopEndProtectPlayers( qfalse );
			coopEndPhase = COOPEND_NONE;
			gi.SetConfigstring( CS_COOP_ENDLEVEL, "" );
			gi.SendServerCommand( -1, "coopmenu closeall" );
			gi.Printf( "coop: alone again, back to the stock menu %s\n", coopEndStockMenu );
			gi.SendConsoleCommand( va( "uimenu %s\n", coopEndStockMenu ) );
		}
		return;
	}
	G_CoopEndProtectPlayers( qtrue );	// a player that just reconnected too

	int players = 0, done = 0, voted = 0, ready = 0;
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( !G_CoopPlayerSlot( i ) )
		{
			continue;
		}
		players++;
		done += coopEndNextDone[i] ? 1 : 0;
		voted += ( coopEndVotes[i] >= 0 ) ? 1 : 0;
		ready += coopEndReady[i] ? 1 : 0;
	}

	switch ( coopEndPhase )
	{
	case COOPEND_DEBRIEF:
		Com_sprintf( coopEndInfo, sizeof( coopEndInfo ), "#D %i %i", done, players );
		if ( done >= players || level.time - coopEndPhaseTime > COOPEND_DEBRIEF_TIMEOUT )
		{
			G_CoopEndAfterDebrief();
		}
		break;

	case COOPEND_VOTE:
		Com_sprintf( coopEndInfo, sizeof( coopEndInfo ), "#V %i %i", voted, players );
		if ( voted >= players || level.time - coopEndPhaseTime > COOPEND_VOTE_TIMEOUT )
		{
			G_CoopEndTally();
		}
		break;

	case COOPEND_LOADOUT:
		Com_sprintf( coopEndInfo, sizeof( coopEndInfo ), "#R %i %i", ready, players );
		if ( ready >= players )
		{
			G_CoopEndLaunch();
		}
		break;

	case COOPEND_WAIT:
		if ( !Q_stricmp( coopEndStockMenu, "saberstyleMenu" ) || !Q_stricmp( coopEndStockMenu, "saberMenu" ) )
		{	// academy3 / academy5: the host's saber screen ends by opening ingameGotoTier
			// with tier_storyinfo 7 / 13 (ui/saberstyle.menu, ui/saber.menu). From there
			// on the stock game chooses a hub mission: that is everybody's vote again.
			const int story = gi.Cvar_VariableIntegerValue( "tier_storyinfo" );
			if ( story == 7 || story == 13 )
			{
				Q_strncpyz( coopEndStockMenu, "ingameGotoTier", sizeof( coopEndStockMenu ) );
				coopEndNextKind = G_CoopEndNextStep();
				G_CoopEndBuildMissions( coopEndNextKind == COOPNEXT_CHOOSE ? coopEndNextTier : NULL, NULL );
				if ( coopEndNumMissions )
				{
					gi.Printf( "coop: the host left the saber screen (tier_storyinfo %i): vote on %i mission(s)\n", story, coopEndNumMissions );
					coopEndInfo[0] = '\0';
					G_CoopEndAfterDebrief();
				}
			}
		}
		break;

	default:
		break;
	}
	G_CoopEndPublish();
}

/*
==============================================================================
Client commands
==============================================================================
*/
// give a joiner exactly the loadout it picked on its weapons screen (the host
// edits its own playerState straight from the menu, like the stock game does)
static void G_CoopEndApplyWeapons( gentity_t *ent, const int *chosen, int numChosen )
{
	playerState_t *ps = &ent->client->ps;

	ps->stats[STAT_WEAPONS] = ( 1 << WP_NONE ) | ( 1 << WP_SABER ) | ( 1 << WP_BLASTER_PISTOL );
	memset( ps->ammo, 0, sizeof( ps->ammo ) );
	memset( ps->inventory, 0, sizeof( ps->inventory ) );
	for ( int c = 0; c < numChosen; c++ )
	{
		for ( const coopEndWeapon_t *w = coopEndWeapons; w->weapon != WP_NONE; w++ )
		{
			if ( w->weapon != chosen[c] )
			{
				continue;
			}
			ps->stats[STAT_WEAPONS] |= ( 1 << w->weapon );
			if ( w->ammoIndex > 0 && w->ammoIndex < AMMO_MAX )
			{
				ps->ammo[w->ammoIndex] = w->ammoAmount;
			}
			break;
		}
	}
	ps->ammo[AMMO_BLASTER] = Q_max( ps->ammo[AMMO_BLASTER], 100 );	// the pistol everybody keeps
	G_RemoveWeaponModels( ent );
	ps->weapon = WP_SABER;
	ps->weaponstate = WEAPON_READY;
	WP_SaberInitBladeData( ent );
	WP_SaberAddG2SaberModels( ent );
	G_AddWeaponModels( ent );
	gi.SendServerCommand( ent->s.number, "wp %i", WP_SABER );
	G_CoopStoreState( ent );
}

// coop_endnext / coop_endvote <index> / coop_endwpn <w1> <w2> <wthrow> /
// coop_endready / coop_endgo (host only)
void G_CoopEndLevelCommand( gentity_t *ent, const char *cmd )
{
	if ( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS )
	{
		return;
	}
	const int slot = ent->s.number;

	if ( !Q_stricmp( cmd, "coop_endnext" ) )
	{
		if ( coopEndPhase == COOPEND_DEBRIEF )
		{
			coopEndNextDone[slot] = qtrue;
		}
		return;
	}
	if ( !Q_stricmp( cmd, "coop_endvote" ) )
	{
		if ( coopEndPhase == COOPEND_VOTE && gi.argc() >= 2 )
		{
			const int index = atoi( gi.argv( 1 ) );
			if ( index >= 0 && index < coopEndNumMissions )
			{
				coopEndVotes[slot] = index;
				gi.Printf( "coop: %s votes for %s\n", ent->client->pers.netname, coopEndMissions[index].map );
			}
		}
		return;
	}
	if ( !Q_stricmp( cmd, "coop_endwpn" ) )
	{
		if ( coopEndPhase == COOPEND_LOADOUT && slot > 0 && ent->health > 0 && gi.argc() >= 2 )
		{	// the host applies its own choices through the menu's uiScripts
			int chosen[4], n = 0;
			for ( int a = 1; a < gi.argc() && n < 4; a++ )
			{
				chosen[n++] = atoi( gi.argv( a ) );
			}
			G_CoopEndApplyWeapons( ent, chosen, n );
			gi.Printf( "coop: %s picked its weapons (mask %i)\n",
				ent->client->pers.netname, ent->client->ps.stats[STAT_WEAPONS] );
		}
		return;
	}
	if ( !Q_stricmp( cmd, "coop_endready" ) )
	{
		if ( coopEndPhase == COOPEND_LOADOUT )
		{
			coopEndReady[slot] = (qboolean)!coopEndReady[slot];
		}
		return;
	}
	if ( !Q_stricmp( cmd, "coop_endgo" ) )
	{
		if ( slot != 0 )
		{
			return;		// the host alone may skip a step
		}
		switch ( coopEndPhase )
		{
		case COOPEND_DEBRIEF:
			for ( int i = 0; i < MAX_CLIENTS; i++ )
			{
				coopEndNextDone[i] = qtrue;
			}
			break;
		case COOPEND_VOTE:
			G_CoopEndTally();
			break;
		case COOPEND_LOADOUT:
			G_CoopEndLaunch();
			break;
		case COOPEND_WAIT:
			if ( coopEndWaitAcked )
			{	// second press: give up on the panels, the host drives the stock screens alone
				G_CoopEndProtectPlayers( qfalse );
				coopEndPhase = COOPEND_NONE;
				gi.SetConfigstring( CS_COOP_ENDLEVEL, "" );
				gi.SendServerCommand( -1, "coopmenu closeall" );
				gi.Printf( "coop: end-of-mission panels closed, the host carries on alone\n" );
			}
			else
			{	// the stock screens are the host's: open them again if it lost them
				coopEndWaitAcked = qtrue;
				gi.SendConsoleCommand( va( "uimenu %s\n", coopEndStockMenu ) );
			}
			break;
		default:
			break;
		}
		return;
	}
}

// "coop_endlevel [menu] [tier_storyinfo]" (cheats): drive the end-of-mission
// flow by hand, the way a level-end script would
void G_CoopEndLevelTestCommand( void )
{
	const char *menu = ( gi.argc() >= 2 ) ? gi.argv( 1 ) : "ingameMissionSelect";

	if ( gi.argc() >= 3 )
	{
		gi.cvar_set( "tier_storyinfo", gi.argv( 2 ) );
	}
	if ( !G_CoopEndLevelMenu( menu ) )
	{
		gi.Printf( "coop_endlevel: needs at least two players and no flow in progress\n" );
		return;
	}
	gi.Printf( "coop_endlevel: faking the end of %s (menu %s, tier_storyinfo %i)\n",
		level.mapname, menu, gi.Cvar_VariableIntegerValue( "tier_storyinfo" ) );
}
