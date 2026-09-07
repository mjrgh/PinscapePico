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

        // Note which axes carry the accelerometer, so the descriptor can say
        // what their counts mean. Only the acceleration sources qualify:
        // nudge.vx and nudge.vy are velocities, in units of the device's own
        // choosing, with no physical name to give them.
        static const char *const axisProps[8] = {
            "x", "y", "z", "rx", "ry", "rz", "slider1", "slider2"
        };
        for (int i = 0 ; i < 8 ; ++i)
        {
            if (auto *v = val->Get(axisProps[i]) ; !v->IsUndefined())
            {
                const std::string src = v->String();
                axisIsAccel[i] = (src == "nudge.x" || src == "nudge.y");
            }
        }
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
    static const uint8_t descPrefix[] = {
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

    };

    // Everything after the axes, unchanged.
    static const uint8_t descSuffix[] = {
            // Hat switch
            HID_USAGE_PAGE    (HID_USAGE_PAGE_DESKTOP),
            HID_USAGE         (HID_USAGE_DESKTOP_HAT_SWITCH),
            HID_LOGICAL_MIN   (1),
            HID_LOGICAL_MAX   (8),
            HID_PHYSICAL_MIN  (0),
            HID_PHYSICAL_MAX_N(315, 2),
            HID_UNIT          (20),  // English rotation units - degrees
            HID_REPORT_COUNT  (1),
            HID_REPORT_SIZE   (4),
            HID_INPUT         (HID_DATA | HID_VARIABLE | HID_ABSOLUTE | HID_NULL_STATE),
        
            // Four bits of padding (to fill out the byte)
            HID_REPORT_COUNT  (1),
            HID_REPORT_SIZE   (4),
            HID_INPUT         (HID_CONSTANT | HID_ABSOLUTE),

        HID_COLLECTION_END 
    };

    // Already built? The descriptor depends only on configuration, which is
    // fixed for as long as the device stays enumerated.
    if (reportDescLen != 0)
    {
        *byteLength = reportDescLen;
        return reportDesc;
    }

    // The axes, emitted in runs so that the ones carrying the accelerometer can
    // declare what their counts mean and the others can stay silent.
    //
    // The wire format does not change: the same eight 16-bit axes in the same
    // order, so a host reading the report as it always has sees exactly what it
    // saw before. What changes is that PHYSICAL_MINIMUM/MAXIMUM and UNIT now say
    // the acceleration those counts stand for, which the Linux input layer turns
    // into input_absinfo::resolution and SDL divides by -- so the scale reaches
    // an application with no driver written for this board.
    //
    // Runs rather than one block because the axis assignment is the user's:
    // the accelerometer can land on any of the eight, and they need not be
    // adjacent. Each run carries its own usages and its own units.
    //
    // UNIT is 0xE011 -- SI Linear in nibble 0, length^1 in nibble 1, time^-2 in
    // nibble 3, which is metres per second squared. UNIT_EXPONENT -2 (0x0E as a
    // 4-bit signed value) puts the physical bounds in hundredths, so +/-16 g is
    // +/-15691, inside the 16-bit field that whole m/s^2 would have rounded away.
    uint8_t *p = reportDesc;
    auto emit = [&p](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes)
            *p++ = b;
    };

    memcpy(p, descPrefix, sizeof(descPrefix));
    p += sizeof(descPrefix);

    // Usage codes for the eight axes, in report order. The two sliders share a
    // usage, which HID allows.
    static const uint8_t axisUsage[8] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x36 };
    const int gRange = nudgeDevice.GetGRange();
    // Hundredths of m/s^2 at full scale, matching UNIT_EXPONENT -2.
    const int16_t physHi = static_cast<int16_t>(gRange * 980.665f);
    const int16_t physLo = static_cast<int16_t>(-physHi);

    emit({ 0x05, 0x01 });                       // USAGE_PAGE (Generic Desktop)
    emit({ 0x16, 0x01, 0x80 });                 // LOGICAL_MINIMUM (-32767)
    emit({ 0x26, 0xFF, 0x7F });                 // LOGICAL_MAXIMUM (32767)
    emit({ 0x75, 0x10 });                       // REPORT_SIZE (16)

    // An axis mapped to nudge.x or nudge.y with no range to report is a
    // contradiction: GetGRange() returns zero only for the null device, so this
    // means the configuration assigned an axis to an accelerometer that isn't
    // there. The axis then goes out with no unit, which is the honest thing to
    // say -- nothing is measuring it -- but it is worth saying out loud rather
    // than leaving a silently unitless axis for someone to puzzle over.
    if (gRange <= 0)
    {
        for (int i = 0 ; i < 8 ; ++i)
        {
            if (axisIsAccel[i])
            {
                Log(LOG_ERROR, "gamepad: an axis is mapped to the nudge device, but no "
                    "accelerometer is configured, so its units can't be declared\n");
                break;
            }
        }
    }

    for (int i = 0 ; i < 8 ; )
    {
        // Take the longest run of axes that agree about carrying acceleration.
        const bool accel = axisIsAccel[i] && gRange > 0;
        int j = i;
        while (j < 8 && (axisIsAccel[j] && gRange > 0) == accel)
            ++j;

        for (int k = i ; k < j ; ++k)
            emit({ 0x09, axisUsage[k] });       // USAGE

        if (accel)
        {
            emit({ 0x36, static_cast<uint8_t>(physLo & 0xff),
                   static_cast<uint8_t>((physLo >> 8) & 0xff) });   // PHYSICAL_MINIMUM
            emit({ 0x46, static_cast<uint8_t>(physHi & 0xff),
                   static_cast<uint8_t>((physHi >> 8) & 0xff) });   // PHYSICAL_MAXIMUM
            emit({ 0x66, 0x11, 0xE0 });         // UNIT (SI Linear, m * s^-2)
            emit({ 0x55, 0x0E });               // UNIT_EXPONENT (-2)
        }
        else
        {
            // Clear them again: these are global items, so a run that inherited
            // the previous run's unit would claim an acceleration it does not
            // report. 0/0 physical bounds mean "same as logical", i.e. none.
            emit({ 0x35, 0x00 });               // PHYSICAL_MINIMUM (0)
            emit({ 0x45, 0x00 });               // PHYSICAL_MAXIMUM (0)
            emit({ 0x65, 0x00 });               // UNIT (None)
            emit({ 0x55, 0x00 });               // UNIT_EXPONENT (0)
        }

        emit({ 0x95, static_cast<uint8_t>(j - i) });   // REPORT_COUNT
        emit({ 0x81, 0x02 });                          // INPUT (Data, Var, Abs)
        i = j;
    }

    memcpy(p, descSuffix, sizeof(descSuffix));
    p += sizeof(descSuffix);

    reportDescLen = static_cast<uint16_t>(p - reportDesc);
    *byteLength = reportDescLen;
    return reportDesc;
}

