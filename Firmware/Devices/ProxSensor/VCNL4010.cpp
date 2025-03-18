// Pinscape Pico - VCNL4010 IR proximity sensor interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <math.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/watchdog.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "I2C.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "Logger.h"
#include "VCNL4010.h"

// global singleton instance
VCNL4010 *vcnl4010 = nullptr;

// construction
VCNL4010::VCNL4010(int iredCurrent, int gpInt) :
    I2CDevice(0x13),
    iredCurrent(iredCurrent),
    gpInt(gpInt)
{
}

// Configure from JSON data
//
// vcnl4010: {
//   i2c: <number>,          // I2C bus number (0 or 1) (note: no I2C address is needed, since VCNL4010 has a fixed address, 0x13)
//   iredCurrent: <number>,  // IR emitter current in milliamps, 10 to 200, in 10mA increments; default is 200
//   interrupt: <gpio>,      // GPIO port number of interrupt input; optional, omit if interrupt pin isn't connected
// }
//
void VCNL4010::Configure(JSONParser &json)
{
    if (auto *val = json.Get("vcnl4010") ; !val->IsUndefined())
    {
        // get the bus
        uint8_t bus = val->Get("i2c")->UInt8(255);
        if (I2C::GetInstance(bus, true) == nullptr)
        {
            Log(LOG_ERROR, "vcnl4010: invalid/undefined I2C bus\n");
            return;
        }

        // read and validate the IRED current setting: it has to be 10 to 200 mA,
        // in 10 mA increments
        int current0 = val->Get("iredCurrent")->Int(200);
        int current = (current0 < 10) ? 10 :
            (current0 > 200) ? 200 :
            (current0 % 10) != 0 ? (current0 + 5)/10 * 10 :
            current0;

        if (current != current0)
            Log(LOG_WARNING, "vcnl4010: invalid iredCurrent %d; must be 10-200 in increments of 10; adjusted to %d\n", current0, current);

        // get the optional interrupt-in GPIO port
        int gpInt = val->Get("interrupt")->Int(-1);
        if (gpInt >= 0)
        {
            // validate it
            if (!IsValidGP(gpInt))
            {
                Log(LOG_ERROR, "vcnl4010: invalid interrupt GPIO port\n");
                return;
            }

            // claim it as a shared input, with pull-up
            if (!gpioManager.ClaimSharedInput("VCNL4010 (INT)", gpInt, true, false, true))
                return;
        }

        // create the singleton
        vcnl4010 = new VCNL4010(current, gpInt);

        // set the polling flag
        if (val->Get("poll")->Bool(false))
            vcnl4010->forcePolling = true;
        
        // Initialize our GPIO IRQ handler.  The IRQ line is pulled low
        // when an interrupt is asserted.  Respond to the falling edge
        // rather than a constant low signal, because we can't clear the
        // hardware signal within the interrupt handler.  Clearing the
        // signal requires an I2C transaction, which isn't allowed
        // within the IRQ handler.  Our I2C connection might be shared
        // with other devices, so we can only access it when it's our
        // turn in the polling cycle.  An edge-sensitive handler lets us
        // handle the interrupt signal and return with it still
        // asserted, without getting called again until we have a chance
        // to clear the signal.
        if (gpInt >= 0)
        {
            gpio_add_raw_irq_handler(gpInt, &IRQ);
            gpio_set_irq_enabled(gpInt, GPIO_IRQ_EDGE_FALL, true);
            irq_set_enabled(IO_IRQ_BANK0, true);
        }

        // initialize it
        vcnl4010->Init(bus);

        // add it to the I2C bus
        I2C::GetInstance(bus, false)->Add(vcnl4010);
    }
}

// initialize the chip
void VCNL4010::Init(int bus)
{
    // send initialization commands
    SendInitCommands(i2c_get_instance(bus));
}

void VCNL4010::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst());
}

