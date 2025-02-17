// Open Pinball Device library - hidapi implementation
//
// This library provides Open Pinball Device access via the portable hidapi
// library, https://github.com/libusb/hidapi/tree/master.  
// 
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <regex>
#include <functional>
#include <chrono>
#include "hidapi/hidapi.h"
#include "hid-report-parser/hid_report_parser.h"
#include "OpenPinballDeviceLib.h"


// namespace access
using namespace OpenPinballDevice;


// --------------------------------------------------------------------------
//
// Device descriptor
//

DeviceDesc::DeviceDesc(const char *path, const wchar_t *version, uint16_t vid, uint16_t pid,
	const wchar_t *friendlyName, const wchar_t *productName, const wchar_t *manufacturer, const wchar_t *serial,
	uint8_t reportID, size_t reportSize) :
	path(path), friendlyName(friendlyName), versionStr(version), versionNum(ParseVersionStr(version)),
	vid(vid), pid(pid), productName(productName), manufacturer(manufacturer), serial(serial),
	reportID(reportID), reportSize(reportSize)
{
}


// parse a version string
uint32_t DeviceDesc::ParseVersionStr(const wchar_t *str)
{
	// match against the required <major>.<minor> pattern
	std::wregex pat(L"(\\d+)\\.(\\d+)");
	std::match_results<const wchar_t*> m;
	if (std::regex_match(str, m, pat))
	{
		// pull out the major and minor parts, and encode the result
		// as a uint32_t, with the major version in the high 16 bits
		// and the minor version in the low 16 bits
		uint32_t major = _wtoi(m[1].str().c_str());
		uint32_t minor = _wtoi(m[2].str().c_str());
		return (major << 16) | minor;
	}

	// not parsed
	return 0;
}

// --------------------------------------------------------------------------
//
// Match a device report type by a usage string descriptor
//

struct ReportDescriptorInfo
{
	uint8_t reportID;            // HID report ID
	size_t reportSize;           // HID report size
	std::wstring usageString;    // usage string matched
};

static bool MatchReportTypeByStringUsage(
	hid_device *hDevice,
	uint16_t mainInterfaceUsagePage, uint16_t mainInterfaceUsage,
	uint16_t controlUsagePage, uint16_t controlUsage,
	const std::wregex &stringUsagePat, ReportDescriptorInfo *reportDescInfo)
{
	// read the report descriptor
	std::unique_ptr<unsigned char> reportDescBuf(new unsigned char[HID_API_MAX_REPORT_DESCRIPTOR_SIZE]);
	unsigned char *rp = reportDescBuf.get();
	int rdSize = hid_get_report_descriptor(hDevice, rp, HID_API_MAX_REPORT_DESCRIPTOR_SIZE);
	if (rdSize > 0)
	{
		// parse the usages
		hidrp::UsageExtractor usageExtractor;
		hidrp::UsageExtractor::Report report;
		usageExtractor.ScanDescriptor(rp, rdSize, report);

		// scan the collections
		bool found = false;
		for (auto &col : report.collections)
		{
			// check for the generic USB "Pinball Device CA" type (Application Collection, usage page 5, usage 2)
			if (col.type == hidrp::COLLECTION_TYPE_APPLICATION
				&& col.usage_page == mainInterfaceUsagePage && col.usage == mainInterfaceUsage)
			{
				// got it - scan the input fields in this collection
				for (auto &f : col.Fields(hidrp::ReportType::input))
				{
					// Check for an opaque byte array, usage 0x00 (undefined/vendor-specific),
					// with an associated usage string that matches the OPD signature string.
					const size_t nStrBuf = 128;
					wchar_t strBuf[nStrBuf];
					if (f.usageRanges.size() == 1 && f.usageRanges.front().Equals(controlUsagePage, controlUsage)
						&& f.stringRanges.size() == 1 && !f.stringRanges.front().IsRange()
						&& hid_get_indexed_string(hDevice, f.stringRanges.front().GetSingle(), strBuf, nStrBuf) == 0
						&& std::regex_match(strBuf, stringUsagePat))
					{
						// fill in the results, if requested
						if (reportDescInfo != nullptr)
						{
							// set the report ID and usage string
							reportDescInfo->reportID = f.reportID;
							reportDescInfo->usageString = strBuf;

							// Figure the report size for the associated input report ID.  The
							// report size scanner returns the combined size of all of the fields
							// in the report in BITS, so take ceil(bits/8) to get the report size
							// in bytes.  Then add 1 for the HID Report ID byte prefix that every
							// HID report must include.  This gives us the size of the USB packets
							// the device sends.
							hidrp::ReportSizeScanner sizeScanner;
							hidrp::DescriptorParser parser;
							parser.Parse(rp, rdSize, &sizeScanner);
							reportDescInfo->reportSize = (sizeScanner.ReportSize(hidrp::ReportType::input, f.reportID) + 7) / 8 + 1;
						}

						// matched
						return true;
					}
				}
			}
		}
	}

	// not matched
	return false;
}

