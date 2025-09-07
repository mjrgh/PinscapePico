// Pinscape Pico - ADS1115 analog-to-digital converter chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>
#include <hardware/i2c.h>
#include <hardware/watchdog.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "I2C.h"
#include "ThunkManager.h"
#include "GPIOManager.h"
#include "ADCManager.h"
#include "ADS1115.h"
#include "CommandConsole.h"

// statics
ADS1115 *ADS1115::chips[8];
int ADS1115::nChipsConfigured = 0;

// Construction.  The ADS1115 reports with 16-bit signed precision
// (-32768..+32767) over a double-ended input range.
ADS1115::ADS1115(int chipNum, uint8_t i2cAddr, int gpAlertRdy) :
    ADC(-32768, 32767, Format("ads1115_%d", chipNum), chipNum == 0 ? "ads1115" : nullptr, Format("ADS1115 ADC [%d]", chipNum)),
    I2CDevice(i2cAddr),
    chipNum(chipNum), gpAlertRdy(gpAlertRdy)
{
}

// binary-to-decimal, for displaying config reg bit patterns
static int FormatBinary(int val, int shift, int bits)
{
    val >>= shift;
    int acc = 0;
    for (int mul = 1 ; bits != 0 ; mul *= 10, --bits, val >>= 1)
        acc += (val & 0x01)*mul;
    return acc;
}

