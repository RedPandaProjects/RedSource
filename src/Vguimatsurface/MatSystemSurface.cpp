//====== Copyright � 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose: Implementation of the VGUI ISurface interface using the 
// material system to implement it
//
//=============================================================================//

#if !defined( _X360 )
#include <windows.h>
#endif
#include "tier1/strtools.h"
#include "tier0/icommandline.h"
#include "tier0/dbg.h"
#include "FileSystem.h"
#include <vgui/vgui.h>
#include <Color.h>
#include "UtlVector.h"
#include "Clip2D.h"
#include <vgui_controls/Panel.h>
#include <vgui/IInput.h>
#include <vgui/Point.h>
#include "bitmap/imageformat.h"
#include "TextureDictionary.h"
#include "Cursor.h"
#include "input.h"
#include <vgui/IHTML.h>
#include <vgui/IVGui.h>
#include "../common/vgui_surfacelib/FontManager.h"
#include "FontTextureCache.h"
#if !defined( _X360 )
#include "../common/vgui/htmlwindow.h"
#endif
#include "MatSystemSurface.h"
#include "inputsystem/iinputsystem.h"
#include <vgui_controls/Controls.h>
#include <vgui/ISystem.h>
#include "icvar.h"
#include "mathlib/mathlib.h"
#include <vgui/ILocalize.h>
#include "mathlib/VMatrix.h"
#include <tier0/vprof.h>
#include "materialsystem/itexture.h"
#include <malloc.h>
#include "../vgui2/vgui2/VPanel.h"
#include <vgui/IInputInternal.h>
#if defined( _X360 )
#include "xbox/xbox_win32stubs.h"
#endif
#include "../common/xbox/xboxstubs.h"

#pragma warning( disable : 4706 )

#include <vgui/IVguiMatInfo.h>
#include <vgui/IVguiMatInfoVar.h>
#include "materialsystem/imaterialvar.h"

#pragma warning( default : 4706 )

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define VPANEL_NORMAL	((vgui::SurfacePlat *) NULL)
#define VPANEL_MINIMIZED ((vgui::SurfacePlat *) 0x00000001)

#define MAX_BATCHED_CHAR_VERTS	4096

using namespace vgui;

class CVguiMatInfoVar : public IVguiMatInfoVar
{
public:
	CVguiMatInfoVar( IMaterialVar *pMaterialVar )
	{
		m_pMaterialVar = pMaterialVar;
	}

	// from IVguiMatInfoVar
	virtual int GetIntValue ( void ) const
	{
		return m_pMaterialVar->GetIntValue();
	}

	virtual void SetIntValue ( int val )
	{
		m_pMaterialVar->SetIntValue( val );
	}

private:
	IMaterialVar *m_pMaterialVar;
};

class CVguiMatInfo : public IVguiMatInfo
{
public:
	CVguiMatInfo( IMaterial *pMaterial )
	{
		m_pMaterial = pMaterial;
	}

	// from IVguiMatInfo
	virtual IVguiMatInfoVar* FindVarFactory( const char *varName, bool *found )
	{
		IMaterialVar *pMaterialVar = m_pMaterial->FindVar( varName, found );

		if ( pMaterialVar == NULL )
			return NULL;
		return new CVguiMatInfoVar( pMaterialVar );
	}

	virtual int GetNumAnimationFrames( void )
	{
		return m_pMaterial->GetNumAnimationFrames();
	}

private:
	IMaterial *m_pMaterial;
};


//-----------------------------------------------------------------------------
// Globals...
//-----------------------------------------------------------------------------
vgui::IInputInternal		*g_pIInput;
static bool					g_bInDrawing;
static CFontTextureCache	g_FontTextureCache;


//-----------------------------------------------------------------------------
// Singleton instance
//-----------------------------------------------------------------------------
CMatSystemSurface g_MatSystemSurface;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CMatSystemSurface, ISurface, 
						VGUI_SURFACE_INTERFACE_VERSION, g_MatSystemSurface );


