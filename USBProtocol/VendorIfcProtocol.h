// Pinscape Pico - USB Vendor Interface Protocol
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file defines the protocol for our vendor interface, which we use
// to expose configuration and control functions.
//
// A USB vendor interface is a custom USB interface that doesn't use one
// of the standard USB device classes, such as HID or MSD, but instead
// defines its own device-specific protocol.  It's called a "vendor"
// interface because it's defined by the company that sells the device.
// A vendor interface on the device must be paired with custom software
// on the host, which normally takes the form of a custom device driver
// provided by the manufacturer.  Windows and Linux also each have a
// notion of generic USB device drivers that let applications access a
// vendor interface without installing a separate kernel driver that's
// specific to the device.  Instead, application software (in user space)
// uses the generic driver to read and write the vendor interface's USB
// endpoints directly.  In either approach (custom device driver or
// generic device driver with a custom application), the device can only
// be meaningfully accessed by custom software on the host that's
// programmed to match the device's custom USB protocol, but the generic
// driver approach has the considerable advantage that there's no need
// for the user to install a device driver, which is almost always a
// more onerous task than installing an application.  The drawback with
// the generic driver approach is that the device can only be used by
// applications specifically written for it, but that's fine for
// special-purpose devices that perform unusual functions that aren't
// relevant to most applications anyway.
//
//
// INTERFACE DISCOVERY ON WINDOWS
//
// On Windows, the vendor interface automatically registers itself as a
// WinUsb device when the device is plugged in, so it's possible to
// discover the interface programmatically without any user action.
// (The only exception is XP, where the WinUsb driver must be manually
// installed first.  WinUsb is a built-in component on all later Windows
// versions.)
//
// To enumerate available Pinscape Pico devices, a Windows program can
// use the Windows Configuration Manager API.  In particular, use
// CM_Get_Device_Interface_List() to list devices under the Pinscape
// Pico Vendor GUID, {D3057FB3-8F4C-4AF9-9440-B220C3B2BA23}.  This will
// return a list of device IDs that can be opened via WinUsb to access
// the protocol defined here.
//
// Note that the vendor interface discovery process via the CM API is
// independent of USB device identifiers (VID/PID), so this procedure
// will enumerate all Pinscape Pico devices, even if the user has
// reconfigured the USB identifiers.  It's also unambiguous; it won't
// accidentally enumerate any non-Pinscape devices, since only Pinscape
// devices will use the private GUID.
//
//
// PROTOCOL USAGE
//
// To use this protocol, discover the interface as described above, and
// then open the interface's two bulk endpoints (IN and OUT) with the
// WinUsb API.  On Linux and other operating systems, you can use the
// open-source "libusb" library, which has similar functionality to
// WinUsb.  From the program's perspective, opening the endpoints
// provides the program with handles that you can read and write as
// though they were ordinary file handles.  To send a request to the
// device, format a VendorRequest struct and write it to the OUT
// endpoint handle.  The device will reply with a VendorResponse, which
// you can read via the IN endpoint handle.  Every request is matched
// with a response.  Some requests and responses come with additional
// data after the basic request/reply struct.  When extra an extra data
// object is needed for a request, simply write it to the OUT handle
// immediately after the request struct; when an extra data object is
// included in a response, read it from the IN handle immediately after
// reading the response struct.  The request/response structs each
// contain a field specifying the size of any additional data object
// appended after the struct.
//
// WinUsb by design is limited to exclusive access to the endpoint
// pipes, so only one program at a time can access the device through
// this interface.  The interface is designed around this limitation, so
// that it only includes functions that are by their nature only
// typically used in exclusive-access scenarios, particularly functions
// related to configuring the device.  Access to the vendor interface
// itself is exclusive, but accessing it doesn't have any effect on the
// OTHER interfaces the device exposes.  Other programs can continue to
// access the various HID interfaces concurrently while a config tool
// or similar program uses the vendor interface.
//
//
// WHY USE A VENDOR INTERFACE?
//
// In a word, performance.  The vendor interface allows for much
// faster data transfers for the configuration data than HID would.
//
// The original Pinscape software used a HID interface for its config
// functions.  In particular, it used its LedWiz emulator interface,
// which defines a single host-to-device reporting format with 8 bytes
// of device-specific data.  The authentic LedWiz encodes its output
// port controls into the 8-byte format; the original Pinscape software
// piggybacks on this by taking advantage of a range of byte values
// that are never used in authentic LedWiz commands, so they'd never be
// generated by regular LedWiz client software and thus can safely be
// used by Pinscape-aware clients to send Pinscape-specific commands to
// Pinscape devices.
//
// The HID approach seemed like a good idea at the time because it
// piggybacked on the existing LedWiz HID report format, thus avoiding
// making any changes to the shape of the HID interface on the PC side.
// This was desirable at the time because it maintained compatibility at
// the HID driver level with legacy LedWiz client software: if our HID
// interface is exactly the same as the genuine LedWiz's HID interface,
// legacy LedWiz client software should work with Pinscape devices
// without any changes to the client software.  Most of the legacy
// clients, particularly Future Pinball, are closed-source programs that
// are no longer being maintained, so it's impossible to update them to
// work with new device interfaces.  The only way to make them work with
// a new device is to make the device emulate an interface they already
// know how to work with, such as the LedWiz interface.
//
// The downside of the compatible-HID approach is that HID can only send
// small data packets, which severely limits its transfer rate.  The
// LedWiz's 8-byte report format limited us to 8 bytes per millisecond,
// or 8000 bytes per second.  This made the old Config Tool rather
// painfully slow when it had to fetch or send the device's full
// configuration data set, which is necessary when you want to edit or
// save the device's settings.  In contrast, a vendor interface can send
// and receive on the USB "bulk endpoints", which can transfer data at
// the native hardware speed of the device.  The KL25Z and Pico are both
// USB 2.0 "full speed" devices, which can transfer at up to 12 MB/s.
//
// The old HID-only approach was never really necessary for LedWiz
// compatibility, and in fact even the original Pinscape device never
// tried to look exactly like an LedWiz.  It also exposed joystick and
// keyboard interfaces alongside the LedWiz control interface, since it
// had a mission to act as a key encoder, accelerometer, and plunger
// sensor in addition to its LedWiz controller function.  This is all
// possible through the USB "composite device" mechanism, which allows
// one physical device to appear as multiple logical devices, so we can
// have the LedWiz-compatible HID interface alongside all of the others.
// The original Pinscape device never included a vendor interface in the
// mix, but it could have without affecting LedWiz compatibility.  The
// more important factor is that HID-level compatibility was never
// essential in the first place, because all of the legacy LedWiz
// clients go through a DLL provided by the manufacturer, LEDWIZ.DLL,
// instead of accessing the HID driver directly.  We can make any legacy
// LedWiz client compatible with just about any device by dropping in a
// replacement LEDWIZ.DLL that exposes the same client interface but
// translates the functions to a different device protocol.  
//

#pragma once
#include <stdint.h>
#include "CompilerSpecific.h"

namespace PinscapePico
{
    // Host-to-device request format.  The host sends this struct to
    // invoke a command on the device.
    struct __PackedBegin VendorRequest
    {
        VendorRequest()
        {
            cmd = CMD_NULL;
            checksum = 0;
            token = 0;
            memset(args.argBytes, 0, sizeof(args.argBytes));
        }

        VendorRequest(uint32_t token, uint8_t cmd, uint16_t xferBytes)
        { 
            this->cmd = cmd;
            this->token = token;
            this->checksum = ComputeChecksum(token, cmd, xferBytes);
            this->xferBytes = xferBytes;
            memset(args.argBytes, 0, sizeof(args.argBytes));
        }

        // Token.  This is an arbitrary 32-bit int supplied by the host
        // to identify the request.  It has no meaning to us other than
        // as an opaque ID for the request.  We echo this back in the
        // response, so that the host can correlate responses to requests.
        uint32_t token;

        // Checksum.  This is a simple integrity check on the packet,
        // to help ensure that the device doesn't attempt ot execute
        // ill-formed commands coming from unrelated applications.  Since
        // we use the generic WinUSB driver on Windows, any user-mode
        // application can send raw data over our endpoints.  (That's
        // the one major drawback of using WinUSB instead of a custom
        // device driver; a custom driver insulates the USB pipes from
        // direct access from application software, which helps ensure
        // that data on the wire is well-formed.)
        //
        // To compute the checksum, compute ~(token + cmd + xferBytes)
        // as a uint32_t, ignoring any overflow.
        uint32_t checksum;

        // figure the correct checksum for the given parameters
        static uint32_t ComputeChecksum(uint32_t token, uint8_t cmd, uint16_t xferBytes) {
            return ~(token + cmd + xferBytes);
        }

        // validate the stored checksum
        bool ValidateChecksum() const { return ComputeChecksum(token, cmd, xferBytes) == checksum; }

        // Command code
        uint8_t cmd;

        //
        // Command codes
        //

        // Null command - this can be used to represent an empty,
        // invalid, or uninitialized request structure.
        static const uint8_t CMD_NULL = 0x00;

        // Query version.  No arguments.  The reply reports the version
        // information via the args.version field.
        static const uint8_t CMD_QUERY_VERSION = 0x01;

        // Query the device's ID information.  No arguments.  The reply
        // reports the Pinscape unit number, Pinscape unit name, and the
        // Pico's 64-bit native hardware ID, via args.ids.
        //
        // The Pinscape unit number and unit name are user-configurable.
        // The unit number is the primary ID for DOF, the Config Tool, and
        // other contexts where the user has to distinguish among Pinscape
        // units when multiple devices are present.  The unit number is a
        // small integer, usually starting at 1 for the first unit, making
        // it easy to type into commands and recognize on-screen.  The
        // unit name is a short user-assigned name that's purely for
        // display purposes, to help the when a list of units is shown
        // on-screen.  The unit number and name aren't guaranteed to be
        // unique, since they're assigned by the user via the config file.
        //
        // The extra transfer data consists of a series of null-terminated
        // strings, composed of single-byte characters, appended
        // back-to-back (so that the next string starts immediately after
        // the terminating null byte of the prior string):
        //
        // - Unit name: the unit name string, assigned in the JSON config.
        //   This is a name assigned by the user, as a convenience for
        //   more easily identifying the device in display listings and
        //   the like.
        //
        // - Target board ID: the name string used in the Pico SDK to
        //   identify the target board in the firmware build process.
        //   The Pico is an open-source design with many clones, some
        //   compatible with the original reference design and some
        //   with different on-board peripherals.  Boards with custom
        //   hardware changes generally require the firmware to be
        //   rebuilt with different parameters.  That's what this ID
        //   indicates: the board configuration that the running
        //   firmware was built for.  This isn't a "live" board ID;
        //   it only indicates which configuration the firmware was
        //   compiled for.  So it won't distinguish between an
        //   original Raspberry Pi Pico and a compatible clone,
        //   for example.
        //
        // - Pico SDK version string: the version of the Pico SDK
        //   used to build the firmware.
        //
        // - Compiler version string: the name and version of the
        //   C++ compiler used to build the firmware, typically
        //   of the form "GNUC x.y.z".
        //    
        // The hardware ID is a unique identifier for the physical Pico
        // unit, programmed permanently into ROM at the factory during the
        // Pico manufacturing process.  It's unique and immutable, so it
        // serves as positive identification for a particular physical
        // Pico device.  It's not affected by resets or firmware updates
        // or anything else, since it's etched into the silicon.
        //
        // This also retrieves the Player Index for the XInput interface,
        // if that interface is enabled and the Player Index is known.
        // The Player Index ia the player number, 0 to 3, assigned by
        // the host when an XBox controller is plugged into a USB port.
        // This ID is permanent (at least for the duration of the USB
        // connection), and it's unique per attached controller, so it
        // can be used to determine which XInput interface on the host
        // side belongs to the Pinscape Pico unit, when multiple XInput
        // controllers are plugged in.  This lets the config tool and
        // other software identify the Pinscape virtual XInput device
        // when there are also one or more real XBox controller units
        // plugged into the system.
        static const uint8_t CMD_QUERY_IDS = 0x02;

        // Query USB interfaces.  No arguments.  The reply extra transfer
        // data contains a USBInterfaces struct describing the USB
        // configuration.
        static const uint8_t CMD_QUERY_USBIFCS = 0x03;

        // Reset the Pico and restart the flash program.  The first byte
        // of the arguments contains a subcommand that specifies the mode
        // to activate after the reset.
        //
        // The USB connection will be dropped across the reset, so the
        // host side will generally need to close and re-open its USB
        // connection.
        //
        // Resetting into Boot Loader mode can be used to install a
        // firmware update.  After the reset, the Pico will reconnect as
        // an RP2 Boot device, with its associated USB virtual disk drive.
        // The RP2 Boot device doesn't expose any form of unique ID
        // information for the Pico, so there's no way to know with
        // certainty which RP2 Boot device corresponds to the same
        // physical Pico that you were connected to via the Pinscape
        // vendor interface.  However, since the reboot into RP2 Boot
        // Loader mode is fast (typically less than 1 second), it's a good
        // bet that the next new RP2 Boot Loader device to attach to the
        // host after issuing this command is the same physical device
        // that you just explicitly rebooted.  So an easy algorithm is to
        // scan for RP2 Boot Loader devices just *before* sending the
        // reboot command, then scan again every second or so until a new
        // Boot Loader device appears that wasn't in the "before" list.
        static const uint8_t CMD_RESET = 0x04;
        static const uint8_t SUBCMD_RESET_NORMAL = 0x01;      // run firmware program in standard mode
        static const uint8_t SUBCMD_RESET_SAFEMODE = 0x02;    // run firmware program in Safe Mode
        static const uint8_t SUBCMD_RESET_BOOTLOADER = 0x03;  // reset into the Pico's native Boot Loader mode

        // Set the wall clock time.  This lets the host PC send the
        // current time of day and calendar date to the Pico.  The Pico
        // depends on the host for the wall clock time because the Pico
        // doesn't have a native clock/calendar; its internal clock
        // resets to zero every time the Pico is reset.  The wall clock
        // time is used on the Pico for timed events, as well as for
        // timestamps in message logging.
        static const uint8_t CMD_SET_CLOCK = 0x05;

