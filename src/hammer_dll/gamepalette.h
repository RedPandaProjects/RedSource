//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef _GAMEPALETTE_H
#define _GAMEPALETTE_H


typedef struct _D3DRMPALETTEENTRY
{
	unsigned char red;          /* 0 .. 255 */
	unsigned char green;        /* 0 .. 255 */
	unsigned char blue;         /* 0 .. 255 */
	unsigned char flags;        /* one of D3DRMPALETTEFLAGS */
} D3DRMPALETTEENTRY, * LPD3DRMPALETTEENTRY;

class CGamePalette
{
public:
	CGamePalette();
	~CGamePalette();

	BOOL Create(LPCTSTR pszFile);

	void SetBrightness(float fValue);
	float GetBrightness();

	operator LOGPALETTE*()
	{ return pPalette; }
	operator D3DRMPALETTEENTRY*()
	{ return (D3DRMPALETTEENTRY*) pPalette->palPalEntry; }
	operator CPalette*()
	{ return &GDIPalette; }

private:
	float fBrightness;

	// CPalette:
	CPalette GDIPalette;

	// palette working with:
	LOGPALETTE *pPalette;
	// to convert & store in pPalette:
	LOGPALETTE *pOriginalPalette;

	// file stored in:
	CString strFile;

	// sizeof each palette:
	size_t uPaletteBytes;
};

#endif