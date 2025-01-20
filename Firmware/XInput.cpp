// Pinscape Pico - XInput (XBox controller emulator) USB device
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <device/usbd_pvt.h>

// local project headers
#include "USBIfc.h"
#include "XInput.h"
#include "Logger.h"
#include "Nudge.h"
#include "CommandConsole.h"


// global XInput singleton
XInput xInput;

XInput::XInput() :
    leftTriggerSource(&nullAxisSource),
    rightTriggerSource(&nullAxisSource),
    xLeftSource(&nullAxisSource),
    yLeftSource(&nullAxisSource),
    xRightSource(&nullAxisSource),
    yRightSource(&nullAxisSource)
{
    CommandConsole::AddCommand(
        "xinput", "XBox controller emulation options",
        "xinput [option...]\n"
        "  -d, --disable         disable sending XInput reports to host\n"
        "  -e, --enable          enable sending XInput reports to host\n"
        "  -s, --status          show status (default if no options are supplied)\n"
        "\n"
        "Note: all options are for the current session; the saved configuration isn't affected.",
        &Command_xinput);
}

// console 'gamepad' command handler
void XInput::Command_xinput(const ConsoleCommandContext *c)
{
    static const auto Status = [](const ConsoleCommandContext *c) {
        c->Printf(
            "XInput status:\n"
            "  Configured:       %s\n"
            "  Player Index:     %d%s\n"
            "  Reporting:        %s\n"
            "  Num reports sent: %llu started, %llu completed\n"
            "  Avg report time:  %.2f ms\n",
            xInput.enabled ? "Yes" : "No",
            xInput.playerIndex, (xInput.playerIndex < 0 ? " (Not assigned)" : ""),
            xInput.reportsEnabled ? "Enabled" : "Disabled",
            xInput.nSendsStarted, xInput.nSendsCompleted,
            xInput.nSendsCompleted == 0 ? 0.0f : static_cast<float>(xInput.totalCompletionTime / xInput.nSendsCompleted) / 1000.0f);
    };

    if (c->argc == 1)
        return Status(c);

    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *s = c->argv[i];
        if (strcmp(s, "-d") == 0 || strcmp(s, "--disable") == 0)
        {
            xInput.reportsEnabled = false;
            c->Print("XInput reports disabled\n");
        }
        else if (strcmp(s, "-e") == 0 || strcmp(s, "--enable") == 0)
        {
            if (xInput.enabled)
            {
                xInput.reportsEnabled = true;
                c->Print("XInput reports enabled\n");
            }
            else
                c->Printf("XInput isn't configured - reports can't be enabled\n");
        }
        else if (strcmp(s, "-s") == 0 || strcmp(s, "--status") == 0)
        {
            Status(c);
        }
        else
        {
            c->Printf("Invalid option \"%s\"\n", s);
            return;
        }
    }
}

bool XInput::Configure(JSONParser &json)
{
    // presume disabled if not configured
    bool enabled = false;

    // check for the main XInput JSON key
    if (auto *val = json.Get("xInput") ; !val->IsUndefined())
    {
        // get the enable status
        enabled = val->Get("enable")->Bool();

        // get the axis settings
        LogicalAxis::CtorParams params{ "xInput", &nudgeDeviceView };
        xLeftSource = LogicalAxis::Configure(params, val, "xLeft");
        yLeftSource = LogicalAxis::Configure(params, val, "yLeft");
        xRightSource = LogicalAxis::Configure(params, val, "xRight");
        yRightSource = LogicalAxis::Configure(params, val, "yRight");
        leftTriggerSource = LogicalAxis::Configure(params, val, "leftTrigger");
        rightTriggerSource = LogicalAxis::Configure(params, val, "rightTrigger");
    }

    // return the 'enabled' status
    return enabled;
}

