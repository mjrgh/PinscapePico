// Pinscape Pico - Toshiba TCD1103 linear CCD photo sensor, 1x1500 pixels
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implementation uses a Pico PIO state machine to generate the
// shift-register clock signal, the Pico's on-board ADC to sample the analog
// pixel signal, and DMA to transfer samples from the ADC to the frame
// buffers.
//
// The TCD1103 has a number of requirements for its logic signal timing,
// particularly for the sequence of signals that initiate a new frame.  It
// requires the host to generate a continuously running master clock signal,
// which also serves as the shift register clock to transfer pixels to the
// analog output.  See the data sheet for details.
//
//
// Implementation Notes
//
// To maximum the frame rate, we capture frames continuously.  Every time
// we finish transferring one frame, we start transferring the next.  The
// sensor is designed with this procedure in mind: it provides an internal
// shift register that serves as the source of the pixel data transfer, with
// an electronic shutter function that transfers a snapshot of the live
// sensor pixels over to the shift register at a moment of the host's
// choosing.  The host can transfer data from the shift register over a
// serial connection at its leisure.  Since the shift register contains a
// snapshot of the entire live pixel array at a moment in time, we always
// get a still snapshot (with no "tearing") regardless of the amount of time
// the transfer takes.  Meanwhile, while the serial data transfer is taking
// place, the live pixels are integrating incident photons for the *next*
// frame snapshot.
//
// To manage the continuous data transfer, we use double buffering.  We use
// DMA to transfer the incoming pixel data from the new frame into one
// buffer, while the other buffer, containing the completed transfer from
// the previous frame, is available for clients to read from.  The TCD1103
// presents the serial pixel output as an analog voltage level proportional
// to the brightness of the current pixel being clocked from the shift
// register, so we use the Pico's on-board ADC to read the incoming pixels.
// The on-board ADC is fast and is integrated with the Pico's DMA
// controller, so we can use it to perform the transfers asynchronously,
// without any CPU involvement in moving the bytes.
//
// We use three DMA channels to manage the double buffering scheme.
// Channels A and B (arbitrary labels we use for identification purposes)
// correspond to the two halves of the double buffer.  Channel C is a "link"
// between the two buffers.  We use the Pico DMA controller's CHAIN_TO
// feature to arrange the DMA channels in a circular pattern:
//
//    A -> C -> B -> C -> A ...
//
// This pattern repeats indefinitely.  Channel C serves two purposes: first,
// it's the intermediary in the CHAIN_TO links from A to B and from B to A.
// Second, it withdraws samples from the DMA FIFO in the period between two
// frames, when we must send the TCD1103 a series of signals to operate the
// electronic shutter, to make a snapshot of the live pixels in the sensor's
// shift register and start clocking them out on its serial output.  This is
// a short period of about 5us during which the sensor isn't generating any
// pixel output.  We still have to read the ADC output during this period,
// though, because we want to keep the Pico's ADC running continuously to
// minimize down time between frames.  So channel C simply reads from the
// ADC and writes to a dummy location in memory, whose only purpose is to
// sink the unused dummy samples from the ADC read between frames.
//
// The cycle A -> C -> B -> C -> A can't be programmed statically in the DMA
// channels, since C's LINK_TO has to change from A to B to A on alternating
// frames.  Even if we could program the LINK_TO chain statically, we'd
// still have to reprogram the A and B output buffers on each cycle, since
// the channel writer pointers auto-increment to the end of their respective
// buffers on each frame.  So we need to reprogram several DMA settings on
// each frame.  To handle this, we program channel C to fire a DMA0 IRQ on
// completion, and we do the necessary channel reprogramming in the
// interrupt handler.  The reprogramming work is extremely lightweight, so
// the IRQ handler only takes about 3us on each cycle.  The IRQ handler
// doesn't add to the overall cycle time, since the DMA transfer into the
// next buffer proceeds asynchronously even while the IRQ handler is
// running.  Because of this, we must keep the DMA channels programmed one
// frame ahead at all times, so that the DMA runs non-stop while we're
// programming the next frame in the IRQ.
//
// To TCD1103 requires three input signals, referred to in the data sheet as
// FM (master clock(, SH (shift gate), and ICG (integration clear gate) to
// operate the electronic shutter and serial data transfer.  The data sheet
// conceives of all three signals as periodic clocks, but that reflects a
// dated assumption about how the host is implemented, namely that the host
// uses a simple hardware clock generator for each signal.  A more modern
// view would conceive of SH and ICG as GPIO output signals from a
// microcontroller.  The data sheet incorrectly claims that SH and ICG must
// be periodic, but empirically, that makes no difference to the sensor.
// It only cares about the timing of the signal edges relative to the other
// signals (and even some of those requirements are suspect: a lot of this
// data sheet was clearly copied-and-pasted from the data sheets for
// earlier Toshiba linear image sensors, and I suspect that some of the
// information is simply out of date, never properly revised for the design
// changes from the chip's predecessors.)
//
// We generate all three signals (FM, SH, and ICG) from PIO programs, one
// PIO program per signal.  The timing of the signals is coordinated in
// the DMA C IRQ handler.
//
// Since the FM, SH, and ICG signals are generated via PIOs, they're
// synchronized with the Pico's 125 MHz system clock.  The FM signal drives
// the sensor's shift register output, and we sample the shift register via
// the Pico ADC, so we must synchronize the ADC cycle with the FM signal.
// The Pico's ADC unfortunately doens't have an external trigger, so there's
// no way to feed the FM signal into the ADC to trigger the cycle directly.
// The closest thing the Pico ADC has to an external trigger is a clock
// trigger: we can program the ADC to run periodically on its master clock.
// So we can synchronize the ADC cycle and the FM cycle by tying them both
// to the same clock signal.  But there's another obstacle!  The ADC
// normally runs on the 48 MHz USB PLL clock, whereas the PIOs run on the
// 125 MHz system clock.  There's no way to reconfigure the PIOs to run on a
// different clock, but happily, the ADC can be moved to alternate clocks,
// including the system clock.  So the way to synchronize the FM and ADC
// cycles is to reprogram the ADC to run on the system clock, and then
// arrange the FM cycle so that two FM cycles exactly equal one ADC cycle in
// terms of system clocks.  With both units on the same clock, this is
// straightforward, because the ADC continuous-mode cycle is fixed at 96
// clock ticks.  This conveniently divides by 4 - convenient because the
// FM signal requires four phases (two square-wave cycles) per ADC cycle.

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>
#include <hardware/adc.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "CommandConsole.h"
#include "PIOHelper.h"
#include "GPIOManager.h"
#include "ADCManager.h"
#include "Devices/ADC/PicoADC.h"
#include "TCD1103.h"
#include "TCD1103_FMpos.pio.h"
#include "TCD1103_FMneg.pio.h"
#include "TCD1103_SHpos.pio.h"
#include "TCD1103_SHneg.pio.h"
#include "TCD1103_ICGpos.pio.h"
#include "TCD1103_ICGneg.pio.h"

