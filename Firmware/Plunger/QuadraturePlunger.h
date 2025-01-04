// Pinscape Pico - Quadrature Plunger Sensors
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file implements the plunger sensor interface for quadrature
// encoder sensors:
//
//   AEDR-8300-1K2
//
// These classes implement plunger interfaces to these chips.  Each chip
// also has a separate "device driver" under the Chips section of the
// source hierarchy, where the chip's low-level GPIO interface is
// implemented.  This division of labor makes the device drivers more
// general-purpose, in case we ever want to use them for some function
// unrelated to plungers.
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
class AEDR8300;
class JSONParser;
class ConsoleCommandContext;
class QuadratureEncoder;

class QuadraturePlungerSensor : public Plunger::Sensor
{
public:
    QuadraturePlungerSensor(QuadratureEncoder *encoder, int typeCode,
        const char *friendlyName, const char *calFileName, const char *devKey);

    // sensor type for feedback reports
    virtual uint16_t GetTypeForFeedbackReport() const { return typeCode; }

    // Configure
    virtual bool Configure(const JSONParser::Value *val) override;

    // display name for log messages
    virtual const char *FriendlyName() const override { return friendlyName; }

    // calibration data updates
    using CalibrationData = Plunger::CalibrationData;
    virtual void OnRestoreCalibration(const CalibrationData &) override;
    virtual void BeginCalibration(CalibrationData &) override;

    // auto-zero
    virtual bool AutoZero(const CalibrationData &cal) override;

    // calibration data filename
    virtual const char *GetCalFileName() const override { return calFileName; }

    // samples are always ready, because we always know the instantaneous
    // position with this type of sensor
    virtual bool IsReady() override { return true; }

    // read the sensor
    virtual bool ReadRaw(Plunger::RawSample &r) override;

    // Figure the average scan time in microseconds.  The quadrature
    // encoder position is always known instantaneously, so the scan
    // time is zero.
    virtual uint32_t GetAvgScanTime() override { return 0; }

    // the native scale depends upon the underlying encoder's line scale
    virtual uint32_t GetNativeScale() const override { return nativeScale; }

    // report extra data - channel A/B status
    virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) override;

protected:
    // device driver interface for the underlying encoder
    QuadratureEncoder *encoder;

    // Sensor type - PinscapePico::FeedbackControllerReport::PLUNGER_xxx
    int typeCode;

    // friendly name, for log messages
    const char *friendlyName;

    // Calibration filename
    const char *calFileName;

    // device key, for reporting in configuration errors relating to
    // the sensor
    const char *devKey;

    // Native scale.  This is computed at construction based on the
    // underlying device's lines-per-inch metric.
    uint32_t nativeScale;

    // Rest count.  This is the native position we report when the
    // plunger is at the rest position, which on the underlying
    // quadrature encoder is defined as the count==zero position.  So
    // this is essentially a DC offset that we add to the encoder's
    // counter to get the native position to report through the generic
    // interface.  This ensures that all values reported through the
    // generic interface are positive, which is required since they're
    // reported as UINT32 values.
    uint32_t restCount;
};
