// Pinscape Pico firmware - MC3416 Accelerometer chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Utils.h"
#include "I2C.h"
#include "JSON.h"
#include "Logger.h"
#include "GPIOManager.h"
#include "Nudge.h"
#include "CommandConsole.h"
#include "MC3416.h"

// singleton instance
MC3416 *MC3416::inst = nullptr;

// construction
MC3416::MC3416(uint16_t addr, int gpInterrupt) :
    I2CDevice(addr),
    gpInterrupt(gpInterrupt)
{
    // add our console command
    CommandConsole::AddCommand(
        "MC3416", "show MC3416 accelerometer status",
        "MC3416 [options]\n"
        "options:\n"
        "   -s, --stats         show statistics (default if no arguments are provided)\n"
        "   -r, --reset-stats   reset statistics counters\n",
        &Command_info);
}

// Configure from JSON data
//
// mc3416: {
//   i2c: <number>,          // I2C bus number (0 or 1)
//   addr: <number>,         // 0x4C or 0x6C (set via VPP level at power-up; GND -> 0x4C, VDD -> 0x6c)
//   interrupt: <gpio>,      // GPIO port connected to the chip's interrupt (INT) pin, if any
//   gRange: <number>,       // dynamic range, in units of Earth's gravity ("g"): 2, 4, 8, 12, or 16
// }
//
// The interrupt GPIO connection is optional, both in the configuration
// and physically in the board wiring.  If there's no interrupt GPIO,
// we'll simply poll the chip whenever we have a chance (i.e., when it's
// our turn for I2C bus access).  The software is more efficient if the
// interrupt line is configured, because it can tell from the interrupt
// signal when the chip has nothing new available, and can thus skip
// unnecessary I2C polling.  
void MC3416::Configure(JSONParser &json)
{
    if (auto *val = json.Get("mc3416") ; !val->IsUndefined())
    {
        // get the I2C bus number and address
        int bus = val->Get("i2c")->Int(-1);
        int addr = val->Get("addr")->Int(0x4C);
        if (I2C::GetInstance(bus, true) == nullptr)
        {
            Log(LOG_ERROR, "MC3416: invalid I2Cn bus number; must be 0 or 1\n");
            return;
        }

        // the I2C address for this chip must be 0x4C or 0x6C
        if (addr != 0x4C && addr != 0x6C)
        {
            Log(LOG_ERROR, "MC3416: invalid address 0x%02X; must be 0x4C or 0x6C\n", addr);
            return;
        }

        // Get the interrupt pin, if any
        int gpIntr = val->Get("interrupt")->Int(-1);
        if (gpIntr != -1)
        {
            // validate it
            if (!IsValidGP(gpIntr))
            {
                Log(LOG_ERROR, "MC3416: invalid interrupt GPIO port\n");
                return;
            }

            // Assign the pin as an input; the physical pin on the chip is
            // open-drain, so it needs a pull-up.  (The open-drain
            // configuration allows the interrupt signal to be shared, to
            // conserve GPIO pins on the MCU.)
            if (!gpioManager.ClaimSharedInput("MC3416 (Interrupt)", gpIntr, true, false, false))
                return;

            // configure the IRQ handler - the interrupt signal from the
            // sensor is active-low, so trigger on the falling edge
            gpio_add_raw_irq_handler(gpIntr, &MC3416::IRQ);
            gpio_set_irq_enabled(gpIntr, GPIO_IRQ_EDGE_FALL, true);
            irq_set_enabled(IO_IRQ_BANK0, true);
        }

        // create the singleton
        inst = new MC3416(addr, gpIntr);

        // get the dynamic range
        inst->gRange = val->Get("gRange")->Int(2);
        if (inst->gRange != 2 && inst->gRange != 4 && inst->gRange != 8 && inst->gRange != 12 && inst->gRange != 16)
        {
            Log(LOG_ERROR, "MC3416: invalid gRange range value %d; must be 2, 4, 8, 12, or 16; using 2\n", inst->gRange);
            inst->gRange = 2;
        }

        // initialize it
        inst->Init(i2c_get_instance(bus));

        // add it to the I2C bus manager for the selected bus
        I2C::GetInstance(bus, false)->Add(inst);
    }
}

