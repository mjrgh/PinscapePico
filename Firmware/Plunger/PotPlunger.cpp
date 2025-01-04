// Pinscape Pico - Potentiometer Plunger Interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the plunger sensor concrete class for potentiometer plungers.
// This is designed to be used with straight "slide" potentiometers with
// a linear taper.  "Linear taper" means that the resistance varies
// linearly with respect to spatial distance as the slider moves from
// one end of the pot to the other end.  For a standard 1980s pinball
// plunger, a slide length of at least 80mm is required.
//
// Note that some slide potentiometers are available with both "linear"
// and "audio" tapers.  This application requires the linear taper type.
// The audio taper has a resistance curve that varies logarithmically
// with respect to slider position, which this software can't read
// properly.
//
// The reason we explicitly don't support audio-taper pots isn't that it
// would be so difficult to write the software, but rather the audio
// taper would inherently yield poor results as a linear position
// sensor.  The problem is that the log curve concentrates all of the
// distance-measuring precision at the end of the scale where the
// distance-vs-resistance curve is shallow.  At the steep end of the
// curve, each resistance step represents a large distance, so the
// precision is poor, and noise in the signal is amplified due to the
// large factor for converting from delta-ohms to delta-millimeters.
// There's just no point in supporting a device that's manifestly wrong
// for the job, when perfectly good linear pots are readily available.
//
// The wiring must be like this:
//
//  GND ----------+  potentiometer fixed end #1 terminal
//  ADC input ----+  potentiometer "Wiper" terminal
//  VREF ---------+  potentiometer fixed end #2 terminal
//
// VREF is the analog reference voltage.  When using the Pico's on-board
// ADC, use the Pico's 3.3V.  The reference voltage might be different
// for other ADC chips.
//
// With this wiring arrangement, the potentiometer forms a voltage
// divider such that the voltage at the ADC input will vary between 0V
// and VREF, linearly with respect to the wiper's distance from the GND
// terminal end.  It will read close to 0V when the wiper is at the GND
// end, close to VREF when the wiper is at the VREF end, and will be
// linearly proportional at all points in between.
// 

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "ADCManager.h"
#include "Devices/ADC/PicoADC.h"
#include "Plunger.h"
#include "PotPlunger.h"

// Construction
PotPlungerSensor::PotPlungerSensor(ADC *adc, int channel) : adc(adc), channel(channel)
{
    // build the friendly name based on the ADC name
    friendlyName = Format("Potentiomter(%s channel %d)", adc->DisplayName(), channel);

    // Remember the sensor's native range minimum - we use this to adjust
    // raw sensor readings from the sensor's INT32 range to the abstract
    // plunger's UINT32 range.  Some ADCs can return negative values that
    // represent differential voltages from a reference other than 0V/GND,
    // so we need to know the minimum in order to adjust to the unsigned
    // range used in the abstract plunger class.
    adcRangeMin = adc->GetNativeMin();
    adcRangeMax = adc->GetNativeMax();
}

bool PotPlungerSensor::Configure(const JSONParser::Value *val)
{
    // enable sampling on the ADC
    adc->EnableSampling();

    // successful configuration
    return true;
}

int PotPlungerSensor::PriorityForPickingDefault() const
{
    // Give low priority to the Pico on-board ADC.  Give slightly
    // higher, but still low, priority to any other (outboard) ADC.
    //
    // If some truly plunger-specific sensor is installed, we always
    // want to favor that over any ADC, thus the low priority for any
    // kind of ADC.
    //
    // When ONLY ADCs are available, but we have both an ouboard ADC and
    // the on-board ADC configured, it's likely that the user intended
    // to use the outboard ADC for the plunger.  The Pico's ADC is
    // relatively low fidelity, and the plunger benefits from a
    // high-quality ADC, so if you have an outboard ADC at all, then you
    // probably installed it for the sake of the plunger.
    //
    // If multiple outboard ADCs are installed, we have no basis for
    // distinguishing which one might be intended for the plunger.
    // Likewise, we have no way to guess which channel might be for the
    // plunger when multiple channels are available on one ADC.  So we
    // return the same result for all channels on a given ADC, and we
    // likewise return the same result for all types of outboard ADCs.
    // 
    return adc == PicoADC::GetInst() ? 100 : 200;
}

uint32_t PotPlungerSensor::GetNativeScale() const
{
    // figure the width of the native range
    return static_cast<uint32_t>(adcRangeMax - adcRangeMin);
}

bool PotPlungerSensor::ReadRaw(Plunger::RawSample &r)
{
    // we can only get a sample if the ADC is configured properly
    if (adc != nullptr)
    {
        // read a sample from the ADC, in its native units
        auto s = adc->ReadNative(channel);

        // adjust from the sensor INT32 range to the plunger UINT32 range
        r.rawPos = static_cast<uint32_t>(s.sample - adcRangeMin);
        r.t = s.timestamp;
        return true;
    }
    else
    {
        // no sample available
        r.rawPos = 0;
        r.t = 0;
        return false;
    }
}
