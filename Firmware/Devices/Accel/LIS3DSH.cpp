// Pinscape Pico firmware - LIS3DSH Accelerometer chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <functional>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/watchdog.h>

// project headers
#include "Utils.h"
#include "I2C.h"
#include "JSON.h"
#include "Logger.h"
#include "GPIOManager.h"
#include "Nudge.h"
#include "USBIfc.h"
#include "CommandConsole.h"
#include "LIS3DSH.h"

// singleton instance
LIS3DSH *LIS3DSH::inst = nullptr;

// construction
LIS3DSH::LIS3DSH(uint16_t addr, int gpInterrupt) :
    I2CDevice(addr),
    gpInterrupt(gpInterrupt)
{
    // add our console command
    CommandConsole::AddCommand(
        "lis3dsh", "show LIS3DSH accelerometer status",
        "lis3dsh [options]\n"
        "options:\n"
        "   -s, --stats         show statistics (default if no arguments are provided)\n"
        "   -r, --reset-stats   reset statistics counters\n",
        &Command_info);
}

// Configure from JSON data
//
// lis3dsh: {
//   i2c: <number>,          // I2C bus number (0 or 1) 
//   addr: <number>          // I2C address, 0x18 or 0x19 (set by SDO/SA0 pin: GND -> 0x18, VDD -> 0x19)
//   interrupt: <gpio>,      // GPIO port connected to the chip's interrupt (INT) pin, if any
//   gRange: <number>,       // dynamic range, in units of Earth's gravity ("g"): 2, 4, 6, 8, or 16
// }
//
// The interrupt GPIO connection is optional, both in the configuration
// and physically in the board wiring.  If there's no interrupt GPIO,
// we'll simply poll the chip whenever we have a chance (i.e., when it's
// our turn for I2C bus access).  The software is more efficient if the
// interrupt line is configured, because it can tell from the interrupt
// signal when the chip has nothing new available, and can thus skip
// unnecessary I2C polling.  
void LIS3DSH::Configure(JSONParser &json)
{
    if (auto *val = json.Get("lis3dsh") ; !val->IsUndefined())
    {
        // get and validate the I2C bus number
        int bus = val->Get("i2c")->Int(-1);
        if (!I2C::ValidateBusConfig("lis3dsh", bus))
            return;

        // get and validate the I2C address
        int addr = val->Get("addr")->Int(-1);
        if (addr != 0x1D && addr != 0x1E)
        {
            Log(LOG_ERROR, "lis3dsh: invalid I2C address 0x%02X; must be 0x1D or 0x1E\n", addr);
            return;
        }

        // Get the interrupt pin, if any
        int gpIntr = val->Get("interrupt")->Int(-1);
        if (gpIntr != -1)
        {
            // validate it
            if (!IsValidGP(gpIntr))
            {
                Log(LOG_ERROR, "lis3dsh: invalid interrupt GPIO port\n");
                return;
            }

            // Assign the pin as an input.  The interrupt output on the
            // LIS3DSH is push-pull, so disable Pico pull-up/down resistors
            // on the GPIO.  
            if (!gpioManager.ClaimSharedInput("LIS3DSH (Interrupt)", gpIntr, false, false, false))
                return;

            // configure the IRQ handler - the interrupt signal from the
            // sensor is active-low, so triger on the falling edge
            gpio_add_raw_irq_handler(gpIntr, &LIS3DSH::IRQ);
        }

        // create the singleton
        inst = new LIS3DSH(addr, gpIntr);

        // get the dynamic range
        inst->gRange = val->Get("gRange")->Int(2);
        if (inst->gRange != 2 && inst->gRange != 4 && inst->gRange != 6 && inst->gRange != 8 && inst->gRange != 16)
        {
            Log(LOG_ERROR, "lis3dsh: invalid gRange range value %d; must be 2, 4, 6, 8, or 16; using 2\n", inst->gRange);
            inst->gRange = 2;
        }

        // initialize it
        inst->Init(i2c_get_instance(bus));

        // add it to the I2C bus manager for the selected bus
        I2C::GetInstance(bus, false)->Add(inst);

        // enable interrupts if configured
        if (gpIntr != -1)
        {
            gpio_set_irq_enabled(gpIntr, GPIO_IRQ_EDGE_FALL, true);
            irq_set_enabled(IO_IRQ_BANK0, true);
        }
    }
}

