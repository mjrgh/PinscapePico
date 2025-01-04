// Pinscape Pico firmware - GPIO manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines some simple resource management and helper functions for GPIOs.


// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <vector>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "GPIOManager.h"
#include "Logger.h"

// global singleton instance
GPIOManager gpioManager;

// construction
GPIOManager::GPIOManager()
{
}

// get the usage string for a port
const char *GPIOManager::GetUsage(int gp) const
{
    // return null if it's out of range
    if (gp < 0 || gp >= _countof(status))
        return nullptr;

    // return the usage if set, otherwise use the owner name
    auto &s = status[gp];
    return s.usage != nullptr ? s.usage : s.owner;
}

// Claim a GPIO for exclusive use
bool GPIOManager::Claim(const char *subsystemName, const char *usage, int gp)
{
    // gp == -1 indicates an optional pin that was left undefined.
    // Allow these to succeed without claiming any resources.
    if (gp == -1)
        return true;

    // validate that the pin is valid
    if (!IsValidGP(gp))
    {
        Log(LOG_ERROR, "%s: Invalid GPIO port %d\n", subsystemName, gp);
        return false;
    }

    // check to see if the pin is already claimed by another subsystem
    auto &s = status[gp];
    if (s.reserved != Status::Res::Free)
    {
        // already claimed - log an error and return failure
        Log(LOG_ERROR, "%s: Unable to assign GP%d; this GPIO is already assigned to %s\n",
            subsystemName, gp, s.owner);
        return false;
    }

    // the pin is valid and available - claim it and return success
    s.reserved = Status::Res::Exclusive;
    s.owner = subsystemName;
    s.usage = usage;
    return true;
}

// Claim a GPIO as a sharable input pin
bool GPIOManager::ClaimSharedInput(
    const char *subsystemName, const char *usage,
    int gp, bool pullUp, bool pullDown, bool hysteresis)
{
    // gp == -1 indicates an optional pin that was left undefined.
    // Allow these to succeed without claiming any resources.
    if (gp == -1)
        return true;

    // validate that the pin is valid
    if (!IsValidGP(gp))
    {
        Log(LOG_ERROR, "%s: Invalid GPIO port %d\n", gp);
        return false;
    }

    // Check the pin status
    auto &s = status[gp];
    switch (s.reserved)
    {
    case Status::Res::Free:
        // The pin is free, so it's safe to claim, and the physical GPIO
        // pin hasn't yet been configured.  Configure it with the
        // specified parameters.
        gpio_init(gp);
        gpio_set_dir(gp, GPIO_IN);
        gpio_set_pulls(gp, pullUp, pullDown);
        gpio_set_input_hysteresis_enabled(gp, hysteresis);

        // Record the reservation
        s.reserved = Status::Res::SharedInput;
        s.owner = subsystemName;
        s.usage = usage;
        s.sharedInput.pullUp = pullUp;
        s.sharedInput.pullDown = pullDown;
        s.sharedInput.hysteresis = hysteresis;

        // Success
        return true;

    case Status::Res::SharedInput:
        // The pin has already been claimed as a shared input, so it can
        // also be claimed by anyone else as a shared input providing that
        // they want the same port configuration parameters.
        if (pullUp != s.sharedInput.pullUp
            || pullDown != s.sharedInput.pullDown
            || hysteresis != s.sharedInput.hysteresis)
        {
            // incompatible parameters - log an error and return failure
            Log(LOG_ERROR, "%s: Unable to use GP%d as a shared input, due to incompatible parameters already set by %s\n",
                subsystemName, gp, s.owner);
            return false;
        }

        // The new request is compatible, so we can simply return success.
        // Note that there's no need to reconfigure the physical GPIO, since
        // the first caller already configured it with identical parameters.
        return true;

    case Status::Res::Exclusive:
    default:
        // it's already claimed in exclusive mode, so it can't be shared;
        // log an error and return failuer
        Log(LOG_ERROR, "%s: Unable to assign GP%d as a shared input; this GPIO is already exclusively assigned to %s\n",
            subsystemName, gp, s.owner);
        return false;
    }
}
