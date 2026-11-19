// Pinscape Pico - WS2812 individually addressable LED driver
// Copyright 2026 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Provides a hardware interface to WS2812B LED strips.


// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <numeric>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/dma.h>

// project header"
#include "Pinscape.h"
#include "Utils.h"

// forwards
class JSONParser;

// WS2812 device driver
class WS2812
{
public:
    // get the WS2812 interface object for a given configuration index
    static WS2812 *GetStrip(int configIndex) {
        return configIndex >= 0 && configIndex < static_cast<int>(strips.size()) ? strips[configIndex].get() : nullptr;
    }

    // get my configuration index
    int GetConfigIndex() const { return configIndex; }

    // get the number of LEDs, the number of DOF ports, and the number of color channels per LED
    int GetNumLeds() const { return nLeds; }
    int GetNumDOFPorts() const { return nLeds * nColorChannels; }
    int GetNumColorChannels() const { return nColorChannels; }

    // configure
    static void Configure(JSONParser &json);

    // periodic task handler
    static void Task();

    // Set a DOF port level.  Each LED in the strip consists of three or
    // four consecutive DOF output ports - three for RGB format, four
    // for RGBW format.  The DOF ports map to the R, G, B, and W
    // channels of the LED:
    //
    //   DOF port     LED/Channel - RGB     LED/Channel - RGBW
    //      0           0/R                     0/R
    //      1           0/G                     0/G
    //      2           0/B                     0/B
    //      3           1/R                     0/W
    //      4           1/G                     1/R
    //      5           1/B                     1/G
    //      6           2/R                     1/B
    //      7           2/G                     1/W
    //      8           2/B                     2/R
    //     -etc-
    //
    // This routine attempts to perform "coherent writes", meaning that
    // each group of three or four channels corresponding to a single
    // physical LED is treated as an indivisible unit when sending
    // updates to the LED string.  For example, when we see a write to
    // the red channel of LED 1, we wait to see if there will also be
    // writes to the green and blue chanels of LED 1 before updating the
    // physical LED chain, so that we can update all three channels for
    // LED 1 in a single transmission to the LED string.  DOF exposes
    // RGB devices not as units but as three separate and indepedent
    // channels.  In most cases, though, the HOST thinks about an RGB
    // LED as a unit displaying a subjective color - the host only
    // breaks down the subjective color into its RGB components because
    // the structure of the DOF interface requires it to do so.  If DOF
    // had a way for the host to say "set LED 7 to puce", nearly every
    // host would do that.  But DOF doesn't have such an interface, so
    // the host has to accomplish that objective by breaking it into
    // three independent port updates, "set port 21 to 0xCC", "set port
    // 22 to 0x88", and "set port 23 to 0x99".  Our "coherent write"
    // algorithm tries to detect those grouped writes, and piece them
    // back together into a single update to the chain.
    void SetDOF(int dofPort, uint8_t level);

    // Get a DOF port value
    uint8_t GetDOF(int dofPort) const;

    // Set an LED RGB level.  This addresses all channels of the LED in
    // a single call.  'led' is the index of the LED in the strip,
    // starting at 0 for the LED at the front end of the strip (nearest
    // the Pico).  The RGB value is formatted in the low 24 bits:
    //
    // Bit  31....24 23....16 15.....8 7......0
    //      xxxxxxxx RRRRRRRR GGGGGGGG BBBBBBBB
    //
    void SetRGB(int led, uint32_t rgb) { SetGRBW(led, RGBWtoGRBW(rgb << 8)); }

    // Set an LED RGBW level.  This addresses all channels of the LED
    // atomically in a single call.  'led' is the index of the LED in
    // the strip, starting at 0 for the LED at the front end of the
    // strip (nearest to the Pico, in terms of wiring topology).  The
    // level value is arranged as follows:
    //
    // Bit  31....24 23....16 15.....8 7......0
    //      RRRRRRRR GGGGGGGG BBBBBBBB WWWWWWWW      (RGBW devices)
    //      RRRRRRRR GGGGGGGG BBBBBBBB xxxxxxxx      (RGB devices)
    //
    // For RGB format, the low byte with the White value is ignored.
    //
    // This routine is for callers that need to use the RGBW format
    // internally.  If the caller has the flexibility to use the
    // WS2812's native GRBW format internally, it's slightly more
    // efficient to call SetGRBW(), since this routine has to rearrange
    // the bytes to match the native order.
    void SetRGBW(int led, uint32_t rgbw) { SetGRBW(led, RGBWtoGRBW(rgbw)); }

    // Set an LED GRBW level.  This is the native format that the
    // WS2812 devices use:
    //
    // Bit  31....24 23....16 15.....8 7......0
    //      GGGGGGGG RRRRRRRR BBBBBBBB WWWWWWWW      (RGBW devices)
    //      GGGGGGGG RRRRRRRR BBBBBBBB xxxxxxxx      (RGB devices)
    //
    // Since this is the native format, this is the most efficient
    // way to set an output.
    void SetGRBW(int led, uint32_t grbw);

    // Get the current RGBW/GRBW setting on an LED
    uint32_t GetGRBW(int led) const { return led >= 0 && led < nLeds ? level[led] : 0; }
    uint32_t GetRGBW(int led) const { return GRBWtoRGBW(GetGRBW(led)); }

    // Convert RGBW to GRBW (native WS2812 format)
    static inline uint32_t RGBWtoGRBW(uint32_t rgbw) {
        return ((rgbw & 0xFF000000) >> 8) | ((rgbw & 0x00FF0000) << 8) | (rgbw & 0x0000FFFF);
    }

