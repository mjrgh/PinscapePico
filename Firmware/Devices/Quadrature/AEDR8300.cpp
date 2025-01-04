// Pinscape Pico - AEDR-8300 Quadrature Sensor
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file implements the basic device driver for the AEDR-8300 linear
// quadrature encoder.  This sensor can be used in Pinscape as a plunger
// position sensor.
//


// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "Logger.h"
#include "GPIOManager.h"
#include "ThunkManager.h"
#include "AEDR8300.h"


// forward/external decrlations
class AEDR8300;
class JSONParser;

// global singleton
std::unique_ptr<AEDR8300> aedr8300;


// Configure from JSON data
//
// aedr8300: {
//    channelA: <number>,      // GPIO port number for encoder channel A connection
//    channelB: <number>,      // GPIO port number for encoder channel B connection
// }
//
void AEDR8300::Configure(JSONParser &json)
{
    if (auto *val = json.Get("aedr8300") ; !val->IsUndefined())
    {
        // create an instance
        std::unique_ptr<AEDR8300> dev(new AEDR8300());

        // do the base class configuration; if that fails, stop here
        if (!dev->ConfigureBase("aedr8300", val))
            return;

        // success - make the device available through the global singleton
        Log(LOG_CONFIG, "aedr8300 configured; chA=GP%d, chB=GP%d\n", dev->gpA, dev->gpB);
        aedr8300.reset(dev.release());
    }
}
