//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Basic BOT handling.
//
// $Workfile:     $
// $Date:         $
//
//-----------------------------------------------------------------------------
// $Log: $
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"
#include "cs_player.h"
#include "in_buttons.h"
#include "movehelper_server.h"
#include "team.h"
#include "cs_gamerules.h"
#include "client.h"


void Bot_Think( CCSPlayer *pBot );

ConVar bot_forcefireweapon( "bot_forcefireweapon", "", 0, "Force bots with the specified weapon to fire." );
ConVar bot_forceattack2( "bot_forceattack2", "0", 0, "When firing, use attack2." );
ConVar bot_forceattackon( "bot_forceattackon", "0", 0, "When firing, don't tap fire, hold it down." );
ConVar bot_flipout( "bot_flipout", "0", 0, "When on, all bots fire their guns." );
ConVar bot_mimic( "bot_mimic", "0", 0, "Bot uses usercmd of player by index." );
static ConVar bot_mimic_yaw_offset( "bot_mimic_yaw_offset", "0", 0, "Offsets the bot yaw." );

static int BotNumber = 1;
static int g_iNextBotTeam = -1;


typedef struct
{
	bool			backwards;

	float			nextturntime;
	bool			lastturntoright;

	float			nextstrafetime;
	float			sidemove;

	QAngle			forwardAngle;
	QAngle			lastAngles;

	int m_WantedTeam;
	float m_flJoinTeamTime;

	bool m_bTempBot;	// Is this slot a dump temp bot or a real bot?
} botdata_t;

static botdata_t g_BotData[ MAX_PLAYERS ];

//-----------------------------------------------------------------------------
// Purpose: Create a new Bot and put it in the game.
// Output : Pointer to the new Bot, or NULL if there's no free clients.
//-----------------------------------------------------------------------------
CBasePlayer *BotPutInServer( bool bFrozen, int iTeam )
{
	g_iNextBotTeam = iTeam;

	char botname[ 64 ];
	Q_snprintf( botname, sizeof( botname ), "Bot%02i", BotNumber );

	edict_t *pEdict = engine->CreateFakeClient( botname );

	if (!pEdict)
	{
		Msg( "Failed to create Bot.\n");
		return NULL;
	}

	// Allocate a CBasePlayer for the bot, and call spawn
	//ClientPutInServer( pEdict, botname );
	//ClientActive( pEdict, false );

	CCSPlayer *pPlayer = ((CCSPlayer *)CBaseEntity::Instance( pEdict ));
	pPlayer->ClearFlags();
	pPlayer->AddFlag( FL_CLIENT | FL_FAKECLIENT );

	if ( bFrozen )
		pPlayer->AddEFlags( EFL_BOT_FROZEN );

	if ( iTeam == -1 )
		iTeam = ( pPlayer->entindex() & 1 ) ? TEAM_TERRORIST : TEAM_CT;

	botdata_t *pData = &g_BotData[pPlayer->entindex()-1];
	pData->m_WantedTeam = iTeam;
	pData->m_flJoinTeamTime = gpGlobals->curtime + 0.3;
	pData->m_bTempBot = true;

	BotNumber++;
	return pPlayer;
}

bool IsTempBot( CBaseEntity *pEnt )
{
	if ( !pEnt )
		return false;
	
	if ( !(pEnt->GetFlags() & FL_FAKECLIENT) )
		return false;

	int i = pEnt->entindex();
	if ( i >= 1 && i < MAX_PLAYERS )
		return g_BotData[i-1].m_bTempBot;
	else
		return false;
}

//-----------------------------------------------------------------------------
// Purpose: Run through all the Bots in the game and let them think.
//-----------------------------------------------------------------------------
void Bot_RunAll( void )
{
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CCSPlayer *pPlayer = ToCSPlayer( UTIL_PlayerByIndex( i ) );

		if ( IsTempBot( pPlayer ) )
		{
			Bot_Think( pPlayer );
		}
	}
}

