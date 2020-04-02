//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
//===========================================================================//

#include "render_pch.h"
#include "r_decal.h"
#include "client.h"
#include <materialsystem/imaterialsystemhardwareconfig.h>
#include "decal.h"
#include "tier0/vprof.h"
#include "materialsystem/materialsystem_config.h"
#include "icliententity.h"
#include "icliententitylist.h"
#include "tier2/tier2.h"
#include "tier1/callqueue.h"
#include "tier1/memstack.h"
#include "mempool.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define DECAL_CACHEENTRY		256		// MUST BE POWER OF 2 or code below needs to change!
#define DECAL_DISTANCE			4

// Empirically determined constants for minimizing overalpping decals
#define MAX_OVERLAP_DECALS		4
#define DECAL_OVERLAP_DIST		8

// This structure contains the information used to create new decals
struct decalinfo_t
{
	Vector		m_Position;			// world coordinates of the decal center
	Vector		m_SAxis;			// the s axis for the decal in world coordinates
	model_t*	m_pModel;			// the model the decal is going to be applied in
	worldbrushdata_t *m_pBrush;		// The shared brush data for this model
	IMaterial*	m_pMaterial;		// The decal material
	float		m_Size;				// Size of the decal (in world coords)
	int			m_Flags;
	int			m_Entity;			// Entity the decal is applied to.
	float		m_scale;
	int			m_decalWidth;
	int			m_decalHeight;
	color32		m_Color;
	Vector		m_Basis[3];
	void		*m_pUserData;
	CUtlVector<SurfaceHandle_t>	m_aApplySurfs;
};

typedef struct
{
	short decalIndex;
	unsigned short frameIndex;
	CDecalVert	decalVert[4];
} decalcache_t;

// UNDONE: Compress this???  256K here?
CClassMemoryPool<decal_t> g_DecalAllocator( 128 ); // 128 decals per block.
static int				g_nDynamicDecals = 0;
static int				g_nStaticDecals = 0;
static int				g_iLastReplacedDynamic = -1;

CUtlVector<decal_t*>		s_aDecalPool;

decalcache_t	gDecalCache[DECAL_CACHEENTRY];
decalcache_t	g_TempCache;

static decal_t	*s_pDecalDestroyList = NULL;

#define MAX_DECALS_DX9		2048
#define MAX_DECALS_DX8		1536
#define MAX_DECALS_DX7		1024
int	g_nMaxDecals = 0;

//
// ConVars that control distance-based decal scaling
//
ConVar r_dscale_nearscale( "r_dscale_nearscale", "1", FCVAR_CHEAT );
ConVar r_dscale_neardist( "r_dscale_neardist", "100", FCVAR_CHEAT );
ConVar r_dscale_farscale( "r_dscale_farscale", "4", FCVAR_CHEAT );
ConVar r_dscale_fardist( "r_dscale_fardist", "2000", FCVAR_CHEAT );
ConVar r_dscale_basefov( "r_dscale_basefov", "90", FCVAR_CHEAT );

ConVar r_decal_cullsize( "r_decal_cullsize", "5", 0, "Decals under this size in pixels are culled" );
ConVar r_spray_lifetime( "r_spray_lifetime", "2", 0, "Number of rounds player sprays are visible" );
ConVar r_queued_decals( "r_queued_decals", "0", 0, "Offloads a bit of decal rendering setup work to the material system queue when enabled." );


// This makes sure all the decals got freed before the engine is shutdown.
class CDecalChecker
{
public:
	~CDecalChecker()
	{
		Assert( g_nDynamicDecals == 0 );
	}
} g_DecalChecker;

// used for decal LOD
VMatrix g_BrushToWorldMatrix;

CUtlVector<SurfaceHandle_t>	s_DecalSurfaces[ MAX_MAT_SORT_GROUPS + 1 ];
static ConVar r_drawdecals( "r_drawdecals", "1", FCVAR_CHEAT, "Render decals." );
static ConVar r_drawbatchdecals( "r_drawbatchdecals", "1", 0, "Render decals batched." );

static void R_DecalCreate( decalinfo_t* pDecalInfo, SurfaceHandle_t surfID, float x, float y, bool bForceForDisplacement );
void R_DecalShoot( int textureIndex, int entity, const model_t *model, const Vector &position, const float *saxis, int flags, const color32 &rgbaColor );
static bool R_DecalUnProject( decal_t *pdecal, decallist_t *entry);
void R_DecalSortInit( void );

static void r_printdecalinfo_f()
{
	int nPermanent = 0;
	int nDynamic = 0;

	for ( int i=0; i < g_nMaxDecals; i++ )
	{
		if ( s_aDecalPool[i] )
		{
			if ( s_aDecalPool[i]->flags & FDECAL_PERMANENT )
				++nPermanent;
			else
				++nDynamic;
		}
	}

	Assert( nDynamic == g_nDynamicDecals );
	Msg( "%d decals: %d permanent, %d dynamic\nmp_decals: %d\n", nPermanent+nDynamic, nPermanent, nDynamic, mp_decals.GetInt() );
}

static ConCommand r_printdecalinfo( "r_printdecalinfo", r_printdecalinfo_f );


//-----------------------------------------------------------------------------
// Computes the offset for a decal polygon
//-----------------------------------------------------------------------------
float ComputeDecalLightmapOffset( SurfaceHandle_t surfID )
{
	float flOffset;
	if ( MSurf_Flags( surfID ) & SURFDRAW_BUMPLIGHT )
	{
		int nWidth, nHeight;
		materials->GetLightmapPageSize( 
			SortInfoToLightmapPage( MSurf_MaterialSortID( surfID ) ), &nWidth, &nHeight );
 		int nXExtent = ( MSurf_LightmapExtents( surfID )[0] ) + 1;

		flOffset = ( nWidth != 0 ) ? (float)nXExtent / (float)nWidth : 0.0f;
	}
	else
	{
		flOffset = 0.0f;
	}
	return flOffset;
}

static VertexFormat_t GetUncompressedFormat( const IMaterial * pMaterial )
{
	// FIXME: IMaterial::GetVertexFormat() should do this stripping (add a separate 'SupportsCompression' accessor)
	return ( pMaterial->GetVertexFormat() & ~VERTEX_FORMAT_COMPRESSED );
}

//-----------------------------------------------------------------------------
// Draws a decal polygon
//-----------------------------------------------------------------------------
void Shader_DecalDrawPoly( CDecalVert *v, IMaterial *pMaterial, SurfaceHandle_t surfID, int vertCount, decal_t *pdecal )
{
#ifndef SWDS
	int vertexFormat = 0;
	CMatRenderContextPtr pRenderContext( materials );

#ifdef USE_CONVARS
	if( ShouldDrawInWireFrameMode() )
	{
		pRenderContext->Bind( g_materialDecalWireframe );
	}
	else
#endif
	{
		Assert( MSurf_MaterialSortID( surfID ) >= 0 && 
			    MSurf_MaterialSortID( surfID )  < g_WorldStaticMeshes.Count() );
		pRenderContext->BindLightmapPage( materialSortInfoArray[MSurf_MaterialSortID( surfID )].lightmapPageID );
		pRenderContext->Bind( pMaterial, pdecal->userdata );
		vertexFormat = GetUncompressedFormat( pMaterial );
	}

	IMesh *pMesh = pRenderContext->GetDynamicMesh( );
	CMeshBuilder meshBuilder;
	meshBuilder.Begin( pMesh, MATERIAL_POLYGON, vertCount );

	byte color[4] = {pdecal->color.r,pdecal->color.g,pdecal->color.b,pdecal->color.a};

	// Deal with fading out... (should this be done in the shader?)
	// Note that we do it with per-vertex color even though the translucency
	// is constant so as to not change any rendering state (like the constant
	// alpha value)
	if (pdecal->flags & FDECAL_DYNAMIC)
	{
		float fadeval;

		// Negative fadeDuration value means to fade in
		if (pdecal->fadeDuration < 0)
		{
			fadeval = - (cl.GetTime() - pdecal->fadeStartTime) / pdecal->fadeDuration;
		}
		else
		{
			fadeval = 1.0 - (cl.GetTime() - pdecal->fadeStartTime) / pdecal->fadeDuration;
		}

		fadeval = clamp( fadeval, 0.0f, 1.0f );
		color[3] = (byte) (255 * fadeval);
	}


	Vector normal(0,0,1), tangentS(1,0,0), tangentT(0,1,0);

	if ( vertexFormat & (VERTEX_NORMAL|VERTEX_TANGENT_SPACE) )
	{
		normal = MSurf_Plane( surfID ).normal;
		if ( vertexFormat & VERTEX_TANGENT_SPACE )
		{
			Vector tVect;
			bool negate = TangentSpaceSurfaceSetup( surfID, tVect );
			TangentSpaceComputeBasis( tangentS, tangentT, normal, tVect, negate );
		}
	}

	float flOffset = ComputeDecalLightmapOffset( surfID );
	for( int i = 0; i < vertCount; i++, v++ )
	{
		meshBuilder.Position3f( VectorExpand( v->m_vPos ) );
		if ( vertexFormat & VERTEX_NORMAL )
		{
			meshBuilder.Normal3fv( normal.Base() );
		}
		meshBuilder.Color4ubv( color );

		// Check to see if we are in a material page.
		if ( pMaterial->InMaterialPage() )
		{
			float offset[2], scale[2];
			pMaterial->GetMaterialOffset( offset );
			pMaterial->GetMaterialScale( scale );
			meshBuilder.TexCoordSubRect2f( 0, v->m_ctCoords.x, v->m_ctCoords.y, offset[0], offset[1], scale[0], scale[1] );
		}
		else
		{
			meshBuilder.TexCoord2f( 0, Vector2DExpand( v->m_ctCoords ) );
		}

		meshBuilder.TexCoord2f( 1, Vector2DExpand( v->m_cLMCoords ) );
		meshBuilder.TexCoord1f( 2, flOffset );
		if ( vertexFormat & VERTEX_TANGENT_SPACE )
		{
			meshBuilder.TangentS3fv( tangentS.Base() ); 
			meshBuilder.TangentT3fv( tangentT.Base() ); 
		}
		meshBuilder.AdvanceVertex();
	}

	meshBuilder.End();
	pMesh->Draw();
#endif
}

//-----------------------------------------------------------------------------
// Gets the decal material and radius based on the decal index
//-----------------------------------------------------------------------------

void R_DecalGetMaterialAndSize( int decalIndex, IMaterial*& pDecalMaterial, float& w, float& h )
{
	pDecalMaterial = Draw_DecalMaterial( decalIndex );
	if (!pDecalMaterial)
		return;

	float scale = 1.0f;

	// Compute scale of surface
	// FIXME: cache this?
	bool found;
	IMaterialVar* pDecalScaleVar = pDecalMaterial->FindVar( "$decalScale", &found, false );
	if( found )
	{
		scale = pDecalScaleVar->GetFloatValue();
	}

	// compute the decal dimensions in world space
	w = pDecalMaterial->GetMappingWidth() * scale;
	h = pDecalMaterial->GetMappingHeight() * scale;
}

#ifndef SWDS



static inline decal_t *MSurf_DecalPointer( SurfaceHandle_t surfID )
{
	WorldDecalHandle_t handle = MSurf_Decals(surfID );
	if ( handle == WORLD_DECAL_HANDLE_INVALID )
		return NULL;

	return s_aDecalPool[handle];
}

