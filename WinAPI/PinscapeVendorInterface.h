// Pinscape Pico - Vendor Interface API
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// This module implements an API for interacting with the Pinscape
// Pico device via its USB Vendor Interface.  This interface provides
// configuration, control, and testing functions.  It's designed for
// programs such as the Pinscape Pico Config Tool that implement
// interactive access to configuration and test functions.
// 
// To access the vendor interface, the first step is to enumerate
// available devices:
// 
//    std::list<PinscapePico::VendorInterfaceDesc> devices;
//    HRESULT hr = PinscapePico::VendorInterface::EnumerateDevices(devices);
//    if (SUCCEEDED(hr))
//       ...
//
// Now you can open a device from the enumeration list:
// 
//    PinscapePico::VendorInterface *vi = nullptr;
//    hr = devices.front().Open(vi);
//    if (SUCCEEDED(hr))
//       ...
// 
// Alternatively, you can open a device by specific unit number,
// unit name, or Pico hardware ID, if you already know which device
// you want to access:
// 
//    hr = VendorInterface::Open(vi, 1);  // open unit #1
// 
// Another approach is to first enumerate Feedback Controller
// interfaces, then use the hardware ID from that interface to
// select a Vendor Interface to open.  That approach lets you
// display more information to user to make an interactive device
// selection.  That has the advantage that the Feedback Controller
// interface can query device information in "shared" mode, without
// the need for an exclusive WinUsb connection (as the vendor
// interface requires).  The Vendor Interface enumerator can't
// obtain any detail on the device beyond its Windows device path,
// which isn't meaningful to a user and thus can't be used for
// interactive device selection.  You can query the same identifier
// information via the Vendor Interface, but only *after* opening
// the WinUsb connection.
// 
// Once you've opened the Vendor Interface object, you can use its
// member functions to query information from the device, change
// configuration settings, and run various tests.  Note that the
// WinUsb connection is inherently exclusive, meaning that only one
// application can open a WinUsb connection to the device at a time.
// The WinUsb connection doesn't affect concurrent access to any of
// the other USB interfaces, though, so this won't block other
// programs that use gamepad or keyboard input or the Feedback
// Controller interface.
//
// 
// The term "vendor interface" is a USB-ism, describing a USB wire
// protocol that's defined by the device manufacturer rather than
// conforming to one of the pre-defined standard USB "classes", such
// as HID, CRC, or mass storage.  Vendor interfaces are generally
// accessed on the host through mating kernel device drivers, but
// Windows and Linux also both provide application-level (user mode)
// access through "generic" USB drivers.  Windows provides a built-in
// generic driver called WinUsb, which is how this API accesses the
// device.  On Linux, an open source library called libusb provides
// similar generic driver access.  This code only works with WinUsb;
// it could be adapted to libusb with some work, since libusb and
// WinUsb are conceptually pretty equivalent, but the details are
// all completely different between the two, naturally.  The reason
// that we chose WinUsb rather than libusb as the basis for this
// code is that WinUsb is built in to all Windows systems from
// Windows 7 onward, so it provides a transparent, driverless,
// plug-and-play install experience for the user, whereas libusb
// requires some manual steps to download and install third-party
// device drivers.
//
// The API also provides functions for discovering Pico devices
// that are currently in RP2 Boot Loader mode, which makes it
// possible for a client program to automate the process of
// installing new firmware on a Pico.  The RP2 Boot Loader is the
// Pico's native ROM-based boot loader.  It's a built-in feature
// of the Pico (actually of the CPU itself), so it's always
// available for any Pico, whether or not any Pinscape software
// is involved.  When the RP2 Boot Loader is active, it exposes
// a virtual disk drive on the USB interface.  Firmware can be
// installed onto the Pico simply by copying a UF2 file (the
// Pico's equivalent of a Windows EXE) onto the RP2 virtual disk.
// 
// One of the functions that you can access through the Pinscape
// Configuration and Control API is a "reboot into RP2" command,
// which resets the Pico and places it in RP2 mode.  After
// invoking this function, you can use the RP2 discovery function
// to find the device's virtual disk, and then copy a UF2 file
// onto the disk to install new firmware.  The Pico automatically
// resets and launches the new firmware after the UF2 is copied,
// so the entire firmware update process can be automated,
// without the need for any user intervention.
// 
// Note that a user can always manually activate the RP2 Boot
// Loader, by power-cycling the Pico while holding down the
// BOOTSEL button (the small pushbutton on top of the Pico).
// The RP2 boot loader and BOOTSEL button are hard-wired
// features of the Pico, so it's impossible for software to
// block user activation of the boot loader, which in turn makes
// it impossible to brick a Pico with errant software.  If the
// automated Config Tool update process ever fails, the user
// can use the BOOTSEL feature to perform a manual re-install.

#pragma once

#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <memory>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <Windows.h>
#include <winusb.h>
#include <usb.h>
#include "Utilities.h"
#include "PinscapePicoAPI.h"

namespace PinscapePico
{
	// forwards/externals
	class FeedbackControllerInterface;

	// Pinscape Pico device object.  This object represents the Pinscape Pico's
	// USB vendor interface, which provides the main configuration and control
	// functions required for utilities like the Config Tool.  It also has static
	// methods for enumerating devices.
	//
	// To create a vendor interface device object, use Enumerate() to get a list
	// of the available Pinscape devices.  That list is in the form of file system
	// paths; call Open() on a path object to open a live connection to the device.
	// You can use the device connection to retrieve the device's information and
	// to send control commands.
	class VendorInterface
	{
		friend class VendorInterfaceDesc;

	public:
		~VendorInterface();

		// forwards
		class IProgressCallback;

		// A shared PinscapePico::VendorInterface object.  This 
		// encapsulates a device object and a mutex, which can be locked
		// before accessing the device to protect against concurrent
		// access from other threads.
		//
		// In addition, the object can optionally store an HWND for a
		// Window registered to receive device-change notifications for
		// the device via RegisterDeviceNotification().
		class Shared
		{
		public:
			Shared() { }
			virtual ~Shared();

			// Construct with a device object and notifier window.
			// If the window is non-null, we'll register it for device
			// notifications via RegisterDeviceNotification().
			Shared(VendorInterface *device, HWND hwndNotify);

			// the underlying device
			std::unique_ptr<VendorInterface> device;
			
			// notifier window and registration handle
			HWND hwndNotify = NULL;
			HANDLE deviceNotifyHandle = NULL;

			// get the underlying device pointer
			VendorInterface *Get() { return device.get(); }

			// set the underlying device pointer and notifier window
			void Set(VendorInterface *ifc, HWND hwndNotify);

			// Register/unregister the current notify window
			void RegisterNotify();
			void UnregisterNotify();

			// Lock the mutex with the given timeout.  Returns true
			// if the mutex was successfully acquired, false if not.
			// Note that this doesn't provide error information; if
			// the caller wants to distinguish errors from timeouts,
			// it can use the native Win32 WaitForSingleObject().
			bool Lock(DWORD timeout_ms = INFINITE) {
				return WaitForSingleObject(mutex, timeout_ms) ==  WAIT_OBJECT_0;
			}

