//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#ifndef SWDS
#include "screen.h"
#include "cl_main.h"
#include "iprediction.h"
#include "proto_oob.h"
#include "demo.h"
#include "tier0/icommandline.h"
#include "ispatialpartitioninternal.h"
#include "GameEventManager.h"
#include "cdll_engine_int.h"
#include "voice.h"
#include "host_cmd.h"
#include "server.h"
#include "convar.h"
#include "dt_recv_eng.h"
#include "dt_common_eng.h"
#include "LocalNetworkBackdoor.h"
#include "vox.h"
#include "sound.h"
#include "r_efx.h"
#include "r_local.h"
#include "decal_private.h"
#include "vgui_baseui_interface.h"
#include "host_state.h"
#include "cl_ents_parse.h"
#include "eiface.h"
#include "server.h"
#include "cl_demoactionmanager.h"
#include "decal.h"
#include "r_decal.h"
#include "materialsystem/imaterial.h"
#include "EngineSoundInternal.h"
#include "master.h"
#include "ivideomode.h"
#include "download.h"
#include "GameUI/IGameUI.h"

#include "audio_pch.h"

#if defined ( _X360 )
#include "matchmaking.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IVEngineClient *engineClient;

extern CNetworkStringTableContainer *networkStringTableContainerClient;
extern CNetworkStringTableContainer *networkStringTableContainerServer;

static ConVar cl_allowupload ( "cl_allowupload", "1", FCVAR_ARCHIVE, "Client uploads customization files" );
static ConVar cl_voice_filter( "cl_voice_filter", "", 0, "Filter voice by name substring" ); // filter incoming voice data
extern ConCommand quit;

void CClientState::ConnectionClosing( const char * reason )
{
	// if connected, shut down host
	if ( m_nSignonState > SIGNONSTATE_NONE )
	{
		ConMsg( "Disconnect: %s.\n", reason );
		if ( !Q_stricmp( reason, INVALID_STEAM_TICKET ) )
		{
			g_eSteamLoginFailure = STEAMLOGINFAILURE_BADTICKET;
		}
		else if ( !Q_stricmp( reason, INVALID_STEAM_LOGON ) )
		{
			g_eSteamLoginFailure = STEAMLOGINFAILURE_NOSTEAMLOGIN;
		}
		else if ( !Q_stricmp( reason, INVALID_STEAM_LOGGED_IN_ELSEWHERE ) )
		{
			g_eSteamLoginFailure = STEAMLOGINFAILURE_LOGGED_IN_ELSEWHERE;
		}
		else if ( !Q_stricmp( reason, INVALID_STEAM_VACBANSTATE ) )
		{
			g_eSteamLoginFailure = STEAMLOGINFAILURE_VACBANNED;
		}
		else
		{
			g_eSteamLoginFailure = STEAMLOGINFAILURE_NONE;
		}
		COM_ExplainDisconnection( true, "Disconnect: %s.\n", reason );
		SCR_EndLoadingPlaque();
		Host_Disconnect(true);
	}
}


void CClientState::ConnectionCrashed( const char * reason )
{
	// if connected, shut down host
	if ( m_nSignonState > SIGNONSTATE_NONE )
	{
		COM_ExplainDisconnection( true, "Disconnect: %s.\n", reason );
		SCR_EndLoadingPlaque();
		Host_EndGame ( true, reason );
	}
}


void CClientState::FileRequested(const char *fileName, unsigned int transferID )
{
	ConMsg( "File '%s' requested from server %s.\n", fileName, m_NetChannel->GetAddress() );

	if ( !cl_allowupload.GetBool() )
	{
		ConMsg( "File uploading disabled.\n" );
		m_NetChannel->DenyFile( fileName, transferID );
		return;
	}

	// TODO check if file valid for uploading
	m_NetChannel->SendFile( fileName, transferID );
}

void CClientState::FileReceived( const char * fileName, unsigned int transferID )
{
#ifndef _XBOX
	// check if the client donwload manager requested this file
	CL_FileReceived( fileName, transferID );
#endif
}

void CClientState::FileDenied(const char *fileName, unsigned int transferID )
{
#ifndef _XBOX
	// check if the file download manager requested that file
	CL_FileDenied( fileName, transferID );
#endif
}


