// Utilities
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <alloca.h>
#include <string.h>
#include <ctype.h>
#include "Utils.h"

// ----------------------------------------------------------------------------
//
// Number formatter
//

const char *NumberFormatterBase::InsertCommas(char *str)
{
    // count digits in the whole part (up to the '.' or end of string)
    int nDigits = 0;
    char *p = str;
    for ( ; *p != 0 && *p != '.' ; ++p)
    {
        if (isdigit(*p))
            ++nDigits;
    }

    // note where the whole part ends
    char *endOfWholePart = p;
    
    // scan ahead to the end of the string
    for ( ; *p != 0 ; ++p) ;
    char *endOfStr = p;

    // count the used space, including the terminal null byte
    used += (endOfStr - str) + 1;

    // figure out how much space is available
    int avail = bufLen - used;

    // Figure the number of commas required: one comma per three-digit
    // group in the whole part, rounding up to the next multiple of
    // three, minus one (since the commas only go BETWEEN groups).
    int nCommas = (nDigits + 2)/3 - 1;

    // If we have any commas, and there's room for all of them, insert
    // them.  Don't insert any unless we have room for all, since it
    // would leave a confused-looking mess if we only inserted some of
    // the commas that belong, whereas a number with no commas at all
    // is still sensible (if less readable).
    if (nCommas > 0 && avail >= nCommas)
    {
        // count the added space
        used += nCommas;

        // Starting at the end of the string, work backwards, moving
        // each character over by enough to make room for the added
        // commas.  Insert a comma every third character once we're
        // in the whole part.
        int nGroup = 0;
        for (char *src = endOfStr, *dst = endOfStr + nCommas ; src >= str ; )
        {
            // if we're in the whole part, count a group character
            if (src < endOfWholePart && isdigit(*src))
                ++nGroup;

            // copy this character
            *dst-- = *src--;

            // if we've reached 3 group characters, and we have any
            // commas left to insert, insert a comma
            if (nGroup == 3 && nCommas > 0)
            {
                *dst-- = ',';
                --nCommas;
                nGroup = 0;
            }
        }
    }

    // return the start of the string
    return str;
}

// ----------------------------------------------------------------------------
//
// Static string pool
//

// global singleton
StringPool stringPool;

const char *StringPool::Add(const char *buf, size_t len)
{
    // allocate space for the string plus null terminator, and copy it
    char *str = new char[len+1];
    memcpy(str, buf, len);
    str[len] = 0;

    // Return the new string.  Note that we deliberately abandon the
    // pointer, as an optimization that takes advantage of our
    // simplifying assumption that the overall program never terminates
    // and thus we will never need to release this memory.  The caller
    // is promising by calling this routine in the first place that the
    // string returned will remain referenced somewhere for the duration
    // of the session, absolving us of the responsibility to keep track
    // of it.
    return str;
}

const char *StringPool::Format(const char *fmt, ...)
{
    // format through FormatV
    va_list va;
    va_start(va, fmt);
    const char *ret = FormatV(fmt, va);
    va_end(va);
    return ret;
}

const char *StringPool::FormatV(const char *fmt, va_list va)
{
    // Measure the size of the formatted string
    va_list vaCount;
    va_copy(vaCount, va);
    int len = vsnprintf(nullptr, 0, fmt, vaCount);
    va_end(vaCount);

    // Format the string
    if (len > 0)
    {
        // allocate space on the stack for the text plus a null terminator
        char *buf = static_cast<char*>(alloca(len + 1));
        if (buf != nullptr)
        {
            // format the text
            vsnprintf(buf, len + 1, fmt, va);

            // add the string to the pool and return the pooled copy
            return Add(buf, len);
        }
    }

    // Failed to measure the string or allocate a temporary stack buffer
    // to foramt it.  As a last resort, just add the format string itself
    // as the pooled string.
    return Add(fmt);
}

const char *Format(const char *fmt, ...)
{
    // format through FormatV
    va_list va;
    va_start(va, fmt);
    const char *ret = FormatV(fmt, va);
    va_end(va);
    return ret;
}

