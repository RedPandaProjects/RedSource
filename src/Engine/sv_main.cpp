//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose:
//
// $Workfile:     $
// $NoKeywords: $
//===========================================================================//

#include "server_pch.h"
#include "decal.h"
#include "host_cmd.h"
#include "cmodel_engine.h"
#include "sv_log.h"
#include "zone.h"
#include "sound.h"
#include "vox.h"
#include "EngineSoundInternal.h"
#include "checksum_engine.h"
#include "master.h"
#include "host.h"
#include "keys.h"
#include "vengineserver_impl.h"
#include "sv_filter.h"
#include "pr_edict.h"
#include "screen.h"
#include "sys_dll.h"
#include "world.h"
#include "sv_main.h"
#include "networkstringtableserver.h"
#include "datamap.h"
#include "filesystem_engine.h"
#include "string_t.h"
#include "vstdlib/random.h"
#include "networkstringtable.h"
#include "dt_send_eng.h"
#include "sv_packedentities.h"
#include "testscriptmgr.h"
#include "PlayerState.h"
#include "saverestoretypes.h"
#include "tier0/vprof.h"
#include "proto_oob.h"
#include "staticpropmgr.h"
#include "checksum_crc.h"
#include "console.h"
#include "tier0/icommandline.h"
#include "gl_matsysiface.h"
#include "GameEventManager.h"
#ifndef SWDS
#include "vgui_baseui_interface.h"
#endif
#include "cbenchmark.h"
#include "client.h"
#include "hltvserver.h"
#include "KeyValues.h"
#include "sv_logofile.h"
#include "sv_steamauth.h"
#include "sv_plugin.h"
#include "DownloadListGenerator.h"
#include "sv_steamauth.h"
#include "LocalNetworkBackdoor.h"
#include "cvar.h"
#include "enginethreads.h"
#include "tier1/functors.h"
#include "vstdlib/jobthread.h"
#include "pure_server.h"
#include "datacache/idatacache.h"
#include "filesystem/IQueuedLoader.h"
#include "vstdlib/jobthread.h"
#include "SourceAppInfo.h"
#include "cl_rcon.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern CNetworkStringTableContainer *networkStringTableContainerServer;
extern CNetworkStringTableContainer *networkStringTableContainerClient;

extern ConVar deathmatch;
extern ConVar sv_sendtables;

// Server default maxplayers value
#define DEFAULT_SERVER_CLIENTS	6
// This many players on a Lan with same key, is ok.
#define MAX_IDENTICAL_CDKEYS	5

CGameServer	sv;

CGlobalVars g_ServerGlobalVariables( false );

static int	current_skill;

static void SV_CheatsChanged_f( IConVar *pConVar, const char *pOldString, float flOldValue )
{
	ConVarRef var( pConVar );
	if ( var.GetInt() == 0 )
	{
		// cheats were disabled, revert all cheat cvars to their default values
		g_pCVar->RevertFlaggedConVars( FCVAR_CHEAT );

		DevMsg( "FCVAR_CHEAT cvars reverted to defaults.\n" );
	}
}

static int g_sv_pure_mode = 0;
static void SV_Pure_f( const CCommand &args )
{
	int pure_mode = -1;
	if ( args.ArgC() == 2 )
	{
		pure_mode = atoi( args[1] );
	}

	Msg( "--------------------------------------------------------\n" );
	if ( pure_mode == 0 || pure_mode == 1 || pure_mode == 2 )
	{
		// Set the value.
		if ( pure_mode == g_sv_pure_mode )
		{
			Msg( "sv_pure value unchanged (current value is %d).\n", g_sv_pure_mode );
		}
		else
		{
			g_sv_pure_mode = pure_mode;
			Msg( "sv_pure set to %d.\n", g_sv_pure_mode );

			if ( sv.IsActive() )
			{
				Msg( "Note: Changes to sv_pure take effect when the next map is loaded.\n" );
			}
		}
	}
	else
	{
		Msg( "sv_pure:"
			"\n\nIf set to 1, the server will force all client files except the whitelisted ones "
			"(in pure_server_whitelist.txt) to match the server's files. "
			"\n\nIf set to 2, the server will force all client files to come from Steam (and it will not load pure_server_whitelist.txt).\n" );
	}

	if ( pure_mode == -1 )
	{
		// If we're a client on a server with sv_pure = 1, display the current whitelist.
#ifndef SWDS
		if ( cl.IsConnected() )
		{
			Msg( "\n\n" );
			extern void CL_PrintWhitelistInfo(); // from cl_main.cpp
			CL_PrintWhitelistInfo();
		}
		else
#endif
		{
			Msg( "\nCurrent sv_pure value is %d.\n", g_sv_pure_mode );
		}
	}
	Msg( "--------------------------------------------------------\n" );
}

static ConCommand sv_pure( "sv_pure", SV_Pure_f, "Show user data." );

ConVar	sv_pure_kick_clients( "sv_pure_kick_clients", "1", 0, "If set to 1, the server will kick clients with mismatching files. Otherwise, it will issue a warning to the client." );
ConVar	sv_pure_trace( "sv_pure_trace", "0", 0, "If set to 1, the server will print a message whenever a client is verifying a CRC for a file." );

ConVar  sv_cheats( "sv_cheats", "0", FCVAR_NOTIFY|FCVAR_REPLICATED, "Allow cheats on server", SV_CheatsChanged_f );
ConVar  sv_lan( "sv_lan", "0", 0, "Server is a lan server ( no heartbeat, no authentication, no non-class C addresses )" );

static	ConVar  tv_enable( "tv_enable", "0", 0, "Activates SourceTV on server." );

static	ConVar	sv_pausable( "sv_pausable","0", FCVAR_NOTIFY, "Is the server pausable." );
static	ConVar	sv_contact( "sv_contact", "", FCVAR_NOTIFY, "Contact email for server sysop" );
static	ConVar	sv_cacheencodedents("sv_cacheencodedents", "1", 0, "If set to 1, does an optimization to prevent extra SendTable_Encode calls.");
		ConVar	sv_voicecodec("sv_voicecodec", "vaudio_speex", 0, "Specifies which voice codec DLL to use in a game. Set to the name of the DLL without the extension.");
static	ConVar	sv_voiceenable( "sv_voiceenable", "1", FCVAR_ARCHIVE|FCVAR_NOTIFY ); // set to 0 to disable all voice forwarding.
		ConVar  sv_downloadurl( "sv_downloadurl", "", FCVAR_REPLICATED, "Location from which clients can download missing files" );
		ConVar  sv_consistency( "sv_consistency", "1", FCVAR_REPLICATED, "Whether the server enforces file consistency for critical files" );
		ConVar	sv_maxreplay("sv_maxreplay", "0", 0, "Maximum replay time in seconds", true, 0, true, 15 );

ConVar  sv_mincmdrate( "sv_mincmdrate", "0", FCVAR_REPLICATED, "This sets the minimum value for cl_cmdrate. 0 == unlimited." );
ConVar  sv_maxcmdrate( "sv_maxcmdrate", "40", FCVAR_REPLICATED, "(If sv_mincmdrate is > 0), this sets the maximum value for cl_cmdrate." );
ConVar  sv_client_cmdrate_difference( "sv_client_cmdrate_difference", "20", FCVAR_REPLICATED, 
	"cl_cmdrate is moved to within sv_client_cmdrate_difference units of cl_updaterate before it "
	"is clamped between sv_mincmdrate and sv_maxcmdrate." );

ConVar  sv_client_min_interp_ratio( "sv_client_min_interp_ratio", "1", FCVAR_REPLICATED, 
								   "This can be used to limit the value of cl_interp_ratio for connected clients "
								   "(only while they are connected).\n"
								   "              -1 = let clients set cl_interp_ratio to anything\n"
								   " any other value = set minimum value for cl_interp_ratio"
								   );
ConVar  sv_client_max_interp_ratio( "sv_client_max_interp_ratio", "5", FCVAR_REPLICATED, 
								   "This can be used to limit the value of cl_interp_ratio for connected clients "
								   "(only while they are connected). If sv_client_min_interp_ratio is -1, "
								   "then this cvar has no effect."
								   );
ConVar  sv_client_predict( "sv_client_predict", "-1", FCVAR_REPLICATED, 
	"This can be used to force the value of cl_predict for connected clients "
	"(only while they are connected).\n"
	"   -1 = let clients set cl_predict to anything\n"
	"    0 = force cl_predict to 0\n"
	"    1 = force cl_predict to 1"
	);


extern ConVar *sv_noclipduringpause;

static bool s_bForceSend = false;

void SV_ForceSend()
{
	s_bForceSend = true;
}

bool g_FlushMemoryOnNextServer;
int g_FlushMemoryOnNextServerCounter;

void SV_FlushMemoryOnNextServer()
{
	g_FlushMemoryOnNextServer = true;
	g_FlushMemoryOnNextServerCounter++;
}

// Prints important entity creation/deletion events to console
#if defined( _DEBUG )
ConVar  sv_deltatrace( "sv_deltatrace", "0", 0, "For debugging, print entity creation/deletion info to console." );
#define TRACE_DELTA( text ) if ( sv_deltatrace.GetInt() ) { ConMsg( text ); };
#else
#define TRACE_DELTA( funcs )
#endif


#if defined( DEBUG_NETWORKING )

//-----------------------------------------------------------------------------
// Opens the recording file
//-----------------------------------------------------------------------------

static FILE* OpenRecordingFile()
{
	FILE* fp = 0;
	static bool s_CantOpenFile = false;
	static bool s_NeverOpened = true;
	if (!s_CantOpenFile)
	{
		fp = fopen( "svtrace.txt", s_NeverOpened ? "wt" : "at" );
		if (!fp)
		{
			s_CantOpenFile = true;
		}
		s_NeverOpened = false;
	}
	return fp;
}

