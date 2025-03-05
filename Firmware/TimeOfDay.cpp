// Pinscape Pico - Time Of Day/Wall Clock Time
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdint.h>
#include <stdarg.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/clocks.h>

// local project headers
#include "JSON.h"
#include "TimeOfDay.h"
#include "crc32.h"
#include "Logger.h"

// global singleton
TimeOfDay timeOfDay;

// construction
TimeOfDay::TimeOfDay()
{
}

// get the current wall-clock time
bool TimeOfDay::Get(DateTime &dt)
{
    // Figure the elapsed time since the reference wall-clock time was set
    uint64_t delta = time_us_64() - sys_time_us;

    // Project the reference time's time-of-day into the future by the
    // elapsed time
    uint32_t curDaySeconds = daySeconds + static_cast<uint32_t>(delta/1000000);

    // carry any excess past midnight (86,400 seconds) into the day number
    int curJdn = jdn + static_cast<int>(curDaySeconds / 86400);
    curDaySeconds %= 86400;
    
    // fill in the return struct - start with the linear time representation
    dt.jdn = curJdn;
    dt.timeOfDay = curDaySeconds;

    // calculate the calendar date from the JDN
    DateTime::JDNToDate(dt.yyyy, dt.mon, dt.dd, curJdn);

    // calculate the wall clock time from the seconds-past-midnight number
    dt.hh = static_cast<uint8_t>(curDaySeconds / 3600);
    dt.mm = static_cast<uint8_t>((curDaySeconds % 3600) / 60);
    dt.ss = static_cast<uint8_t>(curDaySeconds % 60);

    // success
    return true;
}

// set the time
void TimeOfDay::SetTime(const DateTime &dt, bool updateRTC)
{
    // Record the reference system time on the Pico system clock.  The
    // date/time passed by the user represents the current time, at
    // least as of when it was sent from the host PC or retrieved from
    // an outboard calendar chip.  So we now know that this moment in
    // wall-clock time corresponds to the current reading on the Pico's
    // system clock.
    //
    // Note that the wall-clock time provided by the caller will be
    // slightly in our past, because that reading had to propagate
    // across the USB wire if it came from the host, or the I2C bus if
    // it was from a calendar chip.  If we knew how long the path length
    // was, we could apply a correction here.  But the discrepancy
    // should be so small that it's not worth worrying about.  Our
    // DateTime representation only has 1-second resolution, and the
    // propagation time for the value to reach us is probably on the
    // order of milliseconds at most.  It'll disappear in the rounding
    // error, and even if it didn't, I don't foresee using the
    // wall-clock time in a Pinscape context for anything requiring high
    // precision.
    sys_time_us = time_us_64();
    
    // Store the provided wall-clock time, converting the human-friendly
    // format stored there to our internal JDN + seconds-since-midnight
    // format.
    jdn = DateTime::DateToJDN(dt.yyyy, dt.mon, dt.dd);
    daySeconds = dt.hh*3600 + dt.mm*60 + dt.ss;
    
    // we now have a valid wall-clock reference time
    isSet = true;

    // if desired, update RTC chips
    if (updateRTC)
    {
        for (auto *rtc : rtcs)
            rtc->Write(dt);
    }
}

// Global reset data.  This is stored explicitly in an __unitialized_ram()
// linker section, which the CRTL startup code leaves untouched during
// program startup.  That preserves the contents of this area across a
// Pico hardware reset, as long as the power isn't interrupted.
//
// The absence of CRTL initialization on this RAM also means that we'll
// see random data at these locations on the first run after a power-on
// reset.  So we need a way to distinguish random startup data from
// valid data preserved across a software reset.  To accomplish this, we
// store a CRC-32 signature on the struct.  When we initialize the
// struct prior to a reset, we compute the CRC-32 and store it alongside
// the other struct data.  At startup, we re-compute the CRC-32, and see
// if it matches the struct data.  If so, we assume that the struct
// contains valid data preserved across a reset.  It's still possible in
// a million-monkeys-writing-Shakespeare way that the random garbage in
// RAM after a power-on reset just happens to have the correct CRC-32
// value, but the odds of this are extremely small.  We can also further
// reduce the odds by rejecting dates and times that are out of bounds.
struct CrossResetData
{
    // Wall clock date/time computed just before the reset.  Note that
    // we use intentionally unitialized values here, since we don't want
    // the compiler to generate code to set these at startup.  Contrary
    // to all normal coding practice, we actually WANT these to remain
    // uninitialized - that's really the whole point here!
    struct
    {
        int16_t yyyy;
        uint8_t mon;
        uint8_t dd;
        uint8_t hh;
        uint8_t mm;
        uint8_t ss;
    } dateTime;

    // CRC-32 of the DateTime struct, to validate that it was properly
    // populated before the reset.  This lets us distinguish software-
    // initiated resets from power-on resets.  After a power-on reset,
    // uninitialized RAM contains random data, so it's highly improbably
    // that the stored CRC-32 would match the computed CRC-32 of the
    // struct.  (It's not impossible, since the random data could just
    // accidentally happen to contain the right CRC-32 value, but it's
    // so unlikely that we can treat it as impossible for practical
    // purposes.)
    uint32_t crc32;
};
static CrossResetData __uninitialized_ram(crossResetData);

// save the current time of day in preparation for a reset
void TimeOfDay::SaveBeforeReset()
{
    // save the current computed date/time in the specially reserved
    // cross-reset storage area
    DateTime dt;
    Get(dt);

    // save the data in our cross-boot struct
    crossResetData.dateTime = { dt.yyyy, dt.mon, dt.dd, dt.hh, dt.mm, dt.ss };

    // compute and store the CRC-32 value, for post-boot integrity checking
    crossResetData.crc32 = CRC::Calculate(&crossResetData.dateTime, sizeof(crossResetData.dateTime), CRC::CRC_32());
}