// global singleton
std::unique_ptr<TCD1103> tcd1103;


// ADC clock divider.  This is the divider we apply to the system
// clock to derive the ADC clock.  The ADC cycle time is always 96
// ADC clocks.
static const int adcClockDiv = 2;
static const uint32_t adcCycleTicks = 96 * adcClockDiv;

// Round a time in system ticks to an ADC cycle boundary
static inline uint32_t RoundToADCCycle(uint32_t n)
{
    return (n + adcCycleTicks - 1)/adcCycleTicks * adcCycleTicks;
}

// Configure from JSON data
//
// tcd1103: {
//    fm: <gpio>,            // GPIO port number of FM pin (master clock)
//    icg: <gpio>,           // GPIO port number of ICG pin (integration clear gate)
//    sh: <gpio>,            // GPIO port number SH pin (shift gate)
//    os: <gpio>,            // GPIO port number of OS pin (analog pixel output) - must be ADC-capable on the Pico (GP26-29)
//    invertedLogic: true,   // true (default) -> 74HC04 is installed between Pico and logic gates
//                           // per data sheet recommendation; false -> direct logic connections
// }
//
void TCD1103::Configure(JSONParser &json)
{
    if (const auto *var = json.Get("tcd1103") ; !var->IsUndefined())
    {
        // get the pin assignments
        int fm = var->Get("fm")->Int(-1);
        int icg = var->Get("icg")->Int(-1);
        int sh = var->Get("sh")->Int(-1);
        int os = var->Get("os")->Int(-1);

        // validate GPIOs
        if (!IsValidGP(fm) || !IsValidGP(icg) || !IsValidGP(sh) || !IsValidGP(os))
        {
            Log(LOG_ERROR, "tcd1103: one or more invalid/undefined GPIO pins (fm, icg, sh, os)\n");
            return;
        }
        if (!PicoADC::IsValidADCGPIO(os))
        {
            Log(LOG_ERROR, "tcd1103: 'os' must be assigned to an ADC-capable GPIO port (GP26-29)\n");
            return;
        }

        // claim the GPIOs in exclusive mode
        if (!gpioManager.Claim("TCD1103 (FM)", fm)
            || !gpioManager.Claim("TCD1103 (ICG)", icg)
            || !gpioManager.Claim("TCD1103 (SH)", sh)
            || !gpioManager.Claim("TCD1103 (OS)", os))
            return;

        // get the logic polarity
        bool invertedLogic = var->Get("invertedLogic")->Bool(true);

        // create the singleton
        tcd1103.reset(new TCD1103(invertedLogic, fm, icg, sh, os));

        // initialize
        if (!tcd1103->Init())
            tcd1103.reset();
    }
}

// construction
TCD1103::TCD1103(bool invertedLogic, int gpFM, int gpICG, int gpSH, int gpOS) :
    invertedLogic(invertedLogic), gpFM(gpFM), gpICG(gpICG), gpSH(gpSH), gpOS(gpOS)
{
    // set the high/low GPIO levels according to the logic polarity
    logicLow = invertedLogic ? true : false;
    logicHigh = !logicLow;
}

