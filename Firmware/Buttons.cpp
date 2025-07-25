// Pinscape Pico firmware - buttons
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>
#include <algorithm>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "Config.h"
#include "JSON.h"
#include "Logger.h"
#include "USBIfc.h"
#include "Buttons.h"
#include "NightMode.h"
#include "XInput.h"
#include "Reset.h"
#include "GPIOManager.h"
#include "Nudge.h"
#include "Outputs.h"
#include "BootselButton.h"
#include "ADCManager.h"
#include "Plunger/Plunger.h"
#include "Plunger/ZBLaunch.h"
#include "IRRemote/IRCommand.h"
#include "IRRemote/IRTransmitter.h"
#include "IRRemote/IRReceiver.h"
#include "Devices/GPIOExt/PCA9555.h"
#include "Devices/ShiftReg/74HC165.h"

// global list of logical buttons
std::vector<std::unique_ptr<Button>> Button::buttons;

// buttons by name
std::unordered_map<std::string, Button*> Button::namedButtons;

// current shift button states
uint32_t Button::shiftButtonsPressed = 0;

// Nudge device view for buttons.  If any buttons have a nudge axis
// as their data source, we'll create this object, which can be shared
// among all buttons with nudge axis sources.
static NudgeDevice::View *buttonNudgeView = nullptr;


// look up a button by configuration index
Button *Button::Get(int n)
{
    return (n >= 0 && n < buttons.size()) ? buttons[n].get() : nullptr;
}

// look up a button by name
Button *Button::Get(const char *name)
{
    if (auto it = namedButtons.find(name) ; it != namedButtons.end())
        return it->second;
    else
        return nullptr;
}

// define a button as an input/action association
void Button::Add(Button *button)
{
    // add a new list item to represent the button
    buttons.emplace_back(button);
}

// USB keyboard key name table.  This is based on the HID Usage Table
// specification, Usage Page 0x07, Keyboard/Keypad.  The string name in
// the key column isn't anything official; it's just a name we made up
// for use within the JSON configuration.  The comments at the right
// show the original descriptive names used in the HID spec.  The hex
// number in the second column is the USB HID usage number (from Usage
// Page 0x07, Keyboard/Keypad) of the named key, which is what we send
// over the wire to the PC.  The HID usage codes are universal; these
// numbers have the same meanings on all host operating systems,
// although the HID usage spec leaves some of the key assignments
// intentionally ill-defined to accommodate regional keyboard layout
// variations.  (Or more precisely, to ease the transition from the
// jumble of different conventions that were already in effect before
// the HID spec came along.)
//
// All key names are in lower case; we'll convert JSON input to lower
// before matching, to make the JSON case-insensitive.  Some keys have
// aliases for common variations in the conventional name (e.g., "esc"
// and "escape"), to increase the chances that a user who's guessing at
// the name will get it right the first time and won't have to keep
// going back to the documentation to look up every key name.  Most of
// the keys that aren't common on US PC keyboards are left unnamed.  The
// JSON can still assign any key, whether we give it a name or not, by
// using its UBS usage number directly.
static const std::unordered_map<std::string, uint8_t> usbKeyNames{
//  { "", 0x00 },                // 00 Reserved
//  { "", 0x01 },                // 01 Keyboard ErrorRollOver
//  { "", 0x02 },                // 02 Keyboard POSTFail 
//  { "", 0x03 },                // 03 Keyboard ErrorUndefined
    { "a", 0x04 },               // 04 Keyboard a and A
    { "b", 0x05 },               // 05 Keyboard b and B
    { "c", 0x06 },               // 06 Keyboard c and C
    { "d", 0x07 },               // 07 Keyboard d and D
    { "e", 0x08 },               // 08 Keyboard e and E
    { "f", 0x09 },               // 09 Keyboard f and F
    { "g", 0x0A },               // 0A Keyboard g and G
    { "h", 0x0B },               // 0B Keyboard h and H
    { "i", 0x0C },               // 0C Keyboard i and I
    { "j", 0x0D },               // 0D Keyboard j and J
    { "k", 0x0E },               // 0E Keyboard k and K
    { "l", 0x0F },               // 0F Keyboard l and L
    { "m", 0x10 },               // 10 Keyboard m and M
    { "n", 0x11 },               // 11 Keyboard n and N
    { "o", 0x12 },               // 12 Keyboard o and O
    { "p", 0x13 },               // 13 Keyboard p and P
    { "q", 0x14 },               // 14 Keyboard q and Q
    { "r", 0x15 },               // 15 Keyboard r and R
    { "s", 0x16 },               // 16 Keyboard s and S
    { "t", 0x17 },               // 17 Keyboard t and T
    { "u", 0x18 },               // 18 Keyboard u and U
    { "v", 0x19 },               // 19 Keyboard v and V
    { "w", 0x1A },               // 1A Keyboard w and W
    { "x", 0x1B },               // 1B Keyboard x and X
    { "y", 0x1C },               // 1C Keyboard y and Y
    { "z", 0x1D },               // 1D Keyboard z and Z
    { "1", 0x1E },               // 1E Keyboard 1 and !
    { "!", 0x1E },               // 1E Keyboard 1 and !
    { "2", 0x1F },               // 1F Keyboard 2 and @
    { "@", 0x1F },               // 1F Keyboard 2 and @
    { "3", 0x20 },               // 20 Keyboard 3 and #
    { "#", 0x20 },               // 20 Keyboard 3 and #
    { "4", 0x21 },               // 21 Keyboard 4 and $
    { "$", 0x21 },               // 21 Keyboard 4 and $
    { "5", 0x22 },               // 22 Keyboard 5 and %
    { "%", 0x22 },               // 22 Keyboard 5 and %
    { "6", 0x23 },               // 23 Keyboard 6 and ^
    { "^", 0x23 },               // 23 Keyboard 6 and ^
    { "7", 0x24 },               // 24 Keyboard 7 and &
    { "&", 0x24 },               // 24 Keyboard 7 and &
    { "8", 0x25 },               // 25 Keyboard 8 and *
    { "*", 0x25 },               // 25 Keyboard 8 and *
    { "9", 0x26 },               // 26 Keyboard 9 and (
    { "(", 0x26 },               // 26 Keyboard 9 and (
    { "0", 0x27 },               // 27 Keyboard 0 and )
    { ")", 0x27 },               // 27 Keyboard 0 and )
    { "return", 0x28 },          // 28 Keyboard Return (ENTER)
    { "enter", 0x28 },           // 28 Keyboard Return (ENTER)
    { "esc", 0x29 },             // 29 Keyboard ESCAPE
    { "escape", 0x29 },          // 29 Keyboard ESCAPE
    { "backspace", 0x2A },       // 2A Keyboard DELETE (Backspace)
    { "bksp", 0x2A },            // 2A Keyboard DELETE (Backspace)
    { "tab", 0x2B },             // 2B Keyboard Tab
    { "space", 0x2C },           // 2C Keyboard Spacebar
    { "spacebar", 0x2C },        // 2C Keyboard Spacebar
    { "-", 0x2D },               // 2D Keyboard - and (underscore)
    { "_", 0x2D },               // 2D Keyboard - and (underscore)
    { "hyphen", 0x2D },          // 2D Keyboard - and (underscore)
    { "minus", 0x2D },           // 2D Keyboard - and (underscore)
    { "=", 0x2E },               // 2E Keyboard = and +
    { "+", 0x2E },               // 2E Keyboard = and +
    { "equals", 0x2E },          // 2E Keyboard = and +
    { "[", 0x2F },               // 2F Keyboard [ and {
    { "{", 0x2F },               // 2F Keyboard [ and {
    { "lbrack", 0x2F },          // 2F Keyboard [ and {
    { "lbrace", 0x2F },          // 2F Keyboard [ and {
    { "]", 0x30 },               // 30 Keyboard ] and }
    { "}", 0x30 },               // 30 Keyboard ] and }
    { "rbrack", 0x30 },          // 30 Keyboard ] and }
    { "rbrace", 0x30 },          // 30 Keyboard ] and }
    { "\\", 0x31 },              // 31 Keyboard \ and |
    { "||", 0x31 },              // 31 Keyboard \ and |
    { "backslash", 0x31 },       // 31 Keyboard \ and |
//  { "", 0x32 },                // 32 Keyboard Non-US # and 5
    { ";", 0x33 },               // 33 Keyboard ; and :
    { ":", 0x33 },               // 33 Keyboard ; and :
    { "semicolon", 0x33 },       // 33 Keyboard ; and :
    { "colon", 0x33 },       // 33 Keyboard ; and :
    { "'", 0x34 },               // 34 Keyboard ' and "
    { "\"", 0x34 },              // 34 Keyboard ' and "
    { "quote", 0x34 },           // 34 Keyboard ' and "
//  { "", 0x35 },                // 35 Keyboard Grave Accent and Tilde
    { ",", 0x36 },               // 36 Keyboard , and <
    { "<", 0x36 },               // 36 Keyboard , and <
    { "comma", 0x36 },           // 36 Keyboard , and <
    { ".", 0x37 },               // 37 Keyboard . and >
    { ">", 0x37 },               // 37 Keyboard . and >
    { "period", 0x37 },          // 37 Keyboard . and >
    { "dot", 0x37 },             // 37 Keyboard . and >
    { "/", 0x38 },               // 38 Keyboard / and ?
    { "?", 0x38 },               // 38 Keyboard / and ?
    { "slash", 0x38 },           // 38 Keyboard / and ?
    { "caps lock", 0x39 },       // 39 Keyboard Caps Lock
    { "capslock", 0x39 },        // 39 Keyboard Caps Lock
    { "f1", 0x3A },              // 3A Keyboard F1
    { "f2", 0x3B },              // 3B Keyboard F2
    { "f3", 0x3C },              // 3C Keyboard F3
    { "f4", 0x3D },              // 3D Keyboard F4
    { "f5", 0x3E },              // 3E Keyboard F5
    { "f6", 0x3F },              // 3F Keyboard F6
    { "f7", 0x40 },              // 40 Keyboard F7
    { "f8", 0x41 },              // 41 Keyboard F8
    { "f9", 0x42 },              // 42 Keyboard F9
    { "f10", 0x43 },             // 43 Keyboard F10
    { "f11", 0x44 },             // 44 Keyboard F11
    { "f12", 0x45 },             // 45 Keyboard F12
    { "print screen", 0x46 },    // 46 Keyboard PrintScreen
    { "printscreen", 0x46 },     // 46 Keyboard PrintScreen
    { "prntscrn", 0x46 },        // 46 Keyboard PrintScreen
    { "prtscr", 0x46 },          // 46 Keyboard PrintScreen
    { "scrolllock", 0x47 },      // 47 Keyboard Scroll Lock
    { "scroll lock", 0x47 },     // 47 Keyboard Scroll Lock
    { "pause", 0x48 },           // 48 Keyboard Pause
    { "ins", 0x49 },             // 49 Keyboard Insert
    { "insert", 0x49 },          // 49 Keyboard Insert
    { "home", 0x4A },            // 4A Keyboard Home
    { "page up", 0x4B },         // 4B Keyboard PageUp
    { "pageup", 0x4B },          // 4B Keyboard PageUp
    { "pgup", 0x4B },            // 4B Keyboard PageUp
    { "del", 0x4C },             // 4C Keyboard Delete Forward
    { "delete", 0x4C },          // 4C Keyboard Delete Forward
    { "end", 0x4D },             // 4D Keyboard End
    { "page down", 0x4E },       // 4E Keyboard PageDown
    { "pagedown", 0x4E },        // 4E Keyboard PageDown
    { "pagedn", 0x4E },          // 4E Keyboard PageDown
    { "pgdn", 0x4E },            // 4E Keyboard PageDown
    { "right", 0x4F },           // 4F Keyboard RightArrow
    { "right arrow", 0x4F },     // 4F Keyboard RightArrow
    { "left", 0x50 },            // 50 Keyboard LeftArrow
    { "left arrow", 0x50 },      // 50 Keyboard LeftArrow
    { "down", 0x51 },            // 51 Keyboard DownArrow
    { "down arrow", 0x51 },      // 51 Keyboard DownArrow
    { "up", 0x52 },              // 52 Keyboard UpArrow
    { "up arrow", 0x52 },        // 52 Keyboard UpArrow
    { "numlock", 0x53 },         // 53 Keypad Num Lock and Clear
    { "num lock", 0x53 },        // 53 Keypad Num Lock and Clear
    { "clear", 0x53 },           // 53 Keypad Num Lock and Clear
    { "keypad /", 0x54 },        // 54 Keypad /
    { "keypad *", 0x55 },        // 55 Keypad *
    { "keypad -", 0x56 },        // 56 Keypad -
    { "keypad +", 0x57 },        // 57 Keypad +
    { "keypad enter", 0x58 },    // 58 Keypad ENTER
    { "keypad 1", 0x59 },        // 59 Keypad 1 and End
    { "keypad 2", 0x5A },        // 5A Keypad 2 and Down Arrow
    { "keypad 3", 0x5B },        // 5B Keypad 3 and PageDn
    { "keypad 4", 0x5C },        // 5C Keypad 4 and Left Arrow
    { "keypad 5", 0x5D },        // 5D Keypad 5
    { "keypad 6", 0x5E },        // 5E Keypad 6 and Right Arrow
    { "keypad 7", 0x5F },        // 5F Keypad 7 and Home
    { "keypad 8", 0x60 },        // 60 Keypad 8 and Up Arrow
    { "keypad 9", 0x61 },        // 61 Keypad 9 and PageUp
    { "keypad 0", 0x62 },        // 62 Keypad 0 and Insert
    { "keypad .", 0x63 },        // 63 Keypad . and Delete
//  { "", 0x64 },                // 64 Keyboard Non-US \ and |
    { "application", 0x65 },     // 65 Keyboard Application
    { "power", 0x66 },           // 66 Keyboard Power
    { "keypad =", 0x67 },        // 67 Keypad =
    { "f13", 0x68 },             // 68 Keyboard F13
    { "f14", 0x69 },             // 69 Keyboard F14
    { "f15", 0x6A },             // 6A Keyboard F15
    { "f16", 0x6B },             // 6B Keyboard F16
    { "f17", 0x6C },             // 6C Keyboard F17
    { "f18", 0x6D },             // 6D Keyboard F18
    { "f19", 0x6E },             // 6E Keyboard F19
    { "f20", 0x6F },             // 6F Keyboard F20
    { "f21", 0x70 },             // 70 Keyboard F21
    { "f22", 0x71 },             // 71 Keyboard F22
    { "f23", 0x72 },             // 72 Keyboard F23
    { "f24", 0x73 },             // 73 Keyboard F24
    { "execute", 0x74 },         // 74 Keyboard Execute
    { "help", 0x75 },            // 75 Keyboard Help
    { "menu", 0x76 },            // 76 Keyboard Menu
    { "select", 0x77 },          // 77 Keyboard Select
    { "stop", 0x78 },            // 78 Keyboard Stop
    { "again", 0x79 },           // 79 Keyboard Again
    { "undo", 0x7A },            // 7A Keyboard Undo
    { "cut", 0x7B },             // 7B Keyboard Cut
    { "copy", 0x7C },            // 7C Keyboard Copy
    { "paste", 0x7D },           // 7D Keyboard Paste
    { "find", 0x7E },            // 7E Keyboard Find
    { "mute", 0x7F },            // 7F Keyboard Mute
    { "volup", 0x80 },           // 80 Keyboard Volume Up
    { "voldown", 0x81 },         // 81 Keyboard Volume Down
    { "voldn", 0x81 },           // 81 Keyboard Volume Down
//  { "", 0x82 },                // 82 Keyboard Locking Caps Lock
//  { "", 0x83 },                // 83 Keyboard Locking Num Lock
//  { "", 0x84 },                // 84 Keyboard Locking Scroll Lock
    { "keypad ,", 0x85 },        // 85 Keypad Comma
    { "keypad comma", 0x85 },    // 85 Keypad Comma
//  { "", 0x86 },                // 86 Keypad Equal Sign (AS/400)
//  { "", 0x87 },                // 87 Keyboard International1
//  { "", 0x88 },                // 88 Keyboard International2
//  { "", 0x89 },                // 89 Keyboard International3
//  { "", 0x8A },                // 8A Keyboard International4
//  { "", 0x8B },                // 8B Keyboard International5
//  { "", 0x8C },                // 8C Keyboard International6
//  { "", 0x8D },                // 8D Keyboard International7
//  { "", 0x8E },                // 8E Keyboard International8
//  { "", 0x8F },                // 8F Keyboard International9
//  { "", 0x90 },                // 90 Keyboard LANG1
//  { "", 0x91 },                // 91 Keyboard LANG2
//  { "", 0x92 },                // 92 Keyboard LANG3
//  { "", 0x93 },                // 93 Keyboard LANG4
//  { "", 0x94 },                // 94 Keyboard LANG5
//  { "", 0x95 },                // 95 Keyboard LANG6
//  { "", 0x96 },                // 96 Keyboard LANG7
//  { "", 0x97 },                // 97 Keyboard LANG8
//  { "", 0x98 },                // 98 Keyboard LANG9
//  { "", 0x99 },                // 99 Keyboard Alternate Erase
//  { "", 0x9A },                // 9A Keyboard SysReq/Attention
//  { "", 0x9B },                // 9B Keyboard Cancel
//  { "", 0x9C },                // 9C Keyboard Clear
//  { "", 0x9D },                // 9D Keyboard Prior
//  { "", 0x9E },                // 9E Keyboard Return
//  { "", 0x9F },                // 9F Keyboard Separator
//  { "", 0xA0 },                // A0 Keyboard Out
//  { "", 0xA1 },                // A1 Keyboard Oper
//  { "", 0xA2 },                // A2 Keyboard Clear/Again
//  { "", 0xA3 },                // A3 Keyboard CrSel/Props
//  { "", 0xA4 },                // A4 Keyboard ExSel
//  { "", 0xA5 },                // A5 Reserved
//  { "", 0xA6 },                // A6 Reserved
//  { "", 0xA7 },                // A7 Reserved
//  { "", 0xA8 },                // A8 Reserved
//  { "", 0xA9 },                // A9 Reserved
//  { "", 0xAA },                // AA Reserved
//  { "", 0xAB },                // AB Reserved
//  { "", 0xAC },                // AC Reserved
//  { "", 0xAD },                // AD Reserved
//  { "", 0xAE },                // AE Reserved
//  { "", 0xAF },                // AF Reserved
    { "keypad 00", 0xB0 },       // B0 Keypad 00
    { "keypad 000", 0xB1 },      // B1 Keypad 000
//  { "", 0xB2 },                // B2 Thousands Separator
//  { "", 0xB3 },                // B3 Decimal Separator
//  { "", 0xB4 },                // B4 Currency Unit
//  { "", 0xB5 },                // B5 Currency Sub-unit
//  { "", 0xB6 },                // B6 Keypad ( Sel
//  { "", 0xB7 },                // B7 Keypad )
//  { "", 0xB8 },                // B8 Keypad {
//  { "", 0xB9 },                // B9 Keypad }
//  { "", 0xBA },                // BA Keypad Tab
//  { "", 0xBB },                // BB Keypad Backspace
//  { "", 0xBC },                // BC Keypad A
//  { "", 0xBD },                // BD Keypad B
//  { "", 0xBE },                // BE Keypad C
//  { "", 0xBF },                // BF Keypad D
//  { "", 0xC0 },                // C0 Keypad E
//  { "", 0xC1 },                // C1 Keypad F
//  { "", 0xC2 },                // C2 Keypad XOR
//  { "", 0xC3 },                // C3 Keypad ^
//  { "", 0xC4 },                // C4 Keypad %
//  { "", 0xC5 },                // C5 Keypad <
//  { "", 0xC6 },                // C6 Keypad >
//  { "", 0xC7 },                // C7 Keypad &
//  { "", 0xC8 },                // C8 Keypad &&
//  { "", 0xC9 },                // C9 Keypad |
//  { "", 0xCA },                // CA Keypad ||
//  { "", 0xCB },                // CB Keypad :
//  { "", 0xCC },                // CC Keypad #
//  { "", 0xCD },                // CD Keypad Space
//  { "", 0xCE },                // CE Keypad @
//  { "", 0xCF },                // CF Keypad !
//  { "", 0xD0 },                // D0 Keypad Memory Store
//  { "", 0xD1 },                // D1 Keypad Memory Recall
//  { "", 0xD2 },                // D2 Keypad Memory Clear
//  { "", 0xD3 },                // D3 Keypad Memory Add
//  { "", 0xD4 },                // D4 Keypad Memory Subtract
//  { "", 0xD5 },                // D5 Keypad Memory Multiply
//  { "", 0xD6 },                // D6 Keypad Memory Divide
//  { "", 0xD7 },                // D7 Keypad +/-
//  { "", 0xD8 },                // D8 Keypad Clear
//  { "", 0xD9 },                // D9 Keypad Clear Entry
//  { "", 0xDA },                // DA Keypad Binary
//  { "", 0xDB },                // DB Keypad Octal
//  { "", 0xDC },                // DC Keypad Decimal
//  { "", 0xDD },                // DD Keypad Hexadecimal
//  { "", 0xDE },                // DE Reserved
//  { "", 0xDF },                // DF Reserved
    { "ctrl", 0xE0 },            // E0 Keyboard LeftControl
    { "left ctrl", 0xE0 },       // E0 Keyboard LeftControl
    { "left control", 0xE0 },    // E0 Keyboard LeftControl
    { "shift", 0xE1 },           // E1 Keyboard LeftShift
    { "left shift", 0xE1 },      // E1 Keyboard LeftShift
    { "alt", 0xE2 },             // E2 Keyboard LeftAlt
    { "left alt", 0xE2 },        // E2 Keyboard LeftAlt
    { "gui", 0xE3 },             // E3 Keyboard Left GUI (Windows key)
    { "left gui", 0xE3 },        // E3 Keyboard Left GUI (Windows key)
    { "left win", 0xE3 },        // E3 Keyboard Left GUI (Windows key)
    { "left windows", 0xE3 },    // E3 Keyboard Left GUI (Windows key)
    { "right ctrl", 0xE4 },      // E4 Keyboard RightControl
    { "right control", 0xE4 },   // E4 Keyboard RightControl
    { "right shift", 0xE5 },     // E5 Keyboard RightShift
    { "right alt", 0xE6 },       // E6 Keyboard RightAlt
    { "right gui", 0xE7 },       // E7 Keyboard Right GUI (Windows key)
    { "right win", 0xE7 },       // E7 Keyboard Right GUI (Windows key)
    { "right windows", 0xE7 },   // E7 Keyboard Right GUI (Windows key)
};


