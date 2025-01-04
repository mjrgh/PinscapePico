// Pinscape Pico - Potentiometer Plunger Sensor
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file implements the plunger sensor interface for a potentiometer.
//
// Device selection: Use a slide potentiometer, 80mm to 100mm slide range,
// with a LINEAR tape.  The linear taper is very important; the main
// alternative is "audio" taper, and that'll yield poor results.  Check the
// device data sheet before buying and make sure it specifies a linear taper
// for its resistance-vs-position response curve.
//
// Physical setup: Mechanically attach the wiper to the plunger so that the
// wiper moves in lock step with the plunger.  Wire the fixed ends of the
// potentiometer to +3.3V and GND (respectively), and connect the wiper to
// an ADC input.
//
// You can use a Pico ADC GPIO input, but you'll get better results (less
// noise and smoother motion) with a higher resolution outboard ADC.

#pragma once

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "Plunger.h"

// forwards/externals
class JSONParser;
class ADC;


class PotPlungerSensor : public Plunger::Sensor
{
public:
    // constructor
    PotPlungerSensor(ADC *adc, int channel);

    // display name for log messages
    virtual const char *FriendlyName() const override { return friendlyName; }

    // Priority for picking the default.  Give the Pico's built-in ADC
    // the lowest priority, and give outboard ADCs slightly higher
    // priority, but still lower than other devices.  We give higher
    // priority to outboard ADCs because they're more suitable for
    // pot-based plungers, given the relatively low quantization quality
    // of the RP2040 ADC, so if you're going to use a pot plunger at
    // all, and you've installed an outboard ADC, then you probably
    // intend to use it with the plunger.
    virtual int PriorityForPickingDefault() const;

    // sensor type for feedback reports
    virtual uint16_t GetTypeForFeedbackReport() const override { return PinscapePico::FeedbackControllerReport::PLUNGER_POT; }

    // Configure
    virtual bool Configure(const JSONParser::Value *val) override;

    // calibration data filename
    virtual const char *GetCalFileName() const override { return "PotPlunger.cal"; }

    // samples are always ready
    virtual bool IsReady() override { return true; }

    // read the sensor
    virtual bool ReadRaw(Plunger::RawSample &r) override;

    // Figure the average scan time in microseconds.  We'll just report
    // the inverse of the sampling frequency that the ADC reports to us.
    virtual uint32_t GetAvgScanTime() override { return 1000000UL / adcSamplesPerSecond; }

    // the native scale depends upon the underlying ADC
    virtual uint32_t GetNativeScale() const override;

    // we don't have any extra sensor-specific data to report, since the
    // raw reading represents the full extent of the current sensor state
    virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) override { return 0; }

private:
    // ADC device and logical channel we're reading from
    ADC *adc;
    int channel;

    // friendly name of the potentiometer plunger sensor; we build this based
    // on the ADC's display name, as in "Potentiometer (ADS1115 channel 2)"
    const char *friendlyName;

    // ADC sampling rate
    int adcSamplesPerSecond = 1;

    // ADC native value range
    int32_t adcRangeMin = 0;
    int32_t adcRangeMax = 65535;

    // last new sample and its timestamp
    uint16_t sample = 0;
    uint64_t sampleTime = 0;
};
