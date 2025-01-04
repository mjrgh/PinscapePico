// Pinscape Pico - XInput (XBox controller emulator) USB device
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a USB class that emulates an XBox controller.  Pinscape can
// use this to send button, nudge, and plunger readings to the PC, as an
// alternative to keyboard and HID gamepad input.  The XBox interface
// has roughly the same features as the HID gamepad, in that it provides
// a set of joystick axes and buttons that can be mapped to Pinscape
// inputs.  The main reason to choose XInput over the HID gamepad, or
// vide versa, is to maximize compatibility with the particular set of
// game software you use regularly.  Some games have better support for
// HID gamepads, others work better with XBox controllers.  In addition,
// a few commercial games have been reported to crash or otherwise
// misbehave if a HID gamepad is merely connected to the system, even if
// you're not trying to use it for input to that game.  If you encounter
// anything like that within your collection of games, it might be a
// reason to use XInput exclusively.
//
// The XBox USB interface isn't a HID.  It uses a proprietary Microsoft
// vendor interface with its own device driver on the Windows side.  As
// a result, it's not quite as plug-and-play with the Pinscape device as
// a HID gamepad is.  In particular, you have to go through some manual
// steps to associate the Pinscape XInput interface with the XInput
// device driver.  You have to go through basically the same procedure
// as installing a device driver for a new USB device, but you don't
// actually have to install any new software.  You just have to tell
// Windows that the new device uses a driver that's already installed:
//
// - Make sure that XInput is enabled in the Pinscape congfiguration
//
// - Open Device Manager
//
// - In the menu, select View > Devices by Container
//
// - Open Pinscape Pico and find the interface marked with the yellow
//   warning icon; double-click it to bring up properties
//
// - Go to the Driver tab, click Update Driver
//
// - Click "Browse my computer for drivers", then "Let me pick from a
//   list of available drivers", then "XBox 360 Controller for Windows"
//
// - Click through the confirmation screens.  Windows might warn that
//   this is the wrong type of device for the driver.  That's because
//   the Pinscape device doesn't use the USB identifiers (VID/PID) as a
//   physical XBox controller unit, which is how the driver normally
//   recognizes the device.  That's the whole reason we have to go
//   through this manual process.
//
// This implementation is based on some reverse-engineering work on the
// XBox controller that I found on the Web:
//
// https://www.partsnotincluded.com/how-to-emulate-an-xbox-controller-with-arduino-xinput/
// https://www.partsnotincluded.com/xbox-360-controller-led-animations-info/

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Nudge.h"
#include "ButtonHelper.h"

// external/forward declarations
class JSONParser;
class LogicalAxis;
class ConsoleCommandContext;


// XInput - xbox controller emulator USB handler
class XInput
{
public:
    XInput();

    // console 'xinput' command - temporary configuration changes
    static void Command_xinput(const ConsoleCommandContext *ctx);

    // configure; returns true if enabled
    bool Configure(JSONParser &json);

    // initialize the USB connection
    bool Init(uint8_t rhport);

    // periodic task processing
    void Task();

    // Get my assigned player index.  This is the Player Number, 0-3, that
    // the host assigned to this unit.  We infer this from the LED animation
    // comamnds we reeive, since the LED normally shows the player number
    // as the lit quadrant.  Returns -1 if we don't know the playe rnumber.
    int GetPlayerNumber() const { return playerIndex; }

    // Is the XInput USB interface enabled?  If this is false, the XInput device
    // won't be included in the USB composite device during connection setup.
    bool enabled = false;

    // Are XInput device-to-host status reports currently enabled?  When
    // this is false, we won't send any reports to the host.  This can
    // be used to temporarily suspend input without actually removing
    // the XInput interface from the USB connection (which requires a
    // full reset).  This might be useful while running a non-pinball
    // game or other program that can't be configured to ignore the
    // input, since the accelerometer tends to generate continuous input
    // that a non-pinball program would interpret as constant joystick
    // motion.
    bool reportsEnabled = true;

