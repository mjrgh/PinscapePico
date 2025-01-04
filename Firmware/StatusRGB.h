// Pinscape Pico - Status RGB LED
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The original KL25Z Pinscape used that device's on-board RGB LED to
// show a variety of status information, as a quick visual check on the
// device's status.  The Pico also has an on-board LED, but alas, it's
// only monochrome, which greatly reduces how much information we can
// conveniently convey with it.  We do use the Pico's on board LED for
// some very simple health reporting - see PicoLED.h - but we also make
// provision here for an outboard RGB LED for the more information-rich
// display we had on the KL25Z version.  The outboard LED requires a set
// of three GPIO ports (one per color channel) and, obviously, the LED
// itself, wired to the GPIO ports and to power.  Pico ports can sink
// 10mA, which is sufficient to drive a small RGB LED without any
// adidtional circuitry, although the hardware design is free to use a
// higher-current LED if it provides a suitable current booster circuit.
//
// The status LED is optional; you can get all of the same information
// (and much more) via the various USB interfaces.  The LED is just a
// nice bonus feature if you can spare the GPIOs.
//
// It's also possible to wire the status LED through external output
// controller chips, instead of wiring it directly to GPIOs, using the
// Output Manager's "statusled()" computed output function.  That might
// be more convenient in some hardware setups, or even necessary, if
// there aren't enough free GPIOs to accommodate the LED.
//
//   // JSON configuration - outputs[] section
//   outputs: [
//      { device: { type: "tlc5940", port: 0 }, source: "red(statusled())" },
//      { device: { type: "tlc5940", port: 1 }, source: "green(statusled())" },
//      { device: { type: "tlc5940", port: 2 }, source: "blue(statusled())" },
//   ]
//
// Finally, you can view the status LED in real time on the Windows host
// using the StatusLED program, included with the project.  That's meant
// more as an example of how to use the C++ API than as a serious
// utility, but it'll do the job if you want to observe the status
// without attaching a physical LED, or if the LED isn't easily observed
// because it's inside a closed pin cab.  The C++ API also has access to
// all of the device status that goes into the LED color calculation, so
// you can get more detailed information that way, but a simple blinking
// light can still be a nice way to see the status at a glance.
// 

#pragma once
#include <stdint.h>
#include <stdlib.h>

// forward/external declarations
class StatusRGB;
class JSON;

// global singleton
extern StatusRGB statusRGB;

// RGB status LED
class StatusRGB
{
public:
    StatusRGB();

    // Configure from JSON data
    void Configure(JSONParser &json);

    // Perform period tasks.  The main loop should call this as often as
    // possible, at its convenience, to update the color display to match
    // the current software state.
    void Task();

    // RGB color
    struct RGB
    {
        RGB() { }
        RGB(const RGB &rgb) : r(rgb.r), g(rgb.g), b(rgb.b) { }
        RGB(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) { }
        RGB(uint32_t rgb) : r(static_cast<uint8_t>((rgb >> 16) & 0xFF)), g(static_cast<uint8_t>((rgb >> 8) & 0xFF)), b(static_cast<uint8_t>(rgb & 0xFF)) { }
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    // Get the current color
    RGB GetColor() const { return curColor; }

    // Calculate the current color based on live system status
    RGB CalcColor() const;

    // enable/disable
    void Enable(bool enable);

protected:
    // Is the LED configured?
    bool configured = false;

    // Is the LED enabled?
    bool enabled = true;

    // GPIO ports
    int gpioR = -1;
    int gpioG = -1;
    int gpioB = -1;

    // timer interrupt callback
    static void TimerIRQ(uint alarmNum) { statusRGB.OnTimer(); }
    void OnTimer();

    // Color settings for primary and secondary colors.  Many RGB LEDs
    // have severe imbalances in the relative brightness levels of the
    // elements, which can make the mixed color look way off from what
    // it should be for a given #RRGGBB formula.  So we let the user
    // configure the formula levels for the various nominal colors, to
    // get a more accurate appearance on the display result.
    RGB red{ 255, 0, 0 };
    RGB green{ 0, 255, 0 };
    RGB blue{ 0, 0, 255 };
    RGB yellow{ 255, 255, 0 };
    RGB orange{ 255, 128, 0 };
    RGB violet{ 255, 0, 255 };
    RGB cyan{ 0, 255, 255 };
    RGB white{ 255, 255, 255 };

    // parse an HTML-style #RGB or #RRGGBB string; returns true on success,
    // false if the format is incorrect
    static bool ParseColorString(RGB &rgb, const char *s);

    // hardware alarm number
    int alarmNum = -1;

    // next alarm target time
    uint64_t alarmTargetTime = 0;

    // manual color setting
    RGB manualColor{ 0, 0, 0 };
    bool manualOverride = false;

    // Current display color.  This is the NOMINAL color, which is what
    // the color should look like when displayed physically.
    RGB curColor{ 0, 0, 0 };

    // Current GPIO PWM output levels.  This is transformed from the
    // nominal color as needed for the GPIO active high/low configuration,
    // so that the color displayed actually matches the nominal color.
    RGB curGPIO{ 0, 0, 0 };

    // latched GPIO display color - this is latched from curColor each time
    // the counter completes a cycle
    RGB latchedGPIO{ 0, 0, 0 };

    // current counter, for the timer-based PWM
    int pwmCounter = 0;

    // Polarity - active high or active low.  Active high means that the
    // port is driven at VCC when the LED is ON.  Active low means that
    // the port is at GND when the LED is ON.
    bool activeHigh = false;

    // console command
    static void Command_main(const ConsoleCommandContext *c);
};
