// Pinscape Pico - TLC5940 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implements an interface to one or more daisy chains of TLC5940
// chips.  The TLC5940 is a PWM LED driver chip that can drive 16
// independent channels with 12-bit PWM resolution, at up to about 7000
// Hz PWM refresh rate.  The output channels are constant-current sinks,
// designed to drive individual LEDs or small strings of LEDs.  The
// constant current setting is common to all channels, and is set in the
// hardware layout via a current-sensing resistor attached to the chip.
//
// The chip is designed to drive LEDs directly, but the outputs can also
// be used as logic signals to switch external Darlingtons, MOSFETs, or
// the like, which in turn can drive higher-current devices than the
// TLC5940 ports can drive directly.  Some external circuitry is needed
// to accomplish this; one straightforward approach is to use the
// TLC5940 output ports to drive optocouplers on the LED input side,
// with the optocoupler transistor output side providing the input to
// the Darlington base or MOSFET gate.  An optocoupler input is just an
// LED, after all, which is what the TLC5940 outputs are designed to
// drive, and the TLC5940's PWM frequency limits are well within the
// input timing specs for any optocoupler.
//
// This implementation can support more than one TLC5940 chain.  The
// hard limit imposed by the software design is 8 chains (because each
// chain requires one PIO state machine, and the Pico hardware has eight
// of them).  As a practical matter, the Pico only has enough GPIO pins
// for four chains.  In a virtual pinball application, though, you'd
// never need more than one TLC5940 chain, since you can daisy-chain
// many chips on a single interface.  The limit on the number of chips
// in a chain is squishy; the data interface timing imposes a hard limit
// of 21 chips, but people who have built very long chains of these
// chips reports that signal integrity becomes the limiting factor at
// perhaps 8 chips on the chain.  This is still more than most virtual
// pinball systems would ever need; a very decked-out virtual pinball
// system needs around 64 ports total, which only requires 4 TLC5940's.
//
// The TLC5940 interfaces with the host system via an ad hoc set of
// control signals.  The data interface is essentially a serial shift
// register with a synchronous data clock, with some special timing
// requirements related to the PWM cycle.  The chip also requires the
// host to provide the PWM clock signal and some additional signals for
// controlling the PWM cycle.  In all, the chip requires a minimum of
// six signal inputs from the host.  All of the signals are ad hoc; none
// of them make use of any of the standardized bus protocols that have
// become common and that most modern microcontrollers support with
// on-board hardware, such as I2C or SPI.
//
// We implement the specialized signal interfaces using a Pico PIO
// program.  Almost all of the work of controlling the TLC5940 chain is
// done in the PIO program.  Apart from the initial PIO state machine
// setup work, the only thing the main CPU program has to do is feed the
// grayscale data buffer to the PIO program on each PWM refresh cycle,
// which it does by scheduling a DMA transfer.  The PIO signals to the
// CPU that it's ready for a new grayscale buffer by raising an IRQ
// signal, so the main CPU program doesn't have to perform any periodic
// polling.  And since it uses DMA for the transfer, it only has to set
// up the transfer on each cycle, which takes very little CPU time.  As
// a result, the CPU load of running the chips is tiny.  The IRQ takes
// 1.4us on each PWM cycle, and with the PWM frequency set to a typical
// LED-friendly 250Hz, the 1.4us of IRQ overhead is incurred every 4ms,
// which amounts to only 0.035% of main CPU time.  This increases to
// about 1.03% of CPU at the chip's maximum PWM rate of 7324Hz, which is
// still entirely tolerable.
//
// The DMA transfers are double-buffered.  This lets the main CPU
// program manipulate the grayscale buffer (in response to DOF commands
// from the PC, for example) while the DMA transfer proceeds in the
// background, without any concerns about data inconsistency or races
// between the DMA reader and the main CPU thread.  The main reason to
// use double buffering is data consistency, but I'm not sure that's
// really important for this chip, because we have to send a complete
// update on every PWM cycle.  Even if we did poke a value into the DMA
// stream out of order, the inconsistency would be corrected so quickly
// (on the very next PWM cycle) that it would probably never cause any
// observable effect on the physical outputs.  Removing the double
// buffering might make it possible to eliminate the IRQ callback
// entirely by setting up a pair of DMA channels with their triggers
// linked together in a circle, so that the end of one DMA transfer
// triggers the next.  This would eliminate all CPU involvement in the
// operation of the chip, other than poking new grayscale level values
// into the buffer in response to DOF commands and the like.


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
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "Logger.h"
#include "Config.h"
#include "JSON.h"
#include "CommandConsole.h"
#include "Outputs.h"
#include "PIOHelper.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "GPIOManager.h"
#include "TLC5940.h"


