// Pinscape Pico - Pico Clock Synchronization
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//

#include "PicoClockSync.h"

// this class is within the PinscapePico namespace
using namespace PinscapePico;

PicoClockSync::PicoClockSync(std::shared_ptr<PinscapePico::VendorInterface> &dev, double initialSkew) : 
	dev(dev), skew(initialSkew)
{
	// figure the QueryPerformanceCounter ticks-to-microseconds conversion factor
	LARGE_INTEGER ii;
	if (QueryPerformanceFrequency(&ii))
		ticksToMicroseconds = 1.0e6 / static_cast<double>(ii.QuadPart);
}

int64_t PicoClockSync::GetWindowsTime() const
{
	LARGE_INTEGER ii;
	return QueryPerformanceCounter(&ii) ? ii.QuadPart : 0;
}

bool PicoClockSync::Sync(int nAverageRounds, int nFilterRounds)
{
	return ReadPicoTime(nAverageRounds, nFilterRounds, picoSyncTime, winSyncTime);
}

bool PicoClockSync::AdjustSkew(int nAverageRounds, int nFilterRounds)
{
	// take a new USB reading
	uint64_t picoTime;
	int64_t windowsTime;
	if (!ReadPicoTime(nAverageRounds, nFilterRounds, picoTime, windowsTime))
		return false;

	// figure the projected time at the new reference time
	uint64_t picoTimeProjected = picoSyncTime + static_cast<int64_t>((windowsTime - winSyncTime) * ticksToMicroseconds);

	// figure the skew factor that makes the projected time equal the
	// actual new reading
	skew = static_cast<double>(picoTime - picoTimeProjected) / static_cast<double>(picoTimeProjected - picoSyncTime);

	// success
	return true;
}

uint64_t PicoClockSync::ReadPicoTime(int nAverageRounds, int nFilterRounds)
{
	// invoke the reference-point time reader
	uint64_t picoTime;
	int64_t windowsTime;
	if (ReadPicoTime(nAverageRounds, nFilterRounds, picoTime, windowsTime))
	{
		// Return the final time, as the Pico reference time plus the
		// amount of elapsed Windows time since the corresponding Windows
		// reference time.
		return picoTime + static_cast<int64_t>((GetWindowsTime() - windowsTime) * ticksToMicroseconds);
	}
	else
	{
		// error
		return 0;
	}
}

uint64_t PicoClockSync::ProjectPicoTime()
{
	double dw = (GetWindowsTime() - winSyncTime) * ticksToMicroseconds;
	return picoSyncTime + static_cast<int64_t>(dw + dw*skew);
}

uint64_t PicoClockSync::ProjectPicoTime(int64_t winTime)
{
	double dw = (winTime - winSyncTime) * ticksToMicroseconds;
	return picoSyncTime + static_cast<int64_t>(dw + dw*skew);
}

bool PicoClockSync::ReadPicoTime(int nAverageRounds, int nFilterRounds, uint64_t &picoTime, int64_t &windowsTime)
{
	// run the averaging rounds
	int64_t picoOfsSum = 0;
	int64_t winReferenceTime = 0;
	uint64_t picoReferenceTime = 0;
	int64_t picoIntervalSum = 0;
	int64_t winIntervalSum = 0;
	for (int i = 0 ; i < nAverageRounds ; ++i)
	{
		// Run the filtering rounds, to select the sample with the shortest
		// overall time window.  A shorter time window sets a tighter constraint
		// on the uncertainty in the time offset, so the result with the shortest
		// window gives us the most precise reading.
		int64_t dwMin = INT64_MAX;
		int64_t dpMin = INT64_MAX;
		int64_t picoOfs = 0;
		for (int j = 0 ; j < nFilterRounds ; ++j)
		{
			// run a query, noting the Windows time before and after
			int64_t w1, w2;
			uint64_t p1, p2;
			int stat = dev->QueryPicoSystemClock(w1, w2, p1, p2);

			// skip this round in case of error
			if (stat != PinscapeResponse::OK)
				continue;

			// Figure the Windows time delta, and keep this reading if it's the 
			// shortest round-trip so far.
			int64_t dw = w2 - w1;
			if (dw < dwMin)
			{
				// refigure the Windows elapsed time in microseconds
				int64_t dw_us = static_cast<int64_t>(dw * ticksToMicroseconds);

				// figure the pico time difference
				int64_t dp = p2 - p1;

				// Sanity check that the Windows time interval is larger than the
				// Pico time interval.  This must be true for a valid reading
				// because the Windows time reflects the time it took to send the
				// request, process the request on the Pico (p1..p2), and read
				// the reply.  The send and receive parts must be non-zero, so
				// the overall time in the w1..w2 interval must be longer.
				if (dw_us < dp)
					continue;

				// update the best time window so far
				dwMin = dw;
				dpMin = dp;

				// The Pico time interval [p1..p2] occurred during the Windows time 
				// interval [w1..w2], so if we were to express all of the values in 
				// terms of microseconds after a common epoch, w1 <= p1 <= p2 <= w2.
				// However, we can't know where the [p1..p2] interval falls within
				// the larger [w1..w2] interval - it could be aligned at either end
				// or be anywhere in between.  Assume that the USB transit and
				// processing time are evenly split before and after the Pico
				// interval, so w2 represents the same moment in time as p2 plus
				// half of the excess time.
				winReferenceTime = w2;
				picoOfs = (dw_us - dp)/3;
				picoReferenceTime = p2;
			}
		}

		// collect averages
		picoOfsSum += picoOfs;
		picoIntervalSum += dpMin;
		winIntervalSum += dwMin;
	}

	// check the results
	if (picoReferenceTime != 0)
	{
		// Success.  Use the timestamps from the last round, adjusted by the
		// average of the offsets over the averaging rounds.
		picoTime = picoReferenceTime + (picoOfsSum / nAverageRounds);
		windowsTime = winReferenceTime;
		picoInterval = static_cast<int>(picoIntervalSum / nAverageRounds);
		winInterval = static_cast<int>(winIntervalSum / nAverageRounds * ticksToMicroseconds);
		return true;
	}
	else
	{
		// no samples read - return failure
		return 0;
	}
}

