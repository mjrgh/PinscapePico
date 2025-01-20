// Pinscape Pico Button Latency Tester II - Vendor Interface API
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This provides C++ access to the Button Latency Tester II Vendor
// Interface.  The Vendor Interface is designed to be accessed through
// WinUsb on Windows.  Programs can access it directly through its
// documented protocol, but this API is meant to make it easier to
// use by hiding the details of managing the USB connection, and
// providing a function interface instead of a network-style byte
// stream interface.
// 
// This interface code is part of the Windows host tool for the
// Button Latency Tester II project, but it's designed to be fairly
// easy to separate out to import into other projects.  The reason
// you might wish to do so is that it would allow you to measure the
// end-to-end latency for a PARTICULAR APPLICATION, according to the
// specific ways that the application uses the Windows input APIs.
// Windows has many redundant APIs for accessing each type of input,
// largely because Microsoft saw ways to improve performance and/or
// capabilities as PC hardware evolved over the decades.  Some of
// the APIs are therefore faster than others for carrying out the
// same task.  Application developers might find it useful to be
// able to gather hard data to compare the performance of different
// APIs for their applications' input needs.
// 
// To access the vendor interface, the first step is to enumerate
// available devices:
// 
//    std::list<ButtonLatencyTester2::VendorInterface::VendorInterfaceDesc> devices;
//    HRESULT hr = ButtonLatencyTester2::VendorInterface::EnumerateDevices(devices);
//    if (SUCCEEDED(hr))
//       ...
//
// Now you can open a device from the enumeration list:
// 
//    ButtonLatencyTester2::VendorInterface *vi = nullptr;
//    hr = devices.front().Open(vi);
//    if (SUCCEEDED(hr))
//       ...
// 
//
// Linux and MacOS hosts can access the protocol through libusb.
// libusb's programming interface is very similar to WinUsb's, so
// this code should be relatively easy to adapt, for those who wish
// to port it to non-Windows platforms.  The reason we didn't just use
// libusb to start with is that WinUsb provides better integration on
// Windows, in that it doesn't require any user action to install or
// configure a device driver; it provides plug-and-play device setup
// that's as transparent to the user as a HID device.

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <tchar.h>
#include <Windows.h>
#include <winusb.h>
#include <usb.h>
#include <timeapi.h>
#include "../USBProtocol.h"

namespace ButtonLatencyTester2
{
	using WSTRING = std::wstring;
	using TSTRING = std::basic_string<TCHAR>;
	class VendorInterfaceDesc;

	// Pico hardware ID
	struct PicoHardwareId
	{
		PicoHardwareId() { memset(b, 0, sizeof(b)); }
		PicoHardwareId(const uint8_t src[8]) { memcpy(this->b, src, 8); }

		// Clear to all zeroes.  This serves as a null ID.  (There's
		// no documentation anywhere saying that all-zeroes is actually
		// a reserved ID that will never be used in a device, but it
		// seems incredibly unlikely that it would be, and contrary to
		// all industry conventions and common sense.)
		void Clear() { memset(b, 0, sizeof(b)); }

		// The 8 bytes of the ID.  Note that these are binary byte
		// values, each 0x00..0xFF - they're NOT printable ASCII or
		// anything like that.
		uint8_t b[8];

		// Get the string representation of the ID, as a series
		// of 16 hex digits.
		std::string ToString() const;

		// match against another hardware ID object
		bool operator ==(const PicoHardwareId &other) const { return memcmp(b, other.b, sizeof(b)) == 0; }
	};

	// Button Latency Tester II device ID.  This is a collection of 
	// identifiers that can be used to select a device or display
	// device listings.
	struct DeviceID
	{
		// The Pico's hardware ID.  This is a universally unique 
		// 64-bit value that's assigned to each Pico at the factory
		// and burned into ROM.  This ID is immutable and unique for
		// each Pico, so it can be used to identify a physical Pico
		// across firmware updates, USB port changes, etc.
		PicoHardwareId hwid;

		// CPU type: 2040 -> RP2040, 2350 -> RP2350
		uint16_t cpuType = 0;

		// CPU version
		uint8_t cpuVersion = 0;

		// ROM version
		uint8_t romVersion = 0;

		// ROM version name, per the nomenclature used in the Pico SDK
		std::string romVersionName;

