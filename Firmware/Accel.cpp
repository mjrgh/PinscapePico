// Pinscape Pico firmware - Accelerometer Device base class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <unordered_map>
#include <string>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "JSON.h"
#include "USBIfc.h"
#include "Utils.h"
#include "Accel.h"

// ---------------------------------------------------------------------------
//
// Accelerometer device registry
//

// registry singleton
AccelerometerRegistry accelerometerRegistry;

// construction
AccelerometerRegistry::AccelerometerRegistry()
{
    // register the null device
    Register(nullDevice);
}

// task handler
void AccelerometerRegistry::Task()
{
    for (auto &dev : devices)
        dev.second->Task();
}

// register a device
void AccelerometerRegistry::Register(Accelerometer *device)
{
    // add a map entry, so that config file references can look it up by name
    devices.emplace(device->GetConfigKey(), device);

    // If we don't already have a default (apart from the null device),
    // make this the default.  This way we always get a physical device
    // as the default as long as at least one device is configured.  If
    // multiple devices are configured, whichever one is configured
    // first becomes the arbitrary default.
    if (defaultDevice == nullDevice)
        defaultDevice = device;

    // Add logical joystick axes that directly read from the device axes
    class AccelLogicalAxis : public LogicalAxis
    {
    public:
        AccelLogicalAxis(Accelerometer *accel, int axisIndex) : accel(accel), axisIndex(axisIndex) { }
        virtual int16_t Read() override
        {
            int16_t axis[3];
            uint64_t timestamp;
            accel->Read(axis[0], axis[1], axis[2], timestamp);
            return axis[axisIndex];
        }
        Accelerometer *accel;
        int axisIndex;  // 0=X, 1=Y, 2=Z
    };
    LogicalAxis::AddSource(Format("%s.x", device->GetConfigKey()),
        [device](const LogicalAxis::CtorParams&, std::vector<std::string>&) -> LogicalAxis* {
            return new AccelLogicalAxis(device, 0);
    });
    LogicalAxis::AddSource(Format("%s.y", device->GetConfigKey()),
        [device](const LogicalAxis::CtorParams&, std::vector<std::string>&) ->LogicalAxis* {
            return new AccelLogicalAxis(device, 1);
    });
    LogicalAxis::AddSource(Format("%s.z", device->GetConfigKey()),
        [device](const LogicalAxis::CtorParams&, std::vector<std::string>&) ->LogicalAxis* {
            return new AccelLogicalAxis(device, 2);
    });
}

Accelerometer *AccelerometerRegistry::Get(const char *configKey)
{
    auto it = devices.find(configKey);
    return (it != devices.end()) ? it->second : nullptr;
}

Accelerometer *AccelerometerRegistry::Get(const std::string &configKey)
{
    auto it = devices.find(configKey);
    return (it != devices.end()) ? it->second : nullptr;
}
