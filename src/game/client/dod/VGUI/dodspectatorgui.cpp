//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"
#include "dodspectatorgui.h"
#include "hud.h"
#include "dod_shareddefs.h"

#include <vgui/ILocalize.h>
#include <vgui/ISurface.h>
#include <imapoverview.h>
#include "dod_gamerules.h"
#include "c_team.h"
#include "c_dod_team.h"
#include "c_dod_player.h"
#include "c_dod_playerresource.h"

using namespace vgui;

extern ConVar _cl_classmenuopen;

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CDODSpectatorGUI::CDODSpectatorGUI(IViewPort *pViewPort) : CSpectatorGUI(pViewPort)
{
	m_pReinforcementsLabel	= NULL;
	m_pAlliesScoreLabel	= NULL;
	m_pAxisScoreLabel	= NULL;
	m_pTimerLabel = NULL;
	m_pExtraInfo = NULL;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CDODSpectatorGUI::ApplySchemeSettings(vgui::IScheme *pScheme)
{
	BaseClass::ApplySchemeSettings( pScheme );

	m_pReinforcementsLabel	= dynamic_cast<Label *>(FindChildByName("ReinforcementsLabel"));
	m_pAlliesScoreLabel	= dynamic_cast<Label *>(FindChildByName("AlliesScoreValue"));
	m_pAxisScoreLabel	= dynamic_cast<Label *>(FindChildByName("AxisScoreValue"));	
	m_pTimerLabel = dynamic_cast<Label *>(FindChildByName("timerlabel"));
	m_pExtraInfo = dynamic_cast<Label *>(FindChildByName("extrainfo"));
}

bool CDODSpectatorGUI::NeedsUpdate( void )
{
	if ( !C_BasePlayer::GetLocalPlayer() )
		return false;

	if( IsVisible() )
		return true;

	//if ( DODGameRules()->IsGameUnderTimeLimit() && m_nLastTime != DODGameRules()->GetTimeLeft() )
	//	return true;

	if ( m_nLastSpecMode != C_BasePlayer::GetLocalPlayer()->GetObserverMode() )
		return true;

	if ( m_nLastSpecTarget != C_BasePlayer::GetLocalPlayer()->GetObserverTarget() )
		return true;

	return BaseClass::NeedsUpdate();
}

Color CDODSpectatorGUI::GetClientColor(int index)
{
	C_BasePlayer *player = ToBasePlayer( ClientEntityList().GetEnt( index) );

	int team = player->GetTeamNumber();

	Assert( team == TEAM_ALLIES || team == TEAM_AXIS || team == TEAM_SPECTATOR );

	if ( GameResources() )
		return GameResources()->GetTeamColor( team );		
	else
		return Color( 255, 255, 255, 255 );
}

void CDODSpectatorGUI::Update()
{
	BaseClass::Update();
	
	C_BasePlayer *pLocalPlayer = C_BasePlayer::GetLocalPlayer();

	if( pLocalPlayer )
	{
		m_nLastSpecMode = pLocalPlayer->GetObserverMode();
		m_nLastSpecTarget = pLocalPlayer->GetObserverTarget();
	}

	UpdateTimer();

	UpdateScores();
}

void CDODSpectatorGUI::UpdateTimer( void )
{
	C_DODPlayer *pPlayer = C_DODPlayer::GetLocalDODPlayer();

	if( !pPlayer || pPlayer->IsHLTV() )
	{
		wchar_t wText[ 63 ];

		int timer;
		timer = (int)( DODGameRules()->GetTimeLeft() );
		if ( timer < 0 )
			 timer  = 0;

		_snwprintf ( wText, sizeof(wText)/sizeof(wchar_t), L"%d:%02d", (timer / 60), (timer % 60) );
		wText[62] = 0;

		m_pTimerLabel->SetText( wText );
		m_pReinforcementsLabel->SetText( wText );
	}
	else if( pPlayer->GetTeamNumber() == TEAM_SPECTATOR )
	{
		m_pTimerLabel->SetText( L"" );
		m_pReinforcementsLabel->SetText( L"" );
	}
	else
	{
		if( !m_pReinforcementsLabel || !m_pTimerLabel )
		{
			Assert(0);
			return;
		}

		m_pTimerLabel->SetText( "" );

		// we need to know how much longer we are going to be in death cam
		// once we know that, we can ask dodgamerules if we are going to make the next
		// wave. If we aren't, gamerules can tell us the new time based on the reserve wave
		float flSpawnEligibleTime;
		
		if ( pPlayer->GetObserverMode() == OBS_MODE_DEATHCAM )
		{
			flSpawnEligibleTime = pPlayer->GetDeathTime() + DEATH_CAM_TIME;
		}
		else
			flSpawnEligibleTime = 0;

		//will never return negative seconds
		int timer = DODGameRules()->GetReinforcementTimerSeconds( pPlayer->GetTeamNumber(), flSpawnEligibleTime );

		if( timer < 0 )
		{
			m_pReinforcementsLabel->SetVisible( false );
		}
		else
		{
			m_pReinforcementsLabel->SetVisible( true );

			char szMins[4], szSecs[4];

			int mins = timer / 60;
			int secs = timer % 60;

			Q_snprintf( szMins, sizeof(szMins), "%d", mins );
			Q_snprintf( szSecs, sizeof(szSecs), "%d", secs );

			wchar_t wMins[4], wSecs[4];
			g_pVGuiLocalize->ConvertANSIToUnicode(szMins, wMins, sizeof(wMins));
			g_pVGuiLocalize->ConvertANSIToUnicode(szSecs, wSecs, sizeof(wSecs));

			wchar_t wLabel[128];

			if ( _cl_classmenuopen.GetBool() )
			{
				mins = 0;
				secs = 0;
			}

			if ( mins == 1 )		//"1 minute"
			{
				g_pVGuiLocalize->ConstructString( wLabel, sizeof( wLabel ), g_pVGuiLocalize->Find("#Dod_Reinforcements_in_min" ), 2, wMins, wSecs );
			}
			else if ( mins > 0 )	//"2 minutes"
			{
				g_pVGuiLocalize->ConstructString( wLabel, sizeof( wLabel ), g_pVGuiLocalize->Find("#Dod_Reinforcements_in_mins" ), 2, wMins, wSecs );
			}
			else if ( secs == 1 )	//"1 second"
			{
				g_pVGuiLocalize->ConstructString( wLabel, sizeof( wLabel ), g_pVGuiLocalize->Find("#Dod_Reinforcements_in_sec" ), 1, wSecs );
			}
			else if ( secs == 0 )	//"Prepare to Respawn"
			{
				g_pVGuiLocalize->ConstructString( wLabel, sizeof( wLabel ), g_pVGuiLocalize->Find("#Dod_Reinforcements_prepare_to_respawn" ), 0 );
			}
			else					//"2 seconds"
			{
                g_pVGuiLocalize->ConstructString( wLabel, sizeof( wLabel ), g_pVGuiLocalize->Find("#Dod_Reinforcements_in_secs" ), 1, wSecs );
			}

			m_pReinforcementsLabel->SetText(wLabel);
		}
	}
}

void CDODSpectatorGUI::UpdateScores( void )
{
	C_DODTeam *pAlliesTeam = static_cast<C_DODTeam *>( GetGlobalTeam(TEAM_ALLIES) );
	if ( pAlliesTeam )
	{
		wchar_t frags[ 10 ];
		_snwprintf( frags, sizeof( frags ), L"%i",  pAlliesTeam->GetRoundsWon() );

		m_pAlliesScoreLabel->SetText( frags );
	}

	C_DODTeam *pAxisTeam = static_cast<C_DODTeam *>( GetGlobalTeam(TEAM_AXIS) );
	if ( pAxisTeam )
	{
		wchar_t frags[ 10 ];
		_snwprintf( frags, sizeof( frags ), L"%i", pAxisTeam->GetRoundsWon() );
		
		m_pAxisScoreLabel->SetText( frags );
	}
}

bool CDODSpectatorGUI::ShouldShowPlayerLabel( int specmode )
{
	return ( (specmode == OBS_MODE_IN_EYE) || 
			 (specmode == OBS_MODE_CHASE) ||
			 (specmode == OBS_MODE_DEATHCAM) );
}