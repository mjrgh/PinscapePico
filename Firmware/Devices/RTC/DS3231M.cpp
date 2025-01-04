// Pinscape Pico - DS3231M Real-time clock/calendar chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The DS3231M uses a fixed I2C address of 0x68, which also happens to be the
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
#include "DS3231M.h"


// singleton instance
DS3231M *DS3231M::inst;

// JSON configuration
void DS3231M::Configure(JSONParser &json)
{
    if (auto *val = json.Get("ds3231m"); !val->IsUndefined())
    {
        // get the I2C bus number
        uint8_t bus = val->Get("i2c")->UInt8(255);
        if (I2C::GetInstance(bus, true) == nullptr)
        {
            Log(LOG_ERROR, "ds3231m: invalid/undefined I2C bus; must be 0 (I2C0) or 1 (I2C1)\n");
            return;
        }

        // create the singleton instance and add it to the I2C bus
        inst = new DS3231M();
        I2C::GetInstance(bus, false)->Add(inst);

        // Disable All-Call on any TLC59116 chips on this bus, since the
        // default All-Call address is 0x68, the same as the DS3231M's
        // fixed address.
        TLC59116::DisableAllCall(bus);

        // add it to the time-of-day module's RTC list
        timeOfDay.AddRTC(inst);

        // send initialization commands
        inst->SendInitCommands(i2c_get_instance(bus));
        
        // success
        Log(LOG_CONFIG, "DS3231M clock/calendar chip configured\n");
    }
}

void DS3231M::ReadDateTime(I2CX *i2c)
{
    // Set up a read transaction for the timekeeping registers, 0x00-0x06.
    // Note that this chip provides guaranteed coherent read of the timekeeping
    // registers.  That is, the chip snapshots all of the registers at the
    // moment we read the first register, ensuring that the whole set
    // represents a single point in time.  With chips that lack a coherent
    // read guarantee, it would be necessary to read the registers twice to
    // ensure that the seconds place didn't roll over in the middle of the
    // first read.
    uint8_t buf[]{ 0x00 };
    i2c->Read(buf, 1, 7);
}

void DS3231M::WriteDateTime(I2CX *i2c, const DateTime &dt)
{
    // Prepare a write transaction for the timekeeping registers, 0x00-0x06.
    // This chip provides guaranteed coherency on write, as long as we write
    // the registers within a short interval.
    uint8_t centuryBit = (dt.yyyy >= 2000 ? 0x80 : 0x00);
    uint8_t buf[8]{
        0x00,                           // starting register address
        IntToBCD(dt.ss),                // reg 0x00 = seconds in BCD
        IntToBCD(dt.mm),                // reg 0x01 = minutes in BCD
        IntToBCD(dt.hh),                // reg 0x02 = 24-hour clock in BCD
        static_cast<uint8_t>(dt.GetWeekDay() + 1),  // reg 0x03 = weekday - the chip stores values 1-7, so adjust from our 0-6 representation
                                        // (the chip doesn't define any mapping from weekday number to/from label - that's up to us)
        IntToBCD(dt.dd),                // reg 0x04 = day of month, BCD
        static_cast<uint8_t>(IntToBCD(dt.mon) | centuryBit), // reg 0x05 = month, BCD + century bit
        IntToBCD(dt.yyyy % 100),        // reg 0x06 = last two digits of year in BCD
    };

    // queue the write
    i2c->Write(buf, _countof(buf));
}

