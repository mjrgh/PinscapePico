// Pinscape Pico - PCA9685 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The PCA9685 is an 16-channel PWM LED driver with an I2C interface.

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
#include "Outputs.h"

// external declarations
class JSONParser;
namespace PinscapePico {
    struct OutputDevDesc;
    struct OutputDevPortDesc;
}

// PCA9685 chip object.  Each instance of this object represents
// one physical chip.
class PCA9685 : public I2CDevice
{
public:
    // Configure PCA9685 units from JSON data
    static void Configure(JSONParser &json);

    // enable/disable all output pins
    static void EnableOutputs(bool enable);

    // global instances, built from the configuration data
    static std::vector<std::unique_ptr<PCA9685>> chips;

    // get a chip by number (configuration index)
    static PCA9685 *GetChip(int n);

    // count configured chips
    static size_t CountConfigurations() { return std::count_if(chips.begin(), chips.end(),
        [](const std::unique_ptr<PCA9685>& c){ return c != nullptr; }); }

    // count ports across all configuerd chips
    static size_t CountPorts() { return std::accumulate(chips.begin(), chips.end(), 0,
        [](int acc, const std::unique_ptr<PCA9685>& c){ return c != nullptr ? acc + c->nPorts : acc; }); }

    // get my configuration index
    int GetConfigIndex() const { return configIndex; }

    // construction
    PCA9685(int configIndex, i2c_inst_t *i2c, uint16_t addr, OutputManager::Device *oePort) :
        configIndex(configIndex), I2CDevice(addr), i2c(i2c), oePort(oePort) { }

    // Initialize.  Sets up the initial chip register configuration.
    void Init();

    // Set an output level, port 0..15, level 0..4095
    void Set(int port, uint16_t level);

    // get an output level
    uint16_t Get(int port) const;

    // is the given port number valid?
    bool IsValidPort(int port) const { return port >= 0 && port <= 15; }

    // Populate a vendor interface output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query result buffer
    static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

