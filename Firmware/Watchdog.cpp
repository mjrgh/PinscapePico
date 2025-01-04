// Pinscape Pico - Pico Hardware Watchdog
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdlib.h>
#include <stdint.h>

// Pico SDK headers
#include <hardware/watchdog.h>

// project headers
#include "Pinscape.h"
#include "Watchdog.h"
#include "Reset.h"

// Reboot timeout in milliseconds
static int watchdogTimeout = 100;

// enable the watchdog
int WatchdogEnable(int timeout_ms)
{
    // reset the watchdog, in case we're shortening the timeout and
    // already have a higher count
    watchdog_update();

    // enable the hardware watchdog with the new timeout
    watchdog_enable(timeout_ms, true);

    // restart the counter watchdog counter
    watchdog_update();

    // remember the new timeout (for the temporary disabler)
    int oldTimeout = watchdogTimeout;
    watchdogTimeout = timeout_ms;

    // return the old timeout
    return oldTimeout;
}

// ---------------------------------------------------------------------------
//
// watchdog scope-level disabler
//

WatchdogTemporaryDisabler::WatchdogTemporaryDisabler()
{
    // remember the old timeout
    origTimeout = watchdogTimeout;

    // Since the watchdog is going to be disabled, make sure that external
    // hardware is in a state where it will be safe without our attention
    // for an extended period.  This helps avoid any catastrophes involving
    // solenoids getting locked on because we're not servicing their port
    // time-limiter functions.
    picoReset.PrepareForReboot();

    // The Pico SDK doesn't provide a function for disabling the watchdog,
    // so we have to poke the hardware register directly.
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

WatchdogTemporaryDisabler::~WatchdogTemporaryDisabler()
{
    // re-enable the watchdog using the current timeout
    WatchdogEnable(origTimeout);
}

// ---------------------------------------------------------------------------
//
// watchdog scope-level time extender
//

WatchdogTemporaryExtender::WatchdogTemporaryExtender(int temporary_timeout_ms)
{
    // do an immediate update to start the new timeout period
    watchdog_update();

    // change to the new timeout
    origTimeout = WatchdogEnable(temporary_timeout_ms);
}

WatchdogTemporaryExtender::~WatchdogTemporaryExtender()
{
    // enable the watchdog using the original timeout
    WatchdogEnable(origTimeout);
}
