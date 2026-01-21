// Pinscape Pico firmware - Time Range
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <functional>

#include "Pinscape.h"
#include "TimeOfDay.h"
#include "TimeRange.h"

// Parse a time range string.
//
// The "range" argument uses one of the following formats:
//
// "<time> - <time>"
//    A daily clock time range.  The source is active when the wall clock
//    time is between the start and end time.  The time can span midnight.
//
// "<weekday> <time> - <weekday> <time>"
//    A span of time during a week, such as "Mon 9:00 - Wed 17:00" for
//    Monday at 9 AM to Wednesday at 5 PM, including all day Tuesday.
//    The range can span a week boundary, such as "Fri 17:00 - Mon 8:00".
//    If the times are omitted, the range includes all day on the start
//    and end days.
//
// "<weekday>/<weekday>/... <time> - <time>"
//    A time range that applies to one or more days of the week, such as
//    "Mon/Wed/Fri 12:00-13:00" for the lunch hour on Mondays, Wednesdays,
//    and Fridays only.  The time range can be omitted to include all day
//    on the selected days.
//
// "<date> <time> - <date> <time>"
//    A portion of the calendar year, from the starting time on the starting
//    date to the ending time on the ending date.  The times can be omitted
//    to include all day on the endpoint dates.  The date can span the end
//    of the year, as in "Dec 23 - Jan 2".
//
// <time> values use the format "mm:mm:ss am/pm".  The minutes and seconds are
// optional.  If the am/pm marker is absent, the time is taken as a 24-hour
// clock time.  The am/pm marker can also be written as am, pm, a, or p.
//
// <weekday> values are the literals Mon, Tue, Wed, Thu, Fri, Sat, Sun.
//
// <date> values are given as "month day", where month is Jan, Feb, Mar, Apr,
// May, Jun, Jul, Aug, Sep, Oct, Nov, Dec, and day is 1 to 31.
//
// All literals are case-insensitive.
//
TimeRange *TimeRange::Parse(const char *range, std::function<void(const char*, va_list)> recordErrorV)
{
    // start at the beginning of the string
    const char *p = range;

    // start with a default time span of midnight to midnight
    uint32_t tStart = 0, tEnd = 0;

    // start with no time range object
    TimeRange *tr = nullptr;

    // varargs error wrapper
    auto RecordError = [&recordErrorV](const char *msg, ...)
    {
        va_list va;
        va_start(va, msg);
        recordErrorV(msg, va);
        va_end(va);
    };

    // Parse a time element
    auto ParseTime = [&RecordError, &p](uint32_t &tResult)
    {
        // skip leading spaces
        for ( ; isspace(*p) ; ++p) ;

        // parse the hh, mm, and ss fields
        int ele[3]{ 0, 0, 0 };
        int nEle = 0;
        while (nEle < 3 && isdigit(*p))
        {
            // scan and skip the first digit, then scan the second digit if present
            int acc = *p++ - '0';
            if (isdigit(*p))
                acc = (acc*10) + (*p++ - '0');

            // store the element
            ele[nEle++] = acc;

            // scan and skip ':'
            if (*p == ':')
                ++p;
            else
                break;
        }

        // it's an error if we didn't find at least one element
        if (nEle == 0)
        {
            RecordError("invalid time format; expected 'hh:mm:ss [am/pm]', at %s", p);
            return false;
        }

        // check for an AM/PM marker
        for ( ; isspace(*p) ; ++p) ;
        char ampm = tolower(*p);
        if (ampm == 'a' || ampm == 'p')
        {
            // the hour now has to be 1-12
            if (ele[0] < 1 || ele[0] > 12)
            {
                RecordError("invalid time value (hour is out of range for 12-hour clock)");
                return false;
            }

            // 12 AM    -> 00:00
            // 1-11 PM  -> add 12:00 hours
            // otherwise PM adds 12 hundred hours, AM doesn't change anything
            if (ele[0] == 12 && ampm == 'a')
                ele[0] = 0;
            else if (ele[0] != 12 && ampm == 'p')
                ele[0] += 12;

            // skip the 'a'/'p', and then skip the 'm' if present
            ++p;
            if (*p == 'm' || *p == 'M')
                ++p;
        }

        // Range-check the elements.  Allow the special value 24:00:00, which
        // represents the start of the second just past the end of the day.
        // 24:00:00 is necessary to include because our intervals are treated
        // as exclusive of the ending time.
        if ((ele[0] > 23 || ele[1] > 59 || ele[2] > 59) && !(ele[0] == 24 && ele[1] == 0 && ele[2] == 0))
        {
            RecordError("invalid time value; must be 00:00:00 to 24:00:00");
            return false;
        }

        // express the result as seconds since midnight
        tResult = ele[0]*60*60 + ele[1]*60 + ele[2];

        // skip any trailing spaces
        for ( ; isspace(*p) ; ++p) ;

        // successfully parsed
        return true;
    };

    // Parse a weekday.  0=Monday, following the convention of our DateTime class.
    // Returns true on a match, and advances the string pointer past the matched
    // day name.  Returns false if no match, with no error messages; p is still
    // moved, but only past any leading whitespace.
    auto ParseWeekday = [&p](int &dayResult)
    {
        // skip spaces
        for ( ; isspace(*p) ; ++p) ;

        // Check for a weekday
        static const char weekdays[] = "montuewedthufrisatsun";
        const char *w = weekdays;
        for (int day = 0 ; *w != 0 ; w += 3, ++day)
        {
            // if the current day matches, and the token ends after the match
            // (that is, the next character isn't another alphabetic character),
            // it's a match
            if (tolower(p[0]) == w[0] && tolower(p[1]) == w[1] && tolower(p[2]) == w[2]
                && !isalpha(p[3]))
            {
                p += 3;
                dayResult = day;
                return true;
            }
        }

        // not matched
        return false;
    };

    // Parse a calendar date.  On success, fills in dateResult with the "year day"
    // (see YearDayNumber() in the header) of the date matched, advances p past the
    // matched text, and and returns true.  If no match, returns false, with no
    // error messages, and with p advanced only past any leading whitespace.
    auto ParseDate = [&RecordError, &p](int &dateResult)
    {
        // skip spaces, and remember where we started
        for ( ; isspace(*p) ; ++p) ;
        const char *start = p;

        // Check for a weekday
        static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
        const char *m = months;
        for (int mon = 1 ; *m != 0 ; m += 3, ++mon)
        {
            // if the current month matches, and the token ends after the match
            // (that is, the next character isn't another alphabetic character),
            // it's a match
            if (tolower(p[0]) == m[0] && tolower(p[1]) == m[1] && tolower(p[2]) == m[2]
                && !isalpha(p[3]))
            {
                // skip the month and whitespace
                for (p += 3 ; isspace(*p) ; ++p) ;

                // there has to be a day number following
                if (isdigit(*p))
                {
                    // parse the day number
                    int day = *p++ - '0';
                    while (isdigit(*p))
                        day = day*10 + *p++ - '0';

                    // success - fill in the Year Day Number in the result and return true
                    dateResult = YearDayNumber(mon, day);
                    return true;
                }
            }
        }

        // not matched - reset the parse point and return false
        p = start;
        return false;
    };

    // Check for a weekday.  This could be either a "Day Time - Day Time" range,
    // or a "Day/Day/Day Time-Time" range.
    if (int day; ParseWeekday(day))
    {
        // after the first day, we can have:
        //    -          - a full-day range, as in "Mon-Fri"
        //    /Day/...   - a day mask
        //    time       - either Day Time-Time or Day Time-Day Time
        //    end        - a single-day mask with no time
        for ( ; isspace(*p) ; ++p) ;
        if (*p == '-')
        {
            // full-day range - skip the '-' and parse the second day
            ++p;
            if (int endDay; ParseWeekday(endDay))
            {
                // success - create the day range, from 00:00:00 on the
                // starting day to 24:00:00 on the ending day
                tr = new WeekdayTimeRange(day, endDay, 0, 24*60*60);
            }
            else
                RecordError("expected second weekday name in 'Day-Day' range at '%s'", p);
        }
        else if (*p == '/')
        {
            // definitely a day mask list - parse all remaining days
            int dayMask = (1 << day);
            bool ok = true;
            while (*p == '/')
            {
                ++p;
                if (!ParseWeekday(day))
                {
                    RecordError("expected another weekday name in 'Day/Day/...' range at '%s'", p);
                    ok = false;
                    break;
                }
                dayMask |= (1 << day);
                for ( ; isspace(*p) ; ++p) ;
            }

            // if we didn't encounter an error in the day mask, continue to the optional time range
            if (ok)
            {
                // we can now either be at end of string, or at a time range
                if (*p == 0)
                {
                    // it's a simple day mask, with no time range, so it's all day on each selected day
                    tr = new WeekdayMaskTimeRange(dayMask, 0, 24*60*60);
                }
                else if (ParseTime(tStart))
                {
                    // we now need a '-' and the ending time
                    if (*p == '-')
                    {
                        ++p;
                        if (ParseTime(tEnd))
                        {
                            // success
                            tr = new WeekdayMaskTimeRange(dayMask, tStart, tEnd);
                        }
                    }
                    else
                        RecordError("expected '-' in day-and-time range at '%s'", p); 
                }
            }
        }
        else if (isdigit(*p))
        {
            // It's a time, so this is either a "Day Time-Day Time" or "Day Time-Time"
            // range.  In either case, parse the starting time.
            if (ParseTime(tStart))
            {
                // the next token must be the '-'
                for ( ; isspace(*p) ; ++p) ;
                if (*p == '-')
                {
                    // The next token can be:
                    //    Day     - a "Day Time-Day Time" range
                    //    Time    - a single-day mask range, "Day Time-Time"
                    ++p;
                    if (int dayEnd; ParseWeekday(dayEnd))
                    {
                        // it's a "Day Time-Day Time" range - we now just need an ending time
                        if (ParseTime(tEnd))
                        {
                            // success - create the day-and-time range
                            tr = new WeekdayTimeRange(day, dayEnd, tStart, tEnd);
                        }
                    }
                    else
                    {
                        // it's not a day name, so it must be a time, for a single-day mask range
                        if (ParseTime(tEnd))
                        {
                            // success - create the single-day mask range
                            tr = new WeekdayMaskTimeRange(1 << day, tStart, tEnd);
                        }
                    }
                }
                else
                    RecordError("expected '-' in day-and-time range at '%s'", p);
            }
        }
        else if (*p == 0)
        {
            // End of string.  A single day constitutes a day mask expression
            // that covers the whole period of the single day.
            tr = new WeekdayMaskTimeRange(1 << day, 0, 24*60*60);
        }
        else
        {
            // anything else is an error
            RecordError("invalid day-and-time range format - expected /, -, time, or end at '%s'", p);
        }
    }

    // If that didn't work, check for a month, for a "Date Time - Date Time" range
    if (int startDate; tr == nullptr && ParseDate(startDate))
    {
        // Found a date.  The next token can be a time (numeric), '-' for
        // an all-day date range, or end of string for a single day.
        for ( ; isspace(*p) ; ++p) ;
        if (*p == '-')
        {
            // it's an all-day range - parse the second date
            ++p;
            if (int endDate; ParseDate(endDate))
            {
                // success
                tr = new DateAndTimeRange(startDate, endDate, 0, 24*60*60);
            }
            else
                RecordError("expected date range ending date at '%s'", p);
        }
        else if (*p == 0)
        {
            // end of string - the range is a single day, all day
            tr = new DateAndTimeRange(startDate, startDate, 0, 24*60*60);
        }
        else if (ParseTime(tStart))
        {
            // this must be followed by '-', then another date and time
            for ( ; isspace(*p) ; ++p) ;
            if (*p == '-')
            {
                // get the ending date
                ++p;
                if (int endDate; ParseDate(endDate))
                {
                    // get the ending time
                    if (ParseTime(tEnd))
                    {
                        // success
                        tr = new DateAndTimeRange(startDate, endDate, tStart, tEnd);
                    }
                }
                else
                    RecordError("expected date range ending date at '%s'", p);
            }
            else
                RecordError("expected '-' in date-and-time range at '%s'", p);
        }
    }

    // If we didn't match one of the other formats, it must be the simple time-of-day range.
    if (tr == nullptr && ParseTime(tStart))
    {
        if (*p == '-')
        {
            // skip the '-' and parse the end time
            ++p;
            if (ParseTime(tEnd))
            {
                // success - create the simple time range
                tr = new TimeRange(tStart, tEnd);
            }
        }
        else
            RecordError("expected '-' in time range at '%s'", p);
    }

    // wherever we ended up, we have to be at the end of the string now
    for ( ; isspace(*p) ; ++p) ;
    if (*p != 0)
        RecordError("unexpected extra text '%s' after time range ignored", p);

    // if didn't create a result object at all yet, create a default
    // range based on the start/stop times we parsed, if any
    if (tr == nullptr)
        tr = new TimeRange(tStart, tEnd);

    // return the result
    return tr;
}