// JSON configuration
//
// ads1115: {
//   i2c: <number>,       // I2C bus number (0 or 1)
//   addr: <number>,      // I2C bus address for this chip, 7-bit notation
//   ready: <gpNumber>,   // GPIO port connected to ALERT/RDY pin; optional, can be shared by multiple chips
//   channel: 0,          // input channel number, AINx (0 = AIN0 through 3 = AIN3), or a list of channels to read from
//   sampleRate: 860,     // sampling rate, in samples per second
// }
//
// 'channel' can be set to a single channel number or a list of channel
// numbers, such as [0, 3].  When multiple channels are listed, the
// channels are read one at a time in the order listed, looping back to
// the first after reading the last channel.  The chip can physically
// only read from one pin at a time, so we read the channels one at a
// time in a round-robin rotation.
//
// When 'channel' lists multiple physical inputs, they're mapped to
// logical channels for Read() in the order listed.  For example, if you
// set 'channel' to [1,3], Read(0) reads AIN1, and Read(1) reads AIN3.
//
// If multiple ADS1115 chips are attached to the Pico, specify the
// 'ads1115' key as a list, with one object entry (like the one shown
// above) for each chip.  The single-object format shown above is
// equivalent to a single-element array containing that object as its
// single element, which is a convenience for the most common case where
// there's only one ADS1115 in the system.
//
// The ADS1115 only allows certain fixed samples rates (8, 16, 32, 64,
// 128, 250, 474, 860).  The actual rate will be set to the allowed rate
// that's closest to the requested rate.
//
void ADS1115::Configure(JSONParser &json)
{
    if (auto *val = json.Get("ads1115") ; !val->IsUndefined())
    {
        // parse one object from the array (or just the single object, if it's not array
        val->ForEach([](int index, const JSONParser::Value *value)
        {
            // get and validate the I2C bus number
            char facility[20];
            snprintf(facility, _countof(facility), "ads1115[%d]", index);
            int bus = value->Get("i2c")->Int(-1);
            if (!I2C::ValidateBusConfig(facility, bus))
                return;

            // get and validate the I2C address
            int addr = value->Get("addr")->Int(-1);
            if (addr < 0x48 || addr > 0x4B)
            {
                Log(LOG_ERROR, "ads1115[%d]: invalid address 0x%02X; must be 0x48..0x4B)\n", index, addr);
                return;
            }

            // get the READY port and sample rate
            int gpReady = value->Get("ready")->Int(-1);
            int sampleRate = value->Get("sampleRate")->Int(860);

            // create the new chip object (tentatively - we could still fail)
            std::unique_ptr<ADS1115> chipHolder(new ADS1115(index, addr, gpReady));
            ADS1115 *chip = chipHolder.get();
            chip->desiredSamplesPerSecond = sampleRate;

            // look up the sampling rate to get the DR bits in the config register
            static int availableSpeeds[] = { 8, 16, 32, 64, 128, 250, 474, 860 };
            {
                // Find the closest available speed.  Each array element is the
                // samples-per-second setting for the data rate value equal to
                // that element's array index.
                int best = -1, delta = 0;
                for (int i = 0 ; i < 8 ; ++i)
                {
                    int d = abs(availableSpeeds[i] - chip->desiredSamplesPerSecond);
                    if (best < 0 || d < delta)
                        best = i, delta = d;
                }
                
                // select the best matching speed; the index value goes in bits 7:5 of
                // the config register
                chip->drBits = best;
                chip->samplesPerSecond = availableSpeeds[best];
            }

            // get the channel number or list
            if (const auto *channelVal = value->Get("channel");
                channelVal->IsNumber() || channelVal->IsArray() || channelVal->IsString())
            {
                // iterate over the channels
                bool ok = true;
                channelVal->ForEach([chip, index, &ok](int i, const JSONParser::Value *ele)
                {
                    // number -> single-ended AINx input 0-3
                    // string -> double-ended input, using format "0/3" to select +AIN0 -AIN3
                    int mux = -1;
                    if (ele->IsNumber())
                    {
                        // get and validate the channel number, 0-3 to AIN0-AIN3
                        int ch = ele->Int(-1);
                        if (ch < 0 || ch > 3)
                        {
                            Log(LOG_ERROR, "ads1115[%d]: invalid 'channel' number value %d; must be 0 to 3\n", index, ch);
                            ok = false;
                            return;
                        }

                        // the MUX value for a single-ended channel is conveniently equal
                        // to the AIN number plus 4 (4->AIN0, 5->AIN1, 6->AIN2, 7->AIN3)
                        mux = ch + 4;
                    }
                    else if (ele->IsString())
                    {
                        // get the MUX selection, normalizing to upper-case
                        auto s = ele->String();
                        std::transform(s.begin(), s.end(), s.begin(), ::toupper);

                        // look up the MUX selection
                        static const struct { const char *name; int mux; } muxName[] {
                            { "0/1",       0 },
                            { "AIN0/AIN1", 0 },
                            { "0/3",       1 },
                            { "AIN0/AIN3", 1 },
                            { "1/3",       2 },
                            { "AIN1/AIN3", 2 },
                            { "2/3",       3 },
                            { "AIN2/AIN3", 3 },
                            { "0",         4 },
                            { "AIN0",      4 },
                            { "1",         5 },
                            { "AIN1",      5 },
                            { "2",         6 },
                            { "AIN2",      6 },
                            { "3",         7 },
                            { "AIN3",      7 },
                        };
                        for (size_t i = 0 ; i < _countof(muxName) ; ++i)
                        {
                            if (s == muxName[i].name)
                            {
                                mux = muxName[i].mux;
                                break;
                            }
                        }

                        if (mux < 0)
                        {
                            Log(LOG_ERROR, "ads1115[%d]: invalid 'channel' string value \"%s\"; must be 0/1, 0/3, 1/3, or 2/3\n", index, s.c_str());
                            ok = false;
                            return;
                        }
                    }
                    else
                    {
                        Log(LOG_ERROR, "ads1115[%d]: invalid 'channel' value; must be AINx pin number or string naming pin or pin pair", index);
                        ok = false;
                        return;
                    }

                    // make sure it's not too many channels
                    if (i >= _countof(chip->logicalChannel))
                    {
                        // log it on the first overflow only, so that we only log one message for the whole overflow
                        if (i == _countof(chip->logicalChannel))
                            Log(LOG_ERROR, "ads1115[%d]: too many channels specified; maximum is 4\n", index);
                        ok = false;
                        return;
                    }

                    // Add the entry
                    chip->nLogicalChannels = i + 1;
                    chip->logicalChannel[i].mux = mux;
                }, true);

                // abort on error
                if (!ok)
                    return;
            }
            else
            {
                Log(LOG_ERROR, "ads1115[%d]: invalid or missing 'channel' setting; must be a number or list of numbers\n", index);
                return;
            }

            // Get the voltage range, if configured
            static const float pgaVoltages[] = { 6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f };
            if (auto *fsr = value->Get("voltageRange"); !fsr->IsUndefined())
            {
                // get the value as a float
                float f = fsr->Float();

                // find the closest match for the PGA values
                int best = -1;
                float minDelta = 0.0f;
                for (size_t i = 0 ; i < _countof(pgaVoltages) ; ++i)
                {
                    float delta = fabsf(f - pgaVoltages[i]);
                    if (best < 0 || delta < minDelta)
                    {
                        best = i;
                        minDelta = delta;
                    }
                }

                // select the best match
                chip->pga = best;
            }
                
            // If an ALERT/RDY pin was configured, validate it and set it up
            if (gpReady >= 0)
            {
                // validate it
                if (!IsValidGP(gpReady))
                {
                    Log(LOG_ERROR, "ads1115[%d]: invalid READY GPIO port\n", index);
                    return;
                }

                // claim the pin as an input, with pull-up (the ALERT/RDY line is open-drain)
                if (!gpioManager.ClaimSharedInput("ADS1115 (ALERT/RDY)", gpReady, true, false, true))
                    return;

                // set up an interrupt handler
                gpio_add_raw_irq_handler(gpReady, thunkManager.Create(&ADS1115::IRQ, chip));
                gpio_set_irq_enabled(gpReady, GPIO_IRQ_EDGE_FALL, true);
                irq_set_enabled(IO_IRQ_BANK0, true);
            }

            // looks good - add the chip to the ADC manager's list
            adcManager.Add(chipHolder.release());

            // add it to the I2C bus
            I2C::GetInstance(bus, false)->Add(chip);

            // add it to our static list
            if (nChipsConfigured < _countof(chips))
                chips[nChipsConfigured++] = chip;

            // initialize
            chip->Init(i2c_get_instance(bus));

            // success
            char readyTxt[20];
            sprintf(readyTxt, gpReady >= 0 ? "GP%d" : "not connected", gpReady);
            Log(LOG_CONFIG, "ADS1115[%d] configured on I2C%d addr 0x%02X, ALERT/READY %s, %d samples/second (DR=%03d), "
                "full-scale voltage range +/- %.3fV (PGA=%03d)\n",
                index, bus, addr, readyTxt,
                chip->samplesPerSecond, FormatBinary(chip->drBits, 0, 3),
                pgaVoltages[chip->pga], FormatBinary(chip->pga, 0, 3));

        }, true);

        // add our console command if we configured any chips
        if (nChipsConfigured != 0)
        {
            // add a command handler
            CommandConsole::AddCommand(
                "ads1115", "ADS1115 ADC diagnostics",
                "ads1115 [chipNumber] <options>\n"
                "  <chipNumber> gives the chip number, by configuration index; default is 0\n"
                "\n"
                "options:\n"
                "  -s, --stats     show statistics\n"
                "  --reset-stats   reset statistics counters\n",
                &Command_main_S);
        }
    }
}

