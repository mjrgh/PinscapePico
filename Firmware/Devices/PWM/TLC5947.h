// Pinscape Pico - TLC5947 device driver
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The TI TLC5947 is a 24-channel, 12-bit PWM LED driver with an ad hoc
// serial interface.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <numeric>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/dma.h>
#include <hardware/pio.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "PIOHelper.h"

// external declarations
class JSONParser;
class ConsoleCommandContext;
namespace PinscapePico {
    struct OutputDevDesc;
    struct OutputDevPortDesc;
}


// TLC5947 daisy chain object.  Each instance of this object represents
// a daisy chain of TLC5947 chips connected to a common set of GPIOs.
class TLC5947 : public PIOInterruptHandler
{
public:
    // Configure TLC5947 units from JSON data
    static void Configure(JSONParser &json);

    // Global instances, built from the configuration data.  Note that
    // each instance represents an entire daisy chain of chips, all
    // connected to a common set of GPIOs.  Most systems will only
    // ever have a single daisy chain, since a single chain can support
    // as many chips as most people would ever need in a virtual pinball
    // application, and since each chain consumes a rather large block
    // of GPIO ports (four).
    static std::vector<std::unique_ptr<TLC5947>> chains;

    // get a daisy chain by config index
    static TLC5947 *GetChain(int n);

    // Get the number of configured TLC5947 daisy chains
    static size_t CountConfigurations() { return std::count_if(chains.begin(), chains.end(),
        [](const std::unique_ptr<TLC5947>& c){ return c != nullptr; }); }

    // count ports across all configured chains
    static size_t CountPorts() { return std::accumulate(chains.begin(), chains.end(), 0,
        [](int acc, const std::unique_ptr<TLC5947>& c){ return c != nullptr ? acc + c->nPorts : acc; }); }

    // get my configuration index
    int GetConfigIndex() const { return chainNum; }

    // construction/destruction
    TLC5947(int chainNum, int nChips, int gpSIN, int gpSClk, int gpBlank, int gpXlat);
    ~TLC5947();

    // Get/Set an output level, port 0..(nPorts-1), level 0..4095
    uint16_t Get(int port);
    void Set(int port, uint16_t level);

    // is the given port number valid?
    bool IsValidPort(int port) const { return port >= 0 && port < nPorts; }

    // Populate a vendor interface output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query result buffer
    static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

    // Log diagnostics
    void Diagnostics(const ConsoleCommandContext *ctx);

    // PIO IRQ handler
    virtual void PIO_IRQ() override;

protected:
    // Initialize.  Sets up the PIO program.
    bool Init();

    // start a DMA send to the PIO state machine from the current buffer
    void StartDMASend();

    // PIO unit and state machine
    PIO pio = nullptr;
    int piosm = -1;

    // PIO program load offset
    uint pioOffset = 0;

    // my chain number in the configuration
    int chainNum;

    // Number of chips on the daisy chain
    int nChips;

    // Total number of ports.  Each chip has a fixed 24 ports, so this
    // is alays nChips*24.
    int nPorts;

    // GPIO connections to the chip control lines
    int gpSClk;     // Serial Clock
    int gpSIN;      // Serial Data
    int gpBlank;    // BLANK
    int gpXlat;     // XLAT

    // DMA channel for PIO transmissions
    int dmaChannelTx = -1;
    dma_channel_config configTx;

    // Current output port levels, 0..4095.  These are the live registers
    // that the application controls.  The application can update these
    // at any time.  The PIO program transmits the current state as of
    // the start of each shift register cycle.
    //
    // The values we store here must be "left-justified" in the 16-bit
    // array elements, since the PIO program only reads the most
    // significant 12 bits.  So the nominal 0..4095 level must be
    // left-shifted by 4 bits.
    uint16_t *level = nullptr;

    // DMA transmission double buffer.  We maintain two buffers, one
    // that's currently being used for the DMA transmission, and one
    // under construction for the next transmission.
    //
    // Each DMA buffer consists of a two-element header, with the data
    // bit count and grayscale clock count, followed by the data bits
    // in 12-bit left-justified format.
    uint16_t *dmaBuf[2] = { nullptr, nullptr };

    // Double-buffering pointers.  'cur' is the current buffer that the
    // DMA channel owns; 'nxt' is the next buffer under construction
    // with new values for the next transmission.  When the IRQ takes
    // ownership of the next buffer on behalf of the DMA channel, it
    // updates dmaCur to equal dmaNxt.
    volatile int dmaCur = 0;
    volatile int dmaNxt = 0;
    
    // Diagnostic statistics
    struct
    {
        // Number of DMA transmissions started
        uint64_t nSends = 0;

        // send time averaging - we count sends over a time window
        // to calculate the average rate over that window
        struct
        {
            uint32_t n = 0;    // number of sends in current window
            uint64_t t0 = 0;   // start time of current window
        } avgWindow;

        // latest rolling average send cycle time
        float avgSendTime = 0.0f;
    } stats;
};
