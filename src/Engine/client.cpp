//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include "client_pch.h"
#include "networkstringtabledefs.h"
#include <checksum_md5.h>
#include <iregistry.h>
#include "pure_server.h"
#include "netmessages.h"
#include "cl_demo.h"
#include "host_state.h"
#include "host.h"
#include "gl_matsysiface.h"
#include "vgui_baseui_interface.h"
#include "tier0/icommandline.h"
#include <proto_oob.h>
#include "checksum_engine.h"
#include "filesystem_engine.h"
#include "logofile_shared.h"
#include "sound.h"
#include "decal.h"
#include "networkstringtableclient.h"
#include "dt_send_eng.h"
#include "ents_shared.h"
#include "cl_ents_parse.h"
#include "cl_entityreport.h"
#include "MapReslistGenerator.h"
#include "DownloadListGenerator.h"
#include "GameEventManager.h"
#include "host_phonehome.h"
#include "vgui_baseui_interface.h"
#include "clockdriftmgr.h"
#include "snd_audio_source.h"
#include "vgui_controls/Controls.h"
#include "vgui/ILocalize.h"
#include "download.h"
#include "checksum_engine.h"
#include "ModelInfo.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/materialsystem_config.h"
#include "tier1/fmtstr.h"
#include "steam/steam_api.h"
#include "matchmaking.h"

#include "tier0/platform.h"
#include "tier0/systeminformation.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar cl_timeout( "cl_timeout", "30", FCVAR_ARCHIVE, "After this many seconds without receiving a packet from the server, the client will disconnect itself" );
static ConVar cl_logofile( "cl_logofile", "materials/decals/spraylogo.vtf", FCVAR_ARCHIVE, "Spraypoint logo decal." ); // TODO must be more generic
static ConVar cl_soundfile( "cl_soundfile", "sound/player/jingle.wav", FCVAR_ARCHIVE, "Jingle sound file." );
static ConVar cl_forcepreload( "cl_forcepreload", "0", FCVAR_ARCHIVE, "Whether we should force preloading.");
static ConVar cl_allowdownload ( "cl_allowdownload", "1", FCVAR_ARCHIVE, "Client downloads customization files" );
static ConVar cl_downloadfilter( "cl_downloadfilter", "all", FCVAR_ARCHIVE, "Determines which files can be downloaded from the server (all, none, nosounds)" );

extern ConVar sv_downloadurl;
extern ConVar sv_consistency;

extern bool g_bServerGameDLLGreaterThanV5;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CClientState::CClientState()
{
	m_bMarkedCRCsUnverified = false;
	demonum = -1;
	m_tickRemainder = 0;
	m_frameTime = 0;
	m_pAreaBits = NULL;
	m_hWaitForResourcesHandle = NULL;
	m_bUpdateSteamResources = false;
	m_bShownSteamResourceUpdateProgress = false;
	m_pPureServerWhitelist = NULL;
	m_bCheckCRCsWithServer = false;
	m_flLastCRCBatchTime = 0;
	m_nFriendsID = 0;
	m_FriendsName[0] = 0;
}

CClientState::~CClientState()
{
	if ( m_pPureServerWhitelist )
		m_pPureServerWhitelist->Release();
}

// HL1 CD Key
#define GUID_LEN 13

/*
=======================
CL_GetCDKeyHash()

Connections will now use a hashed cd key value
A LAN server will know not to allows more then xxx users with the same CD Key
=======================
*/
const char *CClientState::GetCDKeyHash( void )
{
	if ( IsPC() )
	{
		char szKeyBuffer[256]; // Keys are about 13 chars long.	
		static char szHashedKeyBuffer[64];
		int nKeyLength;
		bool bDedicated = false;

		MD5Context_t ctx;
		unsigned char digest[16]; // The MD5 Hash

		nKeyLength = Q_snprintf( szKeyBuffer, sizeof( szKeyBuffer ), "%s", registry->ReadString( "key", "" ) );

		if (bDedicated)
		{
			ConMsg("Key has no meaning on dedicated server...\n");
			return "";
		}

		if ( nKeyLength == 0 )
		{
			nKeyLength = 13;
			Q_strncpy( szKeyBuffer, "1234567890123", sizeof( szKeyBuffer ) );
			Assert( Q_strlen( szKeyBuffer ) == nKeyLength );

			DevMsg( "Missing CD Key from registry, inserting blank key\n" );

			registry->WriteString( "key", szKeyBuffer );
		}

		if (nKeyLength <= 0 ||
			nKeyLength >= 256 )
		{
			ConMsg("Bogus key length on CD Key...\n");
			return "";
		}

		// Now get the md5 hash of the key
		memset( &ctx, 0, sizeof( ctx ) );
		memset( digest, 0, sizeof( digest ) );
		
		MD5Init(&ctx);
		MD5Update(&ctx, (unsigned char*)szKeyBuffer, nKeyLength);
		MD5Final(digest, &ctx);
		Q_strncpy ( szHashedKeyBuffer, MD5_Print ( digest, sizeof( digest ) ), sizeof( szHashedKeyBuffer ) );
		return szHashedKeyBuffer;
	}

	return "12345678901234567890123456789012";
}

void CClientState::SendClientInfo( void )
{
	CLC_ClientInfo info;
	
	info.m_nSendTableCRC = SendTable_GetCRC();
	info.m_nServerCount = m_nServerCount;
	info.m_bIsHLTV = false;
#if !defined( NO_STEAM )
	info.m_nFriendsID = SteamUser() ? SteamUser()->GetSteamID().GetAccountID() : 0;
#else
	info.m_nFriendsID = 0;
#endif
	Q_strncpy( info.m_FriendsName, m_FriendsName, sizeof(info.m_FriendsName) );

	CheckOwnCustomFiles(); // load & verfiy custom player files

	for ( int i=0; i< MAX_CUSTOM_FILES; i++ )
		info.m_nCustomFiles[i] = m_nCustomFiles[i].crc;
	
	m_NetChannel->SendNetMsg( info );
}

extern IVEngineClient *engineClient;

