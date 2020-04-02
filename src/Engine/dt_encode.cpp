//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include "quakedef.h"
#include "dt.h"
#include "dt_encode.h"
#include "coordsize.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern void DataTable_Warning( const char *pInMessage, ... );
extern bool ShouldWatchThisProp( const SendTable *pTable, int objectID, const char *pPropName );

// The engine implements this.
extern const char* GetObjectClassName( int objectID );

// Check for special flags like SPROP_COORD, SPROP_NOSCALE, and SPROP_NORMAL.
// Returns true if it encoded the float.
static inline bool EncodeSpecialFloat( const SendProp *pProp, float fVal, bf_write *pOut )
{
	int flags = pProp->GetFlags();
	if ( flags & SPROP_COORD )
	{
		pOut->WriteBitCoord( fVal );
		return true;
	}
	else if ( flags & SPROP_COORD_MP )
	{
		pOut->WriteBitCoordMP( fVal, false, false );
		return true;
	}
	else if ( flags & SPROP_COORD_MP_LOWPRECISION )
	{
		pOut->WriteBitCoordMP( fVal, false, true );
		return true;
	}
	else if ( flags & SPROP_COORD_MP_INTEGRAL )
	{
		pOut->WriteBitCoordMP( fVal, true, false );
		return true;
	}
	else if ( flags & SPROP_NOSCALE )
	{
		pOut->WriteBitFloat( fVal );
		return true;
	}
	else if ( flags & SPROP_NORMAL )
	{
		pOut->WriteBitNormal( fVal );
		return true;
	}
	return false;
}


static inline void EncodeFloat( const SendProp *pProp, float fVal, bf_write *pOut, int objectID )
{
	// Check for special flags like SPROP_COORD, SPROP_NOSCALE, and SPROP_NORMAL.
	if( EncodeSpecialFloat( pProp, fVal, pOut ) )
	{
		return;
	}

	unsigned long ulVal;
	if( fVal < pProp->m_fLowValue )
	{
		// clamp < 0
		ulVal = 0;
		
		if(!(pProp->GetFlags() & SPROP_ROUNDUP))
		{
			DataTable_Warning("(class %s): Out-of-range value (%f) in SendPropFloat '%s', clamping.\n", GetObjectClassName( objectID ), fVal, pProp->m_pVarName );
		}
	}
	else if( fVal > pProp->m_fHighValue )
	{
		// clamp > 1
		ulVal = ((1 << pProp->m_nBits) - 1);

		if(!(pProp->GetFlags() & SPROP_ROUNDDOWN))
		{
			DataTable_Warning("%s: Out-of-range value (%f) in SendPropFloat '%s', clamping.\n", GetObjectClassName( objectID ), fVal, pProp->m_pVarName );
		}
	}
	else
	{
		float fRangeVal = (fVal - pProp->m_fLowValue) * pProp->m_fHighLowMul;
		ulVal = RoundFloatToUnsignedLong( fRangeVal );
	}
	
	pOut->WriteUBitLong(ulVal, pProp->m_nBits);
}


// Look for special flags like SPROP_COORD, SPROP_NOSCALE, and SPROP_NORMAL and
// decode if they're there. Fills in fVal and returns true if it decodes anything.
static inline bool DecodeSpecialFloat( SendProp const *pProp, bf_read *pIn, float &fVal )
{
	int flags = pProp->GetFlags();

	if ( flags & SPROP_COORD )
	{
		fVal = pIn->ReadBitCoord();
		return true;
	}
	else if ( flags & SPROP_COORD_MP )
	{
		fVal = pIn->ReadBitCoordMP( false, false );
		return true;
	}
	else if ( flags & SPROP_COORD_MP_LOWPRECISION )
	{
		fVal = pIn->ReadBitCoordMP( false, true );
		return true;
	}
	else if ( flags & SPROP_COORD_MP_INTEGRAL )
	{
		fVal = pIn->ReadBitCoordMP( true, false );
		return true;
	}
	else if ( flags & SPROP_NOSCALE )
	{
		fVal = pIn->ReadBitFloat();
		return true;
	}
	else if ( flags & SPROP_NORMAL )
	{
		fVal = pIn->ReadBitNormal();
		return true;
	}

	return false;
}


