// Pinscape Pico - Feedback Device Controller HID Interface Protocol
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// This file defines the protocol for our Feedback Controller HID
// interface.
//
// The Feedback Controller HID interface is a custom HID that provides
// access to the Pinscape feedback device functions.  This is realized
// as a HID interface principally because HID has some unique technical
// features that make it the best choice for this application.  In
// particular, HID has native support on all major operating systems,
// without the need for users to install any device drivers, making the
// user experience frictionless; and HID by design allows concurrent
// application access to the same device, eliminating many potential
// sources of conflicts that affect other USB driver types.  Apart from
// the technical advantages, HID is also a conceptually good fit for our
// use case, since Pinscape is fundamentally a real-time input/output
// device.  It's a peculiar I/O device that doesn't have a direct
// equivalent in the HID specification's collection of pre-defined
// device types, but HID allows for this sort of extension to its
// pre-defined types with a catch-all "application-defined" usage.
// Our HID report descriptor is thus just an opaque array of bytes.
//
// That's where this file comes in: this file defines the details of
// what we put into the opaque array of bytes in our IN and OUT report
// types.
//
// Our IN and OUT reports use the same basic format.  The first byte of
// a report is a type code that specifies the report sub-type.  The rest
// of the bytes are "arguments", interpreted according to the type code
// in the first byte.
//
// For OUT reports (host-to-device), the reports are essentially
// "commands" to the feedback controller, telling it to carry out some
// function, such as "turn on output port 7", or "query" requests asking
// it to send back some information via an IN report.  IN reports
// (device-to-host) contains status information or event reports
// representing physical input received on the device.  Currently, the
// only physical events we send through the Feedback Controller
// interface are IR remote control commands received.
//
// The type codes for IN reports are meaningful only for IN reports, and
// the codes for OUT reports are only meaningful in OUT reports.
//
// Note that the Feedback Controller interface DOESN'T report input
// events like button presses or accelerometer input.  Those sorts of
// events are reported through our *other* HID interfaces, such as our
// virtual keyboard, gamepad, and XInput devices, that are better
// matched conceptually to each type of input.  Keyboard, gamepad, and
// XInput are all standard device types on the host side, which means
// that applications can access them without special Pinscape-specific
// programming.  Any application that can read from a keyboard can read
// from *our* keyboard, since it looks exactly like an ordinary keyboard
// to applications.  The same goes for gamepads and XInput devices; many
// games have built-in support for one or the other of these, which lets
// them read Pinscape input without any awareness of what a Pinscape is.
// The Feedback Controller interface is reserved for physical inputs
// that don't map onto any of the standard device types; currently, this
// is just IR remote control receiver commands.  Input that doesn't map
// onto standard device types would need special application programming
// to be useful on the host side anyway, so it's appropriate to report
// it through our custom interface, where we can describe it in our own
// terms for maximum fidelity.  It makes no sense to try to force
// non-standard input events into a standard device format, since that
// would just lose information.
//
// The input and output reports are always 63 bytes, consisting of one
// byte for the type code, and 62 bytes of arguments.  Unused argument
// bytes should be set to zero.  (Zeroing unused bytes makes it possible
// to add new arguments to a given message type in the future, while
// maintaining compatibility with older versions.  Zero bytes in
// expanded argument positions will always be defaults that yield the
// same effect that the original smaller message format had.)
//
// Note that on the PC side, when reading and writing reports directly
// through a file handle opened on the HID, the USB HID protocol adds
// one more prefix byte at the start of each message specifying the
// report descriptor type.  That's a fixed byte value, always 0x04
// (see USBIfc::HID::ReportIDFeedbackController in USBIfc.h).
//
// All multi-byte integer fields are encoded in little-endian order.
//
//
// INTERFACE DISCOVERY
//
// On Windows, HID devices can be enumerated through the Setup API; the
// Windows API documentation has examples.  However, since we use the
// generic "undefined" usage to describe the interface as a special-
// purpose device with a custom report structure, it's possible that a
// system-wide enumeration would find other, unrelated devices that
// use the same generic usage to identify their own custom interfaces.
// The normal way to distinguish such cases is to rely on the USB
// VID/PID identifiers, but this isn't recommended for Pinscape Pico,
// because the USB IDs can be reconfigured by the user (to resolve
// conflicts with other devices, for example).  For this reason, we
// associate a "string label" with our HID input/output reports.  The
// string label is a HID mechanism specifically provided for identifying
// vendor-specific usages that don't fit any of the pre-defined usage
// codes.  Here's the procedure that the Pinscape Pico API on Windows
// uses to make the positive identification:
//
// 1. Enumerate all of the Vendor Interface devices, as described in
// VendorIfcProtocol.h.  These can be unambiguously identified via a
// private GUID.
//
// 2. Get the VID/PID for each identified Vendor Interface.
//
// 3. Enumerate USB HID devices system-wide by the normal procedure, and
// filter for matches to the VID/PIDs identified through the Vendor
// Interfaces
//
// 4. Filter the devices identified thus far to match only those with
// Usage Page 0x06 (Generic Device), Usage 0x00 (Undefined).
//
// 5. Read the report descriptor, and check for a string label.  On
// Windows, this can be done by getting the "Button Capabilities" array
// for the report type.  If there's no string index, reject the interface
// (filter it out).  If there's a string index, read the associated
// string, and test it against this C++ regex:
//
//    std::wregex(L"PinscapeFeedbackController/(\\d+)")
//
// If it matches, you've positively identified the interface as the
// Pinscape Pico Feedback Controller interface.  If not, reject the
// interface; it's something else.  On a successful match, the first
// (and only) capture group in the regex will match a decimal digit
// string that's intended to serve as a version number for the
// interface.  This is currently "1", but might change in the future
// if we make changes that require version recognition.
//
//
// ACCESSING THE INTERFACE
//
// Once you've identified the HID interface through the Setup API, you
// can open it as though it were a file.  The Setup API provides you
// with a file system name string that you can use in a CreateFile() API
// call.  The file handle is a read/write handle that you can use to
// send requests (by writing the handle) and receive input (by reading
// from the handle).
//
// HID access is by design sharable - multiple programs can access the
// device at the same time.  The request protocol is designed with that
// in mind.  In particular, each request is self-contained, which makes
// the protocol stateless.  There's no risk that a request sent from
// another program concurrently with your request will confuse the
// protocol state, since there isn't any state to confuse.  Each request
// is simply carried out on the device side, atomically, in the order
// received.  The device handles one request at a time; if multiple
// requests are sent from the host at the same time, they're queued and
// processed serially.
//
// Because the protocol is stateless, there isn't really such a thing as
// a "reply" to a request.  Instead, you should think of incoming data
// from the device as spontaneous reports.  When you send a QUERY, it
// will trigger the device to generate the type of report you requested
// with the query, but there's no guarantee that this will be the ONLY
// report from the device you'll see, or that it will be the NEXT
// report.  It's possible that another program also sent its own,
// different query at around the same time, so you might see one or more
// reports unrelated to your query before the report you requested is
// sent.  You should therefore always loop on reading after sending a
// query request: read a report, check its type, and discard it if it's
// not the report type you requested.  Continue until you either see the
// report you asked for, or you run out of patience (i.e., it's a good
// idea to keep track of time, and fail if the time exceeds a preset
// limit of your choosing).  The device tries to service query requests
// immediately, so it's typical to see a response within a few USB
// polling cycles (a few tens of milliseconds).  A good default timeout
// value is about 100ms , since that's fast enough that it won't be too
// noticeable to a user if a timeout does occur, but long enough that we
// can reasonably expect a reply to any query request when things are
// working properly.
//

