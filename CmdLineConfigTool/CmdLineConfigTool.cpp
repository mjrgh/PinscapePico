// Pinscape Pico Config Tool (prototype)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a command-line interface to Pinscape Pico configuration
// functions.

#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include <algorithm>
#include <regex>
#include <Windows.h>
#include <conio.h>
#include <comdef.h>
#include <Dbt.h>
#include "../WinAPI/FeedbackControllerInterface.h"
#include "../WinAPI/PinscapeVendorInterface.h"
#include "../WinAPI/RP2BootLoaderInterface.h"
#include "../WinAPI/Utilities.h"
#include "../WinAPI/BytePackingUtils.h"
#include "../Firmware/JSON.h"
#include "../GUIConfigTool/Dialog.h"
#include "VersionNumber.h"

// link the Pinscape Pico device API library
#pragma comment(lib, "PinscapePicoAPI")

// bring the main Pinscape API classes into our global namespace
using VendorInterfaceDesc = PinscapePico::VendorInterfaceDesc;
using VendorInterface = PinscapePico::VendorInterface;
using FeedbackControllerInterface = PinscapePico::FeedbackControllerInterface;
using RP2BootDevice = PinscapePico::RP2BootDevice;

// COM initialized
static bool comInited = false;


// --------------------------------------------------------------------------
//
// Error handlers.  These just print an error message (with various types
// of error code information) and exit.
//
static void __declspec(noreturn) ErrorExit(const char *msg)
{
	printf("%s\n", msg);
	exit(1);
}

static void __declspec(noreturn) ErrorExit(const char *msg, HRESULT hr)
{
	if (hr == S_OK)
		printf("%s\n", msg);
	else
	{
		_com_error ce(hr);
		printf("%s (HRESULT %08lx: %" _TSFMT ")\n", msg, hr, ce.ErrorMessage());
	}
	exit(1);
}

static void __declspec(noreturn) ErrorStatExit(const char *msg, int vendorIfcStatusCode)
{
	printf("%s: %s (status code %d)\n", msg, VendorInterface::ErrorText(vendorIfcStatusCode), vendorIfcStatusCode);
	exit(1);
}

// List devices
static void ShowDeviceList()
{
	// get the device list
	HRESULT hr;
	std::list<VendorInterfaceDesc> paths;
	hr = VendorInterface::EnumerateDevices(paths);
	if (!SUCCEEDED(hr))
		ErrorExit("Error enumerating device paths", hr);

	// list the devices
	int nErrors = 0;
	printf("Unit   Hardware ID        Name\n");
	for (auto &path : paths)
	{
		std::unique_ptr<VendorInterface> dev;
		PinscapePico::DeviceID id;
		if (SUCCEEDED(path.Open(dev)) && SUCCEEDED(dev->QueryID(id)))
		{
			printf("%4d   %s   %s\n", id.unitNum, id.hwid.ToString().c_str(), id.unitName.c_str());
		}
		else
		{
			_tprintf(_T("[Error retrieving Hardware ID for path %s]\n"), path.Name());
			++nErrors;
		}
	}

	if (nErrors > 0)
	{
		printf("\nNote: errors might be due to access conflicts with other programs.  The\n"
			"Pinscape Pico USB configuration/control interface requires exclusive access,\n"
			"so only one program can access it at a time.  Try closing any other Pinscape\n"
			"Pico configuration tools or similar programs that are current running, and\n"
			"try again.\n");
	}
}

// Exit with an error message plus a listing of available devices.
// This is handy when the error relates to the user's device selection,
// or lack thereof.
static void __declspec(noreturn) ErrorExitWithDeviceList(std::list<VendorInterfaceDesc> &paths, const char *msg)
{
	printf("%s\n", msg);
	printf("Available devices:\n"
		"  Unit   Hardware ID        Name\n");

	int n = 0;
	for (auto &p : paths)
	{
		++n;
		std::unique_ptr<VendorInterface> dev;
		PinscapePico::DeviceID id;
		if (SUCCEEDED(p.Open(dev)) && SUCCEEDED(dev->QueryID(id)))
			printf("  %4d   %s   %s\n", id.unitNum, id.hwid.ToString().c_str(), id.unitName.c_str());
		else
			_tprintf(_T("  [Error retrieving Hardware ID for path %s]\n"), p.Name());
	}

	if (n != 0)
		printf("\nYou can use the unit number, name, or hardware ID in the --id option.\n");
	else
		printf("\nNo Pinscape units were detected.\n");

	exit(1);
}

// --------------------------------------------------------------------------
//
// Select a device by hardware ID (Pico serial number) or Pinscape Unit Number.
// If the ID is empty, and there's exactly one device in the list, we'll select
// that device, otherwise we'll display a message explaining that a device must
// be selected explicitly.
//
static VendorInterfaceDesc *SelectDevice(
	std::list<VendorInterfaceDesc> &paths, const std::string &id, bool exitOnError)
{
	auto OnError = [exitOnError](const char *msg, HRESULT hr)
	{
		if (exitOnError)
			ErrorExit(msg, hr);
		return nullptr;
	};

	// Enumerate Pinscape Pico devices
	HRESULT hr;
	hr = VendorInterface::EnumerateDevices(paths);
	if (!SUCCEEDED(hr))
		return OnError("Error enumerating device paths", hr);

	// Find the target device
	if (paths.size() == 0)
	{
		// no devices found - there's nothing to match
		return OnError("No Pinscape Pico devices found", S_OK);
	}
	else if (id.size() != 0)
	{
		// device specified - scan for a matching ID
		struct PartialMatch
		{
			PartialMatch(VendorInterfaceDesc *desc, const PinscapePico::DeviceID &id)
				: desc(desc), id(id) { }

			VendorInterfaceDesc *desc;
			PinscapePico::DeviceID id;
		};
		std::list<PartialMatch> partialMatches;
		for (auto &p : paths)
		{
			std::unique_ptr<VendorInterface> dev;
			PinscapePico::DeviceID devId;
			if (SUCCEEDED(p.Open(dev))
				&& SUCCEEDED(dev->QueryID(devId)))
			{
				// check for an exact match
				auto hwid = devId.hwid.ToString();
				if (_stricmp(hwid.c_str(), id.c_str()) == 0
					|| devId.unitNum == atoi(id.c_str())
					|| _stricmp(devId.unitName.c_str(), id.c_str()) == 0)
				{
					// this is the one
					return &p;
				}

				// If the ID string is at least four characters long,
				// check for a partial match against the hardware ID,
				// matching any substring.  This allows matching the
				// hardware ID a few leading or trailing digits rather
				// than having to type in the whole thing.  A few
				// digits is usually enough to pick out a single unit.
				if (StrStrIA(hwid.c_str(), id.c_str()) != nullptr)
					partialMatches.emplace_back(&p, devId);
			}
		}

		// check for partial matches
		if (partialMatches.size() == 1)
		{
			// matched exactly one unit - select it
			return partialMatches.front().desc;
		}
		else if (partialMatches.size() > 1 && partialMatches.size() < paths.size())
		{
			// We matched multiple units, but not all of them, so
			// this does narrow it down somewhat.
			if (exitOnError)
			{
				printf("The specified ID matches multiples hardware IDs:\n"
					"  Unit   Hardware ID        Name\n");

				for (const auto &p : partialMatches)
					printf("  %4d   %s   %s\n", p.id.unitNum, p.id.hwid.ToString().c_str(), p.id.unitName.c_str());

				printf("\n"
					"Please specify a unique hardware ID, or one of the other identifiers.\n");

				exit(1);
			}
		}

		// no match found
		if (exitOnError)
			ErrorExitWithDeviceList(paths, "The specified Pinscape device wasn't found.");
	}
	else if (paths.size() == 1)
	{
		// no device was specified in the arguments, and there's exactly
		// one in the system, so that's the one to use
		return &paths.front();
	}
	else
	{
		// no target device was specified in the arguments, and there's
		// more than one device attached - they must select one explicitly
		// via the --id command argument
		if (exitOnError)
			ErrorExitWithDeviceList(paths, "Multiple Pinscape Pico devices found; "
				"please select one by specifying --id as the first argument.\n");
	}

	// if we got this far, we failed to find a matching device, and the
	// caller didn't want to exit, so just return a null device
	return nullptr;
}

