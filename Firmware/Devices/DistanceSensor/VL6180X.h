// Pinscape Pico - VL6180X time-of-flight distance sensor
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The VL6180X is a self-contained IR distance sensor with an I2C bus
// interface.  The device measures the distance to a nearby reflective
// object (within about 100mm of the sensor) by sending out pulses of IR
// light from an on-chip emitter, and measuring the time it takes for
// the reflected signal to arrive at the on-chip receiver.  This is
// referred to as "time-of-flight" sensing because it measures the
// round-trip "flight" time of the infrared pulses.
//
// The device reports its distance readings in millimeters, which makes
// it straightforward to program for the primary Pinscape use case of
// plunger position sensing.  It's also inexpensive and quite easy to
// set up in a virtual pin cab; you just mount it in front of the
// plunger and run five wires (power, ground, SDA, SCL, interrupt) to
// the Pico.
//
// It's a very nice little sensor in principle, but it has a big
// drawback: the masurement quality isn't good enough for this
// application.  It's not accurate enough, precise enough, or fast
// enough.  Its nominal precision is whole millimeters, which is rather
// coarse for plunger position sensing - a high-def or 4K video display
// sized for a pin cab has a dot pitch of 100 to 200 DPI, so the
// on-screen animation has to be at least that good to look smooth.
// That means the sensor has to be able to resolve to better than 0.1mm.
// To make matters worse, the VL6180X's actual accuracy isn't nearly as
// good as its nominal precision; it's really only accurate to about +/-
// 5 mm from the actual position at any given time.  If you don't want
// to see the on-screen animation jumping around wildly all the time,
// you have to crank the jitter-filter (hystersis) window up to about
// 10mm, which makes the on-screen animation extremely coarse when you
// move the plunger.  The sensor is also very slow by our standards, at
// about 16mm for the fastest sampling rate; for accurate plunger speed
// measurements during a pull-and-release gesture, we need readings at
// about 2.5 ms intervals or better.  I'm implementing this chip more
// for demonstration and experimentation purposes than for serious use
// in pin cab depoyments.
//

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <functional>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "I2C.h"

// forward/external declarations
class VL6180X;
class JSONParser;
class ConsoleCommandContext;

// Global singleton instance.  We only allow one instance of this chip
// in the system, since there's no Pinscape use case where multiple
// chips would be required.
extern VL6180X *vl6180x;

// VL6180X driver
class VL6180X : public I2CDevice
{
public:
    // construction
    VL6180X(int gpCE, int gpInt);

    // configure
    static void Configure(JSONParser &json);

    // Is a new reading ready?  Returns true if a new proximity sample is
    // available since the last Read() call.
    bool IsSampleReady() const { return isSampleReady; }

    // Read a distance value.  The sensor reports readings calibrated in
    // millimeters from the emitter/reciever face of the device.
    // Returns true if a new value was ready since the last Read() call;
    // if no new value is ready, reports the same value as on the last
    // call and returns false;
    bool Read(uint32_t &millimeters, uint64_t &timestamp);

    // I2C device interface
    virtual const char *I2CDeviceName() const override { return "VL6180X"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

protected:
    // initialize
    void Init(int i2c);
    void SendInitCommands(i2c_inst_t *i2c);

    // interrupt handler
    static void IRQ();

    // I2C address - the VL6180X has a fixed address ox 0x29
    static const int i2cAddr = 0x29;

    // GPIO port connected to sensor Chip Enable input (GPIO/CE); -1 if not connected
    int gpCE = -1;

    // GPIO port connected to sensor interrupt output (GPIO1/INT); -1 if not connected
    int gpInt = -1;

    // last reading and timestamp
    uint32_t sample = 0;
    uint64_t tSample = 0;

    // do we have a new sample since the last Read()?
    bool isSampleReady = false;

    // time of last sample-ready interrupt
    uint64_t tInterrupt = 0;

    // result error count - we increment this whenever we poll the status register
    // and find an error status bit set
    uint64_t resultErrorCount = 0;

    // sample read count
    uint64_t sampleReadCount = 0;

    // starting time of continuous sampling mode
    uint64_t continuousModeStartTime = 0;

    // read mode
    enum class ReadMode
    {
        None,           // no read pending
        Value,          // reading value register
        Status,         // reading status register
    };
    ReadMode readMode = ReadMode::None;

    // diagnostic command handler
    void Command_main(const ConsoleCommandContext *c);
};