// Configure buttons from the JSON config data
//
// buttons: array of <Button>
//
// <Button>: object {
//    name: <string>          // optional name for the button, for references elsewhere in the config (e.g., output port source)
//    type: "push|hold|pulse|toggle|shift",   // button type - see below
//    source: <Source>,       // physical or virtual input source (descriptor object; see below)
//    action: <Action>,       // action to perform when button is pressed (descriptor object; see below)
//    shiftMask: <number>,    // shift mask; determines which shift bits control the button; 0 means that the button ignores all shift keys
//    shiftBits: <number>,    // shift bits; these shift state bits must be matched for the button to trigger
//    remoteWake: <bool>,     // if true, this button sends a USB wakeup packet when pressed (off->on transition); default is false
//    // extra parameters, depending upon the type
//
//
// Button types and details:
//
// type: "push",          // basic pushbutton; ON when the underlying source is ON; this is the default if no type is specified
//   // no additional parameters
//
// type: "hold",          // push-and-hold button: ON after the underlying source has been ON for a minimum hold time
//   holdTime: <number>,  // hold time in milliseconds; the button won't turn on until held continuously for this long
//
// type: "pulse",         // pulse button; pulses ON for a timed period when the underlying source switches OFF->ON or OFF->ON
//   tOn: <number>,       // ON pulse time in milliseconds; occurs when underlying source switches OFF to ON
//   tOff: <number>,      // OFF pulse time in milliseconds; occurs when underlying source switched ON to OFF; 0 means no OFF pulse
//   tSpace: <number>,    // minimum time in milliseconds between pulses
//
// type: "toggle",        // toggle button; reverses ON/OFF state each time the underlying source switches ON
//   // no additional parameters
//
// type: "shift",         // shift button; OR this button's shiftBits into the global shift state when pressed
//   tPulse: <number>,    // if defined, this is a SHIFT-OR button, and this defines the pulse time in milliseconds when the button is released
//
// A shift button is a SHIFT-OR button if a pulse time (tPulse) is defined, otherwise it's a
// SHIFT-AND button.
//
// A SHIFT-OR button doesn't perform its action while being held down.  Instead, it waits until
// the user releases the button.  If no OTHER buttons were pressed while the shift was being
// held down, the shift button's action is pulsed for the tPulse time on release.  If any other
// button was pressed while the shift was being held, the shift's action is skipped on release.
// That's the OR: it's either a shift button OR an ordinary button on each press, depending on
// what happens during the press.  The drawback is that the action is delayed until the button
// is released, which can feel laggy to the user, since most buttons engage immediately when
// pressed.  What you gain in return is that the button can perform two distinct functions
// independently of one another, reducing the number of physical buttons you have to install.
//
// A SHIFT-AND button performs its action immediately when pressed, just like any non-shift
// button.  As long as it's being held down, it's shift state ALSO applies.  That's the AND:
// it acts like an ordinary button AND a shift button at the same time, without any
// conditionality on whether or not other buttons are pressed while it's engaged.  This has
// the advantage over SHIFT-OR buttons that the button's main function is carried out as soon
// as the button is pressed, not delayed until release like a SHIFT-OR button.  The downside
// is that the main function is always carried out, whether or not the user only wanted the
// shift feature for a particular press.  This is useful in cases where the main function
// doesn't have any lasting effect, in which case it won't do any harm to engage the main
// function incidentally when you only need to use the shift feature.
//
//
// <Source> object details:
// source: {
//    type: <string>      // see below for the type names
//    // other parameters depending on type
// }
//
// type: "gpio",          // direct GPIO port input
//   gp: <number>,        // GPIO port number, 0..26
//   pull: <bool>,        // true -> enable internal Pico pull-up resistor (if active low) or pull-down (if active high)
//   active: <string>,    // "high" (button reads true when held at 3.3V) or "low" (button reads true when held at GND)
//   enableLogging: <bool>,  // enable GPIO event logging (records GPIO edge times in interrupt handler, for latency analysis on host)
//
// type: "bootsel",       // BOOTSEL button (the little pushbutton on top of the Pico)
//   poll: <number>,      // polling interval in milliseconds; minimum 1, default 100.  Polling the button takes about
//                        // 100us, and locks out interrupts for that whole time, so polling should be done infrequently
//                        // to avoid excessive CPU load and interrupt latency.  Recommended minimum interval is 10ms.
//
// type: "pca9555",       // PCA9555 GPIO extender port
//   chip: <number>,      // chip index in pca9555[] configuration array, starting at 0
//   port: <string>,      // input port name, using NXP or TI notation: "IO1_3", or just "13"
//   active: <string>,    // "high" or "low"
//
//   The PCA9555 has a built-in 100K pull-resistor on each port that
//   can't be disabled in software, so it's easiest to use this chip
//   in active-low mode (button press connects pin to GND).  If you
//   must use a port in active-high mode, you'll have to add an
//   external pull-down resistor, around 10K ohms, connected between
//   the port and GND.
//
// type: "74hc165",       // 74HC165 parallel-in shift register port
//   chain: <number>,     // chain index in 74hc165[] configuration array, starting at 0; this can be omitted if there's only one chain, as is typical
//   chip: <number>,      // chip index in the chain, 0 = first chip (the one attached directly to the Pico)
//   port: <number>,      // port number on the chip, 0 to 7 for A to H, or the port name string "A" to "H";
//                        // the port can also encode the chip number, as in "1B" for port "B" on chip index (the second chp in the chain)
//   active: <string>,    // "high" or "low"
//
// type: "nudge",         // nudge input; simulates button press when an axis exceeds the specified threshold
//   axis: <string>,      // axis name, "x", "y", "z"
//   threshold: <number>, // threshold level where button press is triggered, 0..32767
//   direction: <string>, // "+", "-", or "+-" - the axis direction or directions that trigger the button press
//   onTime: <number>,    // firing time in milliseconds; the button stays on this long after being triggered
//   resetTime: <number>, // reset time in milliseconds; after triggering, the button won't fire again until this time elapses
//
//
// type: "plunger",       // plunger input; converts plunger position or motion into a button event
//   fire: <bool>,        // if true, read as ON when a plunger firing event is in progress; ignores firing events otherwise
//   range: {             // if specified, read as ON when the plunger position is inside or outside the given range
//     min: <number>,     // minimum end of range
//     max: <number>,     // maximum end of range
//     inside: <bool>,    // true -> read as ON when plunger position in between min and max, inclusive;
//                        // false -> read as ON when plunger position is less than min or greater than max
//   },
//
//
// type: "zblaunch",      // ZB Launch input; reads as ON when ZB Launch detects a plunger firing event or the plunger is pushed forward
//   modal: <bool>,       // true -> only read as ON when ZB Launch Mode is engaged
//                        // false -> read as ON during plunger firing events or plunger pushes regardless of ZB Launch Mode
//
//
// type: "IR",            // IR remote control command received
//   code: <string>,      // IR command code to match, in our universal IR format; see IRRemote/IRCommand.h
//   latchTIme: <number>, // Latching time in milliseconds, default 0; after a code is received, the button reads
//                        // as ON for this long, so that it stays continuously ON if the IR auto-repeats within
//                        // the time limit.  Note that this shouldn't be used when mapping IR input to keyboard
//                        // key input, since the latch time will appear as a lag time releasing the key, causing
//                        // unwanted auto-repeats on the Windows side after releasing the physical remote button.
//   firstRepeatDelay: <number>, // First auto-repeat delay time in milliseconds, default 0; after the code is
//                        // received, auto-repeats will be ignored for this long.  This is useful for keyboard
//                        // mappings, since it requires holding down the remote button for this long before
//                        // the PC will see repeated key inputs, simulating a normal PC keyboard.  Most IR
//                        // remotes start sending repeats immediately when a key is held down, which doesn't
//                        // feel right for most key input.  Tip: use a "pulse" button type for the enclosing
//                        // logical button when mapping IR input to PC keyboard keys, so that the repeat
//                        // rate is controlled by the remote rather than the Windows auto-repeat function.
//
// type: "output",        // Output port state
//   port: <string|number>,   // logical output port name or number
//   range: {             // if specified, the value range on the port that triggers the button; default is {min: 1}
//      min: <number>,    // minumum end of range; default 1
//      max: <number>,    // maximum end of range; default 255
//      inside: <bool>,   // true (default) -> read as ON when the output port is between min and max, inclusive;
//                        // false -> read as ON when the output port is less than min or greater than max
//   },
//
//
// type: "clock",         // button is pressed at certain times of day
//   // TBD
//
// type: "adc",           // ADC based source
//   adc: "<adc>",        // the adc entry, for example "pico_adc[0]", required
//   threshold: <number>, // the adc threshold level to compare against the adc reading, default 0
//   above: <bool>,       // true (default) -> read as ON when the adc reading is above the threshold
//                        // false -> read as ON when the adc reading is below the threshold
//
// <Action> object details:
// action: {
//    type: <string>      // see below for the types
//    // other parameter depending on type
// }
//
// type: "none",          // no action; this creates an internal virtual button only; you can also just
//   // no parameters     // omit the action: { } object entirely to get the same effect
//
// type: "key",           // keyboard key
//   key: <string> or <number>,  // type "key" -> key name, OR a numeric USB HID usage code (see USB HID spec for a list)
//
// type: "media",         // media key
//   key: <string>,       // media key name: "mute", "volUp", "volDn", "next", "prev", "stop", "play", "eject"
//                        // Note: the "eject" key is a valid USB key, but the Windows keyboard driver ignores it, so
//                        // it won't generate any events.  It's included for completeness of the media key set but
//                        // shouldn't be used with a Windows host.
//
// type: "gamepad",       // gamepad button
//   button: <number>,    // button number, 1-32, or "hat up", "hat down", "hat left", "hat right"
//
// type: "xInput",        // XInput (XBox controller emulation)
//   button: <string>,    // button name: "up", "down", "left", "right", "start", "back", "l3", "r3", "lb", "rb", "xbox", "a", "b", "x", "y"
//                        // Up/down/left/right are the DPad buttons
//                        // L3 and R3 are the buttons activated by depressing the left and right thumbsticks, respectively
//                        // LB and RB are the left and right "bumper" buttons on the back of the controller
//
// type: "openPinDev",    // Open Pinball Device input
//   button: <number|string>  // button number, 1-32, for generic buttons; or a button name, for a pre-assigned pinball button
//
// type: "reset",         // reset the Pico when button is pressed and held for the specified hold time
//   mode: <string>,      // reset mode: "normal" (reset the Pinscape software), "bootloader" (switch to Pico native boot loader mode)
//   holdTime: <number>,  // required button hold time in milliseconds; shorter presses are ignored
// }
//
// type: "nightmode",     // engage Night Mode when the button is on
//   // no parameters
//
// type: "plungercal",    // engage plunger calibration mode; button must be held ON for about 2 seconds to engage
//   // no parameters
//
// type: "IR",            // send an IR command when the button is pressed
//   code: "string",      // IR command to send, in our universal IR format; see IRRemote/IRCommand.h
//   autoRepeat: <bool>,  // is auto-repeat enabled? if true, the transmission will auto-repeat as long as the logical button stays on,
//                        // otherwise it'll be sent just once each time the logical control switches from off to on
//
// type: "macro",         // macro - execute a series of actions when the button is pressed
//   repeat: true,        // if true, macro repeats if button is still on when macro finishes; otherwise macro runs once per push; default true
//   runToCompletion: true,  // if true, run the macro to completion even if the button is released early; default is true;
//                           // if false, releasing the button halts the macro
//   steps: [             // list of steps to perform
//     { step1 },         // frist step
//     { step2 },         // second setp
//     ...
//   ],
//
// Macro steps:
//   {
//     start: <number>,        // start time in milliseconds, relative to the previous step's start time
//     action: { <action> },   // a normal action description (but not a macro)
//     duration: <number>,     // duration of this action ON time in milliseconds"
//   }
// A macro consists of a series of actions to trigger.  Each action has a
// start time and a duration.  The duration is the length of time that the
// action is in the ON state once started.  The start time is relative to
// the previous step's start time.  Note that a step's start time isn't
// affected by the *duration* of the previous step, only by the previous
// step's start time.  This allows more than one step to be in the ON state
// at once, which is important for things like keyboard chords (where you
// press several keys at once).  For example, this sends a Ctrl+Shift+A
// key combination to the PC;
//
// steps: [
//    { start: 0, duration: 20, action: { type: "key", key: "left ctrl" } },
//    { start: 0, duration: 20, action: { type: "key", key: "left shift" } },
//    { start: 10, duration: 10, action: { type: "key", key: "A" } }
// ]
//
// The first two steps start immediately; they press the left Ctrl and left
// Shift keys for 20 ms.  Meanwhile, the third step starts 10 ms later and
// presses the "A" key.  The first two steps are still ON when the "A" key
// is pressed, so the Ctrl and Shift keys are still down, and the PC sees
// the Ctrl+Shift+A combination.  After 20 ms, all of the actions end and
// all keys are released.
//
void Button::Configure(JSONParser &json)
{
    // Read the buttons array from the JSON
    using MediaKey = USBIfc::MediaControl::Key;
    using Value = JSONParser::Value;
    json.Get("buttons")->ForEach([&json](int index, const Value *val)
    {
        // log extended debug info
        Log(LOG_DEBUGEX, "Button configuration: parsing button #%d\n", index);
        
        // format the JSON locus (for log messages) and pin-out labels
        char jsonLocus[32];
        snprintf(jsonLocus, sizeof(jsonLocus), "Button[%d]", index);
        const char *pinoutLabel = Format("Button #%d", index);

        // get the source input
        std::unique_ptr<Button::Source> source(ParseSource(jsonLocus, pinoutLabel, json, val->Get("source"), "low", false));

        // if parsing failed or there's no source definition, create a null source as a placeholder
        if (source == nullptr)
            source.reset(new NullSource());

        // parse the action
        std::unique_ptr<Button::Action> action(ParseAction(jsonLocus, val->Get("action")));

        // if there's no action, create a null action as a placeholder
        if (action == nullptr)
            action.reset(new NullAction());

        // get the common attributes
        bool remoteWake = val->Get("remoteWake")->Bool();
        uint32_t shiftMask = val->Get("shiftMask")->UInt32();
        uint32_t shiftBits = val->Get("shiftBits")->UInt32();
        
        // add the button according to its type
        auto &type = *val->Get("type");
        Button *button;
        char srcNameBuf[64];
        if (type == "push" || type.IsUndefined())
        {
            // pushbutton
            Button::Add(button = new Pushbutton(source.release(), shiftMask, shiftBits, action.release()));
            Log(LOG_CONFIG, "Button %d: Pushbutton created, action %s, source %s, shift(mask %02x, bits %02x)\n",
                index, buttons.back()->action->Name(), buttons.back()->source->FullName(srcNameBuf, sizeof(srcNameBuf)),
                shiftMask, shiftBits);
        }
        else if (type == "hold")
        {
            // hold button
            int holdTime = val->Get("holdTime")->Int(2000);
            if (holdTime == 0)
                Log(LOG_ERROR, "Button %d: Hold Button has zero hold time (tHold), so it's just a regular pushbutton\n", index);

            // create the button
            HoldButton *holdButton = new HoldButton(source.release(), shiftMask, shiftBits, action.release(), holdTime);
            Button::Add(button = holdButton);

            // check for a shortPress section
            if (const auto *shortPress = val->Get("shortPress") ; !shortPress->IsUndefined())
            {
                // parse the action
                char subLocus[32];
                snprintf(subLocus, sizeof(subLocus), "Button[%d].shortPress");
                holdButton->shortPressAction = ParseAction(subLocus, shortPress->Get("action"));

                // Set the pulse time, converting from JSON milliseconds to stored microseconds
                holdButton->shortPressActionDuration = val->Get("actionTime")->UInt32(20) * 1000UL;
            }

            // log status
            Log(LOG_CONFIG, "Button %d: HoldButton(%d ms hold time), action %s, source %s, shift(mask %02x, bits %02x) created\n",
                index, holdTime, buttons.back()->action->Name(),
                buttons.back()->source->FullName(srcNameBuf, sizeof(srcNameBuf)),
                shiftMask, shiftBits);
        }
        else if (type == "pulse")
        {
            // pulse button
            uint16_t onTime = val->Get("tOn")->UInt16(100);
            uint16_t offTime = val->Get("tOff")->UInt16();
            uint16_t gapTime = val->Get("tSpace")->UInt16();

            // warn if the on and off pulses are both zeroes, since the button won't do anything
            if (onTime == 0 && offTime == 0)
                Log(LOG_ERROR, "Button %d: pulse button has zero ON and OFF pulse times (tOn, tOff), so it can't perform any action\n", index);

            // add the button
            Button::Add(button = new PulseButton(source.release(), shiftMask, shiftBits, action.release(), nullptr, onTime, offTime, gapTime));
            Log(LOG_CONFIG, "Button %d: PulseButton(%d ms on, %d ms off, %d ms gap), action %s, source %s, shift(mask %02x, bits %02x) created\n",
                index, onTime, offTime, gapTime, buttons.back()->action->Name(),
                buttons.back()->source->FullName(srcNameBuf, sizeof(srcNameBuf)),
                shiftMask, shiftBits);
        }
        else if (type == "toggle")
        {
            // toggle button
            Button::Add(button = new ToggleButton(source.release(), shiftMask, shiftBits, action.release()));
            Log(LOG_CONFIG, "Button %d: Toggle button, action %s, source %s, shift(mask %02x, bits %02x) created\n",
                index, buttons.back()->action->Name(), buttons.back()->source->FullName(srcNameBuf, sizeof(srcNameBuf)),
                shiftMask, shiftBits);
        }
        else if (type == "shift")
        {
            uint16_t pulseTime = val->Get("tPulse")->UInt16();
            Button::Add(button = new ShiftButton(source.release(), shiftMask, shiftBits, action.release(), pulseTime));
            Log(LOG_CONFIG, "Button %d: ShiftButton(pulse=%d), action %s, source %s, shift(mask %02x, bits %02x) created\n",
                index, pulseTime, buttons.back()->action->Name(), buttons.back()->source->FullName(srcNameBuf, sizeof(srcNameBuf)),
                shiftMask, shiftBits);
        }
        else
        {
            // invalid - log an error, and create a regular pushbutton as a placeholder
            Log(LOG_ERROR, "Button %d: Invalid button type '%s'\n", index, type.String().c_str());
            Button::Add(button = new Pushbutton(source.release(), shiftMask, shiftBits, action.release()));
        }
        
        // check for a name property; if present, add the button to the
        // by-name index
        const JSONParser::Value *nameVal;
        if (const auto *nameVal = val->Get("name"); !nameVal->IsUndefined())
            namedButtons.emplace(nameVal->String(), button);
        
        // set the remote wakeup flag
        button->remoteWake = remoteWake;
    });
}

