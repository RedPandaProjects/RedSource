//========= Copyright � 1996-2001, Valve LLC, All rights reserved. ============
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

#include "pch_serverbrowser.h"


CSpectateGames::CSpectateGames( vgui::Panel *parent )
	: CInternetGames( parent, "SpectateGames", eSpectatorServer )
{
}

void CSpectateGames::GetNewServerList()
{
	m_vecServerFilters.AddToTail( MatchMakingKeyValuePair_t( "proxy", "1" ) );
	BaseClass::GetNewServerList();
}

