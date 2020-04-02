//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
//--------------------------------------------------------------------------------------------------------------
// downloadthread.cpp
// 
// Implementation file for optional HTTP asset downloading thread
// Author: Matthew D. Campbell (matt@turtlerockstudios.com), 2004
//--------------------------------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------------------------------
// Includes
//--------------------------------------------------------------------------------------------------------------

#ifdef _WIN32

#if !defined( _X360 )
#include "winlite.h"
#include <WinInet.h>
#endif
#include <assert.h>
#include <sys/stat.h>
#ifndef _LINUX
#undef fopen
#endif
#include <stdio.h>

#include "download_internal.h"
#include "tier1/strtools.h"

#if defined( _X360 )
#include "xbox/xbox_win32stubs.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//--------------------------------------------------------------------------------------------------------------
/**
 * Formats a string to spit out via OutputDebugString (only in debug).  OutputDebugString
 * is threadsafe, so this should be fine.
 *
 * Since I don't want to be playing with the developer cvar in the other thread, I'll use
 * the presence of _DEBUG as my developer flag.
 */
void Thread_DPrintf (char *fmt, ...)
{
#ifdef _DEBUG
	va_list		argptr;
	char		msg[4096];
		
	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof(msg), fmt, argptr );
	va_end( argptr );
	OutputDebugString( msg );
#endif // _DEBUG
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Convenience function to name status states for debugging
 */
