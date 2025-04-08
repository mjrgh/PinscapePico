// Pinscape Pico - DS1307 Real-time clock/calendar chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The DS1307 uses a fixed I2C address of 0x68, which also happens to be the
// default power-on All-Call address for the TLC59116 family, which is a
// foundational device in the Pinscape Pico ecosystem.  The All-Call address
// is a broadcast address that all TLC59116's on the bus will acknowledge,
// and is designed for sending simultaneous LED updates to all instances of
// the chip.  Fortunately, the All-Call feature can be disabled, but that
// has to be programmed explicitly on every reset, since it's enabled by
// default at power-on.

#include <stdlib.h>
#include <stdint.h>
#include <list>

#include <pico/stdlib.h>
#include <hardware/i2c.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "I2C.h"
#include "TimeOfDay.h"
#include "Devices/PWM/TLC59116.h"
#include "DS1307.h"


// singleton instance
DS1307 *DS1307::inst;

// JSON configuration
void DS1307::Configure(JSONParser &json)
{
    if (auto *val = json.Get("ds1307"); !val->IsUndefined())
    {
        // get and validate the I2C bus number
        int bus = val->Get("i2c")->Int(-1);
        if (!I2C::ValidateBusConfig("ds3107", bus))
            return;

        // create the singleton instance and add it to the I2C bus
        inst = new DS1307();
        I2C::GetInstance(bus, false)->Add(inst);

        // Disable All-Call on any TLC59116 chips on this bus, since
        // All-Call uses the same bus address (0x68) as the DS1307.
        TLC59116::DisableAllCall(bus);

        // add it to the time-of-day module's RTC list
        timeOfDay.AddRTC(inst);

        // send initialization commands
        inst->SendInitCommands(i2c_get_instance(bus));
        
        // success
        Log(LOG_CONFIG, "DS1307 clock/calendar chip configured\n");
    }
}

void DS1307::ReadDateTime(I2CX *i2c)
{
    // Set up a read transaction for the timekeeping registers,
    // 0x00-0x06.  This chip provides coherent reading of the
    // timekeeping registers.
    uint8_t buf[]{ 0x00 };
    i2c->Read(buf, 1, 7);
}

void DS1307::WriteDateTime(I2CX *i2c, const DateTime &dt)
{
    // Prepare a write transaction for the timekeeping registers, 0x00-0x06.
    // This chip guarantees coherency on write as long as the write occurs
    // within 1 second (because coherency is guaranteed by the chip resetting
    // its internal fractional second counter when writing to register 0x00).
    // Doing the write as a batch takes about 200us, well within the 1s limit.
    uint8_t centuryBit = (dt.yyyy >= 2000 ? 0x80 : 0x00);
    uint8_t buf[8]{
        0x00,                           // starting register address
        IntToBCD(dt.ss),                // reg 0x00 = seconds in BCD + Clock Hold in bit 0x80 (0 -> clock running)
        IntToBCD(dt.mm),                // reg 0x01 = minutes in BCD
        IntToBCD(dt.hh),                // reg 0x02 = 24-hour clock in BCD + 12/24 hour mode in bit 0x40 (0 -> 24-hour mode)
        static_cast<uint8_t>(dt.GetWeekDay() + 1),  // reg 0x03 = weekday - the chip stores values 1-7, so adjust from our 0-6 representation
                                        // (the chip doesn't define any mapping from weekday number to/from label - that's up to us)
        IntToBCD(dt.dd),                // reg 0x04 = day of month, BCD
        static_cast<uint8_t>(IntToBCD(dt.mon)),  // reg 0x05 = month, BCD
        IntToBCD(dt.yyyy % 100),        // reg 0x06 = last two digits of year, BCD
    };

    // queue the write
    i2c->Write(buf, _countof(buf));
}