// Parse a source
Button::Source *Button::ParseSource(
    const char *jsonLocus, const char *pinoutLabel,
    JSONParser &json, const JSONParser::Value *srcVal,
    const char *activeDefault, bool allowBareGPIO)
{
    // check for a bare GPIO port source, if allowed
    if (allowBareGPIO && srcVal->IsNumber())
    {
        // create a GPIO source with default parameters
        return CreateGPIOSource(
            jsonLocus, pinoutLabel, srcVal->Int(-1),
            strcmp(activeDefault, "high") == 0, true, false, 0, 0, 0, 0);
    }
    
    // get the common attributes
    bool activeHigh = (srcVal->Get("active")->String(activeDefault) == "high");

    // parse according to the type
    auto &srcType = *srcVal->Get("type");
    if (srcType == "gpio")
    {
        // get the special parameters for type="gpio"
        int gp = srcVal->Get("gp")->Int();
        bool usePull = srcVal->Get("pull")->Bool(true);
        bool enableLogging = srcVal->Get("enableLogging")->Bool();
        int lpFilterRise_us = srcVal->Get("lowPassFilterRiseTime")->Int(0);
        int lpFilterFall_us = srcVal->Get("lowPassFilterFallTime")->Int(0);
        int debounceTimeOn_us = srcVal->Get("debounceTimeOn")->Int(1500);
        int debounceTimeOff_us = srcVal->Get("debounceTimeOff")->Int(1000);
        return CreateGPIOSource(
            jsonLocus, pinoutLabel, gp, activeHigh, usePull, enableLogging,
            lpFilterRise_us, lpFilterFall_us, debounceTimeOn_us, debounceTimeOff_us);
    }
    else if (srcType == "bootsel")
    {
        // get the polling interval; use 100ms by default, limit to 1ms minimum
        int interval = srcVal->Get("poll")->Int(100);
        interval = std::max(interval, 1);

        // set tup the BOOTSEL source
        return new Button::BOOTSELSource(interval);
    }
    else if (srcType == "pca9555")
    {
        // get the chip number
        int chipNum = srcVal->Get("chip")->Int(-1);
        int debounceTime_us = srcVal->Get("debounceTime")->Int(1000);
        PCA9555 *chip = PCA9555::Get(chipNum);
        if (chip == nullptr)
            Log(LOG_ERROR, "%s: invalid PCA9555 chip number %d\n", jsonLocus, chipNum);

        auto const *portVal = srcVal->Get("port");
        int port = -1;
        if (portVal->IsString())
        {
            // Parse pin name formats.  The NXP data sheet uses the notation "IO1_7",
            // while the TI data sheet reduces this to "17".  We'll accept any
            // combination of "IO" prefix, digits, and "_" separator:
            //
            //   IO1_7      - NXP data sheet notation
            //   IO17
            //   1_7        
            //   17         - TI data sheet notation
            //
            std::string s = portVal->String();
            const char *p = s.c_str();
            int bank = -1;
            if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'o' || p[1] == 'O')) p += 2;
            if (*p == '0' || *p == '1') bank = (*p++ - '0') * 8;
            if (*p == '_') ++p;
            if (*p >= '0' && *p <= '7') port = bank + (*p++) - '0';

            if (*p != 0 || bank == -1 || port == -1)
            {
                Log(LOG_ERROR, "%s: invalid PCA9555 port name syntax \"%s\", "
                    "expected IO0_n or IO1_n, n=0-7\n", jsonLocus, s.c_str());
            }
        }
        else
        {
            Log(LOG_ERROR, "%s: invalid PCA9555 'port' property; must be a port name string\n", jsonLocus);
        }

        // create the source if we have a valid chip and port
        if (chip != nullptr && port >= 0 && port <= 15)
            return new Button::PCA9555Source(chip, port, activeHigh, debounceTime_us);
    }
    else if (srcType == "74hc165")
    {
        // get the chain number 
        int chainNum = srcVal->Get("chain")->Int(0);
        int lpFilterRise_us = srcVal->Get("lowPassFilterRiseTime")->Int(0);
        int lpFilterFall_us = srcVal->Get("lowPassFilterFallTime")->Int(0);
        int debounceTimeOn_us = srcVal->Get("debounceTimeOn")->Int(1500);
        int debounceTimeOff_us = srcVal->Get("debounceTimeOff")->Int(1000);
        auto *chain = C74HC165::GetChain(chainNum);
        if (chain == nullptr)
            Log(LOG_ERROR, "%s: invalid 74HC165 chain index %d\n", jsonLocus, chainNum);

        // get the optional chip number
        int chip = srcVal->Get("chip")->Int(-1);
        if (chip >= 0 && chain != nullptr && !chain->IsValidPort(chip*8 + 7))
            Log(LOG_ERROR, "%s: invalid 74HC165 chip number %d\n", jsonLocus, chip);

        // get the port
        int port = -1;
        auto const *portVal = srcVal->Get("port");
        if (portVal->IsNumber())
        {
            port = portVal->Int(-1);
            if (chip != -1)
            {
                // chip is specified - must be chip-relative, 0-7
                if (port < 0 || port > 7)
                    Log(LOG_ERROR, "%s: invalid 74HC165 port number %d; must be 0-7 when 'chip' is specified\n", jsonLocus, port);
                port += chip*8;
            }

            if (chain != nullptr && !chain->IsValidPort(port))
                Log(LOG_ERROR, "%s: no such 74HC165 port is configured\n", jsonLocus);
        }
        else if (portVal->IsString())
        {
            auto s = portVal->String();
            const char *p = s.c_str();
            bool needPrefix = (chip == -1);
            const char *prefixText = needPrefix ? "<chipNum>" : "";
            if (needPrefix)
            {
                // no chip, so we need a chip number prefix
                chip = 0;
                for ( ; isdigit(*p) ; chip = chip*10 + *p - '0', ++p) ;

                if (p == s.c_str())
                    Log(LOG_ERROR, "%s: 74HC165 port name must include a chip number prefix, such as \"3A\"\n", jsonLocus);
                else if (chain != nullptr && !chain->IsValidPort(chip*8 + 7))
                    Log(LOG_ERROR, "%s: invalid 74HC165 chip number prefix %d in port name\n", jsonLocus, chip);
            }

            // Parse the port name: A-H (TI naming), or D0-D7 (NXP naming)
            if ((*p == 'd' || *p == 'D') && isdigit(p[1]))
                ++p, port = chip*8 + *p++ - '0';
            else if (*p >= 'A' && *p <= 'H')
                port = chip*8 + *p++ - 'A';
            else if (*p >= 'a' && *p <= 'h')
                port = chip*8 + *p++ - 'a';
            else
            {
                Log(LOG_ERROR, "%s: invalid 74HC165 port name \"%s\"; expected %sA-H or %sD0-D7\n",
                    jsonLocus, s.c_str(), prefixText, prefixText);
            }

            // make sure there's no more text after the port name
            if (*p != 0 && port >= 0)
            {
                Log(LOG_WARNING, "%s: extraneous text after 74HC165 port name \"%s\", expected just \"%.*s\"\n",
                    jsonLocus, s.c_str(), static_cast<int>(p - s.c_str()), s.c_str());
            }
        }
        else
        {
            Log(LOG_ERROR, "%s: 74HC165 port must be specified as a port number or name (\"A\" to \"G\")\n", jsonLocus);
        }

        // create the source if we have a valid source designation
        if (chain != nullptr && chain->IsValidPort(port))
        {
            return new Button::C74HC165Source(
                chain, port, activeHigh, lpFilterRise_us, lpFilterFall_us, debounceTimeOn_us, debounceTimeOff_us);
        }
    }
    else if (srcType == "nudge")
    {
        // read the parameters
        std::string axis = srcVal->Get("axis")->String();
        char axisc = 0;
        int threshold = srcVal->Get("threshold")->Int(0);
        std::string dir = srcVal->Get("direction")->String("+-");
        int onTime = srcVal->Get("onTime")->Int(10);
        int resetTime = srcVal->Get("resetTime")->Int(0);
        bool ok = true;

        // validate the axis and direction; set axisc to zero if it's invalid
        if (axis.length() == 1)
            axisc = axis[0];
        if (strchr("xyzXYZ", axisc) == 0)
            ok = false, Log(LOG_ERROR, "%s: invalid nudge axis value \"%s\", expected 'x', 'y', or 'z'\n", jsonLocus, axis.c_str());

        // validate the +/-
        bool plus = false, minus = false;
        if (dir == "+")
            plus = true;
        else if (dir == "-")
            minus = true;
        else if (dir == "+-" || dir == "-+")
            plus = minus = true;
        else
            ok = false, Log(LOG_ERROR, "%s: invalid nudge direction value \"%s\", expected '+', '-' or '+-'\n", jsonLocus, dir.c_str());

        // validate the threshold
        if (threshold < 0 || threshold > 32767)
            ok = false, Log(LOG_ERROR, "%s: nudge threshold value %d out of range, must be 0..32767\n", jsonLocus, threshold);

        // if the parameters look good, create the source
        if (ok)
            return new Button::NudgeDeviceSource(axisc, plus, minus, threshold, onTime, resetTime);
    }
    else if (srcType == "IR")
    {
        // read the parameters
        std::string code = srcVal->Get("code")->String();
        int latchingInterval = srcVal->Get("latchTime")->Int(0);
        int firstRepeatDelay = srcVal->Get("firstRepeatDelay")->Int(0);

        // validate the code string
        bool ok = true;
        IRCommandDesc cmd;
        if (!cmd.Parse(code.c_str()))
        {
            Log(LOG_ERROR, "%s: invalid IR command code string \"%s\"\n", jsonLocus, code.c_str());
            ok = false;
        }

        // create the source
        if (ok)
            return new Button::IRSource(cmd, latchingInterval, firstRepeatDelay);
    }
    else if (srcType == "plunger")
    {
        // set range defaults to an empty range, so that we'll never trigger on range
        // if no range is specified
        int rmin = 1;
        int rmax = 0;
        bool inside = true;

        // read the range
        if (auto *rangeVal = srcVal->Get("range") ; !rangeVal->IsUndefined())
        {
            rmin = rangeVal->Get("min")->Int(0);
            rmax = rangeVal->Get("max")->Int(0);
            inside = rangeVal->Get("inside")->Bool(true);
        }

        // reading the firing mode flag
        bool fire = srcVal->Get("fire")->Bool(false);
        int fireOnTime = srcVal->Get("fireOnTime")->Int(10);

        // set up the plunger
        return new Button::PlungerSource(fire, fireOnTime, inside, rmin, rmax);
    }
    else if (srcType == "zblaunch")
    {
        // ZB launch source
        bool modal = srcVal->Get("modal")->Bool(true);
        return new Button::ZBLaunchSource(modal);
    }
    else if (srcType == "output")
    {
        // set range defaults to an empty range, so that we'll never trigger on range
        // if no range is specified
        int rmin = 1;
        int rmax = 255;
        bool inside = true;

        // read the range
        if (auto *rangeVal = srcVal->Get("range") ; !srcVal->IsUndefined())
        {
            rmin = rangeVal->Get("min")->Int(0);
            rmax = rangeVal->Get("max")->Int(0);
            inside = rangeVal->Get("inside")->Bool(true);
        }

        // reading the firing mode flag
        bool fire = srcVal->Get("fire")->Bool(false);

        // set up the plunger
        auto *dofSource = new Button::DOFSource(inside, rmin, rmax);

        // link to the port after outputs have been configured
        OutputManager::AfterConfigure(json, [val = srcVal->Get("port"), dofSource](JSONParser &json) {
            dofSource->port = OutputManager::Get(json, val); });
            
        // return the new source
        return dofSource;
    }
    else if (srcType == "adc")
    {
        // An ADC source is required
        if (auto *adcVal = srcVal->Get("adc") ; !adcVal->IsUndefined())
        {
	        struct ADCEntry
	        {
	            ADCEntry(ADC* adc, int channel) : adc(adc), channel(channel) {}
	            ADC* adc;
	            int channel;
	        };
	
	        std::unordered_map<std::string, ADCEntry> adcEntryMap;
	
	        // add ADC entries from the ADC Manager list
	        adcManager.Enumerate([&adcEntryMap](ADC *adc)
	        {
	            // add the adc under a key
	            auto Add = [&adcEntryMap](const char *key, ADC *adc)
	            {
	                // add an entry for the device with no suffix, as shorthand for
	                // "device[0]" (logical channel 0 on the device)
	                adcEntryMap.emplace(key, ADCEntry(adc, 0));
	                Log(LOG_DEBUG, "ADCSource: added key %s for %s\n", key, adc->DisplayName());
	                // add channel-numbered keys, with [n] suffixes
	                for (int i = 0, n = adc->GetNumLogicalChannels() ; i < n ; ++i)
	                {
	                    char subkey[32];
	                    snprintf(subkey, sizeof(subkey), "%s[%d]", key, i);
	                    adcEntryMap.emplace(subkey, ADCEntry(adc, i));
	                    Log(LOG_DEBUG, "ADCSource: added subkey %s for %s\n", subkey, adc->DisplayName());
	                }
	            };
	
	            // add an entry under its main key
	            Add(adc->ConfigKey(), adc);
	
	            // add it under its alternate key, if it has one
	            if (const auto* altKey = adc->AltConfigKey(); altKey != nullptr)
	            {
	                Add(altKey, adc);
	                Log(LOG_DEBUG, "ADCSource: added altkey %s for %s\n", altKey, adc->DisplayName());
	            }
	        });
            // look it up
            auto adcStr = adcVal->String();
            if (auto it = adcEntryMap.find(adcStr); it != adcEntryMap.end())
            {
                // select the named ADC entry
                ADCEntry adcEntry = it->second;
                int32_t threshold = srcVal->Get("threshold")->Int32(32767);
                bool above = srcVal->Get("above")->Bool(true);
                Log(LOG_CONFIG, "ADCSource: %s %d %s %d\n", 
					adcEntry.adc->DisplayName(), adcEntry.channel, above ? "above" : "below", threshold);
                return new Button::ADCSource(adcEntry.adc, adcEntry.channel, above, threshold);
            }
            else
            {
                // error - ADC not found
                return Log(LOG_ERROR, "ADCSource: ADC \"%s\" is unknown, or not configured\n", adcStr.c_str()), nullptr;
            }
        }
        else
        {
            return Log(LOG_ERROR, "ADCSource: an \"adc\" item is required.\n"), nullptr;
        }
    }
    else
    {
        Log(LOG_ERROR, "%s: invalid source type \"%s\"\n", jsonLocus, srcType.String().c_str());
    }

    // no source created
    return nullptr;
}

