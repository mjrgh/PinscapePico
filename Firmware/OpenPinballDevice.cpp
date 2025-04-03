// Pinscape Pico - Open Pinball Device interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The Open Pinball Device HID interface is an implementation of the
// Game Controls usage page (0x05) Pinball Device usage (0x02) from the
// HID usage tables published by USB-IF.  It's a proposed standard for
// use by the virtual pin cab builder/user community, and is meant to
// provide an alternative to the traditional joystick HID interface as
// a way for pin cab I/O controllers to convey sensor input to pinball
// simulator programs on the PC.

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
#include "../OpenPinballDevice/OpenPinballDeviceReport.h"
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


// Additional HID usage codes not defined in tinyusb headers - Usage page 0x05 (Game Controls)
// See HID Usage Tables (https://usb.org/sites/default/files/hut1_5.pdf), sections 8 (Game
// Controls page), 8.2 (Pinball Device)
#define HID_USAGE_GAME_PINBALLDEVICE      0x02        // Pinball Device (CA)
#define HID_USAGE_GAME_FLIPPER            0x2A        // Flipper (MC)
#define HID_USAGE_GAME_SECONARY_FLIPPER   0x2B        // Secondary Flipper (MC)
#define HID_USAGE_GAME_BUMP               0x2C        // Bump (MC)
#define HID_USAGE_GAME_NEW_GAME           0x2D        // New Game (OSC)
#define HID_USAGE_GAME_SHOOT_BALL         0x2E        // Shoot Ball (OSC)
#define HID_USAGE_GAME_PLAYER             0x2F        // Player (OSC)

// HID device singleton
USBIfc::OpenPinballDevice openPinballDevice;

// construction
USBIfc::OpenPinballDevice::OpenPinballDevice() : HID("OpenPinballDevice", false, ReportIDPinballDevice, SerialBitPinballDevice)
{
    CommandConsole::AddCommand(
        "pindev", "Open Pinball Controller HID device options",
        "pindev [option...]\n"
        "  -d, --disable         disable pinball device USB reports\n"
        "  -e, --enable          enable pinball device USB reports\n"
        "  -r, --reset-stats     reset statistics\n"
        "  -s, --status          show status (default if no options are supplied)\n"
        "\n"
        "Note: all options are for the current session; the saved configuration isn't affected.",
        &Command_pindev);
}

// console 'pindev' command handler
void USBIfc::OpenPinballDevice::Command_pindev(const ConsoleCommandContext *c)
{
    static const auto Status = [](const ConsoleCommandContext *c) {
        c->Print("Pinball Device HID status:\n");
        openPinballDevice.stats.Log(c, &openPinballDevice);
    };

    if (c->argc == 1)
        return Status(c);

    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *s = c->argv[i];
        if (strcmp(s, "-d") == 0 || strcmp(s, "--disable") == 0)
        {
            openPinballDevice.enabled = false;
            c->Print("Pinbacll Device reports disabled\n");
        }
        else if (strcmp(s, "-e") == 0 || strcmp(s, "--enable") == 0)
        {
            openPinballDevice.enabled = true;
            c->Print("Pinball Device reports enabled\n");
        }
        else if (strcmp(s, "-s") == 0 || strcmp(s, "--status") == 0)
        {
            Status(c);
        }
        else if (strcmp(s, "-r") == 0 || strcmp(s, "--reset-stats") == 0)
        {
            openPinballDevice.stats.Reset();
            c->Print("Pinball Device HID statistics reset\n");
        }
        else
        {
            c->Printf("Invalid option \"%s\"\n", s);
            return;
        }
    }
}