			// Mutex locker.  This is an RAII object that can be
			// instantiated locally to acquire and hold a lock on the
			// mutex for the duration of a scope.  This can be used
			// like so:
			//
			// using DeviceLocker = PinscapePico::VendorInterface::Shared::Locker;
			// if (DeviceLocker l(dev); l.locked) {
			//    // this scope has exclusive device access
			// } 
			// // lock is automatically released on exiting scope
			//
			struct Locker
			{
				Locker(Shared *dev, DWORD timeout_ms = INFINITE) :
					dev(dev) {
					locked = dev->Lock(timeout_ms);
				}

				Locker(std::shared_ptr<Shared> &dev, DWORD timeout_ms = INFINITE) :
					dev(dev.get()), sharedDevice(dev) {
					locked = dev->Lock(timeout_ms);
				}

				~Locker() { if (locked) dev->Unlock(); }

				// true -> the mutex was successfully locked
				bool locked;

				// the shared device
				Shared *dev;

				// a shared pointer to the device, if provided by the caller
				std::shared_ptr<Shared> sharedDevice;
			};

			// unlock the mutex
			void Unlock() { ReleaseMutex(mutex); }

			// Windows Mutex, to serialize access to the device handle
			// across threads
			HANDLE mutex{ CreateMutex(NULL, false, NULL) };
		};

		// Get the error text for a Vendor Interface status code.  These are
		// the codes returned across the wire by most of the device requests;
		// in this API, these functions return 'int' results.  (Note that the
		// API functions for purely Windows-side tasks, such as discovering
		// and opening USB interfaces, generally use HRESULT's, since these
		// are purely local operations with status more meaningfully conveyed
		// in terms of Windows error codes.)  The returned string is a const
		// string with static storage duration.
		static const char *ErrorText(int statusCode);

		// Enumerate all currently attached Pinscape Pico devices. 
		// Populates the path list with the file system path names of 
		// the currently attached devices that provide the Pinscape
		// Pico USB vendor interface.  Returns an HRESULT indicating
		// if the operation was successful.
		static HRESULT EnumerateDevices(std::list<VendorInterfaceDesc> &deviceList);

		// Filter a device list by identification string.  The ID can
		// be given as a unit number, unit name, or hardware ID (in
		// hexadecimal string format).
		// 
		// If we find any exact matches for any of those interpretations
		// of the ID string, we'll filter for the exact matches ONLY.
		// If there aren't any exact matches, though, we'll filter for
		// devices where the ID matches any SUBSTRING of the hardware
		// ID, as long as the ID string is at least four characters
		// long.  This allows the user to select a device by a short
		// but unique fragment of the hardware ID - usually it just
		// takes the first few or last few hex digits of the hardware
		// ID to uniquely identify a unit.  We ignore these fragment
		// matches in the case of any unique match to unit number or
		// name, however, so that unit names that just happen to look
		// like short hex strings won't be treated as ambiguous with
		// hardware IDs.
		// 
		// Populates the match list with descriptors matching the ID
		// string.  If the ID is null or empty, we simply return all
		// active devices.
		static HRESULT EnumerateDevicesByID(std::list<VendorInterfaceDesc> &matchList, const char *id);

		// Open a device by its hardware ID.  Returns S_OK on success,
		// S_FALSE if no match was found, or an HRESULT error if another
		// error occurs.  Typename T can be a direct VendorInterface*,
		// or a unique_ptr or shared_ptr to a VendorInterface.
		template<typename T> static HRESULT Open(T &device, const PicoHardwareId &hwid) {
			return Open(device, [&hwid](const DeviceID &id) { return id.hwid == hwid;	});
		}

		// Open a device by its unit number.  Returns S_OK on success,
		// S_FALSE if no match was found, or an HRESULT error if another
		// error occurs.  Typename T can be a direct VendorInterface*,
		// or a unique_ptr or shared_ptr to a VendorInterface.
		template<typename T> static HRESULT Open(T &device, int unitNum) {
			return Open(device, [&unitNum](const DeviceID &id) { return id.unitNum == unitNum; });
		}

		// Open a device by its unit name.  Returns S_OK on success,
		// S_FALSE if no match was found, or an HRESULT error if another
		// error occurs.  Typename T can be a direct VendorInterface*,
		// or a unique_ptr or shared_ptr to a VendorInterface.
		template<typename T> static HRESULT Open(T &device, const char *unitName) {
			return Open(device, [unitName](const DeviceID &id) { return id.unitName == unitName; });
		}

		// Open by a caller-specified ID match.  Returns S_OK on success,
		// S_FALSE if no match was found, or an HRESULT error if another
		// error occurs.
		using IDMatchFunc = std::function<bool(const DeviceID &)>;
		static HRESULT Open(VendorInterface* &device, IDMatchFunc match);
		static HRESULT Open(std::unique_ptr<VendorInterface> &device, IDMatchFunc match);
		static HRESULT Open(std::shared_ptr<VendorInterface> &device, IDMatchFunc match);

		// Get the USB device serial number
		const wchar_t *GetSerialNumber() const { return serialNum.c_str(); }

		// Enumerate HID interfaces exposed by the Pinscape Pico device.
		// This returns a list of file system paths representing the HID
		// interfaces exposed for the Pico object corresponding to the
		// 'this' instance.  You can open a device in the list via
		// CreateFile(), passing the path string from the list, and then
		// use the handle in HidD_xxx calls; see 
		HRESULT EnumerateAssociatedHIDs(std::list<WSTRING> &hidList);

		// Open the feedback controller HID interface.  If successful,
		// opens a system file handle to the device and returns it in
		// 'handle'.
		HRESULT OpenFeedbackControllerInterface(std::unique_ptr<FeedbackControllerInterface> &ifc);

		// Get the USB CDC virtual COM port name associated with this
		// interface.  Pinscape Pico can optionally be configured to
		// expose a virtual COM port on the USB connection, for message
		// log and command console access.  The COMn port number is
		// assigned automatically by Windows, so the port has to be
		// identified after it's established by enumerating the live
		// ports.  It's fairly straightforward for the user to do that
		// by manually searching through the Device Manager list of
		// ports, but this function saves them the trouble by doing
		// the search automatically, and it's more precise because
		// we can positively identify the CDC port associated with
		// a particular Pinscape Pico vendor interface by matching
		// the parent device that the two interfaces have in common.
		// 
		// This only identifies the *CDC* COM port - in other words,
		// the virtual port exposed via the USB connection.  This
		// doesn't identify the UART port.  There's no way to find
		// the UART port by interrogating Windows, because physical
		// UARTs don't have any general-purpose mechanism for
		// identifying the device on the ohter side of the connection.
		// But the *user* shouldn't need any help identifying a UART
		// connection in the first place, because that's just a
		// matter of which physical port they plugged the cable into.
		//
		// On success, fills in 'name' with the "COMn" device name of
		// the port and returns/ true.  If an error occurs, or there's
		// no port associated with the device, returns false.
		//
		// The COM port name returned is of the form "COMn", where n
		// is a number.  This is the format that most user interfaces
		// use to display available ports and accept as input to
		// select a port.  You can also use this name in many Windows
		// system calls involving COM ports, either directly as the
		// string or by extracting the number suffix.  Note that the
		// number might be more than one digit, since it's possible
		// to add quite a lot of virtual COM ports.  For CreateFile(),
		// prepend the string "\\\\.\\" to the name returned.
		bool GetCDCPort(TSTRING &name);

