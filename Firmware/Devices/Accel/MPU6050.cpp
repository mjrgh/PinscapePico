// Pinscape Pico firmware - MPU-6050 Accelerometer chip interface
// BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>

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
#include "Watchdog.h"
#include "MPU6050.h"

// MPU-6050 register addresses
namespace {
    const uint8_t REG_SMPLRT_DIV   = 0x19;     // sample rate divider
    const uint8_t REG_CONFIG       = 0x1A;     // digital low-pass filter configuration
    const uint8_t REG_ACCEL_CONFIG = 0x1C;     // accelerometer full-scale range
    const uint8_t REG_INT_PIN_CFG  = 0x37;     // interrupt pin configuration
    const uint8_t REG_INT_ENABLE   = 0x38;     // interrupt enable
    const uint8_t REG_INT_STATUS   = 0x3A;     // interrupt status; ACCEL_XOUT_H..TEMP_OUT_L follow at 0x3B..0x42
    const uint8_t REG_PWR_MGMT_1   = 0x6B;     // power management 1
    const uint8_t REG_WHO_AM_I     = 0x75;     // device ID
}

// singleton instance
MPU6050 *MPU6050::inst = nullptr;

// construction
MPU6050::MPU6050(uint16_t addr, int gpInterrupt) :
    I2CDevice(addr),
    gpInterrupt(gpInterrupt)
{
    // add our console command
    CommandConsole::AddCommand(
        "mpu6050", "show MPU-6050 accelerometer status",
        "mpu6050 [options]\n"
        "options:\n"
        "   -s, --stats         show statistics (default if no arguments are provided)\n"
        "   -r, --reset-stats   reset statistics counters\n",
        &Command_info);
}

// Configure from JSON data
//
// mpu6050: {
//   i2c: <number>,          // I2C bus number (0 or 1)
//   addr: <number>,         // 0x68 or 0x69 (set via the AD0 pin; GND -> 0x68, VDD -> 0x69); optional, default 0x68
//   interrupt: <gpio>,      // GPIO port connected to the chip's interrupt (INT) pin, if any
//   gRange: <number>,       // dynamic range, in units of Earth's gravity ("g"): 2, 4, 8, or 16
// }
//
// The interrupt GPIO connection is optional, both in the configuration
// and physically in the board wiring.  If there's no interrupt GPIO,
// we'll simply poll the chip whenever we have a chance (i.e., when it's
// our turn for I2C bus access).  The software is more efficient if the
// interrupt line is configured, because it can tell from the interrupt
// signal when the chip has nothing new available, and can thus skip
// unnecessary I2C polling.
void MPU6050::Configure(JSONParser &json)
{
    if (auto *val = json.Get("mpu6050") ; !val->IsUndefined())
    {
        // get and validate the I2C bus number
        int bus = val->Get("i2c")->Int(-1);
        if (!I2C::ValidateBusConfig("mpu6050", bus))
            return;

        // get and validate the I2C address
        int addr = val->Get("addr")->Int(0x68);
        if (addr != 0x68 && addr != 0x69)
        {
            Log(LOG_ERROR, "mpu6050: invalid address 0x%02X; must be 0x68 or 0x69\n", addr);
            return;
        }

        // Get the interrupt pin, if any
        int gpIntr = val->Get("interrupt")->Int(-1);
        if (gpIntr != -1)
        {
            // validate it
            if (!IsValidGP(gpIntr))
            {
                Log(LOG_ERROR, "mpu6050: invalid interrupt GPIO port\n");
                return;
            }

            // Assign the pin as an input.  We configure the chip's INT pin
            // as open-drain, so it needs a pull-up.  (The open-drain
            // configuration allows the interrupt signal to be shared, to
            // conserve GPIO pins on the MCU.)
            if (!gpioManager.ClaimSharedInput("MPU6050 (Interrupt)", gpIntr, true, false, false))
                return;

            // configure the IRQ handler - we configure the interrupt signal
            // as active-low, so trigger on the falling edge
            gpio_add_raw_irq_handler(gpIntr, &MPU6050::IRQ);
            gpio_set_irq_enabled(gpIntr, GPIO_IRQ_EDGE_FALL, true);
            irq_set_enabled(IO_IRQ_BANK0, true);
        }

        // create the singleton
        inst = new MPU6050(addr, gpIntr);

        // get the dynamic range
        inst->gRange = val->Get("gRange")->Int(2);
        if (inst->gRange != 2 && inst->gRange != 4 && inst->gRange != 8 && inst->gRange != 16)
        {
            Log(LOG_ERROR, "mpu6050: invalid gRange range value %d; must be 2, 4, 8, or 16; using 2\n", inst->gRange);
            inst->gRange = 2;
        }

        // initialize it
        inst->Init(i2c_get_instance(bus));

        // add it to the I2C bus manager for the selected bus
        I2C::GetInstance(bus, false)->Add(inst);
    }
}

