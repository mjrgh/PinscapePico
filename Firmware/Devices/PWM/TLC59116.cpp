// Pinscape Pico - TLC59116 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Important note:  DO NOT USE THE DEFAULT ALL-CALL ADDRESS!
// The TLC59116 features an optional All-Call addressing mode.  This sets
// a special I2C address that all chips on the bus will acknowledge, which
// allows the host to broadcast an LED update simultaneously to all of the
// chips on the bus with a single write transaction.  This is great if
// you're using the chips to control something like a matrix display where
// the visual effect requires precisely synchronized updates.  Our virtual
// pin cab use case has no need for this, so we don't use it.  But more
// importantly, we MUST NOT use it with the default All-Call address,
// because the default address, 0x68, conflicts with another device we
// also support, DS3231M (an RTC chip).  The DS3231M's use of address 0x68
// isn't configurable, so there's nothing we can do on that end to avoid
// the conflict this creates.  We can, however, reprogram the TLC59116's
// All-Call address (via the ALLCALLADR register, 0x1B), or disable the
// All-Call feature entirely via the ALLCALL bit in MODE1 (register 0x00).
// As long as we don't need the feature, the best option is to disable
// it via the MODE1 setting, which we currently do during initialization.
// If some future update to this code requires enabling the All-Call
// feature, it should also reprogram the address in register 0x1B to
// avoid the conflict with DS3231M (and any ohter chips that happen to
// lie somewhere in the 0x60-0x6F address range).
// 
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
#include <hardware/watchdog.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "I2C.h"
#include "Logger.h"
#include "Config.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "Outputs.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "CommandConsole.h"
#include "TLC59116.h"


// chip instances
std::vector<std::unique_ptr<TLC59116>> TLC59116::chips;

// get a chip by number
TLC59116 *TLC59116::GetChip(int n)
{
    return n >= 0 && n < static_cast<int>(chips.size()) ? chips[n].get() : nullptr;
}

// Disable All Call globally across all chips.  This is called during
// startup initialization, before I2C task scheduling starts and before
// we configure any chips, so we can use blocking I2C transactions.
void TLC59116::DisableAllCall(int bus)
{
    // update MODE1 to disable All Call; leave the OSCOFF (oscillator off/sleep)
    // bit set for now, since we're not doing the full initialization yet
    uint8_t tx[] = { REG_MODE1, MODE1_AI2 | MODE1_OSCOFF };

    // scan the bus
    if (auto *i2c = I2C::GetInstance(bus, true); i2c != nullptr)
    {
        // scan each valid address
        auto *hw = i2c->GetHwInst();
        for (uint8_t addr = 0x60 ; addr <= 0x6F ; ++addr)
        {
            // skip the reserved addresses - 68 = All-Call, 6B = SWRST
            if (!(addr == 0x68 || addr== 0x6B))
            {
                // read the current MODE1 register
                uint8_t tx1 = REG_MODE1;
                uint8_t mode1;
                if (i2c_write_timeout_us(hw, addr, &tx1, 1, true, 1000) == 1
                    && i2c_read_timeout_us(hw, addr, &mode1, 1, false, 1000) == 1)
                {
                    // success - set AI2, clear ALLCALL, and write it back
                    mode1 &= ~MODE1_ALLCALL;
                    mode1 |= MODE1_AI2;
                    uint8_t tx2[2] = { REG_MODE1, mode1 };
                    i2c_write_timeout_us(hw, addr, tx2, _countof(tx2), false, 1000);
                }

                // reset the watchdog, since the bus operations take a couple
                // hundred microseconds
                watchdog_update();
            }
        }
    }
}

