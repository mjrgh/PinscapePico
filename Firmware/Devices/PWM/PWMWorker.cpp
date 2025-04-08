// Pinscape Pico firmware - PWMWorker device interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the Pinscape Pico interface to the PWMWorker device.  A
// PWMWorker is a Pico running our special PWMWorker firmware, which
// turns the Pico into an I2C slave device that exposes an interface
// similar to a dedicated PWM controller chip like a TLC59116 or
// PCA9685.  A Pico running PWMWorker can act as a substitute for PWM
// chips of that sort, providing 24 channels of PWM outputs through GPIO
// pins (the other two GPIOs provide the I2C connection to the host Pico
// running Pinscape).

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
#include "Outputs.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "CommandConsole.h"
#include "PWMWorker.h"

// device instances
std::vector<std::unique_ptr<PWMWorker>> PWMWorker::units;

// get a unit by configuration index
PWMWorker *PWMWorker::GetUnit(int n)
{
    return n >= 0 && n < static_cast<int>(units.size()) ? units[n].get() : nullptr;
}

// Configure from JSON data
//
// workerPico: [
//   // Pico #0
//   {
//     i2c: <number>,          // I2C bus number (0 or 1)
//     addr: <number>,         // I2C bus address for this chip, 7-bit notation
//     pwmFreq: <number>,      // PWM frequency for the ports on this chip
//   },
//   // Pico #1, ...
// ]
//
void PWMWorker::Configure(JSONParser &json)
{
    // set the native unit array size to match the JSON array
    auto *cfg = json.Get("workerPico");
    if (cfg->IsObject() || cfg->IsArray())
    {
        // make room
        units.resize(cfg->Length(1));
    
        // parse each entry in the array (or just the single object, if it's not an array)
        cfg->ForEach([](int index, const JSONParser::Value *value)
        {
            // get and validate the I2C bus number
            char facility[30];
            snprintf(facility, _countof(facility), "workerPico[%d]", index);
            int bus = value->Get("i2c")->Int(-1);
            if (!I2C::ValidateBusConfig(facility, bus))
                return;

            // get and validate the I2C address
            uint8_t addr = value->Get("addr")->UInt8(0);
            if (addr == 0)
            {
                Log(LOG_ERROR, "workerPico[%d]: invalid/undefined I2C address\n", index);
                return;
            }
            if (addr < 8 || addr >= 0x7C)
            {
                Log(LOG_ERROR, "workerPico[%d]: invalid I2C address 0x%02x (0x00-0x07 and 0x7C-0x7F are reserved)\n", index, addr);
                return;
            }

            // get and validate the PWM frequency
            int pwmFreq = value->Get("pwmFreq")->Int(20000);
            if (pwmFreq < 8 || pwmFreq > 65535)
            {
                Log(LOG_ERROR, "workerPico[%d]: invalid PWM frequency; must be 8-65535\n", index, pwmFreq);
                return;
            }

            // create the instance and enroll it in the unit list at the same index
            // as the JSON source data  
            auto *unit = new PWMWorker(index, i2c_get_instance(bus), addr, pwmFreq);
            units[index].reset(unit);

            // add it to the I2C bus manager for the selected bus
            I2C::GetInstance(bus, false)->Add(unit);

            // success
            Log(LOG_CONFIG, "workerPico[%d] configured on I2C%d addr 0x%02x\n", index, bus, addr);
        }, true);
    }
    else if (!cfg->IsUndefined())
    {
        Log(LOG_ERROR, "Config: 'workerPico' key must be an object or array\n");
    }

    // Initialize all of the units
    int nConfigured = 0;
    for (auto &unit : units)
    {
        if (unit != nullptr)
        {
            unit->Init();
            ++nConfigured;
        }
    }

    // if any units are configured, add the console command
    if (nConfigured != 0)
    {
        struct PWMWorkerClass : CommandConsole::PWMChipClass
        {
            PWMWorkerClass()
            {
                name = "WorkerPico";
                nInstances = units.size();
                maxLevel = 255;
                selectOpt = "-u <number>\tselect unit number; default is 0\n--unit <num>\tsame as -u";
            }

            virtual bool IsValidInstance(int instance) const { return instance >= 0 && instance < units.size() && units[instance] != nullptr; }
            virtual void ShowStats(const ConsoleCommandContext *c, int instance) const {
                c->Printf("WorkerPico[%d] I2C statistics:\n");
                units[instance]->i2cStats.Print(c);
            }
            virtual int GetNumPorts(int instance) const { return 24; }
            virtual bool IsValidPort(int instance, int port) const { return port >= 0 && port <= 23; }
            virtual void SetPort(int instance, int port, int level) const { units[instance]->Set(port, static_cast<uint8_t>(level)); }
            virtual int GetPort(int instance, int port) const { return units[instance]->level[port]; }
            virtual void PrintPortName(const ConsoleCommandContext *c, int instance, int port) const { c->Printf("GP%d", port == 23 ? 28 : port); }
        };
        CommandConsole::AddCommandPWMChip("worker", "Worker Pico control options", new PWMWorkerClass());
    }
}

