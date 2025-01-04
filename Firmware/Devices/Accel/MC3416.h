// Pinscape Pico firmware - MC3416 Accelerometer chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the interface to an MC3416 accelereometer.  This is a
// 3-axis device with 16-bit resolution per axis and up to 1KHz sampling
// rate, with an I2C interface.
//
// In my prototype testing, the device has good X/Y calibration, but the
// Z axis was wildly out of true, by about 30 degrees.  I don't know if
// this is a defect in my sample part, damage it sustained during
// soldering, or just a quirk of the device.  The Z axis isn't important
// for virtual pin cab use, but the gigantic discrepancy makes me pretty
// hesitant to recommend the part.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "I2C.h"
#include "Accel.h"

// foward/external declarations
class JSONParser;
class ConsoleCommandContext;


// MC3416 hardware interface
class MC3416 : public Accelerometer, public I2CDevice
{
public:
    MC3416(uint16_t addr, int gpInterrupt);

    // Accelerometer interface
    virtual const char *GetConfigKey() const override { return "mc3416"; }
    virtual const char *GetFriendlyName() const override { return "MC3416"; }
    virtual int GetSamplingRate() const override { return 1000; }
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) override;
    virtual void Task() override;
    virtual int GetGRange() const override { return gRange; }

    // singleton instance - created in Configure() if the device is configured
    static MC3416 *inst;

    // configure from JSON data
    static void Configure(JSONParser &json);

    // I2CDevice implementation
    virtual const char *I2CDeviceName() const override { return "MC3416"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

    // Timing statistics
    struct
    {
        uint64_t nReads = 0;            // number of reads
        uint64_t timeBetweenReads = 0;  // cumulative time between reads
        uint64_t i2cLatency = 0;        // I2C polling latency: time between interrupt going low and I2C read started

        uint64_t tRead = 0;             // time of last read
        uint64_t tIntr = 0;             // clock time when interrupt line last went low, for precise sample-redy timestamping
        uint64_t nIntr = 0;             // number of interrupts serviced
        uint64_t nMissedSamples = 0;    // number of missed samples
    } stats;

protected:
    // initialize
    void Init(i2c_inst_t *i2c);
    void SendInitCommands(i2c_inst_t *i2c);

    // static task handler
    static void StaticTask() { inst->Task(); }

    // IRQ handler
    static void IRQ();

    // Range in 'g' units (Earth's gravity).  Valid values are 2, 4, 8,
    // 12, and 16.  We use a default of 2g, since the virtual pin cab
    // application benefit more from higher resolution (higher 'g'
    // settings trade off resolution for dynamic range).
    int gRange = 2;

    // interrupt GPIO
    int gpInterrupt = -1;

    // last acceleration sample, in native INT16 units (which is also the
    // normalized format we return from the device-indepdent interface)
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;

    // last *OUT register bytes, for registers 0x03 to 0x08 (XOUT to ZOUT)
    uint8_t outReg[6]{ 0 };

    // last sample timestamp
    uint64_t timestamp = 0;

    // last temperature reading from the chip, in units of 1/100 of a
    // degree C (e.g., 25.1 C is represented as 25100)
    int temperature;

    // console command interface
    static void Command_info(const ConsoleCommandContext *ctx);
};
