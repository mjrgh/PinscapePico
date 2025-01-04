// Pinscape Pico - Base class for quadrature encoders
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines a base class for basic two-channel quadrature encoders.
// Most of these sensors work the same way, with two GPIO input lines
// representing the states of the two channels, which can be interpreted
// as a two-bit Gray code that encodes one step of relative motion, plus
// or minus, on each state transition.  This base class handles the GPIO
// inputs with interrupt handlers, and keeps a running position counter
// based on detected transitions.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"

// Generic quadrature encoder base class
class QuadratureEncoder
{
public:
    // Construction.  'lpi' is the encoder's lines-per-inch metric, which is
    // typically a fixed feature of the specific sensor's hardware.
    QuadratureEncoder(int lpi) : lpi(lpi) { }

    // Configure the base class.  Subclasses should invoke this with their
    // class-specific key.  We'll read the generic subkeys:
    //
    //   channelA: <number>,       // GPIO port for channel A input from the sensor
    //   channelB: <number>,       // GPIO port for channel B input from the sensor
    //
    // Upon successful configuration, we'll call Init() and return true.  On
    // error, logs an error message and returns false.
    //
    // Recommended configuration coding pattern: each device-specific subclass
    // should provide a static Configure(JSONParser&) function that checks for
    // the presence of its device key.  If found, read any additional keys that
    // the device uses, then create an instance of the subclass, and call this
    // ConfigureBase() method.  If this returns true, make the object available
    // for use (by storing it in a global list of available objects or a global
    // singleton, as appropriate for the subclass).  If this returns false,
    // simply discard the instance, since configuration failed.
    //
    // The 'name' is a string to display in any log messages to identify the
    // specific device type.  This is typically the same as the JSON key for
    // the device, but it doesn't have to be if another name would be more
    // helpful for the user in logged errors.
    bool ConfigureBase(const char *name, const JSONParser::Value *key);

    // Get the current count
    int GetCount() const { return count; }

    // Get the current instantaneous channel A/B status.  Returns the
    // channel status encoded in the two low bits of an int (0x0001 for
    // channel A, 0x0002 for channel B).
    unsigned int GetChannelState() const { return state; }

    // Get the time of the last count change, as a tick count on the Pico's
    // system clock (microseconds since reset)
    uint64_t GetCountTime() const { return tCount; }

    // Reset the counter to zero
    void ZeroCounter();

    // Get the sensor's lines-per-inch metric
    int GetLPI() const { return lpi; }

protected:
    // Initialize - called internally upon successful configuration.
    // This sets up the interrupt handlers for the input pins.  Returns
    // true on success; on failure, logs an error and returns false.
    virtual bool Init();

    // Interrupt handler for the GPIO interrupt for the A and B
    // channel signals.  (The Pico only has a single hardware-level
    // IRQ for the GPIOs, so there's no point in using separate A and
    // B channel handler routines.)
    void IRQ();
    
    // State update - called from the interrupt handler
    inline void UpdateState()
    {
        // Read the new state.  Note that we directly read the ports on every
        // interrupt to be sure that we have the full new state.  This keeps
        // the state accurate after every interrupt even if we missed one or
        // more interrupts.
        unsigned int newState = (gpio_get(gpA) ? 0x01 : 0x00) | (gpio_get(gpB) ? 0x02 : 0x00);

        // State transition table - old state in high two bits, new state in low two bits
        //
        // Plus direction:  00 -> 01 -> 11 -> 10 -> wrap
        //
        // Other transitions (where two bits change at the same time) are invalid,
        // since adjacent states can only vary by one bit.
        //
        // Note that this is deliberately non-const, so that the linker places it
        // in RAM, for fastest access from the interrupt handler.
        static int delta[] = {
            0,     // 00 -> 00  (No change)
            1,     // 00 -> 01
            -1,    // 00 -> 10
            0,     // 00 -> 11  (Invalid)
            -1,    // 01 -> 00
            0,     // 01 -> 01  (No change)
            0,     // 01 -> 10  (Invalid)
            1,     // 01 -> 11
            1,     // 10 -> 00
            0,     // 10 -> 01  (Invalid)
            0,     // 10 -> 10  (No change)
            -1,    // 10 -> 11
            0,     // 11 -> 00  (Invalid)
            -1,    // 11 -> 01
            1,     // 11 -> 10
            0,     // 11 -> 11  (No change)
        };
        count += delta[(state << 2) | newState];
        state = newState;
    }

    // Lines-per-inch metric for the sensor.  This is the number of
    // black/white line pairs per inch on the encoder's bar scale.
    // For most encoders, this is a fixed feature of the device.
    int lpi;

    // GPIO connections for the two input channels
    int gpA = -1;
    int gpB = -1;

    // Current count.  This is incremented and decremented in the
    // interrupt handlers according to the quadrature state transition
    // detected in each interrupt.
    int32_t count = 0;

    // Current A/B state.  A is encoded in bit 0x0001, B in bit 0x0002.
    unsigned int state = 0;

    // last count change time
    uint64_t tCount = 0;
};
