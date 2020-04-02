//***************************************************************************************
//
// File supervisor: Robert Belanger
//
// (c) Copyright 2001 Avid Technology, Inc. . All rights reserved.
//
//
// @doc
//
// @module      BufferFilter.h | Main header file for BufferFilter implementation
//***************************************************************************************

/****************************************************************************************
THIS CODE IS PUBLISHED AS A SAMPLE ONLY AND IS PROVIDED "AS IS".
IN NO EVENT SHALL SOFTIMAGE, AVID TECHNOLOGY, INC. AND/OR THEIR RESPECTIVE 
SUPPLIERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY
DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS CODE . 
 
COPYRIGHT NOTICE. Copyright � 1999-2002 Avid Technology Inc. . All rights reserved. 

SOFTIMAGE is a registered trademark of Avid Technology Inc. or its subsidiaries 
or divisions. Windows NT is a registered trademark of Microsoft Corp. All other
trademarks contained herein are the property of their respective owners. 
****************************************************************************************/
#ifndef _LEGACYFILTER_H_
#define _LEGACYFILTER_H_

#include <SIBCUtil.h>
#include <SIBCString.h>

#include "CXSIFilter.h"

#define BUFFER_FILTER_NAME	"Buffer"
#define BUFFER_FILTER_ID		"buf"

#define	BUFFER_SIZE				(128 * 1024)

//! Legacy filter where we read the entire scene in (used only for binary 3.0)
/*
 * This class is used only for legacy read.
 */
class CLegacyFilter : public CXSIFilter
{
public:
	/*!
	 * Constructor
	*/
	CLegacyFilter();
	/*!
	 * Destructor
	*/
	virtual ~CLegacyFilter();

	/*! Opens the file for either reading or writing
	 * \param in_szFilename	Filename
	 * \param in_Mode	mode
	*/
	int	Open ( CSIBCString in_szFilename,  _SI_FILE_MODE in_Mode );
	/*! Closes the file when finished
	*/
	int	Close ();
	
	/*! Reads a number of bytes into the buffer passed in.
	 * \param out_pBuffer	output buffer
	 * \param SI_Long	number of bytes to read
	 * \return	in_lSize		number of bytes read.
	*/
	int Read ( SI_Char * out_pBuffer, SI_Long in_lSize );
	/*! Writes a number of bytes to the file
	 * \param in_pBuffer	input buffer
	 * \param in_lSize	number of bytes to write
	 * \return int		number of bytes written
	*/
	int Write( SI_Char * in_pBuffer,  SI_Long in_lSize );


	/*! Tests whether we have reached the end of file
	 * \return int	flag for end of file (1 - end of file, 0 - not end of file)
	*/
	int Eof();
	/*! Returns the current position in the file.
	 * \returns int	position in the file (0 - beginning)
	*/
	int Tell();
private:
	void	Flush();

	CSIAccumString		m_szAccumBuffer;

};

#endif
