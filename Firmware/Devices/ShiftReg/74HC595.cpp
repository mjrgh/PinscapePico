// Pinscape Pico - 74HC595 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The 74HC595 is a widely used serial-in, parallel-out shift register
// chip.  The chips can be daisy-chained together to form a virtual
// shift register of arbitrary size.
//
// The chip's ports are simple ON/OFF ports, controlled by one shift
// register bit for each port.  At the client level, though, we offer
// both digital and PWM control options.  If PWM mode is enabled for
// a chain, we use a Pico PIO program to rapidly switch the ports on
// and off to control the duty cycle to provide brightness control
// on the outputs.

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "GPIOManager.h"
#include "JSON.h"
#include "Logger.h"
#include "Outputs.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "CommandConsole.h"
#include "74HC595.h"
#include "74HC595_pwm_pio.pio.h"
#include "74HC595_dig_pio.pio.h"

// global list of active chains
std::vector<std::unique_ptr<C74HC595>> C74HC595::chains;

// Configure from JSON data
//
// "74hc595": { use a plain object if there's only one chain, which is the typical case }
// "74hc595": [ { or use an array }, { for multiple chains } ]
//
// Object contents in either case:
// {
//   nChips: <number>,    // number of chips in the chain
//   pwm: <bool>,         // true -> PWM mode, false -> digital ON/OFF mode
//   shift: <gpio>,       // GPIO port for serial shift clock (SHCP pin) } MUST BE CONSECUTIVE GPIO PORTS
//   latch: <gpio>,       // GPIO port for latch/transfer (STCP pin)     }  (e.g., shift=GP6, latch=GP7
//   data: <gpio>,        // GPIO port for serial data (DS pin)
//   enable: <port>,      // GPIO port for Output Enable (/OE pin) - optional, omit if not wired
//                        // OR an Output Manager "device" object
//
//
// The Reset and Enable pins are optional, since the chip will function
// if you hard-wire Reset to VCC and Enable to GND in the hardware.
// This might be desirable for some installations to conserve GPIO pins
// on the Pico, or just to simplify the wiring.  However, be aware that
// hard-wiring the Enable line is likely to cause startup glitching,
// meaning that ports might randomly fire when powering up the machine.
// This is because the 74HC595 powers up with the output ports in a
// random state, with some ports on and some off, unpredictably, and
// varying from one startup to the next.  The Pinscape software turns
// off all of the ports during the software initialization, so any
// random port firing will only last for a few milliseconds, but that
// can be enough to trigger noticable mechanical action if a port is
// controlling a motor or a solenoid or something similar.  Wiring
// Enable to a GPIO, with a pull-up resistor to VCC, should eliminate
// startup glitching by deterministically disabling all of the outputs
// at power-on, and keeping them disabled until the software finishes
// initializing, at which point it will enable the ports through the
// Enable line.  The Reset line isn't nearly as important, since its
// effect can be simulated with the data signals; omitting it won't
// cause any glitching.
//
void C74HC595::Configure(JSONParser &json)
{
    // parse callback
    int nConfigured = 0;
    auto Parse = [&json, &nConfigured](int index, const JSONParser::Value *value)
    {
        // get and validate the settings for this instance
        int nChips = value->Get("nChips")->Int(0);
        int shift = value->Get("shift")->Int(-1);
        int data = value->Get("data")->Int(-1);
        int latch = value->Get("latch")->Int(-1);
        if (!IsValidGP(shift)
            || !IsValidGP(latch)
            || !IsValidGP(data))
        {
            Log(LOG_ERROR, "74hc595[%d]: one or more invalid/undefined GPIO pins (shift, latch, data)\n", index);
            return;
        }

        // get the enable port, which can be either a GPIO or an Output Manager port
        std::unique_ptr<OutputManager::Device> enablePort;
        if (auto *enableVal = value->Get("enable"); !enableVal->IsUndefined())
        {
            // parse the port specification
            char jsonLocus[32];
            snprintf(jsonLocus, _countof(jsonLocus), "74hc595[%d].enable", index);
            enablePort.reset(OutputManager::ParseDevice(
                jsonLocus, Format("74HC595 (/OE)", index), json, enableVal, false, true));

            // abort on failure
            if (enablePort == nullptr)
                return;

            // set the port to HIGH to disable outputs initially
            enablePort->Set(255);
        }

        // validate the chip count
        if (nChips == 0 || nChips > 16)
        {
            Log(LOG_ERROR, "74hc595[%d]: nchips (number of chips on chain) must be 1-16\n", index);
            return;
        }

        // apply GPIO adjacency constraints (per the PIO program requirements)
        if (latch != shift + 1)
        {
            Log(LOG_ERROR, "74hc595[%d]: 'shift' and 'latch' GPIO numbers must be consecutive, with shift first\n", index);
            return;
        }

        // claim the GPIOs
        if (!gpioManager.Claim(Format("74HC595[%d] (SHIFT)", index), shift)
            || !gpioManager.Claim(Format("74HC595[%d] (LATCH)", index), latch)
            || !gpioManager.Claim(Format("74HC595[%d] (DATA)", index), data))
            return;

        // Get the desired SCLK frequency, if specified.  Use 4 MHz by default, since
        // this is the lowest maximum clock speed in the data sheet, specified for
        // running the chip with 2V power supply.  This is extremely conservative,
        // since Pinscape applications will almost always run the chip at Pico logic
        // level of 3.3V, where the SCLK speed will be significantly higher.  The
        // max speed for 3.3V isn't specified, but the spec at 4V is 20 MHz, so
        // interpolating, 3.3V will probably work at up to around 14 MHz.
        int shiftClockFreq = value->Get("shiftClockFreq")->Int(4000000);

        // get the PWM mode
        bool pwm = value->Get("pwm")->Bool(false);

        // create the device
        C74HC595 *chain;
        if (pwm)
            chain = new PWM74HC595(index, nChips, shift, data, latch, enablePort.release(), shiftClockFreq);
        else
            chain = new Digital74HC595(index, nChips, shift, data, latch, enablePort.release(), shiftClockFreq);

        // store it in the global static list
        chains[index].reset(chain);

        // initialize it
        chain->Init();

        // count it
        ++nConfigured;
    };

    // If the config entry is an array, parse each entry in the array;
    // otherwise just parse it as a single object.
    auto *cfg = json.Get("74hc595");
    if (cfg->IsArray())
    {
        // allocate space for the array, and parse the list
        chains.resize(cfg->Length());
        cfg->ForEach(Parse);
    }
    else if (cfg->IsObject())
    {
        // single object entry
        chains.resize(1);
        Parse(0, cfg);
    }

    // if we have any chips configured, set up a console command
    if (nConfigured != 0)
    {
        CommandConsole::AddCommand(
            "74hc595", "74HC595 output shift register chip diagnostics",
            "74hc595 [chainNum:][chipNum] <options>\n"
            "  <chainNum> selects a chain by configuration index; default is 0\n"
            "  <chipNum> selects a chip on the chain; default is zero\n"
            "\n"
            "options:\n"
            "  <port>=<level>    set a port (A-H, QA-QA, 0-7, Q0-Q7) to level; level is 0-255 for PWM\n"
            "                    ports, 0-1 or low/high for digital ports\n"
            "  --levels, -l      list port levels\n"
            "  --stats, -s       show statistics\n"
            "  --reset-stats     reset statistics\n"
            "  --oe <level>      set /OE (Output Enable) level; level can be high, low, 1, 0\n"
            "\n"
            "Setting a port level suspends output management to allow direct chip testing.\n",
            &Command_main_S);
    }
}

