// Pinscape Pico - Gamepad device interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines the Pinscape Pico's USB virtual gamepad interface, which
// provides a simulated joystick-type control.  In a virtual pinball
// context, gamepads are used for button inputs (as an alternative to
// keyboard keys), and for plunger and nudge inputs (using the joystick
// axes, which inherently represent analog quantities).


// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <ctype.h>
#include <unordered_map>
#include <vector>
#include <functional>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/unique_id.h>

// project headers
#include "USBIfc.h"
#include "Utils.h"
#include "BytePackingUtils.h"
#include "Pinscape.h"
#include "Logger.h"
#include "USBCDC.h"
#include "VendorIfc.h"
#include "Config.h"
#include "JSON.h"
#include "PicoLED.h"
#include "Version.h"
#include "Nudge.h"

// HID device singleton
USBIfc::Gamepad gamepad;

// construction
USBIfc::Gamepad::Gamepad() : HID("gamepad", false, ReportIDGamepad, SerialBitGamepad)
{
    CommandConsole::AddCommand(
        "gamepad", "HID gamepad emulation options",
        "gamepad [option...]\n"
        "  -d, --disable         disable gamepad USB reports\n"
        "  -e, --enable          enable gamepad USB reports\n"
        "  -r, --reset-stats     reset statistics\n"
        "  -s, --status          show status (default if no options are supplied)\n"
        "\n"
        "Note: all options are for the current session; the saved configuration isn't affected.",
        &Command_gamepad);
}

// console 'gamepad' command handler
void USBIfc::Gamepad::Command_gamepad(const ConsoleCommandContext *c)
{
    static const auto Status = [](const ConsoleCommandContext *c) {
        c->Print("Gamepad HID status:\n");
        gamepad.stats.Log(c, &gamepad);
    };

    if (c->argc == 1)
        return Status(c);

    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *s = c->argv[i];
        if (strcmp(s, "-d") == 0 || strcmp(s, "--disable") == 0)
        {
            gamepad.enabled = false;
            c->Print("Gamepad reports disabled\n");
        }
        else if (strcmp(s, "-e") == 0 || strcmp(s, "--enable") == 0)
        {
            gamepad.enabled = true;
            c->Print("Gamepad reports enabled\n");
        }
        else if (strcmp(s, "-s") == 0 || strcmp(s, "--status") == 0)
        {
            Status(c);
        }
        else if (strcmp(s, "-r") == 0 || strcmp(s, "--reset-stats") == 0)
        {
            gamepad.stats.Reset();
            c->Print("Gamepad HID statistics reset\n");
        }
        else
        {
            c->Printf("Invalid option \"%s\"\n", s);
            return;
        }
    }
}

// Configure from JSON data
bool USBIfc::Gamepad::Configure(JSONParser &json)
{
    // presume disabled if not configured
    bool enabled = false;

    // check for the main gamepad JSON key
    if (auto *val = json.Get("gamepad") ; !val->IsUndefined())
    {
        // get the enable status
        enabled = val->Get("enable")->Bool();

        // get the axis settings
        LogicalAxis::CtorParams params{ "gamepad", &nudgeDeviceView };
        xSource = LogicalAxis::Configure(params, val, "x");
        ySource = LogicalAxis::Configure(params, val, "y");
        zSource = LogicalAxis::Configure(params, val, "z");
        rxSource = LogicalAxis::Configure(params, val, "rx");
        rySource = LogicalAxis::Configure(params, val, "ry");
        rzSource = LogicalAxis::Configure(params, val, "rz");
        slider1Source = LogicalAxis::Configure(params, val, "slider1");
        slider2Source = LogicalAxis::Configure(params, val, "slider2");
    }

    // return the 'enabled' status
    return enabled;
}