static bool MatchReportTypeByStringUsage(
	hid_device_info *deviceInfo,
	uint16_t mainInterfaceUsagePage, uint16_t mainInterfaceUsage,
	uint16_t controlUsagePage, uint16_t controlUsage,
	const std::wregex &stringUsagePat, ReportDescriptorInfo *reportDescInfo)
{
	// open the device
	std::unique_ptr<hid_device, decltype(&hid_close)> hDevice(hid_open_path(deviceInfo->path), &hid_close);
	if (hDevice == nullptr)
		return false;

	// match the report on the device handle
	return MatchReportTypeByStringUsage(hDevice.get(), mainInterfaceUsagePage, mainInterfaceUsage,
		controlUsagePage, controlUsage, stringUsagePat, reportDescInfo);
}

// --------------------------------------------------------------------------
//
// HID request/reply processor.  Sends a request packet to the device,
// and awaits a reply packet.  The caller provides a recognition callback
// to identify the reply of interest; the routine ignores other packets,
// and continues reading until the desired reply arrives or the timeout
// lapses.
//
// The request packet is specifed literally, with the report ID header
// byte included if required.
// 
// The callback is invoked on each packet received from the device after
// sending the request.  The callback returns true if the reply is the
// one it's looking for, false if not.  If the callback returns true,
// the main function immediately returns true.  Otherwise, the function
// continues reading packets until the timeout expires or the callback
// accepts a reply.
//
static bool SendRequestAwaitReply(
	hid_device *hDevice, uint32_t timeout_ms,
	const uint8_t *request, size_t requestBytes,
	uint8_t *replyBuf, size_t replyBytes,
	std::function<bool(uint8_t *replyBuf, size_t replyBytes)> testReply)
{
	// send the request; if that 
	if (hid_write(hDevice, request, requestBytes) <= 0)
		return false;

	// read replies until the timeout lapses
	using namespace std::chrono;
	for (auto now = steady_clock::now(), tEnd = now + milliseconds{ timeout_ms } ;
		now <= tEnd ; now = steady_clock::now())
	{
		// read a reply
		auto remainingTimeout = duration_cast<milliseconds>(tEnd - now).count();
		if (hid_read_timeout(hDevice, replyBuf, replyBytes, static_cast<int>(remainingTimeout)) > 0
			&& testReply(replyBuf, replyBytes))
			return true;
	}

	// timed out without a match
	return false;
}