        // Configuration file commands.  These all use the args.config
        // struct, with args.config.subcmd (the first argument byte)
        // set to a sub-command code specifying the request type.
        //
        // Most of the commands require args.config.fileID to be set to
        // one of the CONFIG_FILE_xxx identifiers to select which config
        // file to operate on.  Some commands (SUBCMD_CONFIG_ERASE)
        // accept CONFIG_FILE_ALL as a wildcard to target all of the
        // config file types.
        //
        // SUBCMD_CONFIG_GET
        //   Retrieve configuration (device-to-host).  This sends one 4K
        //   section of the configuration data from the device's flash
        //   memory back to the host, as the reply data block.  The
        //   config.page arguments specifies which 4K section is being
        //   requested.  (config.nPages is ignored.)  The last page might
        //   return less than a full 4K page.  If the page number is past
        //   the last page, the status returns with ERR_EOF.
        //
        //   Note that there are multiple configuration file types:
        //   main, safe mode.  Each file must be retrieved as a separate
        //   object.  Specify which file to retrieve via config.fileID
        //   in the request arguments.
        //
        //   If the requested config file doesn't exist, the status
        //   returns with ERR_NOT_FOUND.  If the file exists but is
        //   invalid (bad checksum, corrupted directory entry), returns
        //   ERR_CONFIG_INVALID.
        //
        // SUBCMD_CONFIG_PUT
        //   Store configuration (host-to-device).  This transfers one 4K
        //   block of a configuration file to the device, which the device
        //   stores in the selected section of flash.  Config files are
        //   text files containing JSON definitions, and can be of any
        //   size up to a limit set in the firmware (currently 128K).
        //   Since the file size can easily exceed the block size, this
        //   command is designed to be used in a simple protocol that
        //   transfers a whole file in 4K chunks:
        //
        //    - First, the host sends a "start of file" SUBCMD_CONFIG_PUT
        //      command, with the page number set to 0xFFFF, and no file
        //      data (that is, no extra transfer data).  The device sets
        //      up the internal data structures to write into flash.
        //
        //    - Next, the host breaks the file into 4K chunks, and sends
        //      a SUBCMD_CONFIG_PUT command for each chunk.  These must
        //      be sent in order, with page number 0x0000 for the first
        //      page, page 0x0001 for the second, and so on.
        //
        //   Upon receiving the last chunk, the device automatically
        //   validates the CRC-32 and closes the flash file.  It's not
        //   necessary to pad the file to a 4K boundary, since the last
        //   page can be of any size up to 4K.  The host must set the
        //   CRC-32 it calculates over the ENTIRE STREAM, so that the
        //   device can compare the host's CRC-32 calculation to its own,
        //   to verify that the file's contents were received intact.
        //
        //   The must always send the complete file, using the protocol
        //   above.  There's no way to update just one page in the middle
        //   of the file, since we don't want to require the host and
        //   device to be in sync on the file's old contents.
        //
        //   Pages must be sent in order from first to last, EXCEPT that
        //   the host is allowed to repeat any page any number of times.
        //   Repeats are specifically allowed so that the host can attempt
        //   to recover from USB transmission errors (such as failing to
        //   receive a reply to a PUT request) by resending a possibly
        //   failed page.  If the host sends the same page more than
        //   once, and the device has already successfully received and
        //   stored that page, the device will reply with ERR_RETRY_OK
        //   to indicate that the same page was already saved.  The
        //   host should count this as a success *if* the host actually
        //   was resending the page.  ERR_RETRY_OK in other cases would
        //   indicate a protocol state mismatch, so should be considered
        //   an error.
        //
        // SUBCMD_CONFIG_ERASE
        //   Erase the configuration file on the device.  This deletes the
        //   JSON configuration file, restoring factory defaults for all
        //   of the user-configurable settings.  This erases the file
        //   specified by args.config.fileID (other arguments field are
        //   ignored).  CONFIG_FILE_ALL can be used to erase all of the
        //   config file types.
        //
        // SUBCMD_CONFIG_EXISTS
        //   Check if the given configuration file exists.  Returns status
        //   OK if the file exists, ERR_NOT_FOUND if not.
        //
        // SUBCMD_CONFIG_TEST_CHECKSUM
        //   Test the configuration data stored in flash to determine if
        //   it's valid.  The device tests the checksum in the flash data,
        //   and returns a reply with status OK if so, ERR_NOT_FOUND if
        //   no config file exists, or ERR_CONFIG_INVALID if the checksum
        //   test fails.  Returns the computed checksum in args.checksum.
        //
        // SUBCMD_CONFIG_RESET
        //   Perform a "factory reset" by clearing the config data area
        //   in flash memory.  This simply erases the last page of the
        //   flash memory space, which has the effect of deleting any
        //   previously saved configuration data.  The firmware will revert
        //   to factory defaults on the next reset when it sees that there
        //   isn't a valid configuration file available in flash.  No
        //   file ID is required for this sub-command, since it inherently
        //   targets all of the config data.
        //
        static const uint8_t CMD_CONFIG = 0x06;
        static const uint8_t SUBCMD_CONFIG_GET = 0x01;
        static const uint8_t SUBCMD_CONFIG_PUT = 0x02;
        static const uint8_t SUBCMD_CONFIG_EXISTS = 0x03;
        static const uint8_t SUBCMD_CONFIG_ERASE = 0x04;
        static const uint8_t SUBCMD_CONFIG_TEST_CHECKSUM = 0x05;
        static const uint8_t SUBCMD_CONFIG_RESET = 0x06;
        
        // Config File IDs for CMD_CONFIG.  Two separate configuration
        // files can be stored on the device: the main config file, used
        // for normal operation, and the "Safe Mode" configuration, used
        // if the device unexpectedly resets within a couple of minutes
        // of a reset.  An unexpected reset usually indicates a software
        // fault that crashed the Pico.  If this happens soon after
        // startup, it's likely that the crash was due to software error
        // somewhere in the initialization process, in which case the
        // same error is likely to keep recurring on every subsequent
        // boot using the same configuration settings.  Safe Mode is
        // designed to try to prevent an infinite reset/crash/reset loop
        // in these cases by bypassing the potentially problematic
        // settings.  The Safe Mode config file lets the user provide
        // custom settings for these cases, so that you can enable a
        // small set of known-working features and devices without
        // loading the full config.

        // Configuration file ID constants, for args.config.fileID
        static const uint8_t CONFIG_FILE_MAIN = 0x00;
        static const uint8_t CONFIG_FILE_SAFE_MODE = 0x01;
        static const uint8_t CONFIG_FILE_ALL = 0xFF;   // wildcard - select all config files, for erase, etc

        // Configuration page size
        static const uint32_t CONFIG_PAGE_SIZE = 4096;

        // Nudge device commands.  The first byte of the request
        // arguments is a sub-command code selecting the action to
        // perform.  Additional argument bytes vary by sub-command.
        //
        // SUBCMD_NUDGE_CALIBRATE
        //   Begin nudge device noise calibration.  This starts a timed
        //   period of data collection to observe the noise range of the
        //   accelerometer axes under normal quiet conditions.  The
        //   device shouldn't be moved or otherwise disturbed during the
        //   calibration period, since the point is to profile the
        //   typical range of readings when the device is at rest.  The
        //   readings are used to set the threshold for detecting
        //   periods of stillness for the purposes of auto-centering the
        //   X/Y readings.  The second byte of the arguments (after the
        //   subcommand code) is a boolean value specifying whether or
        //   not to automatically commit the new readings to flash at
        //   the end of the timed calibration; 0 means don't save, 1
        //   means save, other values are undefined.  The calibration
        //   data go into effect in the live readings in either case,
        //   but if they're not saved to flash, the old settings will
        //   be restored at the next reboot or on an explicit "revert"
        //   command.  The new setting can be manually saved to flash
        //   with a "commit" command.
        //
        // SUBCMD_NUDGE_CENTER
        //   Set the center point for the X/Y readings to the average
        //   of recent readings over the past few seconds.  This forces
        //   auto-centering immediately, without waiting for a period of
        //   stillness.
        //
        // SUBCMD_NUDGE_QUERY_STATUS
        //   Query nudge device status.  Returns a NudgeStatus struct in
        //   the extra reply data.
        //
        // SUBCMD_NUDGE_QUERY_PARAMS
        // SUBCMD_NUDGE_PUT_PARAMS
        //   Query/set nudge device parameters.  Both commands use a
        //   NudgeParams struct in the extra transfer data; "put" sends the
        //   struct with the command transfer data, "query" returns it with
        //   the reply transfer data.  On "put", the new settings go into
        //   effect in the live data, but they're only stored in volatile
        //   RAM, so they'll be lost on the next reboot or on an explicit
        //   "revert" command.  Use the "commit" command to save the new
        //   settings to flash.
        //
        // SUBCMD_NUDGE_COMMIT
        //   Save the current in-memory settings to flash.  This saves
        //   the noise calibration data and parameter settings.
        //
        // SUBCMD_NUDGE_REVERT
        //   Reload the noise calibration data and parameter settings from
        //   the saved data in flash memory.  The loaded data replace any
        //   in-memory data set with a calibration run or with the PUT
        //   PARAMS command.
        //
        static const uint8_t CMD_NUDGE = 0x07;
        static const uint8_t SUBCMD_NUDGE_CALIBRATE = 0x01;
        static const uint8_t SUBCMD_NUDGE_CENTER = 0x02;
        static const uint8_t SUBCMD_NUDGE_QUERY_STATUS = 0x03;
        static const uint8_t SUBCMD_NUDGE_QUERY_PARAMS = 0x04;
        static const uint8_t SUBCMD_NUDGE_PUT_PARAMS = 0x05;
        static const uint8_t SUBCMD_NUDGE_COMMIT = 0x06;
        static const uint8_t SUBCMD_NUDGE_REVERT = 0x07;

        // TV ON commands.  The first byte of the request arguments is
        // a sub-command code selecting the action to perform.  Additional
        // argument bytes vary by sub-command.
        //
        // SUBCMD_TVON_QUERY_STATE
        //   Query the state of the TV ON module.  The args.tvon struct
        //   in the reply arguments is populated with the current TV ON
        //   state.  (See the struct definition for details.)
        //
        // SUBCMD_TVON_SET_RELAY
        //   Manually set the TV ON relay state.  The second byte of the
        //   request arguments (following the sub-command byte) has one
        //   of the following values:
        //
        //      TVON_RELAY_OFF    - turn the relay on
        //      TVON_RELAY_ON     - turn the relay on
        //      TVON_RELAY_PULSE  - pulse the relay on for the configured pulse interval
        //      other             - ignored
        //
        static const uint8_t CMD_TVON = 0x0A;
        static const uint8_t SUBCMD_TVON_QUERY_STATE = 0x01;
        static const uint8_t SUBCMD_TVON_SET_RELAY = 0x02;

        // TV ON relay states, for SUBCMD_TVON_SET_RELAY
        static const uint8_t TVON_RELAY_OFF = 0x01;
        static const uint8_t TVON_RELAY_ON = 0x02;
        static const uint8_t TVON_RELAY_PULSE = 0x03;

        // Statistics commands.  The first byte of the request arguments
        // is a sub-command code selecting the action to perform.  Additional
        // argument bytes vary by sub-command.
        //
        // SUBCMD_STATS_QUERY_STATS
        //   Query global statistics.  Returns data in the additional transfer
        //   bytes, as a Statistics struct.  The second byte of the arguments
        //   contains bit flags, as a combination of QUERYSTATS_FLAG_xxx bits
        //   defined below:
        //
        //     QUERYSTATS_FLAG_RESET_COUNTERS
        //       If set, the rolling-average counters are reset to start a
        //       new averaging window.
        //
        // SUBCMD_STATS_QUERY_CLOCK
        //   Query the Pico system clock.  This returns two UINT64 values
        //   packed into the reply arguments, representing times on the Pico
        //   system clock, expressed in microseconds since the last CPU reset.
        //   The first UINT64 is the clock reading at the start of the Pico's
        //   processing of the query request, and the second is the updated
        //   clock reading just before queueing the USB reply packet.  The
        //   two timestamps allow the caller to bracket the time spent in the
        //   USB transmission and processing of the USB packet on the Pico
        //   and Windows sides, for a more accurate estimate on the Windows
        //   side of the real-time offset between the Windows and Pico clocks.
        //
        //   The main purpose of this query is to allow the Windows host to
        //   determine the Pico clock time that corresponds to any given
        //   Windows clock time, so that the host can meaningfully measure
        //   the elapsed time between events with Pico timestamps and events
        //   with Windows timestamps.  For example, this can be used to
        //   precisely measure the latency between a hardware event on the
        //   Pico, such as a button state change, and the time that a HID
        //   report for the event reaches the PC application layer.
        //
        // SUBCMD_STATS_PREP_QUERY_CLOCK
        //   This command tells the Pico to expect a clock query command
        //   immediately following.  The Pico goes into a polling loop for
        //   the next command, without returning to its normal main loop
        //   processing.  This is designed to make the timing of the clock
        //   query processing as deterministic as possible, by eliminating
        //   the uncertainty of the time it takes to go through the main
        //   loop from the equation.  The caller is expected to send the
        //   clock query command immediately.  To avoid locking up the
        //   Pico, the immediate polling mode exits if no new command is
        //   received within a few milliseconds.
        //
        static const uint8_t CMD_STATS = 0x0B;
        static const uint8_t SUBCMD_STATS_QUERY_STATS = 0x01;
        static const uint8_t SUBCMD_STATS_QUERY_CLOCK = 0x02;
        static const uint8_t SUBCMD_STATS_PREP_QUERY_CLOCK = 0x03;

        // flags for CMD_STATE + SUBCMD_STATS_QUERY_STATS
        static const uint8_t QUERYSTATS_FLAG_RESET_COUNTERS = 0x01;

        // Read data from the in-memory message logger.  The firmware
        // can be configured to save logging messages in memory for
        // later retrieval via the vendor interface.  This retrieves
        // as much logging information as is available and can fit in
        // the returned message.  The log text is returned as plain
        // 8-bit text in the reply data transfer.  If no logging text
        // is available, returns ERR_EOF status.
        //
        // The log.avail field in the reply contains the total number
        // of bytes currently available in the log data on the device.
        // This lets the client distinguish log data that was already
        // available when it started retrieving the log from new log
        // messages that were added during the retrieval loop.  A
        // client that runs interactively can ignore this, since an
        // interactive client will usually want to just keep showing
        // new messages as they arrive until the user closes the
        // window (or equivalent).  A client that runs as a one-shot
        // check on the log can use the available size at the start
        // of its polling loop to stop after it has retrieved all of
        // the messages that were available when it first looked, so
        // that it doesn't keep looping forever if new messages are
        // added in the course of the retrieval loop.
        static const uint8_t CMD_QUERY_LOG = 0x0C;

        // Send an ad hoc IR command.  Teh command is passed in
        // the arguments via args.sendIR.
        static const uint8_t CMD_SEND_IR = 0x0D;

