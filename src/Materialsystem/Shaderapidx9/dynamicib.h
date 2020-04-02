//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
//===========================================================================//

#ifndef DYNAMICIB_H
#define DYNAMICIB_H

#ifdef _WIN32
#pragma once
#endif

#include "locald3dtypes.h"
#include "Recording.h"
#include "ShaderAPIDX8_Global.h"
#include "shaderapi/IShaderUtil.h"

/////////////////////////////
// D. Sim Dietrich Jr.
// sim.dietrich@nvidia.com
/////////////////////////////

#ifdef _WIN32
#pragma warning (disable:4189)
#endif

#include "locald3dtypes.h"
#include "tier1/strtools.h"
#include "tier1/utlqueue.h"
#include "tier0/memdbgon.h"

// Helper function to unbind an index buffer
void Unbind( IDirect3DIndexBuffer9 *pIndexBuffer );

#define X360_INDEX_BUFFER_SIZE_MULTIPLIER 3.0 //minimum of 1, only affects dynamic buffers
//#define X360_BLOCK_ON_IB_FLUSH //uncomment to block until all data is consumed when a flush is requested. Otherwise we only block when absolutely necessary

#define SPEW_INDEX_BUFFER_STALLS //uncomment to allow buffer stall spewing.


class CIndexBuffer
{
public:
	CIndexBuffer( D3DDeviceWrapper *pD3D, int count, bool bSoftwareVertexProcessing, bool dynamic = false );
	~CIndexBuffer();
	
	LPDIRECT3DINDEXBUFFER GetInterface() const { return m_pIB; }
	
	// Use at beginning of frame to force a flush of VB contents on first draw
	void FlushAtFrameStart() { m_bFlush = true; }
	
	// lock, unlock
	unsigned short *Lock( bool bReadOnly, int numIndices, int &startIndex, int startPosition = -1 );	
	void Unlock( int numIndices );

	// Index position
	int IndexPosition() const { return m_Position; }
	
	// Index size
	int IndexSize() const { return sizeof(unsigned short); }

	// Index count
	int IndexCount() const { return m_IndexCount; }

	// Do we have enough room without discarding?
	bool HasEnoughRoom( int numIndices ) const;

	bool IsDynamic() const { return m_bDynamic; }

	// Block until there's a free portion of the buffer of this size, m_Position will be updated to point at where this section starts
	void BlockUntilUnused( int nAllocationSize );

#ifdef CHECK_INDICES
	void UpdateShadowIndices( unsigned short *pData )
	{
		Assert( m_LockedStartIndex + m_LockedNumIndices <= m_NumIndices );
		memcpy( m_pShadowIndices + m_LockedStartIndex, pData, m_LockedNumIndices * IndexSize() );
	}

	unsigned short GetShadowIndex( int i )
	{
		Assert( i >= 0 && i < (int)m_NumIndices );
		return m_pShadowIndices[i];
	}
#endif

	// UID
	unsigned int UID() const 
	{ 
#ifdef RECORDING
		return m_UID; 
#else
		return 0;
#endif
	}

	void HandlePerFrameTextureStats( int frame )
	{
#ifdef VPROF_ENABLED
		if ( m_Frame != frame && !m_bDynamic )
		{
			m_Frame = frame;
			VPROF_INCREMENT_GROUP_COUNTER( "TexGroup_frame_" TEXTURE_GROUP_STATIC_INDEX_BUFFER, 
				COUNTER_GROUP_TEXTURE_PER_FRAME, IndexCount() * IndexSize() );
		}
#endif
	}
	
	static int BufferCount()
	{
#ifdef _DEBUG
		return s_BufferCount;
#else
		return 0;
#endif
	}

	// Marks a fence indicating when this buffer was used
	void MarkUsedInRendering()
	{
#ifdef _X360
		if ( m_bDynamic && m_pIB )
		{
			Assert( m_AllocationRing.Count() > 0 );
			m_AllocationRing[m_AllocationRing.Tail()].m_Fence = Dx9Device()->GetCurrentFence();
		}
#endif
	}

private :
	enum LOCK_FLAGS
	{
		LOCKFLAGS_FLUSH  = D3DLOCK_NOSYSLOCK | D3DLOCK_DISCARD,
#if !defined( _X360 )
		LOCKFLAGS_APPEND = D3DLOCK_NOSYSLOCK | D3DLOCK_NOOVERWRITE
#else
		// X360BUG: forcing all locks to gpu flush, otherwise bizarre mesh corruption on decals
		// Currently iterating with microsoft 360 support to track source of gpu corruption
		LOCKFLAGS_APPEND = D3DLOCK_NOSYSLOCK
#endif
	};

