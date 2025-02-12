// Pinscape Pico - TLC5947 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implements an interface to one or more daisy chains of TLC5947
// chips.  The TLC5947 is a PWM LED driver chip that can drive 24
// independent channels with 12-bit PWM resolution, at 976 Hz PWM
// refresh rate.  The output channels are constant-current sinks,
// designed to drive individual LEDs or small strings of LEDs at a
// configurable current up to 30 mA.  The current applies uniformly
// to each channel, and is configured in hardware by selecting the
// size of a resistor connected to the IREF pin on the chip.
//
// The chip is designed to drive LEDs directly, but the outputs can also
// be used as logic signals to switch external Darlingtons, MOSFETs, or
// the like, which in turn can drive high-curent devices.  External
// circuitry is needed to accomplish this; one straightforward approach
// is to use the TLC5947 output ports to drive optocouplers on the LED
// input side, with the optocoupler transistor output side providing the
// input to the Darlington base or MOSFET gate.  This is well suited to
// the chip, since an optocoupler input is just an LED, which is what
// TLC5947 is purpose-built to drive, and the TLC5947's 976 Hz PWM
// frequency is well within the switching frequency limits of any
// common optocoupler.  Another possible design is to use the TLC5947
// outputs as inputs to inverting-input gate drivers (inverting,
// because the OUTn ports are active low).
//
// This implementation can support more than one TLC5947 chain.  The
// hard limit imposed by the software design is 8 chains (because each
// chain requires one PIO state machine, and the Pico hardware has eight
// of them).  As a practical matter, the Pico only has enough GPIO pins
// for five chains.  In a virtual pinball application, though, you'd
// never need more than one TLC5947 chain, since you can daisy-chain
// many chips on a single interface.  The data sheet doesn't state any
// hard limit on the number of chips in a chain, but as a practical
// matter, it's limited by the propagation delay of the shift register
// output signal, since all chips on the chain share a common shift
// register clock signal.
//
// The TLC5947 interfaces with the host system via an ad hoc set of
// control signals.  The data interface is essentially a serial shift
// register with a synchronous data clock, a latching signal, and a
// blanking signal.  The data, clock, and latching signals load the
// shift register using the normal serial-to-parallel conventions, with
// the added complication that the latch signal also blanks (disables)
// the outputs for the remainder of the current PWM cycle.  The blanking
// signal both disables the outputs and resets the chip's internal PWM
// cycle counter.  This gives the host some degree of control over the
// PWM cycle, but only some, since it runs on an independent 4 MHz
// clock internal to the chip.
//
// We implement the specialized signal interfaces using a Pico PIO
// program.  Almost all of the work of controlling the TLC5947 chain is
// done in the PIO program.  Apart from the initial PIO state machine
// setup work, the only thing the main CPU program has to do is feed the
// grayscale data buffer to the PIO program on each PWM refresh cycle,
// which it does by scheduling a DMA transfer.  The PIO signals to the
// CPU that it's ready for a new grayscale buffer by raising an IRQ
// signal, so the main CPU program doesn't have to perform any periodic
// polling.  And since it uses DMA for the transfer, it only has to set
// up the transfer on each cycle, which takes very little CPU time.  As
// a result, the CPU load of running the chips is minuscule.
//
// The DMA transfers are double-buffered.  This lets the main CPU
// program manipulate the grayscale buffer (in response to DOF commands
// from the PC, for example) while the DMA transfer proceeds in the
// background, without any concerns about data inconsistency or races
// between the DMA reader and the main CPU thread.
//
//
// This code is heavily based on our TLC5940 driver, since that chip has
// a very similar programming interface.  The main difference is that
// TLC5940 requires the host MCU to provide the grayscale clock signal,
// whereas TLC5947 has its own internal 4 MHz grayscale clock.  That
// drives not only the port counters but also the display cycle: the
// chip repeats the current register contents as long as the host
// doesn't send new data.  Even so, we refresh the data on every cycle
// anyway, so that we can maintain sync with the blanking cycle.  In the
// absence of host BLANK signals, the blanking cycle on the chip runs
// freely, which will make it drift out of sync with our clock.  That's
// undesirable because the chip has an old-fashioned and unfriendly
// latching model, where the XLAT signal takes effect IMMEDIATELY,
// without waiting for the next blanking cycle.  To make up for this,
// the chip blanks the outputs for the whole rest of the current cycle
// on XLAT, no matter how far through the cycle it is.  That will make
// the brightness on the outputs glitch horribly when rapid updates are
// sent, such as during fade in/out sequences, since PWM cycles will be
// randomly cut short as the XLATs arrive, randomly dimming the lights
// at whatever rate the updates are being sent.  The only way to avoid
// this is to carefully time updates so that each XLAT arrives at the
// end of a blanking cycle, during the natural blanking period between
// cycles.  And given the free-running internal clock, the only way for
// the Pico to keep in sync with the blanking cycle is to CONTROL the
// blanking cycle, which we can do by sending BLANK signals at fixed
// intervals equal to the natural blanking cycle time.  The BLANK signal
// has the side effect of resetting the internal PWM counters to zero
// when BLANK is de-asserted, so each BLANK signal serves as a
// synchronization point between our clock and the chip's internal
// clock.  This of course requires that we generate the BLANK signals at
// a highly uniform rate, since the PWM brightness depends upon a
// uniform cycle time.  Good thing we're using a PIO to generate the
// signals, then, because the PIOs are extremely good at precise timing.
// As long as we're careful to count our cycles correctly, we can
// maintain perfectly consistent cycle timing between BLANK signals, for
// nice steady brightness on the outputs.  And since we're controlling
// the BLANK signals, we can use that control over the timing to
// perfectly synchronize XLAT latching with BLANK periods, for
// glitch-free updates.
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
#include "TLC5947.h"


