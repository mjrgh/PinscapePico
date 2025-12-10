// Pinscape Pico Button Latency Tester II - Vendor Interface API
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

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
#include <Windows.h>
#include <shlwapi.h>
#include <usb.h>
#include <winusb.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <process.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <SetupAPI.h>
#include <shellapi.h>
#include <winerror.h>
#include <timeapi.h>
#include <Dbt.h>
#include "../../Firmware/crc32.h"
#include "../../Firmware/BytePackingUtils.h"
#include "HiResTimer.h"
#include "BLTVendorInterface.h"

// Windows API library dependencies
#pragma comment(lib, "cfgmgr32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "winusb")
#pragma comment(lib, "setupapi")

// hi-res timer, for Pico clock sync
static HiResTimer hrt;

// Unicode/multibyte conversion utilities
#ifdef UNICODE
std::basic_string<TCHAR> ToTSTRING(const std::wstring &s) { return s; }
#else
std::basic_string<TCHAR> ToTSTRING(const std::wstring &s) 
{
	std::basic_string<TCHAR> dst;
	std::transform(s.begin(), s.end(), dst.begin(), [](wchar_t c) { return c < 255 ? static_cast<TCHAR>(c) : '?'; });
	return dst;
}
#endif

// "Offset Next" - offset of next field in struct type s after m.  This
// is the size of the struct up to and including m, to check if m is
// included in a live copy of the struct with known dynamic size.
#define offsetnext(s, m) (offsetof(s, m) + sizeof(s::m))

// for convenience, import the whole ButtonLatencyTester2 namespace
using namespace ButtonLatencyTester2;

// Generic Windows handle holder
template<typename H> class HandleHolder
{
public:
	HandleHolder(std::function<void(H)> deleter) : handle(NULL), deleter(deleter) { }
	HandleHolder(H handle, std::function<void(H)> deleter) :
		handle(handle), deleter(deleter) { }

	~HandleHolder() {
		if (handle != NULL)
			deleter(handle);
	}

	void reset(H h)
	{
		if (handle != NULL)
			deleter(handle);
		handle = h;
	}

	H get() { return handle; }
	H release()
	{
		H h = handle;
		handle = NULL;
		return h;
	}

	H* operator &() { return &handle; }

	H handle;
	std::function<void(H)> deleter;
};

// statistics
uint64_t VendorInterface::qpcTickTime_us64 = VendorInterface::GetQPCTickTime_us64();
uint64_t VendorInterface::GetQPCTickTime_us64()
{
	LARGE_INTEGER freq;
	if (QueryPerformanceFrequency(&freq) && freq.QuadPart != 0)
	{
		// QueryPerformanceCounter is available - use it to calculate
		// times.  Calculate the time in microseconds per QPC tick and
		// store this for use when calculating time intervals.
		double tickTime_us = 1.0e6 / freq.QuadPart;
		return static_cast<uint64_t>(tickTime_us * 65536.0);
	}
	else
	{
		MessageBoxA(NULL, "Button Latency Tester II",
			"QueryPerformanceCounter() high-precision timekeeping API is not available. "
			"This program requires the QPC subsystem for proper operation.",
			MB_ICONERROR | MB_OK);
		exit(1);
	}
}