// TLC5940 PIO state machine program - generated via pioasm from TLC5940_pio.pio
extern "C" {
#include "TLC5940_pio.pio.h"
}

// daisy chain
std::vector<std::unique_ptr<TLC5940>> TLC5940::chains;

// get a daisy chain by config index
TLC5940 *TLC5940::GetChain(int n)
{
    return n >= 0 && n < static_cast<int>(chains.size()) ? chains[n].get() : nullptr;
}

// Configure from JSON data
//
// tlc5940: {
//   nChips: <number>,       // number of TLC5940 chips on the daisy chain
//   pwmFreq: <number>,      // PWM frequency in Hz, typically around 200Hz, maximum 7324Hz
//   sclk: <gpNumber>,       // GPIO for SCLK pin connection   }  MUST be adjacent GPIO pins
//   gsclk: <gpNumber>,      // GPIO for GSCLK pin connection  }    in order SCLK, GSCLK
//   sin: <gpNumber>,        // GPIO for SIN pin connection
//   blank: <gpNumber>,      // GPIO for BLANK pin connection  }  MUST be adjacent GPIO pins
//   xlat: <gpNumber>,       // GPIO for XLAT pin connection   }    in order BLANK, XLAT
//
//   // Optional elements for Dot Correct register programming.
//   // If provided, the firmware will send the dot correct data
//   // to the chip during initialization, and will put it into
//   // effect by selecting the DC memory register throughout the
//   // session.  If these are omitted, physically connect DCPRG
//   // and VPRG to GND to use the chip's built-in EEPROM DC data.
//   dcprg: <gpNumber>,      // GPIO for DCPRG pin connection; optional, omit if not wired (connect DCPRG to GND)
//   vprg: <gpNumber>,       // GPIO for VPRG pin connection; optional, omit if not wired (connect VPRG to GND)
//   dcData: [<number>, ...],  // Dot Correct data, one value per port, each value in range 0..63
// }
//
// --- OR ---
//
// tlc5940: [ { first chain }, { second chain} ]
//
// Each daisy chain can have one or more chips, since the chips act as
// shift registers that can be chained together.  In practice, there
// will almost always be only one diasy chain in the system, with all of
// the chips connected together on the one chain, so the configuration
// will only have one entry in the tlc5940 array.  However, it's
// possible in principle to have more than one, so the configuration can
// be specified with an array.  For convenience, it can be specified
// instead as a single object, without an array wrapper.
//
// When configuring a logical output that connects to a TLC5940, the
// port number is specified relative to the entire chain, from 0 to
// 16*<number of chips>-1.  The first port on the first chip in the
// chain is labeled port 0 (this is the same numbering used on the data
// sheet for the chip).
//
// Hardware design notes: the BLANK line should pulled up to VCC via a
// weak pull-up resistor to VCC, 10K or higher.  Pico GPIO ports are in
// high-Z state after reset, so the pull-up will keep BLANK high at
// reset and until the software initializes the ports.  The BLANK signal
// will in turn keep the TLC5940 output ports in high-Z state.  This
// should prevent connected devices from spuriously activating during
// power-up.
//
// Note the requirements for the SCLK/GSCLK and BLANK/XLAT outputs to be
// assigned to CONSECUTIVE GPIO PAIRS.  For example, if SCLK is on GP4,
// GSCLK must be on GP5.  This is required because of the way the pins
// are mapped to the PIO.
//
// The dot correct settings - dcprg, vprg, dcData - can be omitted if
// not needed.  If these aren't used, the physical DCPRG and VPRG pins
// on the chip should be wired to GND, which will select the chip's
// EEPROM dot correction settings.  The EEPROM settings are set at the
// factory to trim all channels to maximum current, which should be just
// what you want for a virtual pinball application.  If desired, though,
// you can wire DCPRG and VPRG to GPIO ports on the Pico, and set the
// related config properties.  The Pico software will write the
// specified DC register data to the chip's in-memory register at
// startup, and then select the DC register data instead of the EEPROM
// data, putting your custom trim settings into effect.  This software
// has no provision for reprogramming the EEPROM, but I can't see any
// need for that, given that we can apply any desired user-specified DC
// data via the DC memory register instead.  The point of the EEPROM is
// to let the user store custom brightness trim settings persistently on
// the chip, and we can achieve the same persistence via the JSON
// configuration data, storing the data in the Pico's flash instead of
// the TLC5940 EEPROM.  It's embedded in the hardware either way.
void TLC5940::Configure(JSONParser &json)
{
    // parse callback
    auto Parse = [](int index, const JSONParser::Value *value)
    {
        // get and validate the settings for this instance
        uint8_t nChips = value->Get("nChips")->UInt8(0);
        uint8_t blank = value->Get("blank")->UInt8(255);
        uint8_t gsclk = value->Get("gsclk")->UInt8(255);
        uint8_t sclk = value->Get("sclk")->UInt8(255);
        uint8_t sin = value->Get("sin")->UInt8(255);
        uint8_t xlat = value->Get("xlat")->UInt8(255);
        int dcprg = value->Get("dcprg")->Int(-1);
        int vprg = value->Get("vprg")->Int(-1);
        uint32_t pwmFreq = value->Get("pwmFreq")->UInt32(200);
        if (!IsValidGP(blank) || !IsValidGP(gsclk) || !IsValidGP(sclk) || !IsValidGP(xlat) || !IsValidGP(sin)
            || (vprg != -1 && !IsValidGP(vprg))
            || (dcprg != -1 && !IsValidGP(dcprg)))
        {
            Log(LOG_ERROR, "tlc5940[%d]: one or more invalid/undefined GPIO pins (blank, gsclk, sclk, xlat, sin, dcprg, vprg)\n", index);
            return;
        }
        
        if (nChips == 0 || nChips > 16)
        {
            Log(LOG_ERROR, "tlc5940[%d]: nchips (number of chips on chain) must be 1-16\n", index);
            return;
        }
        if (pwmFreq == 0 || pwmFreq > 7324)
        {
            Log(LOG_ERROR, "tlc5940[%d]: pwmFreq out of range, must be 1 to 7324 Hz\n", index);
            return;
        }

        // apply GPIO adjacency constraints
        if (gsclk != sclk + 1)
        {
            Log(LOG_ERROR, "tlc5940[%d]: 'sclk' and 'gsclk' GPIO numbers must be consecutive, with sclk first\n", index);
            return;
        }
        if (xlat != blank + 1)
        {
            Log(LOG_ERROR, "tlc5940[%d]: 'blank' and 'xlat' GPIO numbers must be consecutive, with blank first\n", index);
            return;
        }

        // claim the GPIOs
        if (!gpioManager.Claim(Format("TLC5940[%d] (BLANK)", index), blank)
            || !gpioManager.Claim(Format("TLC5940[%d] (GSCLK)", index), gsclk)
            || !gpioManager.Claim(Format("TLC5940[%d] (SCLK)", index), sclk)
            || !gpioManager.Claim(Format("TLC5940[%d] (XLAT)", index), xlat)
            || !gpioManager.Claim(Format("TLC5940[%d] (SIN)", index), sin))
            return;

        // create the device
        auto *chain = new TLC5940(index, nChips, sin, sclk, gsclk, blank, xlat, dcprg, vprg, pwmFreq);
        chains[index].reset(chain);

        // check for DC data
        std::unique_ptr<uint8_t> dcData;
        if (auto *dcArray = value->Get("dcData") ; value->IsArray())
        {
            // allocate space - one element per port
            dcData.reset(new uint8_t[nChips * 16]);

            // populate the array
            value->ForEach([nChips, &dcData](int index, const JSONParser::Value *val) {
                if (index < nChips * 16)
                    dcData.get()[index] = val->UInt8(0);
            });
        }

        // initialize
        chain->Init(dcData.get());
    };

    // If the tlc5940 entry is an array, parse each entry in the array;
    // otherwise just parse it as a single object.
    auto *cfg = json.Get("tlc5940");
    if (cfg->IsObject() || cfg->IsArray())
    {
        chains.resize(cfg->Length(1));
        cfg->ForEach(Parse, true);
    }
    else if (!cfg->IsUndefined())
        Log(LOG_ERROR, "Config: 'tlc5940' key must be an object or array\n");
        

    // if any chains are configured, add our console diagnostics command
    if (chains.size() > 0)
    {
        struct TLC5940Class : CommandConsole::PWMChipClass
        {
            TLC5940Class()
            {
                name = "TLC5940";
                nInstances = chains.size();
                maxLevel = 4095;
                selectOpt = "-c <number>\tselect chain number; default is 0\n--chain <num>\tsame as -c";
            }

            virtual bool IsValidInstance(int instance) const { return instance >= 0 && instance < chains.size() && chains[instance] != nullptr; }
            virtual void ShowStats(const ConsoleCommandContext *c, int instance) const { chains[instance]->Diagnostics(c); }
            virtual int GetNumPorts(int instance) const { return chains[instance]->nPorts; }
            virtual bool IsValidPort(int instance, int port) const { return port >= 0 && port < chains[instance]->nPorts; }
            virtual void SetPort(int instance, int port, int level) const { chains[instance]->Set(port, static_cast<uint16_t>(level)); }
            virtual int GetPort(int instance, int port) const { return chains[instance]->Get(port); }
            virtual void PrintPortName(const ConsoleCommandContext *c, int instance, int port) const { c->Printf("Chip %d, OUT%d", port / 16, port % 16); }
        };
        CommandConsole::AddCommandPWMChip("tlc5940", "TLC5940 chip options", new TLC5940Class());
    }
}