void ADS1115::Init(i2c_inst_t *i2c)
{
    // send initialization commands
    SendInitCommands(i2c);
}

void ADS1115::I2CReinitDevice(I2C *i2c)
{
    // note if sampling is running
    bool sampling = (curSamplingChannel >= 0);

    // resend initialization commands
    SendInitCommands(i2c->GetHwInst());

    // if sampling was enabled, restart it
    if (sampling)
        EnableSampling();
}

void ADS1115::SendInitCommands(i2c_inst_t *i2c)
{
    // Note:  during initialization, uniquely, we can use the SDK's blocking
    // I2C APIs.  During normal operation we have to use the asynchronous
    // interface through our class I2C.

    // if a conversion is running, wait for it to finish (or until an arbitrary timeout)
    bool ok = true;
    for (uint64_t tTimeout = time_us_64() + 10000 ; time_us_64() < tTimeout ; )
    {
        // read the config register
        uint8_t tx[1] = { 0x01 };
        uint8_t rx[2] = { 0x00, 0x00 };
        if (i2c_write_timeout_us(i2c, i2cAddr, tx, _countof(tx), true, 1000) != _countof(tx)
            || i2c_read_timeout_us(i2c, i2cAddr, rx, _countof(rx), false, 1000) != _countof(rx))
        {
            // error reading config register - abort
            ok = false;
            Log(LOG_ERROR, "ADS1115[%d]: timeout waiting for config register to show chip idle\n", chipNum);
            break;
        }

        // if the OS bit (0x80) is reading '1', the converter is idle, so we've
        // successfully cleared any conversion in progress
        if ((rx[0] & 0x80) != 0)
            break;

        // reset the watchdog timer, since we're intentionally stalling here
        watchdog_update();
    }

    // set the CONFIG register
    {
        // start with the first channel
        curSamplingChannel = 0;
        
        // Set up the configuration register (0x01)
        //
        //   Register Address = config register address (0x01)
        //
        //   Config High bits:
        //             0x80  OS         operational status, 0 to do nothing just now
        //             0x70  MUX        input multiplexer; for single-ended, 0x40=AIN0, 0x50=AIN1, 0x60=AIN2, 0x70=AIN3
        //             0x0E  PGA        amplifier gain; 0x20 = +/4 4.096V, suitable for 3.3V range on potentiometers and the like
        //             0x01  MODE       0=continuous or 1=single-shot; set to single-shot initially, to leave in low-power state until needed
        //
        //   Config Low bits:
        //             0xE0  DR         data rate; 0x00 = minimum 8 samples/second, 0x0E = maximum 860 samples/second
        //             0x10  COMP_MODE  comparator mode; 0=traditional, 1=window; use traditional by default
        //             0x08  COMP_POL   ALERT/RDY polarity; 0=active low, 1=active high; use active-low for shared use
        //             0x04  COMP_LAT   ALERT/RDY latch; 0-non-latching, 1=latching; set to non-latching
        //             0x03  COMP_QUE   Comparator queue and disable; 0=assert after one conversion
        //
        // Note that COMP_MODE and COMP_LAT have no effect when we enable
        // READY mode by setting TH_HI < TH_LO, which we do below.  READY
        // mode pulses the ALERT/RDY pin for 8us on each sample completion
        // in continuous mode, and asserts in single-shot mode.

        // high byte - OS | MUX | PGA | MODE
        static const uint8_t MODE_SINGLE_SHOT = 0x01, MODE_CONTINUOUS = 0x00;
        configHi = (logicalChannel[0].mux << 4) | (pga << 1) | MODE_SINGLE_SHOT;

        // low byte - DR | COMP_MODE | COMP_POL | COMP_LAT | COMP_QUE
        static const uint8_t COMP_MODE_WINDOW = 0x10, COMP_MODE_TRAD = 0x00;
        static const uint8_t COMP_POL_ACTIVE_HIGH = 0x08, COMP_POL_ACTIVE_LOW = 0x00;
        static const uint8_t COMP_LAT_NONLATCHING = 0x00, COMP_LAT_LATCHING = 0x04;
        static const uint8_t COMP_QUE_ASSERT1 = 0x00, COMP_QUE_ASSERT2 = 0x01, COMP_QUE_ASSERT4 = 0x02, COMP_QUE_DISABLE = 0x03;
        configLo = (drBits << 5) | COMP_MODE_TRAD | COMP_POL_ACTIVE_LOW | COMP_LAT_NONLATCHING | COMP_QUE_ASSERT1;

        // send the command
        uint8_t buf[] = { CONFIG_REG_ADDR, configHi, configLo };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        ok = ok && (result == _countof(buf));
    }

    // set the TH_HI and TH_LO (comparator threshold) registers
    {
        // Set up the comparator for READY mode on the ALERT/RDY pin, by
        // setting HI < LO.  This has the special meaning of enabling READY
        // mode instead of normal comparator alert mode.
        //
        //   0x02  - address = Lo_thresh register, set to 0x07FFF = 32767
        //   0x00  - high byte = 0x7F
        //   0x00  - low byte = 0xFF
        //
        //   0x03  - address = Hi_thresh register, set to 0x8000 = -32768
        //   0x80  - high byte 0x80
        //   0x00  - low byte 0x01
        //
        static const uint8_t bufLo[] = { LO_THRESH_REG_ADDR, 0x7F, 0xFF };
        static const uint8_t bufHi[] = { HI_THRESH_REG_ADDR, 0x80, 0x00 };
        int resultLo = i2c_write_timeout_us(i2c, i2cAddr, bufLo, _countof(bufLo), false, 1000);
        int resultHi = i2c_write_timeout_us(i2c, i2cAddr, bufHi, _countof(bufHi), false, 1000);
        ok = ok && (resultLo == _countof(bufLo) && resultHi == _countof(bufHi));
    }

    // sampling is not running
    curSamplingChannel = -1;

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "ads1115[%d] device initialization %s\n", chipNum, ok ? "OK" : "failed");
}

