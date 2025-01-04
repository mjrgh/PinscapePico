// Pinscape Pico - Firmware Version Info
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdlib.h>
#include <pico/stdlib.h>
#include "Version.h"

// ---------------------------------------------------------------------------
//
// Program build timestamp - contains the time the image was build
//

// cached build timestamp string
char buildTimestamp[16];

// generate the build timestamp into buildTimestamp[]
static void GenBuildTimestamp()
{
    // Magic hash table for decoding the month from the __DATE__
    // macro into a month number 1..12.
    //
    // __DATE__ always uses the format "Mon DD YYYY", where Mon is one
    // of the three-letter English month abbreviations (Jan, Feb, Mar,
    // Apr, May, Jun, Jul, Aug, Sep, Oct, Nov, Dec).  The
    // day-of-the-month and year fields of __DATE__ are easy to parse
    // into numbers, since they're always expressed as numbers (in
    // decimal) to start with.  The month name requires some kind of
    // lookup, though, to convert to a numeric month.  What's the best
    // way to do this?  The simplest would be to compare the month
    // string to a series of fixed strings ("Jan", "Feb", etc) until we
    // find a match, but that's inefficient.  Or, we could construct a
    // std::map keyed by month name, but that's inefficient in its own
    // way, in that we have to build a map for this one lookup in a
    // small set.  What we'd really like to do is come up with a simple
    // arithmetic formula that we can apply to the ASCII character
    // values.  That seems beyond reach, but what we can do is create an
    // ad hoc "perfect hash" for this particular set of strings.  There
    // are many ways to do this; the one here takes advantage of the
    // near-uniqueness of the third letter of the month names: there are
    // only two letters that aren't unique ('n' for Jan/Jun, and 'r' for
    // Mar/Apr).  Both of those cases can be distinguished via the
    // *second* letter, so we don't even need to consider the first
    // letter (which is kind of amusing, in that you'd think at first
    // glance that the first letter would be the place to start).  A
    // little experimentation reveals that if you take the sum of the
    // upper-case ASCII values of the second and third letters, and then
    // take that mod 17, you get a value that's different for every
    // month - a perfect hash key into a 17-bucket hash table.
    //
    // Note that this doesn't reliably reject invalid month names, since
    // we're discarding a lot of information to get such a compact hash
    // code; many other two-letter sequences that aren't valid month
    // names will map to non-zero 'mm' values with this hash formula.
    // But that's okay, since __DATE__ comes from the compiler, not user
    // input.  It's safe to assume it's well-formed.  If we wanted to
    // adapt this approach to parse potentially invalid date strings as
    // well, a simple improvement would be to use the 'mm' result as an
    // index into an array of strings with the month names in order, and
    // test that against the input to make sure they match, rejecting
    // the input as invalid if not.
    constexpr const int month[] ={ 12, 5, 0, 8, 0, 0, 0, 1, 7, 4, 6, 3, 11, 9, 0, 10, 2 };

    // Note: the & ~0x20 clears the "lower-case" bit, converting lower
    // to upper and leaving upper unchanged, for case insensitivity.
    // This blatantly assumes that the character set is ASCII, but
    // that's a pretty safe assumption as long as you're not some kind
    // of retro mainframe fiend porting MVS/TSO to a Pico.
    constexpr const char* const date = __DATE__;
    constexpr const char* const time = __TIME__;
    constexpr const int mm = month[((date[1] & ~0x20) + (date[2] & ~0x20)) % 17];

    // build the date/time string
    char *p = buildTimestamp;
    *p++ = date[7];          // YYYY
    *p++ = date[8];          // YYYY
    *p++ = date[9];          // YYYY
    *p++ = date[10];         // YYYY
    *p++ = (mm / 10) + '0';  // MM
    *p++ = (mm % 10) + '0';  // MM
    *p++ = (date[4] == ' ' ? '0' : date[4]); // DD (leading space to zero)
    *p++ = date[5];          // DD
    *p++ = (time[0] == ' ' ? '0' : time[0]); // hh (leading space to zero)
    *p++ = time[1];          // hh
    *p++ = time[3];          // mm
    *p++ = time[4];          // mm

    // null-terminate it
    *p++ = 0;
}

// Initializer for the build timestamp.  This is a statically constructed
// object whose function is to generate the timestamp during program startup.
class BuildTimestampInitializer
{
    static BuildTimestampInitializer inst;
    BuildTimestampInitializer() { GenBuildTimestamp(); }
};
BuildTimestampInitializer BuildTimestampInitializer::inst;