void VCNL4010::SendInitCommands(i2c_inst_t *i2c)
{
    // Read the Product ID/Revision register.  This is purely for
    // logging purposes, as a troubleshooting aid to verify that the
    // chip is connected and responding to commands.
    //
    // The device can take up to 2.5ms to start up after a reset,
    // so allow a few milliseconds of failures before giving up.
    bool ok = false;
    uint8_t prodIdRev = 0x00;
    for (int i = 0 ; i < 10 ; ++i)
    {
        // try reading the register
        uint8_t buf[] = { 0x81 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf)
            && i2c_read_timeout_us(i2c, i2cAddr, buf, 1, false, 1000) == 1)
        {
            // success - retrieve the product ID code and stop looping
            prodIdRev = buf[0];
            ok = true;
            break;
        }

        // pause briefly before retrying
        watchdog_update();
        sleep_us(1000);
    }

    // log for debugging in the product code read failed
    if (!ok)
        Log(LOG_DEBUG, "VCNL4010 ProductID register (0x81) read failed\n");
    
    // disable all self-timed modes
    {
        static const uint8_t buf[] = { 0x80, 0x00 };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        if (result != _countof(buf))
        {
            Log(LOG_DEBUG, "VCNL4010 Mode register (0x80) write failed\n");
            ok = false;
        }
    }

    // Set the proximity sampling rate to the fastest available rate of
    // 250 samples/second (4ms/sample).  We'd actually prefer this to be
    // a little faster, because the plunger can move fast enough during
    // a firing motion that we need about 400 samples per second for
    // accurate speed calculations.  But this is the best this sensor
    // can do, and it's plenty fast for smooth animation.
    {
        static const uint8_t buf[] = { 0x82, 0x07 };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        if (result != _countof(buf))
        {
            Log(LOG_DEBUG, "VCNL4010 Sampling rate register (0x82) write failed\n");
            ok = false;
        }
    }

    // Set the current for the IR LED (the light source for proximity
    // measurements).  Register 0x83 sets the current in units of 10mA,
    // up to 200mA.
    //
    // Note that the nominal current level isn't the same as the actual
    // current load on the sensor's power supply.  The nominal current
    // set here is the instantaneous current the chip uses to generate
    // IR pulses.  The pulses have a low duty cycle, so the continuous
    // current drawn on the chip's power inputs is much lower than the
    // nominal setting.  The data sheet says that the total continuous
    // power supply current drawn with the most power-hungry settings
    // (with IRED current maxed out at 200mA, sampling frequency maxed
    // at 250 Hz) is only 4mA.  So there's no need to worry about
    // blowing a fuse on the USB port or frying the Pico 3.3V regulator,
    // even at the highest IRED current setting.
    int curRegByte = (iredCurrent < 10 ? 10 : iredCurrent > 200 ? 200 : iredCurrent) / 10;
    {
        const uint8_t buf[] = { 0x83, static_cast<uint8_t>(curRegByte) };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        if (result != _countof(buf))
        {
            Log(LOG_DEBUG, "VCNL4010 IRED current register (0x83) write failed\n");
            ok = false;
        }
    }

    // Enable PROX_ready interrupts (INT_PROX_ready_EN, bit 0x08 in
    // register 0x89).  Do this even if the interrupt isn't wired to
    // a GPIO, since we can still read the interrupt status register
    // (0x8E) to check it.
    {
        static const uint8_t buf[] = { 0x89, 0x08 };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        if (result != _countof(buf))
        {
            Log(LOG_DEBUG, "VCNL4010 PROX_ready interrupts (0x89) write failed\n");
            ok = false;
        }
    }

    // enable self-timed proximity measurements
    // (prox_en [0x02] | selftimed_en [0x01] in command register 0x80)
    {
        static const uint8_t buf[] = { 0x80, 0x03 };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        if (result != _countof(buf))
        {
            Log(LOG_DEBUG, "VCNL4010 PROX_EN register (0x80) write failed\n");
            ok = false;
        }
    }

    // report the result
    Log(ok ? LOG_CONFIG : LOG_ERROR, "VCNL4010 device initialization %s; I2C%d addr 0x%02x, Product ID %d, Rev %d, IR current %d mA\n",
        ok ? "OK" : "failed",
        i2c_hw_index(i2c), i2cAddr,
        prodIdRev >> 4, prodIdRev & 0x0F, curRegByte*10);
}

