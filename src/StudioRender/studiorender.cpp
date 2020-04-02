//===== Copyright � 1996-2007, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
//===========================================================================//

#include <stdlib.h>
#include "studiorender.h"
#include "studiorendercontext.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "optimize.h"
#include "mathlib/vmatrix.h"
#include "tier0/vprof.h"
#include "tier1/strtools.h"
#include "tier1/KeyValues.h"
#include "tier0/memalloc.h"
#include "convar.h"
#include "materialsystem/itexture.h"
#include "tier2/tier2.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Singleton instance
//-----------------------------------------------------------------------------
CStudioRender g_StudioRender;
CStudioRender *g_pStudioRenderImp = &g_StudioRender;


//-----------------------------------------------------------------------------
// Activate to get stats
//-----------------------------------------------------------------------------
//#define REPORT_FLEX_STATS 1

#ifdef REPORT_FLEX_STATS
static int s_nModelsDrawn = 0;
static int s_nActiveFlexCount = 0;
static ConVar r_flexstats( "r_flexstats", "0", FCVAR_CHEAT );
#endif


//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CStudioRender::CStudioRender()
{
	m_pRC = NULL;
	m_pBoneToWorld = NULL;
	m_pFlexWeights = NULL;
	m_pFlexDelayedWeights = NULL;
	m_pStudioHdr = NULL;
	m_pStudioMeshes = NULL;
	m_pSubModel = NULL;
	m_pGlintTexture = NULL;
	m_GlintWidth = 0;
	m_GlintHeight = 0;

	// Cache-align our important matrices
	/*g_pMemAlloc->PushAllocDbgInfo( __FILE__, __LINE__ );*/

	m_PoseToWorld = (matrix3x4_t*)malloc( MAXSTUDIOBONES * sizeof(matrix3x4_t) +32);
	m_PoseToDecal = (matrix3x4_t*)malloc( MAXSTUDIOBONES * sizeof(matrix3x4_t)+32 );

//	g_pMemAlloc->PopAllocDbgInfo();
	m_nDecalId = 1;
}

CStudioRender::~CStudioRender()
{
	free(m_PoseToWorld);
	free(m_PoseToDecal);
}