// Start/stop sampling
void ADS1115::EnableSampling() 
{
    // if sampling isn't already running, initiate it
    if (curSamplingChannel == -1)
    {
        // start the sampling rotation at the first logical channel
        curSamplingChannel = 0;

        // Set the first channel (AIN pin).  The AIN field is in bits 14:12 of
        // the config register, or bits 6:4 of configHi == 0x70.  Mask out the
        // three bits, then mask in the AIN number:
        //
        //   0  000  -> AIN0 + AIN1 double-ended
        //   1  001  -> AIN0 + AIN3 double-ended
        //   2  010  -> AIN1 + AIN3 double-ended
        //   3  011  -> AIN2 + AIN3 double-ended
        //   4  100  -> AIN0 single-ended (AIN0 + GND)
        //   5  101  -> AIN1 single-ended
        //   6  110  -> AIN2 single-ended
        //   7  111  -> AIN3 single-ended
        //
        // We always operate in single-ended mode, so the value to store is
        // the AIN number plus 4 (shifted into bits 6:4).
        configHi &= ~MUX_MASK;
        configHi |= (logicalChannel[0].mux << 4);
        
        // start the first single-shot sample by writing OS (bit 0x80) to Config High
        configHi |= OS_BIT;

        // schedule a register update at the next I2C bus access opening
        configChangePending = true;

        // reset sampling statistics
        tStatsReset = time_us_64();
        for (int i = 0 ; i < nLogicalChannels ; ++i)
            logicalChannel[i].stats.Reset();
    }
}

