// Pinscape Pico - VL6180X time-of-flight distance sensor
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <functional>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/watchdog.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "Logger.h"
#include "JSON.h"
#include "I2C.h"
#include "GPIOManager.h"
#include "CommandConsole.h"
#include "VL6180x.h"

// global singleton
VL6180X *vl6180x = nullptr;

// construction
VL6180X::VL6180X(int gpCE, int gpInt) :
    I2CDevice(i2cAddr),
    gpCE(gpCE), gpInt(gpInt)
{
}

// Configure from JSON data
//
// vl6180x: {
//   i2c: <number>,          // I2C bus number (0 or 1) (note: no I2C address is needed, since VL6180X has a fixed address, 0x29)
//   chipEnable: <gpio>,     // Pico GPIO port of Chip Enable connection (VL6180X "GPIO0/CE" pin); optional, omit if not connected
//   interrupt: <gpio>,      // Pico GPIO port number of interrupt input (from VL6180X "GPIO1/INT" pin); optional, omit if not connected
// }
//
void VL6180X::Configure(JSONParser &json)
{
    if (auto *val = json.Get("vl6180x") ; !val->IsUndefined())
    {
        // get and validate the I2C bus number
        int bus = val->Get("i2c")->Int(-1);
        if (!I2C::ValidateBusConfig("vl6180x", bus))
            return;

        // get the optional Chip Enable GPIO port, if any
        int gpCE = val->Get("chipEnable")->Int(-1);
        if (gpCE >= 0)
        {
            // validate it
            if (!IsValidGP(gpCE))
            {
                Log(LOG_ERROR, "vl6180x: invalid Chip Enable GPIO port\n");
                return;
            }

            // claim it in exclusive mode, for use as a driven output
            if (!gpioManager.Claim("VL6180X (Chip Enable)", gpCE))
                return;

            // set up it up as an output, initially low to disable the chip
            gpio_init(gpCE);
            gpio_put(gpCE, false);
            gpio_set_dir(gpCE, GPIO_OUT);
        }

        // get the optional interrupt-in GPIO port, if any
        int gpInt = val->Get("interrupt")->Int(-1);
        if (gpInt >= 0)
        {
            // validate it
            if (!IsValidGP(gpInt))
            {
                Log(LOG_ERROR, "vl6180x: invalid interrupt GPIO port\n");
                return;
            }

            // claim it as a shared input, with pull-up
            if (!gpioManager.ClaimSharedInput("VL6180X (Interrupt)", gpInt, true, false, true))
                return;
        }

        // create the singleton
        vl6180x = new VL6180X(gpCE, gpInt);
        
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
        vl6180x->Init(bus);
        
        // add it to the I2C bus
        I2C::GetInstance(bus, false)->Add(vl6180x);
    }
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
void VL6180X::IRQ()
{
    // remember the sample arrival time
    vl6180x->tInterrupt = time_us_64();

    // acknowledge the IRQ
    gpio_acknowledge_irq(vl6180x->gpInt, GPIO_IRQ_EDGE_FALL);
}


// I2C interface symbolic constants for register names and contents
//
// Register addresses are two bytes, high byte first
//
#define IDENTIFICATION_MODEL_ID 0x00, 0x00
#define IDENTIFICATION_MODEL_REV_MAJOR 0x00, 0x01
#define IDENTIFICATION_MODEL_REV_MINOR 0x00, 0x02
#define IDENTIFICATION_MODULE_REV_MAJOR 0x00, 0x03
#define IDENTIFICATION_MODULE_REV_MINOR 0x00, 0x04
#define IDENTIFICATION_DATE 0x00, 0x06              // NB - 16-bit register data
#define IDENTIFICATION_TIME 0x00, 0x08              // NB - 16-bit register data

#define SYSTEM_MODE_GPIO0 0x00, 0x10
#define SYSTEM_MODE_GPIO1 0x00, 0x11
#define SYSTEM_HISTORY_CTRL 0x00, 0x12
#define SYSTEM_INTERRUPT_CONFIG_GPIO 0x00, 0x14
#define SYSTEM_INTERRUPT_CLEAR 0x00, 0x15
#define SYSTEM_FRESH_OUT_OF_RESET 0x00, 0x16
#define SYSTEM_GROUPED_PARAMETER_HOLD 0x00, 0x17

#define SYSRANGE_START 0x00, 0x18
#define SYSRANGE_THRESH_HIGH 0x00, 0x19
#define SYSRANGE_THRESH_LOW 0x00, 0x1A
#define SYSRANGE_INTERMEASUREMENT_PERIOD 0x00, 0x1B
#define SYSRANGE_MAX_CONVERGENCE_TIME 0x00, 0x1C
#define SYSRANGE_CROSSTALK_COMPENSATION_RATE 0x00, 0x1E
#define SYSRANGE_CROSSTALK_VALID_HEIGHT 0x00, 0x21
#define SYSRANGE_EARLY_CONVERGENCE_ESTIMATE 0x00, 0x22
#define SYSRANGE_PART_TO_PART_RANGE_OFFSET 0x00, 0x24
#define SYSRANGE_RANGE_IGNORE_VALID_HEIGHT 0x00, 0x25
#define SYSRANGE_RANGE_IGNORE_THRESHOLD 0x00, 0x26
#define SYSRANGE_MAX_AMBIENT_LEVEL_MULT 0x00, 0x2C
#define SYSRANGE_RANGE_CHECK_ENABLES 0x00, 0x2D
#define SYSRANGE_VHV_RECALIBRATE 0x00, 0x2E
#define SYSRANGE_VHV_REPEAT_RATE 0x00, 0x31

#define SYSALS_START 0x00, 0x38
#define SYSALS_THRESH_HIGH 0x00, 0x3A
#define SYSALS_THRESH_LOW 0x00, 0x3C
#define SYSALS_INTERMEASUREMENT_PERIOD 0x00, 0x3E
#define SYSALS_ANALOGUE_GAIN 0x00, 0x3F
#define SYSALS_INTEGRATION_PERIOD 0x00, 0x40

#define RESULT_RANGE_STATUS 0x00, 0x4D
#define RESULT_ALS_STATUS 0x00, 0x4E
#define RESULT_INTERRUPT_STATUS_GPIO 0x00, 0x4F
#define RESULT_ALS_VAL 0x00, 0x50
#define RESULT_HISTORY_BUFFER 0x00, 0x52 
#define RESULT_RANGE_VAL 0x00, 0x62
#define RESULT_RANGE_RAW 0x00, 0x64
#define RESULT_RANGE_RETURN_RATE 0x00, 0x66
#define RESULT_RANGE_REFERENCE_RATE 0x00, 0x68
#define RESULT_RANGE_RETURN_SIGNAL_COUNT 0x00, 0x6C
#define RESULT_RANGE_REFERENCE_SIGNAL_COUNT 0x00, 0x70
#define RESULT_RANGE_RETURN_AMB_COUNT 0x00, 0x74
#define RESULT_RANGE_REFERENCE_AMB_COUNT 0x00, 0x78
#define RESULT_RANGE_RETURN_CONV_TIME 0x00, 0x7C
#define RESULT_RANGE_REFERENCE_CONV_TIME 0x00, 0x80

#define READOUT_AVERAGING_SAMPLE_PERIOD 0x01, 0x0A
#define FIRMWARE_BOOTUP 0x01, 0x19
#define FIRMWARE_RESULT_SCALER 0x01, 0x20
#define I2C_SLAVE_DEVICE_ADDRESS 0x02, 0x12
#define INTERLEAVED_MODE_ENABLE 0x02, 0xA3

// gain settings
enum VL6180X_ALS_Gain 
{
    GAIN_20 = 0,    // 20
    GAIN_10,        // 10.32
    GAIN_5,         // 5.21
    GAIN_2_5,       // 2.60
    GAIN_1_67,      // 1.72
    GAIN_1_25,      // 1.28
    GAIN_1,         // 1.01
    GAIN_40,        // 40 
};

// identification
struct VL6180X_ID 
{
    uint8_t model = 0;              // model number
    uint8_t modelRevMajor = 0;      // model revision number major...
    uint8_t modelRevMinor = 0;      // ...and minor
    uint8_t moduleRevMajor = 0;     // module revision number major...
    uint8_t moduleRevMinor = 0;     // ... and minior
    struct
    {
        uint8_t month = 0;          // month 1..12
        uint8_t day = 0;            // day of month 1..31
        uint16_t year = 0;          // calendar year, 4-digit (e.g., 2016)
        uint8_t phase = 0;          // manufacturing phase, 0..7
        uint8_t hh = 0;             // hour, 0..23
        uint8_t mm = 0;             // minute, 0..59
        uint8_t ss = 0;             // second, 0..59
    } manufDate;                    // manufacturing date and time
};

// range statistics
struct VL6180X_RangeStats
{
    uint16_t returnRate;        // return signal rate
    uint16_t refReturnRate;     // reference return rate
    uint32_t returnCnt;         // return signal count
    uint32_t refReturnCnt;      // reference return count
    uint32_t ambCnt;            // ambient count
    uint32_t refAmbCnt;         // reference ambient count
    uint32_t convTime;          // convergence time
    uint32_t refConvTime;       // reference convergence time
};

// initialize the chip
void VL6180X::Init(int bus)
{
    // set up our console command handler
    CommandConsole::AddCommand(
        "vl6180x", "vl6180x diagnostics",
        "vl6180x [options]\n"
        "options:\n"
        "  -s, --status    show status\n",
        [](const ConsoleCommandContext *c){ vl6180x->Command_main(c); });

    // send initialization commands
    SendInitCommands(i2c_get_instance(bus));
}

void VL6180X::I2CReinitDevice(I2C *i2c)
{
    SendInitCommands(i2c->GetHwInst());
}

void VL6180X::SendInitCommands(i2c_inst_t *i2c)
{
    // If we have a Pico GPIO connection to the chip's Chip Enable port,
    // use it to perform a hard reset on the sensor.  If CE isn't
    // connected to a GPIO, it must be wired to VDD, so the chip will
    // simply reset with the Pico power cycle rather than under software
    // control.  That's not ideal, because the VL6180X won't have gone
    // through a hard reset if the Pico did a soft reset, in which case
    // we shouldn't initialize the chip.
    if (gpCE >= 0)
    {
        // CE should already be low, but make sure
        gpio_put(gpCE, false);
        sleep_us(50);

        // take CE high
        gpio_put(gpCE, true);
    }

    // wait 1ms (per the data sheet) for the chip to come out of reset
    watchdog_update();
    sleep_us(1000);
    watchdog_update();

    // Let's check to see if the chip has gone through a hardware-reset cycle
    uint8_t freshOutOfReset = 0x00;
    bool ok = true;
    {
        uint8_t buf[] = { SYSTEM_FRESH_OUT_OF_RESET };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) != _countof(buf)
            || i2c_read_timeout_us(i2c, i2cAddr, &freshOutOfReset, 1, false, 1000) != 1)
            ok = false;
    }
    Log(LOG_DEBUG, "VL6180X reset flag=0x%02X\n", freshOutOfReset);

    // Check if the chip just came out of hardware reset
    if (freshOutOfReset != 0)
    {
        // Hardware reset, so we have to fully initialize the chip.
        // Send the mandatory initial register assignments, per the manufacturer's app notes - see:
        // http://www.st.com/st-web-ui/static/active/en/resource/technical/document/application_note/DM00122600.pdf
        struct { uint8_t buf[3]; } initCmds[] = {
            { 0x02, 0x07, 0x01 },
            { 0x02, 0x08, 0x01 },
            { 0x00, 0x96, 0x00 },
            { 0x00, 0x97, 0xfd },
            { 0x00, 0xe3, 0x00 },
            { 0x00, 0xe4, 0x04 },
            { 0x00, 0xe5, 0x02 },
            { 0x00, 0xe6, 0x01 },
            { 0x00, 0xe7, 0x03 },
            { 0x00, 0xf5, 0x02 },
            { 0x00, 0xd9, 0x05 },
            { 0x00, 0xdb, 0xce },
            { 0x00, 0xdc, 0x03 },
            { 0x00, 0xdd, 0xf8 },
            { 0x00, 0x9f, 0x00 },
            { 0x00, 0xa3, 0x3c },
            { 0x00, 0xb7, 0x00 },
            { 0x00, 0xbb, 0x28 }, // typical value 0x3c; use 0x28 for fast ranging frequency ~100Hz
            { 0x00, 0xb2, 0x09 },
            { 0x00, 0xca, 0x09 },
            { 0x01, 0x98, 0x01 },
            { 0x01, 0xb0, 0x17 },
            { 0x01, 0xad, 0x00 },
            { 0x00, 0xff, 0x05 },
            { 0x01, 0x00, 0x05 },
            { 0x01, 0x99, 0x05 },
            { 0x01, 0xa6, 0x1b },
            { 0x01, 0xac, 0x3e },
            { 0x01, 0xa7, 0x1f },
            { 0x00, 0x30, 0x00 },
        };
        for (int i = 0 ; i < _countof(initCmds) ; ++i)
        {
            watchdog_update();
            if (i2c_write_timeout_us(i2c, i2cAddr, initCmds[i].buf, 3, false, 1000) != 3)
                ok = false;
        }

        // clear the reset flag
        {
            uint8_t buf[] = { SYSTEM_FRESH_OUT_OF_RESET, 0x00 };
            if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
                ok = false;
        }

        // allow time to settle
        sleep_us(1000);
        watchdog_update();
    }
    else
    {
        // The chip didn't go through a hardware reset, so CE must not be wired,
        // and we must have only reset the Pico without power-cycling the whole
        // system.  In this case, we must have left the chip in continuous ranging
        // mode.  Deactivate continuous mode so that we can update the parameter
        // registers.  To deactivate continuous mode, write bit 0x01 in the
        // SYSRANGE_START register, which toggles the run/stop state.
        {
            static const uint8_t buf[] = { SYSRANGE_START, 0x03 };
            if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
                ok = false;
        }

        // Now wait for a full measurement cycle, to be sure that any sample in
        // progress completes and nothing is pending.  Periodically update the
        // watchdog timer while waiting so it doesn't think we're stuck.
        for (int i = 0 ; i < 12 ; ++i)
        {
            sleep_us(1000);
            watchdog_update();
        }
    }

    // Set up parameters for continuous sampling at the fastest rate of 100 Hz (10ms
    // inter-measurement period).  Note some constraints on the parameters:
    //
    // (Convergence_time + 5 <= Intermeasurement_period * 0.9) (data sheet section 2.5.2)
    //   Desired Intermeasurement_period = 10ms
    //   -> Convergence_time <= 4ms
    //
    // Total execution time = Pre_cal + Convergence_time + Averaging_time (2.7.1, figure 20)
    //   Total execution time = 90% of inter-measurement period? = 10ms (our setting)*90% = 9ms
    //   Pre_cal = constant 3.2ms (fixed)
    //   Convergence_time = 4ms (our setting, above)
    //   Remaining budget for averaging time = 9ms - 3.2ms - 4ms = 1.8ms
    //
    //   Experimentally, it seems that a non-zero averaging time interacts poorly
    //   with a short inter-measurement period.  The timing diagram in the data
    //   sheet (Section 2.7.1, figure 20) suggests that we could use all of the
    //   remaining execution time after the pre-cal and convergence time for
    //   averaging.  If we assume that "total execution time" is 90% of the
    //   inter-measurement period, which isn't stated in the data sheet but is
    //   suggested by the way they calculate the Convergence_time in 2.5.2, we
    //   have 1.8ms, which gives us a READOUT_AVERAGING_SAMPLE_PERIOD of 7
    //   (1300us + 7*64.5us = 1751.5us).  But that seems to yield erratic
    //   results, so my guess is that there's some additional timing overhead
    //   that further constrains the averaging time.  Setting the register to
    //   0 for the minimum averaging period of 1.3ms seems to be the best bet.
    //
    {
        struct {
            bool irqOnly;
            int len;
            uint8_t buf[4];
        } modeCmds[] = {
            { false, 3, SYSTEM_GROUPED_PARAMETER_HOLD, 0x01 },         // set parameter hold while updating settings

            { false, 3, SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04 },          // Enable interrupt when range sample ready (mode 4)
            { true,  3, SYSTEM_MODE_GPIO1, 0x10 },                     // Enable GPIO1 interrupt output (1 in bit 0x10), active low (0 in bit 0x20)
            { false, 3, SYSRANGE_VHV_REPEAT_RATE, 0xFF },              // Set auto calibration period (Max = 255)/(OFF = 0)
            { false, 3, SYSRANGE_INTERMEASUREMENT_PERIOD, 0x00 },      // Set inter-measurement period - 10ms = register value 0x00
            { false, 3, SYSRANGE_MAX_CONVERGENCE_TIME, 0x04 },         // Max range convergence time 4ms - SEE CALCULATION ABOVE
            { false, 3, SYSRANGE_RANGE_CHECK_ENABLES, 0x00 },          // S/N disable, ignore disable, early convergence test disable
            { false, 4, SYSRANGE_EARLY_CONVERGENCE_ESTIMATE, 0x00, 0x00 },  // abort range measurement if convergence rate below this value
            { false, 3, READOUT_AVERAGING_SAMPLE_PERIOD, 0 },          // Sample averaging period (1.3ms + N*64.5us) - set minimum
            { false, 3, SYSRANGE_THRESH_LOW, 0x00 },                   // low threshold; set to minimum to effectively disable threshold events
            { false, 3, SYSRANGE_THRESH_HIGH, 0xff },                  // high threshold; set to maximum to effectively disable threshold events

            { false, 3, SYSTEM_GROUPED_PARAMETER_HOLD, 0x00 },         // end parameter hold

            { false, 3, SYSRANGE_VHV_RECALIBRATE, 0x01 },              // perform a single temperature calibration of the ranging sensor
        };

        for (int i = 0 ; i < _countof(modeCmds) ; ++i)
        {
            // skip IRQ-only commands if there's no IRQ GPIO
            if (modeCmds[i].irqOnly && gpInt < 0)
                continue;

            // send the command
            watchdog_update();
            if (i2c_write_timeout_us(i2c, i2cAddr, modeCmds[i].buf, modeCmds[i].len, false, 1000) != modeCmds[i].len)
               ok = false;
        }
    }
    
    // get the device ID information
    VL6180X_ID id;
    {
        uint8_t buf1[5] = { /*IDENTIFICATION_MODEL_ID*/ 0x00, 0x00, 0xFF, 0xFF, 0xFF };
        uint8_t buf2[4] = { /*IDENTIFICATION_DATE*/ 0x00, 0x06, 0xFF, 0xFF };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf1, 2, true, 1000) == 2
            && i2c_read_timeout_us(i2c, i2cAddr, buf1, _countof(buf1), false, 1000) == _countof(buf1)
            && i2c_write_timeout_us(i2c, i2cAddr, buf2, 2, true, 1000) == 2
            && i2c_read_timeout_us(i2c, i2cAddr, buf2, _countof(buf2), false, 1000) == _countof(buf2))
        {
            id.model = buf1[0];
            id.modelRevMajor = buf1[1];
            id.modelRevMinor = buf1[2];
            id.moduleRevMajor = buf1[3];
            id.moduleRevMinor = buf1[4];
            id.manufDate.year = buf2[0] >> 4;
            id.manufDate.month = buf2[0] & 0x0F;
            id.manufDate.day = buf2[1] >> 3;;
            id.manufDate.phase = buf2[1] & 0x07;
            int t = (static_cast<uint16_t>(buf2[2]) << 8) | buf2[3];
            id.manufDate.hh = t / 3600;
            id.manufDate.mm = (t % 3600) / 60;
            id.manufDate.ss = t % 60;
        }
        else
            ok = false;

        // ID should be 0xB4
        if (id.model != 0xB4)
            Log(LOG_WARNING, "VL6180X device model ID read as 0x%02X - should be 0xB4\n", id.model);
    }

    // set continuous ranging mode (bit 0x02), and toggle the mode on (bit 0x01)
    {
        static const uint8_t buf[] = { SYSRANGE_START, 0x03 };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
            ok = false;
    }

    // note the continuous-mode start time
    continuousModeStartTime = time_us_64();

    // report the result
    Log(ok ? LOG_CONFIG : LOG_ERROR, "VL6180X device initialization %s; product ID 0x%02X, rev %d.%d, module rev %d.%d, manuf %02d/%02d/xxx%d %02d:%02d:%02d\n",
        ok ? "OK" : "failed", id.model, id.modelRevMajor, id.modelRevMinor, id.moduleRevMajor, id.moduleRevMinor,
        id.manufDate.month, id.manufDate.day, id.manufDate.year, id.manufDate.hh, id.manufDate.mm, id.manufDate.ss);
}

