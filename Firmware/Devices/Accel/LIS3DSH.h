// Pinscape Pico firmware - LIS3DSH Accelerometer chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the interface to an LIS3DSH accelereometer.  This is a 3-axis
// MEMS accelerometer that's popular with Arduino and robotics hobbyists.
// Adafruit makes an inexpensive breakout board for the sensor, which
// makes it fairly easy to integrate into a DIY project without designing
// or building any custom circuit boards.
//
// This device includes a temperature sensor, which we expose as a logical
// joystick axis named "lis3dsh.temperature".  You can assign this to a
// gamepad or XInput joystick axis in the JSON configuration like so:
//
// gamepad: {
//    x: "lis3dsh.temperature",
// }
//
// The center of the logical axis represents 25C, and the full scale of
// the axis is approximately 125C in each direction, so the range is about
// -100C to +150C.  This is the same as the range of values that the
// sensor can represent natively, although the full range is unlikely to
// be observed in practice, because the device is only rated to operate
// over -40C to +85C.

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


// LIS3SDH hardware interface
class LIS3DSH : public Accelerometer, public I2CDevice
{
public:
    LIS3DSH(uint16_t addr, int gpInterrupt);

    // Accelerometer interface
    virtual const char *GetConfigKey() const override { return "lis3dsh"; }
    virtual const char *GetFriendlyName() const override { return "LIS3DSH"; }
    virtual int GetSamplingRate() const override { return sampleRate; }
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) override;
    virtual void Task() override;
    virtual int GetGRange() const override { return gRange; }

    // singleton instance - created in Configure() if the device is configured
    static LIS3DSH *inst;

    // configure from JSON data
    static void Configure(JSONParser &json);

    // I2CDevice implementation
    virtual const char *I2CDeviceName() const override { return "LIS3SDH"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;
    virtual void OnI2CCompletionIRQ(const uint8_t *data, size_t len, I2CX *i2c) override;

    // Timing statistics
    struct
    {
        uint64_t nReads = 0;            // number of reads
        uint64_t tLastSample = 0;       // time of last read
        uint64_t timeBetweenReads = 0;  // cumulative time between reads

        uint64_t i2cLatency = 0;        // I2C polling latency: time between interrupt going low and I2C read started

        uint64_t tIntr = 0;             // clock time when interrupt line last went low, for precise sample-redy timestamping
        uint64_t nIntr = 0;             // number of interrupts serviced

        uint64_t tReadStarted = 0;      // time last I2C OUT register read was initiated

        void Reset()
        {
            nReads = 0;
            timeBetweenReads = 0;
            i2cLatency = 0;
            tLastSample = 0;
            tIntr = 0;
            nIntr = 0;
        }
    } stats;

protected:
    // initialize
    void Init(i2c_inst_t *i2c);

    // Send initialization commands, either after a power-on reset or for
    // an I2C bus reset operation.
    void SendInitCommands(i2c_inst_t *i2c, bool isPowerOn);

    // static task handler
    static void StaticTask() { inst->Task(); }

    // IRQ handler
    static void IRQ();

    // register number of I2C read in progress
    int readingRegister = -1;

    // Range in 'g' units (Earth's gravity).  Valid values are 2, 4, 8,
    // and 16.  We use a default of 2g, since the virtual pin cab
    // application benefit more from higher resolution (higher 'g'
    // settings trade off resolution for dynamic range).
    int gRange = 2;

    // interrupt GPIO
    int gpInterrupt = -1;

    // Sampling rate, in samples per second, and its inverse, in
    // microseconds per sample
    int sampleRate = 800;
    int sampleTime_us = 1250;

    // last acceleration sample, in native INT16 units (which is also the
    // normalized format we return from the device-indepdent interface)
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;

    // last _OUT register bytes, for registers 0x03 to 0x08 (XOUT to ZOUT)
    uint8_t outReg[6]{ 0 };

    // last temperature register byte, register 0x0C
    uint8_t tempReg[1]{ 0 };

    // last sample timestamp
    uint64_t timestamp = 0;

    // Last temperature reading, in several formats: the sensor's native
    // INT8 units, which represent the delta from 25 C in units of 1 C;
    // our own USB HID logical axis normalized INT16 units; and 1/100
    // degrees C.
    int8_t temperatureInt8 = 0;
    int16_t temperatureInt16 = 0;
    int temperatureC = 0;

    // timestamp of last temperature reading
    uint64_t temperatureTimestamp = 0;

    // console command interface
    static void Command_info(const ConsoleCommandContext *ctx);
};
