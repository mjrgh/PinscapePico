// Pinscape Pico - Quadrature Plunger Sensors
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
#include "QuadraturePlunger.h"
#include "Devices/Quadrature/QuadratureEncoder.h"

// construction
QuadraturePlungerSensor::QuadraturePlungerSensor(QuadratureEncoder *encoder, int typeCode,
    const char *friendlyName, const char *calFileName, const char *devKey) :
    encoder(encoder), typeCode(typeCode), friendlyName(friendlyName),
    calFileName(calFileName), devKey(devKey)
{
}

// configuration
bool QuadraturePlungerSensor::Configure(const JSONParser::Value *val)
{
    // get the type name for error reports
    std::string typeName = val->Get("type")->String();

    // If the encoder is null, it has to be configured separately
    if (encoder == nullptr)
    {
        Log(LOG_ERROR, "Plunger: the %s sensor configuration is missing, or initialization failed; "
            "the sensor device must be configured via its top-level key '%s:'\n", friendlyName, devKey);
        return false;
    }

    // Figure the native scale, based on the encoder's lines-per-inch
    // metric.
    // 
    // A "line" in the LPI metric is one black/white pair in a typical
    // black/white optical sensor.  The standard quadrature encoding
    // scheme lets us detect four discrete positions per bar pair, so
    // there are LPI*4 counts per inch.  For example, the AEDR-8300-1K2
    // encodes at 75 LPI, so we get 4*75 == 300 counts per inch.
    //
    // Virtually all pinball plunger assemblies made since 1980 have the
    // same geometry, with a little under 3.25" of total travel
    // distance, from the maximum retraction point (pulled all the way
    // back, with the main spring fully compressed) to the maximum
    // forward point (pressed in all the way against the barrel spring).
    // So the total number of quadrature encoder counts over this range
    // is approximately 3.25*4*LPI.
    nativeScale = 13 * encoder->GetLPI();

    // For the underlying encoder, the natural zero point is the plunger
    // resting position.  This is the natural zero because it's where
    // the mechanical plunger is usually sitting at startup, when the
    // encoder has to arbitrarily pick an initial count value; and it's
    // also the only position where the plunger reliably comes to rest
    // at other times, which allows us to auto-zero the plunger from
    // time to time, when we see no movement for an extended period, to
    // correct for any accumulated error in the relative counter arising
    // from missed state transitions (either becuase the physical
    // encoder failed to detect a bar transition or because the Pico was
    // unable to process an interrupt in time).
    //
    // Unfortunately, the resting position is NOT the natural zero for
    // the generic plunger interface.  We have to report the position in
    // unsigned units for the generic interface.  That makes zero the
    // minimum position we can report, so we can't peg the rest position
    // at zero, since we also have to report positions in front of the
    // rest position.  We therefore need to define zero as the maximum
    // forward point.  We rely on the geometry of the standard
    // mechanical plunger, where the rest position is at about 1/6 of
    // the total travel range from the maximum forward position.
    restCount = static_cast<int>(nativeScale/6);

    // successful configuration
    return true;
}

// read a sample
bool QuadraturePlungerSensor::ReadRaw(Plunger::RawSample &r)
{
    // Get the count from the underlying encoder, and add our
    // rest count (the calibrated Z-Axis zero point).  Constrain
    // the result to 0..nativeScale.
    int pos = encoder->GetCount() + restCount;
    r.rawPos = static_cast<uint32_t>(pos < 0 ? 0 : pos > nativeScale ? nativeScale : pos);
    
    // The encoder's count is always instantaneously accurate, since
    // it's updated in an interrupt handler on every state transition
    // the encoder signals.  So every time we sample it, we know that
    // this is the physical position of the plunger RIGHT NOW.  Note
    // that it doesn't matter how long it's been since the last state
    // transition occurred - the timestamp isn't for detecting when
    // motion occurred, but is for determining when the position was
    // measured.  Every time we check the count, we're measuring the
    // position, so the time of this measurement is NOW.
    r.t = time_us_64();

    // a new sample is available ready
    return true;
}

void QuadraturePlungerSensor::OnRestoreCalibration(const CalibrationData &cal)
{
    // set the resting position to the calibrated zero point
    restCount = cal.zero;
}

void QuadraturePlungerSensor::BeginCalibration(CalibrationData &cal)
{
    // set the resting position to the initial zero point
    restCount = cal.zero;

    // zero the encoder
    encoder->ZeroCounter();
}

// auto-zero
bool QuadraturePlungerSensor::AutoZero(const CalibrationData &cal)
{
    // set the resting position to the initial zero point
    restCount = cal.zero;

    // zero the underlying encoder's counter
    encoder->ZeroCounter();
    
    // auto-zero applied
    return true;
}

size_t QuadraturePlungerSensor::ReportSensorData(uint8_t *buf, size_t maxSize)
{
    // make sure we have room in the buffer
    using PR = PinscapePico::PlungerReadingQuadrature;
    if (maxSize < sizeof(PR))
        return 0;

    // build our custom struct in the buffer
    auto *p = reinterpret_cast<PR*>(buf);
    p->cb = sizeof(PR);
    p->sensorType = typeCode;
    p->state = static_cast<uint8_t>(encoder->GetChannelState());

    // return the struct size
    return sizeof(PR);
}
