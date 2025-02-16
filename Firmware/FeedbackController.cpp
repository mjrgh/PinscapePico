// Pinscape Pico - Feedback Device USB HID interface, for DOF and other clients
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a custom HID interface that lets the host control feedback
// devices (lights, solenoids, motors, etc) connected to the Pinscape
// Pico.
//
// This is a two-way HID interface.  Device-to-host reports send
// commands to control feedback devices and access other special
// Pinscape functions, and to make queries about the device's status.
// Host-to-device reports send back query replies.  In addition, the
// interface automatically reports incoming IR commands as they're
// received, to allow host software to implement IR remote control
// features using the physical IR receiver on the Pinscape unit.
//
// Most virtual pinball applications on Windows will access this
// interfae through DOF (Direct Output Framework), for which we'll
// provide a custom driver.  There are a few legacy applications that
// don't use DOF but have support for the LedWiz device.  Those are all
// programmed to an API implemented by a DLL (LEDWIZ.DLL) that was
// originally provided by the LedWiz's manufacturer, and for which an
// open-source replacement already exists.  We'll add support for this
// interface to the open-source replacement DLL to enable access from
// those legacy LedWiz-aware-but-not-DOF-aware programs.  Finally, we'll
// provide a C++ API library for use by any application software that
// wants to directly access the special features beyond what DOF and
// the LedWiz application interfaces expose.
//
// Although we use a HID foundation, this interface doesn't try to
// impersonate any specific type of HID device defined in the HID spec
// (keyboard, joystick, mouse, etc).  The HID spec simply doesn't define
// a suitable collection of capabilities for this type of device.  In
// fact, the functionality we expose is only marginally HID-related; I
// think it's just close enough to make this a reasonable use of HID, in
// that the feedback devices do provide user-visible audio/visual
// effects, much as a speaker system or display would (and those are
// certainly standard HID types).  The designers of HID didn't make any
// provisions for anything quite this specialized, but they did include
// the open-ended "application-defined" usages, specifically to allow
// for extended use cases like this that weren't originally envisioned
// as part of the spec, or are too narrow to justify including in a
// general-purpose standard.
//
// Note that the MAIN reason to use HID ISN'T just that we can make a
// case that we're somewhat HID-relevant; that's more of a defense for
// why this isn't an outright abuse of HID.  The REAL reason to use HID
// is that it provides the best plug-and-play experience for the user.
// Every major host operating system has a built-in HID driver that
// recognizes new HID devices just by plugging them in, without
// bothering the user with device driver installation or any other such
// nonsense.  Further, HID has good application support on the major
// OS's, and tends to seamlessly allow shared access from multiple
// applications concurrently accessing the same device.
//
// This interface is conceptually similar to the LedWiz's HID interface,
// but it's NOT LedWiz-compatible.  The original Pinscape firmware did
// provide a full LedWiz emulation at the USB HID level, but we chose
// not to include that in this newer system.  For one thing, hardly
// anyone in the virtual pinball world uses software that depends on the
// LedWiz at all any more; practically all of the software now in use is
// DOF-aware, allowing it to work with any device that DOF supports.
// For another, even for the remaining legacy LedWiz-only software, USB
// HID-level compatibility was never all that important, because all of
// the legacy LedWiz-aware software accesses the LedWiz through the DLL
// that the vendor supplies, rather than programming directly to the USB
// HID protocol.  The LedWiz manufacturer never even documented their
// protocol; the DLL was always the official programming interface.  We
// can thus make almost any device emulate an LedWiz by replacing the
// DLL with one that exposes the same API and translates to the USB
// protocol for each device.  What's more, the DLL approach makes it
// possible to overcome the fixed 32-port limit of the LedWiz USB
// protocol, since the DLL can expose a single Pinscape unit as a
// collection of virtual LedWiz units, with as many virtual units as
// needed to cover all of the Pinscape device's output ports.



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
#include "Main.h"
#include "Logger.h"
#include "XInput.h"
#include "USBCDC.h"
#include "VendorIfc.h"
#include "Config.h"
#include "JSON.h"
#include "PicoLED.h"
#include "StatusRGB.h"
#include "NightMode.h"
#include "Version.h"
#include "Outputs.h"
#include "NightMode.h"
#include "TVON.h"
#include "TimeOfDay.h"
#include "Plunger/Plunger.h"
#include "IRRemote/IRCommand.h"
#include "IRRemote/IRTransmitter.h"
#include "IRRemote/IRReceiver.h"
#include "../USBProtocol/FeedbackControllerProtocol.h"