static float DecodeFloat(SendProp const *pProp, bf_read *pIn)
{
	float fVal;
	unsigned long dwInterp;

	// Check for special flags..
	if( DecodeSpecialFloat( pProp, pIn, fVal ) )
	{
		return fVal;
	}

	dwInterp = pIn->ReadUBitLong(pProp->m_nBits);
	fVal = (float)dwInterp / ((1 << pProp->m_nBits) - 1);
	fVal = pProp->m_fLowValue + (pProp->m_fHighValue - pProp->m_fLowValue) * fVal;
	return fVal;
}

static inline void DecodeVector(SendProp const *pProp, bf_read *pIn, float *v)
{
	v[0] = DecodeFloat(pProp, pIn);
	v[1] = DecodeFloat(pProp, pIn);

	// Don't read in the third component for normals
	if ((pProp->GetFlags() & SPROP_NORMAL) == 0)
	{
		v[2] = DecodeFloat(pProp, pIn);
	}
	else
	{
		int signbit = pIn->ReadOneBit();

		float v0v0v1v1 = v[0] * v[0] +
			v[1] * v[1];
		if (v0v0v1v1 < 1.0f)
			v[2] = sqrtf( 1.0f - v0v0v1v1 );
		else
			v[2] = 0.0f;

		if (signbit)
			v[2] *= -1.0f;
	}
}

static inline void DecodeQuaternion(SendProp const *pProp, bf_read *pIn, float *v)
{
	v[0] = DecodeFloat(pProp, pIn);
	v[1] = DecodeFloat(pProp, pIn);
	v[2] = DecodeFloat(pProp, pIn);
	v[3] = DecodeFloat(pProp, pIn);
}

int	DecodeBits( DecodeInfo *pInfo, unsigned char *pOut )
{
	bf_read temp;

	// Read the property in (note: we don't return the bits from here because Decode returns
	// the decoded bits.. we're interested in getting the encoded bits).
	temp = *pInfo->m_pIn;
	pInfo->m_pRecvProp = NULL;
	pInfo->m_pData = NULL;
	g_PropTypeFns[pInfo->m_pProp->m_Type].Decode( pInfo );

	// Return the encoded bits.
	int nBits = pInfo->m_pIn->GetNumBitsRead() - temp.GetNumBitsRead();
	temp.ReadBits(pOut, nBits);
	return nBits;
}


// ---------------------------------------------------------------------------------------- //
// Most of the prop types can use this generic FastCopy version. Arrays are a bit of a pain.
// ---------------------------------------------------------------------------------------- //

inline void Generic_FastCopy( 
	const SendProp *pSendProp, 
	const RecvProp *pRecvProp, 
	const unsigned char *pSendData, 
	unsigned char *pRecvData,
	int objectID )
{
	// Get the data out of the ent.
	CRecvProxyData recvProxyData;

	pSendProp->GetProxyFn()( 
		pSendProp,
		pSendData, 
		pSendData + pSendProp->GetOffset(),
		&recvProxyData.m_Value,
		0,
		objectID
		);

	// Fill in the data for the recv proxy.
	recvProxyData.m_pRecvProp = pRecvProp;
	recvProxyData.m_iElement = 0;
	recvProxyData.m_ObjectID = objectID;
	pRecvProp->GetProxyFn()( &recvProxyData, pRecvData, pRecvData + pRecvProp->GetOffset() );
}


// ---------------------------------------------------------------------------------------- //
// DecodeInfo implementation.
// ---------------------------------------------------------------------------------------- //

void DecodeInfo::CopyVars( const DecodeInfo *pOther )
{
	m_pStruct = pOther->m_pStruct;
	m_pData = pOther->m_pData;
	
	m_pRecvProp = pOther->m_pRecvProp;
	m_pProp = pOther->m_pProp;
	m_pIn = pOther->m_pIn;
	m_ObjectID = pOther->m_ObjectID;
	m_iElement = pOther->m_iElement;
}


// ---------------------------------------------------------------------------------------- //
// Int property type abstraction.
// ---------------------------------------------------------------------------------------- //

void Int_Encode( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp, bf_write *pOut, int objectID )
{
	if( pProp->IsSigned() )
	{
		pOut->WriteSBitLong( pVar->m_Int, pProp->m_nBits );
	}
	else
	{
		pOut->WriteUBitLong( (unsigned int)pVar->m_Int, pProp->m_nBits );
	}
}


