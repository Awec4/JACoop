/*
==============================================================================
Co-op voice chat (push-to-talk)

Minimal built-in voice so players don't need Discord for a quick co-op night:

  capture   SDL audio capture device, mono s16 @ 11025 Hz
  codec     IMA ADPCM (4:1, ~5.5 KB/s while talking) - tiny, no dependencies
  transport connectionless packets: "\xff\xff\xff\xffcvoice\n" + binary
            [u8 speaker][u16 len][u16 predictor][u8 index][u8 reserved][data]
            a client sends to the host's address; the host stamps the slot and
            relays to everyone else (loopback client included)
  playback  per-speaker jitter buffers mixed into S_RawSamples, so several
            people can talk over each other without interleaving chunks

Hold +coop_voice (default V) to talk, or set cl_coopVoiceVox to an RMS
threshold (try 400) for open-mic voice activation. cl_coopVoice 0 disables the
whole thing, cl_coopVoiceGain scales what you hear, cl_coopVoiceDevice picks
the capture device (coop_voicemics lists them, -1 = default). cg_coopVoiceTalk
is written for cgame: bits 0..15 = who talked recently, bit 30 = transmitting.
==============================================================================
*/

#include "client.h"
#include "snd_local.h"
#include "SDL.h"

#define VOICE_RATE			11025				// capture / playback sample rate
#define VOICE_CHUNK			1102				// samples per packet (~100 ms)
#define VOICE_PAYLOAD_HDR	4					// predictor + index + pad
#define VOICE_MAX_PAYLOAD	( VOICE_PAYLOAD_HDR + VOICE_CHUNK / 2 )
#define VOICE_RING			( VOICE_RATE * 2 )	// 2s of decoded audio per speaker
#define VOICE_TALK_FLAG		( 1 << 30 )			// local mic is transmitting
#define VOICE_HEARD_MSEC	500					// "talking" indicator lifetime

static cvar_t	*cl_coopVoice;
static cvar_t	*cl_coopVoiceGain;
static cvar_t	*cl_coopVoiceDevice;	// -1 = default mic, else SDL device index
static cvar_t	*cl_coopVoiceVox;		// 0 = push-to-talk only, >0 = RMS gate threshold
static cvar_t	*cl_coopVoiceTalk;	// ROM for cgame: talker bitmask + TX flag

static qboolean		voiceHeld;				// +coop_voice is held
static int			voiceLevel;				// smoothed mic level, for the vox gate
static int			voiceTxUntil;			// realtime: keep the TX indicator / vox tail open
static qboolean		voiceMicTried;			// we attempted opening the capture device
static qboolean		voiceMicFailed;
static SDL_AudioDeviceID	voiceMic = 0;

static short	voiceTxPcm[VOICE_CHUNK * 2];	// captured samples awaiting a full chunk
static int		voiceTxCount;
static short	voiceTxPredictor;				// encoder state while PTT is held
static int		voiceTxIndex;

static short	voiceRxPcm[MAX_CLIENTS][VOICE_RING];
static int		voiceRxHead[MAX_CLIENTS];
static int		voiceRxCount[MAX_CLIENTS];
static int		voiceRxHeard[MAX_CLIENTS];		// realtime of last packet per slot


/*
==============================================================================
IMA ADPCM
==============================================================================
*/

static const int voiceStepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};

