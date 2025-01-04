# Pinscape Pico - USB Protocols

This folder contains C++ header files that define the Pinscape Pico's
custom USB protocols.  The headers are meant to serve both as
documentation for the wire interfaces, and as a basis for C++
implementations of the host and device sides of the protocol.  They
define structs, types, and constants useful for implementing the
protocols, and are extensively commented with narrative descriptions
of the workings of the protocols.  These serve as the specification
and "source of truth" for how the protocols are meant to work.

The headers are designed to be compatible with Microsoft Visual C++ on
the host and the ARM gcc cross-compiler for the device, so that the
same headers can be used in both sides of the implementation.


## FeedbackControllerProtocol.h

This defines the protocol used for Pinscape Pico's "Feedback
Controller" HID device.  The Feedback Controller is a special-purpose
HID device that lets the host send commands to the device to control
feedback ports (for example, activating lights or motors attached to
the device) and access other device functions useful during simulated
pinball game play.

This interface is designed especially for access from DOF, but it's
open to any other programs that want to access it directly, using the
protocol defined in this file.

The Feedback Controller is implemented as a HID device, which makes it
plug-and-play, with no device driver installation required, since it's
accessible through the built-in HID drivers on Windows and virtually
all other modern operating systems.  


## VendorIfcProtocol.h

This is the protocol used for Pinscape Pico's private "Vendor
Interface".  On Windows, the vendor interface can be accessed via the
WinUsb device driver.  WinUsb is a built-in component on all Windows
versions from Vista onward, so this is a plug-and-play interface that
doesn't require any user action to install device drivers.  (The
exception is Windows XP, where WinUsb is available but must be
manually installed.)  On Linux and MacOS, the same USB interface is
accessible via the open-source libusb system.  

The vendor interface is the primary access point for the Pinscape Pico
Config Tool.  It provides access to configuration, control, and
information query functions, such as installing a configuration file,
starting plunger calibration, accessing diagnostic logging
information, and running diagnostic tests.


## C++ API for client access

On Windows, we provide a separate, high-level C++ API for accessing
both of these interfaces from client programs, under the WinAPI/ folder.

The API library makes it much easier to use the protocol interfaces by
managing the details of the USB connections and exposing most of the
commands in the form of C++ function calls.  The protocol definition
headers defined here are mainly for use by programs that can't use the
C++ API and instead need to access the protocols directly at the USB
level, via the local operating system's USB API.

Note that some of the functions provided in the C++ API, particularly
those related to device discovery and connection setup, are details of
the operating system's USB interface that are outside of the scope of
the protocol itself.  The header files defined here don't have any
documentation at that level.  So even if you're planning to write a
program based directly on the USB protocols, you might still find it
useful to review the C++ API code for examples of how to use the
Windows USB APIs to discover the devices and interfaces, establish
connections, and transfer data on the connections.


## License

Copyright 2024, 2025 Michael J Roberts

Released under a BSD 3-clause license - NO WARRANTY
