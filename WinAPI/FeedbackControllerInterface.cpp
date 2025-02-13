// Pinscape Pico Device - Feedback Controller API
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <tchar.h>
#include <ctype.h>
#include <memory>
#include <functional>
#include <list>
#include <string>
#include <regex>
#include <algorithm>
struct IUnknown;  // workaround for Microsoft SDK header bug when compiling with a Win 8.1 target
#include <Windows.h>
#include <shlwapi.h>
#include <usb.h>
#include <winusb.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <process.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <SetupAPI.h>
#include <shellapi.h>
#include <winerror.h>
#include "crc32.h"
#include "BytePackingUtils.h"
#include "PinscapePicoAPI.h"
#include "PinscapeVendorInterface.h"
#include "FeedbackControllerInterface.h"

#pragma comment(lib, "cfgmgr32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "winusb")
#pragma comment(lib, "setupapi")
#pragma comment(lib, "shell32")
#pragma comment(lib, "hid")

// this is all in the PinscapePico namespace
using namespace PinscapePico;


// --------------------------------------------------------------------------
//
// Feedback Controller interface object
//

const std::unordered_map<int, const char*> FeedbackControllerInterface::Desc::plungerTypeNameMap{
	{ PinscapePico::FeedbackControllerReport::PLUNGER_NONE, "None" },
	{ PinscapePico::FeedbackControllerReport::PLUNGER_AEDR8300, "AEDR-8300" },
	{ PinscapePico::FeedbackControllerReport::PLUNGER_POT, "Potentiometer" },
	{ PinscapePico::FeedbackControllerReport::PLUNGER_TCD1103, "TCD1103" },
	{ PinscapePico::FeedbackControllerReport::PLUNGER_TSL1410R, "TSL1410R" },
	{ PinscapePico::FeedbackControllerReport::PLUNGER_VCNL4010, "VCNL4010" },
	{ PinscapePico::FeedbackControllerReport::PLUNGER_VL6180X, "VL6180X" },
};

const char *FeedbackControllerInterface::Desc::GetPlungerTypeName(uint16_t typeCode)
{
	auto it = plungerTypeNameMap.find(typeCode);
	return it == plungerTypeNameMap.end() ? "Unknown" : it->second;
}

FeedbackControllerInterface::Desc::Desc(
	int unitNum, const char *unitName, uint16_t ledWizUnitMask,
	const uint8_t *hwId, const WCHAR *path, int numPorts, int plungerType,
	const VendorInterfaceDesc &vendorIfcDesc) :
	unitNum(unitNum), unitName(unitName), ledWizUnitMask(ledWizUnitMask), hwId(hwId),
	path(path), numOutputPorts(numPorts), plungerType(plungerType),
	vendorIfcDesc(vendorIfcDesc)
{
	if (auto it = plungerTypeNameMap.find(plungerType); it != plungerTypeNameMap.end())
		plungerTypeName = it->second;
}


