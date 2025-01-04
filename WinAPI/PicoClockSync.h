// Pinscape Pico - Pico Clock Synchronizer
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// The PicoClockSync class helps a Windows application synchronize Windows
// timestamps with Pinscape Pico timestamps, so that a time reading taken 
// on the Pico in terms of the Pico's time-since-reset clock can be converted
// to the corresponding point in time on the Windows system clock at high
// precision.  This is particularly useful for measuring the time between
// events across the two systems, where one event was observed on the Pico,
// and the other event was observed on the PC.  Measuring the time between
// two events requires a common reference point for the timestamps of the
// two events, which requires knowing the relationship between the clocks
// on the two systems.
// 
// In principle, this kind of synchronization could be accomplished if we
// could calibrate both systems to a common reference clock, such as an
// NTP server's clock.  If the Windows and Pico clocks were both 
// sufficiently precise, and both were calibrated to an external NTP clock,
// then the timestamps from the two system could be used directly in
// elapsed-time calculations, since they'd both be referenced to a common
// "zero" point.  The problem with that approach is that the Pico doesn't
// get its time from an NTP server, so even though Windows does have a
// good outside time reference for its date/time clock, the Pico doesn't.
// The closest thing the Pico has is a Pinscape API that lets the Windows
// host send its current wall-clock time to the Pico, but that API isn't
// designed for precision, and only aligns the clocks to about the nearest
// second.  That's not good enough if you need high precision, in the
// millisecond or microsecond range.
// 
// This class uses a Pinscape Pico API designed for high-precision clock
// synchronization.  The API lets the Pico send its internal microsecond
// timer clock reading to the Windows host, allowing the Windows host to
// determine the correspondence between Windows time and Pico time to a
// precision on the scale of the USB request round-trip time.  This class
// provides a higher-level wrapper for that API that performs additional
// filtering and averaging to improve the precision and accuracy of the
// clock synchronization.
// 
// Note that we talk about clock "synchronization", we DON'T mean that
// we're changing the Windows clock time or the Pico clock time settings.
// Running methods in this class won't mess with your system date/time
// settings.  The synchronization is purely at the API level: the class
// determines the calibration factors needed to convert between the two
// system clocks, without affecting the operation of the clocks.  Once
// the calibration factors have been calculated, the class can calculate
// the Pico timestamp corresponding to a given Windows timestamp.
// 
// To use the class, create an instance, and call Sync().  That will
// read the Pico clock repeatedly, filtering and averaging the readings
// to obtain better accuracy, and then compute the conversion factors
// needed to convert between Pico and Windows timestamps.  After that,
// you can call ProjectPicoTime() to convert a Windows timestamp to the
// corresponding Pico timestamp value.
// 
// The Sync() call is relatively time-consuming, because of its need to
// send repeated USB commands to the Pico, whereas the ProjectPicoTime()
// call is very fast, since it just does some simple arithmetic using
// the conversion factors computed in Sync().  However, the longer it's
// been since the last Sync(), the less accurate the projected results
// tend to be, because the Pico and Windows clocks tend to have a slight
// amount of "skew" - that is, one clock will tend to run slightly
// faster than the other, so the two clocks will diverge over time.
// Experimentally, the skew seems to be about one part per million,
// which is enough the clocks will diverge by tens to hundreds of
// microseconds over a few minutes of real time.  One way to correct
// for this is to call Sync() every couple of minutes.  Another way is
// to call AdjustSkew() periodically.  The latter is a better approach
// for long-running applications, because you can call AdjustSkew() at
// exponentially increasing intervals: for example, after one minute,
// then two minutes, then four minutes, then eight minutes, etc.  The
// skew adjustment is more accurate after longer elapsed times since
// Sync(), since it works by comparing the ratio of projected to
// measured readings, which become more accurate as the time base
// grows.  A skew adjustment based on results after just a few minutes
// elapsed since Sync() will give good results for a couple of hours,
// and a skew adjustment after a couple of hours might be good for
// the whole day.
//

#pragma once
#include <stdint.h>
#include <memory>
#include <Windows.h>
#include "PinscapeVendorInterface.h"