		// Pico SDK version string.  This gives the version of the
		// SDK used to build the firmware.  The firmware just passes
		// through the version string defined by the SDK headers, so
		// the format matches whatever the SDK headers use, which
		// currently is the ubiquitous "Major.Minor.Patch" format.
		std::string picoSDKVersion;

		// TinyUSB library version, of the form "Major.Minor.Patch".
		std::string tinyusbVersion;

		// Compiler version string, in the form "CompilerName X.Y.Z".
		// This is the name and version of the compiler used to build
		// the firmware, which might be useful for diagnostics and
		// troubleshooting, to determine the provenance of a particular
		// build.  The current SDK toolchain is based on the GNU tools,
		// with compiler name GNUC.
		std::string compilerVersion;

		// Build-target board name.  This is the internal symbol
		// used in the Pico SDK to identify the target board for
		// the firmware build.  This is only the TARGET board,
		// meaning the board that the firmware was configured for
		// during compilation.  This doesn't necessarily reflect
		// the "live" board type that the software is actually
		// running on, since there might be compatible clones for
		// any given board type that will successfully run code
		// that was nominally configured for another board.  For
		// example, this won't distinguish you whether the firmware
		// is running on an original Pico from Raspberry Pi or one
		// of the compatible clones made by Adafruit or Sparkfun.
		std::string targetBoardName;

		// Get a friendly version of the board name for display
		// purposes.  The board name in targetBoardName is an
		// internal identifier used in the SDK, designed for use
		// by the build software, so it's not necessarily in a
		// human-friendly display format.  This reformats the
		// name to make it friendlier for display.
		std::string FriendlyBoardName() const;
	};

	// vendor interface
	class VendorInterface
	{
		friend class VendorInterfaceDesc;

	public:
		~VendorInterface();

		// Enumerate all currently attached BLT-II Pico devices. 
		// Populates the path list with the file system path names of 
		// the currently attached devices that provide the BLT-II USB
		// vendor interface.  Returns an HRESULT indicating if the
		// operation was successful.
		static HRESULT EnumerateDevices(std::list<VendorInterfaceDesc> &deviceList);

		// WinUSB device interface GUID for the Button Latency Tester 
		// vendor interface.  This is the custom GUID that the BLT-II 
		// firmware exposes via its USB MS OS descriptors, which tell 
		// Windows to allow WinUSB access to the device.  This GUID can 
		// be used to enumerate currently connected BLT-II devices via 
		// their WinUSB vendor interfaces.
		static const GUID devIfcGUID;

		// Get the USB device serial number
		const wchar_t *GetSerialNumber() const { return serialNum.c_str(); }

		// Find the CDC (virtual COM) port associated with this device.
		// The firmware exposes a virtual COM port for logging messages
		// and command console access.
		// 
		// If the device exposes a CDC port, this fills in 'name' with
		// the COMn port name string and returns true.  If no CDC port
		// is found (or any error occurs), the function returns false.
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
		bool GetCDCPort(TSTRING &name) const;

		// Get the error text for a Vendor Interface status code.  These are
		// the codes returned across the wire by most of the device requests;
		// in this API, these functions return 'int' results.  (Note that the
		// API functions for purely Windows-side tasks, such as discovering
		// and opening USB interfaces, generally use HRESULT's, since these
		// are purely local operations with status more meaningfully conveyed
		// in terms of Windows error codes.)  The returned string is a const
		// string with static storage duration.
		static const char *ErrorText(int statusCode);

		// Ping the device.  This sends a Ping command to the device,
		// which can be used to test that the connection is working.
		// Returns the status code, which is VendorResponse::OK
		// if the connection is working.
		int Ping() { return SendRequest(VendorRequest::CMD_PING); }

		// Query the BLT-II software version installed on the device.  
		// Returns VendorResponse::OK or an ERR_xxx code.
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
		// hardware and restarts the BLT-II firmware program.  Returns
		// a VendorResponse status code.  The reset will drop the USB
		// connection, so the current handle must be closed after
		// this returns, and a new connection must be opened if the
		// caller wishes to continue sending commands to the same device.
		// Returns VendorResponse::OK or an ERR_xxx code.
		int ResetPico();