void Int_Decode( DecodeInfo *pInfo )
{
	if(pInfo->m_pProp->GetFlags() & SPROP_UNSIGNED)
	{
		pInfo->m_Value.m_Int = pInfo->m_pIn->ReadUBitLong(pInfo->m_pProp->m_nBits);
	}
	else
	{
		pInfo->m_Value.m_Int = pInfo->m_pIn->ReadSBitLong( pInfo->m_pProp->m_nBits );
	}
	
	if ( pInfo->m_pRecvProp )
	{
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
	}
}


int Int_CompareDeltas( const SendProp *pProp, bf_read *p1, bf_read *p2 )
{
	return p1->ReadUBitLong( pProp->m_nBits ) != p2->ReadUBitLong( pProp->m_nBits );
}

const char* Int_GetTypeNameString()
{
	return "DPT_Int";
}


bool Int_IsZero( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp )
{
	return (pVar->m_Int == 0);
}


void Int_DecodeZero( DecodeInfo *pInfo )
{
	pInfo->m_Value.m_Int = 0;

	if ( pInfo->m_pRecvProp )
	{
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
	}
}

bool Int_IsEncodedZero( const SendProp *pProp, bf_read *pIn )
{
	return pIn->ReadUBitLong( pProp->m_nBits ) == 0;
}

void Int_SkipProp( const SendProp *pProp, bf_read *pIn )
{
	pIn->SeekRelative( pProp->m_nBits );
}

// ---------------------------------------------------------------------------------------- //
// Float type abstraction.
// ---------------------------------------------------------------------------------------- //

void Float_Encode( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp, bf_write *pOut, int objectID )
{
	EncodeFloat( pProp, pVar->m_Float, pOut, objectID );
}


void Float_Decode( DecodeInfo *pInfo )
{
	pInfo->m_Value.m_Float = DecodeFloat(pInfo->m_pProp, pInfo->m_pIn);

	if ( pInfo->m_pRecvProp )
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
}


int	Float_CompareDeltas( const SendProp *pProp, bf_read *p1, bf_read *p2 )
{
	if ( pProp->GetFlags() & SPROP_COORD )
	{
		return p1->ReadBitCoord() != p2->ReadBitCoord();
	}
	else if ( pProp->GetFlags() & SPROP_COORD_MP )
	{
		return p1->ReadBitCoordMP( false, false ) != p2->ReadBitCoordMP( false, false );
	}
	else if ( pProp->GetFlags() & SPROP_COORD_MP_LOWPRECISION )
	{
		return p1->ReadBitCoordMP( false, true ) != p2->ReadBitCoordMP( false, true );
	}
	else if ( pProp->GetFlags() & SPROP_COORD_MP_INTEGRAL )
	{
		return p1->ReadBitCoordMP( true, false ) != p2->ReadBitCoordMP( true, false );
	}
	else if ( pProp->GetFlags() & SPROP_NOSCALE )
	{
		return p1->ReadUBitLong( 32 ) != p2->ReadUBitLong( 32 );
	}
	else if ( pProp->GetFlags() & SPROP_NORMAL )
	{
		return p1->ReadUBitLong( NORMAL_FRACTIONAL_BITS+1 ) != p2->ReadUBitLong( NORMAL_FRACTIONAL_BITS+1 );
	}
	else
	{
		return p1->ReadUBitLong( pProp->m_nBits ) != p2->ReadUBitLong( pProp->m_nBits );
	}
}

const char* Float_GetTypeNameString()
{
	return "DPT_Float";
}


bool Float_IsZero( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp )
{
	return (pVar->m_Float == 0);
}


void Float_DecodeZero( DecodeInfo *pInfo )
{
	pInfo->m_Value.m_Float = 0;
	if ( pInfo->m_pRecvProp )
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
}

bool Float_IsEncodedZero( const SendProp *pProp, bf_read *pIn )
{
	return DecodeFloat( pProp, pIn ) == 0.0f;
}

