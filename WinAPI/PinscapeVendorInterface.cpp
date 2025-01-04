// Pinscape Pico Device - Vendor Interface API
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
#include "crc32.h"
#include "BytePackingUtils.h"
#include "Utilities.h"
#include "PinscapePicoAPI.h"
#include "PinscapeVendorInterface.h"
#include "FeedbackControllerInterface.h"
#include "RP2BootLoaderInterface.h"

#pragma comment(lib, "cfgmgr32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "winusb")
#pragma comment(lib, "setupapi")
#pragma comment(lib, "shell32")
#pragma comment(lib, "hid")

// "Offset Next" - offset of next field in struct type s after m.  This
// is the size of the struct up to and including m, to check if m is
// included in a live copy of the struct with known dynamic size.
#define offsetnext(s, m) (offsetof(s, m) + sizeof(s::m))

// this is all in the PinscapePico namespace
using namespace PinscapePico;

// Device ID name formatting
std::string DeviceID::FriendlyBoardName() const
{
	// make a modifiable copy of the name
	std::string name = targetBoardName;

	// replace underscores with spaces, and capitalize each word
	bool startOfWord = true;
	for (char *p = name.data() ; *p != 0 ; ++p)
	{
		// capitalize the start of each word
		if (startOfWord && islower(*p))
		{
			*p = toupper(*p);
			startOfWord = false;
		}

		// replace underscores with spaces
		if (*p == '_')
			*p = ' ';

		// start a word at a space
		if (*p == ' ')
			startOfWord = true;
	}

	// return the modified string
	return name;
}