        // Flash storage access commands.  These commands provide
        // access to the flash for exporting data for backups and
        // image transfers.  The first byte of the arguments is a
        // subcommand code, which selects the specific function to
        // perform.  Additional argument bytes vary by command.
        // Subcommand codes:
        //
        // SUBCMD_FLASH_READ_SECTOR
        //   Read a specified flash sector.  The sector is sent to
        //   the client as the extra transfer data in the reply.
        //   The sector is selected via args.flash.ofs, which
        //   contains a byte offset in flash, starting at 0 for the
        //   first sector of flash.  The reply arguments contain
        //   a CRC-32 of the sector at args.flash.crc32, to allow
        //   the host to check the integrity of the transmission.
        //
        // SUBCMD_FLASH_QUERY_FILESYS
        //   Get information on the mini file system that the
        //   firmware uses to store persistent configuration,
        //   settings, and calibration data.  The reply transfer
        //   contains a FlashFileSysInfo struct describing the
        //   file system.
        //
        static const uint8_t CMD_FLASH_STORAGE = 0x0E;
        static const uint8_t SUBCMD_FLASH_READ_SECTOR = 0x01;
        static const uint8_t SUBCMD_FLASH_QUERY_FILESYS = 0x02;

        // Plunger control/query command.  This command invokes various
        // plunger functions.  The first byte of the arguments is a
        // subcommand code, which selects the specific operation to
        // perform.  Additional argument bytes vary by subcommand.
        // Subcommand byte codes:
        //
        // SUBCMD_PLUNGER_CALIBRATE
        //   Begin plunger calibration.  This puts the device into
        //   calibration mode, during which it monitors sensor input
        //   to determine the sensor readings corresponding to the
        //   plunger's resting position and maximum retraction point.
        //   The user should be guided to initiate this mode with the
        //   plunger *at the resting position*, and once the mode is
        //   initiated, the user should pull back the plunger all the
        //   way, hold it there for a moment, release it, let it come
        //   to rest, and then repeat a few times.  The mode runs on a
        //   timer and automatically ends after about 15 seconds.  The
        //   host-side application can monitor the mode state via the
        //   SUBCMD_PLUNGER_QUERY_READING subcommand.
        //
        //   args.plungerByte contains a flag byte specifying the
        //   auto-save mode to use when the calibration time period
        //   ends.  0 means that the new calibration is temporary;
        //   it's put into effect immediately on the live readings,
        //   but is only stored in volatile RAM on the Pico, so it can
        //   be reverted explicitly by a SUBCMD_PLUNGER_REVERT_SETTINGS
        //   request, or automatically by a Pico reset.  If the flag
        //   byte is 1, the new calibration is immediately committed
        //   to flash, along with the other adjustable settings.
        //
        // SUBCMD_PLUNGER_SET_JITTER_FILTER
        //   Sets the jitter filter window via args.jitterFilter.  The
        //   new setting is only stored in memory, to allow the user to
        //   experiment with different settings interactively before
        //   updating flash.  Use SUBCMD_PLUNGER_COMMIT_SETTINGS to save
        //   the in-memory changes to flash.
        //
        // SUBCMD_PLUNGER_SET_FIRING_TIME_LIMIT
        //   Sets the firing time limit via args.plungerInt.  The time
        //   is given in microseconds.  The special value 0 selects the
        //   default setting.  The new setting is only stored in memory;
        //   use SUBCMD_PLUNGER_COMMIT_SETTINGS to save the new setting
        //   to flash.
        //
        // SUBCMD_PLUNGER_SET_INTEGRATION_TIME
        //   Sets the integration time, in microseconds, via args.plungerInt.
        //   This is only meaningful with certain imaging sensors; for those
        //   sensors, it adjusts the exposure time per image frame, to
        //   optimize image quality according to the lighting conditions
        //   in the individual installation.  This is suported for TCD1103
        //   and TSL1410R.  The valid time range varies by device; see the
        //   device implementations (under Chips/LinearPhotoSensor) for
        //   individual limits.  Note that the exposure time sets an upper
        //   bound on the frame rate for measuring plunger positions, so
        //   exposure times shouldn't exceed about 4ms to ensure that the
        //   frame rate remains high enough for fast position readings.
        //   A value of 0 sets the default for the sensor.  As with other
        //   settings, the new exposure time is only stored in memory;
        //   use SUBCMD_PLUNGER_COMMIT_SETTINGS to save changes to flash.
        //
        // SUBCMD_PLUNGER_SET_ORIENTATION
        //   Set orientation via args.plungerByte.  Set the byte argument
        //   to 0 for standard orientation, 1 for reverse orientation.
        //   Like other settings, this is stored in-memory only; use
        //   SUBCMD_PLUNGER_COMMIT_SETTINGS to save changes to flash.
        //
        // SUBCMD_PLUNGER_SET_SCALING_FACTOR
        //   Set the manual scaling factor, which is a percentage value
        //   to apply to the logical position reading.  This can be used
        //   to tweak the results of the automatic calibration process
        //   as desired.
        //
        // SUBCMD_PLUNGER_SET_SCAN_MODE
        //   Sets the scan mode to the value in args.plungerByte.  Some
        //   imaging sensors provide multiple image analysis algorithms,
        //   because some of the algorithms have tradeoffs that make the
        //   best algorithm vary by installation.  This command lets the
        //   user select the desired mode for applicable sensors.  The
        //   meaning of the mode byte is specific to the sensor.
        //   Currently, this is only applicable to TSL1410S/1412R, where
        //   the mode selects an edge-detection algorithm.  Refer to the
        //   sensor code for details on the available options.  Set this
        //   to 0 to select the default mode for any sensor.
        //
        // SUBCMD_PLUNGER_SET_CAL_DATA
        //   Sets calibration data saved on the host side.  The host can
        //   save calibration data reported through QUERY_READING, and
        //   put it back into effect later via this request.  As with the
        //   other settings requests, the new calibration is stored in
        //   memory until settings are explicitly committed.  Note that
        //   calibration settings are inherently sensor-specific, so the
        //   host should only apply calibration data taken from the same
        //   sensor type as currently configured.  The new settings are
        //   passed in the extra transfer data, via struct PlungerCal.
        //
        // SUBCMD_PLUNGER_COMMIT_SETTINGS
        //   Commits the current adjustable plunger settings (jitter
        //   filter, orientation, scaling factor, integration time, firing
        //   time limit) and calibration data to Pico flash memory.
        //
        // SUBCMD_PLUNGER_REVERT_SETTINGS
        //   Reverts the adjustable settings and calibration data to the
        //   last settings stored in flash.
        //
        // The following QUERY subcommands retrieve data structures with
        // current plunger data.  Each subcommand returns a struct
        // specific to that subcommand in the extra data transfer part of
        // the reply.
        //
        // SUBCMD_PLUNGER_QUERY_READING
        //   Retrieve the current sensor reading via struct PlungerReading.
        //   Depending on the sensor, the response might include additional
        //   data following the PlungerReading struct:
        //
        //    - Imaging sensors append a snapshot of the sensor image
        //
        // SUBCMD_PLUNGER_QUERY_CONFIG
        //   Retrieve config settings via struct PlungerConfig
        //
        static const uint8_t CMD_PLUNGER = 0x0F;
        static const uint8_t SUBCMD_PLUNGER_CALIBRATE = 0x01;
        static const uint8_t SUBCMD_PLUNGER_SET_JITTER_FILTER = 0x02;
        static const uint8_t SUBCMD_PLUNGER_SET_FIRING_TIME_LIMIT = 0x03;
        static const uint8_t SUBCMD_PLUNGER_SET_INTEGRATION_TIME = 0x04;
        static const uint8_t SUBCMD_PLUNGER_SET_ORIENTATION = 0x05;
        static const uint8_t SUBCMD_PLUNGER_SET_SCALING_FACTOR = 0x06;
        static const uint8_t SUBCMD_PLUNGER_SET_CAL_DATA = 0x07;
        static const uint8_t SUBCMD_PLUNGER_SET_SCAN_MODE = 0x08;
        static const uint8_t SUBCMD_PLUNGER_COMMIT_SETTINGS = 0x40;
        static const uint8_t SUBCMD_PLUNGER_REVERT_SETTINGS = 0x41;
        static const uint8_t SUBCMD_PLUNGER_QUERY_READING = 0x81;
        static const uint8_t SUBCMD_PLUNGER_QUERY_CONFIG = 0x82;

        // Button input tests.  This command invokes subcommands for
        // testing the button inputs.  The first byte of the arguments
        // selects the subcommand to perform; additional argument bytes
        // vary by subcommand.  Subcommand byte codes:
        //
        // SUBCMD_BUTTON_QUERY_DESCS
        //   Retrieve a list of the logical button assignments, which
        //   represent the abstract button inputs in the configuration
        //   that map between the physical inputs and button actions,
        //   such as keyboard input and IR transmissions.  The extra
        //   transfer data on return contains a ButtonList struct,
        //   followed by an array of ButtonDesc structs that describe
        //   the current logical buttons.
        //
        // SUBCMD_BUTTON_QUERY_STATES
        //   Retrieve the current button states for the logical buttons.
        //   The extra transfer data on return contains an array of the
        //   logical button states.  Each button is represented by one
        //   byte in the results, 0 for OFF, 1 for ON.  The logical button
        //   state is the final states reported to the host after taking
        //   into account shift states, pulse timers, and so on, so the
        //   logical state doesn't always match the physical state of the
        //   underlying input source.
        //
        //   In addition, args.buttonState returns the global shift
        //   button state.  Each bit in globalShiftState represents a
        //   shift mode, which can be assigned to one or more buttons
        //   of type TYPE_SHIFT.  Pressing that button activates the
        //   shift mode.  The bits in globalShiftState indicates the
        //   combination of shift buttons currently being pressed, which
        //   in turn selects which shifted buttons can be activated.
        //
        // SUBCMD_BUTTON_QUERY_GPIO_STATES
        //   Retrieves the current input states of all of the Pico GPIO
        //   ports.  All GPIO ports are reported whether, or not they're
        //   mapped to buttons.  The port states are returned in the extra
        //   transfer data, one byte per port starting at GP0, 0 for LOW,
        //   1 for HIGH.  This gives the physical state of each port; it
        //   doesn't apply the Active High/Low polarity setting for any
        //   button mapped onto the port.
        //
        // SUBCMD_BUTTON_QUERY_PCA9555_STATES
        //   Retrieves the current input states of all PCA9555 chips.
        //   Returns the states in the extra transfer data, one byte per
        //   port, arranged in order of the chips in the configuration
        //   list.  Each chip has a fixed 16 ports, so it contributes
        //   16 bytes to the returned data.  Each byte represents the
        //   state of one port, 0 for LOW, 1 for HIGH.  This is the raw
        //   port state; it doesn't apply the Active High/Low polarity
        //   of any buttons mapped onto the port.
        //
        // SUBCMD_BUTTON_QUERY_74HC165_STATES
        //   Retrieves the current input state of all 74HC165 chips.
        //   Returns the states in the extra transfer data, one byte per
        //   port, arranged in order of the daisy chains in the config
        //   list.  Each chip has a fixed 8 ports, so it contributes 8
        //   bytes to the returned data.  Each byte represents the state
        //   of one port, 0 for LOW, 1 for HIGH.  This is the raw port
        //   state; it doesn't apply the Active High/Low polarity of any
        //   buttons mapped onto the port.
        //
        // SUBCMD_BUTTON_QUERY_EVENT_LOG
        //   Retrieves the event log for a GPIO button.  Event logging
        //   is a performance measurement tool designed primarily for
        //   measuring the elapsed time (the latency) between a physical
        //   button press and the appearance on the host of the
        //   corresponding HID report.  GPIO buttons are uniquely suited
        //   for this measurement, because a Pico GPIO can be configured
        //   to generate a hardware interrupt on the Pico every time the
        //   voltage level on the pin transitions between high and low
        //   logic level, and the Pico has very low latency servicing
        //   these interrupts, on the order of 200ns.  This lets us
        //   very precisely mark the time when a physical button press
        //   or release occurs, which is what the event log captures.
        //   The PC host can then correlate the timestamps in the event
        //   log with the timestamps of the incoming HID reports that
        //   represent the button presses, and calculate the HID report
        //   latency from the time difference.  Note that calculating
        //   the interval between a host event and a Pico event requires
        //   a common reference point in time, which can be obtained via
        //   the CMD_STATS + SUBCMD_STATS_QUERY_CLOCK command.
        //
        //   The second byte of the command arguments (following the
        //   subcommand code byte in the first arguments byte) gives
        //   the GPIO number to query.  The event log is returned in
        //   the extra transfer data, as a ButtonEventLog struct,
        //   followed by zero or more ButtonEventLogItem structs.
        //   Items in the list are ordered from oldest to newest.
        //
        //   Retrieving events with this request deletes the events
        //   from the Pico's internal buffer, so each request will
        //   retrieve only new events that have occurred since the last
        //   request.  The log buffer on the Pico has a fixed size;
        //   when the buffer is full, the oldest event is discarded
        //   each time a new event is added.  This means that there's
        //   no need for the host to read events when it doesn't need
        //   them; there's no harm in "overflowing" the buffer because
        //   the Pico will just discard old events as needed to make
        //   room.  This also means that the host program sees the
        //   most recent events when it starts running, no matter how
        //   long the Pico has already been collecting events.
        //
        // SUBCMD_BUTTON_CLEAR_EVENT_LOG
        //   Clears the Pico buffer containing the event log for a
        //   GPIO button.  The second byte of the command arguments
        //   (following the subcommand code) gives the GPIO number
        //   for the event log to clear.  Specifying GPIO=255 clears
        //   the logs for all GPIOs.
        //
        static const uint8_t CMD_BUTTONS = 0x10;
        static const uint8_t SUBCMD_BUTTON_QUERY_DESCS = 0x81;
        static const uint8_t SUBCMD_BUTTON_QUERY_STATES = 0x82;
        static const uint8_t SUBCMD_BUTTON_QUERY_GPIO_STATES = 0x83;
        static const uint8_t SUBCMD_BUTTON_QUERY_PCA9555_STATES = 0x84;
        static const uint8_t SUBCMD_BUTTON_QUERY_74HC165_STATES = 0x85;
        static const uint8_t SUBCMD_BUTTON_QUERY_EVENT_LOG = 0xA0;
        static const uint8_t SUBCMD_BUTTON_CLEAR_EVENT_LOG = 0xA1;

