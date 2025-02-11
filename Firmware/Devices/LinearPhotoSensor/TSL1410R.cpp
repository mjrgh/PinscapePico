// Pinscape Pico - TAOS TSL1410R linear imaging sensor
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implementation uses a Pico PIO state machine to generate the
// shift-register clock signal, the Pico's on-board ADC to sample the
// analog pixel signal, and DMA to transfer samples from the ADC to the
// frame buffers.  This code is adapted from our driver for the Toshiba
// TCD1103, which has a similar analog-output shift register interface.

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>
#include <hardware/adc.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
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
#include "Watchdog.h"
#include "TSL1410R.h"
#include "TSL1410R_CLK.pio.h"
#include "TSL1410R_SI.pio.h"

// global singleton
std::unique_ptr<TSL1410R> tsl1410r;

// Configure from JSON data
//
// tsl1410r: {
//    si: <gpio>,            // GPIO port number of SI (Serial Input) - wire to pins 2, 3, and 9 on the sensor
//    clk: <gpio>,           // GPIO port number CLK (Serial Clock) - wire to pins 4 and 9 on the sensor
//    so: <gpio>,            // GPIO port number SO (Serial Output) - wire to pins 6 and 12 on the sensor; must be ADC-capable (GP26-29)
// }
//
// tsl1412s: {
//    // same as above
// }
//
// The key ("tsl1410r" or "tsl1412s") determines which sensor variant we
// configure.  The two sensors are identical at the interface level,
// except for their pixel file sizes: TSL1410R has 1280 pixels, TSL1412S
// has 1536 pixels.
//
// The SO pin must be connected to an ADC-capable GPIO (GP26-29).  The
// other sensor pins (SI and CLK) can be connected to any GPIOs.  Refer
// to the data sheet for the wiring diagram - follow the schematic for
// the "Serial" configuration in Figure 9.  A 0.1uF ceramic capacitor
// (not shown in Figure 9) should be connected between VDD and GND,
// close to the sensor.
//
// We can only handle one sensor of either type at a time, since the
// interface requires exclusive control over the Pico's on-board ADC.
//
void TSL1410R::Configure(JSONParser &json)
{
    // check for both tsl1410r and tsl1412s keys
    const auto *var1410 = json.Get("tsl1410r");
    const auto *var1412 = json.Get("tsl1412s");
    if (!var1410->IsUndefined() && !var1412->IsUndefined())
    {
        Log(LOG_ERROR, "tsl14xx: only one of 'tsl1410r' or 'tsl1412s' can be defined; "
            "only one sensor of either can be configured\n");
        return;
    }

    // we only have one (or zero) variant; check which one we're defining
    int nPixels = 0;
    const JSONParser::Value *var = nullptr;
    const char *key = nullptr;
    const char *variantName = nullptr;
    if (!var1410->IsUndefined())
    {
        nPixels = 1280;
        var = var1410;
        key = "tsl1410r";
        variantName = "TSL1410R";
    }
    else if (!var1412->IsUndefined())
    {
        nPixels = 1536;
        var = var1412;
        key = "tsl1412s";
        variantName = "TSL1412S";
    }
    else
    {
        // neither variant is defined
        return;
    }

    // get the pin assignments
    int si = var->Get("si")->Int(-1);
    int clk = var->Get("clk")->Int(-1);
    int so = var->Get("so")->Int(-1);
    
    // validate GPIOs
    if (!IsValidGP(si) || !IsValidGP(clk) || !IsValidGP(so))
    {
        Log(LOG_ERROR, "%s: one or more invalid/undefined GPIO pins (si, clk, so)\n", key);
        return;
    }
    if (!PicoADC::IsValidADCGPIO(so))
    {
        Log(LOG_ERROR, "%s: 'so' must be assigned to an ADC-capable GPIO port (GP26-29)\n", key);
        return;
    }
    
    // claim the GPIOs in exclusive mode
    if (!gpioManager.Claim(Format("%s (SI)", variantName), si)
        || !gpioManager.Claim(Format("%s (CLK)", variantName), clk)
        || !gpioManager.Claim(Format("%s (SO)", variantName), so))
        return;
    
    // create the singleton
    tsl1410r.reset(new TSL1410R(nPixels, variantName, si, clk, so));
    
    // initialize
    if (!tsl1410r->Init())
        tsl1410r.reset();
}