// initliaze - called after construction on successful configuration
bool TCD1103::Init()
{
    // configure the GPIO outputs
    auto InitOut = [this](int gp, bool state)
    {
        // initialize, set up as an output
        gpio_init(gp);
        gpio_put(gp, state);
        gpio_set_dir(gp, GPIO_OUT);

        // If we're not in inverted-logic mode, set the output drive to
        // maximum.  The TCD1103 data sheet mentions that its logic
        // input gates have relatively high capacitance, which requires
        // high-current drive for fast switching speeds to meet the
        // chip's timing requirements, especially on the master clock
        // signal.  The whole point of the inverted logic mode is that
        // the data sheet recommends using a 74HC04 inverter to buffer
        // the inputs, since that chip provides higher drive strength
        // than most microcontroller GPIO outputs can.  If we're using
        // inverted logic mode, we'll assume that a 74HC04 is indeed in
        // place, so we don't need extra drive strength from the GPIOs.
        // For positive logic, we'll assume a direct connection to the
        // sensor logic inputs, so increase drive strength accordingly.
        if (!invertedLogic)
            gpio_set_drive_strength(gp, GPIO_DRIVE_STRENGTH_12MA);
    };
    InitOut(gpFM, logicLow);
    InitOut(gpICG, logicHigh);
    InitOut(gpSH, logicLow);

    // claim the on-board ADC
    if (!adcManager.ClaimPicoADC("TCD1103"))
        return false;

    // Reconfigure the ADC clock.  Use the system clock as the source,
    // so that we can exactly sync ADC cycle to the PIO program cycle.
    // The PIO also takes its clock base from the system clock, and
    // that's not configurable, so the ADC clock is the free variable
    // here.
    //
    // The normal ADC clock source is 48 MHz, while the system clock is
    // 125 MHz.  The RP2040 datasheet says that the ADC requires a 48
    // MHz clock, but empirically it still works if you overclock it.
    // If we divide the system clock by 2, we get 62.5 MHz, which is
    // still considerably faster than the normal 48 MHz ADC clock but
    // it's at least close.  We could also divide by 3 to get it closer
    // still to 48 MHz, but that makes the sensor scan time a little
    // longer than we'd like.  The TCD1103 has 1546 pixels per frame, so
    // 125MHz/3 clocking would give us a frame capture time of 3.6ms.
    // That's just a little slower than we'd like for plunger motion
    // capture.  125MHz/2 yields 2.3ms, which is just about perfect.
    //
    // The possible ADC clock dividers are 1, 2, 3, and 4 (that's a
    // hardware-level constraint; the hardware clock divider register
    // for the ADC is two bits wide).
    clock_configure(
        clk_adc, 0,
        CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        SYS_CLK_KHZ * KHZ, SYS_CLK_KHZ/adcClockDiv * KHZ);

    // initialize the ADC without our OS pin GPIO input
    adc_init();
    adc_gpio_init(gpOS);
    adc_select_input(gpOS - 26);

    // Set zero wait time between ADC samples, for exactly 96*divider
    // clocks per sample.  This is critically important to our timing,
    // since we have to synchronize the FM clock signal that we generate
    // with the ADC cycle in order to present the ADC input with exactly
    // one pixel per sampling cycle.
    adc_set_clkdiv(0);

    // Configure the ADC for continuous capture, with the FIFO enabled
    // for DMA capture at a sample depth of 1, in 8-bit mode
    adc_fifo_setup(
        true,   // enable FIFO
        true,   // enable DMA DREQ
        1,      // assert DREQ and IRQ when at least 1 sample is in the FIFO
        false,  // disable error bit in samples (it's encoded in the high bit; omit it for 8-bit mode)
        true);  // right-shift samples for 8-bit output to FIFO

    // Set up the PIO programs
    auto TryClaimPIO = [this](PIO pio, PIOProg &pp, const pio_program_t *program)
    {
        // make sure we can add our program to the PIO
        if (!pio_can_add_program(pio, program))
            return false;
        
        // try claiming a state machine on this PIO
        pp.sm = pio_claim_unused_sm(pio, false);
        if (pp.sm < 0)
            return false;
        
        // success - add the program
        pp.ofs = pio_add_program(pio, program);
        pp.pio = pio;
        return true;
    };
    auto ClaimPIO = [this, TryClaimPIO](PIOProg &pp, const pio_program_t *program)
    {
        if (!TryClaimPIO(pio0, pp, program) && !TryClaimPIO(pio1, pp, program))
        {
            Log(LOG_ERROR, "tcd1103: insufficient PIO resources\n");
            return false;
        }
        return true;
    };
    if (!ClaimPIO(pioFM, invertedLogic ?  &TCD1103_FMneg_program : &TCD1103_FMpos_program)
        || !ClaimPIO(pioSH, invertedLogic ?  &TCD1103_SHneg_program : &TCD1103_SHpos_program)
        || !ClaimPIO(pioICG, invertedLogic ?  &TCD1103_ICGneg_program : &TCD1103_ICGpos_program))
        return false;

    // Configure DMA for copying to the stable buffer.  Configure this for
    // 32-bit copies into the stable buffer, with the source to be specified
    // later (since it depends on which double-buffer is ready to read at
    // the time of each copy).
    if ((dmaCopy = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "tcd1103: insufficient DMA channels\n"), false;
    auto dmaConf = dma_channel_get_default_config(dmaCopy);
    channel_config_set_read_increment(&dmaConf, true);
    channel_config_set_write_increment(&dmaConf, true);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_32);
    dma_channel_configure(dmaCopy, &dmaConf, stablePix.pix, nullptr, _countof(stablePix.pix)/4, false);

    // Claim the three DMA channels for ADC transfer
    if ((dmaA = dma_claim_unused_channel(false)) < 0
        || (dmaB = dma_claim_unused_channel(false)) < 0
        || (dmaC = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "tcd1103: insufficient DMA channels\n"), false;

    // Set up our DMA completion interrupt handler on channel C.  Each
    // time channel C completes, we've completed one frame, so we get
    // one interrupt per frame.  The interrupt handler advances all of
    // the pulse timers into the next frame, and resets the chained DMA
    // channels so that we continue looping A -> C -> B -> C -> A.
    //
    // Note that we don't need any special priority for this interrupt
    // handler.  We can actually afford quite high latencies, because
    // all of the time-critical work is handled off-CPU by the DMA and
    // PIO peripherals.  We program all of the asynchronous peripherals
    // a full frame in advance, so when a frame ends and the interrupt
    // fires, we have an entire frame worth of time before we have to
    // respond - about 2.4ms.  For a Pico, that is nearly an eternity.
    irq_add_shared_handler(DMA_IRQ_0, &TCD1103::SIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dmaC, true);

    // Start the DMA loop
    StartDMALoop();

    // set up our console command handler
    CommandConsole::AddCommand(
        "tcd1103", "tcd1103 functions",
        "tcd1103 [options]\n"
        "options:\n"
        "  -s, --status    show status\n"
        "  -t, --timing    show detailed timing information (for debugging)\n",
        &Command_main);

    // success
    Log(LOG_CONFIG, "tcd1103 configured; FM=GP%d, ICG=GP%d, SH=GP%d, OS=GP%d, %s logic, DMA (A=%d, B=%d, C=%d, copy=%d), PIO(FM %d.%d@%u, SH %d.%d@%u, ICG %d.%d@%u)\n",
        gpFM, gpICG, gpSH, gpOS, invertedLogic ? "inverted" : "positive",
        dmaA, dmaB, dmaC, dmaCopy,
        pio_get_index(pioFM.pio), pioFM.sm, pioFM.ofs,
        pio_get_index(pioSH.pio), pioSH.sm, pioSH.ofs,
        pio_get_index(pioICG.pio), pioICG.sm, pioICG.ofs);
    return true;
}

// reset the DMA loop - stops everything and starts from the top
void TCD1103::RestartDMALoop()
{
    // stop the ADC and clear its FIFO
    adc_run(false);
    adc_fifo_drain();

    // stop the PIOs
    pio_sm_set_enabled(pioFM.pio, pioFM.sm, false);
    pio_sm_set_enabled(pioSH.pio, pioSH.sm, false);
    pio_sm_set_enabled(pioICG.pio, pioICG.sm, false);

    // clear the PIO FIFOs
    pio_sm_clear_fifos(pioFM.pio, pioFM.sm);
    pio_sm_clear_fifos(pioSH.pio, pioSH.sm);
    pio_sm_clear_fifos(pioICG.pio, pioICG.sm);

    // reset the zero point for the frame timers
    shTime = 0;
    icgTime = 0;
    frameEndTime = 0;
    
    // start the DMA loop
    StartDMALoop();
}

// start the DMA loop
void TCD1103::StartDMALoop()
{
    // Configure ADC capture channels.  One transfers ADC -> pixBuf[0],
    // the second ADC -> pixBuf[1], and the third ADC -> dummy.  ADC
    // transfers are in 8-bit bytes and trigger on the ADC DREQ.  Channels
    // A and B chain to channel C; channel C chains to A or B, swapped on
    // each write to maintain the double- buffer swapping.
    auto dmaConf = dma_channel_get_default_config(dmaA);
    channel_config_set_read_increment(&dmaConf, false);
    channel_config_set_write_increment(&dmaConf, true);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_8);
    channel_config_set_dreq(&dmaConf, DREQ_ADC);
    channel_config_set_chain_to(&dmaConf, dmaC);
    dma_channel_configure(dmaA, &dmaConf, pixBuf[0].pix, &adc_hw->fifo, nativePixCount, false);

    // DON'T set B's chain-to yet (by setting it to point to itself).  We
    // have everything set up now for A->C->B, but A won't be ready again
    // until its completion routine fires.  If the IRQ service is delayed,
    // that would set up a crash condition, since C would come straight
    // back to B, whose write pointer would be at the end of its buffer
    // and poised to plow into unowned memory.  So we can't allow B to
    // chain to anything yet.
    channel_config_set_chain_to(&dmaConf, dmaB);
    dma_channel_configure(dmaB, &dmaConf, pixBuf[1].pix, &adc_hw->fifo, nativePixCount, false);

    // Channel C is just for pacing; it doesn't transfer any live pixels,
    // but simply runs the pixel clock for the period we need to wait
    // between frames.  We don't need to store the ADC readings during
    // this inter-frame period, so we can turn off auto-increment on C's
    // write pointer and just keep overwriting the same memory byte.
    //
    // The DMA chain loop goes A->C->B->C->A ad infinitum, so the initial
    // setting is C->B.  We'll swap this to C->A after A finishes, at
    // which point control will move to C then B.  C's transfer size is
    // variable, since it has to match the length of the SH/ICG signal
    // period between frames.
    channel_config_set_write_increment(&dmaConf, false);
    channel_config_set_chain_to(&dmaConf, dmaA);
    dma_channel_configure(dmaC, &dmaConf, &dmaCBuf, &adc_hw->fifo, 1, false);

    // save the dmaC configuration, so that we can re-apply it with
    // chain_to and any necessary size changes on each interrupt
    dmaConfC = dmaConf;

    // Configure the FM PIO state machine.  This generates the FM clock
    // signal to the sensor, which drives the pixel transfer.
    pio_sm_set_enabled(pioFM.pio, pioFM.sm, false);    
    auto piocfg = invertedLogic ? TCD1103_FMneg_program_get_default_config(pioFM.ofs) : TCD1103_FMpos_program_get_default_config(pioFM.ofs);
    sm_config_set_sideset_pins(&piocfg, gpFM);        // one sideset pins: FM
    sm_config_set_clkdiv_int_frac(&piocfg, 96*adcClockDiv/4, 0);    // PIO clock divider (see the .pio file for details)
    pio_sm_init(pioFM.pio, pioFM.sm, pioFM.ofs, &piocfg);   // initialize with the configuration and starting program offset
    pio_sm_set_consecutive_pindirs(pioFM.pio, pioFM.sm, gpFM, 1, true);
    pio_gpio_init(pioFM.pio, gpFM);

    // Configure the SH PIO state machine.  This generates the Shift
    // Gate (electronic shutter) signals that set the integration time.
    pio_sm_set_enabled(pioSH.pio, pioSH.sm, false);    
    piocfg = invertedLogic ? TCD1103_SHneg_program_get_default_config(pioSH.ofs) : TCD1103_SHpos_program_get_default_config(pioSH.ofs);
    sm_config_set_sideset_pins(&piocfg, gpSH);              // one sideset pins: SH
    sm_config_set_clkdiv_int_frac(&piocfg, 1, 0);           // use the system clock with no division
    sm_config_set_out_shift(&piocfg, true, true, 32);       // right shifts, autopull, autopull threshold = 32 bits
    pio_sm_init(pioSH.pio, pioSH.sm, pioSH.ofs, &piocfg);   // initialize with the configuration and starting program offset
    pio_sm_set_consecutive_pindirs(pioSH.pio, pioSH.sm, gpSH, 1, true);
    pio_gpio_init(pioSH.pio, gpSH);

    // Set ISR for the SH pulse duration, which is fixed at (ISR+2)*8ns
    pio_sm_put_blocking(pioSH.pio, pioSH.sm, shPulseTicks - 2);
    pio_sm_exec(pioSH.pio, pioSH.sm, pio_encode_out(pio_isr, 32));     // OUT ISR, 32   ; move 32 bits from OSR to ISR

    // Configure the ICG PIO state machine.  This generates the Integration
    // Clear Gate signal that starts each new frame.
    pio_sm_set_enabled(pioICG.pio, pioICG.sm, false);    
    piocfg = invertedLogic ? TCD1103_ICGneg_program_get_default_config(pioICG.ofs) : TCD1103_ICGpos_program_get_default_config(pioICG.ofs);
    sm_config_set_sideset_pins(&piocfg, gpICG);               // one sideset pins: ICG
    sm_config_set_clkdiv_int_frac(&piocfg, 1, 0);             // use the system clock with no division
    sm_config_set_out_shift(&piocfg, true, true, 32);         // right shifts, autopull, autopull threshold = 32 bits
    pio_sm_init(pioICG.pio, pioICG.sm, pioICG.ofs, &piocfg);  // initialize with the configuration and starting program offset
    pio_sm_set_consecutive_pindirs(pioICG.pio, pioICG.sm, gpICG, 1, true);
    pio_gpio_init(pioICG.pio, gpICG);

    // Set ISR for the ICG pulse duration, which is fixed at (ISR+2)*8ns
    pio_sm_put_blocking(pioICG.pio, pioICG.sm, icgPulseTicks - 2);
    pio_sm_exec(pioICG.pio, pioICG.sm, pio_encode_out(pio_isr, 32));     // OUT ISR, 32   ; move 32 bits from OSR to ISR

    // Set up pulse times for the first two frames.  We always have the
    // working frame and the next frame set up, so that the timers are
    // already running when the asynchronous writer crosses the boundary
    // from the working frame to the next frame.  The boundary crossing
    // will trigger the DMA channel C interrupt, which will set up the
    // next frame after the new working frame, keeping the one-frame
    // work-ahead going indefinitely.
    dmaCNextChainTo = dmaA;
    AdvanceTimers();
    AdvanceTimers();

    // Trigger DMA channel A, so that it's ready to go when first ADC
    // sample completes.  The transfer is gated on the ADC DREQ, so the
    // transfer won't actually start until the ADC starts up, which
    // we'll do shortly.
    writerBuf = 0;
    dma_channel_start(dmaA);

    // the DMA loop is now running
    dmaLoopStalled = false;

    // Start the state machines and ADC transfers.
    StartPeripherals();
}

