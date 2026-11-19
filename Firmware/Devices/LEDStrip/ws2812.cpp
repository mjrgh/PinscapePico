// Pinscape Pico - WS2812 individually addressable LED driver
// Copyright 2026 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cstdlib>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/gpio.h>

// project header"
#include "Pinscape.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "JSON.h"
#include "Logger.h"
#include "CommandConsole.h"
#include "GPIOManager.h"
#include "Outputs.h"
#include "ws2812.h"
#include "ws2812.pio.h"

// master list of configured WS2812 LED strips
std::vector<std::unique_ptr<WS2812>> WS2812::strips;

// Configure from JSON data
//
// ws2812: {
//   din: <gpnumber>,      // GPIO connected to the DIN pin on the first LED on the strip
//   nLeds: <number>,      // number of LEDs on the strip
//   format: <string>,     // data format: "rgb" (24-bit) or "rgbw" (32-bit), default is "rgb"
//   freq: <number>,       // data clock frequency in Hz; default 800000
//   refresh: <number>,    // refresh ferquency in Hz; default 200
// }
//
// If more than one WS2812 LED strip is configured, use an array
// of the objects defined above:
//
// ws2812: [
//    { ... },      // first LED strip
//    { ... },      // second LED strip
// }
//
void WS2812::Configure(JSONParser &json)
{
    // Object parser callback.  This is invoked for each object in
    // the ws2812 array, or for the single object if it's not an array.
    int nConfigured = 0;
    auto Parse = [&nConfigured](int index, const JSONParser::Value *value)
    {
        // get and validate the parameters
        int nLeds = value->Get("nLeds")->Int(-1);
        uint8_t din = value->Get("din")->UInt8(255);
        std::string format = value->Get("format")->String("rgb");
        int freq = value->Get("freq")->Int(800000);
        int refresh = value->Get("refresh")->Int(200);
        if (!IsValidGP(din))
        {
            Log(LOG_ERROR, "ws2812[%d]: 'din' (Data In GPIO pin assignment) is invalid or missing\n", index);
            return;
        }
        if (nLeds < 1)
        {
            Log(LOG_ERROR, "ws2812[%d]: 'nLeds' is missing or invalid; the strip must have at least 1 LED\n", index);
            return;
        }
        if (format != "rgb" && format != "rgbw")
        {
            Log(LOG_ERROR, "ws2812[%d]: format \"%s\" is invalid; expected \"rgb\" or \"rgbw\"\n", index, format.c_str());
            return;
        }
        if (refresh < 1 || refresh > 1000)
        {
            Log(LOG_WARNING, "ws2812[%d]: invalid refresh rate %s; using default 200 Hz\n", index, refresh);
            refresh = 200;
        }

        // claim the GPIO
        if (!gpioManager.Claim(Format("WS2812[%d] DIN", index), din))
            return;

        // create the device
        auto *dev = new WS2812(index, din, nLeds, format, freq, refresh);
        strips[index].reset(dev);

        // initialize it; if that fails, delete the device
        if (dev->Init())
            ++nConfigured;
        else
            strips[index].reset();
    };

    // The 'ws2812' entry can be a single WS2812 configuration
    // object, or an array of objects.
    auto *cfg = json.Get("ws2812");
    if (cfg->IsObject() || cfg->IsArray())
    {
        strips.resize(cfg->Length(1));
        cfg->ForEach(Parse, true);
    }
    else if (!cfg->IsUndefined())
        Log(LOG_ERROR, "Config: 'ws2812' key must be an object, or an array of objects\n");

    // If we configured any devices, set up the console command
    if (nConfigured != 0)
    {
        CommandConsole::AddCommand(
            "ws2812", "ws2812 LED strip interface diagnostics",
            "ws2812 [stripNum] <options>\n"
            "  <stripNum> selects a light strip by configuration index, starting at 0; default is 0\n"
            "\n"
            "options:\n"
            "  <port>=<level>    set a port, in the form 1R (first LED, Red channel), level 0..255\n"
            "  <led>=#<color>    set an LED's ports as a group to an HTML hex color color;\n"
            "                    for RGBW devices, add two extra hex digits for the white channel\n"
            "  --levels, -l      list port levels\n"
            "  --stats, -s       show statistics\n"
            "  --reset-stats     reset statistics\n"
            "\n"
            "Setting a port level suspends output management to allow direct chip testing.\n",
            &Command_main_S);
    }
}

