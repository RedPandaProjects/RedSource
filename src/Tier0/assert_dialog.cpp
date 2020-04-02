//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "pch_tier0.h"

#include "tier0/valve_off.h"
#ifdef _X360
#include "xbox/xbox_console.h"
#include "xbox/xbox_vxconsole.h"
#elif defined( _WIN32 )
#include <windows.h>
#elif _LINUX
char *GetCommandLine();
#endif
#include "resource.h"
#include "tier0/valve_on.h"
#include "tier0/threadtools.h"

class CDialogInitInfo
{
public:
	const tchar *m_pFilename;
	int m_iLine;
	const tchar *m_pExpression;
};


class CAssertDisable
{
public:
	tchar m_Filename[512];
	
	// If these are not -1, then this CAssertDisable only disables asserts on lines between
	// these values (inclusive).
	int m_LineMin;		
	int m_LineMax;
	
	// Decremented each time we hit this assert and ignore it, until it's 0. 
	// Then the CAssertDisable is removed.
	// If this is -1, then we always ignore this assert.
	int m_nIgnoreTimes;	

	CAssertDisable *m_pNext;
};

#ifdef _WIN32
static HINSTANCE g_hTier0Instance = 0;
#endif

static bool g_bAssertsEnabled = true;

static CAssertDisable *g_pAssertDisables = NULL;

static int g_iLastLineRange = 5;
static int g_nLastIgnoreNumTimes = 1;
static int g_VXConsoleAssertReturnValue = -1;

// Set to true if they want to break in the debugger.
static bool g_bBreak = false;

static CDialogInitInfo g_Info;


// -------------------------------------------------------------------------------- //
// Internal functions.
// -------------------------------------------------------------------------------- //

#if defined(_WIN32) && !defined(STATIC_TIER0)
BOOL WINAPI DllMain(
  HINSTANCE hinstDLL,  // handle to the DLL module
  DWORD fdwReason,     // reason for calling function
  LPVOID lpvReserved   // reserved
)
{
	g_hTier0Instance = hinstDLL;
	return true;
}
#endif

static bool IsDebugBreakEnabled()
{
	static bool bResult = ( _tcsstr( Plat_GetCommandLine(), _T("-debugbreak") ) != NULL );
	return bResult;
}

static bool AreAssertsDisabled()
{
	static bool bResult = ( _tcsstr( Plat_GetCommandLine(), _T("-noassert") ) != NULL );
	return bResult;
}

static bool AreAssertsEnabledInFileLine( const tchar *pFilename, int iLine )
{
	CAssertDisable **pPrev = &g_pAssertDisables;
	CAssertDisable *pNext;
	for ( CAssertDisable *pCur=g_pAssertDisables; pCur; pCur=pNext )
	{
		pNext = pCur->m_pNext;

		if ( _tcsicmp( pFilename, pCur->m_Filename ) == 0 )
		{
			// Are asserts disabled in the whole file?
			bool bAssertsEnabled = true;
			if ( pCur->m_LineMin == -1 && pCur->m_LineMax == -1 )
				bAssertsEnabled = false;
			
			// Are asserts disabled on the specified line?
			if ( iLine >= pCur->m_LineMin && iLine <= pCur->m_LineMax )
				bAssertsEnabled = false;

			if ( !bAssertsEnabled )
			{
				// If this assert is only disabled for the next N times, then countdown..
				if ( pCur->m_nIgnoreTimes > 0 )
				{
					--pCur->m_nIgnoreTimes;
					if ( pCur->m_nIgnoreTimes == 0 )
					{
						// Remove this one from the list.
						*pPrev = pNext;
						delete pCur;
						continue;
					}
				}
				
				return false;
			}
		}

		pPrev = &pCur->m_pNext;
	}

	return true;
}


CAssertDisable* CreateNewAssertDisable( const tchar *pFilename )
{
	CAssertDisable *pDisable = new CAssertDisable;
	pDisable->m_pNext = g_pAssertDisables;
	g_pAssertDisables = pDisable;

	pDisable->m_LineMin = pDisable->m_LineMax = -1;
	pDisable->m_nIgnoreTimes = -1;
	
	_tcsncpy( pDisable->m_Filename, g_Info.m_pFilename, sizeof( pDisable->m_Filename ) - 1 );
	pDisable->m_Filename[ sizeof( pDisable->m_Filename ) - 1 ] = 0;
	
	return pDisable;
}


void IgnoreAssertsInCurrentFile()
{
	CreateNewAssertDisable( g_Info.m_pFilename );
}


