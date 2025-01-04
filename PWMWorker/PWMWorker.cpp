// Pinscape Pico - PWM Worker Program
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This Pico firmware is a helper for Pinscape Pico.  It turns a Pico
// into an I2C slave that acts as a PWM controller chip, along the lines
// of a TLC59116 or PCA9685.  The idea is that a Pico can be used as an
// alternative to one of those PWM chips.  A Pico equipped with this
// firmware can control 24 PWM channels, so it can replace 1-1/2 of the
// typical dedicated chips, which are mostly 16-channel devices.
//
// The reason you might want to use a Pico in place of a dedicated PWM
// chip is that all of the PWM chips currently in production are only
// available in fine-pitch SMD packages, which are difficult to solder
// by hand.  The Pico, in contrast, is easy to work with by hand, so
// using it in place of an SMD PWM chip makes it possible to design a
// DIY-friendly Pinscape Pico expansion board.
//
// A Pico running this firmware acts as an I2C slave device, so it can
// be placed on an I2C bus alongside other I2C devices.  The I2C slave
// address is programmable, allowing multiple PWM Worker Picos to share
// a single bus, so that you can add as many ports as you need.  (Not an
// unlimited number of ports, since an I2C bus can only handle about 20
// devices in practice, but there should be plenty of headroom for any
// pin cab application.)
//
// Pico GPIO pin assignments:
//
//   GP0   LED0 output
//   GP1   LED1 output
//   ..
//   GP22  LED22 output
//   GP28  LED23 output
//
//   GP26  SDA
//   GP27  SCL
//
//
// I2C register map
// (All addresses reference 8-bit bytes; 16-bit registers are arranged as two
// consecutive byte addresses, with the context in little-endian order)
//
// 0x00    LED0 level, 0-255                0 on reset
// 0x01    LED1 level, 0-255                0 on reset
// ..
// 0x17    LED23 level, 0-255               0 on reset
//
// 0x18    Control register 0               0 on reset
// 0x19    Control register 1               0 on reset
// 0x19    PWM frequency, low byte          } 20000
// 0x1A    PWM frequency, high byte         }  on reset
//
// 0x1E    Version                          read-only; returns firmware version
// 0x1F    WHO AM I                         read-only; returns identification code
//
// 0x20    LED0 configuration register      0 on reset
// 0x21    LED0 power level limit register  0xFF on reset
// 0x22    LED0 time limit, low byte        } 0 on
// 0x23    LED0 time limit, high byte       }  reset
//
// 0x24    LED0 configuration register      0 on reset
// 0x25    LED0 power level limit register  0xFF on reset
// 0x26    LED0 time limit, low byte        } 0 on
// 0x27    LED0 time limit, high byte       }  reset
//
// ...
//
// 0x7C    LED23 configuration register      0 on reset
// 0x7D    LED23 power level limit register  0xFF on reset
// 0x7E    LED23 time limit, low byte        } 0 on
// 0x7F    LED23 time limit, high byte       }  reset
// 
//
// Control register 0:  Global control bit map.  Initial value 0x00.
//   0x01  Enable outputs (outputs are in high-impedance state when disabled)
//
// Control register 1: Global control bit map.  Initial value 0x00.
// Reserved for future use.
//
// PWM frequency register: sets the PWM frequency in Hertz.  The setting
// is global to all outputs.
//
// Port registers: Each port has a group of four register bytes with
// configuration settings for the port.
//
// LEDnn configuration:  A bit map, 0x00 on reset:
//   0x01  Active Low (0=active high [default], 1=active low)
//   0x02  Gamma enabled (0=linear [default], 1=gamma correction enabled)
//
// LEDnn power level limit: 0xFF on reset; sets the reduced power level
// applied after the power level exceeds this limit for the timeout period.
//
// LEDnn time limit: two bytes, little-endian, 0x00 0x00 on reset.  Sets
// the timeout period in milliseconds before the power level limit is
// applied.  When a new level is set exceeding the limit level, the timer
// starts; when the time limit is reached, the port level is reduced to
// the limit level.
//
//
// Gamma correction: When the gamma bit is set, the 8-bit brightness
// level in the Output Level register for an LED is mapped to a 12-bit
// scale through a gamma function, to improve the perceptual linearity
// of the brightness setting.  Delegating the gamma mapping to the PWM
// Worker Pico is advantageous because it allows allows fine-grained
// 12-bit resolution on the physical output while sending only 8-bit
// levels on the I2C bus, conserving I2C bandwidth.  The main Pico gets
// its level settings from the host in 8-bit notation, so there's no
// benefit to expanding this to 12-bit notation in the I2C command, but
// there is a benefit to expanding it to 12 bits for the physical
// output, since that gives us smoother fades by making the steps
// between brightness levels less apparent.  It also allows for finer
// gradations at low brightness; with 8-bit PWM, the gamma curve is so
// flat that everything below about 30/255 maps to zero duty cycle after
// gamma correction, while 12-bit PWM has a non-zero duty cycle down to
// 11/255.
//
// Time limiter:  The output is only allowed to exceed the value in its
// corresponding power level limit register (0x40-0x57) for the time
// specified in its time limit register (0x60-0x8F).  The time limit
// registers are interpreted as unsigned 16-bit integers giving a time
// limit in milliseconds.  This allows the main Pinscape software to
// delegate the time-limiter feature to the PWM Worker, which is more
// robust against faults, such as software crashes on the main Pico or
// I2C bus interruptions.  Setting the power limit to 255 effetively
// disables the limiter, since the value can never go above the limit
// value and thus never trigger the timer.
//