void Float_SkipProp( const SendProp *pProp, bf_read *pIn )
{
	// Check for special flags..
	if(pProp->GetFlags() & SPROP_COORD)
	{
		// Read the required integer and fraction flags
		unsigned int val = pIn->ReadUBitLong(2);
		// this reads two bits, the first bit (bit0 in this word) indicates integer part
		// the second bit (bit1 in this word) indicates the fractional part

		// If we got either parse them, otherwise it's a zero.
		if ( val )
		{
			// sign bit
			int seekDist = 1;

			// If there's an integer, read it in
			if ( val & 1 )
				seekDist += COORD_INTEGER_BITS;
		
			if ( val & 2 )
				seekDist += COORD_FRACTIONAL_BITS;

			pIn->SeekRelative( seekDist );
		}
	}
	else if ( pProp->GetFlags() & SPROP_COORD_MP )
	{
		pIn->ReadBitCoordMP( false, false );
	}
	else if ( pProp->GetFlags() & SPROP_COORD_MP_LOWPRECISION )
	{
		pIn->ReadBitCoordMP( false, true );
	}
	else if ( pProp->GetFlags() & SPROP_COORD_MP_INTEGRAL )
	{
		pIn->ReadBitCoordMP( true, false );
	}
	else if(pProp->GetFlags() & SPROP_NOSCALE)
	{
		pIn->SeekRelative( 32 );
	}
	else if(pProp->GetFlags() & SPROP_NORMAL)
	{
		pIn->SeekRelative( NORMAL_FRACTIONAL_BITS + 1 );
	}
	else
	{
		pIn->SeekRelative( pProp->m_nBits );
	}
}


// ---------------------------------------------------------------------------------------- //
// Vector type abstraction.
// ---------------------------------------------------------------------------------------- //

void Vector_Encode( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp, bf_write *pOut, int objectID )
{
	EncodeFloat(pProp, pVar->m_Vector[0], pOut, objectID);
	EncodeFloat(pProp, pVar->m_Vector[1], pOut, objectID);

	// Don't write out the third component for normals
	if ((pProp->GetFlags() & SPROP_NORMAL) == 0)
	{
		EncodeFloat(pProp, pVar->m_Vector[2], pOut, objectID);
	}
	else
	{
		// Write a sign bit for z instead!
		int	signbit = (pVar->m_Vector[2] <= -NORMAL_RESOLUTION);
		pOut->WriteOneBit( signbit );
	}
}


void Vector_Decode(DecodeInfo *pInfo)
{
	DecodeVector( pInfo->m_pProp, pInfo->m_pIn, pInfo->m_Value.m_Vector );
	
	if( pInfo->m_pRecvProp )
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
}


int	Vector_CompareDeltas( const SendProp *pProp, bf_read *p1, bf_read *p2 )
{
	int c1 = Float_CompareDeltas( pProp, p1, p2 );
	int c2 = Float_CompareDeltas( pProp, p1, p2 );
	int c3;
	
	if ( pProp->GetFlags() & SPROP_NORMAL )
	{
		c3 = p1->ReadOneBit() != p2->ReadOneBit();
	}
	else
	{
		c3 = Float_CompareDeltas( pProp, p1, p2 );
	}

	return c1 | c2 | c3;
}

const char* Vector_GetTypeNameString()
{
	return "DPT_Vector";
}


bool Vector_IsZero( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp )
{
	return ( pVar->m_Vector[0] == 0 ) && ( pVar->m_Vector[1] == 0 ) && ( pVar->m_Vector[2] == 0 );
}


void Vector_DecodeZero( DecodeInfo *pInfo )
{
	pInfo->m_Value.m_Vector[0] = 0;
	pInfo->m_Value.m_Vector[1] = 0;
	pInfo->m_Value.m_Vector[2] = 0;
	if ( pInfo->m_pRecvProp )
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
}

bool Vector_IsEncodedZero( const SendProp *pProp, bf_read *pIn )
{
	float v[3];
	
	DecodeVector( pProp, pIn, v );

	return ( v[0] == 0 ) && ( v[1] == 0 ) && ( v[2] == 0 );
}

void Vector_SkipProp( const SendProp *pProp, bf_read *pIn )
{
	Float_SkipProp(pProp, pIn);
	Float_SkipProp(pProp, pIn);

	// Don't read in the third component for normals
	if ( pProp->GetFlags() & SPROP_NORMAL )
	{
		pIn->SeekRelative( 1 );
	}
	else
	{
		Float_SkipProp(pProp, pIn);
	}
}

#if 0 // We can't ship this since it changes the size of DTVariant to be 20 bytes instead of 16 and that breaks MODs!!!

// ---------------------------------------------------------------------------------------- //
// Quaternion type abstraction.
// ---------------------------------------------------------------------------------------- //

