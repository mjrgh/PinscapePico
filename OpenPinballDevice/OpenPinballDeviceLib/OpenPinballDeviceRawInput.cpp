// Open Pinball Device - Win32 Raw Input API support
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements Raw Input helpers for Open Pinball Device.
//
// This is separated from the main OpenPinballDeviceLib implementation file
// because it's highly Windows-specific.  The main library is based on the
// portable hidapi library, which makes it portable to non-Windows systems.

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <memory>
#include <string>
#include <list>
#include <vector>
#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "../OpenPinballDeviceReport.h"
#include "OpenPinballDeviceRawInput.h"
#include "OpenPinballDeviceLib.h"

#pragma comment(lib, "User32")
#pragma comment(lib, "Hid")

using namespace OpenPinballDevice;

RawInputReader::RawInputReader(HWND hwnd)
{
	// get the size of the Raw Input HID device list
	UINT numDevices = 0;
	GetRawInputDeviceList(0, &numDevices, sizeof(RAWINPUTDEVICELIST));

	// allocate space for the list
	std::unique_ptr<RAWINPUTDEVICELIST> buf(new (std::nothrow) RAWINPUTDEVICELIST[numDevices]);
	if (buf.get() != nullptr)
	{
		// Retrieve the device list; fail if that doesn't return the expected device count
		UINT numActual = numDevices;
		if (GetRawInputDeviceList(buf.get(), &numActual, sizeof(RAWINPUTDEVICELIST)) == numDevices)
		{
			// process the list
			RAWINPUTDEVICELIST *r = buf.get();
			for (UINT i = 0; i < numDevices; ++i, ++r)
			{
				RID_DEVICE_INFO info;
				UINT sz = info.cbSize = sizeof(info);
				if (GetRawInputDeviceInfo(r->hDevice, RIDI_DEVICEINFO, &info, &sz) != (UINT)-1)
				{
					switch (info.dwType)
					{
					case RIM_TYPEHID:
						// HID device - check for pinball devices
						if (info.hid.usUsagePage == HID_USAGE_PAGE_GAME	&& info.hid.usUsage == HID_USAGE_GAME_PINBALL_DEVICE)
						{
							// It's has the right usage code to be an Open Pinball Device.
							// Note that Game::Pinball Device is a standard HID usage code,
							// so this isn't necessarily an Open Pinball Device, but AddDevice()
							// will check to make sure, so we don't need do do any further
							// filtering here.
							AddDevice(r->hDevice, &info.hid);
						}
						break;
					}
				}
			}
		}
	}
}

RawInputReader::~RawInputReader()
{
}

bool RawInputReader::ProcessInput(HRAWINPUT hRawInput)
{
	// get the event data header size
	UINT sz = 0;
	std::vector<byte> buf;
	if (GetRawInputData(hRawInput, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) != 0)
		return false;

	// allocate space and retrieve the event data
	buf.resize(sz);
	if (GetRawInputData(hRawInput, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) != sz)
		return false;

	auto &ri = *reinterpret_cast<RAWINPUT*>(buf.data());
	if (ri.header.dwType == RIM_TYPEHID)
	{
		// it's a HID - check if it's one of our known devices
		if (auto it = devices.find(ri.header.hDevice); it != devices.end())
		{
			// It's one of ours - get the device
			auto &device = it->second;
			
			// Unpack the HID data into our struct.
			// Note that the first byte of the raw HID data is the
			// report ID prefix byte, so our struct starts at the
			// second byte of the raw data.
			OpenPinballDeviceReport report{ 0 };
			auto &rh = ri.data.hid;
			Reader::Populate(report, &rh.bRawData[1], rh.dwSizeHid);

			// check to see if it's a new report, based on the timestamp
			if (report.timestamp != device.state.timestamp)
			{
				// update the device report timestamp
				device.state.timestamp = report.timestamp;

				// Update the combined report timestamp.  Use the WINDOWS timestamp
				// for the combined report, since the report timestamps are relative
				// to the device clocks, which are not synchronized to any external
				// reference point and thus aren't meaningful to compare to one
				// another.
				report.timestamp = GetTickCount64();

				// general button checker
				auto CheckButtonChanges = [this](
					uint32_t newReportButtons, uint32_t &deviceReportButtons, uint32_t &combinedReportButtons,
					void (RawInputReader::*callback)(int, bool))
				{
					// check for any changes
					if (newReportButtons != deviceReportButtons)
					{
						// check each button
						uint32_t bit = 0x00000001;
						for (int button = 0 ; button < 32 ; ++button, bit <<= 1)
						{
							// check for a change in this button's state
							if (auto newBit = (newReportButtons & bit); newBit != (deviceReportButtons & bit))
							{
								// invoke the application callback
								(this->*callback)(button, newBit != 0);

								// Update the combined button state.  The basic assumption
								// when multiple Open Pinball Device interfaces are active
								// in the same system is that each abstract control (button,
								// nudge, plunger) is only actually connected on one device.
								// So if we see an update to button N on this device, it
								// means that this is the ONLY device that has button N
								// mapped at all, hence the current state of button N on
								// this device is also the system-wide state of button N.
								combinedReportButtons &= ~bit;
								combinedReportButtons |= newBit;
							}
						}

						// update the device report state
						deviceReportButtons = newReportButtons;
					}
				};

				// check for changes to the generic buttons
				CheckButtonChanges(report.genericButtons, device.state.genericButtons, state.genericButtons, &RawInputReader::OnGenericButton);

				// check for changes to the pinball-function buttons
				CheckButtonChanges(report.pinballButtons, device.state.pinballButtons, state.pinballButtons, &RawInputReader::OnPinballButton);

				// check for changes to nudge axes
				if (report.axNudge != device.state.axNudge
					|| report.ayNudge != device.state.ayNudge
					|| report.vxNudge != device.state.vxNudge
					|| report.vyNudge != device.state.vyNudge)
				{
					// invoke the application callback
					OnNudge(report.axNudge, report.ayNudge, report.vxNudge, report.vyNudge);

					// update the device state
					device.state.axNudge = report.axNudge;
					device.state.ayNudge = report.ayNudge;
					device.state.vxNudge = report.vxNudge;
					device.state.vyNudge = report.vyNudge;

					// update the combined state
					state.axNudge = report.axNudge;
					state.ayNudge = report.ayNudge;
					state.vxNudge = report.vxNudge;
					state.vyNudge = report.vyNudge;
				}

				// check for changes to the plunger
				if (report.plungerPos != device.state.plungerPos
					|| report.plungerSpeed != device.state.plungerSpeed)
				{
					// invoke the application callback
					OnPlunger(report.plungerPos, report.plungerSpeed);

					// update the device state
					device.state.plungerPos = report.plungerPos;
					device.state.plungerSpeed = report.plungerSpeed;

					// update the combined state
					state.plungerPos = report.plungerPos;
					state.plungerSpeed = report.plungerSpeed;
				}
			}

			// event handled
			return true;
		}
	}

	// it's not our event to handle
	return false;
}

