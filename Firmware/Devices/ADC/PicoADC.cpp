// Pinscape Pico - Pico ADC device
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements the ADC Manager's abstract device interface for the Pico's
// on-board ADC.
//
// We run the native ADC in continuous mode, and we can sample one or more
// of the inputs using the device's round-robin mode.  In round-robin mode,
// the hardware automatically cycles through the enabled ports sequentially,
// emitting one sample for each enabled port, then cycling back to the first
// port.
//
// The Pico ADC has a fixed integration time of 2us when using its default
// 48MHz clock input (the actual time is 96 cycles on its clock source,
// which works out to exactly 2us with a 48MHz clock).  Unfortunately, it
// doesn't have a native averaging mode, so it's up to client software to
// implement oversampling.  We do, by continuously collecting samples into
// a ring buffer, and computing an average over the most recent segment
// of the buffer whenever a client asks to read a sample.
//
// The 2us ADC cycle is too fast to collect samples with an IRQ handler.
// We'd spend about 50% of CPU time in the IRQ handler, and we'd *still*
// miss some samples due to increased interrupt latency when other
// peripherals are firing at the same time.  The only way to keep up with
// the 2us ADC cycle is to move the samples into memory without CPU
// involvement, via DMA.  To keep DMA running continuously, we use two
// channels, which we designate A and B.  Channel A collects samples into
// the lower half of a ring buffer, and channel B collects samples into
// the upper half of the buffer.  We still need an IRQ handler, but the
// IRQ handler is for DMA completion rather than ADC completion, so it
// only fires once ever N/2 samples (where N is the buffer size in
// samples).  If we make N = 1024, this gives us 512*2us = 1ms between
// interrupts, which (a) reduces interrupt overhead to negligible levels,
// and (b) gives us an extremely long latency cushion to respond to
// interrupts, since we only have to response to the Channel A interrupt
// before Channel B finishes 1ms later, and vice versa.  This gives us
// essentially deterministic timing that ensures we never miss a single
// sample.  That's only a nice-to-have when reading a single channel,
// where a missed sample (or even a large number of missed samples) will
// have negligible impact on the averages.  But it's critical in
// round-robin mode, because the interleaving scheme means that clients
// will be reading from the wrong channel if we ever miss even a single
// sample.  That utterly randomizes the results, so it can't be allowed
// to happen, ever, even rarely.  We need perfectly deterministic timing,
// which is really only possible in this environment with the dual DMA
// scheme just described.

#include <stdlib.h>
#include <stdint.h>
#include <pico/stdlib.h>
#include <hardware/adc.h>
#include "ADCManager.h"
#include "PicoADC.h"
#include "Logger.h"
#include "GPIOManager.h"


// singleton instance
PicoADC *PicoADC::inst = nullptr;

// JSON configuration for the ADC device instance
//
// pico_adc: {
//   gpio: 26,           // GPIO pin used for input; must be one of the ADC-capable ports, 26..30
//                       // Port 30 is the Pico's internal temperature sensor on ADC channel 4
// }
//
// The GPIO port can be specified as a single port number, or as a list
// of port numbers (e.g., gpio: [26, 27, 28]).  If multiple ports are
// specified, the ADC runs in round-robin mode, sampling the ports
// sequentially.  Ports must be listed in ascending port number order,
// and a port can't be repeated.
//
// When multiple ports are listed, the ports correspond to the logical
// channels in Read(channel).  The first port listed is channel 0, the
// second is channel 1, etc.
//
void PicoADC::Configure(JSONParser &json)
{
    // Always create a global instance, since this is a fixed hardware
    // feature.  It need not be configured, but the global instance is still
    // useful for resolving conflicting claims for subsystems that access
    // the ADC hardware directly (rather than through this class).
    inst = new PicoADC();

    // check for an explicit configuration
    if (const auto *val = json.Get("pico_adc") ; !val->IsUndefined())
    {
        // Get the GPIO port(s)
        val->Get("gpio")->ForEach([](int index, const JSONParser::Value *value) { inst->gpios.emplace_back(value->Int()); }, true);

        // at least one channel must be configured
        if (inst->gpios.size() == 0)
        {
            Log(LOG_ERROR, "Pico ADC: no GPIOs assigned; ADC not configured\n");
            return;
        }

        // validate and claim the ports
        int gpioPrv = 0;
        for (int gpio : inst->gpios)
        {
            // validate that it's a valid ADC input, either a GPIO input or
            // the special temperature sensor input
            if (!IsValidADCGPIO(gpio) && !IsValidADCTemperatureInput(gpio))
            {
                Log(LOG_ERROR, "Pico ADC: invalid GPIO pin %d; must be one of the ADC-capable pins, 26-29\n", gpio);
                inst->gpios.clear();
                return;
            }

            // if it's a GPIO port (not the special temperature sensor input),
            // claim the port with the GPIO manager
            if (IsValidADCGPIO(gpio) && !gpioManager.Claim("Pico ADC", gpio))
                return;

            // GPIOs must be listed in ascending order, so that the
            // logical channel numbering is in the same order as the
            // round-robin sample collection.
            if (gpio <= gpioPrv)
            {
                Log(LOG_ERROR, "Pico ADC: GPIO ports in 'gpio' must be listed in ascending port "
                    "number order, and ports can't be repeated\n");
            }
            gpioPrv = gpio;
        }

        // set the channel count
        inst->numChannels = static_cast<int>(inst->gpios.size());

        // add it to the ADC manager's available device list
        adcManager.Add(inst);

        // log it
        char buf[128] = "";
        for (int gpio : inst->gpios)
        {
            char *p = buf + strlen(buf);
            snprintf(p, sizeof(buf) - (p - buf), "%d,", gpio);
        }
        const char *s = inst->numChannels == 1 ? "" : "s";
        Log(LOG_CONFIG, "Pico ADC configured; %d channel%s, GPIO%s %s\n",
            inst->numChannels, s, s, buf);
    }
}