// Create a GPIO source
Button::GPIOSource *Button::CreateGPIOSource(
    const char *jsonLocus, const char *pinoutLabel,
    int gp, bool activeHigh, bool usePull, bool enableLogging,
    int lpFilterRiseTime_us, int lpFilterFallTime_us, int debounceOnTime_us, int debounceOffTime_us)
{
    // figure the pull-up/pull-down settings - if pulls are enabled at
    // all, we enable the pull in the opposite direction of the active
    // level, so that the port is pulled in the OFF direction when the
    // connected button/switch is an open circuit
    bool pullUp = usePull && !activeHigh;
    bool pullDown = usePull && activeHigh;
    
    // Validate the GPIO
    if (!IsValidGP(gp))
    {
        Log(LOG_ERROR, "%s: invalid or missing GPIO port for source type 'gpio'\n", jsonLocus);
        return nullptr;
    }

    // Claim the GPIO in shared mode
    if (!gpioManager.ClaimSharedInput(jsonLocus, pinoutLabel, gp, pullUp, pullDown, true))
        return nullptr;

    // create the GPIO source
    return new Button::GPIOSource(gp, activeHigh, usePull, debounceOnTime_us, debounceOffTime_us, enableLogging);
}

// Parse an action
Button::Action *Button::ParseAction(const char *location, const JSONParser::Value *actionVal, bool inMacro)
{
    using MediaKey = USBIfc::MediaControl::Key;
    using Value = JSONParser::Value;

    // parse parameters based on the type
    auto &actionType = *actionVal->Get("type");
    if (actionType == "key")
    {
        // keyboard key by name or USB number
        auto *key = actionVal->Get("key");
        if (key->IsNumber())
        {
            // number -> USB usage code
            int keycode = key->UInt8();
            if (keycode < 1 || keycode > 255)
                return Log(LOG_ERROR, "%s: invalid USB keyboard key code %d, must be 1..255\n", location, keycode), nullptr;
            
            // create the keyboard action
            return new Button::KeyboardKeyAction(keycode);
        }
        else
        {
            // it's not a number, so interpret it as a key name string; convert
            // to lower-case and look it up in the USB key name map
            std::string keyname = key->String();
            std::transform(keyname.begin(), keyname.end(), keyname.begin(), ::tolower);
            if (auto it = usbKeyNames.find(keyname) ; it != usbKeyNames.end())
                return new Button::KeyboardKeyAction(it->second);

            // We didn't match any literal key name, but we could have a chord.
            // Check for a string of key names separated by "+".
            const int MAX_CHORD = 6;
            int chord[MAX_CHORD];
            int nChord = 0;
            for (const char *p = keyname.c_str() ; *p != 0 ; )
            {
                // scan to the next '+' or end of string
                const char *start = p;
                for ( ; *p != 0 && *p != '+' ; ++p) ;

                // look up this key name
                std::string ele(start, p - start);
                std::transform(ele.begin(), ele.end(), ele.begin(), ::tolower);
                if (auto it = usbKeyNames.find(ele); it != usbKeyNames.end())
                {
                    // matched - matched - add it to the chord
                    chord[nChord++] = it->second;

                    // if we're at the end of the string, we can stop
                    if (*p == 0)
                    {
                        // Success - create a macro representing the chord.  The macro
                        // runs as long as the key is held down and doesn't repeat.
                        auto *macro = new Button::MacroAction(false, false);

                        // Add the steps.  Each step is a simple key action, starting
                        // as soon as the source button is pressed, and held as long
                        // as the button remains pressed (duration:"hold").
                        for (int i = 0 ; i < nChord ; ++i)
                            macro->steps.emplace_back(0, 0, true, new Button::KeyboardKeyAction(chord[i]));

                        // return the new macro action
                        return macro;
                    }

                    // skip the '+'
                    ++p;

                    // there's more to come, but if we're out of space in the chord list,
                    // it's an error
                    if (nChord >= MAX_CHORD)
                    {
                        return Log(LOG_ERROR, "%s: key \"%s\" looks like a key chord, but contains too many elements (maximum of %d allowed)\n",
                                   location, key->String().c_str(), MAX_CHORD), nullptr;
                    }
                }
                else
                {
                    // Element name not found - fail.  If we found at least one
                    // prior element, or we're at a "+" now (indicating more
                    // elements follow), explain that we think it looks like a
                    // key chord with an invalid element.  Otherwise just take
                    // it as a single unknown key name.
                    if (*p == '+' || nChord != 0)
                    {
                        return Log(LOG_ERROR, "%s: key \"%s\" looks like a key chord, but element \"%.*s\" doesn't match any known keyboard key name\n",
                                   location, key->String().c_str(), static_cast<int>(p - start), start), nullptr;
                    }
                    else
                    {
                        return Log(LOG_ERROR, "%s: invalid keyboard key name \"%s\"\n", location, key->String().c_str()), nullptr;
                    }
                }
            }
        }
    }
    else if (actionType == "media")
    {
        // media key - get the key name
        static const std::unordered_map<std::string, USBIfc::MediaControl::Key> nameMap{
            { "mute", MediaKey::MUTE },
            { "volUp", MediaKey::VOLUME_UP },
            { "volDn", MediaKey::VOLUME_DOWN },
            { "next", MediaKey::NEXT_TRACK },
            { "prev", MediaKey::PREV_TRACK },
            { "stop", MediaKey::STOP },
            { "play", MediaKey::PLAY_PAUSE },
            { "eject", MediaKey::EJECT },      // Note: the Windows keyboard driver ignores this key, so it won't generate events on the Windows side
        };
        auto keyname = actionVal->Get("key")->String();
        if (auto it = nameMap.find(keyname) ; it != nameMap.end())
            return new Button::MediaKeyAction(it->second);
        else
            return Log(LOG_ERROR, "%s: invalid media key name \"%s\"\n", location, keyname.c_str()), nullptr;
    }
    else if (actionType == "gamepad")
    {
        // game pad action - this can be either a numbered button or a hat switch
        // button by name ("hat up", "hat down", "hat left", "hat right")
        if (auto *btnVal = actionVal->Get("button"); btnVal->IsString())
        {
            static const std::unordered_map<std::string, int> hatSwitchMap{
                { "hat-up", 1 },
                { "hat-down", 2 },
                { "hat-left", 3 },
                { "hat-right", 4 },
            };
            auto btnStr = btnVal->String();
            if (auto it = hatSwitchMap.find(btnStr) ; it != hatSwitchMap.end())
            {
                // found a hat switch button name
                return new Button::GamepadHatSwitchAction(it->second);
            }
            else if (int n = btnVal->Int(-1); gamepad.IsValidButton(n))
            {
                // the string contains a numeric value - treat it as a numbered button
                return new Button::GamepadButtonAction(n);
            }

            // not a valid named or numbered button
            return Log(LOG_ERROR, "%s: invalid button name \"%s\"\n", location, btnStr.c_str()), nullptr;
        }
        else if (!btnVal->IsUndefined())
        {
            // treat any other type as a numbered button
            if (int btn = btnVal->Int8(-1); gamepad.IsValidButton(btn))
                return new Button::GamepadButtonAction(btn);
            else
                return Log(LOG_ERROR, "%s: invalid gamepad button number\n", location), nullptr;
        }
        else
        {
            return Log(LOG_ERROR, "%s: gamepad button action requires 'button' setting\n", location), nullptr;
        }
    }
    else if (actionType == "xInput")
    {
        static const std::unordered_map<std::string, int> nameMap{
            { "up", XInput::DPAD_UP },
            { "down", XInput::DPAD_DOWN },
            { "left", XInput::DPAD_LEFT },
            { "right", XInput::DPAD_RIGHT },
            { "start", XInput::DPAD_START },
            { "back", XInput::DPAD_BACK },
            { "l3", XInput::DPAD_L3 },
            { "r3", XInput::DPAD_R3 },
            { "lb", XInput::BTN_LB },
            { "rb", XInput::BTN_RB },
            { "xbox", XInput::BTN_XBOX },
            { "a", XInput::BTN_A },
            { "b", XInput::BTN_B },
            { "x", XInput::BTN_X },
            { "y", XInput::BTN_Y },
        };
        std::string btnName = actionVal->Get("button")->String();
        std::transform(btnName.begin(), btnName.end(), btnName.begin(), ::tolower);
        if (auto it = nameMap.find(btnName) ; it != nameMap.end())
            return new Button::XInputButtonAction(it->second);
        else
            return Log(LOG_ERROR, "%s: invalid XInput button name \"%s\"\n", location, btnName.c_str()), nullptr;
    }
    else if (actionType == "openPinDev")
    {
        // Open Pinball Device
        // See http://mjrnet.org/pinscape/OpenPinballDevice/OpenPinballDeviceHID.htm

        // Get the button ID.  This can be a number 1-32 for a generic button with a
        // meaning assigned by the user per application, or a string giving the name
        // of one of the pre-defined pinball function buttons.
        auto btn = actionVal->Get("button");
        if (btn->IsNumber())
        {
            // Number -> generic button 1-32.
            int n = btn->Int(0);
            return n >= 1 && n <= 32 ? new Button::OpenPinDevGenericButtonAction(n) :
                (Log(LOG_ERROR, "%s: invalid Open Pinball Device generic button number %d, must be 1-32\n", location, n), nullptr);
        }
        else if (btn->IsString())
        {
            // String name -> pre-defined pinball function button.  The functions are
            // defined in the Open Pinball Device spec, with each function having a
            // fixed bit index in the "pinball buttons" field in the HID report.
            // The string names are Pinscape Pico inventions, since the HID report
            // works strictly in terms of the bit indices, but the names are chosen
            // to be descriptive of the functions defined in the spec.  The point
            // of the pre-defined function buttons is that simulators can map these
            // functions to their own corresponding functions without any need for
            // the user to configure the mapping, since the funtion for each button
            // slot is well-defined in the spec and common to all devices that
            // implement the spec.
            static const std::unordered_map<std::string, int> nameMap{
                { "start", 0 },
                { "exit", 1 },
                { "extra ball", 2 },
                { "coin 1", 3 },
                { "coin 2", 4 },
                { "coin 3", 5 },
                { "coin 4", 6 },
                { "launch", 7 },
                { "fire", 8 },
                { "left flipper", 9 },
                { "right flipper", 10 },
                { "left flipper 2", 11 },
                { "right flipper 2", 12 },
                { "left magnasave", 13 },
                { "right magnasave", 14 },
                { "tilt bob", 15 },
                { "slam tilt", 16 },
                { "coin door", 17 },
                { "service cancel", 18 },
                { "service down", 19 },
                { "service up", 20 },
                { "service enter", 21 },
                { "left nudge", 22 },
                { "forward nudge", 23 },
                { "right nudge", 24 },
                { "volume up", 25 },
                { "volume down", 26 },
            };
            std::string btnName = btn->String();
            std::transform(btnName.begin(), btnName.end(), btnName.begin(), ::tolower);
            if (auto it = nameMap.find(btnName) ; it != nameMap.end())
                return new Button::OpenPinDevPinballButtonAction(it->second);
            else
                return Log(LOG_ERROR, "%s: invalid Open Pinball Device button name \"%s\"\n", location, btnName.c_str()), nullptr;
        }
        else
        {
            return Log(LOG_ERROR, "%s: invalid Open Pinball Device button ID; must be a generic button number (1-32) "
                       "or a string naming pre-defined pinball button function\n", location), nullptr;
        }
    }
    else if (actionType == "reset")
    {
        // Pico reset action - regular reboot or bootloader mode
        std::string mode = actionVal->Get("mode")->String("normal");
        uint16_t holdTime = actionVal->Get("holdTime")->UInt16(2000);
        bool bootLoaderMode = (mode == "bootloader");
        if (!(bootLoaderMode || mode == "normal"))
            return Log(LOG_ERROR, "%s: invalid reset mode \"%s\"\n", location, mode.c_str()), nullptr;

        // create the reset action
        return new Button::ResetAction(bootLoaderMode, holdTime);
    }
    else if (actionType == "nightmode")
    {
        // night mode toggle
        return new Button::NightModeAction();
    }
    else if (actionType == "plungercal")
    {
        // plunger calibration mode
        return new Button::PlungerCalAction();
    }
    else if (actionType == "IR")
    {
        // IR transmitter action - read the parameters
        std::string code = actionVal->Get("code")->String();
        bool autoRepeatEnabled = actionVal->Get("autoRepeat")->Bool();

        // validate the code string
        IRCommandDesc cmd;
        if (!cmd.Parse(code.c_str()))
            return Log(LOG_ERROR, "%s: invalid IR command code string \"%s\"\n", location, code.c_str()), nullptr;

        // create the source
        return new Button::IRAction(cmd, autoRepeatEnabled);
    }
    else if (actionType == "macro")
    {
        // macros can't be nested
        if (inMacro)
            return Log(LOG_ERROR, "%s: nested macro actions aren't allowed\n", location), nullptr;

        // get the run-to-completion flag
        bool runToCompletion = actionVal->Get("runToCompletion")->Bool(true);
        bool repeat = actionVal->Get("repeat")->Bool(true);

        // get the steps list
        auto *steps = actionVal->Get("steps");
        if (!steps->IsArray())
            return Log(LOG_ERROR, "%s: macro 'steps' must be an array\n", location), nullptr;

        // create the action
        std::unique_ptr<MacroAction> macro(new MacroAction(runToCompletion, repeat));

        // process the steps list
        uint32_t curStartTime = 0;
        steps->ForEach([location, &macro, &curStartTime](int stepIndex, const JSONParser::Value *step) -> void
        {
            // parse the time properties
            int start = step->Get("start")->Int(0);
            const auto *durationVal = step->Get("duration");
            int duration = 0;
            bool hold = false;
            if (durationVal->IsString())
            {
                // check for special string values
                if (*durationVal == "hold")
                {
                    hold = true;
                }
                else
                {
                    return (void)Log(LOG_ERROR, "%s: macro step[%d]: invalid 'duration' string value \"%s\"\n",
                                     location, stepIndex, durationVal->String().c_str());
                }
            }
            else
            {
                // for any non-string duration, coerce to number and interpret as a duration in milliseconds
                duration = step->Get("duration")->Int(-1);
            }

            // validate the times
            if (start < 0)
                return (void)Log(LOG_ERROR, "%s: macro step[%d]: start time cannot be negative\n", location, stepIndex);
            if (duration < 0)
                return (void)Log(LOG_ERROR, "%s: macro step[%d]: duration undefined or invalid, must be greater than zero\n", location, stepIndex);

            // parse the action; make sure we got a valid one
            char subloc[128];
            snprintf(subloc, sizeof(subloc), "%s: action macro step[%d]", location, stepIndex);
            std::unique_ptr<Action> action(ParseAction(subloc, step->Get("action"), true));

            // adjust the macro-relative start time by this step's offset from the pervious step
            curStartTime += start;

            // add the step
            macro->steps.emplace_back(curStartTime, duration, hold, action.release());
        });

        // if we didn't successfully parse all of the action steps, delete the whole action
        if (macro->steps.size() != steps->Length())
            macro.reset(nullptr);

        // success - release the new macro action to the caller
        return macro.release();
    }
    else if (actionType == "none" || actionVal->IsUndefined())
    {
        return new Button::NullAction();
    }

    // action type not matched
    Log(LOG_ERROR, "%s: invalid action type \"%s\"\n", location, actionType.String().c_str());
    return nullptr;
}


