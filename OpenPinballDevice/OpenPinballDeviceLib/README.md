# Open Pinball Device client library

This is a C++ library to help applications access Open Pinball Device units.
Open Pinball Device is a custom HID interface, defined at 
[http://mjrnet.org/pinscape/OpenPinballDevice/OpenPinballDeviceHID.htm](http://mjrnet.org/pinscape/OpenPinballDevice/OpenPinballDeviceHID.htm),
that's designed to be implemented by virtual pinball cabinet I/O controllers.
These are specialized USB devices that send input to the PC from the various
pinball-specific sensors and controls commonly used in virtual pin cabs:
plunger sensors, accelerometers (for nudge sensing), leaf switch flipper buttons,
and arcade-style pushbuttons.

The library lets you automatically discover Open Pinball Device units connected
to the computer, and lets you read the sensor data the devices send to the PC.
Automatic discovery allows your application to find devices on its own, without
requiring the user to provide you with a list of them, so in most cases you can
use these devices in your application without imposing any manual configuration
steps on the user.  What's more, the interface's report data format is
specialized for virtual pin cab applications, which further reduces the need for
manual configuration by pre-assigning high-level meanings to the data fields.
There are fields in the reports dedicated to the accelerometer, plunger, and
button inputs, so you don't need the user to tell you which fields correspond to
which pin cab elements, the way you would with, say, a generic joystick report.

You can read the input streams from individual controllers, or you can use
the "combined reader" that merges all of the connected controllers into a
single input stream, as though all of the input were coming from a single
controller unit.  For applications that are only interested in reading the
sensor data, and don't particularly care about the details of the individual
controllers, the combined reader is the simplest interface.  Applications that
need information about the devices themselves, such as for presenting the user
with a list of the available units, can use the more detailed APIs to enumerate
the devices and read their input streams individually.

The implementation is designed for easy portability.  It doesn't directly call
any platform APIs, but instead uses the cross-platform hidapi library for all
device access.  hidapi is available on Windows, Linux, Mac OS, FreeBSD, and
Android.  You can find it at
[https://github.com/libusb/hidapi](https://github.com/libusb/hidapi).

## Basic usage

The simplest way to use the library is through the `OpenPinballDevice::CombinedReader`
class.  This class automatically scans for all available Open Pinball Device
instances, and provides your application with a merged view that combines
the input from all of the devices into a single input stream.  The devices
in a pin cab are by their nature singular, so even if a user has multiple
Open Pinball Device units, the cab should still only have one of each
sensor type.  The combined reader takes advantage of this to provide
the unified view.

```
// create a combined reader
auto *reader = new OpenPinballDevice::CombinedReader();

// read input
for (;;)
{
   // read pinball input
   OpenPinballDevice::OpenPinballDeviceReport report;
   if (reader->Read(report))
   {
      // process the report
   }

   // do other periodic tasks...
}
```

The Read() call is non-blocking: it simply returns false if a report isn't
immediately available.

If you need more granular access, you can get a list of available devices
using the enumeration function:

```
std::list<OpenPinballDevice::DeviceDesc> devices = OpenPinballDevice::EnumerateDevices();
```

The descriptors have the basic USB device information (VID/PID, product name
string, manufacturer string, serial number string), and hidapi information that
can be used to open the device for direct I/O access.  To open the device
at a given descriptor, use the Reader class:

```
auto *reader = OpenPinballDevice::Reader::Open(devices.front());

OpenPinballDevice::OpenPinballDeviceReport report;
if (reader->Read(report))
{
   // process the report
}
```

You now have access to the individual reports coming in from that device.  As
with the combined reader, the Read() call is non-blocking.


## Sample application

For a full usage example, see the Open Pinball Device Viewer sample application
in the Open Pinball Device repository.  That application is a Windows GUI
program, so it's not itself portable to other platforms, but the library
calls work the same way everywhere.


## Building on Windows

A Visual Studio 2022 solution (.sln) file is provided that builds the project
into a static-link .lib file, which you can then link into your own C++ programs.
All dependencies for the Windows build are included, so you should be able to
build by loading the .sln into Visual Studio, and selecting Build > Build Solution
on the main menu.

Statically linked libraries on Windows must be built with the same architecture
and configuration as the target project (e.g., Debug/x86).  In addition, you
might have to adjust some of the compiler options to match your project's
settings, particularly the run-time library linking options (static or DLL),
which can be found in the Visual Studio interface by right-clicking on the
project file, selecting Properties, and navigating to C/C++ > Code Generation > Runtime Library.



## Building on other platforms

I haven't created build scripts on any non-Windows platforms, but the library
should be simple to build.  If you do get it building on another platform, and
you can reduce the build instructions to something that other people will be
able to replicate (such as a CMake script or a platform makefile), please
consider sending a github pull request.

**Third-party dependencies:** You'll need to be able to build the hidapi library
for your platform.  The [hidapi repository]((https://github.com/libusb/hidapi) 
on github has build instructions for all of its supported platforms, and some
platforms have pre-built libraries available.

hidapi is the only external dependency; all other required files are included
in the repository.  Once you have hidapi set up, set the C++ compiler's include
path to point to its folder, and compile the following files:

* OpenPinballDeviceLib.cpp
* hid-report-parser/hid_report_parser.cpp

You can then link these files (plus the hidapi library) into a static-link
library.

## License

Copyright 2024, 2025 Michael J Roberts.

This library is released under the BSD 3-clause license.  The
third-party libraries are licensed under similar terms; see their
individual license files for details.