// Open a device
HRESULT VendorInterfaceDesc::Open(VendorInterface* &device) const
{
	HandleHolder<HANDLE> hDevice(CreateFileW(
		path.c_str(), GENERIC_WRITE | GENERIC_READ,
		FILE_SHARE_WRITE | FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL), CloseHandle);

	// make sure that succeeded
	if (hDevice.get() == INVALID_HANDLE_VALUE)
		return hDevice.release(), HRESULT_FROM_WIN32(GetLastError());

	// open the WinUSB handle
	HandleHolder<WINUSB_INTERFACE_HANDLE> winusbHandle(WinUsb_Free);
	if (!WinUsb_Initialize(hDevice.get(), &winusbHandle))
		return HRESULT_FROM_WIN32(GetLastError());

	// get the device descriptor
	USB_DEVICE_DESCRIPTOR devDesc{ 0 };
	DWORD xferSize = 0;
	if (!WinUsb_GetDescriptor(winusbHandle.get(), USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
		reinterpret_cast<BYTE*>(&devDesc), sizeof(devDesc), &xferSize))
		return HRESULT_FROM_WIN32(GetLastError());

	// Get the serial number string.  For our purposes, we want to
	// interpret this as a simple WCHAR string, but the actual string
	// descriptor we'll get back is more properly a packed struct of
	// { uint8_t, uint8_t, uint16_t[] }.   As long as WCHAR is defined
	// uint16_t, we can treat the first two uint8_t elements as packing
	// into a single uint16_t, and then just pretend the whole thing is
	// a uint16_t array, which is the same as a WCHAR array as long as
	// our assumption that WCHAR==uint16_t holds.  Note that the string
	// descriptor isn't null-terminated, so zero the buffer in advance
	// to ensure that we have a null-terminated wide-character string
	// when we're done.  Note also that the actual string portion starts
	// at index [1] in the WCHAR array, because of those pesky two
	// uint8_t elements at the beginning (which are, by the way, the
	// length in bytes of the overall descriptor, and the USB type code
	// for string descriptor, 0x03).
	WCHAR serialBuf[128];
	static_assert(sizeof(WCHAR) == sizeof(uint16_t));
	ZeroMemory(serialBuf, sizeof(serialBuf));
	if (!WinUsb_GetDescriptor(winusbHandle.get(), USB_STRING_DESCRIPTOR_TYPE,
		devDesc.iSerialNumber, 0x0409 /* language = English */,
		reinterpret_cast<BYTE*>(serialBuf), sizeof(serialBuf), &xferSize))
		return HRESULT_FROM_WIN32(GetLastError());

	// get the interface settings
	USB_INTERFACE_DESCRIPTOR ifcDesc;
	ZeroMemory(&ifcDesc, sizeof(ifcDesc));
	if (!WinUsb_QueryInterfaceSettings(winusbHandle.get(), 0, &ifcDesc))
		return HRESULT_FROM_WIN32(GetLastError());

	// scan the endpoints for the data endpoints
	int epIn = -1, epOut = -1;
	for (unsigned i = 0 ; i < ifcDesc.bNumEndpoints ; ++i)
	{
		// query this endpoint
		WINUSB_PIPE_INFORMATION pipeInfo;
		ZeroMemory(&pipeInfo, sizeof(pipeInfo));
		if (WinUsb_QueryPipe(winusbHandle.get(), 0, i, &pipeInfo))
		{
			// the data endpoints we seek are the "bulk" pipes
			if (pipeInfo.PipeType == UsbdPipeTypeBulk)
			{
				// check the direction
				if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId))
					epIn = pipeInfo.PipeId;
				if (USB_ENDPOINT_DIRECTION_OUT(pipeInfo.PipeId))
					epOut = pipeInfo.PipeId;
			}
		}
	}

	// make sure we found the data pipes
	if (epIn < 0 || epOut < 0)
		return E_FAIL;

	// set the In endpoint's SHORT PACKET policy to fulfill read requests
	// that are smaller than the endpoint packet size
	BYTE policyBool = FALSE;
	WinUsb_SetPipePolicy(winusbHandle.get(), epIn, IGNORE_SHORT_PACKETS,
		static_cast<ULONG>(sizeof(policyBool)), &policyBool);

	// Set up time sync tracking, so that we can get the SOF time
	// and frame counter in HostInputEvent().
	USB_START_TRACKING_FOR_TIME_SYNC_INFORMATION syncInfo{ NULL, TRUE };
	WinUsb_StartTrackingForTimeSync(winusbHandle.get(), &syncInfo);

	// Create a device object for the caller, releasing ownership of
	// the handles to the new object.
	//
	// Note that serialBuf[] is in the raw USB String Descriptor format,
	// which means that the first two bytes contain the length in bytes
	// of the overall object, and the USB Descriptor type code (0x03).
	// So the actual serial number starts at the third byte, which
	// happens to be at WCHAR offset 1.  (It would be more rigorous to
	// reinterpret the buffer as a packed struct of uint8_t, uint8_t,
	// and a uint16_t array, and then pass the address of the uint16_t
	// array to the constructor.  But since the first two uint8_t elements
	// are always packed into the struct, we can count on the byte layout
	// being such that the string portion starts at the third byte, so we
	// can interpret the whole thing as a uint16_t buffer.
	device = new VendorInterface(hDevice.release(), winusbHandle.release(),
		syncInfo.TimeTrackingHandle, path, deviceInstanceId, &serialBuf[1], epIn, epOut);

	// success
	return S_OK;
}

VendorInterface::~VendorInterface()
{
	CloseDeviceHandle();
}

// Open a device into a unique_ptr
HRESULT VendorInterfaceDesc::Open(std::unique_ptr<VendorInterface> &pDevice) const
{
	// open the device through the naked pointer interface
	VendorInterface *dev = nullptr;
	HRESULT hr = Open(dev);

	// if that succeeded, store the naked pointer in the unique pointer
	if (SUCCEEDED(hr))
		pDevice.reset(dev);

	// return the result code
	return hr;
}

// Open a device into a shared_ptr
HRESULT VendorInterfaceDesc::Open(std::shared_ptr<VendorInterface> &pDevice) const
{
	// open the device through the naked pointer interface
	VendorInterface *dev = nullptr;
	HRESULT hr = Open(dev);

	// if that succeeded, store the naked pointer in the shared pointer
	if (SUCCEEDED(hr))
		pDevice.reset(dev);

	// return the result code
	return hr;
}


