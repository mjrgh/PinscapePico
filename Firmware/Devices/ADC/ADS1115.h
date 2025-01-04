// Pinscape Pico - ADS1115 analog-to-digital converter chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <list>

#include <pico/stdlib.h>
#include <hardware/i2c.h>

#include "Pinscape.h"
#include "Utils.h"
#include "I2C.h"
#include "ADCManager.h"

class JSONParser;

class ADS1115 : public ADC, public I2CDevice
{
public:
    // configure from JSON data
    static void Configure(JSONParser &json);

    // Construction
    ADS1115(int chipNum, uint8_t i2cAddr, int gpAlertRdy);

    // Start/stop sampling
    virtual void EnableSampling() override;

    // Get the configured number of logical channels
    virtual int GetNumLogicalChannels() const override { return nLogicalChannels; }

    // Read a sample
    virtual Sample ReadNative(int channel) override;
    virtual Sample ReadNorm(int channel) override;

    // I2CDevice interface
    virtual const char *I2CDeviceName() const override { return "ADS1115"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;
    virtual bool OnI2CWriteComplete(I2CX *i2c) { return false; }

protected:
    // initialize
    void Init(i2c_inst_t *i2c);
    void SendInitCommands(i2c_inst_t *i2c);

    // static array of configured chips; there can be up to 8 (4 per bus, 2 buses)
    static ADS1115 *chips[8];
    static int nChipsConfigured;

    // chip number of the ads1115:[ ] top-level JSON configuration array
    int chipNum = 0;

    // ALERT/RDY line GPIO, or -1 if not connected
    int gpAlertRdy;

    // Logical channels.  The public interface works in terms of logical
    // channels numbers.  A logical channel number is an index into this
    // array, which maps the logical channel number to a physical input
    // pin on the chip.
    int nLogicalChannels = 0;
    struct LogicalChannel
    {
        // Input multiplexer configuration for the channel; sets
        // the MUX field of the config register when this logical
        // channel is being samples:
        //
        //  0 = +AIN0 -AIN1   }
        //  1 = +AIN0 -AIN3    }  Double-ended
        //  2 = +AIN1 -AIN3    }  configurations
        //  3 = +AIN2 -AIN3   }
        //
        //  4 = +AIN0 -GND    }
        //  5 = +AIN1 -GND     }  Single-ended configurations
        //  6 = +AIN2 -GND     }  referenced to GND
        //  7 = +AIN3 -GND   }
        //
        uint8_t mux = 0;

        // latest raw sample
        ADC::Sample raw{ 0, 0 };

        // sample collection statistics
        struct Stats
        {
            // number of samples collected since last reset
            uint64_t n = 0;

            // total conversion time (sum of times from initiating a sample
            // on this channel to receiving the sample)
            uint64_t tConvSum = 0;

            // add a new sample with its conversion time
            void AddConv(uint64_t tConv)
            {
                ++n;
                tConvSum += tConv;
            }

            // reset statistics
            void Reset()
            {
                n = 0;
                tConvSum = 0;
            }
        };
        Stats stats;
    };
    LogicalChannel logicalChannel[4];

    // Current sampling channel.  This is the index in the logicalChannels[]
    // array of the sample currently running, or -1 if sampling is disabled.
    int curSamplingChannel = -1;

    // time current sample collection was initiated
    uint64_t tCurSampleStarted = 0;

    // statistics collection starting time
    uint64_t tStatsReset = 0;

    // current CONFIG register setting (see data sheet or Init() comments for bit meanings)
    uint8_t configHi = 0x03;
    uint8_t configLo = 0x00;

    // config change is pending - send on our next I2C slot
    bool configChangePending = false;

    // desired sample speed - this is the speed set in the configuration,
    // in samples per second
    int desiredSamplesPerSecond = 860;

    // actual sample speed - this is always one of the chip's available
    // sampling rates (8, 16, 32, 64, 128, 250, 474, 860 S/s)
    int samplesPerSecond = 860;

    // Config register PGA field.  This sets the full-scale voltage
    // range, per the data sheet:
    //
    //    0 = +/- 6.144V
    //    1 = +/- 4.096V      <-- our default
    //    2 = +/- 2.048V
    //    3 = +/- 1.024V
    //    4 = +/- 0.512V
    //  5-7 = +/- 0.256V
    //
    // Select 4.096 by default, since the main use for an ADC in a
    // virtual pin cab set is to read a potentiometer that's sensing
    // the ball shooter position, joystick position, or some other
    // similar input control position; and the most common way to
    // configure those is with the potentiometer fixed ends connected
    // to Pico VCC and GND, so the voltage range equals the Pico's
    // 3.3V logic range.  The closest ADS1115 gain setting that
    // fully covers that range is PAGE=1, +/- 4.096V.
    uint8_t pga = 0x01;

    // DR field - sets the data rate:
    //
    //   0  = 8 samples/second
    //   1  = 16
    //   2  = 32
    //   3  = 64
    //   4  = 128
    //   5  = 250
    //   6  = 474
    //   7  = 860
    //
    uint8_t drBits = 0x07;

    // register addresses
    static const uint8_t CONV_REG_ADDR = 0x00;
    static const uint8_t CONFIG_REG_ADDR = 0x01;
    static const uint8_t LO_THRESH_REG_ADDR = 0x02;
    static const uint8_t HI_THRESH_REG_ADDR = 0x03;

    // config high register bits
    static const uint8_t MUX_MASK = 0x70;
    static const uint8_t OS_BIT = 0x80;

    // command-line tools
    static void Command_main_S(const ConsoleCommandContext *ctx);
    void Command_main(const ConsoleCommandContext *ctx);
};