void Quaternion_Encode( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp, bf_write *pOut, int objectID )
{
	EncodeFloat(pProp, pVar->m_Vector[0], pOut, objectID);
	EncodeFloat(pProp, pVar->m_Vector[1], pOut, objectID);
	EncodeFloat(pProp, pVar->m_Vector[2], pOut, objectID);
	EncodeFloat(pProp, pVar->m_Vector[3], pOut, objectID);
}


void Quaternion_Decode(DecodeInfo *pInfo)
{
	DecodeQuaternion( pInfo->m_pProp, pInfo->m_pIn, pInfo->m_Value.m_Vector );
	
	if( pInfo->m_pRecvProp )
	{
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
	}
}


int	Quaternion_CompareDeltas( const SendProp *pProp, bf_read *p1, bf_read *p2 )
{
	int c1 = Float_CompareDeltas( pProp, p1, p2 );
	int c2 = Float_CompareDeltas( pProp, p1, p2 );
	int c3 = Float_CompareDeltas( pProp, p1, p2 );
	int c4 = Float_CompareDeltas( pProp, p1, p2 );
	return c1 | c2 | c3 | c4;
}

const char* Quaternion_GetTypeNameString()
{
	return "DPT_Quaternion";
}


bool Quaternion_IsZero( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp )
{
	return ( pVar->m_Vector[0] == 0 ) && ( pVar->m_Vector[1] == 0 ) && ( pVar->m_Vector[2] == 0 ) && ( pVar->m_Vector[3] == 0 );
}


void Quaternion_DecodeZero( DecodeInfo *pInfo )
{
	pInfo->m_Value.m_Vector[0] = 0;
	pInfo->m_Value.m_Vector[1] = 0;
	pInfo->m_Value.m_Vector[2] = 0;
	pInfo->m_Value.m_Vector[3] = 0;
	if ( pInfo->m_pRecvProp )
	{
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
	}
}

bool Quaternion_IsEncodedZero( const SendProp *pProp, bf_read *pIn )
{
	float v[4];
	
	DecodeQuaternion( pProp, pIn, v );

	return ( v[0] == 0 ) && ( v[1] == 0 ) && ( v[2] == 0 ) && ( v[3] == 0 );
}

void Quaternion_SkipProp( const SendProp *pProp, bf_read *pIn )
{
	Float_SkipProp(pProp, pIn);
	Float_SkipProp(pProp, pIn);
	Float_SkipProp(pProp, pIn);
	Float_SkipProp(pProp, pIn);
}
#endif


// ---------------------------------------------------------------------------------------- //
// String type abstraction.
// ---------------------------------------------------------------------------------------- //

void String_Encode( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp, bf_write *pOut, int objectID )
{
	// First count the string length, then do one WriteBits call.
	int len;
	for ( len=0; len < DT_MAX_STRING_BUFFERSIZE-1; len++ )
	{
		if( pVar->m_pString[len] == 0 )
		{
			break;
		}
	}	
		
	// Optionally write the length here so deltas can be compared faster.
	pOut->WriteUBitLong( len, DT_MAX_STRING_BITS );
	pOut->WriteBits( pVar->m_pString, len * 8 );
}


void String_Decode(DecodeInfo *pInfo)
{
	// Read it in.
	int len = pInfo->m_pIn->ReadUBitLong( DT_MAX_STRING_BITS );

	char *tempStr = pInfo->m_TempStr;

	if ( len >= DT_MAX_STRING_BUFFERSIZE )
	{
		Warning( "String_Decode( %s ) invalid length (%d)\n", pInfo->m_pRecvProp->GetName(), len );
		len = DT_MAX_STRING_BUFFERSIZE - 1;
	}

	pInfo->m_pIn->ReadBits( tempStr, len*8 );
	tempStr[len] = 0;

	pInfo->m_Value.m_pString = tempStr;

	// Give it to the RecvProxy.
	if ( pInfo->m_pRecvProp )
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
}