// TLC5947 PIO state machine program - generated via pioasm from TLC5947_pio.pio
extern "C" {
#include "TLC5947_pio.pio.h"
}

// daisy chain
std::vector<std::unique_ptr<TLC5947>> TLC5947::chains;

// get a daisy chain by config index
TLC5947 *TLC5947::GetChain(int n)
{
    return n >= 0 && n < static_cast<int>(chains.size()) ? chains[n].get() : nullptr;
}

// Configure from JSON data
//
// tlc5947: {
//   nChips: <number>,       // number of TLC5947 chips on the daisy chain
//   sclk: <gpNumber>,       // GPIO for SCLK pin connection
//   sin: <gpNumber>,        // GPIO for SIN pin connection
//   blank: <gpNumber>,      // GPIO for BLANK pin connection  }  MUST be adjacent GPIO pins
//   xlat: <gpNumber>,       // GPIO for XLAT pin connection   }    in order BLANK, XLAT
// }
//
// --- OR ---
//
// tlc5947: [ { first chain }, { second chain} ]
//
// Each daisy chain can have one or more chips, since the chips act as
// shift registers that can be chained together.  In practice, there
// will almost always be only one diasy chain in the system, with all of
// the chips connected together on the one chain, so the configuration
// will only have one entry in the tlc5947 array.  However, it's
// possible in principle to have more than one, so the configuration can
// be specified with an array.  For convenience, it can be specified
// instead as a single object, without an array wrapper.
//
// When configuring a logical output that connects to a TLC5947, the
// port number is specified relative to the entire chain, from 0 to
// 24*<number of chips>-1.  The first port on the first chip in the
// chain is labeled port 0 (this is the same numbering used on the data
// sheet for the chip).
//
// Hardware design notes: the BLANK line should pulled up to VCC via a
// weak pull-up resistor, around 10K.  Pico GPIO ports are weakly pulled
// to ground after reset, so the pull-up will keep BLANK high at reset
// and until the software initializes the ports.  The BLANK signal will
// in turn keep the TLC5947 output ports in high-Z state.  This should
// prevent connected devices from spuriously activating during power-up.
//
// Note the requirements for the BLANK/XLAT outputs to be assigned to
// CONSECUTIVE GPIO PINS, with XLAT at the higher GP number.  For
// example, if BLANK is on GP10, XLAT must be on GP11.  This is required
// because of the way the pins are mapped to the PIO.
void TLC5947::Configure(JSONParser &json)
{
    // parse callback
    auto Parse = [](int index, const JSONParser::Value *value)
    {
        // get and validate the settings for this instance
        uint8_t nChips = value->Get("nChips")->UInt8(0);
        uint8_t blank = value->Get("blank")->UInt8(255);
        uint8_t sclk = value->Get("sclk")->UInt8(255);
        uint8_t sin = value->Get("sin")->UInt8(255);
        uint8_t xlat = value->Get("xlat")->UInt8(255);
        if (!IsValidGP(blank) || !IsValidGP(sclk) || !IsValidGP(xlat) || !IsValidGP(sin))
        {
            Log(LOG_ERROR, "tlc5947[%d]: one or more invalid/undefined GPIO pins (blank, gsclk, sclk, xlat, sin, dcprg, vprg)\n", index);
            return;
        }

        // The data sheet doesn't specify a hard limit on the number of
        // chips in a chain, but our transmission algorithm creates its
        // own limit, arising from the signal timing.  Our transmission
        // algorithm requires sending the complete register file for the
        // whole chain on each PWM refresh cycle, so PWM cycle time sets
        // an upper bound on how long we can spend transmitting, and
        // thus on how many bits we can transmit, and thus on how many
        // chips we can include in the transmission.  The TLC5947 has a
        // fixed PWM cycle of 1024us.  It takes 19.2us at 15MHz to send
        // the 288 bits (24 ports times 12 bits per port) of one chip's
        // register file, so we can fit about 53 chips worth of register
        // data into one refresh cycle.
        //
        // 53 chips amounts to 1272 output ports, which is far more than
        // the 255-port limit that we can expose through DOF due to its
        // 8-bit port numbering, which equates to 10 of the chips.
        // That's not a hard limit, since our internal port numbering
        // scheme isn't so limited, but exceeding it will at the very
        // least create some inconveniences for a user.
        //
        // For configuration purposes, we'll set the limit at 16.  This
        // is arbitrary, since it's way below the hard timing limit, and
        // a bit above the implied DOF limit.  But the point is just to
        // give the user some simple guidance, and to detect cases where
        // the JSON seems way out of bounds, so that we can log an
        // explanatory error rather than crash or manifest mysterious
        // misbehavior.
        if (nChips == 0 || nChips > 16)
        {
            Log(LOG_ERROR, "tlc5947[%d]: nChips (number of chips on chain) must be 1-16\n", index);
            return;
        }

        // apply GPIO adjacency constraints
        if (xlat != blank + 1)
        {
            Log(LOG_ERROR, "tlc5947[%d]: 'blank' and 'xlat' GPIO numbers must be consecutive, with blank first\n", index);
            return;
        }

        // claim the GPIOs
        if (!gpioManager.Claim(Format("TLC5947[%d] (BLANK)", index), blank)
            || !gpioManager.Claim(Format("TLC5947[%d] (SCLK)", index), sclk)
            || !gpioManager.Claim(Format("TLC5947[%d] (XLAT)", index), xlat)
            || !gpioManager.Claim(Format("TLC5947[%d] (SIN)", index), sin))
            return;

        // create the device
        auto *chain = new TLC5947(index, nChips, sin, sclk, blank, xlat);
        chains[index].reset(chain);

        // initialize
        chain->Init();
    };

    // If the tlc5947 entry is an array, parse each entry in the array;
    // otherwise just parse it as a single object.
    auto *cfg = json.Get("tlc5947");
    if (cfg->IsObject() || cfg->IsArray())
    {
        chains.resize(cfg->Length(1));
        cfg->ForEach(Parse, true);
    }
    else if (!cfg->IsUndefined())
        Log(LOG_ERROR, "Config: 'tlc5947' key must be an object or array\n");
        

    // if any chains are configured, add our console diagnostics command
    if (chains.size() > 0)
    {
        struct TLC5947Class : CommandConsole::PWMChipClass
        {
            TLC5947Class()
            {
                name = "TLC5947";
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
            virtual void PrintPortName(const ConsoleCommandContext *c, int instance, int port) const { c->Printf("Chip %d, OUT%d", port / 24, port % 24); }
        };
        CommandConsole::AddCommandPWMChip("tlc5947", "TLC5947 chip options", new TLC5947Class());
    }
}

// Construction
TLC5947::TLC5947(int chainNum, int nChips, int gpSIN, int gpSClk, int gpBlank, int gpXlat) :
    chainNum(chainNum), nChips(nChips), nPorts(nChips*24), gpSClk(gpSClk), gpSIN(gpSIN), gpBlank(gpBlank), gpXlat(gpXlat)
{
    // allocate space for the current level array
    level = new uint16_t[nPorts];
    memset(level, 0, nPorts * sizeof(level[0]));

    // Allocate space for the double DMA buffers.  Each buffer consists
    // one element per brightness level.  Allocate both buffers as a
    // contiguous block, and divvy it up manually by storing a pointer
    // to the halfway point.
    int dmaBufCnt = (nPorts) * 2;
    dmaBuf[0] = new uint16_t[dmaBufCnt];
    dmaBuf[1] = &dmaBuf[0][nPorts];
    memset(dmaBuf[0], 0, dmaBufCnt * sizeof(dmaBuf[0][0]));

    // success
    Log(LOG_CONFIG, "TLC5947[%d] configured, SIN(GP%d), SCLK(GP%d), BLANK(GP%d), XLAT(GP%d)\n",
        chainNum, gpSIN, gpSClk, gpBlank, gpXlat);
}

TLC5947::~TLC5947()
{
    // Delete the level and DMA buffer.  (Note that txLevel isn't
    // a separate allocation unit - it points into dmaBuf - so it
    // doesn't need to be separately deleted.)
    delete[] level;
    delete[] dmaBuf[0];
}

// Initialize
bool TLC5947::Init()
{
    // If an earlier instance has already loaded the PIO program, we can
    // use that existing program copy, as long as there's an available
    // state machine on the PIO.  All state machines on a PIO share the
    // same program storage, so the program in principle only has to be
    // loaded once no matter how many TLC5947 instances are using it,
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
            if (!pio_can_add_program(pio, &TLC5947_program))
                return false;
            
            // try claiming a state machine on this PIO
            this->piosm = pio_claim_unused_sm(pio, false);
            if (this->piosm < 0)
                return false;
            
            // success - add the program
            this->pioOffset = pio_add_program(pio, &TLC5947_program);
            this->pio = pio;
            return true;
        };

        // try each PIO in turn
        if (!TryClaimPIO(pio0) && !TryClaimPIO(pio1))
        {
            // Failed - log an error and return.  This will leave us without a
            // valid DMA channel, so the task handler will know to just ignore
            // this device throughout this session.
            Log(LOG_ERROR, "TLC5947[%d]: insufficient PIO resources\n", chainNum);
            return false;
        }
    }

    // Set up all of the pins as GPIO outputs for the PIO.  Start them
    // all with a LOW level on output, except for BLANK, which we'll hold
    // HIGH throughout initialization to ensure that the outputs remain
    // disabled (blanked) until the counters are properly initialized.
    // The TLC5947 powers up with random data in the counters and shift
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
    InitGP(gpSClk, 0, true);
    InitGP(gpSIN, 0, true);

    // Clock out a string of 0 bits to the TLC5947 shift registers, to
    // clear out the random startup data.  Clock out 288 bits per chip
    // (24 outputs * 12 bits), plus a few extras just to make sure that
    // none of the clock signals got lost in startup noise.
    //
    // We can just bit-bang the clock (keeping SIN LOW = 0 bit value),
    // because we're still in the initialization phase where we're not
    // concerned about blocking the main thread.
    //
    // The chip can accept input at up to 15 MHz (for a daisy-chain
    // configuration), or 66ns per SCLK.  At 8ns per Pico CPU cycle,
    // we need 8 clocks per SCLK cycle, or 4 clocks per up/down op.
    // We can assume that the loop processing takes at least 2 clocks
    // per iteration.
    for (int i = nPorts*12 + 32 ; i > 0 ; --i)
    {
        gpio_put(gpSClk, 1);
        __asm volatile ("nop");
        __asm volatile ("nop");
        __asm volatile ("nop");
        __asm volatile ("nop");
        
        gpio_put(gpSClk, 0);
        __asm volatile ("nop");
        __asm volatile ("nop");
        // two more clocks assumed in loop reinit-and-test
    }

    // Latch the zero values from the TLC5947 shift register into
    // the PWM counter registers.  This completes the initialization,
    // setting all of the LED outputs to fully off (0x000 in all of
    // the counter registers on all of the chips in the daisy chain).
    gpio_put(gpXlat, 1);
    __asm volatile ("nop");  // min 30ns pulse width required
    __asm volatile ("nop");  // -> 4x 8ns CPU clocks
    __asm volatile ("nop");
    __asm volatile ("nop");
    gpio_put(gpXlat, 0);

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
        Log(LOG_ERROR, "TLC5947[%d]: unable to allocate a DMA channel; "
            "this TLC5947 chain will be disabled for this session\n", chainNum);
        return false;
    }
    
    // Set up the DMA configuration for PIO transmission: read from
    // memory with increment, write to PIO TX port with no increment,
    // 16-bit transfers, use PIO DREQ.  Note that the DMA transfer is in
    // 16-bit units, even though the TLC5947 data port word size is 12
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

    // Enable the TLC5947 outputs by taking BLANK low.  All of the
    // outputs should be fully off at this point even with blanking
    // cleared, since we loaded zeroes into all of the counters.
    gpio_put(gpBlank, 0);

    // Set the PIO clock divider for 15 MHz SCLK, 2 PIO clocks per
    // SCLK -> 30 MHz PIO clock.
    uint32_t sysFreq = clock_get_hz(clk_sys);
    uint32_t pioFreq = 30000000;
    float pioClockDiv = static_cast<float>(sysFreq) / static_cast<float>(pioFreq);

    // Configure the PIO state machine
    pio_sm_set_enabled(pio, piosm, false);    
    auto piocfg = TLC5947_program_get_default_config(pioOffset);
    sm_config_set_out_pins(&piocfg, gpSIN, 1);          // one OUT pin: gpSIN
    sm_config_set_set_pins(&piocfg, gpBlank, 2);        // two SET pins: gpBlank, gpXlat
    sm_config_set_sideset_pins(&piocfg, gpSClk);        // one sideset pins: gpSClk
    sm_config_set_out_shift(&piocfg, false, true, 12);  // false=shift LEFT (MSB first), true=autoshift ON, 12=bit refill threshold
    sm_config_set_clkdiv(&piocfg, pioClockDiv);         // PIO clock divider (see above)
    pio_sm_init(pio, piosm, pioOffset, &piocfg);        // initialize with the configuration and starting program offset

    // Pre-load the bit-shift loop count, minus 1 for do-while looping, into ISR
    pio_sm_put_blocking(pio, piosm, nPorts*12 - 1);
    pio_sm_exec(pio, piosm, pio_encode_pull(false, true));     // PULL (if-empty no, blocking yes)
    pio_sm_exec(pio, piosm, pio_encode_mov(pio_isr, pio_osr)); // MOV ISR, OSR
    pio_sm_exec(pio, piosm, pio_encode_out(pio_null, 12));     // OUT NULL, 12 (empty OSR)

    // Pre-load the post-data-transfer wait time into Y.  See the PIO program
    // for details.
    pio_sm_put_blocking(pio, piosm, 2*(15*1024 - 12*nPorts) + 29);
    pio_sm_exec(pio, piosm, pio_encode_pull(false, true));     // PULL (if-empty no, blocking yes)
    pio_sm_exec(pio, piosm, pio_encode_mov(pio_y, pio_osr));   // MOV Y, OSR
    pio_sm_exec(pio, piosm, pio_encode_out(pio_null, 12));     // OUT NULL, 12 (empty OSR)

    // set the PIO pin directions
    pio_sm_set_consecutive_pindirs(pio, piosm, gpSIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpBlank, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpXlat, 1, true);
    pio_sm_set_consecutive_pindirs(pio, piosm, gpSClk, 1, true);

    // Assign the pins to the PIO
    pio_gpio_init(pio, gpBlank);
    pio_gpio_init(pio, gpXlat);
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
    Log(LOG_CONFIG, "TLC5947[%d] initialized, DMA channel %d, PIO %d.%d@%u, clock divider=%.3lf\n",
        chainNum, dmaChannelTx, pio_get_index(pio), piosm, pioOffset, pioClockDiv);
    return true;
}