void CStudioRender::InitDebugMaterials( void )
{
#ifdef _WIN32
	m_pMaterialMRMWireframe = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugmrmwireframe", TEXTURE_GROUP_OTHER, true );
	m_pMaterialMRMWireframe->IncrementReferenceCount();

	m_pMaterialMRMWireframeZBuffer = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugmrmwireframezbuffer", TEXTURE_GROUP_OTHER, true );
	m_pMaterialMRMWireframeZBuffer->IncrementReferenceCount();

	m_pMaterialMRMNormals = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugmrmnormals", TEXTURE_GROUP_OTHER, true );
	m_pMaterialMRMNormals->IncrementReferenceCount();

	m_pMaterialTangentFrame = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugvertexcolor", TEXTURE_GROUP_OTHER, true );
	m_pMaterialTangentFrame->IncrementReferenceCount();

	m_pMaterialTranslucentModelHulls = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugtranslucentmodelhulls", TEXTURE_GROUP_OTHER, true );
	m_pMaterialTranslucentModelHulls->IncrementReferenceCount();

	m_pMaterialSolidModelHulls = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugsolidmodelhulls", TEXTURE_GROUP_OTHER, true );
	m_pMaterialSolidModelHulls->IncrementReferenceCount();

	m_pMaterialAdditiveVertexColorVertexAlpha = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/additivevertexcolorvertexalpha", TEXTURE_GROUP_OTHER, true );
	m_pMaterialAdditiveVertexColorVertexAlpha->IncrementReferenceCount();

	m_pMaterialModelBones = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugmodelbones", TEXTURE_GROUP_OTHER, true );
	m_pMaterialModelBones->IncrementReferenceCount();

	m_pMaterialModelEnvCubemap =
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/env_cubemap_model", TEXTURE_GROUP_OTHER, true );
	m_pMaterialModelEnvCubemap->IncrementReferenceCount();
	
	m_pMaterialWorldWireframe = 
		g_pMaterialSystem->FindMaterial( "//platform/materials/debug/debugworldwireframe", TEXTURE_GROUP_OTHER, true );
	m_pMaterialWorldWireframe->IncrementReferenceCount();

	if( g_pMaterialSystemHardwareConfig->GetDXSupportLevel() >= 90 )
	{
		KeyValues *pVMTKeyValues = new KeyValues( "DepthWrite" );
		pVMTKeyValues->SetInt( "$no_fullbright", 1 );
		pVMTKeyValues->SetInt( "$alphatest", 0 );
		pVMTKeyValues->SetInt( "$nocull", 0 );
		m_pDepthWrite[0][0] = g_pMaterialSystem->FindProceduralMaterial( "__DepthWrite00", TEXTURE_GROUP_OTHER, pVMTKeyValues );

		pVMTKeyValues = new KeyValues( "DepthWrite" );
		pVMTKeyValues->SetInt( "$no_fullbright", 1 );
		pVMTKeyValues->SetInt( "$alphatest", 0 );
		pVMTKeyValues->SetInt( "$nocull", 1 );
		m_pDepthWrite[0][1] = g_pMaterialSystem->FindProceduralMaterial( "__DepthWrite01", TEXTURE_GROUP_OTHER, pVMTKeyValues );

		pVMTKeyValues = new KeyValues( "DepthWrite" );
		pVMTKeyValues->SetInt( "$no_fullbright", 1 );
		pVMTKeyValues->SetInt( "$alphatest", 1 );
		pVMTKeyValues->SetInt( "$nocull", 0 );
		m_pDepthWrite[1][0] = g_pMaterialSystem->FindProceduralMaterial( "__DepthWrite10", TEXTURE_GROUP_OTHER, pVMTKeyValues );

		pVMTKeyValues = new KeyValues( "DepthWrite" );
		pVMTKeyValues->SetInt( "$no_fullbright", 1 );
		pVMTKeyValues->SetInt( "$alphatest", 1 );
		pVMTKeyValues->SetInt( "$nocull", 1 );
		m_pDepthWrite[1][1] = g_pMaterialSystem->FindProceduralMaterial( "__DepthWrite11", TEXTURE_GROUP_OTHER, pVMTKeyValues );

		pVMTKeyValues = new KeyValues( "EyeGlint" );
		m_pGlintBuildMaterial = g_pMaterialSystem->CreateMaterial( "___glintbuildmaterial", pVMTKeyValues );
	}

#endif // _WIN32
}

void CStudioRender::ShutdownDebugMaterials( void )
{
#ifdef _WIN32
	if ( m_pMaterialMRMWireframe )
	{
		m_pMaterialMRMWireframe->DecrementReferenceCount();
		m_pMaterialMRMWireframe = NULL;
	}

	if ( m_pMaterialMRMWireframeZBuffer )
	{
		m_pMaterialMRMWireframeZBuffer->DecrementReferenceCount();
		m_pMaterialMRMWireframeZBuffer = NULL;
	}

	if ( m_pMaterialMRMNormals )
	{
		m_pMaterialMRMNormals->DecrementReferenceCount();
		m_pMaterialMRMNormals = NULL;
	}

	if ( m_pMaterialTangentFrame )
	{
		m_pMaterialTangentFrame->DecrementReferenceCount();
		m_pMaterialTangentFrame = NULL;
	}

	if ( m_pMaterialTranslucentModelHulls )
	{
		m_pMaterialTranslucentModelHulls->DecrementReferenceCount();
		m_pMaterialTranslucentModelHulls = NULL;
	}
	
	if ( m_pMaterialSolidModelHulls )
	{
		m_pMaterialSolidModelHulls->DecrementReferenceCount();
		m_pMaterialSolidModelHulls = NULL;
	}
	
	if ( m_pMaterialAdditiveVertexColorVertexAlpha )
	{
		m_pMaterialAdditiveVertexColorVertexAlpha->DecrementReferenceCount();
		m_pMaterialAdditiveVertexColorVertexAlpha = NULL;
	}
	
	if ( m_pMaterialModelBones )
	{
		m_pMaterialModelBones->DecrementReferenceCount();
		m_pMaterialModelBones = NULL;
	}
	
	if ( m_pMaterialModelEnvCubemap )
	{
		m_pMaterialModelEnvCubemap->DecrementReferenceCount();
		m_pMaterialModelEnvCubemap = NULL;
	}

	if ( m_pMaterialWorldWireframe )
	{
		m_pMaterialWorldWireframe->DecrementReferenceCount();
		m_pMaterialWorldWireframe = NULL;
	}

	// DepthWrite materials
	if ( m_pDepthWrite[0][0] )
	{
		m_pDepthWrite[0][0]->DecrementReferenceCount();
	}

	if ( m_pDepthWrite[0][1] )
	{
		m_pDepthWrite[0][1]->DecrementReferenceCount();
	}

	if ( m_pDepthWrite[1][0] )
	{
		m_pDepthWrite[1][0]->DecrementReferenceCount();
	}

	if ( m_pDepthWrite[1][1] )
	{
		m_pDepthWrite[1][1]->DecrementReferenceCount();
	}

	if ( m_pGlintBuildMaterial )
	{
		m_pGlintBuildMaterial->DecrementReferenceCount();
		m_pGlintBuildMaterial = NULL;
	}
#endif
}

