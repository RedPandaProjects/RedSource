//====== Copyright � 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#include "dme_controls/AttributeSequencePickerPanel.h"
#include "FileSystem.h"
#include "vgui_controls/Button.h"
#include "vgui_controls/FileOpenDialog.h"
#include "dme_controls/AttributeTextEntry.h"
#include "dme_controls/MDLPicker.h"
#include "dme_controls/sequencepicker.h"
#include "tier1/keyvalues.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


using namespace vgui;


//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CAttributeSequencePickerPanel::CAttributeSequencePickerPanel( vgui::Panel *parent, const AttributeWidgetInfo_t &info ) :
	BaseClass( parent, info )
{
}

CAttributeSequencePickerPanel::~CAttributeSequencePickerPanel()
{
}


//-----------------------------------------------------------------------------
// Called when it's time to show the MDL picker
//-----------------------------------------------------------------------------
void CAttributeSequencePickerPanel::ShowPickerDialog()
{
	CMDLPickerFrame *pMDLPickerDialog = new CMDLPickerFrame( this, "Select .MDL File" );
	pMDLPickerDialog->AddActionSignalTarget( this );
	pMDLPickerDialog->DoModal( );
}


//-----------------------------------------------------------------------------
// Called when it's time to show the MDL picker
//-----------------------------------------------------------------------------
void CAttributeSequencePickerPanel::OnMDLSelected( KeyValues *pKeyValues )
{
	const char *pMDLName = pKeyValues->GetString( "mdl", NULL );
	ShowSequencePickerDialog( pMDLName );
}


//-----------------------------------------------------------------------------
// Called when it's time to show the sequence picker
//-----------------------------------------------------------------------------
void CAttributeSequencePickerPanel::ShowSequencePickerDialog( const char *pMDLName )
{
	if ( !pMDLName || !pMDLName[ 0 ] )
		return;

	// Open file
	CSequencePicker::PickType_t pickType = CSequencePicker::PICK_ALL;
	const char *pTextType = GetTextType();
	if ( pTextType )
	{
		if ( !Q_stricmp( pTextType, "activityName" ) )
		{
			pickType = CSequencePicker::PICK_ACTIVITIES;
		}
		else if ( !Q_stricmp( pTextType, "sequenceName" ) )
		{
			pickType = CSequencePicker::PICK_SEQUENCES;
		}
	}

	CSequencePickerFrame *pSequencePickerDialog = new CSequencePickerFrame( this, pickType );
	pSequencePickerDialog->AddActionSignalTarget( this );
	pSequencePickerDialog->DoModal( pMDLName );
}


//-----------------------------------------------------------------------------
// Called when it's time to show the MDL picker
//-----------------------------------------------------------------------------
void CAttributeSequencePickerPanel::OnSequenceSelected( KeyValues *pKeyValues )
{
	// We're either going to get an activity or sequence name
	const char *pActivityName = pKeyValues->GetString( "activity", NULL );
	const char *pSequenceName = pKeyValues->GetString( "sequence", pActivityName );
	if ( !pSequenceName || !pSequenceName[ 0 ] )
		return;

	// Apply to text panel
	m_pData->SetText( pSequenceName );
	SetDirty(true);
	if ( IsAutoApply() )
	{
		Apply();
	}
}