C74HC595::C74HC595(
    int chainNum, int nChips, int gpShift, int gpData, int gpLatch, OutputManager::Device *enablePort, int shiftClockFreq) :
    chainNum(chainNum), nChips(nChips), nPorts(nChips*8), gpShift(gpShift), gpData(gpData), gpLatch(gpLatch),
    enablePort(enablePort), shiftClockFreq(shiftClockFreq)
{
}

C74HC595::~C74HC595()
{
}

// Enable/disable all outputs
void C74HC595::EnableOutputs(bool enable)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            // if this chain has an Output Enable port, set it to (LOW=enable, HIGH=disable)
            if (chain->enablePort != nullptr)
                chain->enablePort->Set(enable ? 0 : 255);
        }
    }
}

// Populate vendor interface output device descriptors
void C74HC595::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            descs->configIndex = chain->chainNum;
            descs->devType = PinscapePico::OutputPortDesc::DEV_74HC595;
            descs->numPorts = chain->nPorts;
            descs->numPortsPerChip = 8;
            descs->pwmRes = chain->IsPWM() ? 256 : 2;     // 8-bit PWM or 1-bit digital
            descs->addr = 0;                              // there's no address to report for these chips
            ++descs;
        }
    }
}

// Populate vendor interface output device port descriptors
void C74HC595::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            auto type = chain->IsPWM() ? descs->TYPE_PWM : descs->TYPE_DIGITAL;
            for (int i = 0 ; i < chain->nPorts ; ++i, ++descs)
                descs->type = type;
        }
    }
}

// Populate an output level query result buffer
void C74HC595::PopulateLevels(PinscapePico::OutputDevLevel* &dst)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            for (int i = 0 ; i < chain->nPorts ; ++i)
            {
                dst->level = chain->GetNativeLevel(i);
                ++dst;
            }
        }
    }
}

// global periodic task handler
void C74HC595::Task()
{
    // run periodic tasks on all chains
    for (auto &chain : chains)
    {
        if (chain != nullptr)
            chain->ChainTask();
    }
}

// DMA completion IRQ - static entrypoint
void __not_in_flash_func(C74HC595::SIRQ)()
{
    // scan the chains for completed work
    for (size_t i = 0 ; i < chains.size() ; ++i)
    {
        // if this chain is configured, call its member function interrupt handler
        if (auto *chain = chains[i].get(); chain != nullptr)
            chain->IRQ();
    }
}

// Console command handler - main static entrypoint
void C74HC595::Command_main_S(const ConsoleCommandContext *c)
{
    // check for a <chain>:<chip> designator
    int chainNum = 0, chipNum = -1;
    int firstOptionIndex = 1;
    if (c->argc >= 2)
    {
        // try parsing the designator; if it has the right syntax, take it as a
        // designator, otherwise treat it as just the first option
        int acc = 0;
        bool ok = false;
        bool haveChain = false;
        for (const char *p = c->argv[1] ; ; ++p)
        {
            if (*p == 0)
            {
                // the final accumulator always goes to the chip number
                chipNum = acc;
                ok = true;
                break;
            }
            else if (isdigit(*p))
            {
                // accumulate the next digit
                acc = acc*10 + (*p - '0');
            }
            else if (*p == ':')
            {
                // if we already have a chain number, it's not valid syntax
                if (haveChain)
                    break;

                // consume the accumulator value as the chain number
                haveChain = true;
                chainNum = acc;
                acc = 0;
            }
            else
            {
                // anything else is invalid syntax, so this isn't a chain:chip designator
                break;
            }
        }

        // if we found a valid chain:chip designator, consume the argument;
        // otherwise reset everything to defaults if we didn't find valid syntax
        if (ok)
            ++firstOptionIndex;
        else
            chainNum = 0, chipNum = -1;
    }

    // look up the chain
    if (chainNum < 0 || chainNum >= chains.size() || chains[chainNum] == nullptr)
        return c->Printf("74hc595: no such chain number (%d)\n", chainNum);

    // get the chain object
    auto *chain = chains[chainNum].get();

    // validate the chip number
    if (chipNum >= chain->nChips)
        return c->Printf("74hc595[%d]: no such chip number (%d)\n", chainNum, chipNum);

    // proceed to the chain's handler
    chain->Command_main(c, chipNum, firstOptionIndex);
}