// Configure from JSON data
bool USBIfc::OpenPinballDevice::Configure(JSONParser &json)
{
    // presume disabled if not configured
    bool enabled = false;

    // check for the main JSON key
    if (auto *val = json.Get("openPinballDevice") ; !val->IsUndefined())
    {
        // get the enable status
        enabled = val->Get("enable")->Bool();

        // if enabled, set up the logical axis inputs
        if (enabled)
        {
            // create a nudge device view
            nudgeDeviceView = nudgeDevice.CreateView();

            // Create the logical axis inputs.  The Open Pinball Device
            // interface defines the axis assignments, so these don't
            // require any user configuration.
            LogicalAxis::CtorParams params{ "openPinballDevice", &nudgeDeviceView };
            axNudgeSource = LogicalAxis::Configure(params, val, "axNudge", "nudge.x");
            ayNudgeSource = LogicalAxis::Configure(params, val, "ayNudge", "nudge.y");
            vxNudgeSource = LogicalAxis::Configure(params, val, "vxNudge", "nudge.vx");
            vyNudgeSource = LogicalAxis::Configure(params, val, "vyNudge", "nudge.vy");
            plungerPosSource = LogicalAxis::Configure(params, val, "plungerPos", "plunger.z0");
            plungerSpeedSource = LogicalAxis::Configure(params, val, "plungerSpeed", "plunger.speed");

            // report
            Log(LOG_CONFIG, "Open Pinball Device HID configured\n");
        }
    }

    // return the 'enabled' status
    return enabled;
}

// USB report descriptor
const uint8_t *USBIfc::OpenPinballDevice::GetReportDescriptor(uint16_t *byteLength)
{
    // Pinball Device Report Descriptor Template
    static const uint8_t desc[] = {
        HID_USAGE_PAGE (HID_USAGE_PAGE_GAME),
        HID_USAGE      (HID_USAGE_GAME_PINBALLDEVICE),
        HID_COLLECTION (HID_COLLECTION_APPLICATION),

            // Report ID
            HID_REPORT_ID     (ReportIDPinballDevice)

            // OpenPinballDeviceReport struct
            HID_REPORT_ITEM   (STRDESC_OPENPINDEV_LBL, RI_LOCAL_STRING_INDEX, RI_TYPE_LOCAL, 1),  // string label for output report
            HID_USAGE         (0x00),            // undefined 0x00 -> undefined/vendor-specific (opaque data for application-specific use)
            HID_LOGICAL_MIN   (0x00),            // byte range
            HID_LOGICAL_MAX   (0xFF),            //   00-FF
            HID_REPORT_SIZE   (8),               // 8-bit bytes
            HID_REPORT_COUNT  (OPENPINDEV_STRUCT_USB_SIZE),   // number of bytes == packed struct size
            HID_INPUT         (HID_ARRAY),       // input (device-to-host), array

        HID_COLLECTION_END 
    };
    *byteLength = sizeof(desc);
    return desc;
}

void USBIfc::OpenPinballDevice::GenericButtonEvent(int buttonNum, bool isDown)
{
    // update the button in the button helper
    genericButtons.OnButtonEvent(buttonNum, isDown);

    // mark the event time
    stats.MarkEventTime(time_us_64());
}

void USBIfc::OpenPinballDevice::PinballButtonEvent(int buttonNum, bool isDown)
{
    // update the button in the button helper - note that the button numbering follows
    // the index in the spec, 0..31, so we have to adjust to a 1-based index
    pinballButtons.OnButtonEvent(buttonNum + 1, isDown);

    // mark the event time
    stats.MarkEventTime(time_us_64());
}

uint16_t USBIfc::OpenPinballDevice::GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // fill in the request buffer if there's room
    uint8_t *p = buf;
    if (type == HID_REPORT_TYPE_INPUT && reqLen >= sizeof(OpenPinballDeviceReport))
    {
        // Snapshot the nudge device view
        if (nudgeDeviceView != nullptr)
            nudgeDeviceView->TakeSnapshot();

        // construct the report
        PutUInt64(p, time_us_64());
        PutUInt32(p, genericButtons.Report());
        PutUInt32(p, pinballButtons.Report());
        PutInt16(p, axNudgeSource->Read());
        PutInt16(p, ayNudgeSource->Read());
        PutInt16(p, vxNudgeSource->Read());
        PutInt16(p, vyNudgeSource->Read());
        PutInt16(p, plungerPosSource->Read());
        PutInt16(p, plungerSpeedSource->Read());
    }

    // return the length populated
    return static_cast<uint16_t>(p - buf);
}