// Construction
TLC5940::TLC5940(int chainNum, int nChips, int gpSIN, int gpSClk, int gpGSClk, int gpBlank, int gpXlat, int gpDCPRG, int gpVPRG, int pwmFreq) :
    chainNum(chainNum), nChips(nChips), nPorts(nChips*16), gpSClk(gpSClk), gpSIN(gpSIN), gpGSClk(gpGSClk),
    gpBlank(gpBlank), gpXlat(gpXlat), gpDCPRG(gpDCPRG), gpVPRG(gpVPRG), pwmFreq(pwmFreq)
{
    // allocate space for the current level array
    level = new uint16_t[nPorts];
    memset(level, 0, nPorts * sizeof(level[0]));

    // Allocate space for the double DMA buffers.  Each buffer
    // consists of a two-element header followed by the brightness
    // level array.  (We allocate the buffers as a contiguous block,
    // and divvy it up manually by storing a pointer to the halfway
    // point.)
    int dmaBufCnt = (nPorts + 2) * 2;
    dmaBuf[0] = new uint16_t[dmaBufCnt];
    dmaBuf[1] = &dmaBuf[0][nPorts + 2];
    memset(dmaBuf[0], 0, dmaBufCnt * sizeof(dmaBuf[0][0]));

    // Set the DMA buffer header with the bit and cycle counts for the
    // PIO program.  These are based on the chip count, so they never
    // change through the session.
    //
    // These values must satisfy the peculiar rules of the PIO program
    // input.  They're used as PIO do-while loop counters, so they have
    // to be ONE LESS than the nominal value; and the PIO program only
    // uses the high-order 12 bits, so they have to be left-shifted by 4
    // bits.
    int nBits = nPorts*12 - 1;
    dmaBuf[0][0] = dmaBuf[1][0] = static_cast<uint16_t>(nBits << 4);
    dmaBuf[0][1] = dmaBuf[1][1] = static_cast<uint16_t>((4096 - nBits) << 4);

    // success
    Log(LOG_CONFIG, "TLC5940[%d] configured, SIN(GP%d), SCLK(GP%d), GSCLK(GP%d), BLANK(GP%d), XLAT(GP%d), %d Hz\n",
        chainNum, gpSIN, gpSClk, gpGSClk, gpBlank, gpXlat, pwmFreq);
}