//-----------------------------------------------------------------------------
// Purpose: A svc_signonnum has been received, perform a client side setup
// Output : void CL_SignonReply
//-----------------------------------------------------------------------------
bool CClientState::SetSignonState ( int state, int count )
{
	if ( !CBaseClientState::SetSignonState( state, count ) )
	{
		CL_Retry();
		return false;
	}

	// ConDMsg ("Signon state: %i\n", state );

	COM_TimestampedLog( "CClientState::SetSignonState: start %i", state );

	switch ( m_nSignonState )
	{
		case SIGNONSTATE_CHALLENGE	:	
			m_bMarkedCRCsUnverified = false;	// Remember that we just connected to a new server so it'll 
												// reverify any necessary file CRCs on this server.
			EngineVGui()->UpdateProgressBar(PROGRESS_SIGNONCHALLENGE);
			break;

		case SIGNONSTATE_CONNECTED :	
			{
				EngineVGui()->UpdateProgressBar(PROGRESS_SIGNONCONNECTED);
				
				// make sure it's turned off when connecting
				EngineVGui()->HideDebugSystem();

				SCR_BeginLoadingPlaque ();
				// Clear channel and stuff
				m_NetChannel->Clear();

				// allow longer timeout
				m_NetChannel->SetTimeout( SIGNON_TIME_OUT );
				m_NetChannel->SetMaxBufferSize( true, NET_MAX_PAYLOAD );
				
				// set user settings (rate etc)
				NET_SetConVar convars;
				Host_BuildConVarUpdateMessage( &convars, FCVAR_USERINFO, false );
				m_NetChannel->SendNetMsg( convars );
			}
			break;

		case SIGNONSTATE_NEW :	
			{
				EngineVGui()->UpdateProgressBar(PROGRESS_SIGNONNEW);

				if ( IsPC() && !demoplayer->IsPlayingBack() )
				{
					// start making sure we have all the specified resources
					StartUpdatingSteamResources();
				}
				else
				{
					// during demo playback dont try to download resource
					FinishSignonState_New();
				}

				// don't tell the server yet that we've entered this state
				return true;
			}
			break;

		case SIGNONSTATE_PRESPAWN	:
			m_nSoundSequence = 1;	// reset sound sequence number after receiving signon sounds
			break;
		
		case SIGNONSTATE_SPAWN :
			{
				Assert( g_ClientDLL );

				EngineVGui()->UpdateProgressBar(PROGRESS_SIGNONSPAWN);

				// Tell client .dll about the transition
				char mapname[256];
				CL_SetupMapName( modelloader->GetName( host_state.worldmodel ), mapname, sizeof( mapname ) );

				COM_TimestampedLog( "LevelInitPreEntity: start", state );
				g_ClientDLL->LevelInitPreEntity(mapname);
				COM_TimestampedLog( "LevelInitPreEntity: end", state );

				phonehome->Message( IPhoneHome::PHONE_MSG_MAPSTART, mapname );

				audiosourcecache->LevelInit( mapname );

				// stop recording demo header
				demorecorder->SetSignonState( SIGNONSTATE_SPAWN );
			}
			break;

		case SIGNONSTATE_FULL:
			{
				CL_FullyConnected();
				m_NetChannel->SetTimeout( cl_timeout.GetFloat() );
				m_NetChannel->SetMaxBufferSize( true, NET_MAX_DATAGRAM_PAYLOAD );

				HostState_OnClientConnected();
				
				if ( m_nMaxClients > 1 )
				{
					g_pMatchmaking->AddLocalPlayersToTeams();
				}
			}
			break;

		case SIGNONSTATE_CHANGELEVEL:	
			m_NetChannel->SetTimeout( SIGNON_TIME_OUT );  // allow 5 minutes timeout
			if ( m_nMaxClients > 1 )
			{
				// start progress bar immediately for multiplayer level transitions
				EngineVGui()->EnabledProgressBarForNextLoad();
			}
			SCR_BeginLoadingPlaque();
			if ( m_nMaxClients > 1 )
			{
				EngineVGui()->UpdateProgressBar(PROGRESS_CHANGELEVEL);
			}
			break;
	}

	COM_TimestampedLog( "CClientState::SetSignonState: end %i", state );

	if ( state >= SIGNONSTATE_CONNECTED )
	{
		// tell server that we entered now that state
		m_NetChannel->SendNetMsg( NET_SignonState( state, count) );
	}

	return true;
}

bool CClientState::HookClientStringTable( char const *tableName )
{
	INetworkStringTable *table = GetStringTable( tableName );
	if ( !table )
	{
		// If engine takes a pass, allow client dll to hook in its callbacks
		if ( g_ClientDLL )
		{
			g_ClientDLL->InstallStringTableCallback( tableName );
		}
        return false;
	}

	char szDownloadableFileTablename[255] = DOWNLOADABLE_FILE_TABLENAME;
	char szModelPrecacheTablename[255] = MODEL_PRECACHE_TABLENAME;
	char szGenericPrecacheTablename[255] = GENERIC_PRECACHE_TABLENAME;
	char szSoundPrecacheTablename[255] = SOUND_PRECACHE_TABLENAME;
	char szDecalPrecacheTablename[255] = DECAL_PRECACHE_TABLENAME;

	// This was added into staging at some point and is not enabled in main or rel.
	if ( 0 )
	{
		Q_snprintf( szDownloadableFileTablename, 255, ":%s", DOWNLOADABLE_FILE_TABLENAME );
		Q_snprintf( szModelPrecacheTablename, 255, ":%s", MODEL_PRECACHE_TABLENAME );
		Q_snprintf( szGenericPrecacheTablename, 255, ":%s", GENERIC_PRECACHE_TABLENAME );
		Q_snprintf( szSoundPrecacheTablename, 255, ":%s", SOUND_PRECACHE_TABLENAME );
		Q_snprintf( szDecalPrecacheTablename, 255, ":%s", DECAL_PRECACHE_TABLENAME );		
	}

	// Hook Model Precache table
	if ( !Q_strcasecmp( tableName, szModelPrecacheTablename ) )
	{
		m_pModelPrecacheTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, szGenericPrecacheTablename ) )
	{
		m_pGenericPrecacheTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, szSoundPrecacheTablename ) )
	{
		m_pSoundPrecacheTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, szDecalPrecacheTablename ) )
	{
		// Cache the id
		m_pDecalPrecacheTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, INSTANCE_BASELINE_TABLENAME ) )
	{
		// Cache the id
		m_pInstanceBaselineTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, LIGHT_STYLES_TABLENAME ) )
	{
		// Cache the id
		m_pLightStyleTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, USER_INFO_TABLENAME ) )
	{
		// Cache the id
		m_pUserInfoTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, SERVER_STARTUP_DATA_TABLENAME ) )
	{
		// Cache the id
		m_pServerStartupTable = table;
		return true;
	}

	if ( !Q_strcasecmp( tableName, szDownloadableFileTablename ) )
	{
		// Cache the id
		m_pDownloadableFileTable = table;
		return true;
	}

	// If engine takes a pass, allow client dll to hook in its callbacks
	g_ClientDLL->InstallStringTableCallback( tableName );

	return false;
}