// ---------------------------------------------------------------------------
//
// Digital output implementation
//

Digital74HC595::Digital74HC595(
    int chainNum, int nChips, int gpShift, int gpData, int gpLatch, OutputManager::Device *enablePort, int shiftClockFreq) :
    C74HC595(chainNum, nChips, gpShift, gpData, gpLatch, enablePort, shiftClockFreq)
{
    // Allocate the level array.  This contains the current working state
    // of the ports internally, packed in transmission order.
    level = new uint8_t[nChips];
    memset(level, 0, nChips * sizeof(level[0]));

    // Allocate the transmit buffer - one byte per chip
    txBufCount = nChips;
    txBuf = new uint8_t[txBufCount];
}

Digital74HC595::~Digital74HC595()
{
    delete[] txBuf;
    delete[] level;
}

// update a port
void Digital74HC595::Set(int port, uint8_t dofLevel)
{
    if (port >= 0 && port < nPorts)
    {
        // translate the DOF 8-bit brightness level to a simple ON/OFF value,
        // where any non-zero DOF level is ON
        bool on = (dofLevel != 0);

        // Figure the array index and bit location for the port:
        //
        // - The array index is the chip number offset from the END of the array.
        //   The PIO program streams out the bytes in array order, so the first
        //   byte out ultimately gets shifted out all the way to the end of the
        //   chain, hence array index [0] holds the bits for the last chip on
        //   the chain, and index [nChips-1] is the first chip on the chain.
        //
        // - The PIO program shifts the bits out LSB first.  The first bit
        //   shifted out goes to QH.  Hence bit 0 (bit pattern 0x01) is QH and
        //   bit 7 (0x80) is QA.  
        //
        int index = nChips - 1 - (port / 8);
        uint8_t bit = 0x80 >> (port % 8);
        uint8_t old = level[index];
        if (on)
            level[index] |= bit;
        else
            level[index] &= ~bit;

        // flag that a transmission is needed on the next Task() call
        if (level[index] != old)
            dirty = true;
    }
}

// Get the port in 8-bit DOF notation - OFF=0, ON=255
uint8_t Digital74HC595::GetDOFLevel(int port) const
{
    if (port >= 0 && port < nPorts)
    {
        int index = nChips - 1 - (port / 8);
        uint8_t bit = 0x80 >> (port % 8);
        return (level[index] & bit) != 0 ? 255 : 0;
    }
    else
        return 0;
}

// per-chain task handler
void Digital74HC595::ChainTask()
{
    // If the last DMA transfer isn't still running, start a new one.
    // Send updates whether we have changes to send or not, so that we
    // quickly restore sanity to the chip state if it's ever disrupted
    // by a signal or power glitch.
    if (pio != nullptr && !dma_channel_is_busy(dmaChan))
    {
        // count the write
        stats.StartWrite();
        
        // copy the level array into the transfer buffer
        memcpy(txBuf, level, nChips);
        
        // start the transfer
        dma_channel_transfer_from_buffer_now(dmaChan, txBuf, txBufCount);

        // the chips are now up-to-date with our data
        dirty = false;
    }
}

// DMA IRQ handler
void __not_in_flash_func(Digital74HC595::IRQ)()
{
    if (dma_channel_get_irq0_status(dmaChan))
    {
        // count statistics
        stats.EndWrite();

        // clear the interrupt status flag in the channel
        dma_channel_acknowledge_irq0(dmaChan);
    }
}

