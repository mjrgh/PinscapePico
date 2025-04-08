// Pinscape Pico firmware - MXC6655XA Accelerometer chip interface
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
#include "USBIfc.h"
#include "CommandConsole.h"
#include "MXC6655XA.h"

// singleton instance
MXC6655XA *MXC6655XA::inst = nullptr;

// construction
MXC6655XA::MXC6655XA(uint16_t addr, int gpInterrupt) :
    I2CDevice(addr),
    gpInterrupt(gpInterrupt)
{
    // add our console command
    CommandConsole::AddCommand(
        "mxc6655xa", "show MXC6655XA accelerometer status",
        "mxc6655xa [options]\n"
        "options:\n"
        "   -s, --stats         show statistics (default if no arguments are provided)\n"
        "   -r, --reset-stats   reset statistics counters\n",
        &Command_info);
}

// Configure from JSON data
//
// mxc6655xa: {
//   i2c: <number>,          // I2C bus number (0 or 1) 
//                           // note - MXC6655XA has a fixed I2C address (0x15), so there's no need to specify it in the JSON
//   interrupt: <gpio>,      // GPIO port connected to the chip's interrupt (INT) pin, if any
//   gRange: <number>,       // dynamic range, in units of Earth's gravity ("g"), 2, 4, or 8
// }
//
// The interrupt GPIO connection is optional, both in the configuration
// and physically in the board wiring.  If there's no interrupt GPIO,
// we'll simply poll the chip whenever we have a chance (i.e., when it's
// our turn for I2C bus access).  The software is more efficient if the
// interrupt line is configured, because it can tell from the interrupt
// signal when the chip has nothing new available, and can thus skip
// unnecessary I2C polling.  
void MXC6655XA::Configure(JSONParser &json)
{
    if (auto *val = json.Get("mxc6655xa") ; !val->IsUndefined())
    {
        // get and validate the I2C bus number
        int bus = val->Get("i2c")->Int(-1);
        if (!I2C::ValidateBusConfig("mxc6655xa", bus))
            return;

        // the I2C address on this chip isn't configurable - it's
        // hard-wired on the chip to 0x15
        const uint16_t addr = 0x15;

        // Get the interrupt pin, if any
        int gpIntr = val->Get("interrupt")->Int(-1);
        if (gpIntr != -1)
        {
            // validate it
            if (!IsValidGP(gpIntr))
            {
                Log(LOG_ERROR, "mxc6655xa: invalid interrupt GPIO port\n");
                return;
            }

            // Assign the pin as an input; the physical pin on the chip is
            // open-drain, so it needs a pull-up.  (The open-drain
            // configuration allows the interrupt signal to be shared by
            // multiple devices, to conserve GPIO pins on the MCU.)
            if (!gpioManager.ClaimSharedInput("MXC6655XA (Interrupt)", gpIntr, true, false, false))
                return;

            // configure the IRQ handler - the interrupt signal from the
            // sensor is active-low, so trigger on the falling edge
            gpio_add_raw_irq_handler(gpIntr, &MXC6655XA::IRQ);
            gpio_set_irq_enabled(gpIntr, GPIO_IRQ_EDGE_FALL, true);
            irq_set_enabled(IO_IRQ_BANK0, true);
        }

        // create the singleton
        inst = new MXC6655XA(addr, gpIntr);

        // get the dynamic range
        inst->gRange = val->Get("gRange")->Int(2);
        if (inst->gRange != 2 && inst->gRange != 4 && inst->gRange != 8)
        {
            Log(LOG_ERROR, "mxc6655xa: invalid gRange range value %d; must be 2, 4, or 8; using 2\n", inst->gRange);
            inst->gRange = 2;
        }

        // initialize it
        inst->Init(i2c_get_instance(bus));

        // add it to the I2C bus manager for the selected bus
        I2C::GetInstance(bus, false)->Add(inst);
    }
}

// initialize
void MXC6655XA::Init(i2c_inst_t *i2c)
{
    // send initialization commands
    SendInitCommands(i2c);

    // Register the device
    accelerometerRegistry.Register(this);

    // Add a HID logical axis for the sensor's temperature output
    class MXC6655XATemperatureLogicalAxis : public LogicalAxis
    {
    public:
        MXC6655XATemperatureLogicalAxis() { }
        virtual int16_t Read() override { return MXC6655XA::inst->temperatureInt16; }
    };
    LogicalAxis::AddSource("mxc6655xa.temperature",
        [](const LogicalAxis::CtorParams&, std::vector<std::string>&) -> LogicalAxis* { return new MXC6655XATemperatureLogicalAxis(); });
}

void MXC6655XA::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst());
}

