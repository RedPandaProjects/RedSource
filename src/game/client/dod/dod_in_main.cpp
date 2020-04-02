//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: TF2 specific input handling
//
// $NoKeywords: $
//=============================================================================//
#include "cbase.h"
#include "kbutton.h"
#include "input.h"

//-----------------------------------------------------------------------------
// Purpose: TF Input interface
//-----------------------------------------------------------------------------
class CDODInput : public CInput
{
public:
};

static CDODInput g_Input;

// Expose this interface
IInput *input = ( IInput * )&g_Input;