bool TimeRange::IsInRange(const DateTime &t)
{
    // If our range's starting time is less than the end time, as in
    // 10AM-2PM, treat the range as all contained within one 24-hour
    // day.  If the starting time is greater than the end time, as
    // in 11PM-1AM, treat it as spanning midnight, so it's really
    // two time ranges: start to midnight, and midnight to end.
    if (tStart < tEnd)
    {
        // the range is contained within one 24-hour period
        return t.timeOfDay >= tStart && t.timeOfDay < tEnd;
    }
    else
    {
        // The range spans midnight, so treat it as two disjoint
        // ranges, start to midnight + midnight to end.  Note that
        // it's not necessary to compare t.timeOfDay to midnight in
        // either subrange, since it's always >= the start-of-day
        // mightnight and always < the end-of-day midnight.
        return t.timeOfDay < tEnd || t.timeOfDay >= tStart;
    }
}

// Weekday time range, as in "Tue 9:00 - Fri 17:00" (from Tuesday at
// 9am to Friday at 5pm, which includes all day Wednesday and Thursday)
bool WeekdayTimeRange::IsInRange(const DateTime &t)
{
    // Determine if the time is in range.  First, the day of
    // the week has to be within the range.
    bool inRange;
    int weekday = t.GetWeekDay();
    if (weekdayStart < weekdayEnd)
    {
        // start day < end day -> a range within one week
        inRange = weekday >= weekdayStart && weekday <= weekdayEnd;
    }
    else
    {
        // end day < start day -> range spans a week boundary
        inRange = weekday <= weekdayEnd || weekday >= weekdayStart;
    }
    
    // Even if it's within the weekday range, it could be out of the
    // time range.  The time range is tStart to midnight on the
    // starting day, all day on days between start and end, and
    // midnight to tEnd on the ending day.
    if ((weekday == weekdayStart && t.timeOfDay < tStart)
        || weekday == weekdayEnd && t.timeOfDay >= tEnd)
        inRange = false;

    // return the result
    return inRange;
}

