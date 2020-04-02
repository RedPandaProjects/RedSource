//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include <stdio.h>

#include <vgui/IPanel.h>
#include <vgui/IClientPanel.h>
#include <vgui/ISurface.h>
#include <vgui/IVGui.h>
#include <vgui/Cursor.h>

#include "vgui_internal.h"
#include "VPanel.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

using namespace vgui;

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
VPanel::VPanel()
{
	_pos[0] = _pos[1] = 0;
	_absPos[0] = _absPos[1] = 0;
	_size[0] = _size[1] = 0;

	_minimumSize[0] = 0;
	_minimumSize[1] = 0;

	_zpos = 0;

	_inset[0] = _inset[1] = _inset[2] = _inset[3] = 0;
	_clipRect[0] = _clipRect[1] = _clipRect[2] = _clipRect[3] = 0;

	_visible = true;
	_enabled = true;
	_clientPanel = NULL;
	_parent = NULL;
	_plat = NULL;
	_popup = false;
	_isTopmostPopup = false;
	_hPanel = INVALID_PANEL;

	_mouseInput = true; // by default you want mouse and kb input to this panel
	_kbInput = true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
VPanel::~VPanel()
{
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::Init(IClientPanel *attachedClientPanel)
{
	_clientPanel = attachedClientPanel;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::Solve()
{
	int absX = _pos[0];
	int absY = _pos[1];
	_absPos[0] = _pos[0];
	_absPos[1] = _pos[1];

	// put into parent space
	VPanel *parent = GetParent();
	if (IsPopup())
	{
		// if we're a popup, draw at the highest level
		parent = (VPanel *)g_pSurface->GetEmbeddedPanel();
	}

	int pinset[4] = {0, 0, 0, 0}; 
	if ( parent )
	{
		parent->GetInset( pinset[0], pinset[1], pinset[2], pinset[3] );

		int pabsX, pabsY;
		parent->GetAbsPos(pabsX, pabsY);

		absX += pabsX + pinset[0];
		absY += pabsY + pinset[1];

		_absPos[0] = clamp( absX, -32767, 32767 );
		_absPos[1] = clamp( absY, -32767, 32767 );
	}

	// set initial bounds
	_clipRect[0] = _absPos[0];
	_clipRect[1] = _absPos[1];

	int wide, tall;
	GetSize( wide, tall );

	int absX2 = absX + wide;
	int absY2 = absY + tall;
	_clipRect[2] = clamp( absX2, -32767, 32767 );
	_clipRect[3] = clamp( absY2, -32767, 32767 );

	// clip to parent, if we're not a popup
	if ( parent && !IsPopup() )
	{ 
		int pclip[4];
		parent->GetClipRect(pclip[0], pclip[1], pclip[2], pclip[3]);

		if (_clipRect[0] < pclip[0])
		{
			_clipRect[0] = pclip[0];
		}

		if (_clipRect[1] < pclip[1])
		{
			_clipRect[1] = pclip[1];
		}

		if(_clipRect[2] > pclip[2])
		{
			_clipRect[2] = pclip[2] - pinset[2];
		}

		if(_clipRect[3] > pclip[3])
		{
			_clipRect[3] = pclip[3] - pinset[3];
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetPos(int x, int y)
{
	_pos[0] = x;
	_pos[1] = y;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::GetPos(int &x, int &y)
{
	x = _pos[0];
	y = _pos[1];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetSize(int wide,int tall)
{
	if (wide<_minimumSize[0])
	{
		wide=_minimumSize[0];
	}
	if (tall<_minimumSize[1])
	{
		tall=_minimumSize[1];
	}

	if (_size[0] == wide && _size[1] == tall)
		return;

	_size[0]=wide;
	_size[1]=tall;

	Client()->OnSizeChanged(wide, tall);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::GetSize(int& wide,int& tall)
{
	wide=_size[0];
	tall=_size[1];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetMinimumSize(int wide,int tall)
{
	_minimumSize[0]=wide;
	_minimumSize[1]=tall;

	// check if we're currently smaller than the new minimum size
	int currentWidth = _size[0];
	if (currentWidth < wide)
	{
		currentWidth = wide;
	}
	int currentHeight = _size[1];
	if (currentHeight < tall)
	{
		currentHeight = tall;
	}

	// resize to new minimum size if necessary
	if (currentWidth != _size[0] || currentHeight != _size[1])
	{
		SetSize(currentWidth, currentHeight);
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::GetMinimumSize(int &wide, int &tall)
{
	wide = _minimumSize[0];
	tall = _minimumSize[1];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetVisible(bool state)
{
	if (_visible == state)
		return;

	// need to tell the surface (in case special window processing needs to occur)
	g_pSurface->SetPanelVisible((VPANEL)this, state);

	_visible = state;

	if( IsPopup() )
	{
		vgui::g_pSurface->CalculateMouseVisible();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetEnabled(bool state)
{
	_enabled = state;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool VPanel::IsVisible()
{
	return _visible;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool VPanel::IsEnabled()
{
	return _enabled;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::GetAbsPos(int &x, int &y)
{
	x = _absPos[0];
	y = _absPos[1];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::GetClipRect(int &x0, int &y0, int &x1, int &y1)
{
	x0 = _clipRect[0];
	y0 = _clipRect[1];
	x1 = _clipRect[2];
	y1 = _clipRect[3];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetInset(int left, int top, int right, int bottom)
{
	_inset[0] = left;
	_inset[1] = top;
	_inset[2] = right;
	_inset[3] = bottom;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::GetInset(int &left, int &top, int &right, int &bottom)
{
	left = _inset[0];
	top = _inset[1];
	right = _inset[2];
	bottom = _inset[3];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void VPanel::SetParent(VPanel *newParent)
{
	if (this == newParent)
		return;

	if (_parent == newParent)
		return;
	
	if (_parent != NULL)
	{
		_parent->_childDar.RemoveElement(this);
		_parent = null;
	}

	if (newParent != NULL)
	{
		_parent = newParent;
		_parent->_childDar.PutElement(this);
		SetZPos(_zpos);						// re-sort parent's panel order if necessary
		if (_parent->Client())
		{
			_parent->Client()->OnChildAdded((VPANEL)this);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
int VPanel::GetChildCount()
{
	return _childDar.GetCount();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
VPanel *VPanel::GetChild(int index)
{
	return _childDar[index];
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
VPanel *VPanel::GetParent()
{
	return _parent;
}

//-----------------------------------------------------------------------------
// Purpose: Sets the Z position of a panel and reorders it appropriately
//-----------------------------------------------------------------------------
void VPanel::SetZPos(int z)
{
	_zpos = z;

	if (_parent)
	{
		// find the child in the list
		int childCount = _parent->GetChildCount();
		int i;
		for (i = 0; i < childCount; i++)
		{
			if (_parent->GetChild(i) == this)
				break;
		}

		if (i == childCount)
			return;

		while (1)
		{
			VPanel *prevChild = NULL, *nextChild = NULL;

			if ( i > 0 )
			{
				prevChild = _parent->GetChild( i - 1 );
			}
			if ( i <(childCount - 1) )
			{
				nextChild = _parent->GetChild( i + 1 );
			}

			// check either side of the child to see if it should move
			if ( i > 0 && prevChild && ( prevChild->_zpos > _zpos ) )
			{
				// swap with the lower
				_parent->_childDar.SetElementAt(prevChild, i);
				_parent->_childDar.SetElementAt(this, i - 1);
				i--;
			}
			else if (i < (childCount - 1) && nextChild && ( nextChild->_zpos < _zpos ) )
			{
				// swap with the higher
				_parent->_childDar.SetElementAt(nextChild, i);
				_parent->_childDar.SetElementAt(this, i + 1);
				i++;
			}
			else
			{
				break;
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: returns the z position of this panel
//-----------------------------------------------------------------------------
int VPanel::GetZPos()
{
	return _zpos;
}

//-----------------------------------------------------------------------------
// Purpose: Moves the panel to the front of the z-order
//-----------------------------------------------------------------------------
void VPanel::MoveToFront(void)
{
	g_pSurface->MovePopupToFront((VPANEL)this);

	if (_parent)
	{
		// move this panel to the end of it's parents list
		_parent->_childDar.MoveElementToEnd(this);

		// Validate the Z order
		int i = _parent->_childDar.GetCount() - 2;
		while (i >= 0)
		{
			if (_parent->_childDar[i]->_zpos > _zpos)
			{
				// we can't be in front of this; swap positions
				_parent->_childDar.SetElementAt(_parent->_childDar[i], i + 1);
				_parent->_childDar.SetElementAt(this, i);

				// check the next value
				i--;
			}
			else
			{
				// order valid
				break;
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Moves the panel to the back of the z-order
//-----------------------------------------------------------------------------
void VPanel::MoveToBack()
{
	if (_parent)
	{
		// move this panel to the end of it's parents list
		_parent->_childDar.RemoveElement(this);
		_parent->_childDar.InsertElementAt(this, 0);

		// Validate the Z order
		int i = 1;
		while (i < _parent->_childDar.GetCount())
		{
			if (_parent->_childDar[i]->_zpos < _zpos)
			{
				// we can't be behind this; swap positions
				_parent->_childDar.SetElementAt(_parent->_childDar[i], i - 1);
				_parent->_childDar.SetElementAt(this, i);

				// check the next value
				i++;
			}
			else
			{
				// order valid
				break;
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Iterates up the hierarchy looking to see if a panel has the specified ancestor
//-----------------------------------------------------------------------------
bool VPanel::HasParent(VPanel *potentialParent)
{
	if (this == potentialParent)
		return true;

	if (_parent)
	{
		return _parent->HasParent(potentialParent);
	}

	return false;
}

SurfacePlat *VPanel::Plat()
{
	return _plat;
}

void VPanel::SetPlat(SurfacePlat *Plat)
{
	_plat = Plat;
}

bool VPanel::IsPopup()
{
	return _popup;
}

void VPanel::SetPopup(bool state)
{
	_popup = state;
}

bool VPanel::IsTopmostPopup() const
{
	return _isTopmostPopup;
}

void VPanel::SetTopmostPopup( bool bEnable )
{
	_isTopmostPopup = bEnable;
}

bool VPanel::IsFullyVisible()
{
	// recursively check to see if the panel and all it's parents are visible
	VPanel *panel = this;
	while (panel)
	{
		if (!panel->_visible)
		{
			return false;
		}

		panel = panel->_parent;
	}

	// we're visible all the way up the hierarchy
	return true;
}

const char *VPanel::GetName()
{
	return Client()->GetName();
}

const char *VPanel::GetClassName()
{
	return Client()->GetClassName();
}

HScheme VPanel::GetScheme()
{
	return Client()->GetScheme();
}


void VPanel::SendMessage(KeyValues *params, VPANEL ifrompanel)
{
	Client()->OnMessage(params, ifrompanel);
}


void VPanel::SetKeyBoardInputEnabled(bool state)
{
	_kbInput = state;
}

void VPanel::SetMouseInputEnabled(bool state)
{
	_mouseInput = state;
}

bool VPanel::IsKeyBoardInputEnabled()
{
	return _kbInput;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool VPanel::IsMouseInputEnabled()
{
	return _mouseInput;
}