// construction
TSL1410R::TSL1410R(int nPixels, const char *variantName, int gpSI, int gpCLK, int gpSO) :
    nativePixCount(nPixels), variantName(variantName), gpSI(gpSI), gpCLK(gpCLK), gpSO(gpSO)
{
}

// initliaze - called after construction on successful configuration
bool TSL1410R::Init()
{
    // configure the GPIO outputs
    auto InitOut = [this](int gp)
    {
        // initialize, set up as an output, initially set to logic low/0
        gpio_init(gp);
        gpio_put(gp, false);
        gpio_set_dir(gp, GPIO_OUT);

        // Set high drive strength, so that the logic signal edges
        // are as fast as we can make them
        gpio_set_drive_strength(gp, GPIO_DRIVE_STRENGTH_12MA);
    };
    InitOut(gpSI);
    InitOut(gpCLK);

    // claim the on-board ADC
    if (!adcManager.ClaimPicoADC("TSL1410R"))
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
    // longer than we'd like.  The TSL1410R has 1280 pixels per frame,
    // so 125MHz/3 clocking would give us a frame capture time of
    // 2.95ms.  That's just a little slower than we'd like for plunger
    // motion capture.  125MHz/2 yields 1.97ms, which exceeds our target
    // minimum frame rate of about 400 fps (2.5ms)
    //
    // Note that the only possible ADC clock dividers are 1, 2, 3, and
    // 4, due to hardware constraints.
    clock_configure(
        clk_adc, 0,
        CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        SYS_CLK_KHZ * KHZ, SYS_CLK_KHZ/adcClockDiv * KHZ);

    // initialize the ADC without our SO pin GPIO input
    adc_init();
    adc_gpio_init(gpSO);
    adc_select_input(gpSO - 26);

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
            Log(LOG_ERROR, "%s: insufficient PIO resources\n", variantName);
            return false;
        }
        return true;
    };
    if (!ClaimPIO(pioSI, &TSL1410R_SI_program)
        || !ClaimPIO(pioCLK, &TSL1410R_CLK_program))
        return false;

    // Configure DMA for copying to the stable buffer.  Configure this for
    // 32-bit copies into the stable buffer, with the source to be specified
    // later (since it depends on which double-buffer is ready to read at
    // the time of each copy).
    if ((dmaCopy = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "%s: insufficient DMA channels\n", variantName), false;

    auto dmaConf = dma_channel_get_default_config(dmaCopy);
    channel_config_set_read_increment(&dmaConf, true);
    channel_config_set_write_increment(&dmaConf, true);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_32);
    dma_channel_configure(dmaCopy, &dmaConf, stablePix.pix, nullptr, nativePixCount/4, false);

    // Allocate the DMA channels for the ADC transfer loop
    if ((dmaA = dma_claim_unused_channel(false)) < 0
        || (dmaB = dma_claim_unused_channel(false)) < 0
        || (dmaC = dma_claim_unused_channel(false)) < 0)
        return Log(LOG_ERROR, "%s: insufficient DMA channels\n", variantName), false;

    // Set up our DMA completion interrupt handler on channel C.  Each
    // time channel C completes, we've completed one frame, so we get
    // one interrupt per frame.  The interrupt handler advances all of
    // the pulse timers into the next frame, and resets the chained DMA
    // channels so that we continue looping A -> C -> B -> C -> A.
    //
    // Note that we don't need any special priority for this interrupt
    // handler.  We can afford quite high latencies, because all of the
    // time-critical work is handled off-CPU by the DMA and PIO
    // peripherals.  We program the peripherals a frame in advance, so
    // when a frame ends and the interrupt fires, we have one full frame
    // worth of time before we have to respond - about 2ms.  Interrupt
    // latencies on the Pico are typically in the microseconds, so the
    // only risk of missing the IRQ is if interrupts are disabled for an
    // extended period (such as during a flash memory erase).
    irq_add_shared_handler(DMA_IRQ_0, &TSL1410R::SIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dmaC, true);

    // Start the DMA loop
    StartDMALoop();

    // set up our console command handler
    CommandConsole::AddCommand(
        "tsl1410r", "tsl1410r functions",
        "tsl1410r [options]\n"
        "options:\n"
        "  -s, --status    show status\n"
        "  -t, --timing    show detailed timing information (for debugging)\n",
        &Command_main);

    // success
    Log(LOG_CONFIG, "%s configured; SI=GP%d, CLK=GP%d, SO=GP%d, DMA (A=%d, B=%d, C=%d, copy=%d), PIO(SI %d.%d@%u, CLK %d.%d@%u)\n",
        variantName,
        gpSI, gpCLK, gpSO,
        dmaA, dmaB, dmaC, dmaCopy,
        pio_get_index(pioSI.pio), pioSI.sm, pioSI.ofs,
        pio_get_index(pioCLK.pio), pioCLK.sm, pioCLK.ofs);
    return true;
}

