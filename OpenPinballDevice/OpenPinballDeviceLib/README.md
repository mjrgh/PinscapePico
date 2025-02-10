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

The main interface is designed for easy portability.  It doesn't directly call
any platform APIs, but instead uses the cross-platform hidapi library for all
device access.  hidapi is available on Windows, Linux, Mac OS, FreeBSD, and
Android.  You can find it at
[https://github.com/libusb/hidapi](https://github.com/libusb/hidapi).

There's a second interface that's based on the Windows-specific Raw Input
API.  That part isn't at all portable, so it's segregated into its own
header and implementation file.  That should make it easy and clean to omit
in its entirety when porting to a non-Windows platform.


## Interface options

The library provides two main programming interfaces, each suitable for a
particular application structure:

* A polling interface, where the program periodically calls a function to
check for new input.  This tends to be a good fit for applications structured
around a video rendering loop, which is typical of DirectX video games.
A rendering-loop-based application usually has to idle briefly between frames
to synchronize with the video hardware, which provides a natural place in
the program structure to poll for input.  The polling makes good use of
the inter-frame time, which otherwise the program would just have to burn
up waiting, plus it's usually the only point in the processing cycle where
new input can be fed into the physics model anyway.  If you're currently
reading keyboard or gamepad input using DirectInput or XInput or a similar
polling-based API, you should find it quite easy to add Open Pinball Device
support, because you'll just have to add a few function calls alongside
the existing calls you're already making to poll for input from DirectInput
or whatever other API you're currently using.

* An event-oriented interface, based on the Windows Raw Input API, where the
program receives input through WM_INPUT messages to its main window.  This
is a good fit for more traditional Windows GUI programs that use a window
procedure and WM_xxx events for other input processing, such as input from
the keyboard and mouse.  If you're already using Raw Input for other devices,
it's extremely easy to add Open Pinball Device support through this interface
option.  If you're not already using Raw Input, but you *are* handling messages
like WM_KEYDOWN, WM_LBUTTONDOWN, WM_MOUSEMOVE, etc., this will still be the
best fit for your program structure, but it'll require a little more work
because you'll have to add some extra Windows calls to set up Raw Input.
That's straightforward but a bit verbose.  It's worth the effort, though,
because Raw Input has extremely low latency and won't add the performance
overhead of polling.


## Basic usage: Polling interface

This API is defined in the header OpenPinballDeviceLib.h.

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


## Basic usage: Raw Input event-based interface

This API is defined in the header OpenPinballDeviceRawInput.h.

If your program **already uses Raw Input**, adding Open Pinball Device support
only requires adding a few lines of code.  First, in the code where you
initialize Raw Input with a call to `RegisterRawInputDevices()`, add the
following element to the array of RAWINPUTDEVICE structs that you pass
to the API (`hwnd` is the window handle of your main input window, where
you're handling WM_INPUT messages):

```
  { HID_USAGE_PAGE_GAME, HID_USAGE_GAME_PINBALL_DEVICE, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd }
```

Second, in the same code where you call `RegisterRawInputDevices()`,
create an OpenPinballDevice::RawInputReader object:

```
    pOpenPinballReader = new OpenPinballDevice::RawInputReader();
```

Stash that pointer somewhere where the window procedure containing your
WM_INPUT handler can get to it, such as a member variable of the window object.

Next, add a call to the `ProcessInput()` method of that instance to your existing
WM_INPUT handler:

```
    case WM_INPUT:
        // check to see if it's an Open Pinball Device event
        if (pOpenPinballReader->ProcessInput(lParam))
            break;

        // It's not Open Pinball Device - process as normal.
        // Any existing code you already had stays here.
        break;
```

And a similar call to your WM_INPUT_DEVICE_CHANGE handler (or add one with
the following):

```
	case WM_INPUT_DEVICE_CHANGE:
        // check for Open Pinball Device changes
        if (pOpenPinballReader->ProcessDeviceChange(wParam, lParam))
            break;

        // Any existing code you already had stays here.
        break;
```

Finally, at window destruction (WM_DESTROY), remember to include the HID_USAGE_GAME_PINBALL_DEVICE
entry from the original `RegisterRawInputDevices()` call in the array you pass to the de-registration
call you make here.

If you **don't** already process Raw Input in your program, you'll
need to add the WM_INPUT and WM_INPUT_DEVICE_CHANGE handlers as
shown above, and you'll also need to add a call to `RegisterRawInputDevices()`
to set up Raw Input message delivery.  You make that call during initialization -
typically while handling the WM_CREATE message for your main window, because
you'll need the input window handle in order to register for events.  The
Microsoft documentation has examples; you should follow those, except that
you'll have to include the RAWINPUTDEVICE struct shown above in your
registration call.  If you're adding Raw Input support purely for Open
Pinball Device's sake, that's the only RAWINPUTDEVICE struct you'll have
to provide during registration.  In addition to the registration call,
you have to make a matching de-registration call, typically in the
handler for WM_DESTROY for your main window, which just reiterates the
original registration call with RIDEV_REMOVE in the flags part of the struct.

Depending on how you'd like to consume the Open Pinball Device input,
your work could be done, or there could be a little more to do.  If
you just want to check the current instantaneous state of the Open
Pinball Device controls at specific points in your program flow,
you merely have to look at `pOpenPinballReader->state`, which is
an `OpenPinballDeviceReport` struct that's always up to date with the
current live state of the devices.  This combines input from all of
the physical Open Pinball Device instances, if there are more than one,
to give you the current state of each abstract control (buttons, nudge,
and plunger).

If you want to carry the event-oriented approach to the individual
abstract controls, you have to add a little more code.  The RawInputReader
class defines a collection of virtual methods that it calls whenever
an abstract control state changes.  These methods are there specifically
for you to override in a subclass.  So rather than creating the RawInputReader
class directly, you first subclass it, overriding the event handler
methods, and then create an instance of your subclass in place of
the base class.

```
class MyOpenPinballReader : public OpenPinballDevice::RawInputReader
{
public:
    MyOpenPinballReader(HWND hwnd) : OpenPinballDevice::RawInputReader(hwnd) { }

    // handle generic button changes (buttonNum 0 to 31)
    virtual void OnGenericButton(int buttonNum, bool pressed) { /* custom handler */ }

    // handle pinball-function button changes (OPENPINDEV_BTN_xxx)
    virtual void OnPinballButton(int buttonNum, bool pressed) { /* custom handler */ }

    // handle nudge change events
    virtual void OnNudge(int16_t ax, int16_t ay, int16_t vx, int16_t vy) { /* custom handler */ }

    // handle plunger change events
    virtual void OnPlunger(int16_t pos, int16_t speed) { /* custom handler */ }
};
```

The OnXxx() handlers are called when a change occurs on the corresponding
abstract control.  They save you the trouble of having to remember the
state of the controls or comparing new and old states: a call to
`OnGenericButton()` only happens when the button in question has changed
state.

These handlers are called **only** from within `ProcessInput()`, which
in turn is only invoked when your own code invokes it, typically from
your WM_INPUT handler.  So you don't have to worry about the event
handlers being called at unpredictable times or from unpredictable
contexts (such as from another thread).  They're only invoked when you
specifically allow them to be invoked by calling `ProcessInput()`.


## Sample applications

For a full usage example based on the polling interface, see the Open
Pinball Device Viewer sample application in the Open Pinball Device
repository.  That application is a Windows GUI program, so it's not
itself portable to other platforms, but the library calls work the
same way everywhere.

The Button Latency Tester 2 sub-project of Pinscape Pico uses the
Raw Input event-based interface.  That program is a lot more complex
because it handles several other input device types in addition to
Open Pinball Device, but it demonstrates how the OpenPinballDevice::RawInputReader
slots into a window-message-based application with a just a few lines
of added code.


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