void USBIfc::Gamepad::ButtonEvent(int buttonNum, bool isDown)
{
    // update the button in the helper
    buttons.OnButtonEvent(buttonNum, isDown);

    // record the event time
    stats.MarkEventTime(time_us_64());
}

void USBIfc::Gamepad::HatSwitchEvent(int buttonNum, bool isDown)
{
    // update the button in the helper
    hatSwitch.OnButtonEvent(buttonNum, isDown);

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

        // Store the hat switch value.  By convention, a hat switch on a
        // real gamepad consists of four switches, UP DOWN LEFT RIGHT, which
        // are typically activated by a joystick.  Moving the joystick to
        // the right activates the RIGHT switch, for example.  Moving the
        // joystick to diagonal positions activates adjacent switches,
        // such as RIGHT+DOWN for the "southeast" joystick position.
        //
        // The HID report DOESN'T report the individual button states.
        // Instead, it reports the joystick position inferred from the
        // button states, as the angular position of the joystick
        // relative to its center position, in a unit system where one
        // unit equals 45 degrees, with zero degrees at North (UP), and
        // the angle increasing in the clockwise direction.  The HID
        // report also has a NULL state that represents the joystick at
        // its center position, with none of the buttons pressed.
        //
        // HID lets us define the system of units to represent this angular
        // coordinate system, by setting a logical range and a physical
        // range.  Since we have 8 possible angular states, we'll use a
        // logical range of 1..8 to represent a physical range of 0..315
        // degrees.  HID interprets any value outside of the logical range
        // as a NULL, so using 1..8 as our logical range lets us use 0 as
        // the NULL value.
        //
        // Since the input source is the four independent buttons, and the
        // HID report value is this peculiarly encoded angular value, we
        // have to map from the button states to the HID report value.
        // Note that about half of the button states have no representation
        // in the HID report, since the notional joystick arrangement can't
        // physically reach those button states.  Since we don't have any
        // control over the physical input arrangement, though, there's no
        // guarantee that our inputs are constrained in the same way as a
        // true hat swtich, so it's entirely possible that our four buttons
        // can be pressed in combinations that aren't possible with a true
        // hat switch, and thus that we'll see input states that can't be
        // represented in the HID report, such as UP+DOWN, or three buttons
        // pressed at once.  We just report NULL (center position) in such
        // cases.
        //
        // The live button state is encoded as a bit vector in our hatSwitch
        // button helper object, with the first button at the least significant
        // bit (bit mask 0x0001), the second button at bit 0x0002, and so on.
        // We assing the buttons in order as UP, DOWN, LEFT, RIGHT, so the
        // low-order four bits of the hatSwitch bit vector for a 4-bit number,
        // inherently with range 0x00 to 0x0F.  We can thus use that as an
        // index into a mapping array, picking out the HID report value that
        // corresponds to each possible combination of the four button states.
        static const uint8_t hatSwitchMap[] {
            // HID    Angle  Dir   R L D U   Hex
            0,     // NULL   NULL  0 0 0 0   0x00
            1,     // 0      N     0 0 0 1   0x01
            5,     // 180    S     0 0 1 0   0x02
            0,     // NULL   NULL  0 0 1 1   0x03
            7,     // 270    W     0 1 0 0   0x04
            8,     // 315    NW    0 1 0 1   0x05
            6,     // 225    SW    0 1 1 0   0x06
            0,     // NULL   NULL  0 1 1 1   0x07
            3,     // 90     E     1 0 0 0   0x08
            2,     // 45     NE    1 0 0 1   0x09
            4,     // 135    SE    1 0 1 0   0x0A
            0,     // NULL   NULL  1 0 1 1   0x0B
            0,     // NULL   NULL  1 1 0 0   0x0C
            0,     // NULL   NULL  1 1 0 1   0x0D
            0,     // NULL   NULL  1 1 1 0   0x0E
            0,     // NULL   NULL  1 1 1 1   0x0F
        };
        *p++ = hatSwitchMap[hatSwitch.Report() & 0x0F];

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