	LPDIRECT3DINDEXBUFFER m_pIB;
#ifdef _X360
	
	struct DynamicBufferAllocation_t
	{
		DWORD	m_Fence; //track whether this memory is safe to use again.
		int	m_iStartOffset;
		int	m_iEndOffset;
	};

	int						m_iNextBlockingPosition; // m_iNextBlockingPosition >= m_Position where another allocation is still in use.
	unsigned char			*m_pAllocatedMemory;
	int						m_iAllocationCount; //The total number of indices the buffer we allocated can hold. Usually greater than the number of indices asked for
	IDirect3DIndexBuffer9	m_D3DIndexBuffer; //Only need one shared D3D header for our usage patterns.
	CUtlLinkedList<DynamicBufferAllocation_t> m_AllocationRing; //tracks what chunks of our memory are potentially still in use by D3D

#endif

	int			m_IndexCount;
	int			m_Position;
	unsigned char	m_bLocked : 1;
	unsigned char	m_bFlush : 1;
	unsigned char	m_bDynamic : 1;

#ifdef VPROF_ENABLED
	int				m_Frame;
#endif

#ifdef _DEBUG
	static int		s_BufferCount;
#endif

#ifdef RECORDING
	unsigned int	m_UID;
#endif

#if !defined( _X360 )
	LockedBufferContext m_LockData;
#endif

protected:
#ifdef CHECK_INDICES
	unsigned short *m_pShadowIndices;
	unsigned int m_NumIndices;
	unsigned int m_LockedStartIndex;
	unsigned int m_LockedNumIndices;
#endif
};

 
#ifdef _DEBUG
int CIndexBuffer::s_BufferCount = 0;
#endif

#if defined( _X360 )
#include "UtlMap.h"
MEMALLOC_DEFINE_EXTERNAL_TRACKING( XMem_CIndexBuffer );
#endif

//-----------------------------------------------------------------------------
// constructor, destructor
//-----------------------------------------------------------------------------

CIndexBuffer::CIndexBuffer( D3DDeviceWrapper *pD3D, int count, 
	bool bSoftwareVertexProcessing, bool dynamic ) :
		m_pIB(0), 
		m_Position(0), 
		m_bFlush(true), 
		m_bLocked(false),
		m_bDynamic(dynamic)
#ifdef VPROF_ENABLED
		,m_Frame( -1 )