// Start the peripherals.  We do this with RAM-resident code, because
// flash code can take several cycles per instruction to load from the
// flash chip.  RAM code is more likely to run in one cycle per opcode.
// It's critical to minimize cycles in the PIO/ADC enabling steps
// because we want all of those peripherals to start from the same time
// reference. h We can't perform these operations atomically, so we at
// least have to try to do them as close together as possible.
//
// This is called from the DMA channel C completion interrupt, so put
// it in RAM (not flash) for faster execution.
void __not_in_flash_func(TCD1103::StartPeripherals)()
{
    // Make masks of all of the state machines on PIO 0 and 1, so that
    // we can fire off all of the PIO SMs simultaneously if they're on
    // the same PIO, and within one clock if they're on separate PIOs.
    uint32_t pioMask0 = 0;
    uint32_t pioMask1 = 0;
    (pioFM.pio == pio0 ? pioMask0 : pioMask1) |= (1 << pioFM.sm);
    (pioSH.pio == pio0 ? pioMask0 : pioMask1) |= (1 << pioSH.sm);
    (pioICG.pio == pio0 ? pioMask0 : pioMask1) |= (1 << pioICG.sm);

    // The peripherals must be started as close to simultaneously as
    // possible, so that they're all synchronized to the same starting
    // point on the system clock.  Disable interrupts for the rest of
    // our work here, to keep the code path timing as deterministic as
    // possible.
    IRQDisabler irqd;

    // Enable the PIOs, synchronizing their clock dividers
    pio_enable_sm_mask_in_sync(pio0, pioMask0);
    pio_enable_sm_mask_in_sync(pio1, pioMask1);

    // Start the ADC in continuous mode
    adc_run(true);

    // this is the start of the first frame
    frameStartTime = time_us_64();
}