// Read the latest sample in native units
ADC::Sample ADS1115::ReadNative(int ch)
{
    // return the latest sample from the selected channel, if valid
    if (ch >= 0 && ch < nLogicalChannels)
        return logicalChannel[ch].raw;
    else
        return { 0, 0 };
}

// Read the latest sample in normalized UINT16 units (0..65535)
ADC::Sample ADS1115::ReadNorm(int ch)
{
    // validate the channel
    if (ch >= 0 && ch < nLogicalChannels)
    {
        // valid - the native range is -32768..+32767, so we simply
        // shift the range up to normalize it to 0..65535
        auto &r = logicalChannel[ch].raw;
        return { r.sample + 32768, r.timestamp };
    }
    else
    {
        // invalid channel - return a null sample (all zeroes)
        return { 0, 0 };
    }
}

void ADS1115::IRQ()
{
    // check for our interrupt
    if ((gpio_get_irq_event_mask(gpAlertRdy) & GPIO_IRQ_EDGE_FALL) != 0)
    {
        // set the alert/ready flag
        alertReadyFlag = true;

        // count the interrupt
        uint64_t t = time_us_64();
        irqStats.n += 1;
        irqStats.tIRQ = t;
        if (curSamplingChannel >= 0)
            logicalChannel[curSamplingChannel].stats.tConvToIrqSum += t - tCurSampleStarted;

        // acknowledge the interrupt
        gpio_acknowledge_irq(gpAlertRdy, GPIO_IRQ_EDGE_FALL);
    }
}