void PWMWorker::Init()
{
    // Note:  during initialization, uniquely, we can use the SDK's blocking
    // I2C APIs.  During normal operation we have to use the asynchronous
    // interface through our class I2C.

    // Request a register reset, to make sure the device is in a known state,
    // in case the Pinscape unit went through a software reset that didn't
    // also reset the remote Pico, leaving a previous register configuration
    // in place.
    bool ok = false;
    {
        // Set the RESET REGISTERS bit in CTRL0 to request a reset.  Allow
        // some time for the other Pico to complete its initialization.
        uint64_t tEnd = time_us_64() + 20000;
        do
        {
            // try the reset; stop looping on success
            const uint8_t tx[2] = { REG_CTRL0, CTRL0_RESET_REGS };
            if (i2c_write_timeout_us(i2c, i2cAddr, tx, _countof(tx), false, 1000) == _countof(tx))
            {
                ok = true;
                break;
            }
        } while (time_us_64() < tEnd);

        // if that was successful, wait for the reset to complete
        if (ok)
        {
            // now poll CTRL0 until the RESET REGISTERS bit clears, indicating
            // that the reset has completed
            uint8_t rx = CTRL0_RESET_REGS;
            tEnd = time_us_64() + 10000;
            do
            {
                // let the watchdog know that the delay is intentional
                watchdog_update();

                // poll the register
                const uint8_t tx[1] = { REG_CTRL0 };
                if (i2c_write_timeout_us(i2c, i2cAddr, tx, 1, true, 1000) != 1
                    || i2c_read_timeout_us(i2c, i2cAddr, &rx, 1, false, 1000) != 1)
                {
                    Log(LOG_ERROR, "WorkerPico[%d]: I2C error polling CTRL0 for register reset\n");
                    ok = false;
                    break;
                }

                // if the bit has cleared, we can stop now
                if ((rx & CTRL0_RESET_REGS) == 0)
                    break;
            }
            while (time_us_64() < tEnd);

            // make sure the bit cleared before we timed out
            if ((rx & CTRL0_RESET_REGS) != 0)
            {
                ok = false;
                Log(LOG_ERROR, "WorkerPico[%d]: timed out waiting for register reset to complete\n");
            }
        }
        else
        {
            Log(LOG_ERROR, "WorkerPico[%d]: I2C error requesting register reset (CTRL0_RESET_REGS)\n");
        }
    }

    // check the WHOAMI ID and version
    char verInfo[64];
    {
        uint8_t addr = REG_VERSION;
        uint8_t buf[2];
        if (i2c_write_timeout_us(i2c, i2cAddr, &addr, 1, true, 1000) != 1
            || i2c_read_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "WorkerPico[%d]: I2C error reading device ID registers\n");
            ok = false;
        }
        else if (buf[1] == WHOAMI_ID)
        {
            sprintf(verInfo, ", WHOAMI=0x%02X (OK), Ver %d", buf[1], buf[0]);
        }
        else
        {
            sprintf(verInfo, ", WHOAMI=0x%02X (MISMATCH: expected 0x%02X), Ver %d", buf[1], WHOAMI_ID, buf[0]);
        }
    }

    // set the PWM frequency
    {
        uint8_t buf[] = { REG_FREQL, static_cast<uint8_t>(pwmFreq & 0xFF), static_cast<uint8_t>((pwmFreq >> 8) & 0xFF) };
        if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        {
            Log(LOG_ERROR, "WorkerPico[%d]: I2C error setting PWM frequency\n");
            ok = false;
        }
    }

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "WorkerPico[%d], I2C%d address 0x%02x: device initialization %s%s\n",
        configIndex, i2c_hw_index(i2c), i2cAddr, ok ? "OK" : "failed", verInfo);
}

