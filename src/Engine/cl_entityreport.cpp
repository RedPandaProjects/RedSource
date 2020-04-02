//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//
#include "client_pch.h"
#include "ivideomode.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar cl_entityreport( "cl_entityreport", "0", FCVAR_CHEAT, "For debugging, draw entity states to console" );

// How quickly to move rolling average for entityreport
#define BITCOUNT_AVERAGE 0.95f
// How long to flush item when something important happens
#define EFFECT_TIME  1.5f
// How long to latch peak bit count for item
#define PEAK_LATCH_TIME 2.0f;

//-----------------------------------------------------------------------------
// Purpose: Entity report event types
//-----------------------------------------------------------------------------
enum
{
	FENTITYBITS_ZERO = 0,
	FENTITYBITS_ADD = 0x01,
	FENTITYBITS_LEAVEPVS = 0x02,
	FENTITYBITS_DELETE = 0x04,
};

//-----------------------------------------------------------------------------
// Purpose: Data about an entity
//-----------------------------------------------------------------------------
typedef struct
{
	// Bits used for last message
	int				bits;
	// Rolling average of bits used
	float			average;
	// Last bit peak
	int				peak;
	// Time at which peak was last reset
	float			peaktime;
	// Event info
	int				flags;
	// If doing effect, when it will finish
	float			effectfinishtime;
	// If event was deletion, remember client class for a little bit
	ClientClass		*deletedclientclass;
} ENTITYBITS;

// List of entities we are keeping data bout
static ENTITYBITS s_EntityBits[ MAX_EDICTS ];

//-----------------------------------------------------------------------------
// Purpose: Zero out structure ( level transition/startup )
//-----------------------------------------------------------------------------
void CL_ResetEntityBits( void )
{
	memset( s_EntityBits, 0, sizeof( s_EntityBits ) );
}

