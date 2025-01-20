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
#include "hidapi/hidapi.h"
#include "hid-report-parser/hid_report_parser.h"
#include "OpenPinballDeviceLib.h"


// namespace access
using namespace OpenPinballDevice;

// --------------------------------------------------------------------------
//
// Device descriptor
//

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

			// open the device so that we can read its report descriptor
			std::unique_ptr<hid_device, decltype(&hid_close)> hDevice(hid_open_path(cur->path), &hid_close);
			if (hDevice != nullptr)
			{
			    // read the report descriptor
				std::unique_ptr<unsigned char> reportDescBuf(new unsigned char[HID_API_MAX_REPORT_DESCRIPTOR_SIZE]);
				unsigned char *rp = reportDescBuf.get();
				int rdSize = hid_get_report_descriptor(hDevice.get(), rp, HID_API_MAX_REPORT_DESCRIPTOR_SIZE);
				if (rdSize > 0)
				{
				    // parse the usages
					hidrp::DescriptorParser parser;
					hidrp::UsageExtractor usageExtractor;
					hidrp::UsageExtractor::Report report;
					usageExtractor.ScanDescriptor(rp, rdSize, report);

					// scan the collections
					bool found = false;
					for (auto &col : report.collections)
					{
				 	    // check for the generic USB "Pinball Device CA" type (Application Collection, usage page 5, usage 2)
						if (col.type == hidrp::COLLECTION_TYPE_APPLICATION
							&& col.usage_page == USAGE_PAGE_GAMECONTROLS && col.usage == USAGE_GAMECONTROLS_PINBALLDEVICE)
						{
						    // got it - scan the input fields in this collection
							for (auto &f : col.Fields(hidrp::ReportType::input))
							{
						 	    // Check for an opaque byte array, usage 0x00 (undefined/vendor-specific),
						 	    // with an associated usage string that matches the OPD signature string.
								const size_t nStrBuf = 128;
								wchar_t strBuf[nStrBuf];
								if (f.usageRanges.size() == 1 && f.usageRanges.front().Equals(USAGE_PAGE_GAMECONTROLS, 0)
									&& f.stringRanges.size() == 1 && !f.stringRanges.front().IsRange()
									&& hid_get_indexed_string(hDevice.get(), f.stringRanges.front().GetSingle(), strBuf, nStrBuf) == 0
									&& wcsncmp(strBuf, L"OpenPinballDeviceStruct/", 24) == 0)
								{
								   // matched
									found = true;

									// Figure the report size for the associated input report ID.  The
									// report size scanner returns the combined size of all of the fields
									// in the report in BITS, so take ceil(bits/8) to get the report size
									// in bytes.  Then add 1 for the HID Report ID byte prefix that every
									// HID report must include.  This gives us the size of the USB packets
									// the device sends.
									hidrp::ReportSizeScanner sizeScanner;
									parser.Parse(rp, rdSize, &sizeScanner);
									size_t reportSize = (sizeScanner.ReportSize(hidrp::ReportType::input, f.reportID) + 7) / 8 + 1;

									// add it to the result list
									list.emplace_back(cur->path, &strBuf[24], cur->vendor_id, cur->product_id,
										cur->product_string, cur->manufacturer_string, cur->serial_number,
										f.reportID, reportSize);

									// stop searching - there should be only one match
									break;
								}
							}
						}

						// stop as soon as we find a match
						if (found)
							break;
					}
				}
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