// --------------------------------------------------------------------------
//
// Get extended device identification details where available.  If extended
// ID information is available, we fill in the result struct and return
// true; otherwise we return false.  This is called internally during
// device enumeration.
//
// This routine has special-case recognition for a few specific device
// types (Pinscape KL25Z, Pinscape Pico), which it uses to get extended ID
// information through the device's native interfaces.  Special device
// recognition is contrary to the spirit of a standardized device-agnostic
// interface like Open Pinball Device - the whole point is to abstract away
// the device details so that an application doesn't need special code for
// multiple device interfaces.  We justify it in this case on the basis
// that it's not critical to any of our other functions - it's just bonus
// information that's helpful to users, and if we encounter a device we
// don't recognize, we can still fall back on the generic USB device
// identification.
//
struct ExtendedDeviceID
{
	// friendly name
	std::wstring friendlyName;
};
static bool GetExtendedDeviceID(hid_device_info *hEnum, hid_device_info *curDevice, ExtendedDeviceID &xid)
{
	// Scan the other HIDs for matching serial numbers, if this device has a
	// serial number string.  The serial number is a property of the device,
	// so it's necessarily the same for all of the device's interfaces.
	// 
	// There's no USB-level requirement that serial numbers are unique across
	// devices, so even if two interfaces have the same serial number, it
	// doesn't guarantee in general that the interfaces come from the same
	// device.  However, the specific devices that we scan for do provide
	// distinct serial numbers per device: they're unique within the device
	// type, but more importantly, they use long enough identifiers that
	// they're also unlikely to collide with any devices of unlike type.
	// Specifically, Pinscape and Pinscape Pico generate serials based on
	// the 64-bit flash hardware ID; flash hardware IDs are guaranteed by
	// industry standards to be universally unique with respect to other
	// flash IDs, and the 64-bit ID space they use is large enough that
	// collisions with any other ID source is vanishingly improbable.  And
	// even in the extremely unlikely event of a collision with an unlike
	// device type, the unlike device won't provide the device-specific
	// HID interfaces that we use to access the expanded ID information,
	// so in the worst case, we needlessly check and reject the unrelated
	// HIDs.  So the serial number match is just a first-level filter that
	// doesn't have to discriminate perfectly, even though it probably
	// actually does accomplish that in practice.
	std::list<hid_device_info*> sameSerialList;
	if (curDevice->serial_number != nullptr)
	{
		for (auto *h = hEnum ; h != nullptr ; h = h->next)
		{
			// match on serial numbers
			if (h->serial_number != nullptr && wcscmp(h->serial_number, curDevice->serial_number) == 0)
				sameSerialList.emplace_back(h);
		}
	}

	// try identifying the device by the product and manufacturer strings
	std::wregex patPinscapePico(L"PinscapePico\\b.*", std::regex_constants::icase);
	std::wregex patPinscapeKL25Z(L"Pinscape Controller", std::regex_constants::icase);
	if (std::regex_match(curDevice->product_string, patPinscapePico))
	{
		// Pinscape Pico - search for the feedback controller interface
		for (auto *ifc : sameSerialList)
		{
			// open the device
			std::unique_ptr<hid_device, decltype(&hid_close)> hDevice(hid_open_path(ifc->path), &hid_close);
			if (hDevice == nullptr)
				continue;

			// The Pinscape Pico feedback controller interface identifies itself
			// using a String Usage in the report descriptor, in the same manner
			// as Open Pinball Device:
			//
			//   Main interface usage page Generic (0x06), usage undefined (0x00)
			//   Control usage page Generic (0x06), usage undefined (0x00)
			//   Control string usage "PinscapeFeedbackController/<version_number>"
			//
			// See USBProtocol/FeedbackControllerProtocol.h in the Pinscape Pico
			// source code for details on this HID interface.
			static const int USAGE_PAGE_GENERIC = 0x06;
			static const int USAGE_GENERIC_UNDEFINED = 0x00;
			std::wregex pat(L"PinscapeFeedbackController/.*");
			ReportDescriptorInfo ri;
			if (MatchReportTypeByStringUsage(hDevice.get(), USAGE_PAGE_GENERIC, USAGE_GENERIC_UNDEFINED, USAGE_PAGE_GENERIC, USAGE_GENERIC_UNDEFINED, pat, &ri))
			{
					// Send the ID query command (see PinscapePico/USBProtocol/FeedbackControllerInterface.h)
					//
					// Request: QUERY DEVICE IDENTIFICATION
					//   0 <HID report ID:BYTE>
					//   1 <0x01:BYTE>
					//   2 <reserved:BYTE[62]>
					//
					// Reply: IDENTIFICATION REPORT
					//   0 <HID report ID:BYTE>
					//   1 <0x01:BYTE>
					//   2 <UnitNumber:BYTE>
					//   3 <UnitName:CHAR[32]>
					//  35 <ProtocolVer:UINT16>
					//  37 <HardwareID:BYTE[8]>
					//  39 <NumPorts:UINT16>
					//  41 <PlungerType:UINT16>
					//  43 <LedWizUnitMask:UINT16>
					//
				const uint8_t cmd[64]{ ri.reportID, 0x01 };
				uint8_t reply[64];
				if (SendRequestAwaitReply(hDevice.get(), 100, cmd, sizeof(cmd), reply, sizeof(reply), 
					[](uint8_t *buf, size_t len) { return buf[1] == 0x01; }))
				{
					// got it - decode the report
					wchar_t name[128];
					swprintf(name, _countof(name), L"Pinscape Pico unit %d (%.32hs)", reply[2], &reply[3]);
					xid.friendlyName = name;

					// successful identification
					return true;
				}
			}
		}
	}
	else if (std::regex_match(curDevice->product_string, patPinscapeKL25Z))
	{
		// KL25Z Pinscape.  Search for the joystick interface, which doubles
		// as the feedback controller and system config interface, using a
		// custom protocol that incorporates the original LedWiz protocol
		// as a compatible subset.   Alternatively, the same OUT interface
		// can be exposed as Generic Desktop/Undefined when no joystick
		// input is configured.
		for (auto *ifc : sameSerialList)
		{
			// open the device
			std::unique_ptr<hid_device, decltype(&hid_close)> hDevice(hid_open_path(ifc->path), &hid_close);
			if (hDevice == nullptr)
				continue;

			// Check the interface for Generic Desktop/Joystick or Generic Desktop/Undefined usage.
			//
			// It would be nice to be able to check the output report size to make sure it matches
			// the Pinscape protocol's 8-byte report format, but hidapi doesn't give us that
			// information directly.  And we can't even reliably get it from the report descriptor,
			// because hidapi can't correctly reconstruct the OUT report descriptor on Windows.
			// (After investigating, it's not entirely clear to me why this is, but the Windows
			// preparsed data has what looks like a corrupted entry for the OUT report - evidently
			// Pinscape KL25Z's OUT report descriptor format is somehow confusing the Windows HID
			// descriptor parser in such a way that screws up the caps struct it builds, even
			// though the HID parser accepts the descriptor and correctly determines the overall
			// report size.  The frustrating thing is that *the only thing we really want* is the
			// report size, which Windows *does* know, but the portable hidapi interface doesn't
			// have a way to pass that back, so it's buried beyond our reach.)
			static const int USAGE_PAGE_GENERIC_DESKTOP = 0x01;
			static const int USAGE_GENERIC_DESKTOP_UNDEFINED = 0x00;
			static const int USAGE_GENERIC_DESKTOP_JOYSTICK = 0x04;
			if (ifc->usage_page == USAGE_PAGE_GENERIC_DESKTOP
				&& (ifc->usage == USAGE_GENERIC_DESKTOP_JOYSTICK || ifc->usage == USAGE_GENERIC_DESKTOP_UNDEFINED))
			{
				// get the report descriptor (just a synthetic reconstruction on Windows, which isn't 100% accurate)
				std::unique_ptr<unsigned char> reportDescBuf(new unsigned char[HID_API_MAX_REPORT_DESCRIPTOR_SIZE]);
				unsigned char *rp = reportDescBuf.get();
				int rdSize = hid_get_report_descriptor(hDevice.get(), rp, HID_API_MAX_REPORT_DESCRIPTOR_SIZE);
				if (rdSize <= 0)
					continue;

				// parse the report, to obtain the IN report ID (should be same as OUT)
				hidrp::UsageExtractor usageExtractor;
				hidrp::UsageExtractor::Report report;
				usageExtractor.ScanDescriptor(rp, rdSize, report);

				// get the report ID from the first field of the first collection
				uint8_t reportID = 0;
				if (report.collections.size() != 0 && report.collections.front().fields != nullptr && report.collections.front().fields->size() != 0)
					reportID = report.collections.front().fields->front().reportID;

				// Send the configuration report query
				// (See Pinscape_Controller_V2 repos -> USBProtocol.h)
				//
				// Request: Query Configuration Report
				//   <Report ID>   - HID report ID
				//   0x41          - extended protocol command code
				//   0x04          - command subcode, Query Configuration Report
				//   zero padding  - to fill out 9 bytes
				//
				// Reply: Configuration Report (after stripping the report ID, if present)
				//   0 <0x8800:UINT16>     - special report marker for configuration reports
				//   2 <nOutputs:UINT16>   - number of output ports
				//   4 <unitNum:BYTE>      - Pinscape unit number
				//   5 <reserved:BYTE>
				//   6 <calZero:UINT16>    - Plunger calibration data
				//   8 <calMax:UINT16>
				//  10 <calTime:UINT16>
				//  12 <flags:BYTE>        - Capability flags (see Pinscape source)
				//  13 <freeMem:UINT16>    - Free RAM size in bytes
				//
				const uint8_t cmd[9]{ reportID, 0x41, 0x04 };
				uint8_t reply[64];
				int unitID = -1;
				if (SendRequestAwaitReply(hDevice.get(), 100, cmd, sizeof(cmd), reply, sizeof(reply), 
					[reportID, &unitID](uint8_t *buf, size_t len) {

					// if there's a report ID, skip that byte
					if (reportID != 0 && len > 0) ++buf, --len;

					// check the header bytes
					if (buf[0] == 0x00 && buf[1] == 0x88)
					{
						// Matched - recover the unit ID and return success
						unitID = buf[4] + 1;
						return true;
					}

					// not matched
					return false;
				}))
				{
					// Got it - set the friendly name.  Note that the unit number
					// in the configuration report is 0-based, but the Pinscape
					// tools adjust this to 1-based for UI displays (so a 0 in
					// the report is displayed as unit #1).
					wchar_t name[128];
					swprintf(name, _countof(name), L"Pinscape KL25Z unit #%d", unitID + 1);
					xid.friendlyName = name;

					// successful identification
					return true;
				}
			}
		}
	}

	// no vendor-specific information available
	return false;
}