// Configure from JSON data
//
// tlc59116: [
//   // TLC59116 chip #0
//   {
//     i2c: <number>,          // I2C bus number (0 or 1)
//     addr: <number>,         // I2C bus address for this chip, 7-bit notation
//     reset: <gpNumber>,      // GPIO port connected to hardware /RESET line (can be shared by multiple chips)
//   },
//   // TLC59116 chip #1, ...
// ]
//
// The /RESET GPIO isn't required, but it's strongly recommended,
// because it ensures that all of the TLC59116 output ports are
// deterministically turned off at power-on and during all software or
// hardware reset cycles.  That's a critical element of our fail-safe
// design for high-power outputs that require time-limiter protection
// ("Flipper Logic").  The software isn't able to enforce port time
// limiters during power transitions, so it's up to the hardware design
// to ensure that all ports are turned off whenever the Pico is in a
// state where the software isn't running.  For TLC59116-controlled
// outputs, this can be accomplished via the TLC59116's hard reset line,
// by tying the reset line to ground via a pull-down resistor.  That
// guarantees that all TLC59116 output ports are off whenever the
// software isn't explicitly countermanding the output port reset by
// driving the GPIO high.  When the Pico is resetting or coming out of
// reset, it sets all GPIOs to high-Z state, so the pull-down to ground
// on the reset line will take precedence; when the software is ready to
// take control of the ports, with time limiters as needed, it takes
// over by driving the GPIO high.
//
void TLC59116::Configure(JSONParser &json)
{
    // set the native chip array size to match the JSON array
    auto *cfg = json.Get("tlc59116");
    if (cfg->IsObject() || cfg->IsArray())
    {
        // make room
        chips.resize(cfg->Length(1));
    
        // parse each entry in the array (or just the single object, if it's not an array)
        cfg->ForEach([](int index, const JSONParser::Value *value)
        {
            // get and validate the settings for this instance
            uint8_t bus = value->Get("i2c")->UInt8(255);
            uint8_t addr = value->Get("addr")->UInt8(0);
            int gpReset = value->Get("reset")->Int(-1);
            if (addr == 0 || I2C::GetInstance(bus, true) == nullptr)
            {
                Log(LOG_ERROR, "tlc59116[%d]: invalid/undefined I2C bus or address\n", index);
                return;
            }

            // check for reserved or invalid I2C addresses
            if (addr < 0x60 || addr > 0x6F || addr == 0x68 || addr == 0x6B)
            {
                Log(LOG_ERROR, "tlc59116[%d]: invalid or reserved address 0x%02x (must be 0x60..0x6F excluding reserved addresses 0x68, 0x6B)\n", index, addr);
                return;
            }

            // validate the RESET line
            if (gpReset != -1 && !IsValidGP(gpReset))
            {
                Log(LOG_ERROR, "tlc59116[%d]: invalid or undefined reset GP\n", index);
                return;
            }

            // create the instance and enroll it in the chip list at the same index
            // as the JSON source data
            auto *chip = new TLC59116(index, i2c_get_instance(bus), addr, gpReset);
            chips[index].reset(chip);

            // add it to the I2C bus manager for the selected bus
            I2C::GetInstance(bus, false)->Add(chip);

            // success
            Log(LOG_CONFIG, "tlc59116[%d] configured on I2C%d addr 0x%02x\n", index, bus, addr);
        }, true);
    }
    else if (!cfg->IsUndefined())
        Log(LOG_ERROR, "Config: 'tlc59116' key must be an object or array\n");

    // Set up the RESET lines.  In most cases, all TLC59116 chips will
    // share a single RESET line, but some hardware designs might use
    // a separate line per chip or per group of chips.  So start by
    // building a list of unique GP lines.
    std::unordered_set<uint8_t> resets;
    for (auto &chip : chips)
    {
        if (chip != nullptr && chip->gpReset != -1 && resets.find(chip->gpReset) == resets.end())
            resets.emplace(chip->gpReset);
    }

    // now set up each reset line
    for (auto gp : resets)
    {
        // claim the GPIO
        if (!gpioManager.Claim("TLC59116 (RESET)", gp))
            return;

        // Set up the reset GPIO.  Configure it as a GPIO output,
        // initially logic low, so that we continue asserting a hard
        // /RESET signal on the TLC59116.  ("Continue", in that the line
        // should be pulled down to ground via a resistor, ensuring that
        // it's deterministically pulled low whenever the software isn't
        // explicitly driving the line high.)
        gpio_init(gp);
        gpio_put(gp, 0);
        gpio_set_dir(gp, GPIO_OUT);
        gpio_set_drive_strength(gp, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_pulls(gp, true, false);
    }

    // Pause briefly with the /RESET lines all held low.  The data sheet
    // calls for a minimum /RESET pulse time of 10ns, so the CPU time it
    // takes to get to the next instruction is really all we need, but
    // let's put in a short delay to be sure.
    sleep_us(2);

    // Now enable all of the chips by driving the /RESET line(s) high.
    // The /RESET line(s) must remain high from this point on for normal
    // operation.
    for (auto gp : resets)
        gpio_put(gp, 1);

    // Pause briefly again to let the chips stabilize as they come out
    // of the reset.  The data sheet specifies the reset time as 400ns,
    // so a few microseconds should be more than enough.
    sleep_us(10);

    // Now initialize all of the chips
    int nConfigured = 0;
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            chip->Init();
            ++nConfigured;
        }
    }

    // if any chips are configured, add the TLC59116 console command
    if (nConfigured != 0)
    {
        struct TLC59116Class : CommandConsole::PWMChipClass
        {
            TLC59116Class()
            {
                name = "TLC59116";
                nInstances = chips.size();
                maxLevel = 255;
                selectOpt = "-c <number>\tselect chip number; default is 0\n--chip <num>\tsame as -c";
            }

            virtual bool IsValidInstance(int instance) const { return instance >= 0 && instance < chips.size() && chips[instance] != nullptr; }
            virtual void ShowStats(const ConsoleCommandContext *c, int instance) const {
                c->Printf("TLC59116[%d] I2C statistics:\n");
                chips[instance]->i2cStats.Print(c);
            }
            virtual int GetNumPorts(int instance) const { return 16; }
            virtual bool IsValidPort(int instance, int port) const { return port >= 0 && port < 16; }
            virtual void SetPort(int instance, int port, int level) const { chips[instance]->Set(port, static_cast<uint8_t>(level)); }
            virtual int GetPort(int instance, int port) const { return chips[instance]->level[port]; }
            virtual void PrintPortName(const ConsoleCommandContext *c, int instance, int port) const { c->Printf("OUT%d", port); }
        };
        CommandConsole::AddCommandPWMChip("tlc59116", "TLC5116 chip options", new TLC59116Class());
    }
}

