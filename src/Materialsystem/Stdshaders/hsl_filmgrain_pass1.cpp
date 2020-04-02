//========= Copyright � 1996-2005, Valve LLC, All rights reserved. ============
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

#include "BaseVSShader.h"
#include "convar.h"
#include "filmgrain_vs20.inc"
#include "hsl_filmgrain_pass1_ps20.inc"
#include "hsl_filmgrain_pass1_ps20b.inc"

//
// First pass converts from RGB to HSL and tweaks with noise similar to After Effects
//

BEGIN_VS_SHADER( hsl_filmgrain_pass1, "Help for Film Grain" )
	BEGIN_SHADER_PARAMS
		// Input textures
		SHADER_PARAM( INPUT, SHADER_PARAM_TYPE_TEXTURE, "", "" )
		SHADER_PARAM( GRAIN, SHADER_PARAM_TYPE_TEXTURE, "", "" )

		// Grain parameters to control positioning and noise params
		SHADER_PARAM( SCALEBIAS, SHADER_PARAM_TYPE_VEC4, "", "Scale and bias for grain placement" )
		SHADER_PARAM( HSLNOISESCALE, SHADER_PARAM_TYPE_VEC4, "", "Strength of film grain" )

	END_SHADER_PARAMS

	SHADER_INIT
	{
		LoadTexture( INPUT );
		LoadTexture( GRAIN );
	}

	SHADER_FALLBACK
	{
		// Requires DX9 + above
		if (!g_pHardwareConfig->SupportsVertexAndPixelShaders())
		{
			Assert( 0 );
			return "Wireframe";
		}
		return 0;
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			pShaderShadow->EnableDepthWrites( false );
			pShaderShadow->EnableDepthTest( false );
			pShaderShadow->EnableAlphaWrites( false );
			pShaderShadow->EnableBlending( false );
			pShaderShadow->EnableCulling( false );
//			pShaderShadow->PolyMode( SHADER_POLYMODEFACE_FRONT_AND_BACK, SHADER_POLYMODE_LINE );

			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
			pShaderShadow->EnableTexture( SHADER_SAMPLER1, true );
			int fmt = VERTEX_POSITION;
			pShaderShadow->VertexShaderVertexFormat( fmt, 1, 0, 0 );

			DECLARE_STATIC_VERTEX_SHADER( filmgrain_vs20 );
			SET_STATIC_VERTEX_SHADER( filmgrain_vs20 );

			if( g_pHardwareConfig->SupportsPixelShaders_2_b() )
			{
				DECLARE_STATIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20b );
				SET_STATIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20b );
			}
			else
			{
				DECLARE_STATIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20 );
				SET_STATIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20 );
			}
		}

		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, INPUT, -1 );
			BindTexture( SHADER_SAMPLER1, GRAIN, -1 );

			SetVertexShaderConstant( VERTEX_SHADER_SHADER_SPECIFIC_CONST_0, SCALEBIAS );

			SetPixelShaderConstant( 0, HSLNOISESCALE );

			DECLARE_DYNAMIC_VERTEX_SHADER( filmgrain_vs20 );
			SET_DYNAMIC_VERTEX_SHADER( filmgrain_vs20 );

			if( g_pHardwareConfig->SupportsPixelShaders_2_b() )
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20b );
				SET_DYNAMIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20b );
			}
			else
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20 );
				SET_DYNAMIC_PIXEL_SHADER( hsl_filmgrain_pass1_ps20 );
			}
		}
		Draw();
	}
END_SHADER