// Define as non-zero if USB logging is desired.  This will initialize
// stdio on the USB connection, with the startup connection delay
// defined in CMakeLists.txt to ensure that a terminal is attached
// before we start generating any messages.
#define ENABLE_USB_LOGGING  0
#if ENABLE_USB_LOGGING
#define IF_USB_LOGGING(x) x
#else
#define IF_USB_LOGGING(x)
#endif


// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/time.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/irq.h>
#include <hardware/pwm.h>
#include <hardware/resets.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>
#include <hardware/sync.h>

// local project headers
#include "i2c_slave.h"
#include "i2c_fifo.h"
#include "PicoLED.h"
#include "Util.h"
#include "Logger.h"
#include "PWMManager.h"
#include "PWMWorker.h"

// ----------------------------------------------------------------------------
//
// Miscellaneous definitions
//

// a Pico clock time that's so far in the future that it's effectively infinity
const uint64_t END_OF_TIME = ~0ULL;

// default PWM frequency (Hz)
static const uint16_t DEFAULT_PWM_FREQ = 20000;

// ----------------------------------------------------------------------------
//
// GPIO configuration
//

// I2C ports
static const int I2C_SDA = 26;
static const int I2C_SCL = 27;
#define I2C_UNIT i2c1    // must match unit associated with the SDA/SCL pins assigned above

// Mapping from nominal LED output number to GPIO port
static uint8_t gpioMap[24]{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 28
};


// ----------------------------------------------------------------------------
//
// I2C slave
//
// The I2C handler does all of its work in the I2C interrupt callback.  The
// only work we do there is to transfer data in and out of the I2C register
// file, which is implemented as a byte array in RAM.  The main thread is
// where all of the real work is carried out: it runs in a continuous loop,
// monitoring the register file for changes, and updating the PWM output ports
// accordingly.  This division of labor allows the I2C handler to service
// requests in real time, which is a requirement of the I2C protocol, since
// the master drives the clock signal.
//

// I2C registers
static uint8_t i2cReg[255];

// register indices
const int REG_LED0 = 0x00;       // level register for LED0 (LED1-LED23 sequentially follow)
const int REG_CTRL0 = 0x18;      // control register 0
const int REG_CTRL1 = 0x19;      // control register 1
const int REG_FREQL = 0x1A;      // PWM frequency, low byte
const int REG_FREQH = 0x1B;      // PWM frequency, high byte
const int REG_VERSION = 0x1E;    // Version identification register (read-only)
const int REG_WHOAMI = 0x1F;     // WHO AM I device identification register (read-only)
const int REG_CONF0_BASE = 0x20; // start of configuration register group for LED0
const int REG_CONFBLK_SIZE = 4;  // size in bytes of LEDn configuration register group
const int REG_CONF_OFS = 0;      // offset of CONF register in LEDn configuration group
const int REG_LIMIT_OFS = 1;     // offset of LIMIT register in LEDn configuration group
const int REG_TIMELIMIT_OFS = 2; // offset of TIMELIMIT register in LEDn configuration group
const int REG_SWRESET = 0xDD;    // software reset

// Identification register values
const uint8_t WHOAMI_ID = 0x24;   // arbitrary value to sanity-check the device type
const uint8_t VERSION_ID = 0x01;  // software version; increment when making I2C interface changes

// Software Reset codes
const uint8_t SWRESET_START = 0x11;  // write to SWRESET register to start a reset sequence
const uint8_t SWRESET_RESET = 0x22;  // write within 1 second of SWRESET_START to execute a CPU reset
const uint8_t SWRESET_BOOTLOADER = 0x33; // write within 1 second of SWRESET_START to execute a Boot Loader mode reset

// Get the port register indices for a given port
static inline int REG_CONF(int port) { return REG_CONF0_BASE + (port * REG_CONFBLK_SIZE); }
static inline int REG_LIMIT(int port) { return REG_CONF(port) + REG_LIMIT_OFS; }
static inline int REG_TIMELIMIT(int port) { return REG_CONF(port) + REG_TIMELIMIT_OFS; }