// Get the CDC (virtual COM) port associated with this interface
bool VendorInterface::GetCDCPort(TSTRING &name) const
{
	// get my device node
	DEVINST di = NULL;
	CONFIGRET cres = CR_SUCCESS;
	if ((cres = CM_Locate_DevNodeW(&di, const_cast<WCHAR*>(deviceInstanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL)) != CR_SUCCESS)
		return false;

	// retrieve my parent device node
	DEVINST devInstParent = NULL;
	if (CM_Get_Parent(&devInstParent, di, 0) != CR_SUCCESS)
		return false;

	// get a device list for the COMPORT class
	const GUID guid = GUID_DEVINTERFACE_COMPORT;
	HDEVINFO devices = SetupDiGetClassDevs(&guid, 0, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	if (devices == INVALID_HANDLE_VALUE)
		return false;

	// enumerate devices in the list
	bool found = false;
	for (DWORD devIndex = 0 ; ; ++devIndex)
	{
		// get the next item
		SP_DEVINFO_DATA devInfoData{ sizeof(SP_DEVINFO_DATA) };
		if (!SetupDiEnumDeviceInfo(devices, devIndex, &devInfoData))
		{
			// stop if we've exhausted the list
			if (GetLastError() == ERROR_NO_MORE_ITEMS)
				break;

			// on other errors, just skip the item and keep looking
			continue;
		}

		// Get the friendly name.  For USB COM ports, this will be of
		// the form "USB Serial Device (COMn)"
		std::wregex comPortPat(L".*\\((COM\\d+)\\)");
		std::match_results<const WCHAR*> match;
		WCHAR friendlyName[256]{ 0 };
		DWORD iPropertySize = 0;
		if (SetupDiGetDeviceRegistryProperty(devices, &devInfoData,
			SPDRP_FRIENDLYNAME, 0L, reinterpret_cast<PBYTE>(friendlyName), sizeof(friendlyName), &iPropertySize)
			&& std::regex_match(friendlyName, match, comPortPat))
		{
			// It has the right format for a COMn port, so it's a possible match.
			// Identify the parent device; if it matches our own parent, this is
			// our COM port.
			DEVINST comDevInstParent = NULL;
			if (CM_Get_Parent(&comDevInstParent, devInfoData.DevInst, 0) == CR_SUCCESS
				&& comDevInstParent == devInstParent)
			{
				// extract the COMn port name substring from the friendly name string
				std::wstring portName = match[1].str();

				// set the name, flag success, and stop searching
				name = ToTSTRING(portName);
				found = true;
				break;
			}
		}
	}

	// done with the device list
	SetupDiDestroyDeviceInfoList(devices);

	// return the result
	return found;
}


// Button Latency Tester II Vendor Interface GUID
// {4D7C1DBD-82ED-4886-956F-7DF0B316DBF5}.  This GUID is specific to
// this interface.  WinUsb is a generic device driver that can be used
// with any vendor-defined private interface, so it requires each
// application to assign a GUID to identify the particular protocol it
// uses over the generic channel that WinUsb provides.
const GUID VendorInterface::devIfcGUID{
	0x4D7C1DBD, 0x82ED, 0x4886, 0x95, 0x6F, 0x7D, 0xF0, 0xB3, 0x16, 0xDB, 0xF5 };

// Enumerate available WinUSB devices
HRESULT VendorInterface::EnumerateDevices(std::list<VendorInterfaceDesc> &devices)
{
	// empty the device list
	devices.clear();

	// Use the Windows config manager to get a list of currently connected
	// devices matching the Latency Tester Vendor Interface GUID.  We have
	// to do this iteratively, because the list is retrieved in a two-step
	// process:  get the list size, then get the list into memory allocated
	// based on the list size.  It's possible for the list to grow between
	// the sizing step and the data copy step - for example, the user could
	// plug in a new matching device in the interim between the two calls.
	// The copy call will fail with a "buffer too small" error if the list
	// does in fact grow between the steps, so we need to keep retrying
	// until it works.
	ULONG devIfcListLen = 0;
	std::unique_ptr<WCHAR, std::function<void(void*)>> devIfcList(nullptr, [](void *p) { HeapFree(GetProcessHeap(), 0, p); });
	for (;;)
	{
		// get the size of the device list for the current GUID
		auto cr = CM_Get_Device_Interface_List_Size(
			&devIfcListLen, const_cast<LPGUID>(&devIfcGUID), NULL,
			CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
		if (cr != CR_SUCCESS)
			return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_INVALID_DATA));

		// allocate space
		devIfcList.reset(reinterpret_cast<WCHAR*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, devIfcListLen * sizeof(WCHAR))));
		if (devIfcList == nullptr)
			return E_OUTOFMEMORY;

		// get the device list
		cr = CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&devIfcGUID),
			NULL, devIfcList.get(), devIfcListLen, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

		// on success, stop looping
		if (cr == CR_SUCCESS)
			break;

		// if the device list grew, go back for another try
		if (cr == CR_BUFFER_SMALL)
			continue;

		// abort on any other error
		return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_INVALID_DATA));
	}

	// process the list - it's a list of consecutive null-terminated strings,
	// ending with an empty string to mark the end
	for (const WCHAR *p = devIfcList.get(), *endp = p + devIfcListLen ; p < endp && *p != 0 ; )
	{
		// get the length of this string; stop at the final empty string
		size_t len = wcslen(p);
		if (len == 0)
			break;

		// Retrieve the device instance ID
		WCHAR instId[MAX_DEVICE_ID_LEN]{ 0 };
		ULONG propSize = MAX_DEVICE_ID_LEN;
		DEVPROPTYPE propType = 0;
		if (CM_Get_Device_Interface_PropertyW(p, &DEVPKEY_Device_InstanceId,
			&propType, reinterpret_cast<BYTE*>(instId), &propSize, 0) == CR_SUCCESS)
		{
			// add the new path to the list
			devices.emplace_back(VendorInterfaceDesc::private_ctor_key_t(), p, len, instId);
		}

		// skip to the next string
		p += len + 1;
	}

	// success
	return S_OK;
}

