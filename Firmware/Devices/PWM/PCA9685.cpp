// Pinscape Pico - PCA9685 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unordered_set>
#include <vector>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/i2c.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "I2C.h"
#include "GPIOManager.h"
#include "Logger.h"
#include "Config.h"
#include "JSON.h"
#include "CommandConsole.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "PCA9685.h"

// chip instances
std::vector<std::unique_ptr<PCA9685>> PCA9685::chips;

// get a chip by number
PCA9685 *PCA9685::GetChip(int n)
{
    return n >= 0 && n < static_cast<int>(chips.size()) ? chips[n].get() : nullptr;
}

// Configure from JSON data
//
// pca9685: [
//   // PCA9685 chip #0
//   {
//     i2c: <number>,          // I2C bus number (0 or 1)
//     addr: <number>,         // I2C bus address for this chip, 7-bit notation
//     oe: <number>|<output>,  // GPIO port connected to hardware /OE (/Output Enable) line, or output manager device port definition
//     drive: "totem-pole|totem|open-drain|od",   // output port drive mode: "totem" = totem-pole, for driving an external MOSFET; "od" = open-drain, for driving LEDs directly
//   },
//   // PCA9685 chip #1, ...
// ]
//
// The "drive" property (drive mode) is required because the appropriate
// mode is a function of the hardware design, which the software can't
// infer from interrogating the chip:
//
// - "totem-pole" (or "totem") mode is used when each output port
// controls an external logic-level N-MOSFET, which acts as a low-side
// switches for a higher- current load.  The wiring for this
// configuration is simple: wire the port directly to the MOSFET gate,
// wire the MOSFET source to ground, and wire the MOSFET drain to the
// load.  The load's other terminal connects to its positive supply
// voltage.  A small resistor between the MOSFET gate and the port (10
// to 100 ohms) might be beneficial to prevent gate ringing, but no
// other external components are generally needed, so it's an easy way
// to build a high-power low-side switch with PWM control.  A good logic
// MOSFET is required, since the gate drive from the PCA9685 is only
// about 2.5V with VCC = 3.3V.
//
// - "open-drain" (or "od") mode is used when the ports directly drive
// LEDs as low-side switches.  Wire the LED anode to the positive supply
// voltage through a current-limiting resistor, and wire the LED cathode
// to the PCA9685 port.  In "od" mode, the port will sink current (up to
// 25mA) when ON and will act like an open circuit (high-Z) when OFF.
// Note that this wiring mode unavoidably exhibits startup glitching,
// where the connected LEDs will flash briefly during a power-on reset.
// This happens because the chip always resets to totem-pole mode with
// the outputs driven low, which will turn connected LEDs on.  The flash
// will end as soon as the software reconfigures the port.
//
// Note on hardware design: the /OE line should have a pull-up resistor
// to VCC, to ensure that outputs are deterministically disabled during
// power-on reset.  All Pico GPIOs power up in high-Z mode, and in that
// mode the pull-up will pull the line high, disabling the PCA9685
// ports.  This helps prevent random solenoid firing and the like during
// startup.  It also ensures that all devices are turned off after a
// reset caused by a software fault, which is critical to protecting
// devices that would overheat if continuously activated for long
// periods.
void PCA9685::Configure(JSONParser &json)
{
    // Size our chip list so that it has one entry per item in the
    // JSON array, so that we have a native slot for each JSON
    // slot.  References to the chips within the JSON data are by
    // array index, so replicating the JSON array layout with our
    // native vector layout makes it easy to resolve references
    // by index.
    auto *cfg = json.Get("pca9685");
    if (cfg->IsArray() || cfg->IsObject())
    {
        // make room
        chips.resize(cfg->Length(1));
        
        // parse each entry in the array (or just the single object, if it's not an array)
        cfg->ForEach([&json](int index, const JSONParser::Value *value)
        {
            // get and validate the settings for this instance
            uint8_t bus = value->Get("i2c")->UInt8(255);
            uint8_t addr = value->Get("addr")->UInt8(0);
            if (addr == 0 || I2C::GetInstance(bus, true) == nullptr)
            {
                Log(LOG_ERROR, "pca9685[%d]: invalid/undefined I2C bus or address\n", index);
                return;
            }

            // Check for reserved or invalid I2C addresses.  The PCA9685
            // address is always in the range 0x40..0x78, except that 0x70
            // is reserved.
            if (addr < 0x40 || addr > 0x78 || addr == 0x70)
            {
                Log(LOG_ERROR, "pca9685[%d]: invalid or reserved I2C address 0x%02x\n", index, addr);
                return;
            }

            // validate the /OE line, if connected
            std::unique_ptr<OutputManager::Device> oePort;
            if (auto *oeVal = value->Get("oe"); !value->IsUndefined())
            {
                // parse the port
                char jsonLocus[32];
                snprintf(jsonLocus, _countof(jsonLocus), "pca9685[%d].oe", index);
                oePort.reset(OutputManager::ParseDevice(
                    jsonLocus, Format("PCA9685[%d] (/OE)", index), json, oeVal, false, true));

                // abort on failure
                if (oePort == nullptr)
                    return;
                
                // set the port to HIGH to disable outputs initially
                oePort->Set(255);
            }

            // create the instance, and enroll it in the 'chips' vector
            // at the same index as in the JSON array
            auto *chip = new PCA9685(index, i2c_get_instance(bus), addr, oePort.release());
            chips[index].reset(chip);

            // if there's a MODE2 register setting, apply it
            if (auto *drive = value->Get("drive"); *drive == "totem" || *drive == "totem-pole")
                chip->mode2 = MODE2_OUTDRV | MODE2_OUTNE_LOW;
            else if (*drive == "od" || *drive == "open-drain")
                chip->mode2 = MODE2_OUTNE_Z;
            else
                Log(LOG_CONFIG, "pca9685[%d]: invalid 'drive' setting '%s' (must be 'totem-pole', 'totem', 'open-drain', or 'od')\n",
                    index, drive->String().c_str());

            // apply inverted-logic mode if specified
            if (value->Get("invertedLogic")->Bool(false))
                chip->mode2 |= MODE2_INVRT;

            // add it to the I2C bus manager for the selected bus
            I2C::GetInstance(bus, false)->Add(chip);

            // success
            char oeBuf[32];
            Log(LOG_CONFIG, "pca9685[%d] configured on I2C%d addr 0x%02x; /OE=%s, drive, %s logic\n",
                index, bus, addr,
                chip->oePort != nullptr ? chip->oePort->FullName(oeBuf, sizeof(oeBuf)) : "Not Connected",
                (chip->mode2 & MODE2_OUTDRV) != 0 ? "totem-pole" : "open-drain",
                (chip->mode2 & MODE2_INVRT) != 0 ? "inverted " : "positive");
        }, true);
    }
    else if (!cfg->IsUndefined())
    {
        // error - if the key is defined at all, it has to be an array or object
        Log(LOG_ERROR, "Config: 'pca9685' key must be an object or array\n");
    }

    // Initialize all of the chips
    int nConfigured = 0;
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            chip->Init();
            ++nConfigured;
        }
    }

    // if we configured any chips, set up a console diagnostic command
    if (nConfigured != 0)
    {
        struct PCA9685Class : CommandConsole::PWMChipClass
        {
            PCA9685Class()
            {
                name = "PCA9685";
                nInstances = chips.size();
                maxLevel = 4095;
                selectOpt = "-c <number>\tselect chip number; default is 0\n--chip <num>\tsame as -c";
            }

            virtual bool IsValidInstance(int instance) const { return instance >= 0 && instance < chips.size() && chips[instance] != nullptr; }
            virtual void ShowStats(const ConsoleCommandContext *c, int instance) const {
                c->Printf("PCA9685[%d] I2C statistics:\n");
                chips[instance]->i2cStats.Print(c);
            }
            virtual int GetNumPorts(int instance) const { return 16; }
            virtual bool IsValidPort(int instance, int port) const { return port >= 0 && port < 16; }
            virtual void SetPort(int instance, int port, int level) const { chips[instance]->Set(port, static_cast<uint16_t>(level)); }
            virtual int GetPort(int instance, int port) const { return chips[instance]->level[port]; }
            virtual void PrintPortName(const ConsoleCommandContext *c, int instance, int port) const { c->Printf("LED%d", port); }
        };
        CommandConsole::AddCommandPWMChip("pca9685", "PCA9685 chip options", new PCA9685Class());
    }
}