bool ADS1115::OnI2CReady(I2CX *i2c)
{
    // count the polling opportunity
    ++nI2CReady;

    // Set up a batch of transactions: update the config register if needed,
    // read the config register to determine if the conversion is still in
    // progress, and read the value register.
    I2C::TXRXBuilder<3, 128> b;
    
    // check for a pending config register change
    if (configChangePending)
    {
        // register address 0x01 = config
        uint8_t buf[] = { CONFIG_REG_ADDR, configHi, configLo };
        b.AddWrite(buf, _countof(buf));

        // clear the pending change
        configChangePending = false;
    }

    // Check to see if a sample is ready.
    //
    // If ALERT/RDY is connected to a GPIO, we can use the ALERT/RDY
    // status to detect when the current conversion is finished.  We run
    // in single-shot mode, so ALERT/RDY simply reflects the current OS
    // bit state: low means that OS=1, which means that the conversion
    // is complete and a sample is ready.  We ALSO have to read the
    // config register in this case, since the ALERT/RDY line might be
    // shared by multiple chips, and thus reflects the OR of the ready
    // status across all of the chips.  We can tell if our sample is
    // ready in this case by checking the OS bit in the config register.
    //
    // If we don't have a GPIO assigned as the ALERT/RDY input, the only
    // way to find out if the sample is ready is to read the config
    // register.
    if (gpAlertRdy < 0 || alertReadyFlag)
    {
        // clear the alert/ready flag
        alertReadyFlag = false;

        // Read the config register (address 0x01)
        uint8_t buf[] = { CONFIG_REG_ADDR };
        b.AddRead(buf, _countof(buf), 2);

        // read the conversion register - register 0x00, receive length 2 bytes
        uint8_t buf2[] = { CONV_REG_ADDR };
        b.AddRead(buf2, _countof(buf2), 2);

        // count the polling read start
        if (curSamplingChannel >= 0)
            logicalChannel[curSamplingChannel].stats.AddPoll(time_us_64() - irqStats.tIRQ);
    }

    // if we queued any work, fire off the transaction batch
    if (b.n != 0)
    {
        i2c->MultiReadWrite(b);
        return true;
    }

    // no I2C work pending
    return false;
}


bool ADS1115::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // note the time the sample was received
    uint64_t tReceived = time_us_64();

    // We always do a batch read, with the config register followed by
    // the conversion register, two bytes each.
    if (len != 4)
        return false;

    // we should only get here when sampling is enabled
    if (curSamplingChannel < 0)
        return false;

    // if the config register OS bit (0x80 of the high byte) is set,
    // the sample has completed
    uint8_t cfg = data[0];
    if ((cfg & OS_BIT) != 0)
    {
        // Decode the big-endian INT16.  Note that this value is signed,
        // so we have to take care with the order of casting to ensure
        // that the value is sign-extended before widening.
        auto *ch = &logicalChannel[curSamplingChannel];
        ch->raw = { static_cast<int16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]), tReceived };

        // count the sample time
        ch->stats.AddConv(tReceived - tCurSampleStarted, tReceived - irqStats.tIRQ);

        // Advance to the next logical channel, wrapping after the last
        // channel.
        if (++ch, ++curSamplingChannel >= nLogicalChannels)
            curSamplingChannel = 0, ch = &logicalChannel[0];
        
        // update the config register to start a new conversion on the new AIN channel
        configHi &= ~MUX_MASK;            // clear the old pin MUX selector
        configHi |= (ch->mux << 4);       // set the pin MUX selector for the new channel
        configHi |= OS_BIT;               // set OS to start a single conversion

        // write the config register (address 0x01)
        uint8_t buf[3] = { CONFIG_REG_ADDR, configHi, configLo };
        i2c->Write(buf, _countof(buf));

        // set the start time for the new sample
        tCurSampleStarted = time_us_64();

        // new I2C transaction initiated
        return true;
    }

    // no more work to schedule for this round
    return false;
}

// command-line tols
void ADS1115::Command_main_S(const ConsoleCommandContext *c)
{
    // check for a chip number argument
    int chipNum = 0;
    if (c->argc >= 2 && isdigit(c->argv[1][0]))
        chipNum = atoi(c->argv[1]);

    // dispatch to the chip object method
    if (chipNum >= 0 && chipNum < nChipsConfigured && chips[chipNum] != nullptr)
        chips[chipNum]->Command_main(c);
    else
        return c->Printf("ads1115: invalid chip number %d\n", chipNum);
}