//-----------------------------------------------------------------------------
// Records an argument for a command, flushes when the command is done
//-----------------------------------------------------------------------------
/*
void SpewToFile( char const* pFmt, ... )
static void SpewToFile( const char* pFmt, ... )
{
	static CUtlVector<unsigned char> s_RecordingBuffer;

	char temp[2048];
	va_list args;

	va_start( args, pFmt );
	int len = Q_vsnprintf( temp, sizeof( temp ), pFmt, args );
	va_end( args );
	Assert( len < 2048 );

	int idx = s_RecordingBuffer.AddMultipleToTail( len );
	memcpy( &s_RecordingBuffer[idx], temp, len );
	if ( 1 ) //s_RecordingBuffer.Size() > 8192)
	{
		FILE* fp = OpenRecordingFile();
		fwrite( s_RecordingBuffer.Base(), 1, s_RecordingBuffer.Size(), fp );
		fclose( fp );

		s_RecordingBuffer.RemoveAll();
	}
}
*/

#endif // #if defined( DEBUG_NETWORKING )


/*void SV_Init(bool isDedicated)
{
	sv.Init( isDedicated );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void SV_Shutdown( void )
{
	sv.Shutdown();
}*/

void CGameServer::Clear( void )
{
	m_pModelPrecacheTable = NULL;
	m_pGenericPrecacheTable = NULL;
	m_pSoundPrecacheTable = NULL;
	m_pDecalPrecacheTable = NULL;
	m_bIsLevelMainMenuBackground = false;

	m_bLoadgame = false;
	
	host_state.SetWorldModel( NULL );	

	Q_memset( m_szStartspot, 0, sizeof( m_szStartspot ) );
	
	num_edicts = 0;
	max_edicts = 0;
	edicts = NULL;
	
	// Clear the instance baseline indices in the ServerClasses.
	if ( serverGameDLL )
	{
		for( ServerClass *pCur = serverGameDLL->GetAllServerClasses(); pCur; pCur=pCur->m_pNext )
		{
			pCur->m_InstanceBaselineIndex = INVALID_STRING_INDEX;
		}
	}

	for ( int i = 0; i < m_TempEntities.Count(); i++ )
	{
		delete m_TempEntities[i];
	}

	m_TempEntities.Purge();

	CBaseServer::Clear();
}



//-----------------------------------------------------------------------------
// Purpose: Create any client/server string tables needed internally by the engine
//-----------------------------------------------------------------------------
void CGameServer::CreateEngineStringTables( void )
{
	int i,j;

	m_StringTables->SetTick( m_nTickCount ); // set first tick

	bool bUseFilenameTables = false;	

	char szDownloadableFileTablename[255] = DOWNLOADABLE_FILE_TABLENAME;
	char szModelPrecacheTablename[255] = MODEL_PRECACHE_TABLENAME;
	char szGenericPrecacheTablename[255] = GENERIC_PRECACHE_TABLENAME;
	char szSoundPrecacheTablename[255] = SOUND_PRECACHE_TABLENAME;
	char szDecalPrecacheTablename[255] = DECAL_PRECACHE_TABLENAME;

	// This was added into staging at some point and is not enabled in main or rel.
	if ( 0 )
	{
		bUseFilenameTables = true;
		Q_snprintf( szDownloadableFileTablename, 255, ":%s", DOWNLOADABLE_FILE_TABLENAME );
		Q_snprintf( szModelPrecacheTablename, 255, ":%s", MODEL_PRECACHE_TABLENAME );
		Q_snprintf( szGenericPrecacheTablename, 255, ":%s", GENERIC_PRECACHE_TABLENAME );
		Q_snprintf( szSoundPrecacheTablename, 255, ":%s", SOUND_PRECACHE_TABLENAME );
		Q_snprintf( szDecalPrecacheTablename, 255, ":%s", DECAL_PRECACHE_TABLENAME );		
	} 	

	m_pDownloadableFileTable = m_StringTables->CreateStringTableEx( 
		szDownloadableFileTablename, 
		MAX_DOWNLOADABLE_FILES,
		0,
		0,
		bUseFilenameTables );

	m_pModelPrecacheTable = m_StringTables->CreateStringTableEx( 
		szModelPrecacheTablename, 
		MAX_MODELS,
		sizeof ( CPrecacheUserData ),
		PRECACHE_USER_DATA_NUMBITS,
		bUseFilenameTables );

	m_pGenericPrecacheTable = m_StringTables->CreateStringTableEx(
		szGenericPrecacheTablename,
		MAX_GENERIC,
		sizeof ( CPrecacheUserData ),
		PRECACHE_USER_DATA_NUMBITS,
		bUseFilenameTables );

	m_pSoundPrecacheTable = m_StringTables->CreateStringTableEx(
		szSoundPrecacheTablename,
		MAX_SOUNDS,
		sizeof ( CPrecacheUserData ),
		PRECACHE_USER_DATA_NUMBITS,
		bUseFilenameTables );

	m_pDecalPrecacheTable = m_StringTables->CreateStringTableEx(
		szDecalPrecacheTablename,
		MAX_BASE_DECALS,
		sizeof ( CPrecacheUserData ),
		PRECACHE_USER_DATA_NUMBITS,
		bUseFilenameTables );

	m_pInstanceBaselineTable = m_StringTables->CreateStringTable(
		INSTANCE_BASELINE_TABLENAME,
		MAX_DATATABLES );

	m_pLightStyleTable = m_StringTables->CreateStringTable( 
		LIGHT_STYLES_TABLENAME, 
		MAX_LIGHTSTYLES );

	m_pUserInfoTable = m_StringTables->CreateStringTable(
		USER_INFO_TABLENAME,
		1<<ABSOLUTE_PLAYER_LIMIT_DW ); // make it a power of 2

	// Send the query info..
	m_pServerStartupTable = m_StringTables->CreateStringTable( 
		SERVER_STARTUP_DATA_TABLENAME,
		4 );
	SetQueryPortFromSteamServer();
	CopyPureServerWhitelistToStringTable();

	Assert ( m_pModelPrecacheTable && 
			 m_pGenericPrecacheTable &&
			 m_pSoundPrecacheTable &&
			 m_pDecalPrecacheTable &&
			 m_pInstanceBaselineTable &&
			 m_pLightStyleTable &&
			 m_pUserInfoTable &&
			 m_pServerStartupTable &&
			 m_pDownloadableFileTable );

	// create an empty lightstyle table with unique index names
	for ( i = 0; i<MAX_LIGHTSTYLES; i++ )
	{
		char name[8]; Q_snprintf( name, 8, "%i", i );
		j = m_pLightStyleTable->AddString( true, name );
		Assert( j==i ); // indices must match 
	}

	for ( i = 0; i<ABSOLUTE_PLAYER_LIMIT; i++ )
	{
		char name[8]; Q_snprintf( name, 8, "%i", i );
		j = m_pUserInfoTable->AddString( true, name );
		Assert( j==i ); // indices must match 
	}

	// set up the downloadable files generator
	DownloadListGenerator().SetStringTable( m_pDownloadableFileTable );
}

void CGameServer::SetQueryPortFromSteamServer()
{
	if ( !m_pServerStartupTable )
		return;
		
	int queryPort = Steam3Server().GetQueryPort();
	m_pServerStartupTable->AddString( true, "QueryPort", sizeof( queryPort ), &queryPort );
}

void CGameServer::CopyPureServerWhitelistToStringTable()
{
	if ( !m_pPureServerWhitelist )
		return;
	
	CUtlBuffer buf;
	m_pPureServerWhitelist->Encode( buf );
	m_pServerStartupTable->AddString( true, "PureServerWhitelist", buf.TellPut(), buf.Base() );
}


void SV_InstallClientStringTableMirrors( void )
{
#ifndef SWDS
#ifndef SHARED_NET_STRING_TABLES

	int numTables = networkStringTableContainerServer->GetNumTables();

	for ( int i =0; i<numTables; i++)
	{
		// iterate through server tables
		CNetworkStringTable *serverTable = 
			(CNetworkStringTable*)networkStringTableContainerServer->GetTable( i );

		if ( !serverTable )
			continue;

		// get mathcing client table
		CNetworkStringTable *clientTable = 
			(CNetworkStringTable*)networkStringTableContainerClient->FindTable( serverTable->GetTableName() );

		if ( !clientTable )
		{
			DevMsg("SV_InstallClientStringTableMirrors! Missing client table \"%s\".\n ", serverTable->GetTableName() );
			continue;
		}

		// link client table to server table
		serverTable->SetMirrorTable( clientTable );
	}
#endif
#endif
}



//-----------------------------------------------------------------------------
// user <name or userid>
//
// Dump userdata / masterdata for a user
//-----------------------------------------------------------------------------
CON_COMMAND( user, "Show user data." )
{
	int		uid;
	int		i;
	
	if ( !sv.IsActive() )
	{
		ConMsg( "Can't 'user', not running a server\n" );
		return;
	}

	if (args.ArgC() != 2)
	{
		ConMsg ("Usage: user <username / userid>\n");
		return;
	}

	uid = atoi(args[1]);

	for (i=0 ; i< sv.GetClientCount() ; i++)
	{
		IClient *cl = sv.GetClient( i );

		if ( !cl->IsConnected() )
			continue;

		if ( ( cl->GetPlayerSlot()== uid ) || !Q_strcmp( cl->GetClientName(), args[1]) )
		{
			ConMsg ("TODO: SV_User_f.\n");
			return;
		}
	}

	ConMsg ("User not in server.\n");
}


