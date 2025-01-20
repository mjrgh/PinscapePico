// Pinscape Pico - 74HC165 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements a device driver for the 74HC165 parallel-in,
// serial-out shift register chip, which can be used in Pinscape Pico to
// add button input ports.
//
// The 74HC165 can read its input ports and transfer the data at very high
// speed.  The safe clocking speed per the data sheet is 12 MHz at 2V VCC
// (and actual safe clocking will be higher for our use case since we're
// using the Pico's 3.3V logic).  That allows a 32-port chain (typical for
// a pin cab) to be read in 3.3us.  That's much slower than the native
// GPIO read speed (8ns), but it's still so fast that we'll be able to see
// a switch change state long before the "bounce" oscillations stabilize,
// which takes on the order of a millisecond even for a fast microswitch.
// This means that we can read switches across even a large switch array
// with essentially zero latency.
//
// To take advantage of the high input speed of these devices, we use a
// PIO program to clock in the shift register bits - that lets us run the
// raw data transfer at the full native speed of the chips.  We then use a
// DMA read to transfer the PIO bit reads into CPU memory, which makes the
// transfers nearly instantaneous with no CPU involvement.  Finally, we
// apply debouncing to the inputs on the second core thread, which
// concerns itself only with button inputs for the low-latency devices,
// allowing it to detect switch state changes within a few microseconds.
//
// The DMA transfer writes directly into the data buffer that clients read
// from to check the current state.  This means that the buffer isn't
// necessarily coherent - that is, adjacent bytes might not be from the
// same transfer, so they might not represent the same point in time on
// the physical port inputs on the external chips.  Our attitude is that
// we just don't care: our use case is that the ports are connected to
// buttons or switches, which are independent physical inputs, so callers
// only want to know the instantaneous real-time state of each input.
// Callers always get the latest data that's been transferred physically,
// so we satisfy that demand for the latest real-time state.  If you were
// trying to use this system to read an N-bit-wide data bus, say, that
// lack of coherence would be important, but that's not a use case we
// contemplate.


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
#include "../USBProtocol/VendorIfcProtocol.h"
#include "74HC165.h"
#include "74HC165_pio.pio.h"

// global list of active chains
std::vector<std::unique_ptr<C74HC165>> C74HC165::chains;

// Configure from JSON data
//
// "74hc165": { use a plain object if there's only one chain, which is the typical case }
// "74hc165": [ { or use an array }, { for multiple chains } ]
//
// Object contents in either case:
// {
//   nChips: <number>,          // number of chips in the chain
//   data: <gpio>,              // GPIO port number for Serial Data Out (QH) pin
//   shift: <gpio>              // GPIO port number for Shift Clock (CLK) pin
//   load: <gpio>,              // GPIO port number for Shift/Load (SH/LD) pin
//   loadPolarity: <bool>,      // logic level of SH/LD pin for LOAD mode; use false for 74HC165 (default is false)
//   shiftClockFreq: <number>,  // desired shift clock frequency, in Hz (default 6000000)
// }
//
// Hardware design note: CLK INH (clock inhibit) should be hard-wired to
// GND.  This software doesn't use the signal and doesn't provide a GPIO
// port for it.  (The hardware design can, of course, use CLK INH for
// its own purposes, but this software doesn't itself provide any logic
// for the signal.  Driving CLK INH high blocks the CLK signal from the
// microcontroller, so it will prevent the CPU from reading the shift
// register data.)
//
// The loadPolarity property is optional, and can be omitted for 74HC165
// chips, since the LOAD state is always LOW (false) on these chips.
// The option is provided for the sake of trying to make this code
// compatible with similar chips (that go by other names than 74HC165)
// that have the same pin structure but use a HIGH logic state for LOAD
// mode on their equivalent of the SH/LD pin.  There's no guarantee that
// this code will actually work with any other similar chip that's not
// an actual 74HC165, but since the SH/LD polarity is one difference we
// know for sure exists with some similar chips, we include the option
// just in case it's enough for compatibility.
//
void C74HC165::Configure(JSONParser &json)
{
    // parse callback
    int nConfigured = 0;
    auto Parse = [&nConfigured](int index, const JSONParser::Value *value)
    {
        // get and validate the settings for this instance
        int nChips = value->Get("nChips")->Int(0);
        int shld = value->Get("load")->Int(-1);
        int clk = value->Get("shift")->Int(-1);
        int qh = value->Get("data")->Int(-1);
        bool loadPolarity = value->Get("loadPolarity")->Bool(false);;
        int shiftClockFreq = value->Get("shiftClockFreq")->Int(6000000);
        if (!IsValidGP(shld) || !IsValidGP(clk) || !IsValidGP(qh))
        {
            Log(LOG_ERROR, "74hc165[%d]: one or more invalid/undefined GPIO pins (shift, data, load)\n", index);
            return;
        }

        if (nChips == 0 || nChips > 16)
        {
            Log(LOG_ERROR, "74hc165[%d]: nchips (number of chips on chain) must be 1-16\n", index);
            return;
        }

        // Claim the GPIOs.  Note that even though QH is an input, we
        // claim it in exclusive mode, as there's no meaningful way the
        // pin can be shared with other subsystems.
        if (!gpioManager.Claim(Format("74HC165[%d] (SH/LD)", index), shld)
            || !gpioManager.Claim(Format("74HC165[%d] (CLK)", index), clk)
            || !gpioManager.Claim(Format("74HC165[%d] (QH)", index), qh))
            return;

        // create the device
        auto *chain = new C74HC165(index, nChips, shld, clk, qh, loadPolarity, shiftClockFreq);
        chains[index].reset(chain);

        // initialize
        chain->Init();
        ++nConfigured;
    };

    // If the config entry is an array, parse each entry in the array;
    // otherwise just parse it as a single object.
    auto *cfg = json.Get("74hc165");
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

    // add a command handler if we successfully configured any chips
    if (nConfigured != 0)
    {
        CommandConsole::AddCommand(
            "74hc165", "74HC165 input shift register chip diagnostics",
            "74hc165 [chainNum:][chipNum] <options>\n"
            "  <chainNum> selects a chain by configuration index; default is 0\n"
            "  <chipNum> selects a chip on the chain; default is zero\n"
            "\n"
            "options:\n"
            "  --stats, -s       show statistics\n"
            "  --reset-stats     reset statistics\n"
            "  --list, -l        list port status\n",
            &Command_main_S);
    }
}