void CClientState::PacketStart( int incoming_sequence, int outgoing_acknowledged	)
{
	// Ack'd incoming messages.
	m_nCurrentSequence = incoming_sequence;
	command_ack = outgoing_acknowledged;
}


void CClientState::PacketEnd()
{
	//
	// we don't know if it is ok to save a demo message until
	// after we have parsed the frame
	//

	// Play any sounds we received this packet
	CL_DispatchSounds();
	
	// Did we get any messages this tick (i.e., did we call PreEntityPacketReceived)?
	if ( GetServerTickCount() != m_nDeltaTick )
		return;

	// How many commands total did we run this frame
	int commands_acknowledged = command_ack - last_command_ack;

//	COM_Log( "cl.log", "Server ack'd %i commands this frame\n", commands_acknowledged );

	//Msg( "%i/%i CL_PostReadMessages:  last ack %i most recent %i acked %i commands\n", 
	//	host_framecount, cl.tickcount,
	//	cl.last_command_ack, 
	//	cl.netchan->outgoing_sequence - 1,
	//	commands_acknowledged );

	// Highest command parsed from messages
	last_command_ack = command_ack;
	
	// Let prediction copy off pristine data and report any errors, etc.
	g_pClientSidePrediction->PostNetworkDataReceived( commands_acknowledged );

#ifndef _XBOX
	demoaction->DispatchEvents();
#endif
}


void CClientState::Disconnect(bool bShowMainMenu)
{
	CBaseClientState::Disconnect(bShowMainMenu);

	// stop any demo activities
#ifndef _XBOX
	demoplayer->StopPlayback();
	demorecorder->StopRecording();
#endif
	S_StopAllSounds( true );
	
	R_DecalTermAll();

	if ( m_nMaxClients > 1 )
	{
		if ( EngineVGui()->IsConsoleVisible() == false )
		{
			// start progress bar immediately for multiplayer level transitions
			EngineVGui()->EnabledProgressBarForNextLoad();
		}
	}

	CL_ClearState();
#ifndef _XBOX
	// End any in-progress downloads
	CL_HTTPStop_f();
#endif

	// stop loading progress bar 
	if (bShowMainMenu)
	{
		SCR_EndLoadingPlaque();
	}

	// notify game ui dll of out-of-in-game status
	EngineVGui()->NotifyOfServerDisconnect();

	if (bShowMainMenu && !engineClient->IsDrawingLoadingImage() && (cl.demonum == -1))
	{
		// we're not in the middle of loading something, so show the UI
		if ( EngineVGui() )
		{
			EngineVGui()->ActivateGameUI();
		}
	}

	HostState_OnClientDisconnected();

	// if we played a demo from the startdemos list, play next one
	if (cl.demonum != -1)
	{
		CL_NextDemo();
	}
}


bool CClientState::ProcessTick( NET_Tick *msg )
{
	int tick = msg->m_nTick;

	m_NetChannel->SetRemoteFramerate( msg->m_flHostFrameTime, msg->m_flHostFrameTimeStdDeviation );

	m_ClockDriftMgr.SetServerTick( tick );

	// Remember this for GetLastTimeStamp().
	m_flLastServerTickTime = tick * host_state.interval_per_tick;
	
	// Use the server tick while reading network data (used for interpolation samples, etc).
	g_ClientGlobalVariables.tickcount = tick;	
	g_ClientGlobalVariables.curtime = tick * host_state.interval_per_tick;
	g_ClientGlobalVariables.frametime = (tick - oldtickcount) * host_state.interval_per_tick;	// We used to call GetFrameTime() here, but 'insimulation' is always
																								// true so we have this code right in here to keep it simple.

	return true;
}


bool CClientState::ProcessStringCmd( NET_StringCmd *msg )
{
	// If we're connect to an HLTV server and it's giving us a connect command, then just do it.
	// Otherwise, use the base class version, which performs filtering.
	return InternalProcessStringCmd( msg, ishltv );
}


