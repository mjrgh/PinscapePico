// Open Pinball Device HID Report structure
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdint.h>

// Open Pinball Device input report v1.0
//
// This is the C/C++-friendly format of the structure, with all fields represented
// with native types, using the local native integer format, for direct access in
// application code.  
//
// The "wire format" of the struct that's transmitted in the USB packets the device
// sends is NOMINALLY the same, with the fields in the same order and using the same
// fixed-bit-width integer types, but it has two important potential differences from
// the local native C/C++ struct format:
//
//  - In the USB packets, there's explicitly no padding.  When you run this struct
//    through your local C/C++ compiler, though, the compiler might add its own
//    hidden padding between fields to meet local alignment requirements.
//
//  - In the USB packets, all integer fields are in little-endian format.  The
//    compiled C/C++ version of the struct will always use the local native integer
//    format.
//
// Because of these potential differences, portable code should never assume that the
// USB format and the local struct format are interchangeable, EVEN IF they happen to
// match on a particular platform, because they might not match on other platforms.
// The following rules should therefore be observed:
//
//  - A C/C++ variable or memory buffer containing the USB packet should be declared
//    as an array of bytes (uint8_t) of the size of the wire packet format, as in
//    uint8_t usb_buffer[OPENPINDEVREPORT_USB_SIZE]
//
//  - Never memcpy() between the USB byte array and the struct type
//
//  - Never cast pointers between the USB byte array type and the struct type
//
//  - Never assume that OPENPINDEV_STRUCT_USB_SIZE == sizeof(OpenPinballDeviceReport)
//
//  - When copying an integer field from the USB packet to the local struct, construct
//    the integer value by interpreting the USB bytes in little-endian order.
//
//  - When copying an integer field from the local struct to the USB packet format,
//    deconstruct the integer value into the little-endian byte format.
//
struct OpenPinballDeviceReport
{
    uint64_t timestamp;            // timestamp, microseconds since an arbitrary zero point
    uint32_t genericButtons;       // button states for 32 user-assigned on/off buttons
    uint32_t pinballButtons;       // button states for pre-defined pinball simulator function buttons
    int16_t axNudge;               // instantaneous nudge acceleration, X axis (left/right)
    int16_t ayNudge;               // instantaneous nudge acceleration, Y axis (front/back)
    int16_t vxNudge;               // instantaneous nudge velocity, X axis
    int16_t vyNudge;               // instantaneous nudge velocity, Y axis
    int16_t plungerPos;            // current plunger position
    int16_t plungerSpeed;          // instantaneous plunger speed
};

// USB packet size of the report.  Note that this might not equal sizeof(OpenPinballDeviceReport),
// due to the possible differences between the native struct layout and the packed format of
// the USB representation.
#define OPENPINDEV_STRUCT_USB_SIZE  28

// Usage String Descriptor text
#define OPENPINDEV_STRUCT_STRDESC   "OpenPinballDeviceStruct/1.0"
#define OPENPINDEV_STRUCT_LSTRDESC  L"OpenPinballDeviceStruct/1.0"

// pinballButtons indices.  Each button's bit mask can be calculated as (1 << index).
#define OPENPINDEV_BTN_START         0      // start button
#define OPENPINDEV_BTN_EXIT          1      // Exit (end game)
#define OPENPINDEV_BTN_EXTRABALL     2      // Extra Ball/Buy-In
#define OPENPINDEV_BTN_COIN1         3      // Coin 1 (left coin chute)
#define OPENPINDEV_BTN_COIN2         4      // Coin 2 (middle coin chute)
#define OPENPINDEV_BTN_COIN3         5      // Coin 3 (right coin chute)
#define OPENPINDEV_BTN_COIN4         6      // Coin 4 (fourth coin chute/dollar bill acceptor)
#define OPENPINDEV_BTN_LAUNCH        7      // Launch Ball
#define OPENPINDEV_BTN_FIRE          8      // Fire button (lock bar top button)
#define OPENPINDEV_BTN_LL_FLIPPER    9      // Left flipper button switch; first switch in double switch stack, actuates lower left flipper
#define OPENPINDEV_BTN_LR_FLIPPER   10      // Right flipper button switch; first switch in double switch stack, actuates lower left flipper
#define OPENPINDEV_BTN_UL_FLIPPER   11      // Second switch in double switch stack for left flipper button, actuates upper left flipper
#define OPENPINDEV_BTN_UR_FLIPPER   12      // Second switch in double switch stack for right flipper button, actuates upper right flipper
#define OPENPINDEV_BTN_LEFTMAGNA    13      // MagnaSave left
#define OPENPINDEV_BTN_RIGHTMAGNA   14      // MagnaSave right
#define OPENPINDEV_BTN_TILTBOB      15      // Tilt bob
#define OPENPINDEV_BTN_SLAMTILT     16      // Slam tilt
#define OPENPINDEV_BTN_COINDOOR     17      // Coin door switch
#define OPENPINDEV_BTN_SVC_CANCEL   18      // Service panel Cancel/Escape
#define OPENPINDEV_BTN_SVC_DOWN     19      // Service panel Down/-
#define OPENPINDEV_BTN_SVC_UP       20      // Service panel Up/+
#define OPENPINDEV_BTN_SVC_ENTER    21      // Service panel Select/Enter
#define OPENPINDEV_BTN_LEFTNUDGE    22      // Left Nudge
#define OPENPINDEV_BTN_FWDNUDGE     23      // Forward Nudge
#define OPENPINDEV_BTN_RIGHTNUDGE   24      // Right Nudge
#define OPENPINDEV_BTN_VOL_UP       25      // Audio volume up
#define OPENPINDEV_BTN_VOL_DOWN     26      // Audio volume down