bool RawInputReader::ProcessDeviceChange(WPARAM wparam, HANDLE hDevice)
{
	// the WPARAM is the event type (GIDC_xxx)
	switch (wparam)
	{
	case GIDC_ARRIVAL:
		// device added - check to see if it's one of ours
		{
			// get the device information, and check for a HID device with
			// Game::Pinball Device usage
			RID_DEVICE_INFO info{ sizeof(info) };
			UINT sz = sizeof(info);
			if (GetRawInputDeviceInfo(hDevice, RIDI_DEVICEINFO, &info, &sz) != static_cast<UINT>(-1)
				&& info.dwType == RIM_TYPEHID
				&& info.hid.usUsagePage == HID_USAGE_PAGE_GAME && info.hid.usUsage == HID_USAGE_GAME_PINBALL_DEVICE)
			{
				// It's a Pinball Device, so it could be one of ours.  Try adding it.
				// Note that Game::Pinball Device is a standard HID usage code, so
				// this isn't necessarily an Open Pinball Device, but AddDevice()
				// checks to make sure, so we don't need do do any further filtering
				// here.
				return AddDevice(hDevice, &info.hid);
			}
		}
		break;

	case GIDC_REMOVAL:
		// device removed - remove it from our internal list if present
		return RemoveDevice(hDevice);
	}

	// not handled return false;
	return false;
}

bool RawInputReader::AddDevice(HANDLE hDevice, const RID_DEVICE_INFO_HID *info)
{
	// if we already have an entry for this device, ignore it
	if (devices.find(hDevice) != devices.end())
		return false;

	//. get the device path, so that we can open the HID directly
	TCHAR devPath[512]{ 0 };
	UINT sz = _countof(devPath);
	if (GetRawInputDeviceInfo(hDevice, RIDI_DEVICENAME, devPath, &sz) < 0)
		return false;

	// open the HID
	HANDLE fp = CreateFile(devPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
	WCHAR prodName[128] = L"";
	WCHAR serial[128] = L"";
	if (fp == INVALID_HANDLE_VALUE)
		return false;

	// query the product name
	if (!HidD_GetProductString(fp, prodName, _countof(prodName)))
		prodName[0] = 0;

	// query the serial number string
	if (!HidD_GetSerialNumberString(fp, serial, _countof(serial)))
		serial[0] = 0;

	// presume that it's not an Open Pinball Device
	bool matched = false;

	// get the preparsed data
	PHIDP_PREPARSED_DATA ppd = NULL;
	if (HidD_GetPreparsedData(fp, &ppd) && ppd != NULL)
	{
		// get the button caps, to get the usage string associated with the input report
		HIDP_BUTTON_CAPS btnCaps;
		USHORT nBtnCaps = 1;
		if (HidP_GetButtonCaps(HidP_Input, &btnCaps, &nBtnCaps, ppd) == HIDP_STATUS_SUCCESS
			&& btnCaps.NotRange.StringIndex != 0)
		{
			// get the usage string, and check against the OPD struct identifier
			WCHAR str[128]{ 0 };
			if (HidD_GetIndexedString(fp, btnCaps.NotRange.StringIndex, str, _countof(str))
				&& wcsncmp(str, OPENPINDEV_STRUCT_LSTRDESC, _countof(OPENPINDEV_STRUCT_LSTRDESC) - 1) == 0)
			{
				// confirmed - it's an OPD interface
				matched = true;

				// add it to our device list
				devices.emplace(std::piecewise_construct,
					std::forward_as_tuple(hDevice),
					std::forward_as_tuple(hDevice, prodName, serial));
			}
		}

		// done with the preparsed data
		HidD_FreePreparsedData(ppd);
	}

	// done with the HidD device object handle
	CloseHandle(fp);

	// return the matched status
	return matched;
}

bool RawInputReader::RemoveDevice(HANDLE hDevice)
{
	// check if we recognize it
	if (auto it = devices.find(hDevice); it != devices.end())
	{
		// it's one of ours - remove the device from our map
		devices.erase(it);

		// matched
		return true;
	}
	else
	{
		// it's not one of our devices
		return false;
	}
}