// initialize
void MPU6050::Init(i2c_inst_t *i2c)
{
    // send initialization commands
    SendInitCommands(i2c);

    // Register the device
    accelerometerRegistry.Register(this);

    // Add a HID logical axis for the sensor's temperature output.  This
    // reports the raw TEMP_OUT register value, which is already an INT16
    // (degrees C = raw/340 + 36.53).
    class MPU6050TemperatureLogicalAxis : public LogicalAxis
    {
    public:
        MPU6050TemperatureLogicalAxis() { }
        virtual int16_t Read() override { return MPU6050::inst->temperatureRaw; }
    };
    LogicalAxis::AddSource("mpu6050.temperature",
        [](const LogicalAxis::CtorParams&, std::vector<std::string>&) -> LogicalAxis* { return new MPU6050TemperatureLogicalAxis(); });
}

void MPU6050::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst());
}

void MPU6050::SendInitCommands(i2c_inst_t *i2c)
{
    // write one register
    bool ok = true;
    auto WriteReg = [i2c, this, &ok](uint8_t reg, uint8_t val) {
        uint8_t buf[] = { reg, val };
        ok = (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf)) && ok;
    };

    // read one register
    auto ReadReg = [i2c, this](uint8_t reg, uint8_t &val) {
        return i2c_write_timeout_us(i2c, i2cAddr, &reg, 1, true, 1000) == 1
            && i2c_read_timeout_us(i2c, i2cAddr, &val, 1, false, 1000) == 1;
    };

    // The reset below takes up to 100ms, which is longer than the normal
    // run-time watchdog timeout, and this routine also runs at run time
    // when the I2C bus manager reinitializes the bus after an error.
    WatchdogTemporaryExtender wte(500);

    // Reset the device (PWR_MGMT_1 DEVICE_RESET bit), so that we start
    // from the power-on register defaults even after a soft reboot of the
    // Pico.  Wait the full 100ms reset time from the data sheet rather
    // than polling the DEVICE_RESET bit: the bit reads as clear before
    // the reset has actually finished, and writes made in that window are
    // lost (in testing, the wake-up write below was dropped, leaving the
    // chip asleep, with no samples and no interrupts).
    WriteReg(REG_PWR_MGMT_1, 0x80);
    sleep_ms(100);
    if (uint8_t pwr; ok && (!ReadReg(REG_PWR_MGMT_1, pwr) || (pwr & 0x80) != 0))
        Log(LOG_ERROR, "MPU6050: device reset did not complete\n");

    // read the WHO_AM_I register to confirm the device is responding
    if (ok && ReadReg(REG_WHO_AM_I, whoAmI))
    {
        // The MPU-6050 reports 0x68 regardless of the AD0 address setting.
        // Compatible parts and clones commonly found on "MPU-6050" breakout
        // boards report other values, but share the registers we use.
        const char *desc =
            whoAmI == 0x68 ? "MPU-6050" :
            whoAmI == 0x70 ? "MPU-6500 (compatible)" :
            whoAmI == 0x71 ? "MPU-9250 (compatible)" :
            whoAmI == 0x73 ? "MPU-9255 (compatible)" :
            "unknown device; expected 0x68 - trying it as an MPU-6050 anyway";
        Log(whoAmI == 0x68 ? LOG_INFO : LOG_WARNING, "MPU6050: WHO_AM_I (reg 0x75)=0x%02X (%s)\n", whoAmI, desc);
    }
    else
    {
        Log(LOG_ERROR, "MPU6050: WHO_AM_I read request failed\n");
        ok = false;
    }

    // Wake up (clear SLEEP), and select the X gyro PLL as the clock
    // source, which the data sheet recommends over the internal
    // oscillator for better stability.  The chip comes out of reset in
    // sleep mode, and produces no samples until this takes effect, so
    // read it back to make sure, and retry a few times if needed.
    bool awake = false;
    for (int tries = 0 ; ok && tries < 5 && !awake ; ++tries)
    {
        if (tries != 0)
            sleep_ms(10);
        WriteReg(REG_PWR_MGMT_1, 0x01);
        uint8_t pwr;
        awake = ReadReg(REG_PWR_MGMT_1, pwr) && (pwr & 0x40) == 0;
    }
    if (ok && !awake)
    {
        Log(LOG_ERROR, "MPU6050: chip did not wake from sleep mode\n");
        ok = false;
    }

    // Set the digital low-pass filter to DLPF_CFG=1 (accelerometer
    // bandwidth 184Hz, 2ms delay).  This sets the internal sampling rate
    // to 1kHz, which with a sample rate divider of 0 gives us 1000
    // samples per second.  (With the filter off, the accelerometer still
    // only produces 1kHz output, so there's nothing to gain there.)
    WriteReg(REG_SMPLRT_DIV, 0x00);
    WriteReg(REG_CONFIG, 0x01);

    // set the accelerometer full-scale range: AFS_SEL bits 4:3 = 0 (2g), 1 (4g), 2 (8g), 3 (16g)
    uint8_t afsSel = (gRange == 4) ? 1 : (gRange == 8) ? 2 : (gRange == 16) ? 3 : 0;
    WriteReg(REG_ACCEL_CONFIG, static_cast<uint8_t>(afsSel << 3));

    // Configure the INT pin:
    //   INT_LEVEL    (0x80) = 1  active low
    //   INT_OPEN     (0x40) = 1  open drain
    //   LATCH_INT_EN (0x20) = 1  hold the pin asserted until cleared (rather than a 50us pulse),
    //                            so that OnI2CReady() can see it by polling the GPIO level
    //   INT_RD_CLEAR (0x10) = 0  clear only by reading INT_STATUS, which we read with each sample
    WriteReg(REG_INT_PIN_CFG, 0xE0);

    // enable the Data Ready interrupt
    WriteReg(REG_INT_ENABLE, 0x01);

    // log status
    char intrDesc[20];
    snprintf(intrDesc, sizeof(intrDesc), gpInterrupt == -1 ? "not connected" : "GP%d", gpInterrupt);
    Log(ok ? LOG_CONFIG : LOG_ERROR, "MPU6050 device initialization %s; I2C%d address 0x%02X, interrupt %s, dynamic range +/- %dg\n",
        ok ? "OK" : "failed", i2c_hw_index(i2c), i2cAddr, intrDesc, gRange);
}

