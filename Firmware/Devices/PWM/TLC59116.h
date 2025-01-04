// Pinscape Pico - TLC59116 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The TI TLC59116 is a 16-channel PWM LED driver with an I2C interface.
//
// The reference Pinscape Pico expansion boards use the TLC59116F to
// control most of the output ports.  The TLC59116F is a variation of
// the chip with open-drain current sinks on the output ports.  Other
// variations of the chip, without the 'F' suffix, provide fixed-current
// sinks, with the current setting controlled at the hardware level, by
// an external resistor tied to a control line on the chip.
//
// All variations use the same software interface, so this class can be
// used with any of them, without any need to specify which type is
// being addressed.  The fixed-current variants are ideal for directly
// driving arrays of identical LEDs, since the fixed current control
// eliminates the need for per-LED current-limiting resistors; each LED
// can be wired directly to the power supply on the anode side and the
// TLC59116 port on the cathode.  The open-drain 'F' variant is superior
// for applications like Pinscape where the outputs are heterogeneous,
// making it undesirable to have a single fixed current setting across
// all ports.  In the Pinscape case, many of the TLC59116 ports drive
// MOSFET and Darlington amplifier circuits, and for these we need the
// TLC ports to act like simple transistor switches, making the open-
// drain design of the 'F' variant ideal.

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

// TLC59116 chip object.  Each instance of this object represents
// one physical TLC59116 chip.
class TLC59116 : public I2CDevice
{
public:
    // Disable All Call across all TLC59116 chips on a bus.  The default
    // All Call address is 0x68, which conflicts with DS3231M's fixed
    // address.  We could resolve that by changing the All Call address
    // to some other address in the 0x60-0x6F range, but we don't need
    // the feature in the first place, so we might as well not consume
    // an address.
    static void DisableAllCall(int bus);

    // Configure TLC59116 units from JSON data
    static void Configure(JSONParser &json);

    // global instances, built from the configuration data
    static std::vector<std::unique_ptr<TLC59116>> chips;

    // get a chip by number
    static TLC59116 *GetChip(int n);

    // get the number of configured chips
    static size_t CountConfigurations() { return std::count_if(chips.begin(), chips.end(),
        [](const std::unique_ptr<TLC59116>& c){ return c != nullptr; });
    }

    // count ports across all configured chips
    static size_t CountPorts() { return std::accumulate(chips.begin(), chips.end(), 0,
        [](int acc, const std::unique_ptr<TLC59116>& c){ return c != nullptr ? c->nPorts : 0; }); }

    // get my configuration index
    int GetConfigIndex() const { return configIndex; }

    // construction
    TLC59116(int configIndex, i2c_inst_t *i2c, uint16_t addr, int gpReset) :
        configIndex(configIndex), I2CDevice(addr), i2c(i2c), gpReset(gpReset) { }

    // Initialize.  Sets up the initial chip register configuration.
    void Init();

    // Set/Get an output level, port 0..15, level 0..255
    void Set(int port, uint8_t level);
    uint8_t Get(int port) const;

    // is the given port number valid?
    bool IsValidPort(int port) const { return port >= 0 && port <= 15; }

    // Populate a vendor interface output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query result buffer
    static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

