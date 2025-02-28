// Pinscape Pico - Fault Handler
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines an exception handler that collects data on the CPU state when a
// hardware exception occurs, and stores it in a portion of RAM that the
// program startup code won't initialize on reset.  It then performs a CPU
// reset, which is the normal default action after a hard fault.  The
// non-initialized RAM section preserves the captured data across the reset,
// allowing the new session to recover the data and record it in the log.
// This is meant to be helpful for debugging purposes, by identifying the
// location and cause of a crash fault.


// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/exception.h>

// local project headers
#include "Pinscape.h"

// externals/forwards
class CommandConsole;

class FaultHandler
{
public:
    // Initialize - sets up the exception handlers.  This must be called
    // once at startup, before any other methods are invoked.
    void Init();

    // Is there valid crash data?
    bool IsCrashLogValid();

    // If the crash log contains valid data, log it.  This also has the
    // side effect of clearing the crash log so that we don't repeat it
    // on a future session where we don't generate a new crash log.
    void LogCrashData();

    // Log the basic crash data to a command console.
    void LogCrashDataTo(CommandConsole *console);

    // Log one line of the stack dump to a command console.  Returns
    // true if there's more stack dump data to log, false if not.
    // lineNum selects the text display line number, starting at zero.
    bool LogStackDataTo(CommandConsole *console, int lineNum);
    
protected:
};

// fault handler singleton
extern FaultHandler faultHandler;
