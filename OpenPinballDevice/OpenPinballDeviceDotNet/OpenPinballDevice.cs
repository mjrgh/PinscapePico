// Open Pinball Device C# interface library
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using static OpenPinballDeviceDotNet.HIDImports;
using System.Reflection.Metadata;
using System.Security.Principal;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using System.Runtime.Versioning;
using System.Runtime.CompilerServices;


namespace OpenPinballDeviceDotNet
{
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
	[SupportedOSPlatform("windows")]
	public class CombinedReader : IDisposable
	{
		// Create a combined reader object.  This automatically
		// scans for attached devices and connects to all.
		public CombinedReader()
		{
			// enumerate available devices
			var devices = DeviceDesc.EnumerateDevices();

			// open a reader on each device
			foreach (var device in devices)
			{
				try
				{
					// open the device and add it to our list
					readers.Add(new Reader(device));
				}
				catch (Exception)
				{
					// If we couldn't open the device, simply skip it.  In
					// most cases, an open failure at this point would mean
					// that the device has been disconnected since we did
					// the enumeration, since the enumerator itself had to
					// be able to open the device to find it.  So an open
					// failure is benign, in that our task here is to open
					// the active devices, and this one is apparently no
					// longer active.
				}
			}
		}

		public void Dispose()
		{
			// dispose of the readers
			foreach (var reader in readers)
				reader.Dispose();

			// forget the reader list
			readers.Clear();
		}

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
		public bool Read(out Report report)
		{
			// create a combined report
			report = new Report();

			// read the current report from each device
			bool newReport = false;
			foreach (var reader in readers)
			{
				// read a report from this device
				Report devReport;
				if (reader.Read(out devReport))
					newReport = true;

				// OR all buttons into the combined report.  Each logical
				// button should only be mapped to a single physical input,
				// so a button's bit should be zero on every device except
				// the one where it's mapped.
				report.genericButtons |= devReport.genericButtons;
				report.pinballButtons |= devReport.pinballButtons;

				// Combine nudge and plunger axes additively.  Each sensor should
				// be unique (there should only be one instance of a plunger sensor
				// and one accelerometer across all devices), so each logical axis
				// should report zero on every device except the one where the
				// associated sensor is actually connected.
				report.axNudge += devReport.axNudge;
				report.ayNudge += devReport.ayNudge;
				report.vxNudge += devReport.vxNudge;
				report.vyNudge += devReport.vyNudge;
				report.plungerPos += devReport.plungerPos;
				report.plungerSpeed += devReport.plungerSpeed;
			}

			// return the new-report-read status
			return newReport;
		}

		// Get the list of connected devices.  This returns all of
		// the devices (and only the devices) that this reader is
		// connected to.
		public ReadOnlyCollection<Reader> Devices { get { return readers.AsReadOnly(); } }

		// internal device list
		protected List<Reader> readers = new List<Reader>();
	}

	// file system safe handle
	class FileHandle : SafeHandleZeroOrMinusOneIsInvalid
	{
		public FileHandle(IntPtr handle) : base(true) { this.handle = handle; }
		override protected bool ReleaseHandle() { return CloseHandle(handle); }
	}


	// Single-device reader
	[SupportedOSPlatform("windows")]
	public class Reader : IDisposable
	{
		public Reader(DeviceDesc desc)
		{
			// remember the device descriptor
			this.desc = desc;

			// open a handle on the device
			hDevice = new FileHandle(CreateFile(desc.devicePath, GENERIC_READ | GENERIC_WRITE, SHARE_READ_WRITE,
				IntPtr.Zero, FileMode.Open, EFileAttributes.Overlapped, IntPtr.Zero));

			// throw an error if the open failed
			if (hDevice.IsInvalid)
				throw new Exception(String.Format("Error opening device (error code {0})", Marshal.GetLastWin32Error()));

			// Allocate the input buffer, and set the first byte to the report ID.
			// The Windows HID driver requires the first byte of the receive buffer
			// to be set to the report ID before each ReadFile() call.
			buffer = new byte[desc.inputReportByteLength];
			lastReport = new byte[desc.inputReportByteLength - 1];
			buffer[0] = desc.inputReportID;

			// start the first read
			StartRead();
		}