    // Convert GRBW to RGBW
    static inline uint32_t GRBWtoRGBW(uint32_t grbw) {
        return ((grbw & 0xFF000000) >> 8) | ((grbw & 0x00FF0000) << 8) | (grbw & 0x0000FFFF);
    }

    // count configured strips
    static size_t CountConfigurations() { return std::count_if(strips.begin(), strips.end(),
        [](const std::unique_ptr<WS2812>& s){ return s != nullptr; }); }


    // count ports across all configuerd chains
    static size_t CountPorts() { return std::accumulate(strips.begin(), strips.end(), 0,
        [](int acc, const std::unique_ptr<WS2812>& s){ return s != nullptr ? acc + (s->nLeds * s->nColorChannels) : acc; }); }

    // Populate an output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query report
    static void PopulateLevels(PinscapePico::OutputDevLevel* &dst);

protected:
    // protected constructor - called from Configure
    WS2812(int configIndex, int gp, int nLeds, std::string &format, int bitFreq, int refreshFreq);

    // Master list of configured WS2812 LED strips.  Each element
    // represents one linear LED strip, which is arranged as a daisy
    // chain of individually addressable WS2812 LEDs.  The entire chain
    // is controlled through a single GPIO port, which connects to the
    // first LED's DIN (data in) pin.  Each additional LED on the strip
    // is daisy-chained, with each LED's DOUT tied to the next LED's
    // DIN.
    static std::vector<std::unique_ptr<WS2812>> strips;

    // initialize
    bool Init();

    // configuration file index, for diagnostic reporting
    int configIndex;

    // GPIO port for DIN (data in serial transmission)
    int gp;

    // number of LEDs in this strip
    int nLeds;

    // format name ("rgb", "rgbw")
    std::string format;

    // number of color channels (3 for RGB, 4 for RGBW)
    int nColorChannels = 3;

    // bit clock frequency, Hz
    int bitFreq;

    // refresh rate, Hz, and its inverse (time between refreshes)
    int refreshFreq;
    int refreshInterval;

    // Output level array.  Each element is the GRBW level for one LED
    // in the strip.  This is stored in native WS2812 GRBW order:
    //
    // Bit  31....24 23....16 15.....8 7......0
    //      GGGGGGGG RRRRRRRR BBBBBBBB WWWWWWWW      (RGBW devices)
    //      GGGGGGGG RRRRRRRR BBBBBBBB xxxxxxxx      (RGB devices)
    //
    // Note that G, R, and B are in the same field positions for all
    // devices.  W (white) is stored in the low-order byte for RGBW
    // devices, RGB devices simply ignore this byte.
    uint32_t *level = nullptr;
    
    // PIO state machine allocation
    PIO pio = nullptr;
    int piosm = -1;
    uint pioOffset = 0;

    // DMA channel for PIO data transmission
    int dmaChannel = -1;
    dma_channel_config dmaConfig;

    // Start time of last DMA transmission.  This is the system clock
    // time when we initiated the DMA send.
    uint64_t tDMA = 0;

    // Minimum delay time after the DMA completion interrupt
    uint32_t minDMAWaitTime = 0;

    // Start a DMA send
    void StartDMASend();

    // DMA IRQ handler
    static void DMAIRQ();

    // Alarm IRQ callback
    static int64_t OnAlarm(alarm_id_t id, void *ctx);

    // Coherent write cache, for DOF writes.  We cache writes to
    // individual DOF channels until one of the following occurs:
    //
    //  - all color components of the same LED have been updated
    //  - we write to a different LED than last time
    //  - no new writes occur over a timeout period
    //
    struct DOFCache
    {
        // target LED index
        int led = -1;
        
        // cache accumulator, in native GRBW order
        uint32_t val = 0x00000000;

        // bit mask of components written so far
        uint32_t mask = 0x00000000;

        // mask matching a full color set 
        uint32_t fullMask = 0xFFFFFF00;

        // time of last write, for aging out the cache
        uint64_t tWrite = 0;
    };
    DOFCache dofCache;

    // flush the cache
    void FlushDOFCache()
    {
        // save the cached components to the level array
        level[dofCache.led] &= ~dofCache.mask;
        level[dofCache.led] |= dofCache.val;
        
        // clear the cache
        dofCache.mask = 0;
        dofCache.val = 0;
        dofCache.led = -1;
    }

    // DMA buffer indices: the buffer currently being transmitted,
    // and the buffer under construction for the next cycle.
    volatile int dmaCur = 0;
    volatile int dmaNxt = 0;

    // Statistics
    struct Stats
    {
        // end a DMA cycle; 'dt' is the elapsed time between DMA start
        // and DMA completion
        void EndCycle(uint64_t dt)
        {
            dmaTotalTime += dt;
            nCycles += 1;
        }

        // reset stats
        void Reset()
        {
            IRQDisabler irqd;
            t0 = time_us_64();
            nCycles = 0;
            dmaTotalTime = 0;
            nAgedOut = 0;
        }

        // start time for statistics
        uint64_t t0 = 0;

        // Number of transmission cycles
        uint64_t nCycles = 0;

        // Cumulative time between DMA starts and completions
        uint64_t dmaTotalTime = 0;
        
        // Number of aged-out DOF cache writes
        uint64_t nAgedOut = 0;
    };
    Stats stats;

    // console commands
    static void Command_main_S(const ConsoleCommandContext *c);
    void Command_main(const ConsoleCommandContext *c, int firstOptionIndex);
};
