// Pinscape Pico - Toshiba TCD1103 linear CCD photo sensor, 1x1500 pixels
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The TCD1103 is a linear CCD array consisting of a single row of 1500
// CCD photo-sensor pixels.  It's essentially a digital camera sensor
// with all of the pixels arranged in a single row.  These sensors are
// designed primarily for bar-code and document scanning applications,
// but they can also serve as non-contact one-dimensional position
// sensors, by detecting the pixel location on the sensor corresponding
// to a visually identifiable feature of the moving object, such as the
// edge of the object (by detecting contrast between dark and lit areas)
// or a light source attached to the object.  Pinscape can use this
// device as a plunger position sensor by detecting the bright spot in
// the image produced by a reflective tip at the end of the plunger,
// with the plunger illuminated against a dark background.
//
// The TCD1103's electrical interface is similar to other sensors in
// this class.  It has an electronic shutter signal that transfers the
// current photosensor charges onto capacitors in an internal shift
// register, and a clock signal that moves the shift-register charges
// onto an external analog output line one by one.  The host reads the
// image by asserting the shutter signal, then driving the clock signal
// to transfer each pixel from the shift register onto the analog out.
// The host reads the analog out for each pixel via an ADC.
//
//
// Software interface
//
// The image scanner runs continuously and asynchronously, using DMA to
// conduct the pixel transfers without loading the CPU.  We use double
// buffering, so that at any given time, the hardware is writing into
// one buffer, while the client can read out of the other buffer.
// Internally, we use a third to construct a snapshot of the latest
// completed buffer for the client on request.  This gives the client a
// stable read buffer that it can use for as long as it wants, without
// worrying about the timing of the asynchronous writer.
//
// The pixel output signal from this sensor is an analog voltage level.
// It's inverted from the brightness: higher brightness is represented
// by lower voltage.  The dynamic range is about 1V, with a 1V floor.
// We use the Pico ADC in 8-bit mode, so dark pixels read at about 0xCC
// and light pixels read at about 0x55.  The exact dark level varies
// slightly in each frame, because it depends upon the integration time
// and other variables.  A per-frame reference reading for the current
// dark level can be obtained by taking an average of the physically
// light-shielded reference pixels at indices [16] to [28] in each frame
// buffer.
//
//
// Electrical connections
//
// The Toshiba data sheet provides a reference connection diagram, which
// includes some extra required parts, particularly a PNP driver
// transistor for the analog output (OS pin) and a logic inverter
// connected between the microcontroller and the TCD1103 logic signal
// input pins.  The data sheet doesn't specify values or part numbers
// for the most of the parts, so here's a verified-working parts list
// that I used in my testing setup:
//
//   - 3.3V power supply (from the Pico 3V3 output)
//   - 74HC04N hex inverter for the logic gate inputs (FM, SH, ICG)
//   - 0.1uF ceramic + 10uF electrolytic decoupling capacitors
//     (connect across GND - Vcc, physically as close as possible to
//     the TCD1103 power pins)
//   - BC212A PNP transistor, with:
//     - 150 ohm resistor on the base
//     - 150 ohm resistor between collector and GND
//     - 2.2K ohm resistor between emitter and Vcc
//
// The purpose of the 74HC04N inverter is to increase the drive strength
// of the input signals enough for the relatively high capacitances of
// the input gates on the sensor, particularly the shift gate (SH) at
// 150pF.  The Pico's native GPIO drive strength might be high enough
// that this buffering isn't required, but I haven't tested it that way;
// I've only tested it with an inverter as shown in the data sheet.  The
// software here can be configured either way, though.  When positive
// logic (no inverter) is selected, the software will automatically set
// the sensor signal GPIOs to maximum (12mA) drive strength, to deal
// with the the high gate capacitance as best we can.
//
// I've published an EAGLE design for a circuit board for this sensor
// that's compatible with the Pico (as well as the original KL25Z
// version of Pinscape), plus a 3D-printed mounting bracket.  The
// mounting bracket is designed specifically for using the sensor as a
// plunger position sensor in a virtual pinball cabinet.  See the
// Pinscape Build Guide for links to those resources and documentation
// on how to use them:
//
//   http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=tcd1103

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
class TCD1103;

// global singleton
extern std::unique_ptr<TCD1103> tcd1103;


// TCD1103 device interface
class TCD1103
{
public:
    // configure from JSON data
    static void Configure(JSONParser &json);

    // construction
    TCD1103(bool invertedLogic, int gpFM, int gpICG, int gpSH, int gpOS);

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
    // speeds up the interrupt handler, reducing the latency it
    // imposes on the rest of the system.
    static void __not_in_flash_func(SIRQ)() { tcd1103->IRQ(); }
    void IRQ();

    // Are we using inverted logic on the TCD1103 logic signals?  Set
    // this to true when using a 74HC04 or equivalent inverter between
    // the Pico and the TCD1103 logic inputs (which is what the chip's
    // data sheet recommends, to increase the logic input drive strength
    // to a level sufficient for the high-capacitance gates on the
    // TCD1103).
    bool invertedLogic;
    
