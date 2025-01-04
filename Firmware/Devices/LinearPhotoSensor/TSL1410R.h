// Pinscape Pico - TAOS TSL1410R and TSL1412S linear photo sensors
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The TSL1410R is the original Pinscape plunger sensor.  It's no longer
// being manufactured, but Pinscape Pico supports it for the sake of
// people (such as myself) with existing installations.  It's really a
// shame they stopped making this chip, since it's so perfect for this
// application.
//
// TSL1410R and TSL1412S are essentially the same devices except for
// the pixel count:
//
//   TSL1410R  - 1280 x 1 pixels
//   TSL1412S  - 1536 x 1 pixels
//
// This code handles both variations.  The user selects which variation
// to use at run-time via the JSON configuration settings.
//
// The TSL1410R/12S are large-format linear imaging sensors.  The
// physical dimensions of these sensors are an uncannily good match to
// the dimensions of the standard modern pinball plunger.  The
// TSL1410R's pixel window is 3.2" long, and a modern pinball plunger
// assembly has a total end-to-end travel distance of about 3.15".  The
// TSL1412S is slightly larger, which works just as well; the sensor
// window extends a little further beyond the plunger travel range, so
// it leaves a bigger margin at either end, which is actually a plus
// because it makes it that much easier to get the alignment right when
// installing it.
//
// By positioning the sensor very close to the plunger, with the sensor
// window aligned along the plunger's long axis, and shining a light at
// the sensor from the opposite side of the plunger, we can use the
// shadow cast by the plunger as an indication of the plunger position.
// This is a wonderfully simple mechanical setup, without the need for
// any lenses or optics.
// 
// The sensor has a pixel pitch of 400 DPI, which translates 1:1 to the
// resolving power of our location sensing, since no optics are
// involved.  In practice, the shadow edge is a little blurry (again, no
// optics), so we can only reliably figure the plunger position to
// within about 3 or 4 pixels, which reduces the effective positional
// resolving power to around 100 DPI.  That's still quite excellent for
// this application, since the dot pitch on a pin cab-scale video
// monitor is also around 100 DPI.  That lets the on-screen plunger
// graphics track the mechanical plunger position with single-pixel
// granularity, which makes the animation smooth and natural.
//
// These sensors are also quite fast.  They can clock out pixels at up
// to 8 MHz, allowing for frame rates as high as 6250 fps on the 1410R
// or 5200 fps on the 1412S.  In practice, the microcontroller's ADC is
// the limiting factor on the frame rate, since every pixel clocked out
// has to be sampled on the ADC, and few ADCs can maintain 8 MSPS.  The
// Pico's ADC can be overclocked to about 1.3 MSPS, but we run it at a
// more leisurely 650 KSPS, which limits our frame rate to about 500
// fps.  That's still plenty fast for our plunger application, which
// requires about 400 fps or better for accurate speed sensing.
//
// The electrical interface to this sensor is also delightfully simple
// and microcontroller-friendly.  The sensor has two logic signal
// inputs, an analog output, and power and ground inputs.  It's happy
// with 3.3V power and draws 40mA max, so it can be powered from the
// Pico's on-board regulator.  Physically, the sensor actually has 6
// logic input pins and 2 analog output pins, but the extra pins can be
// wired together in such a way that we only need the three MCU GPIO
// connections mentioned (plus power):
//
//  - Connect the SI GPIO from the Pico to sensor pins 2, 3, and 10
//  - Connect the CLK GPIO from the Pico to sensor pins 4 and 9
//  - Connect the SO GPIO from the Pico to sensor pins 6 and 12
//  - Connect sensor pins 7 and 8 together
//
// See the TSL1410R data sheet for a more detailed wiring diagram,
// referring to the SERIAL configuration.
// 

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <hardware/sync.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"

// forward/external declarations
class JSONParser;
class ConsoleCommandContext;
class TSL1410R;

// global singleton
extern std::unique_ptr<TSL1410R> tsl1410r;

// TSL1410R interface
class TSL1410R
{
public:
    // configure from JSON data
    static void Configure(JSONParser &json);

    // construction
    TSL1410R(int nPixels, const char *variantName, int gpSI, int gpCLK, int gpSO);

    // sensor variant name ("TSL1410R", "TSL1412S")
    const char *GetVariantName() const { return variantName; }

    // get the number of pixels on the sensor
    int GetPixCount() const { return nativePixCount; }

