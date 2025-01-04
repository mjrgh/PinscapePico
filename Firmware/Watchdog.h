// Pinscape Pico - Pico Hardware Watchdog
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// WATCHDOG SCRATCH REGISTER USAGE
//
// The application uses scratch registers SCRATCH0 through SCRATCH3
// as follows:
//
//    SCRATCH0  - Main loop
//    SCRATCH1  - Main loop
//    SCRATCH2  - TV ON subsystem
//    SCRATCH3  - TV ON subsystem
//    SCRATCH4  - reserved by Pico SDK
//    SCRATCH5  - reserved by Pico SDK
//    SCRATCH6  - reserved by Pico SDK
//    SCRATCH7  - reserved by Pico SDK
//
// Registers SCRATCH4 through SCRATCH7 are reserved by the SDK.  It
// might even be fair to say that ALL of the SCRATCHn registers are
// reserved, since the SDK documentation mentions that the SDK claims
// some of them, but never says which ones, leaving it unspecified which
// are reserved and which aren't.  The SDK's claim on 4-7 is just an
// emprical fact observed by inspecting the current SDK source, so the
// Pico SDK developers might in fact consider all of the registers to be
// reserved for their future use.  But I don't think so: I think they
// chose to use the "top" four registers for their own purposes because
// they assumed that application developers would tend to claim them
// starting at zero, and they were thus hoping that using the top four
// would happen not to conflict with applications that were unaware of
// the SDK's use of the registers.  Plus, the Pico SDK is open-source,
// so its claim on 4-7 (and not 0-3) is effectively a published fact
// even if it's not spelled out in the documentation, so hopefully the
// SDK developers will consider it at least undesirable to claim the
// remaining registers, if not out of the question entirely.

#pragma once
#include <stdlib.h>
#include <stdint.h>

// Enable the hardware watchdog with the given reboot timeout
// in millseconds.  Returns the previous timeout.
extern int WatchdogEnable(int timeout_ms);

// Watchdog temporary disabler.  This disables the watchdog when the
// object comes into scope, and re-enables it when the object goes out
// of scope.  Simply instantiate one of these objects in a function or
// local scope where you want the watchdog temporarily disabled.
//
// The watchdog should be disabled before initiating any operation that
// will by design block the main CPU core from performing its normal
// polling functions for an extended period.  This should be used only
// when absolutely necessary - most long-running operations can and
// should be designed to run asynchronously, allowing the main loop to
// continue running while the operation proceeds in the background via
// external hardware or state machine polling.  However, there are some
// isolated cases of long-running operations that must be performed
// atomically, most notably writes to flash memory.  For such cases,
// it's necessary to disable the watchdog timer, because it will reset
// the Pico if the main loop ever gets blocked for more than the
// watchdog timeout interval.
class WatchdogTemporaryDisabler
{
public:
    WatchdogTemporaryDisabler();
    ~WatchdogTemporaryDisabler();
    int origTimeout;
};

// Watchdog temporary extender.  This temporarily changes the watchdog
// timeout to the given interval, overriding the normal timeout.  This
// can be used for operations that can't be interrupted for relatively
// long periods, such as flash programming.  In such cases we still want
// the watchdog to intervene if the program truly freezes up for a long
// time, but the tolerance for delays needs to be increased from the
// default.
class WatchdogTemporaryExtender
{
public:
    WatchdogTemporaryExtender(int temporary_timeout_ms);
    ~WatchdogTemporaryExtender();
    int origTimeout;
};