C74HC165::C74HC165(int chainNum, int nChips, int shld, int clk, int qh, bool loadPolarity, int shiftClockFreq) :
    chainNum(chainNum), nChips(nChips), nPorts(nChips*8),
    gpSHLD(shld), gpClk(clk), gpQH(qh), loadPolarity(loadPolarity), shiftClockFreq(shiftClockFreq)
{
    // allocate the chip data array
    data = new uint8_t[nChips];
}

C74HC165::~C74HC165()
{
    delete [] data;
}

// read a port level
bool C74HC165::Get(int port)
{
    if (port >= 0 && port < nPorts)
    {
        // Figure the byte and bit location for the port, and pull out
        // that bit from the current data array.  The shift register
        // clocks out bits from the first chip in the chain first, so
        // data[0] has the first chip's bits, data[1] has the second
        // chip's bits, and so on.
        int index = (port >> 3);
        uint8_t bit = 1 << (port & 0x07);
        return (data[index] & bit) != 0;
    }
    return false;
}

// Populate a Vendor Interface button query result buffer with
// ButtonDevice structs representing the configured 74HC165 chips.  The
// caller is responsible for providing enough buffer space; we require
// one PinscapePico::ButtonDevice per daisy chain.  On return, the
// buffer pointer is automatically incremented past the space consumed.
void C74HC165::PopulateDescs(PinscapePico::ButtonDevice* &descs)
{
    int index = 0;
    for (auto &chain : chains)
    {
        descs->configIndex = index++;
        descs->type = PinscapePico::ButtonDesc::SRC_74HC165;
        if (chain != nullptr)
        {
            descs->numPorts = chain->nPorts;
        }
        descs->addr = 0;
        ++descs;
    }
}

// Query the states of the 74HC165 input ports, for a Vendor Interface
// button state query.  Populates the buffer with one byte per input
// port across all daisy chains, arranged in order of the daisy chains
// in the configuration list.  Returns the size in bytes of the
// populated buffer space, or 0xFFFFFFFF on failure.  Note that 0 isn't
// an error: it simply means that there are no 74HC165 ports configured.
size_t C74HC165::QueryInputStates(uint8_t *buf, size_t bufSize)
{
    // add up the amount of space we need - one byte per port across all chains
    size_t resultSize = 0;
    for (auto &chain : chains)
    {
        if (chain != nullptr)
            resultSize += chain->nPorts;
    }
    
    // make sure we have space
    if (resultSize > bufSize)
        return 0xFFFFFFFF;

    // visit each chain
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            // populate the buffer for each port
            for (int i = 0 ; i < chain->nPorts ; ++i)
                *buf++ = chain->Get(i);
        }
    }

    // success - return the populated space
    return resultSize;
}

