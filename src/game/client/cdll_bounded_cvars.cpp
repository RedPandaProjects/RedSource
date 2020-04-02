//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
// $NoKeywords: $
//===========================================================================//

#include "cbase.h"
#include "cdll_bounded_cvars.h"
#include "convar_serverbounded.h"


bool g_bForceCLPredictOff = false;

// ------------------------------------------------------------------------------------------ //
// cl_predict.
// ------------------------------------------------------------------------------------------ //

class CBoundedCvar_Predict : public ConVar_ServerBounded
{
public:
	CBoundedCvar_Predict() :
	  ConVar_ServerBounded( "cl_predict", 
		  "1.0", 
#if defined(DOD_DLL) || defined(CSTRIKE_DLL)
		  FCVAR_USERINFO | FCVAR_CHEAT, 
#else
		  FCVAR_USERINFO, 
#endif
		  "Perform client side prediction." )
	  {
	  }

	  virtual float GetFloat() const
	  {
		  // Used temporarily for CS kill cam.
		  if ( g_bForceCLPredictOff )
			  return 0;

		  static const ConVar *pClientPredict = dynamic_cast< const ConVar* >( g_pCVar->FindCommandBase( "sv_client_predict" ) );
		  if ( pClientPredict && pClientPredict->GetInt() != -1 )
		  {
			  // Ok, the server wants to control this value.
			  return pClientPredict->GetFloat();
		  }
		  else
		  {
			  return GetBaseFloatValue();
		  }
	  }
};

static CBoundedCvar_Predict cl_predict_var;
ConVar_ServerBounded *cl_predict = &cl_predict_var;



// ------------------------------------------------------------------------------------------ //
// cl_interp_ratio.
// ------------------------------------------------------------------------------------------ //

class CBoundedCvar_InterpRatio : public ConVar_ServerBounded
{
public:
	CBoundedCvar_InterpRatio() :
	  ConVar_ServerBounded( "cl_interp_ratio", 
		  "2.0", 
		  FCVAR_USERINFO, 
		  "Sets the interpolation amount (final amount is cl_interp_ratio / cl_updaterate)." )
	  {
	  }

	  virtual float GetFloat() const
	  {
		  static const ConVar *pMin = dynamic_cast< const ConVar* >( g_pCVar->FindCommandBase( "sv_client_min_interp_ratio" ) );
		  static const ConVar *pMax = dynamic_cast< const ConVar* >( g_pCVar->FindCommandBase( "sv_client_max_interp_ratio" ) );
		  if ( pMin && pMax && pMin->GetFloat() != -1 )
		  {
			  return clamp( GetBaseFloatValue(), pMin->GetFloat(), pMax->GetFloat() );
		  }
		  else
		  {
			  return GetBaseFloatValue();
		  }
	  }
};

static CBoundedCvar_InterpRatio cl_interp_ratio_var;
ConVar_ServerBounded *cl_interp_ratio = &cl_interp_ratio_var;


// ------------------------------------------------------------------------------------------ //
// cl_interp
// ------------------------------------------------------------------------------------------ //

class CBoundedCvar_Interp : public ConVar_ServerBounded
{
public:
	CBoundedCvar_Interp() :
	  ConVar_ServerBounded( "cl_interp", 
		  "0.1", 
		  FCVAR_USERINFO, 
		  "Sets the interpolation amount (bounded on low side by server interp ratio settings).", true, 0.0f, true, 0.5f )
	  {
	  }

	  virtual float GetFloat() const
	  {
		  static const ConVar_ServerBounded *pUpdateRate = dynamic_cast< const ConVar_ServerBounded* >( g_pCVar->FindCommandBase( "cl_updaterate" ) );
		  static const ConVar *pMin = dynamic_cast< const ConVar* >( g_pCVar->FindCommandBase( "sv_client_min_interp_ratio" ) );
		  if ( pUpdateRate && pMin && pMin->GetFloat() != -1 )
		  {
			  return max( GetBaseFloatValue(), pMin->GetFloat() / pUpdateRate->GetFloat() );
		  }
		  else
		  {
			  return GetBaseFloatValue();
		  }
	  }
};

static CBoundedCvar_Interp cl_interp_var;
ConVar_ServerBounded *cl_interp = &cl_interp_var;

float GetClientInterpAmount()
{
	static const ConVar_ServerBounded *pUpdateRate = dynamic_cast< const ConVar_ServerBounded* >( g_pCVar->FindCommandBase( "cl_updaterate" ) );
	if ( pUpdateRate )
	{
		// #define FIXME_INTERP_RATIO
		return max( cl_interp->GetFloat(), cl_interp_ratio->GetFloat() / pUpdateRate->GetFloat() );
	}
	else
	{
		AssertMsgOnce( false, "GetInterpolationAmount: can't get cl_updaterate cvar." );
		return 0.1;
	}
}