// USB report descriptor
const uint8_t *USBIfc::Gamepad::GetReportDescriptor(uint16_t *byteLength)
{
    // Gamepad Report Descriptor Template
    //   32 buttons
    //   X/Y/Z joystick
    //   Rx/Ry/Rz joystick
    //   2 sliders
    //
    // *** IMPORTANT ***
    // Any changes to the report layout also require updating ReportLength
    // (static const member variable) to match the report byte length.
    //
    static const uint8_t desc[] = {
        HID_USAGE_PAGE (HID_USAGE_PAGE_DESKTOP),
        HID_USAGE      (HID_USAGE_DESKTOP_GAMEPAD),
        HID_COLLECTION (HID_COLLECTION_APPLICATION),

            // Report ID
            HID_REPORT_ID     (ReportIDGamepad)

            // 32 buttons
            HID_USAGE_PAGE    (HID_USAGE_PAGE_BUTTON),
            HID_USAGE_MIN     (1),                   // button ID #1 to #32
            HID_USAGE_MAX     (32),
            HID_LOGICAL_MIN   (0),                   // button value range 0-1 (off/on)
            HID_LOGICAL_MAX   (1),
            HID_REPORT_COUNT  (32),                  // 32 reports
            HID_REPORT_SIZE   (1),                   // ...1 bit each report
            HID_INPUT         (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

            // 16 bit X, Y, Z, Rz, Rx, Ry (min -32767, max +32767)
            HID_USAGE_PAGE    (HID_USAGE_PAGE_DESKTOP),
            HID_USAGE         (HID_USAGE_DESKTOP_X),
            HID_USAGE         (HID_USAGE_DESKTOP_Y),
            HID_USAGE         (HID_USAGE_DESKTOP_Z),
            HID_USAGE         (HID_USAGE_DESKTOP_RX),
            HID_USAGE         (HID_USAGE_DESKTOP_RY),
            HID_USAGE         (HID_USAGE_DESKTOP_RZ),
            HID_LOGICAL_MIN_N (-32767, 2),
            HID_LOGICAL_MAX_N (32767, 2),
            HID_REPORT_COUNT  (6),
            HID_REPORT_SIZE   (16),
            HID_INPUT         (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
            
            // Two 16 bit sliders
            HID_USAGE_PAGE    (HID_USAGE_PAGE_DESKTOP),
            HID_USAGE         (HID_USAGE_DESKTOP_SLIDER),
            HID_USAGE         (HID_USAGE_DESKTOP_SLIDER),
            HID_LOGICAL_MIN_N (-32767, 2),
            HID_LOGICAL_MAX_N (32767, 2),
            HID_REPORT_COUNT  (2),
            HID_REPORT_SIZE   (16),
            HID_INPUT         (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_COLLECTION_END 
    };
    *byteLength = sizeof(desc);
    return desc;
}

void USBIfc::Gamepad::ButtonEvent(int buttonNum, bool isDown)
{
    // update the button in the helper
    buttons.OnButtonEvent(buttonNum, isDown);

    // record the event time
    stats.MarkEventTime(time_us_64());
}

uint16_t USBIfc::Gamepad::GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // Fill in the request buffer if there's room
    uint8_t *p = buf;
    if (type == HID_REPORT_TYPE_INPUT && reqLen >= ReportLength)
    {
        // Snapshot the nudge device view
        if (nudgeDeviceView != nullptr)
            nudgeDeviceView->TakeSnapshot();

        // *** IMPORTANT ***
        // Any changes to the byte layout below require updating ReportLength
        // (static const member variable) to match the new byte length.

        // Store the button bits, using the "sticky" version in the next
        // report register.  Note that this memcpy() will only put the
        // button bits in the HID packet in the correct order if the local
        // platform is litle-Endian - Pico is, and this code is already
        // deeply Pico-specific, so there's no need for a byte-ordering
        // abstraction here.
        uint32_t btmp = buttons.Report();
        static_assert(sizeof(btmp) == 4);
        memcpy(p, &btmp, 4);
        p += 4;

        // Store the 16-bit axis elements.
        PutInt16(p, xSource->Read());
        PutInt16(p, ySource->Read());
        PutInt16(p, zSource->Read());
        PutInt16(p, rxSource->Read());
        PutInt16(p, rySource->Read());
        PutInt16(p, rzSource->Read());
        PutInt16(p, slider1Source->Read());
        PutInt16(p, slider2Source->Read());

        // *** IMPORTANT ***
        // Any changes to the byte layout above require updating ReportLength
        // (static const member variable) to match the new byte length.

        // only send the report if it changed from the last one
        if (memcmp(lastReportCapture, buf, ReportLength) == 0)
            return 0;

        // capture the new report
        memcpy(lastReportCapture, buf, ReportLength);
    }

    // return the length populated
    return static_cast<uint16_t>(p - buf);
}
