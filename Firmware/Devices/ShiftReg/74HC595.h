// Pinscape Pico - 74HC595 device driver - PWM enabled
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The 74HC595 is an 8-bit serial-in, parallel-out shift register chip.
// These chips can be daisy-chained together to form a virtual shift
// register of arbitrary size.  The daisy-chain arrangement makes
// efficient use of GPIO ports, because the chain only needs the same
// fixed set of GPIO ports on the microcontroller, no matter how many
// chips are chained together.  This can be as few as three GPIO ports,
// depending on the feature set needed.
//
// Each C74HC595 object represents a complete daisy chain, with 8*N
// ports (N = number of chips on the chain).  The ports are numbered
// from 0 = QA on the first chip on the chain, which is the one directly
// connected to the Pico.
//
// This module defines two 74HC595 driver subclasses: one that runs the
// chain in digital ON/OFF output mode, and a second that runs it in PWM
// mode.  The PWM version actually uses BCM (binary code modulation)
// rather than true PWM to achieve the brightness control, since BCM can
// be implemented much more efficiently within the constraints of the
// Pico and its PIO facility; but it has the same effect of modulating
// the duty cycle of the output with 8-bit resolution (i.e., 256 duty
// cycle steps, from 0/255 to 255/255 in 1/255 increments).  A complete
// BCM refresh cycle involves sending the chip 8 complete shift register
// updates, one per bit plane, staggered at power-of-two time intervals.
// The minimum power-of-two interval is the time it takes to send one
// complete register update, which is nPorts / shiftClockFreq.  The
// refresh time thus depends upon the number of chips in the daisy chain
// and the shift clock frequency.  For a 4 MHz shift clock, this yields
// a 980 Hz BCM refresh cycle with a two-chip chain (16 ports) and 240
// Hz with an 8-chip chain (64 ports).  The maximum clock rate for the
// chip varies with supply voltage, and goes up to about 30 MHz at the
// highest voltages, but most Pinscape applications will probably run it
// at Pico 3.3V logic levels, where the maximum clock is closer to the 4
// MHz end.  The external wiring can also be a factor, since higher
// clock speeds are more sensitive to imperfections in the wiring and
// external interference.  240 Hz to 980 Hz is great for LEDs, but might
// not be good for motors and solenoids, since the PWM signal can
// translate to mechanical vibrations that manifest as acoustic noise,
// which is readily audible to humans in this frequency range.
//
// We do our best to minimize the resource impact of the PWM mode, but
// it inherently loads the Pico more than the digital mode does.  PWM
// mode needs a bit more memory and a bit more CPU time.  Both modes
// require one PIO state machine and a few slots in PIO opcode memory,
// plus a DMA channel, so that part's a wash.  PWM mode also requires
// running the shift clock at close to the limiting speeds (at least a
// few MHz), which requires good wiring to the 74HC595 chips - ideally,
// the Pico and the chips should all be contained on a single circuit
// board with short traces between the parts.  Digital mode sends so
// little data that it will happily run at low clock speeds; 100 kHz
// should be quite adequate.  Running at low clock speeds isn't nearly
// as demanding on the external wiring.
// 


#pragma once

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <numeric>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/pio.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "Outputs.h"

namespace PinscapePico {
    struct OutputDevDesc;
    struct OutputDevPortDesc;
}

// forward and external declarations
class JSONParser;

// 74HC595 daisy chain object.  Each instance of this object represents
// a daisy chain of 74HC595 chips connected to a common set of GPIOs.
class C74HC595
{
public:
    // construction/destruction
    C74HC595(
        int chainNum, int nChips, int gpShift, int gpData, int gpLatch,
        OutputManager::Device *enablePort, int shiftClockFreq);
    virtual ~C74HC595();

    // Configure from JSON data
    static void Configure(JSONParser &json);

    // Run periodic tasks on all chains
    static void Task();

    // Global instances, built from the configuration data.  Each
    // instance represents a daisy chain of one or more chips, connected
    // through a common set of GPIOs.  Most Pinscape systems will only
    // use one chain, since each chain can consist of many chips, but we
    // can in principle support any number of chains (as many as you can
    // fit with available GPIOs, anyway).
    static std::vector<std::unique_ptr<C74HC595>> chains;