		// Ping the device.  This sends a Ping command to the device,
		// which can be used to test that the connection is working.
		// Returns the status code, which is PinscapeResponse::OK
		// if the connection is working.
		int Ping() { return SendRequest(PinscapeRequest::CMD_PING); }

		// Query the Pinscape Pico software version installed on the
		// device.  Returns a PinscapeResponse::OK or ERR_xxx code.
		struct Version
		{
			uint8_t major;			// major version number
			uint8_t minor;			// minor version number
			uint8_t patch;          // patch version number
			char buildDate[13];		// build date string, YYYYMMDDhhmm
		};
		int QueryVersion(Version &version);

		// Query the Pico's device ID information
		int QueryID(DeviceID &id);

		// Send a RESET command to the device.  This resets the Pico
		// hardware and restarts the Pinscape firmware program.  Returns
		// a VendorResponse status code.  The reset will drop the USB
		// connection, so the current handle must be closed after
		// this returns, and a new connection must be opened if the
		// caller wishes to continue sending commands to the same device.
		// Returns a PinscapeResponse::OK or ERR_xxx code.
		int ResetPico();

		// Reset the Pico into Safe Mode.  This resets the Pico hardware
		// and restarts the Pinscape firmware program in Safe Mode,
		// which uses an alternative config file that's meant to set up
		// a minimal hardware configuration for troubleshooting startup
		// crashes.
		int EnterSafeMode();

		// Reboot the Pico without loading any settings.  This restarts
		// the firmware with "factory" defaults for all settings.  This
		// doesn't delete any of the settings files; it simply skips
		// loading any of the available files.  On the next normal
		// reboot, the existing settings file will be loaded as usual.
		// This allows starting a session with default settings in place
		// temporarily, for the duration of the session, without any
		// permanent change to the settings files.
		int EnterFactoryMode();


		// Send an ENTER BOOT LOADER command to the device.  This resets
		// the Pico into its ROM boot loader mode.  The USB connection
		// will be dropped when the device resets, so the current handle
		// must be closed after this returns.  The Pinscape firmware
		// device will disappear from the system after the reset, to be
		// replaced by an RP2 Boot device.  This can be used to prepare
		// to load new firmware onto the device by forcing it into RP2
		// Boot mode, which will connect the RP2 Boot device's virtual
		// disk drive, which in turn accepts a UF2 file containing new
		// firmware to load into the device's flash.  Returns a
		// PinscapeResponse::OK or ERR_xxx code.
		int EnterBootLoader();

		// Erase all saved configuration data on the Pico, clearing
		// all settings to factory defaults.  This deletes the config
		// file and all other settings saved on the device, such as
		// the plunger calibration data.  The factory settings will
		// take effect on the next device reboot.
		// 
		// (This doesn't delete the firmware program.  Our "Factory
		// defaults" terminology is figurative; it only means that
		// we clear the *Pinscape* settings, not that we do a full
		// wipe of the Pico.)
		//
		// If you want to delete the JSON configuration file only,
		// use EraseConfig().  That removes the user settings, but
		// preserves internal persistent data, such as calibration
		// data.
		//
		// Returns a PinscapeResponse::OK or ERR_xxx code.
		int FactoryResetSettings();

		// Erase a device-side configuration file.  This deletes the
		// selected file, selected by a PinscapeRequest::CONFIG_FILE_xxx
		// identifier.  CONFIG_FILE_ALL can be used to erase all of the
		// config files currently stored.
		int EraseConfig(uint8_t fileID);

		// Put the configuration file.  This sends a new configuration
		// file to the device, which the device stores in flash memory,
		// overwriting any existing configuration.  The configuration
		// data is a flat character array with the text contents of the
		// config file.  Returns a PinscapeResponse::OK or ERR_xxx code.
		//
		// fileID is one of the PinscapeRequest::CONFIG_FILE_xxx
		// constants, specifying which file we're updating (normal config,
		// Safe Mode config).
		//
		// The new settings in the configuration won't go into effect
		// until the Pico is rebooted.  This function doesn't trigger a
		// reboot, in case the caller wants to update multiple files as
		// a group.  Use ResetPico() to reboot after completing updates.
		int PutConfig(const char *txt, uint32_t len, uint8_t fileID);

		// Retrieve the configuration file stored on the device.
		int GetConfig(std::vector<char> &txt, uint8_t fileID);

		// Check if a config file exists.  Returns PinscapeResponse::OK
		// if the file exists, ERR_NOT_FOUND if it doesn't exist.  Other
		// error codes can be returned if the request itself fails.
		int QueryConfigFileExists(uint8_t fileID, bool &exists);

		// Set the wall clock time on the Pico.  This sends the local
		// system time to the Pico as a reference point for its clock,
		// to enable time-of-day features.  Returns a PinscapeResponse::OK
		// or ERR_xxx code.
		int PutWallClockTime();

		// Query statistics.  Returns a PinscapeResponse::OK or ERR_xxx code.
		// If resetCounters is true, counters for rolling averages will be
		// reset, to start a new rolling-average window.
		int QueryStats(PinscapePico::Statistics *stats, size_t sizeofStats,
			bool resetCounters);

		// Synchronize clocks with the Pico.  This uses the USB SOF (Start
		// Of Frame) signal to create a fixed reference point in time shared
		// between the Windows host and the Pico, so that the two systems
		// can translate event times between their two clocks with high
		// precision.
		// 
		// On success, picoClockOffset is filled in with the time offset
		// between the Pico's system clock and the Windows high-precision
		// QueryPerformanceCounter() clock, in microseconds, such that
		// you can calculate the Pico clock time corresponding to any
		// given Windows clock time as:
		//
		//   T[Pico] = T[Windows] + picoClockOffset
		// 
		// The clock synchronization offset varies over time, because the
		// Pico and Windows clocks are based on independent frequency
		// generators, so the two clocks drift from real time at
		// different rates that are unpredictable.  The Pico clock's rated
		// accuracy is about 30 ppm, so it can drift by up to 0.1 seconds
		// per hour, or 2.5 seconds per day.  The client should therefore
		// refresh the offset from time to time, according to how much
		// precision it needs.  For millisecond-scale measurements, you
		// should refresh about once a minute.
		// 
		// Before using this function, clock synchronization must be
		// explicitly enabled via EnableClockSync().  The clock sync feature
		// adds some run-time overhead on both host and Pico, so it's
		// disabled by default.
		//
		// Returns the usual status code (OK or ERR_xxx).
		int SynchronizeClocks(int64_t &picoClockOffset);