//-----------------------------------------------------------------------------
// Purpose: Record activity
// Input  : entnum - 
//			bitcount - 
//-----------------------------------------------------------------------------
void CL_RecordEntityBits( int entnum, int bitcount )
{
	if ( entnum < 0 || entnum >= MAX_EDICTS ) 
	{
		return;
	}

	ENTITYBITS *slot = &s_EntityBits[ entnum ];

	slot->bits = bitcount;
	// Update average
	slot->average = ( BITCOUNT_AVERAGE ) * slot->average + ( 1.f - BITCOUNT_AVERAGE ) * bitcount;

	// Recompute peak
	if ( realtime >= slot->peaktime )
	{
		slot->peak = 0.0f;
		slot->peaktime = realtime + PEAK_LATCH_TIME;
	}

	// Store off peak
	if ( bitcount > slot->peak )
	{
		slot->peak = bitcount;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Record entity add event
// Input  : entnum - 
//-----------------------------------------------------------------------------
void CL_RecordAddEntity( int entnum )
{
	if ( !cl_entityreport.GetBool() || entnum < 0 || entnum >= MAX_EDICTS )
	{
		return;
	}

	ENTITYBITS *slot = &s_EntityBits[ entnum ];
	slot->flags = FENTITYBITS_ADD;
	slot->effectfinishtime = realtime + EFFECT_TIME;
}

//-----------------------------------------------------------------------------
// Purpose: record entity leave event
// Input  : entnum - 
//-----------------------------------------------------------------------------
void CL_RecordLeavePVS( int entnum )
{
	if ( !cl_entityreport.GetBool() || entnum < 0 || entnum >= MAX_EDICTS )
	{
		return;
	}

	ENTITYBITS *slot = &s_EntityBits[ entnum ];
	slot->flags = FENTITYBITS_LEAVEPVS;
	slot->effectfinishtime = realtime + EFFECT_TIME;
}

//-----------------------------------------------------------------------------
// Purpose: record entity deletion event
// Input  : entnum - 
//			*pclass - 
//-----------------------------------------------------------------------------
void CL_RecordDeleteEntity( int entnum, ClientClass *pclass )
{
	if ( !cl_entityreport.GetBool() || entnum < 0 || entnum >= MAX_EDICTS )
	{
		return;
	}

	ENTITYBITS *slot = &s_EntityBits[ entnum ];
	slot->flags = FENTITYBITS_DELETE;
	slot->effectfinishtime = realtime + EFFECT_TIME;
	slot->deletedclientclass = pclass;
}

//-----------------------------------------------------------------------------
// Purpose: Shows entity status report if cl_entityreport cvar is set
//-----------------------------------------------------------------------------
class CEntityReportPanel : public CBasePanel
{
	typedef CBasePanel BaseClass;
public:
	// Construction
					CEntityReportPanel( vgui::Panel *parent );
	virtual			~CEntityReportPanel( void );

	// Refresh
	virtual void	Paint();
	virtual void	ApplySchemeSettings( vgui::IScheme *pScheme );
	virtual bool	ShouldDraw( void );

	// Helpers
	virtual void	ApplyEffect( ENTITYBITS *entry, int& r, int& g, int& b );

private:
	// Font to use for drawing
	vgui::HFont		m_hFont;
};

static CEntityReportPanel *g_pEntityReportPanel = NULL;

//-----------------------------------------------------------------------------
// Purpose: Creates the CEntityReportPanel VGUI panel
// Input  : *parent - 
//-----------------------------------------------------------------------------
void CL_CreateEntityReportPanel( vgui::Panel *parent )
{
	g_pEntityReportPanel = new CEntityReportPanel( parent );
}

//-----------------------------------------------------------------------------
// Purpose: Instances the entity report panel
// Input  : *parent - 
//-----------------------------------------------------------------------------
CEntityReportPanel::CEntityReportPanel( vgui::Panel *parent ) :
	CBasePanel( parent, "CEntityReportPanel" )
{
	// Need parent here, before loading up textures, so getSurfaceBase 
	//  will work on this panel ( it's null otherwise )
	SetSize( videomode->GetModeWidth(), videomode->GetModeHeight() );
	SetPos( 0, 0 );
	SetVisible( true );
	SetCursor( null );

	m_hFont = vgui::INVALID_FONT;

	SetFgColor( Color( 0, 0, 0, 255 ) );
	SetPaintBackgroundEnabled( false );
	SetPaintBorderEnabled(false);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CEntityReportPanel::~CEntityReportPanel( void )
{
}

void CEntityReportPanel::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );

	// If you change this font, be sure to mark it with
	// $use_in_fillrate_mode in its .vmt file
	m_hFont = pScheme->GetFont( "DefaultVerySmall", false );
	Assert( m_hFont );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CEntityReportPanel::ShouldDraw( void )
{
	if ( !cl_entityreport.GetInt() )
	{
		return false;
	}
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Helper to flash colors
// Input  : cycle - 
//			value - 
// Output : static int
//-----------------------------------------------------------------------------
static int MungeColorValue( float cycle, int& value )
{
	int midpoint;
	int remaining;
	bool invert = false;

	if ( value < 128 )
	{
		invert = true;
		value = 255 - value;
	}

	midpoint = value / 2;

	remaining = value - midpoint;
	midpoint = midpoint + remaining / 2;
		
	value = midpoint + ( remaining / 2 ) * cycle;
	if ( invert )
	{
		value = 255 - value;
	}

	value = max( 0, value );
	value = min( 255, value );
	return value;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : frac - 
//			r - 
//			g - 
//			b - 
//-----------------------------------------------------------------------------
void CEntityReportPanel::ApplyEffect( ENTITYBITS *entry, int& r, int& g, int& b )
{
	bool effectactive = ( realtime <= entry->effectfinishtime ) ? true : false;
	if ( !effectactive )
		return;

	float frequency = 3.0f;

	float frac = ( EFFECT_TIME - ( entry->effectfinishtime - realtime ) ) / EFFECT_TIME;
	frac = min( 1.0, frac );
	frac = max( 0.0, frac );

	frac *= 2.0 * M_PI;
	frac = sin( frequency * frac );

	if ( entry->flags & FENTITYBITS_LEAVEPVS )
	{
		r = MungeColorValue( frac, r );
	}
	else if ( entry->flags & FENTITYBITS_ADD )
	{
		g = MungeColorValue( frac, g );
	}
	else if ( entry->flags & FENTITYBITS_DELETE )
	{
		r = MungeColorValue( frac, r );
		g = MungeColorValue( frac, g );
		b = MungeColorValue( frac, b );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEntityReportPanel::Paint() 
{
	VPROF( "CEntityReportPanel::Paint" );

	if ( !m_hFont )
		return;

	if ( !cl.IsActive() )
		return;

	if ( !entitylist )
		return;

	int top = 5;
	int left = 5;
	int row = 0;
	int col = 0;
	int colwidth = 160;
	int rowheight = vgui::surface()->GetFontTall( m_hFont );

	IClientNetworkable *pNet;
	ClientClass			*pClientClass;
	bool				inpvs;
	int					r, g, b, a;
	bool				effectactive;
	ENTITYBITS			*entry;

	int lastused = entitylist->GetMaxEntities()-1;

	while ( lastused > 0 )
	{
		pNet	= entitylist->GetClientNetworkable( lastused );

		entry = &s_EntityBits[ lastused ];

		effectactive = ( realtime <= entry->effectfinishtime ) ? true : false;

		if ( pNet && pNet->GetClientClass() )
		{
			break;
		}

		if ( effectactive )
			break;

		lastused--;
	}

 	int start = 0;
 	if ( cl_entityreport.GetInt() > 1 )
 	{
 		start = cl_entityreport.GetInt();
 	}

	for ( int i = start; i <= lastused; i++ )
	{
		pNet	= entitylist->GetClientNetworkable( i );

		entry = &s_EntityBits[ i ];

		effectactive = ( realtime <= entry->effectfinishtime ) ? true : false;

		if ( pNet && ((pClientClass = pNet->GetClientClass())) != NULL )
		{
			inpvs = !pNet->IsDormant();
			if ( inpvs )
			{
 				if ( entry->average >= 5 )
 				{
 					r = 200; g = 200; b = 250;
 					a = 255;
 				}
 				else
 				{
 					r = 200; g = 255; b = 100;
 					a = 255;
 				}
			}
			else
			{
				r = 255; g = 150; b = 100;
				a = 255;
			}

			ApplyEffect( entry, r, g, b );

			char	text[256];
			wchar_t unicode[ 256 ];

			Q_snprintf( text, sizeof(text), "(%i) %s", i, pClientClass->m_pNetworkName );
			
			g_pVGuiLocalize->ConvertANSIToUnicode( text, unicode, sizeof( unicode ) );

			DrawColoredText( m_hFont, left + col * colwidth, top + row * rowheight, r, g, b, a, unicode );

			if ( inpvs )
			{
				float fracs[ 3 ];
				fracs[ 0 ] = (float)( entry->bits >> 3 ) / 100.0f;
				fracs[ 1 ] = (float)( entry->peak >> 3 ) / 100.0f;
				fracs[ 2 ] = (float)( (int)entry->average >> 3 ) / 100.0f;

				for ( int j = 0; j < 3; j++ )
				{
					fracs[ j ] = max( 0.0f, fracs[ j ] );
					fracs[ j ] = min( 1.0f, fracs[ j ] );
				}

				int rcright =  left + col * colwidth + colwidth-2;
				int wide = colwidth / 3;
				int rcleft = rcright - wide;
				int rctop = top + row * rowheight;
				int rcbottom = rctop + rowheight - 1;

				vgui::surface()->DrawSetColor( 63, 63, 63, 127 );
 				vgui::surface()->DrawFilledRect( rcleft, rctop, rcright, rcbottom );

				// draw a box around it
				vgui::surface()->DrawSetColor( 200, 200, 200, 127 );
				vgui::surface()->DrawOutlinedRect( rcleft, rctop, rcright, rcbottom );

				// Draw current as a filled rect
				vgui::surface()->DrawSetColor( 200, 255, 100, 192 );
				vgui::surface()->DrawFilledRect( rcleft, rctop + rowheight / 2, rcleft + wide * fracs[ 0 ], rcbottom - 1 );

				// Draw average a vertical bar
				vgui::surface()->DrawSetColor( 192, 192, 192, 255 );
				vgui::surface()->DrawFilledRect( rcleft + wide * fracs[ 2 ], rctop + rowheight / 2, rcleft + wide * fracs[ 2 ] + 1, rcbottom - 1 );

				// Draw peak as a vertical red tick
				vgui::surface()->DrawSetColor( 192, 0, 0, 255 );
				vgui::surface()->DrawFilledRect( rcleft + wide * fracs[ 1 ], rctop + 1, rcleft + wide * fracs[ 1 ] + 1, rctop + rowheight / 2 );
			}

		}
		/*else
		{
			r = 63; g = 63; b = 63;
			a = 220;

			ApplyEffect( entry, r, g, b );

			wchar_t unicode[ 256 ];
			g_pVGuiLocalize->ConvertANSIToUnicode( ( effectactive && entry->deletedclientclass ) ? 
				  entry->deletedclientclass->m_pNetworkName : "unused", unicode, sizeof( unicode ) );

			DrawColoredText( m_hFont, left + col * colwidth, top + row * rowheight, r, g, b, a, 
				L"(%i) %s", i, unicode );
		}*/

		row++;
		if ( top + row * rowheight > videomode->GetModeHeight() - rowheight )
		{
			row = 0;
			col++;
			// No more space anyway, give up
			if ( left + ( col + 1 ) * 200 > videomode->GetModeWidth() )
				return;
		}
	}
}

