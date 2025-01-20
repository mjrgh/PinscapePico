// Pinscape Pico Button Latency Tester II - Pico on-board LED blinker
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Flashes the Pico's on-board LED for simple diagnostic status reporting.

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>
#include <hardware/timer.h>

// project headers
#include "PicoLED.h"


// global singleton
PicoLED picoLED(PICO_DEFAULT_LED_PIN);

PicoLED::PicoLED(int ledPin) : ledPin(ledPin)
{
    // set up the LED GPIO
    gpio_init(ledPin);
    gpio_set_dir(ledPin, GPIO_OUT);
}

// update the blink state
void PicoLED::Task()
{
    uint64_t t = time_us_64();
    if (debugBlink.size() != 0)
    {
        on = debugBlink.front().on;
        if (t - lastBlinkTime >= debugBlink.front().us)
        {
            lastBlinkTime += debugBlink.front().us;
            debugBlink.pop_front();
        }
    }
    else if (on && t - lastBlinkTime >= intervalOn)
    {
        // elapsed time has reached or passed the interval time - blink off
        on = false;

        // Advance the blink time by the interval.  Note that we set it to the last
        // start time plus the interval, instead of just using the current time, to
        // maintain a steady pace even if the task routine isn't called exactly when
        // the next blink transition is ready.  Using the current time would accumulate
        // any delay time into the next blink time
        lastBlinkTime += intervalOn;
    }
    else if (!on && t - lastBlinkTime >= intervalOff)
    {
        // elapsed time has reached or passed the interval time - blink on
        on = true;
        lastBlinkTime += intervalOff;
    }

    // update the LED state
    gpio_put(ledPin, on);
}

void PicoLED::SetBlinkPattern(Pattern n)
{
    if (pattern != n)
    {
        pattern = n;
        UpdateBlinkTime();
    }
}

void PicoLED::UpdateBlinkTime()
{
    // set a default very fast flash rate if no we find no other modes to indicate
    SetInterval(50, 50);

    // set the blink time based on the USB status
    switch (pattern)
    {
    case Pattern::Off:
    default:
        SetInterval(0, 1000);
        break;

    case Pattern::On:
        SetInterval(1000, 0);
        break;

    case Pattern::Disconnected:
        SetInterval(250, 3750);
        break;

    case Pattern::Suspended:
        SetInterval(250, 1750);
        break;

    case Pattern::Connected:
        SetInterval(1000, 1000);
        break;
    }
}

void PicoLED::DiagnosticFlash(int n)
{
    // off for 1 second, blink n times, off for 1 second
    debugBlink.emplace_back(false, 1000);
    while (n-- > 0)
    {
        debugBlink.emplace_back(true, 100);
        debugBlink.emplace_back(false, 400);
    }
    debugBlink.emplace_back(false, 1000);
}

void DiagnosticFlash(int n)
{
    picoLED.DiagnosticFlash(n);
}