// Advance the timers into the next frame
void __not_in_flash_func(TCD1103::AdvanceTimers)()
{
    // Figure the minimum time of the SH/ICG inter-frame period.  This
    // is the time it takes to generate the ICG signal (5us = 625 system
    // clocks), plus some padding at the end to ensure that the rising
    // edge of ICG occurs during the high part of the FM cycle (1/4 of
    // an FM cycle, which is 1/2 of an ADC cycle).  This must then be
    // rounded up to a multiple of the ADC cycle time.
    const uint32_t minInterFrameTime = RoundToADCCycle(625 + adcCycleTicks/8);

    // Figure the new frame duration.  This is the greater of the frame
    // transfer time or the desired integration time.  The frame can't
    // be shorter than the transfer time, since we move the pixels at a
    // fixed speed that we can't increase.  But it can be longer, if
    // extra integration time is desired.  Any extra time beyond the
    // transfer time gets inserted as padding between the end of the
    // transfer and the new frame's SH/ICG signal sequence.  The result
    // must always be ADC-cycle aligned.
    uint32_t integrationTicks = integrationTime_us * 125;
    uint32_t minFrameDuration = adcCycleTicks*nativePixCount + minInterFrameTime;
    uint32_t frameDuration = RoundToADCCycle(std::max(integrationTicks, minFrameDuration));

    // Figure the true inter-frame time, as the excess of the total
    // frame time over the pixel transfer time.
    uint32_t actualInterFrameTime = frameDuration - adcCycleTicks*nativePixCount;

    // Figure the next frame ending time on our absolute clock
    frameEndTime += frameDuration;

    // Figure the timing of the next ICG pulse: it ends at 1/4 of an FM
    // cycle before the end of the frame, and starts 625 ticks before
    // that.
    uint64_t nextICGEnd = frameEndTime - adcCycleTicks/8;
    uint64_t nextICGStart = nextICGEnd - icgPulseTicks;

    // write the ICG pulse time to the PIO and update the internal counter
    pio_sm_put_blocking(pioICG.pio, pioICG.sm, nextICGStart - icgTime - 2);
    icgTime = nextICGEnd;

    // Figure the timing of the frame-ending SH pulse: it starts 13
    // clock ticks (108ns) after the ICG pulse starts.
    uint64_t nextSHStart = nextICGStart + 13;
    uint64_t nextSHEnd = nextSHStart + shPulseTicks;

    // Figure the timing of the exposure-limiting SH pulse that occurs
    // mid-frame.  If the integration time is zero, it means that we use
    // the full transfer time as the integration time, so there is no
    // extra SH pulse.  We also don't need an extra pulse if the
    // integration time is explicitly longer than the transfer time,
    // because the SH pulse from the transfer is already pushed back to
    // the desired limit.
    if (integrationTicks != 0 && integrationTicks < minFrameDuration)
    {
        // work backwards from the frame-ending SH pulse - the
        // integration (exposure) time is the time between the
        // frame-ending SH pulse and the next earlier pulse
        uint64_t intSHEnd = nextSHStart - integrationTicks;
        uint64_t intSHStart = intSHEnd - shPulseTicks;

        // If this doesn't start before the end of the last actually
        // scheduled SH pulse, schedule it.  Don't schedule a pulse
        // unless the timing leaves at least 1us (128 clock ticks) after
        // the previous pulse and before the next pulse, to satisfy the
        // chip's pulse timing requirements.  If the pulse is too close
        // to the next/previous pulse, the chip might miss it - and the
        // difference in exposure time would be trivial anyway if the
        // new pulse is so close to an existing pulse.
        if (intSHStart > shTime + 128 && intSHEnd + 128 < nextSHStart)
        {
            // write the next delay time, and update the internal counter
            pio_sm_put_blocking(pioSH.pio, pioSH.sm, intSHStart - shTime - 2);
            shTime = intSHEnd;
        }
    }

    // write the next SH pulse
    pio_sm_put_blocking(pioSH.pio, pioSH.sm, nextSHStart - shTime - 2);
    shTime = nextSHEnd;

    // reset the buffer pointer for the next chain-to channel
    dma_channel_set_write_addr(dmaCNextChainTo, pixBuf[dmaCNextChainTo == dmaA ? 0 : 1].pix, false);

    // Configure DMA channel C:
    //
    // - Chain_to: The current writer channel (A or B) will chain to C
    // when it finishes, so we need to set C to chain to the other
    // channel when it's done.
    //
    // - Size: Set the transfer count to the value we figured out on the
    // previous iteration, which set the timing for the upcoming frame.
    //
    // Don't trigger the channel - it will be triggered by the chain_to
    // from the current async writer channel (A or B) when the current
    // pixel transfer finishes.
    channel_config_set_chain_to(&dmaConfC, dmaCNextChainTo);
    dma_channel_configure(dmaC, &dmaConfC, &dmaCBuf, &adc_hw->fifo, dmaCNextTransferCount, false);

    // The next-chain-to channel is now armed and ready, so the OTHER
    // channel is now allowed to chain into C.  The next-chain-to channel
    // ISN'T allowed to chain anywhere until we set up the next round.
    int otherChannel = dmaCNextChainTo == dmaA ? dmaB : dmaA;
    SetDMAChainTo(otherChannel, dmaC);
    DisableDMAChainTo(dmaCNextChainTo);

    // Check for a DMA loop stall.  If the other channel isn't busy, the
    // DMA loop might be broken, since it finished before we had a chance
    // to re-arm it.  There's a slight chance that we got there just in
    // nick of time, just before it finished, and that it just triggered
    // channel C; or even that C triggered its next target.  So one of
    // the three channels must be running, and we have to check in the
    // order of triggering in case a change occurs between checks.
    if (!dma_channel_is_busy(otherChannel)
        && !dma_channel_is_busy(dmaC)
        && !dma_channel_is_busy(dmaCNextChainTo))
        dmaLoopStalled = true;

    // swap the chain-to destination
    dmaCNextChainTo = (dmaCNextChainTo == dmaA) ? dmaB : dmaA;

    // Figure the DMA C transfer size for the next frame.  This is the
    // excess of the total frame time over the pixel transfer time,
    // divided by clock ticks per ADC cycle.
    dmaCNextTransferCount = actualInterFrameTime / adcCycleTicks;
}