// initialization
bool Digital74HC595::Init()
{
    // allocate the PIO transfer DMA channel
    if ((dmaChan = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "74HC595[%d]: insufficient DMA channels\n", chainNum), false;

    // use a previously loaded copy of the PIO program, if a state
    // machine is available on the PIO where it's loaded
    for (auto &other : chains)
    {
        // if this chain exists and has set up a PIO, try allocating
        // a state machine on the same PIO unit
        if (other != nullptr)
        {
            auto *digOther = static_cast<Digital74HC595*>(other.get());
            if (digOther->pio != nullptr && (this->piosm = pio_claim_unused_sm(digOther->pio, false)) >= 0)
            {
                // got it - attach to the same PIO
                this->pio = digOther->pio;
                this->pioOffset = digOther->pioOffset;
                
                // we can stop looking
                break;
            }
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
            if (!pio_can_add_program(pio, &C74HC595DIG_program))
                return false;

            // try claiming a state machine on this PIO
            this->piosm = pio_claim_unused_sm(pio, false);
            if (this->piosm < 0)
                return false;

            // success - add the program
            this->pioOffset = pio_add_program(pio, &C74HC595DIG_program);
            this->pio = pio;
            return true;
        };

        // try each PIO in turn
        if (!TryClaimPIO(pio0) && !TryClaimPIO(pio1))
        {
            // Failed - log an error and return.  This will leave us
            // with a null PIO pointer, so the Task routine will know
            // that it should just ignore this instance for the session.
            Log(LOG_ERROR, "74HC595[%d]: insufficient PIO resources\n", chainNum);
            return false;
        }
    }

    // Set up all of the pins as GPIO outputs for the PIO
    auto InitGP = [](int gp, int initialState, bool highDrive)
    {
        // ignore uninitialized pins
        if (IsValidGP(gp))
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
    InitGP(gpShift, 0, false);
    InitGP(gpData, 0, false);
    InitGP(gpLatch, 0, false);

    // Clock out a string of '0' bits long enough to clear the shift
    // registers across the chain.
    for (int i = 0 ; i < nPorts + 1 ; ++i)
    {
        gpio_put(gpShift, 1);
        gpio_put(gpShift, 0);
    }

    // latch the zeroed shift register to turn off all output ports
    gpio_put(gpLatch, 1);
    gpio_put(gpLatch, 0);

    // Set the GPIO clock divider for an 8 MHz opcode clock rate.  This
    // will yield a 4 MHz SHCP clock rate (two opcodes per SHCP cycle),
    // which is the maximum rate allowed per the data sheet for VCC =
    // 2V.  Pinscape implementations will almost certainly be using 3.3V
    // VCC for easy Pico interfacing, in which case the maximum allowed
    // rate will be much higher (somewhere between 4 MHz at 2V and 20
    // MHz at 4V, so probably at least 12 MHz).  Our 8 MHz PIO clock is
    // thus extremely conservative, but we really don't need it to be
    // much faster, since a full chain refresh will only take about
    // 2.3us at this rate for an 8-chip daisy chain.
    const uint32_t sysFreq = clock_get_hz(clk_sys);
    const uint32_t pioFreq = shiftClockFreq * 2;
    const float pioClockDiv = static_cast<float>(sysFreq) / static_cast<float>(pioFreq);

    // the valid PIO divider range is 1.0 to 65536.0
    if (pioClockDiv < 1.0f || pioClockDiv > 65536.0f)
    {
        Log(LOG_ERROR, "74HC595[%d]: shiftClockFreq out of range; must be %d to %d\n",
            chainNum, static_cast<int>(ceilf(static_cast<float>(sysFreq)/(2.0f*65536.0f))), sysFreq/2);
        return false;
    }

    // Configure the PIO state machine
    pio_sm_set_enabled(pio, piosm, false);    
    auto piocfg = C74HC595DIG_program_get_default_config(pioOffset);
    sm_config_set_out_pins(&piocfg, gpData, 1);         // one OUT pin: gpData
    sm_config_set_sideset_pins(&piocfg, gpShift);       // two sideset pins: gpShift, gpLatch
    sm_config_set_out_shift(&piocfg, true, true, 8);    // true=shift RIGHT (LSB first), true=autoshift ON, 8=bit refill threshold
    sm_config_set_clkdiv(&piocfg, pioClockDiv);         // PIO clock divider (see above)
    sm_config_set_fifo_join(&piocfg, PIO_FIFO_JOIN_TX); // we use only the TX FIFO, so make it extra-deep
    pio_sm_init(pio, piosm, pioOffset, &piocfg);        // initialize with the configuration and starting program offset

    // set the PIO pin directions
    pio_sm_set_consecutive_pindirs(pio, piosm, gpData, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpShift, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpLatch, 1, true);

    // pre-load ISR with the port count minus 1 (per the PIO do-while(x--) loop convention)
    pio_sm_put_blocking(pio, piosm, nPorts - 1);
    pio_sm_exec(pio, piosm, pio_encode_pull(false, false));   // PULL noblock  ; load OSR from FIFO
    pio_sm_exec(pio, piosm, pio_encode_out(pio_isr, 32));     // OUT ISR, 32   ; move 32 bits from OSR to ISR

    // Assign the pins to the PIO
    pio_gpio_init(pio, gpData);
    pio_gpio_init(pio, gpShift);
    pio_gpio_init(pio, gpLatch);

    // configure the DMA channel for memory source with increment, destination PIO OUT port fixed, 8-bit transfers
    auto dmaConf = dma_channel_get_default_config(dmaChan);
    channel_config_set_read_increment(&dmaConf, true);
    channel_config_set_write_increment(&dmaConf, false);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_8);
    channel_config_set_dreq(&dmaConf, pio_get_dreq(pio, piosm, true));
    dma_channel_configure(dmaChan, &dmaConf, &pio->txf[piosm], nullptr, txBufCount, false);

    // set up the DMA completion interrupt
    irq_add_shared_handler(DMA_IRQ_0, &C74HC595::SIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dmaChan, true);

    // Start the PIO running.  Note that the first thing it's going to
    // do is PULL from the FIFO, which is currently empty, so it'll
    // block on the first instruction.  It'll start executing for real
    // when we send our first transmission from Task().
    pio_sm_set_enabled(pio, piosm, true);

    // success
    Log(LOG_CONFIG, "74HC595[%d] initialized; digital mode; data=GP%d, shift=GP%d, latch=GP%d; PIO %d.%d@%u, clock divider=%.3lf; DMA channel %d\n",
        chainNum, gpData, gpShift, gpLatch, pio_get_index(pio), piosm, pioOffset, pioClockDiv, dmaChan);
    return true;
}


// Console command handler - chain member function
void Digital74HC595::Command_main(const ConsoleCommandContext *c, int specifiedChipNum, int firstOptionIndex)
{
    int chipNum = specifiedChipNum < 0 ? 0 : specifiedChipNum;
    int argi = firstOptionIndex;
    auto SetControlPort = [c, &argi](const char *opt, OutputManager::Device *port)
    {
        // get the argument
        if (++argi >= c->argc)
            return c->Printf("74hc595: missing level argument for --oe\n"), false;

        // validate the port
        if (port == nullptr)
            return c->Printf("74hc595[%d]: this chain doesn't have an /OE (output enable) port configured\n"), false   ;

        // parse the option
        uint8_t level = 0;
        const char *arg = c->argv[argi];
        if (strcmp(arg, "high") == 0 || strcmp(arg, "1") == 0)
            level = 255;
        else if (strcmp(arg, "low") == 0 || strcmp(arg, "0") == 0)
            level = 0;
        else
            return c->Printf("74hc595: invalid level argument \"%s\" for %s option; expected high, low, 1, or 0\n", arg, opt), false;

        // set the port level
        port->Set(level);

        // success
        return true;
    };

    // at least one argument is required
    if (argi >= c->argc)
        return c->Usage();

    // scan arguments
    for ( ; argi < c->argc ; ++argi)
    {
        const char *a = c->argv[argi];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            c->Printf(
                "74hc595 - chain #%d\n"
                "  Chip TX cycles:   %llu\n"
                "  Average TX time:  %llu ns\n"
                "  /OE:              %s\n",
                chainNum,
                stats.nWrites,
                stats.nWrites != 0 ? stats.tWriteSum*1000ULL / stats.nWrites : 0,
                enablePort != nullptr ? (enablePort->Get() != 0 ? "HIGH" : "LOW") : "Not Configured");
        }
        else if (strcmp(a, "--reset-stats") == 0)
        {
            stats.Reset();
            c->Printf("74hc595 chain %d statistics reset\n", chainNum);
        }
        else if (strcmp(a, "--oe") == 0)
        {
            if (!SetControlPort(a, enablePort.get()))
                return;
        }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--levels") == 0)
        {
            // list ports across all chips, or just the designated chip, if one was explicitly specified
            int start = 0, end = nChips - 1;
            if (specifiedChipNum >= 0)
                start = end = specifiedChipNum;

            for (int i = start ; i <= end ; ++i)
            {
                c->Printf("74HC595[%d] chip %d port levels:\n", chainNum, i);
                 for (int j = 0 ; j < 8 ; ++j)
                    c->Printf("  Q%c: %s\n", j + 'A', GetNativeLevel(i*8 + j) ? "HIGH" : "LOW");
            }
        }
        else if (a[0] == 'q' || a[0] == 'Q'
                 || (a[0] >= 'a' && a[0] <= 'h') || (a[0] >= 'A' && a[0] <= 'H')
                 || (a[0] >= '0' && a[0] <= '7'))
        {
            // parse it as a port=level setting
            const char *p = a;
            int port = 0;

            // skip the 'Q', if present
            if (*p == 'q' || *p == 'Q')
                ++p;

            // parse the port letter
            if (*p >= 'a' && *p <= 'h')
                port = (*p++ - 'a');
            else if (*p >= 'A' && *p <= 'H')
                port = (*p++ - 'A');
            else if (*p >= '0' && *p <= '7')
                port = (*p++ - '0');
            else
                return c->Printf("74hc595: invalid option \"%s\"\n", a);

            // parse the '='
            if (*p++ != '=')
                return c->Printf("74hc595: invalid option \"%s\"\n", a);

            // parse the level
            bool level;
            if (strcmp(p, "high") == 0 || strcmp(p, "1") == 0)
                level = true;
            else if (strcmp(p, "low") == 0 || strcmp(p, "0") == 0)
                level = false;
            else
                return c->Printf("74hc595: invalid <port>=<level> setting in \"%s\"; expected high, low, 1, or 0\n", a);

            // suspend output management
            OutputManager::SuspendIfActive(c);

            // set the port
            Set(port + chipNum*8, level);
            c->Printf("74hc595[%d.%d] port Q%c -> %s\n", chainNum, chipNum, port + 'A', level ? "HIGH" : "LOW");
        }
        else
        {
            return c->Printf("74hc595: invalid option \"%s\"\n", a);
        }
    }
}