// REG_CTRL0 - global configuration register bits
const uint8_t CTRL0_ENABLE_OUTPUTS = 0x01;   // enable outputs (ports are in high-impedance state when not enabled)
const uint8_t CTRL0_HWRESET = 0x40;          // hardware reset flag - set to '1' after a power cycle or hard CPU reset
const uint8_t CTRL0_RESET_REGS = 0x80;       // reset registers to default values

// REG_CONF0..CONF23 - port configuration register bits
const uint8_t CONF_ACTIVE_LOW = 0x01;        // active low
const uint8_t CONF_GAMMA_ENA  = 0x02;        // gamma correction enabled

// service callback context
struct ServiceContext
{
    // Read/write register address
    int addr = 0;

    // Write started
    bool writeStarted = false;

    // SWRESET state.  Writes to the SWRESET register are tracked in the
    // interrupt handler to ensure they're processed immediately, since
    // this register requires a multi-byte protocol sequence.
    struct SWReset
    {
        // Current state - this is the last SWRESET_xxx byte the host wrote
        uint8_t state = 0x00;

        // Timeout - system clock time when the state update becomes invalid
        uint64_t timeout = END_OF_TIME;
    };
    SWReset swReset;
};
static volatile ServiceContext serviceContext;

// I2C interrupt callback
static void __not_in_flash_func(I2CService)(i2c_inst_t *i2c, i2c_slave_event_t event)
{
    switch (event)
    {
    case I2C_SLAVE_RECEIVE:
        // WRITE transaction.  If we don't already have a register address,
        // this is a new transaction, so the first byte is the address.
        if (!serviceContext.writeStarted)
        {
            // start of transaction - first byte is the register address
            serviceContext.addr = i2c_read_byte(i2c);
            serviceContext.writeStarted = true;
        }
        else
        {
            // continuing transaction - read the next byte into the current
            // register address, auto-incrementing the address
            uint8_t b = i2c_read_byte(i2c);

            // handle certain locations specially
            switch (serviceContext.addr)
            {
            case REG_WHOAMI:
            case REG_VERSION:
                // these registers are read-only; ignore the write
                break;

            case REG_SWRESET:
                // Software Reset register.  This register must be written in
                // a certain order to trigger a reset.  Other write sequences
                // are ignored.
                //
                // 0x00 (initial state) -> SWRESET_START - start a reset sequence
                // SWRESET_START -> SWRESET_RESET - execute an ordinary CPU reset
                // SWRESET_START -> SWRESET_BOOTLOADER - reboot into the Pico's native Boot Loader mode
                {
                    // If the previous state's timeout expired, reset the state to initial
                    auto &s = serviceContext.swReset;
                    auto now = time_us_64();
                    if (now >= s.timeout)
                    {
                        s.state = 0x00;
                        s.timeout = END_OF_TIME;
                    }

                    // check for valid transitions
                    if (s.state == 0x00 && b == SWRESET_START)
                    {
                        // start a reset sequence; it must be completed within a timeout
                        s.state = b;
                        s.timeout = now + 5000000;
                    }
                    else if (now < s.timeout && s.state == SWRESET_START && (b == SWRESET_RESET || b == SWRESET_BOOTLOADER))
                    {
                        // Protocol completed - schedule the requested reset.  Don't do
                        // this inline, so that we don't interrupt any remaining I2C
                        // activity for the current transaction.  Instead, just update
                        // the state register and set a brief timeout, long enough to
                        // ensure (or at least make it highly likely) that the current
                        // transaction is finished.  The main thread will execute the
                        // reset at the appointed time.
                        s.state = b;
                        s.timeout = now + 50;
                    }
                }
                break;

            default:
                // ordinary register - update the in-memory register file
                i2cReg[serviceContext.addr] = b;
                break;
            }

            // auto-increment whether or not we stored anything
            serviceContext.addr += 1;
        }
        break;

    case I2C_SLAVE_REQUEST:
        // register read - read the next register file byte
        i2c_write_byte(i2c, i2cReg[serviceContext.addr++]);
        break;

    case I2C_SLAVE_FINISH:
        // master has signaled Stop/Restart - clear the register address counter
        serviceContext.writeStarted = false;
        break;
    }

    // wrap the register address upon reaching the end of the register file
    if (serviceContext.addr >= _countof(i2cReg))
        serviceContext.addr = 0;
}

// Get a 16-bit register with interrupt lockout protection, for coherent
// read/write from the main thread.  Returns true if the value was read
// successfully, false if the I2C writer is currently in the middle of
// updating the byte pair.
inline bool GetReg16(int idx, uint16_t &result)
{
    // disable interrupts so that the address can't get updated while we're working
    IRQDisabler irqd;

    // check that we're not in the middle of a write to this register's byte pair;
    // if so, return failure
    if (serviceContext.addr == idx + 1 && serviceContext.writeStarted)
    {
        result = 0;
        return false;
    }

    // read the register pair (little-endian format) and return success
    result = i2cReg[idx] | (static_cast<uint16_t>(i2cReg[idx+1]) << 8);
    return true;
}

