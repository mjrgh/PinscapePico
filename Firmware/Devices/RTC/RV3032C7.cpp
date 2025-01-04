// Pinscape Pico - RV-3032-C7 Real-time clock/calendar chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

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
#include "RV3032C7.h"


// singleton instance
RV3032C7 *RV3032C7::inst;

// JSON configuration
void RV3032C7::Configure(JSONParser &json)
{
    if (auto *val = json.Get("rv3032c7"); !val->IsUndefined())
    {
        // get the I2C bus number
        uint8_t bus = val->Get("i2c")->UInt8(255);
        if (I2C::GetInstance(bus, true) == nullptr)
        {
            Log(LOG_ERROR, "rv3032c7: invalid/undefined I2C bus; must be 0 (I2C0) or 1 (I2C1)\n");
            return;
        }

        // create the singleton instance and add it to the I2C bus
        inst = new RV3032C7();
        I2C::GetInstance(bus, false)->Add(inst);

        // add it to the time-of-day module's RTC list
        timeOfDay.AddRTC(inst);

        // send initialization commands
        inst->SendInitCommands(i2c_get_instance(bus));

        // success
        Log(LOG_CONFIG, "RV-3032-C7 clock/calendar chip configured\n");
    }
}

void RV3032C7::ReadDateTime(I2CX *i2c)
{
    // Set up a read transaction for the timekeeping registers, 0x01-0x07
    // Note that this chip provides guaranteed coherent read of the timekeeping
    // registers.  That is, the chip snapshots all of the registers at the
    // moment we read the first register, ensuring that the whole set
    // represents a single point in time.  With chips that lack a coherent
    // read guarantee, it would be necessary to read the registers twice to
    // ensure that the seconds place didn't roll over in the middle of the
    // first read.
    uint8_t buf[]{ 0x01 };
    i2c->Read(buf, 1, 7);
}

void RV3032C7::WriteDateTime(I2CX *i2c, const DateTime &dt)
{
    // Prepare a write transaction for the timekeeping registers, 0x01-0x07.
    // This chip provides guaranteed coherency on write, as long as we write
    // the registers within a short interval.
    uint8_t buf[8]{
        0x01,                           // starting register address
        IntToBCD(dt.ss),                // reg 0x01 = seconds in BCD
        IntToBCD(dt.mm),                // reg 0x02 = minutes in BCD
        IntToBCD(dt.hh),                // reg 0x03 = 24-hour clock in BCD
        dt.GetWeekDay(),                // reg 0x04 = weekday, 0-6
                                        // (the chip defines no mapping between number and weekday label; that's up to the application)
        IntToBCD(dt.dd),                // reg 0x05 = day of month, BCD
        IntToBCD(dt.mm),                // reg 0x06 = month
        IntToBCD(dt.yyyy % 100),        // reg 0x07 = last two digits of year in BCD
    };

    // queue the write
    i2c->Write(buf, _countof(buf));
}