// Vendor Interface error code strings.  These correspond to the 
// uint16_T codes reported in the VendorResponse::status field of 
// the Vendor Interface USB protocol reply packet.
const char *VendorInterface::ErrorText(int status)
{
	static std::unordered_map<int, const char*> errorText{
		{ VendorResponse::OK, "Success" },
		{ VendorResponse::ERR_FAILED, "Failed" },
		{ VendorResponse::ERR_TIMEOUT, "Operation timed out" },
		{ VendorResponse::ERR_BAD_XFER_LEN, "Bad transfer length" },
		{ VendorResponse::ERR_USB_XFER_FAILED, "USB transfer failed" },
		{ VendorResponse::ERR_BAD_PARAMS, "Invalid parameters" },
		{ VendorResponse::ERR_BAD_CMD, "Invalid command code" },
		{ VendorResponse::ERR_BAD_SUBCMD, "Invalid subcommand code" },
		{ VendorResponse::ERR_REPLY_MISMATCH, "Reply/request mismatch" },
		{ VendorResponse::ERR_CONFIG_TIMEOUT, "Configuration file transfer timed out" },
		{ VendorResponse::ERR_CONFIG_INVALID, "Configuration file storage is corrupted" },
		{ VendorResponse::ERR_OUT_OF_BOUNDS, "Value out of bounds" },
		{ VendorResponse::ERR_NOT_READY, "Not ready" },
		{ VendorResponse::ERR_EOF, "End of file" },
		{ VendorResponse::ERR_BAD_REQUEST_DATA, "Data or format error in request" },
		{ VendorResponse::ERR_BAD_REPLY_DATA, "Data or format error in reply" },
		{ VendorResponse::ERR_NOT_FOUND, "File/object not found" },
	};

	// look up an existing message
	if (auto it = errorText.find(status); it != errorText.end())
		return it->second;

	// Not found - add a generic description based on the error number.
	// Store it in a static list of strings, and enlist it in the map,
	// so that we can reuse the/ same string if the same code comes up 
	// again.
	char buf[128];
	sprintf_s(buf, "Unknown error code %u", status);
	static std::list<std::string> unknownCodes;
	auto &str = unknownCodes.emplace_back(buf);
	errorText.emplace(status, str.c_str());
	return str.c_str();
}

int VendorInterface::QueryVersion(Version &vsn)
{
	// send the request, capturing the result arguments
	VendorResponse reply;
	int stat = SendRequest(VendorRequest::CMD_QUERY_VERSION, reply);

	// on success, and if the result arguments are the right size, copy the
	// results back to the caller's Version struct
	if (stat == VendorResponse::OK && reply.argsSize >= sizeof(reply.args.version))
	{
		vsn.major = reply.args.version.major;
		vsn.minor = reply.args.version.minor;
		vsn.patch = reply.args.version.patch;
		memcpy(vsn.buildDate, reply.args.version.buildDate, 12);
		vsn.buildDate[12] = 0;
	}

	// return the result
	return stat;
}