// second-core task handler
void C74HC165::SecondCoreTask()
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
            chain->SecondCoreChainTask();
    }
}

// second-core chain task handler
void C74HC165::SecondCoreChainTask()
{
    // reset statistics if desired
    stats.CheckResetRequest();

    // if the DMA channel isn't busy, start a new transfer
    if (!dma_channel_is_busy(dmaChan))
    {
        // start the next transfer
        StartTransfer();
    }
}

void C74HC165::StartTransfer()
{
    // count statistics
    stats.AddDMACycle();

    // send the PIO program the "go" signal by writing the port count to the TX FIFO
    pio_sm_put(pio, piosm, nPorts - 1);

    // start the transfer; the PIO program transfers one 8-bit element per chip
    dma_channel_transfer_to_buffer_now(dmaChan, data, nChips);
}

// initialization
bool C74HC165::Init()
{
    // allocate the PIO transfer DMA channel
    if ((dmaChan = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "74H165[%d]: insufficient DMA channels\n", chainNum), false;

    // Use a previously loaded copy of the PIO program, if a state
    // machine is available on the same PIO where it's loaded.
    for (auto &other : chains)
    {
        // check to see if this chain has loaded the program and has an SM available
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
            if (!pio_can_add_program(pio, &C74HC165_program))
                return false;

            // try claiming a state machine on this PIO
            this->piosm = pio_claim_unused_sm(pio, false);
            if (this->piosm < 0)
                return false;
            
            // success - add the program
            this->pioOffset = pio_add_program(pio, &C74HC165_program);
            this->pio = pio;
            return true;
        };
        
        // try each PIO in turn
        if (!TryClaimPIO(pio0) && !TryClaimPIO(pio1))
        {
            // Failed - log an error and return.  This will leave us
            // with a null PIO pointer, so the Task routine will know
            // that it should just ignore this instance for the session.
            Log(LOG_ERROR, "74HC165[%d]: insufficient PIO resources\n", chainNum);
            return false;
        }
    }

    // set up the PIO output pins
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

    // configure CLK and SHLD as outputs; start with CLK low, SH/LD in SHIFT mode
    InitGP(gpClk, 0, true);
    InitGP(gpSHLD, !loadPolarity, true);

    // configure QH as an input
    gpio_init(gpQH);
    gpio_set_dir(gpQH, GPIO_IN);

    // Set the GPIO clock divider for an 12 MHz opcode clock rate.  This
    // will yield a 6 MHz serial clock rate (two opcodes per cycle),
    // which is the minimum valid clock rate per data sheet for VCC =
    // 2V.  Pinscape implementations will almost certainly be using 3.3V
    // VCC for easy Pico interfacing, in which case the maximum allowed
    // rate will be much higher (probably something like 35 MHz, based
    // on interpolating the numbers in the data sheet).  Our 12 MHz PIO
    // clock is extremely conservative, but we really don't need it to
    // be any faster; a 64-input chain would take only 10us to load, and
    // we have no need to poll any faster than 1ms (1000us) between
    // cycles.
    const uint32_t sysFreq = clock_get_hz(clk_sys);
    const uint32_t pioFreq = shiftClockFreq * 2;
    const float pioClockDiv = static_cast<float>(sysFreq) / static_cast<float>(pioFreq);

    // Configure the PIO state machine
    pio_sm_set_enabled(pio, piosm, false);    
    auto piocfg = C74HC165_program_get_default_config(pioOffset);
    sm_config_set_in_pins(&piocfg, gpQH);               // IN pin: QH
    sm_config_set_out_pins(&piocfg, gpSHLD, 1);         // OUT pin: SH/LD
    sm_config_set_sideset_pins(&piocfg, gpClk);         // one sideset pin: CLK
    sm_config_set_out_shift(&piocfg, true, true, 8);    // true=shift RIGHT (LSB first), true=auto-pull ON, 8=bit refill threshold
    sm_config_set_in_shift(&piocfg, false, true, 8);    // true=shift LEFT (LSB first), true=auto-push ON, 8-bit flush threshold
    sm_config_set_clkdiv(&piocfg, pioClockDiv);         // PIO clock divider (see above)
    pio_sm_init(pio, piosm, pioOffset, &piocfg);        // initialize with the configuration and starting program offset

    // Assign the pins to the PIO
    pio_gpio_init(pio, gpClk);
    pio_gpio_init(pio, gpSHLD);
    pio_gpio_init(pio, gpQH);

    // set the PIO pin directions
    pio_sm_set_consecutive_pindirs(pio, piosm, gpQH, 1, false);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpClk, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpSHLD, 1, true);

    // pre-load Y with the SH/LD bit for LOAD mode
    pio_sm_exec(pio, piosm, pio_encode_set(pio_y, loadPolarity ? 1 : 0));

    // Start the PIO running.  Note that the first thing it's going to
    // do is PULL from the FIFO, which is currently empty, so it'll
    // block on the first instruction.  It'll start executing for real
    // when we send our first transmission from Task().
    pio_sm_set_enabled(pio, piosm, true);

    // configure the DMA channel for PIO IN port source, memory output with increment, 8-bit transfers
    auto dmaConf = dma_channel_get_default_config(dmaChan);
    channel_config_set_read_increment(&dmaConf, false);
    channel_config_set_write_increment(&dmaConf, true);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_8);
    channel_config_set_dreq(&dmaConf, pio_get_dreq(pio, piosm, true));
    dma_channel_configure(dmaChan, &dmaConf, nullptr, &pio->rxf[piosm], nChips, false);

    // start the first DMA transfer
    stats.t0 = time_us_64();
    StartTransfer();

    // success
    Log(LOG_CONFIG, "74HC165[%d] initialized, CLK=GP%d, QH=GP%d, SH/LD=GP%d (LD is active %s); DMA ch %d; PIO %d.%d@%u, clock divider=%.3lf\n",
        chainNum, gpClk, gpQH, gpSHLD, loadPolarity ? "high" : "low", dmaChan,
        pio_get_index(pio), piosm, pioOffset, pioClockDiv);
    return true;
}