// --------------------------------------------------------------------------
//
// Wait for a device to reconnect.  If the device was successfully
// found within the time limit, fills in 'device' with the the device
// pointer and returns true.  If the timeout expires before the device
// can be reopened, returns false.  Note that any errors that occur
// trying to reconnect are silently ignored, so the attempts will be
// repeated until the timeout expires.
//
static bool WaitForReconnect(std::unique_ptr<VendorInterface> &device, const std::string &deviceID, int timeout_ms)
{
	// release the original device
	device.release();

	// note the starting time
	UINT64 t0 = GetTickCount64();

	// loop until the device appears or the timeout expires
	for (;;)
	{
		// wait briefly
		Sleep(250);

		// enumerate devices and select by hardware ID
		std::list<VendorInterfaceDesc> paths;
		VendorInterfaceDesc *selectedPath = SelectDevice(paths, deviceID, false);
		if (selectedPath != nullptr)
		{
			// connect
			HRESULT hr = selectedPath->Open(device);
			if (SUCCEEDED(hr))
			{
				// Re-send the wall-clock time.  A caller who's asking for
				// a reconnect has presumably intentionally caused a Pico
				// reset, such as for a firmware or config file update, so
				// the Pico's internal clock has probably just been reset
				// to zero and thus needs a wall clock time update.
				device->PutWallClockTime();
	
				// success
				return true;
			}
		}

		// abort on timeout
		if (GetTickCount64() - t0 > timeout_ms)
			return false;
	}
}

// --------------------------------------------------------------------------
//
// Update the firmware on a device.  The device must be connected and must
// be running a version of the firmware already.  We sent a command to the
// device (through the Pinscape vendor interface) to reboot into RP2 Boot
// Loader mode, then we watch the list of available drive letters until we
// see a new drive letter appear with the signature files of an RP2 Boot
// Loader MSC (USB Mass Storage Class == virtual disk drive).  We then
// copy the firmware file onto the drive.  The RP2 Boot Loader automatically
// reboots the Pico into the new firmware program after the file copy is
// completed, so we just wait until the device reconnects in Pinscape mode.
// We can be sure we're talking to the same device because the Pico Hardware
// ID is immutable, so the firmware update and reboots won't affect that.
//
static void UpdateFirmware(std::unique_ptr<VendorInterface> &device, 
	const std::string &deviceID, const TCHAR *firmwarePath)
{
	// Enumerate RP2 Boot drives before the reboot
	auto drives0 = RP2BootDevice::EnumerateRP2BootDrives();

	// reboot it into the boot loader
	int stat = device->EnterBootLoader();
	if (stat != PinscapeResponse::OK)
		ErrorStatExit("ENTER BOOT LOADER command failed", stat);

	// the device object is no longer valid
	device.reset();

	// Wait a bit for the device to reappear as an RP2 Boot device
	RP2BootDevice::RP2BootDeviceList drives1;
	auto t0 = GetTickCount64();
	for (;;)
	{
		// wait a short time
		Sleep(250);

		// look for new devices (new since the pre-reboot enumeration)
		drives1 = RP2BootDevice::EnumerateNewRP2BootDrives(drives0);

		// if we found exactly one, stop searching
		if (drives1.size() == 1)
			break;

		// if we found more than one, it's an error, since we don't know which
		// one corresponds to the Pinscape device
		if (drives1.size() > 1)
			ErrorExit("Multiple mew RP2 Boot devices found - can't determine which one is the Pinscape device");

		// check for timeout
		if (GetTickCount64() - t0 > 3000)
			ErrorExit("Timeout waiting for RP2 Boot device to appear");
	}

	// Progress callback object.  In keeping with our character-mode
	// motif, display a simple text progress bar.
	class Progress : public RP2BootDevice::IProgressCallback
	{
	public:
		virtual void ProgressInit(const TCHAR *from, const TCHAR *to, uint32_t fileSizeBytes) override
		{
			// start the bar with 64 blank dots
			size = static_cast<float>(fileSizeBytes);
			printf("[................................................................]\r[");
		}

		virtual void ProgressUpdate(uint32_t bytesCopied)
		{
			// on each 1/64th of the total size, update the bar
			int newBars = static_cast<int>(roundf(static_cast<float>(bytesCopied) * 64.0f / size));
			for (; bars < newBars ; ++bars)
				printf("*");
		}

		float size = 0.0f;
		int bars = 0;
	};
	Progress progress;

	// got it - update its firmware
	printf("RP2 device found - installing firmware\n");
	HRESULT hr = RP2BootDevice::InstallFirmware(firmwarePath, drives1.front().path.c_str(), &progress);
	printf("\n");

	if (!SUCCEEDED(hr))
		ErrorExit("Error installing firmware", hr);

	// The RP2 boot loader should automatically reboot the Pico when the
	// install completes, so the RP2 virtual drive will disappear, and
	// the Pinscape device will take its place.  Wait for the Pinscape
	// device to reappear, matching based on the permanent hardware ID.
	if (!WaitForReconnect(device, deviceID, 3000))
		ErrorExit("Timeout waiting for the Pinscape device to reconnect");
}

// --------------------------------------------------------------------------
//
// Send a JSON configuration file to the device.  This invokes the Pinscape
// vendor interface's Put Config command to send the update.
// 
// The new configuration file won't take effect until the Pico is rebooted.
// This routine doesn't do the reboot - the caller must do that separately
// if the new config is meant to go into effect immediately.
//
// fileID is one of the PinscapeRequest::CONFIG_FILE_xxx constants, specifying
// which configuration file we're updating (normal, Safe Mode).
//
static void PutConfig(std::unique_ptr<VendorInterface> &device, 
	const std::string &deviceID, const char *configFileName, uint8_t fileID)
{
	// open the config file
	FILE *fp = nullptr;
	if (fopen_s(&fp, configFileName, "r") != 0 || fp == nullptr)
		ErrorExit("Can't open config file");

	// read it
	std::string txt;
	for (;;)
	{
		// read the next chunk
		char buf[1024];
		size_t len = fread(buf, 1, sizeof(buf), fp);
		if (len == 0)
			break;

		// append it to the buffer
		txt.append(buf, len);
	}

	// done with the file
	fclose(fp);

	// parse the JSON to ensure that it's well-formed
	JSONParser json;
	json.Parse(txt.c_str(), txt.size());
	if (json.errors.size() != 0)
	{
		// report the JSON errors
		printf("Error: configuration text contains JSON parsing errors\n\n");
		for (auto &e : json.errors)
		{
			// find the line number of the error
			int lineNum = 1, colNum = 0;
			const char *lineStart = txt.c_str();
			const char *lineEnd = nullptr;
			const char *tokPtr = nullptr;
			const char *endp = txt.c_str() + txt.size();
			for (const char *p = txt.c_str() ; p < endp ; ++p)
			{
				// count line starts
				if (*p == '\n')
				{
					++lineNum;
					lineStart = p + 1;
				}

				// flag the token location
				if (p == e.src)
				{
					// note the token location
					tokPtr = p;
					colNum = static_cast<int>(p - lineStart);

					// scan to the end of the line
					for (++p ; p < endp && *p != '\n' ; ++p);
					lineEnd = p;

					// we've identified the token context
					break;
				}
			}

			// show the error
			printf("Line %d, col %d: %s\n", lineNum, colNum, e.message.c_str());

			// show context if available
			if (tokPtr != nullptr)
			{
				printf("|%.*s\n|", static_cast<int>(lineEnd - lineStart), lineStart);
				for (const char *p = lineStart ; p < endp && p != tokPtr ; ++p)
					printf("=");
				printf("^\n\n");
			}
		}

		// abort 
		exit(1);
	}
	else
	{
		// the config file looks valid - send it to the device
		int stat = device->PutConfig(txt.c_str(), static_cast<uint32_t>(txt.size()), fileID);
		if (stat == PinscapeResponse::OK)
			printf("Configuration file update succeeded\n");
		else
			ErrorStatExit("Error updating configuration data", stat);
	}
}