bool CClientState::ProcessServerInfo( SVC_ServerInfo *msg )
{
	// Reset client state
	CL_ClearState();

	if ( !CBaseClientState::ProcessServerInfo( msg ) )
	{
		Disconnect(true);
		return false;
	}
#ifndef _XBOX
	if ( demoplayer->IsPlayingBack() )
	{
		// Because a server doesn't run during
		// demoplayback, but the decal system relies on this...
		m_nServerCount = gHostSpawnCount;    
	}
	else
	{
		// tell demo recorder that new map is loaded and we are receiving
		// it's signon data (will be written into extra demo header file)
		demorecorder->SetSignonState( SIGNONSTATE_NEW );
	}
#endif
	// is server a HLTV proxy ?
	ishltv = msg->m_bIsHLTV;		

	// The CRC of the server map must match the CRC of the client map. or else
	//  the client is probably cheating.
	serverCRC = msg->m_nMapCRC;
	// The client side DLL CRC check.
	serverClientSideDllCRC = msg->m_nClientCRC;

	// Multiplayer game?
	if ( m_nMaxClients > 1 )	
	{
		if ( mp_decals.GetInt() < r_decals.GetInt() )
		{
			r_decals.SetValue( mp_decals.GetInt() );
		}
	}

	g_ClientGlobalVariables.maxClients = m_nMaxClients;
	g_ClientGlobalVariables.network_protocol = msg->m_nProtocol;

#ifdef SHARED_NET_STRING_TABLES
	// use same instance of StringTableContainer as the server does
	m_StringTableContainer = networkStringTableContainerServer;
	CL_HookClientStringTables();
#else
	// use own instance of StringTableContainer
	m_StringTableContainer = networkStringTableContainerClient;
#endif

	CL_ReallocateDynamicData( m_nMaxClients );
	
	if ( sv.IsPaused() )
	{
		if ( msg->m_fTickInterval != host_state.interval_per_tick )
		{
			Host_Error( "Expecting interval_per_tick %f, got %f\n", 
				host_state.interval_per_tick, msg->m_fTickInterval );
			return false;
		}
	}
	else
	{
		host_state.interval_per_tick = msg->m_fTickInterval;
	}

	// Re-init hud video, especially if we changed game directories
	ClientDLL_HudVidInit();

	// Don't verify the map and player .mdl crc's until after any missing resources have
	// been downloaded.  This will still occur before requesting the rest of the signon.


	gHostSpawnCount = m_nServerCount;
	
	videomode->MarkClientViewRectDirty();	// leave intermission full screen
	return true;
}

bool CClientState::ProcessClassInfo( SVC_ClassInfo *msg )
{
	if ( msg->m_bCreateOnClient )
	{
#ifndef _XBOX
		if ( !demoplayer->IsPlayingBack() )
#endif
		{
			// Create all of the send tables locally
			DataTable_CreateClientTablesFromServerTables();

			// Now create all of the server classes locally, too
			DataTable_CreateClientClassInfosFromServerClasses( this );

			// store the current data tables in demo file to make sure
			// they are the same during playback 
#ifndef _XBOX
			demorecorder->RecordServerClasses( serverGameDLL->GetAllServerClasses() );
#endif
		}

		LinkClasses();	// link server and client classes
	}
	else
	{
		CBaseClientState::ProcessClassInfo( msg );
	}
	
	if ( !RecvTable_CreateDecoders( serverGameDLL->GetStandardSendProxies() ) ) // create receive table decoders
	{
		Host_EndGame( true, "CL_ParseClassInfo_EndClasses: CreateDecoders failed.\n" );
		return false;
	}

#ifndef _XBOX
	if ( !demoplayer->IsPlayingBack() )
#endif
	{
		CLocalNetworkBackdoor::InitFastCopy();
	}

	return true;
}

bool CClientState::ProcessSetPause( SVC_SetPause *msg )
{
	CBaseClientState::ProcessSetPause( msg );

	return true;
}



bool CClientState::ProcessVoiceInit( SVC_VoiceInit *msg )
{
#if !defined( NO_VOICE )//#ifndef _XBOX
	if( msg->m_szVoiceCodec[0] == 0 )
	{
		Voice_Deinit();
	}
	else
	{
		Voice_Init( msg->m_szVoiceCodec );
	}
#endif
	return true;
}

ConVar voice_debugfeedback( "voice_debugfeedback", "0" );