static WorldDecalHandle_t DecalToHandle( decal_t *pDecal )
{
	if ( !pDecal )
		return WORLD_DECAL_HANDLE_INVALID;

	int decalIndex = pDecal->m_iDecalPool;
	Assert( decalIndex >= 0 && decalIndex < g_nMaxDecals );
	return static_cast<WorldDecalHandle_t> (decalIndex);
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the max decal count given the dx level.
//-----------------------------------------------------------------------------
void InitMaxDecals( void )
{
	g_nMaxDecals = MAX_DECALS_DX7;
	if ( g_pMaterialSystemHardwareConfig )
	{
		if ( g_pMaterialSystemHardwareConfig->GetDXSupportLevel() >= 90 )
		{
			g_nMaxDecals = MAX_DECALS_DX9;
		}
		else if ( g_pMaterialSystemHardwareConfig->GetDXSupportLevel() >= 80 )
		{
			g_nMaxDecals = MAX_DECALS_DX8;
		}
	}
}

// Init the decal pool
void R_DecalInit( void )
{
	InitMaxDecals();

	Assert( g_DecalAllocator.Count() == 0 );
	g_nDynamicDecals = 0;
	g_nStaticDecals = 0;
	g_iLastReplacedDynamic = -1;

	s_aDecalPool.Purge();
	s_aDecalPool.SetSize( g_nMaxDecals );

	int i;

	// Traverse all surfaces of map and throw away current decals
	//
	// sort the surfaces into the sort arrays
	if ( host_state.worldbrush )
	{
		for( i = 0; i < host_state.worldbrush->numsurfaces; i++ )
		{
			SurfaceHandle_t surfID = SurfaceHandleFromIndex(i);
			MSurf_Decals( surfID ) = WORLD_DECAL_HANDLE_INVALID;
		}
	}

	for( int iDecal = 0; iDecal < g_nMaxDecals; ++iDecal )
	{
		s_aDecalPool[iDecal] = NULL;
	}

	for ( i = 0; i < DECAL_CACHEENTRY; i++ )
		gDecalCache[i].decalIndex = -1;

	R_DecalSortInit();
}

void R_DecalTerm( worldbrushdata_t *pBrushData, bool term_permanent_decals )
{
	if( !pBrushData )
		return;

	for( int i = 0; i < pBrushData->numsurfaces; i++ )
	{
		decal_t *pNext;
		SurfaceHandle_t surfID = SurfaceHandleFromIndex( i, pBrushData );
		for( decal_t *pDecal=MSurf_DecalPointer( surfID ); pDecal; pDecal=pNext )
		{
			pNext = pDecal->pnext;
			if ( term_permanent_decals 
				|| (!(pDecal->flags & FDECAL_PERMANENT)
				     && !(pDecal->flags & FDECAL_PLAYERSPRAY)) )
			{
				R_DecalUnlink( pDecal, pBrushData );
			}
			else if( pDecal->flags & FDECAL_PLAYERSPRAY )
			{
				// time out player spray after some number of rounds
				pDecal->fadeStartTime += 1.0f;
				if( pDecal->fadeStartTime >= r_spray_lifetime.GetFloat() )
				{
					R_DecalUnlink( pDecal, pBrushData );
				}
			}
		}

		if ( term_permanent_decals )
		{
			Assert( MSurf_DecalPointer( surfID ) == NULL );
		}
	}
}

void R_DecalTermAll()
{
	for ( int i = 0; i<s_aDecalPool.Count(); i++ )
	{
		R_DecalUnlink( s_aDecalPool[i], host_state.worldbrush );
	}
}


static int R_DecalIndex( decal_t *pdecal )
{
	return pdecal->m_iDecalPool;
}


static int R_DecalCacheIndex( int index )
{
	return index & (DECAL_CACHEENTRY-1);
}


static decalcache_t *R_DecalCacheSlot( int decalIndex )
{
	int				cacheIndex;

	cacheIndex = R_DecalCacheIndex( decalIndex );	// Find the cache slot

	return gDecalCache + cacheIndex;
}


// Release the cache entry for this decal
static void R_DecalCacheClear( decal_t *pdecal )
{
	int				index;
	decalcache_t	*pCache;

	index = R_DecalIndex( pdecal );
	pCache = R_DecalCacheSlot( index );		// Find the cache slot

	if ( pCache->decalIndex == index )		// If this is the decal that's cached here, clear it.
		pCache->decalIndex = -1;
}


void R_DecalFlushDestroyList( void )
{
	decal_t *pDecal = s_pDecalDestroyList;
	while ( pDecal )
	{
		decal_t *pNext = pDecal->pDestroyList;
		R_DecalUnlink( pDecal, host_state.worldbrush );
		pDecal = pNext;
	}
	s_pDecalDestroyList = NULL;
}

static void R_DecalAddToDestroyList( decal_t *pDecal )
{
	if ( !pDecal->pDestroyList )
	{
		pDecal->pDestroyList = s_pDecalDestroyList;
		s_pDecalDestroyList = pDecal;
	}
}

// Unlink pdecal from any surface it's attached to
void R_DecalUnlink( decal_t *pdecal, worldbrushdata_t *pData )
{
	if ( !pdecal )
		return;

	decal_t *tmp;

	R_DecalCacheClear( pdecal );
	if ( IS_SURF_VALID( pdecal->surfID ) )
	{
		if ( MSurf_DecalPointer( pdecal->surfID ) == pdecal )
		{
			MSurf_Decals( pdecal->surfID ) = DecalToHandle( pdecal->pnext );
		}
		else 
		{
			tmp = MSurf_DecalPointer( pdecal->surfID );
			if ( !tmp )
				Sys_Error("Bad decal list");
			while ( tmp->pnext ) 
			{
				if ( tmp->pnext == pdecal ) 
				{
					tmp->pnext = pdecal->pnext;
					break;
				}
				tmp = tmp->pnext;
			}
		}
		
		// Tell the displacement surface.
		if( SurfaceHasDispInfo( pdecal->surfID ) )
		{
			IDispInfo * pDispInfo = MSurf_DispInfo( pdecal->surfID, pData );
			
			if ( pDispInfo )
				pDispInfo->NotifyRemoveDecal( pdecal->m_DispDecal );
		}
	}

	pdecal->surfID = SURFACE_HANDLE_INVALID;

	if ( !(pdecal->flags & FDECAL_PERMANENT) )
	{
		--g_nDynamicDecals;
		Assert( g_nDynamicDecals >= 0 );
	}
	else
	{
		--g_nStaticDecals;
		Assert( g_nStaticDecals >= 0 );
	}
	
	// Free the decal.
	Assert( s_aDecalPool[pdecal->m_iDecalPool] == pdecal );
	s_aDecalPool[pdecal->m_iDecalPool] = NULL;
	g_DecalAllocator.Free( pdecal );
}


int R_FindFreeDecalSlot()
{
	for ( int i=0; i < g_nMaxDecals; i++ )
	{
		if ( !s_aDecalPool[i] )
			return i;
	}
	return -1;
}

// Uncomment this to spew decals if we run out of space!!!
// #define SPEW_DECALS 
#if defined( SPEW_DECALS )
void SpewDecals()
{
	static bool spewdecals = true;

	if ( spewdecals )
	{
		sspewdecals = false;

		int i = 0;
		for ( i = 0 ; i  < g_nMaxDecals; ++i )
		{
			decal_t *decal = s_aDecalPool[ i ];
			Assert( decal );
			if ( decal )
			{
				bool permanent = ( decal->flags & FDECAL_PERMANENT ) ? true : false;
				Msg( "%i == %s on %i perm %i at %.2f %.2f %.2f on surf %i (%.2f %.2f %2.f)\n", 
					i, 
					decal->material->GetName(), 
					(int)decal->entityIndex, 
					permanent ? 1 : 0,
					decal->position.x, decal->position.y, decal->position.z,
					(int)decal->surfID,
					decal->dx,
					decal->dy,
					decal->scale );
			}
		}
	}
}

#endif

int R_FindDynamicDecalSlot( int iStartAt )
{
	if ( (iStartAt >= g_nMaxDecals) || (iStartAt < 0) )
	{
		iStartAt = 0;
	}

	int i = iStartAt;

	do
	{
		// don't deallocate player sprays or permanent decals
		if ( s_aDecalPool[i] && 
			!(s_aDecalPool[i]->flags & FDECAL_PERMANENT) &&
			!(s_aDecalPool[i]->flags & FDECAL_PLAYERSPRAY) )
			return i;
		
		++i;

		if ( i >= g_nMaxDecals )
			i = 0;
	}
	while ( i != iStartAt );

	DevMsg("R_FindDynamicDecalSlot: no slot available.\n");

#if defined( SPEW_DECALS )
	SpewDecals();
#endif

	return -1;
}	

// Just reuse next decal in list
// A decal that spans multiple surfaces will use multiple decal_t pool entries, as each surface needs
// it's own.
static decal_t *R_DecalAlloc( int flags )
{
	static bool bWarningOnce = false;
	bool bPermanent = (flags & FDECAL_PERMANENT) != 0;

	int dynamicDecalLimit = min( r_decals.GetInt(), g_nMaxDecals );

	// Now find a slot. Unless it's dynamic and we're at the limit of dynamic decals,
	// we can look for a free slot.
	int iSlot = -1;
	if ( bPermanent || (g_nDynamicDecals < dynamicDecalLimit) )
	{
		iSlot = R_FindFreeDecalSlot();
	}

	if ( iSlot == -1 )
	{
		iSlot = R_FindDynamicDecalSlot( g_iLastReplacedDynamic+1 );
		if ( iSlot == -1 )
		{
			if ( !bWarningOnce )
			{
				// Can't find a free slot. Just kill the first one.
				DevWarning( 1, "Exceeded MAX_DECALS (%d).\n", g_nMaxDecals );
				bWarningOnce = true;
			}
			iSlot = 0;
		}

		R_DecalUnlink( s_aDecalPool[iSlot], host_state.worldbrush );
		g_iLastReplacedDynamic = iSlot;
	}
	
	// Setup the new decal.
	decal_t *pDecal = g_DecalAllocator.Alloc();
	s_aDecalPool[iSlot] = pDecal;
	pDecal->pDestroyList = NULL;
	pDecal->m_iDecalPool = iSlot;
	pDecal->surfID = SURFACE_HANDLE_INVALID;

	if ( !bPermanent )
	{
		++g_nDynamicDecals;
	}
	else
	{
		++g_nStaticDecals;
	}
		
	return pDecal;
}

// The world coordinate system is right handed with Z up.
// 
//      ^ Z
//      |
//      |   
//      | 
//X<----|
//       \
//		  \
//         \ Y

void R_DecalSurface( SurfaceHandle_t surfID, decalinfo_t *decalinfo, bool bForceForDisplacement )
{
	// Get the texture associated with this surface
	mtexinfo_t* tex = MSurf_TexInfo( surfID );

	Vector4D &textureU = tex->textureVecsTexelsPerWorldUnits[0];
	Vector4D &textureV = tex->textureVecsTexelsPerWorldUnits[1];

	// project decal center into the texture space of the surface
	float s = DotProduct( decalinfo->m_Position, textureU.AsVector3D() ) + 
		textureU.w - MSurf_TextureMins( surfID )[0];
	float t = DotProduct( decalinfo->m_Position, textureV.AsVector3D() ) + 
		textureV.w - MSurf_TextureMins( surfID )[1];


	// Determine the decal basis (measured in world space)
	// Note that the decal basis vectors 0 and 1 will always lie in the same
	// plane as the texture space basis vectors	textureVecsTexelsPerWorldUnits.

	R_DecalComputeBasis( MSurf_Plane( surfID ).normal,
		(decalinfo->m_Flags & FDECAL_USESAXIS) ? &decalinfo->m_SAxis : 0,
		decalinfo->m_Basis );

	// Compute an effective width and height (axis aligned)	in the parent texture space
	// How does this work? decalBasis[0] represents the u-direction (width)
	// of the decal measured in world space, decalBasis[1] represents the 
	// v-direction (height) measured in world space.
	// textureVecsTexelsPerWorldUnits[0] represents the u direction of 
	// the surface's texture space measured in world space (with the appropriate
	// scale factor folded in), and textureVecsTexelsPerWorldUnits[1]
	// represents the texture space v direction. We want to find the dimensions (w,h)
	// of a square measured in texture space, axis aligned to that coordinate system.
	// All we need to do is to find the components of the decal edge vectors
	// (decalWidth * decalBasis[0], decalHeight * decalBasis[1])
	// in texture coordinates:

	float w = fabs( decalinfo->m_decalWidth  * DotProduct( textureU.AsVector3D(), decalinfo->m_Basis[0] ) ) +
		fabs( decalinfo->m_decalHeight * DotProduct( textureU.AsVector3D(), decalinfo->m_Basis[1] ) );
	
	float h = fabs( decalinfo->m_decalWidth  * DotProduct( textureV.AsVector3D(), decalinfo->m_Basis[0] ) ) +
		fabs( decalinfo->m_decalHeight * DotProduct( textureV.AsVector3D(), decalinfo->m_Basis[1] ) );

	// move s,t to upper left corner
	s -= ( w * 0.5 );
	t -= ( h * 0.5 );

	// Is this rect within the surface? -- tex width & height are unsigned
	if( !bForceForDisplacement )
	{
		if ( s <= -w || t <= -h || 
			 s > (MSurf_TextureExtents( surfID )[0]+w) || t > (MSurf_TextureExtents( surfID )[1]+h) )
		{
			return; // nope
		}
	}

	// stamp it
	R_DecalCreate( decalinfo, surfID, s, t, bForceForDisplacement );
}

//-----------------------------------------------------------------------------
// iterate over all surfaces on a node, looking for surfaces to decal
//-----------------------------------------------------------------------------

static void R_DecalNodeSurfaces( mnode_t* node, decalinfo_t *decalinfo )
{
	// iterate over all surfaces in the node
	SurfaceHandle_t surfID = SurfaceHandleFromIndex( node->firstsurface );
	for ( int i=0; i<node->numsurfaces ; ++i, ++surfID) 
	{
		if ( MSurf_Flags( surfID ) & SURFDRAW_NODECALS )
			continue;

		// Displacement surfaces get decals in R_DecalLeaf.
        if ( SurfaceHasDispInfo( surfID ) )
            continue;

		R_DecalSurface( surfID, decalinfo, false );
	}
}						 


void R_DecalLeaf( mleaf_t *pLeaf, decalinfo_t *decalinfo )
{
	SurfaceHandle_t *pHandle = &host_state.worldbrush->marksurfaces[pLeaf->firstmarksurface];
	for ( int i = 0; i < pLeaf->nummarksurfaces; i++ )
	{
		SurfaceHandle_t surfID = pHandle[i];
		
		// only process leaf surfaces
		if ( MSurf_Flags( surfID ) & (SURFDRAW_NODE|SURFDRAW_NODECALS) )
			continue;

		if ( decalinfo->m_aApplySurfs.Find( surfID ) != -1 )
			continue;

		Assert( !MSurf_DispInfo( surfID ) );

		float dist = fabs( DotProduct(decalinfo->m_Position, MSurf_Plane( surfID ).normal) - MSurf_Plane( surfID ).dist);
		if ( dist < DECAL_DISTANCE )
		{
			R_DecalSurface( surfID, decalinfo, false );
		}
	}

	// Add the decal to each displacement in the leaf it touches.
	for ( int i = 0; i < pLeaf->dispCount; i++ )
	{
		IDispInfo *pDispInfo = MLeaf_Disaplcement( pLeaf, i );

		SurfaceHandle_t surfID = pDispInfo->GetParent();

		if ( MSurf_Flags( surfID ) & SURFDRAW_NODECALS )
			continue;

		// Make sure the decal hasn't already been added to it.
		if( pDispInfo->GetTag() )
			continue;

		pDispInfo->SetTag();

		// Trivial bbox reject.
		Vector bbMin, bbMax;
		pDispInfo->GetBoundingBox( bbMin, bbMax );
		if( decalinfo->m_Position.x - decalinfo->m_Size < bbMax.x && decalinfo->m_Position.x + decalinfo->m_Size > bbMin.x && 
			decalinfo->m_Position.y - decalinfo->m_Size < bbMax.y && decalinfo->m_Position.y + decalinfo->m_Size > bbMin.y && 
			decalinfo->m_Position.z - decalinfo->m_Size < bbMax.z && decalinfo->m_Position.z + decalinfo->m_Size > bbMin.z )
		{
			R_DecalSurface( pDispInfo->GetParent(), decalinfo, true );
		}
	}
}

//-----------------------------------------------------------------------------
// Recursive routine to find surface to apply a decal to.  World coordinates of 
// the decal are passed in r_recalpos like the rest of the engine.  This should 
// be called through R_DecalShoot()
//-----------------------------------------------------------------------------

static void R_DecalNode( mnode_t *node, decalinfo_t* decalinfo )
{
	cplane_t	*splitplane;
	float		dist;
	
	if (!node )
		return;
	if ( node->contents >= 0 )
	{
		R_DecalLeaf( (mleaf_t *)node, decalinfo );
		return;
	}

	splitplane = node->plane;
	dist = DotProduct (decalinfo->m_Position, splitplane->normal) - splitplane->dist;

	// This is arbitrarily set to 10 right now.  In an ideal world we'd have the 
	// exact surface but we don't so, this tells me which planes are "sort of 
	// close" to the gunshot -- the gunshot is actually 4 units in front of the 
	// wall (see dlls\weapons.cpp). We also need to check to see if the decal 
	// actually intersects the texture space of the surface, as this method tags
	// parallel surfaces in the same node always.
	// JAY: This still tags faces that aren't correct at edges because we don't 
	// have a surface normal

	if (dist > decalinfo->m_Size)
	{
		R_DecalNode (node->children[0], decalinfo);
	}
	else if (dist < -decalinfo->m_Size)
	{
		R_DecalNode (node->children[1], decalinfo);
	}
	else 
	{
		if ( dist < DECAL_DISTANCE && dist > -DECAL_DISTANCE )
			R_DecalNodeSurfaces( node, decalinfo );

		R_DecalNode (node->children[0], decalinfo);
		R_DecalNode (node->children[1], decalinfo);
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pList - 
//			count - 
// Output : static int
//-----------------------------------------------------------------------------
static int DecalListAdd( decallist_t *pList, int count )
{
	int			i;
	Vector		tmp;
	decallist_t	*pdecal;

	pdecal = pList + count;
	for ( i = 0; i < count; i++ )
	{
		if ( !Q_strcmp( pdecal->name, pList[i].name ) && 
			pdecal->entityIndex == pList[i].entityIndex )
		{
			VectorSubtract( pdecal->position, pList[i].position, tmp );	// Merge
			if ( VectorLength( tmp ) < 2 )	// UNDONE: Tune this '2' constant
				return count;
		}
	}

	// This is a new decal
	return count + 1;
}


typedef int (__cdecl *qsortFunc_t)( const void *, const void * );

static int __cdecl DecalDepthCompare( const decallist_t *elem1, const decallist_t *elem2 )
{
	if ( elem1->depth > elem2->depth )
		return -1;
	if ( elem1->depth < elem2->depth )
		return 1;

	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: Called by CSaveRestore::SaveClientState
// Input  : *pList - 
// Output : int
//-----------------------------------------------------------------------------
int DecalListCreate( decallist_t *pList )
{
	int total = 0;
	int i, depth;

	if ( host_state.worldmodel )
	{
		for ( i = 0; i < g_nMaxDecals; i++ )
		{
			decal_t *decal = s_aDecalPool[i];

			// Decal is in use and is not a custom decal
			if ( !decal ||
				!IS_SURF_VALID( decal->surfID ) ||
				 (decal->flags & ( FDECAL_CUSTOM | FDECAL_DONTSAVE ) ) )	
				 continue;

			decal_t		*pdecals;
			IMaterial 	*pMaterial;

			// compute depth
			depth = 0;
			pdecals = MSurf_DecalPointer( decal->surfID );
			while ( pdecals && pdecals != decal )
			{
				depth++;
				pdecals = pdecals->pnext;
			}
			pList[total].depth = depth;
			pList[total].flags = decal->flags;
			
			R_DecalUnProject( decal, &pList[total] );

			pMaterial = decal->material;
			Q_strncpy( pList[total].name, pMaterial->GetName(), sizeof( pList[total].name ) );

			// Check to see if the decal should be added
			total = DecalListAdd( pList, total );
		}
	}

	// Sort the decals lowest depth first, so they can be re-applied in order
	qsort( pList, total, sizeof(decallist_t), ( qsortFunc_t )DecalDepthCompare );

	return total;
}
// ---------------------------------------------------------

static bool R_DecalUnProject( decal_t *pdecal, decallist_t *entry )
{
	if ( !pdecal || !IS_SURF_VALID( pdecal->surfID ) )
		return false;

	VectorCopy( pdecal->position, entry->position );
	entry->entityIndex = pdecal->entityIndex;

	// Grab surface plane equation
	cplane_t plane = MSurf_Plane( pdecal->surfID );

	VectorCopy( plane.normal, entry->impactPlaneNormal );
	return true;
}


// Shoots a decal onto the surface of the BSP.  position is the center of the decal in world coords
static void R_DecalShoot_( IMaterial *pMaterial, int entity, const model_t *model, 
						  const Vector &position, const Vector *saxis, int flags, const color32 &rgbaColor, void *userdata = 0 )
{
	decalinfo_t decalInfo;
	VectorCopy( position, decalInfo.m_Position );	// Pass position in global

	if ( !model || model->type != mod_brush || !pMaterial )
		return;

	decalInfo.m_pModel = (model_t *)model;
	decalInfo.m_pBrush = model->brush.pShared;

	// Deal with the s axis if one was passed in
	if (saxis)
	{
		flags |= FDECAL_USESAXIS;
		VectorCopy( *saxis, decalInfo.m_SAxis );
	}

	// More state used by R_DecalNode()
	decalInfo.m_pMaterial = pMaterial;
	decalInfo.m_pUserData = userdata;

	// Don't optimize custom decals
	if ( !(flags & FDECAL_CUSTOM) )
		flags |= FDECAL_CLIPTEST;
	
	decalInfo.m_Flags = flags;
	decalInfo.m_Entity = entity;
	decalInfo.m_Size = pMaterial->GetMappingWidth() >> 1;
	if ( (int)(pMaterial->GetMappingHeight() >> 1) > decalInfo.m_Size )
		decalInfo.m_Size = pMaterial->GetMappingHeight() >> 1;

	// Compute scale of surface
	// FIXME: cache this?
	IMaterialVar *decalScaleVar;
	bool found;
	decalScaleVar = decalInfo.m_pMaterial->FindVar( "$decalScale", &found, false );
	if( found )
	{
		decalInfo.m_scale = 1.0f / decalScaleVar->GetFloatValue();
		decalInfo.m_Size *= decalScaleVar->GetFloatValue();
	}
	else
	{
		decalInfo.m_scale = 1.0f;
	}

	// compute the decal dimensions in world space
	decalInfo.m_decalWidth = pMaterial->GetMappingWidth() / decalInfo.m_scale;
	decalInfo.m_decalHeight = pMaterial->GetMappingHeight() / decalInfo.m_scale;
	decalInfo.m_Color = rgbaColor;

	decalInfo.m_aApplySurfs.Purge();

	// Clear the displacement tags because we use them in R_DecalNode.
	DispInfo_ClearAllTags( decalInfo.m_pBrush->hDispInfos );

	mnode_t *pnodes = decalInfo.m_pBrush->nodes + decalInfo.m_pModel->brush.firstnode;
	R_DecalNode( pnodes, &decalInfo );
}

// Shoots a decal onto the surface of the BSP.  position is the center of the decal in world coords
// This is called from cl_parse.cpp, cl_tent.cpp
void R_DecalShoot( int textureIndex, int entity, const model_t *model, const Vector &position, const Vector *saxis, int flags, const color32 &rgbaColor )
{	
	IMaterial* pMaterial = Draw_DecalMaterial( textureIndex );
	R_DecalShoot_( pMaterial, entity, model, position, saxis, flags, rgbaColor );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *material - 
//			playerIndex - 
//			entity - 
//			*model - 
//			position - 
//			*saxis - 
//			flags - 
//			&rgbaColor - 
//-----------------------------------------------------------------------------

void R_PlayerDecalShoot( IMaterial *material, void *userdata, int entity, const model_t *model, 
	const Vector& position, const Vector *saxis, int flags, const color32 &rgbaColor )
{
	// The userdata that is passed in is actually 
	// the player number (integer), not sure why it can't be zero.
	Assert( userdata != 0 );

	//
	// Linear search through decal pool to retire any other decals this
	// player has sprayed.  It appears that multiple decals can be
	// allocated for a single spray due to the way they are mapped to
	// surfaces.  We need to run through and clean them all up.  This
	// seems like the cleanest way to manage this - especially since
	// it doesn't happen that often.
	//
	int i;
	CUtlVector<decal_t *> decalVec;

	for ( i = 0; i<s_aDecalPool.Count(); i++ )
	{
		decal_t * decal = s_aDecalPool[i];

		if( decal &&
			decal->flags & FDECAL_PLAYERSPRAY &&
			decal->userdata == userdata )
		{
			decalVec.AddToTail( decal );
		}
	}

	// remove all the sprays we found
	for ( i = 0; i < decalVec.Count(); i++ )
	{
		R_DecalUnlink( decalVec[i], host_state.worldbrush );
	}

	// set this to be a player spray so it is timed out appropriately.
	flags |= FDECAL_PLAYERSPRAY;

	R_DecalShoot_( material, entity, model, position, saxis, flags, rgbaColor, userdata );
}

// Generate lighting coordinates at each vertex for decal vertices v[] on surface psurf
static void R_DecalVertsLight( CDecalVert* v, SurfaceHandle_t surfID, int vertCount )
{
	int j;
	float s, t;

	int lightmapPageWidth, lightmapPageHeight;

	materials->GetLightmapPageSize( SortInfoToLightmapPage(MSurf_MaterialSortID( surfID )),
		&lightmapPageWidth, &lightmapPageHeight );
	
	for ( j = 0; j < vertCount; j++, v++ )
	{
		s = DotProduct( v->m_vPos, MSurf_TexInfo( surfID )->lightmapVecsLuxelsPerWorldUnits[0].AsVector3D() ) + 
			MSurf_TexInfo( surfID )->lightmapVecsLuxelsPerWorldUnits[0][3];
		s -= MSurf_LightmapMins( surfID )[0];
		s += MSurf_OffsetIntoLightmapPage( surfID )[0];
		s += 0.5f;
		s *= ( 1.0f / lightmapPageWidth );

		t = DotProduct( v->m_vPos, MSurf_TexInfo( surfID )->lightmapVecsLuxelsPerWorldUnits[1].AsVector3D() ) + 
			MSurf_TexInfo( surfID )->lightmapVecsLuxelsPerWorldUnits[1][3];
		t -= MSurf_LightmapMins( surfID )[1];
		t += MSurf_OffsetIntoLightmapPage( surfID )[1];
		t += 0.5f;
		t *= ( 1.0f / lightmapPageHeight );

		v->m_cLMCoords.x = s;
		v->m_cLMCoords.y = t;
	}
}


//ConVar decal_cachetest( "decal_cachetest", "1" );
static CDecalVert* R_DecalVertsNoclip( decal_t *pdecal, SurfaceHandle_t surfID, IMaterial *pMaterial )
{
	decalcache_t	*pCache;
	int				decalIndex;
	int				outCount;

	decalIndex = R_DecalIndex( pdecal );
	pCache = R_DecalCacheSlot( decalIndex );
	
	// Is the decal cached?
	if ( pCache->decalIndex == decalIndex )
	{
		return &pCache->decalVert[0];
	}
#if 0
	unsigned short frameIndex = (r_framecount & 0xFFFF);
	if ( decal_cachetest.GetBool() && pCache->frameIndex == frameIndex )
	{
		// don't thrash - overwrite the temp cache instead
		pCache = &g_TempCache;
	}
	pCache->frameIndex = frameIndex;
#endif
	pCache->decalIndex = decalIndex;

	CDecalVert *vlist = &pCache->decalVert[0];

	// Use the old code for now, and just cache them
	vlist = R_DecalVertsClip( vlist, pdecal, surfID, pMaterial, &outCount );

	R_DecalVertsLight( vlist, surfID, 4 );

	return vlist;
}


//-----------------------------------------------------------------------------
// Purpose: Check for intersecting decals on this surface
// Input  : *psurf - 
//			*pcount - 
//			x - 
//			y - 
// Output : static decal_t
//-----------------------------------------------------------------------------
// UNDONE: This probably doesn't work quite right any more
// we should base overlap on the new decal basis matrix
// decal basis is constant per plane, perhaps we should store it (unscaled) in the shared plane struct
// BRJ: Note, decal basis is not constant when decals need to specify an s direction
// but that certainly isn't the majority case
static decal_t *R_DecalIntersect( decalinfo_t* decalinfo, SurfaceHandle_t surfID, int *pcount )
{
	decal_t		*plast = NULL;

	// (Same as R_SetupDecalClip).
	IMaterial	*pMaterial = decalinfo->m_pMaterial;

	*pcount = 0;
	
	// Precalculate the extents of decalinfo's decal in world space.
	int mapSize[2] = {pMaterial->GetMappingWidth(), pMaterial->GetMappingHeight()};
	Vector decalExtents[2];
	decalExtents[0] = decalinfo->m_Basis[0] * (mapSize[0] / decalinfo->m_scale) * 0.5f;
	decalExtents[1] = decalinfo->m_Basis[1] * (mapSize[1] / decalinfo->m_scale) * 0.5f;


	float lastArea = 2;
	decal_t *pDecal = MSurf_DecalPointer( surfID );
	while ( pDecal ) 
	{
		pMaterial = pDecal->material;

		// Don't steal bigger decals and replace them with smaller decals
		// Don't steal permanent decals, or player sprays
		if ( !(pDecal->flags & FDECAL_PERMANENT) && 
			 !(pDecal->flags & FDECAL_PLAYERSPRAY) && pMaterial )
		{
			Vector testBasis[3];
			float testWorldScale[2];
			R_SetupDecalTextureSpaceBasis( pDecal, MSurf_Plane( surfID ).normal, pMaterial, testBasis, testWorldScale );

			// Here, we project the min and max extents of the decal that got passed in into
			// this decal's (pDecal's) [0,0,1,1] clip space, just like we would if we were
			// clipping a triangle into pDecal's clip space.
			Vector2D vDecalMin(
				DotProduct( decalinfo->m_Position - decalExtents[0], testBasis[0] ) - pDecal->dx + 0.5f,
				DotProduct( decalinfo->m_Position - decalExtents[1], testBasis[1] ) - pDecal->dy + 0.5f );

			Vector2D vDecalMax( 
				DotProduct( decalinfo->m_Position + decalExtents[0], testBasis[0] ) - pDecal->dx + 0.5f,
				DotProduct( decalinfo->m_Position + decalExtents[1], testBasis[1] ) - pDecal->dy + 0.5f );	

			// Now figure out the part of the projection that intersects pDecal's
			// clip box [0,0,1,1].
			Vector2D vUnionMin( fpmax( vDecalMin.x, 0.0f ), fpmax( vDecalMin.y, 0.0f ) );
			Vector2D vUnionMax( fpmin( vDecalMax.x, 1.0f ), fpmin( vDecalMax.y, 1.0f ) );

			if( vUnionMin.x < 1 && vUnionMin.y < 1 && vUnionMax.x > 0 && vUnionMax.y > 0 )
			{
				// Figure out how much of this intersects the (0,0) - (1,1) bbox.			
				float flArea = (vUnionMax.x - vUnionMin.x) * (vUnionMax.y - vUnionMin.y);

				if( flArea > 0.6 )
				{
					*pcount += 1;
					if ( !plast || flArea <= lastArea ) 
					{
						plast = pDecal;
						lastArea =  flArea;
					}
				}
			}
		}
		
		pDecal = pDecal->pnext;
	}
	
	return plast;
}


// Add the decal to the surface's list of decals.
// If the surface is a displacement, let the displacement precalculate data for the decal.
static void R_AddDecalToSurface( 
	decal_t *pdecal, 
	SurfaceHandle_t surfID,
	decalinfo_t *decalinfo )
{
	pdecal->pnext = NULL;
	decal_t *pold = MSurf_DecalPointer( surfID );
	if ( pold ) 
	{
		while ( pold->pnext ) 
			pold = pold->pnext;
		pold->pnext = pdecal;
	}
	else
	{
		MSurf_Decals( surfID ) = DecalToHandle(pdecal);
	}

	// Tag surface
	pdecal->surfID = surfID;
	pdecal->m_Size = decalinfo->m_Size;

	// Let the dispinfo reclip the decal if need be.
	if( SurfaceHasDispInfo( surfID ) )
	{
		pdecal->m_DispDecal = MSurf_DispInfo( surfID )->NotifyAddDecal( pdecal, decalinfo->m_Size );
	}

	// Add surface to list.
	decalinfo->m_aApplySurfs.AddToTail( surfID );
}

//=============================================================================
//
// Decal batches for rendering.
//
CUtlVector<DecalSortVertexFormat_t>	g_aDecalFormats;

CUtlVector<DecalSortTrees_t>		g_aDecalSortTrees;
CUtlFixedLinkedList<decal_t*>		g_aDecalSortPool;
int									g_nDecalSortCheckCount;
int									g_nBrushModelDecalSortCheckCount;

CUtlFixedLinkedList<decal_t*>		g_aDispDecalSortPool;
CUtlVector<DecalSortTrees_t>		g_aDispDecalSortTrees;
int									g_nDispDecalSortCheckCount;

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void R_DecalSortInit( void )
{
	g_aDecalFormats.Purge();

	g_aDecalSortTrees.Purge();
	g_aDecalSortPool.Purge();
	g_aDecalSortPool.EnsureCapacity( g_nMaxDecals );
	g_aDecalSortPool.SetGrowSize( 128 );
	g_nDecalSortCheckCount = 0;
	g_nBrushModelDecalSortCheckCount = 0;

	g_aDispDecalSortTrees.Purge();
	g_aDispDecalSortPool.Purge();
	g_aDispDecalSortPool.EnsureCapacity( g_nMaxDecals );
	g_aDispDecalSortPool.SetGrowSize( 128 );
	g_nDispDecalSortCheckCount = 0;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void DecalSurfacesInit( bool bBrushModel )
{
	if ( !bBrushModel )
	{
		// Only clear the pool once per frame.
		g_aDecalSortPool.RemoveAll();
		++g_nDecalSortCheckCount;
	}
	else
	{
		++g_nBrushModelDecalSortCheckCount;
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
static void R_DecalMaterialSort( decal_t *pDecal, SurfaceHandle_t surfID )
{
	// Setup the decal material sort data.
	DecalMaterialSortData_t sort;
	if ( pDecal->material->InMaterialPage() )
	{
		sort.m_pMaterial = pDecal->material->GetMaterialPage();
	}
	else
	{
		sort.m_pMaterial = pDecal->material;
	}
	sort.m_iLightmapPage = materialSortInfoArray[MSurf_MaterialSortID( surfID )].lightmapPageID;

	// Does this vertex type exist?
	VertexFormat_t vertexFormat = GetUncompressedFormat( sort.m_pMaterial );
	int iFormat = 0;
	int nFormatCount = g_aDecalFormats.Count();
	for ( ; iFormat < nFormatCount; ++iFormat )
	{
		if ( g_aDecalFormats[iFormat].m_VertexFormat == vertexFormat )
			break;
	}

	// A new vertex format type.
	if ( iFormat == nFormatCount )
	{
		iFormat = g_aDecalFormats.AddToTail();
		g_aDecalFormats[iFormat].m_VertexFormat = vertexFormat;
		int iSortTree = g_aDecalSortTrees.AddToTail();
		g_aDispDecalSortTrees.AddToTail();
		g_aDecalFormats[iFormat].m_iSortTree = iSortTree;
	}

	// Get an index for the current sort tree.
	int iSortTree = g_aDecalFormats[iFormat].m_iSortTree;
	int iTreeType = -1;

	// Lightmapped.
	if ( sort.m_pMaterial->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_LIGHTMAP ) )
	{
		// Permanent lightmapped decals.
		if ( pDecal->flags & FDECAL_PERMANENT )
		{
			iTreeType = PERMANENT_LIGHTMAP;
		}
		// Non-permanent lightmapped decals.
		else
		{
			iTreeType = LIGHTMAP;
		}
	}
	// Non-lightmapped decals.
	else
	{
		iTreeType = NONLIGHTMAP;
		sort.m_iLightmapPage = -1;
	}

	int iSort = g_aDecalSortTrees[iSortTree].m_pTrees[iTreeType]->Find( sort );
	if ( iSort == -1 )
	{
		int iBucket = g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[0][iTreeType].AddToTail();
		g_aDispDecalSortTrees[iSortTree].m_aDecalSortBuckets[0][iTreeType].AddToTail();

		g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[0][iTreeType].Element( iBucket ).m_nCheckCount = -1;
		g_aDispDecalSortTrees[iSortTree].m_aDecalSortBuckets[0][iTreeType].Element( iBucket ).m_nCheckCount = -1;

		for ( int iGroup = 1; iGroup < ( MAX_MAT_SORT_GROUPS + 1 ); ++iGroup )
		{
			g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].AddToTail();
			g_aDispDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].AddToTail();

			g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket ).m_nCheckCount = -1;
			g_aDispDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket ).m_nCheckCount = -1;
		}
		
		sort.m_iBucket = iBucket;
		g_aDecalSortTrees[iSortTree].m_pTrees[iTreeType]->Insert( sort );
		g_aDispDecalSortTrees[iSortTree].m_pTrees[iTreeType]->Insert( sort );

		pDecal->m_iSortTree = iSortTree;
		pDecal->m_iSortMaterial = sort.m_iBucket;
	}
	else
	{
		pDecal->m_iSortTree = iSortTree;
		pDecal->m_iSortMaterial = g_aDecalSortTrees[iSortTree].m_pTrees[iTreeType]->Element( iSort ).m_iBucket;
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void R_DecalReSortMaterials( void ) //X
{
	R_DecalSortInit();
	
	int nDecalCount = s_aDecalPool.Count();
	for ( int iDecal = 0; iDecal < nDecalCount; ++iDecal )
	{
		decal_t *pDecal = s_aDecalPool.Element( iDecal );
		if ( pDecal )
		{
			SurfaceHandle_t surfID = pDecal->surfID;
			R_DecalMaterialSort( pDecal, surfID );
		}
	}
}

// Allocate and initialize a decal from the pool, on surface with offsets x, y
// UNDONE: offsets are not really meaningful in new decal coordinate system
// the clipping code will recalc the offsets
static void R_DecalCreate( 
	decalinfo_t* decalinfo, 
	SurfaceHandle_t surfID, 
	float x, 
	float y, 
	bool bForceForDisplacement )
{
	decal_t			*pdecal;
	int				count, vertCount;

	if( !IS_SURF_VALID( surfID ) )
	{
		ConMsg( "psurface NULL in R_DecalCreate!\n" );
		return;
	}
	
	decal_t *pold = R_DecalIntersect( decalinfo, surfID, &count );
	if ( count >= MAX_OVERLAP_DECALS ) 
	{
		R_DecalUnlink( pold, host_state.worldbrush );
		pold = NULL;
	}

	pdecal = R_DecalAlloc( decalinfo->m_Flags );
	
	pdecal->flags = decalinfo->m_Flags;
	pdecal->color = decalinfo->m_Color;
	VectorCopy( decalinfo->m_Position, pdecal->position );
	if (pdecal->flags & FDECAL_USESAXIS)
		VectorCopy( decalinfo->m_SAxis, pdecal->saxis );
	pdecal->dx = x;
	pdecal->dy = y;
	pdecal->material = decalinfo->m_pMaterial;
	Assert( pdecal->material );
	pdecal->userdata = decalinfo->m_pUserData;

	// Set scaling
	pdecal->scale = decalinfo->m_scale;
	pdecal->entityIndex = decalinfo->m_Entity;

	// Get dynamic information from the material (fade start, fade time)
	bool found;
	IMaterialVar* decalVar = decalinfo->m_pMaterial->FindVar( "$decalFadeDuration", &found, false );
	if ( found  )
	{
		pdecal->flags |= FDECAL_DYNAMIC;
		pdecal->fadeDuration = decalVar->GetFloatValue();
		decalVar = decalinfo->m_pMaterial->FindVar( "$decalFadeTime", &found, false );
		pdecal->fadeStartTime = found ? decalVar->GetFloatValue() : 0.0f;
		pdecal->fadeStartTime += cl.GetTime();
	}

	// Check for Dynamic Scale, and cache values
	decalVar = decalinfo->m_pMaterial->FindVar( "$decalDynamicScale", &found, false );
	if ( found )
	{
		pdecal->flags |= FDECAL_DISTANCESCALE;
	}

	// check for a player spray
	if( pdecal->flags & FDECAL_PLAYERSPRAY )
	{
		// reset the number of rounds this should be visible for
		pdecal->fadeStartTime = 0.0f;

		// Force the scale to 1 for player sprays.
		pdecal->scale = 1.0f;
	}

	// Is this a second-pass decal?
	decalVar = decalinfo->m_pMaterial->FindVar( "$decalSecondPass", &found, false );
	if ( found  )
		pdecal->flags |= FDECAL_SECONDPASS;

	if( !bForceForDisplacement )
	{
		// Check to see if the decal actually intersects the surface
		// if not, then remove the decal
		R_DecalVertsClip( NULL, pdecal, surfID, 
			decalinfo->m_pMaterial, &vertCount );
		if ( !vertCount )
		{
			R_DecalUnlink( pdecal, host_state.worldbrush );
			return;
		}
	}

	// Add to the surface's list
	R_AddDecalToSurface( pdecal, surfID, decalinfo );

	// Add decal material/lightmap to sort list.
	R_DecalMaterialSort( pdecal, surfID );
}

//-----------------------------------------------------------------------------
// Updates all decals, returns true if the decal should be retired
//-----------------------------------------------------------------------------

bool DecalUpdate( decal_t* pDecal )
{
	// retire the decal if it's time has come
	if (pDecal->fadeDuration > 0)
	{
		return (cl.GetTime() >= pDecal->fadeStartTime + pDecal->fadeDuration);
	}
	return false;
}

// Build the vertex list for a decal on a surface and clip it to the surface.
// This is a template so it can work on world surfaces and dynamic displacement 
// triangles the same way.

CDecalVert* R_DecalSetupVerts( IMatRenderContext *pRenderContext, decal_t *pDecal, SurfaceHandle_t surfID, IMaterial *pMaterial, int &outCount, const Vector &vModelOrg )
{
	Vector dir = pDecal->position - vModelOrg;
	VectorNormalize(dir);

	// Clamp the max solid angle effect to something near 90 (~87 degrees here)
	// Because some large things tend to blink out here if you don't
	float dot = 0.05f - DotProduct( dir, MSurf_Plane(surfID).normal );
	if ( dot > 1.0f )
	{
		dot = 1.0f;
	}
	else if ( dot <= 0.0f )
	{
		outCount = 0;
		return NULL;
	}

	Vector worldSpacePosition = ( pDecal->entityIndex != 0 ) ? g_BrushToWorldMatrix.VMul4x3(pDecal->position) : pDecal->position;
	float pixels = pRenderContext->ComputePixelWidthOfSphere( worldSpacePosition, pDecal->m_Size );
	if ( pixels*dot < r_decal_cullsize.GetFloat() )
	{
		outCount = 0;
		return NULL;
	}

	float scaleFactor = 1.0f;

	//
	// Do not scale playersprays
	//
	if( !(pDecal->flags & FDECAL_PLAYERSPRAY) )
	{
		if( pDecal->flags & FDECAL_DISTANCESCALE )
		{
			float nearScale, farScale, nearDist, farDist;

			nearScale = r_dscale_nearscale.GetFloat();
			nearDist = r_dscale_neardist.GetFloat();
			farScale = r_dscale_farscale.GetFloat();
			farDist = r_dscale_fardist.GetFloat();

			Vector playerOrigin = CurrentViewOrigin();

			float dist = (playerOrigin - worldSpacePosition).Length();
			float fov = g_EngineRenderer->GetFov();

			//
			// If the player is zoomed in, we adjust the nearScale and farScale
			//
			if ( fov != r_dscale_basefov.GetFloat() && fov > 0 && r_dscale_basefov.GetFloat() > 0 )
			{
				float fovScale = fov / r_dscale_basefov.GetFloat();
				nearScale *= fovScale;
				farScale *= fovScale;

				if ( nearScale < 1.0f )
					nearScale = 1.0f;
				if ( farScale < 1.0f )
					farScale = 1.0f;
			}

			//
			// Scaling works like this:
			// 
			// 0->nearDist             scale = 1.0
			// nearDist -> farDist     scale = LERP(nearScale, farScale)
			// farDist->inf            scale = farScale
			//
			// scaling in the rest of the code appears to be more of an
			// attenuation factor rather than a scale, so we compute 1/scale
			// to account for this.
			//
			if ( dist < nearDist )
				scaleFactor = 1.0f;
			else if( dist >= farDist )
				scaleFactor = farScale;
			else
			{
				float percent = (dist - nearDist) / (farDist - nearDist);
				scaleFactor = nearScale + percent * (farScale - nearScale);
			}

			//
			// scaling in the rest of the code appears to be more of an
			// attenuation factor rather than a scale, so we compute 1/scale
			// to account for this.
			//
			scaleFactor = 1.0f / scaleFactor;
		}
	}

	float originalScale = pDecal->scale;
	float scaledScale = pDecal->scale * scaleFactor;
	pDecal->scale = scaledScale;

	CDecalVert *v;
	// Distance-scaled decals can't go through the noclip path or they can end up scaled incorrectly
	if ( ( pDecal->flags & FDECAL_NOCLIP ) && !( pDecal->flags & FDECAL_DISTANCESCALE ) )
	{
		v = R_DecalVertsNoclip( pDecal, surfID, pMaterial );
		outCount = 4;
	}
	else
	{
		v = R_DecalVertsClip( NULL, pDecal, surfID, pMaterial, &outCount );
		if ( outCount )
		{
			R_DecalVertsLight( v, surfID, outCount );
		}
	}
	pDecal->scale = originalScale;
	
	return v;
}


//-----------------------------------------------------------------------------
// Renders a single decal, *could retire the decal!!*
//-----------------------------------------------------------------------------

void DecalUpdateAndDrawSingle( IMatRenderContext *pRenderContext, SurfaceHandle_t surfID, decal_t* pDecal, const Vector &vModelOrg )
{
	if( !pDecal->material )
		return;

	// Update dynamic decals
	bool retire = false;
	if ( pDecal->flags & FDECAL_DYNAMIC )
		retire = DecalUpdate( pDecal );

	if( SurfaceHasDispInfo( surfID ) )
	{
		// Dispinfos generate lists of tris for decals when the decal is first
		// created.
	}
	else
	{
		int outCount;
		CDecalVert *v = R_DecalSetupVerts( pRenderContext, pDecal, surfID, pDecal->material, outCount, vModelOrg );

		if ( outCount )
			Shader_DecalDrawPoly( v, pDecal->material, surfID, outCount, pDecal );
	}

	if( retire )
	{
		R_DecalUnlink( pDecal, host_state.worldbrush );
	}
}


//-----------------------------------------------------------------------------
// Renders all decals on a single surface
//-----------------------------------------------------------------------------

void DrawDecalsOnSingleSurface_NonQueued( IMatRenderContext *pRenderContext, SurfaceHandle_t surfID, const Vector &vModelOrg)
{
	decal_t* plist = MSurf_DecalPointer( surfID );

#if 1
	// FIXME!  Make this not truck through the decal list twice.
	while ( plist ) 
	{
		// Store off the next pointer, DecalUpdateAndDrawSingle could unlink
		decal_t* pnext = plist->pnext;

		if (!(plist->flags & FDECAL_SECONDPASS))
		{
			DecalUpdateAndDrawSingle( pRenderContext, surfID, plist, vModelOrg );
		}
		plist = pnext;
	}
	plist = MSurf_DecalPointer( surfID );
	while ( plist ) 
	{
		// Store off the next pointer, DecalUpdateAndDrawSingle could unlink
		decal_t* pnext = plist->pnext;

		if ((plist->flags & FDECAL_SECONDPASS))
		{
			DecalUpdateAndDrawSingle( pRenderContext, surfID, plist, vModelOrg );
		}
		plist = pnext;
	}
#else
	// FIXME!!:  This code screws up since DecalUpdateAndDrawSingle can 
	// unlink items from the decal list.
	// The version below is used instead, which trucks through memory twice. . . 
	// not optimal.
	decal_t* pPrev = 0;
	decal_t* pSecondPass = 0;
	while ( plist ) 
	{
		// Store off the next pointer, DecalUpdateAndDrawSingle could unlink
		decal_t* pnext = plist->pnext;

		if ((plist->flags & FDECAL_SECONDPASS) != 0)
		{
			if (pPrev)
				pPrev->pnext = pnext;
			else
				psurf->pdecals = pnext;
			plist->pnext = pSecondPass;
			pSecondPass = plist;
		}
		else
		{
			DecalUpdateAndDrawSingle( pRenderContext, psurf, plist );
			pPrev = plist;
		}
		plist = pnext;
	}

	// Re-link second pass...
	if (pPrev)
		pPrev->pnext = pSecondPass;
	else
		psurf->pdecals = pSecondPass;

	plist = pSecondPass;
	while ( plist )
	{
		// Store off the next pointer, DecalUpdateAndDrawSingle could unlink
		decal_t* pnext = plist->pnext;
		DecalUpdateAndDrawSingle( pRenderContext, psurf, plist );
		plist = pnext;
	}
#endif
}

void DrawDecalsOnSingleSurface_QueueHelper( SurfaceHandle_t surfID, Vector vModelOrg )
{
	CMatRenderContextPtr pRenderContext( materials );
	DrawDecalsOnSingleSurface_NonQueued( pRenderContext, surfID, vModelOrg );
}

void DrawDecalsOnSingleSurface( IMatRenderContext *pRenderContext, SurfaceHandle_t surfID )
{
	ICallQueue *pCallQueue;
	if ( r_queued_decals.GetBool() && (pCallQueue = pRenderContext->GetCallQueue()) != NULL )
	{
		//queue available and desired
		pCallQueue->QueueCall( DrawDecalsOnSingleSurface_QueueHelper, surfID, modelorg );
	}
	else
	{
		//non-queued mode
		DrawDecalsOnSingleSurface_NonQueued( pRenderContext, surfID, modelorg );
	}
}

void R_DrawDecalsAllImmediate_GatherDecals( IMatRenderContext *pRenderContext, int iGroup, int iTreeType, CUtlVector<decal_t *> &DrawDecals )
{
	int nCheckCount = g_nDecalSortCheckCount;
	if ( iGroup == MAX_MAT_SORT_GROUPS )
	{
		// Brush Model
		nCheckCount = g_nBrushModelDecalSortCheckCount;
	}

	int nSortTreeCount = g_aDecalSortTrees.Count();
	for ( int iSortTree = 0; iSortTree < nSortTreeCount; ++iSortTree )
	{
		int nBucketCount = g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Count();
		for ( int iBucket = 0; iBucket < nBucketCount; ++iBucket )
		{
			if ( g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket ).m_nCheckCount != nCheckCount )
				continue;

			int iHead = g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket ).m_iHead;

			int iElement = iHead;
			while ( iElement != g_aDecalSortPool.InvalidIndex() )
			{
				decal_t *pDecal = g_aDecalSortPool.Element( iElement );
				iElement = g_aDecalSortPool.Next( iElement );

				if ( !pDecal )
					continue;

				DrawDecals.AddToTail( pDecal );
			}
		}
	}
}

void R_DrawDecalsAllImmediate_Gathered( IMatRenderContext *pRenderContext, decal_t **ppDecals, int iDecalCount, const Vector &vModelOrg )
{
	for( int i = 0; i != iDecalCount; ++i )
	{
		decal_t * pDecal = ppDecals[i];

		Assert( pDecal != NULL );

		// Add the decal to the list of decals to be destroyed if need be.
		if ( ( pDecal->flags & FDECAL_DYNAMIC ) && !( pDecal->flags & FDECAL_HASUPDATED ) )
		{
			pDecal->flags |= FDECAL_HASUPDATED;
			if ( DecalUpdate( pDecal ) )
			{
				R_DecalAddToDestroyList( pDecal );
				continue;
			}
		}

		int nCount;
		CDecalVert *pVerts = R_DecalSetupVerts( pRenderContext, pDecal, pDecal->surfID, pDecal->material, nCount, vModelOrg );
		if ( nCount == 0 )
			continue;

		// Bind texture.
		VertexFormat_t vertexFormat = 0;
		if( ShouldDrawInWireFrameMode () )
		{
			pRenderContext->Bind( g_materialDecalWireframe );
		}
		else
		{
			pRenderContext->BindLightmapPage( materialSortInfoArray[MSurf_MaterialSortID( pDecal->surfID )].lightmapPageID );
			pRenderContext->Bind( pDecal->material, pDecal->userdata );
			vertexFormat = GetUncompressedFormat( pDecal->material );
		}

		IMesh *pMesh = NULL;
		pMesh = pRenderContext->GetDynamicMesh();
		CMeshBuilder meshBuilder;
		meshBuilder.Begin( pMesh, MATERIAL_TRIANGLES, nCount, ( ( nCount - 2 ) * 3 ) );

		// Set base color.
		byte color[4] = { pDecal->color.r, pDecal->color.g, pDecal->color.b, pDecal->color.a };

		// Dynamic decals - fading.
		if ( pDecal->flags & FDECAL_DYNAMIC )
		{
			float flFadeValue;

			// Negative fadeDuration value means to fade in
			if ( pDecal->fadeDuration < 0 )
			{
				flFadeValue = -( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
			}
			else
			{
				flFadeValue = 1.0 - ( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
			}

			flFadeValue = clamp( flFadeValue, 0.0f, 1.0f );

			color[3] = ( byte )( 255 * flFadeValue );
		}

		// Compute normal and tangent space if necessary.
		Vector vecNormal( 0.0f, 0.0f, 1.0f ), vecTangentS( 1.0f, 0.0f, 0.0f ), vecTangentT( 0.0f, 1.0f, 0.0f );
		if ( vertexFormat & ( VERTEX_NORMAL | VERTEX_TANGENT_SPACE ) )
		{
			vecNormal = MSurf_Plane( pDecal->surfID ).normal;
			if ( vertexFormat & VERTEX_TANGENT_SPACE )
			{
				Vector tVect;
				bool bNegate = TangentSpaceSurfaceSetup( pDecal->surfID, tVect );
				TangentSpaceComputeBasis( vecTangentS, vecTangentT, vecNormal, tVect, bNegate );
			}
		}

		// Setup verts.
		float flOffset = ComputeDecalLightmapOffset( pDecal->surfID );
		for ( int iVert = 0; iVert < nCount; ++iVert, ++pVerts )
		{
			meshBuilder.Position3fv( pVerts->m_vPos.Base() );
			if ( vertexFormat & VERTEX_NORMAL )
			{
				meshBuilder.Normal3fv( vecNormal.Base() );
			}
			meshBuilder.Color4ubv( color );
			if ( pDecal->material->InMaterialPage() )
			{
				float offset[2], scale[2];
				pDecal->material->GetMaterialOffset( offset );
				pDecal->material->GetMaterialScale( scale );
				meshBuilder.TexCoordSubRect2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y, offset[0], offset[1], scale[0], scale[1] );
			}
			else
			{
				meshBuilder.TexCoord2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y  );
			}
			meshBuilder.TexCoord2f( 1, pVerts->m_cLMCoords.x,  pVerts->m_cLMCoords.y );
			meshBuilder.TexCoord1f( 2, flOffset );
			if ( vertexFormat & VERTEX_TANGENT_SPACE )
			{
				meshBuilder.TangentS3fv( vecTangentS.Base() ); 
				meshBuilder.TangentT3fv( vecTangentT.Base() ); 
			}

			meshBuilder.AdvanceVertex();
		}

		// Setup indices.
		int nTriCount = ( nCount - 2 );
		for ( int iTri = 0; iTri < nTriCount; ++iTri )
		{
			meshBuilder.FastIndex( 0 );
			meshBuilder.FastIndex( iTri + 1 );
			meshBuilder.FastIndex( iTri + 2 );
		}

		meshBuilder.End();
		pMesh->Draw();
	}
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void R_DrawDecalsAllImmediate( IMatRenderContext *pRenderContext, int iGroup, int iTreeType, const Vector &vModelOrg, int nCheckCount )
{
	int nSortTreeCount = g_aDecalSortTrees.Count();
	for ( int iSortTree = 0; iSortTree < nSortTreeCount; ++iSortTree )
	{
		int nBucketCount = g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Count();
		for ( int iBucket = 0; iBucket < nBucketCount; ++iBucket )
		{
			if ( g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket ).m_nCheckCount != nCheckCount )
				continue;
			
			int iHead = g_aDecalSortTrees[iSortTree].m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket ).m_iHead;
			
			int nCount;
			int iElement = iHead;
			while ( iElement != g_aDecalSortPool.InvalidIndex() )
			{
				decal_t *pDecal = g_aDecalSortPool.Element( iElement );
				iElement = g_aDecalSortPool.Next( iElement );
				
				if ( !pDecal )
					continue;

				// Add the decal to the list of decals to be destroyed if need be.
				if ( ( pDecal->flags & FDECAL_DYNAMIC ) && !( pDecal->flags & FDECAL_HASUPDATED ) )
				{
					pDecal->flags |= FDECAL_HASUPDATED;
					if ( DecalUpdate( pDecal ) )
					{
						R_DecalAddToDestroyList( pDecal );
						continue;
					}
				}

				CDecalVert *pVerts = R_DecalSetupVerts( pRenderContext, pDecal, pDecal->surfID, pDecal->material, nCount, vModelOrg );
				if ( nCount == 0 )
					continue;

				// Bind texture.
				VertexFormat_t vertexFormat = 0;
				if( ShouldDrawInWireFrameMode () )
				{
					pRenderContext->Bind( g_materialDecalWireframe );
				}
				else
				{
					pRenderContext->BindLightmapPage( materialSortInfoArray[MSurf_MaterialSortID( pDecal->surfID )].lightmapPageID );
					pRenderContext->Bind( pDecal->material, pDecal->userdata );
					vertexFormat = GetUncompressedFormat( pDecal->material );
				}

				IMesh *pMesh = NULL;
				pMesh = pRenderContext->GetDynamicMesh();
				CMeshBuilder meshBuilder;
				meshBuilder.Begin( pMesh, MATERIAL_TRIANGLES, nCount, ( ( nCount - 2 ) * 3 ) );

				// Set base color.
				byte color[4] = { pDecal->color.r, pDecal->color.g, pDecal->color.b, pDecal->color.a };
				
				// Dynamic decals - fading.
				if ( pDecal->flags & FDECAL_DYNAMIC )
				{
					float flFadeValue;
					
					// Negative fadeDuration value means to fade in
					if ( pDecal->fadeDuration < 0 )
					{
						flFadeValue = -( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
					}
					else
					{
						flFadeValue = 1.0 - ( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
					}
					
					flFadeValue = clamp( flFadeValue, 0.0f, 1.0f );
					
					color[3] = ( byte )( 255 * flFadeValue );
				}
				
				// Compute normal and tangent space if necessary.
				Vector vecNormal( 0.0f, 0.0f, 1.0f ), vecTangentS( 1.0f, 0.0f, 0.0f ), vecTangentT( 0.0f, 1.0f, 0.0f );
				if ( vertexFormat & ( VERTEX_NORMAL | VERTEX_TANGENT_SPACE ) )
				{
					vecNormal = MSurf_Plane( pDecal->surfID ).normal;
					if ( vertexFormat & VERTEX_TANGENT_SPACE )
					{
						Vector tVect;
						bool bNegate = TangentSpaceSurfaceSetup( pDecal->surfID, tVect );
						TangentSpaceComputeBasis( vecTangentS, vecTangentT, vecNormal, tVect, bNegate );
					}
				}
				
				// Setup verts.
				float flOffset = ComputeDecalLightmapOffset( pDecal->surfID );
				for ( int iVert = 0; iVert < nCount; ++iVert, ++pVerts )
				{
					meshBuilder.Position3fv( pVerts->m_vPos.Base() );
					if ( vertexFormat & VERTEX_NORMAL )
					{
						meshBuilder.Normal3fv( vecNormal.Base() );
					}
					meshBuilder.Color4ubv( color );
					if ( pDecal->material->InMaterialPage() )
					{
						float offset[2], scale[2];
						pDecal->material->GetMaterialOffset( offset );
						pDecal->material->GetMaterialScale( scale );
						meshBuilder.TexCoordSubRect2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y, offset[0], offset[1], scale[0], scale[1] );
					}
					else
					{
						meshBuilder.TexCoord2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y  );
					}
					meshBuilder.TexCoord2f( 1, pVerts->m_cLMCoords.x,  pVerts->m_cLMCoords.y );
					meshBuilder.TexCoord1f( 2, flOffset );
					if ( vertexFormat & VERTEX_TANGENT_SPACE )
					{
						meshBuilder.TangentS3fv( vecTangentS.Base() ); 
						meshBuilder.TangentT3fv( vecTangentT.Base() ); 
					}
					
					meshBuilder.AdvanceVertex();
				}

				// Setup indices.
				int nTriCount = ( nCount - 2 );
				for ( int iTri = 0; iTri < nTriCount; ++iTri )
				{
					meshBuilder.FastIndex( 0 );
					meshBuilder.FastIndex( iTri + 1 );
					meshBuilder.FastIndex( iTri + 2 );
				}

				meshBuilder.End();
				pMesh->Draw();
			}
		}
	}
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
inline void R_DrawDecalMeshList( DecalMeshList_t &meshList )
{
	CMatRenderContextPtr pRenderContext( materials );

	int nBatchCount = meshList.m_aBatches.Count();
	for ( int iBatch = 0; iBatch < nBatchCount; ++iBatch )
	{
		if ( g_pMaterialSystemConfig->nFullbright == 1 )
		{
			pRenderContext->BindLightmapPage( MATERIAL_SYSTEM_LIGHTMAP_PAGE_WHITE );
		}
		else
		{
			pRenderContext->BindLightmapPage( meshList.m_aBatches[iBatch].m_iLightmapPage );
		}
		
		pRenderContext->Bind( meshList.m_aBatches[iBatch].m_pMaterial, meshList.m_aBatches[iBatch].m_pProxy );
		meshList.m_pMesh->Draw( meshList.m_aBatches[iBatch].m_iStartIndex, meshList.m_aBatches[iBatch].m_nIndexCount );
	}
}

#define DECALMARKERS_SWITCHSORTTREE ((decal_t *)0x00000000)
#define DECALMARKERS_SWITCHBUCKET	((decal_t *)0xFFFFFFFF)
//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void R_DrawDecalsAll_GatherDecals( IMatRenderContext *pRenderContext, int iGroup, int iTreeType, CUtlVector<decal_t *> &DrawDecals )
{
	int nCheckCount = g_nDecalSortCheckCount;
	if ( iGroup == MAX_MAT_SORT_GROUPS )
	{
		// Brush Model
		nCheckCount = g_nBrushModelDecalSortCheckCount;
	}

	int nSortTreeCount = g_aDecalSortTrees.Count();
	for ( int iSortTree = 0; iSortTree < nSortTreeCount; ++iSortTree )
	{		
		DrawDecals.AddToTail( DECALMARKERS_SWITCHSORTTREE );

		DecalSortTrees_t &sortTree = g_aDecalSortTrees[iSortTree];
		int nBucketCount = sortTree.m_aDecalSortBuckets[iGroup][iTreeType].Count();
		for ( int iBucket = 0; iBucket < nBucketCount; ++iBucket )
		{
			DecalMaterialBucket_t &bucket = sortTree.m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket );
			if ( bucket.m_nCheckCount != nCheckCount )
				continue;

			int iHead = bucket.m_iHead;
			if ( !g_aDecalSortPool.IsValidIndex( iHead ) )
				continue;

			decal_t *pDecalHead = g_aDecalSortPool.Element( iHead );
			Assert( pDecalHead->material );
			if ( !pDecalHead->material )
				continue;

			// Vertex format.
			VertexFormat_t vertexFormat = GetUncompressedFormat( pDecalHead->material );
			if ( vertexFormat == 0 )
				continue;

			DrawDecals.AddToTail( DECALMARKERS_SWITCHBUCKET );

			int iElement = iHead;
			while ( iElement != g_aDecalSortPool.InvalidIndex() )
			{
				decal_t *pDecal = g_aDecalSortPool.Element( iElement );
				iElement = g_aDecalSortPool.Next( iElement );

				if ( !pDecal )
					continue;

				DrawDecals.AddToTail( pDecal );
			}			
		}
	}	
}

void R_DrawDecalsAll_Gathered( IMatRenderContext *pRenderContext, decal_t **ppDecals, int iDecalCount, const Vector &vModelOrg )
{
	DecalMeshList_t		meshList;
	CMeshBuilder		meshBuilder;

	int nVertCount = 0;
	int nIndexCount = 0;

	int nCount;

	int nDecalSortMaxVerts = g_nMaxDecals * 5;
	int nDecalSortMaxIndices = nDecalSortMaxVerts * 3;

	bool bMeshInit = true;
	bool bBatchInit = true;
	
	DecalBatchList_t *pBatch = NULL;
	VertexFormat_t vertexFormat = 0;
	decal_t *pDecalHead = NULL;

	for( int i = 0; i != iDecalCount; ++i )
	{
		decal_t *pDecal = ppDecals[i];
		if( (pDecal == DECALMARKERS_SWITCHSORTTREE) || (pDecal == DECALMARKERS_SWITCHBUCKET) )
		{
			if ( pBatch )
			{
				pBatch->m_nIndexCount = ( nIndexCount - pBatch->m_iStartIndex );
			}

			if ( pDecal == DECALMARKERS_SWITCHSORTTREE )
			{
				if ( !bMeshInit )
				{
					meshBuilder.End();
					R_DrawDecalMeshList( meshList );
					bMeshInit = true;
				}
			}

			bBatchInit = true;
			pBatch = NULL;

			if ( pDecal == DECALMARKERS_SWITCHBUCKET )
			{
				//find the new head decal
				for( int j = i + 1; j != iDecalCount; ++j )
				{
					pDecalHead = ppDecals[j];
					if( (pDecalHead != DECALMARKERS_SWITCHSORTTREE) && (pDecalHead != DECALMARKERS_SWITCHBUCKET) )
						break;
				}

				vertexFormat = GetUncompressedFormat( pDecalHead->material );
			}

			continue;
		}

		// Add the decal to the list of decals to be destroyed if need be.
		if ( ( pDecal->flags & FDECAL_DYNAMIC ) && !( pDecal->flags & FDECAL_HASUPDATED ) )
		{
			pDecal->flags |= FDECAL_HASUPDATED;
			if ( DecalUpdate( pDecal ) )
			{
				R_DecalAddToDestroyList( pDecal );
				continue;
			}
		}

		CDecalVert *pVerts = R_DecalSetupVerts( pRenderContext, pDecal, pDecal->surfID, pDecal->material, nCount, vModelOrg );
		if ( nCount == 0 )
			continue;

		// Overflow - new mesh, batch.
		if ( ( ( nVertCount + nCount ) > nDecalSortMaxVerts ) || ( nIndexCount + ( nCount - 2 ) > nDecalSortMaxIndices ) )
		{
			// Finish this batch.
			if ( pBatch )
			{
				pBatch->m_nIndexCount = ( nIndexCount - pBatch->m_iStartIndex );
			}

			// End the mesh building phase and render.
			meshBuilder.End();
			R_DrawDecalMeshList( meshList );

			// Reset.
			bMeshInit = true;
			pBatch = NULL;
			bBatchInit = true;
		}

		// Create the mesh.
		if ( bMeshInit )
		{
			// Reset the mesh list.
			meshList.m_pMesh = NULL;
			meshList.m_aBatches.RemoveAll();

			// Create a mesh for this vertex format (vertex format per SortTree).
			if ( ShouldDrawInWireFrameMode() )
			{
				meshList.m_pMesh = pRenderContext->GetDynamicMesh( false, NULL, NULL, g_materialDecalWireframe );
			}
			else
			{
				meshList.m_pMesh = pRenderContext->GetDynamicMesh( false, NULL, NULL, pDecalHead->material );
			}
			meshBuilder.Begin( meshList.m_pMesh, MATERIAL_TRIANGLES, nDecalSortMaxVerts, nDecalSortMaxIndices );

			nVertCount = 0;
			nIndexCount = 0;

			bMeshInit = false;
		}

		// Create the batch.
		if ( bBatchInit )
		{
			// Create a batch for this bucket = material/lightmap pair.
			// Todo: we also could flush it right here and continue.
			if ( meshList.m_aBatches.Size() + 1 > meshList.m_aBatches.NumAllocated() )
			{
				Warning( "R_DrawDecalsAll: overflowing m_aBatches. Reduce # of decals in the scene.\n", nDecalSortMaxVerts * meshList.m_aBatches.NumAllocated() );
				meshBuilder.End();
				R_DrawDecalMeshList( meshList );
				return;
			}

			int iBatchList = meshList.m_aBatches.AddToTail();
			pBatch = &meshList.m_aBatches[iBatchList];
			pBatch->m_iStartIndex = nIndexCount;

			if ( ShouldDrawInWireFrameMode() )
			{
				pBatch->m_pMaterial = g_materialDecalWireframe;
			}
			else
			{
				pBatch->m_pMaterial = pDecalHead->material;
				pBatch->m_pProxy = pDecalHead->userdata;
				pBatch->m_iLightmapPage = materialSortInfoArray[MSurf_MaterialSortID( pDecalHead->surfID )].lightmapPageID;
			}

			bBatchInit = false;
		}
		Assert ( pBatch );

		// Set base color.
		byte color[4] = { pDecal->color.r, pDecal->color.g, pDecal->color.b, pDecal->color.a };

		// Dynamic decals - fading.
		if ( pDecal->flags & FDECAL_DYNAMIC )
		{
			float flFadeValue;

			// Negative fadeDuration value means to fade in
			if ( pDecal->fadeDuration < 0 )
			{
				flFadeValue = -( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
			}
			else
			{
				flFadeValue = 1.0 - ( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
			}

			flFadeValue = clamp( flFadeValue, 0.0f, 1.0f );

			color[3] = ( byte )( 255 * flFadeValue );
		}

		// Compute normal and tangent space if necessary.
		Vector vecNormal( 0.0f, 0.0f, 1.0f ), vecTangentS( 1.0f, 0.0f, 0.0f ), vecTangentT( 0.0f, 1.0f, 0.0f );
		if ( vertexFormat & ( VERTEX_NORMAL | VERTEX_TANGENT_SPACE ) )
		{
			vecNormal = MSurf_Plane( pDecal->surfID ).normal;
			if ( vertexFormat & VERTEX_TANGENT_SPACE )
			{
				Vector tVect;
				bool bNegate = TangentSpaceSurfaceSetup( pDecal->surfID, tVect );
				TangentSpaceComputeBasis( vecTangentS, vecTangentT, vecNormal, tVect, bNegate );
			}
		}

		// Setup verts.
		float flOffset = ComputeDecalLightmapOffset( pDecal->surfID );
		for ( int iVert = 0; iVert < nCount; ++iVert, ++pVerts )
		{
			meshBuilder.Position3fv( pVerts->m_vPos.Base() );
			if ( vertexFormat & VERTEX_NORMAL )
			{
				meshBuilder.Normal3fv( vecNormal.Base() );
			}
			meshBuilder.Color4ubv( color );
			if ( pDecal->material->InMaterialPage() )
			{
				float offset[2], scale[2];
				pDecal->material->GetMaterialOffset( offset );
				pDecal->material->GetMaterialScale( scale );
				meshBuilder.TexCoordSubRect2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y, offset[0], offset[1], scale[0], scale[1] );
			}
			else
			{
				meshBuilder.TexCoord2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y  );
			}
			meshBuilder.TexCoord2f( 1, pVerts->m_cLMCoords.x, pVerts->m_cLMCoords.y );
			meshBuilder.TexCoord1f( 2, flOffset );
			if ( vertexFormat & VERTEX_TANGENT_SPACE )
			{
				meshBuilder.TangentS3fv( vecTangentS.Base() ); 
				meshBuilder.TangentT3fv( vecTangentT.Base() ); 
			}

			meshBuilder.AdvanceVertex();
		}

		// Setup indices.
		int nTriCount = ( nCount - 2 );
		for ( int iTri = 0; iTri < nTriCount; ++iTri )
		{
			meshBuilder.FastIndex( nVertCount );
			meshBuilder.FastIndex( nVertCount + iTri + 1 );
			meshBuilder.FastIndex( nVertCount + iTri + 2 );
		}

		// Update counters.
		nVertCount += nCount;
		nIndexCount += ( nTriCount * 3 );
	}

	if ( pBatch )
	{
		pBatch->m_nIndexCount = ( nIndexCount - pBatch->m_iStartIndex );
	}

	if ( !bMeshInit )
	{
		meshBuilder.End();
		R_DrawDecalMeshList( meshList );
	}
}


void R_DrawDecalsAll( IMatRenderContext *pRenderContext, int iGroup, int iTreeType, const Vector &vModelOrg, int nCheckCount )
{
	DecalMeshList_t		meshList;
	CMeshBuilder		meshBuilder;

	int nVertCount = 0;
	int nIndexCount = 0;

	int nDecalSortMaxVerts = g_nMaxDecals * 5;
	int nDecalSortMaxIndices = nDecalSortMaxVerts * 3;

	int nSortTreeCount = g_aDecalSortTrees.Count();
	for ( int iSortTree = 0; iSortTree < nSortTreeCount; ++iSortTree )
	{		
		// Reset the mesh list.
		bool bMeshInit = true;

		DecalSortTrees_t &sortTree = g_aDecalSortTrees[iSortTree];
		int nBucketCount = sortTree.m_aDecalSortBuckets[iGroup][iTreeType].Count();
		for ( int iBucket = 0; iBucket < nBucketCount; ++iBucket )
		{
			DecalMaterialBucket_t &bucket = sortTree.m_aDecalSortBuckets[iGroup][iTreeType].Element( iBucket );
			if ( bucket.m_nCheckCount != nCheckCount )
				continue;
			
			int iHead = bucket.m_iHead;
			if ( !g_aDecalSortPool.IsValidIndex( iHead ) )
				continue;

			decal_t *pDecalHead = g_aDecalSortPool.Element( iHead );
			Assert( pDecalHead->material );
			if ( !pDecalHead->material )
				continue;

			// Vertex format.
			VertexFormat_t vertexFormat = GetUncompressedFormat( pDecalHead->material );
			if ( vertexFormat == 0 )
				continue;

			// New bucket = new batch.
			DecalBatchList_t *pBatch = NULL;
			bool bBatchInit = true;
			
			int nCount;
			int iElement = iHead;
			while ( iElement != g_aDecalSortPool.InvalidIndex() )
			{
				decal_t *pDecal = g_aDecalSortPool.Element( iElement );
				iElement = g_aDecalSortPool.Next( iElement );

				if ( !pDecal )
					continue;

				// Add the decal to the list of decals to be destroyed if need be.
				if ( ( pDecal->flags & FDECAL_DYNAMIC ) && !( pDecal->flags & FDECAL_HASUPDATED ) )
				{
					pDecal->flags |= FDECAL_HASUPDATED;
					if ( DecalUpdate( pDecal ) )
					{
						R_DecalAddToDestroyList( pDecal );
						continue;
					}
				}

				CDecalVert *pVerts = R_DecalSetupVerts( pRenderContext, pDecal, pDecal->surfID, pDecal->material, nCount, vModelOrg );
				if ( nCount == 0 )
					continue;
				
				// Overflow - new mesh, batch.
				if ( ( ( nVertCount + nCount ) > nDecalSortMaxVerts ) || ( nIndexCount + ( nCount - 2 ) > nDecalSortMaxIndices ) )
				{
					// Finish this batch.
					if ( pBatch )
					{
						pBatch->m_nIndexCount = ( nIndexCount - pBatch->m_iStartIndex );
					}

					// End the mesh building phase and render.
					meshBuilder.End();
					R_DrawDecalMeshList( meshList );

					// Reset.
					bMeshInit = true;
					pBatch = NULL;
					bBatchInit = true;
				}
				
				// Create the mesh.
				if ( bMeshInit )
				{
					// Reset the mesh list.
					meshList.m_pMesh = NULL;
					meshList.m_aBatches.RemoveAll();

					// Create a mesh for this vertex format (vertex format per SortTree).
					if ( ShouldDrawInWireFrameMode() )
					{
						meshList.m_pMesh = pRenderContext->GetDynamicMesh( false, NULL, NULL, g_materialDecalWireframe );
					}
					else
					{
						meshList.m_pMesh = pRenderContext->GetDynamicMesh( false, NULL, NULL, pDecalHead->material );
					}
					meshBuilder.Begin( meshList.m_pMesh, MATERIAL_TRIANGLES, nDecalSortMaxVerts, nDecalSortMaxIndices );
					
					nVertCount = 0;
					nIndexCount = 0;
					
					bMeshInit = false;
				}
				
				// Create the batch.
				if ( bBatchInit )
				{
					// Create a batch for this bucket = material/lightmap pair.
					// Todo: we also could flush it right here and continue.
					if ( meshList.m_aBatches.Size() + 1 > meshList.m_aBatches.NumAllocated() )
					{
						Warning( "R_DrawDecalsAll: overflowing m_aBatches. Reduce # of decals in the scene.\n", nDecalSortMaxVerts * meshList.m_aBatches.NumAllocated() );
						meshBuilder.End();
						R_DrawDecalMeshList( meshList );
						return;
					}

					int iBatchList = meshList.m_aBatches.AddToTail();
					pBatch = &meshList.m_aBatches[iBatchList];
					pBatch->m_iStartIndex = nIndexCount;
					
					if ( ShouldDrawInWireFrameMode() )
					{
						pBatch->m_pMaterial = g_materialDecalWireframe;
					}
					else
					{
						pBatch->m_pMaterial = pDecalHead->material;
						pBatch->m_pProxy = pDecalHead->userdata;
						pBatch->m_iLightmapPage = materialSortInfoArray[MSurf_MaterialSortID( pDecalHead->surfID )].lightmapPageID;
					}
											
					bBatchInit = false;
				}
				Assert ( pBatch );
				
				// Set base color.
				byte color[4] = { pDecal->color.r, pDecal->color.g, pDecal->color.b, pDecal->color.a };
				
				// Dynamic decals - fading.
				if ( pDecal->flags & FDECAL_DYNAMIC )
				{
					float flFadeValue;
					
					// Negative fadeDuration value means to fade in
					if ( pDecal->fadeDuration < 0 )
					{
						flFadeValue = -( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
					}
					else
					{
						flFadeValue = 1.0 - ( cl.GetTime() - pDecal->fadeStartTime ) / pDecal->fadeDuration;
					}
					
					flFadeValue = clamp( flFadeValue, 0.0f, 1.0f );
					
					color[3] = ( byte )( 255 * flFadeValue );
				}
				
				// Compute normal and tangent space if necessary.
				Vector vecNormal( 0.0f, 0.0f, 1.0f ), vecTangentS( 1.0f, 0.0f, 0.0f ), vecTangentT( 0.0f, 1.0f, 0.0f );
				if ( vertexFormat & ( VERTEX_NORMAL | VERTEX_TANGENT_SPACE ) )
				{
					vecNormal = MSurf_Plane( pDecal->surfID ).normal;
					if ( vertexFormat & VERTEX_TANGENT_SPACE )
					{
						Vector tVect;
						bool bNegate = TangentSpaceSurfaceSetup( pDecal->surfID, tVect );
						TangentSpaceComputeBasis( vecTangentS, vecTangentT, vecNormal, tVect, bNegate );
					}
				}
				
				// Setup verts.
				float flOffset = ComputeDecalLightmapOffset( pDecal->surfID );
				for ( int iVert = 0; iVert < nCount; ++iVert, ++pVerts )
				{
					meshBuilder.Position3fv( pVerts->m_vPos.Base() );
					if ( vertexFormat & VERTEX_NORMAL )
					{
						meshBuilder.Normal3fv( vecNormal.Base() );
					}
					meshBuilder.Color4ubv( color );
					if ( pDecal->material->InMaterialPage() )
					{
						float offset[2], scale[2];
						pDecal->material->GetMaterialOffset( offset );
						pDecal->material->GetMaterialScale( scale );
						meshBuilder.TexCoordSubRect2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y, offset[0], offset[1], scale[0], scale[1] );
					}
					else
					{
						meshBuilder.TexCoord2f( 0, pVerts->m_ctCoords.x, pVerts->m_ctCoords.y  );
					}
					meshBuilder.TexCoord2f( 1, pVerts->m_cLMCoords.x, pVerts->m_cLMCoords.y );
					meshBuilder.TexCoord1f( 2, flOffset );
					if ( vertexFormat & VERTEX_TANGENT_SPACE )
					{
						meshBuilder.TangentS3fv( vecTangentS.Base() ); 
						meshBuilder.TangentT3fv( vecTangentT.Base() ); 
					}
					
					meshBuilder.AdvanceVertex();
				}
				
				// Setup indices.
				int nTriCount = ( nCount - 2 );
				for ( int iTri = 0; iTri < nTriCount; ++iTri )
				{
					meshBuilder.FastIndex( nVertCount );
					meshBuilder.FastIndex( nVertCount + iTri + 1 );
					meshBuilder.FastIndex( nVertCount + iTri + 2 );
				}
				
				// Update counters.
				nVertCount += nCount;
				nIndexCount += ( nTriCount * 3 );
			}
			
			if ( pBatch )
			{
				pBatch->m_nIndexCount = ( nIndexCount - pBatch->m_iStartIndex );
			}
		}
		
		if ( !bMeshInit )
		{
			meshBuilder.End();
			R_DrawDecalMeshList( meshList );
		}
	}	
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void DecalSurfaceDraw_NonQueued( IMatRenderContext *pRenderContext, int renderGroup, const Vector &vModelOrg, int nCheckCount )
{
	if ( r_drawbatchdecals.GetBool() )
	{
		// Draw world decals.
		R_DrawDecalsAll( pRenderContext, renderGroup, PERMANENT_LIGHTMAP, vModelOrg, nCheckCount );
		
		// Draw lightmapped non-world decals.
		R_DrawDecalsAll( pRenderContext, renderGroup, LIGHTMAP, vModelOrg, nCheckCount );
		
		// Draw non-lit(mod2x) decals.
		R_DrawDecalsAll( pRenderContext, renderGroup, NONLIGHTMAP, vModelOrg, nCheckCount );
	}
	else
	{
		// Draw world decals.
		R_DrawDecalsAllImmediate( pRenderContext, renderGroup, PERMANENT_LIGHTMAP, vModelOrg, nCheckCount );
		
		// Draw lightmapped non-world decals.
		R_DrawDecalsAllImmediate( pRenderContext, renderGroup, LIGHTMAP, vModelOrg, nCheckCount );

		// Draw non-lit(mod2x) decals.
		R_DrawDecalsAllImmediate( pRenderContext, renderGroup, NONLIGHTMAP, vModelOrg, nCheckCount );
	}
}

void DecalSurfaceDraw_QueueHelper( bool bBatched, int renderGroup, Vector vModelOrg, int nCheckCount, decal_t **ppDecals, int iPermanentLightmap, int iLightmap, int iNonLightmap )
{
	CMatRenderContextPtr pRenderContext( materials );

	if( bBatched )
	{
		R_DrawDecalsAll_Gathered( pRenderContext, ppDecals, iPermanentLightmap, vModelOrg );
		ppDecals += iPermanentLightmap;
		R_DrawDecalsAll_Gathered( pRenderContext, ppDecals, iLightmap, vModelOrg );
		ppDecals += iLightmap;
		R_DrawDecalsAll_Gathered( pRenderContext, ppDecals, iNonLightmap, vModelOrg );
	}
	else
	{
		R_DrawDecalsAllImmediate_Gathered( pRenderContext, ppDecals, iPermanentLightmap, vModelOrg );
		ppDecals += iPermanentLightmap;
		R_DrawDecalsAllImmediate_Gathered( pRenderContext, ppDecals, iLightmap, vModelOrg );
		ppDecals += iLightmap;
		R_DrawDecalsAllImmediate_Gathered( pRenderContext, ppDecals, iNonLightmap, vModelOrg );
	}
}

class CQueuedDecalMemoryManager
{
public:
	CQueuedDecalMemoryManager( void )
	{
		m_nCurrentStack = 0;
		MEM_ALLOC_CREDIT();
		m_QueuedDecalMemory[0].Init( 65536, 0, 16384 );
		m_QueuedDecalMemory[1].Init( 65536, 0, 16384 );
	}
	~CQueuedDecalMemoryManager( void )
	{
		m_QueuedDecalMemory[0].FreeAll( true );
		m_QueuedDecalMemory[1].FreeAll( true );
		for( int i = 0; i != 2; ++i )
		{
			for( int j = m_DeleteOnSwitch[i].Count(); --j >= 0; )
			{
				delete []m_DeleteOnSwitch[i].Element(j);
			}

			m_DeleteOnSwitch[i].RemoveAll();
		}		
	}

	void SwitchStack( void )
	{
		m_nCurrentStack = 1 - m_nCurrentStack;
		m_QueuedDecalMemory[m_nCurrentStack].FreeAll( false );

		for( int i = m_DeleteOnSwitch[m_nCurrentStack].Count(); --i >= 0; )
		{
			delete []m_DeleteOnSwitch[m_nCurrentStack].Element(i);
		}
		m_DeleteOnSwitch[m_nCurrentStack].RemoveAll();
	}

	inline void *Alloc( size_t bytes )
	{
		MEM_ALLOC_CREDIT();
		void *pReturn = m_QueuedDecalMemory[m_nCurrentStack].Alloc( bytes, false );
		if( pReturn == NULL )
		{
			int iMaxSize = m_QueuedDecalMemory[m_nCurrentStack].GetMaxSize();
			Warning( "Overflowed decal queued rendering memory stack. Needed %d, have %d/%d\n", bytes, iMaxSize - m_QueuedDecalMemory[m_nCurrentStack].GetUsed(), iMaxSize );
			pReturn = new uint8 * [bytes];
			m_DeleteOnSwitch[m_nCurrentStack].AddToTail( pReturn );
		}
		return pReturn;
	}

	CMemoryStack	m_QueuedDecalMemory[2];
	int				m_nCurrentStack;
	CUtlVector<void *>	m_DeleteOnSwitch[2]; //when we overflow the stack, we do new/delete
};
static CQueuedDecalMemoryManager s_QDMM;


void DecalBeginFrame( void )
{
	s_QDMM.SwitchStack();
}

void DecalSurfaceDraw( IMatRenderContext *pRenderContext, int renderGroup )
{
	//	VPROF_BUDGET( "Decals", "Decals" );
	VPROF( "DecalsDraw" );

	if( !r_drawdecals.GetBool() )
	{
		return;
	}

	int nCheckCount = g_nDecalSortCheckCount;
	if ( renderGroup == MAX_MAT_SORT_GROUPS )
	{
		// Brush Model
		nCheckCount = g_nBrushModelDecalSortCheckCount;
	}

	ICallQueue *pCallQueue;
	if( r_queued_decals.GetBool() && (pCallQueue = pRenderContext->GetCallQueue()) != NULL )
	{
		//queue available and desired
		bool bBatched = r_drawbatchdecals.GetBool();
		static CUtlVector<decal_t *> DrawDecals;

		int iPermanentLightmap, iLightmap, iNonLightmap;
		if( bBatched )
		{
			R_DrawDecalsAll_GatherDecals( pRenderContext, renderGroup, PERMANENT_LIGHTMAP, DrawDecals );
			iPermanentLightmap = DrawDecals.Count();
			R_DrawDecalsAll_GatherDecals( pRenderContext, renderGroup, LIGHTMAP, DrawDecals );
			iLightmap = DrawDecals.Count() - iPermanentLightmap;
			R_DrawDecalsAll_GatherDecals( pRenderContext, renderGroup, NONLIGHTMAP, DrawDecals );
			iNonLightmap = DrawDecals.Count() - (iPermanentLightmap + iLightmap);
		}
		else
		{
			R_DrawDecalsAllImmediate_GatherDecals( pRenderContext, renderGroup, PERMANENT_LIGHTMAP, DrawDecals );
			iPermanentLightmap = DrawDecals.Count();
			R_DrawDecalsAllImmediate_GatherDecals( pRenderContext, renderGroup, LIGHTMAP, DrawDecals );
			iLightmap = DrawDecals.Count() - iPermanentLightmap;
			R_DrawDecalsAllImmediate_GatherDecals( pRenderContext, renderGroup, NONLIGHTMAP, DrawDecals );
			iNonLightmap = DrawDecals.Count() - (iPermanentLightmap + iLightmap);
		}

		if( DrawDecals.Count() )
		{
			decal_t **ppDecals = (decal_t **)s_QDMM.Alloc( DrawDecals.Count() * sizeof( decal_t * ) );
			memcpy( ppDecals, DrawDecals.Base(), DrawDecals.Count() * sizeof( decal_t * ) );
			pCallQueue->QueueCall( DecalSurfaceDraw_QueueHelper, bBatched, renderGroup, modelorg, nCheckCount, ppDecals, iPermanentLightmap, iLightmap, iNonLightmap );
			
			DrawDecals.RemoveAll();
		}
	}
	else
	{
		//non-queued mode
		DecalSurfaceDraw_NonQueued( pRenderContext, renderGroup, modelorg, nCheckCount );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Add decals to sorted decal list.
//-----------------------------------------------------------------------------
void DecalSurfaceAdd( SurfaceHandle_t surfID, int iGroup )
{
	// Performance analysis.
//	VPROF_BUDGET( "Decals", "Decals" );
	VPROF( "DecalsBatch" );
	
	// Go through surfaces decal list and add them to the correct lists.
	decal_t *pDecalList = MSurf_DecalPointer( surfID );
	if ( !pDecalList )
		return;

	int nCheckCount = g_nDecalSortCheckCount;
	if ( iGroup == MAX_MAT_SORT_GROUPS )
	{
		// Brush Model
		nCheckCount = g_nBrushModelDecalSortCheckCount;
	}

	int iTreeType = -1;
	decal_t *pNext = NULL;
	for ( decal_t *pDecal = pDecalList; pDecal; pDecal = pNext )
	{
		// Get the next pointer.
		pNext = pDecal->pnext;

		// Lightmap decals.
		if ( pDecal->material->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_LIGHTMAP ) )
		{
			// Permanent lightmapped decals.
			if ( pDecal->flags & FDECAL_PERMANENT )
			{
				iTreeType = PERMANENT_LIGHTMAP;
			}
			// Non-permanent lightmapped decals.
			else
			{
				iTreeType = LIGHTMAP;
			}
		}
		// Non-lightmapped decals.
		else
		{
			iTreeType = NONLIGHTMAP;
		}

		pDecal->flags &= ~FDECAL_HASUPDATED;
		int iPool = g_aDecalSortPool.Alloc( true );
		if ( iPool != g_aDecalSortPool.InvalidIndex() )
		{
			g_aDecalSortPool[iPool] = pDecal;
						
			DecalSortTrees_t &sortTree = g_aDecalSortTrees[ pDecal->m_iSortTree ];
			DecalMaterialBucket_t &bucket = sortTree.m_aDecalSortBuckets[iGroup][iTreeType].Element( pDecal->m_iSortMaterial );
			if ( bucket.m_nCheckCount == nCheckCount )
			{	
				int iHead = bucket.m_iHead;
				g_aDecalSortPool.LinkBefore( iHead, iPool );
			}
			
			bucket.m_iHead = iPool;
			bucket.m_nCheckCount = nCheckCount;			
		}
	}	
}

#pragma check_stack(off)
#endif // SWDS