//-----------------------------------------------------------------------------
// Dump userids for all current players
//-----------------------------------------------------------------------------
CON_COMMAND( users, "Show user info for players on server." )
{
	if ( !sv.IsActive() )
	{
		ConMsg( "Can't 'users', not running a server\n" );
		return;
	}

	int c = 0;
	ConMsg ("<slot:userid:\"name\">\n");
	for ( int i=0 ; i< sv.GetClientCount() ; i++ )
	{
		IClient *cl = sv.GetClient( i );

		if ( cl->IsConnected() )
		{
			ConMsg ("%i:%i:\"%s\"\n", cl->GetPlayerSlot(), cl->GetUserID(), cl->GetClientName() );
			c++;
		}
	}

	ConMsg ( "%i users\n", c );
}

//-----------------------------------------------------------------------------
// Purpose: Determine the value of sv.maxclients
//-----------------------------------------------------------------------------
bool CL_IsHL2Demo(); // from cl_main.cpp
bool CL_IsPortalDemo(); // from cl_main.cpp

void CGameServer::InitMaxClients( void )
{
	int minmaxplayers = 1;
	int maxmaxplayers = ABSOLUTE_PLAYER_LIMIT;
	int defaultmaxplayers = 1;

	if ( serverGameClients )
	{
		serverGameClients->GetPlayerLimits( minmaxplayers, maxmaxplayers, defaultmaxplayers );

		if ( minmaxplayers < 1 )
		{
			Sys_Error( "GetPlayerLimits:  min maxplayers must be >= 1 (%i)", minmaxplayers );
		}
		else if ( defaultmaxplayers < 1 )
		{
			Sys_Error( "GetPlayerLimits:  default maxplayers must be >= 1 (%i)", minmaxplayers );
		}

		if ( minmaxplayers > maxmaxplayers || defaultmaxplayers > maxmaxplayers )
		{
			Sys_Error( "GetPlayerLimits:  min maxplayers %i > max %i", minmaxplayers, maxmaxplayers );
		}

		if ( maxmaxplayers > ABSOLUTE_PLAYER_LIMIT )
		{
			Sys_Error( "GetPlayerLimits:  max players limited to %i", ABSOLUTE_PLAYER_LIMIT );
		}
	}

	// Determine absolute limit
	m_nMaxClientsLimit = maxmaxplayers;

	// Check for command line override
	int newmaxplayers = CommandLine()->ParmValue( "-maxplayers", -1 );

	if ( newmaxplayers >= 1 )
	{
		// Never go above what the game .dll can handle
		newmaxplayers	= min( newmaxplayers, maxmaxplayers );
		m_nMaxClientsLimit	= newmaxplayers;
	}
	else
	{
		newmaxplayers	= defaultmaxplayers;
	}

	newmaxplayers = clamp( newmaxplayers, minmaxplayers, m_nMaxClientsLimit );

	if ( ( CL_IsHL2Demo() || CL_IsPortalDemo() ) && !IsDedicated() )
	{
		newmaxplayers = 1;
		m_nMaxClientsLimit = 1;
	}

	SetMaxClients( newmaxplayers );
}

//-----------------------------------------------------------------------------
// Purpose: Changes the maximum # of players allowed on the server.
//  Server cannot be running when command is issued.
//-----------------------------------------------------------------------------
CON_COMMAND( maxplayers, "Change the maximum number of players allowed on this server." )
{
	if ( args.ArgC () != 2 )
	{
		ConMsg ("\"maxplayers\" is \"%u\"\n", sv.GetMaxClients() );
		return;
	}

	if ( sv.IsActive() )
	{
		ConMsg( "Cannot change maxplayers while the server is running\n");
		return;
	}

	sv.SetMaxClients( Q_atoi( args[ 1 ] ) );
}

int SV_BuildSendTablesArray( ServerClass *pClasses, SendTable **pTables, int nMaxTables )
{
        int nTables = 0;

        for( ServerClass *pCur=pClasses; pCur; pCur=pCur->m_pNext )
        {
                ErrorIfNot( nTables < nMaxTables, ("SV_BuildSendTablesArray: too many SendTables!") );
                pTables[nTables] = pCur->m_pTable;
                ++nTables;
        }

        return nTables;
}


// Builds an alternate copy of the datatable for any classes that have datatables with props excluded.
void SV_InitSendTables( ServerClass *pClasses )
{
	SendTable *pTables[MAX_DATATABLES];
	int nTables = SV_BuildSendTablesArray( pClasses, pTables, ARRAYSIZE( pTables ) );

	SendTable_Init( pTables, nTables );
}


void SV_TermSendTables( ServerClass *pClasses )
{
	SendTable_Term();
}


//-----------------------------------------------------------------------------
// Purpose: returns which games/mods we're allowed to play
//-----------------------------------------------------------------------------
struct ModDirPermissions_t
{
	int m_iAppID;
	const char *m_pchGameDir;
};

static ModDirPermissions_t g_ModDirPermissions[] =
{
	{ GetAppSteamAppId( k_App_CSS ),        GetAppModName( k_App_CSS ) },
	{ GetAppSteamAppId( k_App_DODS ),       GetAppModName( k_App_DODS ) },
	{ GetAppSteamAppId( k_App_HL2MP ),      GetAppModName( k_App_HL2MP ) },
	{ GetAppSteamAppId( k_App_LOST_COAST ), GetAppModName( k_App_LOST_COAST ) },
	{ GetAppSteamAppId( k_App_HL1DM ),      GetAppModName( k_App_HL1DM ) },
	{ GetAppSteamAppId( k_App_PORTAL ),     GetAppModName( k_App_PORTAL ) },
	{ GetAppSteamAppId( k_App_HL2 ),        GetAppModName( k_App_HL2 ) },
	{ GetAppSteamAppId( k_App_HL2_EP1 ),    GetAppModName( k_App_HL2_EP1 ) },
	{ GetAppSteamAppId( k_App_HL2_EP2 ),    GetAppModName( k_App_HL2_EP2 ) },
	{ GetAppSteamAppId( k_App_TF2 ),        GetAppModName( k_App_TF2 ) },
};

bool ServerDLL_Load()
{
	// Load in the game .dll
	LoadEntityDLLs( GetBaseDirectory() );
	return true;
}

void ServerDLL_Unload()
{
	UnloadEntityDLLs();
}

//-----------------------------------------------------------------------------
// Purpose: Loads the game .dll
//-----------------------------------------------------------------------------
void SV_InitGameDLL( void )
{
	// Clear out the command buffer.
	Cbuf_Execute();

	// Don't initialize a second time
	if ( sv.dll_initialized )
	{
		return;
	}

#if !defined(SWDS)
#if !defined(_X360)
	bool CL_IsHL2Demo();
	if ( CL_IsHL2Demo() && !sv.IsDedicated() && Q_stricmp( COM_GetModDirectory(), "hl2" ) )
	{
		Error( "The HL2 demo is unable to run Mods.\n" );
		return;			
	} 

	if ( CL_IsPortalDemo() && !sv.IsDedicated() && Q_stricmp( COM_GetModDirectory(), "portal" ) )
	{
		Error( "The Portal demo is unable to run Mods.\n" );
		return;			
	} 
#ifndef NO_STEAM
	// check permissions
	if ( SteamApps() && g_pFileSystem->IsSteam() && !CL_IsHL2Demo() && !CL_IsPortalDemo() )
	{
		bool bVerifiedMod = false;

        // find the game dir we're running
		for ( int i = 0; i < ARRAYSIZE( g_ModDirPermissions ); i++ )
		{
			if ( !Q_stricmp( COM_GetModDirectory(), g_ModDirPermissions[i].m_pchGameDir ) )
			{
				// we've found the mod, make sure we own the app
				char szSub[10];
				if ( SteamApps()->GetAppData( g_ModDirPermissions[i].m_iAppID, "subscribed", szSub, sizeof(szSub) ) > 0
					 && Q_atoi( szSub ) > 0 )
				{
					bVerifiedMod = true;
				}
				else
				{
					Error( "No permissions to run '%s'\n", COM_GetModDirectory() );
					return;			
				}

				break;
			}
		}

		if ( !bVerifiedMod )
		{
			// make sure they can run the Source engine
			char szSub[10];
			if ( SteamApps()->GetAppData( 215, "subscribed", szSub, sizeof(szSub) ) == 0 || Q_atoi( szSub ) == 0 )
			{
				Error( "A Source engine game is required to run mods\n" );
				return;
			}
		}
	}
#endif // NO_STEAM
#endif // _X360
#endif

	COM_TimestampedLog( "SV_InitGameDLL" );

	if ( !serverGameDLL )
	{
		Warning( "Failed to load server binary\n" );
		return;
	}

	// Flag that we've started the game .dll
	sv.dll_initialized = true;

	COM_TimestampedLog( "serverGameDLL->DLLInit" );

	// Tell the game DLL to start up
	if(!serverGameDLL->DLLInit(g_AppSystemFactory, g_AppSystemFactory, g_AppSystemFactory, &g_ServerGlobalVariables))
	{
		Host_Error("IDLLFunctions::DLLInit returned false.\n");
	}

	g_pServerPluginHandler->LoadPlugins(); // load 3rd party plugins
	

	// let's not have any servers with no name
	if ( host_name.GetString()[0] == 0 )
	{
		host_name.SetValue( serverGameDLL->GetGameDescription() );
	}

	sv_noclipduringpause = ( ConVar * )g_pCVar->FindVar( "sv_noclipduringpause" );

	COM_TimestampedLog( "SV_InitSendTables" );

	// Make extra copies of data tables if they have SendPropExcludes.
	SV_InitSendTables( serverGameDLL->GetAllServerClasses() );

	host_state.interval_per_tick = serverGameDLL->GetTickInterval();
	if ( host_state.interval_per_tick < MINIMUM_TICK_INTERVAL ||
		 host_state.interval_per_tick > MAXIMUM_TICK_INTERVAL )
	{
		Sys_Error( "GetTickInterval returned bogus tick interval (%f)[%f to %f is valid range]", host_state.interval_per_tick,
			MINIMUM_TICK_INTERVAL, MAXIMUM_TICK_INTERVAL );
	}

	// set maxclients limit based on Mod or commandline settings
	sv.InitMaxClients();

	// Execute and server commands the game .dll added at startup
	Cbuf_Execute();
}