#endif
{
	// For write-combining, ensure we always have locked memory aligned to 4-byte boundaries
	count = ALIGN_VALUE( count, 2 );
	m_IndexCount = count; 

	MEM_ALLOC_D3D_CREDIT();

#ifdef CHECK_INDICES
	m_pShadowIndices = NULL;
#endif

#ifdef RECORDING
	// assign a UID
	static unsigned int uid = 0;
	m_UID = uid++;
#endif

#ifdef _DEBUG
	++s_BufferCount;
#endif

	D3DINDEXBUFFER_DESC desc;
	memset( &desc, 0x00, sizeof( desc ) );
	desc.Format = D3DFMT_INDEX16;
	desc.Size = sizeof(unsigned short) * count;
	desc.Type = D3DRTYPE_INDEXBUFFER;
	desc.Pool = D3DPOOL_DEFAULT;
	desc.Usage = D3DUSAGE_WRITEONLY;
	if ( m_bDynamic )
	{
		desc.Usage |= D3DUSAGE_DYNAMIC;
	}
	if ( bSoftwareVertexProcessing )
	{
		desc.Usage |= D3DUSAGE_SOFTWAREPROCESSING;
	}

	RECORD_COMMAND( DX8_CREATE_INDEX_BUFFER, 6 );
	RECORD_INT( m_UID );
	RECORD_INT( count * IndexSize() );
	RECORD_INT( desc.Usage );
	RECORD_INT( desc.Format );
	RECORD_INT( desc.Pool );
	RECORD_INT( m_bDynamic );

#ifdef CHECK_INDICES
	Assert( desc.Format == D3DFMT_INDEX16 );
	m_pShadowIndices = new unsigned short[count];
	m_NumIndices = count;
#endif

#if !defined( _X360 )
	HRESULT hr = pD3D->CreateIndexBuffer( 
					count * IndexSize(),
					desc.Usage,
					desc.Format,
					desc.Pool, 
					&m_pIB, 
					NULL );
	if ( hr != D3D_OK )
	{
		Warning( "CreateIndexBuffer failed!\n" );
	}

	if ( ( hr == D3DERR_OUTOFVIDEOMEMORY ) || ( hr == E_OUTOFMEMORY ) )
	{
		// Don't have the memory for this.  Try flushing all managed resources
		// out of vid mem and try again.
		// FIXME: need to record this
		pD3D->EvictManagedResources();
		hr = pD3D->CreateIndexBuffer( count * IndexSize(),
			desc.Usage, desc.Format, desc.Pool, &m_pIB, NULL );
	}

	Assert( m_pIB );
	Assert( hr == D3D_OK );

#ifdef MEASURE_DRIVER_ALLOCATIONS
	int nMemUsed = 1024;
	VPROF_INCREMENT_GROUP_COUNTER( "ib count", COUNTER_GROUP_NO_RESET, 1 );
	VPROF_INCREMENT_GROUP_COUNTER( "ib driver mem", COUNTER_GROUP_NO_RESET, nMemUsed );
	VPROF_INCREMENT_GROUP_COUNTER( "total driver mem", COUNTER_GROUP_NO_RESET, nMemUsed );
#endif

#if defined( _DEBUG )
	if ( IsPC() && m_pIB )
	{
		D3DINDEXBUFFER_DESC aDesc;
		m_pIB->GetDesc( &aDesc );
		Assert( memcmp( &aDesc, &desc, sizeof( desc ) ) == 0 );
	}
#endif

#else
	// _X360
	int nBufferSize = (count * IndexSize());
	if ( m_bDynamic )
	{
		m_iAllocationCount = count * X360_INDEX_BUFFER_SIZE_MULTIPLIER;
		Assert( m_iAllocationCount >= count );
		m_iAllocationCount = ALIGN_VALUE( m_iAllocationCount, 2 );
		m_pAllocatedMemory = (unsigned char*)XPhysicalAlloc( m_iAllocationCount * IndexSize(), MAXULONG_PTR, 0, PAGE_READWRITE | MEM_LARGE_PAGES | PAGE_WRITECOMBINE );
	}
	else
	{
		m_iAllocationCount = count;
		m_pAllocatedMemory = (unsigned char*)XPhysicalAlloc( nBufferSize, MAXULONG_PTR, 0, PAGE_READWRITE | PAGE_WRITECOMBINE ); //
	}

	if ( m_pAllocatedMemory )
	{
		MemAlloc_RegisterExternalAllocation( XMem_CIndexBuffer, m_pAllocatedMemory, XPhysicalSize( m_pAllocatedMemory ) );
	}

	m_iNextBlockingPosition = m_iAllocationCount;
#endif


#ifdef VPROF_ENABLED
	if ( !m_bDynamic )
	{
		VPROF_INCREMENT_GROUP_COUNTER( "TexGroup_global_" TEXTURE_GROUP_STATIC_INDEX_BUFFER, 
			COUNTER_GROUP_TEXTURE_GLOBAL, IndexCount() * IndexSize() );
	}
	else if ( IsX360() )
	{
		VPROF_INCREMENT_GROUP_COUNTER( "TexGroup_global_" TEXTURE_GROUP_DYNAMIC_INDEX_BUFFER, 
			COUNTER_GROUP_TEXTURE_GLOBAL, IndexCount() * IndexSize() );
	}
#endif
}