// Handle periodic button tasks.  The main loop must call this when
// convenient, as frequently as possible.  This polls the current button
// states and fires events on state changes.
void Button::Task()
{
    // take a nudge device snapshot, if applicable
    if (buttonNudgeView != nullptr)
        buttonNudgeView->TakeSnapshot();

    // poll each button
    for (auto &b : buttons)
    {
        // poll the button for new input from its source
        b->Poll();

        // perform periodic tasks on the button's action handler
        b->action->Task();
    }
}

// Set or clear bits in the global shift state
void Button::SetShiftState(uint32_t bits, bool state)
{
    if (state)
        shiftButtonsPressed |= bits;
    else
        shiftButtonsPressed &= ~bits;
}

// Match the button's shift mask to the global shift state
bool Button::MatchShiftState()
{
    // Apply the button's shift mask to the live shift state
    // to get the required shift bit pattern
    uint32_t bits = shiftButtonsPressed & shiftMask;

    // If the result matches the shift bits pattern for the
    // button, accept the button press.
    if (bits == shiftBits)
    {
        // The masked bits match the button's shift bits, so
        // accept the button press.  Notify the relevant shift
        // buttons of the usage.
        for (auto &b : buttons)
            b->OnShiftUsage(bits);

        // the button is active in the current shift state
        return true;
    }
    else
    {
        // the button is not active in the current shift state
        return false;
    }
}