		// Read a report into a version 1.0 report struct.
		public bool Read(out Report callerReport)
		{
			// read reports until we block, to be sure we have the latest data
			bool newReport = false;
			while (ReadRaw(lastReport))
			{
				// we have a new report
				newReport = true;

				// process the report's internal data, if the report is big enough
				// to interpret as the OPD report struct
				if (lastReport.Length >= Marshal.SizeOf(typeof(Report)))
				{
					var r = Report.From(lastReport);
					genericButtons.Process(r.genericButtons);
					pinballButtons.Process(r.pinballButtons);
				}
			}

			// copy the most recent report to the caller's buffer
			callerReport = Report.From(lastReport);

			// update the report with the aggregated button state
			callerReport.genericButtons = genericButtons.GetReport();
			callerReport.pinballButtons = pinballButtons.GetReport();

			// return the new-report indication
			return newReport;
		}

		// Read the next report, in raw byte format, with no processing.
		// If a report is available immediately, reads the report into the
		// caller's buffer and returns true.  If no new report is available,
		// returns false.
		bool ReadRaw(byte[] outBuf)
		{
			// check the status of the pending read
			if (readStatus == ERROR_SUCCESS
				|| (readStatus == ERROR_IO_PENDING && GetOverlappedResultEx(hDevice.DangerousGetHandle(), ref ov, out bytesRead, 0, false)))
			{
				// Read completed successfully.  Extract the report into
				// the caller's buffer.  The first byte of the read buffer
				// is the HID report ID prefix, so the struct starts at
				// the second byte of the read buffer.
				Array.Copy(buffer, 1, outBuf, 0, Math.Min(buffer.Length - 1, outBuf.Length));

				// start the next read
				StartRead();

				// tell the caller that we read a report
				return true;
			}
			else if (readStatus == ERROR_IO_PENDING)
			{
				// the read is still pending - return "no report"
				return false;
			}
			else
			{
				// The read status is some error other than PENDING, so the
				// read failed.  Start a new read and return "no report" to
				// the caller.  The new read helps ensure that, if the error
				// is due to a temporary fault, we'll resume reading reports
				// again as soon as the fault clears, without blocking the
				// caller in the meantime.
				StartRead();
				return false;
			}
		}

		// aggregated button states
		class ButtonState
		{
			// last state reported to the client
			uint reported = 0;

			// next state to be reported to the client at the next update
			uint next = 0;

			// live state from the last report
			uint live = 0;

			// get a client report
			public uint GetReport()
			{
				// make the pending next report the new client report, and snapshot
				// the live state as the starting point for the next future report
				reported = next;
				next = live;

				// return the client report
				return reported;
			}

			// process an incoming report
			public void Process(uint state)
			{
				// set the new live state from the report
				live = state;

				// Get the set of buttons that are the same between the pending next
				// report and the last report.  a^b is 0 if a==b and 1 otherwise, so
				// ~(a^b) is 1 iff a==b.
				uint same = ~(next ^ reported);

				// Now set the pending next report state for each unchanged button to
				// match the live state.  This makes changes that occur between the
				// previous and next client reports stick.
				next = (next & ~same) | (state & same);
			}
		}

		ButtonState genericButtons = new ButtonState();
		ButtonState pinballButtons = new ButtonState();

		public void Dispose()
		{ 
			hDevice.Dispose(); 
			hOverlappedEvent.Dispose();
		}

		// get the descriptor
		public DeviceDesc Desc { get { return desc; } }

		// device descriptor
		readonly DeviceDesc desc;