HRESULT FeedbackControllerInterface::Enumerate(std::list<Desc> &units)
{
	// clear any prior list contents
	units.clear();

	// Enumerate Pinscape Pico vendor interfaces.  These devices are
	// positively identifiable as Pinscape Picos because they exhibit
	// the Pinscape vendor interface GUID, which is unique to our
	// device type.  Given a vendor interface, we can get the parent
	// device, which represents the USB composite device that all of
	// the Pinscape interfaces are part of.  All of our HID interfaces
	// are also children of the same parent, so we can find Pinscape
	// HID interfaces by enumerating all HIDs and filtering for the
	// ones that have parents in common with vendor interfaces.  This
	// also lets us figure the association between a particular HID
	// and a particular vendor interface.
	std::list<VendorInterfaceDesc> vendorIfcs;
	HRESULT hresult = VendorInterface::EnumerateDevices(vendorIfcs);
	if (!SUCCEEDED(hresult))
		return hresult;

	// Get each vendor interface's parent, to identify the Pinscape
	// USB composite device object.
	struct VendorItem
	{
		VendorItem(VendorInterfaceDesc path, DEVINST compositeIfc) : desc(path), compositeIfc(compositeIfc) { }
		VendorInterfaceDesc desc;
		DEVINST compositeIfc;
	};
	std::list<VendorItem> vendorItems;
	for (auto &v : vendorIfcs)
	{
		DEVINST parent;
		DEVINST di = NULL;
		if ((CM_Locate_DevNodeW(&di, const_cast<DEVINSTID_W>(v.DeviceInstanceId()), CM_LOCATE_DEVNODE_NORMAL)) == CR_SUCCESS
			&& CM_Get_Parent(&parent, di, 0) == CR_SUCCESS)
			vendorItems.emplace_back(v, parent);
	}

	// Set up a device set for all currently connected HID devices.
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);
	HDEVINFO hdi = SetupDiGetClassDevs(&hidGuid, NULL, NULL,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (hdi == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

	// enumerate HID interfaces in the device set
	SP_DEVICE_INTERFACE_DATA did{ sizeof(SP_DEVICE_INTERFACE_DATA) };
	for (DWORD memberIndex = 0 ;
		SetupDiEnumDeviceInterfaces(hdi, NULL, &hidGuid, memberIndex, &did) ;
		++memberIndex)
	{
		// retrieve the required buffer size for device detail
		DWORD diDetailSize = 0;
		DWORD err = 0;
		if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, NULL, 0, &diDetailSize, NULL)
			&& (err = GetLastError()) != ERROR_INSUFFICIENT_BUFFER)
		{
			hresult = HRESULT_FROM_WIN32(err);
			break;
		}

		// retrieve the device detail and devinfo data
		std::unique_ptr<SP_DEVICE_INTERFACE_DETAIL_DATA_W> diDetail(
			reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(new BYTE[diDetailSize]));
		diDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		SP_DEVINFO_DATA devInfo{ sizeof(SP_DEVINFO_DATA) };
		if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, diDetail.get(), diDetailSize, NULL, &devInfo))
		{
			hresult = HRESULT_FROM_WIN32(GetLastError());
			break;
		}

		// Get the grandparent IDs.  In the Windows device hierarchy, the HID's
		// parent is a HID composite interface, whose parent is the USB composite
		// device interface, which is also the parent of the vendor interface
		// that serves as our reference point.
		DEVINST devInstParent = NULL, devInstGrandparent = NULL;
		if (CM_Get_Parent(&devInstParent, devInfo.DevInst, 0) == CR_SUCCESS
			&& CM_Get_Parent(&devInstGrandparent, devInstParent, 0) == CR_SUCCESS)
		{
			// search for a Pinscape vendor interface with the same composite
			// device parent
			auto itVendorIfc = std::find_if(vendorItems.begin(), vendorItems.end(),
				[devInstParent, devInstGrandparent](VendorItem &v) { return v.compositeIfc == devInstGrandparent; });

			// if we didn't match it, it's not a Pinscape HID
			if (itVendorIfc == vendorItems.end())
				continue;

			// open the device desc to access the HID
			HANDLE hDevice = CreateFileW(diDetail->DevicePath,
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, 0, NULL);
			if (hDevice != INVALID_HANDLE_VALUE)
			{
				// presume this isn't the one we're looking for
				bool matched = false;

				// Check to see if this has the right shape for the Feedback
				// Controller HID interface.  The Feedback Controller HID
				// has Usage Page 0x06 (Generic Device), Usage 0x00 (Undefined),
				// and it has 64-byte IN and OUT reports.
				PHIDP_PREPARSED_DATA ppd;
				if (HidD_GetPreparsedData(hDevice, &ppd))
				{
					// get the device capabilities
					HIDP_CAPS caps;
					if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
					{
						// check for a match to our interface specs
						if (caps.UsagePage == 0x06 && caps.Usage == 0x00
							&& caps.InputReportByteLength == 64
							&& caps.OutputReportByteLength == 64)
						{
							// It's a match for our interface specs.  As a
							// final check, the Feedback Controller interface
							// defines a string label on its input and output
							// reports to uniquely identify the Pinscape type.
							// This ensures that we're not mistaking some other
							// unrelated device that also happens to use the
							// Generic Device/Undefined usage - which is
							// entirely possible given that this usage is by
							// definition generic, so usable by any device
							// that has features not covered by predefined
							// HID usages.
							//
							// Windows treats our array-of-bytes reports as
							// buttons, so we need to get the button caps.
							std::vector<HIDP_BUTTON_CAPS> btnCaps;
							btnCaps.resize(caps.NumberInputButtonCaps);
							USHORT nBtnCaps = caps.NumberInputButtonCaps;
							if (HidP_GetButtonCaps(HIDP_REPORT_TYPE::HidP_Input, btnCaps.data(), &nBtnCaps, ppd) == HIDP_STATUS_SUCCESS
								&& nBtnCaps == 1)
							{
								// check for a string index
								USHORT stringIndex = btnCaps[0].NotRange.StringIndex;
								WCHAR str[128]{ 0 };
								std::wregex pat(L"PinscapeFeedbackController/(\\d+)");
								std::match_results<const wchar_t*> m;
								if (stringIndex != 0
									&& HidD_GetIndexedString(hDevice, stringIndex, str, sizeof(str))
									&& std::regex_match(str, m, pat))
								{
									// success
									int ifcVersion = _wtoi(m[1].str().c_str());
									matched = true;
								}
							}
						}
					}

					// done with the preparsed data
					HidD_FreePreparsedData(ppd);
				}

				// done with the handle
				CloseHandle(hDevice);

				// if this is the correct interface, include it in the list
				if (matched)
				{
					// Create a feedback controller interface
					FeedbackControllerInterface f(diDetail->DevicePath);

					// Query the device's internal IDs, to obtain the unique
					// hardware identifier and the Pinscape unit number.
					FeedbackControllerInterface::IDReport id;
					if (f.Query(FeedbackRequest{ FeedbackRequest::REQ_QUERY_ID }, id, 100))
					{
						// success - add the unit to the result list
						units.emplace_back(id.unitNum, id.unitName, id.ledWizUnitMask,
							id.hwid, diDetail->DevicePath, id.numPorts, id.plungerType,
							itVendorIfc->desc);
					}
				}
			}
			else
			{
				// Failed - check for ACCESS DENIED errors, which can occur
				// with some keyboard interfaces.  Ignore these, as they're
				// not for Pinscape units anyway.  Fail for other errors.
				//
				// The ACCESS DENIED error for keyboards isn't documented
				// anywhere I can find, but there are some mentions of it
				// on the Internet (from other people doing similar device
				// enumerations who encountered it) that say it was added
				// in some Windows 10 update, and the speculation is that
				// it's either an intentional security measure against
				// keyboard loggers, or just an outright regression.  The
				// apparent lack of documentation suggests the latter, but
				// it's still in Windows 11 as of 2024, so if it's a bug,
				// Microsoft might not consider it worth fixing.  Note that
				// the access error *shouldn't* happen at all in this code,
				// because we didn't ask for GENERIC_READ or _WRITE access,
				// but we explicitly test for and ignore it anyway, just in
				// case Microsoft (accidentally or on purpose) expands the
				// scope of the error in the future to include opens with
				// no access rights requested.
				if ((err = GetLastError()) != ERROR_ACCESS_DENIED)
					return HRESULT_FROM_WIN32(err);
			}
		}
	}

	// done with the device list handle
	SetupDiDestroyDeviceInfoList(hdi);

	// return the result code
	return hresult;
}