// ---------------------------------------------------------------------------
//
// USB Vendor Interface (configuration and control) support functions
//

// Query logical button descriptors.  Populates the buffer with a
// PinscapePico::ButtonList header struct, followed by an array of
// PinscapePico::ButtonDesc structs describing the assigned logical
// buttons.  Returns the size in bytes of the populated data, or 0
// if an error occurs.  Note that a successful call will return a
// non-zero size even if no logical buttons are defined, since the
// fixed header struct is always present, so zero buttons isn't a
// special case.
size_t Button::QueryDescs(uint8_t *buf, size_t bufSize)
{
    // Figure the amount of space we need for the transfer data:
    //
    //   ButtonList header
    //   ButtonDesc * number of buttons
    //   ButtonDevice * (number of PCA9555 chips + number of 74HC165 chains)
    //
    using ButtonList = PinscapePico::ButtonList;
    using ButtonDesc = PinscapePico::ButtonDesc;
    using ButtonDevice = PinscapePico::ButtonDevice;
    size_t resultSize =
        sizeof(ButtonList)
        + (buttons.size() * sizeof(ButtonDesc))
        + (PCA9555::CountConfigurations() * sizeof(ButtonDevice))
        + (C74HC165::CountConfigurations() * sizeof(ButtonDevice));

    // make sure there's room for all of the structs; return failure (0) if not
    if (bufSize < resultSize)
        return 0;

    // the ButtonList struct can only count to 65535 (uint16_t numDescs)
    if (buttons.size() > 65535)
        return 0;

    // Pre-clear the result buffer space to all zeroes, to ensure that
    // reserved padding bytes and any other elements we don't explicitly set
    // are properly zeroed.  It might be important for backwards compatility
    // that future versions can count on older versions definitely zeroing
    // unused bytes in structs going across the wire.
    memset(buf, 0, resultSize);

    // populate the header struct
    auto *bl = reinterpret_cast<ButtonList*>(buf);
    bl->cb = sizeof(ButtonList);
    bl->cbDesc = static_cast<uint16_t>(sizeof(ButtonDesc));
    bl->numDescs = static_cast<uint16_t>(buttons.size());
    bl->cbDevice = static_cast<uint16_t>(sizeof(ButtonDevice));
    bl->numDevices = static_cast<uint16_t>(PCA9555::CountConfigurations() + C74HC165::CountConfigurations());

    // populate the logical button descriptors
    auto *bd = reinterpret_cast<ButtonDesc*>(bl + 1);
    bl->ofsFirstDesc = static_cast<uint16_t>(reinterpret_cast<uint8_t*>(bd) - buf);
    for (auto &b : buttons)
        b->PopulateDesc(bd++);

    // populate the device descriptors
    auto *dd = reinterpret_cast<ButtonDevice*>(bd);
    bl->ofsFirstDevice = static_cast<uint16_t>(reinterpret_cast<uint8_t*>(dd) - buf);
    PCA9555::PopulateDescs(dd);
    C74HC165::PopulateDescs(dd);

    // success - return the result size
    return resultSize;
}

// Query logical button states.  Populates the buffer with an array
// of bytes equal in size to the number of defined buttons.  Returns
// the number of bytes populated, or 0xFFFFFFFF on error.  Note that
// it's valid to have zero logical buttons configured, in which case
// the return value is zero, which isn't an error.
size_t Button::QueryStates(uint8_t *buf, size_t bufSize, uint32_t &shiftState)
{
    // make sure we have space - one byte per button
    size_t resultSize = buttons.size();
    if (bufSize < resultSize)
        return 0xFFFFFFFF;

    // popluate the states - 0=OFF, 1=ON
    for (auto &b : buttons)
        *buf++ = b->GetLogicalState() ? 1 : 0;

    // fill in the global shift state
    shiftState = shiftButtonsPressed;

    // success - return the result size
    return resultSize;
}

// Query GPIO input states.  Populates the buffer with an array of
// bytes, one per Pico GPIO port, giving the current live input
// states of the pins.
size_t Button::QueryGPIOStates(uint8_t *buf, size_t bufSize)
{
    // make sure we have space - one byte per GPIO, 32 GPIOs
    size_t resultSize = 32;
    if (bufSize < resultSize)
        return 0;

    // retrieve all GPIOs
    uint32_t allPortMask = gpio_get_all();

    // popoulate one byte per GPIO
    for (int gp = 0, bit = 1 ; gp < 32 ; ++gp, bit <<= 1)
        *buf++ = ((allPortMask & bit) != 0) ? 1 : 0;

    // return the result size
    return resultSize;
}

// Populate a PinscapePico::ButtonDesc for this button
void Button::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->type = GetVendorIfcType();
    source->PopulateDesc(desc);
    action->PopulateDesc(desc);
    desc->shiftMask = shiftMask;
    desc->shiftBits = shiftBits;
}


// ---------------------------------------------------------------------------
//
// Basic pushbutton
//
void Pushbutton::Poll()
{
    // poll the input source for its new state
    bool sourceState = source->Poll();

    // check for a state change
    if (sourceState != lastSourceState)
    {
        // update the source state
        lastSourceState = sourceState;

        // If the source is transitioning OFF to ON, check the shift key
        // state to see if we're active with the current shift keys.  If
        // so, switch the logical state to on.  Note that the modifier
        // key check occurs only at *changes* in the source state - if
        // you push and hold this button while its shift buttons aren't
        // pushed, the push is ignored, even if you push the shift
        // buttons later.  The shift buttons have to be already pressed
        // at the moment you press this button.
        //
        // If the source is transitioning ON to OFF, switch the logical
        // state off, regardless of the state of the shift keys.
        bool newLogicalState = logicalState;
        if (!logicalState && sourceState && MatchShiftState())
        {
            // source changing OFF -> ON, shift buttons selected: switch on
            newLogicalState = true;
        }
        else if (logicalState && !sourceState)
        {
            // source changing ON -> OFF: switch off
            newLogicalState = false;
        }

        // if the logical state has changed, fire an event
        if (newLogicalState != logicalState)
        {
            // record the new state
            logicalState = newLogicalState;

            // if newly on, trigger remote wake if applicable
            if (logicalState && remoteWake)
                usbIfc.SetWakePending();
            
            // fire the state change event on the action handler
            action->OnStateChange(logicalState);
        }
    }
}

// ---------------------------------------------------------------------------
//
// Push-and-hold button
//
void HoldButton::Poll()
{
    // poll the input source for its new state
    bool sourceState = source->Poll();

    // check for a state change
    uint64_t now = time_us_64();
    if (sourceState != lastSourceState)
    {
        // update the source state
        lastSourceState = sourceState;

        // If the source is transitioning OFF to ON, check the shift key
        // state to see if we're active with the current shift keys.  If
        // so, switch the logical state to on.  Note that the modifier
        // key check occurs only at *changes* in the source state - if
        // you push and hold this button while its shift buttons aren't
        // pushed, the push is ignored, even if you push the shift
        // buttons later.  The shift buttons have to be already pressed
        // at the moment you press this button.
        //
        // If the source is transitioning ON to OFF, switch the logical
        // state off, regardless of the state of the shift keys.
        if (!rawLogicalState && sourceState && MatchShiftState())
        {
            // source changing OFF -> ON, shift buttons selected: switch on,
            // and note the transition time
            rawLogicalState = true;
            tOn = now;
        }
        else if (rawLogicalState && !sourceState)
        {
            // Source changing ON -> OFF, so follow that in the internal
            // raw logical state.
            rawLogicalState = false;

            // If logicalState is OFF, we're still in the hold period
            // before the main action is triggered, so this is a short
            // press that never activated the main action.  This is the
            // condition where the short press action fires, if defined.
            if (!logicalState && shortPressAction != nullptr)
            {
                // fire the action (if it's not already firing)
                if (tShortPressEnd == 0)
                    shortPressAction->OnStateChange(true);

                // Set the end time.  Note that if the short-press
                // action was already firing, this has the effect of
                // extending the firing time rather than starting a
                // new action pulse.
                tShortPressEnd = now + shortPressActionDuration;
            }
        }
    }

    // Apply the raw logical state + timer to the logical state
    if (logicalState && !rawLogicalState)
    {
        // logical state was ON, raw logical state is now OFF -> ON->OFF transition
        logicalState = false;
        action->OnStateChange(false);
    }
    else if (!logicalState && rawLogicalState && now > tOn + holdTime_us)
    {
        // logical state was OFF, raw logical state is ON, hold time satisfied -> OFF->ON transition
        logicalState = true;
        action->OnStateChange(true);

        // trigger remote wake if applicable
        if (remoteWake)
            usbIfc.SetWakePending();
    }

    // If the short-press action is firing, and we've reached the
    // end of the pulse time, stop firing.
    if (shortPressAction != nullptr && tShortPressEnd != 0 && now > tShortPressEnd)
    {
        // cancel the action
        shortPressAction->OnStateChange(false);

        // set the end time to zero, which signifies that the action is inactive
        tShortPressEnd = 0;
    }
}

// ---------------------------------------------------------------------------
//
// Toggle button
//
void ToggleButton::Poll()
{
    // poll the input source for its new state
    bool sourceState = source->Poll();

    // If the source changed from OFF to ON since the last reading,
    // and the current shift state matches its shift mask, flip the toggle's
    // logical state.
    if (sourceState && !lastSourceState && MatchShiftState())
    {
        // source control just pulsed ON - invert the toggle state
        logicalState = !logicalState;

        // fire the state change event on the action handler
        action->OnStateChange(logicalState);

        // if newly on, trigger remote wake if applicable
        if (logicalState && remoteWake)
            usbIfc.SetWakePending();
    }

    // reecord the new source state for next time
    lastSourceState = sourceState;
}

// ---------------------------------------------------------------------------
//
// On/Off button
//
void OnOffButton::Poll()
{
    // poll the ON and OFF input sources
    bool onSourceState = source->Poll();
    bool offSourceState = offSource->Poll();

    // If the ON source changed from OFF to ON since the last reading,
    // and the shift mask matches the current shift key state, and the
    // logical control is currently off, turn it on.
    if (onSourceState && !lastOnSourceState && MatchShiftState() && !logicalState)
    {
        // ON source control just pulsed ON - turn on the logical control
        // and fire the event handler
        logicalState = true;
        action->OnStateChange(logicalState);

        // trigger remote wake if applicable
        if (remoteWake)
            usbIfc.SetWakePending();
    }

    // If the OFF source changed from OFF to ON since the last reading,
    // and the shift mask matches the current shift key state, and the
    // logical control is on, turn it off.
    if (offSourceState && !lastOffSourceState && MatchShiftState() && logicalState)
    {
        // OFF source control just pulsed ON - turn off the logical control
        // and fire the event handler
        logicalState = false;
        action->OnStateChange(logicalState);
    }

    // record the new source states for next time
    lastOnSourceState = onSourceState;
    lastOffSourceState = offSourceState;
}

// ---------------------------------------------------------------------------
//
// Pulse button
//

// Construct.  Note that the time arguments are expressed in milliseconds,
// but we use microseconds internally (since that's the unit that the Pico's
// time-related APIs use), so we must convert units here.  Converting is
// almost as simple as muliplying by 1000 (us per ms), with the little
// detail that we should also widen the values to 32 bits to ensure that
// longer time values don't overflow.  The inputs are given in millseconds
// and passed in uint16_t arguments because that's the natural scsale for
// these intervals: you don't want to use intervals shorter than a few
// milliseconds, because whatever you're connecting as the output action
// will probably miss events that are shorter than that; and pulses can't
// reasonably be longer than a few seconds (so, a few thousand ms) without
// creating so much lag that this wouldn't be usable in any sort of
// sensible input control system, which is the only use case we're
// contemplating.
PulseButton::PulseButton(
    Source *source, uint32_t shiftMask, uint32_t shiftBits,
    Action *action, Action *offAction,
    uint16_t onPulseTime_ms, uint16_t offPulseTime_ms,
    uint16_t minPulseSpacing_ms) :
    Button(source, shiftMask, shiftBits, action),
    offAction(offAction),
    onPulseTime_us(static_cast<uint32_t>(onPulseTime_ms) * 1000),
    offPulseTime_us(static_cast<uint32_t>(offPulseTime_ms) * 1000),
    minPulseSpacing_us(static_cast<uint32_t>(minPulseSpacing_ms) * 1000)
{
}