// initialize
void LIS3DSH::Init(i2c_inst_t *i2c)
{
    // The device has an approximately 5ms boot time from power-on, per ST
    // Application Note AN3393, section 2 ("Startup Sequence").  Let's wait
    // until the Pico clock says it's been a little longer than 5ms since the
    // Pico started up, which should be a good proxy for when power was
    // applied to the chip.  (We could just wait 5ms inline here, but that
    // seems wasteful, since the chip will be doing its startup processing in
    // parallel with whatever other work we did before we got here.)
    while (time_us_64() < 7500)
    {
        watchdog_update();
        sleep_us(1000);
    }

    // send initialization commands
    SendInitCommands(i2c, true);
    
    // Register the device
    accelerometerRegistry.Register(this);

    // Add a HID logical axis for the sensor's temperature output
    class LIS3DSHTemperatureLogicalAxis : public LogicalAxis
    {
    public:
        LIS3DSHTemperatureLogicalAxis() { }
        virtual int16_t Read() override { return LIS3DSH::inst->temperatureInt16; }
    };
    LogicalAxis::AddSource("lis3dsh.temperature",
        [](const LogicalAxis::CtorParams&, std::vector<std::string>&) -> LogicalAxis* { return new LIS3DSHTemperatureLogicalAxis(); });
}

void LIS3DSH::SendInitCommands(i2c_inst_t *i2c, bool isPowerOn)
{
    // read the WHO_AM_I register to confirm the device is responding
    bool ok = true;
    {
        // read register 0x0F (WHO_AM_I)
        uint8_t buf[] = { 0x0F }, whoAmI;
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf))
             && (i2c_read_timeout_us(i2c, i2cAddr, &whoAmI, 1, false, 1000) == 1)
             && ok;

        if (ok)
        {
            Log(whoAmI == 0x3F ? LOG_INFO : LOG_ERROR, "LIS3DSH: WHO_AM_I (reg 0x0F)=0x%02x (%s)\n", whoAmI,
                whoAmI == 0x33 ? "LIS3DH - wrong driver selected" : whoAmI == 0x3F ? "LIS3DSH" : "unknown device; expected 0x3F");
        }
    }

    // CTRL_REG4 (0x20)
    // set ODR (data rate in high half) - 0x80 = 800 samples/second
    // set block data update (BDU), bit 0x08
    // set ZEN/YEN/XEN axis enable bits, bits 0x07
    {
        uint8_t buf[] = { 0x20, 0x8F };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // CTRL_REG3 (0x23)
    // DR_EN (0x80)   -> DRDY signal enabled on INT1 - set if an interrupt GPIO is configured
    // INT1_EN (0x08) -> enable interrupt 1 - set if an interrupt GPIO is configured
    {
        uint8_t buf[] = { 0x23, static_cast<uint8_t>(gpInterrupt != -1 ? 0x88 : 0x00) };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // CTRL_REG5 (0x24)
    // FSCALE - set full-scale range (0x00 = 2g, 0x08 = 4g, 0x10 = 6g, 0x18 = 8g, 0x20 = 16g)
    {
        uint8_t scaleBits = (gRange == 16 ? 0x20 : gRange == 8 ? 0x18 : gRange == 6 ? 0x10 : gRange == 4 ? 0x08 : 0x00);
        uint8_t buf[] = { 0x23, scaleBits };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // log status
    char intrDesc[8];
    sprintf(intrDesc, gpInterrupt == -1 ? "N/A" : "GP%d", gpInterrupt);
    Log(ok ? LOG_CONFIG : LOG_ERROR, "LIS3DSH device initialization %s; interrupt %s, dynamic range +/- %dg\n",
        ok ? "OK" : "failed", intrDesc, gRange);
}

void LIS3DSH::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst(), false);
}

void LIS3DSH::Task()
{
}

