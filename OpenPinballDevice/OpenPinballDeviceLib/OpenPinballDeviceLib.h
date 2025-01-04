// Open Pinball Device Library for Windows
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Provides high-level access to Open Pinball Device controllers.

#pragma once
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <memory>
#include <string>
#include <list>
#include <vector>
#include "hidapi/hidapi.h"
#include "../OpenPinballDeviceReport.h"


namespace OpenPinballDevice
{
	// forward declarations
	class Reader;
	struct DeviceDesc;

	// Combined reader.  This is a high-level interface to the whole
	// collection of connected Open Pinball Device controllers, combining
	// reports from all devices into a single logical report state.  This
	// interface is suitable for most pinball simulators and other programs
	// that only need to use the OPD controller as a source of user input.
	// (As opposed to applications that present the user with information
	// on the devices themselves.  Those sorts of applications should use
	// the enumeration function to retrieve device information, and then
	// can create readers for individual devices.)
	// 
	// Once the combined reader is created, its list of devices is static.
	// The reader doesn't scan for new devices connected to the system
	// later; it will only see input from devices that were connected at
	// the time it was created.
	//
	// The sensors and input controls connected to an Open Pinball Device
	// are by design meant to be unique: a pin cab has one plunger, one
	// accelerometer, one Start button, one Exit button, one left flipper
	// button, etc.  However, a cabinet might have more than one Open
	// Pinball Device controller, because it's entirely possible to build
	// controllers at the hardware level that only support a subset of the
	// input devices.  For example, you might have one controller running
	// your plunger sensor and accelerometer, and a separate controller
	// providing the button inputs.  Each controller will report all of
	// the standard OPD fields, so the PC will see two separate nudge
	// input reports, two plunger input reports, and two sets of button
	// inputs.  However, as long as there's only one physical plunger
	// sensor in the system, we'll only see one non-zero plunger report
	// across all of the inputs; likewise with the accelerometer, and
	// likewise with the buttons, as long as each logical button ID is
	// only mapped to one physical control.  As long as these conditions
	// are met, we can easily merge input from multiple devices by
	// adding the inputs together.  A device with no accelerometer
	// always reports zeros on the accelerometer axes, so adding its
	// input into the sum has no effect on the sum.  The buttons work
	// the same way if we think of "adding" as a logical OR operation.
	class CombinedReader
	{
	public:
		// Create a combined reader.  This automatically scans for
		// devices, and connects to each one.
		CombinedReader();

		// destruction
		~CombinedReader();

		// Read a report.  This reads the instantaneous state of the
		// nudge and plunger axes.  Buttons are treated as events: if
		// a button's state changed since the last report was read, 
		// the button will reflect the modified state, ensuring that
		// the caller sees a brief state change even if occurred
		// between calls.
		//
		// Returns true if a new report was available, false if not.
		// On a false return, the same report from the last call is
		// returned, so callers that only need to sample the 
		// instantaneous state can ignore the return value, since the
		// previous state still reflects our latest knowledge of the
		// current state of the device up until the time a new report
		// arrives.
		//
		// This function is specific to the v1 report struct.
		bool Read(OpenPinballDeviceReport &report);

		// Get the list of connected devices.  This includes all of
		// the devices (and only the devices) that we're reading from.
		const std::list<DeviceDesc>& GetDevices() const { return devices; }

	protected:
		// Device list
		std::list<DeviceDesc> devices;

		// readers
		std::list<std::unique_ptr<Reader>> readers;
	};


	// Device descriptor.  Use EnumerateDevices() to get a list of descriptors
	// for active devices.
	struct DeviceDesc
	{
		DeviceDesc(const char *path, const wchar_t *version, uint16_t vid, uint16_t pid,
			const wchar_t *productName, const wchar_t *manufacturer, const wchar_t *serial,
			uint8_t reportID, size_t reportSize) :
			path(path), versionStr(version), versionNum(ParseVersionStr(version)), vid(vid), pid(pid),
			productName(productName), manufacturer(manufacturer), serial(serial),
			reportID(reportID), reportSize(reportSize)
		{ }

		// hidapi device path.  This path can be used in hid_open_path() to open
		// the device for access to its report stream.  (On most systems, this
		// can also be used in native OS HID APIs to open the device.  On Windows,
		// for example, this path can used in CreateFileA().)
		std::string path;

		// Open Pinball Device version string, from the usage string
		std::wstring versionStr;

		// Parsed version number: the major version is in the high-order 16 bits,
		// and the minor version is in the low-order 16 bits.  For example, 1.3
		// is encoded as 0x00010003.  This format is convenient for comparisons
		// and sorting, since you can compare it with the regular integer compare
		// operators.
		uint32_t versionNum;

		// VID/PID
		uint16_t vid;
		uint16_t pid;