// Console command handler - main static entrypoint
void C74HC165::Command_main_S(const ConsoleCommandContext *c)
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
        return c->Printf("74hc165: no such chain number (%d)\n", chainNum);

    // get the chain object
    auto *chain = chains[chainNum].get();

    // validate the chip number
    if (chipNum >= chain->nChips)
        return c->Printf("74hc165[%d]: no such chip number (%d)\n", chainNum, chipNum);

    // proceed to the chain's handler
    chain->Command_main(c, chipNum, firstOptionIndex);
}

// per-chain command handler
void C74HC165::Command_main(const ConsoleCommandContext *c, int specifiedChipNum, int firstOptionIndex)
{
    int chipNum = specifiedChipNum < 0 ? 0 : specifiedChipNum;
    int argi = firstOptionIndex;

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
                "74hc165 - chain #%d\n"
                "  DMA cycles:         %llu\n"
                "  Average DMA cycle:  %.2f us\n"
                "  Last data bytes:    ",
                chainNum,
                stats.nDMACycles,
                stats.nDMACycles != 0 ? static_cast<float>((time_us_64() - stats.t0)*1000ULL / stats.nDMACycles)/1000.0f : 0.0f);
            for (int i = 0 ; i < nChips ; ++i)
                c->Printf("%02X%c", data[i], i + 1 < nChips ? ' ' : '\n');
        }
        else if (strcmp(a, "--reset-stats") == 0)
        {
            stats.RequestReset();
            c->Printf("74hc165 chain %d statistics reset\n", chainNum);
        }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0)
        {
            // list ports across all chips, or just the designated chip, if one was explicitly specified
            int start = 0, end = nChips - 1;
            if (specifiedChipNum >= 0)
                start = end = specifiedChipNum;

            for (int i = start ; i <= end ; ++i)
            {
                c->Printf("74HC165[%d] chip %d port status:\n", chainNum, i);
                c->Printf("  Data: %02X\n", data[i]);
                for (int j = 0 ; j < 8 ; ++j)
                    c->Printf("  %c: %s\n", j + 'A', Get(i*8 + j) ? "HIGH" : "LOW");
            }
        }
        else
        {
            return c->Printf("74hc165: invalid option \"%s\"\n", a);
        }
    }
}

