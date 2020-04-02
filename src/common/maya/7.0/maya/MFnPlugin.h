#ifndef _MFnPlugin
#define _MFnPlugin
//
//-
// ==========================================================================
// Copyright (C) 1995 - 2005 Alias Systems Corp. and/or its licensors.  All 
// rights reserved.
// 
// The coded instructions, statements, computer programs, and/or related 
// material (collectively the "Data") in these files contain unpublished 
// information proprietary to Alias Systems Corp. ("Alias") and/or its 
// licensors, which is protected by Canadian and US federal copyright law and 
// by international treaties.
// 
// The Data may not be disclosed or distributed to third parties or be copied 
// or duplicated, in whole or in part, without the prior written consent of 
// Alias.
// 
// THE DATA IS PROVIDED "AS IS". ALIAS HEREBY DISCLAIMS ALL WARRANTIES RELATING 
// TO THE DATA, INCLUDING, WITHOUT LIMITATION, ANY AND ALL EXPRESS OR IMPLIED 
// WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND/OR FITNESS FOR A 
// PARTICULAR PURPOSE. IN NO EVENT SHALL ALIAS BE LIABLE FOR ANY DAMAGES 
// WHATSOEVER, WHETHER DIRECT, INDIRECT, SPECIAL, OR PUNITIVE, WHETHER IN AN 
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, OR IN EQUITY, 
// ARISING OUT OF ACCESS TO, USE OF, OR RELIANCE UPON THE DATA.
// ==========================================================================
//+
//
// CLASS:    MFnPlugin
//
// *****************************************************************************
//
// CLASS DESCRIPTION (MFnPlugin)
//
//  This class is used in the initializePlugin and uninitializePlugin functions
//  in a Maya plugin in order to register new services with Maya.  The
//  constructor for this class is passed the MObject provided by Maya as an
//  argument to initializePlugin and uninitializePlugin.  Subsequent calls are
//  made to the various "register" methods from inside initializePlugin, and to
//  the various "deregister" methods from inside uninitializePlugin.
//
//  A side effect of including MFnPlugin.h is to embed an API version string
//  into the .o file produced.  If it is necessary to include MFnPlugin.h
//  into more than one file that comprises a plugin, the preprocessor macro
//  MNoVersionString should be defined in all but one of those files
//  prior to the inclusion of MFnPlugin.h.  If this is not done, the linker
//  will produce warnings about multiple definitions of the variable
//  MApiVersion as the .so file is being produced.  These warning are
//  harmless and can be ignored.  Normally, this will not arise as only the
//  file that contains the initializePlugin and uninitializePlugin
//  routines should need to include MFnPlugin.h. Plugin developers on the 
//  Window's API developers will have to define MNoPluginEntry to suppress the
//  the creation of multiple definitions of DllMain. 
//
// *****************************************************************************

#if defined __cplusplus

// *****************************************************************************

// INCLUDED HEADER FILES


#include <maya/MFnBase.h>
#include <maya/MApiVersion.h>
#include <maya/MPxNode.h>
#include <maya/MPxData.h>

#if !defined(MNoPluginEntry)
#ifdef NT_PLUGIN
#include <maya/MTypes.h>
HINSTANCE MhInstPlugin;

extern "C" int APIENTRY
DllMain(HINSTANCE hInstance, DWORD /* dwReason */, LPVOID /* lpReserved */)
{
	MhInstPlugin = hInstance;
	return 1;
}
#endif // NT_PLUGIN
#endif // MNoPluginEntry

// *****************************************************************************

// DECLARATIONS

class MString;
class MFileObject;
class MTypeId;

// *****************************************************************************

// CLASS DECLARATION (MFnPlugin)

/// Register and deregister plug-in services with Maya
/**
  Register plug-in supplied, commands, dependency nodes, etc. with Maya
*/
#ifdef _WIN32
#pragma warning(disable: 4522)
#endif // _WIN32

class OPENMAYA_EXPORT MFnPlugin : public MFnBase 
{
public:
	///
					MFnPlugin();
	///
					MFnPlugin( MObject& object,
							   const char* vendor = "Unknown",
							   const char* version = "Unknown",
							   const char* requiredApiVersion = "Any",
							   MStatus* ReturnStatus = 0L );
	///
	virtual			~MFnPlugin();
	///
	virtual			MFn::Type type() const; 

	///
	MString			vendor( MStatus* ReturnStatus=NULL ) const;
	///
	MString			version( MStatus* ReturnStatus=NULL ) const;
	///
	MString			apiVersion( MStatus* ReturnStatus=NULL ) const;
	///
	MString			name( MStatus* ReturnStatus=NULL ) const;
	///
	MString			loadPath( MStatus* ReturnStatus=NULL ) const;
	///
	MStatus			setName( const MString& newName,
							 bool allowRename = true );