    // I2C callbacks
    virtual const char *I2CDeviceName() const override { return "PCA9685"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;

protected:
    // send initialization commands
    void SendInitCommands();
    
    // Registers
    static const int REG_MODE1 = 0x00;
    static const int REG_MODE2 = 0x01;
    static const int REG_SUBADR1 = 0x02;
    static const int REG_SUBADR2 = 0x03;
    static const int REG_SUBADR3 = 0x04;
    static const int REG_ALLCALLADR = 0x05;
    static const int REG_LED0_ON_L = 0x06;
    static const int REG_LED0_ON_H = 0x07;
    static const int REG_LED0_OFF_L = 0x08;
    static const int REG_LED0_OFF_H = 0x09;
    static const int REG_LED1_ON_L = 0x0A;
    static const int REG_LED1_ON_H = 0x0B;
    static const int REG_LED1_OFF_L = 0x0C;
    static const int REG_LED1_OFF_H = 0x0D;
    static const int REG_LED2_ON_L = 0x0E;
    static const int REG_LED2_ON_H = 0x0F;
    static const int REG_LED2_OFF_L = 0x10;
    static const int REG_LED2_OFF_H = 0x11;
    static const int REG_LED3_ON_L = 0x12;
    static const int REG_LED3_ON_H = 0x13;
    static const int REG_LED3_OFF_L = 0x14;
    static const int REG_LED3_OFF_H = 0x15;
    static const int REG_LED4_ON_L = 0x16;
    static const int REG_LED4_ON_H = 0x17;
    static const int REG_LED4_OFF_L = 0x18;
    static const int REG_LED4_OFF_H = 0x19;
    static const int REG_LED5_ON_L = 0x1A;
    static const int REG_LED5_ON_H = 0x1B;
    static const int REG_LED5_OFF_L = 0x1C;
    static const int REG_LED5_OFF_H = 0x1D;
    static const int REG_LED6_ON_L = 0x1E;
    static const int REG_LED6_ON_H = 0x1F;
    static const int REG_LED6_OFF_L = 0x20;
    static const int REG_LED6_OFF_H = 0x21;
    static const int REG_LED7_ON_L = 0x22;
    static const int REG_LED7_ON_H = 0x23;
    static const int REG_LED7_OFF_L = 0x24;
    static const int REG_LED7_OFF_H = 0x25;
    static const int REG_LED8_ON_L = 0x26;
    static const int REG_LED8_ON_H = 0x27;
    static const int REG_LED8_OFF_L = 0x28;
    static const int REG_LED8_OFF_H = 0x29;
    static const int REG_LED9_ON_L = 0x2A;
    static const int REG_LED9_ON_H = 0x2B;
    static const int REG_LED9_OFF_L = 0x2C;
    static const int REG_LED9_OFF_H = 0x2D;
    static const int REG_LED10_ON_L = 0x2E;
    static const int REG_LED10_ON_H = 0x2F;
    static const int REG_LED10_OFF_L = 0x30;
    static const int REG_LED10_OFF_H = 0x31;
    static const int REG_LED11_ON_L = 0x32;
    static const int REG_LED11_ON_H = 0x33;
    static const int REG_LED11_OFF_L = 0x34;
    static const int REG_LED11_OFF_H = 0x35;
    static const int REG_LED12_ON_L = 0x36;
    static const int REG_LED12_ON_H = 0x37;
    static const int REG_LED12_OFF_L = 0x38;
    static const int REG_LED12_OFF_H = 0x39;
    static const int REG_LED13_ON_L = 0x3A;
    static const int REG_LED13_ON_H = 0x3B;
    static const int REG_LED13_OFF_L = 0x3C;
    static const int REG_LED13_OFF_H = 0x3D;
    static const int REG_LED14_ON_L = 0x3E;
    static const int REG_LED14_ON_H = 0x3F;
    static const int REG_LED14_OFF_L = 0x40;
    static const int REG_LED14_OFF_H = 0x41;
    static const int REG_LED15_ON_L = 0x42;
    static const int REG_LED15_ON_H = 0x43;
    static const int REG_LED15_OFF_L = 0x44;
    static const int REG_LED15_OFF_H = 0x45;
    static const int REG_ALL_LED_ON_L = 0xFA;
    static const int REG_ALL_LED_ON_H = 0xFB;
    static const int REG_ALL_LED_OFF_L = 0xFC;
    static const int REG_ALL_LED_OFF_H = 0xFD;
    static const int REG_PRE_SCALE = 0xFE;
    static const int REG_TestMode = 0xFF;

    // control register bits
    static const uint8_t CTL_AIALL = 0x80;         // auto-increment mode, all registers
    static const uint8_t CTL_AIPWM = 0xA0;         // auto-increment mode, PWM registers only
    static const uint8_t CTL_AICTL = 0xC0;         // auto-increment mode, control registers only
    static const uint8_t CTL_AIPWMCTL = 0xE0;      // auto-increment mode, PWM + control registers only

    // MODE1 register bits
    static const uint8_t MODE1_RESTART = 0x80;
    static const uint8_t MODE1_EXTCLK = 0x40;
    static const uint8_t MODE1_AI = 0x20;
    static const uint8_t MODE1_SLEEP = 0x10;
    static const uint8_t MODE1_SUB1 = 0x08;
    static const uint8_t MODE1_SUB2 = 0x04;
    static const uint8_t MODE1_SUB3 = 0x02;
    static const uint8_t MODE1_ALLCALL = 0x01;

    // MODE2 register bits
    static const uint8_t MODE2_INVRT = 0x10;
    static const uint8_t MODE2_OCH = 0x08;
    static const uint8_t MODE2_OUTDRV = 0x04;
    static const uint8_t MODE2_OUTNE_LOW = 0x00;
    static const uint8_t MODE2_OUTNE_HI = 0x01;
    static const uint8_t MODE2_OUTNE_Z = 0x02;

    // LEDOUTOn register bits
    static const uint8_t LEDOUT_OFF = 0x00;
    static const uint8_t LEDOUT_ON  = 0x01;
    static const uint8_t LEDOUT_PWM = 0x02;
    static const uint8_t LEDOUT_GRPPWM = 0x03;

    // configuration file index
    int configIndex;
    
    // /OE (output enable) port
    std::unique_ptr<OutputManager::Device> oePort;

    // MODE2 register settings from the configuration.
    uint8_t mode2 = MODE2_OUTDRV | MODE2_OUTNE_LOW;

    // Pico SDK I2C instance
    i2c_inst_t *i2c;

    // Current output port levels, 0..4095.  These are the live registers
    // that the application controls.  The application can update these
    // at any time, and the PCA9685 class will take care of propagating
    // changes to the physical chip when I2C bus time is available.
    const static int nPorts = 16;
    uint16_t level[nPorts]{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    // Dirty bits.  This keeps track of which ports have been modified
    // since the last update transmitted to the chip.  Bit (1 << 0)
    // corresponds to LED0, (1 << 1) to LED1, etc.
    uint16_t dirty = 0x0000;

    // Last port levels transmitted to the chip.
    uint16_t txLevel[nPorts]{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
};

