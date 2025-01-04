// Pinscape Pico - AEDR-8300 Quadrature Sensor
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file implements the basic device driver for the AEDR-8300 linear
// quadrature encoder.  This sensor can be used in Pinscape as a plunger
// position sensor.
//


#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "QuadratureEncoder.h"


// forward/external decrlations
class AEDR8300;
class JSONParser;

// global singleton
extern std::unique_ptr<AEDR8300> aedr8300;


// AEDR8300 quadrature encoder implementation
class AEDR8300 : public QuadratureEncoder
{
public:
    AEDR8300() : QuadratureEncoder(75) { }

    // Configure
    static void Configure(JSONParser &json);

protected:
};