CAssertDisable* IgnoreAssertsNearby( int nRange )
{
	CAssertDisable *pDisable = CreateNewAssertDisable( g_Info.m_pFilename );
	pDisable->m_LineMin = g_Info.m_iLine - nRange;
	pDisable->m_LineMax = g_Info.m_iLine - nRange;
	return pDisable;
}


#if ( defined( _WIN32 ) && !defined( _X360 ) )
int CALLBACK AssertDialogProc(
  HWND hDlg,  // handle to dialog box
  UINT uMsg,     // message
  WPARAM wParam, // first message parameter
  LPARAM lParam  // second message parameter
)
{
	switch( uMsg )
	{
		case WM_INITDIALOG:
		{
#ifdef TCHAR_IS_WCHAR
			SetDlgItemTextW( hDlg, IDC_ASSERT_MSG_CTRL, g_Info.m_pExpression );
			SetDlgItemTextW( hDlg, IDC_FILENAME_CONTROL, g_Info.m_pFilename );
#else
			SetDlgItemText( hDlg, IDC_ASSERT_MSG_CTRL, g_Info.m_pExpression );
			SetDlgItemText( hDlg, IDC_FILENAME_CONTROL, g_Info.m_pFilename );
#endif
			SetDlgItemInt( hDlg, IDC_LINE_CONTROL, g_Info.m_iLine, false );
			SetDlgItemInt( hDlg, IDC_IGNORE_NUMLINES, g_iLastLineRange, false );
			SetDlgItemInt( hDlg, IDC_IGNORE_NUMTIMES, g_nLastIgnoreNumTimes, false );
		
			// Center the dialog.
			RECT rcDlg, rcDesktop;
			GetWindowRect( hDlg, &rcDlg );
			GetWindowRect( GetDesktopWindow(), &rcDesktop );
			SetWindowPos( 
				hDlg, 
				HWND_TOP, 
				((rcDesktop.right-rcDesktop.left) - (rcDlg.right-rcDlg.left)) / 2,
				((rcDesktop.bottom-rcDesktop.top) - (rcDlg.bottom-rcDlg.top)) / 2,
				0,
				0,
				SWP_NOSIZE );
		}
		return true;

		case WM_COMMAND:
		{
			switch( LOWORD( wParam ) )
			{
				case IDC_IGNORE_FILE:
				{
					IgnoreAssertsInCurrentFile();
					EndDialog( hDlg, 0 );
					return true;
				}

				// Ignore this assert N times.
				case IDC_IGNORE_THIS:
				{
					BOOL bTranslated = false;
					UINT value = GetDlgItemInt( hDlg, IDC_IGNORE_NUMTIMES, &bTranslated, false );
					if ( bTranslated && value > 1 )
					{
						CAssertDisable *pDisable = IgnoreAssertsNearby( 0 );
						pDisable->m_nIgnoreTimes = value - 1;
						g_nLastIgnoreNumTimes = value;
					}

					EndDialog( hDlg, 0 );
					return true;
				}

				// Always ignore this assert.
				case IDC_IGNORE_ALWAYS:
				{
					IgnoreAssertsNearby( 0 );
					EndDialog( hDlg, 0 );
					return true;
				}
				
				case IDC_IGNORE_NEARBY:
				{
					BOOL bTranslated = false;
					UINT value = GetDlgItemInt( hDlg, IDC_IGNORE_NUMLINES, &bTranslated, false );
					if ( !bTranslated || value < 1 )
						return true;

					IgnoreAssertsNearby( value );
					EndDialog( hDlg, 0 );
					return true;
				}

				case IDC_IGNORE_ALL:
				{
					g_bAssertsEnabled = false;
					EndDialog( hDlg, 0 );
					return true;
				}

				case IDC_BREAK:
				{
					g_bBreak = true;
					EndDialog( hDlg, 0 );
					return true;
				}
			}

			case WM_KEYDOWN:
			{
				// Escape?
				if ( wParam == 2 )
				{
					// Ignore this assert.
					EndDialog( hDlg, 0 );
					return true;
				}
			}
					
		}
		return true;
	}

	return FALSE;
}


static HWND g_hBestParentWindow;


static BOOL CALLBACK ParentWindowEnumProc(
  HWND hWnd,      // handle to parent window
  LPARAM lParam   // application-defined value
)
{
	if ( IsWindowVisible( hWnd ) )
	{
		DWORD procID;
		GetWindowThreadProcessId( hWnd, &procID );
		if ( procID == (DWORD)lParam )
		{
			g_hBestParentWindow = hWnd;
			return FALSE; // don't iterate any more.
		}
	}
	return TRUE;
}


