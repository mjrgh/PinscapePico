// Pinscape Pico - DS3231M Real-time clock/calendar chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Handles the DS3231M RTC family (DS3231M+, DS3231MZ+, DS3231MZ/V+)

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
#include "I2CRTCBase.h"

// forwards
class JSONParser;

class DS3231M : public I2CRTCBase
{
public:
    // configure from JSON data
    static void Configure(JSONParser &json);

    // construction - this device uses a fixed I2C address of 0x68 (7-bit)
    DS3231M() : I2CRTCBase(0x68) { }

    // TimeOfDay::RTC implementation
    virtual const char *DisplayName() const { return "DS3231M"; }

    // I2CDevice overrides
    virtual const char *I2CDeviceName() const override { return "DS3231M"; }

protected:
    // global singleton instance
    static DS3231M *inst;

    // send initialization commands
    virtual void SendInitCommands(i2c_inst_t *i2c) override;

    // Decode timekeeping registers retrieved from the device into a DateTime struct
    virtual void DecodeDateTime(DateTime &dt, const uint8_t *buf, size_t bufLen) override;

    // Start a read/write I2C transaction for the timekeeping registers
    virtual void ReadDateTime(I2CX *i2c) override;
    virtual void WriteDateTime(I2CX *i2c, const DateTime &dt) override;
};
