// Pinscape Pico - Feedback Controller Interface
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// This is an API for accessing the Pinscape Pico's Feedback
// Controller USB interface.  The Feedback Controller interface
// provides application access to the feedback devices (lights,
// solenoids, etc) attached to the Pinscape Pico's output ports.
// It's designed for use by DOF and any other programs that
// generate feedback effects.
// 
// The Feedback Controller is a HID interface, which makes it
// driverless and shareable (multiple applications can access it
// concurrently).  That's why it's a separate interface from the
// Configuration and Control USB vendor interface.  The DOF-type
// functions of the Feedback Controller interface are for use by
// games and other UI-oriented programs, and the user is likely to
// run more than one at a time, either because one program
// launches another (e.g., PinballY launching VP), or because the
// programs are taking turns in the foreground.  Allowing
// concurrent access to the DOF-type functions, by presenting
// the interface as a HID, avoids file-sharing conflicts between
// applications that can be difficult to troubleshoot.

#pragma once

#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <memory>
#include <functional>
#include <list>
#include <string>
#include <Windows.h>
#include <winusb.h>
#include <usb.h>
#include "PinscapePicoAPI.h"

namespace PinscapePico
{
	// Feedback Controller HID communications interface.  This object
	// provides read/write access to a Pinscape Pico unit's feedback
	// controller HID interface, which can be used to send feedback
	// device commands to the device, query status information, and
	// monitor IR commands received on the device's IR remote control
	// receiver.
	class FeedbackControllerInterface
	{
		friend class VendorInterface;

	public:
		// destructor
		~FeedbackControllerInterface();

		// Feedback unit descriptor.  The enumerator function returns
		// a list of descriptors for the available units.  A descriptor
		// can be used to identify the device based on the unit number,
		// and to open a live USB connection to the device.
		struct Desc
		{
			Desc(int unitNum, const char *unitName, uint16_t ledWizUnitMask, const uint8_t *hwId,
				const WCHAR *path, int numPorts, int plungerType, 
				const VendorInterfaceDesc &vendorIfcDesc);

			// match against an int -> match by unit number
			bool operator ==(int n) const { return n == unitNum; }

			// match against a hardware ID
			bool operator ==(const PicoHardwareId &hwId) const { return hwId == this->hwId; }

			// Pinscape Pico Unit Number.  This is the unit number
			// assigned in the configuration, primarily for use by
			// programs like DOF that automatically discover attached
			// units, to distinguish multiple units in the same system.
			// This value is user-configurable, so it's not a permanent
			// or unique identifier for the device.
			int unitNum;

			// Pinscape Unit Name.  This is a human-friendly name
			// assigned in the configuration, which can be included in
			// a displayed list of available units, to help the user
			// identify the units in a system with multiple Pinscape
			// devices attached.  This isn't guaranteed to be unique;
			// it's mostly just to serve as a friendly name in lists.
			std::string unitName;

			// LedWiz unit mask.  This is a bit mask of the unit IDs
			// set in the JSON configuration.  Each bit corresponds
			// to a virtual LedWiz unit number, with the low-order
			// bit corresponding to unit #1, the next bit #2, and so
			// on up to bit 0x8000 for unit #16.  Each '1' bit means
			// that the corresponding unit number can be assigned to
			// a virtual LedWiz on the host.  Unit numbers are to be
			// assigned sequentially starting at the lowest numbered
			// unit in the mask.
			// 
			// Pinscape Pico doesn't emulate the LedWiz's USB protocol,
			// but that's not necessary to work with legacy LedWiz-aware
			// applications, because the public API to the device was
			// always provided through the LEDWIZ.DLL library provided
			// by the manufacturer.  It's sufficient to replace the DLL
			// with a custom version that presents the public API and
			// uses our private protocol to communicate with the device.
			// A replacement DLL that works with Pinscape Pico, as well
			// Pinscape KL25Z, genuine LedWiz devices, and many clones,
			// is available at https://github.com/mjrgh/lwcloneu2/.
			uint16_t ledWizUnitMask = 0;

			// Pico hardware ID.  This is a unique 64-bit identifier
			// assigned at the factory and programmed into ROM on the
			// Pico board.  It's permanent and universally unique, so
			// it can be used to positively identify the same Pico
			// across reboots, firmware updates, USB port changes, etc.
			PicoHardwareId hwId;

			// Number of output ports (-1 if not known)
			int numOutputPorts = -1;

			// Plunger type - one of the sensor type codes from
			// ../USBProtocol/FeedbackControllerProtocol.h.
			int plungerType = PinscapePico::FeedbackControllerReport::PLUNGER_NONE;