    // device-to host report send time and completion time (microseconds on system clock)
    uint64_t tSendStart = 0;
    uint64_t tSendComplete = 0;
    
    // report time statistics
    uint64_t nSends = 0;
    uint64_t totalCompletionTime = 0;
    
    // log the start of a report transmission
    void LogSendStart(uint64_t t) { tSendStart = t; }
    
    // log the completion of a report transmission
    void LogSendComplete(uint64_t t)
    {
        // log the completion time
        tSendComplete = t;
        
        // collect total send time statistics
        totalCompletionTime += tSendComplete - tSendStart;
        nSends += 1;
    }
    
    // OUT endpoint buffer
    uint8_t epOutBuf[64];

    // XInput input report format (device-to-host).
    //
    // Note that the internal byte-level layout of this struct MUST exactly match
    // the message format sent by authentic xbox controller units.  The Windows
    // host will interpret the bytes we send using the original controller format,
    // and doesn't know or care what WE think the struct looks like.  We only
    // define this struct as a notational convenience for constructing the reports,
    // so that we can refer to the contents more mnemonically than as offsets in a
    // byte array.  But the byte array intepretation is controlling; we have to
    // conform the struct to the byte array layout.  That's why this must be a
    // "packed" struct, and must use definite-size int types for all members.
    struct __attribute__((packed)) InReport
    {
        uint8_t messageType = 0x00;     // message type - 0 for control surface report
        uint8_t length = 0x14;          // message length in bytes
        uint16_t buttons = 0;           // button bits (high bit to low bit):
                                        // (low byte)   R3 L3 Back, Start, DPad Right, DPad Left, DPad Down, DPad Up
                                        // (high byte)  Y X B A unused XBOX RB LB
        uint8_t leftTrigger = 0;        // left trigger analog position, 0 (released) to 255 (fully depressed)
        uint8_t rightTrigger = 0;       // right trigger analog position, 0 (released) to 255 (fully depressed)
        int16_t xLeft = 0;              // left joystick X axis analog position, -32767 (west) to +32767 (east)
        int16_t yLeft = 0;              // left joystick Y axis analog position, -32767 (south) to +32767 (north)
        int16_t xRight = 0;             // right joystick X axis analog position, -32767 (west) to +32767 (east)
        int16_t yRight = 0;             // right joystick Y axis analog position, -32767 (south) to +32767 (north)

        uint8_t reserved[6] { 0, 0, 0, 0, 0, 0 };  // reserved/unused

        bool operator==(const InReport &other) { return memcmp(this, &other, sizeof(*this)) == 0; }
        bool operator!=(const InReport &other) { return memcmp(this, &other, sizeof(*this)) != 0; }
    };

    // as a sanity check, make sure that the report struct size matches
    // the USB packet size of 20 bytes
    static_assert(sizeof(InReport) == 20);

    // XInput "rumble" feedback output report format (host-to-device)
    struct __attribute__((packed)) RumbleReport
    {
        uint8_t messageType;            // message type - 0 for rumble data out
        uint8_t length;                 // message length in bytes - 0x08 for rumble data out
        uint8_t reserved1;              // unused/reserved
        uint8_t leftRumble;             // left rumble value, 0 (off) to 255 (maximum intensity)
        uint8_t rightRumble;            // right rumble value, 0 (off) to 255 (maximum intensity)
        uint8_t reserved2[3];           // unused/reserved
    };

    // XInput LED feedback output report format (host-to-device)
    struct __attribute__((packed)) LEDReport
    {
        uint8_t messageType;            // message type - 1 for LED data out
        uint8_t length;                 // message length in bytes - 0x03 for LED data out
        uint8_t led;                    // LED animation number
    };

    // Trigger and joystick axis sources
    LogicalAxis *leftTriggerSource;
    LogicalAxis *rightTriggerSource;
    LogicalAxis *xLeftSource;
    LogicalAxis *yLeftSource;
    LogicalAxis *xRightSource;
    LogicalAxis *yRightSource;