bool CClientState::ProcessVoiceData( SVC_VoiceData *msg )
{
	char chReceived[4096];
	int nBytes = Bits2Bytes( msg->m_nLength );
	nBytes = min( nBytes, sizeof( chReceived ) );
	msg->m_DataIn.ReadBits( chReceived, msg->m_nLength );

#if defined ( _X360 )
	DWORD dwLength = msg->m_nLength;
	XUID xuid = msg->m_xuid;
	Audio_GetXVoice()->PlayIncomingVoiceData( xuid, (byte*)chReceived, dwLength );

	if ( voice_debugfeedback.GetBool() )
	{
		Msg( "Received voice from: %d\n", msg->m_nFromClient + 1 );
	}

	return true;
#endif

#if !defined( NO_VOICE )//#ifndef _XBOX
	int iEntity = msg->m_nFromClient + 1;
	if ( iEntity == (m_nPlayerSlot + 1) )
	{ 
		Voice_LocalPlayerTalkingAck();
	}

	player_info_t playerinfo;
	engineClient->GetPlayerInfo( iEntity, &playerinfo );

	if ( Q_strlen( cl_voice_filter.GetString() ) > 0 && Q_strstr( playerinfo.name, cl_voice_filter.GetString() ) == NULL )
		return true;

	// Data length can be zero when the server is just acking a client's voice data.
	if ( nBytes == 0 )
		return true;

	if ( !Voice_Enabled() )
	{
		return true;
	}

	// Have we already initialized the channels for this guy?
	int nChannel = Voice_GetChannel( iEntity );
	if ( nChannel == VOICE_CHANNEL_ERROR )
	{
		// Create a channel in the voice engine and a channel in the sound engine for this guy.
		nChannel = Voice_AssignChannel( iEntity, msg->m_bProximity );
		if ( nChannel == VOICE_CHANNEL_ERROR )
		{
			// If they used -nosound, then it's not a problem.
			if ( S_IsInitted() )
				ConDMsg("ProcessVoiceData: Voice_AssignChannel failed for client %d!\n", iEntity-1);
			
			return true;
		}
	}

	// Give the voice engine the data (it in turn gives it to the mixer for the sound engine).
	Voice_AddIncomingData( nChannel, chReceived, nBytes, m_nCurrentSequence );
#endif
	return true;
};

bool CClientState::ProcessPrefetch( SVC_Prefetch *msg )
{
	char const *soundname = cl.GetSoundName( msg->m_nSoundIndex );
	if ( soundname && soundname [ 0 ] )
	{
		EngineSoundClient()->PrefetchSound( soundname );
	}
	return true;
}

bool CClientState::ProcessSounds( SVC_Sounds *msg )	
{
	SoundInfo_t defaultSound; defaultSound.SetDefault();
	SoundInfo_t *pDeltaSound = &defaultSound;
	SoundInfo_t sound;

	int startbit = msg->m_DataIn.GetNumBitsRead();

	for ( int i=0; i<msg->m_nNumSounds; i++ )
	{
		sound.ReadDelta( pDeltaSound, msg->m_DataIn );

		pDeltaSound = &sound;	// copy delta values

		if ( msg->m_bReliableSound )
		{
			// client is incrementing the reliable sequence numbers itself
			m_nSoundSequence = ( m_nSoundSequence + 1 ) & SOUND_SEQNUMBER_MASK;
			Assert ( sound.nSequenceNumber == 0 );
			sound.nSequenceNumber = m_nSoundSequence;
		}

		// Add all received sounds to sorted queue (sounds may arrive in multiple messages), 
		//  will be processed after all packets have been completely parsed
		CL_AddSound( sound );
	}

	// check given length against read bits
	return ( msg->m_nLength == (msg->m_DataIn.GetNumBitsRead()-startbit) );
}


bool CClientState::ProcessFixAngle( SVC_FixAngle *msg )
{
	for (int i=0 ; i<3 ; i++)
	{
		// Clamp between -180 and 180
		if (msg->m_Angle[i]>180)
		{
			msg->m_Angle[i] -= 360;
		}
	}

	if ( msg->m_bRelative )
	{
			// Update running counter
		addangletotal += msg->m_Angle[YAW];

		AddAngle a;
		a.total = addangletotal;
		a.starttime = m_flLastServerTickTime;

		addangle.AddToTail( a );
	}
	else
	{

		viewangles = msg->m_Angle;
	}

	return true;
}


bool CClientState::ProcessCrosshairAngle( SVC_CrosshairAngle *msg )
{
	g_ClientDLL->SetCrosshairAngle( msg->m_Angle );

	return true;
}