    // get a daisy chain by config index
    static C74HC595 *GetChain(int n) { return (n >= 0 && n < static_cast<int>(chains.size())) ? chains[n].get() : nullptr; }

    // count configured chains
    static size_t CountConfigurations() { return std::count_if(chains.begin(), chains.end(),
        [](const std::unique_ptr<C74HC595>& c){ return c != nullptr; }); }

    // count ports across all configuerd chains
    static size_t CountPorts() { return std::accumulate(chains.begin(), chains.end(), 0,
        [](int acc, const std::unique_ptr<C74HC595>& c){ return c != nullptr ? c->nPorts : 0; }); }

    // enable/disable outputs across all chips
    static void EnableOutputs(bool enable);

    // get my configuration index
    int GetConfigIndex() const { return chainNum; }

    // Is this chain in PWM mode?
    virtual bool IsPWM() const = 0;

    // Set an output level, port 0..nPorts-1 (port 0 = QA on first chip on chain).
    virtual void Set(int port, uint8_t level) = 0;

    // Get an output level in DOF units, 0-255
    virtual uint8_t GetDOFLevel(int port) const = 0;

    // Get an output level in device units (0-1 for digital mode, 0-255 for PWM mode)
    virtual uint8_t GetNativeLevel(int port) const = 0;

    // is the given port number valid?
    bool IsValidPort(int port) const { return port >= 0 && port < nPorts; }

    // Populate an output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query result buffer
    static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

protected:
    // Initialize
    virtual bool Init() = 0;

    // Periodic per-chain tasks - this updates the chip if we have
    // changes to send.
    virtual void ChainTask() = 0;

    // my chain number in the configuration
    int chainNum;

    // Number of chips on the daisy chain
    int nChips;

    // Total number of ports.  Each chip has a fixed 8 ports, so this is
    // always nChips*8.
    int nPorts;

    // Current output port levels, 0..255, one element per port in port order
    uint8_t *level = nullptr;

    // level array is dirty since last DMA staging buffer prep
    bool dirty = false;

    // Serial clock speed, Hz.  Most Pinscape applications will power the chip
    // at Pico logic level, 3.3V.  The data sheet doesn't specify a maximum SCLK
    // speed for 3.3V; the nearest specs are 4MHz at 2V and 20MHz at 4V.  4MHz
    // is thus completely safe, but interpolation suggests that speeds up to
    // around 14MHz would also work.  The maximum usable rate will also depend
    // upon the wiring to the chip, since such high rates are sensitive to noise
    // in the signal path.
    int shiftClockFreq = 4000000;

    // GPIO connections to the chip control lines
    int gpShift;    // shift clock (SHCP on data sheet)
    int gpData;     // serial data (DS)
    int gpLatch;    // latch/transfer clock (STCP)

    // Enable (/OE) port.  This can be any Output Manager port type.
    std::unique_ptr<OutputManager::Device> enablePort;

    // PIO program information
    PIO pio = nullptr;        // PIO unit
    int piosm = -1;           // state machine number
    uint pioOffset = 0;       // program load offset

    // DMA channel
    int dmaChan = -1;

    // DMA completion interrupt handler
    static void SIRQ();
    virtual void IRQ() = 0;

    // console commands
    static void Command_main_S(const ConsoleCommandContext *c);
    virtual void Command_main(const ConsoleCommandContext *c, int chip, int firstOptionIndex) = 0;
};

// Digital-only implementation
class Digital74HC595 : public C74HC595
{
public:
    // construction/destruction
    Digital74HC595(
        int chainNum, int nChips, int gpShift, int gpData, int gpLatch,
        OutputManager::Device *enablePort, int shiftClockFreq);
    ~Digital74HC595();

    // this chain is in digital (non-PWM) mode
    virtual bool IsPWM() const override { return false; }

    // Set an output level, port 0..nPorts-1 (port 0 = QA on first chip on chain).
    virtual void Set(int port, uint8_t level) override;

    // Get an output level.  Reports true for ON, false for OFF.
    virtual uint8_t GetDOFLevel(int port) const override;
    virtual uint8_t GetNativeLevel(int port) const override { return GetDOFLevel(port) != 0 ? 1 : 0; }

protected:
    // Initialize
    virtual bool Init() override;