// Destruction
VendorInterface::~VendorInterface()
{
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

// Vendor Interface error code strings.  These correspond to the 
// uint16_T codes reported in the VendorResponse::status field of 
// the Vendor Interface USB protocol reply packet.
const char *VendorInterface::ErrorText(int status)
{
	static std::unordered_map<int, const char*> errorText{
		{ PinscapeResponse::OK, "Success" },
		{ PinscapeResponse::ERR_FAILED, "Failed" },
		{ PinscapeResponse::ERR_TIMEOUT, "Operation timed out" },
		{ PinscapeResponse::ERR_BAD_XFER_LEN, "Bad transfer length" },
		{ PinscapeResponse::ERR_USB_XFER_FAILED, "USB transfer failed" },
		{ PinscapeResponse::ERR_BAD_PARAMS, "Invalid parameters" },
		{ PinscapeResponse::ERR_BAD_CMD, "Invalid command code" },
		{ PinscapeResponse::ERR_BAD_SUBCMD, "Invalid subcommand code" },
		{ PinscapeResponse::ERR_REPLY_MISMATCH, "Reply/request mismatch" },
		{ PinscapeResponse::ERR_CONFIG_TIMEOUT, "Configuration file transfer timed out" },
		{ PinscapeResponse::ERR_CONFIG_INVALID, "Configuration file storage is corrupted" },
		{ PinscapeResponse::ERR_OUT_OF_BOUNDS, "Value out of bounds" },
		{ PinscapeResponse::ERR_NOT_READY, "Not ready" },
		{ PinscapeResponse::ERR_EOF, "End of file" },
		{ PinscapeResponse::ERR_BAD_REQUEST_DATA, "Data or format error in request" },
		{ PinscapeResponse::ERR_BAD_REPLY_DATA, "Data or format error in reply" },
		{ PinscapeResponse::ERR_NOT_FOUND, "File/object not found" },
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

// Convert a CONFIGRET result code, used by the ConfigMgr APIs, to
// an HRESULT.  We use HRESULT codes for most of our public interface,
// to try to keep things simple (or at least consistent) for callers.
// The ConfigMgr APIs use their own peculiar error codes, nominally
// typedef'd as CONFIGRET (although they're really just 'int's), so
// we can't return ConfigMgr codes directly from our own functions;
// we have to map them to corresponding HRESULT codes instead.  If
// we wanted to preserve detailed diagnostic information, we'd have
// to come up with a custom mapping table of CR_xxx codes and their
// corresponding HRESULT values.  At the moment, I don't think that's
// worth the effort, since the point of presenting a specific error
// code to the user is to guide them to a suitable corrective action
// they can take, and I don't think any of the low-level errors that
// can happen at the ConfigMgr API level will end up being things
// that most users will be able to fix manually, even with the
// ConfigMgr error code.  Those errors are mostly internal technical
// things in the Windows kernel device driver model.  So for now,
// I'm just mapping every ConfigMgr error to a generic E_FAIL
// HRESULT.  That loses all of the detail of the CR_xxx code, but
// that's no loss if the user wouldn't know what to do with a CR_xxx
// code anyway.  However, I'm at least defining this function to do
// the mapping explicitly - that way we can easily add a more
// information-preserving mapping in the future if that turns out
// to be desirable.
static HRESULT CONFIGRET_TO_HRESULT(CONFIGRET cres)
{ 
	// Convert everything to the generic "failed" error.
	// Note: if there are any particular CR_xxx codes that would
	// convey useful information to the user, we can add suitable
	// HRESULT or HRSULT_FROM_WIN32() codes here for the specific
	// CR_xxx codes that we want to preserve.  DON'T just pass
	// back the CR_xxx code directly, since the caller will
	// interpret whatever we pass back as an HRESULT.
	return E_FAIL; 
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
		path, deviceInstanceId, &serialBuf[1], epIn, epOut);

	// success
	return S_OK;
}

// Get the VID/PID for a given device path
HRESULT VendorInterfaceDesc::GetVIDPID(uint16_t &vid, uint16_t &pid)
{
	// Retrieve the device instance ID for the device
	TCHAR instId[MAX_DEVICE_ID_LEN]{ 0 };
	ULONG propSize = MAX_DEVICE_ID_LEN;
	DEVPROPTYPE propType = 0;
	CONFIGRET cres = CR_SUCCESS;
	if ((cres = CM_Get_Device_Interface_PropertyW(path.c_str(), &DEVPKEY_Device_InstanceId,
		&propType, reinterpret_cast<BYTE*>(instId), &propSize, 0)) != CR_SUCCESS)
		return CONFIGRET_TO_HRESULT(cres);

	// get my device node
	DEVINST di = NULL;
	if ((cres = CM_Locate_DevNode(&di, instId, CM_LOCATE_DEVNODE_NORMAL)) != CR_SUCCESS)
		return CONFIGRET_TO_HRESULT(cres);

	// retrieve the hardware IDs
	TCHAR hwIds[4096]{ 0 };
	propSize = static_cast<ULONG>(sizeof(hwIds));
	if ((cres = CM_Get_DevNode_PropertyW(di, &DEVPKEY_Device_HardwareIds,
		&propType, reinterpret_cast<BYTE*>(hwIds), &propSize, 0)) != CR_SUCCESS)
		return CONFIGRET_TO_HRESULT(cres);

	// Scan the list of hardware IDs and look for the standard VID/PID
	// encoding format.  This *seems* like a hack, and it certainly
	// qualifies as a hack in the sense that it's inelegant, but it's
	// actually not a hack in the sense of being fragile with respect
	// to potential future changes at the Windows system level.  This
	// parsing is well-defined and, believe it or not, "official". 
	// The Windows API documentation explicitly specifies the format
	// of the hardware ID string: see "Standard USB Identifiers" under
	// the "Windows Drivers" section.  That section defines the
	// hardware ID format as:
	//
	//   USB\VID_vvvv&PID_pppp&REV_rrrr
	//
	// The 'vvvv' and 'pppp' strings are hex numbers representing the
	// VID and PID codes (respectively) transmitted from device to host
	// during USB connection setup.  Those are the numbers that this
	// routine exists to find.
	// 
	// Note that the device path that we started with, as well as the
	// "device instance ID" that we obtained above from the path, ALSO
	// use formats that obviously embed the VID and PID.  It's tempting
	// to skip all the extra busy-work above and just parse our numbers
	// straight out of the device path.  And as far as I've observed,
	// that would always work in practice, in all Windows versions
	// through at least 11.  But the formats for the device path and
	// instance ID strings are NOT officially documented, so they could
	// conceivably change in future Windows versions.  That's why I
	// bothered with the extra steps, to get a string that does have
	// an officially documented format.
	static const std::basic_regex<TCHAR> pat(
		_T("USB\\\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})&.*"), 
		std::regex_constants::icase);

	// Parse each path in the list, and take the first one that
	// matches the expected pattern.  The API nominally returns a
	// list here, but the limited set of devices that this code
	// needs to work with - Pinscape Pico, Pi Pico in ROM Boot
	// Loader mode - will always only present one entry here.
	for (TCHAR *p = hwIds, *endp = hwIds + propSize/sizeof(TCHAR) ; 
		p < endp && *p != 0 ; p += _tcslen(p))
	{
		std::match_results<const TCHAR*> m;
		if (std::regex_match(p, m, pat))
		{
			// Got a match - parse out the VID and PID and return success.
			// (Note that the casts to uint16_t are guaranteed not to overflow,
			// since we know from the regex pattern match that we have four
			// hex digits in each number, and four hex digits has exactly the
			// full range of a uint16_t.)
			vid = static_cast<uint16_t>(_tcstol(m[1].str().c_str(), nullptr, 16));
			pid = static_cast<uint16_t>(_tcstol(m[2].str().c_str(), nullptr, 16));
			return S_OK;
		}
	}

	// no matches found
	return E_FAIL;
}

// Get the CDC (virtual COM) port associated with this interface
bool VendorInterfaceDesc::GetCDCPort(TSTRING &name) const
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
				name = ToTCHAR(portName.c_str());
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



// Pinscape Pico GUID {D3057FB3-8F4C-4AF9-9440-B220C3B2BA23}.  This GUID is
// project-specific, randomly generated and assigned as the unique ID for
// the Pinscape Pico WinUSB vendor interface.  It's not any sort of
// pre-defined Microsoft GUID; its only function is to serve as a unique
// identifier that Windows applications can use to distinguish a Pinscape
// Pico device from other WinUSB devices plugged into the same computer,
// so that the app can establish a connection to the correct physical
// device.
const GUID VendorInterface::devIfcGUID {
	0xD3057FB3, 0x8F4C, 0x4AF9, 0x94, 0x40, 0xB2, 0x20, 0xC3, 0xB2, 0xBA, 0x23 };

// Generic WinUSB device interface.  This is the class GUID that's generally
// assigned to WinUSB devices that are installed manually rather than through
// device-side MS OS descriptors.  Zadig and libwdi use this for a manual
// WinUSB install, so this is the GUID that will generally be assigned to the
// Pico ROM Boot Loader on any system where the user has intentionally used
// Zadig to enable WinUSB for the Pico RP2 Boot device.  Note that the RP2
// Boot device *isn't* assigned this interface automatically!  Out of the box,
// RP2 Boot just exposes a naked vendor interface that NOTHING on the Windows
// side will be able to connect to.  The vendor interface can be used with
// WinUSB only if the user manually associates WinUSB with the device, using
// Zadig or some similar Windows-driver-installer tool.  It's also possible
// for someone to associate a different, custom GUID with the RP2 vendor
// interface, by manually entering a custom GUID when setting up the device
// association via Zadig or whatever else, but you'd have to go far out of
// your way to make that happen, and have some really unusual use case to
// even want to, so I think we can rule it out for all practical purposes.
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB,
	0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

// Generic USB device GUID.  This can be used to enumerate all devices that
// are *potentially* accessible via WinUSB, whether or not they've been set
// up with WinUSB driver associations.  This can be used to enumerate Picos
// that are connected while in RP2 Boot device mode even if they haven't
// been set up for WinUSB access yet.  The Pico's RP2 Boot mode exposes a
// generic vendor interface that's capable of connecting through WinUSB,
// but actually doing so requires WinUSB setup via Zadig, libwdi, or some
// other Windows device driver installer.  The Pico unfortunately does not
// expose the USB MS OS descriptors that would allow that driver setup to
// happen automatically, so manual setup is required if it's desired; that's
// required, for example, if you want to use the Raspberry-supplied PicoTool
// program.
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE,
	0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

// Enumerate available WinUSB devices
HRESULT VendorInterface::EnumerateDevices(std::list<VendorInterfaceDesc> &devices)
{
	// empty the device list
	devices.clear();

	// Use the Windows config manager to get a list of currently connected
	// devices matching the Pinscape Pico Vendor Interface GUID.  We have
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

HRESULT VendorInterface::EnumerateDevicesByID(std::list<VendorInterfaceDesc> &matchList, const char *id)
{
	// clear the caller's list
	matchList.clear();

	// Enumerate Pinscape Pico devices
	HRESULT hr;
	std::list<VendorInterfaceDesc> devices;
	hr = VendorInterface::EnumerateDevices(devices);
	if (!SUCCEEDED(hr))
		return hr;

	// Find the target device
	if (devices.size() == 0)
	{
		// no devices found - there's nothing to match; return successfully with
		// an empty list
		return S_OK;
	}
	else if (id != nullptr && strlen(id) != 0)
	{
		// device specified - scan for a matching ID
		std::list<VendorInterfaceDesc> exactMatches;
		std::list<VendorInterfaceDesc> partialMatches;
		for (auto &p : devices)
		{
			std::unique_ptr<VendorInterface> dev;
			PinscapePico::DeviceID devId;
			if (SUCCEEDED(p.Open(dev))
				&& SUCCEEDED(dev->QueryID(devId)))
			{
				// check for an exact match
				auto hwid = devId.hwid.ToString();
				if (_stricmp(hwid.c_str(), id) == 0
					|| (std::regex_match(id, std::regex("\\d+")) && devId.unitNum == atoi(id))
					|| _stricmp(devId.unitName.c_str(), id) == 0)
				{
					// matched - include it in the match list
					exactMatches.emplace_back(p);
				}

				// If the ID string is at least four characters long,
				// check for a partial match against the hardware ID,
				// matching any substring.  This allows matching the
				// hardware ID a few leading or trailing digits rather
				// than having to type in the whole thing.  A few
				// digits is usually enough to pick out a single unit.
				if (strlen(id) >= 4 && StrStrIA(hwid.c_str(), id) != nullptr)
					partialMatches.emplace_back(p);
			}
		}

		// if there are any exact matches, return those in preference
		// to the fragmentary hardware ID matches
		matchList = exactMatches.size() != 0 ? exactMatches : partialMatches;
		return S_OK;
	}
	else
	{
		// no filter criteria specified - return the entire list
		matchList = devices;
		return S_OK;
	}
}

HRESULT VendorInterface::Open(std::unique_ptr<VendorInterface> &device, IDMatchFunc match)
{
	// open the device
	VendorInterface *pdev;
	HRESULT hr = Open(pdev, match);

	// on success, store the pointer in the caller's unique ptr
	if (hr == S_OK)
		device.reset(pdev);

	// return the result
	return hr;
}

HRESULT VendorInterface::Open(std::shared_ptr<VendorInterface> &device, IDMatchFunc match)
{
	// open the device
	VendorInterface *pdev;
	HRESULT hr = Open(pdev, match);

	// on success, store the pointer in the caller's unique ptr
	if (hr == S_OK)
		device.reset(pdev);

	// return the result
	return hr;
}

HRESULT VendorInterface::Open(VendorInterface* &device, IDMatchFunc match)
{
	// Enumerate the available vendor interfaces by device path.  This
	// operates at the Windows USB configuration level, so it doesn't
	// actually open connections to any of the devices, and thus can't
	// find the internal identifiers.  It just gives us a list of
	// Windows pseudo-file system paths to the available devices.  We
	// can at least be certain that these are all Pinscape units,
	// because only Pinscape units advertise our unique vendor 
	// interface GUID.
	std::list<VendorInterfaceDesc> paths;
	HRESULT hr = EnumerateDevices(paths);
	if (!SUCCEEDED(hr))
		return hr;

	// Now we have to ask the devices for their hardware identifiers,
	// which requires opening the devices one by one and querying the
	// identifier information.  WinUsb only allows exclusive access to
	// a device, so the query will fail if any other process is already
	// using a particular device's WinUsb interface.  (HID connections
	// won't stop us, though.)  Therefore don't treat it as an error if
	// we can't open a device, since the one we're looking for might be
	// available even if other devices are in use.
	for (auto &path : paths)
	{
		// open the device
		std::unique_ptr<VendorInterface> curdev;
		if (SUCCEEDED(path.Open(curdev)))
		{
			// query its IDs and check for a match
			DeviceID id;
			if (curdev->QueryID(id) == PinscapeResponse::OK	&& match(id))
			{
				// success - release the device to the caller
				device = curdev.release();
				return S_OK;
			}
		}
	}

	// no match found - return failure
	return S_FALSE;
}

int VendorInterface::QueryVersion(Version &vsn)
{
	// send the request, capturing the result arguments
	PinscapeResponse reply;
	int stat = SendRequest(PinscapeRequest::CMD_QUERY_VERSION, reply);

	// on success, and if the result arguments are the right size, copy the
	// results back to the caller's Version struct
	if (stat == PinscapeResponse::OK && reply.argsSize >= sizeof(reply.args.version))
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
	// send the request, capturing the result arguments
	PinscapeResponse reply;
	std::vector<BYTE> xferIn;
	int stat = SendRequest(PinscapeRequest::CMD_QUERY_IDS, reply, nullptr, 0, &xferIn);

	// on success, and if the result arguments are the right size, copy the
	// results back to the caller's out variable
	if (stat == PinscapeResponse::OK && reply.argsSize >= offsetnext(PinscapeResponse::Args::ID, ledWizUnitMask))
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

		// set the unit number
		id.unitNum = reply.args.id.unitNum;

		// set the XInput player number
		id.xinputPlayerIndex = reply.args.id.xinputPlayerIndex != 0xFF ? reply.args.id.xinputPlayerIndex : -1;

		// set the LedWiz unit number
		id.ledWizUnitMask = reply.args.id.ledWizUnitMask;

		// read the name strings from the extra transfer data
		{
			// set up to read the strings
			const BYTE *p = xferIn.data();
			const BYTE *end = p + xferIn.size();

			// read the unit name, up to the null byte
			const BYTE *start = p;
			for (; p < end && *p != 0 ; ++p) ;
			id.unitName.assign(reinterpret_cast<const char*>(start), p - start);

			// read the board name, up to the null byte
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0; ++p) ;
			id.targetBoardName.assign(reinterpret_cast<const char*>(start), p - start);

			// read the SDK version string, up to the null byte
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0 ; ++p) ;
			id.picoSDKVersion.assign(reinterpret_cast<const char*>(start), p - start);

			// read the GNUC version string, up to the null byte
			if (p < end) ++p;
			for (start = p ; p < end && *p != 0 ; ++p) ;
			id.compilerVersion.assign(reinterpret_cast<const char*>(start), p - start);
		}
	}

	// return the result
	return stat;
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

std::string IRCommand::ToString() const
{
	// Figure the bit width to include in the display.  This isn't 
	// necessary; it's just for aesthetics, to drop leading zeroes
	// while using one of the standard bit width (16, 24, 32, 48,
	// 56, 64).
	int nBits = (command > (1ULL << 56)) ? 64 :
		(command > (1ULL << 48)) ? 56 :
		(command > (1ULL << 32)) ? 48 :
		(command > (1ULL << 24)) ? 32 :
		(command > (1ULL << 16)) ? 24 : 16;

	// format the code
	char buf[128];
	sprintf_s(buf, "%02X.%02X.%0*I64X", protocol, flags, nBits/4, command);
	return buf;
}

bool IRCommand::Parse(const char *str, size_t len)
{
	// match our universal IR code format
	static const std::regex irCmdPat("\\s*([0-9a-f]{2})\\.([0-9a-f]{2})\\.([0-9a-f]{4,16})\\s*", std::regex_constants::icase);
	const std::string_view strView(str, len);
	std::match_results<std::string_view::iterator> m;
	if (!std::regex_match(strView.begin(), strView.end(), m, irCmdPat))
	{
		// invalid format
		return false;
	}

	// extract the elements
	protocol = static_cast<uint8_t>(strtol(m[1].str().c_str(), nullptr, 16));
	flags = static_cast<uint8_t>(strtol(m[2].str().c_str(), nullptr, 16));
	command = strtoll(m[3].str().c_str(), nullptr, 16);

	// success
	return true;
}

int VendorInterface::ResetPico()
{
	uint8_t args = PinscapeRequest::SUBCMD_RESET_NORMAL;
	return SendRequestWithArgs(PinscapeRequest::CMD_RESET, args);
}

int VendorInterface::EnterSafeMode()
{
	uint8_t args = PinscapeRequest::SUBCMD_RESET_SAFEMODE;
	return SendRequestWithArgs(PinscapeRequest::CMD_RESET, args);
}

int VendorInterface::EnterBootLoader()
{
	uint8_t args = PinscapeRequest::SUBCMD_RESET_BOOTLOADER;
	return SendRequestWithArgs(PinscapeRequest::CMD_RESET, args);
}

int VendorInterface::FactoryResetSettings()
{
	PinscapeRequest::Args::Config args{ PinscapeRequest::SUBCMD_CONFIG_RESET };
	return SendRequestWithArgs(PinscapeRequest::CMD_CONFIG, args);
}

int VendorInterface::EraseConfig(uint8_t fileID)
{
	PinscapeRequest::Args::Config args{ PinscapeRequest::SUBCMD_CONFIG_ERASE };
	args.fileID = fileID;
	return SendRequestWithArgs(PinscapeRequest::CMD_CONFIG, args);
}

int VendorInterface::PutConfig(const char *txt, uint32_t txtLen, uint8_t fileID)
{
	// figure the number of 4K pages
	const int PAGE_SIZE = 4096;
	int nPages = static_cast<int>((txtLen + PAGE_SIZE - 1) / PAGE_SIZE);

	// calculate the CRC of the entire file
	uint32_t crc = CRC::Calculate(txt, txtLen, CRC::CRC_32());

	// Send the START OF FILE request.  This is a dummy version of the
	// normal data page request, with no page data included and the page
	// number set to 0xFFFF.
	{
		PinscapeRequest::Args::Config cfg{ PinscapeRequest::SUBCMD_CONFIG_PUT };
		cfg.crc = crc;
		cfg.page = 0xFFFF;
		cfg.nPages = nPages;
		cfg.fileID = fileID;
		if (int stat = SendRequestWithArgs(PinscapeRequest::CMD_CONFIG, cfg); stat != PinscapeResponse::OK)
			return stat;
	}

	// send the pages to the device for storage in the device's flash memory
	const char *curPageTxt = txt;
	int txtLengthRemaining = static_cast<int>(txtLen);
	for (int pageNum = 0 ; pageNum < nPages ; ++pageNum, curPageTxt += PAGE_SIZE, txtLengthRemaining -= PAGE_SIZE)
	{
		// send one page, or the length remaining, whichever is smaller
		int copyLength = txtLengthRemaining < PAGE_SIZE ? txtLengthRemaining : PAGE_SIZE;

		// build the arguments
		PinscapeRequest::Args::Config cfg{ PinscapeRequest::SUBCMD_CONFIG_PUT };
		cfg.crc = crc;
		cfg.page = pageNum;
		cfg.nPages = nPages;
		cfg.fileID = fileID;

		// Send the request to store the page.  Once we start sending
		// a config file, it's important to complete the operation,
		// because abandoning it partway could leave the flash space
		// on the device with a partially written copy of the file,
		// preventing the device from booting normally.  That's still
		// easily recoverable through Safe Mode, but we'd like to make
		// the update as bulletproof as possible even so.  To that end,
		// allow for a few retries on each request in the event of an
		// error.  This will avoid abandoning the transfer if we run
		// into a transient USB hiccup.
		for (int tries = 0 ; ; ++tries)
		{
			// send the request to store the page
			const auto *xferOut = reinterpret_cast<const uint8_t*>(curPageTxt);
			int stat = SendRequestWithArgs(PinscapeRequest::CMD_CONFIG, cfg, xferOut, copyLength);

			// on success, we can stop retrying
			if (stat == PinscapeResponse::OK)
				break;

			// If this was a retry, and the device responded with a 
			// RETRY_OK code, it means that the device actually did
			// receive our previous attempt and processed it
			// successfully.  So the only error was that we didn't
			// get its acknowledgment - the page has in fact been
			// stored successfully, and we can safely move on to
			// the next one.  Note that RETRY_OK is only valid if
			// we actually were resending the page - if we get it
			// on our first try, it suggests that something is out
			// of sync between our state and the device's state,
			// so it might not be safe to proceed.
			if (stat == PinscapeResponse::ERR_RETRY_OK && tries > 0)
				break;

			// This call failed.  If we've already tried a couple
			// of times, give up, since any transient error that
			// the USB driver can recover from automatically should
			// clear quickly.  A repeated error probably won't
			// resolve without user intervention, so we'd just get
			// stuck here indefinitely if we kept retrying.
			if (tries > 3)
				return stat;
		}
	}

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::GetConfig(std::vector<char> &txt, uint8_t fileID)
{
	// retrieve the file in page-sized chunks
	std::list<std::vector<BYTE>> pages;
	size_t totalSize = 0;
	for (int pageNum = 0 ; ; ++pageNum)
	{
		// set up arguments for the next page
		PinscapeRequest::Args::Config cfg{ PinscapeRequest::SUBCMD_CONFIG_GET };
		cfg.page = pageNum;
		cfg.fileID = fileID;

		// allocate space in the page list
		auto &page = pages.emplace_back();

		// request the page
		int stat = SendRequestWithArgs(PinscapeRequest::CMD_CONFIG, cfg, nullptr, 0, &page);
		if (stat == PinscapeResponse::OK)
		{
			// Success - add this page into the total file size and keep
			// going.  
			totalSize += page.size();
		}
		else if (stat == PinscapeResponse::ERR_EOF)
		{
			// Accelerate of file - we've successfully retrieved all of the pages.
			// The last page we just asked for doesn't exist, so drop its
			// empty vector.
			pages.pop_back();

			// Size the vector to the file size
			txt.resize(totalSize);

			// copy the retrieved pages into the result buffer
			char *dst = txt.data();
			for (auto &page : pages)
			{
				// copy this page and advance the output pointer
				memcpy(dst, page.data(), page.size());
				dst += page.size();
			}

			// success
			return PinscapeResponse::OK;
		}
		else
		{
			// fail on any other error
			return stat;
		}
	}
}

int VendorInterface::QueryConfigFileExists(uint8_t fileID, bool &exists)
{
	// query the status
	PinscapeRequest::Args::Config args{ PinscapeRequest::SUBCMD_CONFIG_EXISTS };
	args.fileID = fileID;
	int stat = SendRequestWithArgs(PinscapeRequest::CMD_CONFIG, args);

	// Status OK means it exists, NOT FOUND means it doesn't.  Other errors
	// are possible (USB connection errors, etc), but treat any error as
	// meaning the file doesn't exist.  If the caller needs to distinguish
	// other errors from a definite NOT FOUND, it can check the returned
	// status code.
	exists = (stat == PinscapeResponse::OK);

	// return the status code
	return stat;
}

int VendorInterface::PutWallClockTime()
{
	// get the local system time
	SYSTEMTIME t;
	GetLocalTime(&t);

	// build the arguments
	PinscapeRequest::Args::Clock c{ 0 };
	c.year = t.wYear;
	c.month = static_cast<uint8_t>(t.wMonth);
	c.day = static_cast<uint8_t>(t.wDay);
	c.hour = static_cast<uint8_t>(t.wHour);
	c.minute = static_cast<uint8_t>(t.wMinute);
	c.second = static_cast<uint8_t>(t.wSecond);
	
	// send the request
	return SendRequestWithArgs(PinscapeRequest::CMD_SET_CLOCK, c);
}

int VendorInterface::QueryStats(PinscapePico::Statistics *stats, size_t sizeofStats,
	bool resetCounters)
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
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t args[2]{
		PinscapeRequest::SUBCMD_STATS_QUERY_STATS,
		static_cast<uint8_t>(resetCounters ? PinscapeRequest::QUERYSTATS_FLAG_RESET_COUNTERS : 0) 
	};
	int result = SendRequestWithArgs(PinscapeRequest::CMD_STATS, args, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// the statistics struct is returned in the transfer-in data
	auto *devStats = reinterpret_cast<PinscapePico::Statistics*>(xferIn.data());

	// Copy the smaller of the returned struct or the caller's struct; if
	// the caller's struct is bigger (newer), this will leave new fields
	// with their default zero values; if the caller's struct is smaller
	// (older), this will simply drop the new fields the caller doesn't
	// know about.
	memcpy(stats, devStats, min(sizeofStats, xferIn.size()));

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryPicoSystemClock(int64_t &w1, int64_t &w2, uint64_t &p1, uint64_t &p2)
{
	// send the pre-query command, to get the Pico into fast polling
	// mode, so that it processes the command as quickly as possible
	PinscapeResponse resp;
	uint8_t args = PinscapeRequest::SUBCMD_STATS_PREP_QUERY_CLOCK;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_STATS, args, resp);
	if (result != PinscapeResponse::OK)
		return result;

	// The main purpose of reading the Pico clock is to synchronize
	// time readings with the host clock.  The limiting factor in
	// the precision of the synchronization is the elapsed time in
	// the USB round trip, including time spent going through the
	// Windows API layers and USB driver stack layers.  We can't do
	// anything here to affect the actual USB hardware transmission
	// time, and we also can't do anything to make the Windows API
	// code paths shorter.  But we can at least streamline our own
	// code bracketing the USB send/receive operations, by calling
	// the Windows APIs directly rather than using our helper
	// functions.  That also lets us take the time snapshots
	// immediately before calling the "send" API and immediately
	// after the "receive" API returns.
	PinscapeRequest req{ token++, PinscapeRequest::CMD_STATS, 0 };
	req.args.argBytes[0] = PinscapeRequest::SUBCMD_STATS_QUERY_CLOCK;

	// set up the OVERLAPPED structure
	OVERLAPPEDHolder ovWrite(hDevice, winusbHandle);
	OVERLAPPEDHolder ovRead(hDevice, winusbHandle);

	// record the starting time
	LARGE_INTEGER t1, t2;
	QueryPerformanceCounter(&t1);

	// send the request
	ULONG sz;
	if (!WinUsb_WritePipe(winusbHandle, epOut,
		reinterpret_cast<BYTE*>(&req), static_cast<ULONG>(sizeof(req)), &sz, &ovWrite.ov)
		&& (GetLastError() != ERROR_IO_PENDING || !SUCCEEDED(ovWrite.Wait(REQUEST_TIMEOUT, sz))))
		return PinscapeResponse::ERR_FAILED;

	// read the reply
	if (!WinUsb_ReadPipe(winusbHandle, epIn,
		reinterpret_cast<BYTE*>(&resp), static_cast<ULONG>(sizeof(resp)), &sz, &ovRead.ov)
		&& (GetLastError() != ERROR_IO_PENDING || !SUCCEEDED(ovRead.Wait(REQUEST_TIMEOUT, sz))))
		return PinscapeResponse::ERR_FAILED;
	
	// record the return time snap
	QueryPerformanceCounter(&t2);

	// check the result
	if (resp.token != req.token || resp.argsSize < 16)
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// unpack the reply arguments (the Pico before and after timestamps)
	const uint8_t *p = resp.args.argBytes;
	p1 = GetUInt64(p);
	p2 = GetUInt64(p);

	// pass back the Windows timestamps
	w1 = t1.QuadPart;
	w2 = t2.QuadPart;

	// success
	return PinscapeResponse::OK;
}


int VendorInterface::QueryUSBInterfaceConfig(PinscapePico::USBInterfaces *ifcs, size_t sizeofIfcs)
{
	// zero the caller's struct, to set defaults for unpopulated fields
	memset(ifcs, 0, sizeofIfcs);

	// retrieve the interface information from the device
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	int result = SendRequest(PinscapeRequest::CMD_QUERY_USBIFCS, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// the USB info struct is returned in the transfer-in data
	auto *devIfcs = reinterpret_cast<PinscapePico::USBInterfaces*>(xferIn.data());

	// Copy the smaller of the returned struct or the caller's struct
	memcpy(ifcs, devIfcs, min(sizeofIfcs, xferIn.size()));

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryGPIOConfig(GPIOConfig gpio[30])
{
	// retrieve the GPIO information from the device
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	int result = SendRequest(PinscapeRequest::CMD_QUERY_GPIO_CONFIG, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// the transfer starts with the GPIO struct
	auto *gc = reinterpret_cast<PinscapePico::GPIOConfig*>(xferIn.data());

	// Decode the struct into our local representation.  Note that the
	// size of the port array struct might vary from our local version
	// of the struct, since the firmware might be compiled against a
	// different version of the interface file.  That's why the main
	// struct includes the port struct's size: this lets us traverse
	// the array by the actual size stored in the transfer data.
	const uint8_t *portSrcPtr = reinterpret_cast<const uint8_t*>(&gc->port[0]);
	GPIOConfig *portDst = gpio;
	for (UINT i = 0 ; i < gc->numPorts ; ++i, portSrcPtr += gc->cbPort, ++portDst)
	{
		// get the transfer port struct at the current array offset
		const auto *portSrc = reinterpret_cast<const PinscapePico::GPIOConfig::Port*>(portSrcPtr);

		// store the function and i/o direction
		portDst->func = static_cast<GPIOConfig::Func>(portSrc->func);
		portDst->sioIsOutput = ((portSrc->flags & portSrc->F_DIR_OUT) != 0);
	
		// Store the description string, if present.  The string is a 
		// null-terminated 7-bit ASCII string embedded in the transfer
		// data at the given byte offset from the start of the transfer
		// data.  Zero means that there's no string.
		if (portSrc->usageOfs != 0)
			portDst->usage.assign(reinterpret_cast<const char*>(xferIn.data() + portSrc->usageOfs));
		else
			portDst->usage.clear();
	}

	// success
	return PinscapeResponse::OK;
}


int VendorInterface::QueryFileSysInfo(PinscapePico::FlashFileSysInfo *info, size_t sizeofInfo)
{
	// Zero the caller's struct, so that any fields that are beyond the
	// length of the returned struct will be zeroed on return.
	memset(info, 0, sizeofInfo);

	// retrieve the file system information from the device
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t args = PinscapeRequest::SUBCMD_FLASH_QUERY_FILESYS;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_FLASH_STORAGE, args, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// the file system info struct is returned in the transfer-in data
	auto *devInfo = reinterpret_cast<PinscapePico::FlashFileSysInfo*>(xferIn.data());

	// Copy the smaller of the returned struct or the caller's struct
	memcpy(info, devInfo, min(sizeofInfo, xferIn.size()));

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::ReadFlashSector(uint32_t ofs, std::vector<uint8_t> &sector)
{
	// clear the caller's vector
	sector.clear();

	// set up arguments
	PinscapeRequest::Args::Flash args{ PinscapeRequest::SUBCMD_FLASH_READ_SECTOR };
	args.ofs = ofs;

	// retrieve the sector from the device
	PinscapeResponse resp;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_FLASH_STORAGE, args, resp, nullptr, 0, &sector);
	if (result != PinscapeResponse::OK)
		return result;

	// validate the CRC-32, to ensure the integrity of the data transfer
	uint32_t crc32 = CRC::Calculate(sector.data(), sector.size(), CRC::CRC_32());
	if (resp.args.flash.crc32 != crc32)
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// success
	return result;
}


int VendorInterface::QueryLog(std::vector<uint8_t> &text, size_t *totalAvailable)
{
	// send the request
	PinscapeResponse resp;
	int stat = SendRequest(PinscapeRequest::CMD_QUERY_LOG, resp, nullptr, 0, &text);

	// If the caller wants the total available size, fill it in: if we got
	// a valid reply, use the value from the reply arguments, otherwise
	// just zero it
	if (totalAvailable != nullptr)
		*totalAvailable = (stat == PinscapeResponse::OK) ? resp.args.log.avail : 0;

	// return the status
	return stat;
}

int VendorInterface::SendIRCommand(const IRCommand &cmd, int repeatCount)
{
	// make sure the count is in range
	if (repeatCount < 1 || repeatCount > 255)
		return PinscapeResponse::ERR_OUT_OF_BOUNDS;

	// send it
	PinscapeRequest::Args::SendIR args{ 0 };
	args.protocol = cmd.protocol;
	args.flags = cmd.flags;
	args.code = cmd.command;
	args.count = static_cast<uint8_t>(repeatCount);
	return SendRequestWithArgs(PinscapeRequest::CMD_SEND_IR, args);
}

int VendorInterface::QueryIRCommandsReceived(std::vector<IRCommandReceived> &commands)
{
	// query commands
	uint8_t args = PinscapeRequest::SUBCMD_QUERY_IR_CMD;
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_QUERY_IR, args, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// decode the header
	auto *lst = reinterpret_cast<const PinscapePico::IRCommandList*>(xferIn.data());
	if (xferIn.size() < sizeof(PinscapePico::IRCommandList))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// get the element size and count
	UINT n = lst->numEle;
	UINT eleSize = lst->cbEle;

	// bounds-check it
	if (static_cast<UINT>(lst->cb) + n*eleSize > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// get the pointer to the first element
	const uint8_t *pEle = xferIn.data() + lst->cb;
	
	// make room in the vector
	commands.clear();
	commands.resize(n);

	// populate it
	for (UINT i = 0 ; i < n ; ++i, pEle += eleSize)
	{
		// get the source and destination elements
		auto *src = reinterpret_cast<const PinscapePico::IRCommandListEle*>(pEle);
		auto &dst = commands[i];

		// decode the source element
		dst.elapsedTime_us = src->dt;
		dst.command = src->cmd;
		dst.protocol = src->protocol;
		dst.flags = src->proFlags;
		dst.cmdFlags = src->cmdFlags;

		dst.proHasDittos = (src->proFlags & src->FPRO_DITTOS) != 0;
		dst.hasDitto = (src->cmdFlags & src->F_HAS_DITTO) != 0;
		dst.ditto = (src->cmdFlags & src->F_DITTO_FLAG) != 0;
		dst.hasToggle = (src->cmdFlags & src->F_HAS_TOGGLE) != 0;
		dst.toggle = (src->cmdFlags & src->F_TOGGLE_BIT) != 0;
		dst.isAutoRepeat = (src->cmdFlags & src->F_AUTOREPEAT) != 0;

		static const std::unordered_map<uint8_t, uint8_t> posCodeMap{
			{ IRCommandListEle::F_POS_FIRST, IRCommandReceived::POS_FIRST },
			{ IRCommandListEle::F_POS_MIDDLE, IRCommandReceived::POS_MIDDLE },
			{ IRCommandListEle::F_POS_LAST, IRCommandReceived::POS_LAST },
		};
		auto itPosCode = posCodeMap.find(src->cmdFlags & src->F_POS_MASK);
		dst.posCode = itPosCode != posCodeMap.end() ? itPosCode->second : dst.POS_NULL;
	}

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryIRRawPulsesReceived(std::vector<IRRawPulse> &pulses)
{
	// query raw pulses
	uint8_t args = PinscapeRequest::SUBCMD_QUERY_IR_RAW;
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_QUERY_IR, args, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// decode the header
	auto *lst = reinterpret_cast<const PinscapePico::IRRawList*>(xferIn.data());
	if (xferIn.size() < sizeof(PinscapePico::IRRawList))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// get the element size and count
	UINT n = lst->numRaw;
	UINT eleSize = lst->cbRaw;

	// bounds-check it
	if (static_cast<UINT>(lst->cb) + n*eleSize > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// get the pointer to the first element
	const uint8_t *pEle = xferIn.data() + lst->cb;

	// make room in the vector
	pulses.clear();
	pulses.resize(n);

	// populate it
	for (UINT i = 0 ; i < n ; ++i, pEle += eleSize)
	{
		// get the source and destination elements
		auto *src = reinterpret_cast<const PinscapePico::IRRaw*>(pEle);
		auto &dst = pulses[i];

		// decode the source element
		dst.mark = (src->type != 0);
		dst.t = src->t == 0xFFFF ? -1 : src->t*2;
	}

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryTVONState(TVONState &state)
{
	// send the query request
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_TVON_QUERY_STATE;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_TVON, subcmd, resp);
	if (result != PinscapeResponse::OK)
		return result;

	// parse the arguments
	const auto &t = resp.args.tvon;
	state.powerState = t.powerState;
	state.gpioState = t.gpioState;
	state.relayState = (t.relayState != 0);
	state.relayStatePowerOn = ((t.relayState & t.RELAY_STATE_POWERON) != 0);
	state.relayStateManual = ((t.relayState & t.RELAY_STATE_MANUAL) != 0);
	state.relayStateManualPulse = ((t.relayState & t.RELAY_STATE_MANUAL_PULSE) != 0);
	state.irCommandIndex = t.irCommandIndex;
	state.irCommandCount = t.irCommandCount;

	// look up the power state name; if it's unknown, add an entry with a generic name
	if (auto it = tvOnStateNames.find(t.powerState); it != tvOnStateNames.end())
		state.powerStateName = it->second;
	else
	{
		// synthesize a generic name based on the state number
		char buf[32];
		sprintf_s(buf, "State #%u", t.powerState);
		state.powerStateName = buf;

		// add it to the map, so that we reuse it if we encounter it again during this session
		tvOnStateNames.emplace(t.powerState, buf);
	}

	// success
	return result;
}

int VendorInterface::SetTVRelayManualState(bool on)
{
	// send the request
	PinscapeResponse resp;
	uint8_t args[2]{ 
		PinscapeRequest::SUBCMD_TVON_SET_RELAY, 
		on ? PinscapeRequest::TVON_RELAY_ON : PinscapeRequest::TVON_RELAY_OFF
	};
	return SendRequestWithArgs(PinscapeRequest::CMD_TVON, args);
}

int VendorInterface::PulseTVRelay()
{
	// send the request
	PinscapeResponse resp;
	uint8_t args[2]{
		PinscapeRequest::SUBCMD_TVON_SET_RELAY,
		PinscapeRequest::TVON_RELAY_PULSE
	};
	return SendRequestWithArgs(PinscapeRequest::CMD_TVON, args);
}

int VendorInterface::StartNudgeCalibration(bool autoSave)
{
	uint8_t args[2]{ PinscapeRequest::SUBCMD_NUDGE_CALIBRATE, static_cast<uint8_t>(autoSave ? 1 : 0) };
	return SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, args);
}

int VendorInterface::SetNudgeCenterPoint()
{
	uint8_t args = PinscapeRequest::SUBCMD_NUDGE_CENTER;
	return SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, args);
}

int VendorInterface::QueryNudgeStatus(PinscapePico::NudgeStatus *stat, size_t statSize)
{
	// clear the caller's struct, to zero fields that aren't set by the firmware
	memset(stat, 0, statSize);

	// send the request
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_NUDGE_QUERY_STATUS;
	std::vector<BYTE> xferIn;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// we need at least the 'cb' element in the struct
	if (xferIn.size() < sizeof(PinscapePico::NudgeStatus::cb))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// copy the smaller of the caller's struct of the firmware's struct
	const auto *devStat = reinterpret_cast<PinscapePico::NudgeStatus*>(xferIn.data());
	memcpy(stat, devStat, min(statSize, devStat->cb));

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryNudgeParams(NudgeParams *params, size_t paramsSize)
{
	// clear the caller's struct
	memset(params, 0, sizeof(*params));

	// send the request
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_NUDGE_QUERY_PARAMS;
	std::vector<BYTE> xferIn;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure the struct is at least the expected size
	if (xferIn.size() < sizeof(PinscapePico::NudgeParams))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// make sure the size is within range
	auto const *devParams = reinterpret_cast<NudgeParams*>(xferIn.data());
	if (devParams->cb > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// copy the smaller of the caller's struct or the network struct
	memcpy(params, xferIn.data(), min(paramsSize, devParams->cb));

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::PutNudgeParams(const NudgeParams *params, size_t paramsSize)
{
	// send the request
	uint8_t subcmd = PinscapeRequest::SUBCMD_NUDGE_PUT_PARAMS;
	return SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, subcmd, reinterpret_cast<const BYTE*>(params), paramsSize);
}

int VendorInterface::CommitNudgeSettings()
{
	uint8_t subcmd = PinscapeRequest::SUBCMD_NUDGE_COMMIT;
	return SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, subcmd);
}

int VendorInterface::RevertNudgeSettings()
{
	uint8_t subcmd = PinscapeRequest::SUBCMD_NUDGE_REVERT;
	return SendRequestWithArgs(PinscapeRequest::CMD_NUDGE, subcmd);
}

int VendorInterface::QueryPlungerConfig(PinscapePico::PlungerConfig &config)
{
	// clear the caller's PlungerData struct, to zero any extra fields
	// not present in the firmware's version of the struct
	memset(&config, 0, sizeof(config));

	// Make the request.  The reply transfer data consists of the firmware's
	// version of the PlungerData struct appended with any additional sensor
	// data; for now, load the entire response into the caller's vector.
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_PLUNGER_QUERY_CONFIG;
	std::vector<BYTE> xferIn;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// The returned data starts with a PlungerData struct.  Make sure
	// the returned data is big enough to hold at least the size field
	// at the start of the PlungerData struct
	const auto *devConfig = reinterpret_cast<const PinscapePico::PlungerConfig*>(xferIn.data());
	if (xferIn.size() < sizeof(PinscapePico::PlungerConfig::cb))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Copy the PlungerConfig into the caller's struct, up to the smaller
	// of the caller's and firmware's struct size, and return success.
	memcpy(&config, devConfig, min(sizeof(config), devConfig->cb));
	return PinscapeResponse::OK;
}

int VendorInterface::QueryPlungerReading(PinscapePico::PlungerReading &reading, std::vector<BYTE> &sensorData)
{
	// clear the caller's struct, to zero any extra fields not present in
	// the firmware's version of the struct
	memset(&reading, 0, sizeof(reading));

	// Make the request.  The reply transfer data consists of the firmware's
	// version of the struct appended with any additional sensor data; for
	// now, load the entire response into the caller's vector.
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_PLUNGER_QUERY_READING;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, subcmd, resp, nullptr, 0, &sensorData);
	if (result != PinscapeResponse::OK)
		return result;

	// The returned data starts with a PlungerReading struct.  Make sure
	// the returned data is big enough to hold at least the size field
	// at the start of the struct.
	const auto *devReading = reinterpret_cast<const PinscapePico::PlungerReading*>(sensorData.data());
	if (sensorData.size() < sizeof(PinscapePico::PlungerReading::cb))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Copy the returned PlungerReading into the caller's struct, up to 
	// the smaller of the caller's and firmware's struct size.
	memcpy(&reading, devReading, min(sizeof(reading), devReading->cb));

	// Delete the PlungerReading struct from the start of the caller's
	// vector, so that we leave just the extra sensor data portion.
	// It's easier for the caller to interpret the returned data if we
	// handle the separation of the two portions so that they see a
	// C++ struct for the fixed part rather than just the byte array
	// returned on the wire.
	sensorData.erase(sensorData.begin(), sensorData.begin() + devReading->cb);
	
	// success
	return PinscapeResponse::OK;
}

// set the plunger jitter filter window
int VendorInterface::SetPlungerJitterFilter(int windowSize)
{
	// validate the size
	if (windowSize < 0 || windowSize > UINT16_MAX)
		return PinscapeResponse::ERR_OUT_OF_BOUNDS;

	// send the request
	PinscapeRequest::Args::JitterFilter args{ PinscapeRequest::SUBCMD_PLUNGER_SET_JITTER_FILTER };
	args.windowSize = static_cast<uint16_t>(windowSize);
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::SetPlungerFiringTime(uint32_t maxFiringTime_us)
{
	PinscapeRequest::Args::PlungerInt args{ PinscapeRequest::SUBCMD_PLUNGER_SET_FIRING_TIME_LIMIT };
	args.u = maxFiringTime_us;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::SetPlungerIntegrationTime(uint32_t integrationTime_us)
{
	PinscapeRequest::Args::PlungerInt args{ PinscapeRequest::SUBCMD_PLUNGER_SET_INTEGRATION_TIME };
	args.u = integrationTime_us;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::SetPlungerScalingFactor(uint32_t scalingFactor)
{
	PinscapeRequest::Args::PlungerInt args{ PinscapeRequest::SUBCMD_PLUNGER_SET_SCALING_FACTOR };
	args.u = scalingFactor;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::SetPlungerScanMode(uint8_t scanMode)
{
	PinscapeRequest::Args::PlungerByte args{ PinscapeRequest::SUBCMD_PLUNGER_SET_SCAN_MODE };
	args.b = scanMode;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::SetPlungerOrientation(bool reverseOrientation)
{
	PinscapeRequest::Args::PlungerByte args{ PinscapeRequest::SUBCMD_PLUNGER_SET_ORIENTATION };
	args.b = (reverseOrientation ? 1 : 0);
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::StartPlungerCalibration(bool autoSave)
{
	PinscapeRequest::Args::PlungerByte args{ PinscapeRequest::SUBCMD_PLUNGER_CALIBRATE, static_cast<uint8_t>(autoSave ? 0x01 : 0x00) };
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args);
}

int VendorInterface::SetPlungerCalibrationData(const PinscapePico::PlungerCal *data, size_t sizeofData)
{
	uint8_t args = PinscapeRequest::SUBCMD_PLUNGER_SET_CAL_DATA;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, args, reinterpret_cast<const BYTE*>(data), sizeofData);
}

// Save plunger settings
int VendorInterface::CommitPlungerSettings()
{
	uint8_t subcmd = PinscapeRequest::SUBCMD_PLUNGER_COMMIT_SETTINGS;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, subcmd);
}

// Save plunger settings
int VendorInterface::RevertPlungerSettings()
{
	uint8_t subcmd = PinscapeRequest::SUBCMD_PLUNGER_REVERT_SETTINGS;
	return SendRequestWithArgs(PinscapeRequest::CMD_PLUNGER, subcmd);
}

// Copy a transfer buffer array to a local array of the same type
// taking into account that the transfer buffer might be using a
// different version of the same struct, with a different size.
template<class T> static void CopyTransferArray(T dst[], size_t dstCount, const uint8_t *src, unsigned int srcCount, unsigned int srcEleSize)
{
	// Zero the output buffer, so that any fields that aren't copied
	// from the transfer struct have the well-defined default value
	// zero.  This applies if the event that the transfer struct is
	// an older, smaller version that's missing some fields in the
	// current struct definition we're using.
	memset(dst, 0, dstCount * sizeof(dst[0]));

	// copy the elements
	size_t eleCopySize = min(sizeof(T), srcEleSize);
	size_t copyCount = min(srcCount, dstCount);
	for (size_t i = 0 ; i < copyCount ; ++i, ++dst, src += srcEleSize)
		memcpy(dst, src, eleCopySize);
}

// Copy a transfer buffer array to a local vector of the same type,
// taking into account that the transfer buffer might be using a
// different version of the same struct, with a different size.
template<class T> static void CopyTransferArray(std::vector<T> &dst, const uint8_t *src, unsigned int srcCount, unsigned int srcEleSize)
{
	// clear any old data from the output vector, and size it for
	// the new transfer array
	dst.clear();
	dst.resize(srcCount);

	// copy the transfer array into the vector array
	CopyTransferArray(dst.data(), dst.size(), src, srcCount, srcEleSize);
}

int VendorInterface::QueryButtonConfig(
	std::vector<PinscapePico::ButtonDesc> &buttons,
	std::vector<PinscapePico::ButtonDevice> &devices)
{
	// send the request
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t subcmd = PinscapeRequest::SUBCMD_BUTTON_QUERY_DESCS;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure that the transfer data is large enough to contain the
	// list header fields we need to parse
	if (xferIn.size() < offsetnext(PinscapePico::ButtonList, numDevices))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the transfer data header
	const auto *btnList = reinterpret_cast<const PinscapePico::ButtonList*>(xferIn.data());

	// validate that the arrays are within the transfer bounds
	if (static_cast<size_t>(btnList->ofsFirstDesc + (btnList->numDescs * btnList->cbDesc)) > xferIn.size()
		|| static_cast<size_t>(btnList->ofsFirstDevice + (btnList->numDevices * btnList->cbDevice)) > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// get the array pointers, based on the buffer offsets
	const uint8_t *pDesc = xferIn.data() + btnList->ofsFirstDesc;
	const uint8_t *pDevice = xferIn.data() + btnList->ofsFirstDevice;

	// build the caller's button descriptor list
	CopyTransferArray(buttons, pDesc, btnList->numDescs, btnList->cbDesc);

	// build the caller's device list
	CopyTransferArray(devices, pDevice, btnList->numDevices, btnList->cbDevice);

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryLogicalButtonStates(std::vector<BYTE> &states, uint32_t &shiftState)
{
	// send the request, and pass the returned state byte array directly
	// back to the caller, since it's already in the right format
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_BUTTON_QUERY_STATES;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, subcmd, resp, nullptr, 0, &states);

	// retrieve the shift states if available
	if (result == PinscapeResponse::OK && resp.argsSize >= offsetnext(PinscapeResponse::Args::ButtonState, globalShiftState))
		shiftState = resp.args.buttonState.globalShiftState;

	// return the result
	return result;
}

int VendorInterface::QueryButtonGPIOStates(std::vector<BYTE> &states)
{
	// send the request, and pass the returned state byte array directly
	// back to the caller, since it's already in the right format
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_BUTTON_QUERY_GPIO_STATES;
	return SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, subcmd, resp, nullptr, 0, &states);
}

int VendorInterface::QueryButtonPCA9555States(std::vector<BYTE> &states)
{
	// send the request, and pass the returned state byte array directly
	// back to the caller, since it's already in the right format
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_BUTTON_QUERY_PCA9555_STATES;
	return SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, subcmd, resp, nullptr, 0, &states);
}

int VendorInterface::QueryButton74HC165States(std::vector<BYTE> &states)
{
	// send the request, and pass the returned state byte array directly
	// back to the caller, since it's already in the right format
	PinscapeResponse resp;
	uint8_t subcmd = PinscapeRequest::SUBCMD_BUTTON_QUERY_74HC165_STATES;
	return SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, subcmd, resp, nullptr, 0, &states);
}

int VendorInterface::QueryButtonEventLog(std::vector<PinscapePico::ButtonEventLogItem> &events, int gpioNumber)
{
	// range-check the GPIO number for argument packing
	if (gpioNumber < 0 || gpioNumber > 255)
		return PinscapeResponse::ERR_BAD_PARAMS;

	// send the request
	uint8_t args[2]{ PinscapeRequest::SUBCMD_BUTTON_QUERY_EVENT_LOG, static_cast<uint8_t>(gpioNumber) };
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	int stat = SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, args, nullptr, 0, &xferIn); 
	if (stat != PinscapeResponse::OK)
		return stat;

	// sanity-check the response
	if (xferIn.size() < offsetnext(PinscapePico::ButtonEventLog, cbItem))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the response header, and check that there's enough data to fill
	// out the list with the size claimed in the header.
	const auto *hdr = reinterpret_cast<const PinscapePico::ButtonEventLog*>(xferIn.data());
	size_t minSize = hdr->cb + hdr->cbItem*hdr->numItems;
	if (xferIn.size() < minSize)
		return PinscapeResponse::ERR_BAD_REPLY_DATA;
	
	// allocate the reply list and clear it to all zero bytes (this will
	// zero extra bytes in the caller's struct if the Pico's item struct 
	// is older==smaller than the version we're compiled against)
	events.resize(hdr->numItems);
	memset(events.data(), 0, events.size() * sizeof(PinscapePico::ButtonEventLogItem));

	// populate the reply
	const auto *srcItemBytePtr = reinterpret_cast<const uint8_t*>(xferIn.data() + hdr->cb);
	PinscapePico::ButtonEventLogItem *dstItem = events.data();
	for (unsigned int i = 0 ; i < hdr->numItems ; ++i, ++dstItem, srcItemBytePtr += hdr->cbItem)
		memcpy(dstItem, srcItemBytePtr, hdr->cbItem);
	
	// success
	return stat;
}

int VendorInterface::ClearButtonEventLog(int gpioNumber)
{
	// range-check the GPIO number for argument packing
	if (gpioNumber < 0 || gpioNumber > 255)
		return PinscapeResponse::ERR_BAD_PARAMS;

	// send the request
	uint8_t args[2]{ PinscapeRequest::SUBCMD_BUTTON_CLEAR_EVENT_LOG, static_cast<uint8_t>(gpioNumber) };
	PinscapeResponse resp;
	return SendRequestWithArgs(PinscapeRequest::CMD_BUTTONS, args);
}

int VendorInterface::SetLogicalOutputPortLevel(uint8_t port, uint8_t level)
{
	// send the request
	struct { uint8_t args[3]; } args{ PinscapeRequest::SUBCMD_OUTPUT_SET_PORT, port, level };
	return SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, args);
}

int VendorInterface::SetPhysicalOutputPortLevel(
	uint8_t deviceType, uint8_t configIndex,
	uint8_t port, uint16_t pwmLevel)
{
	PinscapeRequest::Args::OutputDevPort args{ PinscapeRequest::SUBCMD_OUTPUT_SET_DEVICE_PORT };
	args.devType = deviceType;
	args.configIndex = configIndex;
	args.port = port;
	args.pwmLevel = pwmLevel;
	return SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, args);
}

int VendorInterface::QueryLogicalOutputPortConfig(std::vector<PinscapePico::OutputPortDesc> &ports)
{
	// send the request
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t subcmd = PinscapeRequest::SUBCMD_OUTPUT_QUERY_LOGICAL_PORTS;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure that the transfer data is large enough to contain the
	// list header fields we need to parse
	if (xferIn.size() < offsetnext(PinscapePico::OutputPortList, numDescs))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the transfer data header
	const auto *hdr = reinterpret_cast<const PinscapePico::OutputPortList*>(xferIn.data());

	// validate that the array bounds are within the transfer data
	if (static_cast<size_t>(hdr->cb + (hdr->numDescs * hdr->cbDesc)) > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// build the caller's port descriptor list - the array begins after the descriptor
	const uint8_t *pDesc = xferIn.data() + hdr->cb;
	CopyTransferArray(ports, pDesc, hdr->numDescs, hdr->cbDesc);

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryOutputDeviceConfig(std::vector<PinscapePico::OutputDevDesc> &devices)
{
	// send the request
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t subcmd = PinscapeRequest::SUBCMD_OUTPUT_QUERY_DEVICES;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure that the transfer data is large enough to contain the
	// list header fields we need to parse
	if (xferIn.size() < offsetnext(PinscapePico::OutputDevList, cbDesc))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the transfer data header
	const auto *hdr = reinterpret_cast<const PinscapePico::OutputDevList*>(xferIn.data());

	// validate that the descriptor array is within the transfer bounds
	if (static_cast<size_t>(hdr->cb + (hdr->numDescs * hdr->cbDesc)) > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// build the caller's device descriptor list - the descriptors start
	// immediately after the header
	const uint8_t *pDesc = xferIn.data() + hdr->cb;
	CopyTransferArray(devices, pDesc, hdr->numDescs, hdr->cbDesc);

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryOutputDevicePortConfig(std::vector<PinscapePico::OutputDevPortDesc> &devices)
{
	// send the request
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t subcmd = PinscapeRequest::SUBCMD_OUTPUT_QUERY_DEVICE_PORTS;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure that the transfer data is large enough to contain the
	// list header fields we need to parse
	if (xferIn.size() < offsetnext(PinscapePico::OutputDevPortList, cbDesc))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the transfer data header
	const auto *hdr = reinterpret_cast<const PinscapePico::OutputDevPortList*>(xferIn.data());

	// validate that the descriptor array is within the transfer bounds
	if (static_cast<size_t>(hdr->cb + (hdr->numDescs * hdr->cbDesc)) > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// build the caller's device descriptor list - the descriptors start
	// immediately after the header
	const uint8_t *pDesc = xferIn.data() + hdr->cb;
	CopyTransferArray(devices, pDesc, hdr->numDescs, hdr->cbDesc);

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryLogicalOutputLevels(bool &testMode, std::vector<PinscapePico::OutputLevel> &levels)
{
	// send the request
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t subcmd = PinscapeRequest::SUBCMD_OUTPUT_QUERY_LOGICAL_PORT_LEVELS;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure that the transfer data is large enough to contain the
	// list header fields we need to parse
	if (xferIn.size() < offsetnext(PinscapePico::OutputLevelList, flags))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the transfer data header
	const auto *hdr = reinterpret_cast<const PinscapePico::OutputLevelList*>(xferIn.data());

	// validate that the port level list is within the transfer bounds
	if (static_cast<size_t>(hdr->cb + (hdr->numLevels * hdr->cbLevel)) > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// build the caller's port level list
	const uint8_t *pLevel = xferIn.data() + hdr->cb;
	CopyTransferArray(levels, pLevel, hdr->numLevels, hdr->cbLevel);
	
	// get the test mode flag
	testMode = ((hdr->flags & hdr->F_TEST_MODE) != 0);

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::QueryPhysicalOutputDeviceLevels(std::vector<PinscapePico::OutputDevLevel> &levels)
{
	// send the request
	PinscapeResponse resp;
	std::vector<BYTE> xferIn;
	uint8_t subcmd = PinscapeRequest::SUBCMD_OUTPUT_QUERY_DEVICE_PORT_LEVELS;
	int result = SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, subcmd, resp, nullptr, 0, &xferIn);
	if (result != PinscapeResponse::OK)
		return result;

	// make sure that the transfer data is large enough to contain the
	// list header fields we need to parse
	if (xferIn.size() < offsetnext(PinscapePico::OutputDevLevelList, numLevels))
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// Get the transfer data header
	const auto *hdr = reinterpret_cast<const PinscapePico::OutputDevLevelList*>(xferIn.data());

	// validate that the port level list is within the transfer bounds
	if (static_cast<size_t>(hdr->cb + (hdr->numLevels * hdr->cbLevel)) > xferIn.size())
		return PinscapeResponse::ERR_BAD_REPLY_DATA;

	// build the caller's port level list
	const uint8_t *pLevel = xferIn.data() + hdr->cb;
	CopyTransferArray(levels, pLevel, hdr->numLevels, hdr->cbLevel);

	// success
	return PinscapeResponse::OK;
}

int VendorInterface::SetOutputTestMode(bool testMode, uint32_t timeout_ms)
{
	PinscapeRequest::Args::OutputTestMode args{ PinscapeRequest::SUBCMD_OUTPUT_TEST_MODE };
	args.enable = testMode;
	args.timeout_ms = timeout_ms;
	return SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, args);
}

int VendorInterface::SendRequest(uint8_t cmd, const BYTE *xferOutData, size_t xferOutLength, std::vector<BYTE> *xferInData)
{
	// build the request struct
	PinscapeRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));

	// send the request
	return SendRequest(req, xferOutData, xferInData);
}

int VendorInterface::SendRequest(uint8_t cmd, PinscapeResponse &resp,
	const BYTE *xferOutData, size_t xferOutLength, std::vector<BYTE> *xferInData)
{
	// build the request struct
	PinscapeRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));

	// send the request
	return SendRequest(req, resp, xferOutData, xferInData);
}

int VendorInterface::SendRequest(const PinscapeRequest &request,
	const BYTE *xferOutData, std::vector<BYTE> *xferInData)
{
	// send the request, capturing into a temporary response struct (which
	// we'll discard once we're done, since the caller doesn't need it)
	PinscapeResponse resp;
	return SendRequest(request, resp, xferOutData, xferInData);
}

int VendorInterface::SendRequest(
	const PinscapeRequest &request, PinscapeResponse &resp,
	const BYTE *xferOutData, std::vector<BYTE> *xferInData)
{
	// translate HRESULT from pipe operation to status code
	static auto PipeHRESULTToReturnCode = [](HRESULT hr)
	{
		// if HRESULT indicated success, it must be a bad transfer size
		if (SUCCEEDED(hr))
			return PinscapeResponse::ERR_USB_XFER_FAILED;

		// E_ABORT is a timeout error; for others, consider it a general USB transfer error
		return hr == E_ABORT ? PinscapeResponse::ERR_TIMEOUT : PinscapeResponse::ERR_USB_XFER_FAILED;
	};

	// it's an error if the request has a non-zero additional transfer-out
	// data size, and the caller didn't provide the data to send
	if (request.xferBytes != 0 && xferOutData == nullptr)
		return PinscapeResponse::ERR_BAD_XFER_LEN;

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
			return PinscapeResponse::ERR_BAD_XFER_LEN;

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
			return (ret == PinscapeResponse::ERR_TIMEOUT && readCount > 0) ?
				PinscapeResponse::ERR_REPLY_MISMATCH : ret;
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
		return PinscapeResponse::ERR_REPLY_MISMATCH;
	
	// read any additional response data
	if (resp.xferBytes != 0)
	{
		// If the caller didn't provide an output vector, it's an error,
		// but we'll still need to read the data to keep the pipe in sync.
		if (xferInData == nullptr)
		{
			// read the data into a temporary object
			std::unique_ptr<BYTE> buf(new BYTE[resp.xferBytes]);
			Read(buf.get(), resp.xferBytes, sz, REQUEST_TIMEOUT);

			// the absence of an output buffer is a parameter error
			return PinscapeResponse::ERR_BAD_PARAMS;
		}

		// resize the output buffer vector and read the data
		xferInData->resize(resp.xferBytes);
		hr = Read(xferInData->data(), resp.xferBytes, sz, REQUEST_TIMEOUT);
		if (!SUCCEEDED(hr) || sz != resp.xferBytes)
			return PipeHRESULTToReturnCode(hr);
	}

	// the USB exchange was concluded successfully, so return the status
	// code from the device response
	return resp.status;
}

// Enumerate HID interfaces associated with the same physical device
// as the vendor interface that WinUSB is attached to.
//
// Windows generally tries to make every USB interface exposed by a
// physical device look to applications like a whole separate logical
// device.  The physical device itself isn't much of an entity in the
// model.  That's probably ideal for most applications, but for our
// purposes, we need to know which HIDs go with a particular vendor
// (WinUSB) interface.  That's what this function is for; it finds
// the logical HID interfaces exposed by the same physical device as
// the target WinUSB logical device.  The association is important
// for our purposes because we could have multiple Picos installed
// on the same system, and each one will have its own set of feedback
// devices attached.  The HID interfaces can send commands to the
// feedback devices, so in order to route a HID command to the
// correct feedback device, we have to send it to the HID exposed
// by the physical Pico that the target feedback device is actually
// physically connected to.  The vendor interface is where we get
// and set that configuration information about what's connected,
// so we have to be able to tie the HID's physical device back to
// the WinUSB vendor interface's physical device.  You'd think that
// would be straightforward at the Windows driver level, but Windows
// seems very determined to maintain the view where every interface
// is just a separate logical device.  That's why we need a special
// function to make the association - there's not (as far as I can
// tell) a straightforward Win32 API you can call to find the logical
// interfaces associated with a physical device.  I'm sure the
// information is buried in there somewhere, but it's not clear that
// applications are meant to interpret it for this particular purpose.
// Our approach uses our own interfaces instead, and since we control
// those on the device side, we can deliberately arrange them to
// provide us with the information we need.
HRESULT VendorInterface::EnumerateAssociatedHIDs(std::list<WSTRING> &hidList)
{
	// clear anything previously in the list
	hidList.clear();

	// Set up a device set for all currently connected HID devices.
	// Note that we have to enumerate all of the HID devices in the
	// system, and then do our own filtering.  This API doesn't let
	// us filter to the HID interfaces associated with our known
	// device.  (It *seems* like you should be able to do that, by
	// passing the device instance ID as the 'Enumerator' argument,
	// but that doesn't work because our device instance ID is tied
	// to the vendor interface, not the physical device.  There
	// doesn't seem to be anything we can pass for the 'Enumerator'
	// that filters to a physical device.)
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);
	HDEVINFO hdi = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, 
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (hdi == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

	// enumerate HID interfaces in the device set
	HRESULT hresult = S_OK;
	SP_DEVICE_INTERFACE_DATA did{ sizeof(SP_DEVICE_INTERFACE_DATA) };
	for (DWORD memberIndex = 0 ;
		SetupDiEnumDeviceInterfaces(hdi, NULL, &hidGuid, memberIndex, &did) ;
		++memberIndex)
	{
		// retrieve the required buffer size for device detail
		DWORD diDetailSize = 0;
		DWORD err = 0;
		if (!SetupDiGetDeviceInterfaceDetail(hdi, &did, NULL, 0, &diDetailSize, NULL)
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

		// We now have a device path that we can use to access the HID
		// interface.  All we're going to do with the handle is ask for the
		// USB device serial number, which doesn't require any access rights
		// in the CreateFile() call, not even GENERIC_READ.  (And asking for
		// higher levels of access can actively interfere with our job here,
		// because they can trigger ACCESS DENIED errors for some keyboard
		// devices.)
		HANDLE hDevice = CreateFileW(diDetail->DevicePath, 
			0, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (hDevice != INVALID_HANDLE_VALUE)
		{
			// Get the serial number string for the device, and check for
			// a match to the serial number we have on record for the WinUSB
			// device.  The Pinscape Pico firmware generates a serial number
			// that's unique to the physical Pico, based on the Pico's flash
			// memory chip's unique read-only hardware ID.  The serial number
			// can be obtained from any of the exposed USB interfaces, so we
			// can use it to determine which interfaces we see on the Windows
			// side are associated with the same physical Pico.  I'd prefer to
			// make the association based on a Windows unique device ID for
			// physical device, and there might be a way to do that, but if
			// there is, I haven't figured it out yet.  Windows seems pretty
			// determined to give you a virtual view that represents each
			// interface as a separate logical device.
			WCHAR serialNum[128]{ 0 };
			if (HidD_GetSerialNumberString(hDevice, serialNum, sizeof(serialNum))
				&& wcscmp(serialNum, this->serialNum.c_str()) == 0)
			{
				// it's a match - add it to the result list
				hidList.push_back(diDetail->DevicePath);
			}

			// done with the device handle
			CloseHandle(hDevice);
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

	// done with the device list handle
	SetupDiDestroyDeviceInfoList(hdi);

	// return the result code
	return hresult;
}

// Identify the CDC port (virtual USB COM port) associated with this
// vendor interface.
bool VendorInterface::GetCDCPort(TSTRING &name)
{
	// construct a path object for the interface, and ask it to find the COM port
	return VendorInterfaceDesc(VendorInterfaceDesc::private_ctor_key_t(), path.c_str(), path.size(), deviceInstanceId.c_str()).GetCDCPort(name);
}

// Get the feedback controller HID associated with the device
HRESULT VendorInterface::OpenFeedbackControllerInterface(std::unique_ptr<FeedbackControllerInterface> &ifc)
{
	// enumerate associated HIDs
	std::list<WSTRING> hids;
	HRESULT hr = EnumerateAssociatedHIDs(hids);
	if (!SUCCEEDED(hr))
		return hr;

	// Scan for the feedback controller HID
	for (auto &hid : hids)
	{
		// open the HID
		HANDLE h = CreateFileW(hid.c_str(), 
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (h != INVALID_HANDLE_VALUE)
		{
			// presume this isn't the one we're looking for
			bool matched = false;

			// get the preparsed data
			PHIDP_PREPARSED_DATA ppd;
			if (HidD_GetPreparsedData(h, &ppd))
			{
				// get the device capabilities
				HIDP_CAPS caps;
				if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
				{
					// The Feedback Controller uses usage page 0x06 (Generic Device),
					// usage 0x00 (undefined).  Note that there's no need to check
					// the special string label that we use to positively identify
					// the interface in a system-wide HID search, because we're
					// already confining our search to HIDs associated with a physical
					// device that we already know to be a Pinscape Pico unit by way 
					// of the Vendor Interface we're starting with.  A Pinscape Pico
					// only exposes one 0x06/0x00 HID device, so the usage IDs are
					// enough to identify the Feedback Controller interface with
					// certainty.
					if (caps.UsagePage == 0x06 && caps.Usage == 0x00)
					{
						// this is the one
						matched = true;
					}
				}

				// done with the preparsed data
				HidD_FreePreparsedData(ppd);
			}

			// if this is the correct interface, pass it back as a new
			// FeedbackControllerInterface object
			if (matched)
			{
				ifc.reset(new FeedbackControllerInterface(h, hid.c_str()));
				return S_OK;
			}

			// done with the handle
			CloseHandle(h);
		}
	}

	// not found
	return E_FAIL;
}

// Get the USB device descriptor
HRESULT VendorInterface::GetDeviceDescriptor(USB_DEVICE_DESCRIPTOR &desc)
{
	// retrieve the descriptor
	ULONG len = 0;
	if (!WinUsb_GetDescriptor(winusbHandle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
		reinterpret_cast<BYTE*>(&desc), sizeof(desc), &len))
		return HRESULT_FROM_WIN32(GetLastError());

	// success
	return S_OK;
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

// --------------------------------------------------------------------------
//
// Shared device object
//

VendorInterface::Shared::Shared(VendorInterface *device, HWND hwndNotify)
	: device(device), hwndNotify(hwndNotify)
{
	// if a notification window was provided, and we have a valid device
	// handle, register the window to receive notifications
	RegisterNotify();
}

VendorInterface::Shared::~Shared()
{
	// done with the mutex
	CloseHandle(mutex);

	// unregister the notification handle
	UnregisterNotify();
}

void VendorInterface::Shared::Set(VendorInterface *device, HWND hwndNotify)
{
	// unregister any prior notification handle
	UnregisterNotify();

	// set the new device and registration
	this->device.reset(device);
	this->hwndNotify = hwndNotify;
	RegisterNotify();
}

void VendorInterface::Shared::RegisterNotify()
{
	if (hwndNotify != NULL && device != nullptr
		&& device->GetDeviceHandle() != NULL && device->GetDeviceHandle() != INVALID_HANDLE_VALUE)
	{
		DEV_BROADCAST_HANDLE dbh{ sizeof(dbh), DBT_DEVTYP_HANDLE, 0, device->GetDeviceHandle() };
		deviceNotifyHandle = RegisterDeviceNotification(hwndNotify, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
	}
}

void VendorInterface::Shared::UnregisterNotify()
{
	if (deviceNotifyHandle != NULL)
	{
		UnregisterDeviceNotification(deviceNotifyHandle);
		deviceNotifyHandle = NULL;
	}
}