void MXC6655XA::SendInitCommands(i2c_inst_t *i2c)
{
    // execute a software reset
    bool ok = true;
    {
        uint8_t buf[] = { 0x01, 0x10 };
        ok = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf);
    }

    // wait for the 'ORD' bit (firmware initialization data read) in register 0x02 to go high
    uint64_t timeout = time_us_64() + 15000;
    bool ord = false;
    while (time_us_64() < timeout)
    {
        // read register 0x02
        uint8_t buf[] = { 0x02 }, rdbuf;
        ok = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf)
             && i2c_read_timeout_us(i2c, i2cAddr, &rdbuf, 1, false, 1000) == 1;

        // stop on error
        if (!ok)
            break;

        // check ORD
        if ((rdbuf & 0x10) != 0)
        {
            ord = true;
            break;
        }
    }
    if (!ord)
        Log(LOG_ERROR, "MXC6655XA: %s waiting for ORD (OTP read) status\n", ok ? "timeout" : "I2C error");

    // read the WHO_AM_I register to confirm the device is responding
    {
        // read register 0x0F (WHO_AM_I)
        uint8_t buf[] = { 0x0F }, whoAmI;
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf))
             && (i2c_read_timeout_us(i2c, i2cAddr, &whoAmI, 1, false, 1000) == 1)
             && ok;

        if (ok)
            Log(LOG_INFO, "MXC6655XA: WHO_AM_I=0x%02x (%s)\n", whoAmI, (whoAmI & 0x0F) == 0x05 ? "OK" : "expected 0xX5");
    }

    // CONTROL register settings:
    //   ST  (0x80)  Self-test mode    - off
    //   FSR (0x60)  Full-scale range  - 2g = b00, 4g = b01, 8g = b10
    //   PD  (0x01)  power-down        - off
    uint8_t control = (gRange == 4 ? 0x20 : gRange == 8 ? 0x40 : 0x00);

    // write the CONTROL register setting (register 0x0D)
    {
        uint8_t buf[] = { 0x0D, control };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // enable Data Ready (DRDY) interrupts (0x0A, 0x0B)
    {
        // reg 0x0A INT_MASK0   ORZE ORYE x x SHYME SHYPE SHXME SHXPE -> all zeroes
        // reg 0x0B INT_MASK1   TC - 0 0 - - - DRDYE -> enable DRDY
        const uint8_t buf[] = { 0x0A, 0x00, 0x01 };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    }

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "MXC6655XA device initialization %s\n", ok ? "OK" : "failed");
}

void MXC6655XA::Task()
{
}

void MXC6655XA::IRQ()
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
bool MXC6655XA::OnI2CReady(I2CX *i2c)
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
        // Read registers:
        //
        //  Name     (Addr)  - Description
        //
        //  INT_SRC1 (0x01)  - to get the DRDY bit (data ready)
        //  STATUS   (0x02)  - ignore
        //  XOUT_HI  (0x03)  - X axis upper 8 bits
        //  XOUT_LO  (0x04)  - X axis lower 4 bits
        //  YOUT_HI  (0x05)  - Y axis upper 8 bits
        //  YOUT_LO  (0x06)  - Y axis lower 4 bits
        //  ZOUT_HI  (0x07)  - Z axis upper 8 bits
        //  ZOUT_LO  (0x08)  - Z axis lower 4 bits
        //  TOUT     (0x09)  - temperature reading, 8 bits
        //
        // Note that we don't need the STATUS register; we include it
        // because it only adds one byte to the transfer, which is faster
        // than starting a separate transfer to read the two register
        // ranges.
        uint8_t buf[] = { 0x01 };
        i2c->Read(buf, 1, 9);

        // new transaction started
        return true;
    }

    // no transaction started
    return false;
}