// IRQ handler.  Our PIO program fires a PIO IRQ 0 when it finishes
// reading the DMA-transmitted grayscale data, to let us know that it's
// ready to receive the new grayscale data for the next cycle.  The PIO
// IRQ 0 maps to CPU NVIC PIOn_IRQ_0, which we register this routine to
// handle.  The PIO program will spend a while finishing the 4096-clock
// PWM cycle before it starts the new data cycle, so we have some time
// to set up the new DMA feed without anything stalling.  (In the event
// of a stall, the chip will auto-repeat the current data, so there
// won't be any immediate glitching.  But that also means that the
// chip's blanking cycle is running freely without reference to our PIO
// program loop cycle, so the next time we send an XLAT, it will cut the
// chip's current blanking cycle short, briefly dimming the outputs
// because of the shortened cycle.  So we want to avoid stalling as much
// as possible.)
//
// Note that PIOn_IRQ0 is a shared interrupt!  It's shared among all
// TLC5947 driver instances running on the same PIO, and it might even
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
void TLC5947::PIO_IRQ()
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
void TLC5947::StartDMASend()
{
    // the buffer consists of one element per port
    dma_channel_configure(dmaChannelTx, &configTx, &pio->txf[piosm], dmaBuf[dmaCur], nPorts, true);
}