static HWND FindLikelyParentWindow()
{
	// Enumerate top-level windows and take the first visible one with our processID.
	g_hBestParentWindow = NULL;
	EnumWindows( ParentWindowEnumProc, GetCurrentProcessId() );
	return g_hBestParentWindow;
}
#endif

// -------------------------------------------------------------------------------- //
// Interface functions.
// -------------------------------------------------------------------------------- //


DBG_INTERFACE bool ShouldUseNewAssertDialog()
{
	static bool bMPIWorker = ( _tcsstr( Plat_GetCommandLine(), _T("-mpi_worker") ) != NULL );
	if ( bMPIWorker )
	{
		return false;
	}

#ifdef DBGFLAG_ASSERTDLG
	return true;		// always show an assert dialog
#else
	return Plat_IsInDebugSession();		// only show an assert dialog if the process is being debugged
#endif // DBGFLAG_ASSERTDLG
}


DBG_INTERFACE bool DoNewAssertDialog( const tchar *pFilename, int line, const tchar *pExpression )
{
	LOCAL_THREAD_LOCK();

	if ( AreAssertsDisabled() )
		return false;

	// If they have the old mode enabled (always break immediately), then just break right into
	// the debugger like we used to do.
	if ( IsDebugBreakEnabled() )
		return true;

	// Have ALL Asserts been disabled?
	if ( !g_bAssertsEnabled )
		return false;

	// Has this specific Assert been disabled?
	if ( !AreAssertsEnabledInFileLine( pFilename, line ) )
		return false;

	// Now create the dialog.
	g_Info.m_pFilename = pFilename;
	g_Info.m_iLine = line;
	g_Info.m_pExpression = pExpression;

	g_bBreak = false;

#if defined( _X360 )

	char cmdString[XBX_MAX_RCMDLENGTH];

	// Before calling VXConsole, init the global variable that receives the result
	g_VXConsoleAssertReturnValue = -1;

	// Message VXConsole to pop up a PC-side Assert dialog
	_snprintf( cmdString, sizeof(cmdString), "Assert() 0x%.8x File: %s\tLine: %d\t%s",
				&g_VXConsoleAssertReturnValue, pFilename, line, pExpression );
	XBX_SendRemoteCommand( cmdString, false );

	// We sent a synchronous message, so g_xbx_dbgVXConsoleAssertReturnValue should have been overwritten by now
	if ( g_VXConsoleAssertReturnValue == -1 )
	{
		// VXConsole isn't connected/running - default to the old behaviour (break)
		g_bBreak = true;
	}
	else
	{
		// Respond to what the user selected
		switch( g_VXConsoleAssertReturnValue )
		{
		case ASSERT_ACTION_IGNORE_FILE:
			IgnoreAssertsInCurrentFile();
			break;
		case ASSERT_ACTION_IGNORE_THIS:
			// Ignore this Assert once
			break;
		case ASSERT_ACTION_BREAK:
			// Break on this Assert
			g_bBreak = true;
			break;
		case ASSERT_ACTION_IGNORE_ALL:
			// Ignore all Asserts from now on
			g_bAssertsEnabled = false;
			break;
		case ASSERT_ACTION_IGNORE_ALWAYS:
			// Ignore this Assert from now on
			IgnoreAssertsNearby( 0 );
			break;
		case ASSERT_ACTION_OTHER:
		default:
			// Error... just break
			XBX_Error( "DoNewAssertDialog: invalid Assert response returned from VXConsole - breaking to debugger" );
			g_bBreak = true;
			break;
		}
	}

#elif defined( _WIN32 )

	if ( !ThreadInMainThread() )
	{
		int result = MessageBox( NULL,  pExpression, "Assertion Failed", MB_SYSTEMMODAL );

		if ( result == IDCANCEL )
		{
			IgnoreAssertsNearby( 0 );
		}
	/*	else if ( result == IDCONTINUE )
		{
			g_bBreak = true;
		}*/
	}
	else
	{
		HWND hParentWindow = FindLikelyParentWindow();

		DialogBox( g_hTier0Instance, MAKEINTRESOURCE( IDD_ASSERT_DIALOG ), hParentWindow, AssertDialogProc );
	}

#elif _LINUX

	fprintf(stderr, "%s %i %s", pFilename, line, pExpression);

#endif

	return g_bBreak;
}