static void ReleaseMaterialSystemObjects()
{
//	g_StudioRender.UncacheGlint();
}

static void RestoreMaterialSystemObjects( int nChangeFlags )
{
//	g_StudioRender.PrecacheGlint();
}



//-----------------------------------------------------------------------------
// Init, shutdown
//-----------------------------------------------------------------------------
InitReturnVal_t CStudioRender::Init()
{
	if ( g_pMaterialSystem && g_pMaterialSystemHardwareConfig )
	{
		g_pMaterialSystem->AddReleaseFunc( ReleaseMaterialSystemObjects );
		g_pMaterialSystem->AddRestoreFunc( RestoreMaterialSystemObjects );

		InitDebugMaterials();

		return INIT_OK;
	}

	return INIT_FAILED;
}

void CStudioRender::Shutdown( void )
{
	UncacheGlint();
	ShutdownDebugMaterials();

	if ( g_pMaterialSystem )
	{
		g_pMaterialSystem->RemoveReleaseFunc( ReleaseMaterialSystemObjects );
		g_pMaterialSystem->RemoveRestoreFunc( RestoreMaterialSystemObjects );
	}
}


//-----------------------------------------------------------------------------
// Sets the lighting render state
//-----------------------------------------------------------------------------
void CStudioRender::SetLightingRenderState()
{
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	// FIXME: What happens when we use the fixed function pipeline but vertex shaders 
	// are active? For the time being this only works because everything that does
	// vertex lighting does, in fact, have a vertex shader which is used to render it.
	pRenderContext->SetAmbientLightCube( m_pRC->m_LightBoxColors );

	if ( m_pRC->m_Config.bSoftwareLighting || m_pRC->m_NumLocalLights == 0 )
	{
		pRenderContext->DisableAllLocalLights();
	}
	else
	{
		int nMaxLightCount = g_pMaterialSystemHardwareConfig->MaxNumLights();
		LightDesc_t desc;
		desc.m_Type = MATERIAL_LIGHT_DISABLE;

		int i;
		int nLightCount = min( m_pRC->m_NumLocalLights, nMaxLightCount );
		for( i = 0; i < nLightCount; ++i )
		{
			pRenderContext->SetLight( i, m_pRC->m_LocalLights[i] );
		}
		for( ; i < nMaxLightCount; ++i )
		{
			pRenderContext->SetLight( i, desc );
		}
	}
}


//-----------------------------------------------------------------------------
// Shadow state (affects the models as they are rendered)
//-----------------------------------------------------------------------------
void CStudioRender::AddShadow( IMaterial* pMaterial, void* pProxyData, FlashlightState_t *pFlashlightState, VMatrix *pWorldToTexture, ITexture *pFlashlightDepthTexture )
{
	int i = m_ShadowState.AddToTail();
	ShadowState_t& state = m_ShadowState[i];
	state.m_pMaterial = pMaterial;
	state.m_pProxyData = pProxyData;
	state.m_pFlashlightState = pFlashlightState;
	state.m_pWorldToTexture = pWorldToTexture;
	state.m_pFlashlightDepthTexture = pFlashlightDepthTexture;
}

