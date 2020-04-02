//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#ifndef CL_DEMO_H
#define CL_DEMO_H
#ifdef _WIN32
#pragma once
#endif

#include "demofile.h"
#include "cl_demoactionmanager.h"

struct DemoCommandQueue
{
	DemoCommandQueue()
	{
		tick = 0;
	}
	int				tick;
	democmdinfo_t	info;
	int				filepos;
};

class CDemoPlayer : public IDemoPlayer
{

public: // IDemoPlayer interface implementation:
	CDemoPlayer();
	~CDemoPlayer();

	CDemoFile *GetDemoFile();

	bool	StartPlayback( const char *filename, bool bAsTimeDemo );
	void	PausePlayback( float seconds );
	void	SkipToTick( int tick, bool bRelative, bool bPause );
	void	ResumePlayback( void );
	void	StopPlayback( void );

	int		GetPlaybackTick( void );
	float	GetPlaybackTimeScale( void );
	int		GetTotalTicks( void );

	bool	IsPlayingBack( void );
	bool	IsPlaybackPaused( void );
	bool	IsPlayingTimeDemo( void );
	bool	IsSkipping( void );
	bool	CanSkipBackwards( void ) { return false; }
	
	void	SetPlaybackTimeScale( float timescale );
	void	InterpolateViewpoint(); // override viewpoint
	netpacket_t *ReadPacket( void );
	void	ResetDemoInterpolation( void );


public:	// other public functions
	void	MarkFrame( float flFPSVariability );
	void	SetBenchframe( int tick, const char *filename );
	void	ResyncDemoClock( void );
	bool	CheckPausedPlayback( void );
	void	WriteTimeDemoResults( void );
	bool	ParseAheadForInterval( int curtick, int intervalticks );
	void	InterpolateDemoCommand( int targettick, DemoCommandQueue& prev, DemoCommandQueue& next );

protected:
	bool	OverrideView( democmdinfo_t& info );

public:
	
	CDemoFile		m_DemoFile;
	int				m_nStartTick;	// For synchronizing playback during timedemo.
	int				m_nPreviousTick;
	netpacket_t		m_DemoPacket;	// last read demo packet
	bool			m_bPlayingBack; // true if demo playback
	bool			m_bPlaybackPaused; // true if demo is paused right now
	float			m_flAutoResumeTime; // how long do we pause demo playback
	float			m_flPlaybackRateModifier;
	int				m_nSkipToTick;	// skip to tick ASAP, -1 = off
	

	// view origin/angle interpolation:
	CUtlVector< DemoCommandQueue >	m_DestCmdInfo;
	democmdinfo_t					m_LastCmdInfo;
	bool							m_bInterpolateView;
	bool							m_bResetInterpolation;


	// timedemo stuff:
	bool			m_bTimeDemo;	// ture if in timedemo mode
	int				m_nTimeDemoStartFrame;	// host_tickcount at start
	double			m_flTimeDemoStartTime;	// Sys_FloatTime() at second frame of timedemo
	float			m_flTotalFPSVariability; // Frame rate variability
	int				m_nTimeDemoCurrentFrame; // last frame we read a packet
	
	// benchframe stuff
	int				m_nSnapshotTick;
	char			m_SnapshotFilename[MAX_OSPATH];
};

class CDemoRecorder : public IDemoRecorder 
{
public:
	~CDemoRecorder();
	CDemoRecorder();

	CDemoFile *GetDemoFile( void );
	int		GetRecordingTick( void );

	void	StartRecording( const char *filename, bool bContinuously );
	void	SetSignonState( int state );
	bool	IsRecording( void );
	void	PauseRecording( void );
	void	ResumeRecording( void );
	void	StopRecording( void );
	
	void	RecordCommand( const char *cmdstring );  // record a console command
	void	RecordUserInput( int cmdnumber );  // record a user input command
	void	RecordMessages( bf_read &data, int bits ); // add messages to current packet
	void	RecordPacket( void ); // packet finished, write all recorded stuff to file
	void	RecordServerClasses( ServerClass *pClasses ); // packet finished, write all recorded stuff to file

	void	ResetDemoInterpolation( void );

protected:

	void	ResyncDemoClock( void );
	void	StartupDemoFile( void );
	void	StartupDemoHeader( void );
	void	CloseDemoFile( void );
	void	GetClientCmdInfo( democmdinfo_t& info );
	void	WriteDemoCvars( void );
	void	WriteBSPDecals( void );
	void	WriteMessages( bf_write &message );
	bool	ComputeNextIncrementalDemoFilename( char *name, int namesize );

public:
	CDemoFile		m_DemoFile;

	// For synchronizing playback during timedemo.
	int				m_nStartTick; // host_tickcount when starting recoring

	// Name of demo file we are appending onto.
	char			m_szDemoBaseName[ MAX_OSPATH ];  

	// For demo file handle
	bool			m_bIsDemoHeader;	// true, if m_hDemoFile is the header file
	bool			m_bCloseDemoFile;	// if true, demo file will be closed ASAP

	bool			m_bRecording;	  // true if recording
	bool			m_bContinuously; // start new record after each
	int				m_nDemoNumber;	// demo count, increases each changelevel
	int				m_nFrameCount;	// # of demo frames in this segment.

	bf_write		m_MessageData; // temp buffer for all network messages

	bool			m_bResetInterpolation;
};

extern CDemoPlayer *g_pClientDemoPlayer;
extern CDemoRecorder *g_pClientDemoRecorder;

#endif // CL_DEMO_H