FeedbackControllerInterface::FeedbackControllerInterface(HANDLE handle, const WCHAR *path) :
	handle(handle), path(path)
{
	// initialize I/O
	Init();
}

FeedbackControllerInterface::FeedbackControllerInterface(const WCHAR *path) : 
	path(path)
{
	// open a handle on the HID interface
	handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

	// initialize I/O
	Init();
}

void FeedbackControllerInterface::Init()
{
	// initialize the overlapped I/O event
	hReadEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	hWriteEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	// Move our first read.  We always keep an overlapped read
	// outstanding.
	QueueRead();
}

FeedbackControllerInterface::~FeedbackControllerInterface()
{
	// cancel all outstanding I/O requests on the handle
	if (!HasOverlappedIoCompleted(&ov))
		CancelIo(handle);

	// close handles
	CloseHandle(handle);
	CloseHandle(hReadEvent);
	CloseHandle(hWriteEvent);
}

FeedbackControllerInterface *FeedbackControllerInterface::Open(const Desc &desc)
{
	// create the interface
	std::unique_ptr<FeedbackControllerInterface> f(new FeedbackControllerInterface(desc.path.c_str()));

	// if the file handle is invalid, return failure
	if (f->handle == INVALID_HANDLE_VALUE)
		return nullptr;

	// release the interface to the caller
	return f.release();
}

FeedbackControllerInterface *FeedbackControllerInterface::Open(int unitNum)
{
	// enumerate available units
	std::list<Desc> units;
	if (!SUCCEEDED(Enumerate(units)))
		return nullptr;

	// find the device by unit number
	if (auto it = std::find(units.begin(), units.end(), unitNum) ; it != units.end())
		return Open(*it);

	// no matching unit
	return nullptr;
}