int VendorInterface::QueryID(DeviceID &id)
{
	ULONG frameNum = 0;
	LARGE_INTEGER timestamp{ 0, 0 };
	if (WinUsb_GetCurrentFrameNumber(winusbHandle, &frameNum, &timestamp))
		timestamp.QuadPart = 0;


	// send the request, capturing the result arguments
	VendorResponse reply;
	std::vector<BYTE> xferIn;
	int stat = SendRequest(VendorRequest::CMD_QUERY_IDS, reply, nullptr, 0, &xferIn);

	// on success, and if the result arguments are the right size, copy the
	// results back to the caller's out variable
	if (stat == VendorResponse::OK && reply.argsSize >= offsetnext(VendorResponse::Args::ID, romVersion))
	{
		// set the hardware ID
		static_assert(sizeof(id.hwid.b) == sizeof(reply.args.id.hwid));
		memcpy(id.hwid.b, reply.args.id.hwid, sizeof(id.hwid.b));

		// set the hardware versions
		id.cpuType = reply.args.id.cpuType;
		id.cpuVersion = reply.args.id.cpuVersion;
		id.romVersion = reply.args.id.romVersion;

		// set the ROM version name, per the nomenclature used in the SDK
		if (id.romVersion >= 1)
		{
			char buf[32];
			sprintf_s(buf, "RP%d-B%d", reply.args.id.cpuType, id.romVersion - 1);
			id.romVersionName = buf;
		}
		else
			id.romVersionName = "Unknown";

		// read the name strings from the extra transfer data
		{
			// set up to read the strings
			const BYTE *p = xferIn.data();
			const BYTE *end = p + xferIn.size();

			// read the board name, up to the null byte
			const BYTE *start = p;
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0; ++p) ;
			id.targetBoardName.assign(reinterpret_cast<const char*>(start), p - start);

			// read the SDK version string, up to the null byte
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0 ; ++p) ;
			id.picoSDKVersion.assign(reinterpret_cast<const char*>(start), p - start);

			// read the TinyUSB version string, up to the null byte
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0 ; ++p) ;
			id.tinyusbVersion.assign(reinterpret_cast<const char*>(start), p - start);

			// read the GNUC version string, up to the null byte
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0 ; ++p) ;
			id.compilerVersion.assign(reinterpret_cast<const char*>(start), p - start);
		}
	}

	// return the result
	return stat;
}

int VendorInterface::QueryLog(std::vector<uint8_t> &text, size_t *totalAvailable)
{
	// send the request
	VendorResponse resp;
	int stat = SendRequest(VendorRequest::CMD_QUERY_LOG, resp, nullptr, 0, &text);

	// If the caller wants the total available size, fill it in: if we got
	// a valid reply, use the value from the reply arguments, otherwise
	// just zero it
	if (totalAvailable != nullptr)
		*totalAvailable = (stat == VendorResponse::OK) ? resp.args.log.avail : 0;

	// return the status
	return stat;
}

int VendorInterface::QueryStats(Statistics *stats, size_t sizeofStats, bool resetCounters)
{
	// Zero the caller's struct, so that any fields that are beyond the
	// length of the returned struct will be zeroed on return.  This
	// helps with cross-version compatibility when the caller is using
	// a newer version of the struct than the firmware, by setting any
	// new fields to zeroes.  New fields should always be defined in such
	// a way that a zero has the same meaning that would have obtained in
	// the old firmware version before the new fields were added.
	memset(stats, 0, sizeofStats);

	// retrieve the statistics from the device
	VendorResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t args[2]{
		VendorRequest::SUBCMD_STATS_QUERY_STATS,
		static_cast<uint8_t>(resetCounters ? VendorRequest::QUERYSTATS_FLAG_RESET_COUNTERS : 0)
	};
	int result = SendRequestWithArgs(VendorRequest::CMD_STATS, args, resp, nullptr, 0, &xferIn);
	if (result != VendorResponse::OK)
		return result;

	// the statistics struct is returned in the transfer-in data
	auto *devStats = reinterpret_cast<Statistics*>(xferIn.data());

	// Copy the smaller of the returned struct or the caller's struct; if
	// the caller's struct is bigger (newer), this will leave new fields
	// with their default zero values; if the caller's struct is smaller
	// (older), this will simply drop the new fields the caller doesn't
	// know about.
	memcpy(stats, devStats, min(sizeofStats, xferIn.size()));

	// success
	return VendorResponse::OK;
}

int VendorInterface::ResetPico()
{
	uint8_t args = VendorRequest::SUBCMD_RESET_NORMAL;
	return SendRequestWithArgs(VendorRequest::CMD_RESET, args);
}

int VendorInterface::EnterBootLoader()
{
	uint8_t args = VendorRequest::SUBCMD_RESET_BOOTLOADER;
	return SendRequestWithArgs(VendorRequest::CMD_RESET, args);
}