#pragma once
#include <stdint.h>
#include <initializer_list>
#include "CompilerSpecific.h"


// The Feedback Control HID interface protocol version.  This is a
// 16-bit value that should be incremented each time the Feedback
// Control interface is modified to add a new message type, add
// parameters to an existing message type, or otherwise add new
// capabilities that didn't exist in past protocol versions.  Client
// applications running on the host can retrieve this information via a
// query command on the HID protocol, and can use it to determine if
// certain features are available.  This should match the version number
// documented in USB_HID_Feedback_Controller_Protocol.txt, based on the
// version of the protocol specification that the software currently
// implements.
//
// We strive whenever possible to maintain two-way compatibility when
// making changes to the protocol, so it should only rarely be necessary
// for host applications to act conditionally based on the protocol
// version number.  In almost all cases, protocol changes will involve
// adding new request types or arguments, or expanding a list of
// enumerated argument values, so the older version of a protocol should
// always mean the same thing it always did to newer firmware and newer
// clients.  By the same token, a newer protocol will still mean the
// same thing to an older application or firmware version, since any new
// requests or arguments will just be ignored by the older software.
// The main situation where a version number check might be necessary is
// when an application wants to test for the availability of a newer
// feature, to decide whether or not to make an option available to the
// user.
#define FEEDBACK_CONTROL_VERSION 0x0001