// Compare the bits in pBuf1 and pBuf2 and return 1 if they are different.
// This must always seek both buffers to wherever they start at + nBits.
static inline int AreBitsDifferent( bf_read *pBuf1, bf_read *pBuf2, int nBits )
{
	int nDWords = nBits >> 5;

	int diff = 0;
	for ( int iDWord=0; iDWord < nDWords; iDWord++ )
	{
		diff |= (pBuf1->ReadUBitLong(32) != pBuf2->ReadUBitLong(32));
	}

	int nRemainingBits = nBits - (nDWords<<5);
	if (nRemainingBits > 0)
		diff |= pBuf1->ReadUBitLong( nRemainingBits ) != pBuf2->ReadUBitLong( nRemainingBits );
	
	return diff;
}


int String_CompareDeltas( const SendProp *pProp, bf_read *p1, bf_read *p2 )
{
	int len1 = p1->ReadUBitLong( DT_MAX_STRING_BITS );
	int len2 = p2->ReadUBitLong( DT_MAX_STRING_BITS );

	if ( len1 == len2 )
	{
		// check if both strings are empty
		if (len1 == 0)
			return false;

		// Ok, they're short and fast.
		return AreBitsDifferent( p1, p2, len1*8 );
	}
	else
	{
		p1->SeekRelative( len1 * 8 );
		p2->SeekRelative( len2 * 8 );
		return true;
	}
}

const char* String_GetTypeNameString()
{
	return "DPT_String";
}


bool String_IsZero( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp )
{
	return ( pVar->m_pString[0] == 0 );
}


void String_DecodeZero( DecodeInfo *pInfo )
{
	pInfo->m_Value.m_pString = pInfo->m_TempStr;
	pInfo->m_TempStr[0] = 0;
	if ( pInfo->m_pRecvProp )
		pInfo->m_pRecvProp->GetProxyFn()( pInfo, pInfo->m_pStruct, pInfo->m_pData );
}

bool String_IsEncodedZero( const SendProp *pProp, bf_read *pIn )
{
	// Read it in.
	int len = pIn->ReadUBitLong( DT_MAX_STRING_BITS );
	
	pIn->SeekRelative( len*8 );

	return len == 0;
}

void String_SkipProp( const SendProp *pProp, bf_read *pIn )
{
	int len = pIn->ReadUBitLong( DT_MAX_STRING_BITS );
	pIn->SeekRelative( len*8 );
}


// ---------------------------------------------------------------------------------------- //
// Array abstraction.
// ---------------------------------------------------------------------------------------- //

int Array_GetLength( const unsigned char *pStruct, const SendProp *pProp, int objectID )
{
	// Get the array length from the proxy.
	ArrayLengthSendProxyFn proxy = pProp->GetArrayLengthProxy();
	
	if ( proxy )
	{
		int nElements = proxy( pStruct, objectID );
		
		// Make sure it's not too big.
		if ( nElements > pProp->GetNumElements() )
		{
			Assert( false );
			nElements = pProp->GetNumElements();
		}

		return nElements;
	}
	else
	{	
		return pProp->GetNumElements();
	}
}


void Array_Encode( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp, bf_write *pOut, int objectID )
{
	SendProp *pArrayProp = pProp->GetArrayProp();
	AssertMsg( pArrayProp, ("Array_Encode: missing m_pArrayProp for SendProp '%s'.", pProp->m_pVarName) );
	
	int nElements = Array_GetLength( pStruct, pProp, objectID );

	// Write the number of elements.
	pOut->WriteUBitLong( nElements, pProp->GetNumArrayLengthBits() );

	unsigned char *pCurStructOffset = (unsigned char*)pStruct + pArrayProp->GetOffset();
	for ( int iElement=0; iElement < nElements; iElement++ )
	{
		DVariant var;

		// Call the proxy to get the value, then encode.
		pArrayProp->GetProxyFn()( pArrayProp, pStruct, pCurStructOffset, &var, iElement, objectID );
		g_PropTypeFns[pArrayProp->GetType()].Encode( pStruct, &var, pArrayProp, pOut, objectID ); 
		
		pCurStructOffset += pProp->GetElementStride();
	}
}