// ---------------------------------------------------------------------------
//
// PWM output implementation
//

PWM74HC595::PWM74HC595(
    int chainNum, int nChips, int gpShift, int gpData, int gpLatch, OutputManager::Device *enablePort, int shiftClockFreq) :
    C74HC595(chainNum, nChips, gpShift, gpData, gpLatch, enablePort, shiftClockFreq)
{
    // Figure the number of ports to send to the PIO program on each
    // cycle.  This is the number of actual ports, rounded up to the
    // next multiple of 16.  The PIO program works in units of 16
    // ports, so we have to add 8 extra ports (one chip's worth) if
    // we have an odd number of chips.
    nPioChips = (nChips + 1) & ~1;
    nPioPorts = nPioChips * 8;

    // allocate the level data array
    level = new uint8_t[nPioPorts];
    memset(level, 0, nPioPorts * sizeof(level[0]));

    // Allocate the TX buffers: 8 x (port bits + footer)
    //
    //   port bits = 1 word per 2 PIO chips
    //   footer (delayCount) = 1 word
    //
    txBufCount = 8*(nPioChips/2 + 1);
    txBuf[0].buf = new uint16_t[txBufCount];
    txBuf[1].buf = new uint16_t[txBufCount];

    // Pre-populate the TX buffers with the fixed parts.  The header and
    // footer elements of the buffer never change, so we can fill these
    // in once in advance, to reduce the work we have to do every time
    // through PrepareTX().  See the PIO program for the buffer layout.
    for (int i = 0 ; i < 2 ; ++i)
    {
        uint16_t *dst = txBuf[i].buf;
        for (int plane = 0, pulseLength = 1 ; plane < 8 ; ++plane, pulseLength *= 2)
        {
            // this is where the bit plane data for this plane goes
            dst += nPioChips/2;
            
            // Footer: delay count for this plane
            *dst++ =  pulseLength == 1 ? 0 : static_cast<uint16_t>((pulseLength - 1)*nPioPorts - 1);
        }
    }
}

