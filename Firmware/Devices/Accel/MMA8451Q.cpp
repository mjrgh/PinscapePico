// Pinscape Pico firmware - MMA8451Q Accelerometer chip interface
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
#include "MMA8451Q.h"

// singleton instance
MMA8451Q *MMA8451Q::inst = nullptr;

// construction
MMA8451Q::MMA8451Q(uint16_t addr, int gpInterrupt) :
    I2CDevice(addr),
    gpInterrupt(gpInterrupt)
{
    // add our console command
    CommandConsole::AddCommand(
        "mma8451q", "show MMA8451Q accelerometer status",
        "mma8451q [options]\n"
        "options:\n"
        "   -s, --stats         show statistics (default if no arguments are provided)\n"
        "   -r, --reset-stats   reset statistics counters\n",
        &Command_info);
}

// Configure from JSON data
//
// mma8451q: {
//   i2c: <number>,          // I2C bus number (0 or 1) 
//   addr: <number>          // I2C address, 0x1C or 0x1D (set by SA0 pin: GND -> 0x1C, VDD -> 0x1D)
//   interrupt: <gpio>,      // GPIO port connected to the chip's Interrupt 1 (INT1) pin, if any
//   gRange: <number>,       // dynamic range, in units of Earth's gravity ("g"): 2, 4, or 8
// }
//
// The interrupt GPIO connection is optional, both in the configuration
// and physically in the board wiring.  If there's no interrupt GPIO,
// we'll simply poll the chip whenever we have a chance (i.e., when it's
// our turn for I2C bus access).  The software is more efficient if the
// interrupt line is configured, because it can tell from the interrupt
// signal when the chip has nothing new available, and can thus skip
// unnecessary I2C polling.  
void MMA8451Q::Configure(JSONParser &json)
{
    if (auto *val = json.Get("mma8451q") ; !val->IsUndefined())
    {
        // get and validate the I2C bus number
        int bus = val->Get("i2c")->Int(-1);
        if (!I2C::ValidateBusConfig("mma8451q", bus))
            return;

        // get and validate the I2C address
        int addr = val->Get("addr")->Int(-1);
        if (addr != 0x1C && addr != 0x1D)
        {
            Log(LOG_ERROR, "mma8451q: invalid I2C address 0x%02X; must be 0x1C or 0x1D\n", addr);
            return;
        }

        // Get the GPIO connected to the interrupt (INT1) pin, if any
        int gpIntr = -1;
        if (auto valIntr = val->Get("interrupt") ; !valIntr->IsUndefined())
        {
            // validate it
            gpIntr = valIntr->Int(-1);
            if (!IsValidGP(gpIntr))
            {
                Log(LOG_ERROR, "mma8451q: invalid interrupt GPIO port\n");
                return;
            }

            // Assign the pin as an input.  We'll configure the interrupt
            // line on the chip as open-drain (it can be OD or push-pull),
            // to allow sharing the interrupt input with other chips that
            // also provide OD INT lines.  With OD, we need a pull-up on
            // the port.  Also enable the Schmitt trigger (hysteresis) to
            // minimize false edge recognition.
            if (!gpioManager.ClaimSharedInput("MMA8451Q (Interrupt)", gpIntr, true, false, true))
                return;

            // configure the IRQ handler - the interrupt signal from the
            // sensor is active-low, so triger on the falling edge
            gpio_add_raw_irq_handler(gpIntr, &MMA8451Q::IRQ);
        }

        // create the singleton
        inst = new MMA8451Q(addr, gpIntr);

        // get the dynamic range
        inst->gRange = val->Get("gRange")->Int(2);
        if (inst->gRange != 2 && inst->gRange != 4 && inst->gRange != 8)
        {
            Log(LOG_ERROR, "mma8451q: invalid gRange range value %d: must be 2, 4, 8; defaulting to 2\n", inst->gRange);
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
void MMA8451Q::Init(i2c_inst_t *i2c)
{
    // The device boots within 500us of power-on (per the data sheet,
    // see Boot Time (Tbt)).  To be sure the device is ready, wait
    // until the Pico clock reads 1000us; the Pico reset time should
    // be a good lower bound for how long system power has been on.
    while (time_us_64() < 1000)
    {
        watchdog_update();
        sleep_us(100);
    }

    // send initialization commands
    SendInitCommands(i2c, true);
    
    // Register the device in the active accelerometer list
    accelerometerRegistry.Register(this);
}

void MMA8451Q::SendInitCommands(i2c_inst_t *i2c, bool isPowerOn)
{
    // read the WHO_AM_I register to confirm the device is responding
    bool ok = true;
    {
        // read register 0x0F (WHO_AM_I)
        uint8_t buf[] = { 0x0D }, whoAmI;
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf))
             && (i2c_read_timeout_us(i2c, i2cAddr, &whoAmI, 1, false, 1000) == 1)
             && ok;

        if (ok)
        {
            switch (whoAmI)
            {
            case 0x1A:
                Log(LOG_INFO, "MMA8451Q: WHO_AM_I (reg 0x0F)=0x1A, chip identified as MMA8451Q\n");
                break;

            case 0x2A:
                Log(LOG_INFO, "MMA8451Q: WHO_AM_I (reg 0x0F)=0x2A, chip identified as MMA8452Q\n");
                break;

            default:
                Log(LOG_ERROR, "MMA8451Q: WHO_AM_I (reg 0x0F)=0x%02X, expected 0x1A or 0x2A; this is not a supported "
                    "MMA845x series chip ID, so the device might not function correctly\n", whoAmI);
                break;
            }
        }
        else
        {
            Log(LOG_ERROR, "MMA8451Q: WHO_AM_I read request failed\n");
        }
    }

    // Set ACTIVE/STANDBY mode
    auto SetActive = [i2c, this](bool active)
    {
        // read CTRL_REG1 to get the current values of the rest of the bits
        const char *modeName = active ? "ACTIVE" : "STANDBY";
        uint8_t buf[2]{ 0x2A };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, 1, true, 1000) != 1
            || i2c_read_timeout_us(i2c, i2cAddr, &buf[1], 1, false, 1000) != 1)
        {
            Log(LOG_ERROR, "MMA8451Q: Error reading CTRL_REG1 (setting %s mode)\n", modeName);
            return false;
        }

        // set the new ACTIVE mode bit (register 0x2A, bit 0x01 - 0=Standby, 1=Active)
        uint8_t newBit = active ? 0x01 : 0x00;
        buf[1] = (buf[1] & ~0x01) | newBit;
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, 2, false, 1000) != 2)
        {
            Log(LOG_ERROR, "MMA8451Q: Error writing CTRL_REG1 (setting %s mode)\n", modeName);
            return false;
        }
        
        // wait for the mode bit to stick
        uint64_t tTimeout = time_us_64() + 5000;
        for (;;)
        {
            // read the new mode
            if (i2c_write_timeout_us(i2c, i2cAddr, buf, 1, true, 1000) != 1
                || i2c_read_timeout_us(i2c, i2cAddr, &buf[1], 1, false, 1000) != 1)
            {
                Log(LOG_ERROR, "MMA8451Q: Error reading CTRL_REG1 (waiting for %s mode to take effect)\n", modeName);
                return false;
            }
            
            // check for the register bit to match the requested status
            if ((buf[1] & 0x01) == newBit)
                return true;
            
            // check for timeout
            if (time_us_64() > tTimeout)
            {
                Log(LOG_ERROR, "MMA8451Q: Timed out waiting for %s mode to take effect\n", modeName);
                return false;
            }
            
            // wait a bit for the change to stick
            watchdog_update();
            sleep_us(100);
        }
    };

    // Set STANDBY mode - this is required before we can update the control registers
    if (!SetActive(false))
        ok = false;

    // Set CTRL_REG1 (0x2A):
    //
    //     ASLP_RATE   0xC0  -> 0  50Hz (default)
    //     DATA_RATE   0x38  -> 0  800Hz
    //     LNOISE      0x04  -> 1  (Reduced noise mode) ONLY IF gRange < 4, else 0 (Normal mode)
    //     F_READ      0x02  -> 0  fast read mode disabled (read full-resolution 14-bit samples)
    //     ACTIVE      0x01  -> 0  stay in standby while configuring
    {
        uint8_t buf[] = { 0x2A, static_cast<uint8_t>(gRange <= 4 ? 0x04 : 0x00) };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "MMA8451Q: CTRL_REG1 write failed\n");
            ok = false;
        }
    }

    // Set CTRL_REG2 (0x2B):
    //
    //     ST          0x80  -> 0  self-test disabled
    //     RST         0x40  -> 0  no reset
    //     SMODS       0x18  -> 10 sleep-mode power scheme = high-resolution
    //     SLPE        0x04  -> 0  auto-sleep disabled
    //     MODS        0x03  -> 10 active mode power scheme = high-resolution
    {
        uint8_t buf[] = { 0x2B, 0x12 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "MMA8451Q: CTRL_REG2 write failed\n");
            ok = false;
        }
    }

    // Set CTRL_REG3 (0x2C):
    //
    //     FIFO_GATE   0x80  -> 0  bypass
    //     WAKE_TRANS  0x40  -> 0  bypass in SLEEP
    //     WAKE_LNDPRT 0x20  -> 0  bypass in sleep
    //     WAKE_PULSE  0x10  -> 0  bypass in sleep
    //     WAKE_FF_MT  0x04  -> 0  bypass in sleep
    //     IPOL        0x02  -> 0  active low
    //     PP_OD       0x01  -> 1  open-drain
    {
        uint8_t buf[] = { 0x2C, 0x01 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "MMA8451Q: CTRL_REG3 write failed\n");
            ok = false;
        }
    }

    // Set CTRL_REG4 (0x2D):
    //
    //     INT_EN_ASLP   0x80  -> 0  auto-sleep interrupt disabled
    //     INT_EN_FIFO   0x40  -> 0  FIFO interrupt disabled
    //     INT_EN_TRANS  0x20  -> 0  transient interrupt disabled
    //     INT_EN_LNDPRT 0x10  -> 0  landscape/portrait interrupt disabled
    //     INT_EN_PULSE  0x08  -> 0  pulse detection interrupt disabled
    //     INT_EN_FF_MT  0x04  -> 0  freefall/motion interrupt disabled
    //     INT_EN_DRDY   0x01  -> 1  Data Ready interrupt enabled
    {
        uint8_t buf[] = { 0x2D, 0x01 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "MMA8451Q: CTRL_REG4 write failed\n");
            ok = false;
        }
    }

    // Set CTRL_REG5 (0x2E):
    //
    //     INT_CFG_ASLP   0x80  -> 0  auto-sleep interrupt -> INT2
    //     INT_CFG_FIFO   0x40  -> 0  FIFO interrupt -> INT2
    //     INT_CFG_TRANS  0x20  -> 0  transient interrupt -> INT2
    //     INT_CFG_LNDPRT 0x10  -> 0  landscape/portrait interrupt -> INT2
    //     INT_CFG_PULSE  0x08  -> 0  pulse detection interrupt -> INT2
    //     INT_CFG_FF_MT  0x04  -> 0  freefall/motion interrupt -> INT2
    //     INT_CFG_DRDY   0x01  -> 1  Data Ready interrupt -> INT1
    {
        uint8_t buf[] = { 0x2E, 0x01 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "MMA8451Q: CTRL_REG5 write failed\n");
            ok = false;
        }
    }

    // Set XYZ_DATA_CFG (register 0x0E):
    //
    //   HPF_OUT    0x10  -> 0  high-pass filter enabled
    //   FS         0x03  -> 00=2g, 01=4g, 10=8g
    {
        uint8_t buf[] = { 0x09, static_cast<uint8_t>(gRange == 8 ? 0x02 : gRange == 4 ? 0x01 : 0x00) };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "MMA8451Q: XYZ_DATA_CFG write failed\n");
            ok = false;
        }
    }

    // Set ACTIVE mode
    if (!SetActive(true))
        ok = false;

    // log the result
    char intrDesc[8];
    sprintf(intrDesc, gpInterrupt == -1 ? "not connected" : "GP%d", gpInterrupt);
    Log(ok ? LOG_CONFIG : LOG_ERROR, "MMA8451Q device initialization %s; I2C%d address 0x%02X, interrupt %s, dynamic range +/- %dg\n",
        ok ? "OK" : "failed", i2c_hw_index(i2c), i2cAddr, intrDesc, gRange);
}