// shorthand for types in the feedback controller protocol header
using FeedbackRequest = PinscapePico::FeedbackControllerRequest;
using FeedbackReport = PinscapePico::FeedbackControllerReport;

// HID device singleton
USBIfc::FeedbackController feedbackController;

// JSON configuration
bool USBIfc::FeedbackController::Configure(JSONParser &json)
{
    // presume enabled
    bool enabled = true;

    // check for the top-level feedbackController key
    if (auto *val = json.Get("feedbackController") ; !val->IsUndefined())
    {
        // get the enable status
        enabled = val->Get("enable")->Bool();
    }

    // return the 'enabled' status
    return enabled;
}

// initialize
void USBIfc::FeedbackController::Init()
{
    // subscribe for IR command notifications
    irReceiver.Subscribe(this);
}

// Report Descriptor
//
// Each input and output report is a 63-byte USB packet encoding a
// command (host-to-device) or query reply (device-to-host) message.
// (The message format has a fixed size of 63 bytes because we want each
// message to fit into a single USB packet, so that each packet can be
// processed independently.  The Pico uses "Full Speed" USB mode, which
// has a maximum 64-byte packet size, and one byte of each HID report
// packet is reserved for the report ID, leaving 63 bytes of payload.)
// The host sends command messages (as HID OUTPUT reports) to the device
// to perform actions such as turning feedback ports on and off and
// querying status from the device.  The device sends status and query
// reply information back to the host (as HID INPUT reports) when the
// host requests it via OUTPUT commands.  The device doesn't send any
// routine or unsolicited INPUT reports; it only sends when the host
// asks it to.
// 
// See FeedbackControllerProtoco.h for the message format.
//
const uint8_t *USBIfc::FeedbackController::GetReportDescriptor(uint16_t *byteLength)
{
    static const uint8_t desc[] = {
        HID_USAGE_PAGE (HID_USAGE_PAGE_GENERIC_DEVICE),  // usage page Generic Device (0x06)
        HID_USAGE      (0),                              // usage undefined (0x00), for our custom type
        HID_COLLECTION (HID_COLLECTION_APPLICATION),

            // Report ID
            HID_REPORT_ID     (ReportIDFeedbackController)

            // OUTPUT (host-to-device) - 63 bytes of private protocol data
            HID_REPORT_ITEM   (STRDESC_FEEDBACK_LBL, RI_LOCAL_STRING_INDEX, RI_TYPE_LOCAL, 1),  // string label for output report
            HID_USAGE         (0),               // undefined (opaque data for application-specific use)
            HID_REPORT_SIZE   (8),               // 8-bit bytes
            HID_REPORT_COUNT  (63),              // x 63
            HID_OUTPUT        (HID_ARRAY),       // output (host-to-device), array

            // INPUT (device-to-host) - 63 bytes of private protocol data
            HID_REPORT_ITEM   (STRDESC_FEEDBACK_LBL, RI_LOCAL_STRING_INDEX, RI_TYPE_LOCAL, 1),  // string label for input report
            HID_USAGE         (0),               // undefined (opaque data for application-specific use)
            HID_REPORT_SIZE   (8),               // 8-bit bytes
            HID_REPORT_COUNT  (63),              // x 63
            HID_INPUT         (HID_ARRAY),       // input (device-to-host), array

        HID_COLLECTION_END 
    };
    *byteLength = sizeof(desc);
    return desc;
}