void DS3231M::SendInitCommands(i2c_inst_t *i2c)
{
    // no errors so far
    bool ok = true;

    // Set the control register (0x0E)
    // EOSC off, BBSQW off, CONV off, INTCN on, A2IE off, A1IE off
    {
        const uint8_t buf[]{ 0x0E, 0x1C };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            ok = false;
            Log(LOG_DEBUG, "DS3231M: Control register write failed\n");
        }
    }

    // Read the status register (0x0F) to determine if the time registers are valid.
    // Bit 0x80 (OSF) is on after a power outage with no backup, or after any other
    // interruption to the timekeeping function.
    bool isTimeValid = false;
    {
        // read register 0x0F
        const uint8_t tx[]{ 0x0F };
        uint8_t rx[1]{ 0x00 };
        if (i2c_write_timeout_us(i2c, i2cAddr, tx, _countof(tx), true, 1000) == _countof(tx)
            && i2c_read_timeout_us(i2c, i2cAddr, rx, _countof(rx), false, 1000) == _countof(rx))
        {
            // Check the OSF bit (0x80) - the time is valid if the bit is clear, since
            // a '1' in the bit means that the oscillator was interrupted.
            isTimeValid = (rx[0] & 0x80) == 0;
        }
        else
        {
            ok = false;
            Log(LOG_DEBUG, "DS3231M: Status register read failed\n");
        }
    }

    // Write the Status register (0x0F): Clear the OSF bit, clear EN32KHZ, others N/A
    {
        const uint8_t buf[]{ 0x0F, 0x00 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            ok = false;
            Log(LOG_DEBUG, "DS3231M: Status register write failed\n");
        }
    }

    // if the time is valid, read the current timekeeping registers (registers 0x00-0x06)
    // to get the stored date and time
    DateTime dt;
    if (isTimeValid)
    {
        const uint8_t tx[]{ 0x00 };
        uint8_t rx[7];
        if (i2c_write_timeout_us(i2c, i2cAddr, tx, _countof(tx), true, 1000) == _countof(tx)
            && i2c_read_timeout_us(i2c, i2cAddr, rx, _countof(rx), false, 1000) == _countof(rx))
        {
            // success - decode the date/time
            DecodeDateTime(dt, rx, sizeof(rx));

            // set the time in the time-of-day module (but don't update RTCs, as
            // this is coming from an RTC in the first place)
            timeOfDay.SetTime(dt, false);
        }
        else
        {
            ok = false;
            Log(LOG_DEBUG, "DS3231M: Timekeeping registers read failed\n");
        }
    }   

    // log status
    if (ok)
    {
        if (isTimeValid)
        {
            Log(LOG_INFO, "DS3231M initialization commands OK; date read from chip is %02d %s %04d %02d:%02d:%02d\n",
                dt.dd, dt.MonthNameShort(), dt.yyyy, dt.hh, dt.mm, dt.ss);
        }
        else
        {
            Log(LOG_INFO, "DS3231M initialization commands OK");
            Log(LOG_WARNING, "DS3231M has been power cycled and does not have a valid date stored.  Check battery if "
                "this is unexpected.\n");
        }
    }
    else
        Log(LOG_ERROR, "DS3231M initialization commands failed (I2C error)\n");
}

// Decode a DateTime value from the DS3231M timekeeping register set, registers 0x00-0x06
void DS3231M::DecodeDateTime(DateTime &dt, const uint8_t *buf, size_t bufLen)
{
    // the expected timekeeping register length is 7 bytes; if wrong, return an empty date/time
    if (bufLen != 7)
    {
        dt = DateTime();
        return;
    }
    
    // the year consists of a century bit in buf[5] bit 0x80 (0=1900, 1=2000) and a
    // two-digit BCD year value in buf[6]
    int16_t year = ((buf[5] & 0x80) != 0 ? 2000 : 1900) + BCDToInt(buf[6]);

    // the month is in byte 5 bits 4-0
    uint8_t month = BCDToInt(buf[5] & 0x1F);
    
    // the day of the month is in BDC byte 4 bits 5-0
    uint8_t day = BCDToInt(buf[4] & 0x3F);
    
    // the hour can be in 12-hour or 24-hour notation
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
        // 24-hour mode - the hour is in bits 5-0 as BCD
        hour = BCDToInt(buf[2] & 0x3F);
    }
    
    // minute is byte 1 bits 6-0, BCD; second is byte 0 bits 6-0, BCD
    uint8_t minute = BCDToInt(buf[1] & 0x7F);
    uint8_t second = BCDToInt(buf[0] & 0x7F);

    // populate the DateTime object
    dt = DateTime(year, month, day, hour, minute, second);
}