static const int voiceIndexTable[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static int Voice_EncodeSample( short *predictor, int *index, int sample ) {
	int step = voiceStepTable[*index];
	int diff = sample - *predictor;
	int code = 0;
	if ( diff < 0 ) {
		code = 8;
		diff = -diff;
	}
	int tempStep = step;
	if ( diff >= tempStep ) { code |= 4; diff -= tempStep; }
	tempStep >>= 1;
	if ( diff >= tempStep ) { code |= 2; diff -= tempStep; }
	tempStep >>= 1;
	if ( diff >= tempStep ) { code |= 1; }

	int diffq = step >> 3;
	if ( code & 4 ) diffq += step;
	if ( code & 2 ) diffq += step >> 1;
	if ( code & 1 ) diffq += step >> 2;
	*predictor += ( code & 8 ) ? -diffq : diffq;
	if ( *predictor > 32767 ) *predictor = 32767;
	else if ( *predictor < -32768 ) *predictor = -32768;

	*index += voiceIndexTable[code];
	if ( *index < 0 ) *index = 0;
	else if ( *index > 88 ) *index = 88;
	return code & 0xf;
}

static int Voice_DecodeSample( short *predictor, int *index, int code ) {
	int step = voiceStepTable[*index];
	int diffq = step >> 3;
	if ( code & 4 ) diffq += step;
	if ( code & 2 ) diffq += step >> 1;
	if ( code & 1 ) diffq += step >> 2;
	*predictor += ( code & 8 ) ? -diffq : diffq;
	if ( *predictor > 32767 ) *predictor = 32767;
	else if ( *predictor < -32768 ) *predictor = -32768;

	*index += voiceIndexTable[code];
	if ( *index < 0 ) *index = 0;
	else if ( *index > 88 ) *index = 88;
	return *predictor;
}

// returns the payload length (header + packed nibbles); each packet is
// self-contained so a lost one doesn't smear the stream
static int Voice_EncodeChunk( const short *pcm, int count, byte *out ) {
	short predictor = voiceTxPredictor;
	int index = voiceTxIndex;
	*(short *)( out + 0 ) = predictor;
	out[2] = (byte)index;
	out[3] = 0;
	for ( int i = 0; i < count; i += 2 ) {
		int lo = Voice_EncodeSample( &predictor, &index, pcm[i] );
		int hi = ( i + 1 < count ) ? Voice_EncodeSample( &predictor, &index, pcm[i + 1] ) : 0;
		out[VOICE_PAYLOAD_HDR + i / 2] = (byte)( lo | ( hi << 4 ) );
	}
	return VOICE_PAYLOAD_HDR + ( count + 1 ) / 2;
}

static int Voice_DecodeChunk( const byte *in, int payloadLen, short *out ) {
	short predictor = *(const short *)( in + 0 );
	int index = in[2];
	if ( index > 88 ) index = 88;	// garbage tolerance
	int samples = ( payloadLen - VOICE_PAYLOAD_HDR ) * 2;
	for ( int i = 0; i < samples; i += 2 ) {
		byte b = in[VOICE_PAYLOAD_HDR + i / 2];
		out[i] = (short)Voice_DecodeSample( &predictor, &index, b & 0xf );
		if ( i + 1 < samples ) out[i + 1] = (short)Voice_DecodeSample( &predictor, &index, b >> 4 );
	}
	return samples;
}


/*
==============================================================================
Transmit: capture -> encode -> connectionless packet to the host
==============================================================================
*/

static void CL_CoopVoiceDown( void ) { voiceHeld = qtrue; }
static void CL_CoopVoiceUp( void ) { voiceHeld = qfalse; }

static qboolean Voice_OpenMic( void ) {
	if ( voiceMic || voiceMicFailed ) {
		return (qboolean)( voiceMic != 0 );
	}
	if ( voiceMicTried ) {
		return qfalse;
	}
	voiceMicTried = qtrue;
	if ( SDL_InitSubSystem( SDL_INIT_AUDIO ) < 0 ) {
		Com_Printf( "voice: SDL audio unavailable (%s)\n", SDL_GetError() );
		voiceMicFailed = qtrue;
		return qfalse;
	}
	const char	*devName = NULL;
	if ( cl_coopVoiceDevice->integer >= 0 ) {
		devName = SDL_GetAudioDeviceName( cl_coopVoiceDevice->integer, 1 );
		if ( !devName ) {
			Com_Printf( "voice: no capture device %i (coop_voicemics lists them)\n",
				cl_coopVoiceDevice->integer );
			voiceMicFailed = qtrue;
			return qfalse;
		}
	}
	SDL_AudioSpec spec;
	memset( &spec, 0, sizeof( spec ) );
	spec.freq = VOICE_RATE;
	spec.format = AUDIO_S16SYS;
	spec.channels = 1;
	spec.samples = 2048;
	spec.callback = NULL;
	voiceMic = SDL_OpenAudioDevice( devName, 1, &spec, NULL, 0 );	// 1 = capture
	if ( !voiceMic ) {
		Com_Printf( "voice: no capture device (%s)\n", SDL_GetError() );
		voiceMicFailed = qtrue;
		return qfalse;
	}
	return qtrue;
}

static void Voice_SendChunk( void ) {
	static byte	scratch[VOICE_MAX_PAYLOAD];
	byte		pkt[4 + 7 + 3 + VOICE_MAX_PAYLOAD];
	qboolean	send = voiceHeld;

	// vox gate: open the mic stream while the level is over the threshold and
	// hold it a little past word tails so speech doesn't chop
	const int	voxThresh = (int)cl_coopVoiceVox->value;
	if ( voxThresh > 0 ) {
		if ( voiceLevel >= voxThresh ) {
			voiceTxUntil = cls.realtime + 400;
		}
		if ( cls.realtime < voiceTxUntil ) {
			send = qtrue;
		}
	}

	if ( send ) {
		const int	payloadLen = Voice_EncodeChunk( voiceTxPcm, VOICE_CHUNK, pkt + 14 );
		*(int *)( pkt ) = -1;
		memcpy( pkt + 4, "cvoice\n", 7 );
		pkt[11] = 0xff;									// slot: the server fills it in
		*(unsigned short *)( pkt + 12 ) = (unsigned short)payloadLen;
		NET_SendPacket( NS_CLIENT, 14 + payloadLen, pkt, clc.serverAddress );
	}
	else {
		// keep the predictor drifting so the first voiced chunk doesn't pop
		Voice_EncodeChunk( voiceTxPcm, VOICE_CHUNK, scratch );
	}

	voiceTxCount -= VOICE_CHUNK;
	memmove( voiceTxPcm, voiceTxPcm + VOICE_CHUNK, voiceTxCount * sizeof( short ) );
}

static void CL_CoopVoiceMics_f( void ) {
	const int	n = SDL_GetNumAudioDevices( 1 );	// 1 = capture devices
	if ( n <= 0 ) {
		Com_Printf( "voice: no capture devices\n" );
		return;
	}
	Com_Printf( "capture devices (cl_coopVoiceDevice, -1 = default):\n" );
	for ( int i = 0; i < n; i++ ) {
		Com_Printf( "  %i: %s\n", i, SDL_GetAudioDeviceName( i, 1 ) );
	}
}

static void Voice_TransmitFrame( void ) {
	const qboolean	inGame = (qboolean)( cl_coopVoice->integer && cls.state == CA_ACTIVE
							&& clc.serverAddress.type != NA_BAD );
	const qboolean	listen = (qboolean)( inGame && ( voiceHeld || cl_coopVoiceVox->value > 0.0f ) );

	if ( cl_coopVoiceDevice->modified && voiceMic ) {	// mic picker changed: reopen
		SDL_CloseAudioDevice( voiceMic );
		voiceMic = 0;
		voiceMicTried = qfalse;
		voiceMicFailed = qfalse;
	}
	cl_coopVoiceDevice->modified = qfalse;

	if ( !listen || !Voice_OpenMic() ) {
		if ( voiceMic ) {
			SDL_PauseAudioDevice( voiceMic, 1 );
			SDL_ClearQueuedAudio( voiceMic );
		}
		voiceTxCount = 0;
		voiceLevel = 0;
		return;
	}

	SDL_PauseAudioDevice( voiceMic, 0 );
	if ( voiceTxCount == 0 ) {	// fresh press: restart the predictor gently
		voiceTxPredictor = 0;
		voiceTxIndex = 0;
	}

	byte	tmp[4096];
	for ( ;; ) {
		Uint32	got = SDL_DequeueAudio( voiceMic, tmp, sizeof( tmp ) );
		if ( !got ) {
			break;
		}
		const short	*in = (const short *)tmp;
		int			n = got / sizeof( short );
		long		sum = 0;
		for ( int i = 0; i < n; i++ ) {
			sum += abs( in[i] );	// smoothed level feeds the vox gate
			voiceTxPcm[voiceTxCount++] = in[i];
			if ( voiceTxCount == VOICE_CHUNK ) {
				Voice_SendChunk();
			}
		}
		if ( n ) {
			voiceLevel = ( voiceLevel * 3 + (int)( sum / n ) ) / 4;
		}
		if ( voiceTxCount >= sizeof( voiceTxPcm ) / sizeof( short ) ) {
			voiceTxCount = 0;	// should never hit: 2 chunks fit exactly
		}
	}
}


/*
==============================================================================
Receive: host-relayed packet -> per-speaker jitter buffer -> mixed playback
==============================================================================
*/

void CL_CoopVoice_Packet( netadr_t from, msg_t *msg ) {
	if ( !cl_coopVoice->integer || !NET_CompareAdr( from, clc.serverAddress ) ) {
		return;
	}
	byte	hdr[3];
	if ( msg->cursize - msg->readcount < (int)sizeof( hdr ) ) {
		return;
	}
	MSG_ReadData( msg, hdr, sizeof( hdr ) );
	const int	slot = hdr[0];
	const int	payloadLen = hdr[1] | ( hdr[2] << 8 );
	if ( slot >= MAX_CLIENTS || payloadLen <= VOICE_PAYLOAD_HDR
		|| payloadLen > VOICE_MAX_PAYLOAD
		|| msg->cursize - msg->readcount != payloadLen ) {
		return;
	}
	byte	payload[VOICE_MAX_PAYLOAD];
	MSG_ReadData( msg, payload, payloadLen );

	short	decoded[VOICE_CHUNK];
	const int	samples = Voice_DecodeChunk( payload, payloadLen, decoded );
	if ( voiceRxCount[slot] + samples > VOICE_RING ) {
		// burst of loss recovery: drop the oldest half, keep it bounded
		int	drop = VOICE_RING / 2;
		voiceRxHead[slot] = ( voiceRxHead[slot] + drop ) % VOICE_RING;
		voiceRxCount[slot] -= drop;
	}
	for ( int i = 0; i < samples; i++ ) {
		voiceRxPcm[slot][( voiceRxHead[slot] + voiceRxCount[slot] ) % VOICE_RING] = decoded[i];
		voiceRxCount[slot]++;
	}
	voiceRxHeard[slot] = cls.realtime;
}

static void Voice_PlaybackFrame( int msec ) {
	static int	mix[VOICE_RATE / 2];		// <= 500ms per frame
	static short	out[VOICE_RATE / 2];
	int		want = ( msec * VOICE_RATE ) / 1000;
	int		mask = 0;

	if ( want <= 0 ) {
		return;
	}
	if ( want > VOICE_RATE / 2 ) {
		want = VOICE_RATE / 2;
	}
	memset( mix, 0, want * sizeof( mix[0] ) );

	for ( int s = 0; s < MAX_CLIENTS; s++ ) {
		int	n = voiceRxCount[s];
		if ( n > want ) {
			n = want;
		}
		for ( int i = 0; i < n; i++ ) {
			mix[i] += voiceRxPcm[s][( voiceRxHead[s] + i ) % VOICE_RING];
		}
		voiceRxHead[s] = ( voiceRxHead[s] + n ) % VOICE_RING;
		voiceRxCount[s] -= n;
		if ( cls.realtime - voiceRxHeard[s] < VOICE_HEARD_MSEC ) {
			mask |= 1 << s;
		}
	}

	int	max = 0;
	for ( int i = 0; i < want; i++ ) {
		if ( mix[i] > max ) max = mix[i];
		else if ( -mix[i] > max ) max = -mix[i];
	}
	if ( max ) {
		const float	gain = Com_Clamp( 0.0f, 4.0f, cl_coopVoiceGain->value );
		const float	scale = max > 32767 ? 32767.0f / max : 1.0f;
		for ( int i = 0; i < want; i++ ) {
			out[i] = (short)Com_Clamp( -32768, 32767, (int)( mix[i] * scale * gain ) );
		}
		S_RawSamples( want, VOICE_RATE, 2, 1, (byte *)out, 1.0f, qtrue );
	}

	if ( voiceMic && ( voiceHeld || cls.realtime < voiceTxUntil ) ) {
		mask |= VOICE_TALK_FLAG;	// transmitting right now (PTT or vox gate)
	}
	if ( cl_coopVoiceTalk->integer != mask ) {
		Cvar_SetValue( "cg_coopVoiceTalk", mask );
	}
}


/*
==============================================================================
Entry points
==============================================================================
*/

void CL_CoopVoice_Init( void ) {
	cl_coopVoice = Cvar_Get( "cl_coopVoice", "1", CVAR_ARCHIVE );
	cl_coopVoiceGain = Cvar_Get( "cl_coopVoiceGain", "1.0", CVAR_ARCHIVE );
	cl_coopVoiceDevice = Cvar_Get( "cl_coopVoiceDevice", "-1", CVAR_ARCHIVE );
	cl_coopVoiceVox = Cvar_Get( "cl_coopVoiceVox", "0", CVAR_ARCHIVE );
	cl_coopVoiceTalk = Cvar_Get( "cg_coopVoiceTalk", "0", CVAR_TEMP );
	Cmd_AddCommand( "+coop_voice", CL_CoopVoiceDown );
	Cmd_AddCommand( "-coop_voice", CL_CoopVoiceUp );
	Cmd_AddCommand( "coop_voicemics", CL_CoopVoiceMics_f );
}

void CL_CoopVoice_Shutdown( void ) {
	if ( voiceMic ) {
		SDL_CloseAudioDevice( voiceMic );
		voiceMic = 0;
	}
	voiceHeld = qfalse;
	voiceMicTried = qfalse;
	voiceMicFailed = qfalse;
	voiceTxCount = 0;
	voiceLevel = 0;
	voiceTxUntil = 0;
	memset( voiceRxHead, 0, sizeof( voiceRxHead ) );
	memset( voiceRxCount, 0, sizeof( voiceRxCount ) );
	memset( voiceRxHeard, 0, sizeof( voiceRxHeard ) );
	Cmd_RemoveCommand( "+coop_voice" );
	Cmd_RemoveCommand( "-coop_voice" );
	Cmd_RemoveCommand( "coop_voicemics" );
}

void CL_CoopVoice_Frame( int msec ) {
	if ( !cl_coopVoice->integer ) {
		if ( cl_coopVoiceTalk->integer ) {
			Cvar_SetValue( "cg_coopVoiceTalk", 0 );
		}
		return;
	}
	Voice_TransmitFrame();
	Voice_PlaybackFrame( msec );
}