        // Feedback output device tests.  This command invokes
        // subcommands for testing the feedback devices.  The first byte
        // of the arguments selects the subcommand to perform;
        // additional argument bytes vary by subcommand.  Subcommand
        // byte codes:
        //
        // SUBCMD_OUTPUT_SET_PORT
        //   Set the level for a logical port.  The second byte of the
        //   arguments (after the subcommand byte) gives the logical
        //   port number (using the nominal numbering, starting at
        //   port #1), and the third byte gives the PWM level to set.
        //   This is equivalent to exercising the port through DOF or
        //   other programs that access the Feedback Controller HID
        //   interface.  Logical port levels have no effect when test
        //   mode is engaged, because test mode disconnects the physical
        //   output devices from the logical ports.
        //
        // SUBCMD_OUTPUT_TEST_MODE
        //   Enables or disables output test mode.  Test mode decouples
        //   the physical output controller peripherals (outboard PWM
        //   controller chips, shift register chips, and the Pico GPIO
        //   ports) from the Output Manager subsystem, allowing the
        //   physical device output ports to be exercised directly,
        //   without any of the logical output port mappings involved.
        //   This decouples the ports from host DOF control, port timers
        //   (Flipper and Chime Logic), and computed outputs.  This is
        //   useful for testing and troubleshooting the outboard chips
        //   and physical wiring, since it bypasses all of the Output
        //   Manager software logic, eliminating that as the source of
        //   any problems, isolating debugging to the physical wiring
        //   and the low-level device driver software only.  When test
        //   mode is enabled, all of the Output Manager's protective
        //   timers (Chime Logic, Flipper Logic) are disabled, so the
        //   host and user should exercise caution with ports that
        //   would be damaged by prolonged activation.
        //
        //   The arguments to this command are in args.outputTestMode.
        //   Set 'enable' to 1 to enter test mode, 0 to resume normal
        //   Output Manager control.  'timeout' in the arguments sets
        //   the time to stay in this mode before normal operation is
        //   automatically resumed, for protection against host-side
        //   application crashes.  The application should use this by
        //   setting a fairly short timeout, preferably on the order
        //   of a few seconds, and then repeating the command
        //   periodically before the timeout expires as long as the
        //   host wishes to stay in this mode.  This ensures that the
        //   device will return to normal operation within a few
        //   seconds if the application terminates abruptly.
        //
        // SUBCMD_OUTPUT_SET_DEVICE_PORT
        //   Sets the level for a physical port.  This command can only
        //   be used when test mode is engaged, since it addresses the
        //   physical device ports directly, and the Output Manager will
        //   typically countermand any direct port setting within a few
        //   milliseconds if it's not disconnected via test mode.
        //   Arguments are in args.outputDevPort.
        //
        // SUBCMD_OUTPUT_PWMWORKER_RESET
        // SUBCMD_OUTPUT_PWMWORKER_BOOTLOADER
        //   Resets a PWMWorker Pico.  The _RESET command performs a CPU
        //   reset, restarting the firmware; the _BOOTLOADER command puts
        //   the Pico in Boot Loader mode.  The unit is selected by the
        //   second byte of the arguments (following the subcommand code),
        //   as an index into the 'workerPico' configuration array.
        //
        // SUBCMD_OUTPUT_QUERY_LOGICAL_PORTS
        //   Retrieve a list of the logical output ports, which represent
        //   the ports visible to the host through the Feedback Controller
        //   HID interface.  These are the ports as they appear to DOF and
        //   other simulator programs that use the ports to trigger feedback
        //   effects.  The extra transfer data on return contains an
        //   OutputPortList struct, which acts as a list header.  This is
        //   followed in the same transfer by an array of OutputPortDesc
        //   structs that describe the individual logical output ports.
        //
        // SUBCMD_OUTPUT_QUERY_DEVICES
        //   Retrieve a list of the output controller devices.  These are
        //   the physical peripheral devices that control wired outputs:
        //   PWM controllers, output shift registers, and Pico GPIO ports.
        //   The extra transfer data on return contains an OutputDevList
        //   struct, which acts as a list header.  This is followed in the
        //   same transfer block by an array of OutputDevDesc structs that
        //   describe the individual devices.  Each OutputDevDesc struct
        //   is followed by an array of OutputDevPortDesc structs that
        //   describe the individual port usages.
        //
        // SUBCMD_OUTPUT_QUERY_DEVICE_PORTS
        //   Retrieves port-level details for the output devices, to
        //   supplement the device descriptors.  The reply transfer data
        //   contains an OutputDevPortList struct as a list header,
        //   followed by a packed array of OutputDevPortDesc structs,
        //   one per port.  The ports are arranged in the same order as
        //   the device descriptors from SUBCMD_OUTPUT_QUERY_DEVICES,
        //   so that list can be used to associate individual ports with
        //   their devices.
        //
        //   For most of the output devices, all ports are identical,
        //   since the port characteristics that we're interested in
        //   are features of the hardware that can't be configured on
        //   most of the device types.  However, a few devices do have
        //   configurable ports.  The Pico GPIO ports can be configured
        //   individually as inputs, digital outputs, or PWM outputs;
        //   PCA9555 ports can be configured as inputs or digital outs.
        //   The host might wish to know about those port-by-port
        //   configuration differences in some cases.
        //
        // SUBCMD_OUTPUT_QUERY_LOGICAL_PORT_LEVELS
        //   Retrieves the current logical port levels.  The reply
        //   transfer data consists of an OutputLevelList struct as a
        //   list header, followed by an array of OutputLevel elements,
        //   one per port.
        //
        // SUBCMD_OUTPUT_QUERY_DEVICE_PORT_LEVELS
        //   Query the output levels for the physical output devices.
        //   The reply transfer data contains an OutputDevLevelList struct
        //   as a list header, followed by an array of OutputDevLevel
        //   structs giving the current PWM level of each device port.
        //   The level values are in the same order as the ports reported
        //   from SUBCMD_OUTPUT_QUERY_DEVICE_PORTS, so that list can be
        //   used to map the level elements to port descriptors.
        static const uint8_t CMD_OUTPUTS = 0x11;
        static const uint8_t SUBCMD_OUTPUT_SET_PORT = 0x01;
        static const uint8_t SUBCMD_OUTPUT_TEST_MODE = 0x02;
        static const uint8_t SUBCMD_OUTPUT_SET_DEVICE_PORT = 0x03;
        static const uint8_t SUBCMD_OUTPUT_PWMWORKER_RESET = 0x70;
        static const uint8_t SUBCMD_OUTPUT_PWMWORKER_BOOTLOADER = 0x71;
        static const uint8_t SUBCMD_OUTPUT_QUERY_LOGICAL_PORTS = 0x81;
        static const uint8_t SUBCMD_OUTPUT_QUERY_DEVICES = 0x82;
        static const uint8_t SUBCMD_OUTPUT_QUERY_DEVICE_PORTS = 0x83;
        static const uint8_t SUBCMD_OUTPUT_QUERY_LOGICAL_PORT_LEVELS = 0x84;
        static const uint8_t SUBCMD_OUTPUT_QUERY_DEVICE_PORT_LEVELS = 0x85;

        // Ping.  This can be used to test that the connection is working
        // and the device is responsive.  This takes no arguments, and
        // simply sends back an OK status with no other effects.
        static const uint8_t CMD_PING = 0x12;

        // Query GPIO port configuration.  Returns a GPIOConfig struct
        // in the reply transfer data.
        static const uint8_t CMD_QUERY_GPIO_CONFIG = 0x13;

        // Query the IR receiver.  The first byte of the arguments is
        // a sub-command code giving the specific query request:
        //
        // SUBCMD_QUERY_IR_CMD
        //   Query decoded IR commands.  The reply transfer data starts
        //   with an IRCommandList struct describing the size and number
        //   of IRCommandListEle structs that follow.  The rest of the
        //   transfer is a packed array of IRCommandListEle structs
        //   listing the commands received since the last request.  The
        //   device keeps a ring buffer of recent commands, so old
        //   commands are discarded as new commands arrive if they're
        //   not queried first.  This request clears the buffer, so the
        //   next request will only fetch commands that have arrived since
        //   the last request.
        //
        // SUBCMD_QUERY_IR_RAW
        //   Query raw IR pulses.  The reply transfer data starts with
        //   an IRRawList struct describing the size and number of IRRaw
        //   structs that follow.  The IRRaw structs describe the series
        //   of individual IR pulses recently received.  The device saves
        //   each string of consecutive pulses, up to an internal storage
        //   limit, as it receives them.  The buffer is cleared at the
        //   start of each new code, which is delimited by a long quiet
        //   period (of at least 130ms) since the last pulse, and pulses
        //   in a consecutive series that overflow the buffer are simply
        //   discarded.  This request also clears the buffer, so the next
        //   request will only see new pulses since the last request.
        //
        static const uint8_t CMD_QUERY_IR = 0x14;
        static const uint8_t SUBCMD_QUERY_IR_CMD = 0x01;
        static const uint8_t SUBCMD_QUERY_IR_RAW = 0x02;


        // Length of the arguments union data, in bytes.  This is the number
        // of bytes of data in the arguments union that are actually used.
        // At the USB level, the request packet is of fixed length, so the
        // whole args union is always transmitted, but this specifies how
        // many bytes of it are actually meaningful for the command.
        uint8_t argsSize = 0;

        // Transfer length.  If the request requires additional data beyond
        // what can fit in the arguments to be transmitted host-to-device,
        // this specifies the length of the additional data, which the host
        // sends as another packet immediately following the request.  If
        // this is zero, there's not another packet.
        uint16_t xferBytes = 0;

        // Arguments.  This allows a small amount of parameter data to be
        // provided directly with the request packet, rather than as an
        // additional transfer.  The meaning of this section is specific
        // to each command code, and many commands don't use it at all.
        // The host must set argsSize to the number of bytes of the union
        // that are actually used for the request; if no arguments are
        // used, set argsSize to 0.
        union Args
        {
            // raw byte view of the argument data
            uint8_t argBytes[16];

            // Configuration data descriptor, for CMD_CONFIG with SUBCMD_CONFIG_PUT, SMBCMD_CONFIG_GET
            struct __PackedBegin Config
            {
                uint8_t subcmd;      // subcommand code - a SUBCMD_CONFIG_xxx constant
                uint8_t fileID;      // file ID - one of the CONFIG_FILE_xxx constants
                uint16_t page;       // page number, starting at zero (for GET and PUT)
                uint16_t nPages;     // total number of 4K pages (for PUT)
                uint16_t reserved0;  // reserved/padding
                uint32_t crc;        // CRC-32 checksum of entire file contents (for last page of PUT)
            } __PackedEnd config;

            // Ad hoc IR command data, for CMD_SEND_IR
            struct __PackedBegin SendIR
            {
                uint64_t code;       // command code
                uint8_t protocol;    // protocol ID
                uint8_t flags;       // flags (0x02 -> use dittos)
                uint8_t count;       // repeat count
            } __PackedEnd sendIR;

            // clock time, for CMD_SET_CLOCK
            struct __PackedBegin Clock
            {
                int16_t year;        // nominal calendar year (e.g., 2024)
                uint8_t month;       // calendar month, 1-12
                uint8_t day;         // calendar day, 1-31
                uint8_t hour;        // hour, 0-23 (24-hour clock time)
                uint8_t minute;      // minute, 0-59
                uint8_t second;      // second, 0-59
            } __PackedEnd clock;

            // Plunger jitter filter settings, for SUBCMD_PLUNGER_SET_JITTER_FILTER
            struct __PackedBegin JitterFilter
            {
                uint8_t subcmd;      // subcommand - always SUBCMD_PLUNGER_SET_JITTER_FILTER
                uint8_t reserved;    // reserved (for alignment); set to zero
                uint16_t windowSize; // jitter filter window size, in native sensor units
            } __PackedEnd jitterFilter;

            // Plunger INT32 argument
            struct __PackedBegin PlungerInt
            {
                uint8_t subcmd;      // subcommand
                uint8_t reserved[3]; // reserved (for alignment); set to zero
                uint32_t u;          // UINT32 argument
            } __PackedEnd plungerInt;

            // Flash storage sector transfer arguments, for CMD_FLASH_STORAGE + SUBCMD_FLASH_READ_SECTOR
            struct __PackedBegin Flash
            {
                uint8_t subcmd;      // subcommand code
                uint8_t reserved[3]; // reserved (for alignment); set to zero
                uint32_t ofs;        // byte offset in flash storage of start of sector to read
            } __PackedEnd flash;

            // Byte argument, for SUBCMD_PLUNGER_SET_ORIENTATION
            struct __PackedBegin PlungerByte
            {
                uint8_t subcmd;      // subcommand
                uint8_t b;           // byte value; interpretation varies by command
            } __PackedEnd plungerByte;

            // Output test mode command arguments, for CMD_OUTPUTS + SUBCMD_OUTPUT_TEST_MODE
            struct __PackedBegin OutputTestMode
            {
                uint8_t subcmd;      // subcommand - SUBCMD_OUTPUT_TEST_MODE
                uint8_t enable;      // 1 = enable test mode, 0 = resume normal operation
                uint16_t reserved0;  // reserved/padding
                
                // Timeout in milliseconds; used only when enabling the mode (enable == 1).
                // Normal operation resumes after the timeout expires.  0 enables the
                // mode indefinitely, until explicitly disabled with another command.
                uint32_t timeout_ms;
            } __PackedEnd outputTestMode;

            // Output device physical port arguments, for CMD_OUTPUTS + SUBCMD_OUTPUT_SET_DEVICE_PORT
            struct __PackedBegin OutputDevPort
            {
                // subcommand - SUBCMD_OUTPUT_SET_DEVICE_PORT
                uint8_t subcmd;
                
                // Device type, as an OutputPortDesc::DEV_xxx constant
                uint8_t devType;

                // Device configuration index.  Together with the device type, this
                // identifies the targeted device.
                uint8_t configIndex;

                // Port number on the physical device.  Ports are mapped in the standard
                // order for the chip.  For 74HC595 daisy chains, the ports are numbered
                // consecutively across the whole chain starting with 0 for the first port
                // on the first chip in the chain (the "first" chip is the one directly
                // connected to the Pico).
                uint8_t port;

                // PWM level to set.  This uses the physical device's PWM scale:
                //
                //  Pico GPIO:  0-1 (digital mode), 0-255
                //  PWMWorker:  0-255
                //  TLC59116:   0-255
                //  TLC5940:    0-4095
                //  PCA9685:    0-4095
                //  74HC595:    0-1 (digital mode), 0-255 (PWM mode)
                //  PCA9555:    0-1
                //
                // Anything above the maximum level for the device will be clipped to
                // the maximum.
                uint16_t pwmLevel;

            } __PackedEnd outputDevPort;
        } args;
    } __PackedEnd;

    // Plunger calibration settings, for SUBCMD_PLUNGER_SET_CAL_DATA.  This is
    // sent via the extra transfer data.
    struct __PackedBegin PlungerCal
    {
        // structure size, for version sensing
        uint16_t cb;

        // flags
        uint16_t flags;
        static const uint16_t F_CALIBRATED = 0x0001;  // set -> calibration data is valid

        // range of raw sensor readings
        uint32_t calMin;
        uint32_t calZero;
        uint32_t calMax;

        // firing time measurement, microseconds
        uint32_t firingTimeMeasured;

        // extra sensor-specific data; clients should treat this as opaque
        uint32_t sensorData[8];
        
    } __PackedEnd;

