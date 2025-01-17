// Pinscape Pico - USB CDC (virtual COM port)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Our USB interface includes a virtual COM port that we use primarily
// for logging human-readable status messages to the PC, for debugging
// and troubleshooting purposes.

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <malloc.h>
#include <vector>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "Main.h"
#include "Logger.h"
#include "USBCDC.h"

// global singleton
USBCDC usbcdc;

USBCDC::USBCDC()
{
}

// Configure the port
void USBCDC::Configure(const JSONParser::Value *val)
{
    // There's nothing to configure at the moment beyond the existence
    // of the port, which means that we include the CDC interface in the
    // collection of USB interfaces we report to the host during
    // connection setup.
    Configure();
}

void USBCDC::Configure()
{
    // mark the interface as configured
    configured = true;
}

// Handle output CDC data (host-to-device).  (The term "output" is a
// little confusing, because it's incoming data on our side - but the
// USB naming convention is ALWAYS from the HOST's perspective, so
// host-to-device messages are always OUTPUT.)
void USBCDC::OnOutput(const uint8_t *buf, size_t len)
{
    // send the data to our associated logger's console
    usbCdcLogger.console.ProcessInputStr(reinterpret_cast<const char*>(buf), len);
}