    // Get the timestamp of the latest available image
    uint64_t GetPixTimestamp() const;

    // Get the current image buffer and its timestamp.  The buffer
    // remains valid and stable until the next GetPix() call.
    void GetPix(const uint8_t* &pix, uint64_t &timestamp);

    // figure the average scan time from the running totals
    uint32_t GetAvgScanTime() const;

    // Set the requeste integration time.  Zero sets the default time,
    // equal to the frame transfer time.
    void SetIntegrationTime(uint32_t us);
    
protected:
    // Initialize
    bool Init();

    // DMA transfer completion interrupt handler.  We force the IRQ
    // handler code into RAM (rather than flash) because it greatly
    // speeds up the interupt handler, reducing the latency it
    // imposes on the rest of the system.
    static void __not_in_flash_func(SIRQ)() { tsl1410r->IRQ(); }
    void IRQ();

    // GPIO ports
    int gpSI;       // SI - serial input; starts a new frame
    int gpCLK;      // CLK - pixel clock
    int gpSO;       // SO - serial output, as an analog voltage

    // DMA transfer channels.  We use three channels:
    //
    //   A: ADC -> buffer 0  (first buffer in double-buffering scheme)
    //   B: ADC -> buffer 1  (second double-buffer)
    //   C: ADC -> dummy
    //
    // Channel C reads and discards ADC data during the period between
    // frames, when we're generating the ICG/SH signals that start the
    // new frame.  During this time, the ADC is still running, so we need
    // to continue reading it to keep its FIFO clear, but the ADC samples
    // are meaningless because the TCD1103 isn't clocking out any pixels
    // during this period.  Channel C also serves as the bridge that
    // keeps the double-buffering scheme running continuously, using
    // channel chaining.  The chaining is set up like this:
    //
    //  A -> chain_to C
    //  C -> chain_to B
    //  B -> chain_to C
    //
    // On the DMA interrupt for C, we swap C's chain_to pointer to the
    // other buffer, so that the DMA will run continuously in a cycle:
    //
    // A -> C -> B -> C -> A -> C -> B -> C -> A ...
    //
    // The channel C interrupt occurs simultaneously with the chain_to
    // that starts the next A or B transfer.  The A/B transfer takes about
    // 2.4ms, so the interrupt handler has a comfortable time window to
    // swap C's chain_to.  The interrupt handler also calculates the pulse
    // times for the next frame and writes them to the PIO FIFOs, so that
    // the FIFOs always have the next queued up.
    int dmaA = -1;
    int dmaB = -1;
    int dmaC = -1;

    // dummy buffer for dmaC output
    uint32_t dmaCBuf;

    // DMA channel C configuration.  We need to reconfigure C at each
    // interrupt according to the next frame's integration time.
    // (We don't need the same information for A and B, since they keep
    // the same configuration throughout the session.)
    dma_channel_config dmaConfC;

    // DMA channel for transfers to a stable buffer.  Whenever the
    // client requests a frame snapshot, we copy from the inactive
    // buffer (the one that the writer isn't currently writing into)
    // into a private buffer that the writer can't access.  This
    // guarantees that the client has a stable copy for as long as it
    // needs it, so that it can perform long-running image processing on
    // the data.  We use this DMA to make the copy fast.  (Memory-to-
    // memory DMA is about 4x faster than memcpy() on Pico.)
    int dmaCopy = -1;

    // Start the peripherals running (PIOs, ADC)
    void StartPeripherals();

    // Advance signals into the next frame
    void AdvanceTimers();

    // Timing counters, for the clock signals and DMA transfers.  All of
    // the times are referenced to the Pico system clock, which drives
    // the PIOs and DAC cycles.  (We program the ADC to run from the
    // system clock, instead of the normal 48 MHz USB clock that the
    // default SDK configuration sets up.  This lets us keep the ADC
    // cycle perfectly synchronized with the PIOs.)  The zero point is
    // arbitrary, but happens to be the moment we start the PIOs and DAC
    // running, which we make as close to simultaneous as possible to
    // ensure that they start off in sync.  (As long as they start off
    // in sync, they'll stay in sync, since they're all referenced to
    // the same clock source.)
    //
    // A UINT64 is wide enough to hold about 4700 years of 125 MHz
    // system clock ticks, so the risk of overflow is relatively low.
    uint64_t siTime = 0;         // end time (falling edge) of last SI pulse
    uint64_t frameEndTime = 0;   // end time of last DMA transfer of last frame

