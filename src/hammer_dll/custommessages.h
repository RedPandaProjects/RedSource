//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#ifndef CUSTOMMESSAGES_H
#define CUSTOMMESSAGES_H
#pragma once


enum 
{
	CUSTOMMESSAGESSTART = WM_USER + 1,

	WM_MAPDOC_CHANGED,
	WM_DOCTYPE_CHANGED,
	WM_GAME_CHANGED,

	//
	// Posted by CAngleBox:
	//
	ABN_CHANGED,		// The angle in the angle box has changed.

	//
	// Posted by CTextureWindow:
	//
	TWN_SELCHANGED,		// The texture window selection has changed.
	TWN_LBUTTONDBLCLK,	// The user double clicked in the texture window.
};


#endif // CUSTOMMESSAGES_H
