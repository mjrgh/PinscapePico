// Pinscape Pico - Expansion Boards
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module handles the interface to the expansion board environment,
// if any.  The Pico is a pretty bare-bones MCU, so using it to control
// a virtual pinball cabinet will usualy require a number of outboard
// peripherals, such as PWM controllers, high-power switching circuits,
// and various sensors.  These can be connected with ad hoc wiring, but
// in most cases it will be a lot more convenient to use a dedicated
// circuit board specially designed to host the Pico and a collection
// of peripheral chips.  We refer to such boards as Expansion Boards,
// since they expand the Pico's basic I/O capabilities with new ports
// and sensors.
//
// This module doesn't assume any particular expansion board design.
// It's meant to work with a wide variety of host boards.  In
// particular, we don't assume that an expansion board includes any
// specific peripherals devices, or even classes of devices.  Those
// are all handled through their own device-specific classes that are
// configured separately.  Rather, this is merely a container for
// functions and settings that are likely to be relevant to at least
// some host boards, and that aren't handled in the various classes
// for individual peripheral devices.
//

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <pico/stdlib.h>

// forwards/externals
class JSONParser;

// expansion board class
class ExpansionBoard
{
public:
    // Configure
    void Configure(JSONParser &json);

    // Peripheral power control.  An expansion board might provide
    // software control over the peripheral power supply through a
    // GPIO port.  This makes it possible for the software to execute
    // a hard reboot on all peripherals by cycling their power supply,
    // indepedently of the Pico's own power supply.  This can be
    // useful to recover from device errors that can't be cleared by
    // software commands.
    struct PeripheralPower
    {
        int gpEnable = -1;             // GPIO port that enables peripheral power (-1 -> not used)
        bool activeHigh = true;        // true -> power enable signal is active high
        uint32_t offTime = 250;        // power-off time (interval to cut power during a reset) in milliseconds
        uint32_t waitTime = 250;       // wait time (interval to wait after power is restored for chips to cycle) in milliseconds
    };
    PeripheralPower peripheralPower;

    // Cycle power to the peripherals
    void CyclePeripheralPower();
};

// global singleton
extern ExpansionBoard expansionBoard;