		// standard HID string descriptors: product name, manufacturer, serial number
		std::wstring productName;
		std::wstring manufacturer;
		std::wstring serial;

		// input report ID, and byte size of the reports
		uint8_t reportID;
		size_t reportSize;

		// Parse a string version into the DWORD format in versionNum
		static uint32_t ParseVersionStr(const wchar_t *str);
	};

	// Enumerate active devices.  This scans the current collection of
	// connected HID devices, returning a list of DeviceDesc structures
	// for the Open Pinball Device entries found.
	std::list<DeviceDesc> EnumerateDevices();

	// Device reader.  This provides read access to the device reports.
	class Reader
	{
	public:
		// Open a reader object given a device descriptor.  Returns a 
		// pointer to a newly created Reader object on success, or nullptr
		// on failure.  The caller assumes ownership of the Reader object
		// (i.e., the caller must delete the object when done with it).
		static Reader *Open(const DeviceDesc &desc);

		// destruction
		~Reader();

		// Read the latest device state, using the version 1.0 report
		// struct.  Returns true if a new report was read, false if not.
		// On a false return, the caller's buffer is populated with the
		// most recently received report (the same one returned on the
		// prior call).  This lets a caller that only needs to know the
		// current instantaneous state of the pin cab sensors ignore
		// the return code, since it gets the latest, current state in
		// either case.
		// 
		// The call is non-blocking, since it returns immediately if 
		// no new report is available.
		// 
		// The reader processes the report internally to handle button
		// presses as "events".  If more than one report has been
		// received since the last call, the reader processes the
		// intermediate reports to detect any changes to button states
		// that occurred between calls, and reports them as changes in
		// the current state.  This helps ensure that the caller sees
		// each button press for at least a single read cycle, even if 
		// the caller is delayed between reports.
		bool Read(OpenPinballDeviceReport &report);

		// Populate the local native struct from a byte array containing
		// a report in the USB wire format.
		//
		// This routine correctly translates the USB packet byte format
		// to the local struct representation, taking into account the
		// local integer representation and any padding in the struct.
		static void Populate(OpenPinballDeviceReport &report, const uint8_t *buf, size_t bufLen);

		// Get the reader's native operating system handle.  This is only
		// implemented on Windows currently; returns zero if not implemented.
		intptr_t GetNativeHandle();

	protected:
		// protected constructor - clients use Open() to create an object
		Reader(hid_device *hDevice, uint8_t reportID, size_t deviceReportSize);

		// Raw byte reader.  If a new report is available, copies the
		// report into the caller's buffer and returns true.  If no new
		// report is available, returns false immediately.
		bool ReadRaw(void *buf, size_t byteLen);

		// Helper functions to unpack USB packet fields into local integer types.
		// Each advances the pointer by the number of packet bytes it consumes.
		static uint8_t UnpackU8(const uint8_t*& buf) { return Unpack(buf, *buf); }
		static int16_t UnpackI16(const uint8_t* &buf) { return Unpack(buf, static_cast<int16_t>(buf[0] | (static_cast<int16_t>(buf[1]) << 8))); }
		static uint32_t UnpackU32(const uint8_t* &buf) {
			return Unpack(buf, static_cast<uint32_t>(buf[0] | (static_cast<uint32_t>(buf[1]) << 8)
				| (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24)));
		}
		static uint64_t UnpackU64(const uint8_t* &buf) {
			return Unpack(buf, static_cast<uint64_t>(buf[0] | (static_cast<uint64_t>(buf[1]) << 8)
				| (static_cast<uint64_t>(buf[2]) << 16) | (static_cast<uint64_t>(buf[3]) << 24)
				| (static_cast<uint64_t>(buf[4]) << 32) | (static_cast<uint64_t>(buf[5]) << 40)
				| (static_cast<uint64_t>(buf[6]) << 48) | (static_cast<uint64_t>(buf[7]) << 56)));
		}

		// unpack helper - advances the input pointer by the type size (this is
		// essentially a post-increment operator, *p++ for the composite type)
		template<typename T> static T Unpack(const uint8_t* &p, T val) { p += sizeof(T); return val; }

		// Windows file handle to underlying device
		hid_device *hDevice;

		// HID report ID
		uint8_t reportID;

		// Last report
		std::vector<uint8_t> lastReport;

		// Aggregated button states
		struct ButtonState
		{
			// Last state reported to the client
			uint32_t reported = 0;

			// Next state to be reported to the client on the next update
			uint32_t next = 0;

			// Live state from the latest report
			uint32_t live = 0;

			// process in a live report
			void Process(uint32_t state);

			// take a report snapshot
			uint32_t GetReport();
		};
		ButtonState genericButtons;
		ButtonState pinballButtons;

		// HID read buffer
		std::vector<uint8_t> readBuf;
	};
}