// --------------------------------------------------------------------------
//
// Retrieve the config file
//
static void GetConfig(VendorInterface *device, FILE *fp)
{
	// retrieve the configuration
	std::vector<char> buf;
	int stat = device->GetConfig(buf, PinscapePico::VendorRequest::CONFIG_FILE_MAIN);
	if (stat != PinscapeResponse::OK)
		ErrorStatExit("Error retrieving configuration data", stat);

	// write it to the file
	if (fwrite(buf.data(), 1, buf.size(), fp) != buf.size())
		ErrorExit("Error writing configuration output file");
}

// --------------------------------------------------------------------------
//
// Show the log currently stored in the Pico's internal memory, either
// interactively, or just dumped to stdout.  In interactive mode, we
// continue monitoring the log for new entries, and add them to the
// interactive display as they arrive.  In plain mode, we just send out
// the log text that's currently stored.  Plain mode is suitable for
// capturing the log data to a file.
//
static void ShowLog(std::unique_ptr<VendorInterface> &upDevice, const std::string &deviceID, bool isConsole, bool plainMode)
{
	// check the mode
	if (plainMode || !isConsole)
	{
		// Plain non-interactive mode.  Show the log on stdout,
		// stripping ANSI sequences if stdout is redirected to
		// a file.  Only show as much of the log as is available
		// on the first call, to avoid looping indefinitely if
		// our own vendor interface calls are adding log messages.
		uint32_t logRemaining = 0;
		for (int iter = 0 ; ; ++iter)
		{
			// get the next batch of log data
			std::vector<BYTE> inBuf;
			PinscapeResponse resp;
			int stat = upDevice->SendRequest(PinscapeRequest::CMD_QUERY_LOG, resp, nullptr, 0, &inBuf);
			if (stat == PinscapeResponse::OK)
			{
				// if this is the first request, note the total log size available
				if (iter == 0)
					logRemaining = resp.args.log.avail;

				// deduct this chunk from the remaining available
				logRemaining -= min(logRemaining, static_cast<int>(inBuf.size()));

				// If we're not logging to the console, remove ANSI sequences,
				// so that the file will be intelligible in a plain text editor.
				size_t effectiveSize = inBuf.size();
				if (!isConsole)
				{
					// delete escapes, copying the result to the buffer in-place
					// (which is possible because we're only deleting characters:
					// the algorithm works left-to-right, and characters can only
					// stay in place or move left)
					BYTE *src = inBuf.data();
					BYTE *dst = src;
					for (BYTE *endp = src + inBuf.size() ; src < endp ; )
					{
						// check for ANSI sequences
						if (*src == 27)
						{
							// skip to the next alphabetic character
							for (++src ; src < endp && !isalpha(*src) ; ++src);
							if (src == endp)
								break;

							// skip the final ANSI character and continue
							++src;
						}
						else
						{
							// copy this character
							*dst++ = *src++;
						}
					}

					// set the new effective size based
					effectiveSize = dst - inBuf.data();
				}

				// write this chunk to the output
				fwrite(inBuf.data(), effectiveSize, 1, stdout);

				// stop if we're out of data
				if (logRemaining == 0)
					break;
			}
			else if (stat == PinscapeResponse::ERR_EOF)
			{
				// if we haven't shown anything yet, note that the log is empty
				if (isConsole && iter == 0)
					printf("[The log is currently empty]\n");

				// done
				break;
			}
			else
			{
				// error - log it and exit
				char msg[128];
				sprintf_s(msg, "Error retrieving log data (error code %d)", stat);
				ErrorExit(msg);
			}
		}
	}
	else
	{
		//
		// Interactive mode - show the log in the console, with interactive
		// scrolling and ongoing monitoring for new log output
		//

		// Set up a window to handle device removal notifications, in case
		// the Pico is unplugged or reset during the session.  We can't do
		// this through the console window, since we don't have access to
		// its message loop.  Instead, we have to set up our own hidden
		// window just to receive notifications.  This has to be on a
		// separate thread, since the foreground thread will be busy
		// handling console I/O.
		struct ThreadCtx
		{
			ThreadCtx(VendorInterface *device, const std::string &deviceID) : 
				device(device, NULL), deviceID(deviceID) { }

			~ThreadCtx() 
			{ 
				SendMessage(hwnd, WM_CLOSE, 0, 0);
				WaitForSingleObject(hThread, INFINITE);
				CloseHandle(hThread); 
			}

			// device object, shared with the main thread
			VendorInterface::Shared device;

			// device ID
			const std::string &deviceID;

			DWORD threadId = 0;
			HANDLE hThread = NULL;

			HWND hwnd = NULL;
			HANDLE hDeviceNotifier = NULL;

			static DWORD CALLBACK Main(void *param)
			{
				ThreadCtx *self = reinterpret_cast<ThreadCtx*>(param);
				const TCHAR *className = _T("PinscapePico.CmdLineConfigTool.Notify");
				HINSTANCE hInstance = GetModuleHandle(NULL);
				WNDCLASS wc{
					CS_ENABLE, WndProc, 0, sizeof(void*), hInstance, 
					NULL, NULL, NULL, NULL, className
				};
				RegisterClass(&wc);
				self->hwnd = CreateWindow(className, _T("PinscapePico Notifier Window"), WS_OVERLAPPED, 
					CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, self);

				// register for device notifications
				self->RegisterNotify();

				// process messages
				MSG msg;
				while (GetMessage(&msg, NULL, 0, 0) != 0)
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}

				// unregister the device notification
				self->UnregisterNotify();

				// done (return value required by prototype but not used)
				return 0;
			}

			void RegisterNotify()
			{
				// register for device removal notification
				DEV_BROADCAST_HANDLE dbh{ sizeof(dbh), DBT_DEVTYP_HANDLE, 0, device.device->GetDeviceHandle() };
				hDeviceNotifier = RegisterDeviceNotification(hwnd, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
			}

			void UnregisterNotify()
			{
				if (hDeviceNotifier != NULL)
				{
					UnregisterDeviceNotification(hDeviceNotifier);
					hDeviceNotifier = NULL;
				}
			}

			static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
			{
				auto *self = reinterpret_cast<ThreadCtx*>(GetWindowLongPtr(hwnd, 0));
				switch (msg)
				{
				case WM_CREATE:
					// stash our 'self' object from the create parameters in the window long_ptr at index 0
					SetWindowLongPtr(hwnd, 0, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams));
					break;

				case WM_NCDESTROY:
					// window destroyed - exit the message loop
					PostQuitMessage(0);
					break;

				case WM_DEVICECHANGE:
					// device change - check if our device was disconnected
					if (wParam == DBT_DEVICEREMOVECOMPLETE
						&& reinterpret_cast<DEV_BROADCAST_HDR*>(lParam)->dbch_devicetype == DBT_DEVTYP_HANDLE
						&& reinterpret_cast<DEV_BROADCAST_HANDLE*>(lParam)->dbch_handle == self->device.device->GetDeviceHandle())
					{
						// close the device handle
						if (VendorInterface::Shared::Locker l(&self->device); l.locked)
							self->device.device->CloseDeviceHandle();

						// unregister the notifier
						self->UnregisterNotify();
					}
					else if (wParam == DBT_DEVNODES_CHANGED)
					{
						// Device inserted or removed.  This message doesn't come
						// with any details, so it might or might not mean that our
						// device was reconnected.  The only way to find out is to
						// try reconnecting.
						if (VendorInterface::Shared::Locker l(&self->device); l.locked)
						{
							if (!self->device.device->IsDeviceHandleValid())
							{
								std::unique_ptr<VendorInterface> up;
								if (WaitForReconnect(up, self->deviceID, 0))
								{
									// we're reconnected - replace the device in the Shared
									self->device.device.reset(up.release());

									// re-register for notifications
									self->RegisterNotify();
								}
							}
						}
					}
					break;
				}

				return DefWindowProc(hwnd, msg, wParam, lParam);
			}

			void Launch() { hThread = CreateThread(NULL, 0, Main, this, 0, &threadId); }
		};

		// transfer ownership of the device to the thread context, and launch the thread
		ThreadCtx threadCtx(upDevice.release(), deviceID);
		threadCtx.Launch();

		// main log display loop
#define DEFCOLOR "\033[0m\033[37;40m"
#define INVCOLOR "\033[0m\033[30;107m"
		int nRows = 0, nCols = 0, topRow = 0;
		bool newTextSinceError = false;
		std::vector<std::string> text;
		bool redrawByUser = false;
		bool isMore = false;
		std::string locText;
		bool startOfLine = true;
		std::string statusMessage;
		bool newStatusMessage = false;
		for (bool quitLog = false ; !quitLog ; )
		{
			// retrieve new log text since last tme
			bool newTextInView = false;
			std::vector<BYTE> inBuf;
			PinscapeResponse resp;
			int stat = PinscapeResponse::ERR_FAILED;
			bool connected = false;
			if (VendorInterface::Shared::Locker l(&threadCtx.device); l.locked)
			{
				connected = threadCtx.device.device->IsDeviceHandleValid();
				if (connected)
					stat = threadCtx.device.device->SendRequest(PinscapeRequest::CMD_QUERY_LOG, resp, nullptr, 0, &inBuf);
			}
			if (!connected)
			{
				// do nothing while it's disconnected
			}
			else if (stat == PinscapeResponse::OK)
			{
				// break into lines and store in our buffer
				for (const BYTE *src = inBuf.data(), *endp = src + inBuf.size() ; src < endp ; )
				{
					// scan for the next newline
					const BYTE *p = src, *lineStart = p;
					for (; p < endp && *p != '\n' ; ++p) ;

					// note if we found a newline
					bool foundNewline = (p < endp);

					// advance to the next line
					src = p + 1;

					// Trim the \r\n
					if (p > src && *(p-1) == '\r') --p;

					// Append the line to our buffer.  If we left off at the start of
					// a new line, add it as a whole new line, otherwise append it to
					// the existing last line.
					std::string_view thisLine(reinterpret_cast<const char*>(lineStart), p - lineStart);
					if (startOfLine || text.size() == 0)
						text.emplace_back(thisLine);
					else
						text.back().append(thisLine);

					// note if we finished at a newline
					startOfLine = foundNewline;
				}

				// there's new text since any prior error
				newTextSinceError = true;

				// check if the new text is in view
				newTextInView = static_cast<int>(text.size()) <= topRow + nRows;
			}
			else if (stat == PinscapeResponse::ERR_EOF)
			{
				// that's all for now; process user input if available
			}
			else if (newTextSinceError)
			{
				// add an error message to the buffer
				char errmsg[128];
				sprintf_s(errmsg, "\033[41m\033[37;1mError reading log (error code %d)", stat);
				text.emplace_back(errmsg);

				// count this as new text to draw if it's in view
				newTextInView = static_cast<int>(text.size()) <= topRow + nRows;

				// we haven't received any new text since this error yet, obviously;
				// this flag prevents adding an infinite number of error messages if
				// the error condition is persistent, in which case we'll keep getting
				// another error every time we ask for more input
				newTextSinceError = false;
			}

			// get the current console dimensions
			CONSOLE_SCREEN_BUFFER_INFO csbi;
			GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
			int newCols = csbi.srWindow.Right - csbi.srWindow.Left;
			int newRows = csbi.srWindow.Bottom - csbi.srWindow.Top;
			int nTextRows = static_cast<int>(text.size());
			int maxTopRow = max(0, nTextRows - newRows);
			std::string newLocText;
			if (topRow == 0 && maxTopRow == 0)
				newLocText = "[ALL]";
			else if (topRow == 0)
				newLocText = "[TOP]";
			else if (topRow >= maxTopRow)
				newLocText = "[END]";
			else
			{
				char buf[32];
				sprintf_s(buf, "[%d%%]", static_cast<int>(roundf(static_cast<float>(topRow)/static_cast<float>(maxTopRow) * 100.f)));
				newLocText = buf;
			}

			// redraw the screen if the size changed, or we received new text,
			// or the user took action that requires a redraw
			if (newCols != nCols || newRows != nRows || newTextInView || redrawByUser || newLocText != locText || newStatusMessage)
			{
				// remember the new size
				nRows = newRows;
				nCols = newCols;

				// clear the screen, go to top left
				printf(DEFCOLOR "\033[2J\033[1; 1H");

				// draw the text
				for (int screenRow = 0, textRow = topRow ; screenRow + 1 <= nRows && textRow < nTextRows ; ++screenRow, ++textRow)
				{
					// start the line with the default color
					printf(DEFCOLOR "\033[%d;1H", screenRow + 1);

					// draw up to the screen width, so that we don't wrap lines
					int textcol = 0;
					const char *textp = text[textRow].c_str(), *p = textp, *startp = p;
					for (; *p != 0 && textcol <= nCols ; )
					{
						// check for escapes
						if (*p == 27)
						{
							// print the section up to here
							printf("%.*s", static_cast<int>(p - startp), startp);

							// scan the escape sequence - it runs until the first alphabetic character
							for (startp = p ; *p != 0 && !isalpha(*p) ; ++p) ;
							if (*p != 0)
								++p;

							// Replace ESC[0m (set default colors/attributes) with our explicit
							// base color sequence.  This makes our color rendition more consistent 
							// if the Windows terminal is set to inverted colors or some other
							// scheme, by overriding defaults and using our explicit color scheme
							// instead.  The log colors are essentially chosen to look right with
							// a basic white-text-on-black-background color scheme.
							if (p - startp == 4 && memcmp(startp, "\033[0m", 4) == 0)
								printf(DEFCOLOR);
							else
								printf("%.*s", static_cast<int>(p - startp), startp);

							// start the next section
							startp = p;
						}
						else
						{
							// regular character - count as a column
							++textcol;
							++p;
						}
					}

					// display the last section
					printf("%.*s", static_cast<int>(p - startp), startp);
				}

				// draw the instruction line, or the special status message if active
				if (statusMessage.size() != 0)
				{
					printf(INVCOLOR "\033[%d;1H\033[0K%s", nRows + 1, statusMessage.c_str());
				}
				else
				{
					// no status message - draw the regular instruction line
					printf(INVCOLOR "\033[%d;1H\033[0K%-5s | Exit=[Esc], Save=[Shift+S]",
						nRows + 1, newLocText.c_str());
				}

				// we've satisfied any pending user-initiated redraw and status message update
				locText = newLocText;
				redrawByUser = false;
				newStatusMessage = false;
			}

			// check for user input
			if (_kbhit())
			{
				// read the next key input
				int c = _getch();
				int cext = (c == 0 || c == 0xE0 ? _getch() : -1);

				// clear the status message on any keyboard input
				if (statusMessage.size() != 0)
				{
					// clear the message
					statusMessage = "";
					newStatusMessage = true;

					// Take space and escape to just mean "clear the message", and suppress
					// their ordinary meanings (next page/exit) - the user is probably
					// thinking that the status message is a mode that needs to be cleared
					// and doesn't want to exit the whole program or scroll down.  We'll
					// fulfill that expectation by indeed making it a mode, but just for
					// space and escape, which are traditionally used as mode-exit keys.
					// Other keys aren't traditional mode-exiters, so if the user pressed
					// something else, they're probably expecting it to have its normal
					// meaning.
					if (c == ' ' || c == 27)
						c = -1;
				}

				// check the character
				if (c == 'S')
				{
					// Shift+S - save the current buffer contents to a file

					// move to start of line, erase line, show prompt
					printf(INVCOLOR "\033[1G\033[0K"
						"[Select=Ctrl+S, Cancel=Esc] Save to file: ");

					// get the cursor column
					CONSOLE_SCREEN_BUFFER_INFO csbi;
					GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
					int col = csbi.dwCursorPosition.X + 1;
					int startCol = col;

					// read input
					bool accept = false;
					char buf[MAX_PATH] = "";
					char *bufp = buf;
					for (;;)
					{
						int c = _getch();
						if (c == 19)
						{
							// browse mode - show a standard file dialog; COM initialization required
							if (!comInited)
							{
								if (SUCCEEDED(CoInitialize(NULL)))
									comInited = true;
							}

							// show the dialog
							GetFileNameDlg ofn(_T("Save log text"),
								OFN_ENABLESIZING | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
								_T("Text/Log files\0*.txt;*.log\0All Files\0*.*"), _T("log"));
							if (ofn.Save(GetActiveWindow()))
							{
								accept = true;
								sprintf_s(buf, "%" _TSFMT, ofn.GetFilename());
								bufp = buf + strlen(buf);
							}

							// stop here in either case - if they canceled the dialog, cancel the prompt
							break;
						}
						else if (c == 13 || c == 10)
						{
							// enter - accept the current file name
							accept = true;
							break;
						}
						else if (c == 21)
						{
							// Ctrl+U = delete whole line
							printf("\033[%dD\033[0K", col - startCol);
							col = startCol;
							bufp = buf;
							*bufp = 0;
						}
						else if (c == 8)
						{
						backspace:
							// backspace
							if (bufp > buf)
							{
								--bufp;
								--col;
								printf("\033[1D\033[0K");
							}
						}
						else if (c == 27)
						{
							// escape - abort
							break;
						}
						else if (c >= 32 && c <= 126)
						{
							// ordinary character - add to buffer
							if (bufp + 2 < &buf[_countof(buf)] && col + 1 < nCols)
							{
								printf("%c", c);
								++col;
								*bufp++ = c;
							}
						}
						else if (c == 0x00 || c == 0xE0)
						{
							// extended character - get the second byte
							int c = _getch();
							switch (c)
							{
							case 0x4B:		// left arrow
							case 0x53:		// delete
								// treat as backspace
								goto backspace;

							default:
								// ignore anything else
								break;
							}
						}
					}

					// save the file if accepted
					if (accept)
					{
						// try opening the file
						FILE *fp = nullptr;
						*bufp = 0;
						if (fopen_s(&fp, buf, "w") == 0 && fp != nullptr)
						{
							// write the buffered text
							for (auto &t : text)
							{
								// make a local copy of the line
								std::string l = t;

								// strip escape codes
								enum class State
								{
									Plain,    // ordinary plain text
									Esc,      // saw escape character, might be Esc [ sequence
									Seq,      // in an escape sequence
								};
								State state = State::Plain;
								bool inEscape = false;
								char *dst = l.data();
								for (const char *src = dst ; *src != 0 ; )
								{
									char c = *src++;
									switch (state)
									{
									case State::Plain:
										// check for escapes, copy everything else
										if (c == 27)
											state = State::Esc;
										else
											*dst++ = c;
										break;

									case State::Esc:
										// check for [, copy everything else
										if (c == '[')
											state = State::Seq;
										else
											*dst++ = c, state = State::Plain;
										break;

									case State::Seq:
										// suppress everything in an escape; end escape on alphabetic
										if (isalpha(c))
											state = State::Plain;
										break;
									}
								}
								*dst = 0;

								// write this line
								fprintf(fp, "%s\n", l.c_str());
							}

							// done - close the file
							fclose(fp);

							// success
							statusMessage = "OK - Log saved to file";
						}
						else
						{
							// unable to open file
							statusMessage = "FAILED - Error opening file";
						}

						// show the new status message
						newStatusMessage = true;
					}

					// remove the file entry line, make sure we redraw
					printf(DEFCOLOR "\033[255D\033[0K");
					locText = "@@Save";
				}
				else if (c == '\n' || c == '\r' || cext == 0x50)
				{
					// Enter/return/down arrow
					if (topRow < maxTopRow)
					{
						topRow += 1;
						redrawByUser = true;
					}
				}
				else if (cext == 0x48)
				{
					// Up arrow
					if (topRow > 0)
					{
						topRow -= 1;
						redrawByUser = true;
					}
				}
				else if (cext == 0x4f)
				{
					// Accelerate
					if (topRow != maxTopRow)
					{
						topRow = maxTopRow;
						redrawByUser = true;
					}
				}
				else if (cext == 0x47)
				{
					// Home
					if (topRow != 0)
					{
						topRow = 0;
						redrawByUser = true;
					}
				}
				else if (cext == 0x49)
				{
					// Page Up
					int n = max(0, topRow - nRows + 1);
					if (n != topRow)
					{
						topRow = n;
						redrawByUser = true;
					}
				}
				else if (cext == 0x51 || c == ' ')
				{
					// Page Down
					int n = min(maxTopRow, topRow + nRows - 1);
					if (n != topRow)
					{
						topRow = n;
						redrawByUser = true;
					}
				}
				else if (c == 27 || c == 'q' || c == 'Q')
				{
					// escape/Q - reset display attributes and exit the interactive log
					// display loop
					printf("\033[0m\n");
					quitLog = true;
				}
			}
		}

		// transfer the device handle back to the caller's unique_ptr
		if (VendorInterface::Shared::Locker l(&threadCtx.device); l.locked)
			upDevice.reset(threadCtx.device.device.release());
	}
}