WS2812::WS2812(int configIndex, int gp, int nLeds, std::string &format, int bitFreq, int refreshFreq) :
    configIndex(configIndex), gp(gp), nLeds(nLeds), format(format), bitFreq(bitFreq), refreshFreq(refreshFreq)
{
    // Set the number of color channels: 4 for RGBW, 3 for RGB
    if (format == "rgbw")
    {
        nColorChannels = 4;
        dofCache.fullMask = 0xFFFFFFFF;
    }

    // calculate the refresh time as 1/refreshFreq (the frequency
    // is in Hz, and the time is in microseconds)
    refreshInterval = 1000000/refreshFreq;

    // allocate space for the current level array
    level = new uint32_t[nLeds];
    memset(level, 0, nLeds * sizeof(level[0]));

    // Calculate the minimum delay time betwen the DMA completion
    // interrupt and the start of the next DMA transmission.  The WS2812
    // protocol requires a minimum 50us LOW pulse between transmission
    // cycles, which signals the LEDs to latch the last data received
    // and reset their protocol state the next cycle.
    //
    // But we can't just use a fixed 50us delay and call it good,
    // because the DMA interrupt occurs when the **DMA** operation has
    // finished, whereas the LOW pulse starts when **PIO** finishes
    // clocking out the last bit of the last buffer.  There will
    // therefore be a significant (on this scale) interval between the
    // DMA interrupt and PIO completion, because the DMA interrupt will
    // fire as soon as the DMA engine successfully transfers the last
    // buffer word into the PIO TX FIFO.  The FIFO is four DWORDs deep,
    // so the PIO still has 4*24 or 4*32 bits left to clock out at the
    // moment the DMA interrupt occurs.  We have to take that additional
    // time into account when calculating when we can start the next DMA
    // send.
    //
    // So we have to wait at least 50us PLUS 4*nColorBits*timePerBit.
    // There are 8 bits per color channel, and the time per bit is
    // simply 1/freq ('freq' is the number of WS2812 bits per second,
    // and there's no padding between bits in the protocol).  Therefore,
    // the minimum wait time after a DMA interrupt is:
    //
    //   50us + 4*8*nColorChannels/freq
    //
    // To be safe, add in a little extra margin of error to the 50us
    // minimum.  50us is a minimum for the LOW latch/reset pulse, but
    // the LEDs will be perfectly happy if it's even longer, and the
    // longer it is, the less likely they are to miss it.  Note that
    // 'freq' is in Hz, and the time is in microseconds, so we have to
    // roll in a factor of 1e6us/s on the time side.  4*8*nCh will be
    // 768 (for RGB) or 1024 (for RGBW), so 4*8*nCh*1e6 will always fit
    // in a UINT32, so we can safely do this whole calculation with
    // UINT32's.
    //
    minDMAWaitTime = 70 + (32*nColorChannels * 1000000UL)/bitFreq;
}