			// plunger type name
			std::string plungerTypeName = "Unknown";

			// plunger type -> name mapping table
			static const std::unordered_map<int, const char*> plungerTypeNameMap;

			// Look up a plunger type in the mapping table.  This always
			// returns a valid string, by returning "Unknown" if the type
			// isn't in the table.
			static const char *GetPlungerTypeName(uint16_t typeCode);

			// Feedback Controller HID device path.  This is the file
			// system path that can be used to access the device's
			// USB interface.
			WSTRING path;

			// Descriptor for the same device's Pinscape Vendor Interface 
			VendorInterfaceDesc vendorIfcDesc;
		};

		// Enumerate available feedback controller interfaces.
		// Returns a list of descriptors for the available units.
		static HRESULT Enumerate(std::list<Desc> &units);

		// Open a feedback controller interface for a given descriptor.
		// Returns null if the open fails.
		static FeedbackControllerInterface *Open(const Desc &desc);

		// Open an interface for a given unit number.  Returns null
		// if there's no such unit or the open fails.  Note that if
		// you've already enumerated the available units, it's more
		// efficient to open a unit via the Desc struct from the
		// enumeration, since this call requires a separate enumeration
		// to find the matching unit.
		static FeedbackControllerInterface *Open(int unitNum);

		// Get the file system path to this device
		const WCHAR *GetFileSystemPath() const { return path.c_str(); }

		// Test the file system path for validity.  This attempts to
		// open a new handle to the device's file system path, to
		// determine if the device is still connected and accessible.
		// Returns true if so, false if the open fails.  This doesn't
		// affect the internal connection state.
		bool TestFileSystemPath() const;

		// Read an incoming report, with the given timeout.  Returns
		// true on success, false on error.  Call GetReadError() on
		// failure to get the Win32 error code.
		bool Read(FeedbackReport &rpt, DWORD timeout = INFINITE);

		// Write a request, with the given timeout.  Returns true
		// on success, false on error.  Call GetWriteError() on
		// failure to get the Win32 error code.
		bool Write(const FeedbackRequest &req, DWORD timeout = INFINITE);

		// Write a raw data buffer, up to 63 bytes
		bool Write(const uint8_t *buf, size_t nBytes, DWORD timeout = INFINITE);

		// Get the Win32 error code from the last read/write/wait
		DWORD GetReadError() const { return readErr; }
		DWORD GetWriteError() const { return writeErr; }
		DWORD GetWaitError() const { return waitErr; }

		// Query device IDs.  Returns true on success, false on
		// timeout or error.
		struct IDReport;
		bool QueryID(IDReport &id, DWORD timeout) {
			return Query({ FeedbackRequest::REQ_QUERY_ID }, id, timeout);
		}

		// Query device status.  Returns true on success, false
		// on timeout or error.
		struct StatusReport;
		bool QueryStatus(StatusReport &status, DWORD timeout) {
			return Query({ FeedbackRequest::REQ_QUERY_STATUS, { 0x01 } }, status, timeout);
		}

		// Set night mode.  Set 'engage' to true to turn Night Mode
		// on, false to turn Night Mode off and return to normal
		// operating mode.  Returns true on success, false on a
		// timeout or error.
		bool SetNightMode(bool engage, DWORD timeout) {
			return Write(FeedbackRequest{ FeedbackRequest::REQ_NIGHT_MODE,
				{ static_cast<uint8_t>(engage ? 1 : 0) } },
				timeout);
		}

		// TV-ON Relay control.  Returns true on success, false on
		// timeout or other error.
		enum class TVRelayState : uint8_t
		{
			Off = 0,
			On = 1,
			Pulse = 2,
		};
		bool SetTVRelayState(TVRelayState state, DWORD timeout) {
			return Write(FeedbackRequest{ FeedbackRequest::REQ_TV_RELAY,
				{ static_cast<uint8_t>(state) } },
				timeout);
		}

		// Center the nudge device.  This explicitly sets the neutral
		// point in the X/Y plane to the current accelerometer reading.
		// Returns true on success, false on timeout or failure.
		bool CenterNudgeDevice(DWORD timeout) {
			return Write(FeedbackRequest{ FeedbackRequest::REQ_CENTER_NUDGE }, timeout);
		}

		// Send an IR command through the IR transmitter.  The
		// command is queued for transmission if another command
		// is already in progress.  Returns true on success, false
		// on timeout or failure.
		bool SendIR(const IRCommand &cmd, uint8_t repeatCount, DWORD timeout);

