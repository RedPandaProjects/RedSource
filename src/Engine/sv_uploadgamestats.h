//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#ifndef SV_UPLOADGAMESTATS_H
#define SV_UPLOADGAMESTATS_H
#ifdef _WIN32
#pragma once
#endif

bool UploadGameStats( char const *cserIP, unsigned int buildnumber, char const *exe, char const *gamedir, char const *mapname,
	unsigned int blobversion, unsigned int blobsize, const void *pvBlobData )

#endif // SV_UPLOADGAMESTATS_H
