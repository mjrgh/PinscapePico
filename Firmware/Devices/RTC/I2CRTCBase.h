// Pinscape Pico - Base class for I2C-based RTC implementations
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <list>

#include <pico/stdlib.h>
#include <hardware/i2c.h>

#include "Pinscape.h"
#include "Utils.h"
#include "I2C.h"
#include "TimeOfDay.h"

// forwards
class JSONParser;

class I2CRTCBase : public TimeOfDay::RTC, public I2CDevice
{
public:
    // configure from JSON data
    static void Configure(JSONParser &json);

    // construction
    I2CRTCBase(uint8_t i2cAddr) : I2CDevice(i2cAddr) { }

    // Basic TimeOfDay::RTC implementation common to most I2C RTCs
    virtual void Write(const DateTime &dt) override;
    virtual void ReadAsync(std::function<void(const DateTime&)> callback) override;

    // Basic I2CDevice implementation common to most I2C RTCs
    virtual void I2CReinitDevice(I2C *i2c) override { SendInitCommands(i2c->GetHwInst()); }
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;
    virtual bool OnI2CWriteComplete(I2CX *i2c) { return false; }
    virtual void OnI2CTimeout() { InvokeReadCallback(); }
    virtual void OnI2CAbort() { InvokeReadCallback(); }

protected:
    // Send initialization commands.  Must be defined per device.
    virtual void SendInitCommands(i2c_inst_t *i2c) = 0;

    // Decode timekeeping registers retrieved from the device into a DateTime struct
    virtual void DecodeDateTime(DateTime &dt, const uint8_t *data, size_t dataLen) = 0;

    // Initiate an I2C transaction for reading or writing a DateTime value.  The
    // read operation is asynchronous; this just performs the i2c->Read() necessary
    // to start the read.
    virtual void ReadDateTime(I2CX *i2c) = 0;
    virtual void WriteDateTime(I2CX *i2c, const DateTime &dt) = 0;

    // chip write/read pending
    bool writePending = false;
    bool readPending = false;

    // DateTime value for pending write
    DateTime dateTimeToWrite;

    // pending read callback
    std::function<void(const DateTime&)> readCallback = [](const DateTime&){ };

    // call the read callback, and clear it for future invocations
    void InvokeReadCallback() { DateTime dt; InvokeReadCallback(dt); }
    void InvokeReadCallback(const DateTime &dt) { readCallback(dt); readCallback = [](const DateTime&){ }; }
};