// IRQ handler.  Our I2C connection is shared with other devices and
// driven by polling, so we can't perform an I2C transaction here; that
// will have to wait until our polling handler notices that the line has
// gone low.  What we can do is record the time of the interrupt, so
// that the polling routine has precise information (to within interrupt
// service latency) about when the new sample was collected.  This
// allows the polling handler to collect the sample at its leisure, but
// still gives us precise timing information.  Precision timing is
// useful when calculating derivatives (speed, acceleration) of the
// distance readings.
void VCNL4010::IRQ()
{
    // remember the sample arrival time
    vcnl4010->tInterrupt = time_us_64();

    // acknowledge the IRQ
    gpio_acknowledge_irq(vcnl4010->gpInt, GPIO_IRQ_EDGE_FALL);
}

bool VCNL4010::Read(uint16_t &proxCount, uint64_t &timestamp)
{
    // return the last reading
    proxCount = lastProxCount;
    timestamp = tLastSample;

    // consume the reading
    bool ret = isSampleReady;
    isSampleReady = false;

    // return the new/old status
    return ret;
}

bool VCNL4010::OnI2CReady(I2CX *i2c)
{
    // check if the interrupt signal is connected
    if (gpInt >= 0 && !forcePolling)
    {
        // The interrupt line is connected, so a new sample is available
        // if and only if the line is being pulled low.  If no sample is
        // available, there's no need for an I2C transaction on this round.
        if (!gpio_get(gpInt))
        {
            // a sample is available - read the proximity result registers
            // (0x87 = MSB, 0x88 = LSB)
            readReg = 0x87;
            i2c->Read(&readReg, 1, 2);
            return true;
        }
    }
    else
    {
        // We don't have an interrupt line, so we have to poll for input.
        // Don't poll until we reach the estimated time for the next sample;
        // the sensor collects samples automatically at a fixed pace, so
        // there's no point in checking until the next sample interval
        // elapses.
        if (time_us_64() > tNextSampleEst)
        {
            readReg = 0x80;
            i2c->Read(&readReg, 1, 1);
            return true;
        }
    }

    // no transaction needed on this cycle
    return false;
}

bool VCNL4010::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // check which register we're reading
    switch (readReg)
    {
    case 0x80:
        // Command register - we're polling for new input.  Check the
        // prox_data_rdy bit (0x20).  If set, it means that a new sample
        // is available in the prox result registers (0x87, 0x88).
        if ((data[0] & 0x20) != 0)
        {
            // input ready - read it from registers 0x87-0x88
            readReg = 0x87;
            i2c->Read(&readReg, 1, 2);
            return true;
        }
        break;

    case 0x8E:
        // Interrupt status register - polling for input.  Check the
        // prox ready bit (0x08).  If set, it means a new sample is
        // available.
        if ((data[0] & 0x08) != 0)
        {
            readReg = 0x87;
            i2c->Read(&readReg, 1, 2);
            return true;
        }
        break;

    case 0x87:
        // Proximity result registers - this is a new incoming sample.
        if (len == 2)
        {
            // The first byte is register 0x87 (result high byte), second is
            // register 0x88 (result low byte).
            lastProxCount = (static_cast<uint16_t>(data[0]) << 8) | data[1];

            // Record the time we obtained the sample.  If we have an
            // interrupt input, use the time of the last interrupt, since
            // that records precisely when the chip signaled that a new
            // sample was ready.  Otherwise, we don't know when the sample
            // actually arrived at the chip, so the best we can is record
            // the time we read it, which is less precise since the sample
            // could have been sitting there for up to a full cycle (4ms at
            // the maximum 250/s sampling rate).
            tLastSample = (gpInt >= 0) ? tInterrupt : time_us_64();

            // Set the estimated next sample time.  We program the sensor
            // for 250 sample/second, so we expect a new sample in about
            // 4ms.  This is only used when we're in polling mode, to
            // minimize unnecessary I2C bus usage.  In interrupt mode, we
            // can efficiently determine when there's a sample available
            // without any I2C traffic, by checking the interrupt GPIO.
            tNextSampleEst = tLastSample + 4000;

            // flag that a new sample is available for reading at the
            // programmatic interface
            isSampleReady = true;

            // If we're using interrupts, explicitly clear the interrupt
            // status bit (bit 0x08 in register 0x8E) on the device.  For
            // good measure, just clear all of the interrupt status bits, by
            // writing '1' bits to each position with an assigned meaning.
            if (gpInt >= 0)
            {
                uint8_t buf[] = { 0x8E, 0x0F };
                i2c->Write(buf, _countof(buf));
                return true;
            }
        }

        // done
        break;
    }

    // no further operation requested
    return false;
}

