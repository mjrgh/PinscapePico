// Pinscape Pico - Status RGB LED
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements an outboard RGB LED for status displays, to supplement the
// Pico's on-board monochrome green LED.  This provides richer status
// information than the monochrome on-board LED can show, similar to the
// original KL25Z Pinscape.
//
// The RGB LED, if present, is connected to a set of three GPIOs, one for
// each color channel.  The GPIOs are configured as digital outputs, and we
// implement low-color-resolution PWM using timer interrupts.  We use
// timer-based PWM instead of hardware PWM channels, because we need so
// little color resolution that it would be too wasteful to tie up three PWM
// channels for this.  The timer runs every 1.25ms, to implement 16 PWM steps
// (effectively 4 bits of resolution) at a 50Hz refresh rate.  That's fast
// enough to be relatively flicker-free (especially with the primary colors
// that display at 100% duty cycle), but slow enough to use practically
// no CPU time.
//
// Why do we only need 3-bit PWm resolution?  Because the status LED colors
// have to be visually distinguishable in isolation.  That's not a technical
// limitation, but rather a UI requirement, to make the colors stand out
// readily to the eye.  If we used, say, two slightly different shades of
// green as two separate status indications, the user wouldn't be able to
// interpret it reliably, since shades of green in isolation all just look
// like green.  This limits us to the RGB primary colors (red, green, blue),
// the secondary colors (yellow, magenta, cyan, white, from mixing two or
// three of the primary colors at 100%), and just a couple of tertiary
// colors (orange, violet, from mixing one primary at 100% and one at 50%).
// These colors are all distinctive to the eye in isolation.  Everything
// on the color wheel between any of these two special colors tends to
// look too much like its "special" neighbor to count as distinguishable.
//
// Note that we could achieve all of the color mixes identified above with
// just 2 bits of PWM (for 0%, 50% and 100% levels).  But we have plenty of
// CPU power to handle 3 bits, so we splurge for the extra bit.  This lets
// us also use perceptual half-brightness in our flashing patterns, such as
// "Green / Dim Green".  "Dim Green" on its own isn't a distinctive enough
// color, but it does work in a flashing pattern coupled with bright green,
// which gives us some more options for patterns - every color we've
// identified can be coupled with its "dim" counterpart to make a bright/dim
// pattern.  We need 3 bits for "half" brightness because of the need for
// some gamma correction; 50% duty cycle looks to the eye like about 80%
// brightness, which isn't enough of a contrast to make a good flashing
// pattern.  Going to 1/8 duty cycle gets us closer to perceptual half
// brightness, so we need 8 steps in our overall cycle.


#include <stdint.h>
#include <stdlib.h>

#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/timer.h>

#include "Pinscape.h"
#include "Utils.h"
#include "Logger.h"
#include "Config.h"
#include "GPIOManager.h"
#include "PWMManager.h"
#include "JSON.h"
#include "TVON.h"
#include "USBIfc.h"
#include "Nudge.h"
#include "Plunger/Plunger.h"
#include "IRRemote/IRReceiver.h"
#include "IRRemote/IRTransmitter.h"
#include "CommandConsole.h"
#include "StatusRGB.h"


// global singleton
StatusRGB statusRGB;

// construction
StatusRGB::StatusRGB()
{
}

