// Pinscape Pico - Pico ADC device
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements the ADC Manager's abstract device interface for the Pico's
// on-board ADC.

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <pico/stdlib.h>
#include <hardware/adc.h>
#include "ADCManager.h"

//
// ADC device class implementation for Pico on-board ADC
//
class PicoADC : public ADC
{
public:
    // The Pico's on-board ADC is single-ended (it can quantize voltages
    // from 0 to V[ref]), and reports with 12-bit precision (0..4095).
    PicoADC() : ADC(0, 4095, "pico_adc", nullptr, "Pico on-board ADC") { inst = this; }

    // get the singleton instance
    static PicoADC *GetInst() { return inst; }

    // is the device configured?
    bool IsConfigured() const { return gpios.size() != 0; }

    // Configure the on-board ADC
    static void Configure(JSONParser &json);

    // Start/stop sampling
    virtual void EnableSampling() override;

    // Get the number of logical channels configured
    virtual int GetNumLogicalChannels() const override { return static_cast<int>(gpios.size()); }

    // Read the latest sample, in native device units/normalized UINT16 units
    virtual Sample ReadNative(int channel) override;
    virtual Sample ReadNorm(int channel) override;

    // Is the given GPIO port number a valid ADC input?  The only
    // GPIO ports that can be muxed to the ADC are 26-29.
    static bool IsValidADCGPIO(int gpio) { return gpio >= 26 && gpio <= 29; }

    // Is the given GPIO port number a valid ADC temperature input?  We use
    // the non-existent GPIO port 30 to refer to the temperature sensor
    // input on ADC channel 4.  We treat this as an invalid port for the
    // purposes of IsValidADCGPIO(), because it's not really a GPIO - it's a
    // separate input only to the ADC.
    static bool IsValidADCTemperatureInput(int gpio) { return gpio == 30; }

protected:
    // singleton instance
    static PicoADC *inst;

    // DMA transfer IRQ handler.  This fires at the end of each DMA cycle.
    // We set up the next transfer, recycling the buffer.
    static void __not_in_flash_func(SDMAIRQ)() { inst->DMAIRQ(); }
    void DMAIRQ();
    void DMAIRQCheckChannel(int channel, int otherChannel, uint16_t *bufBase);

    // DMA buffer.  We continuously collect samples here in a ring.
    // Readers take an average of recent samples on demand.
    int dmaBufCount = 0;
    std::unique_ptr<uint16_t> dmaBuf;

    // DMA channels
    int dmaChannelA = -1;
    int dmaChannelB = -1;

    // current active channel (set to dmaChannelA or dmaChannelB at any given time)
    volatile int dmaChannelCur = 0;

    // DMA loop timeout time - the interrupt handler updates this to the current
    // time plus a timeout interval on each IRQ service
    volatile uint64_t dmaLoopTimeout = 0;

    // DMA loop stall detected in interrupt handler
    volatile bool dmaLoopStalled = false;

    // start/restart the DMA loop
    void StartDMALoop();

    // GPIO ports.  All ports must be GPIO-capable pins, GP 26-29.  If
    // multiple pins are configured, we use the ADC's "round robin"
    // mode, where it cycles through the configured inputs, sampling one
    // on each reading and then moving to the next.
    std::list<int> gpios;

    // The round-robin mask, if multiple ports are enabled
    uint roundRobinChannelMask = 0;

    // Pico ADC hardware initialization completed.  We do this the first
    // time sampling is enabled.
    bool inited = false;

    // number of channels
    int numChannels = 0;
};