// Start sampling.  The Pico ADC can only sample multiple channels
// in continuous mode via its round-robin mode, so it's not possible
// to enable channels individually; enabling one channel enables them
// all.
void PicoADC::EnableSampling()
{
    // ignore if not configured with GPIOs
    if (gpios.size() == 0)
        return;

    // initialize if we haven't already
    if (!inited)
    {
        // mark it as initialized
        inited = true;

        // allocate the DMA buffer as a multiple of the channel count, so
        // that we'll always be aligned with a full block of N channels
        // when the buffer wraps
        dmaBufCount = numChannels * 1024;
        dmaBuf.reset(new (std::nothrow) uint16_t[dmaBufCount]);
        if (dmaBuf == nullptr)
        {
            Log(LOG_ERROR, "Pico ADC: insufficient memory for DMA buffer\n");
            return;
        }

        // acquire a DMA channel
        if ((dmaChannelA = dma_claim_unused_channel(false)) < 0
            || (dmaChannelB = dma_claim_unused_channel(false)) < 0)
        {
            Log(LOG_ERROR, "Pico ADC: insufficient DMA channels\n");
            return;
        }

        // Set up the end-of-DMA interrupts
        irq_add_shared_handler(DMA_IRQ_0, &PicoADC::SDMAIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(DMA_IRQ_0, true);
        dma_channel_set_irq0_enabled(dmaChannelA, true);
        dma_channel_set_irq0_enabled(dmaChannelB, true);

        // initialize the ADC hardware
        adc_init();

        // Set the maximum sampling rate, based on the fixed integration
        // time of 96 ADC clocks @ 48MHz == 2us.
        const int rawSampleTime_us = 2;
        const int rawSamplesPerSecond = 1000000 / rawSampleTime_us;
        
        // Set the ADC clock divider:
        //
        //    period[seconds] = (1+div) / 48000000
        //    div = (period[seconds] * 48000000) - 1
        //        = (period[us] * 48) - 1
        //
        float clkdiv = (rawSampleTime_us * 48.0f) - 1.0f;
        adc_set_clkdiv(clkdiv);

        // Start the DMA loop
        StartDMALoop();

        // log the result
        Log(LOG_INFO, "Pico ADC configured; clock div %.2lf, DMA channels %d,%d, round-robin mask 0x%02x\n",
            clkdiv, dmaChannelA, dmaChannelB, roundRobinChannelMask);
    }
}

void PicoADC::StartDMALoop()
{
    // stop the ADC and clear the FIFO
    adc_run(false);
    adc_fifo_drain();
    
    // configure channel A for the first half of the buffer, chaining to channel B
    dma_channel_config dmaConfA = dma_channel_get_default_config(dmaChannelA);
    channel_config_set_read_increment(&dmaConfA, false);
    channel_config_set_write_increment(&dmaConfA, true);
    channel_config_set_transfer_data_size(&dmaConfA, DMA_SIZE_16);
    channel_config_set_dreq(&dmaConfA, DREQ_ADC);
    channel_config_set_chain_to(&dmaConfA, dmaChannelB);
    dma_channel_configure(dmaChannelA, &dmaConfA, dmaBuf.get(), &adc_hw->fifo, dmaBufCount/2, false);

    // Configure channel B for the second half of the buffer.  This channel will
    // chain back to channel A when it's done, but don't link it yet - we defer
    // that until the interrupt handler, when we RE-arm channel A.  If we link
    // B back to A while A is still running, we expose the Pico to a potential
    // crash condition if the IRQ response for A is delayed past the time when
    // B completes its transfer.  In that event, A's write pointer will still
    // be at the end of its buffer, where it could plow ahead into memory space
    // it doesn't own.  So we can't link channel B until channel A is re-armed
    // with its write pointer set correctly for a new trasnfer.
    dma_channel_config dmaConfB = dma_channel_get_default_config(dmaChannelB);
    channel_config_set_read_increment(&dmaConfB, false);
    channel_config_set_write_increment(&dmaConfB, true);
    channel_config_set_transfer_data_size(&dmaConfB, DMA_SIZE_16);
    channel_config_set_dreq(&dmaConfB, DREQ_ADC);
    dma_channel_configure(dmaChannelB, &dmaConfB, dmaBuf.get() + dmaBufCount/2, &adc_hw->fifo, dmaBufCount/2, false);

    // Set up the FIFO: DMA enabled, strip the error bit, full 12-bit
    // samples (don't right-shift to 8-bit).
    adc_fifo_setup(true, true, 1, false, false);

    // configure the GPIOs for ADC use
    roundRobinChannelMask = 0;
    for (int gpio : gpios)
    {
        // If it's a GPIO port, configure the port as an ADC input.
        // If it's the temperature sensor (virtual gpio 30), enable it.
        if (gpio >= 26 && gpio <= 29)
            adc_gpio_init(gpio);
        else if (gpio == 30)
            adc_set_temp_sensor_enabled(true);
        
        // add its bit into the round-robin input mask
        roundRobinChannelMask |= (1 << (gpio - 26));
    }

    // set the round-robin mask if more than one port is enabled
    if (gpios.size() > 1)
        adc_set_round_robin(roundRobinChannelMask);
    
    // select the first GPIO input; if we're in round-robin mode, the
    // sampler will automatically cycle through the channels enabled
    adc_select_input(gpios.front() - 26);
    
    // kick off DMA on channel A, and start the ADC in free-running mode
    dmaChannelCur = dmaChannelA;
    dma_channel_start(dmaChannelA);
    adc_run(true);

    // presume the DMA/IRQ loop is functioning (so that we don't immediately
    // repeat this setup)
    dmaLoopTimeout = time_us_64() + 1000000;
}

// Read the latest sample in native units
ADC::Sample PicoADC::ReadNative(int channel)
{
    // The DMA loop depends on the interrupts being serviced in time, so
    // it can stall if interrupts are disabled for an extended period.
    // If it's stalled, based on the last update time, restart it.  Use
    // the stall flag detected in the interrupt handler, and also check
    // the interrupt time.  (The latter is an inelegant catch-all, and
    // it's only there because I'm embarrassingly uncertain about the
    // deterministic correctness of the IRQ stall detection algorithm.
    // The provable correctness depends upon an assumption that the DMA
    // hardware's chain_to processing always marks the target channel as
    // busy before asserting the IRQ.  That appears to be true
    // empirically, but I can't tell from the data sheet if this is
    // guaranteed in the hardware design, or if it's probabilistic and
    // it's only an accident that I've only observed the one outcome.
    // The catch-all will take care of the "other" IRQ/busy ordering
    // case that might arise if my assumption about deterministic
    // ordering is wrong, and will do no harm (as the case will never
    // arise) if the assumption is correct.)
    if (dmaLoopStalled)
    {
        Log(LOG_DEBUG, "Pico ADC: DMA loop stall detected in IRQ handler; restarting\n");
        dmaLoopStalled = false;
        StartDMALoop();
    }
    else if (time_us_64() > dmaLoopTimeout)
    {
        Log(LOG_DEBUG, "Pico ADC: DMA loop stall detected by timeout; restarting\n");
        StartDMALoop();
    }

    // validate the channel
    if (channel < 0 || channel >= numChannels)
        return { 0, 0 };

    // Get the current asynchronous DMA write position.  Turn off
    // interrupts while getting the pointer so that the IRQ handler can't
    // sneak in and reset the outgoing channel to the start of the buffer,
    // which it could do if the active channel happens to finish just as
    // we're checking.
    //
    // Blocking interrupts doesn't stop the DMA write address register
    // from changing, since the DMA controller can update that at any time
    // regardless of interrupt status.  But that's okay, because the DMA
    // controller will only INCREASE the write address.  Since we're
    // explicitly setting up to read OLDER samples, it doesn't matter if
    // newer samples come in while we're working, and so it doesn't matter
    // if the write pointer moves ahead.
    //
    // The reason we have to block interrupts is that the IRQ handler will
    // do two things when a DMA channel completes: first, it'll update
    // dmaChannelCur to reflect that the other channel is running, and
    // second, it'll reset the finishing channels write_addr to the start
    // of its buffer segment.  So if we didn't turn off interrupts, we
    // could read dmaChannelCur just before an interrupt, and then get the
    // recycled write_addr after the interrupt returns, which would be
    // wrong.  Blocking interrupts ensures that we can't get a recycled
    // write_addr - we get a write_addr that can only move forward, since
    // only the DMA hardware can update it after we check dmaChannelCur.
    // So we safely get a pointer to the end of a recent section of the
    // buffer.
    const uint16_t *p;
    {
        IRQDisabler irqd;
        p = reinterpret_cast<const uint16_t*>(dma_channel_hw_addr(dmaChannelCur)->write_addr);
    }

    // Work backwards from the write position to the base of the current
    // round-robin block.  Index [0] is channel 0, [1] is channel 1, [N-1]
    // is channel N-1, [N] is channel 0, [N+1] is channel 1, etc.  So if
    // we take (index/N)*N, we always get a channel 0 sample, and the
    // corresponding sample for channel n is at (index/N)*N+n.
    int32_t index = (p - dmaBuf.get());
    int32_t baseIndex = (index / numChannels) * numChannels;
    int32_t channelIndex = baseIndex + channel;

    // Since the write pointer could be in the middle of an N-block, back
    // off by one whole block, wrapping at the bottom of the array.
    channelIndex -= numChannels;
    if (channelIndex < 0)
        channelIndex += dmaBufCount;

    // now take an average over recent samples
    static const int AverageCount = 256;
    int32_t sum = 0;
    p = dmaBuf.get() + channelIndex;
    for (int i = 0 ; i < AverageCount ; ++i)
    {
        // add it to the total
        sum += *p;

        // back up one N-block
        channelIndex -= numChannels;
        p -= numChannels;
        if (channelIndex < 0)
        {
            channelIndex += dmaBufCount;
            p += dmaBufCount;
        }
    }

    // return the average
    return { sum / AverageCount, time_us_64() };
}

// Read the latest sample in normalized UINT16 units
ADC::Sample PicoADC::ReadNorm(int channel)
{
    // read the sample in native units
    Sample s = ReadNative(channel);

    // The Pico ADC produces native unsigned 12-bit samples.  Normalize to
    // UINT16 using the "shift-and-fill" algorithm: shift left to the
    // desired width, and fill the vacated low-order bits with the same
    // number of bits from the high-order end.  This algorithm produces
    // results within +/-1 of the equivalent floating-point scaling
    // operation across the whole range, and is much faster (since it only
    // requires two bits shifts and an OR).
    s.sample = (s.sample << 4) | (s.sample >> 8);
    return s;
}

// ADC DMA IRQ handler for one channel
void __not_in_flash("IRQ") PicoADC::DMAIRQCheckChannel(int channel, int otherChannel, uint16_t *bufBase)
{
    if (dma_channel_get_irq0_status(channel))
    {
        // This completed, so the other channel is now in progress, thanks
        // to the channel chaining.
        dmaChannelCur = otherChannel;

        // clear the interrupt status flag in the channel
        dma_channel_acknowledge_irq0(channel);

        // re-arm A so that it's ready to take over again when B finishes
        dma_channel_set_trans_count(channel, dmaBufCount/2, false);
        dma_channel_set_write_addr(channel, bufBase, false);

        // this channel is now armed and ready, so enable Other->Self
        SetDMAChainTo(otherChannel, channel);

        // If the other channel isn't busy, the DMA loop might be broken,
        // since it completed before we had a chance to re-arm its
        // chain_to.  There's a slight chance that the other channel could
        // have *just now* finished, between the time we set the chain_to
        // and the time we're checking here, in which case our own channel
        // will be busy again, so we need to check both to be sure.  The
        // order of the check is important - we have to check in the order
        // of the chaining, since it could change between the two busy
        // tests, but only in one direction.
        if (!dma_channel_is_busy(otherChannel) && !dma_channel_is_busy(channel))
            dmaLoopStalled = true;

        // The other channel won't be ready again until its completion IRQ is serviced,
        // so disable Self->Other
        DisableDMAChainTo(channel);

        // update the interrupt time
        dmaLoopTimeout = time_us_64() + 1000000;
    }
}


// ADC DMA IRQ handler
void __not_in_flash("IRQ") PicoADC::DMAIRQ()
{
    // DMA_IRQ0 is a shared interrupt, so check if the interrupt
    // came from one of our channels.
    DMAIRQCheckChannel(dmaChannelA, dmaChannelB, dmaBuf.get());
    DMAIRQCheckChannel(dmaChannelB, dmaChannelA, dmaBuf.get() + dmaBufCount/2);
}