// restore the time of day from just before the reset, if available
void TimeOfDay::RestoreAfterReset()
{
    // Note that the Pico has an on-board RTC device, which seems like
    // the obvious way to keep track of time across a reset.  Sadly,
    // it's not usable for this purpose, because the RTC resets with the
    // Pico, which clears its time counters.  The Pico's watchdog and
    // reset mechanisms are configurable enough that it's maddeningly
    // close to being possible to keep the RTC going across a software
    // reset, but I've concluded that it's only *close* to being
    // possible.  The showstopper is that you can't bypass the RTC reset
    // without bypassing the Resets module reset, which is forced by the
    // PSM (power-on state machine) reset sequence if the Clock
    // Generators module is reset; and I think we always need the Clock
    // Generators module to participate in the reset to get a clean
    // start.  Refer to the RP2040 data sheet, 2.13.2 Power On Sequence.
    // And even if we could skip enough PSM steps to keep the RTC
    // running across our own software resets, it wouldn't help with the
    // scenario where we *really* need some RTC support, which is
    // resetting into the Boot Loader.  In that case, the Boot Loader is
    // going to initiate the next reset, and it's not going to go to any
    // pains to avoid the RTC reset.
    
    // calculate the CRC-32 of the stored DateTime information
    auto &crdt = crossResetData.dateTime;
    uint32_t crc32 = CRC::Calculate(&crdt, sizeof(crdt), CRC::CRC_32());

    // Check our in-RAM time struct to see if we set that just before
    // the rest.  Validate the stored CRC-32, to validate that a time
    // value was actually stored prior to the reset.  This distinguishes
    // orderly resets from power-on resets (when the uninitialized RAM
    // contents are expected to be randomized, hence the CRC-32 is
    // effectively certain not to match) or unexpected watchdog resets,
    // where we didn't get a chance to save the information.  To further
    // reduce the odds of a false positive, reject the struct if the
    // month, day, hour, minute, or second values are out of range for
    // well-formed date/time values.  (We could also require the year to
    // be in some plausible range as well, say 2024 to 2100, but it
    // seems better to leave that flexible, in case anyone has a use
    // case where they want to set their system to a past or future
    // date.)
    if (crc32 == crossResetData.crc32
        && crdt.mon >= 1 && crdt.mon <= 12
        && crdt.dd >= 1 && crdt.dd <= 31
        && crdt.hh >= 0 && crdt.hh <= 23
        && crdt.mm >= 0 && crdt.mm <= 59
        && crdt.ss >= 0 && crdt.ss <= 59)
    {
        // It looks valid - set the time to the saved time.  Note that
        // this isn't precisely accurate, since some non-zero amount of
        // time will have elapsed across the reset.  But that should be
        // relatively small, usually much less than one second of real
        // time.  The main things we use this information for are
        // logging and time-dependent DOF effects, both of which can
        // tolerate imprecise time references.  What's more, the host
        // should send us a proper time update at some point, so we
        // shouldn't have to live with the slightly stale time reference
        // for all that long.
        DateTime dt(crdt.yyyy, crdt.mon, crdt.dd, crdt.hh, crdt.mm, crdt.ss);
        SetTime(dt, false);

        // Invalidate the old record, so that we don't try to reuse it
        // on the next boot.  That's what would happen if we left the
        // old value in place and then encountered an unexpected
        // watchdog reset, which doesn't have a chance to update this
        // with the actual current time the way we do just before an
        // orderly reset.  To invalidate the record, we just have to
        // change the stored CRC to something different from its current
        // value, because it's current value matches the current struct
        // data, and any different valeu won't.
        crossResetData.crc32 ^= 0xFFFFFFFF;
    }
}


// ---------------------------------------------------------------------------
//
// DateTime struct
//

// initialize a Date/Time struct from components
DateTime::DateTime(int16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) :
    hh(hour), mm(minute), ss(second), mon(month), dd(day), yyyy(year)
{
    // calculate seconds since midnight
    timeOfDay = hh*60*60 + mm*60 + ss;

    // calculaet the JDN
    jdn = DateToJDN(yyyy, mon, dd);
}

// date to JDN conversion
int DateTime::DateToJDN(int yyyy, int mon, int dd)
{
    int a = (14 - mon) / 12;
    int m = (mon + 12*a - 3);
    int y = yyyy + 4800 - a;
    return dd + (153*m + 2)/5 + 365*y + y/4 - y/100 + y/400 - 32045;
}

// JDN to date conversion
void DateTime::JDNToDate(int16_t &yyyy, uint8_t &mon, uint8_t &dd, int jdn)
{
    int f = jdn + 68569;
    int e = (4 * f)/146097;
    int g = f - (146097*e + 3) / 4;
    int h = 4000 * (g + 1)/1461001;
    int t = g - (1461 * h/4) + 31;
    int u = (80 * t)/2447;
    int v = u/11;

    yyyy = static_cast<uint16_t>(100 * (e - 49) + h + v);
    mon = static_cast<uint8_t>(u + 2 - 12*v);
    dd = static_cast<uint8_t>(t - 2447*u/80);
}

const char *DateTime::MonthNameShort() const
{
    static const char *name[]{ "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    return mon >= 1 && mon <= 12 ? name[mon-1] : "NUL";
}

const char *DateTime::MonthNameLong() const
{
    static const char *name[]{
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    return mon >= 1 && mon <= 12 ? name[mon-1] : "NULL";
}