// Configure from JSON data
void StatusRGB::Configure(JSONParser &json)
{
    if (auto *val = json.Get("rgbStatusLight") ; !val->IsUndefined())
    {
        // get the GPIO assignments
        gpioR = val->Get("red")->Int(-1);
        gpioG = val->Get("green")->Int(-1);
        gpioB = val->Get("blue")->Int(-1);

        // get the RGB color mix formulas, if provided
        if (auto *colorMix = val->Get("colorMix"); !colorMix->IsUndefined())
        {
            auto ReadColor = [colorMix](const char *name, RGB &slot)
            {
                if (auto *cv = colorMix->Get(name); cv->IsNumber())
                {
                    // parse it as an HTML-style 0xRRGGBB value
                    slot = cv->UInt32();
                }
                else if (cv->IsObject())
                {
                    // parse it as a collection of r, g, b properties
                    slot = { cv->Get("r")->UInt8(), cv->Get("g")->UInt8(), cv->Get("b")->UInt8() };
                }
                else if (cv->IsString())
                {
                    // parse it as an HTML-style #RRGGBB or #RGB string
                    std::string s = cv->String();
                    if (!ParseColorString(slot, s.c_str()))
                    {
                        Log(LOG_ERROR, "rgbStatusLed.colorMix.%s: invalid format \"%s\", expected HTML-style \"#RRGGBB\" or \"#RGB\"\n",
                            name, s.c_str());
                    }
                }
                else if (!cv->IsUndefined())
                {
                    Log(LOG_ERROR, "rgbStatusLed.colorMix.%s: invalid value, expected HTML-style \"#RRGGBB\" string, "
                        "0xRRGGBB number value, or {r,g,b} object\n", name);
                }
            };
            ReadColor("red", red);
            ReadColor("green", green);
            ReadColor("blue", blue);
            ReadColor("yellow", yellow);
            ReadColor("orange", orange);
            ReadColor("violet", violet);
            ReadColor("cyan", cyan);
            ReadColor("white", white);
        }

        // get the active high/low setting; default is active low
        activeHigh = false;
        if (auto activeStr = val->Get("active")->String("low") ; activeStr == "high")
            activeHigh = true;
        else if (activeStr != "low")
            Log(LOG_ERROR, "rgbStatusLed.active: invalid value \"%s\", expected \"high\" or \"low\"; "
                "using active low by default\n", activeStr.c_str());

        // validate the GPIOs
        if (!IsValidGP(gpioR) || !IsValidGP(gpioG) || !IsValidGP(gpioB))
        {
            Log(LOG_ERROR, "RGB Status LED: invalid or missing GPIO assignment(s)\n");
            return;
        }

        // claim the GPIOs for exclusive use
        if (!gpioManager.Claim("Status LED (R)", gpioR)
            || !gpioManager.Claim("Status LED (G)", gpioG)
            || !gpioManager.Claim("Status LED (B)", gpioB))
            return;

        // Configure the GPIOs.  Set them up as PWM outputs, and set the
        // drive strength to the highest setting so that they can directly
        // drive a small LED.
        auto InitGP = [](int gp)
        {
            gpio_init(gp);
            gpio_set_dir(gp, GPIO_OUT);
            gpio_set_drive_strength(gp, GPIO_DRIVE_STRENGTH_12MA);
            return true;
        };
        if (!InitGP(gpioR) || !InitGP(gpioG) || !InitGP(gpioB))
            return;

        // claim an alarm timer
        alarmNum = hardware_alarm_claim_unused(false);
        if (alarmNum < 0)
        {
            Log(LOG_ERROR, "RGB Status LED: no alarm timer available\n");
            return;
        }

        // set up our timer callback; interrupt every 1.25ms
        alarmTargetTime = time_us_64() + 1250;
        hardware_alarm_set_callback(alarmNum, &TimerIRQ);
        hardware_alarm_set_target(alarmNum, alarmTargetTime);

        // configured
        configured = true;
        Log(LOG_CONFIG, "RGB Status LED configured on R=GP%d, G=GP%d, B=GP%d\n",
            gpioR, gpioG, gpioB);

        // set up a console command
        CommandConsole::AddCommand(
            "statusled", "Status LED testing functions",
            "statusled [options]\n"
            "options:\n"
            "  -s, --stats    show information on the status light configuration\n"
            "  --set <rgb>    manually set the output to the given color, in HTML #RGB or #RRGGBB format\n"
            "  --auto         resume automatic display after a --set command\n",
            &Command_main);
    }
}

// parse a color string
bool StatusRGB::ParseColorString(RGB &rgb, const char *p)
{
    // skip '#' if present, but don't require it
    if (*p == '#')
        ++p;

    // count hex digits
    int n = 0;
    uint32_t acc = 0;
    for ( ; *p != 0 ; ++p, ++n)
    {
        acc <<= 4;
        if (isdigit(*p))
            acc += *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            acc += *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            acc += *p - 'A' + 10;
        else
            return false;
    }

    // check which format we have
    if (n == 3)
    {
        // #RGB format
        long r = (acc >> 8) & 0xf;
        long g = (acc >> 4) & 0xf;
        long b = acc & 0xf;
        rgb = {
            static_cast<uint8_t>((r << 4) | r),
            static_cast<uint8_t>((g << 4) | g),
            static_cast<uint8_t>((b << 4) | b)
        };

        return true;
    }
    else if (n == 6)
    {
        // #RRGGBB format
        rgb = {
            static_cast<uint8_t>((acc >> 16) & 0xFF),
            static_cast<uint8_t>((acc >> 8) & 0xFF),
            static_cast<uint8_t>(acc & 0xFF)
        };

        return true;
    }

    // invalid format
    return false;
}