    // Device-to-host response format.  The device responds to each request
    // with this struct to indicate the result of the request.
    struct __PackedBegin VendorResponse
    {
        // Token.  This is the token from the host's request packet, so that
        // the host can correlate the response to the original request.
        uint32_t token;

        // Command code.  This is the same command specified in the
        // corresponding request.
        uint8_t cmd;

        // Argument size.  This is the number of bytes of the arguments
        // union that are populated in the response.  This is set to 0
        // if the response has no arguments.
        uint8_t argsSize;

        // Status code
        uint16_t status;

        //
        // Status codes
        //

        // success
        static const uint16_t OK = 0;

        // general failure
        static const uint16_t ERR_FAILED = 1;

        // client-side timeout on USB transaction
        static const uint16_t ERR_TIMEOUT = 2;

        // transfer length out too long
        static const uint16_t ERR_BAD_XFER_LEN = 3;

        // USB transfer failed
        static const uint16_t ERR_USB_XFER_FAILED = 4;

        // parameter error (missing or bad argument in function call)
        static const uint16_t ERR_BAD_PARAMS = 5;

        // invalid command code
        static const uint16_t ERR_BAD_CMD = 6;

        // invalid subcommand code (for commands that have subcommand variations)
        static const uint16_t ERR_BAD_SUBCMD = 7;

        // mismatched reply for request
        static const uint16_t ERR_REPLY_MISMATCH = 8;

        // config transfer timeout
        static const uint16_t ERR_CONFIG_TIMEOUT = 9;

        // config data invalid
        static const uint16_t ERR_CONFIG_INVALID = 10;

        // out of bounds
        static const uint16_t ERR_OUT_OF_BOUNDS = 11;

        // not ready for current operation
        static const uint16_t ERR_NOT_READY = 12;

        // end of file
        static const uint16_t ERR_EOF = 13;

        // data or data format error in request/reply data
        static const uint16_t ERR_BAD_REQUEST_DATA = 14;
        static const uint16_t ERR_BAD_REPLY_DATA = 15;

        // file (or other object type, depending on context) not found
        static const uint16_t ERR_NOT_FOUND = 16;

        // Retry OK.  This is returned from CMD_CONFIG + SUBCMD_CONFIG_PUT
        // when the host resends a page that we've already seen and
        // successfully stored.
        static const uint16_t ERR_RETRY_OK = 17;

        // Transfer length.  If the response requires additional data to
        // be sent device-to-host, this indicates the length of the data,
        // which the device sends as another packet immediately following
        // the response.  If this is zero, there's not another packet.
        uint16_t xferBytes;

        // reserved (currently just here for padding to a 32-bit boundary)
        uint16_t reserved = 0;

        // Response arguments.  This allows the response to send back a
        // small amount of parameter data directly with the response
        // packet, without the need for an additional transfer.  If
        // the response contains any argument data, argsSize is set to
        // the number of bytes populated.  The meaning of the arguments
        // is specified to the command code.
        union Args
        {
            // raw byte view of the arguments
            uint8_t argBytes[16];

            // Version number, for CMD_QUERY_VERSION
            struct __PackedBegin Version
            {
                uint8_t major;       // major version number
                uint8_t minor;       // minor version number
                uint8_t patch;       // patch version number
                char buildDate[12];  // build date, ASCII, YYYYMMDDhhmm, no null terminator
            } __PackedEnd version;

            // ID numbers, for CMD_QUERY_IDS
            struct __PackedBegin ID
            {
                uint8_t hwid[8];            // the Pico's native hardware ID; permanent and universally unique per device
                uint16_t cpuType;           // CPU type (2040 -> RP2040, 2350 -> RP2350)
                uint8_t cpuVersion;         // CPU version (RP2040: 1 -> B0/B1, 2 -> B2)
                uint8_t romVersion;         // Pico ROM version (1 -> B0, 2 -> B1, 3 -> B2)
                uint8_t unitNum;            // unit number, assigned in the JSON config; identifies the unit to DOF
                uint8_t xinputPlayerIndex;  // player index for the XInput interface; 0xFF if unknown/inactive
                uint16_t ledWizUnitMask;    // LedWiz emulation unit mask; the low-order bit enables unit #1, next bit enables unit #2, etc
            } __PackedEnd id;

            // Checksum, for CMD_CONFIG + SUBCMD_CONFIG_TEST_CHECKSUM.
            // This provides the computed checksum value of the stored
            // config file data.
            uint32_t checksum;

            // CMD_GET_LOG data
            struct __PackedBegin
            {
                uint32_t avail;      // number of bytes of log data currently available on the device side
            } __PackedEnd log;

            // Global button states, for CMD_BUTTONS + SUBCMD_BUTTON_QUERY_STATES
            struct __PackedBegin ButtonState
            {
                uint32_t globalShiftState;  // current global shift state)
            } __PackedEnd buttonState;

            // Flash sector transmission data, for CMD_FLASH_STORAGE + SUBCMD_FLASH_READ_SECTOR
            struct __PackedBegin Flash
            {
                uint32_t crc32;      // CRC-32 of the transmitted sector
            } __PackedEnd flash;

            // TV ON status, for CMD_TVON + SUBCMD_TVON_QUERY_STATE
            struct __PackedBegin TVON
            {
                // Power-sense state.  This is one of the PWR_xxx constants defined below.
                uint8_t powerState;
                static const uint8_t PWR_OFF = 0;           // power was off at last check
                static const uint8_t PWR_PULSELATCH = 1;    // power was off; pulsing the latch to test new state
                static const uint8_t PWR_TESTLATCH = 2;     // power was off; testing the latch to determine new state
                static const uint8_t PWR_COUNTDOWN = 3;     // off-to-on power transition detected, delay countdown in progress
                static const uint8_t PWR_RELAYON = 4;       // pulsing relay
                static const uint8_t PWR_IRREADY = 5;       // ready to send next IR command
                static const uint8_t PWR_IRWAITING = 6;     // waiting for IR pause between commands
                static const uint8_t PWR_IRSENDING = 7;     // IR command sent, waiting for transmission to complete
                static const uint8_t PWR_ON = 8;            // power was on at last check

                // Power-sense GPIO state.  0 if the GPIO is reading low, 1 if reading high.
                uint8_t gpioState;

                // TV hard-wired switch relay state.  This is a combination of bits
                // from the RELAY_STATE_xxx bits defined below.
                uint8_t relayState;
                static const uint8_t RELAY_STATE_POWERON = 0x01;        // pulsed on due to power-sense OFF->ON state transition detected
                static const uint8_t RELAY_STATE_MANUAL = 0x02;         // switched on manually through the USB interface
                static const uint8_t RELAY_STATE_MANUAL_PULSE = 0x04;   // pulsed on manually through the USB interface

                // IR command index - valid during the IR send states; this is the
                // index of the current command being sent, starting at 0, in the
                // list of IR commands to send when a power OFF->ON transition
                // is detected
                uint8_t irCommandIndex;

                // IR command list count (number of commands to send at power-on)
                uint8_t irCommandCount;
                
            } __PackedEnd tvon;
        } args;
    } __PackedEnd;

    // CMD_QUERY_STATS response data struct, returned as the additional
    // transfer data.
    // 
    // The structure size member (cb) is a version marker.  The caller
    // should check this to make sure the version returned contains the
    // fields being accessed.  Any newer version will start with the
    // identical field layout as the next older version, with additional
    // fields added at the end, so a struct with a larger size can
    // always be interpreted using an older version of the struct.
    struct __PackedBegin Statistics
    {
        uint16_t cb;             // structure size, to identify structure version
        uint16_t reserved0;      // reserved/padding
        uint32_t reserved1;      // reserved/padding
        uint64_t upTime;         // time since reboot, in microseconds

        uint64_t nLoops;         // number of iterations through main loop since stats reset
        uint64_t nLoopsEver;     // number of main loop iterations since startup
        uint32_t avgLoopTime;    // average main loop time, microseconds
        uint32_t maxLoopTime;    // maximum main loop time, microseconds
        
        uint64_t nLoops2;        // number of iterations through secondary core main loop since stats reset
        uint64_t nLoopsEver2;    // number of secondary core main loop iterations since startup
        uint32_t avgLoopTime2;   // average secondary core main loop time, microseconds
        uint32_t maxLoopTime2;   // maximum secondary core main loop time, microseconds
        
        uint32_t heapSize;       // total heap size
        uint32_t heapUnused;     // heap space not in use
        uint32_t arenaSize;      // arena size
        uint32_t arenaAlloc;     // arena space allocated
        uint32_t arenaFree;      // arena space not in use
    } __PackedEnd;

    // CMD_QUERY_USBIFC response data struct, returned as the additional
    // transfer data.
    struct __PackedBegin USBInterfaces
    {
        uint16_t cb;             // structure size, to identify the structure version
        uint16_t vid;            // USB VID
        uint16_t pid;            // USB PID

        uint16_t flags;          // flags - a combination of F_xxx bits defined below
        static const uint16_t F_KEYBOARD_CONF = 0x0001;   // keyboard interface configured
        static const uint16_t F_KEYBOARD_ENA  = 0x0002;   // keyboard reporting enabled
        static const uint16_t F_GAMEPAD_CONF  = 0x0004;   // gamepad interface configured
        static const uint16_t F_GAMEPAD_ENA   = 0x0008;   // gamepad reporting enabled
        static const uint16_t F_XINPUT_CONF   = 0x0010;   // XInput interface configured
        static const uint16_t F_XINPUT_ENA    = 0x0020;   // XInput reporting enabled
        static const uint16_t F_PINDEV_CONF   = 0x0040;   // Open Pinball Device configured
        static const uint16_t F_PINDEV_ENA    = 0x0080;   // Open Pinball Device enabled
        static const uint16_t F_CDC_CONF      = 0x1000;   // USB CDC port configured
        
    } __PackedEnd;

    // CMD_QUERY_GPIO_CONFIG response data.  This is returned in the additional
    // response transfer data.
    struct __PackedBegin GPIOConfig
    {
        // structure size, to identify the structure version
        uint16_t cb;

        // size of the Port struct (size of one array element, as in sizeof(port[0]))
        uint16_t cbPort;

        // number of ports in array
        uint16_t numPorts;

        // port description
        struct Port
        {
            // Port function.  This is the RP2040 hardware function selector
            // assigned to the port; see enum gpio_function in the Pico SDK
            // hardware_gpio section.
            //
            //    0   XIP   execute-in-place SPI controller
            //    1   SPI   SPI bus controller
            //    2   UART  UART
            //    3   I2C   I2C bus controller
            //    4   PWM   PWM controller
            //    5   SIO   direct I/O port (basic GPIO, no on-board peripheral connection)
            //    6   PIO0  PIO unit 0
            //    7   PIO1  PIO unit 1
            //    8   GPCK  Clocks
            //    9   USB   USB controller
            // 0x1f   NULL  no function assigned
            //
            // Each pin has a fixed subset of these functions that it can be
            // assigned, which is a fixed feature of the RP2040 determined by
            // the chip's internal wiring.  Likewise, the specific role that
            // a pin serves when assigned to one of its possible functions is
            // also determined by the chip's wiring.  For example, when GP0 is
            // assigned function 2 (UART), it necessarily serves as UART0 TX.
            // Refer to the RP2040 data sheet for a list of the capabilities.
            uint8_t func;

            // Flags - a combination of F_xxx bits defined below
            uint8_t flags;
            static const uint8_t F_DIR_OUT = 0x01;  // for func=5 (SIO), port GPIO direction is Output
                                                    // (if not set, port direction is Input)

            // Port usage string, as an offset from the start of the GPIOConfig
            // struct.  The string is in 7-bit ASCII characters, terminated with
            // a null (0x00) byte.  This is a descriptive name for the port's
            // assigned function, suitable for display in a UI.  For pins
            // assigned to external peripherals, this generally names the chip
            // and pin function (e.g., "TLC5940[0] SCLK".  For internal Pico
            // peripherals, it usually names the Pico unit (e.g., "I2C0").
            // For direct button inputs and outputs, it names the port
            // ("Button #3", "Output #7").  Buttons are numbered from #0, to
            // match up with the JSON array notation.  Output ports are
            // numbered from #1, for consistency with DOF conventions.
            //
            // A value of 0 means that there's no usage string for this port.
            uint16_t usageOfs;
        };

        // GPIO port descriptors for GP0 to GP29
        Port port[30];
    } __PackedEnd;
    

    // CMD_PLUNGER + SUBCMD_PLUNGER_QUERY_READING response data.  This is
    // returned in the additional response transfer data.
    //
    // This struct might be followed in the buffer by a second struct that
    // depends on the sensor type, providing additional details specific to
    // the sensor.  If a second struct is present, it's appended immediately
    // after the PlungerReading struct in the same USB transfer block.  The
    // second struct always starts with a 16-bit struct size field and a
    // 16-bit sensor type field; the size field lets the caller treat an
    // unrecognized struct type as an opaque block of bytes, and the sensor
    // type field is meant as a sanity check to confirm the struct's validity
    // and identity.
    struct __PackedBegin PlungerReading
    {
        // Struct size, in bytes, to identify the struct version.  This
        // is set to sizeof(PlungerData) on the device side, so that the
        // host can compare it to its own struct size.
        uint16_t cb;

        // Flags - a combination of F_xxx bits
        uint16_t flags;
        static const int F_REVERSE     = 0x0001;   // reverse orientation
        static const int F_CALIBRATED  = 0x0002;   // calibration in effect
        static const int F_CALIBRATING = 0x0004;   // calibration mode active
        static const int F_MODIFIED    = 0x0008;   // settings (jitter filter, firing time limit) modified since last save
        static const int F_ZBLAUNCH    = 0x0010;   // ZB Launch event is active

        // Live raw sensor reading, using the sensor's native units
        uint32_t rawPos;

        // Reading timestamp, in microseconds since an arbitrary zero point
        // (typically the last Pico reboot time).
        uint64_t timestamp;

