// Pinscape Pico firmware - GPIO manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines some simple resource management and helper functions for GPIOs.

#pragma once

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"

// Is a GP port number valid?  This is true if the GP is one of the
// exposed GP pins on the standard Pico board.
inline bool IsValidGP(uint8_t gp) { return gp <= 22 || (gp >= 25 && gp <= 28); }

// GPIO manager class
class GPIOManager
{
protected:
    // forward declaration
    class SharedInputCoordinator;

public:
    GPIOManager();

    // Claim a GPIO for exclusive use by the named subsystem.  The
    // subsystem name string must have static storage duration.  Returns
    // true on success, and records the assignment for future reference.
    // If the GPIO has already been claimed, logs an error showing the
    // conflict (listing both subsystem names, to help the user
    // troubleshoot the configuration) and returns false.
    //
    // If gp == -1, the function returns success without claiming any
    // pins.  This can be used to call Claim() on pin assignments that
    // are optional (i.e., that the config file can leave undefined).
    // The caller is thus expected to validate that a valid pin was
    // assigned for any required pin.
    //
    // 'usage' is an optional string giving the nominal usage for the
    // port, for display in user interface contexts, such as in the
    // Windows config tool.  We use the owner subsystem name as the
    // default if the usage string is omitted or null.  We usually
    // set the subsystem name to something that clarifies the location
    // in the JSON configuration tree where the pin assignment is
    // configured, to make it easier to pinpoint the location in the
    // JSON of any conflicts.  This isn't always a friendly name for
    // display in the UI, though, which is why we proivde the separate
    // 'usage' string.
    bool Claim(const char *subsystemName, const char *usage, int gp);
    bool Claim(const char *subsystemName, int gp) { return Claim(subsystemName, nullptr, gp); }

    // Claim multiple GPIOs
    template<typename... T> bool Claim(const char *subsystemName, int gp, T... more) {
        return Claim(subsystemName, gp) && Claim(subsystemName, more...); }
    template<typename... T> bool Claim(const char *subsystemName, const char *usage, int gp, T... more) {
        return Claim(subsystemName, usage, gp) && Claim(subsystemName, usage, more...); }

    // Claim an GPIO for shared use as an input.  Reading a GPIO input
    // pin has no side effects, so an input pin can be shared among
    // multiple readers, as long as they agree on all of the input pin
    // configuration settings (pull-up/down and hysteresis mode).
    //
    // If the port isn't already claimed, this will configure the port
    // with the specified parameters and return true.  If the port is
    // already configured with the same parameters, it'll simply return
    // true to let the caller share the port with other callers.  If the
    // port has been claimed for exclusive use by another caller, or as
    // a shared input but with different parameters, logs an error and
    // returns false.
    //
    // Callers must not change the port settings after claiming a port
    // in shared mode, since other callers might depend upon the
    // original settings staying in effect.  If you need the ability to
    // change the port settings dynamically, you should claim the port
    // in exclusive mode instead.
    bool ClaimSharedInput(const char *subsystemName, int gp, bool pullUp, bool pullDown, bool hysteresis) {
        return ClaimSharedInput(subsystemName, nullptr, gp, pullUp, pullDown, hysteresis); }
    bool ClaimSharedInput(const char *subsystemName, const char *usage, int gp, bool pullUp, bool pullDown, bool hysteresis);

    // Get the usage string for a port.  Returns null if the port
    // hasn't been assigned.
    const char *GetUsage(int gp) const;

protected:
    // GPIO reservation status by port.  Since the RP2040 has a fixed
    // set of 32 GPIOs, we can use a simple fixed array for this,
    // indexed by GP number.  A claimed entry records the claiming
    // subsystem's name string; an unclaimed entry is null.
    struct Status
    {
        // reservation status
        enum class Res
        {
            Free,        // not claimed
            Exclusive,   // reserved exclusively
            SharedInput  // reserved as a shared input
        };
        Res reserved = Res::Free;

        // claiming subsystem (first claimant if shared)
        const char *owner = nullptr;

        // Nominal usage, for displaying the port's function in user interfaces
        // (such as the config tool).  This is optional; if not set, we use the
        // owner string as the default.
        const char *usage = nullptr;

        // Shared input pin configuration.  All sharers must use the
        // same configuration.
        struct
        {
            bool pullUp = false;        // pull-up enabled
            bool pullDown = false;      // pull-down enabled
            bool hysteresis = false;    // input hysteresis (Schmitt trigger mode) enabled
        } sharedInput;
    };
    Status status[32];
};

// global singleton instance
extern GPIOManager gpioManager;