bool CClientState::InstallEngineStringTableCallback( char const *tableName )
{
	INetworkStringTable *table = GetStringTable( tableName );

	if ( !table )
		return false;

	char szDownloadableFileTablename[255] = DOWNLOADABLE_FILE_TABLENAME;
	char szModelPrecacheTablename[255] = MODEL_PRECACHE_TABLENAME;
	char szGenericPrecacheTablename[255] = GENERIC_PRECACHE_TABLENAME;
	char szSoundPrecacheTablename[255] = SOUND_PRECACHE_TABLENAME;
	char szDecalPrecacheTablename[255] = DECAL_PRECACHE_TABLENAME;

	// This was added into staging at some point and is not enabled in main or rel.
	if ( 0 )
	{
		Q_snprintf( szDownloadableFileTablename, 255, ":%s", DOWNLOADABLE_FILE_TABLENAME );
		Q_snprintf( szModelPrecacheTablename, 255, ":%s", MODEL_PRECACHE_TABLENAME );
		Q_snprintf( szGenericPrecacheTablename, 255, ":%s", GENERIC_PRECACHE_TABLENAME );
		Q_snprintf( szSoundPrecacheTablename, 255, ":%s", SOUND_PRECACHE_TABLENAME );
		Q_snprintf( szDecalPrecacheTablename, 255, ":%s", DECAL_PRECACHE_TABLENAME );		
	}


	// Hook Model Precache table
	if ( !Q_strcasecmp( tableName, szModelPrecacheTablename ) )
	{
		table->SetStringChangedCallback( NULL, Callback_ModelChanged );
		return true;
	}

	if ( !Q_strcasecmp( tableName, szGenericPrecacheTablename ) )
	{
		// Install the callback
		table->SetStringChangedCallback( NULL, Callback_GenericChanged );
		return true;
	}

	if ( !Q_strcasecmp( tableName, szSoundPrecacheTablename ) )
	{
		// Install the callback
		table->SetStringChangedCallback( NULL, Callback_SoundChanged );
		return true;
	}

	if ( !Q_strcasecmp( tableName, szDecalPrecacheTablename ) )
	{
		// Install the callback
		table->SetStringChangedCallback( NULL, Callback_DecalChanged );
		return true;
	}

	if ( !Q_strcasecmp( tableName, INSTANCE_BASELINE_TABLENAME ) )
	{
		// Install the callback (already done above)
		table->SetStringChangedCallback( NULL, Callback_InstanceBaselineChanged );
		return true;
	}

	if ( !Q_strcasecmp( tableName, LIGHT_STYLES_TABLENAME ) )
	{
		return true;
	}

	if ( !Q_strcasecmp( tableName, USER_INFO_TABLENAME ) )
	{
		// Install the callback
		table->SetStringChangedCallback( NULL, Callback_UserInfoChanged );
		return true;
	}

	if ( !Q_strcasecmp( tableName, SERVER_STARTUP_DATA_TABLENAME ) )
	{
		return true;
	}

	if ( !Q_strcasecmp( tableName, szDownloadableFileTablename ) )
	{
		return true;
	}

	// The the client.dll have a shot at it
	return false;
}

void CClientState::InstallStringTableCallback( char const *tableName )
{
	// Let engine hook callbacks before we read in any data values at all
	if ( !InstallEngineStringTableCallback( tableName ) )
	{
		// If engine takes a pass, allow client dll to hook in its callbacks
		g_ClientDLL->InstallStringTableCallback( tableName );
	}
}

bool CClientState::IsPaused() const
{
	return m_bPaused || ( g_LostVideoMemory && Host_IsSinglePlayerGame() ) ||
		!host_initialized || 
		demoplayer->IsPlaybackPaused() ||
		EngineVGui()->ShouldPause();
}

float CClientState::GetTime() const
{
	int nTickCount = GetClientTickCount();
	float flTickTime = nTickCount * host_state.interval_per_tick;
	
	// Timestamps are rounded to exact tick during simulation
	if ( insimulation )
	{
		return flTickTime;
	}

	return flTickTime + m_tickRemainder;
}

float CClientState::GetFrameTime() const
{
	if ( CClockDriftMgr::IsClockCorrectionEnabled() )
	{
		return IsPaused() ? 0 : m_frameTime;
	}
	else
	{
		if ( insimulation )
		{
			int nElapsedTicks = ( GetClientTickCount() - oldtickcount );
			return nElapsedTicks * host_state.interval_per_tick;
		}
		else
		{
			return IsPaused() ? 0 : m_frameTime;
		}
	}
}

float CClientState::GetClientInterpAmount()
{
	// we need client cvar cl_interp_ratio
	static const ConVar *s_cl_interp_ratio = NULL;
	if ( !s_cl_interp_ratio )
	{
		s_cl_interp_ratio = g_pCVar->FindVar( "cl_interp_ratio" );
		if ( !s_cl_interp_ratio )
			return 0.1f;
	}
	static const ConVar *s_cl_interp = NULL;
	if ( !s_cl_interp )
	{
		s_cl_interp = g_pCVar->FindVar( "cl_interp" );
		if ( !s_cl_interp )
			return 0.1f;
	}
		
	float flInterpRatio = s_cl_interp_ratio->GetFloat();
	float flInterp = s_cl_interp->GetFloat();

	const ConVar_ServerBounded *pBounded = dynamic_cast<const ConVar_ServerBounded*>( s_cl_interp_ratio );
	if ( pBounded )
		flInterpRatio = pBounded->GetFloat();
	//#define FIXME_INTERP_RATIO
	return max( flInterpRatio / cl_updaterate->GetFloat(), flInterp );
}

//-----------------------------------------------------------------------------
// Purpose: // Clear all the variables in the CClientState.
//-----------------------------------------------------------------------------
void CClientState::Clear( void )
{
	CBaseClientState::Clear();

	m_pModelPrecacheTable = NULL;
	m_pGenericPrecacheTable = NULL;
	m_pSoundPrecacheTable = NULL;
	m_pDecalPrecacheTable = NULL;
	m_pInstanceBaselineTable = NULL;
	m_pLightStyleTable = NULL;
	m_pUserInfoTable = NULL;
	m_pServerStartupTable = NULL;
	m_pAreaBits = NULL;
	
	// Clear all download vars.
	m_pDownloadableFileTable = NULL;
	m_hWaitForResourcesHandle = NULL;
	m_bUpdateSteamResources = false;
	m_bShownSteamResourceUpdateProgress = false;
	m_bDownloadResources = false;

	DeleteClientFrames( -1 ); // clear all
		
	viewangles.Init();
	m_flLastServerTickTime = 0.0f;
	oldtickcount = 0;
	insimulation = false;


	addangle.RemoveAll();
	addangletotal = 0.0f;
	prevaddangletotal = 0.0f;

	memset(model_precache, 0, sizeof(model_precache));
	memset(sound_precache, 0, sizeof(sound_precache));
	ishltv = false;
	cdtrack = 0;
	serverCRC = 0;
	serverClientSideDllCRC = 0;
	last_command_ack = 0;
	command_ack = 0;
	m_nSoundSequence = 0;

	// make sure the client isn't active anymore, but stay
	// connected if we are.
	if ( m_nSignonState > SIGNONSTATE_CONNECTED )
	{
		m_nSignonState = SIGNONSTATE_CONNECTED;
	}
}

void CClientState::ClearSounds()
{
	int c = ARRAYSIZE( sound_precache );
	for ( int i = 0; i < c; ++i )
	{
		sound_precache[ i ].SetSound( NULL );
	}
}

bool CClientState::ProcessConnectionlessPacket( netpacket_t *packet )
{
	Assert( packet );

	return CBaseClientState::ProcessConnectionlessPacket( packet );
}

