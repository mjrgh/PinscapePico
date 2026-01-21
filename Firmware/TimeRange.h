// Pinscape Pico firmware - Time Range
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The TimeRange class provides an abstraction for representing clock
// time and calendar date ranges.  This can be used for scheduling
// services, such as a service that turns a device on and off according
// to the time of day or day of the week.
//
// A major feature of the class is a string parser that accepts time
// range specifications in a human-readable format.  This makes it
// easy to create time ranges from instructions entered by the user,
// such as in the configuration file.
//
// Several types of time and date ranges are supported:
//
// - Time of day: a range that covers a contiguous range of wall clock
//   time every day, such as 2:00 PM to 4:00 PM
//
// - Span of days each week: a range that covers a span of days within
//   a week, such as Tuesday to Thursday.  The starting day and ending
//   day can have specific times of day associated, such as 5:00 PM
//   Friday to 9:00 AM Monday.
//
// - Specific days of the week: a range that includes a specified set
//   of days each week, such as "Monday, Wednesday, and Friday".  This
//   can also specify a start and end time that will be applied to each
//   day.
//
// - Date range: selects a range of dates on the calendar, such as
//   "March 20 to June 21".  The start and end days can also have
//   specific times of day associated.
//

#pragma once

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <functional>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "TimeOfDay.h"

// Base time range class.  The base class selects a range of wall
// clock times every day.
class TimeRange
{
public:
    // Parse a time range specification string, returning a TimeRange
    // object.  A valid object is always returned, whether or not
    // parsing errors occur; in the event of errors, the returned
    // object represents a best effort at parsing what we could, so
    // it might not match the user's intention.  Errors are logged
    // through the callback function.
    static TimeRange *Parse(const char *str, std::function<void(const char*, va_list)>);

    // create from a range of times of day, specified as seconds since midnight
    TimeRange(uint32_t tStart, uint32_t tEnd) : tStart(tStart), tEnd(tEnd) { }

    // is the given system clock time within our range?
    virtual bool IsInRange(const DateTime &t);

protected:
    // Endpoints of the time time-of-day range, as seconds since midnight.
    // If tEnd < tStart, the range spans midnight, so it's from tStart on
    // day N to tEnd on day N+1.
    uint32_t tStart = 0, tEnd = 0;
    
    // Calculate a "year day number".  This converts a date from mm/dd
    // format to a high-radix integer representing the day of the year
    // in non-contiguous ascending order.  (Non-continguous means that
    // the year day numbers representing two consecutive calendar days
    // might not be consecutive integers; i.e., there might be a gap in
    // the year day numbering between two consecutive calendar days.)
    // This format places all dates over the year in a sorting order,
    // where we can determine if one date is before or after another by
    // simple integer comparison of the day number values.  It's also
    // easy to recover the mm/dd date from a day number, although we
    // don't need to do that for our purposes here.  This format is NOT
    // usable to calculate days between dates, because of the non-
    // contiguous numbering.
    static int YearDayNumber(int mm, int dd) { return (mm << 8) + dd; }
};

// Weekday time range.  Selects inputs depending on
// whether the current time is within a specified range
// within the week.
class WeekdayTimeRange : public TimeRange
{
public:
    WeekdayTimeRange(int weekdayStart, int weekdayEnd, uint32_t tStart, uint32_t tEnd) :
        TimeRange(tStart, tEnd), weekdayStart(weekdayStart), weekdayEnd(weekdayEnd) { }

    virtual bool IsInRange(const DateTime &t) override;

protected:
    // Endpoints of the weekday range, 0=Monday, 1=Tuesday, etc.  If
    // weekdayStart > weekdayEnd (e.g., 6-1 for Sunday to Tuesday), the
    // range spans the start of the new week.
    int weekdayStart;
    int weekdayEnd;
};

// Weekday mask time range.  Selects inputs according to the time of day
// on selected days of the week.
class WeekdayMaskTimeRange : public TimeRange
{
public:
    WeekdayMaskTimeRange(int weekdayMask, uint32_t tStart, uint32_t tEnd) :
        TimeRange(tStart, tEnd), weekdayMask(weekdayMask) { }

    virtual bool IsInRange(const DateTime &t) override;

protected:
    // Weekday mask.  This encodes a set of days of the week as bits,
    // with (1<<0)=Monday, (1<<1)=Tuesday, etc.  The time range is only
    // valid for tStart within a day in the mask.
    //
    // For example, 0x15 selects Monday, Wednesday, and Friday, so if
    // tStart is 13:00 and tEnd is 14:00, valid times are from 13:00
    // to 14:00 on Mondays, Wednesdays, and Fridays.
    //
    // If tEnd < tStart, the time range spans midnight, and must START
    // on a day in the mask.  Continuing our 0x15 M-W-F example, if the
    // range is 23:00 to 01:00, the times span 23:00 Monday to 01:00 Tuesday,
    // 23:00 Wednesday to 01:00 Thursday, and 23:00 Friday to 01:00 Saturday.
    int weekdayMask = 0;
};

// Date-and-time range.  Selects inputs according to the time of day
// within a span of calendar dates within a year.
class DateAndTimeRange : public TimeRange
{
public:
    DateAndTimeRange(int yearDayStart, int yearDayEnd, uint32_t tStart, uint32_t tEnd) :
        TimeRange(tStart, tEnd), dateStart(yearDayStart), dateEnd(yearDayEnd) { }

    virtual bool IsInRange(const DateTime &t) override;

protected:
    // Date range, as a "year day number"
    int dateStart = 0, dateEnd = 0;
};