//
// Release resources associated with extension DLLs.
//
void SV_ShutdownGameDLL( void )
{
	if ( !sv.dll_initialized )
	{
		return;
	}

	// Delete any extra SendTable copies we've attached to the game DLL's classes, if any.
	SV_TermSendTables( serverGameDLL->GetAllServerClasses() );
	g_pServerPluginHandler->UnloadPlugins();
	serverGameDLL->DLLShutdown();

	UnloadEntityDLLs();

	sv.dll_initialized = false;
}


ServerClass* SV_FindServerClass( const char *pName )
{
	ServerClass *pCur = serverGameDLL->GetAllServerClasses();
	while ( pCur )
	{
		if ( Q_stricmp( pCur->GetName(), pName ) == 0 )
			return pCur;

		pCur = pCur->m_pNext;
	}
	
	return NULL;
}

ServerClass* SV_FindServerClass( int index )
{
	ServerClass *pCur = serverGameDLL->GetAllServerClasses();
	int count = 0;

	while ( (count < index) && (pCur != NULL) )
	{
		count++;
		pCur = pCur->m_pNext;
	}

	return pCur;
}

//-----------------------------------------------------------------------------
// Purpose: General initialization of the server
//-----------------------------------------------------------------------------
void CGameServer::Init (bool isDedicated)
{
	CBaseServer::Init( isDedicated );

	m_FullSendTables.SetDebugName( "m_FullSendTables" );
	
	dll_initialized = false;
}

bool CGameServer::IsPausable( void ) const
{
	// In single-player, they can always pause it. In multiplayer, check the cvar.
	if ( IsMultiplayer() )
	{
		return sv_pausable.GetBool();
	}
	else
	{
		return true;
	}
}

void CGameServer::Shutdown( void )
{
	m_bIsLevelMainMenuBackground = false;

	CBaseServer::Shutdown();

	// Actually performs a shutdown.
	framesnapshotmanager->LevelChanged();

	IGameEvent *event = g_GameEventManager.CreateEvent( "server_shutdown" );

	if ( event )
	{
		event->SetString( "reason", "quit" );
		g_GameEventManager.FireEvent( event );
	}

	Steam3Server().Shutdown();

	// Log_Printf( "Server shutdown.\n" );
	g_Log.Close();
}


/*
==================
SV_StartSound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
allready running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.  (max 4 attenuation)

Pitch should be PITCH_NORM (100) for no pitch shift. Values over 100 (up to 255)
shift pitch higher, values lower than 100 lower the pitch.
==================
*/
void SV_StartSound ( IRecipientFilter& filter, edict_t *pSoundEmittingEntity, int iChannel, 
	const char *pSample, float flVolume, soundlevel_t iSoundLevel, int iFlags, 
	int iPitch, const Vector *pOrigin, float soundtime, int speakerentity, CUtlVector< Vector >* pUtlVecOrigins )
{

	SoundInfo_t sound; 
	sound.SetDefault();

	sound.nEntityIndex = pSoundEmittingEntity ? NUM_FOR_EDICT( pSoundEmittingEntity ) : 0;
	sound.nChannel = iChannel;
	sound.fVolume = flVolume;
	sound.Soundlevel = iSoundLevel;
	sound.nFlags = iFlags;
	sound.nPitch = iPitch;
	sound.nSpeakerEntity = speakerentity;

	if ( iFlags & SND_STOP )
	{
		Assert( filter.IsReliable() );
	}

	// Compute the sound origin
	if ( pOrigin )
	{
		VectorCopy( *pOrigin, sound.vOrigin );
	}
	else if ( pSoundEmittingEntity )
	{
		IServerEntity *serverEntity = pSoundEmittingEntity->GetIServerEntity();
		if ( serverEntity )
		{
			CM_WorldSpaceCenter( serverEntity->GetCollideable(), &sound.vOrigin );
		}
	}

	// Add actual sound origin to vector if requested
	if ( pUtlVecOrigins )
	{
		(*pUtlVecOrigins).AddToTail( sound.vOrigin );
	}

	// set sound delay
	if ( soundtime != 0.0f )
	{
		// add one tick since server time ends at the current tick
		// we'd rather delay sounds slightly than skip the beginning samples
		// so add one tick of latency
		soundtime += sv.GetTickInterval();

		sound.fDelay = soundtime - sv.GetFinalTickTime();
		sound.nFlags |= SND_DELAY;
#if 0
		static float lastSoundTime = 0;
		Msg("SV: [%.3f] Play %s at %.3f\n", soundtime - lastSoundTime, pSample, soundtime );
		lastSoundTime = soundtime;
#endif
	}
	
	// find precache number for sound
	
	// if this is a sentence, get sentence number
	if ( pSample && TestSoundChar(pSample, CHAR_SENTENCE) )
	{
		sound.bIsSentence = true;
		sound.nSoundNum = Q_atoi( PSkipSoundChars(pSample) );
		if ( sound.nSoundNum >= VOX_SentenceCount() )
		{
			ConMsg("SV_StartSound: invalid sentence number: %s", PSkipSoundChars(pSample));
			return;
		}
	}
	else
	{
		sound.bIsSentence = false;
		sound.nSoundNum = sv.LookupSoundIndex( pSample );
		if ( !sound.nSoundNum || !sv.GetSound( sound.nSoundNum ) )
		{
			ConMsg ("SV_StartSound: %s not precached (%d)\n", pSample, sound.nSoundNum );
			return;
		}
    }

	// now sound message is complete, send to clients in filter
	sv.BroadcastSound( sound, filter );
}