// --------------------------------------------------------------------------
//
// Device enumeration
//

// enumerate devices
std::list<DeviceDesc> OpenPinballDevice::EnumerateDevices()
{
	// create a return list
	std::list<DeviceDesc> list;

    // Get a list of available HID devices.  (Passing VID/PID 0/0 
    // to hid_enumerate() gives us all available HIDs.)
	auto *hEnum = hid_enumerate(0, 0);
	if (hEnum == nullptr)
	{
	    // enumeration failed - return an empty list
		return list;
	}

	// scan the list
	for (auto *cur = hEnum; cur != nullptr; cur = cur->next)
	{
		// check for a generic Pinball Device usage (usage page 0x05 "Game
		// Controls", usage 0x02 "Pinball Device")
		const unsigned short USAGE_PAGE_GAMECONTROLS = 0x05;
		const unsigned short USAGE_GAMECONTROLS_PINBALLDEVICE = 0x02;
		if (cur->usage_page == USAGE_PAGE_GAMECONTROLS && cur->usage == USAGE_GAMECONTROLS_PINBALLDEVICE)
		{
			// It's at least a generic Pinball Device, which is a sort
			// of "base class" (loosely speaking) for Open Pinball Device.
			// To determine if it's an Open Pinball Device specifically,
			// we need to check the HID Input Report Descriptor to see
			// if it matches the OPD report structure.  The OPD report
			// has one Byte Array field with usage 0x00 (unknown/vendor-
			// defined) and an associated Usage String with a unique
			// signature.  The signature string is the real means of
			// positive identification - it's not formally a GUID, but
			// it's meant to be sufficiently long and distinctive to
			// serve the same purpose, as a self-selected universal ID
			// that no other device will ever duplicate by accident.

			// match by string usage
			static const std::wregex pat(L"OpenPinballDeviceStruct/.*");
			ReportDescriptorInfo ri{ 0 };
			if (MatchReportTypeByStringUsage(cur, USAGE_PAGE_GAMECONTROLS, USAGE_GAMECONTROLS_PINBALLDEVICE, USAGE_PAGE_GAMECONTROLS, 0, pat, &ri))
			{
				// Use the USB product name string descriptor as the default
				// friendly name.  If possible, we'll override this with more
				// specific information from the device, if we recognize the
				// device type well enough to ask it for more details.
				const wchar_t *friendlyName = cur->product_string;

				// try getting extended device identification details, for
				// devices that we have special-case support for
				ExtendedDeviceID xid;
				if (GetExtendedDeviceID(hEnum, cur, xid))
				{
					// success - use the friendly name from the device
					friendlyName = xid.friendlyName.c_str();
				}

				// add it to the result list
				list.emplace_back(cur->path, &ri.usageString.c_str()[24], cur->vendor_id, cur->product_id,
					friendlyName, cur->product_string, cur->manufacturer_string,
					cur->serial_number, ri.reportID, ri.reportSize);
			}
		}
	}

	// done with the device list dev
	hid_free_enumeration(hEnum);

	// return the device list
	return list;
}