// --------------------------------------------------------------------------
//
// Show the device's statistics
//
static void ShowStats(VendorInterface *device)
{
	// retrieve the statistics
	PinscapePico::Statistics s{ 0 };
	if (auto result = device->QueryStats(&s, sizeof(s), true); result == PinscapeResponse::OK)
	{
		// interpret the up time into days, hours, minutes, and seconds
		int days = static_cast<int>(s.upTime / 1000000 / 86400);
		int sec = static_cast<int>((s.upTime / 1000000) % 86400);
		char dayStr[32] = "";
		if (days != 0)
			sprintf_s(dayStr, "%d day%s, ", days, days == 1 ? "" : "s");

		// format a large number to add comma group separators, for easier reading
		auto C = [](uint64_t val) -> std::string
		{
			char buf[64], buf2[64]{ 0 };
			sprintf_s(buf, "%I64d", val);
			char *dst = &buf2[sizeof(buf2)];
			*--dst = 0;
			int n = 0;
			for (const char *src = buf + strlen(buf) ; src > buf ; )
			{
				*--dst = *--src;
				if (src > buf && ++n == 3)
					*--dst = ',', n = 0;
			}
			return std::string(dst);
		};

		// format the statistics
		printf("Statistics:\n"
			"  Time since reset:  %s us (%s%d:%02d:%02d hours)\n"
			"  Main loop iters:   %s since boot, %s since last snapshot\n"
			"  Average loop time: %s us\n"
			"  Maximum loop time: %s us\n"
			"  Heap size:         %s bytes\n"
			"  Heap unused:       %s bytes\n"
			"  Arena size:        %s bytes\n"
			"  Arena allocated:   %s bytes\n"
			"  Arena not in use:  %s bytes\n",
			C(s.upTime).c_str(), dayStr, sec / 3600, (sec % 3600) / 60, sec % 60,
			C(s.nLoopsEver).c_str(), C(s.nLoops).c_str(), C(s.avgLoopTime).c_str(), C(s.maxLoopTime).c_str(),
			C(s.heapSize).c_str(), C(s.heapUnused).c_str(),
			C(s.arenaSize).c_str(), C(s.arenaAlloc).c_str(), C(s.arenaFree).c_str());
	}
	else
		printf("Error %d reading stats\n", result);
}