    // Nudge device averaging viewer
    NudgeDevice::View *nudgeDeviceView = nullptr;

    // Button identifiers - these are the bit positions in the buttons uint16_t
    static const int DPAD_UP = 0;
    static const int DPAD_DOWN = 1;
    static const int DPAD_LEFT = 2;
    static const int DPAD_RIGHT = 3;
    static const int DPAD_START = 4;
    static const int DPAD_BACK = 5;
    static const int DPAD_L3 = 6;
    static const int DPAD_R3 = 7;
    static const int BTN_LB = 8;
    static const int BTN_RB = 9;
    static const int BTN_XBOX = 10;
    static const int BTN_A = 12;
    static const int BTN_B = 13;
    static const int BTN_X = 14;
    static const int BTN_Y = 15;

    // Get/set button state, by button ID
    bool GetButton(int id) const { return buttons.live & (1 << id); }
    void SetButton(int id, bool state);

    // Individual button methods
    bool GetDPadUp() { return GetButton(DPAD_UP); }
    bool GetDpadDown() { return GetButton(DPAD_DOWN); }
    bool GetDpadStart() { return GetButton(DPAD_START); }
    bool GetDpadBack() { return GetButton(DPAD_BACK); }
    bool GetDadL3() { return GetButton(DPAD_L3); }
    bool GetDPadR3() { return GetButton(DPAD_R3); }
    bool GetButtonLB() { return GetButton(BTN_LB); }
    bool GetBubtonRB() { return GetButton(BTN_RB); }
    bool GetButtonXBOX() { return GetButton(BTN_XBOX); }
    bool GetButtonA() { return GetButton(BTN_A); }
    bool GetButtonB() { return GetButton(BTN_B); }
    bool GetButtonX() { return GetButton(BTN_X); }
    bool GetButtonY() { return GetButton(BTN_Y); }

    void SetDPadUp(bool state) { SetButton(DPAD_UP, state); }
    void SetDpadDown(bool state) { SetButton(DPAD_DOWN, state); }
    void SetDpadStart(bool state) { SetButton(DPAD_START, state); }
    void SetDpadBack(bool state) { SetButton(DPAD_BACK, state); }
    void SetDadL3(bool state) { SetButton(DPAD_L3, state); }
    void SetDPadR3(bool state) { SetButton(DPAD_R3, state); }
    void SetButtonLB(bool state) { SetButton(BTN_LB, state); }
    void SetBubtonRB(bool state) { SetButton(BTN_RB, state); }
    void SetButtonXBOX(bool state) { SetButton(BTN_XBOX, state); }
    void SetButtonA(bool state) { SetButton(BTN_A, state); }
    void SetButtonB(bool state) { SetButton(BTN_B, state); }
    void SetButtonX(bool state) { SetButton(BTN_X, state); }
    void SetButtonY(bool state) { SetButton(BTN_Y, state); }

    // Button helper
    ButtonHelper buttons;
    
    // Previous input report.  Each time we send a report, we store a
    // copy here.  This lets us diff each new report against the
    // previous report to determine if it's necessary to send a report
    // at all, to reduce unnecessary USB traffic.
    InReport prevReport;

    // Handle an XInput OUT reports (host to device)
    void OnRumbleReport(const RumbleReport *report);
    void OnLEDReport(const LEDReport *report);

    // Handle an XInput transfer completion event (in or out)
    bool OnXfer(uint8_t rhport, uint8_t epAddr, xfer_result_t result, uint32_t nXferBytes);

    // Tinyusb custom class driver callbacks.  Tinyusb uses a C
    // interface, so we have to define these as statics.
    struct Driver
    {
        static void Init();
        static bool DeInit();
        static void Reset(uint8_t rhport);
        static uint16_t Open(uint8_t rhport, const tusb_desc_interface_t *itfDesc, uint16_t maxLen);
        static bool ControlXfer(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request);
        static bool Xfer(uint8_t rhport, uint8_t epAddr, xfer_result_t result, uint32_t nXferBytes);
    };