PWM74HC595::~PWM74HC595()
{
    delete[] level;
    delete[] txBuf[0].buf;
    delete[] txBuf[1].buf;
}

// update a port
void PWM74HC595::Set(int port, uint8_t val)
{
    if (port >= 0 && port < nPorts && level[port] != val)
    {
        // set the internal level memory, mark the port list dirty
        level[port] = val;
        dirty = true;
    }
}

// Get the DOF level for a port
uint8_t PWM74HC595::GetDOFLevel(int port) const
{
    // we use the same 8-bit scale as DOF internally, so simply return
    // the internal level value
    return (port >= 0 && port < nPorts) ? level[port] : 0;
}

// per-chain task handler
void PWM74HC595::ChainTask()
{
    // If DMA is currently transmitting from the last staging buffer,
    // AND we have changes in the levels array, prepare the next staging
    // buffer.  We can't do this until DMA has picked up our last buffer,
    // and there's no need to do this when there are no changes since the
    // last buffer, since we can just keep sending the last one as long
    // as its source data remain unchanged.
    if (txDMA == txStage && dirty)
    {
        // prepare the inactive buffer
        int newStage = txStage ^ 1;
        PrepareTX(&txBuf[newStage]);

        // Set this as the new staging buffer.  Note that we wait
        // to change this until AFTER we finish filling in the new
        // buffer, so that if the current DMA completes while we're
        // in the middle of filling it, the DMA just repeats with
        // the old buffer.  That will cause a one-PWM-cycle delay in
        // applying the latest changes, but it won't interrupt the
        // update cycle.  We don't have to protect this against
        // interrupts, because the order of operations inherently
        // has no race conditions.  In particular, txDMA == txStage
        // CAN'T become false in IRQ context, so there's no test-
        // and-set race.
        txStage = newStage;

        // we've flushed changes to the chip buffer
        dirty = false;
    }
}