namespace PinscapePico
{
	class PicoClockSync
	{
	public:
		// Initialize.  If the skew is known for the device from a
		// past calibration run, the initial skew can be provided;
		// if not known, simply use 0.0.  Skew is best calibrated
		// over relatively long periods, of at least a few minutes
		// and preferably hours, so some applications might find it
		// useful to save the observed skew across sessions, or to
		// perform a calibration run in advance of normal operation
		// to get a good skew reading.  The skew is likely to vary
		// by Pico instance, and probably by environmental conditions
		// (particularly temperature), so there's not a standard
		// universal value to supply here; it has to be measured
		// experimentally per board, and preferably under the same
		// conditions as during normal application operation.  The
		// skew can be updated based on live readings at any time
		// with AdjustSkew(), if a pause to gather data can be
		// tolerated.
		PicoClockSync(std::shared_ptr<PinscapePico::VendorInterface> &dev, double skew);

		// Synchronize with the Pico time.  This reads the current
		// Pico time and saves it along with the current Windows time,
		// to serve as a reference point for projecting Pico times
		// without having to send more USB requests.  Returns true on
		// success, false if the USB timer requests to the Pico failed.
		//
		// nAverageRounds is the number of rounds to use for averaging.
		// 
		// nFilterRounds is the number of inputs to read for each averaging
		// round to select the best samples to use in the averages.
		//
		bool Sync(int nAverageRounds, int nFilterRounds);

		// Project the current Pico time, based on the time snapshot
		// taken at the last Sync().  This doesn't perform an USB
		// requests, so it's very fast, but it gets less accurate with
		// longer elapsed times since the last Sync().
		uint64_t ProjectPicoTime();

		// Project the Pico time for a given Windows time
		uint64_t ProjectPicoTime(int64_t winTime);

		// Read the current time from the Pico.  This queries the Pico
		// clock repeatedly and returns the combined result.  
		// 
		// The return value represents the current time on the Pico
		// clock as of the moment the function returns.
		uint64_t ReadPicoTime(int nAverageRounds, int nFilterRounds);

		// Lower-level time reader.  This passes back the reference
		// times obtained from the USB request, rather than adjusting
		// to the current time.  The Pico time is the Pico clock reading
		// at the moment of the Windows reference time passed back.
		// Returns true on success, false if no time information could
		// be obtained due to failed requests.  The Windows time is in
		// terms of the HiResTimer's "tick" counts.
		bool ReadPicoTime(int nAverageRounds, int nFilterRounds, uint64_t &picoTime, int64_t &windowsTime);

		// Adjust the clock skew.  This takes a new reading from the USB
		// pipe, and compares it to the projected time based on the last
		// synchronization point.  It then adjusts the internal skew
		// correction factor according to the difference between the
		// projected and actual time.  Running this at various intervals
		// should make the projected time more accurate over longer
		// periods by correcting for differences in the clock rates on
		// the two devices.
		bool AdjustSkew(int nAverageRounds, int nFilterRounds);

		// get the current skew reading
		double GetSkew() const { return skew; }

		// last synchronization time in Windows HiResTimer ticks
		int64_t GetLastWinSyncTime() const { return winSyncTime; }

		// get the current Windows time in terms of QueryPerformanceCounter ticks
		int64_t GetWindowsTime() const;

		// convert a Windows QueryPerformanceCounter ticks time to/from microseconds
		double TicksToMicroseconds(int64_t ticks) const { return ticks * ticksToMicroseconds; }
		int64_t MicrosecondsToTicks(double us) const { return static_cast<int64_t>(us / ticksToMicroseconds); }

		// Get the time intervals for Windows and Pico readings used to set the
		// reference time.  The difference between the time Windows determines
		// the uncertainty in the synchronization point, because we only know
		// that the Pico time interval is contained within the larger Windows
		// time interval, but we can't know how it aligns within the interval.
		// So we can only estimate the synchronization point as +/- half of
		// the difference of the intervals.
		int GetWindowsInterval() const { return winInterval; }
		int GetPicoInterval() const { return picoInterval; }
		int GetUncertainty() const { return winInterval - picoInterval; }

	protected:
		// device handle
		std::shared_ptr<PinscapePico::VendorInterface> dev;

		// QueryPerformanceCounter tick time-to-microseconds conversion factor
		double ticksToMicroseconds = 0;

		// synchronization reference time
		int64_t winSyncTime = 0;
		uint64_t picoSyncTime = 0;

		// average time intervals for Windows and Pico readings
		int winInterval = 0;
		int picoInterval = 0;

		// Clock skew adjustment.  This corrects for clock rate
		// differences between the two system clocks, based on comparisons
		// across different readings spaced apart in time.
		double skew = 0.0;
	};
}
