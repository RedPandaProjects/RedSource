//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#ifndef MIXER_CONTROLS_H
#define MIXER_CONTROLS_H
#pragma once


abstract_class IMixerControls
{
protected:
	virtual			~IMixerControls() {}

public:
	enum Control
	{
		// Microphone boost is a boolean switch that sound cards support which boosts the input signal by about +20dB.
		// If this isn't on, the mic is usually way too quiet.
		MicBoost=0,

		// Volume values are 0-1.
		MicVolume,
		
		// Mic playback muting. You usually want this set to false, otherwise the sound card echoes whatever you say into the mic.
		MicMute,
		
		NumControls
	};

	virtual void	Release() = 0;
	
	virtual bool	GetValue_Float(Control iControl, float &value) = 0;
	virtual bool	SetValue_Float(Control iControl, float value) = 0;
	
	// Apps like RealJukebox will switch the waveIn input to use CD audio 
	// rather than the microphone. This should be called at startup to set it back.
	virtual bool	SelectMicrophoneForWaveInput() = 0;
};

// Allocates a set of mixer controls.
IMixerControls* GetMixerControls();


#endif // MIXER_CONTROLS_H