		// Send an ENTER BOOT LOADER command to the device.  This resets
		// the Pico into its ROM boot loader mode.  The USB connection
		// will be dropped when the device resets, so the current handle
		// must be closed after this returns.  The BLT-II device will
		// disappear from the system after the reset, to be replaced by
		// an RP2 Boot device.  This can be used to prepare to load new
		// firmware onto the device by forcing it into RP2 Boot Loader
		// mode, which will connect the RP2 Boot device's virtual disk
		// drive, which in turn accepts a UF2 file containing new firmware
		// to load into the device's flash.  Returns VendorResponse::OK or
		// an ERR_xxx code.
		int EnterBootLoader();

		// Query device performance statistics.  This retrieve information
		// on the device's main loop time and memory layout, which can be
		// useful to check the health of the device, and for debugging and
		// troubleshooting.  (Note that these statistics don't include any
		// button latency measurements; these are just related to the
		// overall device status.)
		//
		// If resetCounters is true, counters for rolling averages will be
		// reset, to start a new rolling-average window.
		//
		// Returns VendorResponse::OK or an ERR_xxx code.
		int QueryStats(Statistics *stats, size_t sizeofStats, bool resetCounters);

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
		// result code is VendorReply::ERR_EOF.
		int QueryLog(std::vector<uint8_t> &text, size_t *totalAvailable = nullptr);


		// Get the current system clock microsecond time.  This retrieves
		// the time on the Windows QueryPerformanceCounter() clock, with
		// the result expressed in microseconds since system startup.
		static uint64_t GetMicrosecondTime()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return (t.QuadPart*qpcTickTime_us64 + 32768L) >> 16;
		}

		// Notify the device of a button input event received on the host.
		// 
		// The gpio is the port the button is connected to on the Button
		// Latency Tester II device (don't confuse this with the GPIO port
		// on the subject device being tested).
		// 
		// tEvent is the system timestamp, in microseconds, at the moment
		// the event was first received in the application layer.  The
		// latency we're measuring is defined as the time between the
		// physical button press and the arrival of the corresponding
		// high-level Windows input event at the application layer, so
		// this timestamp represents the end of the latency interval
		// we're measuring.
		// 
		// The Windows and Pico have independent system clocks, so the
		// event timestamp on the Windows clock isn't directly meaningful
		// on the Pico.  However, the Pico and host can determine the
		// offset between their system clocks to high precision by using
		// the USB SOF (Start-of-Frame) signal as a shared time reference.
		// This routine automatically provides the SOF timing information
		// to the Pico so that it can translate qpcEventTime to the
		// corresponding Pico clock time.
		// 
		// The SOF timing information depends upon USB hardware access,
		// so it might not be available on some systems.  This command
		// will succeed even if the SOF timing is unavailable, but the
		// latency calculation on the Pico will be less accurate, because
		// it won't be able to take into account the USB transit time for
		// the command.  The caller might therefore wish to warn the user
		// if the SOF timing is unavailable, so we pass the status back
		// in sofTimeAvailable.
		struct HostInputEventResult
		{
			// latency measured; valid only if status == Matched
			uint32_t latency = 0;

			// Flag: the USB hardware's Start-of-frame (SOF) timestamp
			// was successfully acquired from the USB driver.  If this
			// is false, the latency measurement is less precise,
			// because the Pico can't determine the precise correspondence
			// between the host and Pico clocks, and therefore has to
			// include the outbound USB request time in the total.  This
			// flag is provided in case the caller wants to alert the
			// user about the effect on the timing data.
			bool sofTimeAvailable = false;

			// status
			enum class Status
			{
				// unknown/invalid status
				Unknown,

				// event matched - latency result is valid
				Matched,

				// Event not matched against a recent button press.  This
				// means that the Pico hasn't recorded any button presses
				// on the specified GPIO, or hasn't recorded one recently
				// enough to count as a match for the current input event.
				NotMatched,

				// Duplicate event.  This indicates that the Pico has already
				// matched a previous host input against the same physical
				// button press.  This can happen if the subject device sends
				// multiple events for a single physical press due to switch
				// bounce or other factors.  The Pico ignores these events,
				// and returns this status to so indicate.
				Duplicate,

			} status = Status::Unknown;
		};
		int HostInputEvent(int gpio, uint64_t tEvent, HostInputEventResult &result);

		// Retrieve latency statistics.  Fills in the vector with a
		// collection of latency data structures.
		struct MeasurementData
		{
			// GPIO port number
			int gp;