    // SI pulse duration, in CPU clock ticks
    uint32_t siPulseTicks = 128;
    uint32_t siPulseEndOfs = 64;

    // ADC clock divider.  This is the divider we apply to the system
    // clock to derive the ADC clock.  The ADC sampling time consists
    // of a fixed 96 ADC clocks for the conversion, plus any amount of
    // extra padding (in ADC clock ticks) that we wish to add.
    //
    // We use 16 system clock ticks (128ns) of extra padding to allow
    // for the analog output "settling time" mentioned in the data
    // sheet.  This is the time between the rising CLK edge and the
    // sensor analog output stabilizing at the new pixel voltage.
    static const int adcClockDiv = 2;             // CPU clock divider to derive ADC clock from CPU clock
    static const uint32_t adcCyclePadding = 16/adcClockDiv;  // padding between samples, in ADC clocks
    static const uint32_t adcCycleFixed = 96;    // ADC hardware's fixed conversion time, in ADC clocks
    static const uint32_t adcCycleTicks = (adcCycleFixed + adcCyclePadding) * adcClockDiv;   // CPU clocks per ADC cycle

    // The SI pulse must always be programmed two frames ahead: the
    // frame we're currently working on, and the one that follows.
    // Whereas the DMA transfer can only be programmed for the current
    // frame.  (This is because we share channel C between the double
    // buffers, so it always has to be set up for the current frame.)
    // When we calculate times for the next frame, therefore, we'll
    // actually program the PIO for the frame-ahead SI pulse, but we can
    // only figure the next DMA setup, and must save the programming for
    // next time.  So we have to stash the pending DMA information
    // somewhere, which is here.
    uint32_t dmaCNextTransferCount = 0;
    int dmaCNextChainTo = 0;

    // DMA loop stall detected in interrupt handler
    volatile bool dmaLoopStalled = false;

    // start/restart the DMA loop
    void StartDMALoop();
    void RestartDMALoop();

    // PIO programs
    struct PIOProg
    {
        PIO pio = nullptr;  // PIO hardware unit where the program is loaded
        int sm = -1;        // state machine assigned
        uint ofs = 0;       // program load offset
    };
    PIOProg pioCLK;    // pixel clock
    PIOProg pioSI;     // SI pulse
    
    // Requested integration time, in microseconds.  This allows the
    // host to adjust the exposure time to compensate for lighting
    // conditions.  For the TSL1410R, the minimum possible integration
    // time equals the frame transfer time plus setup time.  The
    // integration time can be extended by adding padding between
    // frames.
    uint32_t integrationTime_us = 0;
    
    // Number of pixels in the native pixel array.  This depends on the
    // variation of the sensor selected: TSL1410R = 1280 pixels,
    // TSL1412S = 1536 pixels.
    uint32_t nativePixCount = 1280;

    // Variant name ("TSL141R", "TSL1412S")
    const char *variantName = "TSL1410R";

    // Pixel buffers.  We use two buffers, and the writer switches
    // destination buffers on each frame, so there's always a stable
    // buffer for a client to read from.
    struct PixBuf
    {
        // transfer completion timestamp
        uint64_t timestamp = 0;

        // Pixels, as 8-bit ADC readings.  We use the largest size of
        // all of the sensor variations - 1536 for the TSL1412S.  The
        // actual buffer size used depends on the sensor variation
        // configured.
        //
        // Align the buffer on a 4-byte boundary, so that we can use
        // 32-bit DMA transfers to copy from this buffer to a caller's
        // buffer.
        uint8_t __attribute__((aligned(4))) pix[1536];
    };
    PixBuf pixBuf[2];

    // Stable pixel buffer, for client access.  GetPix() makes a copy
    // of the current inactive buffer here.
    PixBuf stablePix;

    // buffer currently in use by writer
    volatile int writerBuf = 0;

    // setup counter, for debugging
    int setupCounter = 0;

    // scan time statistics
    uint64_t totalFrameTime = 0;
    uint64_t nFrames = 0;
    uint64_t frameStartTime = 0;

    // IRQ handler statistics
    uint64_t totalIrqTime = 0;
    uint64_t nIrq = 0;

    // console command handler
    static void Command_main(const ConsoleCommandContext *ctx);
};
