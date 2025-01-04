// Pinscape Pico - Base class for I2C-based RTC implementations
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
#include "DS3231M.h"


void I2CRTCBase::Write(const DateTime &dt)
{
    // store the pending write
    dateTimeToWrite = dt;
    writePending = true;
}

void I2CRTCBase::ReadAsync(std::function<void(const DateTime&)> callback)
{
    // schedule the read
    readCallback = callback;
    readPending = true;
}

bool I2CRTCBase::OnI2CReady(I2CX *i2c)
{
    // set up any pending read/write transactions
    if (readPending)
    {
        // set up a read transaction for the timekeeping registers
        ReadDateTime(i2c);

        // clear the pending read flag and indicate that we started a transaction
        readPending = false;
        return true;
    }
    else if (writePending)
    {
        // set up a write transaction to the timekeeping registers
        WriteDateTime(i2c, dateTimeToWrite);

        // clear the pending write flag and indicate that we started a transaction
        writePending = false;
        return true;
    }

    // no work pending
    return false;
}

bool I2CRTCBase::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we always read the timekeep registers, 7 bytes, register addresses 0x00-0x06
    DateTime dt;
    DecodeDateTime(dt, data, len);

    // Invoke the callback.  Note that if we didn't decode anything, we still
    // want to invoke the callback, with all fields in 'dt' still set to the
    // zero defaults.
    InvokeReadCallback(dt);

    // no more I2C work to queue
    return false;
}