		// Enable/disable time sync.  Clock synchronization adds a small
		// amount of run-time overhead on both the host and the Pico, since
		// each system must keep track of USB hardware frame times, so the
		// synchronization system is disabled by default to eliminate that
		// overhead for clients that don't need it.
		int EnableClockSync(bool enable);

		// Query the Pico system clock time, for synchronizing between Pico
		// timestamps and Windows timestamps.
		// 
		// On success, the values w1..p1..p2..w2 provide timestamps before
		// and after the request.  The 'w' values are the Windows times,
		// expressed in QueryPerformanceCounter ticks, and the 'p' values
		// are Pico timestamps, expressed in microseconds since the last
		// Pico CPU reset:
		// 
		//   w1 = Windows timestamp just before sending the USB request
		//   p1 = Pico timestamp upon receiving the request
		//   p2 = Pico timestamp just before sending the reply
		//   w2 = Windows timestamp upon receiving the reply
		// 
		// Because of the order of events, the following relationship
		// between these timestamps must hold, when they're all expressed
		// in common units with reference to a common epoch:
		// 
		//   w1 < p1 < p2 < w2
		// 
		// This relationship allows the host to determine the correspondence
		// between a given Windows timestamp and a given Pico timestamp to
		// the precision of the time windows, since we know for sure that
		// Pico timestamps in the interval [p1..p2] are fully enclosed within
		// the same real-time interval as the Windows timestamp interval
		// [w1..w2].  For more precise results, the caller should make
		// repeated calls, and use the result with the shortest Windows
		// interval, since this constrains the correspondence to the smallest
		// uncertainty range.  To improve results further, the caller should
		// take an average of several such shortest-window results, since
		// averaging will cancel out the random noise in individual samples.
		// 
		// The Windows and Pico clocks are independent, obviously, so they
		// will typically exhibit some skew.  This will make projections
		// of Pico times from Windows times increasingly inaccurate as the
		// elapsed time from the reference reading increases.  You can
		// correct for skew by taking readings at extended intervals and
		// using the ratio between the projected Pico time and the actual
		// observed Pico time as a correction factor.  Experimentally, the
		// skew is about one part per million, so skew correction should
		// be measured perhaps every few minutes for an extended session.
		// 
		// Returns the usual status code (OK or ERR_xxx).
		int QueryPicoSystemClock(int64_t &w1, int64_t &w2, uint64_t &p1, uint64_t &p2);

		// Query the USB interface configuration.  Returns a status code
		// indicating success or failure.
		int QueryUSBInterfaceConfig(PinscapePico::USBInterfaces *ifcs, size_t sizeofIfcs);

		// Query the GPIO configuration
		struct GPIOConfig
		{
			// port function
			enum class Func
			{
				XIP = 0,     // Execute-in-place controller (serial flash interface)
				SPI = 1,     // SPI bus controller
				UART = 2,    // UART serial port
				I2C = 3,     // I2C bus controller
				PWM = 4,     // PWM controller
				SIO = 5,     // direct input/output port
				PIO0 = 6,    // PIO unit 0
				PIO1 = 7,    // PIO unit 1
				GPCK = 8,    // Clocks (special per port, GP20-GP25)
				USB = 9,     // USB controller
				NONE = 0x1F, // No function
			};
			Func func = Func::NONE;

			// SIO direction, applicable to Func::SIO ports only; 
			// true -> output, false -> input
			bool sioIsOutput = false;

			// Usage description.  This is human-readable string, intended 
			// for display in a UI, describing the assigned function for 
			// the port, based on the JSON configuration settings.
			std::string usage;
		};
		int QueryGPIOConfig(GPIOConfig gpio[30]);

		// Query flash file system information.  The firmware sets up a
		// miniature file system on the Pico's flash storage space, to
		// manage its persistent data (configuration files, settings,
		// calibration data, etc).  This query retrieves basic information
		// on the file system layout, which is mostly useful for backing
		// up the data on the host via the flash sector transfer function.
		//
		// On return, info->fileSysStartOffset and info->fileSysByteLength
		// provide the bounds of the flash data storage area, which is a
		// single contiguous block of storage within the flash.
		// 
		// info->flashSize provides the overall flash size.  This is
		// reliable only when F_FLASH_SIZE_KNOWN is set in the flags.
		// If the flag isn't set, the size is set to 2MB by default, since
		// that's the flash size used in the reference Pico manufactured
		// by Raspberry Pi.  The firmware attempts to determine the actual
		// size of the installed flash by interrogating the flash chip,
		// but there's no guarantee that the interrogation mechanism is
		// implemented on every flash chip - it's one of those industry
		// standards that most manufacturers adhere to but some might
		// not.  I expect that 2MB is a safe *minimum* flash size for
		// every Pico, since all of the clones I've seen have at least
		// that much.  (Excepting some bare-bones RP2040 boards that
		// don't have flash installed at all.  But those probably aren't
		// important for our purposes, since they probably can't run
		// Pinscape anyway.)
		// 
		// Returns a PinscapeResponse::OK or ERR_xxx code.
		int QueryFileSysInfo(PinscapePico::FlashFileSysInfo *info, size_t sizeofInfo);

		// Read a sector from the Pico's flash memory space.  Transfers
		// one sector of data from the Pico to the host, starting at the
		// given offset in flash; the first byte of flash is at offset 0.
		// Returns a PinscapeResponse::OK or ERR_xxx code.  
		// ERR_OUT_OF_BOUNDS means that the address was beyond the end
		// of the flash space.
		int ReadFlashSector(uint32_t ofs, std::vector<uint8_t> &sector);

		// Query the device log.  The device collects error messages and 
		// informational messages in a text buffer that can be retrieved
		// for display to the user.  The device maintains a circular buffer,
		// overwriting older messages as new messages arrive, so the
		// available text is always the most recent, up to the limit.
		// The device buffer size can be configured in the JSON settings.
		//
		// Each call retrieves as much text as possible, up to a fixed 
		// USB transfer limit per call.  'totalAvailable' is filled in on
		// return with the number of bytes available in the device buffer,
		// including the current transfer and anything beyond the transfer
		// limit.  A caller who only wants to retrieve what's available at
		// a given moment in time can use the totalAvailable value returned
		// on the first call to limit subsequent requests, stopping when
		// the original total has been obtained; this prevents getting
		// stuck in a loop retrieving new text that has been newly added
		// since the last call.  Callers providing an interactive display
		// probably *want* to keep adding newly available text as it's
		// added, in which case they can ignore totalAvailable and just
		// keep asking for and displaying new text as it arrives.
		// A null pointer can be passed for totalAvailable if the caller
		// doesn't need this information.
		//
		// If the call succeeds, but no text is available, the
		// result code is PinscapeReply::ERR_EOF.
		int QueryLog(std::vector<uint8_t> &text, size_t *totalAvailable = nullptr);