CIndexBuffer::~CIndexBuffer()
{
#ifdef _DEBUG
	--s_BufferCount;
#endif

	Unlock(0);

#ifdef CHECK_INDICES
	if ( m_pShadowIndices )
	{
		delete [] m_pShadowIndices;
		m_pShadowIndices = NULL;
	}
#endif

#ifdef MEASURE_DRIVER_ALLOCATIONS
	int nMemUsed = 1024;
	VPROF_INCREMENT_GROUP_COUNTER( "ib count", COUNTER_GROUP_NO_RESET, -1 );
	VPROF_INCREMENT_GROUP_COUNTER( "ib driver mem", COUNTER_GROUP_NO_RESET, -nMemUsed );
	VPROF_INCREMENT_GROUP_COUNTER( "total driver mem", COUNTER_GROUP_NO_RESET, -nMemUsed );
#endif

#if !defined( _X360 )
	if ( m_pIB )
	{
		RECORD_COMMAND( DX8_DESTROY_INDEX_BUFFER, 1 );
		RECORD_INT( m_UID );

		Dx9Device()->Release( m_pIB );
	}
#else
	if ( m_pAllocatedMemory )
	{
		MemAlloc_RegisterExternalDeallocation( XMem_CIndexBuffer, m_pAllocatedMemory, XPhysicalSize( m_pAllocatedMemory ) );
		XPhysicalFree( m_pAllocatedMemory );
	}

	m_pAllocatedMemory = NULL;
	m_pIB = NULL;
#endif

#ifdef VPROF_ENABLED
	if ( !m_bDynamic )
	{
		VPROF_INCREMENT_GROUP_COUNTER( "TexGroup_global_" TEXTURE_GROUP_STATIC_INDEX_BUFFER,
			COUNTER_GROUP_TEXTURE_GLOBAL, - IndexCount() * IndexSize() );
	}
	else if ( IsX360() )
	{
		VPROF_INCREMENT_GROUP_COUNTER( "TexGroup_global_" TEXTURE_GROUP_DYNAMIC_INDEX_BUFFER,
			COUNTER_GROUP_TEXTURE_GLOBAL, - IndexCount() * IndexSize() );
	}
#endif
}
	

//-----------------------------------------------------------------------------
// Do we have enough room without discarding?
//-----------------------------------------------------------------------------
inline bool CIndexBuffer::HasEnoughRoom( int numIndices ) const
{
#if !defined( _X360 )
	return ( numIndices + m_Position ) <= m_IndexCount;
#else
	return numIndices <= m_IndexCount; //the ring buffer will free room as needed
#endif
}

//-----------------------------------------------------------------------------
// Block until this part of the index buffer is free
//-----------------------------------------------------------------------------
inline void CIndexBuffer::BlockUntilUnused( int nAllocationSize )
{
	Assert( nAllocationSize <= m_IndexCount );

#ifdef _X360
	Assert( (m_AllocationRing.Count() != 0) || ((m_Position == 0) && (m_iNextBlockingPosition == m_iAllocationCount)) );

	if ( (m_iNextBlockingPosition - m_Position) >= nAllocationSize )
		return;

	Assert( (m_AllocationRing[m_AllocationRing.Head()].m_iStartOffset == 0) || ((m_iNextBlockingPosition == m_AllocationRing[m_AllocationRing.Head()].m_iStartOffset) && (m_Position <= m_iNextBlockingPosition)) );

	int iMinBlockPosition = m_Position + nAllocationSize;
	if( iMinBlockPosition > m_iAllocationCount )
	{
		//Allocation requires us to wrap
		iMinBlockPosition = nAllocationSize;
		m_Position = 0;

		//modify the last allocation so that it uses up the whole tail end of the buffer. Makes other code simpler
		Assert( m_AllocationRing.Count() != 0 );
		m_AllocationRing[m_AllocationRing.Tail()].m_iEndOffset = m_iAllocationCount;

		//treat all allocations between the current position and the tail end of the ring as freed since they will be before we unblock
		while( m_AllocationRing.Count() ) 
		{
			unsigned int head = m_AllocationRing.Head();
			if( m_AllocationRing[head].m_iStartOffset == 0 )
				break;

			m_AllocationRing.Remove( head );
		}
	}

	//now we go through the allocations until we find the last fence we care about. Treat everything up until that fence as freed.
	DWORD FinalFence = 0;
	while( m_AllocationRing.Count() )
	{
		unsigned int head = m_AllocationRing.Head();		
		
		if( m_AllocationRing[head].m_iEndOffset >= iMinBlockPosition )
		{
			//When this frees, we'll finally have enough space for the allocation
			FinalFence = m_AllocationRing[head].m_Fence;
			m_iNextBlockingPosition = m_AllocationRing[head].m_iEndOffset;
			m_AllocationRing.Remove( head );
			break;
		}
		m_AllocationRing.Remove( head );
	}
	Assert( FinalFence != 0 );

#ifdef SPEW_INDEX_BUFFER_STALLS
	if( Dx9Device()->IsFencePending( FinalFence ) )
	{
		float st = Plat_FloatTime();
#endif

		Dx9Device()->BlockOnFence( FinalFence );

#ifdef SPEW_INDEX_BUFFER_STALLS	
		float dt = Plat_FloatTime() - st;
		Warning( "Blocked locking dynamic index buffer for %f ms!\n", 1000.0 * dt );
	}
#endif

#endif
}