void LIS3DSH::IRQ()
{
    // check the interrupt
    if ((gpio_get_irq_event_mask(inst->gpInterrupt) & GPIO_IRQ_EDGE_FALL) != 0)
    {
        // record the time the sample became ready, if we haven't already
        // timestamped it
        uint64_t now = time_us_64();
        if (inst->stats.tIntr == 0)
            inst->stats.tIntr = now;

        // Count the interrupt, EXCEPT when the interrupt is within 500us of
        // the start of the last I2C read.  The LIS3DSH's DRDY signal on INT1
        // has a peculiar behavior that contradicts the timing schematic shown
        // in the data sheet, which we need to work around:
        //
        // * What should happen: when a new sample is loaded into OUT Z (the
        //   last register loaded when a new sample is ready), the ZYXDA bit
        //   in the STATUS register is set to '1', firing INT1 when INT1 is
        //   configured to output ZYXDA (which the manufacturer's Application
        //   Note refers to as DRDY mode).  In our case, INT1 goes low.  INT1
        //   now remains active (low) until the host reads *all three* OUT
        //   registers, OUT X, OUT Y, OUT Z.  Upon reading the last byte of
        //   the third register, ZYXDA clears to '0' and INT1 goes inactive
        //   (high).
        //
        // * What REALLY happens: the first part, where INT1 goes low when
        //   a new sample has been loaded into OUT Z, works as documented.
        //   The second part is wrong in the AN, though.  Instead of INT1
        //   remaining low until all three OUT registers are read, INT1
        //   momentarily toggles to inactive (high) and then back to active
        //   (low) after EACH BYTE of the output registers is read.  So we
        //   see five low-high-low transitions on INT1 as we read out the
        //   six register bytes, triggering our EDGE_FALL interrupt five
        //   times as the I2C transaction proceeds.  The timing of these
        //   five interrupts coincides with the I2C transmission of the
        //   first five register bytes.  After the chip reads out the
        //   sixth register byte (the high byte of OUT Z), INT1 finally
        //   settles at inactive (high) and we stop seeing the repeated
        //   interrupts.
        //
        // Presumably, the extra interrupts are an artifact of some internal
        // logic in the chip that updates the ZYXDA bit, during which update
        // the bit gets momentarily cleared until the update logic sets it
        // back to '1' upon determining that the clear condition hasn't been
        // met yet.  Whatever the reason, the momentary clearing of the bit
        // takes the INT1 line high for long enough for the Pico to detect
        // the edge and invoke the IRQ handler.
        //
        // The extra interrupts contradict the Application Note (the data
        // sheet says nothing about them one way or the other; the woeful
        // inadequacy of the data sheet is probably why there's an Application
        // Note at all), so the behavior is either a bug in the chip or an
        // erratum in the Application Note.  The difference is academic as
        // it's the way the chip actually works.  Fortunately, while the extra
        // interrupts are be spurious, they're not sporadic; they happen
        // consistently every time time, and they have consistent timing.  So
        // we can work around the bug/erratum by filtering out the interrupts
        // that we know to be spurious.  We know that an interrupt is spurious
        // if it happens within the time it takes an I2C OUT X/Y/Z read
        // transaction to complete, which is about 220us.  To be safe,
        // extend that time window a bit, since there can be some variation
        // if other asynchronous (interrupt) events occur while we're setting
        // up the read.
        if (now > inst->stats.tReadStarted + 500)
            inst->stats.nIntr += 1;
        
        // acknowledge the IRQ
        gpio_acknowledge_irq(inst->gpInterrupt, GPIO_IRQ_EDGE_FALL);
    }
}