// initialization
bool PWM74HC595::Init()
{
    // use a previously loaded copy of the PIO program, if a state
    // machine is available on the PIO where it's loaded
    for (auto &other : chains)
    {
        // if this chain exists and has set up a PIO, try allocating
        // a state machine on the same PIO unit
        if (other != nullptr && other->IsPWM())
        {
            auto *pwmOther = static_cast<PWM74HC595*>(other.get());
            if (pwmOther->pio != nullptr && (this->piosm = pio_claim_unused_sm(pwmOther->pio, false)) >= 0)
            {
                // got it - attach to the same PIO
                this->pio = pwmOther->pio;
                this->pioOffset = pwmOther->pioOffset;
            
                // we can stop looking
                break;
            }
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
            if (!pio_can_add_program(pio, &C74HC595PWM_program))
                return false;

            // try claiming a state machine on this PIO
            this->piosm = pio_claim_unused_sm(pio, false);
            if (this->piosm < 0)
                return false;
            
            // success - add the program
            this->pioOffset = pio_add_program(pio, &C74HC595PWM_program);
            this->pio = pio;
            return true;
        };
        
        // try each PIO in turn
        if (!TryClaimPIO(pio0) && !TryClaimPIO(pio1))
        {
            // Failed - log an error and return.  This will leave us
            // with a null PIO pointer, so the Task routine will know
            // that it should just ignore this instance for the session.
            Log(LOG_ERROR, "74HC595[%d]: insufficient PIO resources\n", chainNum);
            return false;
        }
    }

    // Set up all of the pins as GPIO outputs for the PIO
    auto InitGP = [](int gp, int initialState, bool highDrive)
    {
        // ignore uninitialized pins
        if (IsValidGP(gp))
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
    InitGP(gpShift, 0, false);
    InitGP(gpData, 0, false);
    InitGP(gpLatch, 0, false);

    // In case the /RESET signal isn't wired, clock out a string of zero
    // bits sufficient to clear the shift registers.  This isn't
    // necessary if a /RESET line is wired, since the /RESET signal
    // we're currently asserting will zero all of the shift register
    // bits.  But it doesn't hurt either way.
    for (int i = 0 ; i < nPorts + 1 ; ++i)
    {
        gpio_put(gpShift, 1);
        gpio_put(gpShift, 0);
    }

    // latch the zeroed shift register to turn off all output ports
    gpio_put(gpLatch, 1);
    gpio_put(gpLatch, 0);

    // Set the GPIO clock divider for 2X the shift clock speed, since
    // we clock SHCP over two PIO cycles.
    const uint32_t sysFreq = clock_get_hz(clk_sys);
    const uint32_t pioFreq = shiftClockFreq * 2;
    const float pioClockDiv = static_cast<float>(sysFreq) / static_cast<float>(pioFreq);

    // the valid PIO divider range is 1.0 to 65536.0
    if (pioClockDiv < 1.0f || pioClockDiv > 65536.0f)
    {
        Log(LOG_ERROR, "74HC595[%d]: shiftClockFreq out of range; must be %d to %d\n",
            chainNum, static_cast<int>(ceilf(static_cast<float>(sysFreq)/(2.0f*65536.0f))), sysFreq/2);
        return false;
    }

    // Configure the PIO state machine
    pio_sm_set_enabled(pio, piosm, false);    
    auto piocfg = C74HC595PWM_program_get_default_config(pioOffset);
    sm_config_set_out_pins(&piocfg, gpData, 1);         // one OUT pin: gpData
    sm_config_set_sideset_pins(&piocfg, gpShift);       // two sideset pins: gpShift, gpLatch
    sm_config_set_out_shift(&piocfg, true, true, 16);   // true=shift RIGHT (LSB first), true=autoshift ON, 16 bit refill threshold
    sm_config_set_clkdiv(&piocfg, pioClockDiv);         // PIO clock divider (see above)
    pio_sm_init(pio, piosm, pioOffset, &piocfg);        // initialize with the configuration and starting program offset

    // pre-load ISR with the port count minus 1 (per the PIO do-while(x--) loop convention)
    pio_sm_put_blocking(pio, piosm, nPioPorts - 1);
    pio_sm_exec(pio, piosm, pio_encode_pull(false, false));   // PULL noblock  ; load OSR from FIFO
    pio_sm_exec(pio, piosm, pio_encode_out(pio_isr, 32));     // OUT ISR, 32   ; move 32 bits from OSR to ISR

    // set the PIO pin directions
    pio_sm_set_consecutive_pindirs(pio, piosm, gpData, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpShift, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpLatch, 1, true);

    // Assign the pins to the PIO
    pio_gpio_init(pio, gpData);
    pio_gpio_init(pio, gpShift);
    pio_gpio_init(pio, gpLatch);

    // Start the PIO running.  Note that the first thing it's going to
    // do is PULL from the FIFO, which is currently empty, so it'll
    // block on the first instruction.  It'll start executing for real
    // when we send our first transmission from Task().
    pio_sm_set_enabled(pio, piosm, true);

    // allocate the PIO transfer DMA channel
    if ((dmaChan = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "74HC595[%d]: insufficient DMA channels\n", chainNum), false;

    // configure for memory source with increment, destination PIO OUT port fixed, 16-bit transfers
    auto dmaConf = dma_channel_get_default_config(dmaChan);
    channel_config_set_read_increment(&dmaConf, true);
    channel_config_set_write_increment(&dmaConf, false);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_16);
    channel_config_set_dreq(&dmaConf, pio_get_dreq(pio, piosm, true));
    dma_channel_configure(dmaChan, &dmaConf, &pio->txf[piosm], nullptr, txBufCount, false);

    // set up the DMA completion interrupt
    irq_add_shared_handler(DMA_IRQ_0, &C74HC595::SIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dmaChan, true);

    // start the first DMA transfer
    PrepareTX(&txBuf[0]);
    dma_channel_transfer_from_buffer_now(dmaChan, txBuf[0].buf, txBufCount);
    stats.nDMA = 1;
    stats.t0DMA = time_us_64();

    // success
    Log(LOG_CONFIG, "74HC595[%d] initialized; PWM mode; data=GP%d, shift=GP%d, latch=GP%d; PIO %d.%d@%u, clock divider=%.3lf; DMA channel %d\n",
        chainNum, gpData, gpShift, gpLatch, pio_get_index(pio), piosm, pioOffset, pioClockDiv, dmaChan);
    return true;
}

// DMA IRQ handler
void __not_in_flash_func(PWM74HC595::IRQ)()
{
    if (dma_channel_get_irq0_status(dmaChan))
    {
        // start DMA on the staging buffer
        txDMA = txStage;
        dma_channel_transfer_from_buffer_now(dmaChan, txBuf[txDMA].buf, txBufCount);
        
        // count statistics
        stats.nDMA += 1;
        
        // clear the interrupt status flag in the channel
        dma_channel_acknowledge_irq0(dmaChan);
    }
}

// Prepare a DMA buffer.  Refer to the PIO program for the buffer format.
//
// This is a CPU-intensive function that's called in the main task loop, so we
// link it in RAM space for faster operation.
void __not_in_flash_func(PWM74HC595::PrepareTX)(TXBuf *tx)
{
    // note the starting time
    uint64_t t0 = time_us_64();

    // Loop over the 8 bit planes of the BCM encoding
    uint16_t *p = tx->buf;
    for (int plane = 0, pwmBit = 0x01 ; plane < 8 ; ++plane, pwmBit <<= 1)
    {
        // Write the data bits.  The first bit out goes to the end of
        // the daisy chain, and the PIO clocks out bits starting at the
        // LSB of the first word we send, so first LSB = last chip QH.
        // Each subsequent bit works goes to the next previous port in
        // the daisy chain.
        const uint8_t *plevel = &level[nPioPorts];
        for (int port = nPioPorts ; ; )
        {
            // Write 16 bits.  This could obviously be expressed more
            // compactly as a loop, but it's about 50% faster to unroll
            // it like this, probably because it eliminates a looping
            // variable for the BCM plane bit (the bit accumulated into
            // 'acc' on each step). The reason we can do this is that
            // nPioPorts is guaranteed to be a multiple of 16 (per the
            // constructor).
            int acc = 0;
            plevel -= 16;
            if ((plevel[15] & pwmBit) != 0) acc |= 0x0001;
            if ((plevel[14] & pwmBit) != 0) acc |= 0x0002;
            if ((plevel[13] & pwmBit) != 0) acc |= 0x0004;
            if ((plevel[12] & pwmBit) != 0) acc |= 0x0008;
            if ((plevel[11] & pwmBit) != 0) acc |= 0x0010;
            if ((plevel[10] & pwmBit) != 0) acc |= 0x0020;
            if ((plevel[9]  & pwmBit) != 0) acc |= 0x0040;
            if ((plevel[8]  & pwmBit) != 0) acc |= 0x0080;
            if ((plevel[7]  & pwmBit) != 0) acc |= 0x0100;
            if ((plevel[6]  & pwmBit) != 0) acc |= 0x0200;
            if ((plevel[5]  & pwmBit) != 0) acc |= 0x0400;
            if ((plevel[4]  & pwmBit) != 0) acc |= 0x0800;
            if ((plevel[3]  & pwmBit) != 0) acc |= 0x1000;
            if ((plevel[2]  & pwmBit) != 0) acc |= 0x2000;
            if ((plevel[1]  & pwmBit) != 0) acc |= 0x4000;
            if ((plevel[0]  & pwmBit) != 0) acc |= 0x8000;
            *p++ = static_cast<uint16_t>(acc);

            // advance by 16 ports
            port -= 16;
            if (port <= 0)
                break;
        }

        // Skip the footer word.  This is prepopulated at initialization
        // and never changes, so we don't have to update it on each prepare.
        p += 1;
    }

    // collect statistics
    stats.AddPrep(time_us_64() - t0);
}

// Console command handler - chain member function
void PWM74HC595::Command_main(const ConsoleCommandContext *c, int specifiedChipNum, int firstOptionIndex)
{
    int chipNum = specifiedChipNum < 0 ? 0 : specifiedChipNum;
    int argi = firstOptionIndex;
    auto SetControlPort = [c, &argi](const char *opt, OutputManager::Device *port)
    {
        // get the argument
        if (++argi >= c->argc)
            return c->Printf("74hc595: missing level argument for --oe\n"), false;

        // validate the port
        if (port == nullptr)
            return c->Printf("74hc595[%d]: this chain doesn't have an /OE (output enable) port configured\n"), false   ;

        // parse the option
        uint8_t level = 0;
        const char *arg = c->argv[argi];
        if (strcmp(arg, "high") == 0 || strcmp(arg, "1") == 0)
            level = 255;
        else if (strcmp(arg, "low") == 0 || strcmp(arg, "0") == 0)
            level = 0;
        else
            return c->Printf("74hc595: invalid level argument \"%s\" for %s option; expected high, low, 1, or 0\n", arg, opt), false;

        // set the port level
        port->Set(level);

        // success
        return true;
    };

    // at least one argument is required
    if (argi >= c->argc)
        return c->Usage();

    // scan arguments
    for ( ; argi < c->argc ; ++argi)
    {
        const char *a = c->argv[argi];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            uint64_t dtDMA = (time_us_64() - stats.t0DMA);
            c->Printf(
                "74hc595 - chain #%d\n"
                "  DMA TX started:       %llu\n"
                "  Average DMA TX time:  %llu us (%u Hz PWM refresh cycle)\n"
                "  TX prep cycles:       %llu\n"
                "  Average TX prep time: %llu us\n"
                "  /OE:                  %s\n",
                chainNum,
                stats.nDMA,
                stats.nDMA != 0 ? dtDMA / stats.nDMA : 0,
                dtDMA != 0 ? stats.nDMA*1000000ULL / dtDMA : 0,
                stats.nPreps,
                stats.nPreps != 0 ? stats.tPrepSum / stats.nPreps : 0,
                enablePort != nullptr ? (enablePort->Get() != 0 ? "HIGH" : "LOW") : "Not Configured");
        }
        else if (strcmp(a, "--reset-stats") == 0)
        {
            stats.Reset();
            c->Printf("74hc595 chain %d statistics reset\n", chainNum);
        }
        else if (strcmp(a, "--oe") == 0)
        {
            if (!SetControlPort(a, enablePort.get()))
                return;
        }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--levels") == 0)
        {
            // list ports across all chips, or just the designated chip, if one was explicitly specified
            int start = 0, end = nChips - 1;
            if (specifiedChipNum >= 0)
                start = end = specifiedChipNum;

            for (int i = start ; i <= end ; ++i)
            {
                c->Printf("74HC595[%d] chip %d port levels:\n", chainNum, i);
                for (int j = 0 ; j < 8 ; ++j)
                    c->Printf("  Q%c: %d\n", j + 'A', GetNativeLevel(i*8 + j));
            }
        }
        else if (a[0] == 'q' || a[0] == 'Q'
                 || (a[0] >= 'a' && a[0] <= 'h') || (a[0] >= 'A' && a[0] <= 'H')
                 || (a[0] >= '0' && a[0] <= '7'))
        {
            // parse it as a port=level setting
            const char *p = a;
            int port = 0;

            // skip the 'Q', if present
            if (*p == 'q' || *p == 'Q')
                ++p;

            // parse the port letter
            if (*p >= 'a' && *p <= 'h')
                port = (*p++ - 'a');
            else if (*p >= 'A' && *p <= 'H')
                port = (*p++ - 'A');
            else if (*p >= '0' && *p <= '7')
                port = (*p++ - '0');
            else
                return c->Printf("74hc595: invalid option \"%s\"\n", a);

            // parse the '='
            if (*p++ != '=')
                return c->Printf("74hc595: invalid option \"%s\"\n", a);

            // parse the level
            bool level = atoi(p);

            // suspend output management
            OutputManager::SuspendIfActive(c);

            // set the port
            Set(port + chipNum*8, level);
            c->Printf("74hc595[%d.%d] %d port Q%c -> %d\n", chainNum, chipNum, port + 'A', level);
        }
        else
        {
            return c->Printf("74hc595: invalid option \"%s\"\n", a);
        }
    }
}