namespace PinscapePico
{
    // HID Report ID.  In the raw HID format visible on the host,
    // the first byte of every IN and OUT report must contain this
    // byte value.
    const uint8_t FEEDBACK_CONTROLLER_HID_REPORT_ID = 4;

    // Host-to-device request format.  The host sends this struct to
    // invoke a command on the device.
    struct __PackedBegin FeedbackControllerRequest
    {
        FeedbackControllerRequest() { }
        FeedbackControllerRequest(uint8_t type) : type(type) { memset(args, 0, sizeof(args)); }
        FeedbackControllerRequest(uint8_t type, const std::initializer_list<uint8_t> argsList) : type(type) {
            size_t copySize = argsList.size();
            memcpy(args, data(argsList), copySize < sizeof(args) ? copySize : sizeof(args));
            if (copySize < sizeof(args))
                memset(&args[copySize], 0, sizeof(args) - copySize);
        }
        
        // the first byte of every report is the type code
        uint8_t type = REQ_INVALID;

        // The rest of the report is arguments, interpreted according
        // to the type code.
        uint8_t args[62];

        //
        // Request (host-to-device) type codes
        //

        // INVALID/EMPTY REQUEST
        // <0x00:BYTE)
        static const uint8_t REQ_INVALID = 0x00;

        // QUERY DEVICE IDENTIFICATION
        // <0x01:BYTE>
        //
        // The device sends back a RPT_ID report on receipt, with
        // information on the device's identifiers.
        static const uint8_t REQ_QUERY_ID = 0x01;

        // QUERY STATUS
        // <0x02:BYTE> <Mode:BYTE>
        //
        // This requests one status report, or enables or disables
        // continuous status reporting mode, according to the <Mode>
        // argument byte:
        //
        //   0 -> end continuous reporting mode
        //   1 -> send one status report
        //   2 -> enable continuous reporting mode
        //
        // Status reports are sent with type code RPT_STATUS.  When one
        // report is requested (<Mode> == 1), the device sends back an
        // RPT_STATUS report after receiving this command.  When
        // continuous status reporting is enabled, the device sends a
        // status report on each USB polling cycle where no other type
        // of IN report is pending.  Other query types and physical
        // input event reports are given priority.
        static const uint8_t REQ_QUERY_STATUS = 0x02;

        // NIGHT MODE
        // <0x10:BYTE> <Mode:BYTE>
        //
        // The <Mode> byte specifies the new Night Mode setting:
        //
        //   0 -> night mode off (normal/day mode)
        //   1 -> night mode on
        //   * -> other values are reserved for future use
        static const uint8_t REQ_NIGHT_MODE = 0x10;