void DS1307::SendInitCommands(i2c_inst_t *i2c)
{
    // no errors so far
    bool ok = true;

    // Read the current timekeeping registers to determine if the time has been set.
    // On first power-up, the date reads 00:00:00 day 0x01 01/01/00, with the Clock
    // Hold bit (bit 0x80 in register 0x00) set to '1'.
    const static uint8_t powerUpPat[7]{ 0x80, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 };
    DateTime dt;
    bool isTimeValid = false;
    {
        uint8_t tx = 0x00;
        uint8_t rx[7];
        if (i2c_write_timeout_us(i2c, i2cAddr, &tx, 1, true, 1000) == 1
            && i2c_read_timeout_us(i2c, i2cAddr, rx, _countof(rx), false, 1000) == _countof(rx))
        {
            // Check the for power-up pattern - if it matches, the time hasn't been set
            isTimeValid = memcmp(rx, powerUpPat, sizeof(rx)) != 0;

            // if the time is valid, pass it to the time-of-day module
            if (isTimeValid)
            {
                // decode it
                DecodeDateTime(dt, rx, sizeof(rx));

                // set the current time
                timeOfDay.SetTime(dt, false);
            }
        }
        else
        {
            ok = false;
            Log(LOG_DEBUG, "DS1307: Timekeeping registers read failed\n");
        }
    }

    // log status
    if (ok)
    {
        if (isTimeValid)
        {
            Log(LOG_INFO, "DS1307 initialization OK; date read from chip is %02d %s %04d %02d:%02d:%02d\n",
                dt.dd, dt.MonthNameShort(), dt.yyyy, dt.hh, dt.mm, dt.ss);
        }
        else
        {
            Log(LOG_INFO, "DS1307 initialization commands OK");
            Log(LOG_WARNING, "DS1307 has been power cycled and does not have a valid date stored.  Check battery if "
                "this is unexpected.\n");
        }
    }
    else
        Log(LOG_ERROR, "DS1307 initialization commands failed (I2C error)\n");
}

// Decode a DateTime value from the DS1307 timekeeping register set, registers 0x00-0x06
void DS1307::DecodeDateTime(DateTime &dt, const uint8_t *buf, size_t bufLen)
{
    // the expected timekeeping register length is 7 bytes; if wrong, return an empty date/time
    if (bufLen != 7)
    {
        dt = DateTime();
        return;
    }

    // The chip only stores the last two digits of the year, so we just
    // have to assume we're in the 21st century.  We know for a fact it's
    // not earlier than the 21st century, since this software didn't exist
    // until 2024, and 2100 is beyond the expected service life.
    int16_t year = 2000 + BCDToInt(buf[6]);

    // the month is in byte 5 bits 4-0
    uint8_t month = BCDToInt(buf[5] & 0x1F);
    
    // the day of the month is in BDC byte 4 bits 5-0
    uint8_t day = BCDToInt(buf[4] & 0x3F);
    
    // The hour can be stored in 12-hour or 24-hour notation, determined
    // by bit 0x40 of register 0x02.
    uint8_t hour;
    if ((buf[2] & 0x40) != 0)
    {
        // 12-hour mode: bit 5 is AM/PM, hour is in bits 4-0 as BCD.
        // Change 12:00 to 00:00, then add 12:00 if it's PM.
        hour = BCDToInt(buf[2] & 0x1F);
        if (hour == 12)
            hour = 0;
        if ((buf[2] & 0x20) != 0)
            hour += 12;
    }
    else
    {
        // 24-hour mode - the hour is in bits 5-0 as BCD, already
        // in the correct range 0-23
        hour = BCDToInt(buf[2] & 0x3F);
    }
    
    // minute is byte 1 bits 6-0, BCD; second is byte 0 bits 6-0, BCD
    uint8_t minute = BCDToInt(buf[1] & 0x7F);
    uint8_t second = BCDToInt(buf[0] & 0x7F);

    // populate the DateTime object
    dt = DateTime(year, month, day, hour, minute, second);
}