// initialize
void MC3416::Init(i2c_inst_t *i2c)
{
    // send initialization commands
    SendInitCommands(i2c);

    // Register the device
    accelerometerRegistry.Register(this);
}

void MC3416::SendInitCommands(i2c_inst_t *i2c)
{
    // wait for the OTP_BUSY bit (0x80) in the device status register (0x05) to clear,
    // indicating device is ready to use
    uint64_t timeout = time_us_64() + 20000;
    bool otpReady = false;
    bool ok = true;
    while (time_us_64() < timeout)
    {
        // read register 0x02
        uint8_t buf[] = { 0x05 }, rdbuf;
        ok = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf)
             && i2c_read_timeout_us(i2c, i2cAddr, &rdbuf, 1, false, 1000) == 1;

        // stop on error
        if (!ok)
            break;

        // check OTP_BUSY
        if ((rdbuf & 0x80) == 0)
        {
            otpReady = true;
            break;
        }
    }
    if (!otpReady)
        Log(LOG_ERROR, "MC3416: %s waiting for OTP_BUSY status to clear\n", ok ? "timeout" : "I2C error");

    // add the desired 'g' range register (the catch-all register value 0 is the default of 2g)
    uint8_t scaleReg = (gRange == 4) ? 0x19 : (gRange == 8) ? 0x29 : (gRange == 16) ? 0x39 : (gRange == 12) ? 0x49 : 0x09;
    {
        uint8_t buf[] = { 0x20, scaleReg };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // enable sample-ready interrupts
    {
        const uint8_t buf[] = { 0x06, 0x80 };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // set the data rate to 1K samples/second
    {
        uint8_t buf[] = { 0x08, 0x05 };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // set state to WAKE in the MODE register (0x07)
    {
        // interrupt active low, interrupt open drain, watchdog enabled (0x30), WAKE (0x01)
        uint8_t buf[] = { 0x07, 0x31 };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "MC3416 device initialization %s\n", ok ? "OK" : "failed");
}

void MC3416::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst());
}

void MC3416::Task()
{
}

void MC3416::IRQ()
{
    // Sample ready - count missed samples, and record the time the
    // sample completed at the sensor.  Note that we don't actually read
    // the sample here, because the I2C bus is a shared resource that we
    // can only access in normal thread context.
    if (inst->stats.tIntr != 0)
        inst->stats.nMissedSamples += 1;
    else
        inst->stats.tIntr = time_us_64();

    // count the interrupt
    inst->stats.nIntr += 1;

    // acknowledge the IRQ
    gpio_acknowledge_irq(inst->gpInterrupt, GPIO_IRQ_EDGE_FALL);
}

// I2C bus available for our use
bool MC3416::OnI2CReady(I2CX *i2c)
{
    // If the interrrupt is asserted, or the interrupt line isn't
    // connected, read interrupt status and axis registers.  It's more
    // efficient to check the interrupt line when we have GPIO access
    // to it, since we can avoid an unnecessary I2C transaction when
    // we can tell from the interrupt status that the chip has no new
    // data available.
    //
    // Note that the interrupt line is active-low - the chip signals an
    // interrupt by pulling the line to ground.
    if (gpInterrupt == -1 || !gpio_get(gpInterrupt))
    {
        // Read:
        //
        //  XOUT_EX_L   (0x0D)   - X axis low 8 bits
        //  XOUT_EX_H   (0x0E)   - X axis high 8 bits
        //  YOUT_EX_L   (0x0F)   - Y axis low 8 bits
        //  YOUT_EX_H   (0x10)   - Y axis high 8 bits
        //  ZOUT_EX_L   (0x11)   - Z axis low 8 bits
        //  ZOUT_EX_H   (0x12)   - Z axis high 8 bits
        //  STATUS_2    (0x13)   - Status register
        //  INTR_STAT_2 (0x14)   - Interrupt status register
        uint8_t buf[] = { 0x0D };
        i2c->Read(buf, 1, 8);

        // new transaction started
        return true;
    }

    // no transaction started
    return false;
}