void Array_Decode( DecodeInfo *pInfo )
{
	SendProp *pArrayProp = pInfo->m_pProp->GetArrayProp();
	AssertMsg( pArrayProp, ("Array_Decode: missing m_pArrayProp for a property.") );

	// Setup a DecodeInfo that is used to decode each of the child properties.
	DecodeInfo subDecodeInfo;
	subDecodeInfo.CopyVars( pInfo );
	subDecodeInfo.m_pProp = pArrayProp;

	int elementStride = 0;	
	ArrayLengthRecvProxyFn lengthProxy = 0;
	if ( pInfo->m_pRecvProp )
	{
		RecvProp *pArrayRecvProp = pInfo->m_pRecvProp->GetArrayProp();
		subDecodeInfo.m_pRecvProp = pArrayRecvProp;
		
		// Note we get the OFFSET from the array element property and the STRIDE from the array itself.
		subDecodeInfo.m_pData = (char*)pInfo->m_pData + pArrayRecvProp->GetOffset();
		elementStride = pInfo->m_pRecvProp->GetElementStride();
		Assert( elementStride != -1 ); // (Make sure it was set..)

		lengthProxy = pInfo->m_pRecvProp->GetArrayLengthProxy();
	}

	int nElements = pInfo->m_pIn->ReadUBitLong( pInfo->m_pProp->GetNumArrayLengthBits() );

	if ( lengthProxy )
		lengthProxy( pInfo->m_pStruct, pInfo->m_ObjectID, nElements );

	for ( subDecodeInfo.m_iElement=0; subDecodeInfo.m_iElement < nElements; subDecodeInfo.m_iElement++ )
	{
		g_PropTypeFns[pArrayProp->GetType()].Decode( &subDecodeInfo );
		subDecodeInfo.m_pData = (char*)subDecodeInfo.m_pData + elementStride;
	}
}


int Array_CompareDeltas( const SendProp *pProp, bf_read *p1, bf_read *p2 )
{
	SendProp *pArrayProp = pProp->GetArrayProp();
	AssertMsg( pArrayProp, ("Array_CompareDeltas: missing m_pArrayProp for SendProp '%s'.", pProp->m_pVarName) );

	int nLengthBits = pProp->GetNumArrayLengthBits(); 
	int length1 = p1->ReadUBitLong( nLengthBits );
	int length2 = p2->ReadUBitLong( nLengthBits );

	int bDifferent = length1 != length2;
	
	// Compare deltas on the props that are the same.
	int nSame = min( length1, length2 );
	for ( int iElement=0; iElement < nSame; iElement++ )
	{
		bDifferent |= g_PropTypeFns[pArrayProp->GetType()].CompareDeltas( pArrayProp, p1, p2 );
	}

	// Now just eat up the remaining properties in whichever buffer was larger.
	if ( length1 != length2 )
	{
		bf_read *buffer = (length1 > length2) ? p1 : p2;

		int nExtra = max( length1, length2 ) - nSame;
		for ( int iEatUp=0; iEatUp < nExtra; iEatUp++ )
		{
			SkipPropData( buffer, pArrayProp );
		}
	}
	
	return bDifferent;
}


void Array_FastCopy( 
	const SendProp *pSendProp, 
	const RecvProp *pRecvProp, 
	const unsigned char *pSendData, 
	unsigned char *pRecvData, 
	int objectID )
{
	const RecvProp *pArrayRecvProp = pRecvProp->GetArrayProp();
	const SendProp *pArraySendProp = pSendProp->GetArrayProp();

	CRecvProxyData recvProxyData;
	recvProxyData.m_pRecvProp =	pArrayRecvProp;
	recvProxyData.m_ObjectID = objectID;

	// Find out the array length and call the RecvProp's array-length proxy.
	int nElements = Array_GetLength( pSendData, pSendProp, objectID );
	ArrayLengthRecvProxyFn lengthProxy = pRecvProp->GetArrayLengthProxy();
	if ( lengthProxy )
		lengthProxy( pRecvData, objectID, nElements );

	const unsigned char *pCurSendPos = pSendData + pArraySendProp->GetOffset();
	unsigned char *pCurRecvPos = pRecvData + pArrayRecvProp->GetOffset();
	for ( recvProxyData.m_iElement=0; recvProxyData.m_iElement < nElements; recvProxyData.m_iElement++ )
	{
		// Get this array element out of the sender's data.
		pArraySendProp->GetProxyFn()( pArraySendProp, pSendData, pCurSendPos, &recvProxyData.m_Value, recvProxyData.m_iElement, objectID );
		pCurSendPos += pSendProp->GetElementStride();
		
		// Write it into the receiver.
		pArrayRecvProp->GetProxyFn()( &recvProxyData, pRecvData, pCurRecvPos );
		pCurRecvPos += pRecvProp->GetElementStride();
	}
}