void MMA8451Q::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst(), false);
}

void MMA8451Q::Task()
{
}

void MMA8451Q::IRQ()
{
    // check the interrupt
    if ((gpio_get_irq_event_mask(inst->gpInterrupt) & GPIO_IRQ_EDGE_FALL) != 0)
    {
        // record the time the sample became ready, if we haven't already
        // timestamped it
        uint64_t now = time_us_64();
        if (inst->stats.tIntr == 0)
            inst->stats.tIntr = now;

        // count the interrupt
        inst->stats.nIntr += 1;
        
        // acknowledge the IRQ
        gpio_acknowledge_irq(inst->gpInterrupt, GPIO_IRQ_EDGE_FALL);
    }
}

// I2C bus available for our use
bool MMA8451Q::OnI2CReady(I2CX *i2c)
{
    // If we have an interrupt input configured, only check for a new
    // sample when the interrupt line is low.  If the interrupt input
    // isn't configured, check for a sample when enough time has elapsed
    // that a new sample should be ready, starting a little early, since
    // it takes a non-zero time to set up and transmit the I2C request.
    //
    // In either case, start by checking the DRDY status bit to see if a
    // sample is ready; even if we have an interrupt input, we still
    // have to check ZYXDR, since the interrupt line could be shared with
    // other chips, hence the interrupt signal could be coming from one
    // of the ohter chips.
    if (gpInterrupt >= 0 ?
        !gpio_get(gpInterrupt) :
        (time_us_64() > timestamp + sampleTime_us - 500))
    {
        // set up a read on STATUS (register 0x00)
        uint8_t buf[] = { 0x00 };
        readingRegister = 0x00;
        stats.tReadStarted = time_us_64();
        i2c->Read(buf, 1, 1);

        // new transaction started
        return true;
    }

    // no transaction started
    return false;
}