void PCA9685::Init()
{
    SendInitCommands();
}

void PCA9685::I2CReinitDevice(I2C *)
{
    SendInitCommands();
}

void PCA9685::SendInitCommands()
{
    // Note:  during initialization, uniquely, we can use the SDK's blocking
    // I2C APIs.  During normal operation we have to use the asynchronous
    // interface through our class I2C.

    // Check to see if the chip is currently in SLEEP mode.  If it is,
    // we're coming out of a power cycle and need to fully initialize
    // the chip.  If we're not in SLEEP mode, we must be coming out of a
    // Pico reset without a power cycle, in which case the PCA9685 is
    // already running, so we just need to set the new configuration
    // and clear outputs.
    bool sleepMode = false;
    bool ok = true;
    {
        // read MODE1
        uint8_t buf[] = { REG_MODE1 }, mode1;
        ok = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf)
             && i2c_read_timeout_us(i2c, i2cAddr, &mode1, 1, false, 1000) == 1;

        // note the SLEEP mode bit
        sleepMode = (mode1 & MODE1_SLEEP) != 0;
        if (!sleepMode)
            Log(LOG_DEBUG, "PCA9685(i2c addr 0x%02x) is already out of SLEEP mode; skipping initialization\n", i2cAddr);
    }

    // Store the configured MODE2 setting, to configure the drive mode
    {
        uint8_t buf[] = { REG_MODE2, mode2 };
        ok = ok && i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf);
    }

    // do a full initialization if we're in SLEEP mode
    if (sleepMode)
    {
        // Enable AUTO INCREMENT mode
        {
            uint8_t buf[] = { REG_MODE1, MODE1_ALLCALL | MODE1_SLEEP | MODE1_AI };
            ok = ok && i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf);
        }
        
        // Start the oscillator by clearing the SLEEP it in MODE1
        {
            uint8_t buf[] = { REG_MODE1, MODE1_ALLCALL | MODE1_AI };
            ok = ok && i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf);
        }

        // wait 500us for the oscillator to stabilize
        sleep_us(500);
    }

    // Set all outputs to FULL OFF (ON = 00:00, OFF = 00:10 - the 0x10 in
    // ALL_LED_OFF_HIGH is the FULL OFF bit)
    {
        uint8_t buf[] = { REG_ALL_LED_ON_L, 0x00, 0x00, 0x00, 0x10 };
        ok = ok && i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) == _countof(buf);
    }

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "PCA9685(i2c addr 0x%02x) device initialization %s\n", i2cAddr, ok ? "OK" : "failed");
}