		// Send an IR command
		int SendIRCommand(const IRCommand &irCmd, int repeatCount = 1);

		// Query recent IR commands received.  The device collects IR
		// commands received in a ring buffer, which can be queried with
		// this request.  This returns all buffered commands and clears
		// the buffer, so the next call will only see new commands since
		// this call.  The device discards older commands as new commands
		// arrive when the buffer is full, so the host will always see
		// the most recent commands at the time of the call.
		int QueryIRCommandsReceived(std::vector<IRCommandReceived> &commands);

		// Query recent raw IR pulses.  The device collects the sequence
		// of raw IR timed pulses in an internal buffer, which can be
		// queried with this request.  This request removes the returned
		// pulses from the buffer on the device, so the next call will
		// only see new pulses that have arrived since.  The device
		// clears the entire buffer each time a new series of pulses
		// starts after a long quiet period.
		struct IRRawPulse
		{
			int t = 0;          // duration in microseconds; -1 means longer than the maximum (130ms)
			bool mark = false;  // true -> "mark" (IR carrier on), false -> "space" (IR carrier off)
		};
		int QueryIRRawPulsesReceived(std::vector<IRRawPulse> &pulses);

		// Query the TV ON state
		struct TVONState
		{
			// Power-sense state.  This is one of the 
			// PinscapePico::VendorResponse::Args::TVON::PWR_xxx
			// constants defined in USBProtocol/VendorIfcProtocol.h.
			unsigned int powerState = 0;

			// State name.  This is a descriptive name for the state,
			// provided as a convenience for callers that only need
			// the state for UI display purposes.
			std::string powerStateName;

			// Power-sense GPIO state.  If the power-sensing circuit
			// is configured, this provides the current GPIO state of
			// the SENSE input pin, false for low, true for high.  The
			// SENSE pin indicates the state of the "latch" that the
			// sensor circuit uses to detect when a power transition
			// occurs.
			bool gpioState = false;

			// Relay state.  This is true if the relay is on for any
			// reason, false if it's currently off.
			bool relayState = false;

			// Relay state sources.  These are the states of the
			// sources that can individually turn the relay ON; if any
			// of these are ON, the relay is ON.  The "Power On" source
			// is the automatic power-sense system; this is ON during
			// the relay pulse generated after an OFF->ON power state
			// transition is detected on the power-sense circuit.  The
			// "Manual" source is the manual on/off USB command, via
			// SetTVRelayManualState() in this interface, or the similar
			// function in the Feedback Controller HID interface.  The
			// "Manual Pulse" source is the manual timed pulse generated
			// through the USB command, via PulseTVRelay() in this
			// interface or the similar Feedback Controller command.
			bool relayStatePowerOn = false;
			bool relayStateManual = false;
			bool relayStateManualPulse = false;

			// Curent IR command, as an index into the configured list
			// of commands to send when a power-on transition occurs,
			// starting at 0 for the first configured command.  This
			// is only valid during the IR command send/wait states.
			unsigned int irCommandIndex = 0;

			// Number of IR commands in the configured list of commands
			// to send when a power-on transition occurs.
			unsigned int irCommandCount = 0;
		};
		int QueryTVONState(TVONState &state);

		// Set the TV relay's manual on/off state.  This provides a
		// channel of control over the relay separate from the automatic
		// power-sense countdown system.  When the manual state is ON,
		// the relay is ON.  When the manual state is OFF, the relay is
		// on or off according to the power-sense state.  Note that
		// setting MANUAL OFF doesn't override the power-sense pulse;
		// the manual mode is *in addition to* the automatic mode.
		// This is intended for testing purposes, to let the user
		// manually activate the relay to verify its operation.
		int SetTVRelayManualState(bool on);

		// Pulse the TV relay manually.  This is yet another channel
		// of control over the relay, separate from the automatic
		// power-sense countdown pulse and the MANUAL ON control.
		// This pulses the relay ON for the interval programmed in
		// the configuration file.  This can be used for testing
		// purposes, as well as for host-side control over the TV
		// power state.  For example, it can be used to turn on the
		// TV from a Windows startup script.
		int PulseTVRelay();

		// Move nudge device calibration.  This starts a timed data
		// collection period where the nudge device collects readings
		// from the accelerometer to measure the typical noise range
		// when the device is stationary.  The accelerometer shouldn't
		// be moved or disturbed during the calibration period, since
		// the point is to measure the typical range of readings when
		// the device is still.  The nudge device uses these readings
		// to set the motion thresholds for auto-centering.
		//
		// If autoSave is true, the new readings are automatically
		// committed to flash, along with all other in-memory settings,
		// at the end of the calibration process.  If false, the new
		// settings are only stored in volatile RAM, so they'll be
		// lost on a device reboot or on an explicit "revert" command.
		// If auto-save isn't selected, the new settings can be manually
		// saved via a "commit" command.
		int StartNudgeCalibration(bool autoSave);

		// Set the nudge device "center point" to the average of recent
		// readings.  This takes the average over the last few seconds,
		// and sets it as the zero point in the X/Y plane, and the +1g
		// point on the Z axis.  This is the same data used to set the
		// center point automatically when auto-centering is enabled;
		// this request simply applies the centering immediately without
		// waiting for a period of stillness to be measured.  There's
		// nothing special about this new manual center point, and in
		// particular, it doesn't prevent the auto-centering mechanism
		// from resetting the center point again the next time it
		// detects a period of stillness.
		int SetNudgeCenterPoint();

		// Query nudge device status
		int QueryNudgeStatus(PinscapePico::NudgeStatus *stat, size_t statSize);

		// Query/put nudge device parameter settings.  On a "put", the
		// new parameters go into effect in the live readings, but 
		// they're only stored in volatile RAM on the Pico, so they'll
		// be lost on the next Pico reset or on an explicit "revert" 
		// command.  Use a "commit" call after the "put" to save the
		// new settings to flash on the device.  The point of separating
		// "put" and "commit" is that it allows the user to test out new
		// settings temporarily, without overwriting the old settings;
		// if the new settings aren't satisfactory, the old ones can be
		// restored with a "revert" command.
		int QueryNudgeParams(NudgeParams *params, size_t paramsSize);
		int PutNudgeParams(const NudgeParams *params, size_t paramsSize);

		// Commit/revert the nudge settings, including the noise calibration
		// readings and the cabinet position model parameters.  
		int CommitNudgeSettings();
		int RevertNudgeSettings();

		// Query the plunger configuration.  This retrieves the data
		// in the form of a PinscapePico::PlungerConfig struct.
		// Returns a PinscapeResponse::OK or ERR_xxx code.
		int QueryPlungerConfig(PinscapePico::PlungerConfig &config);