// Enable/disable
void StatusRGB::Enable(bool f)
{
    // set the new mode
    enabled = f;

    // run the task update
    Task();
}

// Task handler
void StatusRGB::Task()
{
    // calculate the current color
    RGB rgb = CalcColor();

    // save it as the current nominal color
    curColor = rgb;

    // if active-low, invert the levels
    if (!activeHigh)
    {
        rgb.r = 255 - rgb.r;
        rgb.g = 255 - rgb.g;
        rgb.b = 255 - rgb.b;
    }
    
    // Update the internal GPIO level setting
    curGPIO = rgb;
}

// Timer callbacks
void StatusRGB::OnTimer()
{
    // Update the counters.  We implement an 16-level color space, so
    // the counter increases by 16 on each update.
    pwmCounter += 16;
    if (pwmCounter > 255)
    {
        // new cycle - reset the counter and latch the current color
        pwmCounter = 0;
        latchedGPIO = curGPIO;
    }

    // update each channel
    gpio_put(gpioR, latchedGPIO.r > pwmCounter);
    gpio_put(gpioG, latchedGPIO.g > pwmCounter);
    gpio_put(gpioB, latchedGPIO.b > pwmCounter);

    // figure the next target time; if we've fallen behind, catch up
    alarmTargetTime += 1250;
    uint64_t now = time_us_64();
    if (alarmTargetTime <= now + 100)
        alarmTargetTime = now + 1250;

    // set the next time
    hardware_alarm_set_target(alarmNum, alarmTargetTime);
}