TLC5940::~TLC5940()
{
    // Delete the level and DMA buffer.  (Note that txLevel isn't
    // a separate allocation unit - it points into dmaBuf - so it
    // doesn't need to be separately deleted.)
    delete[] level;
    delete[] dmaBuf[0];
}

// Initialize
bool TLC5940::Init(const uint8_t *dcData)
{
    // If an earlier instance has already loaded the PIO program, we can
    // use that existing program copy, as long as there's an available
    // state machine on the PIO.  All state machines on a PIO share the
    // same program storage, so the program in principle only has to be
    // loaded once no matter how many TLC5940 instances are using it,
    // as long as a state machine is available.
    for (auto &other : chains)
    {
        // if this chain exists and has set up a PIO, try allocating
        // a state machine on the same PIO unit
        if (other != nullptr && other->pio != nullptr
            && (this->piosm = pio_claim_unused_sm(other->pio, false)) >= 0)
        {
            // got it - attach to the same PIO
            this->pio = other->pio;
            this->pioOffset = other->pioOffset;

            // we can stop looking
            break;
        }
    }

    // If we couldn't attach to an existing copy of the program,
    // try loading a new copy in the other PIO.
    if (this->piosm < 0)
    {
        // try one PIO
        auto TryClaimPIO = [this](PIO pio)
        {
            // make sure we can add our program to the PIO
            if (!pio_can_add_program(pio, &TLC5940_program))
                return false;
            
            // try claiming a state machine on this PIO
            this->piosm = pio_claim_unused_sm(pio, false);
            if (this->piosm < 0)
                return false;
            
            // success - add the program
            this->pioOffset = pio_add_program(pio, &TLC5940_program);
            this->pio = pio;
            return true;
        };

        // try each PIO in turn
        if (!TryClaimPIO(pio0) && !TryClaimPIO(pio1))
        {
            // Failed - log an error and return.  This will leave us without a
            // valid DMA channel, so the task handler will know to just ignore
            // this device throughout this session.
            Log(LOG_ERROR, "TLC5940[%d]: insufficient PIO resources\n", chainNum);
            return false;
        }
    }

    // Set up all of the pins as GPIO outputs for the PIO.  Start them
    // all with a LOW level on output, except for BLANK, which we'll hold
    // HIGH throughout initialization to ensure that the outputs remain
    // disabled (blanked) until the counters are properly initialized.
    // The TLC5940 powers up with random data in the counters and shift
    // registers, so we have to explicitly clear it out.
    auto InitGP = [](int gp, int initialState, bool highDrive)
    {
        if (gp != -1)
        {
            // initialize the pin as an output
            gpio_init(gp);
            gpio_put(gp, initialState);
            gpio_set_dir(gp, GPIO_OUT);
            
            // set the slew rate to FAST, since these are relatively
            // high-speed clock and data signals
            gpio_set_slew_rate(gp, GPIO_SLEW_RATE_FAST);
            
            // set a higher drive strength for designated high-speed pins
            if (highDrive)
                gpio_set_drive_strength(gp, GPIO_DRIVE_STRENGTH_12MA);
        }
    };
    InitGP(gpBlank, 1, false);
    InitGP(gpXlat, 0, false);
    InitGP(gpGSClk, 0, true);
    InitGP(gpSClk, 0, true);
    InitGP(gpSIN, 0, true);
    InitGP(gpDCPRG, 0, false);
    InitGP(gpVPRG, 0, false);

    // Clock out a string of 0 bits to the TLC5940 shift registers, to
    // clear out the random startup data.  Clock out 192 bits per chip
    // (16 outputs * 12 bits), plus a few extras just to make sure that
    // none of the clock signals got lost in startup noise.
    //
    // We can just bit-bang the clock (keeping SIN LOW = 0 bit value),
    // because we're still in the initialization phase where we're not
    // concerned about blocking the main thread.
    for (int i = nPorts*12 + 32 ; i > 0 ; --i)
    {
        gpio_put(gpSClk, 1);
        gpio_put(gpSClk, 0);
    }

    // Latch the zero values from the TLC5940 shift register into
    // the PWM counter registers.  This completes the initialization,
    // setting all of the LED outputs to fully off (0x000 in all of
    // the counter registers on all of the chips in the daisy chain).
    gpio_put(gpXlat, 1);
    gpio_put(gpXlat, 0);

    // If DCPRG and VPRG are wired, and a dot correction data array was
    // provided, initialize the on-chip DC registers with the data
    // specified in the configuration, and enable the in-memory DC
    // register by taking DCPRG high.
    if (gpDCPRG != -1 && gpVPRG != -1 && dcData != nullptr)
    {
        // take VPRG high to enter DC programming mode
        sleep_us(1);
        gpio_put(gpVPRG, 1);
        sleep_us(1);
        
        // shift out the DC register data, 6 bits per port, last chip to
        // first chip, port 15 to port 0, MSB to LSB
        const uint8_t *p = dcData + nPorts - 1;
        for (int i = 0 ; i < nPorts ; ++i)
        {
            // clock out 6 bits for this port
            uint8_t c = *--p;
            for (int bitn = 0 ; bitn < 6 ; ++bitn, c <<= 1)
            {
                // clock out the next bit (MSB first)
                int bit = (c & 0x20) >> 5;
                gpio_put(gpSIN, bit);
                gpio_put(gpSClk, 1);
                gpio_put(gpSClk, 0);
            }
        }

        // latch the shift register data into the DC register
        gpio_put(gpXlat, 1);
        gpio_put(gpXlat, 0);

        // take VPRG low to exit DC programming mode
        sleep_us(1);
        gpio_put(gpVPRG, 0);
        sleep_us(1);

        // an extra clock is needed for the next grayscale cycle
        gpio_put(gpSClk, 1);
        gpio_put(gpSClk, 0);
    }

    // Claim a free DMA channel, for sending grayscale data to the PIO
    dmaChannelTx = dma_claim_unused_channel(false);
    if (dmaChannelTx < 0)
    {
        // Failed to allocate a channel - abort initialization.  By
        // stopping here, we won't set up the BLANK interrupt timer,
        // which will prevent us from trying to send any data on the
        // device.  So this chain instance will remain a valid C++
        // object, so the output manager will be able to send port
        // updates without crashing or otherwise misbehaving, but
        // we'll be inert at the hardware level.
        Log(LOG_ERROR, "TLC5940[%d]: unable to allocate a DMA channel; "
            "this TLC5940 chain will be disabled for this session\n", chainNum);
        return false;
    }
    
    // Set up the DMA configuration for PIO transmission: read from
    // memory with increment, write to PIO TX port with no increment,
    // 16-bit transfers, use PIO DREQ.  Note that the DMA transfer is in
    // 16-bit units, even though the TLC5940 data port word size is 12
    // bits, since DMA only works in whole bytes.  Our PIO program only
    // uses the high-order 12 bits from each 16-bit write.  Note that
    // it uses the *high-order* bits, so we have to left-justify the
    // port values in the transmission buffer (i.e., left-shift each
    // 12-bit value by 4 bits to align it at the high end of the
    // 16-bit container).
    configTx = dma_channel_get_default_config(dmaChannelTx);
    channel_config_set_read_increment(&configTx, true);
    channel_config_set_write_increment(&configTx, false);
    channel_config_set_transfer_data_size(&configTx, DMA_SIZE_16);
    channel_config_set_dreq(&configTx, pio_get_dreq(pio, piosm, true));

    // Enable the TLC5940 outputs by taking BLANK low.  All of the
    // outputs should be fully off at this point even with blanking
    // cleared, since we loaded zeroes into all of the counters.
    gpio_put(gpBlank, 0);

    // Figure the PIO clock divider.  The target clock rate for the PIO
    // is the rate that executes one full loop of the PIO program in
    // exactly the time of one PWM cycle:
    //
    //    T[PWM_CYCLE] = 1/PWMFREQ
    //    T[PIO_LOOP] = <instructions per cycle> * T[PIO_CLOCK_CYCLE]
    //    T[PIO_CLOCK_CYCLE] = 1/PIOFREQ
    //
    // The PIO loop consists of 2 instructions per grayscale count
    // (fixed at 4096 by the chip design), plus (in the current PIO
    // program implementation) 8 cycles in loop setup and blanking
    // interval, for a total of 4096*2 + 8 = 8200 cycles.  Therefore:
    //
    //    8200 / PIOFREQ = 1 / PWMFREQ
    //    PIOFREQ = 8200 * PWMFREQ
    //
    uint32_t sysFreq = clock_get_hz(clk_sys);
    uint32_t pioFreq = pwmFreq * (8192 + 8);
    float pioClockDiv = static_cast<float>(sysFreq) / static_cast<float>(pioFreq);

    // Configure the PIO state machine
    pio_sm_set_enabled(pio, piosm, false);    
    auto piocfg = TLC5940_program_get_default_config(pioOffset);
    sm_config_set_out_pins(&piocfg, gpSIN, 1);          // one OUT pin: gpSIN
    sm_config_set_set_pins(&piocfg, gpBlank, 2);        // two SET pins: gpBlank, gpXlat
    sm_config_set_sideset_pins(&piocfg, gpSClk);        // two sideset pins: gpSClk, gpGSClk
    sm_config_set_out_shift(&piocfg, false, true, 12);  // false=shift LEFT (MSB first), true=autoshift ON, 12=bit refill threshold
    sm_config_set_clkdiv(&piocfg, pioClockDiv);         // PIO clock divider (see above)
    pio_sm_init(pio, piosm, pioOffset, &piocfg);        // initialize with the configuration and starting program offset

    // set the PIO pin directions
    pio_sm_set_consecutive_pindirs(pio, piosm, gpSIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpBlank, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpXlat, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpSClk, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpGSClk, 1, true);

    // Assign the pins to the PIO
    pio_gpio_init(pio, gpBlank);
    pio_gpio_init(pio, gpXlat);
    pio_gpio_init(pio, gpGSClk);
    pio_gpio_init(pio, gpSClk);
    pio_gpio_init(pio, gpSIN);

    // Start the PIO running.  Note that the first thing it's going to
    // do is PULL from the FIFO, which is currently empty, so it'll
    // block on the first instruction.  The actual start of execution
    // will occur when we start our first DMA update.
    pio_sm_set_enabled(pio, piosm, true);

    // Map PIO interrupt flag 0 (INTR register bit SM0, set from our PIO
    // state machine when it executes opcode "IRQ 0") to map to CPU
    // interrupt PIOn_IRQ_0.  (I think of this as "mapping" the
    // interrupt from the state machine to the CPU IRQ space, but it's
    // really more just a matter of enabling the trigger, since the
    // mapping is just a fixed feature of the hardware; SM0 can either
    // trigger one of the PIOn_IRQm interupts or not.  That's why the
    // SDK routine uses the term "enable".  At any rate, setting this
    // 'enable' bit will cause the state machine opcode "IRQ 0" to fire
    // PIOn_IRQ0 on the CPU NVIC, so it does have the effect of a
    // mapping.)
    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);

    // enable the IRQ at the NVIC level
    irq_set_enabled(pio == pio0 ? PIO0_IRQ_0 : PIO1_IRQ_0, true);

    // Register our PIOn_IRQ0 handler with the PIO helper for our
    // assigned PIO.
    PIOHelper::For(pio)->RegisterIRQ0(this);

    // Start the first DMA send
    StartDMASend();
    
    // success
    Log(LOG_CONFIG, "TLC5940[%d] initialized, DMA channel %d, PIO %d.%d@%u, clock divider=%.3lf\n",
        chainNum, dmaChannelTx, pio_get_index(pio), piosm, pioOffset, pioClockDiv);
    return true;
}

