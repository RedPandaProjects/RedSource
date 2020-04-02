//========== Copyright � 2005, Valve Corporation, All rights reserved. ========
//
// Purpose:
//
//=============================================================================

#ifndef OCCLUSIONQUERY_H
#define OCCLUSIONQUERY_H

#include "tier1/utlsymbol.h"
#include "tier1/utlrbtree.h"

#ifndef MATSYS_INTERNAL
#error "This file is private to the implementation of IMaterialSystem/IMaterialSystemInternal"
#endif

#if defined( _WIN32 )
#pragma once
#endif


#include "tier1/utllinkedlist.h"
#include "shaderapi/ishaderapi.h"


class IMatRenderContextInternal;


//-----------------------------------------------------------------------------
// Dictionary of all known materials
//-----------------------------------------------------------------------------
class COcclusionQueryMgr
{
public:
	COcclusionQueryMgr();

	// Allocate and delete query objects.
	OcclusionQueryObjectHandle_t CreateOcclusionQueryObject( );
	void OnCreateOcclusionQueryObject( OcclusionQueryObjectHandle_t h );
	void DestroyOcclusionQueryObject( OcclusionQueryObjectHandle_t h );

	// Bracket drawing with begin and end so that we can get counts next frame.
	void BeginOcclusionQueryDrawing( OcclusionQueryObjectHandle_t h );
	void EndOcclusionQueryDrawing( OcclusionQueryObjectHandle_t h );

	// Used to make the handle think it's never had a successful query before
	void ResetOcclusionQueryObject( OcclusionQueryObjectHandle_t );

	// Get the number of pixels rendered between begin and end on an earlier frame.
	// Calling this in the same frame is a huge perf hit!
	int OcclusionQuery_GetNumPixelsRendered( OcclusionQueryObjectHandle_t h, bool bDoQuery );
	void OcclusionQuery_IssueNumPixelsRenderedQuery( OcclusionQueryObjectHandle_t h );

	// Internal stuff for occlusion query
	void AllocOcclusionQueryObjects( void );
	void FreeOcclusionQueryObjects( void );

	// Advance frame
	void AdvanceFrame();

private:
	//-----------------------------------------------------------------------------
	// Occlusion query objects
	//-----------------------------------------------------------------------------
	struct OcclusionQueryObject_t 
	{
		ShaderAPIOcclusionQuery_t m_QueryHandle;
		int m_LastResult;
		int m_nFrameIssued;
		bool m_bHasBeenIssued;

		OcclusionQueryObject_t(void)
		{
			m_QueryHandle = INVALID_SHADERAPI_OCCLUSION_QUERY_HANDLE;
			m_LastResult = -1;
			m_nFrameIssued = -1;
			m_bHasBeenIssued = false;
		}
	};

	// Flushes an outstanding query
	void FlushQuery( OcclusionQueryObjectHandle_t hOcclusionQuery );

	// Occlusion query objects
	CUtlFixedLinkedList<OcclusionQueryObject_t>	m_OcclusionQueryObjects;
	CThreadFastMutex m_Mutex;
	int m_nFrameCount;
};


//-----------------------------------------------------------------------------
// Singleton
//-----------------------------------------------------------------------------
extern COcclusionQueryMgr *g_pOcclusionQueryMgr;


#endif // OCCLUSIONQUERY_H
