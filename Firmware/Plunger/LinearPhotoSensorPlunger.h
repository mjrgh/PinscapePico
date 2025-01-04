// Pinscape Pico - Plunger sensor interface for linear photo sensors (TCD1103, TSL1410R)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

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
class TCD1103;
class TSL1410R;


// ---------------------------------------------------------------------------
//
// Base class for linear photo sensor plungers
//
class LinearPhotoSensorPlunger : public Plunger::Sensor
{
public:
    // Construction
    LinearPhotoSensorPlunger(uint32_t nPixels);

    // Configure
    virtual bool Configure(const JSONParser::Value *val) override;

    // Read the sensor
    virtual bool ReadRaw(Plunger::RawSample &r) override;

    // Get the native scale - this is the number of pixels in the sensor image
    virtual uint32_t GetNativeScale() const override { return nPixels; }

    // Our extra sensor data report provides the raw pixel array
    virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) override;

protected:
    // Get a pointer to the latest raw image data from the sensor.  We
    // let the callee provide the buffer on the assumption that the
    // sensor maintains its own frame buffers.  We thus avoid allocating
    // more memory and copying bytes by getting a pointer to a buffer
    // that the sensor has to maintain internally anyway.
    //
    // Returns true if the frame buffer is "owned" by the client,
    // meaning that the buffer is guaranteed to be stable until the
    // client releases it with ReleaseFrame().  Returns false if the
    // frame is volatile, meaning it could be overwritten asynchronously
    // by the hardware writer.  A valid buffer must be returned in
    // either case; the only difference is whether the buffer is stable
    // or volatile.  
    virtual bool GetRawFrame(const uint8_t* &buf, uint64_t &timestamp) = 0;

    // Release the current frame buffer.  The base class assumes that
    // the sensor uses some kind of double-buffering scheme, where it
    // rotates through two or more frame buffers as it transfers data
    // (presumably asynchronously) from the underlying hardware into
    // memory.  At any given time, the sensor is writing into one
    // buffer, and the other buffer or buffers are considered to be
    // either "owned" by the client or free for reuse.  This function
    // explicitly tells the subclass that the base code is done reading
    // the current frame, so the sensor can safely reuse this buffer for
    // the next frame.
    virtual void ReleaseFrame() = 0;

    // Scan a frame for the image edge.  Returns the position as a
    // pixel coordinate from 0 to nPixels-1.
    virtual uint32_t ScanFrame(const uint8_t *pix, bool reverseOrientation) = 0;

    // Number of pixels in the sensor's image file
    uint32_t nPixels;

    // last sample returned - we'll repeat this when asked for a sample
    // when no new frame is available from the sensor
    Plunger::RawSample lastSample{ 0, 0 };
};


// ---------------------------------------------------------------------------
//
// Toshiba TCD1103 interface
//
class TCD1103Plunger : public LinearPhotoSensorPlunger
{
public:
    TCD1103Plunger();

    // display name for log messages
    virtual const char *FriendlyName() const override { return "TCD1103"; }
    
    // Configure
    virtual bool Configure(const JSONParser::Value *val) override;

      // sensor type for feedback reports
    virtual uint16_t GetTypeForFeedbackReport() const { return PinscapePico::FeedbackControllerReport::PLUNGER_TCD1103; }

    // calibration data filename
    virtual const char *GetCalFileName() const override { return "tcd1103.cal"; }

    // Figure the average scan time in microseconds.  We'll just report
    // the inverse of the sampling frequency that the ADC reports to us.
    virtual uint32_t GetAvgScanTime() override;

    // check if a new frame is ready from the sensor
    virtual bool IsReady() override;

    // Set integration time
    virtual void SetIntegrationTime(uint32_t us) override;

protected:
    // Raw frame interface
    virtual bool GetRawFrame(const uint8_t* &buf, uint64_t &timestamp) override;
    virtual void ReleaseFrame() override;
    virtual uint32_t ScanFrame(const uint8_t *pix, bool reverseOrientation) override;
};


// ---------------------------------------------------------------------------
//
// TAOS TSL1410R/TSL1412S plungers
//
class TSL14XXPlunger : public LinearPhotoSensorPlunger
{
public:
    TSL14XXPlunger(int nPixels);

    // variant name and sensor hardware configuration key name, for disagnostics
    virtual const char *VariantName() const = 0;
    virtual const char *SensorConfigKey() const = 0;

    // Configure
    virtual bool Configure(const JSONParser::Value *val) override;

    // sensor type for feedback reports
    virtual uint16_t GetTypeForFeedbackReport() const { return PinscapePico::FeedbackControllerReport::PLUNGER_TSL1410R; }

    // Figure the average scan time in microseconds.  We'll just report
    // the inverse of the sampling frequency that the ADC reports to us.
    virtual uint32_t GetAvgScanTime() override;

    // check if a new frame is ready from the sensor
    virtual bool IsReady() override;

    // Set integration time
    virtual void SetIntegrationTime(uint32_t us) override;

    // Set the scan mode
    virtual void SetScanMode(uint8_t scanMode) override;

protected:
    // Raw frame interface
    virtual bool GetRawFrame(const uint8_t* &buf, uint64_t &timestamp) override;
    virtual void ReleaseFrame() override;
    virtual uint32_t ScanFrame(const uint8_t *pix, bool reverseOrientation) override {
        return (this->*scanMethodFunc)(pix, reverseOrientation);
    }

    // frame scanning method function
    uint32_t (TSL14XXPlunger::*scanMethodFunc)(const uint8_t *pix, bool reverseOrientation) = &TSL14XXPlunger::ScanFrameSteadySlope;

    // Scan frame methods
    uint32_t ScanFrameSteadySlope(const uint8_t *pix, bool reverseOrientation);    // method 0
    uint32_t ScanFrameSteepestSlope(const uint8_t *pix, bool reverseOrientation);  // method 1
    uint32_t ScanFrameSpeedGap(const uint8_t *pix, bool reverseOrientation);       // method 2

    // For method 2 (slope search with gap size increasing with speed),
    // keep track of the prior two raw position readings, to use as an
    // estimate of the speed in the new frame.
    int prvRawResult0 = 0;
    int prvRawResult1 = 0;
};

// TSL1410R
class TSL1410RPlunger : public TSL14XXPlunger
{
public:
    TSL1410RPlunger() : TSL14XXPlunger(1280) { }
    virtual const char *FriendlyName() const override { return "TSL1410R"; }
    virtual const char *GetCalFileName() const override { return "tsl1410r.cal"; }
    virtual const char *VariantName() const override { return "TSL1410R"; }
    virtual const char *SensorConfigKey() const override { return "tsl1410r"; }
};

// TSL1412S
class TSL1412SPlunger : public TSL14XXPlunger
{
public:
    TSL1412SPlunger() : TSL14XXPlunger(1536) { }
    virtual const char *FriendlyName() const override { return "TSL1412S"; }
    virtual const char *GetCalFileName() const override { return "tsl1412s.cal"; }
    virtual const char *VariantName() const override { return "TSL1412S"; }
    virtual const char *SensorConfigKey() const override { return "tsl1412s"; }
};
