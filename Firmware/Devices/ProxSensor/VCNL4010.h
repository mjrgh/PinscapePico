// Pinscape Pico - VCNL4010, VCNL4020, VCNL3020 IR proximity sensor interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The Vishay VCNL4010 is an IR proximity sensor that detects the presence of
// nearby objects by measuring the intensity of reflected IR light pulses.
// The device is primarily a proximity sensor, meaning that it detects the
// *presence* of a nearby reflecting object, as a binary proposition, present
// or not present.  It also has the ability to measure the intensity of the
// reflected IR signal as an analog quantity.  The intensity of the reflected
// light varies with the distance to the object according to a power law, so
// the analog intensity measurement can be used to estimate the distance
// relative to pre-measured calibration points.
//
// This code was developed and tested against the VCNL4010, but it should also
// work with VCNL4020 (a newer iteration of the VCNL4010) and VCNL3020 (which
// is a 4020 minus the ambient light sensor feature).  All of these chips use
// the same I2C address and register set, so they should work with this driver
// without any software changes.  However, note that the PCB footprints of the
// '10 and '20 iterations are different, so they're not interchangeable at the
// hardware level.

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
class VCNL4010;
class JSONParser;
class ConsoleCommandContext;

// Global singleton instance.  We only allow one instance of this chip in the
// system, since there's no Pinscape use case where multiple chips would be
// required.  (The chip even has "singleton" practically baked into its
// hardware deisgn, in that it has a fixed I2C address, preventing multiple
// instances from sharing an I2C bus.  You could still have multiple instances
// by placing each on its own hardware bus, but for Pinscape purposes I don't
// think there's any need to worry about that possibility.)
extern VCNL4010 *vcnl4010;


// VCNL4010/VCNL4020/VCNL3020 driver
class VCNL4010 : public I2CDevice
{
public:
    // Construction
    //
    // iredCurrent is the IR emitter current setting, 10mA to 200mA in 10mA
    // increments
    //
    // gpInt is the GPIO port number connected to the chip's interrupt line,
    // or -1 if it's not connected
    VCNL4010(int iredCurrent, int gpInt);

    // configure
    static void Configure(JSONParser &json);

    // Is a new reading ready?  Returns true if a new proximity sample is
    // available since the last Read() call.
    bool IsSampleReady() const { return isSampleReady; }

    // Read a proximity count value.  Returns true if this is a new value
    // since the last value read, false if not.  For a false return, the value
    // passed back is the same as the previous value read.
    //
    // The proximity count is the raw 16-bit unsigned value reported by the
    // sensor, which represents the intensity of the reflected signal detected
    // at the receiver.
    bool Read(uint16_t &proxCount, uint64_t &timestamp);

    // I2C device interface
    virtual const char *I2CDeviceName() const override { return "VCNL4010"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

protected:
    // VCNL4010/3020/4020 all use a fixed I2C address, 0x13 (7-bit notation)
    static const int i2cAddr = 0x13;

    // initialize the chip (called after successful configuration); i2c
    // is the I2C bus number (0 or 1)
    void Init(int i2c);
    void SendInitCommands(i2c_inst_t *i2c);

    // interrupt handler
    static void IRQ();

    // IR LED current setting (from configuration)
    int iredCurrent = 0;

    // GPIO connected to sensor INT (interrupt) pin; -1 if not connected
    int gpInt = -1;

    // use polling even if interrupt pin is configured - for debugging
    bool forcePolling = false;

    // time we retrieved the last sample
    uint64_t tLastSample = 0;

    // Estimated time of next sample.  When we're in polling mode, we use this
    // to wait to poll the device until enough time has elapsed for a sample to
    // be ready.  The device has a fairly predictable time between samples, so
    // this lets us minimize unnecessary bus traffic.
    uint64_t tNextSampleEst = 0;

    // time of last Sample Ready interrupt (used only if the interrupt GPIO is configured)
    uint64_t tInterrupt = 0;

    // last raw proximity reading
    uint16_t lastProxCount = 0;

    // flag: a new sample is available since last Read() call
    bool isSampleReady = false;

    // I2C read state - this tracks which register we're expecting to get on
    // read completion
    uint8_t readReg = 0x00;
};