    // Animation sequence codes
    // (from https://www.partsnotincluded.com/xbox-360-controller-led-animations-info/)
    //
    // The physical xbox controller has four small LEDs positioned in a circle
    // around the central "xbox" button.  The LEDs are used locally (without USB
    // commands being sent) for button press events and to show connection status
    // (a chasing-circle animation plays while connecting).  The USB host can also
    // trigger its own animations by sending this report, with the code in the 'led'
    // field specifying which animation to play:
    // 
    // 0x00	 All off
    // 0x01	 All blinking (300ms on, 300ms off)
    // 0x02	 #1 flashes, then on
    // 0x03	 #2 flashes, then on
    // 0x04	 #3 flashes, then on
    // 0x05	 #4 flashes, then on
    // 0x06	 #1 on
    // 0x07	 #2 on
    // 0x08	 #3 on
    // 0x09	 #4 on
    // 0x0A	 Rotating (#1 - #2 - #4 - #3)
    // 0x0B	 Blinking (300ms on, 300ms off, 4 cycles)*
    // 0x0C	 Slow blinking (300ms on/700ms off, 16 cycles)*
    // 0x0D	 Alternating (#1+#4 - #2+#3)*
    //
    // * Starred animations are temporary; the previous animation resumes
    // after the new one finishes the pattern
    enum class Animation
    {
        None = -1,             // none/invalid
        AllOff = 0x00,         // All off
        AllBlink = 0x01,       // All blinking, 300ms on, 300ms off
        Flash1 = 0x02,         // #1 flashes 3 times at 300ms/300ms, then on
        Flash2 = 0x03,         // #2 flashes 3 times at 300ms/300ms, then on
        Flash3 = 0x04,         // #3 flashes 3 times at 300ms/300ms, then on
        Flash4 = 0x05,         // #4 flashes 3 times at 300ms/300ms, then on
        On1 = 0x06,            // #1 on
        On2 = 0x07,            // #2 on
        On3 = 0x08,            // #3 on
        On4 = 0x09,            // #4 on
        Rotate = 0x0A,         // Rotating (#1 - #2 - #3 - #4), each on for 100ms
        Blink = 0x0B,          // All blinking, 300ms on, 300ms, 4 cycles, then resume previous
        SlowBlink = 0x0C,      // Slow blinking, 300ms on, 700ms off, 16 cycles, then resume previous
        Alternate = 0x0D,      // Alternating, #1+#4 / #2+#3, 7 cycles, then resume previous
    };

    // set the animation state
    void SetAnimation(Animation anim);

    // pop the previous animation after a temporary animation ends
    void PopAnimation();

    // current animation
    Animation animation = Animation::AllOff;

    // previous animation - the previous animation, restored when a temporary
    // animation (Blink, SlowBlink, Alternate) ends
    Animation prevAnimation = Animation::AllOff;
    
    // current animation start time, for calculating blink phase
    uint64_t tAnimationStart = 0;

    // Current LED states.  These are updated in the Task() routine based on
    // the current animation state and time.
    bool led[4] { false, false, false, false };

    // Current rumble states
    uint8_t leftRumble = 0;
    uint8_t rightRumble = 0;

    // update the current animation state
    void UpdateAnimation();

    // User Index.  The user index is essentially the Player Number for
    // this unit when playing a game.  The user index is assigned by the
    // hsot.  There doesn't appear to be a formal way for the protocol
    // to set the user index on the device (there's no reason for the
    // real controllers to know this information).  But we can infer it
    // from the LED animations, because the host always sets the LED to
    // show the player number via which LED quadrant is lit.  So if the
    // host sends animations Flash1 + On1, that means we're Player 1.
    // That's the state between games, anyway.  Once a game is started,
    // it probably can send other animations for its own purposes.
    // But we'll just assume that the latest FlashN + OnN sequence is
    // the player number indication.
    int playerIndex = 0;
    bool playerIndexSetByHost = false;
};

// global singleton
extern XInput xInput;