void ADS1115::Command_main(const ConsoleCommandContext *c)
{
    if (c->argc < 2)
        return c->Usage();

    // skip the chip number argument if present, as that's already been handled in the static dispatcher
    int argi = 1;
    if (isdigit(c->argv[argi][0]))
        ++argi;

    // scan options
    for ( ; argi < c->argc ; ++argi)
    {
        const char *a = c->argv[argi];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            // show statistics
            char readyTxt[24];
            sprintf(readyTxt, gpAlertRdy < 0 ? "Not Connected" : "GP%d", gpAlertRdy);
            c->Printf(
                "ADS1115 chip #%d (I2C addr 0x%02X)\n"
                "Config reg sent:        %02X%02X (OS=%d, MUX=%03d, PGA=%03d, MODE=%d, DR=%02d, COMP_MODE=%d, COMP_POL=%d, COMP_LAT=%d, COMP_QUE=%02d)\n"
                "Alert/Ready:            %s\n"
                "Logical channels:       %d\n"
                "Current scan channel:   %d\n"
                "I2CReady passes:        %llu\n"
                "Avg I2C pass interval:  %llu us\n",
                chipNum, i2cAddr,
                configHi, configLo,
                FormatBinary(configHi, 7, 1), FormatBinary(configHi, 4, 3), FormatBinary(configHi, 1, 3), FormatBinary(configHi, 0, 1),
                FormatBinary(configLo, 5, 3), FormatBinary(configLo, 4, 1), FormatBinary(configLo, 3, 1), FormatBinary(configLo, 2, 1),
                FormatBinary(configLo, 0, 2),
                readyTxt, nLogicalChannels, curSamplingChannel,
                nI2CReady, (time_us_64() - tStatsReset) / nI2CReady);
            
            if (gpAlertRdy >= 0)
            {
                c->Printf(
                    "Number of IRQs:         %llu\n",
                    irqStats.n);
            }

            LogicalChannel *ch = &logicalChannel[0];
            uint64_t dt = time_us_64() - tStatsReset;
            for (int i = 0 ; i < nLogicalChannels ; ++i, ++ch)
            {
                static const char *muxName[]{ "AIN0/AIN1", "AIN0/AIN3", "AIN1/AIN3", "AIN2/AIN3", "AIN0", "AIN1", "AIN2", "AIN3" };
                c->Printf(
                    "\nChannel %d:\n"
                    "  Input:                %s (MUX=%03d)\n"
                    "  Last sample (raw):    %d\n"
                    "  Timestamp:            %llu\n"
                    "  Sample count:         %llu\n"
                    "  Polling reads:        %llu\n"
                    "  Avg conv time:        %llu us\n"
                    "  Avg sample interval:  %llu us\n",
                    i,
                    muxName[ch->mux], FormatBinary(ch->mux, 0, 3),
                    ch->raw.sample,
                    ch->raw.timestamp,
                    ch->stats.nConv,
                    ch->stats.nPoll,
                    ch->stats.nPoll != 0 ? ch->stats.tConvSum / ch->stats.nPoll : 0,
                    ch->stats.nPoll != 0 ? dt / ch->stats.nPoll : 0);

                if (gpAlertRdy >= 0)
                {
                    c->Printf(
                        "  Avg conv-to-IRQ time: %llu us\n"
                        "  Avg IRQ-to-poll time: %llu us\n"
                        "  Avg IRQ-to-recv time: %llu us\n",
                        ch->stats.nPoll != 0 ? ch->stats.tConvToIrqSum / ch->stats.nPoll : 0,
                        ch->stats.nPoll != 0 ? ch->stats.tIrqToPollSum / ch->stats.nPoll : 0,
                        ch->stats.nPoll != 0 ? ch->stats.tIrqToReceiveSum / ch->stats.nPoll : 0);
                }
            }
        }
        else if (strcmp(a, "--reset-stats") == 0)
        {
            // reset statistics
            tStatsReset = time_us_64();
            nI2CReady = 0;
            irqStats.Reset();
            for (int i = 0 ; i < nLogicalChannels ; ++i)
                logicalChannel[i].stats.Reset();

            c->Printf("Cleared statistics counters\n");
        }
        else
        {
            return c->Printf("ads1115_%d: unknown option \"%s\"\n", chipNum, a);
        }
    }
}