// Calculate the new color display
StatusRGB::RGB StatusRGB::CalcColor() const
{
    // if disabled, set to all off
    if (!enabled)
        return { 0, 0, 0 };

    // check for a manual override
    if (manualOverride)
        return manualColor;
    
    // Figure the color to display for the current point in a flash cycle.
    // The times are in 1024 microsecond units (approximately milliseconds,
    // just faster to divide).
    static auto Flash = [](RGB onColor, RGB offColor, int period, int onTime) -> RGB
    {
        // Get the current system time in 1024us units
        uint64_t now = time_us_64() >> 10;

        // get the time modulo the period, to determine where we are
        // in the duty cycle
        int t = static_cast<int>(now % period);

        // Figure if we're in the ON or OFF period; the ON period is
        // the first 'onTime' units of the cycle, and the OFF period
        // is the balance of the cycle.  If we're in the ON period,
        // return the ON color, else the OFF color.
        return t <= onTime ? onColor : offColor;
    };

    // Double flash pattern - two on flashes per period.
    static auto Flash2 = [](RGB onColor, RGB offColor, int period, int onTime1, int offTime1, int onTime2) -> RGB
    {
        // Get the current system time in 1024us units
        uint64_t now = time_us_64() >> 10;

        // get the time modulo the period, to determine where we are
        // in the duty cycle
        int t = static_cast<int>(now % period);

        // Figure if we're in the ON-OFF-ON-OFF periods
        return t <= onTime1 ? onColor :
            t <= onTime1 + offTime1 ? offColor :
            t <= onTime1 + offTime1 + onTime2 ? onColor :
            offColor;
    };
    
    // Start with the USB connection status
    if (!usbIfc.WasEverMounted())
    {
        // never mounted, so we're waiting for the initial PC connection
        // after a reset; short yellow flashes
        return Flash2(yellow, { 0, 0, 0 }, 2000, 50, 100, 50);
    }
    if (!usbIfc.IsMounted())
    {
        // not mounted - two short red flashes every 2 seconds
        return Flash2(red, { 0, 0, 0 }, 2000, 50, 100, 50);
    }
    if (usbIfc.IsSuspended())
    {
        // mounted but suspended - short green flashes every 2 seconds
        return Flash(green, { 0, 0, 0 }, 2000, 50);
    }

    // The KL25Z Pinscape software had a fourth USB trouble mode that we
    // omit: alternating red/yellow, meaning that the host PC hasn't been
    // polling for HID input on the joystick interface over the past few
    // seconds.  The KL25Z software took that as a proxy for connection
    // trouble, which works on Windows because the Windows HID driver polls
    // HIDs for input whether or not any applications are using the input.
    // So if we didn't see any HID polling for a few seconds, it was a sure
    // sign that the connection was broken.  This information was useful on
    // the KL25Z because the mbed stack seemed unable to detect certain
    // kinds of USB connection failures; it would often think things were
    // fine even when Windows saw the device as disconnected.
    //
    // Unfortunately, this approach doesn't work on Linux, because the Linux
    // HID driver doesn't poll all the time, as the Windows HID driver does.
    // The Linux driver only polls when an application is actively asking
    // for input.  So KL25Z Pinscape goes into red/yellow error mode on
    // Linux any time there isn't an application consuming joystick input.
    // Even worse, the supposed error triggers the firmware's corrective
    // measures to try to recover the USB connection, which include a full
    // reset on the KL25Z.  To work around this, Linux users have to
    // manually disable the reset-on-USB-trouble option, and even after
    // doing that they still get the annoying red/yellow error status
    // indication when no joystick-aware applications are running.
    //
    // So for this version, we've completely removed the joystick polling
    // check and the associated red/yellow error status.
    //
    // Fortunately, we don't seem to need the HID polling proxy on the Pico.
    // The TinyUSB library seems reliable at detecting and reporting the
    // connection status, unlike the mbed stack, so we just don't need this
    // additional check.  So we've dropped it - there's no equivalent of the
    // KL25Z red/yellow status in this version.


    // Next, check the TV ON status
    if (tvOn.IsInCountdown())
    {
        // TV ON countdown in progress - blue flashes, 500ms on, 500ms off
        return Flash(blue, { 0, 0, 0 }, 1000, 500);
    }
    else if (tvOn.IsSendingCommands())
    {
        // TV relay or IR in progress - fast blue
        return Flash(blue, { 0, 0, 0 }, 100, 50);
    }

    // If we're in safe mode, show fast red/orange
    if (config.IsSafeMode())
        return Flash(red, orange, 500, 250);

    // If we're using factory settings, show slow green/dim green
    if (config.IsFactorySettings())
        return Flash(green, { 0, 32, 0 }, 2000, 1000);

    // Plunger calibration mode shows solid blue
    if (plunger.IsCalMode())
        return blue;

    // We're not in plunger calibration mode, so check i the calibration
    // button is being held down.  If so, flash blue.
    if (plunger.IsCalButtonPushed())
        return Flash(blue, { 0, 0, 0}, 500, 250);

    // If nudge calibration is active, show solid violet
    if (nudgeDevice.IsCalMode())
        return violet;

    // If the plunger is enabled but not calibrated, show slow green/yellow
    if (plunger.IsEnabled() && !plunger.IsCalibrated())
        return Flash(green, yellow, 2000, 1000);

    // We didn't find any other condition to report, so display slow
    // alternating blue/green for "normal healthy operation".
    return Flash(blue, green, 2000, 1000);
}

// console command
void StatusRGB::Command_main(const ConsoleCommandContext *c)
{
    if (c->argc < 2)
        return c->Usage();

    auto &s = statusRGB;
    for (int argi = 1 ; argi < c->argc ; ++argi)
    {
        const char *a = c->argv[argi];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            c->Printf(
                "Status LED\n"
                "  Red:     GP%d\n"
                "  Green:   GP%d\n"
                "  Blue:    GP%d\n"
                "  Outputs: Active %s\n"
                "  Color:   #%02X%02X%02X\n",
                s.gpioR, s.gpioG, s.gpioB,
                s.activeHigh ? "High" : "Low",
                s.curColor.r, s.curColor.g, s.curColor.b);
        }
        else if (strcmp(a, "--set") == 0)
        {
            if (++argi >= c->argc)
                return c->Printf("statusled: missing color argument for --set\n");

            RGB rgb;
            if (!ParseColorString(rgb, c->argv[argi]))
                return c->Printf("statusled: invalid color argument \"%s\" for --set, expected #RGB or #RRGGBB format\n", c->argv[argi]);

            s.manualColor = rgb;
            s.manualOverride = true;
            c->Printf("Color manually set to #%02X%02X%02X\n", rgb.r, rgb.g, rgb.b);
        }
        else if (strcmp(a, "--auto") == 0)
        {
            c->Printf("Resuming automatic status light display\n");
            s.manualOverride = false;
        }
        else
        {
            return c->Printf("statusled: unknown option \"%s\"\n", a);
        }
    }
}