// IRQ handler.  Our PIO program fires a PIO IRQ 0 when it finishes
// reading the DMA-transmitted grayscale data, to let us know that it's
// ready to receive the new grayscale data for the next cycle.  The PIO
// IRQ 0 maps to CPU NVIC PIOn_IRQ_0, which we register this routine to
// handle.  The PIO program will spend a while finishing the 4096-clock
// PWM cycle before it starts the new data cycle, so we have some time
// to set up the new DMA feed without anything stalling.  (Stalling the
// PIO would be bad because it would manifest as a dimming or even
// blinking of the lights - the lights will stay off as long as the PIO
// isn't continuously sending out clock pulses.)
//
// Note that PIOn_IRQ0 is a shared interrupt!  It's shared among all
// TLC5940 driver instances running on the same PIO, and it might even
// be shared with unrelated PIO programs that get loaded into the same
// PIO instruction space and run on other state machines.  We must
// therefore check to see if the underlying condition that triggers our
// IRQ exists, specifically, that our DMA channel is idle.  If the
// triggering condition doesn't exist, the interrupt was generated by
// some other state machine, and we can simply return to let the next
// handler run.  It's also possible that we have work to do AND some
// other state machine has work to do - that case is also taken care of
// by checking for actual work to do.  The other handlers will do the
// same, so everyone who has work to do detect it and take care of it,
// and everyone who's not involved in the current IRQ will ignore it.
void TLC5940::PIO_IRQ()
{
    // Check to see if our DMA channel is busy.  If so, our state
    // machine doesn't have any work for us to do, so the interrupt
    // signal must have been raised by some other state machine that's
    // sharing the IRQ.
    if (dma_channel_is_busy(dmaChannelTx))
        return;

    // Check for new data.  If any changes have been made since the last
    // update, the main program will have set dmaNxt to the new buffer.
    // If it's the same as the current buffer, there haven't been any
    // changes, so we can just re-transmit the current buffer.
    if (dmaNxt != dmaCur)
    {
        // There's new data.  Take ownership of the new buffer by
        // making it current.
        dmaCur = dmaNxt;
    }

    // set up the new transmission
    StartDMASend();

    // collect statistics
    stats.nSends += 1;
    stats.avgWindow.n += 1;
    uint64_t now = time_us_64();
    if (stats.avgWindow.n > 1000)
    {
        stats.avgSendTime = static_cast<float>(now - stats.avgWindow.t0) / 1000000.0 / static_cast<float>(stats.avgWindow.n);
        stats.avgWindow.t0 = now;
        stats.avgWindow.n = 0;
    }
}