    // chain task handler
    virtual void ChainTask() override;

    // PIO transfer buffer.  This has a one-byte prefix with the number of
    // ports minus one, followed by the port bits in daisy chain clocking
    // order, in which the low-order bit of the first byte is the last port
    // (QH) on the last chip.
    uint8_t *txBuf;

    // size of transmit buffer in bytes
    size_t txBufCount;

    // internal level state array
    uint8_t *level;

    // IRQ handler
    virtual void IRQ() override;

    // statistics
    struct Stats
    {
        uint64_t nWrites = 0;        // number of updates sent to chip
        uint64_t t0DMA = 0;          // start time of current DMA transfer
        uint64_t tWriteSum = 0;      // sum of DMA transfer times

        // add a write event
        void StartWrite()
        {
            nWrites += 1;
            t0DMA = time_us_64();
        }

        // end a write event
        void EndWrite()
        {
            tWriteSum += time_us_64() - t0DMA;
        }

        // reset statistics
        void Reset()
        {
            nWrites = 0;
            tWriteSum = 0;
        }
    };
    Stats stats;

    // command handler
    virtual void Command_main(const ConsoleCommandContext *c, int chip, int firstOptionIndex) override;
};


// PWM implementation
class PWM74HC595 : public C74HC595
{
public:
    // construction/destruction
    PWM74HC595(
        int chainNum, int nChips, int gpShift, int gpData, int gpLatch,
        OutputManager::Device *enablePort, int shiftClockFreq);

    ~PWM74HC595();

    // this chain is in PWM mode
    virtual bool IsPWM() const override { return true; }

    // Set an output level, port 0..nPorts-1 (port 0 = QA on first chip on chain).
    virtual void Set(int port, uint8_t level) override;

    // Get an output level.  Reports true for ON, false for OFF.
    virtual uint8_t GetDOFLevel(int port) const override;
    virtual uint8_t GetNativeLevel(int port) const override { return GetDOFLevel(port); }

protected:
    // Initialize
    virtual bool Init() override;

    // chain task handler
    virtual void ChainTask() override;

    // IRQ handler
    virtual void IRQ() override;

    // command handler
    virtual void Command_main(const ConsoleCommandContext *c, int chip, int firstOptionIndex) override;

    // PIO TX port count.  This is the number of ports that we send to the
    // PIO program on each cycle, equal to the actual number of ports rounded
    // up to the next multiple of 16.  The PIO program works in units of 16
    // ports, so we have to add 8 extra ports (one chip's worth) when we have
    // an odd number of physical chips.  The pseudo chip goes at the end of
    // the chain, since this lets the PIO program clock out the extra bits
    // first, so that they'll harmlessly fall off the end of the chain.
    int nPioChips;
    int nPioPorts;

    // PIO transmit buffers.  We use double-buffering: at any given time, one
    // buffer is in flight through DMA, and the other is staged awaiting the
    // next transmission.  On each DMA-completion interrupt, we kick off the
    // next DMA transmission with the staging buffer, and stage the next
    // transmission into the other buffer.
    struct TXBuf
    {
        uint16_t *buf = nullptr;
    };
    TXBuf txBuf[2];

    // size of the transmit buffer, in DMA elements
    int txBufCount = 0;

    // current staging buffer
    volatile int txStage = 0;

    // current DMA transmission buffer
    volatile int txDMA = 0;

    // prepare a transmission
    void PrepareTX(TXBuf *tx);

    // statistics
    struct Stats
    {
        uint64_t nPreps = 0;         // number of TX prepares
        uint64_t tPrepSum = 0;       // total time spent in write prep, in microseconds

        uint64_t nDMA = 0;           // number of DMA transfers started
        uint64_t t0DMA = 0;          // starting time of DMA statistics

        // add a buffer prepare event
        void AddPrep(uint64_t dt)
        {
            nPreps += 1;
            tPrepSum += dt;
        }

        // reset statistics
        void Reset()
        {
            nPreps = 0;
            tPrepSum = 0;
            nDMA = 0;
            t0DMA = time_us_64();
        }
    };
    Stats stats;
};