bool VL6180X::Read(uint32_t &distance, uint64_t &timestamp)
{
    // return the last reading
    distance = sample;
    timestamp = tSample;

    // consume the reading
    bool ret = isSampleReady;
    isSampleReady = false;

    // return the new/old status
    return ret;
}

bool VL6180X::OnI2CReady(I2CX *i2c)
{
    // check if the interrupt signal is connected
    if (gpInt >= 0)
    {
        // The interrupt line is connected, so a new sample is available
        // if and only if the line is being pulled low.  If no sample is
        // available, there's no need for an I2C transaction on this round.
        if (!gpio_get(gpInt))
        {
            // a sample is available - read the result register
            uint8_t buf[] = { RESULT_RANGE_VAL };
            i2c->Read(buf, 2, 1);
            readMode = ReadMode::Value;
            return true;
        }
    }
    else
    {
        // We don't have an interrupt line, so we have to poll the
        // status register.
        uint8_t buf[] = { RESULT_INTERRUPT_STATUS_GPIO };
        i2c->Read(buf, 2, 1);
        readMode = ReadMode::Status;
        return true;
    }

    // no transaction needed on this cycle
    return false;
}

bool VL6180X::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // check which register we're reading
    switch (readMode)
    {
    case ReadMode::Status:
        // status register - we're polling to see if a new value is available
        // big 0x01 -> range result ready
        if ((data[0] & 0xC0) != 0)
        {
            // error detected - count it and clear the interrupt status
            resultErrorCount += 1;
            uint8_t buf[] = { SYSTEM_INTERRUPT_CLEAR, 0x07 };
            i2c->Write(buf, _countof(buf));
            return true;
        }
        else if ((data[0] & 0x07) == 0x04)
        {
            // input ready
            uint8_t buf[] = { RESULT_RANGE_VAL };
            i2c->Read(buf, 2, 1);
            readMode = ReadMode::Value;
            return true;
        }
        break;

    case ReadMode::Value:
        // value - the range sample value in millimeters
        readMode = ReadMode::None;
        sample = data[0];

        // Record the time we obtained the sample.  If we have an
        // interrupt input, use the time of the last interrupt, since
        // that records precisely when the chip signaled that a new
        // sample was ready.  Otherwise, we don't know when the sample
        // actually arrived at the chip, so the best we can is record
        // the time we read it, which is less precise since the sample
        // could have been sitting there for up to a full cycle (4ms at
        // the maximum 250/s sampling rate).
        tSample = (gpInt >= 0) ? tInterrupt : time_us_64();

        // flag that a new sample is available for reading at the
        // programmatic interface
        isSampleReady = true;

        // count the sample
        sampleReadCount += 1;

        // clear the interupt status
        {
            uint8_t buf[] = { SYSTEM_INTERRUPT_CLEAR, 0x07 };
            i2c->Write(buf, _countof(buf));
            return true;
        }

        // done
        break;
    }

    // no further operation requested
    readMode = ReadMode::None;
    return false;
}

// Console command handler
void VL6180X::Command_main(const ConsoleCommandContext *c)
{
    // make sure we have at least one option
    if (c->argc <= 1)
        return c->Usage();

    // process options
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--status") == 0)
        {
            c->Printf(
                "VL6180X status:\n"
                "  Interrupt status: %s\n"
                "  Num samples read: %llu\n"
                "  Avg sample time:  %.2f ms\n"
                "  Errors:           %llu\n"
                "  Last sample:      %d\n",
                gpInt >= 0 ? (gpio_get(gpInt) ? "Asserted" : "Not asserted") : "Not connected",
                sampleReadCount,
                static_cast<float>((time_us_64() - continuousModeStartTime)/sampleReadCount) / 1000.0f,
                resultErrorCount, sample);
        }
        else
        {
            // invalid syntax
            return c->Printf("tcd1103: unknown option \"%s\"\n", a);
        }
    }
}
