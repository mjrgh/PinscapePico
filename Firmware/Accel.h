// Pinscape Pico firmware - Accelerometer Device base class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the base class for Accelerometer devices.  This is an
// abstract device-independent interface that upper layers of the code
// can use to access accelerometers, without being tied to any one
// accelerometer.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <unordered_map>
#include <string>

// Pico SDK headers
#include <pico/stdlib.h>

// external/forward declarations
class JSONParser;
class Accelerometer;
class AccelerometerRegistry;

// Abstract accelerometer device interface
class Accelerometer
{
public:
    // Configuration key name for this instance.  This should be all
    // alphanumerics, with the addition of an "[n]" suffix if and only if
    // multiple instance of the same device are configured.
    virtual const char *GetConfigKey() const = 0;

    // Get the friendly name for the device, for log messages
    virtual const char *GetFriendlyName() const = 0;

    // Get the sampling rate, in samples per second
    virtual int GetSamplingRate() const = 0;

    // Get the range, in 'g' units (standard Earth gravities).  For
    // example, this returns 2 if the device's dynamic range is set
    // to +/- 2g.
    virtual int GetGRange() const = 0;

    // Get the last reading.  Most accelerometer chips can be configured
    // to generate samples at a fixed rate.  The subclass should take
    // samples as they become available from the device, such as via an
    // interrupt handler or polling in the Task() routine, and store the
    // last reading internally.  This reports back the last reading.  For
    // a device that takes samples only on demand, this can be implemented
    // as initiating a device sample cycle, but it shouldn't block; if a
    // sample can't be obtained immediately, this should initiate an
    // asynchronous request to the device and immediately return the last
    // sample.  The timestamp is the time the sample was read (in terms of
    // the Pico system clock, in microseconds since reset).  It's fine to
    // return the same sample repeatedly; the caller can use the timestamp
    // to determine if the sample is new since its last request.
    //
    // The axis readings report here are normalized to the full 16-bit
    // signed int range (-32768 .. +32767).  Each device subclass must
    // adjust the native device readings to the 16-bit range when
    // implementing this.
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) = 0;

    // Period task handler
    virtual void Task() = 0;
};

// Null accelerometer device.  This is a placeholder device that we'll use
// if no actual devices are configured.
class NullAccelerometer : public Accelerometer
{
public:
    virtual const char *GetConfigKey() const override { return "null"; }
    virtual const char *GetFriendlyName() const override { return "Null Device"; }
    virtual int GetSamplingRate() const override { return 1; }
    virtual void Read(int16_t &x, int16_t &y, int16_t &z, uint64_t &timestamp) override { x = y = z = 0; timestamp = 0; }
    virtual void Task() override { }
    virtual int GetGRange() const { return 2; }
};

// registry singleton
extern AccelerometerRegistry accelerometerRegistry;

// Accelerometer registeration table.  This is a global singleton where
// the configured accelerometer devices are registered.
class AccelerometerRegistry
{
public:
    AccelerometerRegistry();

    // run each configured device's task handler
    void Task();

    // Register a device.  Each device calls this upon successful
    // configuration, to add itself as a source for nudge and axis
    // inputs.
    void Register(Accelerometer *device);

    // look up a device by configuration key
    Accelerometer *Get(const char *configKey);
    Accelerometer *Get(const std::string &configKey);

    // get the default device
    Accelerometer *GetDefaultDevice() const { return defaultDevice; }

    // get the null device
    Accelerometer *GetNullDevice() const { return nullDevice; }

    // Get the number of configured devices, not counting the null device
    int GetNumConfigured() const { return static_cast<int>(devices.size()) - 1; }

protected:
    // Configured accelerometer device instances.  Each accelerometer
    // device that's enabled in the configuration gets an entry here.
    std::unordered_map<std::string, Accelerometer*> devices;

    // Null accelerometer instance.  This is a placeholder that we use if
    // no physical devices are configured, so that there always a valid
    // device object.
    Accelerometer *nullDevice = new NullAccelerometer();

    // Default accelerometer.  This is the first physical accelerometer
    // device configured, or the null accelerometer if no physical
    // devices are available.
    Accelerometer *defaultDevice = nullDevice;
};