        // Current HID joystick "Z axis" reading, suitable for use as HID
        // joystick input to Visual Pinball and many other simulators.  VP's
        // convention, which is shared by most of the other widely-used
        // simulators, is to accept the plunger position via the joystick Z
        // axis.  (Thus our terminology, although Z is just the default for
        // VP, which makes the axis selection configurable.  Some other
        // programs are less flexible.)  The zero point on the Z axis is
        // (again, by VP's convention) the physical plunger's resting
        // position, and the axis is positive in the "pull" direction
        // (towards the player).  The maximum positive value on the axis
        // represents the physical plunger's fully retracted position.
        //
        // VP accepts any integer range for this axis, via the normal HID
        // Report Descriptor mechanism.  The HID device can define whatever
        // range it chooses, as long as it normalizes the range so that the
        // physical resting position corresponds to Z=0 and the maximum
        // retraction position corresponds to Z=Z[max].  Given that
        // normalization, logical units on the axis map linearly to physical
        // distance units.
        //
        // Pinscape Pico always uses a signed 16-bit range for this axis, so
        // Z[max] = +32767.  When calibrated against a standard post-1980
        // Williams plunger, which can be retracted about 70mm from the rest
        // position, 1mm equals about 468 logical units, so 1 logical unit
        // equals about 0.002mm.
        //
        // The 'z' value is processed through special filtering that's
        // designed specifically to yield more accurate physics modeling in
        // Visual Pinball.  Most of the processing relates to "firing
        // events", where the user pulls back and releases the plunger.
        // During a firing event, the plunger's motion is so fast that it's
        // impossible for a HID client to infer the instantaneous speed.
        // But Visual Pinball and other simulators try to do so anyway,
        // which results in essentially random speed calculations.  Since
        // the simulator uses the speed to determine the force of the ball
        // launch when the plunger collides with a simulated ball in the
        // shooter lane, simulators using the inferred-speed method will
        // calculate random launch forces, ruining the illusion of
        // simulation for players attempting skill shots.  To fix this,
        // Pinscape replaces the raw position report on this axis with an
        // idealized model that "tricks" the simulator into calculating the
        // speed more accurately.  It's not really trickery in the normal
        // sense; rather, it's a matter of reformatting the raw data in such
        // a way that the simulator can reconstruct the actual mechanical
        // plunger motion.  However, it does lose some information, because
        // it requires reducing the sampling rate during high-speed events
        // to a rate low enough to reliably pass through HID and the
        // simulator's input chain.
        int16_t z;

        // Unprocessed Z reading.  This is the instantaneous HID Z Axis
        // value, without the special "firing event" processing that's
        // applied to the 'z' value.  Simulators that can read the extra
        // information we provide on instantaneous speed can use the
        // unprocessed Z reading for on-screen animation.
        int16_t z0;

        // Previous position, next position, and delta t used for speed
        // readings.  These are mostly for debugging purposes, to let
        // the host program check the firmware's math, and display the
        // raw data to the user.  The firmware uses a three-point scheme
        // to calculate the speed, which requires a one-sample
        // look-ahead (z0Nxt) and one-sample history (z0Prv) surrounding
        // the currently reported sample.  The reported sample is always
        // one reading behind the physical sensor input, since we need
        // one sample of lookahead to do the three-point calculation.
        // The three-point calculation gives us a more accurate reading
        // on the instantaneous speed at the currently reported sample,
        // especially during accelerations, since it averages the slope
        // before and after the current reporting point in time.
        int16_t z0Prv;
        int16_t z0Nxt;
        int64_t dt;

        // Instantaneous speed reading, in Z Axis logical distance units per
        // centisecond (1 cs = 1/100 of a second = 10 ms).  Positive values
        // represent motion in the positive Z direction (towards the user),
        // so this is negative during firing events, when the plunger is
        // being driven forward (towards the ball) by the main spring.
        //
        // A "Z axis logical distance unit" is the abstract unit system for
        // our HID Z Axis reports for the sensor position, where 0 is the
        // rest position and +32767 is the maximum retraction point.  The
        // peculiar time unit (centiseconds) was chosen so that the range of
        // speeds for a physical plunger will mostly span the 16-bit signed
        // range we use for joystick logical axis reports, without risk of
        // overflow.  Using as much of the range as possible is desirable
        // because it preserves more precision than using a smaller part of
        // the range.  Peak observed speeds for a standard post-1980s
        // Williams plunger mechanism have been measured at around 4.6mm/ms
        // using a high-precision quadrature sensor; this equals 21532
        // LZU/cs.  The maximum value of 32767 LZU/cs corresponds to 7mm/ms,
        // which gives us a comfortable amount of headroom for faster
        // mechanisms than the reference system I used in my speed tests
        // (from higher spring tensions, say).
        //
        // The instantaneous speed calculation is designed to be passed in
        // HID reports alongside the Z-Axis position reading, to give the
        // PC-side pinball simulator a more accurate speed reading than it
        // can calculate on its own from HID reports.  HID reports are too
        // slow to allow the simulator calculate the speed from the first
        // time derivative of the position reports, so a simulator that
        // needs the speed for its physics calculations should always use
        // the reported speed instead of trying to infer it from the time
        // evolution of the Z-position reports.
        int16_t speed;

        // Firing state - one of the FireState enum values (see below).  This
        // indicates when the device detects the phases of a pull-back-and-
        // release gesture, where the user pulls back the plunger to a
        // retracted position and then releases it so that the spring propels
        // it forward.  We call this a "firing" event because it's how you
        // launch (fire) the ball into play on a real machine (one that has a
        // conventional plunger, anyway).
        uint16_t firingState;

        // Calibration data.  These are in terms of normalized raw sensor
        // units, which use a UINT16 scale (0..65535).  The minimum and
        // maximum are the extreme points observed during the most recent
        // calibration procedure.  The zero point is the observed sensor
        // reading when the plunger is at its resting position (at the
        // equilibrium point between the main spring and barrel spring
        // tensions, without the user applying any outside force).  The
        // directionality is relative to the logical joystick axis
        // mapping, which is positive in the direction of plunger
        // retraction (the numbers increase as you pull the plunger back).
        // So the "maximum" is the sensor reading observed at the furthest
        // point of retraction.  The logical axis convention also defines
        // logical zero as the resting position of the plunger, when it's
        // at equilibrium between the spring tensions.  We use this
        // logical axis convention because it's the mapping that Visual
        // Pinball and most of the other pinball simulators use, which
        // makes it straightforward for the user to assign inputs in the
        // simulators.
        uint32_t calMin;         // calibration minimum
        uint32_t calZero;        // calibration zero point
        uint32_t calMax;         // calibration maximum

        // Sensor-specific calibration data.  This is an opaque block
        // of data for the sensor's use.  It's reported here so that the
        // host can save it for later restoration; the host shouldn't try
        // to interpret it.
        uint32_t calSensorData[8];

        // Plunger firing time observed in most recent calibration
        // procedure, in microseconds.  This is the time it takes for the
        // plunger to reach the zero point when released from the fully
        // retracted position.
        uint32_t firingTimeMeasured;

        // Firing state time limit.  This is a configurable limit on the time
        // allowed in the Firing state.  If the plunger doesn't cross the zero
        // point within this time, the firing state ends and normal plunger
        // tracking resumes.  The time limit essentially sets a minimum speed
        // that triggers a firing event, which helps prevent misinterpreting
        // manual forward motion as a release event.  Microseconds.
        uint32_t firingTimeLimit;

        // Integration time.  This is a configurable setting applicable to
        // certain imaging sensors, controlling the exposure time per frame.
        // Zero selects the default integration time for the sensor, which
        // is typically the frame data, which in turn is a function of the
        // physical design of the sensor and the details of the Pico
        // electronic and software interface to the sensor.  Microseconds.
        uint32_t integrationTime;

        // Manual scaling factor, as a percentage value (100 represents
        // scaling factor 1.0).  This is a configurable setting that
        // lets the user manually tweak the results of the calibration
        // to expand or compress the plunger travel range to make the
        // on-screen animation better match the physical plunger action.
        // This is particularly useful for sensors that have non-uniform
        // distance resolving power across their range, such as proximity
        // sensors, because the automatic calibration process might not
        // be able to distinguish distances well enough at the low-res
        // end of the travel range to set the optimal endpoint.  This
        // lets you magnify or compress the calibrated range when that
        // happens.
        uint32_t manualScalingFactor;

        // jitter filter data
        uint32_t jfWindow;       // window size, in native sensor units
        uint32_t jfLo;           // low end of current window
        uint32_t jfHi;           // high end of current window
        uint32_t jfLastPre;      // last pre-filtered reading
        uint32_t jfLastPost;     // last post-filtered reading

        // Scan mode.  Some imaging sensors have multiple image analysis
        // algorithms that might have tradeoffs that make the best choice
        // vary by installation, so we make the mode configurable to let
        // the user find the best option for their setup.  The mode values
        // are specific to the individual sensors.
        uint8_t scanMode;
    } __PackedEnd;

    // Additional sensor-specific data for quadrature-encoder plungers.  This
    // is appended to the PlungerReading struct when a quadrature encoder is
    // selected as the plunger sensor type.
    struct __PackedBegin PlungerReadingQuadrature
    {
        // struct identification: size in bytes, sensor type code
        uint16_t cb;
        uint16_t sensorType;

        // Current A and B channel states, encoded in the low two bits
        // (channel A in bit 0x01, channel B in bit 0x02)
        uint8_t state;
    } __PackedEnd;

    // Additional sensor-specific data for imaging sensors.  This is appended
    // to the PlungerReading struct when an imaging sensor is selected as the
    // plunger sensor type.
    struct __PackedBegin PlungerReadingImageSensor
    {
        // Struct identification: size in bytes, sensor type code.  Note
        // that 'cb' includes the actual populated size of the pix[] array.
        uint16_t cb;
        uint16_t sensorType;

        // reserved (padding)
        uint16_t reserved;

        // Image timestamp.  This is the time the image frame was captured,
        // expressed as the number of microseconds from an arbitrary epoch,
        // which is typically the time the Pico was last physically reset.
        uint64_t timestamp;

        // number of pixels in image array
        uint16_t nPix;

        // Pixel array.  This is a snapshot of the latest image frame read
        // from the sensor.  The actual size of the array is given by nPix.
        // Each byte represents one pixel, as returned from the sensor.  Most
        // sensors use linear grayscale brightness values, but some (such as
        // TCD1103) are negative images (lower number = brighter light).
        uint8_t pix[1];

    } __PackedEnd;

    // Additional sensor-specific data for VCNL4010.  This is appended
    // to the PlungerReading struct when this sensor is active.
    struct __PackedBegin PlungerReadingVCNL4010
    {
        // struct identification: size in bytes, sensor type code
        uint16_t cb;
        uint16_t sensorType;     // FeedbackControllerReport::PLUNGER_VCNL4010

        // last proximity count reading from the sensor
        uint16_t proxCount;
    } __PackedEnd;
    
    // Plunger firing state.  This is reported in the PlungerReading
    // report via the firingState field.
    //
    // The firing state represents the firmware's monitoring of the plunger
    // motion, to detect when the user is performing a ball-launch "firing"
    // event by pulling back and releasing the plunger.  A firing event occurs
    // when the plunger shoots forward and crosses the resting position, which
    // on a real pinball machine would be the point where the plunger strikes
    // the ball (if a ball is sitting in the shooter lane) and launches it
    // into play.
    //
    // The firmware tracks these motions for two reasons.  The first is to
    // generate "ZB Launch" signals, which can optionally simulate Launch Ball
    // button presses using a mechanical plunger.  Many virtual pin cab
    // builders include both a plunger and a Launch Ball button on their
    // machines, but some prefer the cleaner and more "real pinball machine"
    // look of just a plunger, without a separate Launch Ball button.  The ZB
    // Launch feature is designed with the latter case in mind, in that it
    // lets you simulate the effect of a Launch Ball button using the natural
    // gesture of pulling back and releasing the plunger.  The second reason
    // to detect firing events on the firmware side is that the firmware CAN
    // detect these events accurately, thanks to its high-speed access to the
    // sensor data.  It's NOT possible for software on the PC host side to
    // accurately track the firing motion because the USB HID connection is
    // too slow - it can't accurately measure the speed from the position
    // reports alone because they're not frequent enough to capture this
    // high-speed motion.  The firing event tracking on the device side allows
    // the device to manipulate the Z Axis HID position reports in such a way
    // that a simulator on the PC can accurately reconstruct the release
    // motion.  It's a bit counterintuitive, but the "manipulated" reports
    // yield a more accurate reconstruction of reality in the simulator than
    // literal position reports would, because the sampling rate on the
    // literal reports is so low the simulator receives effectively random
    // numbers during high-speed events.
    enum class PlungerFiringState : uint16_t
    {
        // Not in a firing event
        None = 0,

        // Plunger is moving forward, possibly in a firing motion.  The
        // firmware holds the Z Axis report at the starting position of the
        // forward motion while waiting to see if the motion continues.  This
        // gives the PC-side simulator time to observe the starting point of
        // the firing event, if a firing event actually follows.  The hold
        // ends if the plunger ends up moving too slowly to look like a true
        // firing event.
        Moving = 1,

        // Firing event detected.  The plunger crossed the zero point from a
        // moving state.  The firmware holds the Z Axis report at the forward
        // position briefly to give the PC-side simulator time to observe the
        // other end of the event.  If the ZB Launch Ball feature is enabled,
        // this triggers a simulated Launch Ball button press.  This state
        // lasts for a timed interval before advancing to the Settling state.
        Fired = 2,

        // After a firing event, the firmware enters this state to wait for
        // the mechanical plunger to stop bouncing.  The plunger usually
        // bounces very rapidly after hitting the front spring, which lasts
        // for a second or two until the motion dies down and the plunger
        // comes to rest.  During this state, the firmware holds the Z Axis
        // position at zero, so that the simulator doesn't see a lot of random
        // noise from all the bouncing.  This state ends when the motion slows
        // down enough to make the USB position reports reliable again.
        Settling = 3,
    };


    // CMD_PLUNGER + SUBCMD_PLUNGER_QUERY_CONFIG response data.  This
    // is returned in the additional response transfer data.
    struct __PackedBegin PlungerConfig
    {
        // Struct size, in bytes, to identify the struct version.  This
        // is set to sizeof(PlungerData) on the device side, so that the
        // host can compare it to its own struct size.
        uint16_t cb;

        // Sensor type.  This is a plunger type code from struct
        // FeedbackControllerReport (see FeedbackControllerProtocol.h).
        uint16_t sensorType;

        // Sensor native scale.  This is the maximum value in the unit
        // system that the sensor uses at the hardware level.  The minimum
        // on the native scale is always zero.  Even if the underlying
        // hardware device can report values with a non-zero minimum, the
        // sensor device driver will adjust the range before scaling so that
        // the minimum value is zero.  For example, a sensor that reports
        // -1000 to +1000 at the hardware level will be adjusted to a range
        // of 0 to 2000 for our reporting purposes, so nativeScale will be
        // set to 2000, and rawPos will always read from 0 to 2000.)
        //
        // The plunger logic on the device uses the native scale internally
        // for most of its internal processing, to minimize precision loss
        // from rescaling.
        uint32_t nativeScale;

        // Flags (no flags currently defined)
        uint32_t flags;
    } __PackedEnd;

