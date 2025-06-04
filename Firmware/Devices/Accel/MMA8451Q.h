// Pinscape Pico firmware - MMA8451Q Accelerometer chip interface
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the interface to an MMA8451Q accelerometer.  This is the 3-axis
// MEMS accelerometer that was built into the venerable FRDM-KL25Z development
// board, which was the hardware platform for the original Pinscape Controller.
// It makes an excellent virtual pin cab nudge device thanks to its high
// resolution (14 bits) and low noise levels (99 ug/sqrt(Hz), per the data
// sheet, less than half the claimed noise level of LIS3DH).
//
// The MMA8451Q is no longer in production, but old stock is still available
// as of this writing.  In particular, Adafruit sells an MMA8451Q breakout
// board that's ideal for a DIY project with the Pico, since the board comes
// fully assembled, except for a standard 0.1" header strip.  The header
// strip is the only soldering required, and is quite easy to do by hand.


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


// MMA8451Q hardware interface
class MMA8451Q : public Accelerometer, public I2CDevice
{
public:
    MMA8451Q(uint16_t addr, int gpInterrupt);

    // Accelerometer interface
    virtual const char *GetConfigKey() const override { return "mma8451q"; }
    virtual const char *GetFriendlyName() const override { return "MMA8451Q"; }
    virtual int GetSamplingRate() const override { return sampleRate; }
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) override;
    virtual void Task() override;
    virtual int GetGRange() const override { return gRange; }

    // singleton instance - created in Configure() if the device is configured
    static MMA8451Q *inst;

    // configure from JSON data
    static void Configure(JSONParser &json);

    // I2CDevice implementation
    virtual const char *I2CDeviceName() const override { return "MMA8451Q"; }
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

    // Range in 'g' units (Earth's gravity).  Valid values are 2, 4,
    // and 8.  We use a default of 2g, since the virtual pin cab
    // application benefit more from higher resolution (higher 'g'
    // settings trade off resolution for dynamic range).
    int gRange = 2;

    // interrupt GPIO
    int gpInterrupt = -1;

    // Sampling rate, in samples per second, and its inverse, in
    // microseconds per sample
    int sampleRate = 800;
    int sampleTime_us = 1250;

    // last acceleration sample, in normalized (device-independent) INT16 units
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;

    // last acceleration sample, in device-native INT12 units (-2048..+2047)
    int16_t xRaw = 0;
    int16_t yRaw = 0;
    int16_t zRaw = 0;

    // last _OUT register bytes, for registers 0x03 to 0x08 (XOUT to ZOUT)
    uint8_t outReg[6]{ 0 };

    // last temperature register bytes, registers 0x0C to 0x0D
    uint8_t tempReg[2]{ 0 };

    // last sample timestamp
    uint64_t timestamp = 0;

    // console command interface
    static void Command_info(const ConsoleCommandContext *ctx);
};

