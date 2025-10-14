// Pinscape Pico - Pico on-board LED blinker
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
#include "PicoBoardType.h"

// global singleton
PicoLED picoLED;

PicoLED::PicoLED()
{
    // set up the Pico LED (which can vary by target board)
    PicoBoardType::LED::Init();
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
    PicoBoardType::LED::Write(on);
}

void PicoLED::SetUSBStatus(bool mounted, bool suspended)
{
    // figure the USBStatus mode based on the flags
    if (mounted)
        usbStatus = suspended ? USBStatus::Suspended : USBStatus::Connected;
    else
        usbStatus = USBStatus::Dismounted;

    // update the blink time
    UpdateBlinkTime();
}

void PicoLED::UpdateBlinkTime()
{
    // set a default very fast flash rate if no we find no other modes to indicate
    SetInterval(50, 50);
    
    // set the blink time based on the USB status
    switch (usbStatus)
    {
    case USBStatus::Connected:
        // Mounted and running normally - slow blink, 1s on/1s off
        SetInterval(1000, 1000);
        break;

    case USBStatus::Suspended:
        // Suspend mode (host low-power sleep mode) - 200ms flash every two seconds
        picoLED.SetInterval(200, 1800);
        break;

    case USBStatus::Dismounted:
        // Dismounted - 250ms every 4 seconds
        picoLED.SetInterval(250, 3750);
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