    // CMD_BUTTONS + SUBCMD_BUTTON_QUERY_DESCS return data.  This is
    // returned as extra transfer data from this subcommand.  The
    // ButtonList struct is essentially a list header; it's followed
    // by an array of zero or more ButtonDesc structs, then an array
    // of zero or more ButtonDevice structs.
    //
    // Each ButtonDesc represents a logical button assigned in the
    // user's configuration.  The structs are in the same order as the
    // logical buttons in the configuration list, so the position in
    // the array of each struct implies its logical button number,
    // starting at 0.
    //
    // Each ButtonDevice represents a physical input device that's
    // configured for button input.  This includes an entry for each
    // PCA9555 chip, and an entry for each 74HC165 daisy chain.
    // There's no entry for the Pico's GPIO ports, since those are
    // always present (being fixed components of the Pico itself).
    // Virtual button sources, such as accelerometer triggers and
    // plunger triggers, also aren't included, since those aren't
    // directly connected to physical input ports.
    struct __PackedBegin ButtonList
    {
        // size in bytes of the ButtonList struct
        uint16_t cb;

        // offset (from the first byte of the ButtonList struct) of the start of
        // the ButtonDesc array
        uint16_t ofsFirstDesc;

        // size in bytes of each individual ButtonDesc struct
        uint16_t cbDesc;

        // number of ButtonDesc structs
        uint16_t numDescs;

        // offset (from the first byte of the ButtonDesc struct) of the start of
        // the ButtonDevice array
        uint16_t ofsFirstDevice;

        // size in bytes of each individual ButtonDevice struct
        uint16_t cbDevice;

        // number of ButtonDevice structs
        uint16_t numDevices;

        // reserved/padding
        uint16_t reserved0;
    } __PackedEnd;

    struct __PackedBegin ButtonDesc
    {
        // button type
        uint8_t type;
        static const uint8_t TYPE_PUSH = 0x01;   // ordinary pushbutton, ON when pushed, OFF when released
        static const uint8_t TYPE_HOLD = 0x02;   // push-and-hold button; delayed ON after behind held down, OFF when released
        static const uint8_t TYPE_PULSE = 0x03;  // pulse button; ON for a set pulse time after each OFF/ON and/or ON/OFF change
        static const uint8_t TYPE_TOGGLE = 0x04; // toggle button: pushing the button toggles its ON/OFF state
        static const uint8_t TYPE_ONOFF = 0x05;  // on/off button: toggle button with two separate inputs that set ON and OFF state independently
        static const uint8_t TYPE_SHIFT = 0x06;  // shift button; selects alternate functions of other buttons when held

        // source device type
        uint8_t sourceType;
        static const uint8_t SRC_GPIO = 0x01;      // Pico GPIO pin input
        static const uint8_t SRC_BOOTSEL = 0x02;   // Pico BOOTSEL button input
        static const uint8_t SRC_PCA9555 = 0x03;   // PCA9555 GPIO extender port
        static const uint8_t SRC_74HC165 = 0x04;   // 74HC165 shift register input port
        static const uint8_t SRC_ACCEL = 0x05;     // accelerometer threshold trigger input
        static const uint8_t SRC_PLUNGER = 0x06;   // plunger threshold trigger input
        static const uint8_t SRC_ZBLAUNCH = 0x07;  // ZB Launch Ball trigger input
        static const uint8_t SRC_IR = 0x08;        // IR remote receiver input (triggers on receiving a programmed IR command)
        static const uint8_t SRC_CLOCK = 0x09;     // time-of-day trigger
        static const uint8_t SRC_OUTPORT = 0x0A;   // output port trigger (DOF port); button is ON when the DOF port PWM level in a specified range
        static const uint8_t SRC_NULL = 0x0B;      // null source; always reads as off

        // Source device details.  These elements vary by source type:
        //
        // - GPIO: unit is always 0, port is the Pico GPxx number
        //
        // - PCA9555: unit is the index of the chip in the configuration list,
        //   starting at 0; port is the IO number on the chip, 0..7 for IO0_0..7
        //   and 8..15 for IO1_0..7
        //
        // - 74HC165: unit is the index of the daisy chain in the config list,
        //   and port is the index of the port in the chain, starting at 0 for
        //   the first port on the first chip.  Note that the unit number
        //   identifies the whole chain, not the individual chip; the chip
        //   within the chain can be inferred from the port number, since
        //   there are a fixed 8 ports per chip.
        //
        // - Output (DOF) port: unit is zero, port is the output port number
        //
        // These fields are unused for other source types.
        uint8_t sourceUnit;
        uint8_t sourcePort;

        // Source polarity: 0 for active low, 1 for active high.  This only
        // applies to physical input sources (GPIO, PCA9555, 74HC165); it's
        // unused for other source types.
        uint8_t sourceActiveHigh;

        // action type
        uint8_t actionType;
        static const uint8_t ACTION_NONE = 0x01;    // no action
        static const uint8_t ACTION_KEY = 0x02;     // keyboard key
        static const uint8_t ACTION_MEDIA = 0x03;   // media key (extended keyboard set for media actions)
        static const uint8_t ACTION_GAMEPAD = 0x04; // gamepad button press
        static const uint8_t ACTION_XINPUT = 0x05;  // XInput (XBox controller emulator) button press
        static const uint8_t ACTION_RESET = 0x06;   // Pico hardware reset
        static const uint8_t ACTION_NIGHTMODE = 0x07;   // engage Night Mode when button is on
        static const uint8_t ACTION_PLUNGERCAL = 0x08;  // activate plunger calibration mode
        static const uint8_t ACTION_IR = 0x09;      // send an IR command
        static const uint8_t ACTION_MACRO = 0x0A;   // execute a macro (a multi-step sequence of other actions)
        static const uint8_t ACTION_OPENPINDEV = 0x0B;  // Open Pinball Device button input

        // Action detail:
        //
        // - Key: the USB usage code, from the HID Keyboard Usage Page 0x07,
        //   of the mapped keyboard key
        //
        // - Media: the USB usage code, from HID Consumer Usage Page 0x0C,
        //   of the mapped media control key
        //
        // - Gamepad: joystick nominal button number, 1..32
        //
        // - XInput: controller button number, 0=Up, 1=Down, 2=Left, 3=Right,
        //   4=Start, 5=Back, 6=L3, 7=R3, 8=LB, 9=RB, 10=XBOX, 12=A, 13=B, 14=X, 15=Y
        //
        // - Open Pinball Device: 1..32 = generic button; 33-65 = pinball button,
        //   using the index in the spec + 33
        //
        // Unused for other action types.
        uint8_t actionDetail;

        // flags - a bitwise combination of FLAG_xxx bits
        uint8_t flags;
        static const uint8_t FLAG_EVENTLOG = 0x01;   // event logging is enabled for this button

        // Shift mask and shift bits.  The mask selects which shift bits affect
        // the button, and the shift bits set the state that has to match.  For
        // a shift buttons (type == TYPE_SHIFT), the mask is ignored, and the
        // shift bits specify which bit or bits this button adds to the global
        // shift state when pressed.  For other buttons, the button is engaged
        // when the bitwise-AND of (global shift state) and shiftMask equals
        // shiftBits.  If shiftMask is zero, the button is insensitive to the
        // shift state, since it masks out all shift bits.
        uint32_t shiftMask;
        uint32_t shiftBits;
    } __PackedEnd;

    struct __PackedBegin ButtonDevice
    {
        // Configuration index.  This is the device's index in the JSON
        // configuration section for the device type.
        uint8_t configIndex;

        // device type, as a ButtonDesc::SRC_xxx constant (SRC_PCA9555, SRC_74HC165)
        uint8_t type;

        // Number of ports associated with this device.  For PCA9555 chips, this
        // is always 16.  For 74HC165 chips, this is the number of ports across
        // the whole daisy chain, which is 8 times the number of chained chips.
        uint16_t numPorts;

        // Address.  For PCA9555, this is ((bus << 8) | addr), where bus is the
        // Pico I2C bus number (0 or 1) and addr is the chip's 7-bit I2C address.
        // Unused for 74HC165.
        uint32_t addr;
    } __PackedEnd;

    // Button event log, for CMD_BUTTONS + SUBCMD_BUTTON_QUERY_EVENT_LOG.
    // This struct is the header for the event list; it's followed by zero
    // or more ButtonEventLogItem structs, as a packed array.  The event
    // log item list represents the history of rising-edge and falling-edge
    // interrupts received on the GPIO, with a timestamp for each event.
    // An edge corresponds to a physical press or release of the button.
    struct __PackedBegin ButtonEventLog
    {
        // size of this struct
        uint16_t cb;

        // number of ButtonEventLogItem structs that follow
        uint16_t numItems;

        // size of the ButtonEventLogItem struct
        uint16_t cbItem;

        // reserved/padding
        uint16_t reserved0;
    } __PackedEnd;

    // Button event log item.  Zero or more of these structs follow
    // a ButtonEventLog header struct in response to a CMD_BUTTONS +
    // SUBCMD_BUTTON_QUERY_EVENT_LOG request.
    struct __PackedBegin ButtonEventLogItem
    {
        // Time of the button press, in microseconds since the last Pico CPU reset
        uint64_t t;

        // Event type.  Note that the PRESS and RELEASE events represent the
        // state of the button, NOT the raw voltage level.  The button logic
        // takes the active high/low configuration into account and just
        // reports the abstract button state, so callers don't have to worry
        // about the wiring polarity.
        uint8_t eventType;
        static const uint8_t EVENTTYPE_RELEASE = 0;   // button was released (switching from on to off)
        static const uint8_t EVENTTYPE_PRESS = 1;     // button was pressed (switching from off to on)

        // reserved/padding
        uint8_t reserved0[7];
    } __PackedEnd;

    // Logical Output port list, for CMD_OUTPUTS + SUBCMD_OUTPUT_QUERY_PORTS.
    // The reply transfer data starts with an OutputPortList struct as a list
    // header.  This is followed in the transfer by zero or more OutputPortDesc
    // structs describing the individual ports.
    struct __PackedBegin OutputPortList
    {
        // size in bytes of the OutputPortList struct
        uint16_t cb;

        // size in bytes of the OutputPortDesc struct
        uint16_t cbDesc;

        // number of OutputPortDesc structs that follow
        uint16_t numDescs;

        // reserved/padding
        uint16_t reserved0;
    } __PackedEnd;

    struct __PackedBegin OutputPortDesc
    {
        // Flags
        uint8_t flags;
        static const uint8_t F_NOISY        = 0x01;   // designated noisy port for Night Mode
        static const uint8_t F_GAMMA        = 0x02;   // gamma correction enabled for the port
        static const uint8_t F_INVERTED     = 0x04;   // use inverted logic for the port
        static const uint8_t F_FLIPPERLOGIC = 0x08;   // flipper/chime logic enabled for the port
        static const uint8_t F_COMPUTED     = 0x10;   // the port has a computed data source instead of direct host control

        // Device identification.  For all of the physical device types, this is the
        // configuration index of the chip.  Unused for other types.
        uint8_t devId;

        // Underlying device type, as one of the DEV_xxx constants defined below
        uint8_t devType;
        static const uint8_t DEV_VIRTUAL = 1;     // virtual; not connected to any physical device
        static const uint8_t DEV_GPIO = 2;        // Pico GPIO port output
        static const uint8_t DEV_TLC59116 = 3;    // TLC59116 I2C PWM controller port
        static const uint8_t DEV_TLC5940 = 4;     // TLC5940 serial-interface PWM controller port
        static const uint8_t DEV_PCA9685 = 5;     // PCA9685 I2C PWM controller port
        static const uint8_t DEV_PCA9555 = 6;     // PCA9555 I2C GPIO extender port
        static const uint8_t DEV_74HC595 = 7;     // 74HC595 shift register port
        static const uint8_t DEV_ZBLAUNCH = 8;    // ZB Launch virtual port
        static const uint8_t DEV_PWMWORKER = 9;   // external Pico running PWMWorker firmware

        // Device port.  For GPIO ports, this is the nominal Pico GP number of the
        // assigned port.  For all of the other physical devices, it's the nominal port
        // number on the chip, using the chip's data sheet labeling for the output pins.
        // Most of these chips use pin labeling of the form OUTPUT0..N, LED0..N, or
        // something similar.  The port number we use here corresponds to the numeric
        // suffix, so (for example) OUTPUT7 on a TLC59116 would be represented here by
        // devPort == 7.
        uint8_t devPort;

    } __PackedEnd;

    // Output device list, for CMD_OUTPUTS + SUBCMD_OUTPUT_QUERY_DEVICES.  The reply
    // transfer data starts with an OutputDevList struct as a list header.  This is
    // followed in the transfer by zero or more OutputDevDesc structs describing the
    // individual devices.
    struct __PackedBegin OutputDevList
    {
        // size in bytes of the OutputDevList struct
        uint16_t cb;

        // number of OutputDevDesc structs that follow
        uint16_t numDescs;

        // size of each OutputDevDesc struct - this is the base struct only,
        // excluding its array of port descriptors
        uint16_t cbDesc;
        
        // padding/reserved
        uint16_t reserved0;
    } __PackedEnd;

    struct __PackedBegin OutputDevDesc
    {
        // Address.  For I2C chips (TLC59116, PCA9685), this is ((bus << 8) | addr),
        // where bus is 0 or 1 for I2C0 or I2C1 (the Pico I2C unit that the device
        // is connected to) and addr is the chip's 7-bit I2C address.  Unused for
        // other device types.
        uint32_t addr;

        // Number of output ports for this configuration entry.  For individually
        // configured chips, this is simply the number of physical output ports
        // on the chip.  For daisy-chained devices, this is the total number of
        // ports in the whole chain.  The number of chips in the chain can be
        // inferred from (numPorts / numPortsPerChip).
        uint16_t numPorts;

        // Number of output ports PER CHIP.  For daisy chains (74HC595, TLC5940),
        // this is the number of chips on each physical chip in the chain.  For
        // individually configured chips, this is the same as numPorts.
        uint16_t numPortsPerChip;

        // PWM resolution.  This is the number of discrete PWM duty cycle steps that the
        // device supports.  The range of PWM levels is 0 to pwmRes-1.  A digital on/off
        // output port (Pico GPIO in digital mode, shift register port, PCA9555 port)
        // reports 2 here.  An 8-bit PWM chip (TLC59116) reports 256; a 12-bit chip
        // (TLC5940, PCA9685) reports 4096.
        uint16_t pwmRes;

        // Device configuration index
        uint8_t configIndex;

        // Device type - one of the OutputPortDesc::DEV_xxx constants
        uint8_t devType;

    } __PackedEnd;

    // Physical output device port list, for CMD_OUTPUTS +
    // SUBCMD_OUTPUT_QUERY_DEVICE_PORTS.  The reply starts with the
    // OutputDevPortList struct as a header, followed by a packed array
    // of OutputDevPortDesc structs, one per port.  The port descriptors
    // are arranged in the same order as the device descriptors returned
    // from SUBCMD_OUTPUT_QUERY_DEVICES, so the device descriptor list
    // can be used to find the device associated with each port.
    struct __PackedBegin OutputDevPortList
    {
        // size of this struct
        uint16_t cb;

