// Pinscape Pico - Time Of Day/Wall Clock Time
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The Pico has an on-board real-time clock that counts the microseconds
// since the last power-on or reset, but it doesn't have any on-board
// means of remembering the wall-clock time.  This module provides
// wall-clock time features using two mechanisms:
//
// 1. Get the time from the PC host.  Pinscape isn't designed to be a
// standalone embedded system; it's designed to be a USB peripheral
// connected to a PC host, and virtually all PCs have wall-time clocks
// as standard hardware.  The nature of USB is such that we can't
// initiate any sorts of requests to the PC, so there's no way that we
// can proactively ask the host what time it is.  But we can at least
// provide a USB interface that lets the host decide on its own to send
// us the curent time, and we can encourage host software that's
// Pinscape-aware to take advantage of this interface at key junctures.
// This is a lot easier than it might sound, because most PC pinball
// software that can access feedback controllers is based on DOF.  We
// can get practically all of the relevant host software to cooperate,
// without even knowing it, by embedding the time-of-day functionaltiy
// in DOF.  We can simply include a set-time-of-day call in the Pinscape
// Pico DOF driver's initial connection setup, and the Pico will get an
// update on the wall clock time every time a new DOF program starts up.
// For anyone using a DOF-aware game launcher like PinballY, this will
// get us the time of day shortly after system startup, since most
// pinball cab machines boot directly into their launcher programs.
//
// 2. Get the time from an external Real Time Calendar chip on an
// expansion board.  There are numerous calendar chips available that
// provide battery-backed time keeping, with time-of-day and calendar
// functions.  They're specifically designed to provide this sort of
// wall-clock-time functionality for embedded systems, and many of them
// have I2C interfaces, which is convenient for our design since we have
// I2C support for several other chips already.  In systems where a
// calendar chip is present, we can read the current time from the
// chip at restart, and we can re-read it any time.
//
// In either case, we only need to learn the wall-clock time once, and
// from that point on, we can use the Pico's microsecond clock to
// calculate the current wall-clock time in terms of the elapsed time
// since the last wall-clock time we received from the host or off-board
// calendar chip.  We don't have to count on the host or an external
// chip to give us constant updates.  Ideally, the host would send us
// updates once in a while - infrequently, so as to impose no meaningful
// load on the host or the Pico or the USB connection, but often enough
// to prevent the Pico's microsecond clock from drifting too far from
// the reference clock.  Once a day would probably be more than adequate
// to eliminate any meaningful drift, so doing it once at the start of
// each new DOF session should work perfectly.

#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <list>
#include <functional>

// external declarations
class JSONParser;

// date/time struct
struct DateTime
{
    // set up a blank DateTime
    DateTime() { }
    
    // Initialize with date/time components.  This automatically
    // fills in the derived fields (timeOfDay, jdn).
    DateTime(int16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

    // time of day, as the number of seconds since midnight
    uint32_t timeOfDay = 0;

    // Time of day, in 24-hour clock time.  This is redundant with the
    // seconds-since midnight value in timeOfDay, but we provide it as
    // a convenience for cases where the reference point is the nominal
    // clock time.  When the human user supplies a time via a
    // configuration file entry or other user interface, it's always
    // preferable to express the time in nominal clock hours, rather
    // than asking the user to do the arithmetic to convert to seconds
    // since midnight or the like.
    uint8_t hh = 0;   // hour, 0-23 (as in 24-hour/military time)
    uint8_t mm = 0;   // minute, 0-59
    uint8_t ss = 0;   // second, 0-59

    // Date, as the Julian day number (JDN), which is the number of days
    // since January 1, 4713 BC on the proleptic Julian calendar.  The
    // JDN is useful for calendar arithmetic involving days between
    // dates, and also makes it easy to calculate the day of the week
    // for a given date.
    //
    // Formally, the JDN epoch is Noon UTC on 1/1/4713 BC on the
    // proleptic Julian calendar.  For our purposes, though, we do
    // everything in terms of local time, with no adjustments for time
    // zone or daylight/summer time.  Our JDN epoch is therefore Noon
    // Local Time on the same date.
    int jdn = 0;

    // Calendar date.  This is always expressed as a Gregorian calendar
    // date, *even for historical dates before the Gregorian calendar
    // came into use*.  Dates expressed in terms of a calendar before
    // that calendar came into use are known as "proleptic" dates, and
    // they're perfectly unambiguous as long as everyone understands
    // which calendar they're relative to.  A calendar is just a system
    // for labeling sequential days, so it can be projected forward and
    // backward arbitrarily far in time.  You just have to be aware that
    // the proleptic Gregorian date for a given day might not match the
    // nominal date that people alive at the time would have used to
    // refer to the same day in contemporary writings, because they
    // would have written the date in terms of whichever calendar they
    // were using at the time.  So if you wanted to compare a given
    // Gregorian date against a date written down in a document from
    // 1150 CE, say, you'd have to take the respective calendars into
    // account by translating the date from the old document into its
    // proleptic Gregorian equivalent.
    //
    // Dates BCE are represented as negative years.  We use the
    // astronomer's convention that "year 0" is what most people would
    // call the year 1 BCE, -1 is 2 BCE, -2 is 3 BCE, and so on.
    uint8_t mon = 0;    // month, 1-12
    uint8_t dd = 0;     // day of the month, 1-31
    int16_t yyyy = 0;   // year on the modern Gregorian calendar
    
    // JDN conversions
    static int DateToJDN(int yyyy, int mon, int dd);
    static void JDNToDate(int16_t &yyyy, uint8_t &mon, uint8_t &dd, int jdn);

    // Month name - short (Jan, Feb, etc), long (January, February, etc)
    // English (non-localized)
    const char *MonthNameShort() const;
    const char *MonthNameLong() const;

    // Get the weekday - 0=Monday
    uint8_t GetWeekDay() const { return static_cast<uint8_t>(jdn % 7); }
};

// Time Of Day class
class TimeOfDay
{
public:
    TimeOfDay();