int VendorInterface::HostInputEvent(int gpio, uint64_t tEvent, HostInputEventResult &result)
{
	// set up the request arguments
	VendorRequest::Args::HostInputEvent h;
	h.gp = gpio;

	// get the current USB hardware frame counter and SOF time
	USB_FRAME_NUMBER_AND_QPC_FOR_TIME_SYNC_INFORMATION qpcInfo{ timeTrackingHandle };
	if (timeTrackingHandle != NULL && WinUsb_GetCurrentFrameNumberAndQpc(winusbHandle, &qpcInfo))
	{
		// Figure the time at the SOF, at microframe 0.  The current time in the 
		// qpcInfo struct is the time at the start of the current hardware *microframe*,
		// not the overall frame.  Each microframe is 125us, so deduct 125us times the
		// microframe number to get the SOF time.
		uint64_t tMicroframe = hrt.TicksToUs64(qpcInfo.CurrentQueryPerformanceCounter.QuadPart);
		uint64_t tSOF = tMicroframe - qpcInfo.CurrentHardwareMicroFrameNumber*125;

		// figure the elapsed time between SOF and the event
		int64_t dt = static_cast<int64_t>(tSOF - tEvent);

		// Get the frame index
		uint16_t frameIndex = static_cast<uint16_t>(qpcInfo.CurrentHardwareFrameNumber);

		// fill in command arguments
		h.dtEventToSof = static_cast<int32_t>(dt);
		h.sofTimestamp = tSOF;
		h.usbFrameCounter = frameIndex;

		// tell the caller that SOF information was available
		result.sofTimeAvailable = true;
	}
	else
	{
		// SOF time is unavailable - use the elapsed time from event to now,
		// and fill in the frame counter with the special invalid marker 0xFFFF
		h.dtEventToSof = static_cast<int32_t>(tEvent - static_cast<uint64_t>(hrt.GetTime_us()));
		h.sofTimestamp = 0;
		h.usbFrameCounter = 0xFFFF;

		// tell the caller that SOF information was NOT available
		result.sofTimeAvailable = false;
	}

	// send the request
	VendorResponse resp;
	int stat = SendRequestWithArgs(VendorRequest::CMD_HOST_INPUT_EVENT, h, resp);

	// pass back the measured latency, if available
	if (stat == VendorResponse::OK)
	{
		if (resp.argsSize >= offsetnext(VendorResponse::Args::HostInputResult, status))
		{
			auto &hir = resp.args.hostInputResult;
			result.latency = hir.latency;

			static const std::unordered_map<int, HostInputEventResult::Status> statusMap{
				{ VendorResponse::Args::HostInputResult::STAT_MATCHED, HostInputEventResult::Status::Matched },
				{ VendorResponse::Args::HostInputResult::STAT_NO_MATCH, HostInputEventResult::Status::NotMatched },
				{ VendorResponse::Args::HostInputResult::STAT_DUPLICATE, HostInputEventResult::Status::Duplicate },
			};
			auto it = statusMap.find(hir.status);
			result.status = it != statusMap.end() ? it->second : HostInputEventResult::Status::Unknown;
		}
		else
			stat = VendorResponse::ERR_BAD_REPLY_DATA;
	}

	// return the status
	return stat;
}

int VendorInterface::QueryMeasurements(std::vector<MeasurementData> &data)
{
	// clear any prior data in the caller's vector
	data.clear();

	// query the data
	uint8_t args{ VendorRequest::SUBCMD_MEASUREMENTS_GET };
	std::vector<uint8_t> xferIn;
	if (int stat = SendRequestWithArgs(VendorRequest::CMD_MEASUREMENTS, args, nullptr, 0, &xferIn); 
		stat != VendorResponse::OK)
		return stat;

	// make sure the result has at least the size field
	using ListHeader = ButtonLatencyTester2::MeasurementsList;
	using ListEle = ButtonLatencyTester2::MeasurementData;
	if (xferIn.size() < offsetnext(ListHeader, nData))
		return VendorResponse::ERR_BAD_REPLY_DATA;

	// parse the list header
	const auto *hdr = reinterpret_cast<ListHeader*>(xferIn.data());
	if (hdr->cb < offsetnext(ListHeader, nData) 
		|| hdr->cbData < offsetnext(ListEle, latencyMedian)
		|| xferIn.size() < static_cast<size_t>(hdr->cb + hdr->nData*hdr->cbData))
		return VendorResponse::ERR_BAD_REPLY_DATA;

	// make room in the output list, and clear to all zeroes
	data.resize(hdr->nData);
	memset(data.data(), 0, data.size() * sizeof(MeasurementData));

	// parse the list element
	const auto *ele = reinterpret_cast<ListEle*>(xferIn.data() + hdr->cb);
	for (unsigned int i = 0 ; i < hdr->nData ; ++i, ++ele)
	{
		auto &d = data[i];
		d.gp = ele->gp;
		d.nPresses = ele->nPresses;
		d.nHostEvents = ele->nHostEvents;
		d.latencySum = ele->latencySum;
		d.latencySquaredSum = ele->latencySquaredSum;
		d.latencyMin = ele->latencyMin;
		d.latencyMax = ele->latencyMax;
		d.latencyMedian = ele->latencyMedian;
	}

	// success
	return VendorResponse::OK;
}

std::string PicoHardwareId::ToString() const
{
	// Format the string as a series of hex digits, two digits per
	// byte, in order of the bytes in the ID array.  This happens
	// to be equivalent to interpreting the ID as a 64-bit int in
	// big-endian byte order, but it's better to think of the ID
	// as just an array of 8 bytes, since that avoids any confusion
	// about endianness.
	char buf[17];
	sprintf_s(buf, "%02X%02X%02X%02X%02X%02X%02X%02X",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

	// return the string
	return buf;
}

int VendorInterface::SendRequest(uint8_t cmd, const BYTE *xferOutData, size_t xferOutLength, std::vector<BYTE> *xferInData)
{
	// build the request struct
	VendorRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));

	// send the request
	return SendRequest(req, xferOutData, xferInData);
}

