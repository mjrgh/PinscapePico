// Pinscape Pico - Keyboard and Media Control HID implementation
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implements our virtual USB Keyboard and Media Control devices.
//
// The Keyboard device implements an emulator for an ordinary USB
// keyboard.  We use a very standard Report Descriptor for the keyboard
// to ensure that all host operating systems will recognize it properly.
// Some hosts are inflexible about what sort of keyboard format they'll
// accept, even though in principle the USB HID spec allows for
// considerable variation.
//
// Because of that inflexibility in what sort of keyboard report format
// we can count on hosts to accept, we ALSO provide a Media Control
// device as a separate HID interface, alongside the keyboard interface.
// The Media Control device is a package deal with the keyboard - it's
// enabled if the keyboard is, and from the user's perspective, it's
// part of the same imaginary keyboard we implement.  The Media Control
// device adds a number of special media keys that appear on many of the
// fancier physical keyboards, for functions such as audio volume
// control, muting, and basic audio/video player actions (play, pause,
// skip back, skip ahead).  Although some of these functions also have
// equivalents in the regular USB Keyboard Usage Page, the Consumer page
// versions seem to be preferred by keyboard manufacturers, perhaps
// because Windows is more consistent about recognizing the Consumer
// Usage Page keys.  Or maybe Windows handles the Consumer Usage Page
// keys because that's what all of the keyboard makers use.  Either way,
// if you want a Mute button to work reliably on Windows, you're better
// off mapping it as Usage Page 0x0C (Consumer) Usage 0xE2 (Mute).


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
#include "Pinscape.h"
#include "Logger.h"
#include "USBCDC.h"
#include "VendorIfc.h"
#include "Config.h"
#include "JSON.h"
#include "PicoLED.h"
#include "Version.h"
#include "CommandConsole.h"


// ----------------------------------------------------------------------------
//
// HID device singletons
//
USBIfc::Keyboard keyboard;
USBIfc::MediaControl mediaControl;


// ----------------------------------------------------------------------------
//
// Keyboard
//

USBIfc::Keyboard::Keyboard() : HID("kb", true, ReportIDKeyboard, SerialBitKeyboard)
{
    CommandConsole::AddCommand(
        "kb", "HID keyboard emulation options",
        "kb [option...]\n"
        "  -d, --disable         disable keyboard USB reports\n"
        "  -e, --enable          enable keyboard USB reports\n"
        "  -m, --disable-media   disable media control reports\n"
        "  -M, --enable-media    enable media control reports\n"
        "  -r, --reset-stats     reset statistics\n"
        "  -s, --status          show status (default if no options are supplied)\n"
        "\n"
        "Note: all options are for the current session; the saved configuration isn't affected.",
        &Command_kb);
}

// console 'kb' command handler
void USBIfc::Keyboard::Command_kb(const ConsoleCommandContext *c)
{
    static const auto Status = [](const ConsoleCommandContext *c) {
        c->Printf("Keyboard HID status:\n");
        keyboard.stats.Log(c, &keyboard);
        c->Printf("\nMedia HID status;\n");
        mediaControl.stats.Log(c, &mediaControl);
    };

    if (c->argc == 1)
        return Status(c);

    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *s = c->argv[i];
        if (strcmp(s, "-d") == 0 || strcmp(s, "--disable") == 0)
        {
            keyboard.enabled = false;
            c->Print("Keyboard reports disabled\n");
        }
        else if (strcmp(s, "-e") == 0 || strcmp(s, "--enable") == 0)
        {
            keyboard.enabled = true;
            c->Print("Keyboard reports enabled\n");
        }
        else if (strcmp(s, "-m") == 0 || strcmp(s, "--disable-media") == 0)
        {
            mediaControl.enabled = false;
            c->Print("Media control reports disable\n");
        }
        else if (strcmp(s, "-M") == 0 || strcmp(s, "--enable-media") == 0)
        {
            mediaControl.enabled = true;
            c->Print("Media control reports enabled\n");
        }
        else if (strcmp(s, "-s") == 0 || strcmp(s, "--status") == 0)
        {
            Status(c);
        }
        else if (strcmp(s, "-r") == 0 || strcmp(s, "--reset-stats") == 0)
        {
            keyboard.stats.Reset();
            mediaControl.stats.Reset();
            c->Print("Keyboard/media control HID statistics reset\n");
        }
        else
        {
            c->Printf("Invalid option \"%s\"\n", s);
            return;
        }
    }
}