// Get the latest image timestamp
uint64_t TCD1103::GetPixTimestamp() const
{
    // return the newer of the stable buffer or the inactive async writer buffer
    IRQDisabler irqd;
    return std::max(pixBuf[writerBuf ^ 1].timestamp, stablePix.timestamp);
}

// get the current image buffer and timestamp
void TCD1103::GetPix(const uint8_t* &pix, uint64_t &timestamp)
{
    // check for a DMA loop stall
    if (dmaLoopStalled)
    {
        Log(LOG_DEBUG, "TCD1103 DMA loop stall detected; restarting\n");
        dmaLoopStalled = false;
        RestartDMALoop();
    }
    
    // Set up the DMA transfer.  Transfer from the channel that the
    // writer ISN'T currently writing into.
    //
    // There's a race condition here, but thanks to the TCD1103's pixel
    // array structure, it's a race that we can safely ignore.  The race
    // is between our copy setup and the asynchronous ADC transfer
    // buffer writer: it's possible that the writer could finish with
    // the current half of the double-buffer, and start writing into the
    // buffer we select as the read buffer, between the time we set up
    // our DMA and the time our DMA finishes.  But even if that happens,
    // we'll still have a stable result from the copy, thanks to the
    // TCD1103 structure.  Here's why.  Our DMA copy takes approximately
    // 5us (including setup time).  If the asynchronous writer happens
    // to switch buffers during that 5us, it will have a chance to do
    // 5us worth of writing into the same buffer that we're reading
    // from.  The ADC is programmed for 1.56us sampling cycles, so 5us
    // is enough time for the async writer to scribble over the first 3
    // samples in our buffer.  This is where the special structure of
    // the TCD1103 pixel array comes to the rescue: the first 16 pixel
    // array elements are "dummies" that aren't electrically connected
    // to live pixels.  They're meaningless samples that the client
    // ignores.  So those up-to-3 samples that might get scribbled over
    // in the worst-case race condition are going to be ignored anyway,
    // rendering the race harmless.  (And we have a considerable margin
    // of safety beyond this; with 16 dummy samples, we could tolerate
    // up to 24us of overlap in the transfers, which is a quite
    // leisurely interval on a 125MHz M0+.)
    //
    // This analysis depends upon the DMA transfer actually occurring in
    // 5us of real time, so we do have to turn off interrupts while
    // setting up the DMA transfer, to be sure that a random IRQ handler
    // from another subsystem can't steal any of our 5us during the DMA
    // setup.  Once the DMA is initiated, it's unaffected by interrupts,
    // so we only have to block interrupts long enough to figure out the
    // DMA source and pull the DMA trigger.
    //
    // One small caveat: The one bit of timing that isn't perfectly
    // deterministic here is the asynchronous DMA operation itself.  In
    // a vacuum, that time is predictable, but it's possible that some
    // other, higher-priority and bus-saturating DMA operation could be
    // taking place simultaneously, in which case our DMA transfer could
    // be locked out for an extended interval and thus take much longer
    // than 5us of real time to complete.  But if that were to happen,
    // this hypothetical other DMA transfer would also preempt the ADC
    // transfer for the same interval, making it late by the same amount
    // of real time.  So I think our original analysis still holds in
    // terms of the RELATIVE timing of our transfer and the asynchronous
    // ADC transfer.
    {
        // disable IRQs and get the current write buffer
        IRQDisabler irqd;
        auto &buf = pixBuf[writerBuf ^ 1];

        // start the transfer
        dma_channel_set_write_addr(dmaCopy, stablePix.pix, false);
        dma_channel_transfer_from_buffer_now(dmaCopy, buf.pix, sizeof(buf.pix)/4);

        // set the timestamp
        stablePix.timestamp = buf.timestamp;
    }

    // wait for the DMA to complete
    dma_channel_wait_for_finish_blocking(dmaCopy);

    // pass back the stable buffer to the caller
    pix = stablePix.pix;
    timestamp = stablePix.timestamp;
}