bool WS2812::Init()
{
    // Tentatively claim a free DMA channel.  This is an RAII object
    // that frees the channel if we exit before taking ownership.
    struct DMAClaim
    {
        DMAClaim() : ch(dma_claim_unused_channel(false)) { }
        ~DMAClaim() { if (ch >= 0) dma_channel_unclaim(ch); }
        int Release() { int ret = ch; ch = -1; return ret; }
        int ch;
    } dmaClaim;
    if (dmaClaim.ch < 0)
    {
        Log(LOG_ERROR, "WS2812[%d]: unable to allocate a DMA channel\n", configIndex);
        return false;
    }

    // If an earlier instance has already loaded the PIO program, we can
    // use the existing copy, as long as there's an available state
    // machine on the same PIO unit.  All SM's on a PIO unit share the
    // same program storage, so the program can be reused as many times
    // as there are available SM's on the unit.  This is important
    // because PIO program storage space is extremely limited.
    for (auto &other : strips)
    {
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

    // if we couldn't attach to an existing copy of the program, load
    // a new one
    if (this->piosm < 0)
    {
        // try one PIO
        auto TryClaimPIO = [this](PIO pio)
        {
            // make sure there's space to load the program into this PIO
            if (!pio_can_add_program(pio, &ws2812_program))
                return false;

            // try claiming a state machine on the PIO
            this->piosm = pio_claim_unused_sm(pio, false);
            if (this->piosm < 0)
                return false;

            // success - add the program
            this->pioOffset = pio_add_program(pio, &ws2812_program);
            this->pio = pio;
            return true;
        };

        // try each PIO
        if (!TryClaimPIO(pio0) && !TryClaimPIO(pio1))
        {
            // Failed - log an error and return
            Log(LOG_ERROR, "WS2812[%d]: insufficient PIO resources\n", configIndex);
            return false;
        }
    }

    // initialize the PIO program, which also initializes the GPIO pin
    ws2812_program_init(pio, piosm, pioOffset, gp, bitFreq, format == "rgbw");

    // configure our DMA channel to write data to the PIO
    dmaConfig = dma_channel_get_default_config(dmaClaim.ch);
    channel_config_set_read_increment(&dmaConfig, true);
    channel_config_set_write_increment(&dmaConfig, false);
    channel_config_set_transfer_data_size(&dmaConfig, DMA_SIZE_32);
    channel_config_set_dreq(&dmaConfig, pio_get_dreq(pio, piosm, true));
    dma_channel_configure(dmaClaim.ch, &dmaConfig, &pio->txf[piosm], nullptr, nLeds, false);

    // successful setup - take ownership of the DMA channel
    dmaChannel = dmaClaim.Release();

    // set up the DMA completion interrupt
    irq_add_shared_handler(DMA_IRQ_0, &WS2812::DMAIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dmaChannel, true);

    // reset statistics
    stats.Reset();

    // set up the first DMA transmission
    StartDMASend();

    // success
    Log(LOG_CONFIG, "WS2812[%d] initialized; DIN on GP%d, %d Hz refresh, %d bits/second, DMA channel %d, PIO %d.%d@%u\n",
        configIndex, gp, refreshFreq, bitFreq, dmaChannel, pio_get_index(pio), piosm, pioOffset);
    return true;
}

// periodic task handler
void WS2812::Task()
{
    // age out old DOF write cache entries
    // notify each configured LED string
    uint64_t now = time_us_64();
    for (size_t i = 0 ; i < strips.size() ; ++i)
    {
        // if this entry is configured, call its member function interrupt handler
        if (auto *s = strips[i].get(); s != nullptr && s->dofCache.led >= 0 && now - s->dofCache.tWrite > 5000)
        {
            // flush the DOF cache
            s->FlushDOFCache();

            // count the timeout
            s->stats.nAgedOut += 1;
        }
    }
}

// Start a DMA transmission
void WS2812::StartDMASend()
{
    // note the start time of the transmission
    tDMA = time_us_64();

    // transmit the level buffer
    dma_channel_transfer_from_buffer_now(dmaChannel, level, nLeds);
}

// DMA completion IRQ - static entrypoint
void __not_in_flash_func(WS2812::DMAIRQ)()
{
    // notify each configured LED string
    for (size_t i = 0 ; i < strips.size() ; ++i)
    {
        // if this entry is configured, call its member function interrupt handler
        if (auto *s = strips[i].get(); s != nullptr && dma_channel_get_irq0_status(s->dmaChannel))
        {
            // count statistics
            uint64_t now = time_us_64();
            s->stats.EndCycle(now - s->tDMA);
            
            // clear the interrupt status flag in the channel
            dma_channel_acknowledge_irq0(s->dmaChannel);

            // Calculate the start time for the next DMA transmission,
            // based on the refresh frequency.
            uint64_t tNext = s->tDMA + s->refreshInterval;
            uint64_t dt = tNext < now ? 0 : tNext - now;

            // The WS2812 protocol requires a LOW pulse of AT LEAST 50us
            // betweeen transmission cycles, to act as a latch/reset
            // signal to the chain.  But that's not just a 50us delay
            // from right now, because the DMA interrupt fires when the
            // DMA writer finishes its last transfer.  That transfer
            // goes into the PIO TX FIFO, which is 4 DWORDs deep, so
            // when DMA finishes, the PIO still has 4*24 or 4*32 bits
            // still left in the FIFO that it needs to clock out.  The
            // 50us LOW pulse has to be timed to occur after the PIO
            // finishes clocking out that last pending bit.  We already
            // calculated the time that will take, and stashed the
            // result in minDMAWaitTime.  So we just need to make sure
            // that the wait time we just calculated is at least that.
            if (dt < s->minDMAWaitTime)
                dt = s->minDMAWaitTime;

            // We're in IRQ context, so we can't just sit here waiting
            // for this time to elapse.  We need to return ASAP to let
            // the main program go about its business.  So set an alarm
            // timer that fires after the delay.
            add_alarm_in_us(dt, &WS2812::OnAlarm, s, true);
        }
    }
}

// DMA completion IRQ - member function
int64_t __not_in_flash_func(WS2812::OnAlarm)(alarm_id_t id, void *ctx)
{
    // the user data context is 'this'
    auto *self = reinterpret_cast<WS2812*>(ctx);

    // start the new DMA transfer
    self->StartDMASend();

    // this is a one shot, so return 0 for "done, do not repeat"
    return 0;
}

// bit offsets of the ports in the level[] array entries (GRBW byte order)
static const int dofBitOffset[]{ 16, 24, 8, 0 };

// cache mask for each color component
static const uint32_t dofCacheMask[]{ 0x00FF0000, 0xFF000000, 0x0000FF00, 0x000000FF };

void WS2812::SetDOF(int port, uint8_t level)
{
    // Figure the LED unit and color channel offset.  DOF ports are
    // mapped to individual LEDs in consecutive blocks of 3 DOF ports
    // for RGB devices, or 4 DOF ports for RGBW devices.  So we get the
    // LED index by dividing by the number of color channels.  The
    // remainder is the color channel index, 0 to 3.  DOF ports are
    // mapped in R,G,B or R,G,B,W order.
    auto [led, color] = std::div(port, nColorChannels);
    if (led >= 0 && led < nLeds)
    {
        // Flush the DOF write cache if we're writing to a new LED, or
        // we're re-writing the same channel of the last LED.  In the
        // first case, we assume that DOF has finished with its grouped
        // update of the last LED.  In the second case, we assume that
        // DOF is starting a new write to the same LED, so we should
        // send out the old one.
        uint32_t mask = dofCacheMask[color];
        if (dofCache.led >= 0 && (dofCache.led != led || (dofCache.mask & mask) != 0))
            FlushDOFCache();

        // cache the write
        dofCache.led = led;
        dofCache.val |= static_cast<uint32_t>(level) << dofBitOffset[color];
        dofCache.mask |= mask;
        dofCache.tWrite = time_us_64();

        // if this LED is fully written, flush the cache
        if (dofCache.mask == dofCache.fullMask)
            FlushDOFCache();
    }
}

uint8_t WS2812::GetDOF(int port) const
{
    auto [led, color] = std::div(port, nColorChannels);
    if (led >= 0 && led < nLeds)
    {
        // pull out the color component from the level[] array for the target LED
        return static_cast<uint8_t>((level[led] >> dofBitOffset[color]) & 0xFF);
    }
    else
    {
        // invalid port, so return a fixed 0x00 level
        return 0x00;
    }
}

void WS2812::SetGRBW(int led, uint32_t grbw)
{
    if (led >= 0 && led < nLeds)
        level[led] = grbw;
}

// Populate an output port query result buffer
void WS2812::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &s : strips)
    {
        if (s != nullptr)
        {
            descs->configIndex = s->configIndex;
            descs->devType = PinscapePico::OutputPortDesc::DEV_WS2812;
            descs->numPorts = s->nLeds * s->nColorChannels;
            descs->numPortsPerChip = s->nColorChannels;
            descs->pwmRes = 256;
            descs->addr = 0;   // these devices have no equivalent of a bus address
            ++descs;
        }
    }
}

