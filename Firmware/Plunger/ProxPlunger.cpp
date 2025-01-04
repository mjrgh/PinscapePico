// Pinscape Pico - Proximity/Distance Plunger Sensors
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

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
#include "Logger.h"
#include "Plunger.h"
#include "ProxPlunger.h"
#include "Devices/ProxSensor/VCNL4010.h"
#include "Devices/DistanceSensor/VL6180X.h"


// ---------------------------------------------------------------------------
//
// VCNL4010/VCNL4020/VCNL3020 proximity sensor
//

VCNL4010Plunger::VCNL4010Plunger()
{
    // Calculate the scaling factor for a default proximity count range.
    // The maximum count is fixed at 65535, and the theoretical minimum
    // is zero, but we can't use zero as the range minimum because the
    // inverse relationship between count and distance would cause a
    // divide-by-zero with a minimum of zero.  The physical minimum has
    // to be non-zero anyway due to the inherent DC offset in the signal.
    // So pick a minimum that's well below the typical physical minimum.
    minProxCount = 100;
    maxProxCount = 65535;
    parkProxCount = 20000;
    CalcScalingFactor();
}


bool VCNL4010Plunger::Configure(const JSONParser::Value *val)
{
    // Make sure that the underlying VCNL4010 device was configured
    if (vcnl4010 == nullptr)
    {
        Log(LOG_ERROR, "Plunger: VCNL4010 device is not configured; use the \"vcnl4010\" "
            "top-level JSON key to define the device configuration\n");
        return false;
    }
    
    // Set up the power-law function.  The power-law exponent the caller
    // provides is the X in Count = 1/Distance^X.  We need to calculate
    // the inverse of that, Distance = 1/Count^(1/X).  We have a special
    // case for X=2, since we can use the more efficient sqrt() function
    // to implement that instead of the more general pow() - since that
    // function is installed by default, we don't need to do anything if
    // X == 2.
    float powerLawExponent = val->Get("powerLaw")->Float(2.0f);
    if (powerLawExponent != 2.0f)
    {
        float invExp = 1.0f / powerLawExponent;
        powerLawFunc = [invExp](int x) { return pow(static_cast<float>(x), invExp); };
    }

    // configured
    return true;
}

// is a reading ready?
bool VCNL4010Plunger::IsReady()
{
    return vcnl4010->IsSampleReady();
}

// read
bool VCNL4010Plunger::ReadRaw(Plunger::RawSample &s)
{
    // retrieve a proximity count sample from the sensor
    bool isNew = vcnl4010->Read(lastProxCount, s.t);

    // collect extrema when in calibration mode
    if (calibrating)
    {
        // Calibration mode - note the new min/max
        if (lastProxCount < minProxCount)
            minProxCount = lastProxCount;
        if (lastProxCount > maxProxCount)
            maxProxCount = lastProxCount;

        // average the parking position over the first 1000ms
        if (time_us_64() - calibrationStartTime < 1000000)
        {
            calParkSum += lastProxCount;
            calParkCnt += 1;
        }

        // recalculate the scaling factor for the new limits
        CalcScalingFactor();
    }
    else
    {
        // Not calibrating

        // Apply jitter filtering to the sensor reading.  We filter the
        // sensor reading before conversion to a distance reading, because
        // the hysteresis filter works best at the analog-to-digital stage,
        // which is what the prox count represents.  Distance is inversely
        // related to the prox count, so it's no longer linear with respect
        // to the analog quantity being measured, which distorts the curve
        // too much for straightforward hysteresis filtering to work.  So
        // it's better to filter before the distance conversion.
        uint32_t filtered = plunger.ApplyJitterFilter(lastProxCount);
        lastProxCount = (filtered > 65535 ? 65535 : filtered);
    }

    // If the count at or below the DC offset, it means that the sensor
    // wasn't able to detect the object at all, so the plunger has moved
    // out of sensor range.  We know that the plunger can't actually be
    // missing, so this just means that it's pulled back beyond the
    // sensor's reach.  Peg this as the maximum retraction point, which
    // we represent as 39320 distance units.
    if (lastProxCount <= dcOffset)
    {
        s.rawPos = 39320;
        return isNew;
    }

    // convert to distance units
    s.rawPos = ProxCountToPosition(lastProxCount);
    return isNew;
}