bool CClientState::ProcessBSPDecal( SVC_BSPDecal *msg )
{
	model_t	* model;

	if ( msg->m_nEntityIndex )
	{
		model = GetModel( msg->m_nModelIndex );
	}
	else
	{
		model = host_state.worldmodel;
		if ( !model )
		{
			Warning( "ProcessBSPDecal:  Trying to project on world before host_state.worldmodel is set!!!\n" );
		}
	}

	if ( model == NULL )
	{
		IMaterial *mat = Draw_DecalMaterial( msg->m_nDecalTextureIndex );
		char const *matname = "???";
		if ( mat )
		{
			matname = mat->GetName();
		}

		Warning( "Warning! Static BSP decal (%s), on NULL model index %i for entity index %i.\n", 
			matname,
			msg->m_nModelIndex, 
			msg->m_nEntityIndex );
		return true;
	}

	if (r_decals.GetInt())
	{
		g_pEfx->DecalShoot( 
			msg->m_nDecalTextureIndex, 
			msg->m_nEntityIndex, 
			model, 
			vec3_origin, 
			vec3_angle,
			msg->m_Pos, 
			NULL, 
			msg->m_bLowPriority ? 0 : FDECAL_PERMANENT );
	}

	return true;
}


bool CClientState::ProcessGameEvent(SVC_GameEvent *msg)
{
	int startbit = msg->m_DataIn.GetNumBitsRead();

	IGameEvent *event = g_GameEventManager.UnserializeEvent( &msg->m_DataIn );

	int length = msg->m_DataIn.GetNumBitsRead() - startbit;

	if ( length != msg->m_nLength )
	{
		DevMsg("CClientState::ProcessGameEvent: KeyValue length mismatch.\n" );
		return true;
	}

	if ( !event )
	{
		DevMsg("CClientState::ProcessGameEvent: UnserializeKeyValue failed.\n" );
		return true;
	}

	g_GameEventManager.FireEventClientSide( event );

	return true;
}


bool CClientState::ProcessUserMessage(SVC_UserMessage *msg)
{
	// buffer for incoming user message
	byte userdata[ MAX_USER_MSG_DATA ] = { 0 };
	bf_read userMsg( "UserMessage(read)", userdata, sizeof( userdata ) );
	msg->m_DataIn.ReadBits( userdata, msg->m_nLength );
	userMsg.StartReading( userdata, Bits2Bytes( msg->m_nLength ) );

	// dispatch message to client.dll
	if ( !g_ClientDLL->DispatchUserMessage( msg->m_nMsgType, userMsg ) )
	{
		ConMsg( "Couldn't dispatch user message (%i)\n", msg->m_nMsgType );
		return false;
	}

	return true;
}


bool CClientState::ProcessEntityMessage(SVC_EntityMessage *msg)
{
	// Look up entity
	IClientNetworkable *entity = entitylist->GetClientNetworkable( msg->m_nEntityIndex );

	if ( !entity )
	{
		// message was send to use, even we don't have this entity TODO change that on server side
		return true;
	}

	// route to entity 
	MDLCACHE_CRITICAL_SECTION_( g_pMDLCache );

	// buffer for incoming user message
	byte entityData[ MAX_ENTITY_MSG_DATA ] = { 0 };
	bf_read entMsg( "EntityMessage(read)", entityData, sizeof( entityData ) );
	msg->m_DataIn.ReadBits( entityData, msg->m_nLength );
	entMsg.StartReading( entityData, Bits2Bytes( msg->m_nLength ) );

	entity->ReceiveMessage( msg->m_nClassID, entMsg );

	return true;
}