void TLC59116::Init()
{
    // Note:  during initialization, uniquely, we can use the SDK's blocking
    // I2C APIs.  During normal operation we have to use the asynchronous
    // interface through our class I2C.

    // Clear the SLEEP bit in MODE1 to turn on the oscillator.  Also turn off
    // ALLCALL to disable response to the broadcast All-Call address.
    //
    // Important: DO NOT USE THE POWER-ON DEFAULT ALL-CALL ADDRESS (0x68).
    // That conflicts with the DS3231M RTC chip's fixed address.  At the moment,
    // we simply disable the All-Call feature entirely, but if some future update
    // to this code enables it, the All-Call address set in register 0x1B must
    // be changed to a different address to avoid conflicting with DS3231M.
    //
    // Register = MODE1 (0x00)
    // Value = AI2 (auto-increment all registers, roll over to 00000 after 11011)
    uint8_t buf2[] = { REG_MODE1, MODE1_AI2 };
    int result = i2c_write_timeout_us(i2c, i2cAddr, buf2, _countof(buf2), false, 1000);
    bool ok = (result == _countof(buf2));

    // wait 500us after turning on the oscillator
    watchdog_update();
    sleep_us(500);
    watchdog_update();

    // Set all output drivers to individual PWM control:
    //  Register address = LEDOUT0 | AI (auto-increment enabled)
    //  Set LEDOUT0 = 0b10101010  (0b10 in each 2-bit LED block, for "individual PWM control")
    //  Set LEDOUT1 = 0b10101010  (by auto-increment)
    //  Set LEDOUT2 = 0b10101010
    //  Set LEDOUT3 = 0b10101010
    const uint8_t ALL_PWM = LEDOUT_PWM | (LEDOUT_PWM << 2) | (LEDOUT_PWM << 4) | (LEDOUT_PWM << 6);
    uint8_t buf[] = { REG_LEDOUT0 | CTL_AIALL, ALL_PWM, ALL_PWM, ALL_PWM, ALL_PWM };
    result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
    ok = ok && (result == _countof(buf));

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "tlc59116[%d], I2C%d addr 0x%02x: device initialization %s\n",
        configIndex, i2c_hw_index(i2c), i2cAddr, ok ? "OK" : "failed");
}