uint16_t USBIfc::FeedbackController::GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // prepare a status report
    auto SendStatus = [](uint8_t* &p)
    {
        // STATUS report
        // <TypeCode:0x02> <Status:BYTE> <PowerSense:BYTE>
        *p++ = FeedbackReport::RPT_STATUS;

        // Flags
        uint8_t flags = 0;
        if (plunger.IsEnabled()) flags |= 0x01;          // plunger enabled
        if (plunger.IsCalibrated()) flags |= 0x02;       // plunger calibrated
        if (nightModeControl.Get()) flags |= 0x04;       // night mode
        if (timeOfDay.IsSet()) flags |= 0x08;            // time of day clock set
        if (config.IsSafeMode()) flags |= 0x10;          // booted in safe mode (0x10)
        if (!config.IsFactorySettings()) flags |= 0x20;  // configuration loaded (0x20)
        *p++ = flags;

        // TV ON state
        *p++ = static_cast<uint8_t>(tvOn.GetState());    // power sense step number (see spec)

        // Status LED color
        auto led = statusRGB.GetColor();
        *p++ = led.r;
        *p++ = led.g;
        *p++ = led.b;
    };
    
    // fill in the request buffer if there's room
    uint8_t *p = buf;
    if (type == HID_REPORT_TYPE_INPUT && reqLen >= 63)
    {
        // Check for pending input report requests.  Give first priority
        // to current one-off requests; if there are none of those, send
        // status if we're in continuous status reporting mode.
        if ((pendingInputReports & 0x00000001) != 0)
        {
            // build an ID report
            Log(LOG_DEBUGEX, "Feedback controller: sending ID report\n");
            *p++ = FeedbackReport::RPT_ID;
            
            // get the hardware ID
            pico_unique_board_id_t hwid;
            pico_get_unique_board_id(&hwid);

            // Get the unit name into a fixed-length 32-character buffer,
            // ensuring null-termination.  (The 32-character buffer limits
            // the name to 31 characters because of the null terminator.)
            const int MAX_NAME = 32;
            char name[MAX_NAME];
            memset(name, 0, MAX_NAME);
            strncpy(name, unitID.unitName.c_str(), MAX_NAME);
            name[MAX_NAME-1] = 0;
            
            // build the report arguments

            // add the unit number
            *p++ = unitID.unitNum;

            // add the unit name
            memcpy(p, name, MAX_NAME);
            p += MAX_NAME;

            // add the feedback interface version
            PutUInt16(p, FEEDBACK_CONTROL_VERSION);

            // add the Pico hardware ID
            PutBytes(p, hwid.id, 8);
            static_assert(sizeof(hwid.id) == 8);

            // add the port count
            PutUInt16(p, static_cast<uint16_t>(OutputManager::GetNumPorts()));

            // add the plunger type
            PutUInt16(p, plunger.GetSensorTypeForFeedbackReport());

            // add the LedWiz unit mask
            PutUInt16(p, unitID.ledWizUnitMask);

            // report request fulfilled
            pendingInputReports &= ~0x00000001;
        }
        else if ((pendingInputReports & 0x00000002) != 0)
        {
            // STATUS - we want to send a status report explicitly, even if we're not
            // in continuous reporting mode
            Log(LOG_DEBUGEX, "Feedback controller: sending status report\n");
            SendStatus(p);
            pendingInputReports &= ~0x00000002;
        }

        // If we didn't generate an explicitly requested report, and
        // there's an IR command buffered, report the IR command.
        if (p == buf && irCmdWrite != irCmdRead)
        {
            // IR COMMAND RECEIVED report
            // <TypeCode:0x03> <Protocol:BYTE> <ProtocolFlags:BYTE> <CommandFlags:BYTE> <CommandCode:UINT64>
            //
            // <Protocol><ProtocolFlags><CommandCode> express the
            // command in our standard universal notation (see
            // IRRemote/IRCommand.h).  The <Flags> in this part are the
            // flag bits that are part of the code specification.
            //
            // <CommandFlags> contains the concrete flags for this
            // individual command event:
            //
            //   0x01   Toggle bit is present
            //   0x02   Toggle bit (only valid if 0x01 is set)
            //   0x04   Ditto bit is present
            //   0x08   Ditto bit (only valid if 0x04 is set)
            //   0x10   } Position code:
            //   0x20   }   '00' (0x00) -> Null (no position code), '01' (0x10) -> First, '10' (0x20) -> Middle, '11' (0x30) -> Last
            //   0x40   Auto-repeat flag - the remote is auto-repeating the command
            //          (presumably because the user is holding down the button)
            //   0x80   Reserved, always zero
            Log(LOG_DEBUGEX, "Feedback controller: sending IR report\n");

            // retrieve the command and remove it from the buffer
            auto &cmdInfo = irCmdBuf[irCmdRead];
            auto &cmd = cmdInfo.cmd;
            irCmdRead = (irCmdRead + 1) % IRCmdBufSize;

            // build the Flags2 byte
            uint8_t flags2 = 0;
            if (cmd.hasToggle) {
                flags2 |= 0x01;
                if (cmd.toggle) flags2 |= 0x02;
            }
            if (cmd.hasDittos) {
                flags2 |= 0x04;
                if (cmd.ditto) flags2 |= 0x08;
            }
            switch (cmd.position) {
            case IRCommandReceived::Position::First:  flags2 |= 0x10; break;
            case IRCommandReceived::Position::Middle: flags2 |= 0x20; break;
            case IRCommandReceived::Position::Last:   flags2 |= 0x30; break;
            };
            if (cmd.isAutoRepeat) flags2 |= 0x40;

            // build the report
            *p++ = FeedbackReport::RPT_IR_COMMAND;
            *p++ = cmd.proId;
            *p++ = cmd.useDittos ? 0x02 : 0x00;
            PutUInt64(p, cmd.code);
            *p++ = flags2;
            PutUInt64(p, cmdInfo.dt);
        }

        // If we didn't generate any other report, and we're in
        // continuous status reporting mode, generate a status report on
        // every polling cycle.
        if (p == buf && continuousStatusMode)
            SendStatus(p);

        // If we generated a message, zero the rest of the buffer, to zero
        // out unused bytes after the last defined message parameter.
        if (p != buf && p < &buf[63])
        {
            memset(p, 0, buf + 63 - p);
            p = &buf[63];
        }
    }

    // return the length populated
    return static_cast<uint16_t>(p - buf);
}