// --------------------------------------------------------------------------
//
// Send an IR command.  This commands the device to send a code via its
// physical IR transmitter, if one is installed.
//
static void SendIR(VendorInterface *device, const char *irCmdStr)
{
	// check for a repeat count
	static const std::regex irCmdPat("([0-9a-f]{2}\\.[0-9a-f]{2}\\.[0-9a-f]{4,16})(\\*\\d+)?", std::regex_constants::icase);
	std::match_results<const char *> m;
	if (std::regex_match(irCmdStr, m, irCmdPat))
	{
		// parse into the code and repeat count sections
		std::string code = m[1];
		int count = m[2].matched ? atoi(m[1].str().c_str()) : 1;

		// range-check the repeat count (the transmitter API does this, too, but
		// we can provide more specific error messaging if we check first)
		if (count < 1 || count > 255)
			ErrorExit("The repeat count must be from 1 to 255");

		// parse the code
		PinscapePico::IRCommand cmd;
		if (cmd.Parse(code))
		{
			// send the report
			int stat = device->SendIRCommand(cmd, count);

			// report results
			printf("Sending %s : %s\n", cmd.ToString().c_str(), VendorInterface::ErrorText(stat));
			return;
		}
	}

	// if we didn't send the code and return, there wa sa format error
	printf("Invalid IR command format \"%s\"; expected <protocol>.<flags>.<code>, where <protocol> and <flags> are "
		"2-digit hex numbers, and <code> is a 4-digit to 16-digit hex number\n", irCmdStr);
	exit(1);
}