void MPU6050::Task()
{
}

void MPU6050::IRQ()
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
bool MPU6050::OnI2CReady(I2CX *i2c)
{
    // If the interrupt is asserted, or the interrupt line isn't
    // connected, read interrupt status and axis registers.  It's more
    // efficient to check the interrupt line when we have GPIO access
    // to it, since we can avoid an unnecessary I2C transaction when
    // we can tell from the interrupt status that the chip has no new
    // data available.
    //
    // Note that we configure the interrupt line as active-low - the chip
    // signals an interrupt by pulling the line to ground.
    if (gpInterrupt == -1 || !gpio_get(gpInterrupt))
    {
        // Read:
        //
        //  INT_STATUS   (0x3A)  - interrupt status (bit 0 = DATA_RDY_INT); reading clears it
        //  ACCEL_XOUT_H (0x3B)  - X axis high 8 bits
        //  ACCEL_XOUT_L (0x3C)  - X axis low 8 bits
        //  ACCEL_YOUT_H (0x3D)  - Y axis high 8 bits
        //  ACCEL_YOUT_L (0x3E)  - Y axis low 8 bits
        //  ACCEL_ZOUT_H (0x3F)  - Z axis high 8 bits
        //  ACCEL_ZOUT_L (0x40)  - Z axis low 8 bits
        //  TEMP_OUT_H   (0x41)  - temperature high 8 bits
        //  TEMP_OUT_L   (0x42)  - temperature low 8 bits
        uint8_t buf[] = { REG_INT_STATUS };
        i2c->Read(buf, 1, 9);

        // new transaction started
        return true;
    }

    // no transaction started
    return false;
}