		// Query a plunger reading.  The reply consists of a fixed-size
		// struct, PinscapePico::PlungerData, and optional additional data
		// that varies by sensor.  On success, the function fills in the
		// caller's PlungerData struct with the fixed part, and passes back
		// any additional sensor-specific data in the vector.  Returns a
		// PinscapeResponse::OK or ERR_xxx code.
		int QueryPlungerReading(PinscapePico::PlungerReading &reading, std::vector<BYTE> &sensorData);

		// Set the adjustable plunger settings.  These put the settings
		// into effect immediately for the the live plunger readings, but
		// don't save them to flash - they're stored only in volatile RAM
		// on the Pico until explicitly commited via CommitPlungerSettings().
		// This allows the user to experiment with different settings,
		// observing their effects on the plunger readings and joystick
		// inputs, before replacing previously saved settings.  The last
		// saved settings can be restored at any time, replacing the
		// temporary in-memory settings, via RevertPlungerSettings().  The
		// settings automatically revert to the last settings saved in
		// flash on a Pico reset.
		//
		//  These return the usual PinscapeResponse::OK or ERR_xxx codes. 
		int SetPlungerJitterFilter(int windowSize);
		int SetPlungerFiringTime(uint32_t maxFiringTime_us);
		int SetPlungerIntegrationTime(uint32_t integrationTime_us);
		int SetPlungerScalingFactor(uint32_t scalingFactor);
		int SetPlungerOrientation(bool reverseOrientation);
		int SetPlungerScanMode(uint8_t mode);

		// Set plunger calibration data.  This can be used to restore
		// calibration data previously saved on the host (which can be
		// obtained from a QueryPlungerReading() request).  As with the
		// adjustable settings, the calibration set by this request is 
		// only stored in volatile RAM, until commited to flash via
		// CommitPlungerSettings().
		//
		// Returns PinscapeResponse::OK on success, or an ERR_xxx code
		// on failure.
		int SetPlungerCalibrationData(const PinscapePico::PlungerCal *data, size_t sizeofData);

		// Move plunger calibration.  The calibration process runs on a
		// timer once initiated, gathering data over the timed period and
		// putting the new calibration into effect when the period ends.
		// 
		// If autoSave is false, the new calibration is only stored in 
		// memory until explicitly saved to flash via CommitPlungerSettings().
		// This allows the user to test the effect of the calibration before 
		// committing it to flash.  If autoSave is true, the new calibration
		// readings are committed to flash when the timed calibration period
		// completes, along with the current values of all of the other
		// adjustable settings.
		// 
		// Returns the usual PinscapeResponse::OK or ERR_xxx codes. 
		int StartPlungerCalibration(bool autoSave);

		// Commit/revert plunger settings and calibration data.  The plunger
		// settings for jitter filtering, integration time, orientation,
		// scaling factor, and firing time limit, as well as the calibration
		// readings, are stored in temporary working memory on the device
		// until explicitly committed to flash.  This allows the user to
		// experiment with different settings, observing their effects on
		// the readings and gamepad axis inputs to the PC, before overwriting
		// the last saved changes - it's the same idea as editing a Word
		// document, where changes only apply to a temporary in-memory
		// working copy until explicitly saved to the file copy.  Saving
		// commits the in-memory working settings to flash, making them
		// the new persistent settings.  Reverting restores the settings
		// last saved to flash and makes them the active working settings.
		// These functions return PinscapeResponse::OK or ERR_xxx codes.
		int CommitPlungerSettings();
		int RevertPlungerSettings();

		// Query the button configuration.  Populates the vectors of
		// ButtonDesc and ButtonDevice structs.  The ButtonDesc structs
		// describe the logical button assignments, and the ButtonDevice
		// structs describe the physical input devices.
		int QueryButtonConfig(
			std::vector<PinscapePico::ButtonDesc> &buttons,
			std::vector<PinscapePico::ButtonDevice> &devices);

		// Query the logical button states.  The returned vector gives
		// the ON/OFF state of each configured logical button as a byte
		// value, 0 for OFF, 1 for ON.  The order is the same as the
		// button descriptors returned from QueryButtonConfig().
		int QueryLogicalButtonStates(std::vector<BYTE> &states, uint32_t &shiftState);

		// Query a GPIO button event log.  Buttons mapped to GPIO sources
		// can be configured (in the JSON config file) to log the Pico
		// microsecond clock time of each on/off transition on the GPIO
		// pin.  GPIO inputs are uniquely suited for precise button event
		// time measurement on the Pico because of their ability to
		// generate CPU interrupts on voltage level changes, which the
		// Pico can service with very low latency, on the order of 200ns.
		// When a GPIO-source button is configured for event logging,
		// Pinscape uses an interrupt handler to collect a timestamp on
		// the Pico microsecond clock for each change in the logic level
		// input on the button's input GPIO pin.  This request retrieves
		// the log  of recent events for a given GPIO, allowing a Windows
		// program to correlate physical changes in the button state with
		// the corresponding HID reports received on the host.  This in
		// turn can be used to measure the latency between a physical
		// button press and the HID input reaching the pinball simulator
		// on the PC side.  (Computing times between Pico and Windows
		// events requires a common clock reference point, which can be
		// obtained via QueryPicoSystemClock().)
		// 
		// Returns a status code (OK or ERR_xxx).  On success, the event
		// vector is populated with the most recent events for the GPIO,
		// ordered from oldest to newest.  Timestamps are in terms of the
		// Pico's system clock, which reads in microseconds since the
		// last Pico CPU reset.  The events returned are removed from
		// the Pico's internal buffer, so each call only returns new
		// events that occurred after the last call.  The Pico's buffer
		// uses a fixed size; when the buffer is full, the oldest event
		// is discarded to make room for each new event.  When the
		// host application first starts, it will thus see the most
		// recent events, no matter how long the Pico has been running.
		int QueryButtonEventLog(std::vector<PinscapePico::ButtonEventLogItem> &events, int gpioNumber);

		// Clear the GPIO button event log for a given GPIO.  This
		// deletes all logged events on the Pico for the given GPIO.
		// Passing gpioNumber == 255 clears the logs for all GPIOs.
		int ClearButtonEventLog(int gpioNumber);

		// Query the button states of the physical button input devices.
		// Each function retrieves a byte vector giving the states of
		// all of the input ports for the given device type.
		//
		// For the GPIO states, the vector has one byte for each Pico
		// GPIO, in numbered order from 0.  All of the GPIO ports are
		// included, whether or not they're configured as buttons, to
		// make the mapping from array index to port number easy.  For
		// the other devices, the ports are in the order they appear
		// in the device descriptor lists from QueryButtonConfig().
		// As with the GPIO ports, all of the PCA9555 and 74HC165
		// ports across all chips are included in the vector, whether
		// or not the ports are actually assigned to logical buttons,
		// to make the mapping from array index to descriptor easy.
		int QueryButtonGPIOStates(std::vector<BYTE> &states);
		int QueryButtonPCA9555States(std::vector<BYTE> &states);
		int QueryButton74HC165States(std::vector<BYTE> &states);

		// Set an output port logical level
		int SetLogicalOutputPortLevel(uint8_t portNumber, uint8_t level);