// --------------------------------------------------------------------------
//
// IR learn mode.  This waits for the device to receive a command on its
// physical IR receiver (if one is installed), and displays the received
// code in Pinscape's universal IR code format.  The universal format is
// suitable for use in config file entries that specify IF codes, and for
// this program's --ir-send command.  We provide interactive prompts for
// when to press the remote button to transmit the code to be learned.
//
static void LearnIR(VendorInterface *device)
{
	// IR commands are received on the HID feedback controller interface, 
	// so open that for reading.	
	std::unique_ptr<FeedbackControllerInterface> f;
	HRESULT hr = device->OpenFeedbackControllerInterface(f);
	if (!SUCCEEDED(hr))
		ErrorExit("Unable to open feedback controller interface", hr);

	// clear out any prior input from our view of the input stream
	FeedbackReport report;
	while (f->Read(report, 0)) { }

	// show instructions
	printf("IR command learning mode - this will monitor for IR input and\n"
		"report the command code received.\n\n"
		"Point your remote control at the Pinscape unit's IR receiver.\n"
		"PRESS AND HOLD the desired button.\n\n"
		"Waiting for IR Input - Press Escape to cancel\n");

	// monitor for IR input or an escape key
	using IRReport = FeedbackControllerInterface::IRReport;
	IRReport firstCode;
	for (bool done = false, gotCode = false ; !done ; )
	{
		// check for keyboard input cancellation
		while (_kbhit())
		{
			if (_getch() == 27)
			{
				done = true;
				break;
			}
		}

		// wait for a report; time out after 50ms so that we remain
		// responsive with our keyboard input checks
		if (f->Read(report, 50) && report.type == FeedbackReport::RPT_IR_COMMAND)
		{
			// If this is the first code we've received, save it
			// for comparison to the next code.  If we have a code,
			// compare it.
			if (!gotCode)
			{
				// this is the first code - save it
				gotCode = f->Decode(firstCode, report);
				if (gotCode)
					printf("[Received %s]\n", firstCode.command.ToString().c_str());
			}
			else
			{
				// We already have a code - decode the new code
				IRReport newCode;
				if (f->Decode(newCode, report))
				{
					// check for a match to the first code
					printf("[Received %s]\n", newCode.command.ToString().c_str());
					if (newCode.command == firstCode.command)
					{
						// It's a match - we now know the code and
						// whether or not we have dittos.  if the new
						// code has a ditto, set the protocol ditto
						// flag.
						if (newCode.command.hasDitto && newCode.command.ditto)
							newCode.command.flags |= 0x02;

						// display the results, and we're done
						printf("\n"
							"Code learned: %s\n"
							"You can use this code string in the JSON configuration settings\n"
							"and in --ir-send commands.\n",
							newCode.command.ToString().c_str());
						break;
					}
				}
			}
		}
	}
}

// --------------------------------------------------------------------------
//
// Pulse the TV ON relay
//
static void PulseTVRelay(VendorInterface *device)
{
	if (auto stat = device->PulseTVRelay(); stat == PinscapeResponse::OK)
		printf("TV relay pulse OK\n");
	else
		ErrorStatExit("Error pulsing TV relay", stat);
}

//
// Set the TV relay manual state
//
static void SetTVRelay(VendorInterface *device, bool on)
{
	if (auto stat = device->SetTVRelayManualState(on); stat == PinscapeResponse::OK)
		printf("TV relay manual set to manual %s\n", on ? "ON" : "OFF");
	else
		ErrorStatExit("Error setting TV relay manual state", stat);
}

