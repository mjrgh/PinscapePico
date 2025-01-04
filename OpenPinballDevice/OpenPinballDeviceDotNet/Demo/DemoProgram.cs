// OpenPinballDeviceDotNet Library Demo Program
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a simple C# console application demonstrating (and testing) the
// basics of the Open Pinball Device access library.
//

using System;
using System.IO;
using OpenPinballDeviceDotNet;

// Show a listing of active devices.  This uses DeviceDesc.EnumerateDevices()
// to dynamically discover the active devices in the system.  The enumerator
// returns a List of DeviceDesc objects describing the active devices.  Each
// descriptor provides human-readable identifying information on a device,
// along with a file system path that can be used in CreateFile() to access
// the device's live HID data input stream.
//
// If you want to open a file handle on the device for direct access, use
// the devicePath string in the descriptor as the filename in CreateFile().
//
// You can also pass the descriptor to the Reader constructor to create a
// higher-level read helper.  The Reader opens a file handle on the file,
// reads the raw byte stream, and parses it into the Open Pinball Device
// structure fields for easier interpretation.
var descriptors = DeviceDesc.EnumerateDevices();
Console.WriteLine("Discovered {0} Open Pinball Device instances(s)", descriptors.Count);
foreach (var desc in descriptors)
{ 
	System.Console.WriteLine("  {0} (manuf. {1}, serial {2}, VID/PID {3:X4}/{4:X4})",
		desc.productName, desc.manufacturer, desc.serial, desc.vid, desc.pid);
}

// Most pinball simulator applications and the like don't actually care
// about the physical device list, since all they're interested in is the
// sensor data input the devices provide.  To simplify this common use
// case, the library provides a Combined Reader that hides the physical
// device details and just hands back the input data, parsed into the
// high-level pin cab sensor abstractions.
//
// The Combined Reader is so named because it merges input from all active
// devices, providing the caller with a single merged input stream, so
// that the caller can think of the input as coming from the overall pin
// cab rather than from discrete physical devices.  The pin cab sensor
// inputs defined in the Open Pinball Interface are by their nature
// cabinet-wide - a pin cab only needs one accelerometer, one plunger,
// one Start button, etc.  But the physical implementation might involve
// two or more controller boards, simply because it might be more
// convenient or cost-effective for the user to implement it that way.
// The Combined Reader hides these details of physical implementation.
//
// To create a combined reader, you simply instantiate the class.  The
// constructor automatically searches for active devices and opens each
// one for reading with a system file handle.
var reader = new CombinedReader();

// You can get a list of the active devices the reader discovered via
// reader.Devices, which returns a list of the underlying single-device
// Reader objects.  Even if the application doesn't need to access the
// devices individually for its own purposes, it might still be useful
// information, such as for message logs for troubleshooting purposes.
var devices = reader.Devices;
Console.WriteLine("\nCombined reader found {0} device(s)", devices.Count);
foreach (var device in devices)
{
	Console.WriteLine("  {0} (manuf. {1}, serial {2})",
		device.Desc.productName, device.Desc.manufacturer, device.Desc.serial);
}

// Simulators will usually just start calling reader.Read() at convenient
// intervals at this point.  Read() is non-blocking, and always returns
// the current instantaneous status of the pin cab sensors (based on the
// latest reports received across all devices), so the application can
// call Read() at whatever intervals are convenient for it without any
// special consideration for the actual timing of the input reports.
// The system HID driver typically polls devices at 8-10 ms intervals,
// so the application can minimize latency by calling Read() at least
// that often.  But even if the caller samples input at a lower rate,
// Read() will always reflect the latest instantaneous state; Read()
// doesn't buffer a backlog of reports, but rather always catches up
// to the latest input at every call.
Console.WriteLine("Reading input - press Q to stop...");
for (; ;)
{
	Thread.Sleep(100);
	if (Console.KeyAvailable)
	{
		// check for Quit
		var ch = Console.ReadKey();
		if (ch.Key == ConsoleKey.Q)
			break;
	}

	Report r;
	if (reader.Read(out r))
		Console.WriteLine("ax={0}, ay={1}, plunger={2}", r.axNudge, r.ayNudge, r.plungerPos);
}