		// Set a physical output device port PWM level.  This should
		// only be used when Output Test Mode is engaged, because the
		// Output Manager might otherwise countermand any change made
		// here on its next periodic update.  
		// 
		// The device type is a PinscapePico::OutputPortDesc::DEV_xxx
		// constant.  configIndex is the index of the device within
		// the list of like devices in the JSON configuration.  The
		// device port is the port number relative to the configuration
		// object.  For individually addressable chips (PCA9685, PCA9555,
		// TLC59116), this is the output port number on the chip,
		// starting with port 0.  For daisy-chained devices (TLC5940,
		// 74HC595), this is the port number on the overall chain,
		// starting at 0 for the first port on the first chip in the
		// chain (the one directly connected to the Pico).  a
		int SetPhysicalOutputPortLevel(
			uint8_t deviceType, uint8_t configIndex,
			uint8_t port, uint16_t pwmLevel);

		// Query the output port configuration.  Populates a vector
		// of output port descriptors, providing the list of logical
		// output ports.  The logical ports are the ports that host
		// programs such as DOF can access via the USB Feedback
		// Controller interface.  The JSON configuration defines the
		// mapping between logical ports and physical devices.
		int QueryLogicalOutputPortConfig(std::vector<PinscapePico::OutputPortDesc> &ports);

		// Query the output device configuration.  Populates a
		// vector of output device descriptors, providing the list
		// of configured physical devices that can be mapped to
		// output ports.  The physical devices are the peripheral
		// chips attached to the Pico that can be used to control
		// external feedback devices: PWM controller chips (TLC5940,
		// TLC59116, PCA9685), shift registers (74HC595), GPIO
		// extenders (PCA9555).  Note that the logical output ports
		// can also be mapped directly to Pico GPIO ports, but the
		// device list returned here doesn't include an entry for
		// the GPIO ports, because those are a fixed feature of the
		// Pico, so we normalize them out of the list.
		int QueryOutputDeviceConfig(std::vector<PinscapePico::OutputDevDesc> &devices);

		// Query the physical output device port configuration.
		// Populates a vector of device port descriptors.  The
		// ports are listed in the same order as the device
		// descriptors from QueryOutputDeviceConfig(), so the
		// association from ports to devices can be determined by
		// traversing the lists in parallel.
		int QueryOutputDevicePortConfig(std::vector<PinscapePico::OutputDevPortDesc> &devicePorts);

		// Query the logical output port levels.  Populates a
		// vector of output level structs, providing the current
		// state of each port.  Also indicates if test mode is
		// active.
		int QueryLogicalOutputLevels(bool &testMode, std::vector<PinscapePico::OutputLevel> &levels);

		// Query physical output device port levels.  Populates a
		// vector of output level structs, providing the current
		// state of each physical port.  The ports follow the order
		// of the descriptors returned by QueryOutputDeviceConfig(),
		// so the caller should use the descriptor list to relate
		// the level list to the output devices.
		//
		// The Pico's own GPIO ports are returned separately (not
		// in the generic port list), because they're not included
		// in the descriptor list, and because they have the unique
		// feature that each GPIO port can be configured as a PWM
		// or digital output, or as an input or output.
		int QueryPhysicalOutputDeviceLevels(std::vector<PinscapePico::OutputDevLevel> &levels);

		// Set output test mode.  In test mode, the logical output
		// ports are disconnected from the physical output ports,
		// allowing the host to test physical device ports directly.
		// When in test mode, logical port updates from the host are
		// ignored, including DOF commands, and computed ports (based
		// on the dataSource in the JSON configuration) are disabled.
		// Port timers (Flipper Logic and Chime Logic) are also
		// disabled, since those apply to the logical ports.
		// 
		// Test mode is useful for testing and troubleshooting a
		// new setup, since it lets the user access the physical
		// device ports directly.  This makes it easier to isolate
		// problems with ports that aren't responding to DOF 
		// commands as expected, since it bypasses the USB Feedback
		// Controller interface, Output Manager logic, and logical
		// port configuration settings.  If a port still doesn't
		// respond to direct operation in test mode, the problem
		// is likely in the wiring or in the physical controller
		// chip, or in the firmware's low-level device driver
		// software.
		//
		// The timeout sets a time limit when entering test mode,
		// in milliseconds; zero enables the mode indefinitely.
		// The timeout is meant to ensure that the device returns
		// to normal operation if the host program exits abnormally,
		// or simply fails (due to an oversight in its programming)
		// to cancel test mode before terminating.  If possible,
		// use a short timeout, on the order of a few seconds, and
		// then repeat the command periodically, within the timeout
		// period, to keep the mode active as long as needed.  Each
		// new command will reset the timeout from the moment the
		// command is received (timeouts aren't cumulative).   The
		// timeout is ignored when exiting test mode.
		//
		// Caution: suspending output management disables Flipper
		// Logic and computed outputs.  Be careful with devices that
		// depend on Flipper Logic to limit activation time, since
		// the time limiter won't function during suspensions.
		int SetOutputTestMode(bool testMode, uint32_t timeout_ms);



		// ------------------------------------------------------------
		//
		// Low-level access - these functions directly access the USB
		// pipe with the byte protocol.
		//
		 
		// Send request with optional IN and OUT data.  If xferOutData
		// is not null, it contains OUT data (device-to-host) for the
		// request, which will be sent immediately after the command
		// packet.  If xferInData is not null, the vector will be
		// populated with the IN data (host-to-device) that the device
		// returns in its response to the request.
		int SendRequest(uint8_t cmd,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr);

		// Send a request, capturing the response struct
		int SendRequest(uint8_t cmd, PinscapeResponse &response,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr);

		// Send a request with arguments and optional OUT data
		template<typename ArgsType> int SendRequestWithArgs(
			uint8_t cmd, ArgsType &args,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr)
		{
			// build the request struct
			PinscapeRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));
			req.argsSize = sizeof(args);
			memcpy(&req.args, &args, sizeof(ArgsType));