bool RunMimicCommand( CUserCmd& cmd )
{
	if ( bot_mimic.GetInt() <= 0 )
		return false;

	if ( bot_mimic.GetInt() > gpGlobals->maxClients )
		return false;

	
	CBasePlayer *pPlayer = UTIL_PlayerByIndex( bot_mimic.GetInt() );
	if ( !pPlayer )
		return false;

	if ( !pPlayer->GetLastUserCommand() )
		return false;

	cmd = *pPlayer->GetLastUserCommand();
	cmd.viewangles[YAW] += bot_mimic_yaw_offset.GetFloat();
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Simulates a single frame of movement for a player
// Input  : *fakeclient - 
//			*viewangles - 
//			forwardmove - 
//			sidemove - 
//			upmove - 
//			buttons - 
//			impulse - 
//			msec - 
// Output : 	virtual void
//-----------------------------------------------------------------------------
static void RunPlayerMove( CCSPlayer *fakeclient, const QAngle& viewangles, float forwardmove, float sidemove, float upmove, unsigned short buttons, byte impulse, float frametime )
{
	if ( !fakeclient )
		return;

	CUserCmd cmd;

	// Store off the globals.. they're gonna get whacked
	float flOldFrametime = gpGlobals->frametime;
	float flOldCurtime = gpGlobals->curtime;

	float flTimeBase = gpGlobals->curtime;
	fakeclient->SetTimeBase( flTimeBase );

	Q_memset( &cmd, 0, sizeof( cmd ) );

	if ( !RunMimicCommand( cmd ) )
	{
		VectorCopy( viewangles, cmd.viewangles );
		cmd.forwardmove = forwardmove;
		cmd.sidemove = sidemove;
		cmd.upmove = upmove;
		cmd.buttons = buttons;
		cmd.impulse = impulse;
		cmd.random_seed = random->RandomInt( 0, 0x7fffffff );
	}

	MoveHelperServer()->SetHost( fakeclient );
	fakeclient->PlayerRunCommand( &cmd, MoveHelperServer() );

	// save off the last good usercmd
	fakeclient->SetLastUserCommand( cmd );

	// Clear out any fixangle that has been set
	fakeclient->pl.fixangle = FIXANGLE_NONE;

	// Restore the globals..
	gpGlobals->frametime = flOldFrametime;
	gpGlobals->curtime = flOldCurtime;
}

//-----------------------------------------------------------------------------
// Purpose: Run this Bot's AI for one frame.
//-----------------------------------------------------------------------------
void Bot_Think( CCSPlayer *pBot )
{
	// Make sure we stay being a bot
	pBot->AddFlag( FL_FAKECLIENT );

	botdata_t *botdata = &g_BotData[ pBot->entindex() - 1 ];

	float forwardmove = 0.0;
	float sidemove = botdata->sidemove;
	float upmove = 0.0;
	unsigned short buttons = 0;
	byte  impulse = 0;
	float frametime = gpGlobals->frametime;

	if ( pBot->GetTeamNumber() == TEAM_UNASSIGNED && gpGlobals->curtime > botdata->m_flJoinTeamTime )
	{
		pBot->HandleCommand_JoinTeam( botdata->m_WantedTeam );
	}
	else if ( pBot->GetTeamNumber() != TEAM_UNASSIGNED && pBot->PlayerClass() == CS_CLASS_NONE )
	{
		// If they're on a team but haven't picked a class, choose a random class..
		pBot->HandleCommand_JoinClass( 0 );
	}
	else
	{
		QAngle vecViewAngles;
		vecViewAngles = pBot->GetLocalAngles();

		// Create some random values
		if ( pBot->IsAlive() && (pBot->GetSolid() == SOLID_BBOX) )
		{
			trace_t trace;

			// Stop when shot
			if ( !pBot->IsEFlagSet(EFL_BOT_FROZEN) )
			{
				if ( pBot->m_iHealth == 100 )
				{
					forwardmove = 600 * ( botdata->backwards ? -1 : 1 );
					if ( botdata->sidemove != 0.0f )
					{
						forwardmove *= random->RandomFloat( 0.1, 1.0f );
					}
				}
				else
				{
					forwardmove = 0;
				}
			}

			// Only turn if I haven't been hurt
			if ( !pBot->IsEFlagSet(EFL_BOT_FROZEN) && pBot->m_iHealth == 100 )
			{
				Vector vecEnd;
				Vector forward;

				QAngle angle;
				float angledelta = 15.0;

				int maxtries = (int)360.0/angledelta;

				if ( botdata->lastturntoright )
				{
					angledelta = -angledelta;
				}

				angle = pBot->GetLocalAngles();

				Vector vecSrc;
				while ( --maxtries >= 0 )
				{
					AngleVectors( angle, &forward, NULL, NULL );

					vecSrc = pBot->GetLocalOrigin() + Vector( 0, 0, 36 );

					vecEnd = vecSrc + forward * 10;

					UTIL_TraceHull( vecSrc, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, 
						MASK_PLAYERSOLID, pBot, COLLISION_GROUP_NONE, &trace );

					if ( trace.fraction == 1.0 )
					{
						//if ( gpGlobals->curtime < botdata->nextturntime )
						//{
						break;
						//}
					}

					angle.y += angledelta;

					if ( angle.y > 180 )
						angle.y -= 360;
					else if ( angle.y < -180 )
						angle.y += 360;

					botdata->nextturntime = gpGlobals->curtime + 2.0;
					botdata->lastturntoright = random->RandomInt( 0, 1 ) == 0 ? true : false;

					botdata->forwardAngle = angle;
					botdata->lastAngles = angle;

				}


				/*
				if ( gpGlobals->curtime >= botdata->nextstrafetime )
				{
					botdata->nextstrafetime = gpGlobals->curtime + 1.0f;

					if ( random->RandomInt( 0, 5 ) == 0 )
					{
						botdata->sidemove = -600.0f + 1200.0f * random->RandomFloat( 0, 2 );
					}
					else
					{
						botdata->sidemove = 0;
					}
					sidemove = botdata->sidemove;

					if ( random->RandomInt( 0, 20 ) == 0 )
					{
						botdata->backwards = true;
					}
					else
					{
						botdata->backwards = false;
					}
				}
				*/

				pBot->SetLocalAngles( angle );
				vecViewAngles = angle;
			}

			// If bots are being forced to fire a weapon, see if I have it
			else if ( bot_forcefireweapon.GetString() )
			{
				CBaseCombatWeapon *pWeapon = pBot->Weapon_OwnsThisType( bot_forcefireweapon.GetString() );
				if ( pWeapon )
				{
					// Switch to it if we don't have it out
					CBaseCombatWeapon *pActiveWeapon = pBot->GetActiveWeapon();

					// Switch?
					if ( pActiveWeapon != pWeapon )
					{
						pBot->Weapon_Switch( pWeapon );
					}
					else
					{
						// Start firing
						// Some weapons require releases, so randomise firing
						if ( bot_forceattackon.GetBool() || (RandomFloat(0.0,1.0) > 0.5) )
						{
							buttons |= bot_forceattack2.GetBool() ? IN_ATTACK2 : IN_ATTACK;
						}
					}
				}
			}

			if ( bot_flipout.GetInt() )
			{
				if ( bot_forceattackon.GetBool() || (RandomFloat(0.0,1.0) > 0.5) )
				{
					buttons |= bot_forceattack2.GetBool() ? IN_ATTACK2 : IN_ATTACK;
				}
			}
		}
		else
		{
			// Wait for Reinforcement wave
			if ( !pBot->IsAlive() )
			{
				// Try hitting my buttons occasionally
				if ( random->RandomInt( 0, 100 ) > 80 )
				{
					// Respawn the bot
					if ( random->RandomInt( 0, 1 ) == 0 )
					{
						buttons |= IN_JUMP;
					}
					else
					{
						buttons = 0;
					}
				}
			}
		}

		if ( bot_flipout.GetInt() >= 2 )
		{

			QAngle angOffset = RandomAngle( -1, 1 );

			botdata->lastAngles += angOffset;

			for ( int i = 0 ; i < 2; i++ )
			{
				if ( fabs( botdata->lastAngles[ i ] - botdata->forwardAngle[ i ] ) > 15.0f )
				{
					if ( botdata->lastAngles[ i ] > botdata->forwardAngle[ i ] )
					{
						botdata->lastAngles[ i ] = botdata->forwardAngle[ i ] + 15;
					}
					else
					{
						botdata->lastAngles[ i ] = botdata->forwardAngle[ i ] - 15;
					}
				}
			}

			botdata->lastAngles[ 2 ] = 0;

			pBot->SetLocalAngles( botdata->lastAngles );
		}
	}

	// Fix up the m_fEffects flags
	pBot->PostClientMessagesSent();

	pBot->SetPunchAngle( QAngle( 0, 0, 0 ) );
	RunPlayerMove( pBot, pBot->GetLocalAngles(), forwardmove, sidemove, upmove, buttons, impulse, frametime );
}



// Handler for the "bot" command.
CON_COMMAND_F( "bot_old", "Add a bot.", FCVAR_CHEAT )
{
	// Disable the CS bots, otherwise they'll interfere with the bot code here.
	//extern bool g_bEnableCSBots;
	//g_bEnableCSBots = false;

	CCSPlayer *pPlayer = CCSPlayer::Instance( UTIL_GetCommandClientIndex() );

	// The bot command uses switches like command-line switches.
	// -count <count> tells how many bots to spawn.
	// -team <index> selects the bot's team. Default is -1 which chooses randomly.
	//	Note: if you do -team !, then it 
	// -class <index> selects the bot's class. Default is -1 which chooses randomly.
	// -frozen prevents the bots from running around when they spawn in.

	int count = args.FindArgInt( "-count", 1 );
	count = clamp( count, 1, 16 );

	int iTeam = -1;
	const char *pVal = args.FindArg( "-team" );
	if ( pVal )
	{
		if ( pVal[0] == '!' )
		{
			if ( pPlayer->GetTeamNumber() == TEAM_TERRORIST )
				iTeam = TEAM_CT;
			else
				iTeam = TEAM_TERRORIST;
		}
		else if ( pVal[0] == 't' || pVal[0] == 'T' )
		{
			iTeam = TEAM_TERRORIST;
		}
		else if ( pVal[0] == 'c' || pVal[0] == 'C' )
		{
			iTeam = TEAM_CT;
		}
		else
		{
			iTeam = atoi( pVal );
			if ( iTeam == 1 )
				iTeam = TEAM_TERRORIST;
			else
				iTeam = TEAM_CT;
		}
	}

	// Look at -frozen.
	bool bFrozen = !!args.FindArg( "-frozen" );
		
	// Ok, spawn all the bots.
	while ( --count >= 0 )
	{
		extern CBasePlayer *BotPutInServer( bool bFrozen, int iTeam );
		BotPutInServer( bFrozen, iTeam );
	}
}


// Handle the "PossessBot" command.
void PossessBot_f( const CCommand &args )
{
	CCSPlayer *pPlayer = CCSPlayer::Instance( UTIL_GetCommandClientIndex() );
	if ( !pPlayer )
		return;

	// Put the local player in control of this bot.
	if ( args.ArgC() != 2 )
	{
		Warning( "PossessBot <client index>\n" );
		return;
	}

	int iBotClient = atoi( args[1] );
	int iBotEnt = iBotClient + 1;
	
	if ( iBotClient < 0 || 
		iBotClient >= gpGlobals->maxClients || 
		pPlayer->entindex() == iBotEnt )
	{
		Warning( "PossessBot <client index>\n" );
	}
	else
	{
		edict_t *pPlayerData = pPlayer->edict();
		edict_t *pBotData = engine->PEntityOfEntIndex( iBotEnt );
		if ( pBotData && pBotData )
		{
			// SWAP EDICTS

			// Backup things we don't want to swap.
			edict_t oldPlayerData = *pPlayerData;
			edict_t oldBotData = *pBotData;

			// Swap edicts.
			edict_t tmp = *pPlayerData;
			*pPlayerData = *pBotData;
			*pBotData = tmp;

			// Restore things we didn't want to swap.
			//pPlayerData->m_EntitiesTouched = oldPlayerData.m_EntitiesTouched;
			//pBotData->m_EntitiesTouched = oldBotData.m_EntitiesTouched;
		
			CBaseEntity *pPlayerBaseEnt = CBaseEntity::Instance( pPlayerData );
			CBaseEntity *pBotBaseEnt = CBaseEntity::Instance( pBotData );

			// Make the other a bot and make the player not a bot.
			pPlayerBaseEnt->RemoveFlag( FL_FAKECLIENT );
			pBotBaseEnt->AddFlag( FL_FAKECLIENT );
		
						
			// Point the CBaseEntities at the right players.
			pPlayerBaseEnt->NetworkProp()->SetEdict( pPlayerData );
			pBotBaseEnt->NetworkProp()->SetEdict( pBotData );

			// Freeze the bot.
			pBotBaseEnt->AddEFlags( EFL_BOT_FROZEN );
		}
	}
}


ConCommand cc_PossessBot( "PossessBot", PossessBot_f, "Toggle. Possess a bot.\n\tArguments: <bot client number>", FCVAR_CHEAT );

