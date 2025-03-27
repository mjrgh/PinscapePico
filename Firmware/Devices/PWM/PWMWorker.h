// Pinscape Pico firmware - PWMWorker device interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is the interface to an external Pico running our PWMWorker
// firmware.  PWMWorker turns the Pico into a virtual 24-channel PWM
// controller chip, similar to a TLC59116 or PCA9685.  The PWMWorker
// Pico acts as an I2C slave, with a register interface that allows the
// host Pico to control 24 of the slave Pico's GPIO ports as PWM
// outputs.  From the host's perspective (i.e., this program), the
// worker Pico's output ports are abstractly labeled 0 to 23, so that we
// don't have to know or care about the physical GPIO pin mapping on the
// worker Pico.
//
// The I2C register interface that the PWMWorker program exposes is
// intentionally similar to the interfaces of the common dedicated PWM
// chips, so that the interface is familiar to programmers who've worked
// with other chips, and to make it relatively easy to adapt an existing
// driver for one of the other chips to the new device.  Our driver here
// is very similar to our TLC59116 driver.

#pragma once
// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <numeric>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "I2C.h"

// external declarations
class JSONParser;
class ConsoleCommandContext;
namespace PinscapePico {
    struct OutputDevDesc;
    struct OutputDevPortDesc;
    struct OutputDevLevel;
}

// PWM Worker unit.  Each instance of this object represents one
// physical external Pico running the PWMWorker software.
class PWMWorker : public I2CDevice
{
public:
    // Configure from JSON data
    static void Configure(JSONParser &json);

    // Startup-only port configuration - configures the special port
    // properties on the device side.  This can be called ONLY during
    // initial configuration, because it sends I2C commands to the
    // device inline rather than scheduling them.
    //
    // (The configuration-time restriction is purely to keep the
    // implementation simple.  Pinscape Pico doesn't currently need to
    // be able to change these settings dynamically, because output
    // ports are configured only at startup, and can never change after
    // that.  If we did want to allow dynamic updates, we'd just have to
    // add the same sort of local-vs-remote register tracking that we
    // use for the level registers, extending that to the port setup
    // registers.  But since we don't need that, we can save the extra
    // code and the extra runtime overhead of checking for changes that
    // will never occur in practice.)
    void ConfigurePort(int portNum, bool gamma, bool activeLow);

    // Startup-only port configuratiojn - configure flipper logic
    void ConfigureFlipperLogic(int portNum, uint8_t limitLevel, uint16_t timeout);

    // global instances, built from the configuration data
    static std::vector<std::unique_ptr<PWMWorker>> units;

    // get a unit by number
    static PWMWorker *GetUnit(int n);

    // get the number of configured units
    static size_t CountConfigurations() { return std::count_if(units.begin(), units.end(),
        [](const std::unique_ptr<PWMWorker>& w){ return w != nullptr; });
    }

    // count ports across all configured chips
    static size_t CountPorts() { return std::accumulate(units.begin(), units.end(), 0,
        [](int acc, const std::unique_ptr<PWMWorker>& w){ return w != nullptr ? acc + w->nPorts : acc; }); }

    // get my configuration index
    int GetConfigIndex() const { return configIndex; }

    // construction
    PWMWorker(int configIndex, i2c_inst_t *i2c, uint16_t addr, int pwmFreq) :
        configIndex(configIndex), I2CDevice(addr), i2c(i2c), pwmFreq(pwmFreq) { }

    // Initialize.  Sets up the initial chip register configuration.
    void Init();

    // Enable/disable outputs across all units
    static void EnableOutputs(bool enable);

    // Set/Get an output level for a port (by abstract port number, 0-23)
    void Set(int port, uint8_t level);
    uint8_t Get(int port) const;

    // Is the given port number valid?  Ports are numbered 0 to 23, which are
    // abstract labels assigned on the PWMWorker side (NOT physical GPIO port
    // numbers)
    bool IsValidPort(int port) const { return port >= 0 && port <= 23; }

    // Populate a vendor interface output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query result buffer
    static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

    // Reboot a PWMWorker Pico, optionally setting it to Boot Loader
    // mode.  This lets the host initiate a Boot Loader update without
    // the need to manually operate the BOOTSEL button on the PWMWorker
    // Pico, which might be inconvenient or difficult to reach in a pin
    // cab controller setup.
    static void RebootRemotePico(int unit, bool bootLoaderMode);
    void Reboot(bool bootLoaderMode) { swresetPending = bootLoaderMode ? SWRESET_BOOTLOADER : SWRESET_RESET; }