// report sensor-specific data in the vendor interface
size_t VCNL4010Plunger::ReportSensorData(uint8_t *buf, size_t maxSize)
{
    // make sure we have room in the buffer
    using PR = PinscapePico::PlungerReadingVCNL4010;
    if (maxSize < sizeof(PR))
        return 0;

    // build our custom struct in the buffer
    auto *p = reinterpret_cast<PR*>(buf);
    p->cb = sizeof(PR);
    p->sensorType = PinscapePico::FeedbackControllerReport::PLUNGER_VCNL4010;
    p->proxCount = lastProxCount;

    // return the struct size
    return sizeof(PR);
}

// restore calibration data
void VCNL4010Plunger::OnRestoreCalibration(const CalibrationData &cd)
{
    // restore our calibration points from the private data if the
    // struct contains valid calibration data
    if (cd.calibrated)
    {
        // calibration data is present - use the stored values
        minProxCount = cd.raw[0];
        maxProxCount = cd.raw[1];
        parkProxCount = cd.raw[2];
    }
    else
    {
        // no calibration data available - use defaults
        minProxCount = 100;
        maxProxCount = 65535;
        parkProxCount = 20000;
    }

    // figure the scaling factor from the calibration points
    CalcScalingFactor();
}

// begin calibration
void VCNL4010Plunger::BeginCalibration(CalibrationData &)
{
    // Reset our internal calibration points.  A calibration should
    // always be initiated with the plunger at the rest position, so set
    // the initial park count to the initial reading.  This is also the
    // lowest reading we've seen so far.  Set the initial maximum count
    // to the sensor's actual maximum of 65535.  We haven't actually
    // seen anything higher than lastProxCount yet, but it's practically
    // a certainty in a proper installation that we'll be able to max
    // out the prox count when the plunger is at the forward end of the
    // range - this end really doesn't need calibration.  Pegging the
    // maximum to the known sensor limit also gives us a more stable
    // initial scaling factor, since we're working with a non-empty
    // range; if the initial range is empty, the scaling factor
    // calculation is unstable because it has to divide by zero.
    parkProxCount = lastProxCount;
    minProxCount = lastProxCount;
    maxProxCount = 65535;

    // calculate the initial scaling factor
    CalcScalingFactor();

    // flag that calibration is in progress
    calibrating = true;
    calibrationStartTime = time_us_64();

    // start the park position averaging
    calParkSum = lastProxCount;
    calParkCnt = 1;
}

// end calibration
void VCNL4010Plunger::EndCalibration(CalibrationData &cd)
{
    // apply the initial parking position average
    parkProxCount = static_cast<uint32_t>(calParkSum / calParkCnt);
    
    // calculate the new scaling factor
    CalcScalingFactor();

    // Set the position-units zero position to the result we get by
    // running our proxy-units park position through the distance
    // conversion calculation, so that they exactly agree.
    cd.zero = ProxCountToPosition(parkProxCount);

    // Do the same with the observed maximum.  The maximum position
    // value observed during calibration was probably at an intermediate
    // scaling factor, so it doesn't correspond to the final range.  The
    // maximum position corresponds to the minimum proximity count,
    // since the position axis is positive in the retraction direction,
    // which is away from the sensor, thus lower proximity count values.
    cd.max = ProxCountToPosition(minProxCount);

    // save our proximity count range in the private sensor section;
    // this allows us to restore it in future sessions when the generic
    // calibration data are loaded
    cd.raw[0] = minProxCount;
    cd.raw[1] = maxProxCount;
    cd.raw[2] = parkProxCount;

    // calibration done
    calibrating = false;
}

