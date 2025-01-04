// Pinscape Pico - Proximity/Distance Plunger Sensors
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file implements the plunger sensor interface for proximity and
// distance sensors:
//
//   VCNL4010/4020/3020
//   VL6180X
//
// These classes implement plunger interfaces to these chips.  Each chip
// also has a separate "device driver" under the Chips section of the
// source hierarchy, where the chip's low-level hardware interface is
// implemented.  The classes defined here are essentially adapters that
// implement the abstract Plunger::Sensor interface concretely for each
// device type.  This division of labor makes the device drivers more
// general-purpose, in case we ever want to use them for some function
// unrelated to plungers.
//
// The VCNLxxxx sensors are proximity sensors rather than distance
// sensors.  They don't measure distance directly; instead, they measure
// the intensity of a reflected IR signal.  The reflected intensity
// varies with distance, so it can be used as a proxy for distance, but
// it takes a little bit of work to calculate the correspondence,
// because the relationship between distance and reflected intensity
// isn't linear.  The abstract plunger class requires a linear distance
// measurement, so most of the work in the adapter class involves
// converting the sensor's intensity readings to linear distances.
//
// The VL6180X directly measures linear distance, so its adapter class
// just has to implement the abstract interface.
//

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <functional>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "Plunger.h"
#include "../USBProtocol/FeedbackControllerProtocol.h"

// forward/external declarations
class JSONParser;
class ConsoleCommandContext;


// ---------------------------------------------------------------------------
//
// VCNL4010/4020/3020 plunger sensor interface
//
class VCNL4010Plunger : public Plunger::Sensor
{
public:
    VCNL4010Plunger();
    
    // display name for log messages
    virtual const char *FriendlyName() const override { return "VCNL4010"; }

    // configure from JSON data
    virtual bool Configure(const JSONParser::Value *val) override;

    // is a reading ready?
    virtual bool IsReady() override;

    // read
    virtual bool ReadRaw(Plunger::RawSample &s) override;

    // we do the jitter filtering on the sensor data, so don't apply it to the raw sample
    virtual bool UseJitterFilter() const override { return false; }

    // calibration file name
    virtual const char *GetCalFileName() const override { return "vcnl4010.cal"; }

    // sensor type for feedback controller reports
    virtual uint16_t GetTypeForFeedbackReport() const { return PinscapePico::FeedbackControllerReport::PLUNGER_VCNL4010; }

    // report sensor-specific data in the vendor interface
    virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) override;

    // Get the native scale.  The native scale of the underlying sensor
    // hardware isn't relevant for this particular sensor, because the
    // physical quantity that the sensor reports is reflected light
    // intensity.  That isn't linear with respect to distance, hence
    // there's simply is no "scaling factor" that can convert from the
    // sensor's native units to distance units.  Instead, we have to do
    // an internal non-linear calculation to figure the distance based
    // on the sensor reading.  Since the distance figure that we pass up
    // to the abstract interface is something that we have to synthesize
    // from the sensor readings, we're free to choose any unit system we
    // like for the distance values.  The one we choose maps 1:1 to
    // logical Z Axis distance units, where the distance between the
    // rest position and the maximum retraction position equals 32767
    // units.  Since the raw reading is reported as unsigned, we have to
    // offset the scale by the distance from maximum forward position to
    // the rest position.  The scale has to be independent of
    // calibration, though, so we'll use an idealized figure based on
    // the standard mechanical setup, where the rest position is at
    // about 1/6 of the total travel distance.  This makes the overall
    // travel range (with 1/6 in front of the rest position and 5/6
    // behind it) 39320 units.
    virtual uint32_t GetNativeScale() const override { return 39320; }

    // calibration notifications
    using CalibrationData = Plunger::CalibrationData;
    virtual void OnRestoreCalibration(const CalibrationData &) override;
    virtual void BeginCalibration(CalibrationData &) override;
    virtual void EndCalibration(CalibrationData &) override;