void PWMWorker::ConfigurePort(int port, bool gamma, bool activeLow)
{
    // format the CONF<n> register bit map
    uint8_t conf = 0;
    if (gamma) conf |= CONF_GAMMA_ENA;
    if (activeLow) conf |= CONF_ACTIVE_LOW;
    
    // build the transaction buffer
    uint8_t buf[] = {
        static_cast<uint8_t>(REG_CONF(port)),         // starting address = CONF<n>
        static_cast<uint8_t>(conf),                   // CONF<n> register value
    };

    // send the command
    if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        Log(LOG_ERROR, "WorkerPico[%d]: I2C error writing CONFn for port %d\n", configIndex, port);
}

void PWMWorker::ConfigureFlipperLogic(int port, uint8_t limitLevel, uint16_t timeout)
{
    // build the transaction buffer
    uint8_t buf[] = {
        static_cast<uint8_t>(REG_LIMIT(port)),        // starting address = LIMIT<n>
        limitLevel,                                   // LIMIT<n> register value
        static_cast<uint8_t>(timeout & 0xFF),         // TIMELIMIT<n> low byte
        static_cast<uint8_t>((timeout >> 8) & 0xFF),  // TIMELIMIT<n> high byte
    };

    // send the command
    if (i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000) != _countof(buf))
        Log(LOG_ERROR, "WorkerPico[%d]: I2C error writing LIMITn, TIMELIMITn for port %d\n", configIndex, port);
}

void PWMWorker::I2CReinitDevice(I2C *)
{
    Init();
}

void PWMWorker::EnableOutputs(bool enable)
{
    for (auto &unit : units)
    {
        if (unit != nullptr)
        {
            // set or clear the enable bit in the CTRL0 register
            if (enable)
                unit->ctrl0.localVal |= CTRL0_ENABLE_OUTPUTS;
            else
                unit->ctrl0.localVal &= ~CTRL0_ENABLE_OUTPUTS;
        }
    }
}

// set a port level
void PWMWorker::Set(int port, uint8_t newLevel)
{
    // validate the abstract port number (0..23)
    if (port >= 0 && port <= 23)
    {
        // stage the change
        level[port] = newLevel;

        // Set or clear the dirty bit for this output.  Note that it's
        // possible for an output to be changed and then reverted back
        // to the last transmitted state in the span between chip updates,
        // so we explicitly clear the dirty bit here if the new level is
        // the same as the transmitted level.
        uint32_t bit = (1UL << port);
        if (newLevel != txLevel[port])
            dirty |= bit;
        else
            dirty &= ~bit;
    }
}

// get a port level
uint8_t PWMWorker::Get(int port) const
{
    return (port >= 0 && port <= 23) ? level[port] : 0;
}

// Populate vendor interface output device descriptors
void PWMWorker::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &unit : units)
    {
        if (unit != nullptr)
        {
            descs->configIndex = unit->configIndex;
            descs->devType = PinscapePico::OutputPortDesc::DEV_PWMWORKER;
            descs->numPorts = 24;
            descs->numPortsPerChip = 24;
            descs->pwmRes = 256;  // 8-bit PWM
            descs->addr = (i2c_hw_index(unit->i2c) << 8) | unit->i2cAddr;
            ++descs;
        }
    }
}

// Populate vendor interface output device port descriptors
void PWMWorker::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &unit : units)
    {
        if (unit != nullptr)
        {
            for (int i = 0 ; i < 24 ; ++i, ++descs)
                descs->type = descs->TYPE_PWM;  // all ports are PWM outputs
        }
    }
}

// Populate an output level query result buffer
void PWMWorker::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
{
    for (auto &unit : units)
    {
        if (unit != nullptr)
        {
            for (int i = 0 ; i < 24 ; ++i)
            {
                levels->level = unit->level[i];
                ++levels;
            }
        }
    }
}

void PWMWorker::RebootRemotePico(int unitNum, bool bootLoaderMode)
{
    if (unitNum >= 0 && unitNum < units.size() && units[unitNum] != nullptr)
        units[unitNum]->Reboot(bootLoaderMode);
}