void CClientState::FullConnect( netadr_t &adr )
{
	CBaseClientState::FullConnect( adr );
	m_NetChannel->SetDemoRecorder( g_pClientDemoRecorder );
	m_NetChannel->SetDataRate( cl_rate->GetFloat() );

	// Not in the demo loop now
	demonum = -1;		
	
	// We don't have a backed up cmd history yet
	lastoutgoingcommand = -1;

	// we didn't send commands yet
	chokedcommands = 0;
	
	// Report connection success.
	if ( Q_stricmp("loopback", adr.ToString() ) )
	{
		ConMsg( "Connected to %s\n", adr.ToString() );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
// Output : model_t
//-----------------------------------------------------------------------------
model_t *CClientState::GetModel( int index )
{
	if ( !m_pModelPrecacheTable )
	{
		return NULL;
	}

	if ( index <= 0 )
	{
		return NULL;
	}

	if ( index >= m_pModelPrecacheTable->GetNumStrings() )
	{
		Assert( 0 ); // model index for unkown model requested
		return NULL;
	}

	CPrecacheItem *p = &model_precache[ index ];
	model_t *m = p->GetModel();
	if ( m )
	{
		return m;
	}

	char const *name = m_pModelPrecacheTable->GetString( index );

	if ( host_showcachemiss.GetBool() )
	{
		ConDMsg( "client model cache miss on %s\n", name );
	}

	m = modelloader->GetModelForName( name, IModelLoader::FMODELLOADER_CLIENT );
	if ( !m )
	{
		const CPrecacheUserData *data = CL_GetPrecacheUserData( m_pModelPrecacheTable, index );
		if ( data && ( data->flags & RES_FATALIFMISSING ) )
		{
			COM_ExplainDisconnection( true, "Cannot continue without model %s, disconnecting\n", name );
			Host_Disconnect(true);
		}
	}

	p->SetModel( m );
	return m;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *name - 
// Output : int -- note -1 if missing
//-----------------------------------------------------------------------------
int CClientState::LookupModelIndex( char const *name )
{
	if ( !m_pModelPrecacheTable )
	{
		return -1;
	}
	int idx = m_pModelPrecacheTable->FindStringIndex( name );
	return ( idx == INVALID_STRING_INDEX ) ? -1 : idx;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
//			*name - 
//-----------------------------------------------------------------------------
void CClientState::SetModel( int tableIndex )
{
	if ( !m_pModelPrecacheTable )
	{
		return;
	}

	// Bogus index
	if ( tableIndex < 0 || tableIndex >= m_pModelPrecacheTable->GetNumStrings() )
	{
		return;
	}

	CPrecacheItem *p = &model_precache[ tableIndex ];
	const CPrecacheUserData *data = CL_GetPrecacheUserData( m_pModelPrecacheTable, tableIndex );

	bool bLoadNow = ( data && ( data->flags & RES_PRELOAD ) ) || IsX360();
	if ( CommandLine()->FindParm( "-nopreload" ) ||	CommandLine()->FindParm( "-nopreloadmodels" ))
	{
		bLoadNow = false;
	}
	else if ( cl_forcepreload.GetInt() || CommandLine()->FindParm( "-preload" ) )
	{
		bLoadNow = true;
	}

	if ( bLoadNow )
	{
		char const *name = m_pModelPrecacheTable->GetString( tableIndex );
		p->SetModel( modelloader->GetModelForName( name, IModelLoader::FMODELLOADER_CLIENT ) );
	}
	else
	{
		p->SetModel( NULL );
	}

	// log the file reference, if necssary
	if (MapReslistGenerator().IsEnabled())
	{
		char const *name = m_pModelPrecacheTable->GetString( tableIndex );
		MapReslistGenerator().OnModelPrecached( name );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
// Output : model_t
//-----------------------------------------------------------------------------
char const *CClientState::GetGeneric( int index )
{
	if ( !m_pGenericPrecacheTable )
	{
		Warning( "Can't GetGeneric( %d ), no precache table [no level loaded?]\n", index );
		return "";
	}

	if ( index <= 0 )
		return "";

	if ( index >= m_pGenericPrecacheTable->GetNumStrings() )
	{
		return "";
	}

	CPrecacheItem *p = &generic_precache[ index ];
	char const *g = p->GetGeneric();
	return g;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *name - 
// Output : int -- note -1 if missing
//-----------------------------------------------------------------------------
int CClientState::LookupGenericIndex( char const *name )
{
	if ( !m_pGenericPrecacheTable )
	{
		Warning( "Can't LookupGenericIndex( %s ), no precache table [no level loaded?]\n", name );
		return -1;
	}
	int idx = m_pGenericPrecacheTable->FindStringIndex( name );
	return ( idx == INVALID_STRING_INDEX ) ? -1 : idx;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
//			*name - 
//-----------------------------------------------------------------------------
void CClientState::SetGeneric( int tableIndex )
{
	if ( !m_pGenericPrecacheTable )
	{
		Warning( "Can't SetGeneric( %d ), no precache table [no level loaded?]\n", tableIndex );
		return;
	}
	// Bogus index
	if ( tableIndex < 0 || 
		 tableIndex >= m_pGenericPrecacheTable->GetNumStrings() )
	{
		return;
	}

	char const *name = m_pGenericPrecacheTable->GetString( tableIndex );
	CPrecacheItem *p = &generic_precache[ tableIndex ];
	p->SetGeneric( name );
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
// Output : char const
//-----------------------------------------------------------------------------
char const *CClientState::GetSoundName( int index )
{
	if ( index <= 0 || !m_pSoundPrecacheTable )
		return "";

	if ( index >= m_pSoundPrecacheTable->GetNumStrings() )
	{
		return "";
	}

	char const *name = m_pSoundPrecacheTable->GetString( index );
	return name;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
// Output : model_t
//-----------------------------------------------------------------------------
CSfxTable *CClientState::GetSound( int index )
{
	if ( index <= 0 || !m_pSoundPrecacheTable )
		return NULL;

	if ( index >= m_pSoundPrecacheTable->GetNumStrings() )
	{
		return NULL;
	}

	CPrecacheItem *p = &sound_precache[ index ];
	CSfxTable *s = p->GetSound();
	if ( s )
		return s;

	char const *name = m_pSoundPrecacheTable->GetString( index );

	if ( host_showcachemiss.GetBool() )
	{
		ConDMsg( "client sound cache miss on %s\n", name );
	}

	s = S_PrecacheSound( name );

	p->SetSound( s );
	return s;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *name - 
// Output : int -- note -1 if missing
//-----------------------------------------------------------------------------
int CClientState::LookupSoundIndex( char const *name )
{
	if ( !m_pSoundPrecacheTable )
		return -1;

	int idx = m_pSoundPrecacheTable->FindStringIndex( name );
	return ( idx == INVALID_STRING_INDEX ) ? -1 : idx;
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
//			*name - 
//-----------------------------------------------------------------------------
void CClientState::SetSound( int tableIndex )
{
	// Bogus index
	if ( !m_pSoundPrecacheTable )
		return;

	if ( tableIndex < 0 || tableIndex >= m_pSoundPrecacheTable->GetNumStrings() )
	{
		return;
	}

	CPrecacheItem *p = &sound_precache[ tableIndex ];
	const CPrecacheUserData *data = CL_GetPrecacheUserData( m_pSoundPrecacheTable, tableIndex );

	bool bLoadNow = ( data && ( data->flags & RES_PRELOAD ) ) || IsX360();
	if ( CommandLine()->FindParm( "-nopreload" ) ||	CommandLine()->FindParm( "-nopreloadsounds" ))
	{
		bLoadNow = false;
	}
	else if ( cl_forcepreload.GetInt() || CommandLine()->FindParm( "-preload" ) )
	{
		bLoadNow = true;
	}

	if ( bLoadNow )
	{
		char const *name = m_pSoundPrecacheTable->GetString( tableIndex );
		p->SetSound( S_PrecacheSound( name ) );
	}
	else
	{
		p->SetSound( NULL );
	}

	// log the file reference, if necssary
	if (MapReslistGenerator().IsEnabled())
	{
		char const *name = m_pSoundPrecacheTable->GetString( tableIndex );
		MapReslistGenerator().OnSoundPrecached( name );
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
// Output : model_t
//-----------------------------------------------------------------------------
char const *CClientState::GetDecalName( int index )
{
	if ( index <= 0 || !m_pDecalPrecacheTable )
	{
		return NULL;
	}

	if ( index >= m_pDecalPrecacheTable->GetNumStrings() )
	{
		return NULL;
	}

	CPrecacheItem *p = &decal_precache[ index ];
	char const *d = p->GetDecal();
	return d;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : index - 
//			*name - 
//-----------------------------------------------------------------------------
void CClientState::SetDecal( int tableIndex )
{
	if ( !m_pDecalPrecacheTable )
		return;

	if ( tableIndex < 0 || 
		 tableIndex >= m_pDecalPrecacheTable->GetNumStrings() )
	{
		return;
	}

	char const *name = m_pDecalPrecacheTable->GetString( tableIndex );
	CPrecacheItem *p = &decal_precache[ tableIndex ];
	p->SetDecal( name );

	Draw_DecalSetName( tableIndex, (char *)name );
}


//-----------------------------------------------------------------------------
// Purpose: sets friends info locally to be sent to other users
//-----------------------------------------------------------------------------
void CClientState::SetFriendsID( uint friendsID, const char *friendsName )
{
	m_nFriendsID = friendsID;
	Q_strncpy( m_FriendsName, friendsName, sizeof(m_FriendsName) );
}


void CClientState::CheckOthersCustomFile( CRC32_t crcValue )
{
	if ( crcValue == 0 )
		return; // not a valid custom file

	if ( !cl_allowdownload.GetBool() )
		return; // client doesn't want to download anything

	CCustomFilename filehex( crcValue );

	if ( g_pFileSystem->FileExists( filehex.m_Filename ) )
		return; // we already have this file (assuming the CRC is correct)

	// we don't have it, request download from server
	m_NetChannel->RequestFile( filehex.m_Filename );
}

void CClientState::AddCustomFile( int slot, const char *resourceFile)
{
	if ( Q_strlen(resourceFile) <= 0 )
		return; // no resource file given

	if ( !COM_IsValidPath( resourceFile ) )
	{
		Msg("Customization file '%s' has invalid path.\n", resourceFile  );
		return;
	}

	if ( slot < 0 || slot >= MAX_CUSTOM_FILES )
		return; // wrong slot

	if ( !g_pFileSystem->FileExists( resourceFile ) )
	{
		DevMsg("Couldn't find customization file '%s'.\n", resourceFile );
		return; // resource file doesn't exits
	}

	if ( g_pFileSystem->Size( resourceFile ) > MAX_CUSTOM_FILE_SIZE )
	{
		Msg("Customization file '%s' is too big ( >%i bytes).\n", resourceFile, MAX_CUSTOM_FILE_SIZE );
		return; // resource file doesn't exits
	}

	CRC32_t crcValue;

	// Compute checksum of resource file
	CRC_File( &crcValue, resourceFile );

	// Copy it into materials/downloads if it's not there yet, so the server doesn't have to 
	// transmit the file back to us.
	bool bCopy = true;
	CCustomFilename filehex( crcValue );
	if ( g_pFileSystem->FileExists( filehex.m_Filename ) )
	{
		// check if existing file already has same CRC, 
		// then we don't need to copy it anymore
		CRC32_t test;
		CRC_File( &test, filehex.m_Filename );
		if ( test == crcValue )
			bCopy = false;
	}

	if ( bCopy )
	{
		// Copy it over under the new name
		COM_CopyFile( resourceFile, filehex.m_Filename );

		if ( !g_pFileSystem->FileExists( filehex.m_Filename ) )
		{
			Warning( "CacheCustomFiles: can't copy '%s' to '%s'.\n", resourceFile, filehex.m_Filename );
			return;
		}
	}

	/* Finally, validate the VTF file. TODO
	CUtlVector<char> fileData;
	if ( LogoFile_ReadFile( crcValue, fileData ) )
	{
		bValid = true;
	}
	else
	{
		Warning( "CL_LogoFile_OnConnect: logo file '%s' invalid.\n", logotexture );
	} */

	m_nCustomFiles[slot].crc = crcValue; // first slot is logo
	m_nCustomFiles[slot].reqID = 0;

}

void CClientState::CheckOwnCustomFiles()
{
	// clear file CRCs
	Q_memset( m_nCustomFiles, 0, sizeof(m_nCustomFiles) );
	
	if ( m_nMaxClients == 1 )
		return;	// not in singleplayer

	if ( IsPC() )
	{
		AddCustomFile( 0, cl_logofile.GetString() );
		AddCustomFile( 1, cl_soundfile.GetString() );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CClientState::DumpPrecacheStats( const char * name )
{
	if ( !name || !name[0] )
	{
		ConMsg( "Can only dump stats when active in a level\n" );
		return;
	}

	CPrecacheItem *items = NULL;
	
	if ( !Q_strcmp(MODEL_PRECACHE_TABLENAME, name ) )
	{
		items = model_precache;
	}
	else if ( !Q_strcmp(GENERIC_PRECACHE_TABLENAME, name ) )
	{
		items = generic_precache;
	}
	else if ( !Q_strcmp(SOUND_PRECACHE_TABLENAME, name ) )
	{
		items = sound_precache;
	}
	else if ( !Q_strcmp(DECAL_PRECACHE_TABLENAME, name ) )
	{
		items = decal_precache;
	}

	INetworkStringTable *table = GetStringTable( name );

	if ( !items || !table)
	{
		ConMsg( "Precache table '%s' not found.\n", name );
		return;
	}

	int count =  table->GetNumStrings();
	int maxcount = table->GetMaxStrings();

	ConMsg( "\n" );
	ConMsg( "Precache table %s:  %i of %i slots used\n", table->GetTableName(),
		count, maxcount );

	for ( int i = 0; i < count; i++ )
	{
		char const *name = table->GetString( i );
		CPrecacheItem *slot = &items[ i ];
		const CPrecacheUserData *p = CL_GetPrecacheUserData( table, i );

		if ( !name || !slot || !p )
			continue;

		ConMsg( "%03i:  %s (%s):   ",
			i,
			name, 
			GetFlagString( p->flags ) );


		if ( slot->GetReferenceCount() == 0 )
		{
			ConMsg( " never used\n" );
		}
		else
		{
			ConMsg( " %i refs, first %.2f mru %.2f\n",
				slot->GetReferenceCount(), 
				slot->GetFirstReference(), 
				slot->GetMostRecentReference() );
		}
	}

	ConMsg( "\n" );
}

void CClientState::ReadDeletions( CEntityReadInfo &u )
{
	VPROF( "ReadDeletions" );
	while ( u.m_pBuf->ReadOneBit()!=0 )
	{
		int idx = u.m_pBuf->ReadUBitLong( MAX_EDICT_BITS );	
		
		Assert( !u.m_pTo->transmit_entity.Get( idx ) );

		CL_DeleteDLLEntity( idx, "ReadDeletions" );
	}
}

void CClientState::ReadEnterPVS( CEntityReadInfo &u )
{
	VPROF( "ReadEnterPVS" );

	TRACE_PACKET(( "  CL Enter PVS (%d)\n", u.m_nNewEntity ));

	int iClass = u.m_pBuf->ReadUBitLong( m_nServerClassBits );

	int iSerialNum = u.m_pBuf->ReadUBitLong( NUM_NETWORKED_EHANDLE_SERIAL_NUMBER_BITS );

	CL_CopyNewEntity( u, iClass, iSerialNum );

	if ( u.m_nNewEntity == u.m_nOldEntity ) // that was a recreate
		u.NextOldEntity();
}

void CClientState::ReadLeavePVS( CEntityReadInfo &u )
{
	VPROF( "ReadLeavePVS" );
	// Sanity check.
	if ( !u.m_bAsDelta )
	{
		Assert(0); // cl.validsequence = 0;
		ConMsg( "WARNING: LeavePVS on full update" );
		u.m_UpdateType = Failed;	// break out
		return;
	}

	Assert( !u.m_pTo->transmit_entity.Get( u.m_nOldEntity ) );

	if ( u.m_UpdateFlags & FHDR_DELETE )
	{
		CL_DeleteDLLEntity( u.m_nOldEntity, "ReadLeavePVS" );
	}

	u.NextOldEntity();
}

void CClientState::ReadDeltaEnt( CEntityReadInfo &u )
{
	VPROF( "ReadDeltaEnt" );
	CL_CopyExistingEntity( u );
	
	u.NextOldEntity();
}

void CClientState::ReadPreserveEnt( CEntityReadInfo &u )
{
	VPROF( "ReadPreserveEnt" );
	if ( !u.m_bAsDelta )  // Should never happen on a full update.
	{
		Assert(0); // cl.validsequence = 0;
		ConMsg( "WARNING: PreserveEnt on full update" );
		u.m_UpdateType = Failed;	// break out
		return;
	}

	Assert( u.m_pFrom->transmit_entity.Get(u.m_nOldEntity) );
	
	// copy one of the old entities over to the new packet unchanged
	if ( u.m_nNewEntity >= MAX_EDICTS )
	{
		Host_Error ("CL_ReadPreserveEnt: u.m_nNewEntity == MAX_EDICTS");
	}
	
	u.m_pTo->last_entity = u.m_nOldEntity;
	u.m_pTo->transmit_entity.Set( u.m_nOldEntity );

	// Zero overhead
	if ( cl_entityreport.GetBool() )
		CL_RecordEntityBits( u.m_nOldEntity, 0 );

	u.NextOldEntity();
}


//-----------------------------------------------------------------------------
// Purpose: Starts checking that all the necessary files are local
//-----------------------------------------------------------------------------
void CClientState::StartUpdatingSteamResources()
{
	if ( IsX360() )
	{
		return;
	}

	// we can only do this when in SIGNONSTATE_NEW, 
	// since the completion of this triggers the continuation of SIGNONSTATE_NEW
	Assert(m_nSignonState == SIGNONSTATE_NEW);

	// make sure we have all the necessary resources locally before continuing
	m_hWaitForResourcesHandle = g_pFileSystem->WaitForResources(m_szLevelNameShort);
	m_bUpdateSteamResources = true;
	m_bShownSteamResourceUpdateProgress = false;
	m_bDownloadResources = false;
}

//-----------------------------------------------------------------------------
// Purpose: checks to see if we're done updating files
//-----------------------------------------------------------------------------
void CClientState::CheckUpdatingSteamResources()
{
	if ( IsX360() )
	{
		return;
	}

	VPROF_BUDGET( "CheckUpdatingSteamResources", VPROF_BUDGETGROUP_STEAM );

	if (m_bUpdateSteamResources)
	{
		bool bComplete = false;
		float flProgress = 0.0f;
		g_pFileSystem->GetWaitForResourcesProgress(m_hWaitForResourcesHandle, &flProgress, &bComplete);

		if (bComplete)
		{
			m_hWaitForResourcesHandle = NULL;
			m_bUpdateSteamResources = false;
			m_bDownloadResources = false;

			if ( m_pDownloadableFileTable )
			{
				bool allowDownloads = true;
				bool allowSoundDownloads = true;
				if ( !Q_strcasecmp( cl_downloadfilter.GetString(), "none" ) )
				{
					allowDownloads = allowSoundDownloads = false;
				}
				else if ( !Q_strcasecmp( cl_downloadfilter.GetString(), "nosounds" ) )
				{
					allowSoundDownloads = false;
				}

				if ( allowDownloads )
				{
					char extension[4];
					for ( int i=0; i<m_pDownloadableFileTable->GetNumStrings(); ++i )
					{
						const char *fname = m_pDownloadableFileTable->GetString( i );

						if ( !allowSoundDownloads )
						{
							Q_ExtractFileExtension( fname, extension, sizeof( extension ) );
							if ( !Q_strcasecmp( extension, "wav" ) || !Q_strcasecmp( extension, "mp3" ) )
							{
								continue;
							}
						}

						CL_QueueDownload( fname );
					}
				}

				if ( CL_GetDownloadQueueSize() )
				{
					// make sure the loading dialog is up
					EngineVGui()->StartCustomProgress();
					EngineVGui()->ActivateGameUI();
					m_bDownloadResources = true;
				}
				else
				{
					m_bDownloadResources = false;
					FinishSignonState_New();
				}
			}
			else
			{
				Host_Error( "Invalid download file table." );
			}
		}
		else if (flProgress > 0.0f)
		{
			if (!m_bShownSteamResourceUpdateProgress)
			{
				// make sure the loading dialog is up
				EngineVGui()->StartCustomProgress();
				EngineVGui()->ActivateGameUI();
				m_bShownSteamResourceUpdateProgress = true;
			}

			// change it to be updating steam resources
			EngineVGui()->UpdateCustomProgressBar( flProgress, g_pVGuiLocalize->Find("#Valve_UpdatingSteamResources") );
		}
	}

	if ( m_bDownloadResources )
	{
		// Check on any HTTP downloads in progress
		bool stillDownloading = CL_DownloadUpdate();
		if ( !stillDownloading )
		{
			m_bDownloadResources = false;
			FinishSignonState_New();
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: At a certain rate, this function will verify any unverified
// file CRCs with the server.
//-----------------------------------------------------------------------------
void CClientState::CheckFileCRCsWithServer()
{
	VPROF_( "CheckFileCRCsWithServer", 1, VPROF_BUDGETGROUP_OTHER_NETWORKING, false, BUDGETFLAG_CLIENT );
	const float flBatchInterval = 1.0f / 3.0f;
	const int nBatchSize = 3;
	
	// Don't do this yet..
	if ( !m_bCheckCRCsWithServer )
		return;

	if ( m_nSignonState != SIGNONSTATE_FULL )
		return;

	// Only send a batch every so often.
	float flCurTime = Plat_FloatTime();
	if ( (flCurTime - m_flLastCRCBatchTime) < flBatchInterval )
		return;
	
	m_flLastCRCBatchTime = flCurTime;
	
	CUnverifiedCRCFile crcFiles[nBatchSize];
	int count = g_pFileSystem->GetUnverifiedCRCFiles( crcFiles, ARRAYSIZE( crcFiles ) );
	if ( count == 0 )
		return;
	
	// Send the messages to the server.
	for ( int i=0; i < count; i++ )
	{
		CLC_FileCRCCheck crcCheck;
		V_strncpy( crcCheck.m_szPathID, crcFiles[i].m_PathID, sizeof( crcCheck.m_szPathID ) );
		V_strncpy( crcCheck.m_szFilename, crcFiles[i].m_Filename, sizeof( crcCheck.m_szFilename ) );
		crcCheck.m_CRC = crcFiles[i].m_CRC;

		m_NetChannel->SendNetMsg( crcCheck );
	}
}


//-----------------------------------------------------------------------------
// Purpose: sanity-checks the variables in a VMT file to prevent the client from
// making player etc. textures that glow or show through walls etc.  Anything
// other than $baseTexture and $bumpmap is hereby verboten.
//-----------------------------------------------------------------------------
bool CheckSimpleMaterial( IMaterial *pMaterial )
{
	if ( !pMaterial )
		return false;

	const char *name = pMaterial->GetShaderName();
	if ( Q_strncasecmp( name, "VertexLitGeneric", 16 ) &&
		 Q_strncasecmp( name, "UnlitGeneric", 12 ) )
		return false;

	if ( pMaterial->GetMaterialVarFlag( MATERIAL_VAR_IGNOREZ ) )
		return false;

	if ( pMaterial->GetMaterialVarFlag( MATERIAL_VAR_WIREFRAME ) )
		return false;

	if ( pMaterial->GetMaterialVarFlag( MATERIAL_VAR_SELFILLUM ) )
		return false;

	if ( pMaterial->GetMaterialVarFlag( MATERIAL_VAR_ADDITIVE ) )
		return false;

	if ( pMaterial->GetMaterialVarFlag( MATERIAL_VAR_NOFOG ) )
		return false;

	if ( pMaterial->GetMaterialVarFlag( MATERIAL_VAR_HALFLAMBERT ) )
		return false;

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: find a filename in the string table, ignoring case and slash mismatches.  Returns the index, or INVALID_STRING_INDEX if not found.
//-----------------------------------------------------------------------------
int FindFilenameInStringTable( INetworkStringTable *table, const char *searchFname )
{
	char searchFilename[MAX_PATH];
	char tableFilename[MAX_PATH];

	Q_strncpy( searchFilename, searchFname, MAX_PATH );
	Q_FixSlashes( searchFilename );

	for ( int i=0; i<table->GetNumStrings(); ++i )
	{
		const char *tableFname = table->GetString( i );
		Q_strncpy( tableFilename, tableFname, MAX_PATH );
		Q_FixSlashes( tableFilename );

		if ( !Q_strcasecmp( searchFilename, tableFilename ) )
		{
			return i;
		}
	}

	return INVALID_STRING_INDEX;
}

//-----------------------------------------------------------------------------
// Purpose: find a filename in the string table, ignoring case and slash mismatches.
// Returns the consistency type, with CONSISTENCY_NONE being a Not Found result.
//-----------------------------------------------------------------------------
ConsistencyType GetFileConsistencyType( INetworkStringTable *table, const char *searchFname )
{
	int index = FindFilenameInStringTable( table, searchFname );
	if ( index == INVALID_STRING_INDEX )
	{
		return CONSISTENCY_NONE;
	}

	int length = 0;
	unsigned char *userData = NULL;
	userData = (unsigned char *)table->GetStringUserData( index, &length );
	if ( userData && length == sizeof( ExactFileUserData ) )
	{
		switch ( userData[0] )
		{
		case CONSISTENCY_EXACT:
		case CONSISTENCY_SIMPLE_MATERIAL:
			return (ConsistencyType)userData[0];
		default:
			return CONSISTENCY_NONE;
		}
	}
	else
	{
		return CONSISTENCY_NONE;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Does a CRC check compared to the CRC stored in the user data.
//-----------------------------------------------------------------------------
bool CheckCRCs( unsigned char *userData, int length, const char *filename )
{
	if ( userData && length == sizeof( ExactFileUserData ) )
	{
		if ( userData[0] != CONSISTENCY_EXACT && userData[0] != CONSISTENCY_SIMPLE_MATERIAL )
		{
			return false;
		}

		ExactFileUserData *exactFileData = (ExactFileUserData *)userData;

		CRC32_t crc;
		if ( !CRC_File( &crc, filename ) )
		{
			return false;
		}

		return ( crc == exactFileData->crc );
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: completes the SIGNONSTATE_NEW state
//-----------------------------------------------------------------------------
void CClientState::FinishSignonState_New()
{
	// make sure we're still in the right signon state
	if (m_nSignonState != SIGNONSTATE_NEW)
		return;

	if ( !m_bMarkedCRCsUnverified )
	{
		// Mark all file CRCs unverified once per server. We may have verified CRCs for certain files on
		// the previous server, but we need to reverify them on the new server.
		m_bMarkedCRCsUnverified = true;
		g_pFileSystem->MarkAllCRCsUnverified();
	}

	// Check for a new whitelist. It's good to do it early in the connection process here because if we wait until later,
	// the client may have loaded some files w/o the proper whitelist restrictions and we'd have to reload them.
	m_bCheckCRCsWithServer = false;	// Don't check CRCs yet.. wait until we got a whitelist and cleaned out our files based on it to send CRCs.
	CL_CheckForPureServerWhitelist();
	
	// Verify the map and player .mdl crc's now that we've finished downloading missing resources (maps etc)
	if ( !CL_CheckCRCs( m_szLevelName ) )
	{
		Host_Error( "Unabled to verify map %s\n", ( m_szLevelName && m_szLevelName[0] ) ? m_szLevelName : "unknown" );
		return;
	}

	CL_InstallAndInvokeClientStringTableCallbacks();
	
#if 0

	// HACK!!!!  For use only on PC not yet using a whitelist!
	// install hooks
	if ( IsPC() && 	( m_nMaxClients > 1 ) )
	{
		m_pModelPrecacheTable->SetStringChangedCallback( NULL, Callback_ModelChanged );

		int nTableCount = m_StringTableContainer->GetNumTables();
		for ( int iTable =0; iTable < nTableCount; ++iTable )
		{
			// iterate through server tables
			CNetworkStringTable *pTable = (CNetworkStringTable*)m_StringTableContainer->GetTable( iTable );
			if ( !pTable )
				continue;

			pfnStringChanged pCallbackFunction = pTable->GetCallback();
			if ( pCallbackFunction )
				for ( int iString = 0; iString < pTable->GetNumStrings(); ++iString )
				{
					int userDataSize;
					const void *pUserData = pTable->GetStringUserData( iString, &userDataSize );
					(*pCallbackFunction)( NULL, pTable, iString, pTable->GetString( iString ), pUserData );
				}
		}

		materials->CacheUsedMaterials();
	}

#endif

	materials->CacheUsedMaterials();

	// force a consistency check
	ConsistencyCheck( true );
	
	CL_RegisterResources();

	// Done with all resources, issue prespawn command.
	// Include server count in case server disconnects and changes level during d/l

	// Tell rendering system we have a new set of models.
	R_LevelInit();

	EngineVGui()->UpdateProgressBar(PROGRESS_SENDCLIENTINFO);
	if ( !m_NetChannel )
		return;
	
	SendClientInfo();

	CL_SetSteamCrashComment();

	// tell server that we entered now that state
	m_NetChannel->SendNetMsg( NET_SignonState( m_nSignonState, m_nServerCount ) );
}


//-----------------------------------------------------------------------------
// Purpose: run a file consistency check if enforced by server
//-----------------------------------------------------------------------------
void CClientState::ConsistencyCheck(bool bChanged )
{
	// get the default config for the current card as a starting point.
	// server must have sent us this table
	if ( !m_pDownloadableFileTable )
		return;

	// no checks during single player or demo playback
	if( (m_nMaxClients == 1) || demoplayer->IsPlayingBack() )
		return;

	// only if we are connected
	if ( !IsConnected() )
		return;

	// only if enforce by server
	if ( !sv_consistency.GetBool() )
		return;

	// check if material configuration changed
	static MaterialSystem_Config_t s_LastConfig;
	MaterialSystem_Config_t newConfig = materials->GetCurrentConfigForVideoCard();

	if ( Q_memcmp( &s_LastConfig, &newConfig, sizeof(MaterialSystem_Config_t) ) )
	{
		// remember last config we tested
		s_LastConfig = newConfig;
		bChanged = true;
	}

	if ( !bChanged )
		return;

	const char *errorFilename = NULL;

	// check CRCs and model sizes
	Color red(  200,  20,  20, 255 );
	Color blue( 100, 100, 200, 255 );
	for ( int i=0; i<m_pDownloadableFileTable->GetNumStrings(); ++i )
	{
		int length = 0;
		unsigned char *userData = NULL;
		userData = (unsigned char *)m_pDownloadableFileTable->GetStringUserData( i, &length );
		const char *filename = m_pDownloadableFileTable->GetString( i );

		//
		// CRC Check
		//
		if ( userData && userData[0] == CONSISTENCY_EXACT && length == sizeof( ExactFileUserData ) )
		{
			if ( !CheckCRCs( userData, length, filename ) )
			{
				ConColorMsg( red, "Bad CRC for %s\n", filename );
				errorFilename = filename;
			}
		}

		//
		// Bounds Check
		//
		// This is simply asking for the model's mins and maxs.  Also, it checks each material referenced
		// by the model, to make sure it doesn't ignore Z, isn't overbright, etc.
		//
		// TODO: Animations and facial expressions can still pull verts out past this.
		//
		else if ( userData && userData[0] == CONSISTENCY_BOUNDS && length == sizeof( ModelBoundsUserData ) )
		{
			ModelBoundsUserData *boundsData = (ModelBoundsUserData *)userData;
			model_t *pModel = modelloader->GetModelForName( filename, IModelLoader::FMODELLOADER_CLIENT );
			if ( !pModel )
			{
				ConColorMsg( red, "Can't find model for %s\n", filename );
				errorFilename = filename;
			}
			else
			{
				if ( pModel->mins.x < boundsData->mins.x ||
					pModel->mins.y < boundsData->mins.y ||
					pModel->mins.z < boundsData->mins.z )
				{
					ConColorMsg( red, "Model %s exceeds mins (%.1f %.1f %.1f vs. %.1f %.1f %.1f)\n", filename,
						pModel->mins.x, pModel->mins.y, pModel->mins.z,
						boundsData->mins.x, boundsData->mins.y, boundsData->mins.z);
					errorFilename = filename;
				}
				if ( pModel->maxs.x > boundsData->maxs.x ||
					pModel->maxs.y > boundsData->maxs.y ||
					pModel->maxs.z > boundsData->maxs.z )
				{
					ConColorMsg( red, "Model %s exceeds maxs (%.1f %.1f %.1f vs. %.1f %.1f %.1f)\n", filename,
						pModel->maxs.x, pModel->maxs.y, pModel->maxs.z,
						boundsData->maxs.x, boundsData->maxs.y, boundsData->maxs.z);
					errorFilename = filename;
				}

				// Check each texture
				IMaterial *materials[ 128 ];
				int materialCount = Mod_GetModelMaterials( pModel, ARRAYSIZE( materials ), materials );

				for ( int j = 0; j<materialCount; ++j )
				{
					IMaterial *pMaterial = materials[j];

					if ( !CheckSimpleMaterial( pMaterial ) )
					{
						ConColorMsg( red, "Model %s has a bad texture %s\n", filename, pMaterial->GetName() );
						errorFilename = filename;
						break;
					}
				}
			}
		}
	}

	if ( errorFilename && *errorFilename )
	{
		COM_ExplainDisconnection( true, "Server is enforcing consistency for this file:\n%s\n", errorFilename );
		Host_Error( "Server is enforcing file consistency for %s\n", errorFilename );
	}
}

void CClientState::UpdateAreaBits_BackwardsCompatible()
{
	if ( m_pAreaBits )
	{
		memcpy( m_chAreaBits, m_pAreaBits, sizeof( m_chAreaBits ) );
		
		// The whole point of adding this array was that the client could react to closed portals.
		// If they're using the old interface to set area portal bits, then we use the old 
		// behavior of assuming all portals are open on the clent.
		memset( m_chAreaPortalBits, 0xFF, sizeof( m_chAreaPortalBits ) );

		m_bAreaBitsValid = true;
	}
}


unsigned char** CClientState::GetAreaBits_BackwardCompatibility()
{
	return &m_pAreaBits;
}


void CClientState::RunFrame()
{
	CBaseClientState::RunFrame();

	// Since cl_rate is a virtualized cvar, make sure to pickup changes in it.
	if ( m_NetChannel )
		m_NetChannel->SetDataRate( cl_rate->GetFloat() );

	ConsistencyCheck( false );

	// Check if paged pool is low ( < 8% free )
	static bool s_bLowPagedPoolMemoryWarning = false;
	PAGED_POOL_INFO_t ppi;
	if ( ( SYSCALL_SUCCESS == Plat_GetPagedPoolInfo( &ppi ) ) &&
		( ( ppi.numPagesFree * 12 ) < ( ppi.numPagesUsed + ppi.numPagesFree ) ) )
	{
		con_nprint_t np;
		np.time_to_live = 1.0;
		np.index = 1;
		np.fixed_width_font = false;
		np.color[ 0 ] = 1.0;
		np.color[ 1 ] = 0.2;
		np.color[ 2 ] = 0.0;
		Con_NXPrintf( &np, "WARNING:  OS Paged Pool Memory Low" );

		// Also print a warning to console
		static float s_flLastWarningTime = 0.0f;
		if ( !s_bLowPagedPoolMemoryWarning ||
			 ( Plat_FloatTime() - s_flLastWarningTime > 3.0f ) )	// print a warning no faster than once every 3 sec
		{
			s_bLowPagedPoolMemoryWarning = true;
			s_flLastWarningTime = Plat_FloatTime();
			Warning( "OS Paged Pool Memory Low!\n" );
			Warning( "  Currently using %d pages (%d Kb) of total %d pages (%d Kb total)\n",
				ppi.numPagesUsed, ppi.numPagesUsed * Plat_GetMemPageSize(),
				( ppi.numPagesFree + ppi.numPagesUsed ), ( ppi.numPagesFree + ppi.numPagesUsed ) * Plat_GetMemPageSize() );
			Warning( "  Please see http://www.steampowered.com for more information.\n" );
		}
	}
	else if ( s_bLowPagedPoolMemoryWarning )
	{
		s_bLowPagedPoolMemoryWarning = false;
		Msg( "Info: OS Paged Pool Memory restored - currently %d pages free (%d Kb) of total %d pages (%d Kb total).\n",
			ppi.numPagesFree, ppi.numPagesFree * Plat_GetMemPageSize(),
			( ppi.numPagesFree + ppi.numPagesUsed ), ( ppi.numPagesFree + ppi.numPagesUsed ) * Plat_GetMemPageSize() );
	}

}