// --------------------------------------------------------------------------
//
// Fix the JOY.CPL display, by deleting the DirectInput registry keys
// that screw up the display.
// 
// The DirectInput subsystem creates some registry keys for each gamepad
// or joystick HID interface it finds, to give each joystick an ID number
// within the DirectInput system, and to store axis calibration data for
// the device.  The mechanism for assigning this ID number isn't
// documented, but it's probably based on the underlying Device Manager
// identifier for the device, probably the Device Instance ID.  In most
// cases, when you unplug and reconnect a device, Windows will recognize
// the device as the one it saw before and assign it the same ID.  But
// not always.  In some cases, particularly if an application keeps an
// open handle to the device through the unplug-reconnect sequence,
// DirectInput will assign it a new joystick ID.  But it also just seems
// to happen randomly, even when no applications are running (although
// it's much less probable in this case).  Whatever the cause, the ID
// change often screws up the JOY.CPL presentation, making it display
// the wrong set of axes and/or buttons.  The reasons aren't clear, but
// after much experimentation, I've come to think it's just a bug in
// JOY.CPL, because it doesn't seem to affect the DirectInput COM
// interfaces - even when JOY.CPL sees a corrupted view of the axes
// and buttons, the DirectInput COM interfaces still enumerate the
// right devices and the right controls within the devices.  And it
// DEFINITELY doesn't affect anything at the HID level.  So it's
// practically harmless, except that it makes the device list LOOK
// horribly corrupted when you run JOY.CPL.
// 
// The proximate cause of the JOY.CPL bug is some registry keys that
// DirectInput maintains per device.  DI populates the keys for a
// device automatically when JOY.CPL (and presumably any other DI
// application) opens the device.  Again, the DI COM interfaces don't
// seem to be fazed, so the keys apparently aren't themselves bad or
// corrupt; it's just that some combinations of values screws up the
// JOY.CPL presentation of the device list and control layout.
// 
// The easy workaround is to delete the keys.  This forces DI to
// rebuild them from scratch the next time a DI application access
// them.  JOY.CPL always seems happy with the results of a full clean
// build of the keys; it only gets screwed up when the keys are
// modified for an existing device that gets assigned a new joystick
// ID on reconnecting.
// 
// This routine performs the key removal.  There's just one main key
// that we remove, along with its subkeys.  In testing, we don't
// actually have to delete the whole tree; it's sufficient to delete
// the "Joystick ID" values that are populated in some of the subkeys.
// But it's easier to just nuke the whole tree and let DI rebuild it
// all from scratch.
// 
// Since these keys are under HKEY_CURRENT_USER, and are normally
// owned by the current user account, normal user privileges are
// adequate.  There should be no need for elevation.
//
static void FixJoyCpl(std::unique_ptr<VendorInterface> &device, const std::string &deviceID)
{
	// Form the key name:
	// HKEY_CURRENT_USER\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\DirectInput\VID_vid&PID_pid\Calibration
	USB_DEVICE_DESCRIPTOR dd;
	device->GetDeviceDescriptor(dd);
	char keyName[128];
	sprintf_s(keyName,
		"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\DirectInput\\VID_%04X&PID_%04X\\Calibration",
		dd.idVendor, dd.idProduct);

	// get a list of existing boot drives
	auto oldBootDrives = PinscapePico::RP2BootDevice::EnumerateRP2BootDrives();

	// reboot the device into boot-loader mode
	printf("Rebooting device to Boot Loader mode\n");
	if (int stat = device->EnterBootLoader(); stat != PinscapeResponse::OK)
		ErrorStatExit("Error rebooting to Boot Loader mode", stat);

	// close our handle
	device.reset();

	// wait for the boot loader drive to appear
	auto newBootDrives = PinscapePico::RP2BootDevice::EnumerateNewRP2BootDrives(oldBootDrives);
	for (UINT64 tEnd = GetTickCount64() + 2000 ; newBootDrives.size() == 0 && GetTickCount64() < tEnd ; )
	{
		Sleep(100);
		newBootDrives = PinscapePico::RP2BootDevice::EnumerateNewRP2BootDrives(oldBootDrives);
	}

	// delete the key
	printf("Deleting JOY.CPL registry key (%s)\n", keyName);
	if (auto result = RegDeleteTreeA(HKEY_CURRENT_USER, keyName); result != ERROR_SUCCESS)
		printf("Error deleting key HKCU\\%s (Win32 error code %d)\n", keyName, result);

	// if we can identify the drive, reboot it
	if (newBootDrives.size() == 1)
	{
		printf("Rebooting Pico\n");
		if (!newBootDrives.front().RebootPico())
		{
			printf("Pico reboot attempt failed.  Please manually reset the device by unplugging and reconnecting it.\n");
			exit(1);
		}
	}
	else
	{
		printf("Unable to identify the Pico's boot drive. Please manually reset the device by unplugging and reconnecting it.\n");
		exit(1);
	}

	// reconnect
	WaitForReconnect(device, deviceID, 1000);
}
	
// --------------------------------------------------------------------------
// 
// Show command line options and exit
//
static void UsageExit()
{
	printf(
		"Usage: ConfigTool [-q|--quiet] [--id <unit>] [--list] [options]\n"
		"\n"
		"-q or --quiet runs the command in quiet mode, suppressing the normal\n"
		"program banner and listing of device information.  This must be the\n"
		"first option, if used.\n"
		"\n"
		"--id is only required if multiple Pinscape Pico devices are currently\n"
		"running, to select which one to address for this command.  The <unit>\n"
		"can be specified as the configured unit number, unit name, or Pico\n"
		"hardware ID.  --id must be the first option after -q/--quiet.\n"
		"\n"
		"--list shows a list of all attached Pinscape Pico devices.\n"
		"\n"
		"Options:\n"
		"  --help, -?, /?                show this command-line help message\n"
		"  --reset                       reboot the Pico\n"
		"  --safe-mode                   reboot the Pico into Safe Mode\n"
		"  --stats                       display device memory usage and time statistics\n"
		"  --log                         view device log messages interactively\n"
		"  --log=plain                   display device log message on the console\n"
		"  --update <file>               update firmware from the given .UF2 file\n"
		"  --get-config                  display device configuration file on console\n"
		"  --get-config=<file>           write device configuration file to <file>\n"
		"  --put-config <file>           install configuration file <file> on device\n"
		"  --erase-config                delete the JSON configuration file from device\n"
		"  --factory-reset               delete all configuration data from device\n"
		"  --fix-joycpl                  fix JOY.CPL display (deletes corrupted registry keys)\n"
		"  --put-safemode-config <file>  install <file> as the safe-mode config file\n"
		"  --ir-learn                    wait for an IR command to be received, display it\n"
		"  --ir-send <code>              send <code> through the IR transmitter\n"
	    "  --pulse-tv-relay              pulse the TV ON relay for the configured interval\n"
		"  --tv-relay on|off             set the TV relay manual state to ON or OFF\n");

	exit(1);
}