// reset the DMA loop - stops everything and starts from the top
void TSL1410R::RestartDMALoop()
{
    // stop the ADC and clear its FIFO
    adc_run(false);
    adc_fifo_drain();

    // stop the PIOs
    pio_sm_set_enabled(pioCLK.pio, pioCLK.sm, false);
    pio_sm_set_enabled(pioSI.pio, pioSI.sm, false);

    // clear the PIO FIFOs
    pio_sm_clear_fifos(pioCLK.pio, pioCLK.sm);
    pio_sm_clear_fifos(pioSI.pio, pioSI.sm);

    // drive clock and SI low
    gpio_put(gpCLK, false);
    gpio_put(gpSI, false);

    // reset the zero point on the frame timers
    siTime = 0;
    frameEndTime = 0;

    // start the DMA loop
    StartDMALoop();
}

// start the DMA loop
void TSL1410R::StartDMALoop()
{
    // Configure the three DMA channels for ADC capture.  One transfers
    // ADC -> pixBuf[0], the second ADC -> pixBuf[1], and the third
    // ADC -> dummy.  ADC transfers are in 8-bit bytes and trigger on
    // the ADC DREQ.  Channels A and B chain to channel C; channel C
    // chains to A or B, swapped on each write to maintain the double-
    // buffer swapping.
    auto dmaConf = dma_channel_get_default_config(dmaA);
    channel_config_set_read_increment(&dmaConf, false);
    channel_config_set_write_increment(&dmaConf, true);
    channel_config_set_transfer_data_size(&dmaConf, DMA_SIZE_8);
    channel_config_set_dreq(&dmaConf, DREQ_ADC);
    channel_config_set_chain_to(&dmaConf, dmaC);
    dma_channel_configure(dmaA, &dmaConf, pixBuf[0].pix, &adc_hw->fifo, nativePixCount, false);

    // Don't allow B to chain to C yet.  We can't allow the loop to
    // complete until A's completion interrupt is serviced, at which point
    // A will be re-armed and ready to accept an incoming chain_to.  If
    // IRQ service is delayed, A's write pointer will be at the end of its
    // buffer, so it would plow into unowned memory if we allowed the
    // chain_to trigger.
    channel_config_set_chain_to(&dmaConf, dmaB);
    dma_channel_configure(dmaB, &dmaConf, pixBuf[1].pix, &adc_hw->fifo, nativePixCount, false);

    // Channel C is a dummy transfer, so turn off auto-increment on the
    // write and simply keep overwriting a single memory byte.  Set it
    // up initially to chain to B, since we'll start with A->C->B, but
    // we'll swap this on each frame completion to maintain double
    // buffering.  The transfer size is variable, to be determined in
    // AdvanceCounters(), since it has to match the inter-frame time.
    channel_config_set_write_increment(&dmaConf, false);
    channel_config_set_chain_to(&dmaConf, dmaB);
    dma_channel_configure(dmaC, &dmaConf, &dmaCBuf, &adc_hw->fifo, 1, false);

    // save the dmaC configuration, so that we can re-apply it with
    // chain_to and any necessary size changes on each interrupt
    dmaConfC = dmaConf;

    // Configure the CLK PIO state machine.  This generates the pixel
    // clock signal to the sensor, which drives the pixel transfer.
    // Each CLK cycle clocks out one pixel on the sensor's analog out.
    pio_sm_set_enabled(pioCLK.pio, pioCLK.sm, false);    
    auto piocfg = TSL1410R_CLK_program_get_default_config(pioCLK.ofs);
    sm_config_set_sideset_pins(&piocfg, gpCLK);                   // one sideset pins: CLK
    sm_config_set_clkdiv_int_frac(&piocfg, 1, 0);                 // use the system clock with no division
    sm_config_set_out_shift(&piocfg, true, true, 32);             // right shifts, autopull, autopull threshold = 32 bits
    pio_sm_init(pioCLK.pio, pioCLK.sm, pioCLK.ofs, &piocfg);      // initialize with the configuration and starting program offset
    pio_sm_set_consecutive_pindirs(pioCLK.pio, pioCLK.sm, gpCLK, 1, true);
    pio_gpio_init(pioCLK.pio, gpCLK);

    // Pre-load the CLK PIO's ISR with (adcCycleTicks-16)/2-2 - see the PIO program comments
    pio_sm_put_blocking(pioCLK.pio, pioCLK.sm, (adcCycleTicks - 16)/2 - 2);
    pio_sm_exec(pioCLK.pio, pioCLK.sm, pio_encode_out(pio_isr, 32));   // OUT ISR, 32   ; move 32 bits from OSR to ISR

    // Configure the SI PIO state machine.  This generates the Serial
    // Input pulse that starts a new frame.
    pio_sm_set_enabled(pioSI.pio, pioSI.sm, false);    
    piocfg = TSL1410R_SI_program_get_default_config(pioSI.ofs);
    sm_config_set_sideset_pins(&piocfg, gpSI);              // one sideset pins: SI
    sm_config_set_clkdiv_int_frac(&piocfg, 1, 0);           // use the system clock with no division
    sm_config_set_out_shift(&piocfg, true, true, 32);       // right shifts, autopull, autopull threshold = 32 bits
    pio_sm_init(pioSI.pio, pioSI.sm, pioSI.ofs, &piocfg);   // initialize with the configuration and starting program offset
    pio_sm_set_consecutive_pindirs(pioSI.pio, pioSI.sm, gpSI, 1, true);
    pio_gpio_init(pioSI.pio, gpSI);

    // Pre-load ISR with the SI pulse duration, which is fixed at (ISR+2)*8ns
    pio_sm_put_blocking(pioSI.pio, pioSI.sm, siPulseTicks - 2);
    pio_sm_exec(pioSI.pio, pioSI.sm, pio_encode_out(pio_isr, 32));     // OUT ISR, 32   ; move 32 bits from OSR to ISR

    // Set up pulse times for the first two frames.  We always have the
    // working frame and the next frame set up, so that the timers are
    // already running when the asynchronous writer crosses the boundary
    // from the working frame to the next frame.  The boundary crossing
    // will trigger the DMA channel C interrupt, which will set up the
    // next frame after the new working frame, keeping the one-frame
    // work-ahead going indefinitely.
    dmaCNextChainTo = dmaA;
    setupCounter = 0;
    AdvanceTimers();
    AdvanceTimers();

    // Set the wait time between ADC cycles.  adc_set_clkdiv(N) sets
    // a cycle time of N+1 ADC clock ticks = (N+1)*adcClockDiv CPU
    // clock ticks.  So to get M CPU clock ticks, call adc_set_clkdiv
    // with M/adcClockDiv - 1.
    adc_set_clkdiv(adcCycleTicks/adcClockDiv - 1);

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
// reference.  We can't perform these operations atomically, so we at
// least have to try to do them as close together as possible.
void __not_in_flash_func(TSL1410R::StartPeripherals)()
{
    // Make masks of all of the state machines on PIO 0 and 1, so that
    // we can fire off all of the PIO SMs simultaneously if they're on
    // the same PIO, and within one clock if they're on separate PIOs.
    uint32_t pioMask0 = 0;
    uint32_t pioMask1 = 0;
    (pioSI.pio == pio0 ? pioMask0 : pioMask1) |= (1 << pioSI.sm);
    (pioCLK.pio == pio0 ? pioMask0 : pioMask1) |= (1 << pioCLK.sm);

    // The peripherals must be started as close to simultaneously as
    // possible, so that they're all synchronized to the same starting
    // point on the system clock.  Disable interrupts for the rest of
    // our work here, to keep the code path timing as deterministic as
    // possible.
    IRQDisabler irqd;

    // Enable the PIOs
    pio_enable_sm_mask_in_sync(pio0, pioMask0);
    pio_enable_sm_mask_in_sync(pio1, pioMask1);

    // Start the ADC in continuous mode
    adc_run(true);

    // this is the start of the first frame
    frameStartTime = time_us_64();
}

// Advance the timers into the next frame
//
// This is called from the DMA channel C completion interrupt, so put
// it in RAM (not flash) for faster execution.
void __not_in_flash_func(TSL1410R::AdvanceTimers)()
{
    // Round a time in system ticks to an ADC cycle boundary
    static const auto RoundToADCCycle = [](uint32_t n) { return (n + adcCycleTicks - 1)/adcCycleTicks * adcCycleTicks; };

    // Figure the minimum time for the inter-frame period.  The data
    // sheet says that we have to leave at least 20us after the end of
    // one transfer cycle before starting the next, to allow time for
    // the pixel charges to transfer from the integration capacitor to
    // the sampling capacitor.  20us is 2500 system clock ticks.  We
    // also need one extra ADC cycle, since the shift register needs one
    // extra clock after all of the pixels have been output to trigger
    // the charge transfer.  Add a couple of extra clock CLK cycles
    // (equal to ADC cycles) just to be sure that we're not on the
    // borderline of the chip's minimum requirements.
    const uint32_t minInterFrameTime = RoundToADCCycle(2500) + 5*adcCycleTicks;

    // Figure the new frame duration.  This is the greater of the frame
    // transfer time or the desired integration time.  The frame can't
    // be shorter than the transfer time, since we move the pixels at a
    // fixed speed that we can't increase.  But it can be longer, if
    // extra integration time is desired.  Any extra time beyond the
    // transfer time gets inserted as padding between the end of the
    // transfer and the SI signal that starts the new frame.  The result
    // must always be ADC-cycle aligned.
    uint32_t integrationTicks = integrationTime_us * 125;
    uint32_t minFrameDuration = adcCycleTicks*nativePixCount + minInterFrameTime;
    uint32_t frameDuration = RoundToADCCycle(std::max(integrationTicks, minFrameDuration));

    // Figure the true inter-frame time, as the excess of the total
    // frame time over the pixel transfer time.
    uint32_t actualInterFrameTime = frameDuration - adcCycleTicks*nativePixCount;

    // Figure the next frame ending time on our absolute clock
    frameEndTime += frameDuration;

    // Figure the timing of the next SI pulse.  Per the data sheet, the
    // pulse should straddle rising edge of the CLK signal, with the
    // rising edge of SI at least 20ns before the rising edge of CLK.
    // We start each ADC cycle with CLK high, so set up the SI pulse
    // to straddle the start of the frame.
    uint64_t nextSIEnd = frameEndTime + siPulseEndOfs;
    uint64_t nextSIStart = nextSIEnd - siPulseTicks;

    // write the SI pulse time to the PIO and update the internal counter
    uint32_t siCounter = static_cast<uint32_t>(nextSIStart - siTime - 2);
    pio_sm_put_blocking(pioSI.pio, pioSI.sm, siCounter);
    siTime = nextSIEnd;

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
uint64_t TSL1410R::GetPixTimestamp() const
{
    // Return true if the current non-writer buffer is newer than
    // the stable copy we got on the last GetPix call.
    IRQDisabler irqd;
    return std::max(pixBuf[writerBuf ^ 1].timestamp, stablePix.timestamp);
}

// get the current image buffer and timestamp
void TSL1410R::GetPix(const uint8_t* &pix, uint64_t &timestamp)
{
    // check for a DMA loop stall
    if (dmaLoopStalled)
    {
        Log(LOG_DEBUG, "%s DMA loop stall detected; restarting\n", variantName);
        dmaLoopStalled = false;
        RestartDMALoop();
    }

    // Set up the DMA transfer.  Transfer from the channel that the
    // writer ISN'T currently writing into.
    //
    // There's a race condition here, but it's completely benign.  The
    // race is between our copy setup and the asynchronous ADC transfer
    // buffer writer: it's possible that the writer could finish with
    // the current half of the double-buffer, and start writing into the
    // buffer we select as the read buffer, between the time we set up
    // our DMA and the time our DMA finishes.  But even if that happens,
    // the worst damage is that the first pixel of the old frame is
    // replaced with the corresponding pixel from the new frame.  The
    // DMA transfer to the stable buffer takes about 5us overall, and in
    // that time, the ADC can take 3 samples (we program the ADC for
    // 1.56us per cycle).  But the DMA transfer to the stable buffer
    // will already be well past the first three pixels by the time even
    // one ADC transfer occurs.  In the worst case, the first ADC
    // transfer hits the old buffer just as the stable-buffer transfer
    // starts, overwriting the first pixel, so we get a single pixel of
    // "tearing" across the frames.  Even that overstates the problem,
    // though, because the nature of the mechanical setup (for plunger
    // sensor use) requires that the pixels at either end of the sensor
    // are beyond the range of motion of the plunger, meaning that
    // they're always either dark or bright - so even with the
    // "tearing", that single pixel will *still* be the same across
    // frames anyway.  So whether we get that one pixel from the new
    // frame or the old frame barely matters.
    //
    // If you wish to adapt this code for an application where the
    // potential tearing across frames could actually be a problem,
    // there's a fairly simple solution.  After the DMA transfer
    // completes, check to see if 'writerBuf' has changed from the value
    // it had on entry.  If so, simply go back and repeat the transfer
    // with the new 'writerBuf' value.  This is guaranteed to converge
    // after a maximum of two iterations because the stable-buffer copy
    // only takes about 5us, and the ADC frame transfer takes at least
    // 2ms, leaving no chance that a second ADC frame transfer could
    // complete during the second 5us iteration.  Given the harmlessness
    // of a torn frame in Pinscape's plunger-sensor application, I don't
    // think it's worth the overhead of the extra checks, so I'm leaving
    // that out of this implementation, but it would be easy to add for
    // an application that requires it.
    {
        // disable IRQs and get the current write buffer
        IRQDisabler irqd;
        auto &buf = pixBuf[writerBuf ^ 1];

        // start the transfer
        dma_channel_set_write_addr(dmaCopy, stablePix.pix, false);
        dma_channel_transfer_from_buffer_now(dmaCopy, buf.pix, nativePixCount/4);

        // set the timestamp
        stablePix.timestamp = buf.timestamp;
    }

    // Wait for the DMA to complete.  Note that we do this outside of
    // the IRQ-disabled code, because IRQs won't have any effect on the
    // DMA transfer once started.
    dma_channel_wait_for_finish_blocking(dmaCopy);

    // pass back the stable buffer to the caller
    pix = stablePix.pix;
    timestamp = stablePix.timestamp;
}

// DMA transfer completion interrupt transfer
void __not_in_flash_func(TSL1410R::IRQ)()
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
uint32_t TSL1410R::GetAvgScanTime() const
{
    return static_cast<uint32_t>(totalFrameTime / nFrames);
}

// set the integration time
void TSL1410R::SetIntegrationTime(uint32_t us)
{
    // zero -> default -> frame integration time
    integrationTime_us = us;
}

// Console command handler
void TSL1410R::Command_main(const ConsoleCommandContext *c)
{
    // make sure we have at least one option
    if (c->argc <= 1)
        return c->Usage();

    // process options
    auto *t = tsl1410r.get();
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--status") == 0)
        {
            // status
            c->Printf(
                "TSL1410R status:\n"
                "  Device variant:        %s\n"
                "  Pixels:                %u\n"
                "  Min integration time:  %lu us (actual int. time is greater of frame xfer time or this minimum)\n"
                "  Frames captured:       %llu\n"
                "  Avg frame xfer time:   %.2lf ms\n"
                "  Interrupts:            %llu\n"
                "  Avg time in ISR:       %llu us\n",
                t->variantName,
                t->nativePixCount,
                t->integrationTime_us,
                t->nFrames,
                static_cast<double>(t->totalFrameTime)/static_cast<double>(t->nFrames)/1000.0,
                t->nIrq, t->totalIrqTime / t->nIrq);
        }
        else if (strcmp(a, "-t") == 0 || strcmp(a, "--timing") == 0)
        {
            // detailed timing information
            c->Printf(
                "TSL1410R internal frame timing information:\n"
                "  Frame end time:  %llu\n"
                "  SI time:         %llu (frame end %+d)\n"
                "  SI pulse ticks:  %lu\n"
                "  SI pulse offset: %lu\n"
                "  Next xfer cnt:   %lu\n"
                "  Write channel:   %c\n"
                "  DMA C chain_to:  %c\n",
                t->frameEndTime,
                t->siTime, static_cast<int>(t->siTime - t->frameEndTime),
                t->siPulseTicks,
                t->siPulseEndOfs,
                t->dmaCNextTransferCount,
                t->writerBuf == 0 ? 'A' : 'B',
                t->dmaCNextChainTo == t->dmaA ? 'B' : 'A' /* note: live is always opposite of next */);
        }
        else if (strcmp(a, "--pulse-ticks") == 0)
        {
            // note - this option is for debugging/testing
            if (i + 1 >= c->argc)
                return c->Printf("missing --pulse-ticks argument\n");
            
            t->siPulseTicks = atoi(c->argv[++i]);
            t->dmaLoopStalled = true;
        }
        else if (strcmp(a, "--pulse-offset") == 0)
        {
            // note - this option is for debugging/testing
            if (i + 1 >= c->argc)
                return c->Printf("missing --pulse-offset argument\n");

            t->siPulseEndOfs = atoi(c->argv[++i]);
            t->dmaLoopStalled = true;
        }
        else if (strcmp(a, "--reset") == 0)
        {
            // Note - this option is for debugging/testing, to exercise
            // the DMA loop restart code.  You can also trigger the DMA
            // loop restart more organically using 'devtest --block-irq N',
            // where N >= about 10; that'll actually stall the DMA loop
            // by blocking interrupts for more than a full cycle, so the
            // stall detection code should notice and restart the loop
            // at the next GetPix() call.

            // DMA aborts are synchronous, so extend the watchdog in case
            // it takes a few cycles
            WatchdogTemporaryExtender wde(2500);

            // stop the ADC, so that we don't trigger any further DMA activity
            adc_run(false);
            adc_fifo_drain();
            
            // Abort any DMA in progress.  Our DMA channels are chained
            // in a loop, so it's possible that a channel could be
            // triggered after we abort it.  So do all of the aborts twice;
            // the first time we hit a running channel will ensure that we
            // don't fire its chain_to, and the second time will pick up
            // any chain_to that fired while we were on the first loop.
            // Triggers occur at the ADC rate, which is about 2us per
            // sample, so we should be able to cancel all three channels
            // twice well within tha time: thus, if a re-trigger does
            // sneak in on us, it can only happen once, so twice through
            // the loop should definitely catch it.
            //
            // Per errata note RP2040-E13, we could get a completion IRQ
            // on C during the abort, so we need to disable the IRQ first.
            dma_channel_set_irq0_enabled(t->dmaC, false);
            for (int i = 0 ; i < 2 ; ++i)
            {
                dma_channel_abort(t->dmaA);
                dma_channel_abort(t->dmaB);
                dma_channel_abort(t->dmaC);
            }

            // clear any IRQ signal on C and re-enable IRQs
            dma_channel_acknowledge_irq0(t->dmaC);
            dma_channel_set_irq0_enabled(t->dmaC, true);

            // set the loop stall flag so that we restart at the next pixel read
            t->dmaLoopStalled = true;
        }
        else
        {
            // invalid option
            return c->Printf("tsl1410r: unknown option \"%s\"\n", a);
        }
    }
}