			// number of physical button presses recorded on the GPIO
			uint64_t nPresses;

			// number of host events matched against physical button presses
			uint64_t nHostEvents;

			// Sum of latency measured for host events, in microseconds
			uint64_t latencySum;

			// Sum of squares of latency measurements (for std dev calculation)
			uint64_t latencySquaredSum;

			// maximum and minimum latencies recorded
			uint64_t latencyMin;
			uint64_t latencyMax;

			// Median latency calculated in the firmware, in microseconds.  This
			// is calculated from the most recent samples, up to a fixed maximum
			// count set in the firmware (currently 1000 samples).
			uint16_t latencyMedian;

			// compute the average latency for measured host events
			double GetAvg() const { 
				return nHostEvents == 0 ? 0.0 : static_cast<double>(latencySum) / static_cast<double>(nHostEvents); 
			}

			// compute the latency standard deviation
			double GetStdDev() const {
				if (nHostEvents == 0) return 0.0;
				double mean = GetAvg();
				return sqrt(static_cast<double>(latencySquaredSum)/static_cast<double>(nHostEvents) - mean*mean);
			}

		};
		int QueryMeasurements(std::vector<MeasurementData> &data);

		// Clear latency measurements data.  This resets all latency
		// timing statistics gathered on the device.
		int ClearMeasurements()
		{
			uint8_t subcmd = VendorRequest::SUBCMD_MEASUREMENTS_RESET;
			return SendRequestWithArgs(VendorRequest::CMD_MEASUREMENTS, subcmd);
		}


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
		int SendRequest(uint8_t cmd, VendorResponse &response,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr);