			// send the request
			return SendRequest(req, xferOutData, xferInData);
		}

		// Send a request with arguments in and out, and optional OUT data
		template<typename ArgsType> int SendRequestWithArgs(
			uint8_t cmd, ArgsType &args, PinscapeResponse &resp,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr)
		{
			// build the request struct
			PinscapeRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));
			req.argsSize = sizeof(args);
			memcpy(&req.args, &args, sizeof(ArgsType));

			// send the request
			return SendRequest(req, resp, xferOutData, xferInData);
		}

		// Send a request with a pre-built request struct
		int SendRequest(const PinscapeRequest &request,
			const BYTE *xferOutData = nullptr, std::vector<BYTE> *xferInData = nullptr);

		// Send a request with pre-built request struct, capturing the response
		int SendRequest(const PinscapeRequest &request, PinscapeResponse &resp,
			const BYTE *xferOutData = nullptr, std::vector<BYTE> *xferInData = nullptr);

		// Get the USB device descriptor
		HRESULT GetDeviceDescriptor(USB_DEVICE_DESCRIPTOR &desc);

		// Raw data read/write.  These can be used to transfer arbitrary
		// binary data to/from the device via the vendor interface bulk data
		// endpoints.
		// 
		// The timeout specifies a maximum wait time in milliseconds before
		// the operation is aborted.  INFINITE can be used to wait definitely,
		// but this should be avoided, since USB connections are by design
		// subject to termination at any time.  Using timeouts for blocking
		// calls like this help make the calling program more robust by
		// ensuring it won't hang if the device is removed in the middle of
		// an operation.
		HRESULT Read(BYTE *buf, size_t bufSize, size_t &bytesRead, DWORD timeout_ms = INFINITE);
		HRESULT Write(const BYTE *buf, size_t len, size_t &bytesWritten, DWORD timeout_ms = INFINITE);

		// flush the pipe
		void FlushRead();
		void FlushWrite();

		// reset the pipes
		void ResetPipes();

		// Get the underlying device handle
		HANDLE GetDeviceHandle() const { return hDevice; }

		// Is the device handle valid?  This tests if the device has an
		// open handle: the original creation succeeded, and the handle
		// hasn't been explicitly closed.  This doesn't attempt any
		// operations on the handle, so the handle might be open but no
		// longer connected to the physical device, such as after the
		// device has been unplugged.
		bool IsDeviceHandleValid() const { return hDevice != NULL && hDevice != INVALID_HANDLE_VALUE; }

		// Close the underlying device handle.  This can be used if the
		// application receives notification that the device has been
		// disconnected via WM_DEVICECHANGE.
		void CloseDeviceHandle();

		// WinUSB device interface GUID for the Pinscape Pico vendor interface.
		// This is the custom GUID that the Pinscape firmware exposes via its
		// USB MS OS descriptors, which tell Windows to allow WinUSB access to
		// the device.  This GUID can be used to enumerate currently connected
		// Pinscape devices via their WinUSB vendor interfaces.  (It's also
		// possible to enumerate Pinscape devices via the generic WinUSB GUID,
		// but that's less selective, since it finds all devices that expose
		// WinUSB vendor interfaces.  The custom GUID selects only Pinscape
		// devices.)
		static const GUID devIfcGUID;

	protected:
		VendorInterface(HANDLE hDevice, WINUSB_INTERFACE_HANDLE winusbHandle,
			const WSTRING &path, const WSTRING &deviceInstanceId, const WCHAR *serialNum,
			UCHAR epIn, UCHAR epOut) :
			hDevice(hDevice),
			winusbHandle(winusbHandle),
			path(path),
			deviceInstanceId(deviceInstanceId),
			serialNum(serialNum),
			epIn(epIn),
			epOut(epOut)
		{ }

		// Windows file handle and WinUSB device handle to an open device
		HANDLE hDevice = NULL;

		// WinUSB handle to the device
		WINUSB_INTERFACE_HANDLE winusbHandle = NULL;

		// USB SOF time tracking handle
		HANDLE timeTrackingHandle = NULL;

		// File system path of the device
		WSTRING path;

		// Device Instance ID.  This is a unique identifier for the device
		// assigned by Windows.
		WSTRING deviceInstanceId;

		// Serial number string reported by the device
		WSTRING serialNum;

		// Endpoints
		UCHAR epIn = 0;
		UCHAR epOut = 0;

		// Common request timeout, in milliseconds.  We use this for most
		// requests as the write timeout.
		static const DWORD REQUEST_TIMEOUT = 3000;

		// Next request token.  We use this to generate a unique token for
		// each request within our session, for correlation of responses to
		// requests.  We simply increment this on each request.  To help
		// distinguish our requests from any replies in the pipe from
		// previous application sessions, we start it at the system tick
		// count.  This gives us a somewhat random starting value that's
		// likely to be unique among recent application invocations.  This
		// isn't truly unique, of course, since the tick counter rolls over
		// every 49 days, but it's still very unlikely to match a recent
		// request token.
		uint32_t token = static_cast<uint32_t>(GetTickCount64());

		// TV ON state names
		std::unordered_map<int, std::string> tvOnStateNames{
			{ PinscapeResponse::Args::TVON::PWR_OFF, "Power Off" },
			{ PinscapeResponse::Args::TVON::PWR_PULSELATCH, "Pulsing Latch" },
			{ PinscapeResponse::Args::TVON::PWR_TESTLATCH, "Testing Latch" },
			{ PinscapeResponse::Args::TVON::PWR_COUNTDOWN, "Countdown" },
			{ PinscapeResponse::Args::TVON::PWR_RELAYON, "Pulsing Relay" },
			{ PinscapeResponse::Args::TVON::PWR_IRREADY, "IR Ready" },
			{ PinscapeResponse::Args::TVON::PWR_IRWAITING, "IR Waiting" },
			{ PinscapeResponse::Args::TVON::PWR_IRSENDING, "IR Sending" },
			{ PinscapeResponse::Args::TVON::PWR_ON, "Power On" },
		};

		// Overlapped I/O tracker.  This encapsulates all of the resources
		// associated with an overlapped read or write: the transfer buffer
		// and the system OVERLAPPED struct.  These resources must remain
		// valid until the I/O completes, so they must be heap-allocated,
		// and can't be released until the OVERLAPPED event reports SET
		// state.
		struct IOTracker
		{
			IOTracker(VendorInterface *ifc, size_t bufSize) : 
				ov(ifc->hDevice, ifc->winusbHandle)
			{ 
				buf.resize(bufSize); 
			}

			IOTracker(VendorInterface *ifc, const uint8_t *data, size_t dataSize) :
				ov(ifc->hDevice, ifc->winusbHandle)
			{
				buf.resize(dataSize);
				memcpy(buf.data(), data, dataSize);
			}

			// has the I/O completed?
			bool IsCompleted() const { return WaitForSingleObject(ov.ov.hEvent, 0) == WAIT_OBJECT_0; }

			// transfer buffer
			std::vector<uint8_t> buf;

			// OVERLAPPED struct
			OVERLAPPEDHolder ov;
		};

		// Unresolved I/O transactions.  If an I/O times out, we'll
		// throw it on this list, to keep the resources associated with
		// the transaction alive until the I/O finally resolves, if it
		// ever does.  The WinUsb driver doesn't support synchronous
		// cancellation, so if we run out of time waiting for an I/O
		// to complete, we can't abandon the resources associated with
		// the I/O, because the WinUsb driver will retain pointers to
		// the resources until the I/O resolves, and could write into
		// that memory space at any time in the future.
		std::list<std::unique_ptr<IOTracker>> timedOutIOs;

		// next list cleanup time
		UINT64 tCleanUpTimedOutIOs = 0;

		// Try cleaning up the timed-out I/O list.  If 'now' is true,
		// we'll do the clean up immediately, no matter how long it's
		// been since the last pass.  Otherwise, we'll only attempt
		// the cleanup periodically.
		void CleanUpTimedOutIOs(bool now);
	};
}