void MMA8451Q::OnI2CCompletionIRQ(const uint8_t *data, size_t len, I2CX *i2c)
{
    // if it's a status read, check for data ready
    if (readingRegister == 0x00 && len == 1)
    {
        // check XYZDR (bit 0x08) to see if a sample is ready
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
            //
            //  OUT_X_MSB    (0x01)  - X axis high 8 bits
            //  OUT_X_LSB    (0x02)  - X axis low 8 bits
            //  OUT_Y_MSB    (0x03)  - Y axis low 8 bits
            //  OUT_Y_LSB    (0x04)  - Y axis high 8 bits
            //  OUT_Z_MSB    (0x05)  - Z axis low 8 bits
            //  OUT_Z_LSB    (0x06)  - Z axis high 8 bits
            //
            uint8_t buf[] = { 0x01 };
            readingRegister = 0x01;
            stats.tReadStarted = time_us_64();
            i2c->Read(buf, 1, 6);
        }
    }
}

// I2C read completed
bool MMA8451Q::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // check which register we're reading
    switch (readingRegister)
    {
    case 0x01:
        // OUT_X_MSB et seq - accelerometer axis registers.
        // We read all three registers, 2 bytes each, so we expect
        // 6 bytes in the response.
        if (len == 6)
        {
            // Retrieve the new axis data.  The axis values are encoded
            // as 14-bit signed ints, 2's complement format), high byte
            // first, left-justified in a 16-bit field.  To extract the
            // numerical value, assemble the 12 bits in an INT16 and
            // divide by 4.  (We divide rather than right-shift because
            // the effect of '>>' on a negative value is implementation-
            // defined in C++.)
            static auto GetINT14 = [](const uint8_t *buf) {
                uint16_t w = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
                return static_cast<int16_t>(w) / 4;
            };
            xRaw = GetINT14(&data[0]);
            yRaw = GetINT14(&data[2]);
            zRaw = GetINT14(&data[4]);
            
            // save the X/Y/Z output registers, for diagnostics
            memcpy(outReg, &data[0], sizeof(outReg));
            
            // Normalize to the full 16-bit signed range, using the
            // "shift-and-fill" algorithm.
            static auto INT14ToINT16 = [](int16_t x) -> int16_t {
                return x < 0 ?
                    (--x, ((x << 2) | ((x & 0x1FFF) >> 11)) + 1) :
                    ((x << 2) | ((x & 0x1FFF) >> 11));
            };
            x = INT14ToINT16(xRaw);
            y = INT14ToINT16(yRaw);
            z = INT14ToINT16(zRaw);
            
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
        }
        break;

    case 0x00:
        // Status read.  We handle this in the IRQ handler, so this shouldn't
        // be reached.
        break;
    }

    // no new transaction started
    return false;
}

