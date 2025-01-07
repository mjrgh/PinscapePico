// Utilities
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <alloca.h>
#include <string.h>
#include "Utils.h"

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