//-----------------------------------------------------------------------------
// Make sure the panel is the same size as the viewport
//-----------------------------------------------------------------------------
CMatEmbeddedPanel::CMatEmbeddedPanel() : BaseClass( NULL, "MatSystemTopPanel" )
{
	SetPaintBackgroundEnabled( false );

#if defined( _X360 )
	SetPos( 0, 0 );
	SetSize( GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
#endif
}

void CMatEmbeddedPanel::OnThink()
{
	int x, y, width, height;
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	pRenderContext->GetViewport( x, y, width, height );
	SetSize( width, height );
	SetPos( x, y );
	Repaint();
}

VPANEL CMatEmbeddedPanel::IsWithinTraverse(int x, int y, bool traversePopups)
{
	VPANEL retval = BaseClass::IsWithinTraverse( x, y, traversePopups );
	if ( retval == GetVPanel() )
		return 0;
	return retval;
}


//-----------------------------------------------------------------------------
// Constructor, destructor
//-----------------------------------------------------------------------------
CMatSystemSurface::CMatSystemSurface() : m_pEmbeddedPanel(NULL), m_pWhite(NULL), m_BatchedCharVerts(0, 256)
{
	m_iBoundTexture = -1; 
	m_HWnd = NULL; 
	m_bIn3DPaintMode = false;
	m_bDrawingIn3DWorld = false;
	m_PlaySoundFunc = NULL;
	m_bInThink = false;
	m_bAllowJavaScript = false;
	m_bAppDrivesInput = false;
	m_nLastInputPollCount = 0;

	m_hCurrentFont = NULL;
	m_pRestrictedPanel = NULL;

	m_bNeedsKeyboard = true;
	m_bNeedsMouse = true;

	memset( m_WorkSpaceInsets, 0, sizeof( m_WorkSpaceInsets ) );
}

CMatSystemSurface::~CMatSystemSurface()
{
}


//-----------------------------------------------------------------------------
// Connect, disconnect...
//-----------------------------------------------------------------------------
bool CMatSystemSurface::Connect( CreateInterfaceFn factory )
{
	if ( !BaseClass::Connect( factory ) )
		return false;

	if ( !g_pFullFileSystem )
	{
		Warning( "MatSystemSurface requires the file system to run!\n" );
		return false;
	}

	if ( !g_pMaterialSystem )
	{
		Warning( "MatSystemSurface requires the material system to run!\n" );
		return false;
	}

	if ( !g_pVGuiPanel )
	{
		Warning( "MatSystemSurface requires the vgui::IPanel system to run!\n" );
		return false;
	}

	g_pIInput = (IInputInternal *)factory( VGUI_INPUTINTERNAL_INTERFACE_VERSION, NULL );
	if ( !g_pIInput )
	{
		Warning( "MatSystemSurface requires the vgui::IInput system to run!\n" );
		return false;
	}

	if ( !g_pVGui )
	{
		Warning( "MatSystemSurface requires the vgui::IVGUI system to run!\n" );
		return false;
	}

	Assert( g_pVGuiSurface == this );

	// initialize vgui_control interfaces
	if ( !vgui::VGui_InitInterfacesList( "MATSURFACE", &factory, 1 ) )
		return false;

	return true;	
}

void CMatSystemSurface::Disconnect()
{
	g_pIInput = NULL;
	BaseClass::Disconnect();
}


//-----------------------------------------------------------------------------
// Access to other interfaces...
//-----------------------------------------------------------------------------
void *CMatSystemSurface::QueryInterface( const char *pInterfaceName )
{
	// We also implement the IMatSystemSurface interface
	if (!Q_strncmp(	pInterfaceName, MAT_SYSTEM_SURFACE_INTERFACE_VERSION, Q_strlen(MAT_SYSTEM_SURFACE_INTERFACE_VERSION) + 1))
		return (IMatSystemSurface*)this;

	// We also implement the IMatSystemSurface interface
	if (!Q_strncmp(	pInterfaceName, VGUI_SURFACE_INTERFACE_VERSION, Q_strlen(VGUI_SURFACE_INTERFACE_VERSION) + 1))
		return (vgui::ISurface*)this;

	return BaseClass::QueryInterface( pInterfaceName );
}


//-----------------------------------------------------------------------------
// Initialization and shutdown...
//-----------------------------------------------------------------------------
InitReturnVal_t CMatSystemSurface::Init( void )
{
	InitReturnVal_t nRetVal = BaseClass::Init();
	if ( nRetVal != INIT_OK )
		return nRetVal;

	// Allocate a white material
	KeyValues *pVMTKeyValues = new KeyValues( "UnlitGeneric" );
	pVMTKeyValues->SetInt( "$vertexcolor", 1 );
	pVMTKeyValues->SetInt( "$vertexalpha", 1 );
	pVMTKeyValues->SetInt( "$ignorez", 1 );
	pVMTKeyValues->SetInt( "$no_fullbright", 1 );
	pVMTKeyValues->SetInt( "$nocull", 1 );
	m_pWhite.Init( "VGUI_White", TEXTURE_GROUP_OTHER, pVMTKeyValues );

	if ( IsPC() )
	{
		// Set up a material with which to reference the final image for subsequent display using vgui
		pVMTKeyValues = new KeyValues( "UnlitGeneric" );
		pVMTKeyValues->SetString( "$basetexture", "_rt_FullScreen" );
		pVMTKeyValues->SetInt( "$nocull", 1 );
		pVMTKeyValues->SetInt( "$nofog", 1 );
		pVMTKeyValues->SetInt( "$ignorez", 1 );
		m_FullScreenBufferMaterial.Init( "VGUI_3DPaint_FullScreen", TEXTURE_GROUP_OTHER, pVMTKeyValues );
		m_FullScreenBufferMaterial->Refresh();
		m_nFullScreenBufferMaterialId = -1;
	}

	m_DrawColor[0] = m_DrawColor[1] = m_DrawColor[2] = m_DrawColor[3] = 255;
	m_nTranslateX = m_nTranslateY = 0;
	EnableScissor( false );
	SetScissorRect( 0, 0, 100000, 100000 );
	m_flAlphaMultiplier = 1.0f;

	// By default, use the default embedded panel
	m_pDefaultEmbeddedPanel = new CMatEmbeddedPanel;
	SetEmbeddedPanel( m_pDefaultEmbeddedPanel->GetVPanel() );

	m_iBoundTexture = -1;

	// Initialize font info..
	m_pDrawTextPos[0] = m_pDrawTextPos[1] = 0;
	m_DrawTextColor[0] = m_DrawTextColor[1] = m_DrawTextColor[2] = m_DrawTextColor[3] = 255;

	m_bIn3DPaintMode = false;
	m_bDrawingIn3DWorld = false;
	m_PlaySoundFunc = NULL;

	// Input system
	InitInput();

	// Initialize cursors
	InitCursors();

	// fonts initialization
	char language[64];
	bool bValid;
	if ( IsPC() )
	{
		bValid = system()->GetRegistryString( "HKEY_CURRENT_USER\\Software\\Valve\\Steam\\Language", language, sizeof(language)-1 );
	}
	else
	{
		Q_strncpy( language, XBX_GetLanguageString(), sizeof( language ) );
		bValid = true;
	}

	if ( bValid )
	{
		FontManager().SetLanguage( language );
	}
	else
	{
		FontManager().SetLanguage( "english" );
	}

	// font manager needs the file system and material system for bitmap fonts
	FontManager().SetInterfaces( g_pFullFileSystem, g_pMaterialSystem );

	return INIT_OK;
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::Shutdown( void )
{
	for ( int i = m_FileTypeImages.First(); i != m_FileTypeImages.InvalidIndex(); i = m_FileTypeImages.Next( i ) )
	{
		delete m_FileTypeImages[ i ];
	}
	m_FileTypeImages.RemoveAll();

	// Release all textures
	TextureDictionary()->DestroyAllTextures();
	m_iBoundTexture = -1;

	// Release the standard materials
	m_pWhite.Shutdown();
	m_FullScreenBufferMaterial.Shutdown();
	m_FullScreenBuffer.Shutdown();

#if defined( ENABLE_HTMLWINDOW )
	// we need to delete these BEFORE we close our window down, as the browser is using it
	// if this DOESN'T run then it will crash when we close the main window
	for (int i=0;i<GetHTMLWindowCount();i++)
	{
		delete GetHTMLWindow(i);
	}
#endif

	m_Titles.Purge();
	m_PaintStateStack.Purge();

#if !defined( _X360 )
 	// release any custom font files
	// use newer function if possible
	HMODULE gdiModule = ::LoadLibrary( "gdi32.dll" );
	typedef int (WINAPI *RemoveFontResourceExProc)(LPCTSTR, DWORD, PVOID);
	RemoveFontResourceExProc pRemoveFontResourceEx = NULL;
	if ( gdiModule )
	{
		pRemoveFontResourceEx = (RemoveFontResourceExProc)::GetProcAddress(gdiModule, "RemoveFontResourceExA");
	}

	for (int i = 0; i < m_CustomFontFileNames.Count(); i++)
 	{
		if (pRemoveFontResourceEx)
		{
			// dvs: Keep removing the font until we get an error back. After consulting with Microsoft, it appears
			// that RemoveFontResourceEx must sometimes be called multiple times to work. Doing this insures that
			// when we load the font next time we get the real font instead of Ariel.
			int nRetries = 0;
			while ( (*pRemoveFontResourceEx)(m_CustomFontFileNames[i].String(), 0x10, NULL) && ( nRetries < 10 ) )
			{
				nRetries++;
				Msg( "Removed font resource %s on attempt %d.\n", m_CustomFontFileNames[i].String(), nRetries );
			}
		}
		else
		{
			// dvs: Keep removing the font until we get an error back. After consulting with Microsoft, it appears
			// that RemoveFontResourceEx must sometimes be called multiple times to work. Doing this insures that
			// when we load the font next time we get the real font instead of Ariel.
			int nRetries = 0;
			while ( ::RemoveFontResource(m_CustomFontFileNames[i].String()) && ( nRetries < 10 ) )
			{
				nRetries++;
				Msg( "Removed font resource %s on attempt %d.\n", m_CustomFontFileNames[i].String(), nRetries );
			}
		}
 	}
#endif

 	m_CustomFontFileNames.RemoveAll();
	m_BitmapFontFileNames.RemoveAll();
	m_BitmapFontFileMapping.RemoveAll();

	Cursor_ClearUserCursors();

#if !defined( _X360 )
	if ( gdiModule )
	{
		::FreeLibrary(gdiModule);
	}
#endif

	BaseClass::Shutdown();
}

void CMatSystemSurface::SetEmbeddedPanel(VPANEL pEmbeddedPanel)
{
	m_pEmbeddedPanel = pEmbeddedPanel;
	((VPanel *)pEmbeddedPanel)->Client()->RequestFocus(0);
}

//-----------------------------------------------------------------------------
// hierarchy root
//-----------------------------------------------------------------------------
VPANEL CMatSystemSurface::GetEmbeddedPanel()
{
	return m_pEmbeddedPanel;
}

//-----------------------------------------------------------------------------
// Purpose: cap bits
// Warning: if you change this, make sure the SurfaceV28 wrapper above reports
//          the correct capabilities.
//-----------------------------------------------------------------------------
bool CMatSystemSurface::SupportsFeature(SurfaceFeature_e feature)
{
	switch (feature)
	{
	case ISurface::ANTIALIASED_FONTS:
	case ISurface::DROPSHADOW_FONTS:
		return true;

	case ISurface::OUTLINE_FONTS:
		if ( IsX360() )
			return false;
	case ISurface::ESCAPE_KEY:
		return true;

	case ISurface::OPENING_NEW_HTML_WINDOWS:
	case ISurface::FRAME_MINIMIZE_MAXIMIZE:
	default:
		return false;
	};
}

//-----------------------------------------------------------------------------
// Hook needed to Get input to work
//-----------------------------------------------------------------------------
void CMatSystemSurface::AttachToWindow( void *hWnd, bool bLetAppDriveInput )
{
	InputDetachFromWindow( m_HWnd );
	m_HWnd = hWnd;
	if ( hWnd )
	{
		InputAttachToWindow( hWnd );
		m_bAppDrivesInput = bLetAppDriveInput;
	}
	else
	{
		// Never call RunFrame stuff
		m_bAppDrivesInput = true;
	}
}

bool CMatSystemSurface::HandleInputEvent( const InputEvent_t &event )
{
	if ( !m_bAppDrivesInput )
	{
		g_pIInput->UpdateButtonState( event );
	}

	return InputHandleInputEvent( event );
}


//-----------------------------------------------------------------------------
// Draws a panel in 3D space. Assumes view + projection are already set up
// Also assumes the (x,y) coordinates of the panels are defined in 640xN coords
// (N isn't necessary 480 because the panel may not be 4x3)
// The width + height specified are the size of the panel in world coordinates
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawPanelIn3DSpace( vgui::VPANEL pRootPanel, const VMatrix &panelCenterToWorld, int pw, int ph, float sw, float sh )
{
	Assert( pRootPanel );

	// FIXME: When should such panels be solved?!?
	SolveTraverse( pRootPanel, false );

	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	// Force Z buffering to be on for all panels drawn...
	pRenderContext->OverrideDepthEnable( true, false );

	Assert(!m_bDrawingIn3DWorld);
	m_bDrawingIn3DWorld = true;

	StartDrawingIn3DSpace( panelCenterToWorld, pw, ph, sw, sh );

	((VPanel *)pRootPanel)->Client()->Repaint();
	((VPanel *)pRootPanel)->Client()->PaintTraverse(true, false);

	FinishDrawing();

	// Reset z buffering to normal state
	pRenderContext->OverrideDepthEnable( false, true ); 

	m_bDrawingIn3DWorld = false;
}


//-----------------------------------------------------------------------------
// Purpose: Setup rendering for vgui on a panel existing in 3D space
//-----------------------------------------------------------------------------
void CMatSystemSurface::StartDrawingIn3DSpace( const VMatrix &screenToWorld, int pw, int ph, float sw, float sh )
{
	g_bInDrawing = true;
	m_iBoundTexture = -1; 

	int px = 0;
	int py = 0;

	m_pSurfaceExtents[0] = px;
	m_pSurfaceExtents[1] = py;
	m_pSurfaceExtents[2] = px + pw;
	m_pSurfaceExtents[3] = py + ph;

	// In order for this to work, the model matrix must have its origin
	// at the upper left corner of the screen. We must also scale down the
	// rendering from pixel space to screen space. Let's construct a matrix
	// transforming from pixel coordinates (640xN) to screen coordinates
	// (wxh, with the origin at the upper left of the screen). Then we'll
	// concatenate it with the panelCenterToWorld to produce pixelToWorld transform
	VMatrix pixelToScreen;

	// First, scale it so that 0->pw transforms to 0->sw
	MatrixBuildScale( pixelToScreen, sw / pw, -sh / ph, 1.0f );

	// Construct pixelToWorld
	VMatrix pixelToWorld;
	MatrixMultiply( screenToWorld, pixelToScreen, pixelToWorld );

	// make sure there is no translation and rotation laying around
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadMatrix( pixelToWorld );

	// These are only here so that FinishDrawing works...
	pRenderContext->MatrixMode( MATERIAL_PROJECTION );
	pRenderContext->PushMatrix();

	pRenderContext->MatrixMode( MATERIAL_VIEW );
	pRenderContext->PushMatrix();

	// Always enable scissoring (translate to origin because of the glTranslatef call above..)
	EnableScissor( true );

	m_nTranslateX = 0;
	m_nTranslateY = 0;
	m_flAlphaMultiplier = 1.0f;
}


//-----------------------------------------------------------------------------
// Purpose: Setup ortho for vgui
//-----------------------------------------------------------------------------

// we may need to offset by 0.5 texels to account for the different in pixel vs. texel centers in dx7-9
// however, we do this fixup already when we set up the texture coordinates for all materials/fonts
// so in theory we shouldn't need to do any adjustments for setting up the screen
// HOWEVER, we must do the offset, else the driver will think the text is something that should
// be antialiased, so the text will look broken if antialiasing is turned on (usually forced on in the driver)
float g_flPixelOffsetX = 0.5f;
float g_flPixelOffsetY = 0.5f;
bool g_bCheckedCommandLine = false;

void CMatSystemSurface::StartDrawing( void )
{
	if ( !g_bCheckedCommandLine )
	{
		g_bCheckedCommandLine = true;
		
		const char *pX = CommandLine()->ParmValue( "-pixel_offset_x", (const char*)NULL );
		if ( pX )
			g_flPixelOffsetX = atof( pX );

		const char *pY = CommandLine()->ParmValue( "-pixel_offset_y", (const char*)NULL );
		if ( pY )
			g_flPixelOffsetY = atof( pY );
	}

	g_bInDrawing = true;
	m_iBoundTexture = -1; 

	int x, y, width, height;

	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	pRenderContext->GetViewport( x, y, width, height);

	m_pSurfaceExtents[0] = x;
	m_pSurfaceExtents[1] = y;
	m_pSurfaceExtents[2] = x + width;
	m_pSurfaceExtents[3] = y + height;

	pRenderContext->MatrixMode( MATERIAL_PROJECTION );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->Scale( 1, -1, 1 );
	pRenderContext->Ortho( g_flPixelOffsetX, g_flPixelOffsetY, width + g_flPixelOffsetX, height + g_flPixelOffsetY, -1.0f, 1.0f ); 

	// make sure there is no translation and rotation laying around
	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	// Always enable scissoring (translate to origin because of the glTranslatef call above..)
	EnableScissor( true );

	m_nTranslateX = 0;
	m_nTranslateY = 0;

	pRenderContext->MatrixMode( MATERIAL_VIEW );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::FinishDrawing( void )
{
	// We're done with scissoring
	EnableScissor( false );

	// Restore the matrices
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	pRenderContext->MatrixMode( MATERIAL_PROJECTION );
	pRenderContext->PopMatrix();

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();

	pRenderContext->MatrixMode( MATERIAL_VIEW );
	pRenderContext->PopMatrix();

  	Assert( g_bInDrawing );
	g_bInDrawing = false;
}


//-----------------------------------------------------------------------------
// frame
//-----------------------------------------------------------------------------
void CMatSystemSurface::RunFrame()
{
	int nPollCount = g_pInputSystem->GetPollCount();
	if ( m_nLastInputPollCount == nPollCount )
		return;

	// If this isn't true, we've lost input!
	if ( !m_bAppDrivesInput && m_nLastInputPollCount != nPollCount - 1 )
	{
		Assert( 0 );
		Warning( "Vgui is losing input messages! Call brian!\n" );
	}

	m_nLastInputPollCount = nPollCount;

	if ( m_bAppDrivesInput )
		return;

	// Generate all input messages
	int nEventCount = g_pInputSystem->GetEventCount();
	const InputEvent_t* pEvents = g_pInputSystem->GetEventData( );
	for ( int i = 0; i < nEventCount; ++i )
	{
		HandleInputEvent( pEvents[i] );
	}
}


//-----------------------------------------------------------------------------
// Sets up a particular painting state...
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetupPaintState( const PaintState_t &paintState )
{
	m_nTranslateX = paintState.m_iTranslateX;
	m_nTranslateY = paintState.m_iTranslateY;
	SetScissorRect( paintState.m_iScissorLeft, paintState.m_iScissorTop, 
		paintState.m_iScissorRight, paintState.m_iScissorBottom );
}

//-----------------------------------------------------------------------------
// Indicates a particular panel is about to be rendered 
//-----------------------------------------------------------------------------
void CMatSystemSurface::PushMakeCurrent(VPANEL pPanel, bool useInSets)
{
	int inSets[4] = {0, 0, 0, 0};
	int absExtents[4];
	int clipRect[4];

	if (useInSets)
	{
		g_pVGuiPanel->GetInset(pPanel, inSets[0], inSets[1], inSets[2], inSets[3]);
	}

	g_pVGuiPanel->GetAbsPos(pPanel, absExtents[0], absExtents[1]);
	int wide, tall;
	g_pVGuiPanel->GetSize(pPanel, wide, tall);
	absExtents[2] = absExtents[0] + wide;
	absExtents[3] = absExtents[1] + tall;

	g_pVGuiPanel->GetClipRect(pPanel, clipRect[0], clipRect[1], clipRect[2], clipRect[3]);

	int i = m_PaintStateStack.AddToTail();
	PaintState_t &paintState = m_PaintStateStack[i];
	paintState.m_pPanel = pPanel;

	// Determine corrected top left origin
	paintState.m_iTranslateX = inSets[0] + absExtents[0] - m_pSurfaceExtents[0];	
	paintState.m_iTranslateY = inSets[1] + absExtents[1] - m_pSurfaceExtents[1];

	// Setup clipping rectangle for scissoring
	paintState.m_iScissorLeft	= clipRect[0] - m_pSurfaceExtents[0];
	paintState.m_iScissorTop	= clipRect[1] - m_pSurfaceExtents[1];
	paintState.m_iScissorRight	= clipRect[2] - m_pSurfaceExtents[0];
	paintState.m_iScissorBottom	= clipRect[3] - m_pSurfaceExtents[1];
	
	SetupPaintState( paintState );
}

void CMatSystemSurface::PopMakeCurrent(VPANEL pPanel)
{
	// draw any remaining text
	if ( m_BatchedCharVerts.Count() )
	{
		DrawFlushText();
	}

	int top = m_PaintStateStack.Count() - 1;

	// More pops that pushes?
	Assert( top >= 0 );

	// Didn't pop in reverse order of push?
	Assert( m_PaintStateStack[top].m_pPanel == pPanel );

	m_PaintStateStack.Remove(top);

	if (top > 0)
		SetupPaintState( m_PaintStateStack[top-1] );

//	m_iBoundTexture = -1; 
}


//-----------------------------------------------------------------------------
// Color Setting methods
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetColor(int r, int g, int b, int a)
{
  	Assert( g_bInDrawing );
	m_DrawColor[0]=(unsigned char)r;
	m_DrawColor[1]=(unsigned char)g;
	m_DrawColor[2]=(unsigned char)b;
	m_DrawColor[3]=(unsigned char)(a * m_flAlphaMultiplier);
}

void CMatSystemSurface::DrawSetColor(Color col)
{
  	Assert( g_bInDrawing );
	DrawSetColor(col[0], col[1], col[2], col[3]);
}


//-----------------------------------------------------------------------------
// material Setting methods 
//-----------------------------------------------------------------------------
void CMatSystemSurface::InternalSetMaterial( IMaterial *pMaterial )
{
	if (!pMaterial)
	{
		pMaterial = m_pWhite;
	}

	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	m_pMesh = pRenderContext->GetDynamicMesh( true, NULL, NULL, pMaterial );
}


//-----------------------------------------------------------------------------
// Helper method to initialize vertices (transforms them into screen space too)
//-----------------------------------------------------------------------------
void CMatSystemSurface::InitVertex( vgui::Vertex_t &vertex, int x, int y, float u, float v )
{
	vertex.m_Position.Init( x + m_nTranslateX, y + m_nTranslateY );
	vertex.m_TexCoord.Init( u, v );
}


//-----------------------------------------------------------------------------
// Draws a line!
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawTexturedLineInternal( const Vertex_t &a, const Vertex_t &b )
{
	Assert( !m_bIn3DPaintMode );

	// Don't bother drawing fully transparent lines
	if( m_DrawColor[3] == 0 )
		return;

	vgui::Vertex_t verts[2] = { a, b };
	
	verts[0].m_Position.x += m_nTranslateX + g_flPixelOffsetX;
	verts[0].m_Position.y += m_nTranslateY + g_flPixelOffsetY;
	
	verts[1].m_Position.x += m_nTranslateX + g_flPixelOffsetX;
	verts[1].m_Position.y += m_nTranslateY + g_flPixelOffsetY;

	vgui::Vertex_t clippedVerts[2];

	if (!ClipLine( verts, clippedVerts ))
		return;

	meshBuilder.Begin( m_pMesh, MATERIAL_LINES, 1 );

	meshBuilder.Color4ubv( m_DrawColor );
	meshBuilder.TexCoord2fv( 0, clippedVerts[0].m_TexCoord.Base() );
	meshBuilder.Position3f( clippedVerts[0].m_Position.x, clippedVerts[0].m_Position.y, m_flZPos );
	meshBuilder.AdvanceVertex();

	meshBuilder.Color4ubv( m_DrawColor );
	meshBuilder.TexCoord2fv( 0, clippedVerts[1].m_TexCoord.Base() );
	meshBuilder.Position3f( clippedVerts[1].m_Position.x, clippedVerts[1].m_Position.y, m_flZPos );
	meshBuilder.AdvanceVertex();

	meshBuilder.End();
	m_pMesh->Draw();
}

void CMatSystemSurface::DrawLine( int x0, int y0, int x1, int y1 )
{
	Assert( g_bInDrawing );

	// Don't bother drawing fully transparent lines
	if( m_DrawColor[3] == 0 )
		return;

	vgui::Vertex_t verts[2];
	verts[0].Init( Vector2D( x0, y0 ), Vector2D( 0, 0 ) );
	verts[1].Init( Vector2D( x1, y1 ), Vector2D( 1, 1 ) );
	
	InternalSetMaterial( );
	DrawTexturedLineInternal( verts[0], verts[1] );
}


void CMatSystemSurface::DrawTexturedLine( const Vertex_t &a, const Vertex_t &b )
{
	IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(m_iBoundTexture);
	InternalSetMaterial( pMaterial );
	DrawTexturedLineInternal( a, b );
}


//-----------------------------------------------------------------------------
// Draws a line!
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawPolyLine( int *px, int *py ,int n )
{
	Assert( g_bInDrawing );

	Assert( !m_bIn3DPaintMode );

	// Don't bother drawing fully transparent lines
	if( m_DrawColor[3] == 0 )
		return;

	InternalSetMaterial( );
	meshBuilder.Begin( m_pMesh, MATERIAL_LINES, n );

	for ( int i = 0; i < n ; i++ )
	{
		int inext = ( i + 1 ) % n;

		vgui::Vertex_t verts[2];
		vgui::Vertex_t clippedVerts[2];
		
		int x0, y0, x1, y1;

		x0 = px[ i ];
		x1 = px[ inext ];
		y0 = py[ i ];
		y1 = py[ inext ];

		InitVertex( verts[0], x0, y0, 0, 0 );
		InitVertex( verts[1], x1, y1, 1, 1 );

		if (!ClipLine( verts, clippedVerts ))
			continue;

		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.TexCoord2fv( 0, clippedVerts[0].m_TexCoord.Base() );
		meshBuilder.Position3f( clippedVerts[0].m_Position.x+ g_flPixelOffsetX, clippedVerts[0].m_Position.y + g_flPixelOffsetY, m_flZPos );
		meshBuilder.AdvanceVertex();

		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.TexCoord2fv( 0, clippedVerts[1].m_TexCoord.Base() );
		meshBuilder.Position3f( clippedVerts[1].m_Position.x+ g_flPixelOffsetX, clippedVerts[1].m_Position.y + g_flPixelOffsetY, m_flZPos );
		meshBuilder.AdvanceVertex();
	}

	meshBuilder.End();
	m_pMesh->Draw();
}


void CMatSystemSurface::DrawTexturedPolyLine( const vgui::Vertex_t *p,int n )
{
	int iPrev = n - 1;
	for ( int i=0; i < n; i++ )
	{
		DrawTexturedLine( p[iPrev], p[i] );
		iPrev = i;
	}
}


//-----------------------------------------------------------------------------
// Draws a quad: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawQuad( const vgui::Vertex_t &ul, const vgui::Vertex_t &lr, unsigned char *pColor )
{
	Assert( !m_bIn3DPaintMode );

	if ( !m_pMesh )
		return;

	meshBuilder.Begin( m_pMesh, MATERIAL_QUADS, 1 );

	meshBuilder.Position3f( ul.m_Position.x, ul.m_Position.y, m_flZPos );
	meshBuilder.Color4ubv( pColor );
	meshBuilder.TexCoord2f( 0, ul.m_TexCoord.x, ul.m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.Position3f( lr.m_Position.x, ul.m_Position.y, m_flZPos );
	meshBuilder.Color4ubv( pColor );
	meshBuilder.TexCoord2f( 0, lr.m_TexCoord.x, ul.m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.Position3f( lr.m_Position.x, lr.m_Position.y, m_flZPos );
	meshBuilder.Color4ubv( pColor );
	meshBuilder.TexCoord2f( 0, lr.m_TexCoord.x, lr.m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.Position3f( ul.m_Position.x, lr.m_Position.y, m_flZPos );
	meshBuilder.Color4ubv( pColor );
	meshBuilder.TexCoord2f( 0, ul.m_TexCoord.x, lr.m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.End();
	m_pMesh->Draw();
}


//-----------------------------------------------------------------------------
// Purpose: Draws an array of quads
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawQuadArray( int quadCount, vgui::Vertex_t *pVerts, unsigned char *pColor )
{
	Assert( !m_bIn3DPaintMode );

	if ( !m_pMesh )
		return;

	meshBuilder.Begin( m_pMesh, MATERIAL_QUADS, quadCount );

	vgui::Vertex_t ulc, lrc;
	for (int i = 0; i < quadCount; ++i )
	{
		vgui::Vertex_t &ul = pVerts[2*i];
		vgui::Vertex_t &lr = pVerts[2*i + 1];

		if ( !ClipRect( ul, lr, &ulc, &lrc ) )
			continue;	

		meshBuilder.Position3f( ulc.m_Position.x, ulc.m_Position.y, m_flZPos );
		meshBuilder.Color4ubv( pColor );
		meshBuilder.TexCoord2f( 0, ulc.m_TexCoord.x, ulc.m_TexCoord.y );
		meshBuilder.AdvanceVertex();

		meshBuilder.Position3f( lrc.m_Position.x, ulc.m_Position.y, m_flZPos );
		meshBuilder.Color4ubv( pColor );
		meshBuilder.TexCoord2f( 0, lrc.m_TexCoord.x, ulc.m_TexCoord.y );
		meshBuilder.AdvanceVertex();

		meshBuilder.Position3f( lrc.m_Position.x, lrc.m_Position.y, m_flZPos );
		meshBuilder.Color4ubv( pColor );
		meshBuilder.TexCoord2f( 0, lrc.m_TexCoord.x, lrc.m_TexCoord.y );
		meshBuilder.AdvanceVertex();

		meshBuilder.Position3f( ulc.m_Position.x, lrc.m_Position.y, m_flZPos );
		meshBuilder.Color4ubv( pColor );
		meshBuilder.TexCoord2f( 0, ulc.m_TexCoord.x, lrc.m_TexCoord.y );
		meshBuilder.AdvanceVertex();
	}

	meshBuilder.End();
	m_pMesh->Draw();
}


//-----------------------------------------------------------------------------
// Purpose: Draws a rectangle colored with the current drawcolor
//		using the white material
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawFilledRect( int x0, int y0, int x1, int y1 )
{
	Assert( g_bInDrawing );

	// Don't even bother drawing fully transparent junk
	if( m_DrawColor[3]==0 )
		return;

	vgui::Vertex_t rect[2];
	vgui::Vertex_t clippedRect[2];
	InitVertex( rect[0], x0, y0, 0, 0 );
	InitVertex( rect[1], x1, y1, 0, 0 );

	// Fully clipped?
	if ( !ClipRect(rect[0], rect[1], &clippedRect[0], &clippedRect[1]) )
		return;	
	
	InternalSetMaterial();
	DrawQuad( clippedRect[0], clippedRect[1], m_DrawColor );
}

//-----------------------------------------------------------------------------
// Purpose: Draws an array of rectangles colored with the current drawcolor
//		using the white material
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawFilledRectArray( IntRect *pRects, int numRects )
{
	Assert( g_bInDrawing );

	// Don't even bother drawing fully transparent junk
	if( m_DrawColor[3]==0 )
		return;

	if ( !m_pMesh )
		return;

	InternalSetMaterial( );

	meshBuilder.Begin( m_pMesh, MATERIAL_QUADS, numRects );

	for (int i = 0; i < numRects; ++i )
	{
		vgui::Vertex_t rect[2];
		vgui::Vertex_t clippedRect[2];
		InitVertex( rect[0], pRects[i].x0, pRects[i].y0, 0, 0 );
		InitVertex( rect[1], pRects[i].x1, pRects[i].y1, 0, 0 );
		
		ClipRect( rect[0], rect[1], &clippedRect[0], &clippedRect[1] );
	
		vgui::Vertex_t &ul = clippedRect[0];
		vgui::Vertex_t &lr = clippedRect[1];

		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.Position3f( ul.m_Position.x, ul.m_Position.y, m_flZPos );
		meshBuilder.AdvanceVertex();

		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.Position3f( lr.m_Position.x, ul.m_Position.y, m_flZPos );
		meshBuilder.AdvanceVertex();

		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.Position3f( lr.m_Position.x, lr.m_Position.y, m_flZPos );
		meshBuilder.AdvanceVertex();

		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.Position3f( ul.m_Position.x, lr.m_Position.y, m_flZPos );
		meshBuilder.AdvanceVertex();
	}

	meshBuilder.End();
	m_pMesh->Draw();
}

//-----------------------------------------------------------------------------
// Draws a fade with the current draw color oriented according to argument
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawFilledRectFade( int x0, int y0, int x1, int y1, unsigned int alpha0, unsigned int alpha1, bool bHorizontal )
{
	Assert( g_bInDrawing );

	// Scale the desired alphas by the surface alpha
	float alphaScale = m_DrawColor[3] / 255.f;
	alpha0 *= alphaScale;
	alpha1 *= alphaScale;

	// Don't even bother drawing fully transparent junk
	if ( alpha0 == 0 && alpha1 == 0 )
		return;

	vgui::Vertex_t rect[2];
	vgui::Vertex_t clippedRect[2];
	InitVertex( rect[0], x0, y0, 0, 0 );
	InitVertex( rect[1], x1, y1, 0, 0 );

	// Fully clipped?
	if ( !ClipRect(rect[0], rect[1], &clippedRect[0], &clippedRect[1]) )
		return;	
	
	InternalSetMaterial();

	unsigned char colors[4][4] = {0};
	for ( int i=0; i<4; i++ )
	{
		// copy the rgb and leave the alpha at zero
		Q_memcpy( colors[i], m_DrawColor, 3 );
	}

	unsigned char nAlpha0 = (alpha0 & 0xFF);
	unsigned char nAlpha1 = (alpha1 & 0xFF);

	if ( bHorizontal )
	{
		// horizontal fade
		colors[0][3] = nAlpha0;
		colors[1][3] = nAlpha1;
		colors[2][3] = nAlpha1;
		colors[3][3] = nAlpha0;
	}
	else
	{
		// vertical fade
		colors[0][3] = nAlpha0;
		colors[1][3] = nAlpha0;
		colors[2][3] = nAlpha1;
		colors[3][3] = nAlpha1;
	}

	meshBuilder.Begin( m_pMesh, MATERIAL_QUADS, 1 );

	meshBuilder.Color4ubv( colors[0] );
	meshBuilder.Position3f( clippedRect[0].m_Position.x, clippedRect[0].m_Position.y, m_flZPos );
	meshBuilder.TexCoord2f( 0, clippedRect[0].m_TexCoord.x, clippedRect[0].m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.Color4ubv( colors[1] );
	meshBuilder.Position3f( clippedRect[1].m_Position.x, clippedRect[0].m_Position.y, m_flZPos );
	meshBuilder.TexCoord2f( 0, clippedRect[1].m_TexCoord.x, clippedRect[0].m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.Color4ubv( colors[2] );
	meshBuilder.Position3f( clippedRect[1].m_Position.x, clippedRect[1].m_Position.y, m_flZPos );
	meshBuilder.TexCoord2f( 0, clippedRect[1].m_TexCoord.x, clippedRect[1].m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.Color4ubv( colors[3] );
	meshBuilder.Position3f( clippedRect[0].m_Position.x, clippedRect[1].m_Position.y, m_flZPos );
	meshBuilder.TexCoord2f( 0, clippedRect[0].m_TexCoord.x, clippedRect[1].m_TexCoord.y );
	meshBuilder.AdvanceVertex();

	meshBuilder.End();
	m_pMesh->Draw();
}

//-----------------------------------------------------------------------------
// Purpose: Draws an unfilled rectangle in the current drawcolor
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawOutlinedRect(int x0,int y0,int x1,int y1)
{		
	// Don't even bother drawing fully transparent junk
	if ( m_DrawColor[3] == 0 )
		return;

	DrawFilledRect(x0,y0,x1,y0+1);     //top
	DrawFilledRect(x0,y1-1,x1,y1);	   //bottom
	DrawFilledRect(x0,y0+1,x0+1,y1-1); //left
	DrawFilledRect(x1-1,y0+1,x1,y1-1); //right
}


//-----------------------------------------------------------------------------
// Purpose: Draws an outlined circle in the current drawcolor
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawOutlinedCircle(int x, int y, int radius, int segments)
{
	Assert( g_bInDrawing );

	Assert( !m_bIn3DPaintMode );

	// Don't even bother drawing fully transparent junk
	if( m_DrawColor[3]==0 )
		return;

	// NOTE: Gotta use lines instead of linelist or lineloop due to clipping
	InternalSetMaterial( );
	meshBuilder.Begin( m_pMesh, MATERIAL_LINES, segments );

	vgui::Vertex_t renderVertex[2];
	vgui::Vertex_t vertex[2];
	vertex[0].m_Position.Init( m_nTranslateX + x + radius, m_nTranslateY + y );
	vertex[0].m_TexCoord.Init( 1.0f, 0.5f );

	float invDelta = 2.0f * M_PI / segments;
	for ( int i = 1; i <= segments; ++i )
	{
		float flRadians = i * invDelta;
		float ca = cos( flRadians );
		float sa = sin( flRadians );
					 
		// Rotate it around the circle
		vertex[1].m_Position.x = m_nTranslateX + x + (radius * ca);
		vertex[1].m_Position.y = m_nTranslateY + y + (radius * sa);
		vertex[1].m_TexCoord.x = 0.5f * (ca + 1.0f);
		vertex[1].m_TexCoord.y = 0.5f * (sa + 1.0f);

		if (ClipLine( vertex, renderVertex ))
		{
			meshBuilder.Color4ubv( m_DrawColor );
			meshBuilder.TexCoord2fv( 0, renderVertex[0].m_TexCoord.Base() );
			meshBuilder.Position3f( renderVertex[0].m_Position.x, renderVertex[0].m_Position.y, m_flZPos );
			meshBuilder.AdvanceVertex();

			meshBuilder.Color4ubv( m_DrawColor );
			meshBuilder.TexCoord2fv( 0, renderVertex[1].m_TexCoord.Base() );
			meshBuilder.Position3f( renderVertex[1].m_Position.x, renderVertex[1].m_Position.y, m_flZPos );
			meshBuilder.AdvanceVertex();
		}

		vertex[0].m_Position = vertex[1].m_Position;
		vertex[0].m_TexCoord = vertex[1].m_TexCoord;
	}

	meshBuilder.End();
	m_pMesh->Draw();
}


//-----------------------------------------------------------------------------
// Loads a particular texture (material)
//-----------------------------------------------------------------------------
int CMatSystemSurface::CreateNewTextureID( bool procedural /*=false*/ )
{
	return TextureDictionary()->CreateTexture( procedural );
}

#ifdef _X360
void CMatSystemSurface::DestroyTextureID( int id )
{
	TextureDictionary()->DestroyTexture( id );
}
#endif

#ifdef _X360
void CMatSystemSurface::UncacheUnusedMaterials()
{
	// unbind any currently set texture (which may be uncached)
	DrawSetTexture( -1 );

	// X360TBD: Need to only destroy "marked" textures
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : id - 
//			*filename - 
//			maxlen - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CMatSystemSurface::DrawGetTextureFile(int id, char *filename, int maxlen )
{
	if ( !TextureDictionary()->IsValidId( id ) )
		return false;

	IMaterial *texture = TextureDictionary()->GetTextureMaterial(id);
	if ( !texture )
		return false;

	Q_strncpy( filename, texture->GetName(), maxlen );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : id - texture id
// Output : returns IMaterial for the referenced texture
//-----------------------------------------------------------------------------
IVguiMatInfo *CMatSystemSurface::DrawGetTextureMatInfoFactory(int id)
{
	if ( !TextureDictionary()->IsValidId( id ) )
		return NULL;

	IMaterial *texture = TextureDictionary()->GetTextureMaterial(id);

	if ( texture == NULL )
		return NULL;

	return new CVguiMatInfo(texture);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *filename - 
// Output : int
//-----------------------------------------------------------------------------
int CMatSystemSurface::DrawGetTextureId( char const *filename )
{
	return TextureDictionary()->FindTextureIdForTextureFile( filename );
}

//-----------------------------------------------------------------------------
// Associates a texture with a material file (also binds it)
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextureFile(int id, const char *pFileName, int hardwareFilter, bool forceReload /*= false*/)
{
	TextureDictionary()->BindTextureToFile( id, pFileName );
	DrawSetTexture( id );
}


//-----------------------------------------------------------------------------
// Associates a texture with a material file (also binds it)
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextureMaterial(int id, IMaterial *pMaterial)
{
	TextureDictionary()->BindTextureToMaterial( id, pMaterial );
	DrawSetTexture( id );
}


void CMatSystemSurface::ReferenceProceduralMaterial( int id, int referenceId, IMaterial *pMaterial )
{
	TextureDictionary()->BindTextureToMaterialReference( id, referenceId, pMaterial );
}


//-----------------------------------------------------------------------------
// Binds a texture
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTexture( int id )
{
	// if we're switching textures, flush any batched text
	if ( id != m_iBoundTexture )
	{
		DrawFlushText();
		m_iBoundTexture = id;

		if ( IsX360() && id == -1 )
		{
			// ensure we unbind current material that may go away
			CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
			pRenderContext->Bind( m_pWhite );
		}
	}
}


//-----------------------------------------------------------------------------
// Returns texture size
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawGetTextureSize(int id, int &iWide, int &iTall)
{
	TextureDictionary()->GetTextureSize( id, iWide, iTall );
}


//-----------------------------------------------------------------------------
// Draws a textured rectangle
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawTexturedRect( int x0, int y0, int x1, int y1 )
{
	Assert( g_bInDrawing );

	// Don't even bother drawing fully transparent junk
	if( m_DrawColor[3] == 0 )
		return;

	float s0, t0, s1, t1;
	TextureDictionary()->GetTextureTexCoords( m_iBoundTexture, s0, t0, s1, t1 );

	vgui::Vertex_t rect[2];
	vgui::Vertex_t clippedRect[2];
	InitVertex( rect[0], x0, y0, s0, t0 );
	InitVertex( rect[1], x1, y1, s1, t1 );

	// Fully clipped?
	if ( !ClipRect(rect[0], rect[1], &clippedRect[0], &clippedRect[1]) )
		return;	

	IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(m_iBoundTexture);
	InternalSetMaterial( pMaterial );
	DrawQuad( clippedRect[0], clippedRect[1], m_DrawColor );
}

//-----------------------------------------------------------------------------
// Draws a textured rectangle
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawTexturedSubRect( int x0, int y0, int x1, int y1, float texs0, float text0, float texs1, float text1 )
{
	Assert( g_bInDrawing );

	// Don't even bother drawing fully transparent junk
	if( m_DrawColor[3] == 0 )
		return;

	float s0, t0, s1, t1;
	TextureDictionary()->GetTextureTexCoords( m_iBoundTexture, s0, t0, s1, t1 );

	float ssize = s1 - s0;
	float tsize = t1 - t0;

	// Rescale tex values into range of s0 to s1 ,etc.
	texs0 = s0 + texs0 * ( ssize );
	texs1 = s0 + texs1 * ( ssize );
	text0 = t0 + text0 * ( tsize );
	text1 = t0 + text1 * ( tsize );

	vgui::Vertex_t rect[2];
	vgui::Vertex_t clippedRect[2];
	InitVertex( rect[0], x0, y0, texs0, text0 );
	InitVertex( rect[1], x1, y1, texs1, text1 );

	// Fully clipped?
	if ( !ClipRect(rect[0], rect[1], &clippedRect[0], &clippedRect[1]) )
		return;	

	IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(m_iBoundTexture);
	InternalSetMaterial( pMaterial );
	DrawQuad( clippedRect[0], clippedRect[1], m_DrawColor );
}

//-----------------------------------------------------------------------------
// Draws a textured polygon
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawTexturedPolygon(int n, Vertex_t *pVertices)
{
	Assert( !m_bIn3DPaintMode );

	Assert( g_bInDrawing );

	// Don't even bother drawing fully transparent junk
	if( (n == 0) || (m_DrawColor[3]==0) )
		return;

	int iCount;
	Vertex_t **ppClippedVerts;

	// Clip vertices...
	iCount = ClipPolygon( n, pVertices, m_nTranslateX, m_nTranslateY, &ppClippedVerts );
	if (iCount <= 0)
		return;

	IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(m_iBoundTexture);
	InternalSetMaterial( pMaterial );

	meshBuilder.Begin( m_pMesh, MATERIAL_POLYGON, iCount );

	for (int i = 0; i < iCount; ++i)
	{
		meshBuilder.Color4ubv( m_DrawColor );
		meshBuilder.TexCoord2fv( 0, ppClippedVerts[i]->m_TexCoord.Base() );
		meshBuilder.Position3f( ppClippedVerts[i]->m_Position.x, ppClippedVerts[i]->m_Position.y, m_flZPos );
		meshBuilder.AdvanceVertex();
	}

	meshBuilder.End();
	m_pMesh->Draw();
}



//-----------------------------------------------------------------------------
//
// Font-related methods begin here
//
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// Purpose: creates a new empty font
//-----------------------------------------------------------------------------
HFont CMatSystemSurface::CreateFont()
{
	return FontManager().CreateFont();
}

//-----------------------------------------------------------------------------
// Purpose: adds glyphs to a font created by CreateFont()
//-----------------------------------------------------------------------------
bool CMatSystemSurface::SetFontGlyphSet(HFont font, const char *windowsFontName, int tall, int weight, int blur, int scanlines, int flags)
{
	return FontManager().SetFontGlyphSet(font, windowsFontName, tall, weight, blur, scanlines, flags);
}

//-----------------------------------------------------------------------------
// Purpose: adds glyphs to a font created by CreateFont()
//-----------------------------------------------------------------------------
bool CMatSystemSurface::SetBitmapFontGlyphSet(HFont font, const char *windowsFontName, float scalex, float scaley, int flags)
{
	return FontManager().SetBitmapFontGlyphSet(font, windowsFontName, scalex, scaley, flags);
}

//-----------------------------------------------------------------------------
// Purpose: returns the max height of a font
//-----------------------------------------------------------------------------
int CMatSystemSurface::GetFontTall(HFont font)
{
	return FontManager().GetFontTall(font);
}

//-----------------------------------------------------------------------------
// Purpose: returns the max height of a font
//-----------------------------------------------------------------------------
int CMatSystemSurface::GetFontAscent(HFont font, wchar_t wch)
{
	return FontManager().GetFontAscent(font,wch);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : font - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CMatSystemSurface::IsFontAdditive(HFont font)
{
	return FontManager().IsFontAdditive(font);
}

//-----------------------------------------------------------------------------
// Purpose: returns the abc widths of a single character
//-----------------------------------------------------------------------------
void CMatSystemSurface::GetCharABCwide(HFont font, int ch, int &a, int &b, int &c)
{
	FontManager().GetCharABCwide(font, ch, a, b, c);
}

//-----------------------------------------------------------------------------
// Purpose: returns the pixel width of a single character
//-----------------------------------------------------------------------------
int CMatSystemSurface::GetCharacterWidth(HFont font, int ch)
{
	return FontManager().GetCharacterWidth(font, ch);
}

//-----------------------------------------------------------------------------
// Purpose: returns the area of a text string, including newlines
//-----------------------------------------------------------------------------
void CMatSystemSurface::GetTextSize(HFont font, const wchar_t *text, int &wide, int &tall)
{
	FontManager().GetTextSize(font, text, wide, tall);
}

//-----------------------------------------------------------------------------
// Purpose: adds a custom font file (only supports true type font files (.ttf) for now)
//-----------------------------------------------------------------------------
bool CMatSystemSurface::AddCustomFontFile( const char *fontFileName )
{
	char fullPath[MAX_PATH];
	bool bFound = false;
	if ( IsPC() )
	{
		// windows needs an absolute path for ttf
		bFound = g_pFullFileSystem->GetLocalPath( fontFileName, fullPath, sizeof( fullPath ) );
	}	
	else
	{
		bFound = ( ( g_pFullFileSystem->GetDVDMode() == DVDMODE_STRICT ) || g_pFullFileSystem->FileExists( fontFileName, "GAME" ) );
		Q_strncpy( fullPath, fontFileName, MAX_PATH );

	}
	if ( !bFound )
	{
		Msg( "Couldn't find custom font file '%s'\n", fontFileName );
		return false;
	}

	// only add if it's not already in the list
	Q_strlower( fullPath );
	CUtlSymbol sym(fullPath);
	int i;
	for ( i = 0; i < m_CustomFontFileNames.Count(); i++ )
	{
		if ( m_CustomFontFileNames[i] == sym )
			break;
	}
	if ( !m_CustomFontFileNames.IsValidIndex( i ) )
	{
	 	m_CustomFontFileNames.AddToTail( fullPath );

		if ( IsPC() )
		{
			// make sure it's on disk
			// only do this once for each font since in steam it will overwrite the
			// registered font file, causing windows to invalidate the font
			g_pFullFileSystem->GetLocalCopy( fullPath );
		}
	}

	// try and use the optimal custom font loader, will makes sure fonts are unloaded properly
	// this function is in a newer version of the gdi library (win2k+), so need to try get it directly
#if !defined( _X360 )
	bool successfullyAdded = false;
	HMODULE gdiModule = ::LoadLibrary("gdi32.dll");
	if (gdiModule)
	{
		typedef int (WINAPI *AddFontResourceExProc)(LPCTSTR, DWORD, PVOID);
		AddFontResourceExProc pAddFontResourceEx = (AddFontResourceExProc)::GetProcAddress(gdiModule, "AddFontResourceExA");
		if (pAddFontResourceEx)
		{
			int result = (*pAddFontResourceEx)(fullPath, 0x10, NULL);
			if (result > 0)
			{
				successfullyAdded = true;
			}
		}
		::FreeLibrary(gdiModule);
	}

	// add to windows
	bool success = successfullyAdded || (::AddFontResource(fullPath) > 0);
	if ( !success )
	{
		Msg( "Failed to load custom font file '%s'\n", fullPath );
	}
	Assert( success );
	return success;
#else
	return true;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: adds a bitmap font file
//-----------------------------------------------------------------------------
bool CMatSystemSurface::AddBitmapFontFile( const char *fontFileName )
{
	bool bFound = false;
	bFound = ( ( g_pFullFileSystem->GetDVDMode() == DVDMODE_STRICT ) || g_pFullFileSystem->FileExists( fontFileName, IsX360() ? "GAME" : NULL ) );
	if ( !bFound )
	{
		Msg( "Couldn't find bitmap font file '%s'\n", fontFileName );
		return false;
	}
	char path[MAX_PATH];
	Q_strncpy( path, fontFileName, MAX_PATH );

	// only add if it's not already in the list
	Q_strlower( path );
	CUtlSymbol sym( path );
	int i;
	for ( i = 0; i < m_BitmapFontFileNames.Count(); i++ )
	{
		if ( m_BitmapFontFileNames[i] == sym )
			break;
	}
	if ( !m_BitmapFontFileNames.IsValidIndex( i ) )
	{
	 	m_BitmapFontFileNames.AddToTail( path );

		if ( IsPC() )
		{
			// make sure it's on disk
			// only do this once for each font since in steam it will overwrite the
			// registered font file, causing windows to invalidate the font
			g_pFullFileSystem->GetLocalCopy( path );
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetBitmapFontName( const char *pName, const char *pFontFilename )
{
	char fontPath[MAX_PATH];
	Q_strncpy( fontPath, pFontFilename, MAX_PATH );
	Q_strlower( fontPath );

	CUtlSymbol sym( fontPath );
	int i;
	for (i = 0; i < m_BitmapFontFileNames.Count(); i++)
	{
		if ( m_BitmapFontFileNames[i] == sym )
		{
			// found it, update the mapping
			int index = m_BitmapFontFileMapping.Find( pName );
			if ( !m_BitmapFontFileMapping.IsValidIndex( index ) )
			{
				index = m_BitmapFontFileMapping.Insert( pName );	
			}
			m_BitmapFontFileMapping.Element( index ) = i;
			break;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
const char *CMatSystemSurface::GetBitmapFontName( const char *pName )
{
	// find it in the mapping symbol table
	int index = m_BitmapFontFileMapping.Find( pName );
	if ( index == m_BitmapFontFileMapping.InvalidIndex() )
	{
		return "";
	}

	return m_BitmapFontFileNames[m_BitmapFontFileMapping.Element( index )].String();
}

void CMatSystemSurface::ClearTemporaryFontCache( void )
{
	FontManager().ClearTemporaryFontCache();
}

//-----------------------------------------------------------------------------
// Purpose: Force a set of characters to be rendered into the font page.
//-----------------------------------------------------------------------------
void CMatSystemSurface::PrecacheFontCharacters( HFont font, wchar_t *pCharacterString )
{
	if ( !pCharacterString || !pCharacterString[0] )
		return;

	StartDrawing();
	DrawSetTextFont( font );

	int numChars = 0;
	while( pCharacterString[ numChars ] )
	{
		numChars++;
	}
	int *pTextureIDs_ignored = (int *)_alloca( numChars*sizeof( int ) );
	float **pTexCoords_ignored = (float **)_alloca( numChars*sizeof( float * ) );
	g_FontTextureCache.GetTextureForChars( m_hCurrentFont, FONT_DRAW_DEFAULT, pCharacterString, pTextureIDs_ignored, pTexCoords_ignored, numChars );

	FinishDrawing();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextFont(HFont font)
{
	Assert( g_bInDrawing );

	m_hCurrentFont = font;
}

//-----------------------------------------------------------------------------
// Purpose: Renders any batched up text
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawFlushText()
{
	if ( !m_BatchedCharVerts.Count() )
		return;

	IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(m_iBoundTexture);
	InternalSetMaterial( pMaterial );
	DrawQuadArray( m_BatchedCharVerts.Count() / 2, m_BatchedCharVerts.Base(), m_DrawTextColor );
	m_BatchedCharVerts.RemoveAll();
}

//-----------------------------------------------------------------------------
// Sets the text color
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextColor(int r, int g, int b, int a)
{
	int adjustedAlpha = (a * m_flAlphaMultiplier);
	if ( r != m_DrawTextColor[0] || g != m_DrawTextColor[1] || b != m_DrawTextColor[2] || adjustedAlpha != m_DrawTextColor[3] )
	{
		// text color changed, flush any existing text
		DrawFlushText();

		m_DrawTextColor[0] = (unsigned char)r;
		m_DrawTextColor[1] = (unsigned char)g;
		m_DrawTextColor[2] = (unsigned char)b;
		m_DrawTextColor[3] = (unsigned char)adjustedAlpha;
	}
}

//-----------------------------------------------------------------------------
// Purpose: alternate color set
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextColor(Color col)
{
	DrawSetTextColor(col[0], col[1], col[2], col[3]);
}

//-----------------------------------------------------------------------------
// Purpose: change the scale of a bitmap font
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextScale(float sx, float sy)
{
	FontManager().SetFontScale( m_hCurrentFont, sx, sy );
}

//-----------------------------------------------------------------------------
// Text rendering location
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetTextPos(int x, int y)
{
	Assert( g_bInDrawing );

	m_pDrawTextPos[0] = x;
	m_pDrawTextPos[1] = y;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawGetTextPos(int& x,int& y)
{
	Assert( g_bInDrawing );

	x = m_pDrawTextPos[0];
	y = m_pDrawTextPos[1];
}

#pragma warning( disable : 4706 )
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawUnicodeString( const wchar_t *pString, FontDrawType_t drawType /*= FONT_DRAW_DEFAULT */ )
{
	wchar_t	ch;

	while ( ( ch = *pString++ ) )
	{
		DrawUnicodeChar( ch );	
	}
}
#pragma warning( default : 4706 )

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawUnicodeChar(wchar_t ch, FontDrawType_t drawType /*= FONT_DRAW_DEFAULT */ )
{
	CharRenderInfo info;
	info.drawType = drawType;
	if ( DrawGetUnicodeCharRenderInfo( ch, info ) )
	{
		DrawRenderCharFromInfo( info );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CMatSystemSurface::DrawGetUnicodeCharRenderInfo( wchar_t ch, CharRenderInfo& info )
{
	Assert( g_bInDrawing );
	info.valid = false;

	if ( !m_hCurrentFont )
	{
		return info.valid;
	}

	info.valid = true;
	info.ch = ch;
	DrawGetTextPos(info.x, info.y);

	info.currentFont = m_hCurrentFont;
	info.fontTall = GetFontTall(m_hCurrentFont);

	GetCharABCwide(m_hCurrentFont, ch, info.abcA, info.abcB, info.abcC);
	bool bUnderlined = FontManager().GetFontUnderlined( m_hCurrentFont );
	
	// Do prestep before generating texture coordinates, etc.
	if ( !bUnderlined )
	{
		info.x += info.abcA;
	}

	// get the character texture from the cache
	info.textureId = 0;
	float *texCoords = NULL;
	if (!g_FontTextureCache.GetTextureForChar(m_hCurrentFont, info.drawType, ch, &info.textureId, &texCoords))
	{
		info.valid = false;
		return info.valid;
	}

	int fontWide = info.abcB;
	if ( bUnderlined )
	{
		fontWide += ( info.abcA + info.abcC );
		info.x-= info.abcA;
	}

	InitVertex( info.verts[0], info.x, info.y, texCoords[0], texCoords[1] );
	InitVertex( info.verts[1], info.x + fontWide, info.y + info.fontTall, texCoords[2], texCoords[3] );

	info.shouldclip = true;

	return info.valid;
}

//-----------------------------------------------------------------------------
// Purpose: batches up characters for rendering
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawRenderCharInternal( const CharRenderInfo& info )
{
	Assert( g_bInDrawing );

	// draw try to draw fully transparent characters
	if ( m_DrawTextColor[3] == 0 )
		return;

	vgui::Vertex_t clippedRect[2];

	if ( info.shouldclip )
	{
		// Fully clipped?
		if ( !ClipRect(info.verts[0], info.verts[1], &clippedRect[0], &clippedRect[1]) )
			return;	
	}
	else
	{
		clippedRect[0] = info.verts[0];
		clippedRect[1] = info.verts[1];
	}

	if ( m_BatchedCharVerts.Count() > MAX_BATCHED_CHAR_VERTS )
	{
		DrawFlushText();
	}

	// batch
	m_BatchedCharVerts.AddToTail( clippedRect[0] );
	m_BatchedCharVerts.AddToTail( clippedRect[1] );

	// old drawing code, only left in to help debugging batching
//	IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(m_iBoundTexture);
//	InternalSetMaterial( pMaterial );
//	DrawQuad( clippedRect[0], clippedRect[1], m_DrawTextColor );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawRenderCharFromInfo( const CharRenderInfo& info )
{
	if ( !info.valid )
		return;

	int x = info.x;

	// get the character texture from the cache
	DrawSetTexture( info.textureId );
	
	DrawRenderCharInternal( info );

	// Only do post step
	x += ( info.abcB + info.abcC );

	// Update cursor pos
	DrawSetTextPos(x, info.y);
}

//-----------------------------------------------------------------------------
// Renders a text buffer
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawPrintText(const wchar_t *text, int iTextLen, FontDrawType_t drawType /*= FONT_DRAW_DEFAULT */ )
{
	Assert( g_bInDrawing );
	if (!text)
		return;

	if (!m_hCurrentFont)
		return;

	int x = m_pDrawTextPos[0] + m_nTranslateX;
	int y = m_pDrawTextPos[1] + m_nTranslateY;

	int iTall = GetFontTall(m_hCurrentFont);
	int iLastTexId = -1;

	int iCount = 0;
	vgui::Vertex_t *pQuads = (vgui::Vertex_t*)stackalloc((2 * iTextLen) * sizeof(vgui::Vertex_t) );

	int iTotalWidth = 0;
	for (int i=0; i<iTextLen; ++i)
	{
		wchar_t ch = text[i];

		bool bUnderlined = FontManager().GetFontUnderlined( m_hCurrentFont );
		int abcA,abcB,abcC;
		GetCharABCwide(m_hCurrentFont, ch, abcA, abcB, abcC);

		iTotalWidth += abcA;
		int iWide = abcB;
		if ( bUnderlined )
		{
			iWide += ( abcA + abcC );
			x-= abcA;
		}

		if ( !iswspace( ch ) || bUnderlined )
		{
			// get the character texture from the cache
			int iTexId = 0;
			float *texCoords = NULL;
			if (!g_FontTextureCache.GetTextureForChar(m_hCurrentFont, drawType, ch, &iTexId, &texCoords))
				continue;

			Assert( texCoords );

			if (iTexId != iLastTexId)
			{
				// FIXME: At the moment, we just draw all the batched up
				// text when the font changes. We Should batch up per material
				// and *then* draw
				if (iCount)
				{
					IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(iLastTexId);
					InternalSetMaterial( pMaterial );
					DrawQuadArray( iCount, pQuads, m_DrawTextColor );
					iCount = 0;
				}

				iLastTexId = iTexId;
			}

 			vgui::Vertex_t &ul = pQuads[2*iCount];
 			vgui::Vertex_t &lr = pQuads[2*iCount + 1];
			++iCount;

			ul.m_Position.x = x + iTotalWidth;
			ul.m_Position.y = y;
			lr.m_Position.x = ul.m_Position.x + iWide;
			lr.m_Position.y = ul.m_Position.y + iTall;

			// Gets at the texture coords for this character in its texture page
			ul.m_TexCoord[0] = texCoords[0];
			ul.m_TexCoord[1] = texCoords[1];
			lr.m_TexCoord[0] = texCoords[2];
			lr.m_TexCoord[1] = texCoords[3];
		}

		iTotalWidth += iWide + abcC;
		if ( bUnderlined )
		{
			iTotalWidth -= abcC;
		}
	}

	// Draw any left-over characters
	if (iCount)
	{
		IMaterial *pMaterial = TextureDictionary()->GetTextureMaterial(iLastTexId);
		InternalSetMaterial( pMaterial );
		DrawQuadArray( iCount, pQuads, m_DrawTextColor );
	}

	m_pDrawTextPos[0] += iTotalWidth;

	stackfree(pQuads);
}


//-----------------------------------------------------------------------------
// Returns the screen size
//-----------------------------------------------------------------------------
void CMatSystemSurface::GetScreenSize(int &iWide, int &iTall)
{
	int x, y;
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	pRenderContext->GetViewport( x, y, iWide, iTall );
}

//-----------------------------------------------------------------------------
// Purpose: Notification of a new screen size
//-----------------------------------------------------------------------------
void CMatSystemSurface::OnScreenSizeChanged( int nOldWidth, int nOldHeight )
{
	int iNewWidth, iNewHeight;
	GetScreenSize( iNewWidth, iNewHeight );

	Msg( "Changing resolutions from (%d, %d) -> (%d, %d)\n", nOldWidth, nOldHeight, iNewWidth, iNewHeight );

	// update the root panel size
	ipanel()->SetSize(m_pEmbeddedPanel, iNewWidth, iNewHeight);

	// clear font texture cache
	g_FontTextureCache.Clear();
	m_iBoundTexture = -1;

	// reload fonts
	FontManager().ClearAllFonts();
	scheme()->ReloadFonts();

	// notify every panel
	VPANEL panel = GetEmbeddedPanel();
	ivgui()->PostMessage(panel, new KeyValues("OnScreenSizeChanged", "oldwide", nOldWidth, "oldtall", nOldHeight), NULL);
}

//-----------------------------------------------------------------------------
// Returns the size of the embedded panel
//-----------------------------------------------------------------------------
void CMatSystemSurface::GetWorkspaceBounds(int &x, int &y, int &iWide, int &iTall)
{
	// NOTE: This is equal to the viewport size by default,
	// but other embedded panels can be used
	x = m_WorkSpaceInsets[0];
	y = m_WorkSpaceInsets[1];
	g_pVGuiPanel->GetSize(m_pEmbeddedPanel, iWide, iTall);

	iWide -= m_WorkSpaceInsets[2];
	iTall -= m_WorkSpaceInsets[3];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetWorkspaceInsets( int left, int top, int right, int bottom )
{
	m_WorkSpaceInsets[0] = left;
	m_WorkSpaceInsets[1] = top;
	m_WorkSpaceInsets[2] = right;
	m_WorkSpaceInsets[3] = bottom;
}

//-----------------------------------------------------------------------------
// A bunch of methods needed for the windows version only
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetAsTopMost(VPANEL panel, bool state)
{
}

void CMatSystemSurface::SetAsToolBar(VPANEL panel, bool state)		// removes the window's task bar entry (for context menu's, etc.)
{
}

void CMatSystemSurface::SetForegroundWindow (VPANEL panel)
{
	BringToFront(panel);
}

void CMatSystemSurface::SetPanelVisible(VPANEL panel, bool state)
{
}

void CMatSystemSurface::SetMinimized(VPANEL panel, bool state)
{
	if (state)
	{
		g_pVGuiPanel->SetPlat(panel, VPANEL_MINIMIZED);
		g_pVGuiPanel->SetVisible(panel, false);
	}
	else
	{
		g_pVGuiPanel->SetPlat(panel, VPANEL_NORMAL);
	}
}

bool CMatSystemSurface::IsMinimized(vgui::VPANEL panel)
{
	return (g_pVGuiPanel->Plat(panel) == VPANEL_MINIMIZED);

}

void CMatSystemSurface::FlashWindow(VPANEL panel, bool state)
{
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetTitle(VPANEL panel, const wchar_t *title)
{
	int entry = GetTitleEntry( panel );
	if ( entry == -1 )
	{
		entry = m_Titles.AddToTail();
	}

	TitleEntry *e = &m_Titles[ entry ];
	Assert( e );
	wcsncpy( e->title, title, sizeof( e->title )/ sizeof( wchar_t ) );
	e->panel = panel;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
wchar_t const *CMatSystemSurface::GetTitle( VPANEL panel )
{
	int entry = GetTitleEntry( panel );
	if ( entry != -1 )
	{
		TitleEntry *e = &m_Titles[ entry ];
		return e->title;
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Private lookup method
// Input  : *panel - 
// Output : TitleEntry
//-----------------------------------------------------------------------------
int CMatSystemSurface::GetTitleEntry( vgui::VPANEL panel )
{
	for ( int i = 0; i < m_Titles.Count(); i++ )
	{
		TitleEntry* entry = &m_Titles[ i ];
		if ( entry->panel == panel )
			return i;
	}
	return -1;
}

void CMatSystemSurface::SwapBuffers(VPANEL panel)
{
}

void CMatSystemSurface::Invalidate(VPANEL panel)
{
}

void CMatSystemSurface::ApplyChanges()
{
}

// notify icons?!?
VPANEL CMatSystemSurface::GetNotifyPanel()
{
	return NULL;
}

void CMatSystemSurface::SetNotifyIcon(VPANEL context, HTexture icon, VPANEL panelToReceiveMessages, const char *text)
{
}

bool CMatSystemSurface::IsWithin(int x, int y)
{
	return true;
}

bool CMatSystemSurface::ShouldPaintChildPanel(VPANEL childPanel)
{
	if ( m_pRestrictedPanel && ( m_pRestrictedPanel != childPanel ) && 
		 !g_pVGuiPanel->HasParent( childPanel, m_pRestrictedPanel ) )
	{
		return false;
	}

	bool isPopup = ipanel()->IsPopup(childPanel);
	return !isPopup;
}

bool CMatSystemSurface::RecreateContext(VPANEL panel)
{
	return false;
}

//-----------------------------------------------------------------------------
// Focus-related methods
//-----------------------------------------------------------------------------
bool CMatSystemSurface::HasFocus()
{
	return true;
}

void CMatSystemSurface::BringToFront(VPANEL panel)
{
	// move panel to top of list
	g_pVGuiPanel->MoveToFront(panel);

	// move panel to top of popup list
	if ( g_pVGuiPanel->IsPopup( panel ) )
	{
		MovePopupToFront( panel );
	}
}


// engine-only focus handling (replacing WM_FOCUS windows handling)
void CMatSystemSurface::SetTopLevelFocus(VPANEL pSubFocus)
{
	// walk up the hierarchy until we find what popup panel belongs to
	while (pSubFocus)
	{
		if (ipanel()->IsPopup(pSubFocus) && ipanel()->IsMouseInputEnabled(pSubFocus))
		{
			BringToFront(pSubFocus);
			break;
		}
		
		pSubFocus = ipanel()->GetParent(pSubFocus);
	}
}


//-----------------------------------------------------------------------------
// Installs a function to play sounds
//-----------------------------------------------------------------------------
void CMatSystemSurface::InstallPlaySoundFunc( PlaySoundFunc_t soundFunc )
{
	m_PlaySoundFunc = soundFunc;
}


//-----------------------------------------------------------------------------
// plays a sound
//-----------------------------------------------------------------------------
void CMatSystemSurface::PlaySound(const char *pFileName)
{
	if (m_PlaySoundFunc)
		m_PlaySoundFunc( pFileName );
}


//-----------------------------------------------------------------------------
// handles mouse movement
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetCursorPos(int x, int y)
{
	CursorSetPos( m_HWnd, x, y );
}

void CMatSystemSurface::GetCursorPos(int &x, int &y)
{
	CursorGetPos( m_HWnd, x, y );
}

void CMatSystemSurface::SetCursor(HCursor hCursor)
{
	if ( _currentCursor != hCursor )
	{
		_currentCursor = hCursor;
		CursorSelect( hCursor );
	}
}

void CMatSystemSurface::EnableMouseCapture( VPANEL panel, bool state )
{
	if ( state )
	{
		::SetCapture( reinterpret_cast< HWND >( m_HWnd ) );
	}
	else
	{
		::ReleaseCapture();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Turns the panel into a standalone window
//-----------------------------------------------------------------------------
void CMatSystemSurface::CreatePopup(VPANEL panel, bool minimized,  bool showTaskbarIcon, bool disabled , bool mouseInput , bool kbInput)
{
	if (!g_pVGuiPanel->GetParent(panel))
	{
		g_pVGuiPanel->SetParent(panel, GetEmbeddedPanel());
	}
	((VPanel *)panel)->SetPopup(true);
	((VPanel *)panel)->SetKeyBoardInputEnabled(kbInput);
	((VPanel *)panel)->SetMouseInputEnabled(mouseInput);

	HPanel p = ivgui()->PanelToHandle( panel );

	if ( m_PopupList.Find( p ) == m_PopupList.InvalidIndex() )
	{
		m_PopupList.AddToTail( p );
	}
	else
	{
		MovePopupToFront( panel );
	}
}


//-----------------------------------------------------------------------------
// Create/destroy panels..
//-----------------------------------------------------------------------------
void CMatSystemSurface::AddPanel(VPANEL panel)
{
	if (g_pVGuiPanel->IsPopup(panel))
	{
		// turn it into a popup menu
		CreatePopup(panel, false);
	}
}

void CMatSystemSurface::ReleasePanel(VPANEL panel)
{
	// Remove from popup list if needed and remove any dead popups while we're at it
	RemovePopup( panel );

	int entry = GetTitleEntry( panel );
	if ( entry != -1 )
	{
		m_Titles.Remove( entry );
	}
}


//-----------------------------------------------------------------------------
// Popup accessors used by VGUI
//-----------------------------------------------------------------------------
int CMatSystemSurface::GetPopupCount(  )
{
	return m_PopupList.Count();
}

VPANEL CMatSystemSurface::GetPopup(  int index )
{
	HPanel p = m_PopupList[ index ];
	VPANEL panel = ivgui()->HandleToPanel( p );
	return panel;
}

void CMatSystemSurface::ResetPopupList(  )
{
	m_PopupList.RemoveAll();
}

void CMatSystemSurface::AddPopup( VPANEL panel )
{
	HPanel p = ivgui()->PanelToHandle( panel );

	if ( m_PopupList.Find( p ) == m_PopupList.InvalidIndex() )
	{
		m_PopupList.AddToTail( p );
	}
}


void CMatSystemSurface::RemovePopup( vgui::VPANEL panel )
{
	// Remove from popup list if needed and remove any dead popups while we're at it
	int c = GetPopupCount();

	for ( int i = c -  1; i >= 0 ; i-- )
	{
		VPANEL popup = GetPopup(i );
		if ( popup && ( popup != panel ) )
			continue;

		m_PopupList.Remove( i );
	}
}

//-----------------------------------------------------------------------------
// Methods associated with iterating + drawing the panel tree
//-----------------------------------------------------------------------------
void CMatSystemSurface::AddPopupsToList( VPANEL panel )
{
	if (!g_pVGuiPanel->IsVisible(panel))
		return;

	// Add to popup list as we visit popups
	// Note:  popup list is cleared in RunFrame which occurs before this call!!!
	if ( g_pVGuiPanel->IsPopup( panel ) )
	{
		AddPopup( panel );
	}

	int count = g_pVGuiPanel->GetChildCount(panel);
	for (int i = 0; i < count; ++i)
	{
		VPANEL child = g_pVGuiPanel->GetChild(panel, i);
		AddPopupsToList( child );
	}
}


//-----------------------------------------------------------------------------
// Purpose: recurses the panels calculating absolute positions
//			parents must be solved before children
//-----------------------------------------------------------------------------
void CMatSystemSurface::InternalSolveTraverse(VPANEL panel)
{
	// solve the parent
	((VPanel *)panel)->Solve();
	
	// now we can solve the children
	for (int i = 0; i < ((VPanel *)panel)->GetChildCount(); i++)
	{
		VPanel *child = ((VPanel *)panel)->GetChild(i);
		if (child->IsVisible())
		{
			InternalSolveTraverse((VPANEL)child);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: recurses the panels giving them a chance to do a user-defined think,
//			PerformLayout and ApplySchemeSettings
//			must be done child before parent
//-----------------------------------------------------------------------------
void CMatSystemSurface::InternalThinkTraverse(VPANEL panel)
{
	// think the parent
	((VPanel *)panel)->Client()->Think();

	// and then the children...
	for (int i = 0; i < ((VPanel *)panel)->GetChildCount(); i++)
	{
		VPanel *child = ((VPanel *)panel)->GetChild(i);
		if ( child->IsVisible() )
		{
			InternalThinkTraverse((VPANEL)child);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: recurses the panels giving them a chance to do apply settings,
//-----------------------------------------------------------------------------
void CMatSystemSurface::InternalSchemeSettingsTraverse(VPANEL panel, bool forceApplySchemeSettings)
{
	// apply to the children...
	for (int i = 0; i < ((VPanel *)panel)->GetChildCount(); i++)
	{
		VPanel *child = ((VPanel *)panel)->GetChild(i);
		if ( forceApplySchemeSettings || child->IsVisible() )
		{	
			InternalSchemeSettingsTraverse((VPANEL)child, forceApplySchemeSettings);
		}
	}
	// and then the parent
	((VPanel *)panel)->Client()->PerformApplySchemeSettings();
}

//-----------------------------------------------------------------------------
// Purpose: Walks through the panel tree calling Solve() on them all, in order
//-----------------------------------------------------------------------------
void CMatSystemSurface::SolveTraverse(VPANEL panel, bool forceApplySchemeSettings)
{
	{
		VPROF( "InternalSchemeSettingsTraverse" );
		InternalSchemeSettingsTraverse(panel, forceApplySchemeSettings);
	}

	{
		VPROF( "InternalThinkTraverse" );
		InternalThinkTraverse(panel);
	}

	{
		VPROF( "InternalSolveTraverse" );
		InternalSolveTraverse(panel);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Restricts rendering to a single panel
//-----------------------------------------------------------------------------
void CMatSystemSurface::RestrictPaintToSinglePanel(VPANEL panel)
{
	if ( panel && m_pRestrictedPanel && m_pRestrictedPanel == input()->GetAppModalSurface() )
	{
		return;	// don't restrict drawing to a panel other than the modal one - that's a good way to hang the game.
	}

	m_pRestrictedPanel = panel;

	if ( !input()->GetAppModalSurface() )
	{
		input()->SetAppModalSurface( panel );	// if painting is restricted to this panel, it had better be modal, or else you can get in some bad state...
	}
}


//-----------------------------------------------------------------------------
// Is a panel under the restricted panel?
//-----------------------------------------------------------------------------
bool CMatSystemSurface::IsPanelUnderRestrictedPanel( VPANEL panel )
{
	if ( !m_pRestrictedPanel )
		return true;

	while ( panel )
	{
		if ( panel == m_pRestrictedPanel )
			return true;

		panel = ipanel()->GetParent( panel );
	}
	return false;
}


//-----------------------------------------------------------------------------
// Main entry point for painting
//-----------------------------------------------------------------------------
void CMatSystemSurface::PaintTraverseEx(VPANEL panel, bool paintPopups /*= false*/ )
{
	if ( !ipanel()->IsVisible( panel ) )
		return;

	VPROF( "CMatSystemSurface::PaintTraverse" );
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	bool bTopLevelDraw = false;

	if ( g_bInDrawing == false )
	{
		// only set the 2d ortho mode once
		bTopLevelDraw = true;
		StartDrawing();

		// clear z + stencil buffer
		// NOTE: Stencil is used to get 3D painting in vgui panels working correctly 
		pRenderContext->ClearBuffers( false, true, true );
		pRenderContext->SetStencilEnable( true );
		pRenderContext->SetStencilFailOperation( STENCILOPERATION_KEEP );
		pRenderContext->SetStencilZFailOperation( STENCILOPERATION_KEEP );
		pRenderContext->SetStencilPassOperation( STENCILOPERATION_REPLACE );
		pRenderContext->SetStencilCompareFunction( STENCILCOMPARISONFUNCTION_GREATEREQUAL );
		pRenderContext->SetStencilReferenceValue( 0 );
		pRenderContext->SetStencilTestMask( 0xFFFFFFFF );
		pRenderContext->SetStencilWriteMask( 0xFFFFFFFF );
	}

	float flOldZPos = m_flZPos;

	// NOTE You might expect we'd have to draw these under the popups so they would occlude
	// them, but there are a few things we do have to draw on top of, esp. the black
	// panel that draws over the top of the engine to darken everything.
	m_flZPos = 0.0f;								
	if ( panel == GetEmbeddedPanel() )
	{
		if ( m_pRestrictedPanel )
		{
			// Paint the restricted panel, and its parent.
			// NOTE: This call has guards to not draw popups. If the restricted panel
			// is a popup, it won't draw here.
			ipanel()->PaintTraverse( ipanel()->GetParent( m_pRestrictedPanel ), true );
		}
		else
		{
			// paint traverse the root panel, painting all children
			VPROF( "ipanel()->PaintTraverse" );
			ipanel()->PaintTraverse( panel, true );
		}
	}
	else
	{
		// If it's a popup, it should already have been painted above
		VPROF( "ipanel()->PaintTraverse" );
		if ( !paintPopups || !ipanel()->IsPopup( panel ) )
		{
			ipanel()->PaintTraverse( panel, true );
		}
	}

	// draw the popups
	if ( paintPopups )
	{
		// now draw the popups front to back
		// since depth-test and depth-write are on, the front panels will occlude the underlying ones
		{
			VPROF( "CMatSystemSurface::PaintTraverse popups loop" );
			int popups = GetPopupCount();
			if ( popups > 254 )
			{
				Warning( "Too many popups! Rendering will be bad!\n" );
			}

			// HACK! Using stencil ref 254 so drag/drop helper can use 255.
			int nStencilRef = 254;
			for ( int i = popups - 1; i >= 0; --i )
			{
				VPANEL popupPanel = GetPopup( i );

				if ( !popupPanel )
					continue;

				if ( !ipanel()->IsFullyVisible( popupPanel ) )
					continue;

				if ( !IsPanelUnderRestrictedPanel( popupPanel ) )
					continue;

				// This makes sure the drag/drop helper is always the first thing drawn
				bool bIsTopmostPopup = ( (VPanel *)popupPanel )->IsTopmostPopup();

				// set our z position
				pRenderContext->SetStencilReferenceValue( bIsTopmostPopup ? 255 : nStencilRef );
				--nStencilRef;

				m_flZPos = ((float)(i) / (float)popups);
				ipanel()->PaintTraverse( popupPanel, true );
			}
		}
	}

	// Restore the old Z Pos
	m_flZPos = flOldZPos;

	if ( bTopLevelDraw )
	{
		// only undo the 2d ortho mode once
		VPROF( "FinishDrawing" );

		// Reset stencil to normal state
		pRenderContext->SetStencilEnable( false );

		FinishDrawing();
	}
}

//-----------------------------------------------------------------------------
// Draw a panel
//-----------------------------------------------------------------------------
void CMatSystemSurface::PaintTraverse(VPANEL panel)
{
	PaintTraverseEx( panel, false );
}


//-----------------------------------------------------------------------------
// Begins, ends 3D painting from within a panel paint() method
//-----------------------------------------------------------------------------
void CMatSystemSurface::Begin3DPaint( int iLeft, int iTop, int iRight, int iBottom )
{
	if ( IsX360() )
	{
		Assert( 0 );
		return;
	}

	Assert( iRight > iLeft );
	Assert( iBottom > iTop );

	// Can't use this while drawing in the 3D world since it relies on
	// whacking the shared depth buffer
	Assert( !m_bDrawingIn3DWorld );
	if ( m_bDrawingIn3DWorld )
		return;

	m_n3DLeft = iLeft;
	m_n3DRight = iRight;
	m_n3DTop = iTop;
	m_n3DBottom = iBottom;

	// Can't use this feature when drawing into the 3D world
	Assert( !m_bDrawingIn3DWorld );
	Assert( !m_bIn3DPaintMode );
	m_bIn3DPaintMode = true;

	// Save off the matrices in case the painting method changes them.
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();

	pRenderContext->MatrixMode( MATERIAL_VIEW );
	pRenderContext->PushMatrix();

	pRenderContext->MatrixMode( MATERIAL_PROJECTION );
	pRenderContext->PushMatrix();

	// For 3d painting, use the off-screen render target the material system allocates
	// NOTE: We have to grab it here, as opposed to during init,
	// because the mode hasn't been set by now.
	if ( !m_FullScreenBuffer )
	{
		m_FullScreenBuffer.Init( materials->FindTexture( "_rt_FullScreen", "render targets" ) );
	}

	// FIXME: Set the viewport to match the clip rectangle?
	// Set the viewport to match the scissor rectangle
	pRenderContext->PushRenderTargetAndViewport( m_FullScreenBuffer, 
		0, 0, iRight - iLeft, iBottom - iTop );

	pRenderContext->CullMode( MATERIAL_CULLMODE_CW );

	// NOTE: Stencil is used to get 3D painting in vgui panels working correctly 
	pRenderContext->SetStencilFailOperation( STENCILOPERATION_KEEP );
	pRenderContext->SetStencilZFailOperation( STENCILOPERATION_KEEP );
	pRenderContext->SetStencilPassOperation( STENCILOPERATION_REPLACE );
	pRenderContext->SetStencilCompareFunction( STENCILCOMPARISONFUNCTION_EQUAL );
	pRenderContext->Flush();

	// Don't draw the 3D scene w/ stencil
	pRenderContext->SetStencilEnable( false );
}

void CMatSystemSurface::End3DPaint()
{
	if ( IsX360() )
	{
		Assert( 0 );
		return;
	}

	// Can't use this feature when drawing into the 3D world
	Assert( !m_bDrawingIn3DWorld );
	Assert( m_bIn3DPaintMode );
	m_bIn3DPaintMode = false;

	// Reset stencil to set stencil everywhere we draw 
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );

	pRenderContext->SetStencilEnable( true );
	pRenderContext->SetStencilFailOperation( STENCILOPERATION_KEEP );
	pRenderContext->SetStencilZFailOperation( STENCILOPERATION_KEEP );
	pRenderContext->SetStencilPassOperation( STENCILOPERATION_REPLACE );
	pRenderContext->SetStencilCompareFunction( STENCILCOMPARISONFUNCTION_GREATEREQUAL );

	// Restore the matrices
	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();

	pRenderContext->MatrixMode( MATERIAL_VIEW );
	pRenderContext->PopMatrix();

	pRenderContext->MatrixMode( MATERIAL_PROJECTION );
	pRenderContext->PopMatrix();

	// Restore the viewport (it was stored off in StartDrawing)
	pRenderContext->PopRenderTargetAndViewport();
	pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);

	// Draw the full-screen buffer into the panel
	DrawFullScreenBuffer( m_n3DLeft, m_n3DTop, m_n3DRight, m_n3DBottom );

	// ReSet the material state
	InternalSetMaterial( NULL );
}


//-----------------------------------------------------------------------------
// Gets texture coordinates for drawing the full screen buffer
//-----------------------------------------------------------------------------
void CMatSystemSurface::GetFullScreenTexCoords( int x, int y, int w, int h, float *pMinU, float *pMinV, float *pMaxU, float *pMaxV )
{
	int nTexWidth = m_FullScreenBuffer->GetActualWidth();
	int nTexHeight = m_FullScreenBuffer->GetActualHeight();
	float flOOWidth = 1.0f / nTexWidth;
	float flOOHeight = 1.0f / nTexHeight;

	*pMinU = ( (float)x + 0.5f ) * flOOWidth;
	*pMinV = ( (float)y + 0.5f ) * flOOHeight;
	*pMaxU = ( (float)(x+w) - 0.5f ) * flOOWidth;
	*pMaxV = ( (float)(y+h) - 0.5f ) * flOOHeight;
}


//-----------------------------------------------------------------------------
// Draws the fullscreen buffer into the panel
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawFullScreenBuffer( int nLeft, int nTop, int nRight, int nBottom )
{
	// Draw a textured rectangle over the area
	if ( m_nFullScreenBufferMaterialId == -1 )
	{
		m_nFullScreenBufferMaterialId = CreateNewTextureID();
		DrawSetTextureMaterial( m_nFullScreenBufferMaterialId, m_FullScreenBufferMaterial );
	}

	float flGetAlphaMultiplier = DrawGetAlphaMultiplier();
	unsigned char oldColor[4];
	oldColor[0] = m_DrawColor[0];
	oldColor[1] = m_DrawColor[1];
	oldColor[2] = m_DrawColor[2];
	oldColor[3] = m_DrawColor[3];

	DrawSetAlphaMultiplier( 1.0f );
	DrawSetColor( 255, 255, 255, 255 );

	DrawSetTexture( m_nFullScreenBufferMaterialId );

	float u0, u1, v0, v1;
	GetFullScreenTexCoords( 0, 0, nRight - nLeft, nBottom - nTop, &u0, &v0, &u1, &v1 );
	DrawTexturedSubRect( nLeft, nTop, nRight, nBottom, u0, v0, u1, v1 );

	m_DrawColor[0] = oldColor[0];
	m_DrawColor[1] = oldColor[1];
	m_DrawColor[2] = oldColor[2];
	m_DrawColor[3] = oldColor[3];
	DrawSetAlphaMultiplier( flGetAlphaMultiplier );
}


//-----------------------------------------------------------------------------
// Draws a rectangle, setting z to the current value
//-----------------------------------------------------------------------------
float CMatSystemSurface::GetZPos() const
{
	return m_flZPos;
}


//-----------------------------------------------------------------------------
// Some drawing methods that cannot be accomplished under Win32
//-----------------------------------------------------------------------------
#define CIRCLE_POINTS		360

void CMatSystemSurface::DrawColoredCircle( int centerx, int centery, float radius, int r, int g, int b, int a )
{
	Assert( g_bInDrawing );
	// Draw a circle
	int iDegrees = 0;
	Vector vecPoint, vecLastPoint(0,0,0);
	vecPoint.z = 0.0f;
	Color clr;
	clr.SetColor( r, g, b, a );
	DrawSetColor( clr );

	for ( int i = 0; i < CIRCLE_POINTS; i++ )
	{
		float flRadians = DEG2RAD( iDegrees );
		iDegrees += (360 / CIRCLE_POINTS);

		float ca = cos( flRadians );
		float sa = sin( flRadians );
					 
		// Rotate it around the circle
		vecPoint.x = centerx + (radius * sa);
		vecPoint.y = centery - (radius * ca);

		// Draw the point, if it's not on the previous point, to avoid smaller circles being brighter
		if ( vecLastPoint != vecPoint )
		{
			DrawFilledRect( vecPoint.x, vecPoint.y,  vecPoint.x + 1, vecPoint.y + 1 );
		}

		vecLastPoint = vecPoint;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Draws colored text to a vgui panel
// Input  : *font - font to use
//			x - position of text
//			y - 
//			r - color of text
//			g - 
//			b - 
//			a - alpha ( 255 = opaque, 0 = transparent )
//			*fmt - va_* text string
//			... - 
// Output : int - horizontal # of pixels drawn
//-----------------------------------------------------------------------------
int CMatSystemSurface::DrawColoredText( vgui::HFont font, int x, int y, int r, int g, int b, int a, char *fmt, va_list argptr )
{
	Assert( g_bInDrawing );
	int len;
	char data[1024];

	DrawSetTextPos( x, y );
	DrawSetTextColor( r, g, b, a );

	len = Q_vsnprintf(data, sizeof( data ), fmt, argptr);

	DrawSetTextFont( font );

	wchar_t szconverted[ 1024 ];
	g_pVGuiLocalize->ConvertANSIToUnicode( data, szconverted, 1024 );
	
	DrawPrintText( szconverted, wcslen(szconverted ) );

	int totalLength = DrawTextLen( font, data );

	return x + totalLength;
}

int CMatSystemSurface::DrawColoredText( vgui::HFont font, int x, int y, int r, int g, int b, int a, char *fmt, ... )
{
	va_list argptr;
	va_start( argptr, fmt );
	int ret = DrawColoredText( font, x, y, r, g, b, a, fmt, argptr );
	va_end(argptr);
	return ret;
}


//-----------------------------------------------------------------------------
// Draws text with current font at position and wordwrapped to the rect using color values specified
//-----------------------------------------------------------------------------
void CMatSystemSurface::SearchForWordBreak( vgui::HFont font, char *text, int& chars, int& pixels )
{
	chars = pixels = 0;
	while ( 1 )
	{
		char ch = text[ chars ];
		int a, b, c;
		GetCharABCwide( font, ch, a, b, c );

		if ( ch == 0 || ch <= 32 )
		{
			if ( ch == 32 && chars == 0 )
			{
				pixels += ( b + c );
				chars++;
			}
			break;
		}

		pixels += ( b + c );
		chars++;
	}
}

//-----------------------------------------------------------------------------
// Purpose: If text width is specified, reterns height of text at that width
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawTextHeight( vgui::HFont font, int w, int& h, char *fmt, ... )
{
	if ( !font )
		return;

	int len;
	char data[8192];

	va_list argptr;
	va_start( argptr, fmt );
	len = Q_vsnprintf( data, sizeof( data ), fmt, argptr );
	va_end( argptr );

	int x = 0;
	int y = 0;

	int ystep = GetFontTall( font );
	int startx = x;
	int endx = x + w;
	//int endy = y + h;
	int endy = 0;

	int chars = 0;
	int pixels = 0;
	for ( int i = 0 ; i < len; i += chars )
	{
		SearchForWordBreak( font, &data[ i ], chars, pixels );

		if ( data[ i ] == '\n' )
		{
			x = startx;
			y += ystep;
			chars = 1;
			continue;
		}

		if ( x + ( pixels ) >= endx )
		{
			x = startx;
			// No room even on new line!!!
			if ( x + pixels >= endx )
				break;

			y += ystep;
		}

		for ( int j = 0 ; j < chars; j++ )
		{
			int a, b, c;
			char ch = data[ i + j ];

			GetCharABCwide( font, ch, a, b, c );
	
			x += a + b + c;
		}
	}

	endy = y+ystep;

	h = endy;
}


//-----------------------------------------------------------------------------
// Draws text with current font at position and wordwrapped to the rect using color values specified
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawColoredTextRect( vgui::HFont font, int x, int y, int w, int h, int r, int g, int b, int a, char *fmt, ... )
{
	Assert( g_bInDrawing );
	if ( !font )
		return;

	int len;
	char data[8192];

	va_list argptr;
	va_start( argptr, fmt );
	len = Q_vsnprintf( data, sizeof( data ), fmt, argptr );
	va_end( argptr );

	DrawSetTextPos( x, y );
	DrawSetTextColor( r, g, b, a );
	DrawSetTextFont( font );

	int ystep = GetFontTall( font );
	int startx = x;
	int endx = x + w;
	int endy = y + h;

	int chars = 0;
	int pixels = 0;

	char word[ 512 ];
	char space[ 2 ];
	space[1] = 0;
	space[0] = ' ';

	for ( int i = 0 ; i < len; i += chars )
	{
		SearchForWordBreak( font, &data[ i ], chars, pixels );

		if ( data[ i ] == '\n' )
		{
			x = startx;
			y += ystep;
			chars = 1;
			continue;
		}

		if ( x + ( pixels ) >= endx )
		{
			x = startx;
			// No room even on new line!!!
			if ( x + pixels >= endx )
				break;

			y += ystep;
		}

		if ( y + ystep >= endy )
			break;


		if ( chars <= 0 )
			continue;

		Q_strncpy( word, &data[ i ], chars + 1 );

		DrawSetTextPos( x, y );

		wchar_t szconverted[ 1024 ];
		g_pVGuiLocalize->ConvertANSIToUnicode( word, szconverted, 1024 );
		DrawPrintText( szconverted, wcslen(szconverted ) );

		// Leave room for space, too
		x += DrawTextLen( font, word );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Determine length of text string
//-----------------------------------------------------------------------------
int	CMatSystemSurface::DrawTextLen( vgui::HFont font, char *fmt, ... )
{
	va_list argptr;
	char data[1024];
	int len;

	va_start(argptr, fmt);
	len = Q_vsnprintf(data, sizeof( data ), fmt, argptr);
	va_end(argptr);

	int i;
	int x = 0;

	for ( i = 0 ; i < len; i++ )
	{
		int a, b, c;
		GetCharABCwide( font, data[i], a, b, c );

		// Ignore a
		//	x += a;
		x += b;
		x += c;
	}

	return x;
}

//-----------------------------------------------------------------------------
// Disable clipping during rendering
//-----------------------------------------------------------------------------
void CMatSystemSurface::DisableClipping( bool bDisable )
{
	EnableScissor( !bDisable );
}


//-----------------------------------------------------------------------------
// Purpose: unlocks the cursor state
//-----------------------------------------------------------------------------
bool CMatSystemSurface::IsCursorLocked() const
{
	return ::IsCursorLocked();
}


//-----------------------------------------------------------------------------
// Sets the mouse Get + Set callbacks
//-----------------------------------------------------------------------------
void CMatSystemSurface::SetMouseCallbacks( GetMouseCallback_t GetFunc, SetMouseCallback_t SetFunc )
{
	// FIXME: Remove! This is obsolete
	Assert(0);
}


//-----------------------------------------------------------------------------
// Tells the surface to ignore windows messages
//-----------------------------------------------------------------------------
void CMatSystemSurface::EnableWindowsMessages( bool bEnable )
{
	EnableInput( bEnable );
}

void CMatSystemSurface::MovePopupToFront(VPANEL panel)
{
	HPanel p = ivgui()->PanelToHandle( panel );

	int index = m_PopupList.Find( p );
	if ( index == m_PopupList.InvalidIndex() )
		return;

	m_PopupList.Remove( index );
	m_PopupList.AddToTail( p );

	// If the modal panel isn't a parent, restore it to the top, to prevent a hard lock
	if ( input()->GetAppModalSurface() )
	{
		if ( !g_pVGuiPanel->HasParent(panel, input()->GetAppModalSurface()) )
		{
			HPanel p = ivgui()->PanelToHandle( input()->GetAppModalSurface() );
			index = m_PopupList.Find( p );
			if ( index != m_PopupList.InvalidIndex() )
			{
				m_PopupList.Remove( index );
				m_PopupList.AddToTail( p );
			}
		}
	}
}

void CMatSystemSurface::MovePopupToBack(VPANEL panel)
{
	HPanel p = ivgui()->PanelToHandle( panel );

	int index = m_PopupList.Find( p );
	if ( index == m_PopupList.InvalidIndex() )
	{
		return;
	}

	m_PopupList.Remove( index );
	m_PopupList.AddToHead( p );
}


bool CMatSystemSurface::IsInThink( VPANEL panel)
{
	if ( m_bInThink )
	{
		if ( panel == m_CurrentThinkPanel ) // HasParent() returns true if you pass yourself in
		{
			return false;
		}

		return ipanel()->HasParent( panel, m_CurrentThinkPanel);
	}
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CMatSystemSurface::IsCursorVisible()
{
	return (_currentCursor != dc_none);
}


bool CMatSystemSurface::IsTextureIDValid(int id)
{
	// FIXME:
	return true;
}

void CMatSystemSurface::SetAllowHTMLJavaScript( bool state )
{ 
	m_bAllowJavaScript = state; 
}

IHTML *CMatSystemSurface::CreateHTMLWindow(vgui::IHTMLEvents *events,VPANEL context)
{
#if defined( ENABLE_HTMLWINDOW )
	HtmlWindow *IE = new HtmlWindow(events,context,reinterpret_cast<HWND>(m_HWnd), m_bAllowJavaScript, false);
	IE->Show(false);
	_htmlWindows.AddToTail(IE);
	return dynamic_cast<IHTML *>(IE);
#else
	Assert( 0 );
	return NULL;
#endif
}


void CMatSystemSurface::DeleteHTMLWindow(IHTML *htmlwin)
{
#if defined( ENABLE_HTMLWINDOW )
	HtmlWindow *IE =static_cast<HtmlWindow *>(htmlwin);

	if(IE)
	{
		_htmlWindows.FindAndRemove( IE );
		delete IE;
	}
#elif !defined( _X360 )
#error "GameUI now NEEDS the HTML component!!"
#endif
}



void CMatSystemSurface::PaintHTMLWindow(IHTML *htmlwin)
{
#if defined( ENABLE_HTMLWINDOW )
	HtmlWindow *IE = static_cast<HtmlWindow *>(htmlwin);
	if(IE)
	{
		//HBITMAP bits;
		HDC hdc = ::GetDC(reinterpret_cast<HWND>(m_HWnd));
		IE->OnPaint(hdc);
		::ReleaseDC( reinterpret_cast<HWND>(m_HWnd), hdc );
	}
#endif
}

void CMatSystemSurface::DrawSetTextureRGBA(int id, const unsigned char* rgba, int wide, int tall, int hardwareFilter, bool forceUpload)
{
	DrawSetTextureRGBAEx( id, rgba, wide, tall, IMAGE_FORMAT_RGBA8888 );
}

void CMatSystemSurface::DrawSetTextureRGBAEx( int id, const unsigned char* rgba, int wide, int tall, ImageFormat format )
{
	TextureDictionary()->SetTextureRGBAEx( id, (const char *)rgba, wide, tall, format );
}

void CMatSystemSurface::DrawSetSubTextureRGBA(int textureID, int drawX, int drawY, unsigned const char *rgba, int subTextureWide, int subTextureTall)
{
	TextureDictionary()->SetSubTextureRGBA( textureID, drawX, drawY, rgba, subTextureWide, subTextureTall );
}

void CMatSystemSurface::SetModalPanel(VPANEL )
{
}

VPANEL CMatSystemSurface::GetModalPanel()
{
	return 0;
}

void CMatSystemSurface::UnlockCursor()
{
	::LockCursor( false );
}

void CMatSystemSurface::LockCursor()
{
	::LockCursor( true );
}

void CMatSystemSurface::SetTranslateExtendedKeys(bool state)
{
}

VPANEL CMatSystemSurface::GetTopmostPopup()
{
	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: gets the absolute coordinates of the screen (in screen space)
//-----------------------------------------------------------------------------
void CMatSystemSurface::GetAbsoluteWindowBounds(int &x, int &y, int &wide, int &tall)
{
	// always work in full window screen space
	x = 0;
	y = 0;
	GetScreenSize(wide, tall);
}

// returns true if the specified panel is a child of the current modal panel
// if no modal panel is set, then this always returns TRUE
static bool IsChildOfModalSubTree(VPANEL panel)
{
	if ( !panel )
		return true;

	VPANEL modalSubTree = input()->GetModalSubTree();

	if ( modalSubTree )
	{
		bool restrictMessages = input()->ShouldModalSubTreeReceiveMessages();

		// If panel is child of modal subtree, the allow messages to route to it if restrict messages is set
		bool isChildOfModal = ipanel()->HasParent( panel, modalSubTree );
		if ( isChildOfModal )
		{
			return restrictMessages;
		}
		// If panel is not a child of modal subtree, then only allow messages if we're not restricting them to the modal subtree
		else
		{
			return !restrictMessages;
		}
	}

	return true;
}

void CMatSystemSurface::CalculateMouseVisible()
{
	int i;
	m_bNeedsMouse = false;
	m_bNeedsKeyboard = false;

	if ( input()->GetMouseCapture() != 0 )
		return;

	for (i = 0 ; i < surface()->GetPopupCount() ; i++ )
	{
		VPanel *pop = (VPanel *)surface()->GetPopup(i) ;
		if ( !pop )
			continue;
		bool isChildOfModalSubPanel = IsChildOfModalSubTree( (VPANEL)pop );
		if ( !isChildOfModalSubPanel )
			continue;
		
		bool isVisible=pop->IsVisible();
		VPanel *p= pop->GetParent();

		while (p && isVisible)
		{
			if( p->IsVisible()==false)
			{
				isVisible=false;
				break;
			}
			p=p->GetParent();
		}
	
		if ( isVisible )
		{
			m_bNeedsMouse = m_bNeedsMouse || pop->IsMouseInputEnabled();
			m_bNeedsKeyboard = m_bNeedsKeyboard || pop->IsKeyBoardInputEnabled();
		}
	}

	if (m_bNeedsMouse)
	{
		// NOTE: We must unlock the cursor *before* the set call here.
		// Failing to do this causes s_bCursorVisible to not be set correctly
		// (UnlockCursor fails to set it correctly)
		UnlockCursor();
		if ( _currentCursor == vgui::dc_none )
		{
			SetCursor(vgui::dc_arrow);
		}
	}
	else
	{
		SetCursor(vgui::dc_none);
		LockCursor();
	}
}

bool CMatSystemSurface::NeedKBInput()
{
	return m_bNeedsKeyboard;
}

void CMatSystemSurface::SurfaceGetCursorPos(int &x, int &y)
{
	GetCursorPos( x, y );
}
void CMatSystemSurface::SurfaceSetCursorPos(int x, int y)
{
	SetCursorPos( x, y );
}

//-----------------------------------------------------------------------------
// Purpose: global alpha setting functions
//-----------------------------------------------------------------------------
void CMatSystemSurface::DrawSetAlphaMultiplier( float alpha /* [0..1] */ )
{
	m_flAlphaMultiplier = clamp(alpha, 0.0f, 1.0f);
}

//-----------------------------------------------------------------------------
// Purpose: data accessor
//-----------------------------------------------------------------------------
float CMatSystemSurface::DrawGetAlphaMultiplier()
{
	return m_flAlphaMultiplier;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *curOrAniFile - 
// Output : vgui::HCursor
//-----------------------------------------------------------------------------
vgui::HCursor CMatSystemSurface::CreateCursorFromFile( char const *curOrAniFile, char const *pPathID )
{
	return Cursor_CreateCursorFromFile( curOrAniFile, pPathID );
}

void CMatSystemSurface::SetPanelForInput( VPANEL vpanel )
{
	g_pIInput->AssociatePanelWithInputContext( DEFAULT_INPUT_CONTEXT, vpanel );
	if ( vpanel )
	{
		m_bNeedsKeyboard = true;
	}
	else
	{
		m_bNeedsKeyboard = false;
	}
}

#if !defined( _X360 )
static bool GetIconSize( ICONINFO& iconInfo, int& w, int& h )
{
	w = h = 0;

	HBITMAP bitmap = iconInfo.hbmColor;
	BITMAP bm;
	if ( 0 == GetObject((HGDIOBJ)bitmap, sizeof(BITMAP), (LPVOID)&bm) ) 
	{
        return false; 
	}

	w = bm.bmWidth;
	h = bm.bmHeight;

	return true;
}

// If rgba is NULL, bufsize gets filled in w/ # of bytes required
static bool GetIconBits( HDC hdc, ICONINFO& iconInfo, int& w, int& h, unsigned char *rgba, size_t& bufsize )
{
	if ( !iconInfo.hbmColor || !iconInfo.hbmMask )
		return false;

	if ( !rgba )
	{
		if ( !GetIconSize( iconInfo, w, h ) )
			return false;
		
		bufsize = (size_t)( ( w * h ) << 2 );
		return true;
	}

	bool bret = false;

	Assert( w > 0 );
	Assert( h > 0 );
	Assert( bufsize == (size_t)( ( w * h ) << 2 ) );

	DWORD *maskData = new DWORD[ w * h ];
	DWORD *colorData =  new DWORD[ w * h ];
	DWORD *output = (DWORD *)rgba;

	BITMAPINFO bmInfo;

	memset( &bmInfo, 0, sizeof( bmInfo ) );
	bmInfo.bmiHeader.biSize = sizeof( bmInfo.bmiHeader );
	bmInfo.bmiHeader.biWidth = w; 
    bmInfo.bmiHeader.biHeight = h; 
    bmInfo.bmiHeader.biPlanes = 1; 
    bmInfo.bmiHeader.biBitCount = 32; 
    bmInfo.bmiHeader.biCompression = BI_RGB; 

	// Get the info about the bits
	if ( GetDIBits( hdc, iconInfo.hbmMask, 0, h, maskData, &bmInfo, DIB_RGB_COLORS ) == h &&
         GetDIBits( hdc, iconInfo.hbmColor, 0, h, colorData, &bmInfo, DIB_RGB_COLORS ) == h )
	{
		bret = true;

		for ( int row = 0; row < h; ++row )
		{
			// Invert
			int r = ( h - row - 1 );
			int rowstart = r * w;

			DWORD *color = &colorData[ rowstart ];
			DWORD *mask = &maskData[ rowstart ];
			DWORD *outdata = &output[ row * w ];

			for ( int col = 0; col < w; ++col )
			{
				unsigned char *cr = ( unsigned char * )&color[ col ];

				// Set alpha
				cr[ 3 ] =  mask[ col ] == 0 ? 0xff : 0x00;
				
				// Swap blue and red
				unsigned char t = cr[ 2 ];
				cr[ 2 ] = cr[ 0 ];
				cr[ 0 ] = t;

				*( unsigned int *)&outdata[ col ] = *( unsigned int * )cr;
			}
		}
	}

	delete[] colorData;
	delete[] maskData;

	return bret;
}

static bool ShouldMakeUnique( char const *extension )
{
	if ( !Q_stricmp( extension, "cur" ) )
		return true;
	if ( !Q_stricmp( extension, "ani" ) )
		return true;
	return false;
}
#endif // !_X360

vgui::IImage *CMatSystemSurface::GetIconImageForFullPath( char const *pFullPath )
{
	vgui::IImage *newIcon = NULL;

#if !defined( _X360 )
	SHFILEINFO info = { 0 };
	DWORD_PTR dwResult = SHGetFileInfo( 
		pFullPath,
		0,
		&info,
		sizeof( info ),
		SHGFI_TYPENAME | SHGFI_ICON | SHGFI_SMALLICON | SHGFI_SHELLICONSIZE 
	);
	if ( dwResult )
	{
		if ( info.szTypeName[ 0 ] != 0 )
		{
			char ext[ 32 ];
			Q_ExtractFileExtension( pFullPath, ext, sizeof( ext ) );

			char lookup[ 512 ];
			Q_snprintf( lookup, sizeof( lookup ), "%s", ShouldMakeUnique( ext ) ? pFullPath : info.szTypeName );
			
			// Now check the dictionary
			unsigned short idx = m_FileTypeImages.Find( lookup );
			if ( idx == m_FileTypeImages.InvalidIndex() )
			{
				ICONINFO iconInfo;
				if ( 0 != GetIconInfo( info.hIcon, &iconInfo ) )
				{
					int w, h;
					size_t bufsize = 0;
					
					HDC hdc = ::GetDC(reinterpret_cast<HWND>(m_HWnd));

					if ( GetIconBits( hdc, iconInfo, w, h, NULL, bufsize ) )
					{
						byte *bits = new byte[ bufsize ];
						if ( bits && GetIconBits( hdc, iconInfo, w, h, bits, bufsize ) )
						{
							newIcon = new MemoryBitmap( bits, w, h );
						}
						delete[] bits;
					}

					::ReleaseDC( reinterpret_cast<HWND>(m_HWnd), hdc );
				}

				idx = m_FileTypeImages.Insert( lookup, newIcon );
			}

			newIcon = m_FileTypeImages[ idx ];
		}

		DestroyIcon( info.hIcon );
	}
#endif
	return newIcon;
}

const char *CMatSystemSurface::GetResolutionKey( void ) const
{
	Assert( !IsPC() );
	int x, y, width, height;
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	pRenderContext->GetViewport( x, y, width, height );
	if( height <= 480 )
	{
		return "_lodef";
	}
	else
	{
		return "_hidef";
	}
}