		// file handle
		FileHandle hDevice;

		// overlapped I/O
		NativeOverlapped ov = new NativeOverlapped();
		EventWaitHandle hOverlappedEvent = new EventWaitHandle(false, EventResetMode.ManualReset);
		int readStatus = 0;
		uint bytesRead = 0;
		byte[] buffer;

		// previous report data
		byte[] lastReport;

		// start an overlapped read
		void StartRead()
		{
			// reinitialize the OVERLAPPED struct
			ov.EventHandle = hOverlappedEvent.SafeWaitHandle.DangerousGetHandle();
			if (ReadFile(hDevice.DangerousGetHandle(), buffer, (uint)buffer.Length, out bytesRead, ref ov) != 0)
				readStatus = ERROR_SUCCESS;
			else
				readStatus = Marshal.GetLastWin32Error();
		}
	}


	// Input report from the device
	[StructLayout(LayoutKind.Sequential)]
	public class Report
	{
		public UInt64 timestamp = 0;             // timestamp, microseconds since an arbitrary zero point
		public UInt32 genericButtons = 0;        // button states for 32 general-purpose on/off buttons
		public UInt32 pinballButtons = 0;        // button states for pre-defined pinball simulator function buttons
		public byte llFlipper = 0;               // lower left flipper button duty cycle
		public byte lrFlipper = 0;               // lower right flipper button duty cycle
		public byte ulFlipper = 0;               // upper left flipper button duty cycle
		public byte urFlipper = 0;               // upper right flipper button duty cycle
		public UInt16 axNudge = 0;               // instantaneous nudge acceleration, X axis (left/right)
		public UInt16 ayNudge = 0;               // instantaneous nudge acceleration, Y axis (front/back)
		public UInt16 vxNudge = 0;               // instantaneous nudge velocity, X axis
		public UInt16 vyNudge = 0;               // instantaneous nudge velocity, Y axis
		public UInt16 plungerPos = 0;            // current plunger position
		public UInt16 plungerSpeed = 0;          // instantaneous plunger speed

		public static unsafe Report From(byte[] buf)
		{
			// allocate space for the struct on the stack
			var structSize = Marshal.SizeOf(typeof(Report));
			var r = stackalloc byte[structSize];

			// zero the memory, so that any fields beyond the end of buf[]
			// will be deterministically zeroed in the new structure
			Unsafe.InitBlock(r, 0, (uint)structSize);

			// copy the smaller of the buffer or the struct size
			Marshal.Copy(buf, 0, (IntPtr)r, Math.Min(structSize, buf.Length));

			// marshal it back to a report struct
			var ret = Marshal.PtrToStructure<Report>((IntPtr)r);
			if (ret == null) throw new Exception("Can't create Report struct");
			return ret;
		}
	}