void PCA9685::EnableOutputs(bool enable)
{
    for (auto &chip : chips)
    {
        // If the chip is configured and has an /OE port, set the port LOW for enabled, HIGH for disabled
        if (chip != nullptr && chip->oePort != nullptr)
            chip->oePort->Set(enable ? 0 : 255);
    }
}

void PCA9685::Set(int port, uint16_t newLevel)
{
    if (port >= 0 && port < nPorts)
    {
        // cap the level at 4095
        if (newLevel > 4095)
            newLevel = 4095;

        // stage the change
        level[port] = newLevel;

        // Set or clear the dirty bit for this output.  Note that it's
        // possible for an output to be changed and then reverted back
        // to the last transmitted state in the span between chip updates,
        // so we explicitly clear the dirty bit here if the new level is
        // the same as the transmitted level.
        uint16_t bit = (1 << port);
        if (newLevel != txLevel[port])
            dirty |= bit;
        else
            dirty &= ~bit;
    }
}

uint16_t PCA9685::Get(int port) const
{
    return port >= 0 && port < nPorts ? level[port] : 0;
}

// Populate vendor interface output device descriptors
void PCA9685::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            descs->configIndex = chip->configIndex;
            descs->devType = PinscapePico::OutputPortDesc::DEV_PCA9685;
            descs->numPorts = 16;
            descs->numPortsPerChip = 16;
            descs->pwmRes = 4096;  // 12-bit PWM
            descs->addr = (i2c_hw_index(chip->i2c) << 8) | chip->i2cAddr;
            ++descs;
        }
    }
}