const char* Array_GetTypeNameString()
{
	return "DPT_Array";
}


bool Array_IsZero( const unsigned char *pStruct, DVariant *pVar, const SendProp *pProp )
{
	int nElements = Array_GetLength( pStruct, pProp, -1 );
	return ( nElements == 0 );
}


void Array_DecodeZero( DecodeInfo *pInfo )
{
	ArrayLengthRecvProxyFn lengthProxy = pInfo->m_pRecvProp->GetArrayLengthProxy();
	if ( lengthProxy )
		lengthProxy( pInfo->m_pStruct, pInfo->m_ObjectID, 0 );
}

bool Array_IsEncodedZero( const SendProp *pProp, bf_read *pIn )
{
	SendProp *pArrayProp = pProp->GetArrayProp();
	AssertMsg( pArrayProp, ("Array_IsEncodedZero: missing m_pArrayProp for a property.") );

	int nElements = pIn->ReadUBitLong( pProp->GetNumArrayLengthBits() );

	for ( int i=0; i < nElements;  i++ )
	{
		// skip over data
		g_PropTypeFns[pArrayProp->GetType()].IsEncodedZero( pArrayProp, pIn );
	}
	
	return nElements == 0;;
}

void Array_SkipProp( const SendProp *pProp, bf_read *pIn )
{
	SendProp *pArrayProp = pProp->GetArrayProp();
	AssertMsg( pArrayProp, ("Array_SkipProp: missing m_pArrayProp for a property.") );

	int nElements = pIn->ReadUBitLong( pProp->GetNumArrayLengthBits() );

	for ( int i=0; i < nElements;  i++ )
	{
		// skip over data
		g_PropTypeFns[pArrayProp->GetType()].SkipProp( pArrayProp, pIn );
	}
}


// ---------------------------------------------------------------------------------------- //
// Datatable type abstraction.
// ---------------------------------------------------------------------------------------- //

const char* DataTable_GetTypeNameString()
{
	return "DPT_DataTable";
}



PropTypeFns g_PropTypeFns[DPT_NUMSendPropTypes] =
{
	// DPT_Int
	{
		Int_Encode,
		Int_Decode,
		Int_CompareDeltas,
		Generic_FastCopy,
		Int_GetTypeNameString,
		Int_IsZero,
		Int_DecodeZero,
		Int_IsEncodedZero,
		Int_SkipProp,
	},

	// DPT_Float
	{
		Float_Encode,
		Float_Decode,
		Float_CompareDeltas,
		Generic_FastCopy,
		Float_GetTypeNameString,
		Float_IsZero,
		Float_DecodeZero,
		Float_IsEncodedZero,
		Float_SkipProp,
	},

	// DPT_Vector
	{
		Vector_Encode,
		Vector_Decode,
		Vector_CompareDeltas,
		Generic_FastCopy,
		Vector_GetTypeNameString,
		Vector_IsZero,
		Vector_DecodeZero,
		Vector_IsEncodedZero,
		Vector_SkipProp,
	},

	// DPT_String
	{
		String_Encode,
		String_Decode,
		String_CompareDeltas,
		Generic_FastCopy,
		String_GetTypeNameString,
		String_IsZero,
		String_DecodeZero,
		String_IsEncodedZero,
		String_SkipProp,
	},

	// DPT_Array
	{
		Array_Encode,
		Array_Decode,
		Array_CompareDeltas,
		Array_FastCopy,
		Array_GetTypeNameString,
		Array_IsZero,
		Array_DecodeZero,
		Array_IsEncodedZero,
		Array_SkipProp,
	},
	 
	// DPT_DataTable
	{
		NULL,
		NULL,
		NULL,
		NULL,
		DataTable_GetTypeNameString,
		NULL,
		NULL,
		NULL,
		NULL,
	},
#if 0 // We can't ship this since it changes the size of DTVariant to be 20 bytes instead of 16 and that breaks MODs!!!

	// DPT_Quaternion
	{
		Quaternion_Encode,
		Quaternion_Decode,
		Quaternion_CompareDeltas,
		Generic_FastCopy,
		Quaternion_GetTypeNameString,
		Quaternion_IsZero,
		Quaternion_DecodeZero,
		Quaternion_IsEncodedZero,
		Quaternion_SkipProp,
	},
#endif
};