        // TV RELAY ON/OFF/PULSE
        // <0x11:BYTE> <Mode:BYTE>
        //
        // The <Mode> argument byte specifies the operation to apply to
        // the TV relay output port:
        //
        //   0 -> manual mode off
        //   1 -> manual mode on (turns on relay until next 'manual mode off' command)
        //   2 -> manual pulse (turns on relay for the configured TV ON pulse time)
        //   * -> other values are reserved for future use
        static const int REQ_TV_RELAY = 0x11;

        // CENTER NUDGE DEVICE
        // <0x12:BYTE>
        //
        // This immediately applies the average of recent readings (over
        // the last few seconds) as the center point for the
        // accelerometer X/Y axes.  No arguments.
        static const int REQ_CENTER_NUDGE = 0x12;

        // IR SEND
        // <0x13:BYTE> <Protocol:BYTE> <Flags:BYTE> <Command:UINT64> <Count:BYTE>
        //
        // This sends an ad hoc IR remote control command on the IR
        // emitter.  If another command is already in progress, the new
        // command is queued, and sent as soon as the current command
        // (and any previously queued commands) are finished.  The
        // protocol, flags, and command code use the Pinscape universal
        // remote control code format; see IRRemote/IRCommand.h.  This
        // is the same format that the "learning" receiver feature uses,
        // so the easiest way to get the correct code for a given button
        // on a given remote control is via the learning feature in the
        // Config Tool.
        static const int REQ_IR_TX = 0x13;

        // SET WALL CLOCK TIME
        // <0x14:BYTE> <Year:UINT16> <Month:BYTE> <Day:BYTE> <Hour:BYTE> <Minute:BYTE> <Second:BYTE>
        //
        // This lets the host PC inform the Pico of the current clock
        // time, to enable features that trigger different effects
        // according to the time of day.  It's necessary for the host PC
        // to send the time to the Pico after each Pico reset, because
        // the Pico doesn't have its own time-of-day clock or calendar;
        // its internal clock resets to zero each time the Pico resets.
        //
        // Sets the Pico's internal clock to the given date and time of
        // day.  The time and date are expressed in the local time zone;
        // there's no provision for storing the time zone or converting
        // between local and universal time, or for seasonal (daylight
        // time) changes.  The Year, Month, and Day fields are all
        // expressed in nominal calendar terms, with the four-digit
        // year, month 1-12, day 1-31.  The time is on a 24-hour clock,
        // with the hour in the range 0-23.
        static const int REQ_SET_CLOCK = 0x14;

        // ALL PORTS OFF
        // <0x20:BYTE>
        //
        // Turns off all output ports by setting their PWM brightness/
        // intensity settings to zero.
        static const int REQ_ALL_OFF = 0x20;

        // SET OUTPUT PORT BLOCK
        // <0x21:BYTE> <NumPorts:BYTE> <FirstPortNumber:BYTE> <Level1:BYTE> <Level2:BYTE> ... <LevelN:BYTE>
        //
        // This sets the PWM level (brightness/intensity) values for a
        // contiguously numbered block of output ports.  Each port in
        // the block can be assigned its own separate value.
        //
        // <NumPorts> specifies the number of ports to be updated, and
        // <FirstPortNumber> specifies the starting port number.  The
        // remaining bytes give the level values to assign to the ports,
        // one byte per port, in sequential order from the first port
        // number specified in <FirstPortNumber>.  Up to 60 ports can be
        // set in a single command.
        //
        // Comparison to SET OUTPUT PORTS: SET OUTPUT PORT BLOCK can set
        // more ports in a single command, but all ports in this command
        // must be numbered sequentially, since the only port number that
        // can be specified is the first of the block.  SET OUTPUT PORTS
        // can only set half as many ports per command, but it can set
        // any random collection of ports, without any restrictions on
        // adjacency in their numbering.
        static const int REQ_SET_PORT_BLOCK = 0x21;