// --------------------------------------------------------------------------
//
// Aggregate device reader
//

CombinedReader::CombinedReader() 
{
	// enumerate devices
	auto allDevices = EnumerateDevices();

	// open a reader on each device
	for (const auto &device : allDevices)
	{
		// try opening this device
		auto *reader = Reader::Open(device);
		if (reader != nullptr)
		{
			// success - add it to our reader list and active device list
			readers.emplace_back(reader);
			devices.emplace_back(device);
		}
	}
}

CombinedReader::~CombinedReader()
{
}

bool CombinedReader::Read(OpenPinballDeviceReport &report)
{
	// zero the caller's report, so that we can add/OR the fields from the
	// individual reports
	memset(&report, 0, sizeof(report));

	// read raw reports from each device
	bool newReport = false;
	for (auto &reader : readers)
	{
		// read a report from this device
		OpenPinballDeviceReport devReport;
		if (reader->Read(devReport))
			newReport = true;

		// keep the latest time code
		report.timestamp = std::max(report.timestamp, devReport.timestamp);

		// OR all buttons.  Each logical button should only be mapped on
		// a single device, so a button's bit should be zero on every
		// device except the one where it's mapped.
		report.genericButtons |= devReport.genericButtons;
		report.pinballButtons |= devReport.pinballButtons;

		// For the nudge and plunger axes, use any non-zero value reported
		// from any device.  There should never be more than one physical
		// instance of each sensor, even if there are multiple HIDs, so
		// this approach of picking any non-zero input should naturally
		// find the one device that's reporting.  If there actually are
		// multiple devices, our algorithm will have the effect of picking
		// one sensor's input arbitrarily, specifically the last sensor in
		// our HID list order.  That will at least yield consistent results
		// during a session, since the HID list is fixed throughout the
		// session.  There's no practical use case for multiple instances
		// of a given sensor, so we don't need to go out of our way to
		// handle that if it does come up.
		if (devReport.axNudge != 0) report.axNudge = devReport.axNudge;
		if (devReport.ayNudge != 0) report.ayNudge = devReport.ayNudge;
		if (devReport.vxNudge != 0) report.vxNudge = devReport.vxNudge;
		if (devReport.vyNudge != 0) report.vyNudge = devReport.vyNudge;
		if (devReport.plungerPos != 0) report.plungerPos = devReport.plungerPos;
		if (devReport.plungerSpeed != 0) report.plungerSpeed = devReport.plungerSpeed;
	}

	// return true if there were any new reports across all devices
	return newReport;
}