void USBIfc::FeedbackController::SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen)
{
    // input messages must be at least one byte
    if (reqLen == 0)
        return;

    // log the request for debugging
    if (buf[0] == prvRequestCmd && time_us_64() < prvRequestBatchStartTime + 5000000)
    {
        // repeat - count it, defer logging it
        prvRequestCnt += 1;
    }
    else
    {
        // log the previous request count
        if (prvRequestCnt > 0)
            Log(LOG_DEBUGEX, "Feedback request repeated: %02x, %d repeats\n", buf[0], prvRequestCmd);

        // log the new one
        Log(LOG_DEBUGEX, "Feedback request received: %02x, req len %d\n", buf[0], reqLen);

        // remember it for next time
        prvRequestCmd = buf[0];
        prvRequestCnt = 0;
        prvRequestBatchStartTime = time_us_64();
    }

    // Handle the host-to-device report based on the message type
    // in the first byte.
    switch (buf[0])
    {
    case FeedbackRequest::REQ_QUERY_ID:
        // QUERY UNIT NUMBER
        // Queue a UNIT NUMBER report (message type 0x01) by setting
        // the bit in the pending input report bits.  Message type N is
        // represented by bit N-1 in the bit vector.
        pendingInputReports |= 0x00000001;
        break;

    case FeedbackRequest::REQ_QUERY_STATUS:
        // QUERY STATUS <Mode:BYTE>
        // Queue a STATUS report (message type 0x02) by setting the
        // bit in the pending input report bits, OR turn continuous
        // status reporting on or off, according to the Mode byte in
        // the message.
        switch (buf[1])
        {
        case 0x00:
            // end continuous reporting mode
            continuousStatusMode = false;
            break;

        case 0x01:
            // Send one status report.  The status report message type
            // code is 0x02, which corresponds to bit 1 (0x00000002)
            // in the pending report vector.
            pendingInputReports |= 0x00000002;
            break;

        case 0x02:
            // enable continuous reporting mode
            continuousStatusMode = true;
            break;
        }
        break;

    case FeedbackRequest::REQ_NIGHT_MODE:
        // NIGHT MODE <Engage:BYTE>
        switch (buf[1])
        {
        case 0x00:
            // night mode off
            nightModeControl.Set(false);
            break;

        case 0x01:
            // night mode on
            nightModeControl.Set(true);
            break;

        default:
            // Other <Engage> values are intentionally ignored, so that
            // new values can be added in the future, with assurance that
            // older firmware versions will at least behave predictably
            // by ignoring the unrecognized code.
            break;
        }
        break;

    case FeedbackRequest::REQ_TV_RELAY:
        // TV RELAY <Mode:BYTE>
        switch (buf[1])
        {
        case 0:
            // turn manual mode off
            tvOn.ManualSetRelay(false);
            break;

        case 1:
            // turn manual mode on
            tvOn.ManualSetRelay(true);
            break;

        case 2:
            // manual pulse
            tvOn.ManualPulseRelay();
            break;
        }
        break;

    case FeedbackRequest::REQ_CENTER_NUDGE:
        // CENTER NUDGE DEVICE
        nudgeDevice.RequestManualCentering();
        break;

    case FeedbackRequest::REQ_IR_TX:
        // AD HOC IR TRANSMIT <Protocol:BYTE> <Flags:BYTE> <CommandCode:UINT64> <Count:BYTE>
        {
            const uint8_t *p = &buf[1];
            uint8_t protocol = *p++;
            uint8_t flags = *p++;
            uint64_t code = GetUInt64(p);
            uint8_t count = *p++;
            irTransmitter.QueueCommand(IRCommandDesc(protocol, code, flags), count);
        }
        break;

    case FeedbackRequest::REQ_SET_CLOCK:
        // SET WALL CLOCK TIME <Year:UINT16> <Month:BYTE> <Day:BYTE> <Hour:BYTE> <Minute:BYTE> <Second:BYTE>
        // The hour is 0..23 (24-hour clock representation)
        {
            // Build a DateTime struct from the parameters.  Note that we
            // only have to populate the calendar and clock fields, not
            // the linear time fields (JDN, seconds-past-midnight).
            DateTime dt;
            const uint8_t *p = &buf[1];
            dt.yyyy = GetUInt16(p);
            dt.mon = *p++;
            dt.dd = *p++;
            dt.hh = *p++;
            dt.mm = *p++;
            dt.ss = *p++;

            // set the reference time
            timeOfDay.SetTime(dt, true);
        }
        break;

    case FeedbackRequest::REQ_ALL_OFF:
        // ALL PORTS OFF
        OutputManager::AllOff();
        break;

    case FeedbackRequest::REQ_SET_PORT_BLOCK:
        // SET PORT BLOCK <NumberOfPorts:BYTE> <FirstPortNumber:BYTE> <Level1:BYTE> ...
        // This can set up to 60 ports in one message.  If the port count
        // is greater than 60, the message is ill-formed.
        if (int n = buf[1], port = buf[2] ; n <= 60)
        {
            // visit all level entries
            const uint8_t *p = &buf[3];
            for (int i = 0 ; i < n ; ++i)
                OutputManager::Set(port++, *p++);
        }
        break;

    case FeedbackRequest::REQ_SET_PORTS:
        // SET PORTS <NumberOfPorts:BYTE> <PortNumber1:BYTE> <Level1:BYTE> ...
        // This can set up to 30 ports in one message.  If the port count
        // is greater than 30, the message is ill-formed.
        if (int n = buf[1] ; n <= 30)
        {
            // visit all port/level pairs
            const uint8_t *p = &buf[2];
            for (int i = 0 ; i < n ; ++i)
            {
                int port = *p++;
                uint8_t level = *p++;
                OutputManager::Set(port, level);
            }
        }
        break;

    case FeedbackRequest::REQ_LEDWIZ_SBA:
        // LEDWIZ SBA <FirstPortNumber:BYTE> <State1:BYTE> <State2:BYTE> <State3:BYTE> <State4:BYTE> <Period:BYTE>
        {
            int portNum = buf[1];
            uint8_t period = buf[6];
            const uint8_t *sp = &buf[2];
            uint8_t s = *sp++;
            for (int i = 0, bit = 0x01 ; i < 32 ; bit <<= 1, ++i)
            {
                // advance to the next message byte after passing bit 8
                if (bit == 0x100)
                    s = *sp++, bit = 0x01;
                
                // set the port
                OutputManager::SetLedWizSBA(portNum++, (s & bit) != 0, period);
            }
        }
        break;

    case FeedbackRequest::REQ_LEDWIZ_PBA:
        // LEDWIZ PBA <FirstPortNumber:BYTE> <NumPorts:BYTE> <Port1:BYTE> ...
        if (int n = buf[2] ; n < 60)
        {
            int portNum = buf[1];
            const uint8_t *sp = &buf[3];
            for (int i = 0 ; i < n ; ++i)
                OutputManager::SetLedWizPBA(portNum++, *sp++);
        }
        break;
    }
}

// Receive an IR command
void USBIfc::FeedbackController::OnIRCommandReceived(const IRCommandReceived &command, uint64_t dt)
{
    // add the command to our ring buffer, for reporting to the USB host
    irCmdBuf[irCmdWrite] = { command, dt };

    // bump the write pointer
    irCmdWrite = (irCmdWrite + 1) % IRCmdBufSize;

    // if the write pointer collided with the read pointer, the buffer
    // has overflowed; bump the read pointer to discard the oldest item
    if (irCmdWrite == irCmdRead)
        irCmdRead = (irCmdRead + 1) % IRCmdBufSize;
}