        // SET OUTPUT PORTS
        // <0x22:BYTE> <NumPorts:BYTE> <PortNumber1:BYTE> <Level1:BYTE> <PortNumber2:BYTE> <Level2:BYTE> ... <PortNumberN:BYTE> <LevelN:BYTE>
        //
        // This sets a collection of individually addressed output
        // ports.  <NumPorts> specifies the number of ports in the
        // command.  The remaining argument bytes are pairs of bytes
        // specifying the port number to update, and the new PWM level
        // setting to apply.  Up to 30 ports can be set in a single
        // command.
        //
        // Comparison to SET OUTPUT PORT BLOCK: This command can set
        // a collection of randomly numbered ports, but it can only
        // set 30 ports per command.  SET OUTPUT PORT BLOCK can set
        // twice as many ports in one command, but all of the ports
        // have to be grouped into a sequentially numbered block.
        // It's more efficient to use SET OUTPUT PORT BLOCK when
        // the ports to be set are grouped together, and it might
        // be more efficient to use SET OUTPUT PORTS when they're
        // not grouped.
        static const int REQ_SET_PORTS = 0x22;          // set individual output ports

        // LEDWIZ SBA
        // <0x30:BYTE> <FirstPortNumber:BYTE> <State1:BYTE> <State2:BYTE> <State3:BYTE> <State4:BYTE> <Period:BYTE>
        //
        // Emulates an LedWiz SBA command.  The four StateN bytes contain
        // bit masks for the On/Off state for the 32 consecutive ports
        // starting at FirstPortNumber, with the bits ordered from the
        // least significant end of each byte.  That is, (State1 & 0x01)
        // gives the On/Off state for FirstPortNumber, (state1 & 0x02)
        // is the state for FirstPortNumber+1, etc.  The Period byte
        // gives the period to use for waveform profiles (PBA values
        // 129..132), in units of 250ms; valid values are 1..7.
        //
        // Per our usual convention, the first Pinscape Pico logical
        // port is labeled #1.
        //
        // This command is designed to support LedWiz emulation on the
        // host side, via a custom ledwiz.dll that replaces the original
        // one supplied by Groovy Game Gear.  Since the DLL was always
        // the official API to the original LedWiz, it shouldn't be
        // necessary to emulate the LedWiz at the USB level, since all
        // legacy LedWiz-aware applications should be going through the
        // DLL.
        static const int REQ_LEDWIZ_SBA = 0x30;

        // LEDWIZ PBA
        // <0x31:BYTE> <FirstPortNumber:BYTE> <NumPorts:BYTE> <Port1:BYTE> ...
        //
        // Emulates an LedWiz PBA command or group of PBA commands.
        // Each PortN byte contains a "profile" setting for its port, which
        // is a valid from 0-48 giving a PWM duty cycle in units of 1/48,
        // OR a waveform ID value:
        //
        //   129 = sawtooth
        //   130 = on/off flash (50% duty cycle)
        //   131 = on/ramp down (50% duty cycle flash with linear fade-out)
        //   132 = ramp up/on (50% duty cycle flash with linear fade-in)
        //
        // The PortN bytes address sequential ports starting at
        // FirstPortNumber.  Per our usual conventions, the first output
        // ports is labeled #1.  Up to 60 ports may be set on one call.
        //
        // The original LedWiz protocol could only set 8 ports at a time
        // via USB PBA messages, but the public API exposed by the DLL
        // sets all 32 ports at once.  Our larger message size allows
        // mapping the 32-port PBA API call to a single USB message.
        static const int REQ_LEDWIZ_PBA = 0x31;
    } __PackedEnd;