bool FeedbackControllerInterface::TestFileSystemPath() const
{
	// try opening the file system path
	HANDLE h = CreateFileW(path.c_str(),
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
		// file opened - close the handle and return success
		CloseHandle(h);
		return true;
	}

	// unable to open handle
	return false;
}

bool FeedbackControllerInterface::SendIR(const IRCommand &cmd, uint8_t repeatCount, DWORD timeout)
{
	// prepare the request
	FeedbackRequest req{ FeedbackRequest::REQ_IR_TX };
	uint8_t *p = req.args;
	*p++ = cmd.protocol;
	*p++ = cmd.flags;
	PutUInt64(p, cmd.command);
	*p++ = repeatCount;

	// send it
	return Write(req, timeout);
}

bool FeedbackControllerInterface::SendClockTime(DWORD timeout)
{
	// get the local system time
	SYSTEMTIME t;
	GetLocalTime(&t);

	// prepare the request
	FeedbackRequest req{ FeedbackRequest::REQ_SET_CLOCK };
	uint8_t *p = req.args;
	PutUInt16(p, t.wYear);
	*p++ = static_cast<uint8_t>(t.wMonth);
	*p++ = static_cast<uint8_t>(t.wDay);
	*p++ = static_cast<uint8_t>(t.wHour);
	*p++ = static_cast<uint8_t>(t.wMinute);
	*p++ = static_cast<uint8_t>(t.wSecond);

	// send it
	return Write(req, timeout);
}

bool FeedbackControllerInterface::SetPortBlock(
	int firstPortNum, int nPorts, const uint8_t *levels, DWORD timeout)
{
	// Validate firstPortNum and nPorts.  The port number has to
	// be in 1..255, and the number of ports must be in 1..60.
	// If any of the parameters are invalid, ignore the call.
	if (firstPortNum < 1 || firstPortNum > 255
		|| nPorts < 1 || nPorts > 60)
		return false;

	// build the request
	FeedbackRequest req{ FeedbackRequest::REQ_SET_PORT_BLOCK };
	uint8_t *p = req.args;
	*p++ = static_cast<uint8_t>(nPorts);
	*p++ = static_cast<uint8_t>(firstPortNum);
	memcpy(p, levels, nPorts);

	// send the request and return the result
	return Write(req, timeout);
}

bool FeedbackControllerInterface::SetPorts(
	int nPorts, const uint8_t *pairs, DWORD timeout)
{
	// Validate the port count - it has to be 1..30
	if (nPorts < 1 || nPorts > 30)
		return false;

	// build the request
	FeedbackRequest req{ FeedbackRequest::REQ_SET_PORTS };
	uint8_t *p = req.args;
	*p++ = static_cast<uint8_t>(nPorts);
	memcpy(p, pairs, static_cast<size_t>(nPorts) * 2);

	// send the request and return the result
	return Write(req, timeout);
}

bool FeedbackControllerInterface::LedWizSBA(int firstPortNum,
	uint8_t bank0, uint8_t bank1, uint8_t bank2, uint8_t bank3,
	uint8_t globalPulseSpeed, DWORD timeout)
{
	// build the request
	FeedbackRequest req{ FeedbackRequest::REQ_LEDWIZ_SBA };
	uint8_t *p = req.args;
	*p++ = static_cast<uint8_t>(firstPortNum);
	*p++ = bank0;
	*p++ = bank1;
	*p++ = bank2;
	*p++ = bank3;
	*p++ = globalPulseSpeed;

	// send the request and return the result
	return Write(req, timeout);
}

bool FeedbackControllerInterface::LedWizPBA(
	int firstPortNum, int nPorts, const uint8_t *profiles, DWORD timeout)
{
	// Validate the port count - it has to be 1..60
	if (nPorts < 1 || nPorts > 60)
		return false;

	// build the request
	FeedbackRequest req{ FeedbackRequest::REQ_LEDWIZ_PBA };
	uint8_t *p = req.args;
	*p++ = static_cast<uint8_t>(firstPortNum);
	*p++ = static_cast<uint8_t>(nPorts);
	memcpy(p, profiles, nPorts);

	// send the request and return the result
	return Write(req, timeout);
}

bool FeedbackControllerInterface::Write(const FeedbackRequest &req, DWORD timeout)
{
	// build the full HID request by adding the report ID prefix
	uint8_t buf[64]{ PinscapePico::FEEDBACK_CONTROLLER_HID_REPORT_ID, req.type };
	memcpy(&buf[2], req.args, 62);
	return WriteRaw(buf, timeout);
}

