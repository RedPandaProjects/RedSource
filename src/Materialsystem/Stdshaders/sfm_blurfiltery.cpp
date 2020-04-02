//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
// $NoKeywords: $
//===========================================================================//

#include "BaseVSShader.h"
#include "blurfilter_vs20.inc"
#include "blurfilter_ps20.inc"
#include "blurfilter_ps20b.inc"


BEGIN_VS_SHADER_FLAGS( sfm_blurfiltery_shader, "Help for BlurFilterY", SHADER_NOT_EDITABLE )
	BEGIN_SHADER_PARAMS
	END_SHADER_PARAMS

	SHADER_INIT
	{
		if( params[BASETEXTURE]->IsDefined() )
		{
			LoadTexture( BASETEXTURE );
		}
	}
	
	SHADER_FALLBACK
	{
		// Requires DX9 + above
		if ( g_pHardwareConfig->GetDXSupportLevel() < 90 )
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
			pShaderShadow->EnableAlphaWrites( true );

			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
			int fmt = VERTEX_POSITION;
			pShaderShadow->VertexShaderVertexFormat( fmt, 1, 0, 0 );

			// Pre-cache shaders
			blurfilter_vs20_Static_Index vshIndex;
			pShaderShadow->SetVertexShader( "BlurFilter_vs20", vshIndex.GetIndex() );

			if( g_pHardwareConfig->SupportsPixelShaders_2_b() )
			{
				DECLARE_STATIC_PIXEL_SHADER( blurfilter_ps20b );
				SET_STATIC_PIXEL_SHADER( blurfilter_ps20b );
			}
			else
			{
				DECLARE_STATIC_PIXEL_SHADER( blurfilter_ps20 );
				SET_STATIC_PIXEL_SHADER( blurfilter_ps20 );
			}

			if ( IS_FLAG_SET( MATERIAL_VAR_ADDITIVE ) )
				EnableAlphaBlending( SHADER_BLEND_ONE, SHADER_BLEND_ONE );
		}

		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );

			ITexture *src_texture=params[BASETEXTURE]->GetTextureValue();
			int height  = src_texture->GetActualHeight();

			float v[4];

			// The temp buffer is 1/4 back buffer size
			float dY = 1.0f / height;

			// Tap offsets
			v[0] = 0.0f;
			v[1] = 1.3366f * dY;
			v[2] = 0;
			v[3] = 0;
			pShaderAPI->SetVertexShaderConstant( VERTEX_SHADER_SHADER_SPECIFIC_CONST_0, v, 1 );
			v[0] = 0.0f;
			v[1] = 3.4295f * dY;
			pShaderAPI->SetVertexShaderConstant( VERTEX_SHADER_SHADER_SPECIFIC_CONST_1, v, 1 );
			v[0] = 0.0f;
			v[1] = 5.4264f * dY;
			pShaderAPI->SetVertexShaderConstant( VERTEX_SHADER_SHADER_SPECIFIC_CONST_2, v, 1 );

			v[0] = 0.0f;
			v[1] = 7.4359f * dY;
			pShaderAPI->SetPixelShaderConstant( 0, v, 1 );
			v[0] = 0.0f;
			v[1] = 9.4436f * dY;
			pShaderAPI->SetPixelShaderConstant( 1, v, 1 );
			v[0] = 0.0f;
			v[1] = 11.4401f * dY;
			pShaderAPI->SetPixelShaderConstant( 2, v, 1 );

			v[0] = v[1] = v[2] = v [3]=1.0;
			pShaderAPI->SetPixelShaderConstant( 3, v, 1 );

			pShaderAPI->SetVertexShaderIndex( 0 );
			
			if( g_pHardwareConfig->SupportsPixelShaders_2_b() )
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( blurfilter_ps20b );
				SET_DYNAMIC_PIXEL_SHADER( blurfilter_ps20b );
			}
			else
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( blurfilter_ps20 );
				SET_DYNAMIC_PIXEL_SHADER( blurfilter_ps20 );
			}
		}
		Draw();
	}
END_SHADER