    // Device-to-host (IN) report format.  The device sends this struct
    // to the host to report query results and input events.
    //
    // Pinscape only sends IN reports on the Feedback Controller
    // interface in response to host QUERY-type commands, or when
    // suitable physical input events occur.  Host requests that don't
    // specifically ask for returned information don't trigger any
    // reply.  This is typically a low-traffic interface as a result,
    // since this interface doesn't concern itself with any of the sorts
    // of physical inputs that update frequently (such as button presses
    // and accelerometer readings).
    struct __PackedBegin FeedbackControllerReport
    {
        // the first byte of the report is the report type code
        uint8_t type = RPT_INVALID;

        // The remaining 62 bytes of the report are arguments, the
        // meaning of which vary by report type.
        uint8_t args[62];

        //
        // Input report (device-to-host) type codes
        //

        // invalid/empty report
        static const int RPT_INVALID = 0x00;               

        // IDENTIFICATION REPORT
        // <0x01:BYTE> <UnitNumber:BYTE> <UnitName:CHAR[32]> <ProtocolVer:UINT16>
        //   <HardwareID:BYTE[8]> <NumPorts:UINT16> <PlungerType:UINT16>
        //   <LedWizUnitMask:UINT16>
        //
        // This report is sent to the host in response to a REQ_QUERY_ID
        // command.  The arguments provide the host with identifying
        // information for the device.
        //
        // <UnitNumber>     = the Pinscape unit number set in the configuration, used as the DOF identifier
        // <UnitName>       = the Pinscape unit name set in the configuration, as a user-friendly name for unit listings
        // <ProtocolVer>    = version number of the Feedback Controller protocol (FEEDBACK_CONTROL_VERSION)
        // <NumPorts>       = the number of output ports currently configured
        // <PlungerType>    = plunger sensor type code; one of the PLUNGER_xxx constants below
        // <HardwareID>     = the Pico's unique 64-bit hardware ID, as an array of 8 bytes
        // <LedWizUnitMask> = mask of unit numbers to assign as virtual LedWiz units
        //
        // The Unit Number is a small integer (typically starting at 1 for
        // the first Pinscape unit in a system, and numbered sequentially
        // for any additional units) that identifies the Pico for DOF and
        // other application purposes.  This number is configurable by the
        // user, so it's NOT necessarily unique, stable, or permanent.  It's
        // meant to be unique within the local system, but that's up to the
        // user to ensure, because the Pico has no way of knowing about any
        // other Picos connected to the system and so can't coordinate the
        // uniqueness requirement on its own.  The point of the Unit Number
        // is to facilitate automatic discovery of Pinscape units by DOF
        // and other application software.  Automatic discovery means that
        // applications find the Pinscape units on their own, by scanning
        // the system for attached USB devices, as opposed to requiring
        // the user to enter the device ID (e.g., the hardware ID) in
        // application-specific config files.  For a system with a single
        // Pinscape Pico unit, there's no need even for so much as a Unit
        // Number, since applications can easily see that there's exactly
        // one Pico unit in the system and thus there's no ambiguity about
        // which unit goes with which feedback devices.  The Unit Number
        // comes into play when the system has multiple Pinscape Picos, in
        // which case DOF and other software need a way to distinguish the
        // units.  Rather than requiring the user to tell every application
        // individually about the Pico hardware IDs, we provide the Unit
        // Number, which the user only needs to configure once, on the Pico
        // itself.  Applications can then use the simplified scheme that
        // DOF itself uses, of addressing distinct units of a given device
        // type as Pinscape Pico #1, Pinscape Pico #2, etc.
        //
        // The Unit Name is a user-assigned name set in the JSON config,
        // meant to serve as a human-friendly name that can be shown in
        // device listings.  It's not meant as an identifier to
        // distinguish units; it's just a way to show users something
        // more meaningful than a number when listing units.  The string
        // is in single-byte characters from the Unicode Latin-1 code page
        // (i.e., the first 256 Unicode code points).  It's padded at the
        // end with null bytes if it's shorter than 32 bytes.  There's no
        // null terminator if it's excatly 32 bytes.
        //
        // The hardware ID is a 64-bit opaque identifier that's universally
        // unique among all physical Picos.  The hardware ID is physically
        // stored in ROM on the Pico and programmed at the factory, so it's
        // permanent - it will never change for a given Pico board.  The
        // ID's uniqueness and permanence make it the best way to identify
        // the Pico on the host side, since it will never be affected by
        // resets, firmware updates, changing which USB port the device is
        // plugged into, or any of the many other factors that tend to make
        // other USB device identifiers unreliable.
        //
        // The port count reflects the number of *configured* output
        // ports, as opposed to the number of physical ports.  From the
        // application's perspective, only the logical (configured)
        // ports are directly addressable, so that's what this number
        // represents.
        //
        // The LedWiz Unit Mask is a mask of desired unit numbers that an
        // LedWiz emulator DLL on the PC side should assign to this device
        // to create a collection of virtual LedWiz units for use by legacy
        // software based on the LEDWIZ.DLL public API.  The low-order bit
        // (0x0001) corresponds to unit #1, bit 0x0002 is unit #2, bit 0x0004
        // is unit #3, and so on, up to 0x8000 for unit #16.  The emulator
        // DLL should start at the lowest unit number in the mask, and assign
        // additional units as needed.  For example, if the mask is 0xFFF0,
        // the first virtual unit should be assigned as #5, the second as #6,
        // etc.  The reason that we use a mask instead of just giving the
        // unit number directly is that the emulator will have to create more
        // than one virtual LedWiz if the Pico has more than 32 output ports
        // configured, since each LedWiz can only represent 32 ports.
        static const int RPT_ID = 0x01;