		// Send a request with arguments and optional OUT data
		template<typename ArgsType> int SendRequestWithArgs(
			uint8_t cmd, ArgsType &args,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr)
		{
			// build the request struct
			VendorRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));
			req.argsSize = sizeof(args);
			memcpy(&req.args, &args, sizeof(ArgsType));

			// send the request
			return SendRequest(req, xferOutData, xferInData);
		}

		// Send a request with arguments in and out, and optional OUT data
		template<typename ArgsType> int SendRequestWithArgs(
			uint8_t cmd, ArgsType &args, VendorResponse &resp,
			const BYTE *xferOutData = nullptr, size_t xferOutLength = 0,
			std::vector<BYTE> *xferInData = nullptr)
		{
			// build the request struct
			VendorRequest req(token++, cmd, static_cast<uint16_t>(xferOutLength));
			req.argsSize = sizeof(args);
			memcpy(&req.args, &args, sizeof(ArgsType));

			// send the request
			return SendRequest(req, resp, xferOutData, xferInData);
		}

		// Send a request with a pre-built request struct
		int SendRequest(const VendorRequest &request,
			const BYTE *xferOutData = nullptr, std::vector<BYTE> *xferInData = nullptr);

		// Send a request with pre-built request struct, capturing the response
		int SendRequest(const VendorRequest &request, VendorResponse &resp,
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

	protected:
		// the constructor is protected - clients create interface objects
		// through the enumeration mechanism
		VendorInterface(HANDLE hDevice, WINUSB_INTERFACE_HANDLE winusbHandle, HANDLE timeTrackingHandle,
			const WSTRING &path, const WSTRING &deviceInstanceId, const WCHAR *serialNum,
			UCHAR epIn, UCHAR epOut) :
			hDevice(hDevice),
			winusbHandle(winusbHandle),
			timeTrackingHandle(timeTrackingHandle),
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

		// time-tracking handle, for Start-of-Frame time queries
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

		// QueryPerformanceCounter tick time, in 48.16 fixed-point format.
		// This lets us convert tick times to microseconds at high precision
		// with all integer artihmetic.
		static uint64_t qpcTickTime_us64;

		// calculate the qpcTickTime_us64 value; used for startup initialization
		static uint64_t GetQPCTickTime_us64();

		// Windows OVERLAPPED struct holder
		class OVERLAPPEDHolder
		{
		public:
			OVERLAPPEDHolder(HANDLE hFile, WINUSB_INTERFACE_HANDLE winusbHandle) : hFile(hFile), winusbHandle(winusbHandle)
			{
				ZeroMemory(&ov, sizeof(ov));
				ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			}

			~OVERLAPPEDHolder()
			{
				CloseHandle(ov.hEvent);
			}

			template<typename SizeType> HRESULT Wait(DWORD timeout, SizeType &bytesTransferred)
			{
				switch (WaitForSingleObject(ov.hEvent, timeout))
				{
				case WAIT_TIMEOUT:
					// timeout - cancel the I/O and return ABORT status
					Cancel(timeout);
					return E_ABORT;

				case WAIT_OBJECT_0:
					// I/O completed - return the result from the OVERLAPPED struct
					{
						ULONG sz;
						if (WinUsb_GetOverlappedResult(winusbHandle, &ov, &sz, FALSE))
						{
							bytesTransferred = static_cast<SizeType>(sz);
							return S_OK;
						}
						else
							return HRESULT_FROM_WIN32(GetLastError());
					}

				case WAIT_FAILED:
					// error in the wait - cancel the I/O and return the underlying error code
					Cancel(timeout);
					CancelIoEx(hFile, &ov);
					return HRESULT_FROM_WIN32(GetLastError());

				default:
					// other error - cancel the I/O and return generic FAIL status
					Cancel(timeout);
					return E_FAIL;
				}
			}

			// cancel an I/O request, waiting for up to the timeout for the response to clear
			void Cancel(DWORD timeout)
			{
				// cancel the I/O
				CancelIoEx(hFile, &ov);

				// Cancellation isn't necessarily synchronous, so allow some time
				// for the request to clear, up to the timeout.  This effectively
				// doubles the timeout period the caller requested, but that's
				// preferable to leaving the I/O hanging, because an incomplete
				// I/O tends to break the protocol flow for future requests. 
				// Allowing some extra time for the I/O to clear is better for
				// application stability.  In practice, WinUsb seems to resolve
				// cancellations quickly (order of milliseconds), but not
				// immediately, so the extra wait is worthwhile: it improves
				// the chances that the pipe will recover and we can keep making
				// requests, without incurring much actual wait time.
				//
				// We don't care what the outcome of the wait is, because there's
				// nothing more we can do here to resolve the problem if the wait
				// times out or fails with an error.  The purpose of the wait is
				// to allow time for WinUsb to recover *when that's possible*,
				// with the timeout to prevent a freeze when recovery isn't
				// possible.  In the event that the cancellation doesn't complete
				// within the timeout, the caller is responsible for keeping the
				// OVERLAPPED struct's memory valid, because the WinUsb driver
				// will hang onto the pointer to it until the I/O actually does
				// complete, and could write into the memory at any time until
				// then.
				WaitForSingleObject(ov.hEvent, timeout);
			}

			// the original device handle
			HANDLE hFile;

			// the WinUsb handle layered on the device handle
			WINUSB_INTERFACE_HANDLE winusbHandle;

			// system overlapped I/O tracking struct
			OVERLAPPED ov;
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

	// Vendor Interface descriptor object.  The device enumerator returns a 
	// list of these objects representing the connected devices.  The object
	// can then be used to open a live USB connection to the physical device.
	class VendorInterfaceDesc
	{
		// Note that the vendor interface constructor is private, for use by 
		// the enumerator in the base class.  We let the enumerator access it 
		// by making the main class a friend, and we let std::list's emplace() 
		// access it via the private ctor key type that only friends can use.
		friend class VendorInterface;
		struct private_ctor_key_t {};

	public:
		// construct - called from the enumerator
		VendorInterfaceDesc(private_ctor_key_t, const WCHAR *path, size_t len, const WCHAR *deviceInstanceId) :
			path(path, len),
			deviceInstanceId(deviceInstanceId)
		{ }

		// Open the path to get a live handle to a device
		HRESULT Open(VendorInterface* &device) const;
		HRESULT Open(std::unique_ptr<VendorInterface> &device) const;
		HRESULT Open(std::shared_ptr<VendorInterface> &device) const;

		// the device's file system name as a string
		const WCHAR *Name() const { return path.c_str(); }

		// get the Win32 device instance ID for the underlying device
		const WCHAR *DeviceInstanceId() const { return deviceInstanceId.c_str(); }

	protected:
		// File system path to the device
		WSTRING path;

		// Device Instance ID.  This is a unique identifier for
		// the device that Windows assigns.  It serves as the device
		// identifier in some system APIs.
		WSTRING deviceInstanceId;
	};

}