// Start a DMA transmission from the current active send buffer
void TLC5940::StartDMASend()
{
    dma_channel_configure(dmaChannelTx, &configTx, &pio->txf[piosm], dmaBuf[dmaCur], nPorts + 2, true);
}

// Populate vendor interface output device descriptors
void TLC5940::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            descs->configIndex = chain->chainNum;
            descs->devType = PinscapePico::OutputPortDesc::DEV_TLC5940;
            descs->numPorts = chain->nPorts;
            descs->numPortsPerChip = 16;
            descs->pwmRes = 4096;  // 12-bit PWM
            descs->addr = 0;       // there's no address to report for these chips
            ++descs;
        }
    }
}

// Populate vendor interface output device port descriptors
void TLC5940::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            for (int i = 0 ; i < chain->nPorts ; ++i, ++descs)
                descs->type = descs->TYPE_PWM;  // all TLC5940 ports are PWM outputs
        }
    }
}

// Populate an output level query result buffer
void TLC5940::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            for (int i = 0 ; i < chain->nPorts ; ++i)
            {
                // Pass back the nominal level, 0..4095.  The level[i] value
                // is in the left-justified 16-bit PIO format, so we need
                // to shift right 4 bits to get the 12-bit level.
                levels->level = chain->level[i] >> 4;
                ++levels;
            }
        }
    }
}