// I2C bus available for our use
bool LIS3DSH::OnI2CReady(I2CX *i2c)
{
    // Check if we have an interrupt signal from the chip, which tells
    // us asynchronously when a sample becomes available.
    if (gpInterrupt != -1)
    {
        // Interrupt line is available, so we can check the status
        // without a status read.  If the line is low, a sample is
        // ready.
        if (!gpio_get(gpInterrupt))
        {
            // Read:
            //
            //  OUT_X_L    (0x28)  - X axis low 8 bits
            //  OUT_X_H    (0x29)  - X axis high 8 bits
            //  OUT_Y_L    (0x2A)  - Y axis low 8 bits
            //  OUT_Y_H    (0x2B)  - Y axis high 8 bits
            //  OUT_Z_L    (0x2C)  - Z axis low 8 bits
            //  OUT_Z_H    (0x2D)  - Z axis high 8 bits
            //
            uint8_t buf[] = { static_cast<uint8_t>(0x28) };
            readingRegister = 0x28;
            stats.tReadStarted = time_us_64();
            i2c->Read(buf, 1, 6);

            // new transaction started
            return true;
        }
    }
    else
    {
        // No interrupt signal is available.  Determine if a sample is
        // available by reading the ZYXDA bit in the status register.  We
        // know the approximate time the previous sample became ready, and
        // how long each sample takes, so we can skip the status read
        // (letting other devices have their next turn at the I2C bus
        // sooner) if it's much too early for a new sample to be ready.
        // Check a *little* early, since we can't know precisely when the
        // prior sample became available; we only know when we tried reading
        // it, which is always delayed from its actual ready time by however
        // long it took us to come to this point in the main task loop,
        // which averages to half of the task loop time.  That varies
        // unpredictably according to the work done elsewhere in the loop,
        // so let's keep this simple and just start checking 1ms early.
        if (time_us_64() > timestamp + sampleTime_us - 1000)
        {
            // A new sample could be ready, based on elapsed time since the
            // last sample.  Read the status register to find out for sure.
            //
            // We CAN't do the status + data registers read as a batch.  That
            // would cause a race condition with the hardware that would make
            // us miss some reads, because reading the data registers resets
            // the ZYXDA bit in the status register automatically.  So if we
            // did the STATUS/X/Y/Z read as a single batch, we could catch it
            // at a moment where the status is still showing "not ready", but
            // then a sample *becomes* ready just after the status read,
            // setting the ZYXDA bit just after we read status.  But then our
            // transaction would continue, reading the X/Y/Z registers, which
            // would clear the newly set ZYXDA bit.  We'd see the "not ready"
            // status and discard these readings, but then the next time we
            // checked, the status would still say "not ready".  So we'd miss
            // this reading entirely.  That could continue pathologically for
            // an indefinite number of cycles (although it's highly unlikely,
            // since we have to catch it at just the right moment to trigger
            // the race condition).  To do this properly, we have to first
            // read the status register, and continue to the X/Y/Z registers
            // only if the status shows that a sample is ready.  That
            // guarantees that we'll never accidentally clear the sample bit
            // without actually reading the associated sample.
            //
            // (It's still possible to lose samples with this approach,
            // because we could simply be too slow in going through the main
            // loop to catch every sample.  That's acceptable, since if the
            // main loop can't keep up with the hardware sample rate as far as
            // collecting the samples, it *also* wouldn't be able to process
            // the samples, so it might as well skip them at the I2C
            // collection level.  As long as the main loop runs faster than
            // the sample clock, though, we'll never miss a sample.)
            uint8_t buf[] = { static_cast<uint8_t>(0x27) };
            readingRegister = 0x27;
            i2c->Read(buf, 1, 1);

            // new transaction started
            return true;
        }
    }

    // no transaction started
    return false;
}

void LIS3DSH::OnI2CCompletionIRQ(const uint8_t *data, size_t len, I2CX *i2c)
{
    // if it's a status read, check for data ready
    if (readingRegister == 0x27 && len == 1)
    {
        // check XYZDA (bit 0x08) to see if a sample is ready
        if ((data[0] & 0x08) != 0)
        {
            // A sample is ready - read the X/Z/Y OUT registers.  We only
            // get here when we're operating in non-interrupt mode (i.e.,
            // the chip's INT1 line isn't connected to a Pico GPIO), so we
            // don't have a true asynchronous signal that tells us precisely
            // when the sample became available.  We at least know that
            // "right now", minus the I2C transmission time for the 8-bit
            // status register, is the upper bound for when the sample
            // became available, so record that as the "interrupt" time.
            // We'll just stipulate that the I2C transfer time and IRQ
            // service time add up to about 25us.
            stats.tIntr = time_us_64() - 25;

            // Start the X/Y/Z OUT register read
            uint8_t buf[] = { static_cast<uint8_t>(0x28) };
            readingRegister = 0x28;
            stats.tReadStarted = time_us_64();
            i2c->Read(buf, 1, 6);
        }
    }
}