void RV3032C7::SendInitCommands(i2c_inst_t *i2c)
{
    // Set the backup power mode.  The factory default is DISABLED; change it
    // to Level Switching Mode (LSM), which is the recommended mode when a
    // backup battery is used.
    //
    // This chip has two sets of configuration registers: the RAM registers
    // at 0xC0-0xCA, and the EEPROM backups.  By default, the chip periodically
    // refreshes the RAM registers from EEPROM backup, so to get our settings
    // to stick, we have to save the RAM registers in EEPROM, which we can do
    // by writing 0x11 (UPDATE RAM->EEPROM) to the EE COMMAND register, 0x3F.
    // And to prevent such a refresh from happening while we're in the process
    // of programming the registers, disable auto refreshes in the CONTROL1
    // register.
    bool ok = true;
    {
        // Register 0x10 (CONTROL 1)
        // EERD = b1 (0x04), EEPROM Memory Refresh Disabled
        const uint8_t buf0[]{ 0x10, 0x04 };
        
        // Register 0xC0 (EEPROM power management unit)
        // BSM = b10 (0x40), LSM mode, all other bits set to default (0)
        const uint8_t buf1[]{ 0xC0, 0x40 };

        // Register 0x3F (EEPROM COMMAND register)
        // Command = 0x11, UPDATE ALL CONFIGURATION RAM->EEPROM
        const uint8_t buf2[]{ 0x3F, 0x11 };

        // Fire off the transactions
        if (i2c_write_timeout_us(i2c, i2cAddr, buf0, _countof(buf0), false, 100) != _countof(buf0)
            || i2c_write_timeout_us(i2c, i2cAddr, buf1, _countof(buf1), false, 100) != _countof(buf1)
            || i2c_write_timeout_us(i2c, i2cAddr, buf2, _countof(buf2), false, 100) != _countof(buf2))
        {
            Log(LOG_ERROR, "RV-3032-C7: error initializing configuration registers\n");
            ok = false;
        }
    }

    // Read the PORF (power-on reset flag) bit from the status register to determine if the
    // timekeeping registers are valid, then clear the bit to indicate that registers are
    // valid until the next power-on reset.
    bool isTimeValid = false;
    {
        uint8_t tx[]{ 0x0D, 0x00 };
        uint8_t rx[1];
        if (i2c_write_timeout_us(i2c, i2cAddr, tx, 1, true, 1000) == 1      // send status register address
            && i2c_read_timeout_us(i2c, i2cAddr, rx, 1, false, 1000) == 1   // read status register contents
            && i2c_write_timeout_us(i2c, i2cAddr, tx, 2, true, 1000) == 1)  // update status register to clear POFR bit
        {
            // the time is valid if the POFR bit is cleared
            isTimeValid = ((rx[0] & 0x02) == 0);
        }
        else
        {
            Log(LOG_ERROR, "RV-3032-C7: error checking/clearing POFR flag in status register\n");
            ok = false;
        }
    }

    // if the time registers are valid, read them
    DateTime dt;
    if (isTimeValid)
    {
        // read the current timekeeping registers - registers 0x01-0x07        
        const uint8_t tx[]{ 0x01 };
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
            Log(LOG_ERROR, "RV-3032-C7: error reading timekeeping registers\n");
            ok = false;
        }
    }   

    // log status
    if (ok)
    {
        Log(LOG_INFO, "RV-3032-C7 initialization commands OK; date on chip is %02d %s %04d %02d:%02d:%02d\n",
            dt.dd, dt.MonthNameShort(), dt.yyyy, dt.hh, dt.mm, dt.ss);
    }
    else
        Log(LOG_ERROR, "RV-3032-C7 initialization commands failed (I2C error)\n");
}

// Decode a DateTime value from the RV3032C7 timekeeping register set, registers 0x00-0x06
void RV3032C7::DecodeDateTime(DateTime &dt, const uint8_t *buf, size_t bufLen)
{
    // the expected timekeeping register length is 7 bytes; if wrong, return an empty date/time
    if (bufLen != 7)
    {
        dt = DateTime();
        return;
    }

    // The last two digits of the year are in byte 6, in BCD format.  The
    // chip is designed only to represent years in the 21st century, so the
    // base year is always 2000.
    int16_t year = 2000 + BCDToInt(buf[6]);

    // the month is in byte 5 bits 4-0
    uint8_t month = BCDToInt(buf[5] & 0x1F);

    // the day of the month is in BDC byte 4 bits 5-0
    uint8_t day = BCDToInt(buf[4] & 0x3F);

    // the hour is in byte 3, always in 24-hour clock format (0-23), in BCD
    uint8_t hour = BCDToInt(buf[3] & 0x3F);

    // minute is byte 1 bits 6-0, BCD; second is byte 0 bits 6-0, BCD
    uint8_t minute = BCDToInt(buf[1] & 0x7F);
    uint8_t second = BCDToInt(buf[0] & 0x7F);

    // populate the DateTime object
    dt = DateTime(year, month, day, hour, minute, second);
}