// set a 16-bit register (no interrupt protection)
inline void SetReg16(int idx, uint16_t val)
{
    i2cReg[idx] = static_cast<uint8_t>(val & 0xFF);
    i2cReg[idx+1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

// ----------------------------------------------------------------------------
//
// Port level computation
//

// linear output mapping from 8-bit scale to float duty cycle
static const float linearDutyCycleMap[256] = {
    0.0f/255.0f, 1.0f/255.0f, 2.0f/255.0f, 3.0f/255.0f, 4.0f/255.0f, 5.0f/255.0f, 6.0f/255.0f, 7.0f/255.0f,
    8.0f/255.0f, 9.0f/255.0f, 10.0f/255.0f, 11.0f/255.0f, 12.0f/255.0f, 13.0f/255.0f, 14.0f/255.0f, 15.0f/255.0f,
    16.0f/255.0f, 17.0f/255.0f, 18.0f/255.0f, 19.0f/255.0f, 20.0f/255.0f, 21.0f/255.0f, 22.0f/255.0f, 23.0f/255.0f,
    24.0f/255.0f, 25.0f/255.0f, 26.0f/255.0f, 27.0f/255.0f, 28.0f/255.0f, 29.0f/255.0f, 30.0f/255.0f, 31.0f/255.0f,
    32.0f/255.0f, 33.0f/255.0f, 34.0f/255.0f, 35.0f/255.0f, 36.0f/255.0f, 37.0f/255.0f, 38.0f/255.0f, 39.0f/255.0f,
    40.0f/255.0f, 41.0f/255.0f, 42.0f/255.0f, 43.0f/255.0f, 44.0f/255.0f, 45.0f/255.0f, 46.0f/255.0f, 47.0f/255.0f,
    48.0f/255.0f, 49.0f/255.0f, 50.0f/255.0f, 51.0f/255.0f, 52.0f/255.0f, 53.0f/255.0f, 54.0f/255.0f, 55.0f/255.0f,
    56.0f/255.0f, 57.0f/255.0f, 58.0f/255.0f, 59.0f/255.0f, 60.0f/255.0f, 61.0f/255.0f, 62.0f/255.0f, 63.0f/255.0f,
    64.0f/255.0f, 65.0f/255.0f, 66.0f/255.0f, 67.0f/255.0f, 68.0f/255.0f, 69.0f/255.0f, 70.0f/255.0f, 71.0f/255.0f,
    72.0f/255.0f, 73.0f/255.0f, 74.0f/255.0f, 75.0f/255.0f, 76.0f/255.0f, 77.0f/255.0f, 78.0f/255.0f, 79.0f/255.0f,
    80.0f/255.0f, 81.0f/255.0f, 82.0f/255.0f, 83.0f/255.0f, 84.0f/255.0f, 85.0f/255.0f, 86.0f/255.0f, 87.0f/255.0f,
    88.0f/255.0f, 89.0f/255.0f, 90.0f/255.0f, 91.0f/255.0f, 92.0f/255.0f, 93.0f/255.0f, 94.0f/255.0f, 95.0f/255.0f,
    96.0f/255.0f, 97.0f/255.0f, 98.0f/255.0f, 99.0f/255.0f, 100.0f/255.0f, 101.0f/255.0f, 102.0f/255.0f, 103.0f/255.0f,
    104.0f/255.0f, 105.0f/255.0f, 106.0f/255.0f, 107.0f/255.0f, 108.0f/255.0f, 109.0f/255.0f, 110.0f/255.0f, 111.0f/255.0f,
    112.0f/255.0f, 113.0f/255.0f, 114.0f/255.0f, 115.0f/255.0f, 116.0f/255.0f, 117.0f/255.0f, 118.0f/255.0f, 119.0f/255.0f,
    120.0f/255.0f, 121.0f/255.0f, 122.0f/255.0f, 123.0f/255.0f, 124.0f/255.0f, 125.0f/255.0f, 126.0f/255.0f, 127.0f/255.0f,
    128.0f/255.0f, 129.0f/255.0f, 130.0f/255.0f, 131.0f/255.0f, 132.0f/255.0f, 133.0f/255.0f, 134.0f/255.0f, 135.0f/255.0f,
    136.0f/255.0f, 137.0f/255.0f, 138.0f/255.0f, 139.0f/255.0f, 140.0f/255.0f, 141.0f/255.0f, 142.0f/255.0f, 143.0f/255.0f,
    144.0f/255.0f, 145.0f/255.0f, 146.0f/255.0f, 147.0f/255.0f, 148.0f/255.0f, 149.0f/255.0f, 150.0f/255.0f, 151.0f/255.0f,
    152.0f/255.0f, 153.0f/255.0f, 154.0f/255.0f, 155.0f/255.0f, 156.0f/255.0f, 157.0f/255.0f, 158.0f/255.0f, 159.0f/255.0f,
    160.0f/255.0f, 161.0f/255.0f, 162.0f/255.0f, 163.0f/255.0f, 164.0f/255.0f, 165.0f/255.0f, 166.0f/255.0f, 167.0f/255.0f,
    168.0f/255.0f, 169.0f/255.0f, 170.0f/255.0f, 171.0f/255.0f, 172.0f/255.0f, 173.0f/255.0f, 174.0f/255.0f, 175.0f/255.0f,
    176.0f/255.0f, 177.0f/255.0f, 178.0f/255.0f, 179.0f/255.0f, 180.0f/255.0f, 181.0f/255.0f, 182.0f/255.0f, 183.0f/255.0f,
    184.0f/255.0f, 185.0f/255.0f, 186.0f/255.0f, 187.0f/255.0f, 188.0f/255.0f, 189.0f/255.0f, 190.0f/255.0f, 191.0f/255.0f,
    192.0f/255.0f, 193.0f/255.0f, 194.0f/255.0f, 195.0f/255.0f, 196.0f/255.0f, 197.0f/255.0f, 198.0f/255.0f, 199.0f/255.0f,
    200.0f/255.0f, 201.0f/255.0f, 202.0f/255.0f, 203.0f/255.0f, 204.0f/255.0f, 205.0f/255.0f, 206.0f/255.0f, 207.0f/255.0f,
    208.0f/255.0f, 209.0f/255.0f, 210.0f/255.0f, 211.0f/255.0f, 212.0f/255.0f, 213.0f/255.0f, 214.0f/255.0f, 215.0f/255.0f,
    216.0f/255.0f, 217.0f/255.0f, 218.0f/255.0f, 219.0f/255.0f, 220.0f/255.0f, 221.0f/255.0f, 222.0f/255.0f, 223.0f/255.0f,
    224.0f/255.0f, 225.0f/255.0f, 226.0f/255.0f, 227.0f/255.0f, 228.0f/255.0f, 229.0f/255.0f, 230.0f/255.0f, 231.0f/255.0f,
    232.0f/255.0f, 233.0f/255.0f, 234.0f/255.0f, 235.0f/255.0f, 236.0f/255.0f, 237.0f/255.0f, 238.0f/255.0f, 239.0f/255.0f,
    240.0f/255.0f, 241.0f/255.0f, 242.0f/255.0f, 243.0f/255.0f, 244.0f/255.0f, 245.0f/255.0f, 246.0f/255.0f, 247.0f/255.0f,
    248.0f/255.0f, 249.0f/255.0f, 250.0f/255.0f, 251.0f/255.0f, 252.0f/255.0f, 253.0f/255.0f, 254.0f/255.0f, 255.0f/255.0f,
};

// Gamma correction table for an output device using a 0..1 float scale
// Maps 8-bit input to gamma-corrected 'float' output
static const float gammaDutyCycleMap[256] = {
    0.000000f, 0.000000f, 0.000001f, 0.000004f, 0.000009f, 0.000017f, 0.000028f, 0.000042f,
    0.000062f, 0.000086f, 0.000115f, 0.000151f, 0.000192f, 0.000240f, 0.000296f, 0.000359f,
    0.000430f, 0.000509f, 0.000598f, 0.000695f, 0.000803f, 0.000920f, 0.001048f, 0.001187f,
    0.001337f, 0.001499f, 0.001673f, 0.001860f, 0.002059f, 0.002272f, 0.002498f, 0.002738f,
    0.002993f, 0.003262f, 0.003547f, 0.003847f, 0.004162f, 0.004494f, 0.004843f, 0.005208f,
    0.005591f, 0.005991f, 0.006409f, 0.006845f, 0.007301f, 0.007775f, 0.008268f, 0.008781f,
    0.009315f, 0.009868f, 0.010442f, 0.011038f, 0.011655f, 0.012293f, 0.012954f, 0.013637f,
    0.014342f, 0.015071f, 0.015823f, 0.016599f, 0.017398f, 0.018223f, 0.019071f, 0.019945f,
    0.020844f, 0.021769f, 0.022720f, 0.023697f, 0.024701f, 0.025731f, 0.026789f, 0.027875f,
    0.028988f, 0.030129f, 0.031299f, 0.032498f, 0.033726f, 0.034983f, 0.036270f, 0.037587f,
    0.038935f, 0.040313f, 0.041722f, 0.043162f, 0.044634f, 0.046138f, 0.047674f, 0.049243f,
    0.050844f, 0.052478f, 0.054146f, 0.055847f, 0.057583f, 0.059353f, 0.061157f, 0.062996f,
    0.064870f, 0.066780f, 0.068726f, 0.070708f, 0.072726f, 0.074780f, 0.076872f, 0.079001f,
    0.081167f, 0.083371f, 0.085614f, 0.087895f, 0.090214f, 0.092572f, 0.094970f, 0.097407f,
    0.099884f, 0.102402f, 0.104959f, 0.107558f, 0.110197f, 0.112878f, 0.115600f, 0.118364f,
    0.121170f, 0.124019f, 0.126910f, 0.129844f, 0.132821f, 0.135842f, 0.138907f, 0.142016f,
    0.145170f, 0.148367f, 0.151610f, 0.154898f, 0.158232f, 0.161611f, 0.165037f, 0.168509f,
    0.172027f, 0.175592f, 0.179205f, 0.182864f, 0.186572f, 0.190327f, 0.194131f, 0.197983f,
    0.201884f, 0.205834f, 0.209834f, 0.213883f, 0.217982f, 0.222131f, 0.226330f, 0.230581f,
    0.234882f, 0.239234f, 0.243638f, 0.248094f, 0.252602f, 0.257162f, 0.261774f, 0.266440f,
    0.271159f, 0.275931f, 0.280756f, 0.285636f, 0.290570f, 0.295558f, 0.300601f, 0.305699f,
    0.310852f, 0.316061f, 0.321325f, 0.326645f, 0.332022f, 0.337456f, 0.342946f, 0.348493f,
    0.354098f, 0.359760f, 0.365480f, 0.371258f, 0.377095f, 0.382990f, 0.388944f, 0.394958f,
    0.401030f, 0.407163f, 0.413356f, 0.419608f, 0.425921f, 0.432295f, 0.438730f, 0.445226f,
    0.451784f, 0.458404f, 0.465085f, 0.471829f, 0.478635f, 0.485504f, 0.492436f, 0.499432f,
    0.506491f, 0.513614f, 0.520800f, 0.528052f, 0.535367f, 0.542748f, 0.550194f, 0.557705f,
    0.565282f, 0.572924f, 0.580633f, 0.588408f, 0.596249f, 0.604158f, 0.612133f, 0.620176f,
    0.628287f, 0.636465f, 0.644712f, 0.653027f, 0.661410f, 0.669863f, 0.678384f, 0.686975f,
    0.695636f, 0.704366f, 0.713167f, 0.722038f, 0.730979f, 0.739992f, 0.749075f, 0.758230f,
    0.767457f, 0.776755f, 0.786126f, 0.795568f, 0.805084f, 0.814672f, 0.824334f, 0.834068f,
    0.843877f, 0.853759f, 0.863715f, 0.873746f, 0.883851f, 0.894031f, 0.904286f, 0.914616f,
    0.925022f, 0.935504f, 0.946062f, 0.956696f, 0.967407f, 0.978194f, 0.989058f, 1.000000f
};

//
// Compute the new level for a port
//
static float CalcDutyCycle(int port, uint8_t level)
{
    // apply gamma correction or a simple linear mapping
    uint8_t conf = i2cReg[REG_CONF(port)];
    float duty = ((conf & CONF_GAMMA_ENA) != 0) ? linearDutyCycleMap[level] : gammaDutyCycleMap[level];

    // invert the duty cycle if active-low
    if ((conf & CONF_ACTIVE_LOW) != 0)
        duty = 1.0f - duty;

    // done
    return duty;
}

// ----------------------------------------------------------------------------
//
// Set initial register values
//
static void InitRegs()
{
    // WHO AM I
    i2cReg[REG_WHOAMI] = WHOAMI_ID;
    i2cReg[REG_VERSION] = VERSION_ID;

    // PWM frequency
    SetReg16(REG_FREQL, DEFAULT_PWM_FREQ);

    // clear the level registers
    memset(&i2cReg[REG_LED0], 0x00, 24);

    // initialize the port configuration registers
    uint8_t *pConf = &i2cReg[REG_CONF0_BASE];
    for (int i = 0 ; i < 24 ; ++i)
    {
        // CONF = 0
        *pConf++ = 0;

        // limit level = 0xFF (effectively disables the power limiter)
        *pConf++ = 0xFF;

        // time limit 0 (16-bit register)
        *pConf++ = 0;
        *pConf++ = 0;
    }

    // Set CTRL0 and CTRL1 to all bits 0.  Note that we set CTRL0 last,
    // so that we reset the CTRL0_RESET_REGS bit in this register as the
    // very last thing we do.  The host can poll this bit to determine
    // if a requested register reset has been applied, so we don't want
    // to clear it until all of the other work is done.
    i2cReg[REG_CTRL1] = 0;
    i2cReg[REG_CTRL0] = 0;
}

// ----------------------------------------------------------------------------
//
// Main entrypoint
//
int main()
{
    // enable USB logging, if desired
    IF_USB_LOGGING(stdio_usb_init());
    
    // Enable the hardware watchdog, so that we reboot the CPU if we
    // ever stall.  Set a relatively long timeout during initialization.
    watchdog_enable(1000, true);
    watchdog_update();

    // Set up the PWM manager at the default frequency
    PWMManager pwmManager;
    uint16_t freq = DEFAULT_PWM_FREQ;
    pwmManager.SetFreq(freq);

    // start with outputs disabled - the host can enable them via the ENABLE
    // OUTPUTS bit in the CTRL0 register
    bool outputsEnabled = false;

    // Assign the PWM output ports
    for (int i = 0 ; i < _countof(gpioMap) ; ++i)
    {
        // disconnect the input buffer and pull-up/down, to reduce
        // leakage for active-low ports
        int gp = gpioMap[i];
        gpio_set_input_enabled(gp, false);
        gpio_set_pulls(gp, false, false);

        // initialize PWM on the port
        pwmManager.InitGPIO(gp);
    }

    // set initial I2C register values
    InitRegs();

    // set the HWRESET bit in CTRL0 - this is set only after an actual hard
    // reset, not after a software register reset, so InitRegs() doesn't set it
    i2cReg[REG_CTRL0] |= CTRL0_HWRESET;
    
    // Get our I2C slave address.  Use a default of 0x30, but allow this to be
    // overridden with data stored at the top of flash space.  We use the top
    // of flash as virtual EEPROM memory, since the Pico loader requires that
    // the user program entrypoint is at the bottom of flash memory, and the
    // Pico SDK linker always arranges the program storage in a contiguous
    // block.  This means that some varying amount of flash starting at the
    // lowest address will be occupied by the user program (PWMWorker), and
    // the rest of flash above that point will be left unchanged each time
    // a new program is installed.  That makes the TOP of flash the safest
    // place to put extra user data, since it's as far away as possible from
    // the unpredictably sized contiguous block that the user program will
    // occupy on each program update.  This program is pretty small, so in
    // practice we could use anything in the top half or even top two-thirds
    // of flash, but we really only need about 40 bytes for the address data,
    // so we'll just put it at the very top and be done with it.
    //
    // To ensure that an address has actually been programmed in the flash
    // data area, we'll look for a distinctive signature string.  If we find
    // the string, we'll take it to mean that an address has been programmed.
    // If not, we'll use the default address.
    static const uint8_t DEFAULT_I2C_ADDR = 0x30;
    uint8_t i2cAddr = DEFAULT_I2C_ADDR;
    const uint32_t TOP_OF_FLASH = 0x10000000 + 2*1024*1024;
    const uint32_t FLASH_SECTOR_SIZE = 4096;
    const char addrSignature[] = "PinscapePicoPWMWorker I2C Addr [";
    const char addrSignatureSuffix[] = "] \032";
    const size_t addrSignatureLen = sizeof(addrSignature) - 1;
    const size_t addrSignatureSuffixLen = sizeof(addrSignatureSuffix) - 1;
    const char *addrDataBlock = reinterpret_cast<const char*>(TOP_OF_FLASH - FLASH_SECTOR_SIZE);
    if (memcmp(addrDataBlock, addrSignature, addrSignatureLen) == 0
        && memcmp(addrDataBlock + addrSignatureLen + 2, addrSignatureSuffix, addrSignatureSuffixLen) == 0)
    {
        // looks good - read the two bytes after the signature prefix as a hex number
        char c1 = addrDataBlock[addrSignatureLen];
        char c2 = addrDataBlock[addrSignatureLen+1];
        static const auto HexToInt = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
        };
        i2cAddr = (HexToInt(c1) << 4) | HexToInt(c2);

        Log(LOG_INFO, "I2C slave address configuration found in flash, set to 0x%02X\n", i2cAddr);
    }
    else
    {
        Log(LOG_INFO, "No flash configuration data found; I2C slave address set to default (0x%02X)\n", i2cAddr);
    }

    // Set up I2C on GP26 and GP27
    gpio_init(I2C_SDA);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);

    gpio_init(I2C_SCL);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);

    i2c_init(I2C_UNIT, 400000);
    i2c_slave_init(I2C_UNIT, i2cAddr, &I2CService);

    // Current port settings
    struct Port
    {
        // last setting from I2C register
        uint8_t lastRegLevel = 0;

        // Computed output level, as a fractional duty cycle.  This is the
        // duty cycle after taking into account the power reduction timeout
        // and gamma correction.
        float computedLevel = 0.0f;

        // power level reduction time
        uint64_t tPowerTimeout = END_OF_TIME;

        // reset the port
        void Reset()
        {
            lastRegLevel = 0;
            computedLevel = 0.0f;
            tPowerTimeout = END_OF_TIME;
        }
    };
    Port ports[24];

    // initialization finished - reduce the watchdog timeout
    watchdog_update();
    watchdog_enable(100, true);

    // SWRESET register timer.  To execute a reset, a specific sequence
    // of bytes must be written to SWRESET within a timeout.  This is
    // to ensure that accidental or random writes don't trigger a reset.
    uint64_t swresetTimeout = END_OF_TIME;
    uint8_t swresetState = 0x00;

    // Main loop
    for (;;)
    {
        // let the watchdog know we're running
        watchdog_update();

        // set the blink pattern: "heartbeat" by default, "busy" if an I2C write is in progress
        picoLED.SetBlinkPattern(
            !outputsEnabled ? PicoLED::Pattern::Standby :
            serviceContext.writeStarted ? PicoLED::Pattern::Busy :
            PicoLED::Pattern::Heartbeat);

        // update the status LED
        picoLED.Task();

        // Check for SWRESET events.  If the SWRESET state is one of the reset
        // codes, and we've reached the timeout expiration, it means that the
        // host has completed the SWRESET protocol and it's time to execute
        // the requested reset.
        uint64_t now = time_us_64();
        if (auto &s = serviceContext.swReset; now > s.timeout)
        {
            // check which type of reset was requested
            switch (s.state)
            {
            case SWRESET_RESET:
                // CPU reset requested - reboot by setting the watchdog to a zero timeout
                watchdog_enable(0, false);
                while (true) { }

            case SWRESET_BOOTLOADER:
                // reset to Boot Loader mode requested - reboot through the ROM call,
                // setting the on-board LED as the activity pin
                reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
                while (true) { }
            }
        }

        // check for a register reset
        if ((i2cReg[REG_CTRL0] & CTRL0_RESET_REGS) != 0)
        {
            // re-initialize registers
            InitRegs();

            // let the watchdog know the extra time that took was intentional
            watchdog_update();
        }

        // check for level changes and timeouts
        Port *port = ports;
        uint8_t *pLevel = &i2cReg[REG_LED0];
        uint8_t *pConf = &i2cReg[REG_CONF0_BASE];
        for (int i = 0 ; i < _countof(ports) ; ++i, ++port, ++pLevel, pConf += 4)
        {
            // get the level and limit settings for the port as set in the I2C registers
            uint8_t newRegLevel = *pLevel;
            uint8_t powerLimit = pConf[REG_LIMIT_OFS];

            // get the time limit for the port; if it's locked out due to a write in
            // progress, take a miss on this port for this round, and just leave the
            // register updates pending for the next round
            uint16_t timeLimit;
            if (!GetReg16(REG_TIMELIMIT(i), timeLimit))
                continue;

            // check for a change in the I2C register setting
            if (newRegLevel != port->lastRegLevel)
            {
                // check if the new limit is above the limit level
                if (newRegLevel > powerLimit)
                {
                    // If this newly takes us over the limit, set the timeout.
                    // If we were alerady over the limit, keep the existing timeout,
                    // since we've already burned through some of the allowed
                    // over-limit time.
                    if (port->lastRegLevel <= powerLimit)
                        port->tPowerTimeout = now + timeLimit*1000;
                }
                else
                {
                    // setting a level at or below the limit level clears the timer,
                    // since a level below the limit should be safe indefinitely
                    port->tPowerTimeout = END_OF_TIME;
                }
                
                // remember the new register value
                port->lastRegLevel = newRegLevel;
            }

            // Calculate the new duty cycle based on current settings.
            // Calculate on every cycle even if the host port level
            // didn't change, since the duty cycle ALSO depends on the
            // configuration registers, so a chance in configuration
            // might change the physical port duty cycle even when
            // there's no change in the port's logical level.
            float newComputedLevel = CalcDutyCycle(i, newRegLevel);

            // check for power limit timeouts
            if (now > port->tPowerTimeout)
            {
                // calculate the duty cycle at the reduced level
                newComputedLevel = CalcDutyCycle(i, powerLimit);
            }

            // if the computed level has changed, update the physical port
            if (newComputedLevel != port->computedLevel)
            {
                port->computedLevel = newComputedLevel;
                pwmManager.SetLevel(gpioMap[i], newComputedLevel);
            }
        }

        // Check for a PWM frequency change
        if (uint16_t newFreq; GetReg16(REG_FREQL, newFreq) && newFreq != freq)
            pwmManager.SetFreq(freq = newFreq);


        // check for an OUTPUT ENABLE change
        if (bool newEna = ((i2cReg[REG_CTRL0] & CTRL0_ENABLE_OUTPUTS) != 0); newEna != outputsEnabled)
            pwmManager.EnableOutputs(outputsEnabled = newEna);
    }

    // exit (not reached)
    return 0;
}