    // GPIO ports
    int gpFM;       // FM - master clock (clocks pixels from shift register onto OS)
    int gpICG;      // ICG - integration clear gate (electronic shutter)
    int gpSH;       // SH - shift gate (moves live CCD pixels into shift register)
    int gpOS;       // OS - analog pixel output

    // Logic high/low levels, based on whether the TCD1103 logic signals
    // are wired directly or through an inverter.  The data sheet
    // recommends using a 74HC04 inverter for all logic inputs to ensure
    // sufficient drive strength for the relatively high-capacitance
    // loads that the chip's logic gates present.  When an inverter is
    // interposed, we have to invert the logic signals generated on the
    // Pico to cancel out the logic inversion from the 74HC04.
    //
    // These are expressed as bool, giving the values to use for
    // gpio_put() in Pico SDK when writing to a GPIO connected to one of
    // the TCD1103 logic inputs.
    bool logicLow = false;
    bool logicHigh = true;

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
    // the PIOs and DAC cycles.  (Note that the DAC normally uses the
    // USB 48 MHz clock, but we reprogram it to use the system PLL clock
    // instead, so that we can keep the DAC cycle exactly synchronized
    // with the signals we generate from the PIOs, which are always
    // referenced to the system PLL clock.)  The zero point is
    // arbitrary, but happens to be the moment we start the PIOs and DAC
    // running, which we make as close to simultaneous as possible to
    // ensure that they start off in sync.  (As long as they start off
    // in sync, they'll stay in sync, since they're all referenced to
    // the same clock source.)
    //
    // A UINT64 is wide enough to hold about 4700 years of 125 MHz
    // system clock ticks, so the risk of overflow is relatively low.
    uint64_t shTime = 0;         // end time (falling edge) of last SH pulse
    uint64_t icgTime = 0;        // end time (rising edge) of last ICG pulse
    uint64_t frameEndTime = 0;   // end time of last DMA transfer of last frame

    // SH and ICG pulse durations, in 8ns system clock ticks
    static const uint32_t shPulseTicks = 128;   // 128*8ns = 1024ns
    static const uint32_t icgPulseTicks = 625;  // 625*8ns = 5000ns

    // The SH and ICG pulses must always be programmed two frames ahead:
    // the frame we're currently working on, and the one that follows.
    // Whereas the DMA transfer can only be programmed for the current
    // frame.  (This is because we share channel C between the double
    // buffers, so it always has to be set up for the current frame.)
    // When we calculate times for the next frame, therefore, we'll
    // actually program the PIOs for the frame-ahead SH and ICG pulses,
    // but we can only figure the next DMA setup, and must save the
    // programming for next time.  So we have to stash the pending DMA
    // information somewhere, which is here.
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
    PIOProg pioFM;
    PIOProg pioSH;
    PIOProg pioICG;
    
    // Requested integration time, in microseconds.  This allows the
    // host to adjust the exposure time to compensate for lighting
    // conditions.
    uint32_t integrationTime_us = 0;

    // Number of pixels in the native pixel array
    static const uint32_t nativePixCount = 1546;

    // Pixel buffers.  We use two buffers, and the writer switches
    // destination buffers on each frame, so there's always a stable
    // buffer for a client to read from.
    struct PixBuf
    {
        // transfer completion timestamp
        uint64_t timestamp = 0;

        // Pixels, as 8-bit ADC readings.  Each shift register frame
        // consists of 32 dummy outputs, then 1500 "effective" outputs
        // (the actual pixels exposed through the sensor window), then
        // 14 more dummy outputs, for a total of 1546 ADC readings.  The
        // observable image consists only of the 1500 effective outputs,
        // but the chip always generates the dummies at either end when
        // transferring the shift register contents to the host, so we
        // have to allocate space for them in our array.
        //
        // The dummy outputs are a combination of internal shift
        // register positions (on the chip) not connected to sensor
        // pixels, and sensor pixels that are covered by the mask around
        // the sensor window.  The covered pixels are deliberately
        // included in the chip's design, to serve as a reference point
        // for the voltage level on a sensor pixel that's not receiving
        // any light.  The unconnected dummy entries have no apparent
        // purpose; they're probably just artifacts of the chip's
        // internal architecture that leak through into the external
        // interface.
        //
        // Align the buffer on a 4-byte boundary, and round the size up
        // to a multiple of 4, so that we can use 32-bit DMA transfers
        // to copy from this buffer to a caller's buffer.
        uint8_t __attribute__((aligned(4))) pix[(nativePixCount + 3) & ~3];
    };
    PixBuf pixBuf[2];

    // Stable pixel buffer, for client access.  GetPix() makes a copy
    // of the current inactive buffer here.
    PixBuf stablePix;

    // buffer currently in use by writer
    volatile int writerBuf = 0;

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