int VendorInterface::SendRequest(uint8_t cmd, VendorResponse &resp,
	const BYTE *xferOutData, size_t xferOutLength, std::vector<BYTE> *xferInData)
{
	// build the request struct
	VendorRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));

	// send the request
	return SendRequest(req, resp, xferOutData, xferInData);
}

int VendorInterface::SendRequest(const VendorRequest &request,
	const BYTE *xferOutData, std::vector<BYTE> *xferInData)
{
	// send the request, capturing into a temporary response struct (which
	// we'll discard once we're done, since the caller doesn't need it)
	VendorResponse resp;
	return SendRequest(request, resp, xferOutData, xferInData);
}

int VendorInterface::SendRequest(
	const VendorRequest &request, VendorResponse &resp,
	const BYTE *xferOutData, std::vector<BYTE> *xferInData)
{
	// translate HRESULT from pipe operation to status code
	static auto PipeHRESULTToReturnCode = [](HRESULT hr)
	{
		// if HRESULT indicated success, it must be a bad transfer size
		if (SUCCEEDED(hr))
			return VendorResponse::ERR_USB_XFER_FAILED;

		// E_ABORT is a timeout error; for others, consider it a general USB transfer error
		return hr == E_ABORT ? VendorResponse::ERR_TIMEOUT : VendorResponse::ERR_USB_XFER_FAILED;
	};

	// it's an error if the request has a non-zero additional transfer-out
	// data size, and the caller didn't provide the data to send
	if (request.xferBytes != 0 && xferOutData == nullptr)
		return VendorResponse::ERR_BAD_XFER_LEN;

	// send the request data
	size_t sz = 0;
	HRESULT hr = Write(reinterpret_cast<const BYTE*>(&request), sizeof(request), sz, REQUEST_TIMEOUT);
	if (!SUCCEEDED(hr) || sz != sizeof(request))
		return PipeHRESULTToReturnCode(hr);

	// send the additional OUT data, if any
	if (xferOutData != nullptr && request.xferBytes != 0)
	{
		// validate the transfer length
		if (request.xferBytes > UINT16_MAX)
			return VendorResponse::ERR_BAD_XFER_LEN;

		// send the data
		hr = Write(xferOutData, request.xferBytes, sz, REQUEST_TIMEOUT);
		if (!SUCCEEDED(hr) || sz != request.xferBytes)
			return PipeHRESULTToReturnCode(hr);
	}

	// Read the device reply, until we run out of input or get a matching token.
	// This will clear out any pending replies from past requests that were
	// aborted or canceled on the client side but still pending in the device.
	int readCount = 0;
	for (;; ++readCount)
	{
		// read a reply
		hr = Read(reinterpret_cast<BYTE*>(&resp), sizeof(resp), sz, REQUEST_TIMEOUT);
		if (!SUCCEEDED(hr))
		{
			// on timeout, if we read any replies, the real problem is that
			// we rejected all of the available replies due to a token
			// mismatch
			auto ret = PipeHRESULTToReturnCode(hr);
			return (ret == VendorResponse::ERR_TIMEOUT && readCount > 0) ?
				VendorResponse::ERR_REPLY_MISMATCH : ret;
		}

		// if it's not the right size for a response, skip it; this could be
		// a leftover packet from an earlier request's transfer data that
		// wasn't fully read
		if (sz != 0 && sz != sizeof(resp))
			continue;

		// if the token matches, we've found our reply
		if (resp.token == request.token)
			break;

		// if there's extra transfer data, skip it
		if (resp.xferBytes != 0)
		{
			std::unique_ptr<BYTE> buf(new BYTE[resp.xferBytes]);
			if (!SUCCEEDED(hr = Read(buf.get(), resp.xferBytes, sz, REQUEST_TIMEOUT)))
				return PipeHRESULTToReturnCode(hr);
		}
	}

	// make sure the reply command matches the request command
	if (resp.cmd != request.cmd)
		return VendorResponse::ERR_REPLY_MISMATCH;

	// read any additional response data
	if (resp.xferBytes != 0)
	{
		// make room for the transfer-in data
		uint8_t *xferInPtr = nullptr;
		std::unique_ptr<BYTE> dummyBuf;
		if (xferInData != nullptr)
		{
			// the caller provided a buffer - size it to hold the transfer
			xferInData->resize(resp.xferBytes);
			xferInPtr = xferInData->data();
		}
		else
		{
			// the caller didn't provide a buffer - create a dummy buffer
			dummyBuf.reset(new BYTE[resp.xferBytes]);
			xferInPtr = dummyBuf.get();
		}

		// read the data (which might arrive in multiple chunks)
		for (auto xferRemaining = resp.xferBytes ; xferRemaining != 0 ; )
		{
			// resize the output buffer vector and read the data
			hr = Read(xferInPtr, xferRemaining, sz, REQUEST_TIMEOUT);
			if (!SUCCEEDED(hr))
				return PipeHRESULTToReturnCode(hr);

			// deduct this read from the remaining total and bump the read pointer
			xferRemaining -= static_cast<uint16_t>(sz);
			xferInPtr += sz;
		}

		// if we had to create a dummy buffer, it's a parameter error
		if (xferInData == nullptr)
			return VendorResponse::ERR_BAD_PARAMS;
	}

	// the USB exchange was concluded successfully, so return the status
	// code from the device response
	return resp.status;
}