    // I2C callbacks
    virtual const char *I2CDeviceName() const override { return "TLC59116"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

protected:
    // register addresses
    static const uint8_t REG_MODE1 = 0x00;         // MODE1
    static const uint8_t REG_MODE2 = 0x01;         // MODE2
    static const uint8_t REG_PWM0 = 0x02;          // PWM 0
    static const uint8_t REG_PWM1 = 0x03;          // PWM 1
    static const uint8_t REG_PWM2 = 0x04;          // PWM 2
    static const uint8_t REG_PWM3 = 0x05;          // PWM 3
    static const uint8_t REG_PWM4 = 0x06;          // PWM 4
    static const uint8_t REG_PWM5 = 0x07;          // PWM 5
    static const uint8_t REG_PWM6 = 0x08;          // PWM 6
    static const uint8_t REG_PWM7 = 0x09;          // PWM 7
    static const uint8_t REG_PWM8 = 0x0A;          // PWM 8
    static const uint8_t REG_PWM9 = 0x0B;          // PWM 9
    static const uint8_t REG_PWM10 = 0x0C;         // PWM 10
    static const uint8_t REG_PWM11 = 0x0D;         // PWM 11
    static const uint8_t REG_PWM12 = 0x0E;         // PWM 12
    static const uint8_t REG_PWM13 = 0x0F;         // PWM 13
    static const uint8_t REG_PWM14 = 0x10;         // PWM 14
    static const uint8_t REG_PWM15 = 0x11;         // PWM 15
    static const uint8_t REG_GRPPWM = 0x12;        // Group PWM duty cycle
    static const uint8_t REG_GRPFREQ = 0x13;       // Group frequency register
    static const uint8_t REG_LEDOUT0 = 0x14;       // LED driver output status register 0
    static const uint8_t REG_LEDOUT1 = 0x15;       // LED driver output status register 1
    static const uint8_t REG_LEDOUT2 = 0x16;       // LED driver output status register 2
    static const uint8_t REG_LEDOUT3 = 0x17;       // LED driver output status register 3

    // control register bits
    static const uint8_t CTL_AIALL = 0x80;         // auto-increment mode, all registers
    static const uint8_t CTL_AIPWM = 0xA0;         // auto-increment mode, PWM registers only
    static const uint8_t CTL_AICTL = 0xC0;         // auto-increment mode, control registers only
    static const uint8_t CTL_AIPWMCTL = 0xE0;      // auto-increment mode, PWM + control registers only

    // MODE1 bits
    static const uint8_t MODE1_AI2 = 0x80;         // auto-increment mode enable
    static const uint8_t MODE1_AI1 = 0x40;         // auto-increment bit 1
    static const uint8_t MODE1_AI0 = 0x20;         // auto-increment bit 0
    static const uint8_t MODE1_OSCOFF = 0x10;      // oscillator off (low-power sleep mode)
    static const uint8_t MODE1_SUB1 = 0x08;        // subaddress 1 enable
    static const uint8_t MODE1_SUB2 = 0x04;        // subaddress 2 enable
    static const uint8_t MODE1_SUB3 = 0x02;        // subaddress 3 enable
    static const uint8_t MODE1_ALLCALL = 0x01;     // all-call enable

    // MODE2 bits
    static const uint8_t MODE2_EFCLR = 0x80;       // clear error status flag (fixed-current chip variants only; N/A in 'F' open-drain variant)
    static const uint8_t MODE2_DMBLNK = 0x20;      // group blinking mode
    static const uint8_t MODE2_OCH = 0x08;         // outputs change on ACK (vs Stop command)

    // LEDOUTn states
    static const uint8_t LEDOUT_OFF = 0x00;        // driver is off
    static const uint8_t LEDOUT_ON = 0x01;         // fully on
    static const uint8_t LEDOUT_PWM = 0x02;        // individual PWM control via PWMn register
    static const uint8_t LEDOUT_GROUP = 0x03;      // PWM control + group dimming/blinking via PWMn + GRPPWM

    // configuration file index
    int configIndex;

    // reset GPIO, or -1 if no reset line is configured
    int gpReset;

    // Pico SDK I2C instance
    i2c_inst_t *i2c;

    // Current output port levels, 0..255.  These are the live registers
    // that the application controls.  The application can update these
    // at any time, and the TLC59116 class will take care of propagating
    // changes to the physical chip when I2C bus time is available.
    const static int nPorts = 16;
    uint8_t level[nPorts]{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    // Dirty bits.  This keeps track of which ports have been modified
    // since the last update transmitted to the chip.  Bit (1 << 0)
    // corresponds to LED0, (1 << 1) to LED1, etc.
    uint16_t dirty = 0x0000;

    // Last port levels transmitted to the chip.
    uint8_t txLevel[nPorts]{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
};