protected:
    // Convert from raw sensor count values to distance units.
    //
    // Distance units are normalized for use in a mechanical plunger system.
    // 32767 distance units equals the calibrated distance between the
    // plunger's rest position and the maximum retraction position.  The
    // maximum forward position of the plunger is fixed at 0 units, the park
    // position is fixed at 6553 units, and the maximum retracted position is
    // 39320 units.
    uint32_t CountToDistance(int count);

    // Calculate the scaling factor for count -> distance conversions.
    // This uses the data collected during calibration to figure the
    // conversion factors.
    void CalcScalingFactor();

    // Convert a raw prox count from the sensor to a position reading,
    // using the current calibration data.
    uint32_t ProxCountToPosition(uint32_t proxCount);

    // last raw proximity count reading from the sensor
    uint16_t lastProxCount = 0;

    // Power law function for the relationship between sensor count readings
    // and distance.  For our distance calculations, we use this relationship:
    //
    //    distance = <scaling factor> * 1/PowerLaw(count - <DC offset>) + <scaling offset>
    //
    // where all of the constants in <angle brackets> are determined
    // through calibration.
    //
    // The default power law is 1/sqrt(x), which is the best fit for the data
    // I've observed in my own testing.  The actual relationship might vary by
    // setup, though, so I'm making it possible to configure different
    // functions here.  The Vishay data sheet includes a plot of their own
    // observed distance-vs-count curve for their reference setup, and while
    // they don't state a best-fit formula, it looks like they saw something
    // like 1/x^0.31 rather than my 1/x^0.5.  1/sqrt is what you'd expect from
    // the classic point-source-at-a-distance physics problem, but I think the
    // fact that we have a reflector involved changes the geometry too much
    // for that solution to apply straightforwardly, so 1/sqrt might not work
    // for everyone.
    static float SqrtPowerLaw(int x) { return sqrtf(static_cast<float>(x)); }
    static float CubePowerLaw(int x) { return pow(static_cast<float>(x), 1.0f/3); }
    std::function<float(int)> powerLawFunc = &SqrtPowerLaw;

    // flag: calibration is in progress; timestamp of start of calibration
    bool calibrating = false;
    uint64_t calibrationStartTime = 0;

    // Initial park position average.  We use the average of the first
    // 1000ms of readings to calculate the parking position.
    uint64_t calParkSum = 0;
    uint32_t calParkCnt = 0;

    // minimum and maximum observed proximity counts during calibration
    uint16_t minProxCount = 0;
    uint16_t maxProxCount = 0;

    // proximity count observed at "park" position during calibration
    uint16_t parkProxCount = 0;

    // DC Offset for converting from count to distance.  Per the Vishay
    // application notes, the sensor brightness signal contains a fixed
    // component that comes from a combination of physical factors such
    // as internal reflections, ambient light, ADC artifacts, and sensor
    // noise.  This must be subtracted from the reported proximity count
    // to get a measure of the actual reflected brightness level.  The
    // DC offset depends on external factors, which makes it unique to
    // each installation, so it can be determined empirically, through
    // the calibration process.
    uint16_t dcOffset = 0;

    // Scaling factor and offset for converting from count to distance.
    // We calculate these based on the counts collected at known points
    // during calibration.
    float scalingFactor = 0;
    float scalingOffset = 0;
};


// ---------------------------------------------------------------------------
//
// VL6180X plunger sensor interface
//
class VL6180XPlunger : public Plunger::Sensor
{
public:
    // construction
    VL6180XPlunger();

    // display name for log messages
    virtual const char *FriendlyName() const override { return "VL6180X"; }

    // configure from JSON data
    virtual bool Configure(const JSONParser::Value *val) override;

    // is a reading ready?
    virtual bool IsReady() override;

    // read
    virtual bool ReadRaw(Plunger::RawSample &s) override;

    // calibration file name
    virtual const char *GetCalFileName() const override { return "vl6180x.cal"; }

    // sensor type for feedback controller reports
    virtual uint16_t GetTypeForFeedbackReport() const { return PinscapePico::FeedbackControllerReport::PLUNGER_VL6180X; }

    // Get the native scale.  The VL6180X reports distance readings in
    // millimeters, in 1mm increments, with a maximum reading of 255.
    virtual uint32_t GetNativeScale() const override { return 255; }

    // report sensor-specific data in the vendor interface - nothing
    // extra for this sensor
    virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) override { return 0; }

protected:
};