void PulseButton::Poll()
{
    // If a pulse is in progress, process it
    switch (pulseState)
    {
    case PulseState::None:
        // No pulse is in progress.  Compare the state of the underlying
        // input control to the logical state.  If the logical state is
        // out of date, send a pulse to inform the host of the change.
        // 
        // A pulse can only be initiated when the current shift state
        // matches the button's shift condition, so ignore state
        // changess when the shift state doesn't match.  I don't think
        // there are any sensible use cases for shifted pulse buttons
        // with actual physical shift buttons, but shifting might be
        // useful when a shift bit represents some abstract mode, such
        // as night mode being in effect, or a DOF signal being on or
        // off.  
        if (bool newState = source->Poll(); newState != logicalState && MatchShiftState())
        {
            // Start a pulse of the appropriate type.  If we're switching
            // from ON to OFF, and there's a separate OFF action, use the
            // OFF action; otherwise use the base action, which serves as
            // the ON action in all cases, and as the combined ON/OFF
            // action when there's no separate OFF action.
            pulseAction = !newState && offAction != nullptr ?
                          offAction.get() : action.get();

            // Figure the duration of the pulse.  We only actually send
            // a pulse if the pulse time is non-zero.
            uint64_t dt = newState ? onPulseTime_us : offPulseTime_us;
            if (dt != 0)
            {
                // Start the pulse.  Note that the action is pulsed ON even
                // when the underlying control is switching OFF.
                pulseAction->OnStateChange(true);

                // trigger remote wake if applicable
                if (remoteWake)
                    usbIfc.SetWakePending();

                // set pulse state, and set the end time for the current pulse
                pulseState = PulseState::Pulse;
                tEnd = time_us_64() + dt;
            }

            // the logical state is now in sync with the input state
            logicalState = newState;
        }
        break;

    case PulseState::Pulse:
        // A pulse is in progress.  Check to see if it's finished.
        if (time_us_64() >= tEnd)
        {
            // pulse ended - turn off the current action
            pulseAction->OnStateChange(false);

            // switch modes to the space between pulses, and set the end time
            pulseState = PulseState::Space;
            pulseAction = nullptr;
            tEnd += minPulseSpacing_us;
        }
        break;

    case PulseState::Space:
        // We're in the time padding between pulses.  Check to see if it's finished.
        if (time_us_64() >= tEnd)
        {
            // padding finished - return to the base state
            pulseState = PulseState::None;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
//
// Shift button
//

ShiftButton::ShiftButton(Source *source, uint32_t shiftMask, uint32_t shiftBits, Action *action, uint16_t pulseTime_ms) :
    Button(source, shiftMask, shiftBits, action),
    pulseTime_us(static_cast<uint32_t>(pulseTime_ms) * 1000)
{
}

void ShiftButton::OnShiftUsage(uint32_t maskedBits)
{
    // if my shift bit is in the set of masked bits used for this
    // button activation, record the usage
    if ((maskedBits & shiftBits) == shiftBits)
        shiftUsed = true;
}

void ShiftButton::Poll()
{
    // get the new source state
    bool newSourceState = source->Poll();

    // check for a change from our logical state
    if (newSourceState != logicalState)
    {
        // record the new state
        logicalState = newSourceState;

        // update the global shift state
        SetShiftState(shiftBits, newSourceState);

        // if newly on, trigger remote wake if applicable
        if (logicalState && remoteWake)
            usbIfc.SetWakePending();

        // check the transition direction
        if (newSourceState)
        {
            // New push.  If the pulse time is zero, this is a Shift-AND
            // button, so we immediately pass the push through to the action.
            // For a Shift-OR button, we take no action on push; we have to
            // wait until the button is released to determine whether or not
            // to fire the action.
            if (pulseTime_us == 0)
                action->OnStateChange(true);

            // No other button's shifted function has been engaged during
            // the new press yet.
            shiftUsed = false;
        }
        else
        {
            // Release.  If the pulse time is zero, this is a Shift-AND
            // button, so we pass the state change through to the action
            // immediately.  Otherwise, it's a Shift-OR button, so we
            // pulse the action only if no other buttons were pressed
            // while this button was being pressed.
            if (pulseTime_us == 0)
            {
                // Shift-AND mode - simply pass through the state change
                action->OnStateChange(false);
            }
            else if (!shiftUsed && !pulseActive)
            {
                // Shift-OR mode, and no other button was pressed, so we
                // engage our own action with a timed pulse.  The timed
                // pulse can only be sent if there isn't already a pulse
                // in progress.

                // Fire the action.  Note that the action is pulsed ON
                // when the underlying control turns OFF (i.e., the button
                // press is released)
                action->OnStateChange(true);

                // set the pulse state and end time
                pulseActive = true;
                tPulseEnd = time_us_64() + pulseTime_us;
            }
        }
    }

    // if a timed pulse is in progress, check to see if it's finished
    if (pulseActive && time_us_64() >= tPulseEnd)
    {
        // end the pulse in the action
        action->OnStateChange(false);

        // reset the pulse state
        pulseActive = false;
        tPulseEnd = 0;
    }
}

// ---------------------------------------------------------------------------
//
// Null source
//

void Button::NullSource::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->sourceType = PinscapePico::ButtonDesc::SRC_NULL;
}

// ---------------------------------------------------------------------------
//
// Debounced input source
//
bool Button::DebouncedSource::Poll()
{
    // poll the new physical state
    bool live = PollPhysical();

    // check for a change from the last read
    if (live != logicalState)
    {
        // Make sure the last state change has been stable for the
        // minimum hold time.  Ignore changes within the hold time,
        // since these might represent the rapid oscillations known
        // as "bounce" that typically occur for a brief interval after
        // the moment of physical switch contact.
        uint32_t dt = logicalState ? dtOn : dtOff;
        uint64_t now = time_us_64();
        if (now - tLastLogicalChange > dt)
        {
            // the last state has been stable long enough - apply the change
            logicalState = live;
            tLastLogicalChange = now;
        }
    }

    // return the new logical state
    return logicalState;
}

// ---------------------------------------------------------------------------
//
// Second-core debounced source
//

// list of all sources handled on the second core
std::list<Button::SecondCoreDebouncedSource*> Button::SecondCoreDebouncedSource::all;

Button::SecondCoreDebouncedSource::SecondCoreDebouncedSource(
    bool activeHigh, uint32_t lpFilterRise, uint32_t lpFilterFall, uint32_t dtOn, uint32_t dtOff) :
    activeHigh(activeHigh), lpFilterRise(lpFilterRise), lpFilterFall(lpFilterFall), dtOn(dtOn), dtOff(dtOff)
{
    // add the global list entry for the second core task handler
    all.emplace_back(this);
}

void Button::SecondCoreDebouncedSource::SecondCoreTask()
{
    // visit each enlisted button
    for (auto *b : all)
        b->SecondCorePoll();
}

void Button::SecondCoreDebouncedSource::SecondCorePoll()
{
    // check for a change of the physical state of the GPIO input
    bool live = PollPhysical();

    // if the new state differs from the last physical state, mark the edge time
    uint64_t now = time_us_64();
    if (live != lastPhysicalState)
        tEdge = now;

    // record the new physical state for next time
    lastPhysicalState = live;

    // check for a change from the last debounced state
    if (live != debouncedPhysicalState)
    {
        // apply the low-pass filter: the NEW state must be in effect continuously
        // for the lp filter time before we can recognize the change
        if (now - tEdge >= (live ? lpFilterRise : lpFilterFall))
        {
            // apply the "hold" filter: the PRIOR state must have been in effect
            // for at least the hold time before we can recognize the change
            if (now - tLastDebouncedStateChange >= (debouncedPhysicalState == activeHigh ? dtOn : dtOff))
            {
                // update the state and record the new time
                debouncedPhysicalState = live;
                tLastDebouncedStateChange = now;
                
                // process the change
                OnDebouncedStateChange(live);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//
// GPIO button input source
//

// list of all sources handled on the second core
std::list<Button::GPIOSource*> Button::GPIOSource::allgs;

Button::GPIOSource::GPIOSource(
    int gpNumber, bool activeHigh, bool usePull,
    int lpFilterRiseTime_us, int lpFilterFallTime_us, int debounceTimeOn_us, int debounceTimeOff_us,
    bool enableEventLogging) :
    SecondCoreDebouncedSource(activeHigh, lpFilterRiseTime_us, lpFilterFallTime_us, debounceTimeOn_us, debounceTimeOff_us),
    gpNumber(gpNumber), usePull(usePull), eventLog(enableEventLogging)

{
    // Note that Configure() claims the port in shared mode before creating
    // the GPIOSource object, and the claim automatically configures the
    // physical GPIO as an input with the specified parameters, so we don't
    // have to do any further GPIO setup here.

    // add myself to the master list
    allgs.emplace_back(this);
}

void Button::GPIOSource::OnDebouncedStateChange(bool newState)
{
    // if event logging is in effect, add an event
    if (eventLog.enable)
    {
        const auto TypePress = EventLog::Event::TypePress;
        const auto TypeRelease = EventLog::Event::TypeRelease;
        eventLog.AddEvent(activeHigh == newState ? TypePress : TypeRelease);
    }
}

void Button::GPIOSource::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->sourceType = PinscapePico::ButtonDesc::SRC_GPIO;
    desc->sourcePort = gpNumber;
    desc->sourceActiveHigh = (activeHigh ? 1 : 0);
    
    if (eventLog.enable)
        desc->flags |= PinscapePico::ButtonDesc::FLAG_EVENTLOG;
}

bool Button::GPIOSource::ClearEventLog(int gpio)
{
    // return success if we match the GPIO, or the GPIO is the wildcard 255
    bool ok = (gpio == 255);

    // scan all logs
    for (auto *b : allgs)
    {
        // match on the exact GPIO or the wildcard 255
        if (b->gpNumber == gpio || gpio == 255)
            b->eventLog.Clear();
    }

    return ok;
}

size_t Button::GPIOSource::QueryEventLog(uint8_t *buf, size_t buflen, int gpio)
{
    // find the event log for the GPIO
    for (auto *b : allgs)
    {
        // match on GPIO
        if (b->gpNumber == gpio && b->eventLog.enable)
        {
            // Make sure there's room for the header plus at least one item.
            // (If there's no room for one item, we'd only be able to return
            // a header with an empty list, which would be pointless as it
            // wouldn't let the caller retrieve any items, no matter how many
            // calls they made.  If we can retrieve even one item, the caller
            // can eventually retrieve everything by making repreated calls.)
            using Header = PinscapePico::ButtonEventLog;
            using Item = PinscapePico::ButtonEventLogItem;
            if (buflen < sizeof(Header) + sizeof(Item))
                return 0;

            // build the header at the start of the buffer
            auto *hdr = reinterpret_cast<Header*>(buf);
            hdr->cb = sizeof(Header);
            hdr->numItems = 0;
            hdr->cbItem = sizeof(Item);

            // now add items, oldest first, until we run out of items
            // or run out of space in the buffer
            size_t rem = buflen - sizeof(Header);
            auto *item = reinterpret_cast<Item*>(hdr + 1);
            EventLog::Event ev;
            while (rem >= sizeof(Item) && b->eventLog.GetEvent(ev))
            {
                // count the event in the header
                hdr->numItems += 1;

                // populate the buffer item
                item->t = ev.t;
                item->eventType = ev.type;

                // count the space consumed and advance the output pointer
                rem -= sizeof(Item);
                ++item;
            }

            // success - return the final size consumed
            return reinterpret_cast<uint8_t*>(item) - buf;
        }
    }

    // the GPIO isn't logging events - we have no data to return
    return 0;
}

Button::GPIOSource::EventLog::EventLog(bool enable) : enable(enable)
{
    // if enabled, initialize a mutex for coordinating activities across the cores
    if (enable)
        mutex_init(&mutex);
}

void Button::GPIOSource::EventLog::Clear()
{
    if (enable)
    {
        // acquire the mutex while working
        MutexLocker locker(&mutex);
        
        // reset the read/write pointers
        read = 0;
        write = 0;
    }
}

void Button::GPIOSource::EventLog::AddEvent(int type)
{
    if (enable)
    {
        // acquire the mutex while working
        MutexLocker locker(&mutex);
        
        // add the event at the write pointer
        event[write].Init(type);
        
        // bump the write pointer
        if (++write >= MaxEvents)
            write = 0;
        
        // if the write pointer collided with the read pointer, bump the read
        // pointer (deleting the oldest event)
        if (write == read && ++read >= MaxEvents)
            read = 0;
    }
}

bool Button::GPIOSource::EventLog::GetEvent(Event &e)
{
    if (enable)
    {
        // acquire the mutex while working
        MutexLocker locker(&mutex);
        
        // if the queue is empty, there's nothing to do
        if (read == write)
            return false;
        
        // load the latest event into the caller's buffer
        e = event[read];
        
        // bump the read pointer
        if (++read >= MaxEvents)
            read = 0;
        
        // successfully read an event
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
//
// BOOTSEL Button input source
//

bool Button::BOOTSELSource::Poll()
{
    // Check BOOTSEL periodically.  Reading BOOTSEL is fairly slow - it
    // takes about 100us per call - and worse, it blocks interrupts for
    // the entire call, which will extend interrupt latency for events
    // that occur while a read is in progress.  To minimize the impact,
    // throttle the calls with a user-configurable time
    uint64_t now = time_us_64();
    if (now - tCheck > checkInterval)
    {
        // read the button and update the check time
        state = BootselButton::Read();
        tCheck = now;
    }

    // return the state
    return state;
}

// ---------------------------------------------------------------------------
//
// PCA9555 port extender input source
//

Button::PCA9555Source::PCA9555Source(PCA9555 *chip, uint8_t port, bool activeHigh, int debounceTime_us) :
    DebouncedSource(debounceTime_us, debounceTime_us),
    chip(chip), port(port), activeHigh(activeHigh)
{
}

const char *Button::PCA9555Source::FullName(char *buf, size_t buflen) const
{
    snprintf(
        buf, buflen, "PCA9555[%d] port IO%d_%d",
        chip != nullptr ? chip->GetConfigIndex() : -1,
        port / 8, port % 8);
    return buf;
}

void Button::PCA9555Source::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->sourceType = PinscapePico::ButtonDesc::SRC_PCA9555;
    desc->sourceUnit = chip->GetConfigIndex();
    desc->sourcePort = port;
    desc->sourceActiveHigh = (activeHigh ? 1 : 0);
}

bool Button::PCA9555Source::PollPhysical()
{
    // read the GPIO state
    bool state = chip->Read(port);

    // If we're active high, the physical HIGH/LOW state maps directly to
    // the logical ON/OFF sate.  If we're active low, the logical state is
    // the inverse of the physical state.
    return  activeHigh ? state : !state;
}

// ---------------------------------------------------------------------------
//
// 74HC165 shift-register chip input source
//

Button::C74HC165Source::C74HC165Source(
    C74HC165 *chain, int port, bool activeHigh,
    int lpFilterRiseTime_us, int lpFilterFallTime_us, int debounceTimeOn_us, int debounceTimeOff_us) :
    SecondCoreDebouncedSource(activeHigh, lpFilterRiseTime_us, lpFilterFallTime_us, debounceTimeOn_us, debounceTimeOff_us),
    chain(chain), port(port)
{
}

const char *Button::C74HC165Source::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "74HC165[%d] chip %d port %c", chain != nullptr ? chain->GetConfigIndex() : -1, port >> 3, (port & 0x07) + 'A');
    return buf;
}

void Button::C74HC165Source::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->sourceType = PinscapePico::ButtonDesc::SRC_74HC165;
    desc->sourceUnit = chain->GetConfigIndex();
    desc->sourcePort = port;
    desc->sourceActiveHigh = (activeHigh ? 1 : 0);
}

bool Button::C74HC165Source::PollPhysical()
{
    // read the port state
    return chain->Get(port);
}

// ---------------------------------------------------------------------------
//
// IR Receiver Source
//

Button::IRSource::IRSource(const IRCommandDesc &cmd,
    uint32_t latchingInterval_ms, uint32_t firstRepeatDelay_ms) :
    cmd(cmd),
    latchingInterval_us(latchingInterval_ms * 1000),
    firstRepeatDelay_us(firstRepeatDelay_ms * 1000)
{
    // subscribe for events that match our command desciptor
    irReceiver.Subscribe(this, { cmd });
}

void Button::IRSource::OnIRCommandReceived(const IRCommandReceived &cmd, uint64_t dt)
{
    // If this is a repeated code, ignore it if it's within the first
    // repeat delay interval.  If it's not repeated, start a new repeat
    // delay interval.
    uint64_t now = time_us_64();
    if (cmd.isAutoRepeat)
    {
        // ignore auto-repeats within the initial delay interval
        if (now < tFirstRepeatDelayEnd)
            return;
    }
    else
    {
        // set the repeat delay
        tFirstRepeatDelayEnd = now + firstRepeatDelay_us;
    }

    // flag that the code has been received
    received = true;

    // set the latching timer
    tLatchingEnd = now + latchingInterval_us;
}

bool Button::IRSource::Poll()
{
    // Trigger when our command has been received, and then immediately
    // clear the received flag.  We only trigger once when a code is
    // received.
    bool triggered = received;
    received = false;

    // read as ON if we were triggered OR we're within the latching
    // interval of the last activation
    return triggered || time_us_64() < tLatchingEnd;
}


// ---------------------------------------------------------------------------
//
// Nudge input source.  This interprets nudge device readings past
// a threshold level as virtual button presses.
//

Button::NudgeDeviceSource::NudgeDeviceSource(
    char axis, bool plus, bool minus, int16_t threshold, uint32_t onTime_ms, uint32_t resetTime_ms) :
    axis(isupper(axis) ? tolower(axis) : axis), plus(plus), minus(minus), threshold(threshold),
    onTime_us(onTime_ms * 1000), resetTime_us(resetTime_ms * 1000)
{
    // if we don't already have a nudge viewer singleton, create one
    if (buttonNudgeView == nullptr)
        buttonNudgeView = nudgeDevice.CreateView();

    // set the reader, based on the axis
    switch (axis)
    {
    case 'x':
    case 'X':
        ReadDevice = []() -> int16_t { return buttonNudgeView->GetX(); };
        break;

    case 'y':
    case 'Y':
        ReadDevice = []() -> int16_t { return buttonNudgeView->GetY(); };
        break;

    case 'z':
    case 'Z':
        ReadDevice = []() -> int16_t { return buttonNudgeView->GetZ(); };
        break;

    default:
        ReadDevice = []() -> int16_t { return 0; };
        break;
    }
}

bool Button::NudgeDeviceSource::Poll()
{
    // if we're within the post-firing ON interval, read as ON
    uint64_t now = time_us_64();
    if (now < onUntilTime)
        return true;

    // if we're within the post-firing lockout interval, read as OFF
    if (now < offUntilTime)
        return false;
    
    // get the instantaneous reading from the nudge device
    int16_t cur = (*this->ReadDevice)();

    // check if it's past the threshold in the specified direction(s)
    if ((plus && cur > threshold) || (minus && cur < -threshold))
    {
        // It's past the threshold, so fire the button.  We fire as a
        // one-time pulse of duration "on time", then lock out further
        // firing until the lockout time has elapsed.
        onUntilTime = now + onTime_us;
        offUntilTime = onUntilTime + resetTime_us;

        // return an ON pulse for this reading
        return true;
    }

    // not fired
    return false;
}

// ---------------------------------------------------------------------------
//
// Plunger source
//

const char *Button::PlungerSource::FullName(char *buf, size_t buflen) const
{
    char rangeBuf[32] = "";
    if (rangeMin <= rangeMax)
        sprintf(rangeBuf, "%sin [%d..%d]", inside ? "" : "not ", rangeMin, rangeMax);
    const char *fireBuf = fire ? "firing event" : "";

    snprintf(buf, buflen, "Plunger (%s%s%s)", fireBuf, fireBuf[0] != 0 && rangeBuf[0] != 0 ? ", " : "", rangeBuf);
    return buf;
}

bool Button::PlungerSource::Poll()
{
    // if we're still in a firing event, read as on
    uint64_t now = time_us_64();
    if (now < fireOnUntil)
        return true;

    // check if we trigger on firing events; if so, we're on if a firing
    // event is in progress
    if (fire && plunger.IsFiring())
    {
        fireOnUntil = now + fireOnTime_us;
        return true;
    }
    
    // get the current plunger reading
    int z = plunger.GetZ();

    // check to see if we're within the min..max range
    bool isInside = (z >= rangeMin && z <= rangeMax);

    // trigger according to the inside/outside setting
    return inside ? isInside : !isInside;
}

// ---------------------------------------------------------------------------
//
// ZB Launch source
//
bool Button::ZBLaunchSource::Poll()
{
    // if we're modal, the source is always off if ZB Launch Mode is disengaged
    if (modal && !zbLaunchBall.IsActive())
        return false;
    
    // we're either modeless, or ZB Launch is engaged, so our state
    // equals the ZB Launch virtual button state
    return zbLaunchBall.IsFiring();
}


// ---------------------------------------------------------------------------
//
// DOF Output Port Source
//
bool Button::DOFSource::Poll()
{
    // get the current port value
    if (port != nullptr)
    {
        int level = reinterpret_cast<OutputManager::Port*>(port)->Get();
        bool inRange = (level >= range.min && level <= range.max);
        return range.inside ? inRange : !inRange;
    }
    return false;
}

void Button::DOFSource::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->sourceType = PinscapePico::ButtonDesc::SRC_OUTPORT;
    desc->sourcePort = portNum;
}

// ---------------------------------------------------------------------------
//
// ADC Source
//
Button::ADCSource::ADCSource(ADC* adc, int channel, bool above, int32_t threshold) :
    adc(adc), channel(channel), above(above), threshold(threshold) {
    // enable sampling on the ADC
    adc->EnableSampling();
    // build the friendly name based on the ADC name
    fullName = Format("ADCSource(%s %i %s %i)", adc->DisplayName(), channel, above ? "above" : "below", threshold);
    Log(LOG_DEBUG, "ADCSource: %s\n", fullName);
}

bool Button::ADCSource::Poll() {
    // Read the adc
    ADC::Sample s = adc->ReadNorm(channel);
    // compare to the threshold
    if (above) {
        return s.sample > threshold;
    }
    return s.sample < threshold;
}

// ---------------------------------------------------------------------------
//
// Action class
//

// static name generation buffer
char Button::Action::nameBuf[Button::Action::NAME_BUF_LEN];

// ---------------------------------------------------------------------------
//
// Keyboard key action
//

void Button::KeyboardKeyAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->actionType = PinscapePico::ButtonDesc::ACTION_KEY;
    desc->actionDetail = usbKeyCode;
}

void Button::KeyboardKeyAction::OnStateChange(bool state)
{
    // pass the event to the keyboard device handler
    keyboard.KeyEvent(usbKeyCode, state);
}

// ---------------------------------------------------------------------------
//
// Media key action
//

void Button::MediaKeyAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->actionType = PinscapePico::ButtonDesc::ACTION_MEDIA;
    desc->actionDetail = USBIfc::MediaControl::GetUsageFor(key);
}