bool CClientState::ProcessPacketEntities( SVC_PacketEntities *msg )
{
	CL_PreprocessEntities(); // setup client prediction

	if ( !msg->m_bIsDelta )
	{
		// Delta too old or is initial message
#ifndef _XBOX			
		// we can start recording now that we've received an uncompressed packet
		demorecorder->SetSignonState( SIGNONSTATE_FULL );
#endif
		// Tell prediction that we're recreating entities due to an uncompressed packet arriving
		if ( g_pClientSidePrediction  )
		{
			g_pClientSidePrediction->OnReceivedUncompressedPacket();
		}
	}
	else
	{
		if ( m_nDeltaTick == -1  )
		{
			// we requested a full update but still got a delta compressed packet. ignore it.
			return true;
		}
	}
	
	TRACE_PACKET(( "CL Receive (%d <-%d)\n", m_nCurrentSequence, msg->m_nDeltaFrom ));
	TRACE_PACKET(( "CL Num Ents (%d)\n", msg->m_nUpdatedEntries ));

	if ( g_pLocalNetworkBackdoor )
	{
		if ( m_nSignonState == SIGNONSTATE_SPAWN  )
		{	
			// We are done with signon sequence.
			SetSignonState( SIGNONSTATE_FULL, m_nServerCount );
		}

		// ignore message, all entities are transmitted using fast local memcopy routines
		m_nDeltaTick = GetServerTickCount();
		return true;
	}
	
	if ( !CL_ProcessPacketEntities ( msg ) )
		return false;

	return CBaseClientState::ProcessPacketEntities( msg );
}


bool CClientState::ProcessTempEntities( SVC_TempEntities *msg )
{
	bool bReliable = false;

	float fire_time = cl.GetTime();

#ifndef _XBOX
	// delay firing temp ents by cl_interp in multiplayer or demoplayback
	if ( cl.m_nMaxClients > 1 || demoplayer->IsPlayingBack() )
	{
		float flInterpAmount = GetClientInterpAmount();
		fire_time += flInterpAmount;
	}
#endif

	if ( msg->m_nNumEntries == 0 )
	{
		bReliable = true;
		msg->m_nNumEntries = 1;
	}

	int flags = bReliable ? FEV_RELIABLE : 0;

	// Don't actually queue unreliable events if playing a demo and skipping ahead
#ifndef _XBOX
	if ( !bReliable && demoplayer->IsSkipping() )
	{
		return true;
	}
#endif
	bf_read &buffer = msg->m_DataIn; // shortcut

	int classID = -1;
	void *from = NULL;
	C_ServerClassInfo *pServerClass = NULL;
	ClientClass *pClientClass = NULL;
	unsigned char data[CEventInfo::MAX_EVENT_DATA];
	bf_write toBuf( data, sizeof(data) );
	CEventInfo *ei = NULL;
	
	for (int i = 0; i < msg->m_nNumEntries; i++ )
	{
		float delay = 0.0f;

		if ( buffer.ReadOneBit() )
		{
			delay = (float)buffer.ReadSBitLong( 8 ) / 100.0f;
		}

		toBuf.Reset();

		if ( buffer.ReadOneBit() )
		{
			from = NULL; // full update

			classID = buffer.ReadUBitLong( m_nServerClassBits ); // classID 
		
			// Look up the client class, etc.
	
			// Match the server classes to the client classes.
			pServerClass = m_pServerClasses ? &m_pServerClasses[ classID - 1 ] : NULL;

			if ( !pServerClass )
			{
				DevMsg("CL_QueueEvent: missing server class info for %i.\n", classID - 1 );
				return false;
			}

			// See if the client .dll has a handler for this class
			pClientClass = FindClientClass( pServerClass->m_ClassName );
		
			if ( !pClientClass || !pClientClass->m_pRecvTable )
			{
				DevMsg("CL_QueueEvent: missing client receive table for %s.\n", pServerClass->m_ClassName );
				return false;
			}

			RecvTable_MergeDeltas( pClientClass->m_pRecvTable, NULL, &buffer, &toBuf );
		}
		else
		{
			Assert( ei );

			bf_read fromBuf( ei->pData, Bits2Bytes(ei->bits) );
		
			RecvTable_MergeDeltas( pClientClass->m_pRecvTable, &fromBuf, &buffer, &toBuf );
		}

		// Add a slot
		ei = &cl.events[ cl.events.AddToTail() ];

		Assert( ei );

		int size = Bits2Bytes(toBuf.GetNumBitsWritten() );
		
		ei->classID			= classID;
		ei->fire_delay		= fire_time + delay;
		ei->flags			= flags;
		ei->pClientClass	= pClientClass;
		ei->bits			= toBuf.GetNumBitsWritten();
		
		ei->pData			= new byte[size]; // copy raw data
		Q_memcpy( ei->pData, data, size );
	}

	return true;
}

#endif // swds
