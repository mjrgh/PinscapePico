// Pinscape Pico - ADC management
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>
#include <hardware/adc.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>

#include "Pinscape.h"
#include "Utils.h"
#include "Logger.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "Utils.h"
#include "USBIfc.h"
#include "Devices/ADC/PicoADC.h"
#include "Devices/ADC/ADS1115.h"
#include "ADCManager.h"


// ---------------------------------------------------------------------------
//
// ADC manager
//

// global singleton
ADCManager adcManager;

// construction
ADCManager::ADCManager()
{
}

// configure
void ADCManager::Configure(JSONParser &json)
{
    // configure the ADC chips
    PicoADC::Configure(json);
    ADS1115::Configure(json);
}

// USB HID logical axis source for reading directly from an ADC
class ADCAxisSource : public LogicalAxis
{
public:
    ADCAxisSource(const CtorParams &params, ADC *adc, int channel) : adc(adc), channel(channel)
    {
        // make sure sampling is enabled on the device
        adc->EnableSampling();
    }

    // ADC devices return values normalized to UINT16.  Our HID interfaces
    // all use a normalized signed INT16 range for axis usage.  So to
    // convert an ADC to a logical axis, offset the ADC range so that the
    // center of the UINT16 range, +32768, translates to the center of the
    // logical axis range, 0.
    virtual int16_t Read() override { return static_cast<int16_t>(static_cast<int32_t>(adc->ReadNorm(channel).sample) - 32768); }

    // ADC and the logical channel to read
    ADC *adc;
    int channel;
};

// add a device
void ADCManager::Add(ADC *adc)
{
    // add it to our internal list
    adcs.emplace_back(adc);

    // Add a HID device logical axis entry for each channel, for direct
    // mapping from the input channels to analog-axis HID usages, such as
    // joystick axes and slider controls.  Name each channel as 'key[n]',
    // where 'key' is the device config key and 'n' is the channel number,
    // starting at zero.
    auto Add = [adc](int channel, const char *name) {
        LogicalAxis::AddSource(name, [adc, channel](const LogicalAxis::CtorParams &params, std::vector<std::string>&) -> LogicalAxis* {
            return new ADCAxisSource(params, adc, channel);
        });
    };
    int nChannels = adc->GetNumLogicalChannels();
    const char *key = adc->ConfigKey();
    const char *altKey = adc->AltConfigKey();
    for (int channel = 0 ; channel < nChannels ; ++channel)
    {
        // add it as 'key[n]'
        char name[64];
        snprintf(name, sizeof(name), "%s[%d]", key, channel);
        Add(channel, name);

        // if the sensor has an alternate key, add that as well
        if (altKey != nullptr)
        {
            snprintf(name, sizeof(name), "%s[%d]", altKey, channel);
            Add(channel, name);
        }
    }

    // also add a name under just the un-indexed config key for the first
    // channel, so that devices that the user doesn't have to think about
    // this as an array for devices with only one channel
    if (nChannels == 0)
    {
        Add(0, key);
        if (altKey != nullptr)
            Add(0, altKey);
    }
}

// enumerate devices
void ADCManager::Enumerate(std::function<void(ADC*)> func)
{
    for (auto &adc : adcs)
        func(adc.get());
}

// claim the Pico on-board ADC
bool ADCManager::ClaimPicoADC(const char *subsystemName)
{
    // if another subsystem already owns it, log an error and fail
    if (picoAdcOwner != nullptr)
    {
        Log(LOG_ERROR, "%s requires exclusive access to the Pico's built-in ADC, "
            "which is already in use by %s\n", subsystemName, picoAdcOwner);
        return false;
    }

    // set the new owner and return success
    picoAdcOwner = subsystemName;
    return true;
}