// --------------------------------------------------------------------------
//
// Single device reader
//


// Open a device for reading
Reader *Reader::Open(const DeviceDesc &desc)
{
	// connect to the device
	auto *hDevice = hid_open_path(desc.path.c_str());

	// fail if we couldn't open the device
	if (hDevice == nullptr)
		return nullptr;

	// put it in non-blocking mode
	hid_set_nonblocking(hDevice, 1);

	// create the new reader
	return new Reader(hDevice, desc.reportID, desc.reportSize);
}

Reader::Reader(hid_device *hDevice, uint8_t reportID, size_t deviceReportSize) :
	hDevice(hDevice), reportID(reportID)
{
	// Set up the report reader buffer: size it to match the
	// input report length, and set the first byte to the
	// report ID indicated in the button caps.  The Windows
	// APIs that read HID input reports require the receive
	// buffer to be initialized with the report ID in its
	// first byte.
	readBuf.resize(deviceReportSize);
	readBuf[0] = reportID;

	// size the last-report buffer for the payload portion of
	// the report (minus the HID report ID header byte)
	lastReport.resize(deviceReportSize - 1);
}

Reader::~Reader()
{
	// close the hidapi device
	hid_close(hDevice);
}

intptr_t Reader::GetNativeHandle()
{
#ifdef _WIN32
	// For Windows, use our hid_get_native_handle() modification.
	// Note that this call isn't part of the standard hidapi library;
	// this is something we added for our private Windows version.
	return hid_get_native_handle(hDevice);

#else
	// This isn't currently implemented for other platforms.  It
	// requires a custom, platform-specific modification to the
	// hidapi source files.  Return 0 for "not implemented".
	return 0;

#endif
}