void TLC59116::I2CReinitDevice(I2C *)
{
    Init();
}

// set a port level
void TLC59116::Set(int port, uint8_t newLevel)
{
    if (port >= 0 && port < nPorts)
    {
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

// get a port level
uint8_t TLC59116::Get(int port) const
{
    return port >= 0 && port < nPorts ? level[port] : 0;
}

// Populate vendor interface output device descriptors
void TLC59116::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            descs->configIndex = chip->configIndex;
            descs->devType = PinscapePico::OutputPortDesc::DEV_TLC59116;
            descs->numPorts = 16;
            descs->numPortsPerChip = 16;
            descs->pwmRes = 256;  // 8-bit PWM
            descs->addr = (i2c_hw_index(chip->i2c) << 8) | chip->i2cAddr;
            ++descs;
        }
    }
}

// Populate vendor interface output device port descriptors
void TLC59116::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &chip : chips)
    {
        if (chip != nullptr)
        {
            for (int i = 0 ; i < 16 ; ++i, ++descs)
                descs->type = descs->TYPE_PWM;  // all TLC59116 ports are PWM outputs
        }
    }
}

// Populate an output level query result buffer
void TLC59116::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
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
bool TLC59116::OnI2CReady(I2CX *i2cx)
{
    // check for work to do
    if (dirty != 0)
    {
        // Generate a command to send the minimal number of bytes
        // that accomplishes an update of all of the dirty ports
        // as a single contiguous command.  This is just a matter
        // of finding the first and last changed elements, in LEDn
        // order, and transmitting a command to set that range.
        uint16_t bit = 0x0001;
        for (uint8_t regno = REG_PWM0 ; regno <= REG_PWM15 ; ++regno, bit <<= 1)
        {
            // look for the first dirty bit
            if ((dirty & bit) != 0)
            {
                // this is the first dirty bit
                int first = regno;

                // scan for the last dirty bit
                int last = regno;
                for (++regno, bit <<= 1 ; regno <= REG_PWM15 ; ++regno, bit <<= 1)
                {
                    if ((dirty & bit) != 0)
                        last = regno;
                }

                // update our internal record of the last transmitted state
                int n = last - first + 1;
                int idx0 = first - REG_PWM0;
                memcpy(&txLevel[idx0], &level[idx0], n * sizeof(txLevel[0]));

                // clear the dirty bits
                dirty = 0;

                // Build the command buffer to send the PWMn register
                // updates, including only the stretch from first to
                // last dirty elements.  The longest possible command is
                // 17 bytes (register address byte plus 16 LED level
                // bytes).  The starting address register is 'first',
                // with the auto-increment flag set for the PWM group.
                uint8_t buf[17] = { static_cast<uint8_t>(CTL_AIPWM | first) };
                uint8_t *dst = &buf[1];
                const uint8_t *src = &level[idx0];
                for (int i = 0 ; i < n ; ++i)
                    *dst++ = *src++;

                // Start the I2C transmission.  This is a one-way WRITE
                // command, with no response to read back.
                i2cx->Write(buf, n + 1);
                               
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
bool TLC59116::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we don't currently solicit any input, so this should never
    // be called

    // no further transaction required
    return false;
}