// periodic task processing
void XInput::Task()
{
    // Send input to the host if the IN endpoint is ready
    uint64_t t = time_us_64();
    if (enabled
        && reportsEnabled
        && !usbIfc.IsSuspended()
        && tud_ready() && !usbd_edpt_busy(0, USBIfc::EndpointInXInput))
    {
        // take a snapshot of the accelerometer state
        nudgeDeviceView->TakeSnapshot();
        
        // Construct the new report.  Set the buttons to the next button
        // state under construction, and load all of the axis controls
        // from their logical sources.
        InReport report;
        report.buttons = static_cast<uint16_t>(buttons.Report());
        report.leftTrigger = leftTriggerSource->ReadUI8();
        report.rightTrigger = rightTriggerSource->ReadUI8();
        report.xLeft = xLeftSource->Read();
        report.yLeft = yLeftSource->Read();
        report.xRight = xRightSource->Read();
        report.yRight = yRightSource->Read();

        // send the report only if it differs from the previous report
        if (report != prevReport)
        {
            // save the report to compare on the next round
            prevReport = report;

            // log the report start time
            LogSendStart(t);

            // claim the endpoint and start the transfer
            usbd_edpt_claim(0, USBIfc::EndpointInXInput);
            usbd_edpt_xfer(0, USBIfc::EndpointInXInput, reinterpret_cast<uint8_t*>(&report), sizeof(report));
            usbd_edpt_release(0, USBIfc::EndpointInXInput);
        }
    }

    // update the animation state
    UpdateAnimation();
}


// Handle an incoming rumble feedback report on our XInput interface
void XInput::OnRumbleReport(const RumbleReport *report)
{
    Log(LOG_XINPUT, "XInput rumble: left=%d, right=%d\n", report->leftRumble, report->rightRumble);
    leftRumble = report->leftRumble;
    rightRumble = report->rightRumble;
}

// Handle an incoming LED feedback report on our XInput interface
void XInput::OnLEDReport(const LEDReport *report)
{
    Log(LOG_XINPUT, "XInput LED: animation #%d\n", report->led);
    SetAnimation(static_cast<Animation>(report->led));
}

// Initialize
bool XInput::Init(uint8_t rhport)
{
    return usbd_edpt_xfer(rhport, USBIfc::EndpointOutXInput, epOutBuf, sizeof(epOutBuf));
}

// Handle a transfer completion event
bool XInput::OnXfer(uint8_t rhport, uint8_t epAddr, xfer_result_t result, uint32_t nXferBytes)
{
    // check which direction we're going
    if (epAddr == USBIfc::EndpointInXInput)
    {
        // input endpoint - send completed; note the completion time
        LogSendComplete(time_us_64());
    }
    else
    {
        // Output endpoint - receiving a report.  The xbox output reports
        // all use the first byte to indicate the message type, and the second
        // byte to indicate the message size in bytes.
        const uint8_t *msg = epOutBuf;
        uint8_t msgType = msg[0];
        uint8_t msgLen = msg[1];
        switch (msgType)
        {
        case 0:
            // Rumble feedback control - should be an 8-byte message
            if (msgLen == 8)
                OnRumbleReport(reinterpret_cast<const RumbleReport*>(msg));
            break;

        case 1:
            // LED animation control - should be 3 bytes
            if (msgLen == 3)
                OnLEDReport(reinterpret_cast<const LEDReport*>(msg));
            break;

        default:
            // unknown report type
            {
                char buf[64 + 64*3 + 1 + 1];
                char *p1 = buf;
                char *p2 = buf + nXferBytes + 1;
                for (unsigned int i = 0 ; i < nXferBytes && i < 64 ; ++i)
                {
                    uint8_t b = msg[i];
                    *p1++ = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                    uint8_t bh = (b >> 4) & 0x0f, bl = b & 0x0f;
                    *p2++ = static_cast<char>(bh < 10 ? bh + '0' : bh + 'A' - 10);
                    *p2++ = static_cast<char>(bl < 10 ? bl + '0' : bl + 'A' - 10);
                }
                *p1++ = ' ';
                *p2 = 0;
                Log(LOG_XINPUT, "Unknown XInput OUT report type %02x: %s\n", msgType, buf);
            }
            break;
        }

        // set up the next transfer
        if (!usbd_edpt_xfer(rhport, epAddr, epOutBuf, sizeof(epOutBuf)))
            return false;
    }

    // okay
    return true;
}


// Set button state
void XInput::SetButton(int id, bool state)
{
    // note that we number buttons from 0, whereas ButtonHelper numbers them from 1
    buttons.OnButtonEvent(id + 1, state);
}