        // number of OutputDevPortDesc structs that follow
        uint16_t numDescs;

        // size of the port descriptor struct
        uint16_t cbDesc;

        // reserved/padding
        uint16_t reserved0;
    } __PackedEnd;
    
    struct __PackedBegin OutputDevPortDesc
    {
        // port configuration type - one of the TYPE_xxx constants defined below
        uint8_t type;
        static const uint8_t TYPE_UNUSED  = 0x00;   // port is not assigned as an output
        static const uint8_t TYPE_DIGITAL = 0x01;   // port is a digital output
        static const uint8_t TYPE_PWM     = 0x02;   // port is a PWM output
    } __PackedEnd;


    // Logical output port level list, for CMD_OUTPUTS +
    // SUBCMD_OUTPUT_QUERY_LOGICAL_PORT_LEVELS.  The reply starts with the
    // OutputLevelList struct as a list header, followed by a packed array
    // of OutputLevel structs.  The array size is indicated in the header
    // struct.
    struct __PackedBegin OutputLevelList
    {
        // size of the header struct
        uint16_t cb;

        // size of each individual OutputLevel struct
        uint16_t cbLevel;

        // number of level structs (same as number of logical ports)
        uint16_t numLevels;

        // flags - a combination of F_xxx bits below
        uint16_t flags;
        static const uint16_t F_TEST_MODE = 0x0001;  // Test Mode is active
        
    } __PackedEnd;

    struct __PackedBegin OutputLevel
    {
        // Host level - this is the current level as commanded by the host,
        // before applying computed data sources and time limiters.  This is
        // a value 0..255, representing the PWM duty cycle from 0% to 100%.
        uint8_t hostLevel;

        // Calculated level - this is the current output level from the port,
        // after applying computed data sources.
        uint8_t calcLevel;

        // Device level - this is the final level sent to the device, after
        // applying flipper logic time limitation, duty cycle inversion,
        // and any other processing.
        uint8_t outLevel;

    } __PackedEnd;

    // Physical output device port level list header, for CMD_OUTPUTS +
    // SUBCMD_OUTPUT_QUERY_DEVICE_PORT_LEVELS.  The reply starts with
    // the OutputDevLevelList struct as a list header, followed by a
    // packed array of OutputDevLevel structs.  The ports in the level
    // array are arranged in the same order as the device descriptors
    // returned from SUBCMD_OUTPUT_QUERY_DEVICES, so the caller can
    // use the descriptor list to interpret the level list.
    struct __PackedBegin OutputDevLevelList
    {
        // size of the header struct
        uint16_t cb;

        // size of each individual OutputDevLevel struct
        uint16_t cbLevel;

        // number of level structs that follow the header; this
        // is the sum of all of the ports across all of the output
        // devices
        uint16_t numLevels;
    } __PackedEnd;

    struct __PackedBegin OutputDevLevel
    {
        // Port level, as a PWM duty cycle level on the underlying
        // device's native scale, 0 to OutputDevDesc::pwmRes-1.
        uint16_t level;
    } __PackedEnd;

    // Flash file system information, for CMD_FLASH_STORAGE + SUBCMD_FLASH_QUERY_FILESYS
    struct __PackedBegin FlashFileSysInfo
    {
        // size of the struct
        uint16_t cb;

        // Number of sectors reserved for the central directory structure,
        // which is a fixed-sized structure at the top of the flash storage
        // space that contains the map of the file layout, including the
        // names of the files and the flash ranges allocated to them.  This
        // is given as the number of 4K sectors.
        uint16_t numDirSectors;

        // Flash offset of the start of the file system data, and length
        // in bytes of the file system data.  The entire file system is
        // contained in a single contiguous block of flash at the given
        // offset, so the host can back up all files by saving this
        // contiguous range of flash.  The space occupied by the central
        // directory is included in this range, and is always located at
        // the top end (i.e., it's the last numDirSectors*4096 bytes).
        uint32_t fileSysStartOffset;
        uint32_t fileSysByteLength;

        // Flash memory size, in bytes.  This is the physical capacity of
        // the Pico's on-board flash chip.  The reference Pico manufactured
        // by Raspberry Pi has a 2MB chip installed, but the RP2040 CPU can
        // accommodate capacities up to 16MB, and there are existing third-
        // party clones with larger sizes than the standard 2MB, as well as
        // devices with no on-board flash at all.
        //
        // This field contains reliable information if F_FLASH_SIZE_KNOWN is
        // set in the flags.  If that flag is NOT set, this field contains a
        // default size of 2MB, which is the size of the flash part used in
        // the reference Pico manufactured by Raspberry Pi.  I think it's
        // safe to assume that all Picos that have any flash installed have
        // at least 2MB, so the value here should be a safe lower bound even
        // if F_FLASH_SIZE_KNOWN isn't set.  The reason for the uncertainty
        // is that the flash chip is external to the Pico's RP2040 CPU, and
        // by design is allowed to vary in size up to 16MB.  The CPU has no
        // direct knowledge of the installed flash size as a result.  The
        // firmware attempts to determine the flash size by interrogating
        // some industry-standard SPI register locations (known as SFDP)
        // that most newer flash chips implement per the standards.  If
        // these registers are properly implemented, the firmware can
        // reliably determine the actual flash size.  The reference Pico's
        // flash chip implements the registers, but there's no guarantee
        // that every clone does, so we provide this flag, in case the host
        // software needs to distinguish a definitely known flash size
        // from a default guess.  There might be cases, for example, where
        // it's safer to use the maximum Pico flash size of 16MB when the
        // actual size is unknown, or when it would be better to require
        // the user to specify the size, such as when exporting a full
        // backup image.
        uint32_t flashSizeBytes;

        // Flags
        uint32_t flags;
        static const uint32_t F_FLASH_SIZE_KNOWN = 0x00000001;  // flash size known (see above)

        // reserved/padding
        uint32_t reserved0;

    } __PackedEnd;


    // IR command list, for CMD_QUERY_IR + SUBCMD_QUERY_IR_CMD
    struct __PackedBegin IRCommandList
    {
        // size of the IRCommandList header struct
        uint16_t cb;

        // size of the IRCommandListEle struct
        uint16_t cbEle;

        // number of IRCommandEle structs that follow the IRCommandList header
        uint16_t numEle;

        // reserved/padding
        uint16_t reserved0;
    } __PackedEnd;

    struct __PackedBegin IRCommandListEle
    {
        uint64_t dt;         // elapsed time since previous command received, in microseconds
        
        uint64_t cmd;        // command code     } These use the Pinscape universal IR command
        uint8_t protocol;    // protocol ID      } format.  See IRRemote/IRCommand.h.
        
        uint8_t proFlags;    // protocol flags
        static const uint8_t FPRO_DITTOS = 0x02;    // protocol supports dittos
        
        uint8_t cmdFlags;    // command flags - specifies properties of the specific code received
        static const uint8_t F_HAS_TOGGLE = 0x01;   // toggle bit is present in code
        static const uint8_t F_TOGGLE_BIT = 0x02;   // value of toggle bit
        static const uint8_t F_HAS_DITTO  = 0x04;   // ditto flag is present
        static const uint8_t F_DITTO_FLAG = 0x08;   // value of ditto flag
        static const uint8_t F_POS_MASK   = 0x30;   // position mask - these two bits give the position code
        static const uint8_t F_POS_FIRST  = 0x10;   // position = first
        static const uint8_t F_POS_MIDDLE = 0x20;   // position = first
        static const uint8_t F_POS_LAST   = 0x30;   // position = first
        static const uint8_t F_AUTOREPEAT = 0x40;   // auto-repeat flag

        uint8_t reserved0[5];   // reserved/padding
    } __PackedEnd;

    // Raw IR pulse list, for CMD_QUERY_IR + SUBCMD_QUERY_IR_RAW
    struct __PackedBegin IRRawList
    {
        // size of the list header struct
        uint16_t cb;

        // size of the IRRaw element struct
        uint16_t cbRaw;

        // number of IRRaw elements following the list header
        uint16_t numRaw;

        // reserved/padding
        uint16_t reserved0;
        
    } __PackedEnd;
    
    struct __PackedBegin IRRaw
    {
        // Duration of the pulse, in 2us increments.  0xFFFF represents
        // a pulse longer than the maximum of 131.068ms.
        uint16_t t;

        // Pulse type: 1 = "mark" (IR on), 0 = "space" (IR off)
        uint8_t type;

        // reserved/padding
        uint8_t reserved0;
        
    } __PackedEnd;

    // Nudge device status, returned from CMD_NUDGE + SUBCMD_NUDGE_QUERY_STATUS
    struct __PackedBegin NudgeStatus
    {
        // size of the struct in bytes
        uint16_t cb;

        // flags - a combination of F_xxx bits below
        uint8_t flags;
        static const uint16_t F_MODIFIED    = 0x0001;   // in-memory parameters modified from last flash save/load
        static const uint16_t F_CALIBRATING = 0x0002;   // calibration in progress

        // G range setting.  This provides the scale of all of the 16-bit
        // acceleration readings in the report.  32768 device units equals
        // this value in standard Earth gravity units.  E.g., if gRange ==
        // 2, 32768 device units equals 2g.
        //
        // The G range is a function of the physical accelerometer sensor,
        // and the settings in the JSON configuration.  Most of the
        // supported accelerometer chips have configurable G ranges, with
        // possible settings ranging from 2g to 16g, depending on the chip
        // type.  The range reported here is the dynamic range actually
        // configured on the live sensor for this session.
        uint8_t gRange;

        // last raw device reading; the axes are mapped to the nudge axes,
        // but the readings aren't adjusted for centering or any of the
        // other filter stages
        int16_t xRaw;
        int16_t yRaw;
        int16_t zRaw;

        // last filtered device reading; these are the results of running
        // the raw axis values through the enabled filter stages (auto
        // centering, DC blocking, noise attenuation)
        int16_t xFiltered;
        int16_t yFiltered;
        int16_t zFiltered;

        // Reading timestamp, in microseconds since system startup
        uint64_t timestamp;

        // Last centering time, in microseconds before the current time.
        // This is when the centering position was last set, either by
        // auto-centering or by a manual centering command.
        uint64_t lastCenteringTime;

        // current centering position
        int16_t xCenter;
        int16_t yCenter;
        int16_t zCenter;

        // Auto-centering thresholds.  These are the maximum variances
        // from the rolling average used to detect periods of stillness
        // for auto-centering, set via calibration.
        int16_t xThreshold;
        int16_t yThreshold;
        int16_t zThreshold;
        
        // trailing average snapshot; this is the average over the past
        // few seconds of the raw readings
        int16_t xAvg;
        int16_t yAvg;
        int16_t zAvg;

        // Velocity reading.  The device integrates the accelerations over
        // time into an instantaneous velocity.  These are scaled according
        // to the scaling factor in the nudge parameters.
        int16_t vx;
        int16_t vy;
        int16_t vz;

    } __PackedEnd;


    // Nudge device parameters, for CMD_NUDGE + SUBCMD_NUDGE_QUERY_PARAMS and
    // SUBCMD_NUDGE_PUT_PARAMS.
    struct __PackedBegin NudgeParams
    {
        // size of the struct in bytes
        uint16_t cb;

        // Flags
        uint16_t flags;
        static const uint16_t F_AUTOCENTER = 0x0001;   // enable auto-centering mode

        // Auto-centering interval, in milliseconds.  The center point is
        // updated from the recent average of readings when the device has
        // been stationary (below the noise threshold) continuously for this
        // long.  Auto-centering is an ongoing process; the system continually
        // monitors device readings, and updates the center point again every
        // time a sufficient period of stillness occurs.
        uint16_t autoCenterInterval;

        // auto-center thresholds, in normalized INT16 axis units
        uint16_t xThresholdAutoCenter;
        uint16_t yThresholdAutoCenter;
        uint16_t zThresholdAutoCenter;

        // DC filter adaptation time constant, in milliseconds.  This is the
        // approximate settling time to cancel out a change in the DC level,
        // which is the gravity component of any fixed tilt in the
        // accelerometer.  Shorter times make the device adapt to DC level
        // changes faster, which is generally desirable, except that it also
        // more strongly attenuates true motion signals at low frequencies.
        // Longer times make it take longer to cancel out changes to the
        // fixed tilt, but allow more of the low-frequency signals through.
        // Values around 200ms should work in most cases.
        uint16_t dcTime;

        // Jitter filter window sizes.  The "jitter" filter is a simple
        // hysteresis filter that reports each output as the midpoint of a
        // moving window set by the most recent readings.  When consecutive
        // readings stay within the window, the window stays the same, so
        // the output is constant over those readings, always reporting the
        // midpoint of the same window.  When a reading goes outside the
        // window, the window is moved just far enough to enclose the new
        // reading.  The window size should be set to around the average
        // noise level observed when the device is at rest, since that's the
        // main type of noise we're trying to eliminate with the filter.
        uint16_t xJitterWindow;
        uint16_t yJitterWindow;
        uint16_t zJitterWindow;

        // Velocity decay time, in milliseconds.  This is the half-life of
        // the device's internal calculation of the cabinet's instantaneous
        // velocity.  The device tracks velocity by integrating the
        // acceleration readings over time, so it's essentially a sum of all
        // of the readings since reboot.  As such, any bias in the
        // acceleration data will cause the velocity to grow indefinitely.
        // Any such growth over time is obviously wrong, since a pin cab is
        // inherently a stationary system, meaning that its average velocity
        // over time should be zero.  Any long-term increase can therefore
        // only be due to measurement errors in the acceleration data.  The
        // device corrects for this possibility by attenuating the
        // accumulated velocity over time, so that any accumulated error
        // from the past will be gradually be zeroed out.  The decay time
        // sets the strength of this effect.  Shorter times cancel erroneous
        // readings more quickly, but also attenuate legitimate readings
        // more strongly.  Longer times will have a less visible effect on
        // the live data.
        uint16_t velocityDecayTime_ms;

        // Velocity report scaling factor.  This is the conversion factor
        // between the device's internal velocity calculation, in mm/s, and
        // the INT16 units used in the USB reports.  The factor should be
        // chosen so that the highest typical velocity readings translate to
        // full-scale in INT16 units, or 32768 units.  For example, if the
        // maximum reading observed for a particular installation is around
        // 300 mm/s, setting this to 32768/300 = 109 would max out the INT16
        // readings at 300 mm/s.  You might want to leave some headroom
        // above the actual observed maximum to allow for outliers.
        uint16_t velocityScalingFactor;

    } __PackedEnd;
    
} // end namespace PinscapePico