// read the last sample
void MMA8451Q::Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp)
{
    x = this->x;
    y = this->y;
    z = this->z;
    timestamp = this->timestamp;
}

// command console status command
void MMA8451Q::Command_info(const ConsoleCommandContext *c)
{
    if (inst == nullptr)
        return c->Print("MMA8451Q not configured\n");

    static const auto ShowStats = [](const ConsoleCommandContext *c)
    {
        auto &s = inst->stats;
        c->Printf(
            "MMA8451Q status:\n"
            "Number of IRQs serviced:    %llu\n"
            "Number of samples read:     %llu\n"
            "IRQs without sample reads:  %llu (%.2lf%%)\n"
            "Average read interval:      %.2f ms\n"
            "Average read latency:       %lu us\n"
            "X,Y,Z (Normalized INT16):   %d,%d,%d\n"
            "X,Y,Z (Native INT14):       %d,%d,%d\n"
            "Sample reg bytes [01-06]:   %02X.%02X %02X.%02X %02X.%02X\n",
            s.nIntr, s.nReads,
            s.nIntr <= s.nReads ? 0ULL : s.nIntr - s.nReads,
            s.nIntr <= s.nReads ? 0.0 : static_cast<double>(s.nIntr - s.nReads)/static_cast<double>(s.nIntr)*100.0,
            static_cast<float>(s.nReads != 0 ? s.timeBetweenReads / s.nReads : 0) / 1000.0f,
            static_cast<uint32_t>(s.nReads != 0 ? s.i2cLatency / s.nReads : 0),
            inst->x, inst->y, inst->z,
            inst->xRaw, inst->yRaw, inst->zRaw,
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
            IRQDisabler d;
            inst->stats.Reset();
            c->Print("MMA8451Q statistics counters reset\n");
        }
        else
        {
            return c->Usage();
        }
    }
}