// I2C read completed
bool MPU6050::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we expect 9 bytes back for the register read
    if (len == 9)
    {
        // Check the DATA_RDY_INT bit in the status register - it's set
        // if there's new data to read.  We can't know for sure that
        // there's a new sample, even if we're using an interrupt
        // signal, because the interrupt signal could be shared with
        // other peripherals, and we also poll the chip when there's no
        // interrupt line at all.
        if ((data[0] & 0x01) != 0)
        {
            // Retrieve the new axis data.  The axis values are encoded
            // as 16-bit signed ints (2's complement format), MSB first.
            // The full INT16 range covers the selected +/- gRange, which
            // matches the normalized format that we return through the
            // hardware abstraction interface, so we don't have to
            // perform any additional scaling.
            static auto Get16 = [](const uint8_t *buf) {
                uint16_t w = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
                return static_cast<int16_t>(w);
            };
            x = Get16(&data[1]);
            y = Get16(&data[3]);
            z = Get16(&data[5]);

            // save the X/Y/Z output registers, for diagnostics
            memcpy(outReg, &data[1], sizeof(outReg));

            // Retrieve the temperature: degrees C = raw/340 + 36.53, which
            // we store in units of 1/1000 degree C
            temperatureRaw = Get16(&data[7]);
            temperature = (static_cast<int>(temperatureRaw) * 1000) / 340 + 36530;

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

            // Reading INT_STATUS cleared the latched interrupt, so
            // there's no need for an extra transaction here to clear it.
        }
    }

    // no new transaction started
    return false;
}

// read the last sample, in normalized INT16 units
void MPU6050::Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp)
{
    x = this->x;
    y = this->y;
    z = this->z;
    timestamp = this->timestamp;
}

// command console status command
void MPU6050::Command_info(const ConsoleCommandContext *c)
{
    if (inst == nullptr)
        return c->Print("MPU6050 not configured\n");

    static const auto ShowStats = [](const ConsoleCommandContext *c)
    {
        auto &s = inst->stats;
        c->Printf(
            "MPU-6050 status:\n"
            "WHO_AM_I:                  0x%02X\n"
            "Number of IRQs serviced:   %llu\n"
            "Number of samples read:    %llu\n"
            "Average read interval:     %.2f ms\n"
            "Average read latency:      %lu us\n"
            "Missed samples:            %llu (1 in %llu)\n"
            "X,Y,Z (Native INT16):      %d,%d,%d\n"
            "Sample reg bytes [3B-40]:  %02X.%02X %02X.%02X %02X.%02X\n"
            "Temperature:               %.2f C (%.2f F)\n",
            inst->whoAmI,
            s.nIntr, s.nReads,
            static_cast<float>(s.nReads != 0 ? s.timeBetweenReads / s.nReads : 0) / 1000.0f,
            static_cast<uint32_t>(s.nReads != 0 ? s.i2cLatency / s.nReads : 0),
            s.nMissedSamples, s.nMissedSamples == 0 ? 0 : s.nIntr / s.nMissedSamples,
            inst->x, inst->y, inst->z,
            inst->outReg[0], inst->outReg[1], inst->outReg[2], inst->outReg[3], inst->outReg[4], inst->outReg[5],
            inst->GetTemperatureC(), inst->GetTemperatureF());
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
            s.nIntr = 0;
            s.nMissedSamples = 0;
            c->Print("MPU6050 statistics counters reset\n");
        }
        else
            return c->Usage();
    }
}