    // I2C callbacks
    virtual const char *I2CDeviceName() const override { return "WorkerPico"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

protected:
    // register indices
    static const int REG_LED0 = 0x00;       // level register for LED0 (LED1-LED23 sequentially follow)
    static const int REG_CTRL0 = 0x18;      // control register 0
    static const int REG_CTRL1 = 0x19;      // control register 1
    static const int REG_FREQL = 0x1A;      // PWM frequency, low byte
    static const int REG_FREQH = 0x1B;      // PWM frequency, high byte
    static const int REG_VERSION = 0x1E;    // Version identification register (read-only)
    static const int REG_WHOAMI = 0x1F;     // WHO AM I device identification register (read-only)
    static const int REG_CONF0_BASE = 0x20; // start of configuration register group for LED0
    static const int REG_CONFBLK_SIZE = 4;  // size in bytes of LEDn configuration register group
    static const int REG_CONF_OFS = 0;      // offset of CONF register in LEDn configuration group
    static const int REG_LIMIT_OFS = 1;     // offset of LIMIT register in LEDn configuration group
    static const int REG_TIMELIMIT_OFS = 2; // offset of TIMELIMIT register in LEDn configuration group
    static const int REG_SWRESET = 0xDD;    // SWRESET register

    // Get the port register indices for a given port
    static inline int REG_CONF(int port) { return REG_CONF0_BASE + (port * REG_CONFBLK_SIZE); }
    static inline int REG_LIMIT(int port) { return REG_CONF(port) + REG_LIMIT_OFS; }
    static inline int REG_TIMELIMIT(int port) { return REG_CONF(port) + REG_TIMELIMIT_OFS; }

    // WHOAMI register value
    const uint8_t WHOAMI_ID = 0x24;   // arbitrary value to sanity-check the device type

    // REG_CTRL0 global configuration register bits
    static const uint8_t CTRL0_ENABLE_OUTPUTS = 0x01;   // enable outputs (ports are in high-impedance state when not enabled)
    static const uint8_t CTRL0_HWRESET = 0x40;          // hardware reset flag; set by the hardware after a CPU reset
    static const uint8_t CTRL0_RESET_REGS = 0x80;       // register reset request flag; write '1' to reset registers to power-on defaults

    // REG_CONF0..CONF23 port configuration register bits
    static const uint8_t CONF_ACTIVE_LOW = 0x01;   // active low
    static const uint8_t CONF_GAMMA_ENA  = 0x02;   // gamma correction enabled

    // SWRESET codes
    static const uint8_t SWRESET_START = 0x11;       // initiate a reset sequence
    static const uint8_t SWRESET_RESET = 0x22;       // normal CPU reset to restart firmware
    static const uint8_t SWRESET_BOOTLOADER = 0x33;  // reset to Boot Loader mode
    
    // configuration file index
    int configIndex;

    // Pico SDK I2C instance
    i2c_inst_t *i2c;

    // PWM frequency
    int pwmFreq = 20000;

    // Current/pending register.  The I2C service routine checks for
    // changes since the last update and transmits as needed.
    struct ChipReg
    {
        ChipReg(uint8_t addr, uint8_t resetVal) : addr(addr), chipVal(resetVal), localVal(resetVal) { }
        uint8_t addr;      // register address
        uint8_t chipVal;   // last value written to the chip
        uint8_t localVal;  // local value, to be written on the next I2C service call

        // add an I2C TX transaction as needed
        void OnI2CReady(I2C::TXRXBuilderBase *b);
    };

    // CTRL0 register
    ChipReg ctrl0{ REG_CTRL0, 0x00 };

    // pending SWRESET code; 0x00 means that no SWRESET is pending
    uint8_t swresetPending = 0x00;

    // Current output port levels, 0..255.  These are the live registers
    // that the application controls.  The application can update these
    // at any time, and the PWMWorker class will take care of propagating
    // changes to the physical chip when I2C bus time is available.
    const static int nPorts = 24;
    uint8_t level[nPorts]{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    // Dirty bits.  This keeps track of which ports have been modified
    // since the last update transmitted to the chip.  Bit (1 << 0)
    // corresponds to LED0, (1 << 1) to LED1, etc.
    uint32_t dirty = 0x00000000;

    // Last port levels transmitted to the chip.
    uint8_t txLevel[nPorts]{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
};