// Set the new animation state
void XInput::SetAnimation(Animation newAnim)
{
    // If we don't know the player number yet, check for a FlashN + OnN sequence.
    // The Microsoft XInput documentation says that the player is assigned when
    // the device is plugged into the USB port, and never changes.  So we'll
    // assume that the first Flash+On sequence that we see tells us the player
    // number for the duration of the session.  This might be important if some
    // game later sends us other animations to display purely for visual effect.
    // I'm not sure to what extent the interface allows games to play back
    // custom effects on the LEDs; they might be restricted to system-level use,
    // only for visually identifying the controller to the player.  But it seems
    // best to assume that games might also be allowed to send custom animations,
    // so we'll take the first implied player ID as the definitive one for the
    // whole session, taking the documentation at face value that it won't change.
    // It seems likely that even if games can send animations, the device driver
    // will always override that during the initial setup when a controller is
    // plugged in, ensuring that we'll always get a player indicator first.
    if (!playerIndexSetByHost)
    {
        playerIndexSetByHost = true;
        if (newAnim == Animation::On1 && prevAnimation == Animation::Flash1)
            playerIndex = 0;
        else if (newAnim == Animation::On2 && prevAnimation == Animation::Flash2)
            playerIndex = 1;
        else if (newAnim == Animation::On3 && prevAnimation == Animation::Flash3)
            playerIndex = 2;
        else if (newAnim == Animation::On4 && prevAnimation == Animation::Flash4)
            playerIndex = 4;
        else
            playerIndexSetByHost = false;

        if (playerIndexSetByHost)
            Log(LOG_XINPUT, "Ready Player %d: XInput player assignment (player index %d) detected from LED animation\n",
                playerIndex + 1, playerIndex);
    }
    
    // If the current animation isn't one of the temporary animations,
    // save it as the previous state.  Don't save the current animation
    // if it's a temporary one - any new temporary animation just replaces
    // the old temporary animation without saving it.  The stack is never
    // more than one level deep.
    if (animation != Animation::Blink && animation != Animation::SlowBlink && animation != Animation::Alternate)
        prevAnimation = animation;

    // remember the new animation and start time
    animation = newAnim;
    tAnimationStart = time_us_64();
}

// Pop the previous animation after a temporary animation ends
void XInput::PopAnimation()
{
    animation = prevAnimation;
    tAnimationStart = time_us_64();
}

// Update the animation state
void XInput::UpdateAnimation()
{
    // calculate the offset from the start of the animation
    uint64_t dt = time_us_64() - tAnimationStart;
    uint32_t phase = 0;

    // update according to the animation currently in effect
    switch (animation)
    {
    case Animation::AllOff:
        // all off
        led[0] = led[1] = led[2] = led[3] = false;
        break;

    case Animation::AllBlink:
        // All blink at 600ms period/50% duty cycle.  Figure the phase in
        // the 600ms cycle; if it's in the first half, it's on, else off.
        phase = static_cast<uint32_t>(dt % 600000);
        led[0] = led[1] = led[2] = led[3] = (phase < 300000);
        break;

    case Animation::Flash1:
    case Animation::Flash2:
    case Animation::Flash3:
    case Animation::Flash4:
        // Blink at 600ms period/50% duty cycle, 3 cycles = 1800ms, then
        // on.  So: if we're past 1800ms from the start of the
        // animation, it's just solid on, otherwise figure the phase in
        // the 600ms cycle.  All others (apart from the one that's being
        // addressed) are off.
        led[0] = led[1] = led[2] = led[3] = false;
        phase = static_cast<uint32_t>(dt % 600000);
        led[static_cast<int>(animation) - static_cast<int>(Animation::Flash1)] =
            (dt > 1800000 ? true : phase < 300000);
        break;

    case Animation::On1:
    case Animation::On2:
    case Animation::On3:
    case Animation::On4:
        // the specified LED is on, all others are off
        led[0] = led[1] = led[2] = led[3] = false;
        led[static_cast<int>(animation) - static_cast<int>(Animation::On1)] = true;
        break;

    case Animation::Rotate:
        // rotate at 100ms per segment, 1-2-3-4
        phase = static_cast<uint32_t>(dt % 400000);
        led[0] = (phase < 100000);
        led[1] = (phase >= 100000 && phase < 200000);
        led[2] = (phase >= 200000 && phase < 300000);
        led[3] = (phase >= 300000);
        break;

    case Animation::Blink:
        // all blinking, 300ms on, 300ms off, 4 cycles, then resume previous
        if (dt > 4*600000)
            PopAnimation();
        else
        {
            phase = static_cast<uint32_t>(dt % 600000);
            led[0] = led[1] = led[2] = led[3] = (phase < 300000);
        }
        break;

    case Animation::SlowBlink:
        // slow blinking, 300ms on, 700ms off, 16 cycles, then resume previous
        if (dt > 16*1000000)
            PopAnimation();
        else
        {
            phase = static_cast<uint32_t>(dt % 1000000);
            led[0] = led[1] = led[2] = led[3] = (phase < 300000);
        }
        break;

    case Animation::Alternate:
        // alternating 1+4 / 2+#, 300ms each side, 7 cycles, then resume previous
        if (dt > 7*600000)
            PopAnimation();
        else
        {
            phase = static_cast<uint32_t>(dt % 600000);
            led[0] = led[3] = (phase < 300000);
            led[1] = led[2] = (phase >= 300000);
        }
        break;
    }
}