void Button::MediaKeyAction::OnStateChange(bool state)
{
    // pass the event to the media control device handler
    mediaControl.KeyEvent(key, state);
}

// ---------------------------------------------------------------------------
//
// Gamepad button action
//

void Button::GamepadButtonAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->actionType = PinscapePico::ButtonDesc::ACTION_GAMEPAD;
    desc->actionDetail = buttonNum;
}

void Button::GamepadButtonAction::OnStateChange(bool state)
{
    // pass the event to the gamepad button device handler
    gamepad.ButtonEvent(buttonNum, state);
}

// ---------------------------------------------------------------------------
//
// Gamepad hat switch action
//

void Button::GamepadHatSwitchAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    // gamepad action
    desc->actionType = PinscapePico::ButtonDesc::ACTION_GAMEPAD;

    // Our buttonNum is 1..4 for UP DOWN LEFT RIGHT.  The actionDetail in
    // the report labels them in the same order, but starting at 100.
    desc->actionDetail = buttonNum + 99;
}

void Button::GamepadHatSwitchAction::OnStateChange(bool state)
{
    // pass the event to the gamepad hat switch device handler
    gamepad.HatSwitchEvent(buttonNum, state);
}

// ---------------------------------------------------------------------------
//
// XInput (xbox controller emulation) button action
//

void Button::XInputButtonAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    desc->actionType = PinscapePico::ButtonDesc::ACTION_XINPUT;
    desc->actionDetail = buttonNum;
}

void Button::XInputButtonAction::OnStateChange(bool state)
{
    // pass the event to the gamepad button device handler
    xInput.SetButton(buttonNum, state);
}

// ---------------------------------------------------------------------------
//
// Open Pinball Device generic button action
//

void Button::OpenPinDevGenericButtonAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    // action detail 1..32 corresponds to our generic buttons 1..32
    desc->actionType = PinscapePico::ButtonDesc::ACTION_OPENPINDEV;
    desc->actionDetail = buttonNum;
}

void Button::OpenPinDevGenericButtonAction::OnStateChange(bool state)
{
    // pass the event to the Open Pinball Device button device handler
    openPinballDevice.GenericButtonEvent(buttonNum, state);
}

// ---------------------------------------------------------------------------
//
// Open Pinball Device pre-defined pinball button action
//

void Button::OpenPinDevPinballButtonAction::PopulateDesc(PinscapePico::ButtonDesc *desc) const
{
    // action detail 33..64 corresponds to pinball buttons 0..31, so add 33 to our internal index
    desc->actionType = PinscapePico::ButtonDesc::ACTION_OPENPINDEV;
    desc->actionDetail = buttonNum + 33;
}

void Button::OpenPinDevPinballButtonAction::OnStateChange(bool state)
{
    // pass the event to the Open Pinball Device button device handler
    openPinballDevice.PinballButtonEvent(buttonNum, state);
}

// ---------------------------------------------------------------------------
//
// Night Mode action
//

void Button::NightModeAction::OnStateChange(bool state)
{
    // Set the global Night Mode status to the new state
    nightModeControl.Set(state);
}

// ---------------------------------------------------------------------------
//
// Plunger Calibration Mode buton
//

void Button::PlungerCalAction::OnStateChange(bool state)
{
    // Set the plunger's virtual calibration button
    plunger.PushCalButton(state);
}

// ---------------------------------------------------------------------------
//
// IR action
//

void Button::IRAction::OnStateChange(bool state)
{
    // set our internal state variable
    this->state = state;
    
    // Send the IR command when an Off -> On transition occurs.  If
    // auto-repeat is enabled, pass a pointer to our internal state
    // variable, so that the transmitter will auto-repeat as long as the
    // logical control stays on.
    if (state)
        irTransmitter.QueueCommand(cmd, 1, autoRepeatEnabled ? &this->state : nullptr);
}

// ---------------------------------------------------------------------------
//
// Reset action
//

void Button::ResetAction::OnStateChange(bool state)
{
    // if we're switching from OFF to ON, note the start of the new hold time
    if (state && !lastState)
        t0 = time_us_64();

    // remember the new state
    lastState = state;
}

void Button::ResetAction::Task()
{
    // if the button is ON and we've reached the hold time, execute the reset
    if (lastState && time_us_64() - t0 > holdTime_us)
        picoReset.Reboot(bootLoaderMode);
}

// ---------------------------------------------------------------------------
//
// Macro action
//

void Button::MacroAction::OnStateChange(bool state)
{
    // check the new state
    if (state)
    {
        // off -> on - activate the macro if it's not alerady running
        if (!isRunning)
            StartMacro(state);
    }
    else
    {
        // on -> off - if we're not in run-to-completion mode, cancel the macro
        if (isRunning && !runToCompletion)
            StopMacro(state);
    }

    // remember the new source state
    sourceState = state;
}

void Button::MacroAction::Task()
{
    // if we're running, schedule steps
    if (isRunning)
    {
        // run the scheduler
        ScheduleSteps(sourceState);

        // if we're no longer running, and the underlying source is still
        // activated, and the macro repeats, start up again
        if (!isRunning && sourceState && repeat)
            StartMacro(true);
    }

    // Now go through all of the steps to execute action tasks for steps
    // that are currently activated.
    for (auto &step : steps)
        step.action->Task();
}

void Button::MacroAction::StartMacro(bool newSourceState)
{
    // we're now running
    isRunning = true;
    
    // set the start time
    tStart = time_us_64();

    // Set the next event time to the start time.  There isn't necessarily
    // an event ready to run, since the first event could have a startup
    // delay, but the scheduler will figure that out when it scans the
    // tasks.
    tNext = tStart;
    
    // Run the scheduler
    ScheduleSteps(newSourceState);
}

void Button::MacroAction::StopMacro(bool newSourceState)
{
    // no longer running
    isRunning = false;

    // cancel any active steps
    for (auto &step : steps)
    {
        if (step.state)
        {
            step.state = false;
            step.action->OnStateChange(false);
        }
    }
}

void Button::MacroAction::ScheduleSteps(bool newSourceState)
{
    // check to see if we've reached the next start time
    uint64_t now = time_us_64();
    uint32_t dt = static_cast<uint32_t>(now - tStart);
    if (tNext != 0 && now >= tNext)
    {
        // If we're late, adjust the overall macro start time by the
        // amount of lateness.  This ensures that subsequent steps
        // aren't short-changed on their duration.  This errs on the
        // side of elongating steps, which seems better than the
        // alternative of shortening them, since a step that's too
        // short might never send a USB event if it misses its
        // polling cycle.
        uint32_t lateness = static_cast<uint32_t>(now - tNext);
        tStart += lateness;
        dt -= lateness;

        // Start all of the events that are scheduled at this offset
        // from the macro start time
        for (auto &step : steps)
        {
            if (step.tStart == dt && !step.state)
            {
                step.state = true;
                step.action->OnStateChange(true);
            }
        }

        // find the next start time - this is the start time of the
        // next action with a start time that's still in the future
        tNext = 0;
        for (auto &step : steps)
        {
            if (step.tStart > dt)
            {
                tNext = tStart + step.tStart;
                break;
            }
        }
    }

    // Check all of the running actions to see if we've reached
    // their end times
    int nRunning = 0;
    for (auto &step : steps)
    {
        // if this step is still running, check it for completion
        if (step.state)
        {
            // If it's a duration:"hold" step, it completes when the button is released.
            // Otherwise, it completes when the elapsed time reaches its ending time (tEnd).
            if (step.hold ? !newSourceState : dt >= step.tEnd)
            {
                // duration expired - end the step and switch off its action
                step.state = false;
                step.action->OnStateChange(false);
            }
            else
            {
                // it's still running
                ++nRunning;
            }
        }
    }

    // If there's no next step awaiting scheduling, and no steps are
    // currently running, the macro is done
    if (tNext == 0 && nRunning == 0)
        isRunning = false;
}
