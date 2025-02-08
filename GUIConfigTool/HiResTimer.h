// Pinscape Pico - Config Tool - High-Resolution Timer
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class is a convenience wrapper for the Windows QueryPerformanceTimer()
// high-resolution clock API.  The QPC API provides access to a hardware clock
// running at a high frequency, typically on the microsecond scale.

#pragma once
#include <stdint.h>
#include <Windows.h>
#include <timeapi.h>

class HiResTimer
{
public:
	HiResTimer();
	~HiResTimer() { }

	// Get the current time in QPC ticks.  This gets the time at
	// the finest precision scale available on the hardware.
	inline int64_t GetTime_ticks()
	{
		if (qpcAvailable)
		{
			// use the high-res performance counter
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}
		else
		{
			// no QPC, so use the low-res system timer instead
			return timeGetTime();
		}
	}

	// Get the current time in seconds
	inline double GetTime_seconds() { return GetTime_ticks() * tickTime_sec; }

	// Get the current time in microseconds
	inline double GetTime_us() { return GetTime_ticks() * tickTime_us; }

	// get the tick time in seconds/microseconds
	inline double GetTickTime_sec() const { return tickTime_sec; }
	inline double GetTickTime_us() const { return tickTime_us; }

	// convert ticks to microseconds
	inline double TicksToUs(int64_t ticks) const { return ticks * tickTime_us; }

	// get the native tick frequency
	inline uint64_t GetFreq() const { return freq.QuadPart; }

protected:
	// Performance counter clock period in seconds.  Multiply an
	// interval read from the performance counter by this factor to
	// convert from ticks to seconds.
	double tickTime_sec;

	// Performance counter clock period in microseconds.  Multiply
	// an interval by this factor to convert to microseconds.
	double tickTime_us;

	// is the QPC timer available?
	bool qpcAvailable;

	// native clock frequency
	LARGE_INTEGER freq;
};