void PWMWorker::ChipReg::OnI2CReady(I2C::TXRXBuilderBase *b)
{
    // if the local value has changed since the last update, add a TX transaction
    // to send the upate
    if (chipVal != localVal)
    {
        // add the transaction to set this register
        const uint8_t buf[2]{ addr, localVal };
        b->AddWrite(buf, _countof(buf));

        // consider the chip up to date with the local value
        chipVal = localVal;
    }
}


// Start an I2C transaction.  The I2C bus manager calls this when it's
// our turn to use the bus.
bool PWMWorker::OnI2CReady(I2CX *i2cx)
{
    // I2C transaction builder, to bundle control register updates and port updates
    I2C::TXRXBuilder<2, 64> txb;

    // If a reboot is pending, just send that
    if (swresetPending != 0)
    {
        // send the two-step reset sequence
        uint8_t tx1[]{ REG_SWRESET, SWRESET_START };   // send the Start Reset Sequence code
        uint8_t tx2[]{ REG_SWRESET, swresetPending };  // ..then send the actual reset command to execute
        txb.AddWrite(tx1, _countof(tx1));
        txb.AddWrite(tx2, _countof(tx2));
        i2cx->MultiReadWrite(txb);

        // the pending command has been sent - clear it
        swresetPending = 0x00;

        // stop here - there's no need to send any other pending updates when
        // we just scheduled a reset
        return true;
    }

    // check for register updates
    ctrl0.OnI2CReady(&txb);

    // check for pending port level changes
    if (dirty != 0)
    {
        // We have at least one port that needs updating.
        //
        // Generate a command to send the minimal number of bytes
        // that accomplishes an update of all of the dirty ports
        // as a single contiguous command.  This is just a matter
        // of finding the first and last changed elements, in LEDn
        // order, and transmitting a command to set that range.
        uint32_t bit = 0x0001;
        for (uint8_t regno = REG_LED0 ; regno < REG_LED0+24 ; ++regno, bit <<= 1)
        {
            // look for the first dirty bit
            if ((dirty & bit) != 0)
            {
                // this is the first dirty bit
                int first = regno;

                // scan for the last dirty bit
                int last = regno;
                for (++regno, bit <<= 1 ; regno < REG_LED0+24 ; ++regno, bit <<= 1)
                {
                    if ((dirty & bit) != 0)
                        last = regno;
                }

                // update our internal record of the last transmitted state
                int n = last - first + 1;
                int idx0 = first - REG_LED0;
                memcpy(&txLevel[idx0], &level[idx0], n * sizeof(txLevel[0]));

                // Build the command buffer to send the LEDn register
                // updates, including only the stretch from first to
                // last dirty elements.  The longest possible command is
                // 25 bytes (register address byte plus 24 LED level
                // bytes).  The starting address register is 'first'.
                uint8_t buf[25] = { static_cast<uint8_t>(first) };
                uint8_t *dst = &buf[1];
                const uint8_t *src = &level[idx0];
                for (int i = 0 ; i < n ; ++i)
                    *dst++ = *src++;

#if 0 // Low-level debug instrumentation - disable by default
                {
                    char hexBytes[26*3+1];
                    char *ph = hexBytes;
                    for (int i = 0 ; i < n + 1 ; ++i)
                    {
                        uint8_t b = buf[i];
                        uint8_t bl = b & 0x0F;
                        uint8_t bh = (b >> 4) & 0x0F;
                        *ph++ = bh + (bh < 10 ? '0' : 'A' - 10);
                        *ph++ = bl + (bl < 10 ? '0' : 'A' - 10);
                        *ph++ = ' ';
                    }
                    *(ph-1) = 0;
                    Log(LOG_DEBUG, "Worker[%d]: dirty %06lx, sending LED%d n=%d [%s]\n", configIndex, dirty, idx0, n, hexBytes);
                }
#endif // end debug

                // add the transaction to the builder
                txb.AddWrite(buf, n + 1);

                // txLevel is now up to date with level
                dirty = 0;
            }
        }
    }

    // if there's any I2C work to send, start it
    if (txb.n != 0)
    {
        i2cx->MultiReadWrite(txb);
        return true;
    }

    // no work for this round
    return false;
}

// Receive I2C data.  The I2C bus manager calls this when it receives
// the response to a READ operation that we initiated.
bool PWMWorker::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c)
{
    // we don't currently solicit any input, so this should never
    // be called

    // no further transaction required
    return false;
}