// Calculate the scaling factor for count -> distance conversions.
// This uses the data collected during calibration to figure the
// conversion factors.
void VCNL4010Plunger::CalcScalingFactor()
{
    // Don't let the minimum go below 100.  The inverse relationship
    // between proximity count and distance makes the calculation
    // meaningless at zero and unstable at very small count values, so
    // we need a reasonable floor to keep things in a usable range.  The
    // physical sensor has a practical minimum that the manufacturer's
    // application notes refer to as the DC offset, which comes from
    // internal reflections, ambient light, and detector noise.  This is
    // typically at least 2000 in my testing, and often much higher, up
    // to 20000, so a floor of 100 is quite conservative.
    if (minProxCount < 100)
        minProxCount = 100;

    // Set a ceiling of 65535, since the sensor's data format makes
    // higher readings impossible
    if (maxProxCount > 65535)
        maxProxCount = 65535;

    // Figure the scaling factor and offset over the range from the park
    // position to the maximum retracted position, which corresponds to
    // the minimum count (lowest intensity reflection) we've observed.
    // (Note the slightly confusing reversal: MIN prox count equals MAX
    // distance.)
    //
    // Do all calculations with the counts *after* subtracting out the
    // signal's DC offset, which is the brightness level registered on
    // the sensor when there's no reflective target in range.  We can't
    // directly measure the DC offset in a plunger setup, since that
    // would require removing the plunger entirely, but we can guess
    // that the minimum reading observed during calibration is
    // approximately equal to the DC offset.  The minimum brightness
    // occurs when the plunger is at the most distance point in its
    // travel range from the sensor, which is when it's pulled all the
    // way back.  The plunger travel distance is just about at the limit
    // of the VCNL4010's sensitivity, so the inverse curve should be
    // very nearly flat at this point, thus this should be a close
    // approximation of the true DC offset.
    //
    // The result is in abstract linear distance units, using a unit
    // system that we're free to define here.  The only likely consumer
    // of the distance-units output in a Pinscape system will be the
    // Plunger class, which accepts sensor output in any unit system
    // that will fit into a uint32_t.  The Plunger will ultimately
    // convert sensor output to Z Axis units, which is normalized so
    // that the distance between the calibrated park position and the
    // maximum retraction position equals exactly 32767 units.  If we
    // use the same normalization, our units will convert to the Plunger
    // class's Z Axis units 1:1, with no loss of precision.  So choose a
    // scaling factor such that (max - park) maps to 32767 distance
    // units.  All of our distance output readings have to be positive
    // because of the uint32_t container, so we have to offset our park
    // position enough that our minimum reading is positive when
    // converted to distance units.  The park position in a mechanical
    // plunger at about 1/6 of the total travel distance, so with 32767
    // units on the back side of the park position, we need 6553 units
    // on the front side.  The plunger's total travel distance is then
    // 39320 units, with the maximum forward position fixed at 0 units
    // and the maximum retraction position at 39320 units.
    // 
    const int dcOffsetDelta = 50;
    dcOffset = (minProxCount > dcOffsetDelta) ? static_cast<uint16_t>(minProxCount - dcOffsetDelta) : 0;
    int park = parkProxCount - dcOffset;
    float parkInv = 1.0f / powerLawFunc(park);
    scalingFactor = 32767.0f / (1.0f / powerLawFunc(minProxCount - dcOffset) - parkInv);
    scalingOffset = 6553.0f - (scalingFactor * parkInv);
}

// convert a prox count reading to distance units, using the calibrated
// scaling factor and DC offset
uint32_t VCNL4010Plunger::ProxCountToPosition(uint32_t proxCount)
{
    // figure the distance based on our inverse power curve
    float d =  scalingFactor/powerLawFunc(proxCount - dcOffset) + scalingOffset;

    // constrain to the valid range for return
    return d < 0.0f ? 0 : d > 39320.0f ? 39320U : static_cast<uint32_t>(d);
}


// ---------------------------------------------------------------------------
//
// VL6180X
//

VL6180XPlunger::VL6180XPlunger()
{
}

// configure from JSON data
bool VL6180XPlunger::Configure(const JSONParser::Value *val)
{
    // Make sure that the underlying VL6180X device was configured
    if (vl6180x == nullptr)
    {
        Log(LOG_ERROR, "Plunger: VL6180X device is not configured; use the \"vl6180x\" "
            "top-level JSON key to define the device configuration\n");
        return false;
    }

    // success
    return true;
}

// is a reading ready?
bool VL6180XPlunger::IsReady()
{
    return vl6180x->IsSampleReady();
}

// read the sensor
bool VL6180XPlunger::ReadRaw(Plunger::RawSample &s)
{
    // read a sample from the sensor
    return vl6180x->Read(s.rawPos, s.t);
}