// DMA transfer completion interrupt transfer
void __not_in_flash_func(TCD1103::IRQ)()
{
    // This is a shared interrupt handler, so check to make sure our channel
    // is actually in an interrupt state (the interrupt might have come from
    // some unrelated channel that's using the same IRQ)
    if (dma_channel_get_irq0_status(dmaC))
    {
        // set the completion time for the current frame
        uint64_t t0 = time_us_64();
        pixBuf[writerBuf].timestamp = t0;

        // Swap the writer to the other half of the double buffer
        writerBuf ^= 1;

        // clear the interrupt status flag in the channel
        dma_channel_acknowledge_irq0(dmaC);

        // collect frame timing statistics
        totalFrameTime += t0 - frameStartTime;
        nFrames += 1;

        // this is the start time for the next frame
        frameStartTime = t0;

        // advance all of the timers into the next frame
        AdvanceTimers();

        // collect interrupt run-time statistics
        nIrq += 1;
        totalIrqTime += time_us_64() - t0;
    }
}

// get the average scan time (microseconds)
uint32_t TCD1103::GetAvgScanTime() const
{
    return static_cast<uint32_t>(totalFrameTime / nFrames);
}

// set the integration time
void TCD1103::SetIntegrationTime(uint32_t us)
{
    // zero -> default -> frame integration time
    integrationTime_us = us;
}

