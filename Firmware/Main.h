// Pinscape Pico - Main Loop definitions
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string>

#include <pico/stdlib.h>
#include <pico/mutex.h>

// FlashStorage central directory size.  This must be a multiple of the
// flash sector size, which is 4K on a standard Pico.  Each file requires a
// 32-byte entry in the central directory, so one sector can hold 128 files.
// (A single sector is thus a gracious plenty for the current system design,
// since we only actually use a handful of files.)
const uint32_t FLASHSTORAGE_CENTRAL_DIRECTORY_SIZE = 4096;

// Unit identification.  This stores the configured Pinscape identifiers.
struct UnitID
{
    // Pinscape unit number.  This is used on the host side in contexts
    // where the user has to select among Pinscape Pico units, such as
    // the Config Tool and the DOF configuration.  This is meant to be a
    // human-friendly ID that's easy for the user to recognize and type;
    // it's typically simply 1 for the first Pinscape Pico unit on the
    // machine, 2 for the second, etc.  The ID doesn't have any meaning
    // other than to distinguish Pinscape Pico units on the local
    // machine.  It should thus be unique among other Pinscape Pico
    // units, but otherwise can be any desired value.  USB doesn't have
    // any way for one USB device to find other USB devices attached to
    // the same system, so we have no way to enforce uniqueness - that's
    // up to the user to configure properly.
    uint8_t unitNum = 1;

    // Pinscape unit name.  This is a human-friendly name that can be
    // displayed in lists of units on the host side.  It doesn't have
    // any particular uniqueness or format requirements, although it
    // will help avoid confusion for the user if it's unique among local
    // Pinscape Pico units, and the protocol contexts where it's used
    // (vendor interface and feedback control HID) impose a limit of
    // 31 ASCII characters (plus null temrinator).
    std::string unitName;

    // LedWiz emulation unit mask.  This sets the unit numbers that
    // a PC-side LedWiz DLL should assign to the virtual LedWiz units
    // it creates for its DLL API emulation.  This is for use with
    // the virtual pin cab community's custom LedWiz.dll replacement,
    // which can provide emulated LedWiz API access to Pinscape Pico
    // devices (as well as to Pinscape KL25Z units, genuine LedWiz
    // units, and open-source LedWiz clones).
    //
    // Each bit in the mask specified whether or not the corresponding
    // unit number can be assigned.  The low-order bit corresponds to
    // unit #1, bit 0x0002 is unit #2, bit 0x0004 is unit #3, etc, up
    // to bit 0x8000 for unit #16.  If a bit is set, the host-side DLL
    // is allowed to assign a virtual unit at that unit number.  The
    // DLL should assign the first virtual unit at the lowest-order
    // '1' bit, and assign additional units to sequentially higher
    // numbered units per the mask.
    //
    // Important: Pinscape Pico DOES NOT present an LedWiz-compatible
    // HID interface to the PC.  The LedWiz emulation we're talking
    // about here is strictly host-side, implemented through a custom
    // LedWiz.dll that replaces the manufacturer LedWiz.dll supplied by
    // Groovy Game Gear (the company that sells the original LedWiz).
    // The replacement DLL exposes an LedWiz-compatible API through
    // its DLL exports, but it communicates with the Pico through the
    // Pinscape Pico native USB interfaces.  That makes the Pico appear
    // as an LedWiz to applications that use the DLL API, but NOT to
    // applications that communicate directly with the LedWiz through
    // its custom USB HID protocol.  Groovy Game Gear explicitly made
    // the DLL API the official API to the LedWiz, and never published
    // its HID protocol, so virtually all pre-DOF game software that
    // uses the LedWiz does so through the DLL API.  This makes it
    // possible to provide access to non-LedWiz devices through the
    // DLL API by replacing the DLL, without any need for the devices
    // to use the same USB HID protocol as the LedWiz.  We take this
    // approach with Pinscape Pico because it lets us expose
    // extended functionality that goes beyond what the original
    // LedWiz HID protocol can provide, such as more than 32 ports
    // per device.  This approach also makes it unnecessary to use
    // the same VID/PID namespace as the original LedWiz, which
    // avoids conflicts with software that looks for those VID/PID
    // combinations without checking the HID format for compatibility.
    // The original manufacturer LedWiz.dll, for example, crashes if
    // it encounters a device using an LedWiz VID/PID but with a
    // different HID report format.  (That crash was actually the
    // main reason for coming up with a virtual pin cab replacement
    // for the manufacturer DLL in the first place, but once we had
    // our own replacement DLL in place, it allowed us to add other
    // new and extended features, such as emulation support for
    // devices that use other USB protocols.)
    int ledWizUnitMask = 0xFFFF;
};
extern UnitID unitID;


// Main loop statistics
struct CoreMainLoopStats
{
    // sum of time spent in main loop since last stats reset
    uint64_t totalTime = 0;

    // number of main loop iterations since last stats reset
    uint64_t nLoops = 0;

    // number of loop ierations since startup
    uint64_t nLoopsEver = 0;

    // maximum time of a single loop iteration
    uint32_t maxTime = 0;

    // add a time sample
    void AddSample(uint32_t dt) volatile
    {
        totalTime += dt;
        nLoops += 1;
        nLoopsEver += 1;
        if (dt > maxTime)
            maxTime = dt;
    }

    // reset the counters
    void Reset() volatile
    {
        totalTime = 0;
        nLoops = 0;
        maxTime = 0;
    }
};
extern CoreMainLoopStats mainLoopStats;

// Second-core loop statistics.  This struct is writable from the
// secondary core only, so there's no need for a mutex or spin lock
// for writing it.
extern volatile CoreMainLoopStats secondCoreLoopStats;

// Second-core loop stats reset requested.  The main core can write
// to this to ask the second core to reset the stats struct.
extern volatile bool secondCoreLoopStatsResetRequested;