void CStudioRender::ClearAllShadows()
{
	m_ShadowState.RemoveAll();
}

void CStudioRender::GetFlexStats( )
{
#ifdef REPORT_FLEX_STATS
	static bool s_bLastFlexStats = false;
	bool bDoStats = r_flexstats.GetInt() != 0;
	if ( bDoStats )
	{
		if ( !s_bLastFlexStats )
		{
			s_nModelsDrawn = 0;
			s_nActiveFlexCount = 0;
		}

		// Count number of active weights
		int nActiveFlexCount = 0;
		for ( int i = 0; i < MAXSTUDIOFLEXDESC; ++i )
		{
			if ( fabs( m_FlexWeights[i] ) >= 0.001f || fabs( m_FlexDelayedWeights[i] ) >= 0.001f )
			{
				++nActiveFlexCount;
			}
		}

		++s_nModelsDrawn;
		s_nActiveFlexCount += nActiveFlexCount;
	}
	else
	{
		if ( s_bLastFlexStats )
		{
			if ( s_nModelsDrawn )
			{
				Msg( "Average number of flexes/model: %d\n", s_nActiveFlexCount / s_nModelsDrawn );
			}
			else
			{
				Msg( "No models rendered to take stats of\n" );
			}

			s_nModelsDrawn = 0;
			s_nActiveFlexCount = 0;
		}
	}

	s_bLastFlexStats = bDoStats;
#endif
}


//-----------------------------------------------------------------------------
// Main model rendering entry point
//-----------------------------------------------------------------------------
void CStudioRender::DrawModel( const DrawModelInfo_t& info, const StudioRenderContext_t &rc, 
	matrix3x4_t *pBoneToWorld, const FlexWeights_t &flex, int flags )
{
	VPROF( "CStudioRender::DrawModel");

	m_pRC = const_cast< StudioRenderContext_t* >( &rc );
	m_pFlexWeights = flex.m_pFlexWeights;
	m_pFlexDelayedWeights = flex.m_pFlexDelayedWeights;
	m_pBoneToWorld = pBoneToWorld;

	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	// Disable flex if we're told to...
	bool flexConfig = m_pRC->m_Config.bFlex;
	if (flags & STUDIORENDER_DRAW_NO_FLEXES)
	{
		m_pRC->m_Config.bFlex = false;
	}

	// Enable wireframe if we're told to...
	bool bWireframe = m_pRC->m_Config.bWireframe;
	if ( flags & STUDIORENDER_DRAW_WIREFRAME )
	{
		m_pRC->m_Config.bWireframe = true;
	}

	int boneMask = BONE_USED_BY_VERTEX_AT_LOD( info.m_Lod );

	// Preserve the matrices if we're skinning
	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	m_VertexCache.StartModel();

	m_pStudioHdr = info.m_pStudioHdr;
	m_pStudioMeshes = info.m_pHardwareData->m_pLODs[info.m_Lod].m_pMeshData;

	// Bone to world must be set before calling drawmodel; it uses that here
	ComputePoseToWorld( m_PoseToWorld, m_pStudioHdr, boneMask, m_pRC->m_ViewOrigin, pBoneToWorld );

	R_StudioRenderModel( pRenderContext, info.m_Skin, info.m_Body, info.m_HitboxSet, info.m_pClientEntity,
		info.m_pHardwareData->m_pLODs[info.m_Lod].ppMaterials, 
		info.m_pHardwareData->m_pLODs[info.m_Lod].pMaterialFlags, flags, boneMask, info.m_Lod, info.m_pColorMeshes);

	// Draw all the decals on this model
	// If the model is not in memory, this code may not function correctly
	// This code assumes the model has been rendered!
	// So skip if the model hasn't been rendered
	// Also, skip if we're rendering to the shadow depth map
	if ( ( m_pStudioMeshes != 0 ) && !(flags & STUDIORENDER_SHADOWDEPTHTEXTURE) )
	{
		if ((flags & STUDIORENDER_DRAW_GROUP_MASK) != STUDIORENDER_DRAW_TRANSLUCENT_ONLY)
		{
			DrawDecal( info, info.m_Lod, info.m_Body );
		}

		// Draw shadows
		if ( !( flags & STUDIORENDER_DRAW_NO_SHADOWS ) )
		{
			DrawShadows( info, flags, boneMask );
		}

		if( (flags & STUDIORENDER_DRAW_GROUP_MASK) != STUDIORENDER_DRAW_TRANSLUCENT_ONLY &&
			!( flags & STUDIORENDER_DRAW_NO_SHADOWS ) )
		{
			DrawFlashlightDecals( info, info.m_Lod );
		}
	}

	// Restore the matrices if we're skinning
	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();

	// Restore the configs
	m_pRC->m_Config.bFlex = flexConfig;
	m_pRC->m_Config.bWireframe = bWireframe;

#ifdef REPORT_FLEX_STATS
	GetFlexStats();
#endif

	pRenderContext->SetNumBoneWeights( 0 );
	m_pRC = NULL;
	m_pBoneToWorld = NULL;
	m_pFlexWeights = NULL;
	m_pFlexDelayedWeights = NULL;
}