// Console command handler
void TCD1103::Command_main(const ConsoleCommandContext *c)
{
    // make sure we have at least one option
    if (c->argc <= 1)
        return c->Usage();

    // process options
    auto *t = tcd1103.get();
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--status") == 0)
        {
            // status
            c->Printf(
                "TCD1103 status:\n"
                "  Integration time:     %lu us%s\n"
                "  Frames captured:      %llu\n"
                "  Avg frame xfer time:  %.2lf ms\n"
                "  Interrupts:           %llu\n"
                "  Avg time in ISR:      %llu us\n",
                t->integrationTime_us, t->integrationTime_us == 0 ? " (0 => integrate over frame transfer time)" : "",
                t->nFrames,
                static_cast<double>(t->totalFrameTime)/static_cast<double>(t->nFrames)/1000.0,
                t->nIrq, t->totalIrqTime / t->nIrq);
        }
        else if (strcmp(a, "-t") == 0 || strcmp(a, "--timing") == 0)
        {
            // detailed timing information
            c->Printf(
                "TCD1103 internal frame timing information:\n"
                "  Frame end time:  %llu\n"
                "  SH time:         %llu (frame end %+d)\n"
                "  ICG time:        %llu (frame end %+d)\n"
                "  Next xfer cnt:   %lu\n"
                "  Write channel:   %c\n"
                "  DMA C chain_to:  %c\n",
                t->frameEndTime,
                t->shTime, static_cast<int>(t->shTime - t->frameEndTime),
                t->icgTime, static_cast<int>(t->icgTime - t->frameEndTime),
                t->dmaCNextTransferCount,
                t->writerBuf == 0 ? 'A' : 'B',
                t->dmaCNextChainTo == t->dmaA ? 'B' : 'A' /* note: live is always opposite of next */);
        }
        else
        {
            // invalid syntax
            return c->Printf("tcd1103: unknown option \"%s\"\n", a);
        }
    }
}