void TLC5940::Diagnostics(const ConsoleCommandContext *c)
{
    auto &irqStats = PIOHelper::For(pio)->stats;
    c->Printf(
        "TLC5940 chain #%d:\n"
        "  Number of sends:  %lld\n"
        "  DMA status:       %s\n"
        "  Sends per second: %.1f (%.2lf ms per send)\n"
        "  Avg time in ISR:  %.2f us (%lld us over %lld calls)\n",
        chainNum, stats.nSends, dma_channel_is_busy(dmaChannelTx) ? "Busy" : "Idle",
        1.0f / stats.avgSendTime, stats.avgSendTime * 1000.0f,
        irqStats.AvgTime(), irqStats.t, irqStats.n);
}

// Get a port level
uint16_t TLC5940::Get(int port)
{
    // make sure the port is in range
    if (port >= 0 && port < nPorts)
    {
        // the level[] array is in reverse order of port number, and
        // the entries are 12-bit numbers left-justified in 16-bit
        // fields (so shift right 4 bits to get the 12-bit level)
        return level[nPorts - 1- port] >> 4;
    }

    // invalid port; just return a dummy 0 value
    return 0;
}

// Set a port level.  Ports are numbered from 0 to (nPorts-1).  Port 0
// is the first port (OUT0) on the first chip in the chain.
void TLC5940::Set(int port, uint16_t newLevel)
{
    // make sure the port is in range
    if (port >= 0 && port < nPorts)
    {
        // Figure the level[] array index for the port.  The serial data
        // stream to the whole daisy chain is sent out with the LAST
        // CHIP FIRST, since the chain forms a single giant virtual
        // shift register: the first bit out lands at the "right" end of
        // the last chip's register, and the last bit out lands at the
        // "left" end of the first chip's chip register.  So in our
        // array, elements [0]..[15] are for the LAST chip in the daisy
        // chain, [16]..[31] are the second-to-last chip, etc.
        //
        // Within each chip, the bits are also arranged in high-to-low
        // order.  The first 12 bits sent to a chip correspond to OUT15
        // on that chip, then next 12 bits are OUT14, etc.  That means
        // that our array is arranged in monotonic reverse order of
        // virtual port number:
        //
        //     [nPorts-1]     OUT0 on last chip in daisy chain
        //     [nPorts-2]     OUT1 on last chip
        //     ...
        //     [nPorts-16]    OUT15 on last chip
        //     [nPorts-17]    OUT0 on second-to-last chip
        //     ...
        //     [1]            OUT14 on first chip
        //     [0]            OUT15 on first chip
        //
        // The "first chip on the chain" is the chip whose SIN is
        // connected to the Pico.  The second chip is the chip whose SIN
        // is connected to the first chip's SOUT, and so forth.
        int index = nPorts - 1 - port;

        // Update the level in the live buffer.  Note that we store the
        // values in the PIO DMA format, with the 12 bits of the TLC5940
        // counter value left-justified in the 16-bit array slot by
        // shifting left four bits.
        uint16_t newStoredLevel = (newLevel << 4);
        level[index] = newStoredLevel;
        
        // Check to see if we need to start a new DMA buffer.  If the
        // current and next buffer are the same, we've already handed
        // that buffer over to the DMA channel, so we can't modify it
        // any longer and must switch to the other buffer.  If next
        // and current are different, we've already done the switch,
        // so we just need to poke the new value into the pending
        // buffer.  Note that there's a race with the IRQ if cur !=
        // next, since the IRQ could swoop in and take ownership of
        // next at any time.  So we have to disable interrupts while
        // updating the 'next' buffer if it already exists.
        {
            IRQDisabler irqDisabler;
            if (dmaCur != dmaNxt)
            {
                // We've already started a 'next' buffer on this cycle
                // with other changes, so just poke the new value into
                // the pending buffer.
                uint16_t *txLevel = &dmaBuf[dmaNxt][2];
                txLevel[index] = newStoredLevel;
            }
            else
            {
                // The IRQ handler already has ownership of the current
                // 'next' buffer, so we can't modify it.  Instead, we have
                // to create a brand new 'next' buffer, as a snapshot of the
                // current live data.
                //
                // There's no race with the IRQ in this case, because the
                // IRQ won't touch the 'cur' or 'next' pointers when they're
                // already the same.  So we can unmask interrupts for this
                // fairly lengthy operation of making the new buffer copy.
                irqDisabler.Restore();
                
                // determine which buffer is next, BUT DON'T UPDATE THE
                // INTERNAL POINTER YET, since we don't want the IRQ handler
                // to take ownership yet
                int pendingDmaNxt = dmaCur ^ 1;
                
                // Make a snapshot of the live data into the new buffer.
                // Note that the level buffer starts at the third element,
                // after the two header elements.
                uint16_t *txLevel = &dmaBuf[pendingDmaNxt][2];
                memcpy(txLevel, level, nPorts * sizeof(txLevel[0]));
                
                // Now we can update the next pointer that the IRQ handler
                // sees - it can take possession of the new buffer at any
                // time now that we've completed the new copy.
                dmaNxt = pendingDmaNxt;
            }
        }
    }
}

