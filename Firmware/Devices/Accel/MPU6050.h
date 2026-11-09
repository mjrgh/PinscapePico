// Pinscape Pico firmware - MPU-6050 Accelerometer chip interface
// BSD-3-Clause license / NO WARRANTY
//
// Defines the interface to an InvenSense (TDK) MPU-6050.  This is a
// 6-axis motion sensor, combining a 3-axis accelerometer and a 3-axis
// gyroscope, with 16-bit resolution per axis and an I2C interface.  We
// only use the accelerometer portion, since that's all the nudge system
// needs.  The chip is widely available on inexpensive breakout boards,
// most commonly the "GY-521".
//
// The chip's I2C address is 0x68 or 0x69, selected by the AD0 pin
// (GND -> 0x68, VDD -> 0x69).  Most breakout boards pull AD0 low, so
// 0x68 is the usual address.
//
// Many breakout boards sold as MPU-6050 actually carry a compatible
// part or a clone (MPU-6500, MPU-9250, and various unbranded chips).
// Those report a different WHO_AM_I value, but share the register
// layout we use, so the driver logs a warning and carries on.

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


// MPU-6050 hardware interface
class MPU6050 : public Accelerometer, public I2CDevice
{
public:
    MPU6050(uint16_t addr, int gpInterrupt);

    // Accelerometer interface
    virtual const char *GetConfigKey() const override { return "mpu6050"; }
    virtual const char *GetFriendlyName() const override { return "MPU-6050"; }
    virtual int GetSamplingRate() const override { return 1000; }
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) override;
    virtual void Task() override;
    virtual int GetGRange() const override { return gRange; }

    // singleton instance - created in Configure() if the device is configured
    static MPU6050 *inst;

    // configure from JSON data
    static void Configure(JSONParser &json);

    // I2CDevice implementation
    virtual const char *I2CDeviceName() const override { return "MPU6050"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

    // Get the latest temperature reading, in units of 1/1000 of a degree
    // C (e.g., 25.1C is represented as 25100).  The chip measures the
    // die temperature along with each sample, so it's free to include.
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
        uint64_t tIntr = 0;             // clock time when interrupt line last went low, for precise sample-ready timestamping
        uint64_t nIntr = 0;             // number of interrupts serviced
        uint64_t nMissedSamples = 0;    // number of missed samples
    } stats;

protected:
    // initialize
    void Init(i2c_inst_t *i2c);
    void SendInitCommands(i2c_inst_t *i2c);

    // IRQ handler
    static void IRQ();

    // Range in 'g' units (Earth's gravity).  Valid values are 2, 4, 8,
    // and 16.  We use a default of 2g, since the virtual pin cab
    // application benefits more from higher resolution (higher 'g'
    // settings trade off resolution for dynamic range).
    int gRange = 2;

    // interrupt GPIO
    int gpInterrupt = -1;

    // WHO_AM_I register value read during initialization
    uint8_t whoAmI = 0;

    // last acceleration sample, in native INT16 units (which is also the
    // normalized format we return from the device-independent interface)
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;

    // last *OUT register bytes, for registers 0x3B to 0x40 (ACCEL_XOUT_H to ACCEL_ZOUT_L)
    uint8_t outReg[6]{ 0 };

    // last sample timestamp
    uint64_t timestamp = 0;

    // last raw temperature reading (TEMP_OUT register, INT16); this is
    // also what we report through the "mpu6050.temperature" logical axis
    int16_t temperatureRaw = 0;

    // last temperature reading from the chip, in units of 1/1000 of a
    // degree C (e.g., 25.1 C is represented as 25100)
    int temperature = 0;

    // console command interface
    static void Command_info(const ConsoleCommandContext *ctx);
};