// Populate physical output port descriptions
void WS2812::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &s : strips)
    {
        if (s != nullptr)
        {
            int n = s->nLeds * s->nColorChannels;
            for (int i = 0 ; i < n ; ++i, ++descs)
                descs->type = descs->TYPE_PWM;
        }
    }
}

// Populate an output level query result buffer
void WS2812::PopulateLevels(PinscapePico::OutputDevLevel* &dst)
{
    for (auto &s : strips)
    {
        if (s != nullptr)
        {
            int n = s->nLeds * s->nColorChannels;
            for (int i = 0 ; i < n ; ++i, ++dst)
                dst->level = s->GetDOF(i);
        }
    }
}

// Console command handler - main static entrypoint
void WS2812::Command_main_S(const ConsoleCommandContext *c)
{
    // check for a <strip> index
    int stripNum = 0;
    int firstOptionIndex = 1;
    if (c->argc >= 2)
    {
        // try parsing the strip number
        int acc = 0;
        bool ok = false;
        bool haveChain = false;
        for (const char *p = c->argv[1] ; ; ++p)
        {
            if (*p == 0)
            {
                stripNum = acc;
                ok = true;
                break;
            }
            else if (isdigit(*p))
            {
                // accumulate the next digit
                acc = acc*10 + (*p - '0');
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
            stripNum = 0;
    }

    // look up the strip
    if (stripNum < 0 || stripNum >= strips.size() || strips[stripNum] == nullptr)
        return c->Printf("ws2812: no such strip number (%d)\n", stripNum);

    // get the strip object
    auto *strip = strips[stripNum].get();
    
    // proceed to the member function for the strip
    strip->Command_main(c, firstOptionIndex);
}

void WS2812::Command_main(const ConsoleCommandContext *c, int firstOptionIndex)
{
    // at least one argument is required
    int argi = firstOptionIndex;
    if (argi >= c->argc)
        return c->Usage();

    // scan arguments
    for ( ; argi < c->argc ; ++argi)
    {
        const char *a = c->argv[argi];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            uint64_t dt = time_us_64() - stats.t0;
            c->Printf(
                "ws2812 - strip #%d\n"
                "  DMA TX cycles:        %llu\n"
                "  Average DMA TX time:  %llu us\n"
                "  Average cycle time:   %llu us (%llu Hz)\n"
                "  DOF cache timeouts:   %llu\n",
                configIndex,
                stats.nCycles,
                stats.nCycles != 0 ? stats.dmaTotalTime/stats.nCycles : 0,
                stats.nCycles != 0 ? dt/stats.nCycles : 0,
                dt != 0 ? 1000000ULL*stats.nCycles/dt : 0,
                stats.nAgedOut);
        }
        else if (strcmp(a, "--reset-stats") == 0)
        {
            stats.Reset();
            c->Printf("ws2812 chain %d statistics reset\n", configIndex);
        }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--levels") == 0)
        {
            if (nColorChannels == 4)
            {
                for (int i = 0 ; i < nLeds ; ++i)
                {
                    uint32_t l = level[i];
                    c->Printf("  %2dR: %3d, %2dG: %3d, %2dB: %3d, %2dW: %3d  (#%08lX)\n",
                              i + 1, (l >> 16) & 0xff,
                              i + 1, (l >> 24) & 0xff,
                              i + 1, (l >> 8) & 0xff, l & 0xff,
                              l);
                }
            }
            else
            {
                for (int i = 0 ; i < nLeds ; ++i)
                {
                    uint32_t l = level[i];
                    c->Printf("  %2dR: %3d, %2dG: %3d, %2dB: %3d  (#%06lX)\n",
                              i + 1, (l >> 16) & 0xff,
                              i + 1, (l >> 24) & 0xff,
                              i + 1, (l >> 8) & 0xff,
                              l);
                }
            }
        }
        else if (isdigit(a[0]))
        {
            // parse it as a port=level setting:  <lednum><color>=<level>, <color> in R,G,B,W
            const char *p = a;
            int ledNum = 0;
            int colorNum = -1;
            int level = -1;

            // parse the port number
            for ( ; isdigit(*p) ; ledNum = ledNum*10 + *p++ - '0') ;

            // If there's a color channel character, it's a DOF port level setting: "1R=255".
            // Otherwise, it's an LED number and HTML hex color code setting: "3=#ff00ff"
            if (*p && *(p+1) == '#')
            {
                // LED port and hex color code - parse the color code
                uint32_t color = 0;
                p += 2;
                auto ParseHexDigit = [&color, &p]() {
                    color <<= 4;
                    char c = *p++;
                    if (isdigit(c))
                        color += (c - '0');
                    else if (c >= 'a' && c <= 'f')
                        color += (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        color += (c - 'A' + 10);
                    else
                        return false;

                    return true;
                };
                for (int i =  nColorChannels*2 ; i > 0 ; --i)
                {
                    if (!ParseHexDigit())
                        return c->Printf("Invalid hex color code in set (%s)\n", a);
                }

                // validate the LED number
                if (ledNum < 1 || ledNum > nLeds)
                    return c->Printf("Invalid LED number in set (%s); must be 1 to %d\n", a, nLeds);

                // suspend output management
                OutputManager::SuspendIfActive(c);

                // set the level
                if (nColorChannels == 4)
                    SetRGBW(ledNum - 1, color);
                else
                    SetRGB(ledNum - 1, color);

                // report success
                c->Printf("ws2812[%d] LED %d -> %08x\n", configIndex, ledNum, GetRGBW(ledNum - 1));
            }
            else
            {
                // DOF port level setting - parse the color channel code
                switch (*p)
                {
                case 'r': case 'R': colorNum = 0; ++p; break;
                case 'g': case 'G': colorNum = 1; ++p; break;
                case 'b': case 'B': colorNum = 2; ++p; break;
                case 'w': case 'W': colorNum = 3; ++p; break;
                }
                
                // parse the '='
                if (*p == '=')
                {
                    // skip the '=' and parse the level
                    for (++p, level = 0 ; isdigit(*p) ; level = level*10 + *p++ - '0') ;
                }
                
                // validate syntax
                if (ledNum < 1 || ledNum > nLeds)
                    return c->Printf("Invalid LED number in set (%s); must be 1 to %d\n", a, nLeds);
                if (colorNum < 0 || colorNum == 3 && nColorChannels == 3)
                    return c->Printf("Invalid color channel name in set (%s); must be in R,G,B%s\n", a, nColorChannels == 3 ? "" : ",W");
                if (level < 0 || level > 255)
                    return c->Printf("Invalid brightness level in set (%s); must be 0 to 255\n", a);
                if (*p != 0)
                    return c->Printf("Invalid syntax in set (%s); extraneous extra text at end\n", a);
                
                // suspend output management
                OutputManager::SuspendIfActive(c);
                
                // translate the LED number + color channel to a DOF port number
                int portNum = ((ledNum - 1) * nColorChannels) + colorNum;
                
                // set the port
                SetDOF(portNum, level);

                // report success
                static const char *colorName[]{ "Red", "Green", "Blue", "White" };
                c->Printf("ws2812[%d] LED %d %s -> %d\n", configIndex, ledNum, colorName[colorNum], level);
            }
        }
        else
        {
            return c->Printf("74hc595: invalid option \"%s\"\n", a);
        }
    }
}