//-----------------------------------------------------------------------------
// lock, unlock
//-----------------------------------------------------------------------------
unsigned short* CIndexBuffer::Lock( bool bReadOnly, int numIndices, int& startIndex, int startPosition )
{
	Assert( !m_bLocked );

#if defined( _X360 )
	if ( m_pIB && m_pIB->IsSet( Dx9Device() ) )
	{
		Unbind( m_pIB );
	}
#endif

	unsigned short* pLockedData = NULL;

	// For write-combining, ensure we always have locked memory aligned to 4-byte boundaries
	if( m_bDynamic )
		numIndices = ALIGN_VALUE( numIndices, 2 );

	// Ensure there is enough space in the IB for this data
	if ( numIndices > m_IndexCount ) 
	{ 
		Error( "too many indices for index buffer. . tell a programmer (%d>%d)\n", ( int )numIndices, ( int )m_IndexCount );
		Assert( false ); 
		return 0; 
	}
	
#ifndef _X360
	if ( !m_pIB )
		return 0;
#endif

	DWORD dwFlags;
	
	if ( m_bDynamic )
	{
		Assert( startPosition < 0 );
#if !defined( _X360 )
		dwFlags = LOCKFLAGS_APPEND;
	
		// If either user forced us to flush,
		// or there is not enough space for the vertex data,
		// then flush the buffer contents
		// xbox must not append at position 0 because nooverwrite cannot be guaranteed
		
		if ( !m_Position || m_bFlush || !HasEnoughRoom(numIndices) )
		{
			m_bFlush = false;
			m_Position = 0;

			dwFlags = LOCKFLAGS_FLUSH;
		}
#else
		if ( m_bFlush )
		{
#			if ( defined( X360_BLOCK_ON_IB_FLUSH ) )
			{
				if( m_AllocationRing.Count() )
				{
					DWORD FinalFence = m_AllocationRing[m_AllocationRing.Tail()].m_Fence;

					m_AllocationRing.RemoveAll();
					m_Position = 0;
					m_iNextBlockingPosition = m_iAllocationCount;

#				if ( defined( SPEW_VERTEX_BUFFER_STALLS ) )
					if( Dx9Device()->IsFencePending( FinalFence ) )
					{
						float st = Plat_FloatTime();
#				endif
						Dx9Device()->BlockOnFence( FinalFence );
#				if ( defined ( SPEW_VERTEX_BUFFER_STALLS ) )
						float dt = Plat_FloatTime() - st;
						Warning( "Blocked FLUSHING dynamic index buffer for %f ms!\n", 1000.0 * dt );
					}
#				endif
				}
			}
#			endif
			m_bFlush = false;
		}
#endif
	}
	else
	{
		dwFlags = D3DLOCK_NOSYSLOCK;
	}
	
	if ( bReadOnly )
	{
		dwFlags |= D3DLOCK_READONLY;
	}

	int position = m_Position;
	if( startPosition >= 0 )
	{
		position = startPosition;
	}

	RECORD_COMMAND( DX8_LOCK_INDEX_BUFFER, 4 );
	RECORD_INT( m_UID );
	RECORD_INT( position * IndexSize() );
	RECORD_INT( numIndices * IndexSize() );
	RECORD_INT( dwFlags );

#ifdef CHECK_INDICES
	m_LockedStartIndex = position;
	m_LockedNumIndices = numIndices;
#endif

	HRESULT hr = D3D_OK;

#if !defined( _X360 )
	if ( m_bDynamic )
	{
		hr = Dx9Device()->Lock( m_pIB, position * IndexSize(), numIndices * IndexSize(), 
						   reinterpret_cast< void** >( &pLockedData ), dwFlags, &m_LockData );
	}
	else
	{
		hr = Dx9Device()->Lock( m_pIB, position * IndexSize(),  numIndices * IndexSize(), 
						   reinterpret_cast< void** >( &pLockedData ), dwFlags );
	}
#else
	if ( m_bDynamic )
	{
		// Block until earlier parts of the buffer are free
		BlockUntilUnused( numIndices );
		position = m_Position;
		m_pIB = NULL;
		Assert( (m_Position + numIndices) <= m_iAllocationCount );
	}
	else
	{
		//static, block until last lock finished?
		m_Position = position;
	}
	pLockedData = (unsigned short *)(m_pAllocatedMemory + (position * IndexSize()));
	
#endif

	switch ( hr )
	{
		case D3DERR_INVALIDCALL:
			Msg( "D3DERR_INVALIDCALL - Index Buffer Lock Failed in %s on line %d(offset %d, size %d, flags 0x%x)\n", V_UnqualifiedFileName(__FILE__), __LINE__, position * IndexSize(), numIndices * IndexSize(), dwFlags );
			break;
		case D3DERR_DRIVERINTERNALERROR:
			Msg( "D3DERR_DRIVERINTERNALERROR - Index Buffer Lock Failed in %s on line %d (offset %d, size %d, flags 0x%x)\n", V_UnqualifiedFileName(__FILE__), __LINE__, position * IndexSize(), numIndices * IndexSize(), dwFlags );
			break;
		case D3DERR_OUTOFVIDEOMEMORY:
			Msg( "D3DERR_OUTOFVIDEOMEMORY - Index Buffer Lock Failed in %s on line %d (offset %d, size %d, flags 0x%x)\n", V_UnqualifiedFileName(__FILE__), __LINE__, position * IndexSize(), numIndices * IndexSize(), dwFlags );
			break;
	}

	Assert( pLockedData != NULL );
	   
	if ( !IsX360() )
	{
		startIndex = position;
	}
	else
	{
		startIndex = 0;
	}

	Assert( m_bLocked == false );
	m_bLocked = true;
	return pLockedData;
}