// I2C read completed
bool MC3416::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we expect 8 bytes back for the register read
    if (len == 8)
    {
        // Check the NEW_DATA bit in the status register - it's set
        // if there's new data to read.  We can't know for sure that
        // there's a new sample, even if we're using an interrupt
        // signal, because the interrupt signal could be shared with
        // other peripherals.  (This device, like many others, has
        // an open-drain interrupt line, specifically so that the
        // application designer has the option to wire-OR the signals
        // from multiple chips, as a way to conserve MCU GPIOs.)
        if ((data[0] & 0x01) != 0)
        {
            // Retrieve the new axis data.  The axis values are encoded
            // as 16-bit signed ints (2's complement format), LSB first.
            // platform.)  This matches the normalized format that we
            // return through the hardware abstraction interface, so we
            // don't have to perform any additional scaling conversion
            // between device units and normalized units.
            static auto Get16 = [](const uint8_t *buf) {
                uint16_t w = (static_cast<uint16_t>(buf[1]) << 8) | buf[0];
                return static_cast<int16_t>(w);
            };
            x = Get16(&data[0]);
            y = Get16(&data[1]);
            z = Get16(&data[2]);

            // save the X/Y/Z output registers, for diagnostics
            memcpy(outReg, &data[0], sizeof(outReg));

            // use the timestamp from the last interrupt (if available)
            uint64_t now = time_us_64();
            if (gpInterrupt != -1)
            {
                // record the sample time from the interrupt, and clear
                // the time, so that the next interrupt knows that we
                // successfully read this sample before the next one
                // arrived
                IRQDisabler irqd;
                timestamp = stats.tIntr;
                stats.tIntr = 0;
            }
            else
            {
                // no interrupt line - timestamp it with the I2C read time
                timestamp = now;
            }

            // collect latency stats
            stats.nReads += 1;
            if (timestamp != 0)
                stats.i2cLatency += now - timestamp;

            // collect time-between-samples stats
            if (stats.tRead != 0)
                stats.timeBetweenReads += now - stats.tRead;
            stats.tRead = now;

            // The new-sample interrupt is cleared automatically by
            // reading the interrupt status register, so there's no need
            // for an extra transaction here to clear it.  Likewise, the
            // new-data bit in the status register is cleared by reading
            // it.
        }
    }

    // no new transaction started
    return false;
}

// read the last sample, in normalized INT16 units
void MC3416::Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp)
{
    x = this->x;
    y = this->y;
    z = this->z;
    timestamp = this->timestamp;
}

// command console status command
void MC3416::Command_info(const ConsoleCommandContext *c)
{
    if (inst == nullptr)
        return c->Print("MC3416 not configured\n");

    static const auto ShowStats = [](const ConsoleCommandContext *c)
    {
        auto &s = inst->stats;
        c->Printf(
            "MC3416 status:\n"
            "Number of IRQs serviced:   %llu\n"
            "Number of samples read:    %llu\n"
            "Average read interval:     %.2f ms\n"
            "Average read latency:      %lu us\n"
            "Missed samples:            %llu (1 in %llu)\n"
            "X,Y,Z (Native INT16):      %d,%d,%d\n"
            "Sample reg bytes [0D-12]:  %02X.%02X %02X.%02X %02X.%02X\n",
            s.nIntr, s.nReads,
            static_cast<float>(s.nReads != 0 ? s.timeBetweenReads / s.nReads : 0) / 1000.0f,
            static_cast<uint32_t>(s.nReads != 0 ? s.i2cLatency / s.nReads : 0),
            s.nMissedSamples, s.nMissedSamples == 0 ? 0 : s.nIntr / s.nMissedSamples,
            inst->x, inst->y, inst->z,
            inst->outReg[0], inst->outReg[1], inst->outReg[2], inst->outReg[3], inst->outReg[4], inst->outReg[5]);
    };

    // with zero arguments, just show statistics
    if (c->argc == 1)
        return ShowStats(c);

    // process arguments
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            ShowStats(c);
        }
        else if (strcmp(a, "-r") == 0 || strcmp(a, "--reset-stats") == 0)
        {
            // reset statistics for next time
            auto &s = inst->stats;
            s.nReads = 0;
            s.timeBetweenReads = 0;
            s.i2cLatency = 0;
            c->Print("MC3416 statistics counters reset\n");
        }
        else
            return c->Usage();
    }
}