bool Reader::Read(OpenPinballDeviceReport &callerReport)
{
	// Read reports until a read blocks, to be sure that we have
	// the latest report, which gives us the real-time status of
	// the device as of the time the device sent its most recent
	// report.
	bool newReport = false;
	while (ReadRaw(lastReport.data(), lastReport.size()))
	{
		// got a report
		newReport = true;

		// if the report is at least the v1 struct size, process
		// the internal fields
		if (lastReport.size() >= OPENPINDEV_STRUCT_USB_SIZE)
		{
			// interpret the raw byte buffer as a report
			const auto *rp = reinterpret_cast<const OpenPinballDeviceReport*>(lastReport.data());

			// aggregate button state changes from the last report
			genericButtons.Process(rp->genericButtons);
			pinballButtons.Process(rp->pinballButtons);
		}
	}

	// Fill in the caller's buffer with the last report.  Copy the 
	// smaller of the actual struct size from the report or the caller's
	// struct size.  If the caller's struct is bigger, this will leave 
	// fields beyond the end of the device's report set to zero, which 
	// serves as the default for unpopulated fields; if the caller's
	// struct is smaller, this will simply drop the added fields
	// reported by the device, copying only the portion that the caller
	// and device have in common.
	Populate(callerReport, lastReport.data(), lastReport.size());

	// zero bytes past the end of the actual report received
	if (lastReport.size() < sizeof(callerReport))
		memset(reinterpret_cast<uint8_t*>(&callerReport) + lastReport.size(), 0, sizeof(callerReport) - lastReport.size());

	// update the report with the aggregated button state
	callerReport.genericButtons = genericButtons.GetReport();
	callerReport.pinballButtons = pinballButtons.GetReport();

	// return the new-report indication
	return newReport;
}

void Reader::Populate(OpenPinballDeviceReport &r, const uint8_t *buf, size_t bufLen)
{
	// make sure it's at least the v1 struct size
	if (bufLen < OPENPINDEV_STRUCT_USB_SIZE)
	{
		// insufficient data - clear the caller's buffer and stop here
		memset(&r, 0, sizeof(r));
		return;
	}

	// unpack all fields
	r.timestamp = UnpackU64(buf);
	r.genericButtons = UnpackU32(buf);
	r.pinballButtons = UnpackU32(buf);
	r.axNudge = UnpackI16(buf);
	r.ayNudge = UnpackI16(buf);
	r.vxNudge = UnpackI16(buf);
	r.vyNudge = UnpackI16(buf);
	r.plungerPos = UnpackI16(buf);
	r.plungerSpeed = UnpackI16(buf);
}

bool Reader::ReadRaw(void *buf, size_t byteLength)
{
	// read a packet
	int actual = hid_read(hDevice, readBuf.data(), readBuf.size());

	// if we didn't get the requested length, return failure
	if (actual != readBuf.size())
		return false;

	// Copy the payload to the caller's buffer.  The payload starts
	// at the second byte of the packet, following the HID report ID
	// prefix byte.  Copy the smaller of the actual read size or the
	// caller's buffer size.
	memcpy(buf, &readBuf.data()[1], std::min(byteLength, static_cast<size_t>(actual - 1)));

	// success
	return true;
}

void Reader::ButtonState::Process(uint32_t state)
{
	// set the new live state from the report
	live = state;

	// Get the set of buttons that are the same between the pending next
	// report and the last report.  a^b is 0 if a==b and 1 otherwise, so
	// ~(a^b) is 1 iff a==b.
	uint32_t same = ~(next ^ reported);

	// Now set the pending next report state for each unchanged button to
	// match the live state.  This makes changes that occur between the
	// previous and next client reports stick.
	next = (next & ~same) | (state & same);
}

uint32_t Reader::ButtonState::GetReport()
{
	// make the pending next report the new client report, and snapshot
	// the live state as the starting point for the next report
	reported = next;
	next = live;

	// return the client report
	return reported;
}