bool USBIfc::Keyboard::Configure(JSONParser &json)
{
    // presume disabled if not configured
    bool enabled = false;

    // check for our top-level keyboard JSON key
    if (auto *val = json.Get("keyboard") ; !val->IsUndefined())
    {
        // get the enable status
        enabled = val->Get("enable")->Bool();
    }

    // return the 'enabled' status
    return enabled;
}

const uint8_t *USBIfc::Keyboard::GetReportDescriptor(uint16_t *byteLength)
{
    static const uint8_t desc[] = {
        // Keyboard.  This is just a copy of Tinyusb's generic keyboard
        // report descriptor, which is essentially the reference format
        // for keyboards in the USB specification.  Most physical keyboards
        // use this report format because nearly all hosts will accept it.
        // We define our own private copy of the descriptor here, rather
        // than using the one from the Tinyusb headers, to insulate the
        // code from any changes in future Tinyusb versions.  We have
        // some dependencies elsewhere in this class upon the details of
        // the descriptor, so if we relied upon the #included version,
        // our code could break simply by building against a new version
        // of Tinyusb, without any changes having been made to our own
        // code.  That sort of thing can be annoying to track down
        // because the source of the breakage is hidden in third-party
        // code.  Better to define our own copy - that keeps it stable
        // no matter what Tinyusb does with their copy, and makes it
        // easier to correlate dependency breakage with the code that
        // changed, since both ends are part of the same code body.
        HID_USAGE_PAGE (HID_USAGE_PAGE_DESKTOP),
        HID_USAGE      (HID_USAGE_DESKTOP_KEYBOARD),
        HID_COLLECTION (HID_COLLECTION_APPLICATION),

            // Report ID
            HID_REPORT_ID    (ReportIDKeyboard)

            // 8 bits for modifier keys (Control, Shift, Alt, GUI).
            // The bits are packed into one byte in the reports in the
            // following order.  The USB usage code is shown for
            // reference, but this doesn't appear in the report itself
            // (it only appears in the report descriptor, below), as
            // the bits in the report are positional.
            //
            //   Code  Bit    Function
            //   224   0x01   Left Ctrl
            //   225   0x02   Left Shift
            //   226   0x04   Left Alt
            //   227   0x08   Left GUI (Windows key)
            //   228   0x10   Right Ctrl
            //   229   0x20   Right Shift
            //   230   0x40   Right Alt
            //   231   0x80   Right GUI
            //
            HID_USAGE_PAGE   (HID_USAGE_PAGE_KEYBOARD),
            HID_USAGE_MIN    (224),
            HID_USAGE_MAX    (231),
            HID_LOGICAL_MIN  (0),
            HID_LOGICAL_MAX  (1),
            HID_REPORT_COUNT (8),
            HID_REPORT_SIZE  (1),
            HID_INPUT        (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        
            // 8 bits reserved
            HID_REPORT_COUNT (1),
            HID_REPORT_SIZE  (8),
            HID_INPUT        (HID_CONSTANT),

            // 5 bits for LED Indicators.  As with the modifier keys,
            // these bits are positional.  The Usage column gives the USB
            // usage ID for the indicator, but this isn't stored in the
            // reports, just in the descriptor.
            //
            // Usage Bit    Function
            // 1     0x01   NumLock
            // 2     0x02   CapsLock
            // 3     0x04   ScrollLock
            // 4     0x08   Compose
            // 5     0x10   Kana
            //
            HID_USAGE_PAGE   (HID_USAGE_PAGE_LED),
            HID_USAGE_MIN    (1),
            HID_USAGE_MAX    (5),
            HID_REPORT_COUNT (5),
            HID_REPORT_SIZE  (1),
            HID_OUTPUT       (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

            // 3 bits of padding, to fill out the remainder of the LED byte
            HID_REPORT_COUNT (1),
            HID_REPORT_SIZE  (3),
            HID_OUTPUT       (HID_CONSTANT),

            // 6 keycodes, 8 bits each
            HID_USAGE_PAGE   (HID_USAGE_PAGE_KEYBOARD),
            HID_USAGE_MIN    (0),
            HID_USAGE_MAX_N  (255, 2),
            HID_LOGICAL_MIN  (0),
            HID_LOGICAL_MAX_N(255, 2),
            HID_REPORT_COUNT (6),
            HID_REPORT_SIZE  (8),
            HID_INPUT        (HID_DATA | HID_ARRAY | HID_ABSOLUTE),

        HID_COLLECTION_END
    };
    *byteLength = sizeof(desc);
    return desc;
}

uint8_t USBIfc::Keyboard::ModKeyToBitMask(uint8_t keyCode)
{
    // The modifier keys are in the range 224-231.  These keys are mapped
    // into bits in the HID report in consecutive report order, with the
    // first code (224, Left Control) at bit 0 (mask 0x01).  (The actual
    // implementation of the mapping in the Tinyusb header is a little
    // subtle: it defines a section with eight one-bit inputs, mapped
    // to usages HID_USAGE_MIN(224) through HID_USAGE_MAX(231).  That
    // maps the eight bits in the report, in order from least to most
    // significant, to the key codes 224 through 231.  When a bit in the
    // resulting report mask is '1', it means that the key for the
    // corresponding usage code (i.e., the key code) is pressed.  This
    // bit-mapped layout is not only the default Tinyusb format, but
    // also the de facto standard format that practically every physical
    // keyboard uses, since it practically guarantees universal host
    // compatibility.  If we ever did want to change our HID report
    // layout to something non-standard, we'd have to change this
    // accordingly, but I see little reason we'd ever do that, so I
    // don't think the assumption we're making here is much of a
    // maintenance liability.)
    if (keyCode >= 224 && keyCode <= 231)
        return 1 << (keyCode - 224);
    else
        return 0;
}

void USBIfc::Keyboard::KeyEvent(uint8_t keyCode, bool isDown)
{
    // set or clear the bit in the live keys
    liveKeys.set(keyCode, isDown);
    
    // If the new key state is different from the last report,
    // record the change for the next report.  Ignore it if it's
    // the same as the last report; that way, if we flip the bit
    // and then flip it back within a single USB polling cycle,
    // we'll send one report with the state change.  This ensures
    // that a very quick button tap generates at least one report
    // to the host, even if the tap occurs entirely within one
    // reporting cycle.
    if (reportedKeys.test(keyCode) != isDown)
        nextKeys.set(keyCode, isDown);

    // mark the event time
    stats.MarkEventTime(time_us_64());
}

bool USBIfc::Keyboard::SendReport()
{
    // Only send a report if we have state changes since the last report,
    // so that we don't send unnecessary empty reports.
    if (nextKeys != reportedKeys)
        return USBIfc::HID::SendReport();

    // no report sent
    return false;
}

uint16_t USBIfc::Keyboard::GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // fill in the request buffer if there's room
    uint8_t *p = buf;
    if (type == HID_REPORT_TYPE_INPUT && reqLen >= 8)
    {
        // Generate the modifier-key bit vector.  This encodes the keys
        // from USB code points 224-231 (left shift, left ctrl, etc) into
        // a packed 8-bit vector, as defined in our HID report format.
        uint8_t mod = 0;
        for (uint8_t bit = 1, key = 224 ; key <= 231 ; ++key, bit <<= 1)
        {
            if (nextKeys.test(key))
                mod |= bit;
        }

        // Scan for pressed keys, from code points 4 through 221.  (We've
        // already encoded the code points from 224 to 231 for the modifier
        // keys above, and the remaining code points are unused/reserved.)
        uint8_t keys[7] = { 0, 0, 0, 0, 0, 0, 0 };
        size_t nKeys = 0;
        for (uint8_t key = 4 ; key <= 221 && nKeys < _countof(keys) ; ++key)
        {
            if (nextKeys.test(key))
                keys[nKeys++] = key;
        }

        // If we have 7 or more keys down, it's a "rollover" error, since
        // the HID report can only represent 6 keys per report.  Report this
        // by replacing the last key element with the ErrorRollOver usage
        // code from the USB spec (0x01).
        if (nKeys > 6)
        {
            Log(LOG_ERROR, "Keyboard rollover error\n");
            keys[5] = 0x01;  // Keyboard ErrorRollOver per USB Usage Page 0x07
        }

        // populate the HID report
        *p++ = mod;        // modifier keys, 8 bits
        *p++ = 0;          // reserved, 8 bits
        *p++ = keys[0];    // keycode 1
        *p++ = keys[1];    // keycode 2
        *p++ = keys[2];    // keycode 3
        *p++ = keys[3];    // keycode 4
        *p++ = keys[4];    // keycode 5
        *p++ = keys[5];    // keycode 6

        // remember the keys sent, and snapshot the new live key state as the
        // starting point for the next report
        reportedKeys = nextKeys;
        nextKeys = liveKeys;
    }

    // return the length populated
    return static_cast<uint16_t>(p - buf);
}

void USBIfc::Keyboard::SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen)
{
    Log(LOG_INFO, "Kb LED %02x\n", buf[0]);
    
    // store the new LED status
    ledState = buf[0];
}