// Populate vendor interface output device descriptors
void TLC5947::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            descs->configIndex = chain->chainNum;
            descs->devType = PinscapePico::OutputPortDesc::DEV_TLC5947;
            descs->numPorts = chain->nPorts;
            descs->numPortsPerChip = 24;
            descs->pwmRes = 4096;  // 12-bit PWM
            descs->addr = 0;       // there's no address to report for these chips
            ++descs;
        }
    }
}

// Populate vendor interface output device port descriptors
void TLC5947::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            for (int i = 0 ; i < chain->nPorts ; ++i, ++descs)
                descs->type = descs->TYPE_PWM;  // all TLC5947 ports are PWM outputs
        }
    }
}

// Populate an output level query result buffer
void TLC5947::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
{
    for (auto &chain : chains)
    {
        if (chain != nullptr)
        {
            // the level[i] array is in reverse order of port number
            for (int i = chain->nPorts - 1 ; i >= 0 ; --i)
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

void TLC5947::Diagnostics(const ConsoleCommandContext *c)
{
    auto &irqStats = PIOHelper::For(pio)->stats;
    c->Printf(
        "TLC5947 chain #%d:\n"
        "  Number of sends:  %lld\n"
        "  DMA status:       %s\n"
        "  Sends per second: %.1f (%.2lf ms per send)\n"
        "  Avg time in ISR:  %.2f us (%lld us over %lld calls)\n",
        chainNum, stats.nSends, dma_channel_is_busy(dmaChannelTx) ? "Busy" : "Idle",
        1.0f / stats.avgSendTime, stats.avgSendTime * 1000.0f,
        irqStats.AvgTime(), irqStats.t, irqStats.n);
}

// Get a port level
uint16_t TLC5947::Get(int port)
{
    // make sure the port is in range
    if (port >= 0 && port < nPorts)
    {
        // the level[] array is in reverse order of port number, and
        // the entries are 12-bit numbers left-justified in 16-bit
        // fields (so shift right 4 bits to recover the 12-bit level)
        return level[nPorts - 1- port] >> 4;
    }

    // invalid port; just return a dummy 0 value
    return 0;
}

// Set a port level.  Ports are numbered from 0 to (nPorts-1).  Port 0
// is the first port (OUT0) on the first chip in the chain.
void TLC5947::Set(int port, uint16_t newLevel)
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
        // array, elements [0]..[23] are for the LAST chip in the daisy
        // chain, [24]..[47] are the second-to-last chip, etc.
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
        //     [nPorts-22]    OUT23 on last chip
        //     [nPorts-23]    OUT0 on second-to-last chip
        //     ...
        //     [1]            OUT22 on first chip
        //     [0]            OUT23 on first chip
        //
        // The "first chip on the chain" is the chip whose SIN is
        // connected to the Pico.  The second chip is the chip whose SIN
        // is connected to the first chip's SOUT, and so forth.
        int index = nPorts - 1 - port;

        // Update the level in the live buffer.  Note that we store the
        // values in the PIO DMA format, with the 12 bits of the TLC5947
        // counter value left-justified in the 16-bit array slot by
        // shifting left four bits.  (Note that we're not rescaling the
        // value to 16 bits - we're just left-aligning a 12-bit value
        // in a 16-bit container.  The low-order 4 bits of the 16-bit
        // container aren't used.)
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
                uint16_t *txLevel = &dmaBuf[dmaNxt][0];
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
                
                // Make a snapshot of the live data into the new buffer
                uint16_t *txLevel = &dmaBuf[pendingDmaNxt][0];
                memcpy(txLevel, level, nPorts * sizeof(txLevel[0]));
                
                // Now we can update the next pointer that the IRQ handler
                // sees - it can take possession of the new buffer at any
                // time now that we've completed the new copy.
                dmaNxt = pendingDmaNxt;
            }
        }
    }
}