// I2C read completed
bool MXC6655XA::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we expect 9 bytes back for our register read
    if (len == 9)
    {
        // Check the DRDY bit in the status register - it's set if the
        // device has new data for us to read.  We have to check the
        // DRDY bit on each read for two reasons: first, we can be
        // configured without a GPIO connection to the chip's interrupt
        // line, in which case we have to poll the device to find out
        // when new readings are available; and second, even if we do
        // have GPIO access to the interrupt signal, the interrupt line
        // could be shared with other chips, so the interrupt might not
        // be coming from the accelerometer at all.  (What's more, the
        // chip can be configured to signal interrupts for several
        // conditions besides Data Ready, so even if we have an
        // interrupt and even if it's coming from the accelerometer, it
        // might not mean Data Ready.  We don't currently enable any of
        // the chip's other interrupt conditions, so this one doesn't
        // currently apply, but it could in the future if we were to
        // want to take advantage of other features of the chip.  This
        // is a bit of future-proofing that we get for free because we
        // already have to check DRDY for the other two reasons.)
        if ((data[0] & 0x01) != 0)
        {
            // Retrieve the new axis data.  The axis values are encoded
            // as 12-bit signed ints (2's complement format).  Each
            // 12-bit value is left-justified in a 16-bit field,
            // arranged in the buffer as consecutive bytes in big-endian
            // order.  To extract the numerical value, assemble the 12
            // bits in an INT16 and divide by 16.  (We divide by 16
            // rather than shifting right by 4 bits because the effect
            // of '>>' on a negative value is implementation-defined in
            // C++.  Division is well defined, and hopefully the
            // compiler will optimize it to an arithmetic right-shift
            // anyway, which *is* well defined on any given target
            // platform.)
            static auto Get16 = [](const uint8_t *buf) {
                uint16_t w = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
                return static_cast<int16_t>(w) / 16;
            };
            xRaw = Get16(&data[2]);
            yRaw = Get16(&data[4]);
            zRaw = Get16(&data[6]);

            // save the X/Y/Z output registers, for diagnostics
            memcpy(outReg, &data[2], sizeof(outReg));

            // Normalize to the full 16-bit signed range, using the
            // "shift-and-fill" algorithm, which is basically: shift left,
            // and fill the vacated low-order bits with the same number of
            // bits from the high-order end of the reading.  For signed
            // values such as we're using, we have to elaborate this
            // slightly, by decrementing negative values before the shift-
            // and-fill operation and incrementing after.
            //
            // The shift-and-fill scheme is pretty obtuse, but it works out
            // to be within +/-1 of the floating-point equivalent, which is
            // much slower to compute:
            //
            //   static_cast<int16_t>(static_cast<float>(x) * 32767.0f/2047.0f)
            //
            // Note that there's a slight anomaly at x=-2048, because of the
            // asymmetry of the 2's complement positive and negative ranges.
            // -2047 -> -32767, and -2048 -> -32768, so the step size
            // between these two last steps is only 1, rather than the ~15
            // step throughout the rest of the range.  That's necessary to
            // avoid overflow, so it's equivalent to clipping the value at
            // the extreme.  It shouldn't have much effect on the quality of
            // the signal, since an accelerometer reading right at the limit
            // is probably already being clipped at the physical sensor, so
            // we're not losing any information that wasn't already gone.
            static auto INT12ToINT16 = [](int16_t x) -> int16_t {
                return x < 0 ?
                    (--x, ((x << 4) | ((x & 0x07FF) >> 7)) + 1) :
                    ((x << 4) | ((x & 0x07FF) >> 7));
            };
            x = INT12ToINT16(xRaw);
            y = INT12ToINT16(yRaw);
            z = INT12ToINT16(zRaw);

            // Retrieve the temperature, in normalized INT16 units, and
            // in 1000ths of degrees C.  The latter unit system 
            // because the native unit from the chip is 0.568 C, with a
            // zero point at 25 C.  Multiplying by 100 lets us retain
            // the half-degree native precision without a float
            // conversion.)
            static auto INT8ToINT16 = [](int x) {
                return x < 0 ?
                    (--x, ((x << 8) | ((x & 0x7F) << 1)) + 2) :
                    ((x << 8) | ((x & 0x07F) << 1));
            };
            temperatureInt16 = INT8ToINT16(static_cast<int8_t>(data[8]));
            temperature = (static_cast<int>(static_cast<int8_t>(data[8])) * 586) + 25000;

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
            
            // clear the DRDY interrupt bit by writing the DRDY
            // bit (0x01) to the INT_CLR1 register (0x01)
            uint8_t buf[] = { 0x01, 0x01 };
            i2c->Write(buf, _countof(buf));

            // new transaction started
            return true;
        }
    }

    // no new transaction started
    return false;
}

// read the last sample
void MXC6655XA::Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp)
{
    x = this->x;
    y = this->y;
    z = this->z;
    timestamp = this->timestamp;
}

// command console status command
void MXC6655XA::Command_info(const ConsoleCommandContext *c)
{
    if (inst == nullptr)
        return c->Print("MXC6655XA not configured\n");

    static const auto ShowStats = [](const ConsoleCommandContext *c)
    {
        auto &s = inst->stats;
        c->Printf(
            "MXC6655XA status:\n"
            "Number of IRQs serviced:   %llu\n"
            "Number of samples read:    %llu\n"
            "Average read interval:     %.2f ms\n"
            "Average read latency:      %lu us\n"
            "Samples missed:            %llu (1 in %llu)\n"
            "Temperature reading:       %.3lf C (%.3lf F)\n"
            "Sample (X,Y,Z)(INT16):     %d,%d,%d\n"
            "Raw (X,Y,Z)(INT12):        %d,%d,%d\n"
            "Sample reg bytes [3-8]:    %02X.%02X %02X.%02X %02X.%02X\n",
            s.nIntr, s.nReads,
            static_cast<float>(s.nReads != 0 ? s.timeBetweenReads / s.nReads : 0) / 1000.0f,
            static_cast<uint32_t>(s.nReads != 0 ? s.i2cLatency / s.nReads : 0),
            s.nMissedSamples, s.nMissedSamples == 0 ? 0 : s.nIntr / s.nMissedSamples,
            inst->GetTemperatureC(), inst->GetTemperatureF(),
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
            auto &s = inst->stats;
            s.nReads = 0;
            s.timeBetweenReads = 0;
            s.i2cLatency = 0;
            c->Print("MXC6655XA statistics counters reset\n");
        }
        else
            return c->Usage();
    }
}