	///
	MStatus			setVersion( const MString& newVersion );

	///
	MStatus			registerCommand(const MString& commandName,
									MCreatorFunction creatorFunction,
									MCreateSyntaxFunction 
									    createSyntaxFunction = NULL);
	///
	MStatus			deregisterCommand(	const MString& commandName );
	///
	MStatus 		registerModelEditorCommand(const MString& commandName,
								   		MCreatorFunction creatorFunction,
								   		MCreatorFunction paneCreatorFunction);
	///
	MStatus			deregisterModelEditorCommand(const MString& commandName);
	///
    MStatus         registerContextCommand( const MString& commandName,
											MCreatorFunction creatorFunction);

	///
    MStatus         registerContextCommand( const MString& commandName,
											MCreatorFunction creatorFunction,
											const MString& toolCmdName,
											MCreatorFunction toolCmdCreator,
											MCreateSyntaxFunction
												toolCmdSyntax = NULL
											);

	///
    MStatus         deregisterContextCommand( const MString& commandName );
	///
    MStatus         deregisterContextCommand( const MString& commandName,
											  const MString& toolCmdName );
	///
	MStatus			registerNode(	const MString& typeName,
									const MTypeId& typeId,
									MCreatorFunction creatorFunction,
									MInitializeFunction initFunction,
									MPxNode::Type type = MPxNode::kDependNode,
									const MString* classification = NULL); 
	///
	MStatus			deregisterNode(	const MTypeId& typeId );
	///
	MStatus			registerShape(	const MString& typeName,
									const MTypeId& typeId,
									MCreatorFunction creatorFunction,
									MInitializeFunction initFunction,
									MCreatorFunction uiCreatorFunction,
									const MString* classification = NULL); 
	///
	MStatus			registerTransform(	const MString& typeName,
										const MTypeId& typeId,
										MCreatorFunction creatorFunction,
										MInitializeFunction initFunction,
										MCreatorFunction xformCreatorFunction,
										const MTypeId& xformId,
										const MString* classification = NULL);
	///
	MStatus			registerData(	const MString& typeName,
									const MTypeId& typeId,
									MCreatorFunction creatorFunction,
									MPxData::Type type = MPxData::kData );
	///
	MStatus			deregisterData(	const MTypeId& typeId );
	///
	MStatus         registerDevice( const MString& deviceName,
									MCreatorFunction creatorFunction );
	///
	MStatus         deregisterDevice( const MString& deviceName );
	///
	MStatus			registerFileTranslator( const MString& translatorName,
										char* pixmapName,
										MCreatorFunction creatorFunction,
										char* optionsScriptName = NULL,
										char* defaultOptionsString = NULL,
										bool requiresFullMel = false );
	///
	MStatus			deregisterFileTranslator( const MString& translatorName );
	///
	MStatus			registerIkSolver( const MString& ikSolverName,
										MCreatorFunction creatorFunction );
	///
	MStatus			deregisterIkSolver( const MString& ikSolverName );
	///
	MStatus			registerUI(const MString & creationProc,
							   const MString & deletionProc,
							   const MString & creationBatchProc = "",
							   const MString & deletionBatchProc = "");

	///
	MStatus			registerDragAndDropBehavior( const MString& behaviorName,
												 MCreatorFunction creatorFunction);

	///
	MStatus         deregisterDragAndDropBehavior( const MString& behaviorName );

	///
	static MObject  findPlugin( const MString& pluginName );

	///
	static bool		isNodeRegistered(	const MString& typeName);

	///
	MTypeId			matrixTypeIdFromXformId(const MTypeId& xformTypeId, MStatus* ReturnStatus=NULL);

	///
	MStringArray	addMenuItem(
							const MString& menuItemName,
							const MString& parentName, 
							const MString& commandName, 
							const MString& commandParams,
							bool needOptionBox = false,
							MString *optBoxFunction = NULL,
							MStatus *retStatus = NULL
							);
	///
	MStatus			removeMenuItem(MStringArray& menuItemNames);


protected:
	virtual const char* className() const;

private:
					MFnPlugin( const MObject& object,
							   const char* vendor = "Unknown",
							   const char* version = "Unknown",
							   const char* requiredApiVersion = "Any",
							   MStatus* ReturnStatus = 0L );
	MFnPlugin&		operator=( const MFnPlugin & ) const;
	MFnPlugin&		operator=( const MFnPlugin & );
	MFnPlugin*		operator& () const;
	MFnPlugin*		operator& ();
};
#ifdef _WIN32
#pragma warning(default: 4522)
#endif // _WIN32

// *****************************************************************************
#endif /* __cplusplus */
#endif /* _MFnPlugin */