// Weekday mask time range, as in "Mon/Wed/Fri 9:00-17:00" (9am to 5pm
// on Mondays, Wednesdays, and Fridays)
bool WeekdayMaskTimeRange::IsInRange(const DateTime &t)
{
    // Determine first if the weekday is in the mask
    int weekday = t.GetWeekDay();
    bool dayInMask = ((weekdayMask & (1 << weekday)) != 0);
    
    // Also check to see if the PRIOR weekday is in the mask
    int priorWeekday = (weekday == 0 ? 6 : weekday - 1);
    bool priorDayInMask = ((weekdayMask & (1 << priorWeekday)) != 0);
    
    // Now check the time range
    if (tStart < tEnd)
    {
        // the time range is all within a single day, so we only
        // have to check that the current time is between the start
        // and end points, and the current day is in the mask
        return dayInMask && t.timeOfDay >= tStart && t.timeOfDay < tEnd;
    }
    else
    {
        // The time range spans midnight, so it's a match if the
        // current time is greater than the starting time AND
        // current day is in the mask, OR the current time is less
        // than the ending time AND the PRIOR day is in the mask.
        // We have to match the PRIOR day in the latter case because
        // the time period started on the prior day, before the
        // clock rolled over at midnight.
        return (dayInMask && t.timeOfDay >= tStart) || (priorDayInMask && t.timeOfDay < tEnd);
    }
}

// Calendar date and time range, as in "Mar 1 0:00:00 - Nov 30 23:59:59"
bool DateAndTimeRange::IsInRange(const DateTime &t)
{
    // First, determine if the date is in range.  We work in terms
    // of "year day numbers", where we can determine if one date is
    // before or after another by simple integer comparison.
    bool inRange = false;
    int day = YearDayNumber(t.mon, t.dd);
    if (dateStart < dateEnd)
    {
        // the date range is all within one year, so we're in range
        // if the date falls between the start and end dates
        inRange = day >= dateStart && day <= dateEnd;
    }
    else
    {
        // the date range spans the end of the year, so we're in
        // range if the date is after the starting date or before
        // the ending date
        inRange = day >= dateStart || day <= dateEnd;
    }
    
    // If the date is on the starting or ending date, the time
    // has to be after the starting time or before the ending
    // time (respectively).  Days between the endpoints include
    // the full 24-hour period, so the time of day isn't a factor
    // unless the date falls exactly on one of the endpoints.
    if ((day == dateStart && t.timeOfDay < tStart)
        || (day == dateEnd && t.timeOfDay >= tEnd))
        inRange = false;
    
    // return the result
    return inRange;
}

