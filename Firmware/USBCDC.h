// Pinscape Pico - USB CDC (virtual COM port)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Our USB interface includes a virtual COM port that we use primarily
// for logging human-readable status messages to the PC, for debugging
// and troubleshooting purposes.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "Logger.h"
#include "JSON.h"

// global isngleton
class USBCDC;
extern USBCDC usbcdc;

// USB CDC (virtual COM port)
class USBCDC
{
public:
    USBCDC();

    // Handle incoming CDC port output text (host-to-device)
    void OnOutput(const uint8_t *buf, size_t len);

    // configure the port from the serialPort.usb configuration item
    void Configure(const JSONParser::Value *val);

    // configure from fixed parameters; currently just enables the interface
    void Configure();

    // is the USB CDC port configured?
    bool IsConfigured() const { return configured; }

protected:
    // USB CDC port configured
    bool configured = false;
};