void CStudioRender::DrawModelStaticProp( const DrawModelInfo_t& info, 
	const StudioRenderContext_t &rc, const matrix3x4_t& rootToWorld, int flags )
{
	VPROF( "CStudioRender::DrawModelStaticProp");

	m_pRC = const_cast<StudioRenderContext_t*>( &rc );

	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	memcpy( &m_StaticPropRootToWorld, &rootToWorld, sizeof(matrix3x4_t) );
	memcpy( &m_PoseToWorld[0], &rootToWorld, sizeof(matrix3x4_t) );
	m_pBoneToWorld = &m_StaticPropRootToWorld;

	bool flexConfig = m_pRC->m_Config.bFlex;
	m_pRC->m_Config.bFlex = false;
	bool bWireframe = m_pRC->m_Config.bWireframe;
	if ( flags & STUDIORENDER_DRAW_WIREFRAME )
	{
		m_pRC->m_Config.bWireframe = true;
	}

	int lod = info.m_Lod;
	m_pStudioHdr = info.m_pStudioHdr;
	m_pStudioMeshes = info.m_pHardwareData->m_pLODs[lod].m_pMeshData;

	R_StudioRenderModel( pRenderContext, info.m_Skin, info.m_Body, info.m_HitboxSet, info.m_pClientEntity,
		info.m_pHardwareData->m_pLODs[lod].ppMaterials, 
		info.m_pHardwareData->m_pLODs[lod].pMaterialFlags, flags, BONE_USED_BY_ANYTHING, lod, info.m_pColorMeshes);

	// If we're not shadow depth mapping
	if ( ( flags & STUDIORENDER_SHADOWDEPTHTEXTURE ) == 0 )
	{
		// FIXME: Should this occur in a separate call?
		// Draw all the decals on this model
		if ((flags & STUDIORENDER_DRAW_GROUP_MASK) != STUDIORENDER_DRAW_TRANSLUCENT_ONLY)
		{
			DrawDecal( info, lod, info.m_Body );
		}

		// Draw shadows
		if ( !( flags & STUDIORENDER_DRAW_NO_SHADOWS ) )
		{
			DrawShadows( info, flags, BONE_USED_BY_ANYTHING );
		}

		if( (flags & STUDIORENDER_DRAW_GROUP_MASK) != STUDIORENDER_DRAW_TRANSLUCENT_ONLY &&
			!( flags & STUDIORENDER_DRAW_NO_SHADOWS ) )
		{
			DrawFlashlightDecals( info, lod );
		}
	}

	// Restore the configs
	m_pRC->m_Config.bFlex = flexConfig;
	m_pRC->m_Config.bWireframe = bWireframe;

	pRenderContext->SetNumBoneWeights( 0 );
	m_pBoneToWorld = NULL;
	m_pRC = NULL;
}