// ----------------------------------------------------------------------------
//
// Custom Tinyusb class driver for XInput.  The xbox controller uses a
// proprietary vendor interface (in other words, not one of the standard
// USB classes, such as HID, CDC, or mass storage).  Tinyusb needs a
// suitable class driver for each interface.  It provides a built-in
// class driver for vendor interfaces that are accessible on the host
// via user-mode pass-through drivers, such as WinUsb or libusb, but
// that generic vendor class driver has some rigid requirements that
// make it unsuitable for XInput.  The main showstopper is that the
// Tinyusb generic vendor class driver only supports bulk-transfer
// endpoints, whereas XInput uses interrupt endpoints (a la HID).  Since
// XInput can't use any of the standard USB class drivers, and it can't
// use the generic vendor class driver, we must resort to creating our
// own custom driver on the Tinyusb side.  (Note that the term "driver"
// makes it sound like we're talking about something you install on the
// Windows host, but in this case it's on the Pico side.  Windows comes
// with its own device driver for xbox controllers, so the Windows side
// is taken care of.  What we're defining here is a Tinyusb extension to
// cover a USB class that's not already built in to Tinyusb.)

// class driver initialization
void XInput::Driver::Init()
{
}

// deinitialization
bool XInput::Driver::DeInit()
{
    return true;
}


// class driver reset
void XInput::Driver::Reset(uint8_t /*rhport*/)
{
}

// Class driver open callback.  Tinyusb calls this for EVERY interface
// in the configuration, EVEN IF Tinyusb has its own built-in driver for
// the interface class, so we'll get a call for each HID, CDC, etc, in
// addition to the XInput interface we actually want to claim.  The idea
// is that this lets the custom application code override Tinyusb's
// built-in driver for any interface that needs special handling, even
// if the interface nominally uses one of the standard classes.  For
// this implementation, we just want to claim the XInput interface, so
// we'll filter for that and return a "not claimed" indication for the
// others.
uint16_t XInput::Driver::Open(uint8_t rhport, const tusb_desc_interface_t *itfDesc, uint16_t maxLen)
{
    // Filter for the XInput interface.  We can recognize it by its
    // interface number alone, but make doubly sure by also verifying
    // the interface class and subclass.
    if (itfDesc->bInterfaceNumber == USBIfc::IFCNUM_XINPUT
        && itfDesc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC
        && itfDesc->bInterfaceSubClass == 0x5D)
    {
        // log it for debugging
        Log(LOG_XINPUT, "XInput class driver claiming USB interface %d\n", itfDesc->bInterfaceNumber);

        // scan for endpoints
        const uint8_t *startp = reinterpret_cast<const uint8_t*>(itfDesc);
        const uint8_t *p = startp;
        for (const uint8_t *endp = p + maxLen ; p < endp ; p = tu_desc_next(p))
        {
            // filter for endpoint descriptors
            if (tu_desc_type(p) == TUSB_DESC_ENDPOINT)
            {
                // it's an endpoint descriptor - open the endpoint
                auto const *ep = reinterpret_cast<const tusb_desc_endpoint_t*>(p);
                usbd_edpt_open(rhport, ep);

                // if it's the OUT endpoint, set up the initial transfer
                if (ep->bEndpointAddress == USBIfc::EndpointOutXInput)
                    usbd_edpt_xfer(rhport, ep->bEndpointAddress, xInput.epOutBuf, sizeof(xInput.epOutBuf));
            }

            // stop if we hit a new interface descriptor
            if (p != startp && tu_desc_type(p) == TUSB_DESC_INTERFACE)
                break;
        }

        // return the size (in bytes) of the XInput descriptor group
        return static_cast<uint16_t>(p - startp);
    }

    // it's not the XInput interface - return 0 to tell Tinyusb to use the
    // appropriate built-in class driver
    return 0;
}

// XInput device control request callback
bool XInput::Driver::ControlXfer(uint8_t /*rhport*/, uint8_t /*stage*/, const tusb_control_request_t * /*request*/)
{
    return true;
}

// XInput transfer completion callback
bool XInput::Driver::Xfer(uint8_t rhport, uint8_t epAddr, xfer_result_t result, uint32_t nXferBytes)
{
    return xInput.OnXfer(rhport, epAddr, result, nXferBytes);
}