// --------------------------------------------------------------------------
//
// Main entrypoint
//
int main(int argc, char **argv)
{
	// check for quiet mode
	int argi = 1;
	bool quietMode = false;
	if (argi + 1 < argc && (strcmp(argv[argi], "-q") == 0 || strcmp(argv[argi], "--quiet") == 0))
	{
		quietMode = true;
		++argi;
	}

	// version banner
	if (!quietMode)
	{
		printf("Pinscape Pico Config Tool  Version %s, build %s\n"
			"Copyright 2024 Michael J Roberts / BSD-3-Clause License / NO WARRANTY\n\n",
			gVersionString, GetBuildTimestamp());
	}

	// Scan for a device ID argument (it's required to be the first argument, if present)
	std::string deviceID;
	if (argi + 1 < argc && strcmp(argv[argi], "--id") == 0)
	{
		// get the device ID, and trim the argument pair from the list
		// of remaining arguments to process
		deviceID = argv[argi + 1];
		argi += 2;
	}

	// with no arguments, just show usage
	if (argi == argc && deviceID.size() == 0)
	{
		printf("\n");
		UsageExit();
	}

	// check for a lone --list argument
	if (argi < argc && strcmp(argv[argi], "--list") == 0)
	{
		// show the list
		ShowDeviceList();

		// if that's the only remaining argument, we're done
		if (++argc >= argc)
			return 0;
	}
	
	// open the device
	HRESULT hr;
	std::unique_ptr<VendorInterface> device;
	{
		// get the selected device
		std::list<VendorInterfaceDesc> paths;
		VendorInterfaceDesc *selectedPath = SelectDevice(paths, deviceID, true);

		// connect
		if (!SUCCEEDED(hr = selectedPath->Open(device)))
		{
			char buf[MAX_PATH + 32];
			if (hr == E_ACCESSDENIED)
				sprintf_s(buf, "Can't open device due to access error; the device might already be in use by another program.\nPath: %" _TSFMT,
					selectedPath->Name());
			else
				sprintf_s(buf, "Device open failed, path %" _TSFMT, selectedPath->Name());
			ErrorExit(buf, hr);
		}
	}

	// query the firmware version
	VendorInterface::Version vsn;
	int stat;
	if ((stat = device->QueryVersion(vsn)) != PinscapeResponse::OK)
		ErrorStatExit("Error querying version", stat);

	// query the Pico hardware ID
	PinscapePico::DeviceID devId;
	if ((stat = device->QueryID(devId)) != PinscapeResponse::OK)
		ErrorStatExit("Error querying hardware ID", stat);

	// show the selected device stats
	if (!quietMode)
	{
		printf("Connected to Pinscape Pico unit #%d (%s)\n"
			"Hardware ID %s (%s, RP%d CPU v%d, ROM %s)\n"
			"Firmware version %d.%d.%d, build date %s (Pico SDK %s, TinyUSB %s, %s)\n\n",
			devId.unitNum, devId.unitName.c_str(), devId.hwid.ToString().c_str(),
			devId.FriendlyBoardName().c_str(), devId.cpuType, devId.cpuVersion, devId.romVersionName.c_str(),
			vsn.major, vsn.minor, vsn.patch, vsn.buildDate,
			devId.picoSDKVersion.c_str(), devId.tinyusbVersion.c_str(), devId.compilerVersion.c_str());
	}

	// this is now the selected device
	deviceID = devId.hwid.ToString();

	// Send a wall clock time update to the Pico.  We do this as a routine
	// matter on each new connection.  The idea is that if every application
	// interaction with the Pico sends a time update as a matter of course,
	// there won't be a need for the user to manually set up a separate
	// program launch just for the sake of updating the Pico's clock, since
	// the Pico will be so likely to get the time incidentally from some
	// application interaction that would have happened anyway.  The Config
	// Tool isn't actually a very good example of that, since most users
	// probably won't run that routinely - we're talking more about DOF
	// clients, front ends, and anything else the user keeps in the
	// foreground most of the time on a pin cab.  But we'll do it here
	// just for the sake of setting a good example.
	device->PutWallClockTime();

	// process the command line
	for ( ; argi < argc ; ++argi)
	{
		if (strcmp(argv[argi], "--list") == 0)
		{
			ShowDeviceList();
		}
		else if (strcmp(argv[argi], "--update") == 0)
		{
			// update the firmware from the specified path
			if (++argi >= argc)
				ErrorExit("Missing UF2 filename; usage is --update <filename>");

			UpdateFirmware(device, deviceID, ToTCHAR(argv[argi]).c_str());
		}
		else if (strcmp(argv[argi], "--put-config") == 0)
		{
			// get the config file
			if (++argi >= argc)
				ErrorExit("Missing config file name; usage is --put-config <filename>");

			// send it to the device
			PutConfig(device, deviceID, argv[argi], PinscapeRequest::CONFIG_FILE_MAIN);
		}
		else if (strcmp(argv[argi], "--put-safemode-config") == 0)
		{
			// get the config file
			if (++argi >= argc)
				ErrorExit("Missing config file name; usage is --put-config <filename>");

			// send it to the device
			PutConfig(device, deviceID, argv[argi], PinscapeRequest::CONFIG_FILE_SAFE_MODE);
		}
		else if (strcmp(argv[argi], "--erase-config") == 0)
		{
			stat = device->EraseConfig(PinscapeRequest::CONFIG_FILE_ALL);
			if (stat == PinscapeResponse::OK)
				printf("Device configuration file erased; factory defaults restored\n");
			else
				ErrorStatExit("Error erasing the device's configuration file", stat);
		}
		else if (strcmp(argv[argi], "--get-config") == 0)
		{
			// retrieve the config file and write to stdout
			GetConfig(device.get(), stdout);
		}
		else if (strncmp(argv[argi], "--get-config=", 13) == 0)
		{
			// retrieve the config file and save to the named file
			const char *filename = &argv[argi][13];
			FILE *fp = nullptr;
			if (fopen_s(&fp, filename, "w") != 0 || fp == nullptr)
				ErrorExit("Error opening configuration output file");

			// retrieve the data and write to the file
			GetConfig(device.get(), fp);

			// done
			fclose(fp);
			printf("Configuration data saved to %s\n", filename);
		}
		else if (strcmp(argv[argi], "--factory-reset") == 0)
		{
			// factory reset - confirm
			printf("Warning: this will delete the configuration file and all other\n"
				"saved settings on the device, including plunger calibration data.\n"
				"If you only wish to clear the settings file, use --put-config to\n"
				"send an empty text file.\n"
				"\n"
				"Do you really want to delete all configuration data? [y/N] ");

			char buf[128];
			if (fgets(buf, _countof(buf), stdin) != nullptr && (buf[0] == 'y' || buf[0] == 'Y'))
			{
				stat = device->FactoryResetSettings();
				if (stat != PinscapeResponse::OK)
					ErrorStatExit("Error executing factory reset", stat);

				printf("Success - all saved settings deleted, factory defaults restored\n");
			}
			else
				printf("Canceled - no changes made\n");
		}
		else if (strcmp(argv[argi], "--reset") == 0)
		{
			// reset the Pico
			if ((stat = device->ResetPico()) != PinscapeResponse::OK)
				ErrorStatExit("Reset request failed", stat);

			// the device handle is now invalid
			device.reset();

			// wait for reconnect
			printf("Reset succeeded\n");
			if (!WaitForReconnect(device, deviceID, 3000))
				ErrorExit("Timeout waiting for the device to reconnect");
		}
		else if (strcmp(argv[argi], "--safe-mode") == 0)
		{
			// reset the Pico in safe mode
			if ((stat = device->EnterSafeMode()) != PinscapeResponse::OK)
				ErrorStatExit("Safe Mode reset request failed", stat);

			// the device handle is now invalid
			device.reset();

			// wait for reconnect
			printf("Safe Mode reset succeeded\n");
			if (!WaitForReconnect(device, deviceID, 3000))
				ErrorExit("Timeout waiting for the device to reconnect");
		}
		else if (strcmp(argv[argi], "--log") == 0 || strncmp(argv[argi], "--log=", 6) == 0)
		{
			// Figure the mode.  If there's an "=plain" qualifier, OR if
			// the console is redirected, use plain mode.  Otherwise use
			// interactive mode.
			DWORD consoleMode = 0;
			bool isConsole = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &consoleMode);
			bool plainMode = !isConsole;
			if (argv[argi][5] == '=')
			{
				const char *opt = &argv[argi][6];
				if (strcmp(opt, "plain") == 0)
					plainMode = true;
				else if (strcmp(opt, "interactive") == 0)
					/* accept, but this has no effect, since the console mode takes priority */ ;
				else
					ErrorExit("Invalid --log=xxx sub-option; valid options are 'plain' and 'interactive'");
			}

			// show the log
			ShowLog(device, deviceID, isConsole, plainMode);
		}
		else if (strcmp(argv[argi], "--stats") == 0)
		{
			// show device statistics
			ShowStats(device.get());
		}
		else if (strcmp(argv[argi], "--ir-send") == 0)
		{
			// get the command
			if (++argi >= argc)
				ErrorExit("Missing IR command: usage is --ir-send <protocol>.<flags>.<command>, hex notation");

			// send the command
			SendIR(device.get(), argv[argi]);
		}
		else if (strcmp(argv[argi], "--ir-learn") == 0)
		{
			// learn an IR command
			LearnIR(device.get());
		}
		else if (strcmp(argv[argi], "--pulse-tv-relay") == 0)
		{
			// pulse the TV relay
			PulseTVRelay(device.get());
		}
		else if (strcmp(argv[argi], "--tv-relay") == 0)
		{
			// get the mode option
			if (++argi >= argc)
				ErrorExit("Missing ON/OFF mode for --tv-relay\n");

			// send the command
			SetTVRelay(device.get(), _stricmp(argv[argi], "on") == 0);
		}
		else if (strcmp(argv[argi], "--fix-joycpl") == 0)
		{
			// fix JOY.CPL registry keys
			FixJoyCpl(device, deviceID);
		}
		else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "/?") == 0 || strcmp(argv[argi], "-?") == 0)
		{
			UsageExit();
		}
		else
		{
			printf("Unrecognized option \"%s\"\n", argv[argi]);
			break;
		}
	}

	// terminate COM
	if (comInited)
		CoUninitialize();

	// normal completion
	return 0;
}