// I2C read completed
bool LIS3DSH::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // check which register we're reading
    switch (readingRegister)
    {
    case 0x28:
        // OUT_X_L et seq - accelerometer axis registers.
        // We read all three registers, 2 bytes each, so we expect
        // 6 bytes in the response.
        if (len == 6)
        {
            // Retrieve the new axis data.  The axis values are encoded
            // as 16-bit signed ints (2's complement format), low byte
            // first.
            static auto GetINT16 = [](const uint8_t *buf) {
                uint16_t ui = (static_cast<uint16_t>(buf[1]) << 8) | buf[0];
                return static_cast<int16_t>(ui);
            };
            x = GetINT16(&data[0]);
            y = GetINT16(&data[2]);
            z = GetINT16(&data[4]);
            
            // save the X/Y/Z output registers, for diagnostics
            memcpy(outReg, &data[0], sizeof(outReg));
            
            // use the timestamp from the last interrupt (if available)
            uint64_t now = time_us_64();
            if (stats.tIntr != 0)
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
            if (stats.tLastSample != 0)
                stats.timeBetweenReads += now - stats.tLastSample;
            stats.tLastSample = now;
            
            // Note that reading the X/Y/Z registers has the side effect of
            // clearing the interrupt status, so there's no need for any
            // additional action to reset the IRQ line.
            
            // Take a temperature reading once a second.  We only do this at
            // these fairly long intervals because it costs an extra I2C
            // transaction (since the ADC registers aren't adjacent to the
            // axis readouts in the sensor's register file), and since we
            // really don't need frequent temperature updates.  Doing this
            // extra transaction infrequently won't add any meaningful I2C
            // bandwidth load.
            if (now > temperatureTimestamp + 1000000)
            {
                // read OUT_T, which holds the temperature reading
                uint8_t buf[] = { 0x0C };
                i2c->Read(buf, 1, 1);
                readingRegister = 0x0C;
                
                // new transaction started
                return true;
            }
        }
        break;

    case 0x27:
        // Status read.  We handle this in the IRQ handler, so this shouldn't
        // be reached.
        break;

    case 0x0C:
        // OUT_T - temperature output
        if (len == 1)
        {
            // Interpret the temperature.  This an 8-bit signed integer
            // representing the temperature delta from 25 C in units of 1 C.
            tempReg[0] = data[0];
            temperatureInt8 = static_cast<int8_t>(data[0]);

            // interpret the temperature into normalized INT16 units
            static auto INT8ToINT16 = [](int16_t x) -> int16_t {
                return x < 0 ?
                    (--x, ((x << 8) | ((x & 0x07F) << 1) | ((x & 0x40) >> 6)) + 1) :
                    ((x << 8) | ((x & 0x7F) << 1) | ((x & 0x40) >> 6));
            };
            temperatureInt16 = INT8ToINT16(temperatureInt8);
            
            // Interpret the temperature into degrees C, in units of 1/100 C
            temperatureC = temperatureInt8*100 + 2500;

            // update the temperature timestamp
            temperatureTimestamp = time_us_64();
        }
        break;
    }

    // no new transaction started
    return false;
}

// read the last sample
void LIS3DSH::Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp)
{
    x = this->x;
    y = this->y;
    z = this->z;
    timestamp = this->timestamp;
}

// command console status command
void LIS3DSH::Command_info(const ConsoleCommandContext *c)
{
    if (inst == nullptr)
        return c->Print("LIS3DSH not configured\n");

    static const auto ShowStats = [](const ConsoleCommandContext *c)
    {
        auto &s = inst->stats;
        c->Printf(
            "LIS3DSH status:\n"
            "Number of IRQs serviced:    %llu\n"
            "Number of samples read:     %llu\n"
            "IRQs without sample reads:  %llu (%.2lf%%)\n"
            "Average read interval:      %.2f ms\n"
            "Average read latency:       %lu us\n"
            "X,Y,Z (Native/Norm INT16):  %d,%d,%d\n"
            "Sample reg bytes [28-2D]:   %02X.%02X %02X.%02X %02X.%02X\n"
            "Temperature:                %.2f C (%.2f F)\n"
            "Temperature reg [0C]:       %02X (INT8 %d)\n",
            s.nIntr, s.nReads,
            s.nIntr <= s.nReads ? 0ULL : s.nIntr - s.nReads,
            s.nIntr <= s.nReads ? 0.0 : static_cast<double>(s.nIntr - s.nReads)/static_cast<double>(s.nIntr)*100.0,
            static_cast<float>(s.nReads != 0 ? s.timeBetweenReads / s.nReads : 0) / 1000.0f,
            static_cast<uint32_t>(s.nReads != 0 ? s.i2cLatency / s.nReads : 0),
            inst->x, inst->y, inst->z,
            inst->outReg[0], inst->outReg[1], inst->outReg[2], inst->outReg[3], inst->outReg[4], inst->outReg[5],
            inst->temperatureC/100.0f, inst->temperatureC/100.0f * 9.0f/5.0f + 32.0f,
            inst->tempReg[0], inst->temperatureInt8);
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
            IRQDisabler d;
            inst->stats.Reset();
            c->Print("LIS3DSH statistics counters reset\n");
        }
        else
        {
            return c->Usage();
        }
    }
}