        // plunger type codes
        static const int PLUNGER_NONE = 0;         // no plunger configured
        static const int PLUNGER_POT = 1;          // potentiometer
        static const int PLUNGER_AEDR8300 = 2;     // AEDR-8300 quadrature encoder
        static const int PLUNGER_VCNL4010 = 3;     // VCNL4010 IR proximity sensor
        static const int PLUNGER_VL6180X = 4;      // VL6180X IR time-of-flight distance sensor
        static const int PLUNGER_TCD1103 = 5;      // TCD1103 linear optical sensor (1500 pixels)
        static const int PLUNGER_TSL1410R = 6;     // TSL1410R linear optical sensor (1280 pixels)
        static const int PLUNGER_TSL1412S = 7;     // TSL1412S linear optical sensor (1536 pixels)

        // STATUS REPORT
        // <0x02:BYTE> <Flags:BYTE> <TvOnState:BYTE> <StatusLed:RGB>
        //
        // <Flags>     = Bit flags:
        //               0x01   plunger enabled
        //               0x02   plunger calibrated
        //               0x04   night mode active
        //               0x08   wall-clock time has been set
        //               0x10   Safe Mode boot (due to unexpected reset)
        //               0x20   user configuration loaded
        //                *     all other bits reserved for future use
        //
        // <TvOnState> = Current TV ON power sensing state:
        //               0  Default, power was on at last check, or awaiting power to return
        //               1  Power is off, writing power sense latch
        //               2  Power is off, reading power sense latch
        //               3  Off-to-on power transition detected, TV ON delay countdown in progress
        //               4  Pulsing TV relay
        //               5  ready to send next TV ON IR command
        //               6  waiting for delay between IR commands
        //               7  IR command transmission in progress
        //
        // <StatusLed> = Current status LED color, as a 3-byte RGB value
        //               <Red:BYTE> <Green:BYTE> <Blue:BYTE>
        //
        // This is sent in response to a REQ_QUERY_STATUS command.  In
        // addition, a status report is sent on each USB polling cycle
        // when continuous status reporting mode is enabled and no other
        // query or event reports are pending.
        static const int RPT_STATUS = 0x02;

