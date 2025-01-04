# Open Pinball Device HID

This repository contains:

* The Open Pinball Device HID specification
* C++ and C# libraries to support writing host-side applications that connect to USB devices implementing the OPD HID interface
* Example projects demonstrating how to use the libraries

## Specification

[OpenPinballDeviceHID.htm](OpenPinballDeviceHID.htm)

The Open Pinball Device HID interface defines a HID interface that
virtual pinball cabinet I/O controllers can implement to provide a
well-defined way to send sensor input to host applications.  It's
particularly designed for use by pinball simulators such as Visual
Pinball.  The interface is documented and published in the hope that
most current and future pinball simulators and pin cab I/O controllers
will adopt it, with the aim of improving compatibility between devices
and simulators and minimizing manual user configuration work.

## Libraries

OpenPinballDeviceLib/ contains a C++ statically linked library for
accessing devices that implement the Open Pinball Device HID
interface.  System-level USB access is handled through a third-party,
open-source library, [hidapi](https://github.com/libusb/hidapi), which
provides portable HID access across many platforms, including Windows,
Linux, and MacOS.  Project files are provided for Microsoft Visual
Studio on Windows.  The code is written in portable C++ with no direct
dependencies on any OS APIs, so it should be readily portable to other
platforms where hidapi is available, but build scripts are currently
only provided for Visual Studio on Windows.

OpenPinballDeviceDotNet/ is a C# version of the library, to provide
the same access to .NET applications.  The Visual Studio solution 
includes a simple C# demo program.  C# libraries can generally be
used easily in other .NET languages, such as VB.NET.

## Sample Application

OpenPinballDeviceViewer/ is a Windows GUI program that shows a list of
Open Pinball Device units currently connected, and displays live input
from a device.  It's designed to serve as the OPD equivalent of the
Windows "Set up USB Devices" control panel (joy.cpl), which lets you
view live input from a joystick or gamepad to check if it's working
properly.  The device viewer application provides a similar display
showing the accelerometer, plunger, and button inputs from an OPD
unit.

## License

Copyright 2024, 2025 Michael J Roberts

Released under a BSD-3-Clause license - NO WARRANTY