	// Device descriptor.  Use EnumerateDevices() to get a list of
	// descriptors for currently active devices.
	[SupportedOSPlatform("windows")]
	public class DeviceDesc
	{
		// Enumerate currently active devices.  This scans the current
		// collection of connected HID devices, returning a list of
		// DeviceDesc structures describing the devices found.
		public static List<DeviceDesc> EnumerateDevices()
		{
			// usages
			const int HID_USAGE_PAGE_GAME = 0x05;
			const int HID_USAGE_GAME_PINBALL_DEVICE = 0x02;

			// set up a list for the results
			var newList = new List<DeviceDesc>();

			// set up a device list containing all active HID devices
			Guid hidGuid;
			HidD_GetHidGuid(out hidGuid);
			IntPtr hdi = SetupDiGetClassDevs(ref hidGuid, null, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
			if (hdi != INVALID_HANDLE_VALUE)
			{
				// enumerate devices in the list
				var did = new SP_DEVICE_INTERFACE_DATA();
				did.cbSize = Marshal.SizeOf(did);
				for (UInt32 memberIndex = 0; SetupDiEnumDeviceInterfaces(hdi, IntPtr.Zero, ref hidGuid, memberIndex, ref did); ++memberIndex)
				{
					// retrieve the required buffer size for device interface detail data
					uint diDetailSize = 0;
					int err = 0;
					if (!SetupDiGetDeviceInterfaceDetail(hdi, ref did, IntPtr.Zero, 0, out diDetailSize, IntPtr.Zero)
						&& (err = Marshal.GetLastWin32Error()) != ERROR_INSUFFICIENT_BUFFER)
						continue;

					// retrieve the device interface detail data, which will give us the file system path to the device
					var diDetail = new SP_DEVICE_INTERFACE_DETAIL_DATA();
					diDetail.cbSize = (IntPtr.Size == 8) ? (uint)8 : (uint)5;
					HIDImports.SP_DEVINFO_DATA devInfoData = new HIDImports.SP_DEVINFO_DATA();
					devInfoData.cbSize = (uint)Marshal.SizeOf(devInfoData);
					if (!SetupDiGetDeviceInterfaceDetail(hdi, ref did, ref diDetail, diDetailSize, out diDetailSize, out devInfoData))
						continue;

					// open the device path
					IntPtr hDevice = CreateFile(diDetail.DevicePath, 0, SHARE_READ_WRITE, IntPtr.Zero, System.IO.FileMode.Open, 0, IntPtr.Zero);
					if (hDevice != INVALID_HANDLE_VALUE)
					{
						// Retrieve the preparsed HID report descriptor information, for details
						// on the input report format.  The Open Pinball Device's top-level
						// application collection usage type is Pinball Device, but that's a
						// well-known HID CA type that could be used by other devices as well,
						// so that by itself isn't enough to positively identify an Open Pinball
						// Device.  To determine the subtype, we have to check its input report
						// format, which we can positively identify by the unique string usage 
						// signature.
						IntPtr ppd = IntPtr.Zero;
						if (HidD_GetPreparsedData(hDevice, out ppd))
						{
							// get the capabilities
							var caps = new HIDP_CAPS();
							if (HidP_GetCaps(ppd, ref caps) == HIDP_STATUS_SUCCESS)
							{
								// check for the Pinball Device CA usage
								if (caps.UsagePage == HID_USAGE_PAGE_GAME && caps.Usage == HID_USAGE_GAME_PINBALL_DEVICE)
								{
									// It's a HID Pinball Device of some kind.  Now check to see if it's
									// an Open Pinball Device specifically, by checking for the custom
									// usage name string on the first button usage.
									var btnCaps = new HIDP_BUTTON_CAPS();
									ushort btnCapsLen = 1;
									var usageStringBuffer = new byte[256];
									string usageString;
									if (caps.NumberInputButtonCaps == 1
										&& HidP_GetButtonCaps(HidP_Input, ref btnCaps, ref btnCapsLen, ppd) == HIDP_STATUS_SUCCESS
										&& btnCapsLen == 1
										&& btnCaps.IsStringRange == 0
										&& btnCaps.StringMin != 0
										&& HidD_GetIndexedString(hDevice, btnCaps.StringMin, usageStringBuffer, (uint)usageStringBuffer.Length)
										&& (usageString = Encoding.Unicode.GetString(usageStringBuffer).TrimEnd('\0')).StartsWith("OpenPinballDeviceStruct/"))
									{
										// Success - it's an Open Pinball Device.  Retrieve the additional
										// data needed to populate the descriptor.

										// the Open Pinball Device version follows the '/' in the usage string
										string version = usageString.Substring(24);

										// get the VID/PID information
										var attr = new HIDD_ATTRIBUTES();
										HidD_GetAttributes(hDevice, ref attr);

										// get the standard HID device identifier strings
										var productNameBuffer = new byte[256];
										var manufacturerBuffer = new byte[256];
										var serialBuffer = new byte[256];
										var productName = HidD_GetProductString(hDevice, productNameBuffer, (uint)productNameBuffer.Length) ?
											Encoding.Unicode.GetString(productNameBuffer).TrimEnd('\0') : "Unknown Device";
										var manufacturer = HidD_GetManufacturerString(hDevice, manufacturerBuffer, (uint)manufacturerBuffer.Length) ?
											Encoding.Unicode.GetString(manufacturerBuffer).TrimEnd('\0') : "N/A";
										var serial = HidD_GetSerialNumberString(hDevice, serialBuffer, (uint)serialBuffer.Length) ?
											Encoding.Unicode.GetString(serialBuffer).TrimEnd('\0') : "N/A";

										// add it to the list
										newList.Add(new DeviceDesc(diDetail.DevicePath, version, attr.VendorID, attr.ProductID,
											productName, manufacturer, serial, btnCaps.ReportID, caps.InputReportByteLength));
									}
								}
							}

							// done with the preparsed data
							HidD_FreePreparsedData(ppd);
						}

						// done with the device handle
						CloseHandle(hDevice);
					}
				}

				// done with the device list handle
				SetupDiDestroyDeviceInfoList(hdi);
			}

			// return the result list
			return newList;
		}

		// Windows file system path to device.  The device can be opened with
		// CreateFile() using this path.
		public readonly string devicePath;

		// Open Pinball Device structure version string.  This is the version
		// suffix found in the "OpenPinballDeviceStruct/V.V" usage string that
		// uniquely identifies the custom structure usage in the HID report
		// descriptor - the "V.V" part, which gives the major and minor version
		// number of the struct definition.
		public readonly string structVersionString;

		// USB Product and Vendor ID (VID/PID) for the physical device.  Taken
		// together, these two fields were intended in the USB specification to
		// serve as a universally unique identifier for the device.  The VID's
		// uniqueness was meant to be guaranteed by requiring all devices makers
		// to obtain a VID assigned through a central registry maintained by the
		// USB standards body, USB-IF.  Vendors are responsible for ensuring
		// uniqueness among their own products' PIDs.  Unfortunately, many
		// smaller companies and virtually all open-source developers use
		// unregistered VIDs, so a VID/PID isn't as strong a guarantee of
		// positive product identification as it was meant to be.  Host
		// software that's concerned about false identification can use other
		// back-up means of ID, such as checking the product string or the
		// HID interface shape.
		//
		// Note that the Open Pinball Device interface is explicitly NOT tied
		// to any VID/PID, because the whole point is to define a common
		// interface that can be implemented by many devices from different
		// developers, so that host software can access the high-level virtual
		// pin cab-related features defined in the interface without having to
		// be programmed with the hardware details of every device.
		public readonly ushort vid;
		public readonly ushort pid;

		// Basic HID strings: product name, manufacturer, serial number string.
		// These are standard strings that HID devices are supposed to provide
		// to the host via the HID descriptor.  These are mostly useful as
		// human-readable identifiers, for identifying devices in error messages,
		// UI displays, log messages, etc.
		public readonly string productName;
		public readonly string manufacturer;
		public readonly string serial;

		// HID input report ID and byte length.  The report ID is the one-byte
		// prefix in every HID report that identifies the report type.
		public readonly byte inputReportID;
		public readonly ushort inputReportByteLength;

		// protected constructor - clients instantiate via EnumerateDevices()
		protected DeviceDesc(string devicePath, string structVersionString, ushort vid, ushort pid, 
			string productName, string manufacturer, string serial, byte inputReportID, ushort inputReportByteLength)
		{
			this.devicePath = devicePath;
			this.structVersionString = structVersionString;
			this.vid = vid;
			this.pid = pid;
			this.productName = productName;
			this.manufacturer = manufacturer;
			this.serial = serial;
			this.inputReportID = inputReportID;
			this.inputReportByteLength = inputReportByteLength;
		}
	}
}