//-----------------------------------------------------------------------------
// Purpose: Sets bits of playerbits based on valid multicast recipients
// Input  : usepas - 
//			origin - 
//			playerbits - 
//-----------------------------------------------------------------------------
void SV_DetermineMulticastRecipients( bool usepas, const Vector& origin, CBitVec< ABSOLUTE_PLAYER_LIMIT >& playerbits )
{
	// determine cluster for origin
	int cluster = CM_LeafCluster( CM_PointLeafnum( origin ) );
	byte pvs[MAX_MAP_LEAFS/8];
	int visType = usepas ? DVIS_PAS : DVIS_PVS;
	const byte *pMask = CM_Vis( pvs, sizeof(pvs), cluster, visType );

	playerbits.ClearAll();

	// Check for relevent clients
	for (int i = 0; i < sv.GetClientCount(); i++ )
	{
		CGameClient *pClient = sv.Client( i );

		if ( !pClient->IsActive() )
			continue;

		// HACK:  Should above also check pClient->spawned instead of this
		if ( !pClient->edict || pClient->edict->IsFree() || pClient->edict->GetUnknown() == NULL )
			continue;
		
		// Always add the HLTV client
		if ( pClient->IsHLTV() )
		{
			playerbits.Set( i );
			continue;
		}

		Vector vecEarPosition;
		serverGameClients->ClientEarPosition( pClient->edict, &vecEarPosition );

		int iBitNumber = CM_LeafCluster( CM_PointLeafnum( vecEarPosition ) );
		if ( !(pMask[iBitNumber>>3] & (1<<(iBitNumber&7)) ) )
			continue;

		playerbits.Set( i );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Write single ConVar change to all connected clients
// Input  : *var - 
//			*newValue - 
//-----------------------------------------------------------------------------
void SV_ReplicateConVarChange( ConVar const *var, const char *newValue )
{
	Assert( var );
	Assert( var->IsFlagSet( FCVAR_REPLICATED ) );
	Assert( newValue );

	if ( !sv.IsActive() || !sv.IsMultiplayer() )
		return;

	NET_SetConVar cvarMsg( var->GetName(), Host_CleanupConVarStringValue( newValue ) );

	sv.BroadcastMessage( cvarMsg );
}


//-----------------------------------------------------------------------------
// Purpose: Execute a command on all clients or a particular client
// Input  : *var - 
//			*newValue - 
//-----------------------------------------------------------------------------
void SV_ExecuteRemoteCommand( const char *pCommand, int nClientSlot )
{
	if ( !sv.IsActive() || !sv.IsMultiplayer() )
		return;

	NET_StringCmd cmdMsg( pCommand );

	if ( nClientSlot >= 0 )
	{
		CEngineSingleUserFilter filter( nClientSlot + 1, true );
		sv.BroadcastMessage( cmdMsg, filter );
	}
	else
	{
		sv.BroadcastMessage( cmdMsg );
	}
}



/*
==============================================================================

CLIENT SPAWNING

==============================================================================
*/

CGameServer::CGameServer()
{
	m_nMaxClientsLimit = 0;
	m_pPureServerWhitelist = NULL;
}


CGameServer::~CGameServer()
{
	if ( m_pPureServerWhitelist )
		m_pPureServerWhitelist->Release();
}


//-----------------------------------------------------------------------------
// Purpose: Disconnects the client and cleans out the m_pEnt CBasePlayer container object
// Input  : *clientedict - 
//-----------------------------------------------------------------------------
void CGameServer::RemoveClientFromGame( CBaseClient *client )
{
	CGameClient *cl = (CGameClient*)client;

	// we must have an active server and a spawned client 
	if ( !cl->edict || !cl->IsSpawned() || !IsActive() )
		return;

	Assert( g_pServerPluginHandler );

	g_pServerPluginHandler->ClientDisconnect( cl->edict );
	// release the DLL entity that's attached to this edict, if any
	serverGameEnts->FreeContainingEntity( cl->edict );

}

CBaseClient *CGameServer::CreateNewClient(int slot )
{
	CBaseClient *cl = new CGameClient( slot, this );

	const char *pszValue = NULL;
	if ( cl && CommandLine()->CheckParm( "-netspike", &pszValue ) && 
		pszValue )
	{
		cl->SetTraceThreshold( Q_atoi( pszValue ) );
	}

	return cl;
}



/*
================
SV_FinishCertificateCheck

For LAN connections, make sure we don't have too many people with same cd key hash
For Authenticated net connections, check the certificate and also double check won userid
 from that certificate
================
*/
bool CGameServer::FinishCertificateCheck( netadr_t &adr, int nAuthProtocol, const char *szRawCertificate )
{
	// Now check auth information
	switch ( nAuthProtocol )
	{
	default:
	case PROTOCOL_AUTHCERTIFICATE:
		RejectConnection( adr, "Authentication disabled!!!\n");
		return false;

	case PROTOCOL_STEAM:
		return true; // the SteamAuthServer() state machine checks this
		break;

	case PROTOCOL_HASHEDCDKEY:
		if ( AllowDebugDedicatedServerOutsideSteam() )
			return true;

		if ( (g_pFileSystem->IsSteam() && !Host_IsSinglePlayerGame()) || sv.IsDedicated()) // PROTOCOL_HASHEDCDKEY isn't allowed for multiplayer servers
		{
			RejectConnection( adr, "CD Key authentication invalid for internet servers.\n" );
			return false;
		}

		if ( Q_strlen( szRawCertificate ) != 32 )
		{
			RejectConnection( adr, "Invalid CD Key.\n" );
			return false;
		}

		int nHashCount = 0;

		// Now make sure that this hash isn't "overused"
		for ( int i=0; i< GetClientCount(); i++ )
		{
			CBaseClient *cl = Client(i);

			if ( !cl->IsConnected() )
				continue;

			if ( Q_strnicmp ( szRawCertificate, cl->m_GUID, SIGNED_GUID_LEN ) )
				continue;

			nHashCount++;
		}

		if ( nHashCount >= MAX_IDENTICAL_CDKEYS )
		{
			RejectConnection( adr, "CD Key already in use.\n");
			return false;
		}
		break;
	}

	return true;
}

/*
=============================================================================

The PVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.

=============================================================================
*/

static int		s_FatBytes;
static byte*	s_pFatPVS = 0;

CUtlVector<int> g_AreasNetworked;

static void SV_AddToFatPVS( const Vector& org )
{
	int		i;
	byte	pvs[MAX_MAP_LEAFS/8];

	CM_Vis( pvs, sizeof(pvs), CM_LeafCluster( CM_PointLeafnum( org ) ), DVIS_PVS );
	for (i=0 ; i<s_FatBytes ; i++)
	{
		s_pFatPVS[i] |= pvs[i];
	}
}

//-----------------------------------------------------------------------------
// Purpose: Zeroes out pvs, this way we can or together multiple pvs's for a player
//-----------------------------------------------------------------------------
void SV_ResetPVS( byte* pvs, int pvssize )
{
	s_pFatPVS = pvs;
	s_FatBytes = Bits2Bytes(CM_NumClusters());

	if ( s_FatBytes > pvssize )
	{
		Sys_Error( "SV_ResetPVS:  Size %i too big for buffer %i\n", s_FatBytes, pvssize );
	}

	Q_memset (s_pFatPVS, 0, s_FatBytes);
	g_AreasNetworked.RemoveAll();
}

/*
=============
Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
void SV_AddOriginToPVS( const Vector& origin )
{
	SV_AddToFatPVS( origin );
	int area = CM_LeafArea( CM_PointLeafnum( origin ) );
	int i;
	for( i = 0; i < g_AreasNetworked.Count(); i++ )
	{
		if( g_AreasNetworked[i] == area )
		{
			return;
		}
	}
	g_AreasNetworked.AddToTail( area );
}

void CGameServer::BroadcastSound( SoundInfo_t &sound, IRecipientFilter &filter )
{
	int num = filter.GetRecipientCount();
	
	// don't add sounds while paused, unless we're in developer mode
	if ( IsPaused() && !developer.GetInt() )
		return;

	for ( int i = 0; i < num; i++ )
	{
		int index = filter.GetRecipientIndex( i );

		if ( index < 1 || index > GetClientCount() )
		{
			Msg( "CGameServer::BroadcastSound:  Recipient Filter for sound (reliable: %s, init: %s) with bogus client index (%i) in list of %i clients\n", 
					filter.IsReliable() ? "yes" : "no",
					filter.IsInitMessage() ? "yes" : "no",
					index, num );

			continue;
		}

		CGameClient *cl = Client( index - 1 );

		// client must be fully connect to hear sounds
		if ( !cl->IsActive() )
		{
			continue;
		}

		cl->SendSound( sound, filter.IsReliable() );
	}
}

bool CGameServer::IsInPureServerMode() const
{
	return (m_pPureServerWhitelist != NULL);
}

void CGameServer::FinishRestore()
{
#ifndef SWDS
	CSaveRestoreData currentLevelData;
	char			name[MAX_OSPATH];

	if ( !m_bLoadgame )
		return;

	g_ServerGlobalVariables.pSaveData = &currentLevelData;
	// Build the adjacent map list
	serverGameDLL->BuildAdjacentMapList();

	if ( !saverestore->IsXSave() )
	{
		Q_snprintf( name, sizeof( name ), "%s%s.HL2", saverestore->GetSaveDir(), m_szMapname );
	}
	else
	{
		Q_snprintf( name, sizeof( name ), "%s:\\%s.HL2", GetCurrentMod(), m_szMapname );
	}

	Q_FixSlashes( name );

	saverestore->RestoreClientState( name, false );

	if ( g_ServerGlobalVariables.eLoadType == MapLoad_Transition )
	{
		for ( int i = 0; i < currentLevelData.levelInfo.connectionCount; i++ )
		{
			saverestore->RestoreAdjacenClientState( currentLevelData.levelInfo.levelList[i].mapName );
		}
	}

	saverestore->OnFinishedClientRestore();

	g_ServerGlobalVariables.pSaveData = NULL;

	// Reset
	m_bLoadgame = false;
	saverestore->SetIsXSave( IsX360() );
#endif
}

void CGameServer::CopyTempEntities( CFrameSnapshot* pSnapshot )	
{
	Assert( pSnapshot->m_pTempEntities == NULL );

	if ( m_TempEntities.Count() > 0 )
	{
		// copy temp entities if any
		pSnapshot->m_nTempEntities = m_TempEntities.Count();

		pSnapshot->m_pTempEntities = new CEventInfo*[pSnapshot->m_nTempEntities];

		Q_memcpy( pSnapshot->m_pTempEntities, m_TempEntities.Base(), m_TempEntities.Count() * sizeof( CEventInfo * ) );

		// clear server list
		m_TempEntities.RemoveAll();
	}
}

static ConVar sv_parallel_sendsnapshot( "sv_parallel_sendsnapshot", "1" );

void SV_ParallelSendSnapshot( CGameClient *& pClient )
{
	CClientFrame *pFrame = pClient->GetSendFrame();
	if ( !pFrame )
		return;
	pClient->SendSnapshot( pFrame );
	pClient->UpdateSendState();
}

void CGameServer::SendClientMessages ( bool bSendSnapshots )
{
	VPROF_BUDGET( "SendClientMessages", VPROF_BUDGETGROUP_OTHER_NETWORKING );
	
	// build individual updates
	int receivingClientCount = 0;
	CGameClient*	pReceivingClients[ABSOLUTE_PLAYER_LIMIT];
	for (int i=0; i< GetClientCount(); i++ )
	{
		CGameClient* client = Client(i);
		
		// Update Host client send state...
		if ( !client->ShouldSendMessages() )
			continue;
		
		// Append the unreliable data (player updates and packet entities)
		if ( bSendSnapshots && client->IsActive() )
		{
			// Add this client to the list of clients we're gonna send to.
			pReceivingClients[receivingClientCount] = client;
			++receivingClientCount;
		}
		else
		{
			// Connected, but inactive, just send reliable, sequenced info.
			if ( client->IsFakeClient() )
				continue;
				
			// if client never send a netchannl packet yet, send S2C_CONNECTION 
			// because it could get lost in multiplayer
			if ( NET_IsMultiplayer() && client->m_NetChannel->GetSequenceNr(FLOW_INCOMING) == 0 )
			{
				NET_OutOfBandPrintf ( m_Socket, client->m_NetChannel->GetRemoteAddress(), "%c00000000000000", S2C_CONNECTION );
			}

#ifdef SHARED_NET_STRING_TABLES
			sv.m_StringTables->TriggerCallbacks( client->m_nDeltaTick );
#endif

			client->m_NetChannel->Transmit();
			client->UpdateSendState();
		}
	}

	if ( receivingClientCount )
	{
		// if any client wants an update, take new snapshot now
		CFrameSnapshot* pSnapshot = framesnapshotmanager->TakeTickSnapshot( m_nTickCount );

		// copy temp ents references to pSnapshot
		CopyTempEntities( pSnapshot );

		// Compute the client packs
		SV_ComputeClientPacks( receivingClientCount, pReceivingClients, pSnapshot );

		if ( receivingClientCount > 1 && sv_parallel_sendsnapshot.GetBool() )
		{
			ParallelProcess( pReceivingClients, receivingClientCount, &SV_ParallelSendSnapshot );
		}
		else
		{
			for (int i = 0; i < receivingClientCount; ++i)
			{
				CGameClient *pClient = pReceivingClients[i];
				CClientFrame *pFrame = pClient->GetSendFrame();

				if ( !pFrame )
					continue;
								
				pClient->SendSnapshot( pFrame );
				pClient->UpdateSendState();

			}
		}
	
		pSnapshot->ReleaseReference();
	}

	// Allow game .dll to run code, including unsetting EF_MUZZLEFLASH and EF_NOINTERP on effects fields
	// etc.
	serverGameClients->PostClientMessagesSent();
}

void CGameServer::SetMaxClients( int number )
{
	m_nMaxclients = clamp( number, 1, m_nMaxClientsLimit );

	ConMsg( "maxplayers set to %i\n", m_nMaxclients );

	deathmatch.SetValue( m_nMaxclients > 1 );
}

//-----------------------------------------------------------------------------
// A potential optimization of the client data sending; the optimization
// is based around the fact that we think that we're spending all our time in
// cache misses since we are accessing so much memory
//-----------------------------------------------------------------------------





/*
==============================================================================
SERVER SPAWNING

==============================================================================
*/

void SV_WriteVoiceCodec(bf_write &pBuf)
{
	// Only send the voice codec in multiplayer. Otherwise, we don't want voice.

	SVC_VoiceInit voiceinit( sv.IsMultiplayer() ? sv_voicecodec.GetString() : "", 5 );

	voiceinit.WriteToBuffer( pBuf );
}

// Gets voice data from a client and forwards it to anyone who can hear this client.
ConVar voice_debugfeedbackfrom( "voice_debugfeedbackfrom", "0" );

void SV_BroadcastVoiceData(IClient * cl, int nBytes, char * data, int64 xuid )
{
	// Disable voice?
	if( !sv_voiceenable.GetInt() )
		return;

	// Build voice message once
	SVC_VoiceData voiceData;
	voiceData.m_nFromClient = cl->GetPlayerSlot();
	voiceData.m_nLength = nBytes * 8;	// length in bits
	voiceData.m_DataOut = data;
	voiceData.m_xuid = xuid;

	if ( voice_debugfeedbackfrom.GetBool() )
	{
		Msg( "Sending voice from: %s - playerslot: %d\n", cl->GetClientName(), cl->GetPlayerSlot() + 1 );
	}

	for(int i=0; i < sv.GetClientCount(); i++)
	{
		IClient *pDestClient = sv.GetClient(i);

		bool bSelf = (pDestClient == cl);

		// Only send voice to active clients
		if( !pDestClient->IsActive() )
			continue;

		// Does the game code want cl sending to this client?

		bool bHearsPlayer = pDestClient->IsHearingClient( voiceData.m_nFromClient );
		voiceData.m_bProximity = pDestClient->IsProximityHearingClient( voiceData.m_nFromClient );

		if ( IsX360() && bSelf == true )			
			continue;
			
		if ( !bHearsPlayer && !bSelf )
			continue;	

		voiceData.m_nLength = nBytes * 8;

		// Is loopback enabled?
		if( !bHearsPlayer )
		{
			// Still send something, just zero length (this is so the client 
			// can display something that shows knows the server knows it's talking).
			voiceData.m_nLength = 0;	
		}

		pDestClient->SendNetMsg( voiceData );
	}
}


// UNDONE: "player.mdl" ???  This should be set by name in the DLL
/*
================
SV_CreateBaseline

================
*/
void SV_CreateBaseline (void)
{
	SV_WriteVoiceCodec( sv.m_Signon );

	ServerClass *pClasses = serverGameDLL->GetAllServerClasses();

	// Send SendTable info.
	if ( sv_sendtables.GetInt() )
	{
#ifdef _XBOX
		Error( "sv_sendtables not allowed on XBOX." );
#endif
		sv.m_FullSendTablesBuffer.EnsureCapacity( NET_MAX_PAYLOAD );
		sv.m_FullSendTables.StartWriting( sv.m_FullSendTablesBuffer.Base(), sv.m_FullSendTablesBuffer.Count() );
		
		SV_WriteSendTables( pClasses, sv.m_FullSendTables );
		
		if ( sv.m_FullSendTables.IsOverflowed() )
		{
			Host_Error("SV_CreateBaseline: WriteSendTables overflow.\n" );
			return;
		}

		// Send class descriptions.
		SV_WriteClassInfos(pClasses, sv.m_FullSendTables);

		if ( sv.m_FullSendTables.IsOverflowed() )
		{
			Host_Error("SV_CreateBaseline: WriteClassInfos overflow.\n" );
			return;
		}
	}

	// If we're using the local network backdoor, we'll never use the instance baselines.
	if ( !g_pLocalNetworkBackdoor )
	{
		int		count = 0;
		int		bytes = 0;
		
		for ( int entnum = 0; entnum < sv.num_edicts ; entnum++)
		{
			// get the current server version
			edict_t *edict = sv.edicts + entnum;

			if ( edict->IsFree() || !edict->GetUnknown() )
				continue;

			ServerClass *pClass   = edict->GetNetworkable() ? edict->GetNetworkable()->GetServerClass() : 0;

			if ( !pClass )
			{
				Assert( pClass );
				continue;	// no Class ?
			}

			if ( pClass->m_InstanceBaselineIndex != INVALID_STRING_INDEX )
				continue; // we already have a baseline for this class

			SendTable *pSendTable = pClass->m_pTable;

			//
			// create entity baseline
			//
			
			char packedData[MAX_PACKEDENTITY_DATA];
			bf_write writeBuf( "SV_CreateBaseline->writeBuf", packedData, sizeof( packedData ) );


			// create basline from zero values
			if ( !SendTable_Encode(
				pSendTable, 
				edict->GetUnknown(), 
				&writeBuf, 
				entnum,
				NULL,
				false
				) )
			{
				Host_Error("SV_CreateBaseline: SendTable_Encode returned false (ent %d).\n", entnum);
			}

			// copy baseline into baseline stringtable
			SV_EnsureInstanceBaseline( pClass, entnum, packedData, writeBuf.GetNumBytesWritten() );

			bytes += writeBuf.GetNumBytesWritten();
			count ++;
		}
		DevMsg("Created class baseline: %i classes, %i bytes.\n", count,bytes); 
	}

	g_GameEventManager.ReloadEventDefinitions();

	SVC_GameEventList gameevents;
	char data[NET_MAX_PAYLOAD];
	gameevents.m_DataOut.StartWriting( data, sizeof(data) );

	g_GameEventManager.WriteEventList( &gameevents );
	gameevents.WriteToBuffer( sv.m_Signon );
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : runPhysics -
//-----------------------------------------------------------------------------
bool SV_ActivateServer()
{
	COM_TimestampedLog( "SV_ActivateServer" );
#ifndef SWDS
	EngineVGui()->UpdateProgressBar(PROGRESS_ACTIVATESERVER);
#endif

	COM_TimestampedLog( "serverGameDLL->ServerActivate" );

	bool bPrevState = networkStringTableContainerServer->Lock( false );
	// Activate the DLL server code
	g_pServerPluginHandler->ServerActivate( sv.edicts, sv.num_edicts, sv.GetMaxClients() );

	// all setup is completed, any further precache statements are errors
	sv.m_State = ss_active;
	
	COM_TimestampedLog( "SV_CreateBaseline" );

	// create a baseline for more efficient communications
	SV_CreateBaseline();

	sv.allowsignonwrites = false;

	// set skybox name
	ConVar const *skyname = g_pCVar->FindVar( "sv_skyname" );

	if ( skyname )
	{
		Q_strncpy( sv.m_szSkyname, skyname->GetString(), sizeof( sv.m_szSkyname ) );
	}
	else
	{
		Q_strncpy( sv.m_szSkyname, "unknown", sizeof( sv.m_szSkyname ) );
	}

	COM_TimestampedLog( "Send Reconnects" );

	// Tell connected clients to reconnect
	sv.ReconnectClients();

	// Tell what kind of server has been started.
	if ( sv.IsMultiplayer() )
	{
		ConDMsg ("%i player server started\n", sv.GetMaxClients() );
	}
	else
	{
		ConDMsg ("Game started\n");
	}

	if ( tv_enable.GetBool() )
	{
		if ( CommandLine()->FindParm("-nohltv") )
		{
			// let user now that SourceTV will not work
			ConMsg ("SourceTV is disabled on this server.\n");
		}
		else
		{
			// create SourceTV object if not already there
			if ( !hltv )
			{
				hltv = new CHLTVServer;
				hltv->Init( NET_IsDedicated() );
			}
#if !defined( NO_STEAM )
			Steam3Server().UpdateSpectatorPort( NET_GetUDPPort( NS_HLTV ) );
#endif
			if ( hltv->IsActive() && hltv->IsMasterProxy() )
			{
				// HLTV master already running, just activate client
				hltv->m_MasterClient->ActivatePlayer();
				hltv->StartMaster( hltv->m_MasterClient );
			}
			else
			{
				// create new HLTV client
				CGameClient *cl = (CGameClient*)sv.CreateFakeClient( "SourceTV" );
				hltv->StartMaster( cl );
			}
		}
	}
	else
	{
#if !defined( NO_STEAM )
		Steam3Server().UpdateSpectatorPort( 0 );
#endif		
		// make sure HLTV is disabled
		if ( hltv )
			hltv->Shutdown();
	}

	if (sv.IsDedicated())
	{
		// purge unused models and their data hierarchy (materials, shaders, etc)
		modelloader->PurgeUnusedModels();
	}

	if ( sv.IsMultiplayer() )
	{
		Steam3Server().Activate();
		sv.SetQueryPortFromSteamServer();
	}
	networkStringTableContainerServer->Lock( bPrevState );

	// Heartbeat the master server in case we turned SrcTV on or off.
	Steam3Server().SendUpdatedServerDetails();
	if ( IsUsingMasterLegacyMode() )
		master->Heartbeat_Legacy_f();
#ifndef NO_STEAM
	else
		SteamMasterServerUpdater()->ForceHeartbeat();
#endif

	COM_TimestampedLog( "SV_ActivateServer(finished)" );

	return true;
}

#include "tier0/memdbgoff.h"

static void SV_AllocateEdicts()
{
	sv.edicts = (edict_t *)Hunk_AllocName( sv.max_edicts*sizeof(edict_t), "edicts" );

	// Invoke the constructor so the vtable is set correctly..
	for (int i = 0; i < sv.max_edicts; ++i)
	{
		new( &sv.edicts[i] ) edict_t;
	}

	sv.edictchangeinfo = (IChangeInfoAccessor *)Hunk_AllocName( sv.max_edicts * sizeof( IChangeInfoAccessor ), "edictchangeinfo" );
}

#include "tier0/memdbgon.h"


void CGameServer::ReloadWhitelist( const char *pMapName )
{
	// Always return - until we get the whilelist stuff resolved for TF2.
	if ( m_pPureServerWhitelist )
	{
		m_pPureServerWhitelist->Release();
		m_pPureServerWhitelist = NULL;
	}

	// Don't do sv_pure stuff in SP games.
	if ( GetMaxClients() <= 1 )
		return;

	// Get rid of the old whitelist.
	if ( m_pPureServerWhitelist )
	{
		m_pPureServerWhitelist->Release();
		m_pPureServerWhitelist = NULL;
	}

	// Don't use the whitelist if sv_pure is not set.
	if ( g_sv_pure_mode == 0 )
		return;

	m_pPureServerWhitelist = CPureServerWhitelist::Create( g_pFileSystem );
	if ( g_sv_pure_mode == 2 )
	{
		// sv_pure 2 means to ignore the pure_server_whitelist.txt file and force everything to come from Steam.
		m_pPureServerWhitelist->EnableFullyPureMode();
		Msg( "Server using sv_pure 2.\n" );
	}
	else
	{
		const char *pGlobalWhitelistFilename = "pure_server_whitelist.txt";
		const char *pMapWhitelistSuffix = "_whitelist.txt";
		
		// Load the new whitelist.
		KeyValues *kv = new KeyValues( "" );
		bool bLoaded = kv->LoadFromFile( g_pFileSystem, pGlobalWhitelistFilename, "game" );
		if ( bLoaded )
			bLoaded = m_pPureServerWhitelist->LoadFromKeyValues( kv );
		
		if ( !bLoaded )
			Warning( "Can't load pure server whitelist in %s.\n", pGlobalWhitelistFilename );
		
		// Load the per-map whitelist.
		char testFilename[MAX_PATH] = "maps";
		V_AppendSlash( testFilename, sizeof( testFilename ) );
		V_strncat( testFilename, pMapName, sizeof( testFilename ) );
		V_strncat( testFilename, pMapWhitelistSuffix, sizeof( testFilename ) );
		
		kv->Clear();
		if ( kv->LoadFromFile( g_pFileSystem, testFilename ) )
			m_pPureServerWhitelist->LoadFromKeyValues( kv );

		kv->deleteThis();
	}

	// Now cache in all files that we'll need CRCs for later..
	float startTime = Plat_FloatTime();
	Msg( "Caching file CRCs for pure server...\n" );	

	int oldFlags = g_pFileSystem->GetWhitelistSpewFlags();
	if ( sv_pure_trace.GetInt() > 0 )
		g_pFileSystem->SetWhitelistSpewFlags( oldFlags | WHITELIST_SPEW_WHILE_LOADING );	
	
	m_pPureServerWhitelist->CacheFileCRCs();

	g_pFileSystem->SetWhitelistSpewFlags( oldFlags );	
	Msg( "Finished caching file CRCs for pure server in %d seconds.\n", (int)(Plat_FloatTime() - startTime) );
}


/*
================
SV_SpawnServer

This is called at the start of each level
================
*/
bool CGameServer::SpawnServer( char *mapname, char *startspot )
{
	int		i;
	char	szDllName[MAX_QPATH];

	Assert( serverGameClients );

	ReloadWhitelist( mapname );

	COM_TimestampedLog( "SV_SpawnServer(%s)", mapname );
#ifndef SWDS
	EngineVGui()->UpdateProgressBar(PROGRESS_SPAWNSERVER);
#endif
	COM_SetupLogDir( mapname );

	g_Log.Open();
	g_Log.Printf( "Loading map \"%s\"\n", mapname );
	g_Log.PrintServerVars();

#ifndef SWDS
	SCR_CenterStringOff();
#endif

	if ( startspot )
	{
		ConDMsg("Spawn Server: %s: [%s]\n", mapname, startspot );
	}
	else
	{
		ConDMsg("Spawn Server: %s\n", mapname );
	}

	// Any partially connected client will be restarted if the spawncount is not matched.
	gHostSpawnCount = ++m_nSpawnCount;

	//
	// make cvars consistant
	//
	deathmatch.SetValue( IsMultiplayer() ? 1 : 0 );
	if ( coop.GetInt() )
	{
		deathmatch.SetValue( 0 );
	}

	current_skill = (int)(skill.GetFloat() + 0.5);
	current_skill = max( current_skill, 0 );
	current_skill = min( current_skill, 3 );

	skill.SetValue( (float)current_skill );

	COM_TimestampedLog( "StaticPropMgr()->LevelShutdown()" );

#if !defined( SWDS )
	g_pShadowMgr->LevelShutdown();
#endif // SWDS
	StaticPropMgr()->LevelShutdown();

	// if we have a relay proxy running, stop it now
	if ( hltv && !hltv->IsMasterProxy() )
	{
		hltv->Shutdown();
	}

	COM_TimestampedLog( "Host_FreeToLowMark" );

	Host_FreeStateAndWorld( true );
	Host_FreeToLowMark( true );

	COM_TimestampedLog( "sv.Clear()" );

	Clear();

	COM_TimestampedLog( "framesnapshotmanager->LevelChanged()" );

	// Clear out the state of the most recently sent packed entities from
	// the snapshot manager
	framesnapshotmanager->LevelChanged();

	// set map name
	Q_strncpy( m_szMapname, mapname, sizeof( m_szMapname ) );

	// set startspot
	if (startspot)
	{
		Q_strncpy(m_szStartspot, startspot, sizeof( m_szStartspot ) );
	}
	else
	{
		m_szStartspot[0] = 0;
	}

	if ( g_FlushMemoryOnNextServer )
	{
		g_FlushMemoryOnNextServer = false;
		if ( IsX360() )
		{
			g_pQueuedLoader->PurgeAll();
		}
		g_pDataCache->Flush();
		g_pMaterialSystem->CompactMemory();
		g_pFileSystem->AsyncFinishAll();
#if !defined( SWDS )
		extern CThreadMutex g_SndMutex;
		g_SndMutex.Lock();
		g_pFileSystem->AsyncSuspend();
		g_pThreadPool->SuspendExecution();
		//g_pMemAlloc->CompactHeap();
		g_pThreadPool->ResumeExecution();
		g_pFileSystem->AsyncResume();
		g_SndMutex.Unlock();
#endif // SWDS
	}

	// Preload any necessary data from the xzps:
	g_pFileSystem->SetupPreloadData();
	g_pMDLCache->InitPreloadData( false );

	// Allocate server memory
	max_edicts = MAX_EDICTS;


	g_ServerGlobalVariables.maxEntities = max_edicts;
	g_ServerGlobalVariables.maxClients = GetMaxClients();
#ifndef SWDS
	g_ClientGlobalVariables.network_protocol = PROTOCOL_VERSION;
#endif

	// Assume no entities beyond world and client slots
	num_edicts = GetMaxClients()+1;

	COM_TimestampedLog( "SV_AllocateEdicts" );

	SV_AllocateEdicts();

	serverGameEnts->SetDebugEdictBase( edicts );

	allowsignonwrites = true;

	serverclasses = 0;		// number of unique server classes
	serverclassbits = 0;		// log2 of serverclasses

	// Assign class ids to server classes here so we can encode temp ents into signon
	//  if needed
	AssignClassIds();

	COM_TimestampedLog( "Set up players" );

	// allocate player data, and assign the values into the edicts
	for ( i=0 ; i< GetClientCount() ; i++ )
	{
		CGameClient * cl = Client(i);

		// edict for a player is slot + 1, world = 0
		cl->edict = edicts + i + 1;
	
		// Setup up the edict
		InitializeEntityDLLFields( cl->edict );
	}

	COM_TimestampedLog( "Set up players(done)" );

	m_State = ss_loading;
	
	// Set initial time values.
	m_flTickInterval = host_state.interval_per_tick;
	m_nTickCount = (int)( 1.0 / host_state.interval_per_tick ) + 1; // Start at appropriate 1

	g_ServerGlobalVariables.tickcount = m_nTickCount;
	g_ServerGlobalVariables.curtime = GetTime();

	// Load the world model.
	char szModelName[MAX_PATH];
	char szNameOnDisk[MAX_PATH];
	Q_snprintf( szModelName, sizeof( szModelName ), "maps/%s.bsp", mapname );
	GetMapNameOnDisk( szNameOnDisk, szModelName, sizeof( szNameOnDisk ) );
	g_pFileSystem->AddSearchPath( szNameOnDisk, "GAME", PATH_ADD_TO_HEAD );
	g_pFileSystem->BeginMapAccess();

	if ( !CommandLine()->FindParm( "-allowstalezip" ) )
	{
		if ( g_pFileSystem->FileExists( "stale.txt", "GAME" ) )
		{
			Warning( "This map is not final!!  Needs to be rebuilt without -keepstalezip and without -onlyents\n" );
		}
	}

	COM_TimestampedLog( "modelloader->GetModelForName(%s) -- Start", szModelName );

	host_state.SetWorldModel( modelloader->GetModelForName( szModelName, IModelLoader::FMODELLOADER_SERVER ) );
	if ( !host_state.worldmodel )
	{
		ConMsg( "Couldn't spawn server %s\n", szModelName );
		m_State = ss_dead;
		g_pFileSystem->EndMapAccess();
		return false;
	}

	COM_TimestampedLog( "modelloader->GetModelForName(%s) -- Finished", szModelName );

	if ( IsMultiplayer() && !IsX360() )
	{
#ifndef SWDS
		EngineVGui()->UpdateProgressBar(PROGRESS_CRCMAP);
#endif
		// Server map CRC check.
		CRC32_Init(&worldmapCRC);
		if ( !CRC_MapFile( &worldmapCRC, szNameOnDisk ) )
		{
			ConMsg( "Couldn't CRC server map: %s\n", szNameOnDisk );
			m_State = ss_dead;
			g_pFileSystem->EndMapAccess();
			return false;
		}

#ifndef SWDS
		EngineVGui()->UpdateProgressBar(PROGRESS_CRCCLIENTDLL);
#endif

		// DLL CRC check.
		Q_snprintf( szDllName, sizeof( szDllName ), "bin\\client.dll" );
		Q_FixSlashes( szDllName );
		if ( !CRC_File( &clientDllCRC, szDllName ) )
		{
			clientDllCRC = 0xFFFFFFFF; // we don't require a CRC, its optional
		}
	}
	else
	{
		worldmapCRC	= 0;
		clientDllCRC = 0;
	}

	m_StringTables = networkStringTableContainerServer;

	COM_TimestampedLog( "SV_CreateNetworkStringTables" );

#ifndef SWDS
	EngineVGui()->UpdateProgressBar(PROGRESS_CREATENETWORKSTRINGTABLES);
#endif

	// Create network string tables ( including precache tables )
	SV_CreateNetworkStringTables();

	// Leave empty slots for models/sounds/generic (not for decals though)
	PrecacheModel( "", 0 );
	PrecacheGeneric( "", 0 );
	PrecacheSound( "", 0 );

	COM_TimestampedLog( "Precache world model (%s)", szModelName );

#ifndef SWDS
	EngineVGui()->UpdateProgressBar(PROGRESS_PRECACHEWORLD);
#endif
	// Add in world
	PrecacheModel( szModelName, RES_FATALIFMISSING | RES_PRELOAD, host_state.worldmodel );

	COM_TimestampedLog( "Precache brush models" );

	// Add world submodels to the model cache
	for ( i = 1 ; i < host_state.worldbrush->numsubmodels ; i++ )
	{
		// Add in world brush models
		char localmodel[5]; // inline model names "*1", "*2" etc
		Q_snprintf( localmodel, sizeof( localmodel ), "*%i", i );

		PrecacheModel( localmodel, RES_FATALIFMISSING | RES_PRELOAD, modelloader->GetModelForName( localmodel, IModelLoader::FMODELLOADER_SERVER ) );
	}

#ifndef SWDS
	EngineVGui()->UpdateProgressBar(PROGRESS_CLEARWORLD);
#endif
	COM_TimestampedLog( "SV_ClearWorld" );

	// Clear world interaction links
	// Loads and inserts static props
	SV_ClearWorld();

	//
	// load the rest of the entities
	//

	COM_TimestampedLog( "InitializeEntityDLLFields" );

	InitializeEntityDLLFields( edicts );
	edicts->ClearFree();

	if (coop.GetFloat())
	{
		g_ServerGlobalVariables.coop = (coop.GetInt() != 0);
	}
	else
	{
		g_ServerGlobalVariables.deathmatch = (deathmatch.GetInt() != 0);
	}

	g_ServerGlobalVariables.mapname   = MAKE_STRING( m_szMapname );
	g_ServerGlobalVariables.startspot = MAKE_STRING( m_szStartspot );

	GetTestScriptMgr()->CheckPoint( "map_load" );

	// set game event
	IGameEvent *event = g_GameEventManager.CreateEvent( "server_spawn" );
	if ( event )
	{
		event->SetString( "hostname", host_name.GetString() );
		event->SetString( "address", net_local_adr.ToString( false ) );
		event->SetInt(    "port", GetUDPPort() );
		event->SetString( "game", com_gamedir );
		event->SetString( "mapname", GetMapName() );
		event->SetInt(    "maxplayers", GetMaxClients() );
		event->SetInt(	  "password", 0 );				// TODO
#if defined( _WIN32 )
		event->SetString( "os", "WIN32" );
#else
		event->SetString( "os", "LINUX" );
#endif
		event->SetInt( "dedicated", IsDedicated() ? 1 : 0 );

		g_GameEventManager.FireEvent( event );
	}

	COM_TimestampedLog( "SV_SpawnServer -- Finished" );

	g_pFileSystem->EndMapAccess();
	return true;
}


void CGameServer::UpdateMasterServerPlayers()
{
	for ( int i=0; i < GetClientCount() ; i++ )
	{
		CGameClient *client = Client(i);
		
		if ( !client->IsConnected() )
			continue;

		CPlayerState *pl = serverGameClients->GetPlayerState( client->edict );
		if ( !pl )
			continue;

		if ( !client->m_SteamID || !client->m_SteamID->IsValid() )
			continue;
#ifndef NO_STEAM
		SteamGameServer()->GSSetUserData( *client->m_SteamID, client->GetClientName(), pl->frags );
#endif
	}
}


//-----------------------------------------------------------------------------
// SV_IsSimulating
//-----------------------------------------------------------------------------
bool SV_IsSimulating( void )
{
	if ( sv.IsPaused() )
		return false;

#ifndef SWDS
	// Don't simulate in single player if console is down or the bug UI is active and we're in a game 
	if ( !sv.IsMultiplayer() )
	{
		if ( g_LostVideoMemory )
			return false;

		// Don't simulate in single player if console is down or the bug UI is active and we're in a game 
		if ( cl.IsActive() && ( Con_IsVisible() || EngineVGui()->ShouldPause() ) )
			return false;
	}
#endif //SWDS
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool SV_HasPlayers()
{
	/*int i;
	for ( i = 0; i < sv.clients.Count(); i++ )
	{
		if ( sv.clients[ i ]->active )
		{
			return true;
		}
	}

	return false; */

	return sv.GetClientCount() > 0;
}

//-----------------------------------------------------------------------------
// Purpose: Run physics code (simulating == false means we're paused, but we'll still
//  allow player usercmds to be processed
//-----------------------------------------------------------------------------
void SV_Think( bool bIsSimulating )
{
	VPROF( "SV_Physics" );
	
	g_ServerGlobalVariables.tickcount   = sv.m_nTickCount;
	g_ServerGlobalVariables.curtime		= sv.GetTime();
	g_ServerGlobalVariables.frametime	= bIsSimulating ? host_state.interval_per_tick : 0;

	// in singleplayer only run think/simulation if localplayer is connected
	bIsSimulating =  bIsSimulating && ( sv.IsMultiplayer() || cl.IsActive() );

	g_pServerPluginHandler->GameFrame( bIsSimulating );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : simulating - 
//-----------------------------------------------------------------------------
void SV_PreClientUpdate(bool bIsSimulating )
{
	if ( !serverGameDLL )
		return;

	serverGameDLL->PreClientUpdate( bIsSimulating );
}


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
/*
==================
SV_Frame

==================
*/
CFunctor *g_pDeferredServerWork;

void SV_FrameExecuteThreadDeferred()
{
	if ( g_pDeferredServerWork )
	{
		(*g_pDeferredServerWork)();
		delete g_pDeferredServerWork;
		g_pDeferredServerWork = NULL;
	}
}

void SV_SendClientUpdates( bool bIsSimulating, bool bSendDuringPause )
{
	bool bForcedSend = s_bForceSend;
	s_bForceSend = false;

	// ask game.dll to add any debug graphics
	SV_PreClientUpdate( bIsSimulating );

	// This causes network messages to be sent
	sv.SendClientMessages( bIsSimulating || bForcedSend );

	// tricky, increase stringtable tick at least one tick
	// so changes made after this point are not counted to this server
	// frame since we already send out the client snapshots
	networkStringTableContainerServer->SetTick( sv.m_nTickCount + 1 );
}

void SV_Frame( bool finalTick )
{
	VPROF( "SV_Frame" );

	if ( serverGameDLL && finalTick )
	{
		serverGameDLL->Think( finalTick );
	}

	if ( !sv.IsActive() || !Host_ShouldRun() )
	{
		return;
	}

	g_ServerGlobalVariables.frametime = host_state.interval_per_tick;

	bool bIsSimulating = SV_IsSimulating();
	bool bSendDuringPause = sv_noclipduringpause ? sv_noclipduringpause->GetBool() : false;

	// unlock sting tables to allow changes, helps to find unwanted changes (bebug build only)
	networkStringTableContainerServer->Lock( false );
	
	// Run any commands from client and play client Think functions if it is time.
	sv.RunFrame(); // read network input etc

	if ( SV_HasPlayers() )
	{	
		bool serverCanSimulate = ( serverGameDLL && !serverGameDLL->IsRestoring() ) ? true : false;

		if ( serverCanSimulate && ( bIsSimulating || bSendDuringPause ) )
		{
			sv.m_nTickCount++;

			networkStringTableContainerServer->SetTick( sv.m_nTickCount );
		}

		SV_Think( bIsSimulating );
	}
	else if ( sv.IsMultiplayer() )
	{
		SV_Think( false );	// let the game.dll systems think
	}

	// Send the results of movement and physics to the clients
	if ( finalTick )
	{
		if ( !IsEngineThreaded() || sv.IsMultiplayer() )
			SV_SendClientUpdates( bIsSimulating, bSendDuringPause );
		else
			g_pDeferredServerWork = CreateFunctor( SV_SendClientUpdates, bIsSimulating, bSendDuringPause );

	}

	// lock string tables
	networkStringTableContainerServer->Lock( true );

	// let the steam auth server process new connections
	if ( IsPC() && sv.IsMultiplayer() )
	{
		Steam3Server().RunFrame();
	}
}