// Populate vendor interface output device port descriptors
void PCA9685::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            for (int i = 0 ; i < 16 ; ++i, ++descs)
                descs->type = descs->TYPE_PWM;  // all PCA9685 ports are PWM outputs
        }
    }
}

// Populate an output level query result buffer
void PCA9685::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
{
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            for (int i = 0 ; i < 16 ; ++i)
            {
                levels->level = chip->level[i];
                ++levels;
            }
        }
    }
}

// Start an I2C transaction.  The I2C bus manager calls this when it's
// our turn to use the bus.
bool PCA9685::OnI2CReady(I2CX *i2cx)
{
    // check for work to do
    if (dirty != 0)
    {
        // Generate a command to send the minimal number of bytes that
        // accomplishes an update of all of the dirty ports as a single
        // contiguous command.  This is just a matter of finding the
        // first and last changed elements, in LEDn order, and
        // transmitting a command to set that range.  Each output has
        // FOUR registers that we need to set, so the register number
        // is incremented by 4 on each iteration.
        uint16_t bit = 0x0001;
        for (uint8_t regno = REG_LED0_ON_L, port = 0 ; port < 16 ; ++port, regno += 4, bit <<= 1)
        {
            // look for the first dirty bit
            if ((dirty & bit) != 0)
            {
                // this is the first dirty port
                int first = port;
                int firstRegno = regno;

                // scan for the last dirty bit
                int last = port;
                for (++port, regno += 4, bit <<= 1 ; port < 16 ; ++port, regno += 4, bit <<= 1)
                {
                    if ((dirty & bit) != 0)
                        last = port;
                }

                // update our internal record of the last transmitted state
                int n = last - first + 1;
                memcpy(&txLevel[first], &level[first], n * sizeof(txLevel[0]));

                // clear the dirty bits
                dirty = 0;

                // Phase calculation: this chip allows the host to control the phase
                // of each output, by setting the ON and OFF points in the counter
                // cycle independently.  The point is that we can stagger the ON
                // times of the outputs evenly across the 4096-count cycle.  If all
                // of the outputs use counter==0 as the ON trigger, there will be a
                // surge of current demand as all of the attached devices turn on
                // simultaneously.  To distribute the ON times of the 16 outputs
                // evenly, we can stagger the ON counts by 256 counts per output -
                // 0, 256, 512, ... 3840.
                //
                // It's a nice idea... but unfortunately, the implementation in the
                // chip seems to be a bit half-assed, to the point where it's best
                // not to use the phase feature.
                //
                // Here's what's wrong.  By observation, it seems that if a port's
                // ON pulse spans the counter rollover point (where the master PWM
                // counter rolls over from 4095 to 0), there will be a visible
                // glitch when the port is updated.  The chip is documented as
                // applying changes to a port's ON/OFF registers that occur during a
                // cycle at the rollover point.  What the data sheet doesn't say is
                // that, when this register update occurs, the chip also turns the
                // port off by fiat at the rollover point.  If the port is
                // programmed with a phase such that the pulse length spans the
                // rollover point, the off-by-fiat will cut short that last pulse
                // just before the update, causing a momentary drop in brightness,
                // perceptible as a little glitch on the port if an LED is attached.
                // If phase = N, N in 1..4095, all duty cycles above 4095-N will
                // show the glitch.  This makes the glitch more noticeable for
                // higher phase numbers, since a larger range of brightnesses will
                // trigger it.
                //
                // For Pinscape purposes, I think the glitch would be much more
                // troublesome than synchronized ON transitions across the ports.
                // Most Pinscape deployments will use outputs from a PWM chip like
                // the PCA9685 not as direct load drivers, but rather as inputs to
                // MOSFET or Darlington driver stages.  The current load on the
                // PCA9685 outputs will thus be fairly small, so the aggregate surge
                // load on the power supply at this stage should be easily
                // tolerated.  On the output side of the MOSFET/Darlington drivers
                // we're assuming, surges will be more significant, but this portion
                // of the system in a pin cab is inherently subject to surges anyway
                // because of the high-power devices typically used, and power
                // supplies must be sized appropriately.  I don't think we'll be
                // making anything worse by ignoring the phasing feature.
                const uint16_t deltaPhase = 0;
                uint16_t onPhase = first * deltaPhase;

                // Build the command buffer.  For each output, we need to update
                // LEDx_ON_L, LEDx_ON_H, LEDx_OFF_L, and LEDx_OFF_H.
                uint8_t buf[1 + 16*4] = { static_cast<uint8_t>(firstRegno) };
                uint8_t *dst = &buf[1];
                const uint16_t *src = &level[first];
                for (int i = 0 ; i < n ; ++i, onPhase += deltaPhase)
                {
                    // Check for fully on (level 4095) and fully off (level 0).
                    uint16_t v = *src++;
                    if (v == 0)
                    {
                        // fully off - set ON=00:00, OFF=00:10, to set the FULL OFF
                        // bit 0x10 in LEDx_OFF_H.
                        *dst++ = 0x00;
                        *dst++ = 0x00;
                        *dst++ = 0x00;
                        *dst++ = 0x10;   // FULL OFF bit 0x10
                    }
                    else if (v == 4095)
                    {
                        // max value -> fully on - set ON=00:10, OFF=00:00, to set
                        // the FULL ON bit 0x10 in LEDx_ON_H
                        *dst++ = 0x00;
                        *dst++ = 0x10;   // FULL ON bit 0x10
                        *dst++ = 0x00;
                        *dst++ = 0x00;
                    }
                    else
                    {
                        // Scaled value between 0 and 4095.  Set the ON time to a
                        // rolling phase value according to the register position,
                        // and set the OFF time to the start time plus the duty
                        // cycle.
                        uint16_t offPhase = (onPhase + v) % 4096;
                        *dst++ = static_cast<uint8_t>(onPhase & 0xFF);         // ON low byte
                        *dst++ = static_cast<uint8_t>((onPhase >> 8) & 0x0F);  // ON high byte, clear FULL ON bit 0x10
                        *dst++ = static_cast<uint8_t>(offPhase & 0xFF);        // OFF low byte
                        *dst++ = static_cast<uint8_t>((offPhase >> 8) & 0x0F); // OFF high byte, clear FULL OFF bit 0x10
                    }
                }

                // Start the I2C transmission.  This is a one-way WRITE
                // command, with no response to read back.
                i2cx->Write(buf, dst - &buf[0]);

                // done, transaction started
                return true;
            }
        }
    }

    // we didn't find any dirty elements, to there's no work
    // to send on this round
    return false;
}

// Receive I2C data.  The I2C bus manager calls this when it receives
// the response to a READ operation that we initiated.
bool PCA9685::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we don't currently solicit any input, so this should never
    // be called

    // no further transaction required
    return false;
}
