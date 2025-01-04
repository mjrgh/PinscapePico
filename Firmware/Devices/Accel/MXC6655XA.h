// Pinscape Pico firmware - MXC6655XA Accelerometer chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the interface to an MXC6655XA accelereometer.  This is a
// 3-axis device with 12-bit resolution per axis, interfacing to the
// host CPU via I2C.  The chip automatically takes readings every 10ms
// (100 Hz, per the data sheet).
//
// This device includes a temperature sensor, which we expose as a
// logical joystick axis named "mxc6655xa.temperature".  You can assign
// this to a gamepad or XInput joystick axis in the JSON configuration
// like so:
//
// gamepad: {
//    x: "mxc6655xa.temperature",
// }
//
// The center of the logical axis represents 25C, and the full scale of
// the axis is 75C in each direction, so the range is -50C to +100C.
// This is the range of values that the sensor can represent natively,
// although the full range is unlikely to be observed in practice,
// because the device is only rated to operate over -40C to +85C.

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


// MXC6655XA hardware interface
class MXC6655XA : public Accelerometer, public I2CDevice
{
public:
    MXC6655XA(uint16_t addr, int gpInterrupt);

    // Accelerometer interface
    virtual const char *GetConfigKey() const override { return "mxc6655xa"; }
    virtual const char *GetFriendlyName() const override { return "MXC6655XA"; }
    virtual int GetSamplingRate() const override { return 100; }
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) override;
    virtual void Task() override;
    virtual int GetGRange() const override { return gRange; }

    // singleton instance - created in Configure() if the device is configured
    static MXC6655XA *inst;

    // configure from JSON data
    static void Configure(JSONParser &json);

    // I2CDevice implementation
    virtual const char *I2CDeviceName() const override { return "MXC6655XA"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

    // Get the latest temperature reading, in units of 1/1000 of a degree
    // C (e.g., 25.1C is represented as 25100).  This is an extra
    // feature of the chip that we don't particularly need for a virtual
    // pinball setup, but it's practically free to include it since the
    // chip always measures it.  The chip samples the temperature every
    // 10ms (the same interval as the acceleration data).
    int GetTemperature() const { return temperature; }

    // translate the temperature to degrees C and F
    float GetTemperatureC() const { return static_cast<float>(temperature)/1000.0f; }
    float GetTemperatureF() const { return GetTemperatureC() * 9.0f/5.0f + 32.0f; }

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

    // interrupt handler
    static void IRQ();

    // Range in 'g' units (Earth's gravity).  Valid values are 2, 4, 8,
    // and 16.  We use a default of 2g, since the virtual pin cab
    // application benefit more from higher resolution (higher 'g'
    // settings trade off resolution for dynamic range).
    int gRange = 2;

    // interrupt GPIO
    int gpInterrupt = -1;

    // last acceleration sample, in normalized INT16 units (-32768..+32767)
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;

    // last acceleration sample, in native INT12 units (-2048..+2047)
    int16_t xRaw = 0;
    int16_t yRaw = 0;
    int16_t zRaw = 0;

    // last *OUT register bytes, for registers 0x03 to 0x08 (XOUT to ZOUT)
    uint8_t outReg[6]{ 0 };

    // last sample timestamp
    uint64_t timestamp = 0;

    // Last temperature reading from the chip, in normalized INT16
    // units.  The chip represents the temperature natively as an INT8,
    // with zero representing 25C and a possible range from -50C to
    // +97C.  This simply rescales that unit system to an INT16, for use
    // in logical HID joystick axis reports.
    int16_t temperatureInt16;

    // last temperature reading from the chip, in units of 1/100 of a
    // degree C (e.g., 25.1 C is represented as 25100)
    int temperature;

    // console command interface
    static void Command_info(const ConsoleCommandContext *ctx);
};
