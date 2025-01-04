// Pinscape Pico - Config Tool - High-Resolution Windows Timer
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include "HiResTimer.h"

#pragma comment(lib, "winmm.lib")

HiResTimer::HiResTimer()
{
	// Get the performance counter frequency.  If successful, set up
	// to read times using the performance counter.  Otherwise read via
	// the low-res timer.
	if (QueryPerformanceFrequency(&freq) && freq.QuadPart != 0)
	{
		// QueryPerformanceCounter is available - use it to calculate
		// times.  Calculate the time in microseconds per QPC tick and
		// store this for use when calculating time intervals.
		qpcAvailable = true;
		tickTime_sec = 1.0 / freq.QuadPart;
		tickTime_us = 1.0e6 / freq.QuadPart;
	}
	else
	{
		// QPC isn't available on this system, so fall back on the
		// low-res timer.  That reads in milliseconds (although it
		// doesn't necessarily have millisecond precision), so the
		// "tick" time is 1ms in this case.
		qpcAvailable = false;
		tickTime_sec = 1.0e-3;
		tickTime_us = 1000.0;
		freq.QuadPart = 1000ULL;
	}
}