		// Send a time-of-day clock update to the Pico.  This
		// sends the current system wall clock time to the Pico so
		// that it can sync time-of-day features with the current
		// local time.  Once the Pico knows the current time of
		// day, it keeps track of the passage of time from this
		// reference point, so it's not necessary to keep sending
		// new updates continuously.  However, the Pico loses its
		// memory of the clock time every time it's reset, so the
		// host has to send an update at least once after each
		// power cycle or device reset.  It's a good policy to
		// send an update at the start of every new application
		// session, just in case the Pico has been rebooted
		// recently.  Returns true on success, false on timeout
		// or error.
		bool SendClockTime(DWORD timeout = INFINITE);

		// Turn off all output ports.  Returns true on success,
		// false on timeout or error.
		bool AllPortsOff(DWORD timeout = INFINITE) {
			return Write(FeedbackRequest{ FeedbackRequest::REQ_ALL_OFF }, timeout);
		}

		// Set a block of output ports to new PWM levels.  This
		// sets the block of sequentially numbered ports starting
		// at firstPortNum to the specified new levels.  The
		// 'levels' array contains one byte per port, with the
		// corresponding port's new PWM level value, 0-255.
		// Returns true on success, false on timeout or error.
		bool SetPortBlock(int firstPortNum, int nPorts, const uint8_t *levels, DWORD timeout = INFINITE);

		// Set a collection of output ports to new PWM levels.
		// The 'pairs' array consists of two bytes per port: the
		// first byte of each pair is the port number, and the
		// second byte is the new PWM level for that port.  For
		// example, if nPorts == 2 and the 'pairs' array contains
		// { 0x07, 0xFF, 0x22, 0x00), the call sets port number 0x07
		// to level 0xFF, and sets port number 0x22 to level 0x00.
		// Returns true on success, false on timeout or error.
		bool SetPorts(int nPorts, const uint8_t *pairs, DWORD timeout = INFINITE);

		// LedWiz SBA command emulation.  This activates LedWiz
		// mode on a block of 32 ports, and sets the On/Off state
		// of the ports according to the bit masks in the BankN
		// bytes.  Each BankN byte represents 8 consecutive ports,
		// ordered with the least significant bit first.
		// 
		// The original LedWiz DLL API version of the SBA call
		// doesn't take a first-port parameter, since it simply 
		// sets the whole 32-port complement of the device.  We
		// add the first port parameter to allow access to more
		// than 32 Pinscape Pico ports.  DLL implementations can
		// accomplish this by creating a separate virtual LedWiz
		// unit for each group of 32 ports on the Pico.
		//
		// The SBA command also sets the time period for the
		// LedWiz-style waveform patterns that ports can be set
		// to display.  The time period value is expressed in
		// units of 250ms, and must be in the range 1..7.  The
		// time period applies to all 32 ports in the block, and
		// remains in effect until the next SBA.
		//
		// Once a port is placed in LedWiz mode, it remains in
		// LedWiz mode until one of the DOF-style commands is
		// sent to the port.  Ports can thus be freely switched
		// between "DOF mode" and "LedWiz mode" simply by calling
		// the appropriate APIs.
		bool LedWizSBA(int firstPortNum,
			uint8_t bank0, uint8_t bank1, uint8_t bank2, uint8_t bank3,
			uint8_t globalPulseSpeed, DWORD timeout = INFINITE);

		// LedWiz PBA command emulation.  This activates LedWiz
		// mode on a group of ports, and sets the "profile" value
		// for each port.  The profile values are applied sequentially
		// to consecutive ports starting at the given first port
		// number.  Per our usual convention, ports are numbered from 
		// #1.
		//
		// The original LedWiz DLL API version of the PBA call doesn't
		// take a first-port parameter, since it simply sets the whole
		// 32-port complement of the device.  We add the first port
		// parameter to allow access to more than 32 Pinscape Pico
		// ports.  DLL implementations can accomplish this by creating
		// a separate virtual LedWiz unit for each group of 32 ports
		// on the Pico.
		// 
		// nPorts must be in the range 1..60.
		// 
		// The profile is one of the following:
		//
		//   0-48    = PWM duty cycle, in units of 1/48 cycle
		//   49      = 100% PWM duty cycle (equivalent to 48)
		//   129     = sawtooth wave pattern (linear ramp up/down)
		//   130     = blink pattern (50% duty cycle)
		//   131     = on/ramp down (50% duty cycle blink with fade-out)
		//   132     = ramp up/on (50% duty cycle blink with fade-in)
		//
		// Other values are invalid, and their effects are undefined.
		// The four wave patterns use the period set with the LedWizSBA()
		// function.
		// 
		// Once a port is placed in LedWiz mode, it remains in
		// LedWiz mode until one of the DOF-style commands is
		// sent to the port.  Ports can thus be freely switched
		// between "DOF mode" and "LedWiz mode" simply by calling
		// the appropriate APIs.
		bool LedWizPBA(int firstPortNum, int nPorts, const uint8_t *profiles, 
			DWORD timeout = INFINITE);