static const char *StateString( DWORD dwStatus )
{
	switch (dwStatus)
	{
		case INTERNET_STATUS_RESOLVING_NAME:
			return "INTERNET_STATUS_RESOLVING_NAME";
		case INTERNET_STATUS_NAME_RESOLVED:
			return "INTERNET_STATUS_NAME_RESOLVED";
		case INTERNET_STATUS_CONNECTING_TO_SERVER:
			return "INTERNET_STATUS_CONNECTING_TO_SERVER";
		case INTERNET_STATUS_CONNECTED_TO_SERVER:
			return "INTERNET_STATUS_CONNECTED_TO_SERVER";
		case INTERNET_STATUS_SENDING_REQUEST:
			return "INTERNET_STATUS_SENDING_REQUEST";
		case INTERNET_STATUS_REQUEST_SENT:
			return "INTERNET_STATUS_REQUEST_SENT";
		case INTERNET_STATUS_REQUEST_COMPLETE:
			return "INTERNET_STATUS_REQUEST_COMPLETE";
		case INTERNET_STATUS_CLOSING_CONNECTION:
			return "INTERNET_STATUS_CLOSING_CONNECTION";
		case INTERNET_STATUS_CONNECTION_CLOSED:
			return "INTERNET_STATUS_CONNECTION_CLOSED";
		case INTERNET_STATUS_RECEIVING_RESPONSE:
			return "INTERNET_STATUS_RECEIVING_RESPONSE";
		case INTERNET_STATUS_RESPONSE_RECEIVED:
			return "INTERNET_STATUS_RESPONSE_RECEIVED";
		case INTERNET_STATUS_HANDLE_CLOSING:
			return "INTERNET_STATUS_HANDLE_CLOSING";
		case INTERNET_STATUS_HANDLE_CREATED:
			return "INTERNET_STATUS_HANDLE_CREATED";
		case INTERNET_STATUS_INTERMEDIATE_RESPONSE:
			return "INTERNET_STATUS_INTERMEDIATE_RESPONSE";
		case INTERNET_STATUS_REDIRECT:
			return "INTERNET_STATUS_REDIRECT";
		case INTERNET_STATUS_STATE_CHANGE:
			return "INTERNET_STATUS_STATE_CHANGE";
	}
	return "Unknown";
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Callback function to update status information for a download (connecting to server, etc)
 */
void __stdcall DownloadStatusCallback( HINTERNET hOpenResource, DWORD dwContext, DWORD dwStatus, LPVOID pStatusInfo, DWORD dwStatusInfoLength )
{
	RequestContext *rc = (RequestContext*)pStatusInfo;

	switch (dwStatus)
	{
		case INTERNET_STATUS_RESOLVING_NAME:
		case INTERNET_STATUS_NAME_RESOLVED:
		case INTERNET_STATUS_CONNECTING_TO_SERVER:
		case INTERNET_STATUS_CONNECTED_TO_SERVER:
		case INTERNET_STATUS_SENDING_REQUEST:
		case INTERNET_STATUS_REQUEST_SENT:
		case INTERNET_STATUS_REQUEST_COMPLETE:
		case INTERNET_STATUS_CLOSING_CONNECTION:
		case INTERNET_STATUS_CONNECTION_CLOSED:
			if ( rc )
			{
				rc->fetchStatus = dwStatus;
			}
			else
			{
				//Thread_DPrintf( "** No RequestContext **\n" );
			}
			//Thread_DPrintf( "DownloadStatusCallback %s\n", StateString(dwStatus) );
			break;
	}
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Reads data from a handle opened by InternetOpenUrl().
 */
void ReadData( RequestContext& rc )
{
	const int BufferSize = 2048;
	unsigned char data[BufferSize];
	DWORD dwSize = 0;

	if ( !rc.nBytesTotal )
	{
		rc.status = HTTP_ERROR;
		rc.error = HTTP_ERROR_ZERO_LENGTH_FILE;
		return;
	}
	rc.nBytesCurrent = rc.nBytesCached;
	rc.status = HTTP_FETCH;

	while ( !rc.shouldStop )
	{
		// InternetReadFile() will block until there is data, or the socket gets closed.  This means the
		// main thread could request an abort while we're blocked here.  This is okay, because the main
		// thread will not wait for this thread to finish, but will clean up the RequestContext at some
		// later point when InternetReadFile() has returned and this thread has finished.
		if ( !InternetReadFile( rc.hDataResource, (LPVOID)data, BufferSize, &dwSize ) )
		{
			// if InternetReadFile() returns 0, there was a socket error (connection closed, etc)
			rc.status = HTTP_ERROR;
			rc.error = HTTP_ERROR_CONNECTION_CLOSED;
			return;
		}
		if ( !dwSize )
		{
			// if InternetReadFile() succeeded, but we read 0 bytes, we're at the end of the file.

			// if the file doesn't exist, write it out
			char path[_MAX_PATH];
			Q_snprintf( path, sizeof(path), "%s\\%s", rc.basePath, rc.gamePath );
			struct stat buf;
			int rt = stat(path, &buf);
			if ( rt == -1 )
			{
				FILE *fp = fopen( path, "wb" );
				if ( fp )
				{
					fwrite( rc.data, rc.nBytesTotal, 1, fp );
					fclose( fp );
				}
			}

			// Let the main thread know we finished reading data, and wait for it to let us exit.
			rc.status = HTTP_DONE;
			return;
		}
		else
		{
			// We've read some data.  Make sure we won't walk off the end of our buffer, then
			// use memcpy() to save the data off.
			DWORD safeSize = (DWORD) min( rc.nBytesTotal - rc.nBytesCurrent, (int)dwSize );
			//Thread_DPrintf( "Read %d bytes @ offset %d\n", safeSize, rc.nBytesCurrent );
			if ( safeSize != dwSize )
			{
				//Thread_DPrintf( "Warning - read more data than expected!\n" );
			}
			if ( rc.data && safeSize > 0 )
			{
				memcpy( rc.data + rc.nBytesCurrent, data, safeSize );
			}
			rc.nBytesCurrent += safeSize;
		}
	}

	// If we get here, rc.shouldStop was set early (user hit cancel).
	// Let the main thread know we aborted properly.
	rc.status = HTTP_ABORTED;
}

//--------------------------------------------------------------------------------------------------------------
const char *StatusString[] =
{
	"HTTP_CONNECTING",
	"HTTP_FETCH",
	"HTTP_DONE",
	"HTTP_ABORTED",
	"HTTP_ERROR",
};

//--------------------------------------------------------------------------------------------------------------
const char *ErrorString[] =
{
	"HTTP_ERROR_NONE",
	"HTTP_ERROR_ZERO_LENGTH_FILE",
	"HTTP_ERROR_CONNECTION_CLOSED",
	"HTTP_ERROR_INVALID_URL",
	"HTTP_ERROR_INVALID_PROTOCOL",
	"HTTP_ERROR_CANT_BIND_SOCKET",
	"HTTP_ERROR_CANT_CONNECT",
	"HTTP_ERROR_NO_HEADERS",
	"HTTP_ERROR_FILE_NONEXISTENT",
	"HTTP_ERROR_MAX",
};

//--------------------------------------------------------------------------------------------------------------
/**
 * Closes all open handles, and waits until the main thread has given the OK
 * to quit.
 */
void CleanUpDownload( RequestContext& rc, HTTPStatus status, HTTPError error = HTTP_ERROR_NONE )
{
	if ( status != HTTP_DONE || error != HTTP_ERROR_NONE )
	{
		//Thread_DPrintf( "CleanUpDownload() - http status is %s, error state is %s\n",
		//	StatusString[status], ErrorString[error] );
	}

	rc.status = status;
	rc.error = error;

	// Close all HINTERNET handles we have open
	if ( rc.hDataResource && !InternetCloseHandle(rc.hDataResource) )
	{
		//Thread_DPrintf( "Failed to close data resource for %s%s\n", rc.baseURL, rc.gamePath );
	}
	else if ( rc.hOpenResource && !InternetCloseHandle(rc.hOpenResource) )
	{
		//Thread_DPrintf( "Failed to close open resource for %s%s\n", rc.baseURL, rc.gamePath );
	}
	rc.hDataResource = NULL;
	rc.hOpenResource = NULL;

	// wait until the main thread says we can go away (so it can look at rc.data).
	while ( !rc.shouldStop )
	{
		Sleep( 100 );
	}

	// Delete rc.data, which was allocated in this thread
	if ( rc.data != NULL )
	{
		delete[] rc.data;
		rc.data = NULL;
	}

	// and tell the main thread we're exiting, so it can delete rc.cachedData and rc itself.
	rc.threadDone = true;
}

//--------------------------------------------------------------------------------------------------------------
static void DumpHeaders( RequestContext& rc )
{
#ifdef _DEBUG
	DWORD dwSize;

	// First time we will find out the size of the headers.
	HttpQueryInfo ( rc.hDataResource, HTTP_QUERY_RAW_HEADERS_CRLF, NULL, &dwSize, NULL );
	char *lpBuffer =  new char [dwSize + 2];

	// Now we call HttpQueryInfo again to get the headers.
	if (!HttpQueryInfo ( rc.hDataResource, HTTP_QUERY_RAW_HEADERS_CRLF, (LPVOID)lpBuffer, &dwSize, NULL))
	{
		return;
	}
	*(lpBuffer + dwSize) = '\n';
	*(lpBuffer + dwSize + 1) = '\0';

	Thread_DPrintf( "------------------------------\n%s%s\n%s------------------------------\n",
		rc.baseURL, rc.gamePath, lpBuffer );
#endif
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Main download thread function - implements a (partial) synchronous HTTP download.
 */
DWORD __stdcall DownloadThread( void *voidPtr )
{
	RequestContext& rc = *(RequestContext *)voidPtr;

	URL_COMPONENTS url;
	char urlBuf[6][BufferSize];
	url.dwStructSize = sizeof(url);

	url.dwSchemeLength    = BufferSize;
	url.dwHostNameLength  = BufferSize;
	url.dwUserNameLength  = BufferSize;
	url.dwPasswordLength  = BufferSize;
	url.dwUrlPathLength   = BufferSize;
	url.dwExtraInfoLength = BufferSize;

	url.lpszScheme     = urlBuf[0];
	url.lpszHostName   = urlBuf[1];
	url.lpszUserName   = urlBuf[2];
	url.lpszPassword   = urlBuf[3];
	url.lpszUrlPath    = urlBuf[4];
	url.lpszExtraInfo  = urlBuf[5];

	char fullURL[BufferSize*2];
	DWORD fullURLLength = BufferSize*2;
	Q_snprintf( fullURL, fullURLLength, "%s%s", rc.baseURL, rc.gamePath, fullURL );
	/*
	if ( !InternetCombineUrl( rc.baseURL, rc.gamePath, fullURL, &fullURLLength, 0 ) )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_INVALID_URL );
		return rc.status;
	}
	*/

	if ( !InternetCrackUrl( fullURL, fullURLLength, 0, &url ) )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_INVALID_URL );
		return rc.status;
	}

	/// @TODO: Add FTP support (including restart of aborted transfers) -MDC 2004/01/08
	// We should be able to handle FTP downloads as well, but I don't have a server to check against, so
	// I'm gonna disallow it in case something bad would happen.
	if ( url.nScheme != INTERNET_SCHEME_HTTP && url.nScheme != INTERNET_SCHEME_HTTPS )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_INVALID_PROTOCOL );
		return rc.status;
	}

	// Open a socket etc for the download.
	// The first parameter, "Half-Life", is the User-Agent that gets sent with HTTP requests.
	// INTERNET_OPEN_TYPE_PRECONFIG specifies using IE's proxy info from the registry for HTTP downloads.
	rc.hOpenResource = InternetOpen( "Half-Life 2", INTERNET_OPEN_TYPE_PRECONFIG ,NULL, NULL, 0);

	if ( !rc.hOpenResource )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_CANT_BIND_SOCKET );
		return rc.status;
	}

	InternetSetStatusCallback( rc.hOpenResource, (INTERNET_STATUS_CALLBACK)DownloadStatusCallback );

	if ( rc.shouldStop )
	{
		CleanUpDownload( rc, HTTP_ABORTED );
		return rc.status;
	}

	// Set up some flags
	DWORD flags = 0;
	flags |= INTERNET_FLAG_RELOAD;			// Get from server, not IE's cache
	flags |= INTERNET_FLAG_NO_CACHE_WRITE;	// Don't write to IE's cache, since we're doing our own caching of partial downloads
	flags |= INTERNET_FLAG_KEEP_CONNECTION;	// Use keep-alive semantics.  Since each file downloaded is a separate connection
											//   from a separate thread, I don't think this does much.  Can't hurt, though.
	if ( url.nScheme == INTERNET_SCHEME_HTTPS )
	{
		// The following flags allow us to use https:// URLs, but don't provide much in the way of authentication.
		// In other words, this allows people with only access to https:// servers (?!?) to host files as transparently
		// as possible.
		flags |= INTERNET_FLAG_SECURE;						// Use SSL, etc.  Kinda need this for HTTPS URLs.
		flags |= INTERNET_FLAG_IGNORE_CERT_CN_INVALID;		// Don't check hostname on the SSL cert.
		flags |= INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;	// Don't check for expired SSL certs.
	}

	// Request a partial if we have the data
	char headers[BufferSize] = "";
	DWORD headerLen = 0;
	char *headerPtr = NULL;
	if ( *rc.cachedTimestamp && rc.nBytesCached )
	{
		if ( *rc.serverURL )
		{
			Q_snprintf( headers, BufferSize, "If-Range: %s\nRange: bytes=%d-\nReferer: hl2://%s\n",
				rc.cachedTimestamp, rc.nBytesCached, rc.serverURL );
		}
		else
		{
			Q_snprintf( headers, BufferSize, "If-Range: %s\nRange: bytes=%d-\n",
				rc.cachedTimestamp, rc.nBytesCached );
		}
		headerPtr = headers;
		headerLen = (DWORD)-1L; // the DWORD cast is because we get a signed/unsigned mismatch even with an L on the -1.
		//Thread_DPrintf( "Requesting partial download\n%s", headers );
	}
	else if ( *rc.serverURL )
	{
		Q_snprintf( headers, BufferSize, "Referer: hl2://%s\n", rc.serverURL );
		headerPtr = headers;
		headerLen = (DWORD)-1L; // the DWORD cast is because we get a signed/unsigned mismatch even with an L on the -1.
		//Thread_DPrintf( "Requesting full download\n%s", headers );
	}

	rc.hDataResource = InternetOpenUrl(rc.hOpenResource, fullURL, headerPtr, headerLen, flags,(DWORD)(&rc) );

	// send the request off
	if ( !rc.hDataResource )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_CANT_CONNECT );
		return rc.status;
	}

	if ( rc.shouldStop )
	{
		CleanUpDownload( rc, HTTP_ABORTED );
		return rc.status;
	}

	//DumpHeaders( rc ); // debug

	// check the status (are we gonna get anything?)
	DWORD size = sizeof(DWORD);
	DWORD code;
	if ( !HttpQueryInfo( rc.hDataResource, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &size, NULL ) )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_NO_HEADERS );
		return rc.status;
	}

	// Only status codes we're looking for are HTTP_STATUS_OK (200) and HTTP_STATUS_PARTIAL_CONTENT (206)
	if ( code != HTTP_STATUS_OK && code != HTTP_STATUS_PARTIAL_CONTENT )
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_FILE_NONEXISTENT );
		return rc.status;
	}

	// get the timestamp, and save it off for future resumes, in case we abort this transfer later.
	size = BufferSize;
	if ( !HttpQueryInfo( rc.hDataResource, HTTP_QUERY_LAST_MODIFIED, rc.cachedTimestamp, &size, NULL ) )
	{
		rc.cachedTimestamp[0] = 0;
	}
	rc.cachedTimestamp[BufferSize-1] = 0;

	// If we're not getting a partial download, don't use any cached data, even if we have some.
	if ( code != HTTP_STATUS_PARTIAL_CONTENT )
	{
		if ( rc.nBytesCached )
		{
			//Thread_DPrintf( "Partial download refused - getting full version\n" );
		}
		rc.nBytesCached = 0; // start from the beginning
	}

	// Check the resource size, and allocate a buffer
	size = sizeof(code);
	if ( HttpQueryInfo( rc.hDataResource, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &code, &size, NULL ) )
	{
		rc.nBytesTotal = code + rc.nBytesCached;
		if ( code > 0 )
		{
			rc.data = new unsigned char[rc.nBytesTotal];
		}
	}
	else
	{
		CleanUpDownload( rc, HTTP_ERROR, HTTP_ERROR_ZERO_LENGTH_FILE );
		return rc.status;
	}

	// copy cached data into buffer
	if ( rc.cacheData && rc.nBytesCached )
	{
		int len = min( rc.nBytesCached, rc.nBytesTotal );
		memcpy( rc.data, rc.cacheData, len );
	}

	if ( rc.shouldStop )
	{
		CleanUpDownload( rc, HTTP_ABORTED );
		return rc.status;
	}

	// now download the actual data
	ReadData( rc );

	// and stick around until the main thread has gotten the data.
	CleanUpDownload( rc, rc.status, rc.error );
	return rc.status;
}


#endif // _WIN32