bool FeedbackControllerInterface::Write(const uint8_t *data, size_t nBytes, DWORD timeout)
{
	uint8_t buf[64]{ PinscapePico::FEEDBACK_CONTROLLER_HID_REPORT_ID };
	memcpy(&buf[1], data, min(nBytes, 63));
	return WriteRaw(buf, timeout);
}

bool FeedbackControllerInterface::WriteRaw(const uint8_t *buf, DWORD timeout)
{
	// set up the overlapped write struct
	OVERLAPPED ovw;
	memset(&ovw, 0, sizeof(ovw));
	ovw.hEvent = hWriteEvent;

	// queue the write
	DWORD bytesWritten = 0;
	if (WriteFile(handle, buf, 64, &bytesWritten, &ovw))
	{
		// success - make sure the size matches
		if (bytesWritten != 64)
		{
			writeErr = ERROR_BAD_LENGTH;
			return false;
		}

		// success
		return true;
	}
	else if (auto err = GetLastError(); err == ERROR_IO_PENDING)
	{
		// I/O pending - wait for completion, up to the timeout
		UINT64 tStop = GetTickCount64() + timeout;
		for (;;)
		{
			// check for completion
			UINT64 now = GetTickCount64();
			DWORD curTimeout = (timeout == INFINITE) ? INFINITE : (now > tStop) ? 0 : static_cast<DWORD>(tStop - now);
			if (GetOverlappedResultEx(handle, &ovw, &bytesWritten, curTimeout, TRUE))
			{
				// successful completion - check the length
				if (bytesWritten != 64)
				{
					writeErr = ERROR_BAD_LENGTH;
					return false;
				}

				// success
				writeErr = 0;
				return true;
			}

			// not completed - check the error
			DWORD err = GetLastError();
			if (err == ERROR_IO_INCOMPLETE || err == WAIT_TIMEOUT)
			{
				// Request timed out.  Set the error code to indicate
				// timeout, cancel the I/O operation, and return failure.
				CancelIoEx(handle, &ovw);
				writeErr = ERROR_TIMEOUT;
				return false;
			}
			else if (waitErr == WAIT_IO_COMPLETION)
			{
				// The request was interrupted by an "alert" (some other I/O
				// completion routine or APC was queued).  Our own write is
				// still pending, so go back and do another wait.
				continue;
			}
			else
			{
				// This is an actual error, so presumably our I/O has failed.
				// Cancel it to be sure and return failure.
				CancelIoEx(handle, &ovw);
				writeErr = err;
				return false;
			}
		}
	}
	else
	{
		// write failed - stash the error and return failure
		writeErr = err;
		return false;
	}
}

bool FeedbackControllerInterface::Read(FeedbackReport &rpt, DWORD timeout)
{
	// return the current buffer and queue a new read
	auto ReturnData = [&rpt, this]()
	{
		// Make sure it's the expected size
		waitErr = 0;
		if (bytesRead == 64)
		{
			// copy the data to the caller's request struct, minus the first
			// byte containing the HID Report ID code
			memcpy(&rpt, &readBuf[1], 63);
			static_assert(sizeof(rpt) == 63);
		}
		else
		{
			// wrong length
			waitErr = ERROR_BAD_LENGTH;
		}

		// queue the next read
		QueueRead();

		// return success if there was no wait error
		return waitErr == 0;
	};

	// if the last read completed synchronously, simply return the data
	if (readErr == 0)
		return ReturnData();

	// wait for the last queued read to complete
	UINT64 tStop = GetTickCount64() + timeout;
	for (;;)
	{
		// check for completion
		UINT64 now = GetTickCount64();
		DWORD curTimeout = (timeout == INFINITE) ? INFINITE : (now > tStop) ? 0 : static_cast<DWORD>(tStop - now);
		if (GetOverlappedResultEx(handle, &ov, &bytesRead, curTimeout, TRUE))
		{
			// success - the request data
			return ReturnData();
		}

		// update the error indicator for the new wait
		waitErr = GetLastError();
		if (waitErr == ERROR_IO_INCOMPLETE || waitErr == WAIT_TIMEOUT)
		{
			// request timed out - simply return failure with the request
			// still running
			return false;
		}
		else if (waitErr == WAIT_IO_COMPLETION)
		{
			// The request was interrupted by an "alert" (some other I/O
			// completion routine or APC was queued).  This allows the
			// system to perform an asynchronous operation on this thread
			// while a wait is in progress.  Our read is still in progress,
			// so resume the wait.
			continue;
		}
		else
		{
			// This is an actual error, so presumably our I/O has failed.
			// Cancel it just to be sure, and queue a new read.
			CancelIoEx(handle, &ov);
			QueueRead();

			// Return failure.  Note that the error code that interrupted
			// our wait gets dropped here, since it's probably more useful
			// to keep the error code from the new read we just queued.
			return false;
		}
	}

	// timed out
	return false;
}

