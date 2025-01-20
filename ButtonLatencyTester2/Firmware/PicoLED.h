// Pinscape Pico Button Latency Tester II - on-board LED blinking patterns for diagnostics
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This controls the Pico's on-board LED, which we use as a very coarse
// diagnostic aid.  For the most part, we just keep the LED blinking at
// a constant rate as an indication that the program is still running.
// The blinking is controlled by the main program thread, so the LED
// will only continue blinking as long as the main program is running
// properly.  The blinking will stop if the main thread ever gets stuck
// in a loop, jumps to a random location in memory, halts in an
// exception handler, or otherwise stops running through the main loop.
// Steady blinking thus indicates that the main thread is at a minimum
// running through its outer loop correctly and invoking the blink
// updater when it should.  We also provide a way of flashing the LED a
// specified number of times, as a crude way of visually indicating a
// diagnostic condition (which can be helpful if you're trying to debug
// low-level initialization code that's failing before it can set up
// proper logging).

#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <list>

class PicoLED
{
public:
    PicoLED(int ledPin);

    // update the blink state - the main loop must call this periodically
    // (as frequently as possible) to keep the blinker running
    void Task();

    // Set the current blinking pattern
    enum class Pattern
    {
        // solid off
        Off,

        // solid on
        On,

        // USB disconnected (short blink at 4-second intervals)
        Disconnected,

        // USB suspended (short blink at 2-second intervals)
        Suspended,

        // USB connected
        Connected,
    };
    void SetBlinkPattern(Pattern pat);


    // Signal a diagnostic flash count.  This is a very primitive instrumentation tool
    // that can provide an extremely limited amount of status information via the Pico's
    // on-board LED, which can be useful when other, more information-rich outside
    // connections (especially the USB CDC port) aren't working or aren't available for
    // some reason, such as when working on the boot-time startup code.  This schedules
    // a series of 'n' short flashes, 100ms flashes every 500ms, set off timewise by a
    // 1-second blanking period before and after the flash sequence.  The sequence is
    // dinstinctive enough that it can convey a small set of distinct symbols (small
    // integers) to an alert observer.  It's obviously a lot easier to debug in terms
    // of 'printf' style text sent to a console, and the obvious way to do that is via
    // the USB CDC layer.  But the USB CDC layer has to be provided by the program
    // itself, so when the program isn't working well enough to get that CDC connection
    // running properly, we might need other forms of instrumentation that don't depend
    // on so many code layers all working.
    void DiagnosticFlash(int n);

protected:
    // Update the blink timing.  We call this internally after any of the
    // dependent mode flags are updated.
    void UpdateBlinkTime();

    // set the interval - called internally from UpdateBlinkTime()
    void SetInterval(int on_ms, int off_ms) { intervalOn = on_ms*1000; intervalOff = off_ms*1000; }

    // Pico LED GPIO pin
    int ledPin;

    // Current blinking pattern
    Pattern pattern = Pattern::On;

    // blink time in microseconds
    int intervalOn = 1000;
    int intervalOff = 0;

    // debug blinker action queue
    struct DebugBlinkAction
    {
        DebugBlinkAction(bool on, int ms) : on(on), us(ms*1000) { }
        bool on;
        int us;
    };
    std::list<DebugBlinkAction> debugBlink;

    // starting time of last blink, in terms of the system microsecond timer
    uint64_t lastBlinkTime = 0;

    // current LED state
    bool on = false;
};

// LED blinker singleton
extern PicoLED picoLED;

// Set a diagnostic flash count
void DiagnosticFlash(int n);