// ----------------------------------------------------------------------------
//
// Media Control
//

const uint8_t USBIfc::MediaControl::usages[] = {
    0xE2,   // Mute -> bit 0x01
    0xE9,   // Volume Up - > bit 0x02
    0xEA,   // Volume Down -> bit 0x04
    0xB5,   // Next Track -> bit 0x08
    0xB6,   // Previous Track -> bit 0x10
    0xB7,   // Stop -> bit 0x20
    0xCD,   // Play/Pause -> bit 0x40
    0xB8,   // Eject -> bit 0x80
};

const uint8_t *USBIfc::MediaControl::GetReportDescriptor(uint16_t *byteLength)
{
    static const uint8_t desc[] = {
        HID_USAGE_PAGE (HID_USAGE_PAGE_CONSUMER),
        HID_USAGE      (HID_USAGE_CONSUMER_CONTROL),
        HID_COLLECTION (HID_COLLECTION_APPLICATION),
            HID_REPORT_ID    (ReportIDMediaControl)
            HID_LOGICAL_MIN  (0x00),
            HID_LOGICAL_MAX  (0x01),
            HID_REPORT_COUNT (8),
            HID_REPORT_SIZE  (1),
            HID_USAGE        (usages[0]),   // Mute -> bit 0x01
            HID_USAGE        (usages[1]),   // Volume Up - > bit 0x02
            HID_USAGE        (usages[2]),   // Volume Down -> bit 0x04
            HID_USAGE        (usages[3]),   // Next Track -> bit 0x08
            HID_USAGE        (usages[4]),   // Previous Track -> bit 0x10
            HID_USAGE        (usages[5]),   // Stop -> bit 0x20
            HID_USAGE        (usages[6]),   // Play/Pause -> bit 0x40
            HID_USAGE        (usages[7]),   // Eject -> bit 0x80
            HID_INPUT        (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        HID_COLLECTION_END
    };
    *byteLength = sizeof(desc);
    return desc;
}

uint8_t USBIfc::MediaControl::GetUsageFor(Key key)
{
    int idx = static_cast<int>(key);
    return (idx > 0 && idx < static_cast<int>(_countof(usages))) ? usages[idx] : 0;
}

bool USBIfc::MediaControl::SendReport()
{
    // Only send a report if we have state changes since the last report,
    // so that we don't send unnecessary empty reports.
    if (nextKeys != reportedKeys)
        return USBIfc::HID::SendReport();

    // no report sent
    return false;
}

uint16_t USBIfc::MediaControl::GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // fill in the request buffer if there's room
    uint8_t *p = buf;
    if (type == HID_REPORT_TYPE_INPUT && reqLen >= 1)
    {
        // fill the report
        *p++ = nextKeys;        // new key state bits

        // Remember the keys sent, and snapshot the live key state as the starting
        // point for the next report
        reportedKeys = nextKeys;
        nextKeys = liveKeys;
    }

    // return the length populated
    return static_cast<uint16_t>(p - buf);
}

void USBIfc::MediaControl::KeyEvent(Key key, bool isDown)
{
    // Get the mask bit for the key.  The Key enum is arranged in
    // the same order as the HID report bits for the keys, so we
    // can get the mask by shifting a bit left by the enum's
    // int value.
    uint8_t mask = 1 << static_cast<int>(key);

    // set or clear the bit in the live keys
    isDown ? (liveKeys |= mask) : (liveKeys &= ~mask);

    // If the bit hasn't been changed yet in the pending report
    // register, update it now to match.  If the pending report
    // bit has already been changed, leave it alone.  This ensures
    // that a very fast pulse that happens entirely within a
    // single USB reporting cycle still generates one report
    // with the key state changed.  Otherwise, the host would
    // entirely miss quick taps on keys that happen between USB
    // polling cycles.
    if ((nextKeys & mask) == (reportedKeys & mask))
        isDown ? (nextKeys |= mask) : (nextKeys &= ~mask);

    // mark the event time
    stats.MarkEventTime(time_us_64());
}