HRESULT VendorInterface::Read(BYTE *buf, size_t bufSize, size_t &bytesRead, DWORD timeout_ms)
{
	// clean up old I/Os periodically
	CleanUpTimedOutIOs(false);

	// size_t can overflow ULONG on 64-bit platforms, so check first
	if (bufSize > ULONG_MAX)
		return E_INVALIDARG;

	// create an IO tracker for the transaction
	std::unique_ptr<IOTracker> io(new IOTracker(this, bufSize));

	// start the asynchronous read
	DWORD err;
	HRESULT ret;
	if (WinUsb_ReadPipe(winusbHandle, epIn, io->buf.data(), static_cast<ULONG>(bufSize), NULL, &io->ov.ov)
		|| (err = GetLastError()) == ERROR_IO_PENDING)
	{
		// I/O successfully started - wait for completion
		ret = io->ov.Wait(timeout_ms, bytesRead);

		// on success, copy the result back to the caller's buffer
		if (SUCCEEDED(ret))
			memcpy(buf, io->buf.data(), bufSize);

		// If the I/O didn't complete, save the IOTracker to the timed-out list.
		// We can't destroy the IOTracker until the transaction completes because
		// WinUsb can write into its buffers up at any time until then.
		if (!io->IsCompleted())
			timedOutIOs.emplace_back(io.release());

	}
	else
	{
		// failed - return the error code from the Read attempt
		ret = HRESULT_FROM_WIN32(err);
	}

	// return the result
	return ret;
}

HRESULT VendorInterface::Write(const BYTE *buf, size_t len, size_t &bytesWritten, DWORD timeout_ms)
{
	// clean up old I/Os periodically
	CleanUpTimedOutIOs(false);

	// size_t can overflow ULONG on 64-bit platforms, so check first
	if (len > ULONG_MAX)
		return E_INVALIDARG;

	// create an IO tracker for the transaction
	std::unique_ptr<IOTracker> io(new IOTracker(this, buf, len));

	// write the data
	DWORD err;
	HRESULT ret;
	if (WinUsb_WritePipe(winusbHandle, epOut, io->buf.data(), static_cast<ULONG>(len), NULL, &io->ov.ov)
		|| (err = GetLastError()) == ERROR_IO_PENDING)
	{
		// I/O successfully started - wait for completion
		ret = io->ov.Wait(timeout_ms, bytesWritten);

		// If the I/O didn't complete, save the IOTracker to the timed-out list.
		// We can't destroy the IOTracker until the transaction completes because
		// WinUsb can write into its buffers up at any time until then.
		if (!io->IsCompleted())
			timedOutIOs.emplace_back(io.release());
	}
	else
	{
		// I/O failed - return the error code from the Write attempt
		ret = HRESULT_FROM_WIN32(err);
	}

	// return the result
	return ret;
}

void VendorInterface::CleanUpTimedOutIOs(bool now)
{
	if (now || GetTickCount64() >= tCleanUpTimedOutIOs)
	{
		// scan the list
		for (decltype(timedOutIOs.begin()) cur = timedOutIOs.begin(), nxt = cur ; cur != timedOutIOs.end() ; cur = nxt)
		{
			// move to the next before we potentially unlink the current one
			++nxt;

			// if the I/O has completed, we can discard the tracker
			if ((*cur)->IsCompleted())
				timedOutIOs.erase(cur);
		}

		// set the next cleanup time
		tCleanUpTimedOutIOs = GetTickCount64() + 2500;
	}
}

void VendorInterface::ResetPipes()
{
	WinUsb_ResetPipe(winusbHandle, epIn);
	WinUsb_ResetPipe(winusbHandle, epOut);
}

void VendorInterface::FlushRead()
{
	WinUsb_FlushPipe(winusbHandle, epIn);
}

void VendorInterface::FlushWrite()
{
	WinUsb_FlushPipe(winusbHandle, epOut);
}

void VendorInterface::CloseDeviceHandle()
{
	// close the time-tracking handle
	if (timeTrackingHandle != NULL)
	{
		USB_STOP_TRACKING_FOR_TIME_SYNC_INFORMATION sti{ timeTrackingHandle };
		WinUsb_StopTrackingForTimeSync(winusbHandle, &sti);
		timeTrackingHandle = NULL;
	}

	// close the WinUSB handle
	if (winusbHandle != NULL)
	{
		WinUsb_Free(winusbHandle);
		winusbHandle = NULL;
	}

	// close the file handle
	if (hDevice != NULL && hDevice != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hDevice);
		hDevice = NULL;
	}
}