void CIndexBuffer::Unlock( int numIndices )
{
#if defined( _X360 )
	Assert( (m_Position + numIndices) <= m_iAllocationCount );
#else
	Assert( (m_Position + numIndices) <= m_IndexCount );
#endif

	if ( !m_bLocked )
		return;

	// For write-combining, ensure we always have locked memory aligned to 4-byte boundaries
	if( m_bDynamic )
		ALIGN_VALUE( numIndices, 2 );

#ifndef _X360
	if ( !m_pIB )
		return;
#endif

	RECORD_COMMAND( DX8_UNLOCK_INDEX_BUFFER, 1 );
	RECORD_INT( m_UID );

#ifdef CHECK_INDICES
	m_LockedStartIndex = 0;
	m_LockedNumIndices = 0;
#endif

#if !defined( _X360 )
	if ( m_bDynamic )
	{
		Dx9Device()->Unlock( m_pIB, &m_LockData, numIndices*IndexSize() );
	}
	else
	{
		Dx9Device()->Unlock( m_pIB );
	}
#else
	if ( m_bDynamic )
	{
		Assert( (m_Position == 0) || (m_AllocationRing[m_AllocationRing.Tail()].m_iEndOffset == m_Position) );

		DynamicBufferAllocation_t LockData;
		LockData.m_Fence = Dx9Device()->GetCurrentFence(); //This isn't the correct fence, but it's all we have access to for now and it'll provide marginal safety if something goes really wrong.
		LockData.m_iStartOffset	= m_Position;
		LockData.m_iEndOffset = LockData.m_iStartOffset + numIndices;
		Assert( (LockData.m_iStartOffset == 0) || (LockData.m_iStartOffset == m_AllocationRing[m_AllocationRing.Tail()].m_iEndOffset) );
		m_AllocationRing.AddToTail( LockData );
		
		void* pLockedData = m_pAllocatedMemory + (LockData.m_iStartOffset * IndexSize());
		
		//Always re-use the same index buffer header based on the assumption that D3D copies it off in the draw calls.
		m_pIB = &m_D3DIndexBuffer;
		XGSetIndexBufferHeader( numIndices * IndexSize(), 0, D3DFMT_INDEX16, 0, 0, m_pIB );
		XGOffsetResourceAddress( m_pIB, pLockedData );

		// Invalidate the GPU caches for this memory.
		// FIXME: Should dynamic allocations be 4k aligned?
		Dx9Device()->InvalidateGpuCache( pLockedData, numIndices * IndexSize(), 0 );
	}
	else
	{
		if ( !m_pIB )
		{
			int nBufferSize = m_IndexCount * IndexSize();
			XGSetIndexBufferHeader( nBufferSize, 0, D3DFMT_INDEX16, 0, 0, &m_D3DIndexBuffer );
			XGOffsetResourceAddress( &m_D3DIndexBuffer, m_pAllocatedMemory );
			m_pIB = &m_D3DIndexBuffer;
		}

		// Invalidate the GPU caches for this memory.
		Dx9Device()->InvalidateGpuCache( m_pAllocatedMemory, m_IndexCount * IndexSize(), 0 );
	}
#endif

	m_Position += numIndices;
	m_bLocked = false;
}

#ifdef _WIN32
#pragma warning (default:4189)
#endif

#include "tier0/memdbgoff.h"

#endif  // DYNAMICIB_H