		// Perform a query.  This sends the specified query request,
		// then waits for the corresponding reply.  Returns true on
		// success, false if the request fails or times out.
		template<typename T> bool Query(const FeedbackRequest &req, T &reply, DWORD timeout_ms = INFINITE)
		{
			// try reading a raw report
			FeedbackReport rawReply;
			if (!QueryRaw(req, rawReply, T::ReportType, timeout_ms))
				return false;

			// got the desired report type - decode it
			return Decode(reply, rawReply);
		}
		bool QueryRaw(const FeedbackRequest &req, FeedbackReport &reply, uint8_t replyType, DWORD timeout_ms);

		// Decode a device ID report.  Returns true on success,
		// false if the report is of the wrong type.
		struct IDReport
		{
			static const uint8_t ReportType = FeedbackReport::RPT_ID;

			uint8_t unitNum;          // unit number
			char unitName[32];        // unit name
			uint16_t protocolVersion; // feedback controller protocol version number
			uint8_t hwid[8];          // Pico hardware ID (opaque 64-bit unique identifier embedded in hardware ROM)
			uint16_t numPorts;        // number of configured logical output ports
			uint16_t plungerType;     // plunger type (PinscapePico::FeedbackControllerReport::PLUNGER_xxx)
			uint16_t ledWizUnitMask;  // LedWiz unit mask (each bit represents a virtual LedWiz unit number, 
									  // starting at unit #1 for the low-order bit)
		};
		bool Decode(IDReport &id, const FeedbackReport &rpt);

		// Decode a device status report.  Returns true on
		// success, false if the report is of the wrong type.
		struct StatusReport
		{
			static const uint8_t ReportType = FeedbackReport::RPT_STATUS;

			bool plungerEnabled;     // plunger enabled
			bool plungerCalibrated;  // plunger calibrated
			bool nightMode;          // night mode active
			bool clockSet;           // wall clock time has been set
			bool safeMode;           // booted in Safe Mode due to unexpected reset
			bool configLoaded;       // user configuration loaded successfully

			COLORREF led;            // status LED color, as a COLORREF RGB value

			// original status flags; these are decoded into the
			// bool fields above for easier application use
			uint8_t flags;

			uint8_t tvOnState;        // TV ON power sensing state
			static const uint8_t TVON_DEFAULT = 0x00;  // default, power still off/on since last check
			static const uint8_t TVON_W_LATCH = 0x01;  // power off, writing power sense latch
			static const uint8_t TVON_R_LATCH = 0x02;  // power off, reading power sense latch
			static const uint8_t TVON_DELAY   = 0x03;  // off->on transition detected, TV ON countdown in progress
			static const uint8_t TVON_RELAY_PULSE = 0x04; // countdown down, pulsing TV ON relay output
			static const uint8_t TVON_IR_READY = 0x05; // ready to send next TV ON IR command
			static const uint8_t TVON_IR_DELAY = 0x06; // waiting for delay between IR commands
			static const uint8_t TVON_IR_TX   = 0x07;  // IR command transmission in progress

		};
		bool Decode(StatusReport &status, const FeedbackReport &rpt);

		// Decode an IR report.  Returns true on success, false
		// if the report is of the wrong type.
		struct IRReport
		{
			static const uint8_t ReportType = FeedbackReport::RPT_IR_COMMAND;

			// IR command received
			IRCommandReceived command;
		};
		bool Decode(IRReport &ir, const FeedbackReport &rpt);

	protected:
		// protected constructor - create this object via the
		// enumerator methods in the parent class
		FeedbackControllerInterface(HANDLE handle, const WCHAR *path);

		// create from a path
		FeedbackControllerInterface(const WCHAR *path);

		// raw write
		bool WriteRaw(const uint8_t *data, DWORD timeout = INFINITE);

		// internal initialization
		void Init();

		// Queue a new read
		void QueueRead();

		// file handle to the underlying HID interface
		HANDLE handle;

		// file system path used to open the file handle
		WSTRING path;

		// Overlapped I/O structures
		HANDLE hReadEvent;
		HANDLE hWriteEvent;
		OVERLAPPED ov;

		// read buffer for incoming reports
		uint8_t readBuf[64];
		DWORD bytesRead = 0;

		// Windows error from last ReadFile()/WriteFile(), or 0 on success
		DWORD readErr = 0;
		DWORD writeErr = 0;

		// Windows error from last wait operation
		DWORD waitErr = 0;
	};

}
