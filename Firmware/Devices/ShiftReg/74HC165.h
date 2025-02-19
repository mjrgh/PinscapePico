// Pinscape Pico - 74HC165 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The 74HC165 is an 8-bit parallel input, serial output shift register
// chip.  The chips can be daisy-chained together to form a virtual
// shift register of arbitrary size.  This daisy-chain arrangement makes
// efficient use of GPIO ports on the microcontroller, because the chain
// only consumes three GPIO ports on the host controller, no matter how
// many chips are chained together.  Pinscape Pico can use these chips
// to implement digital (on/off) input ports for button inputs.
//
// The input ports on the 74HC165 don't have any built-in pull-up or
// pull-down resistors, so the hardware design must include suitable
// pull resistors for all input ports.  The Pinscape software can be
// configured per port for either pull direction (by setting "active
// high" or "active low" mode), so you can use whichever arrangement is
// more convenient for your hardware design.

#pragma once

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/pio.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"

// forward and external declarations
class JSONParser;
namespace PinscapePico { struct ButtonDevice; }

// 74HC165 daisy chain object.  Each instance of this object represents
// a daisy chain of 74HC595 chips connected to a common set of GPIOs.
class C74HC165
{
public:
    // Configure from JSON data
    static void Configure(JSONParser &json);

    // Second-core task handler
    static void SecondCoreTask();

    // Global instances, built from the configuration data.  Each
    // instance represents a daisy chain of one or more chips, connected
    // through a common set of GPIOs.  Most Pinscape systems will only
    // use one chain, since each chain can consist of many chips, but we
    // can in principle support any number of chains (as many as you can
    // fit with available GPIOs, anyway).
    static std::vector<std::unique_ptr<C74HC165>> chains;

    // get a daisy chain by config index
    static C74HC165 *GetChain(int n) { return (n >= 0 && n < CountConfigurations()) ? chains[n].get() : nullptr; }

    // get the number of configured daisy chains
    static int CountConfigurations() { return static_cast<int>(chains.size()); }

    // construction/destruction
    C74HC165(int chainNum, int nChips, int gpSHLD, int gpClk, int gpQH, bool loadPolarity, int shiftClockFreq);
    ~C74HC165();

    // get my config index
    int GetConfigIndex() const { return chainNum; }

    // Read an input port, port 0..(nPorts-1).  This returns the last
    // level read during a polling cycle.
    bool Get(int port);

    // is the given port number valid?
    bool IsValidPort(int port) const { return port >= 0 && port < nPorts; }

    // Populate a Vendor Interface button query result buffer with
    // ButtonDevice structs representing the configured 74HC165 chips.
    // The caller is responsible for providing enough buffer space; we
    // require one PinscapePico::ButtonDevice per daisy chain.  On
    // return, the buffer pointer is automatically incremented past the
    // space consumed.
    static void PopulateDescs(PinscapePico::ButtonDevice* &descs);

    // Query the states of the 74HC165 input ports, for a Vendor Interface
    // button state query.  Populates the buffer with one byte per input
    // port across all daisy chains, arranged in order of the daisy chains
    // in the configuration list.  Returns the size in bytes of the populated
    // buffer space, or 0xFFFFFFFF on failure.  Note that 0 isn't an error:
    // it simply means that there are no 74HC165 ports configured.
    static size_t QueryInputStates(uint8_t *buf, size_t bufSize);

protected:
    // Initialize
    bool Init();

    // Periodic per-chain second-core tasks.  This starts the next DMA
    // input cycle when the last one has finished.
    void SecondCoreChainTask();

    // start a DMA transfer from the PIO to the data buffer
    void StartTransfer(volatile uint8_t *destBuf);

    // my chain number in the configuration
    int chainNum;

    // Number of chips on the daisy chain
    int nChips;

    // Total number of ports.  Each chip has a fixed 8 ports, so this is
    // always nChips*8.
    int nPorts;

    // GPIO connections to the chip control lines
    int gpSHLD;      // shift/load (SH/LD)
    int gpClk;       // serial clock (CLK)
    int gpQH;        // serial data out (QH)

    // DMA channel
    int dmaChan = -1;

    // DMA enabled (can be disabled for debugging)
    volatile bool dmaEnabled = true;

    // Desired shift clock (CLK) frequency
    int shiftClockFreq = 6000000;

    // LOAD polarity.  This is the logic level to set on the SHLD
    // (shift/load) pin to put the chip in PARALLEL LOAD mode.
    //
    // For SN74HC165, this is false (LOW).
    //
    // The reason we make this a variable is that there are some very
    // similar shift-register chips that use almost the identical pin
    // set and clock timing structure as 74HC195, but use the opposite
    // polarity for their equivalent of the SHLD pin.  It's our hope
    // that allowing for the polarity difference will make to possible
    // to use this same software with some of these other chips, without
    // the need for a separate device class for each other chip.  There
    // might still be other differences with some such chips, such as
    // clock timing details, that do end up requiring separate device
    // classes, but the polarity difference is one detail we already
    // know about, so we're including this option for the sake of making
    // the class as broadly compatible as possible.
    bool loadPolarity;

    // PIO unit and state machine
    PIO pio = nullptr;
    int piosm = -1;

    // PIO program load offset
    uint pioOffset = 0;

    // Current pin state data, one byte per chip in daisy-chain order,
    // with the bits encoded in serial order.  The DMA transfer writes
    // directly into this buffer.
    volatile uint8_t *data = nullptr;

    // Statistics
    struct Statistics
    {
        // reset all - call only from the main-core thread
        void RequestReset() { resetRequested = true; }

        // apply reset - call only from the second-core thread
        void CheckResetRequest()
        {
            if (resetRequested)
            {
                resetRequested = false;
                t0 = time_us_64();
                nDMACycles = 0;
            }
        }

        // count a DMA cycle
        void AddDMACycle()
        {
            nDMACycles += 1;
        }

        // reset time
        uint64_t t0 = 0;

        // number of DMA cycles
        uint64_t nDMACycles = 0;

        // Reset requested.  The main core thread sets this when it
        // wants to reset the statistics; the second core thread applies
        // it during polling..
        bool resetRequested = false;
    };
    Statistics stats;

    // console commands
    static void Command_main_S(const ConsoleCommandContext *c);
    void Command_main(const ConsoleCommandContext *c, int chip, int firstOptionIndex);

    // Debugging - switch GPIO ownership between CPU and PIO.  CPU control
    // allows bit-bang access, for testing purposes.
    void PinsToCPU();
    void PinsToPIO();

    // scratch DMA buffer for debug transfers
    volatile uint8_t *scratchData = nullptr;
};