void FeedbackControllerInterface::QueueRead()
{
	// reset the OVERLAPPED struct
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = hReadEvent;

	// start the read
	if (ReadFile(handle, readBuf, sizeof(readBuf), &bytesRead, &ov))
		readErr = 0;
	else
		readErr = GetLastError();
}

bool FeedbackControllerInterface::QueryRaw(
	const FeedbackRequest &request, FeedbackReport &report, uint8_t replyType, DWORD timeout_ms)
{
	// figure the absolute end time
	UINT64 tTimeout = timeout_ms == INFINITE ? ~0ULL : GetTickCount64() + timeout_ms;

	// send an ID request to the device
	if (Write(request, timeout_ms))
	{
		// read until the timeout expires, since there might be other
		// report types (already buffered on the PC side, or pending on
		// the device side) that we need to skip
		UINT64 now;
		while ((now = GetTickCount64()) <= tTimeout)
		{
			// figure the new timeout
			if (timeout_ms != INFINITE)
				timeout_ms = static_cast<DWORD>(tTimeout - now);

			// read a report, skip anything but ID reports
			if (Read(report, timeout_ms) && report.type == replyType)
				return true;
		}
	}

	// timed out
	return false;
}

bool FeedbackControllerInterface::Decode(IDReport &id, const FeedbackReport &rpt)
{
	if (rpt.type != FeedbackReport::RPT_ID)
		return false;

	const uint8_t *p = rpt.args;
	id.unitNum = *p++;
	memcpy(id.unitName, p, 32);
	p += 32;
	id.protocolVersion = GetUInt16(p);
	GetBytes(p, id.hwid, 8);
	id.numPorts = GetUInt16(p);
	id.plungerType = GetUInt16(p);
	id.ledWizUnitMask = GetUInt16(p);
	return true;
}

bool FeedbackControllerInterface::Decode(StatusReport &stat, const FeedbackReport &rpt)
{
	// validate the report type
	if (rpt.type != FeedbackReport::RPT_STATUS)
		return false;

	// get the arguments
	const uint8_t *p = rpt.args;

	// decode flags
	uint8_t flags = stat.flags = *p++;
	stat.plungerEnabled = ((flags & 0x01) != 0);
	stat.plungerCalibrated = ((flags & 0x02) != 0);
	stat.nightMode = ((flags & 0x04) != 0);
	stat.clockSet = ((flags & 0x08) != 0);
	stat.safeMode = ((flags & 0x10) != 0);
	stat.configLoaded = ((flags & 0x20) != 0);

	// get the TV ON state
	stat.tvOnState = *p++;

	// get the status LED color into a COLORREF
	uint8_t ledR = *p++;
	uint8_t ledG = *p++;
	uint8_t ledB = *p++;
	stat.led = RGB(ledR, ledG, ledB);

	// success
	return true;
}


bool FeedbackControllerInterface::Decode(IRReport &ir, const FeedbackReport &rpt)
{
	// validate the report type
	if (rpt.type != FeedbackReport::RPT_IR_COMMAND)
		return false;

	// get the arguments
	const uint8_t *p = rpt.args;

	// protocol ID
	ir.command.protocol = *p++;

	// decode the protocol flags
	uint8_t proFlags = ir.command.flags = *p++;
	ir.command.proHasDittos = (proFlags & 0x02) != 0;

	// decode the command code
	ir.command.command = GetUInt64(p);

	// decode the command instance flags
	uint8_t cmdFlags = ir.command.cmdFlags = *p++;
	ir.command.hasToggle = (cmdFlags & 0x01) != 0;
	ir.command.toggle = (cmdFlags & 0x02) != 0;
	ir.command.hasDitto = (cmdFlags & 0x04) != 0;
	ir.command.ditto = (cmdFlags & 0x08) != 0;
	ir.command.posCode = static_cast<uint8_t>((cmdFlags & 0x30) >> 4);
	ir.command.isAutoRepeat = (cmdFlags & 0x40) != 0;

	// decode the elapsed since the last command received
	ir.command.elapsedTime_us = GetUInt64(p);

	// success
	return true;
}