    // Get the current wall-clock time, if available.  If we know the
    // wall-clock time (from the host or from a connected calendar chip),
    // fills in the date/time struct and returns true.  If the clock
    // time isn't known, fills in the struct with the time since boot,
    // and returns false.
    //
    // The time is always expressed in local time.  We have no support
    // for time zone conversions or UTC adjustments.
    bool Get(DateTime &dt);

    // Is the wall clock time known?
    bool IsSet() const { return isSet; }

    // Update our current wall clock time with data from an external
    // source, such as the PC host (via the USB connection) or an RTC
    // chip.  This update will serve as our internal reference point for
    // future queries for the current time of day.
    //
    // The DateTime struct only needs to be populated with the clock
    // time and calendar date (hh, mm, ss, mon, dd, yyyy).  We can
    // derived the other elements from these, so they don't need to be
    // provided (and will be ignored if they are).  We take our input in
    // terms of the clock/calendar representation because it's
    // convenient for callers: it's a format that's always readily
    // available on Windows and Linux hosts, and it's also the format
    // that most calendar/RTC chips use natively.
    //
    // This is always expressed in LOCAL time, in the system's local
    // time zone.  We have no support for time zone conversions or UTC
    // adjustments.
    //
    // If updateRTC is true, we'll also update all of the configured RTC
    // chips with the new time setting.  This should be done when
    // receiving a time update from the PC host or from the user, since
    // those external sources take precedence over any past value stored
    // in RTCs.
    void SetTime(const DateTime &dt, bool updateRTC);

    // Save/restore the time-of-day information across a Pico reset.
    // This uses an __uninitialized_ram() data area (which the CRTL
    // startup code will leave untouched across a reset, preserving
    // the pre-reset contents as long as power isn't interrupted) to
    // store the time of day just before the reset, and then restore
    // the information just after the reset.  We use some checksum
    // information to verify that the information was actually stored
    // and isn't just random garbage after a power interruption.
    // After restoring the data, we explicitly clear the area to
    // ensure that the *next* reset won't try to reuse the old data,
    // in case the next reset isn't as orderly.
    void SaveBeforeReset();
    void RestoreAfterReset();

