//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "BaseVSShader.h"
#include "common_hlsl_cpp_consts.h"
#include "convar.h"

#include "downsample_nohdr_ps20.inc"
#include "downsample_nohdr_ps20b.inc"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


static ConVar r_bloomtintr( "r_bloomtintr", "0.3" );
static ConVar r_bloomtintg( "r_bloomtintg", "0.59" );
static ConVar r_bloomtintb( "r_bloomtintb", "0.11" );
static ConVar r_bloomtintexponent( "r_bloomtintexponent", "2.2" );

BEGIN_VS_SHADER_FLAGS( Downsample_nohdr, "Help for Downsample_nohdr", SHADER_NOT_EDITABLE )

	BEGIN_SHADER_PARAMS
	END_SHADER_PARAMS

	SHADER_INIT
{
		LoadTexture( BASETEXTURE );
	}

	SHADER_FALLBACK
	{
		if ( g_pHardwareConfig->GetDXSupportLevel() < 90 )
		{
			return "Downsample_nohdr_DX80";
		}
		return 0;
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			pShaderShadow->EnableDepthWrites( false );
			pShaderShadow->EnableAlphaWrites( true );
			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, false );
			pShaderShadow->EnableSRGBWrite( false );

			pShaderShadow->VertexShaderVertexFormat( VERTEX_POSITION, 1, 0, 0 );

			pShaderShadow->SetVertexShader( "Downsample_vs20", 0 );
			
			if( g_pHardwareConfig->SupportsPixelShaders_2_b() )
			{
				DECLARE_STATIC_PIXEL_SHADER( downsample_nohdr_ps20b );
				SET_STATIC_PIXEL_SHADER( downsample_nohdr_ps20b );
			}
			else
			{
				DECLARE_STATIC_PIXEL_SHADER( downsample_nohdr_ps20 );
				SET_STATIC_PIXEL_SHADER( downsample_nohdr_ps20 );
			}
		}

		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );

			int width, height;
			pShaderAPI->GetBackBufferDimensions( width, height );

			float v[4][4];
			float dX = 1.0f/width;
			float dY = 1.0f/height;

			v[0][0] = .5*dX;
			v[0][1] = .5*dY;
			v[1][0] = 2.5*dX;
			v[1][1] = .5*dY;
			v[2][0] = .5*dX;
			v[2][1] = 2.5*dY;
			v[3][0] = 2.5*dX;
			v[3][1] = 2.5*dY;
			pShaderAPI->SetVertexShaderConstant( VERTEX_SHADER_SHADER_SPECIFIC_CONST_0, &v[0][0], 4 );

			pShaderAPI->SetVertexShaderIndex( 0 );

			float flPixelShaderParams[4] = { r_bloomtintr.GetFloat(),
											 r_bloomtintg.GetFloat(),
											 r_bloomtintb.GetFloat(),
											 r_bloomtintexponent.GetFloat() };
			pShaderAPI->SetPixelShaderConstant( 0, flPixelShaderParams, 1 );

			
			if( g_pHardwareConfig->SupportsPixelShaders_2_b() )
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( downsample_nohdr_ps20b );
				SET_DYNAMIC_PIXEL_SHADER( downsample_nohdr_ps20b );
			}
			else
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( downsample_nohdr_ps20 );
				SET_DYNAMIC_PIXEL_SHADER( downsample_nohdr_ps20 );
			}
		}
		Draw();
	}
END_SHADER