        // IR COMMAND RECEIVED
        // <0xF0:BYTE> <Protocol:BYTE> <ProtocolFlags:BYTE> <Command:UINT64> <CommandFlags:BYTE> <ElapsedTime:UINT64>
        //
        // This is sent when the IR remote control receiver successfully
        // decodes an IR input command, indicating that the user pointed a
        // remote at our IR receiver and pressed a button.  All IR commands
        // that the IR protocol handlers can successfully decode generate
        // events; it's not necessary for specific commands to be
        // pre-programmed on the device.
        //
        // <Protocol>      = the protocol identifier for the code received; see IRRemote/IRProtocolID.h
        //
        // <ProtocolFlags> = bit flags describing protocol variations:
        //                    0x02 = the protocol uses "dittos" (i.e., the protocol has a distinct
        //                           pulse format representing auto-repeats)
        //
        // <Command>       = the command coded, identifying the remote button pressed; command
        //                   codes are arbitrary 16-bit to 64-bit numbers assigned by the
        //                   manufacturer of the remote control device to identify individual
        //                   buttons; they're generally unique among the buttons on a given
        //                   remote control, but they're not guaranteed to be unique among
        //                   other devices, even those that use the same protocol, and they
        //                   have no broader meaning other than being ad hoc identifiers for
        //                   that single device's buttons
        //
        // <CommandFlags>  = bit flags describing details of this specific command received:
        //                    0x01 = Toggle bit is present (i.e., the protocol uses toggle bits)
        //                    0x02 = Toggle bit
        //                    0x04 = Ditto bit is present (i.e., the protocol uses dittos)
        //                    0x08 = Ditto bit; if set, this is an auto-repeat from the user holding down the button
        //                    0x10 } Position code:
        //                    0x20 }  '00' (0x00) -> Null (the protocol doesn't use position codes)
        //                            '01' (0x10) -> "First" (first code from a new button press)
        //                            '10' (0x20) -> "Middle" (repeated code from holding down a button)
        //                            '11' (0x30) -> "Last" (last code after releasing the button)
        //                    0x40 = Auto-repeat flag
        //                    0x80 = Reserved for future use
        //
        // <ElapsedTime>   = elapsed time since the previous command received, in microseconds
        //
        // The <ProtocolFlags> byte represents general features of the
        // protocol itself, while <CommandFlags> contains details of the
        // specific code transmission received.  The main difference is that
        // some protocols *define* a "ditto" format (a special pulse timing
        // format that represents auto-repeats), but not all remotes using
        // those protocols actually *use* the ditto format.  The command
        // flags can be used to determine whether the ditto format was
        // actually used for a particular code transmission, so you can
        // detect whether or not a remote uses dittos at all by observing
        // what it sends when a button is held down for long enough to make
        // it auto-repeat.
        //
        // The "Auto-repeat flag" in the command flags indicates the
        // protocol decoder's judgment about the repeat status of the
        // command, which it infers from the protocol's special repeat
        // coding (dittos, toggles, position codes) for protocols that
        // define such special coding, or simply from proximity in time for
        // protocols that don't.  Applications are free to ignore this flag
        // and make their own inferences based on the raw data provided in
        // the other bits.  It's probably more reliable in general to go
        // with the protocol decoder's interpretation as indicated by the
        // flag, but there might be special cases where an application is
        // designed to work with a particular remote control that's known to
        // deviate from the usual protocol behavior in such a way that the
        // firmware's protocol decoder gets the auto-repeat status wrong.
        //
        // Note that there isn't any special "learning" mode on the device
        // side.  The device continuously monitors the IR receiver for
        // incoming transmissions, and generates this event every time it
        // successfully decodes a command from a known protocol.  This can
        // be used on the application side to implement a learning mode in
        // which the application prompts the user to manually send commands
        // (by pressing buttons on a remote) and uses these events to
        // associate button presses with command codes received.
        static const int RPT_IR_COMMAND = 0xF0;
    } __PackedEnd;
}