    // Real-time clock device interface.  This defines an abstract
    // interface to outboard RTC chips, for use in expansion boards.
    // Equipping an expansion board with a battery-backed RTC chip
    // allows the Pico to recover the current calendar date and wall
    // clock time at reset without help from the PC host.  The Pico has
    // its own on-board RTC that can keep track of the wall clock time
    // as long as the Pico is running, but it resets with each Pico
    // reset, so it's useless as a clock reference.  Numerous
    // inexpensive I2C RTC chips with battery backup capability are
    // available that can fill this gap.  In the absence of an RTC,
    // Pinscape can still learn the wall clock time from the PC host,
    // but the transmission must be initiated on the host side due to
    // the nature of the USB protocols, so we can't count on this.
    //
    // If any RTC chips are configured, the time-of-day module will
    // automatically program them with the current time whenever the PC
    // sends the time; this is how the RTCs learn the time in the first
    // place.  On Pico reset, we'll query the current time from any
    // available RTC.
    //
    // INITIALIZATION: During configuration, each chip is expected to
    // try loading the current date and time from the chip, and if
    // available, it must call timeOfDay.SetTime() to set the time.
    class RTC
    {
    public:
        // Write the current date and time to the RTC.  We call this
        // on each configured RTC whenever we get a time update sent
        // from the PC host.
        virtual void Write(const DateTime &curTime) = 0;

        // Read the curent date and time from the RTC, and invoke the
        // callback upon completion.  The callback must be invoked from
        // ordinary user-mode context (not interrupt context), and it
        // can be invoked with a nested call if the information is
        // available immediately.  RTC chips that use I2C or SPI
        // interfaces can only retrieve the information asynchronously
        // due to the design of our I2C/SPI subsystems, which is why
        // a callback is needed.
        //
        // If the time information is unavailable or an error occurs
        // retrieving it, invoke the callback with a DateTime with all
        // fields set to zero.
        virtual void ReadAsync(std::function<void(const DateTime&)> callback) = 0;

        // Display name of the chip, for logging messages
        virtual const char *DisplayName() const = 0;

    protected:
        // Helper routines for converting between BCD and native
        // integers.  Many RTC chips encode their timekeeping registers
        // in BCD format (binary-coded decimal).  A BCD byte encodes a
        // decimal value from 0 to 99, with the low nibble representing
        // the decimal units, and the high nibble representing the
        // decimal "tens" place.  For example, the decimal value 10 is
        // encoded in BCD as 0x10.
        static inline uint8_t BCDToInt(uint8_t bcd) { return static_cast<uint8_t>((((bcd >> 4) & 0x0F) * 10) + (bcd & 0x0F)); }
        static inline uint8_t IntToBCD(uint8_t i) { return static_cast<uint8_t>(((i / 10) << 4) | (i % 10)); }
    };

    // Add an RTC to the collection of configured RTCs.  Each RTC should
    // call this upon successful configuration (that is, the device is
    // physically present, and has been enabled in the JSON config).
    void AddRTC(RTC *rtc) { rtcs.emplace_back(rtc); }
    
protected:
    // internal list of all RTC devices
    std::list<RTC*> rtcs;
    
    // Do we have an outside time setting?  This is always false at
    // startup, since the Pico doesn't have its own timekeeping facility
    // when powered off, and so always powers up and comes out of a hard
    // reset with its internal real-time clock zeroed.  This will be set
    // if we can read the clock time from an external calendar chip or
    // receive a clock time update from the host.
    bool isSet = false;

    // Wall clock time as of the last update.  We can calculate the
    // current wall clock time by projecting forward from the last
    // update time by the elapsed time on the system clock since this
    // time reference was set.
    //
    // Set the initial values to proleptic (notional) Gregorian date
    // January 1, 0001 BC, which the logger renders as 01-01-0000.  This
    // is a good placeholder when the actual date is unknown, because
    // 0000 is easily recognizable and should intuitively mean "null" to
    // most people, which is exactly what it does mean in this case.
    // (Our log formatter uses the era notation that astronomers use,
    // where there's no era marker like AD/CE/BC/BCE, just a year number
    // that can be positive, zero, or negative.  In that system, the
    // year commonly labeled "1 BC" is instead rendered as year 0, "2
    // BC" is year -1, and so on.  This system is appealing to the
    // orderly mind because it doesn't require any special cases to do
    // arithmetic on year numbers across the epoch boundary.)
    int jdn = 1721060;            // Julian Day Number at last update (default
                                  //   1/1/0000, internal year 0 = 1 BC)
    uint32_t daySeconds = 0;      // time of day at last update, seconds since midnight

    // Pico system clock time of last wall clock time setting.  This is
    // the local Pico time-since-boot corresponding to the jdn and
    // daySeconds references values set above.  This serves as our
    // anchor point in time to project future Pico system clock times
    // to the corresponding wall clock times.
    uint64_t sys_time_us = 0;
};

// global singleton
extern TimeOfDay timeOfDay;
