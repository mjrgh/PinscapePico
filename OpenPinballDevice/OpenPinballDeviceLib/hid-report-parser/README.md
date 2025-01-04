hid-report-parser
=================

Parser for USB HID input reports and their descriptors.

Pros:

- Depends only on the C/C++ standard library
- Only two source files:
  [`src/hid_report_parser.cpp`](./src/hid_report_parser.cpp) and
  [`src/hid_report_parser.h`](./src/hid_report_parser.h)
- High level programming interface that can map specific report fields onto
  the `int32_t` and `bool` variables of the program with a few lines of code
  - The descriptor's array input items are mapped onto bitfields so the
    program has to deal with only `bool` values to handle buttons/keys
- Report IDs are automatically handled behind the scene

Cons:

- The high level interface may not be flexible enough for certain use cases
- The library uses `stl` containers that perform dynamic memory allocation
  - Allocations take place in the `SelectiveInputReportParser::Init` method
    that maps report fields onto the program's variables - this is performed
    only once per report descriptor typically when a USB device is connected
  - Input reports are parsed into the program's variables without allocations

The dependency on `stl` makes this library a bad fit for embedded hardware
with limited resources (tiny amount of RAM, lack of `stl` support) but it
can provide great help while prototyping on beefier systems like the rp2040.


Usage
=====

Install this repo as a library into your Arduino IDE or simply copy the
[`src/hid_report_parser.cpp`](./src/hid_report_parser.cpp) and
[`src/hid_report_parser.h`](./src/hid_report_parser.h) files into your project.

Here is an example that handles the input reports of the connected USB mice:

```cpp
#include "hid_report_parser.h"

struct MountedMouse {
	hid::BitField<hid::MouseConfig::NUM_BUTTONS> buttons;
	hid::Int32Array<hid::MouseConfig::NUM_AXES> axes;
	hid::SelectiveInputReportParser parser;
};

std::map<uint16_t, std::unique_ptr<MountedMouse>> mounted_mice;

uint16_t map_key(uint8_t device_addr, uint8_t hid_interface_number) {
	return ((uint16_t)device_addr << 8) | hid_interface_number;
}

static constexpr int ERR_IGNORED_INTERFACE = 1;

// Returns zero on success.
int on_mount_hid_interface(uint8_t dev_addr, uint8_t hid_interface_num,
		const void* descriptor, size_t size) {
	// This example handles only mice and ignores other device types.
	std::unique_ptr<MountedMouse> p(new MountedMouse);
	auto buttons_ref = p->buttons.Ref();
	auto axes_ref = p->axes.Ref();
	// Using the builtin mouse mapping config. Check out the other builtin
	// mapping configs too: hid::KeyboardConfig, hid::GamepadConfig, etc...
	// You can create and use your own config if you know what you are doing.
	hid::MouseConfig cfg;
	auto cfg_root = cfg.Init(&buttons_ref, &axes_ref);

	// The parser.Init call tries to map the report fields specified in the
	// config onto the `buttons` and `axes` variables of the application.
	int res = p->parser.Init(cfg_root, descriptor, size);
	// Returns hid::ERR_COULD_NOT_MAP_ANY_USAGES if the descriptor doesn't
	// have an application container tagged with Usage(Mouse).
	if (res)
		return res;
	mounted_mice[map_key(dev_addr, hid_interface_num)] = std::move(p);

	// The cfg object is no longer needed after the parser.Init call but
	// it contains potentially useful information in its member variables:

	// Optional: Check which report fields are mapped onto variables.
	printf("V_SCROLL mapped=%d, H_SCROLL mapped=%d\n",
		cfg.axes.mapped[hid::MouseConfig::V_SCROLL] ? 1 : 0,
		cfg.axes.mapped[hid::MouseConfig::H_SCROLL] ? 1 : 0);
	// Optional: Check field properties. Unmapped fields equal to zero/false.
	printf("X min/max: %d/%d\n",
		cfg.axes.properties[hid::MouseConfig::X].logical_min,
		cfg.axes.properties[hid::MouseConfig::X].logical_max);
	return 0;
}

// Returns zero on success.
int on_input_report_received(uint8_t dev_addr, uint8_t hid_interface_num,
		const void* report, size_t size) {
	auto it = mounted_mice.find(map_key(dev_addr, hid_interface_num));
	if (it == mounted_mice.end())
		return ERR_IGNORED_INTERFACE;
	MountedMouse& m = *it->second.get();

	int res = m.parser.Parse(report, size);
	if (res)
		return res == hid::ERR_NOTHING_CHANGED ? 0 : res;

	// TODO: Process the mouse input. Unmapped fields equal to zero/false.
	printf("buttons=%d%d%d%d%d x=%-5d y=%-5d v_scroll=%-5d h_scroll=%-5d\n",
		m.buttons[hid::MouseConfig::BTN_LEFT]    ? 1 : 0,
		m.buttons[hid::MouseConfig::BTN_RIGHT]   ? 1 : 0,
		m.buttons[hid::MouseConfig::BTN_MIDDLE]  ? 1 : 0,
		m.buttons[hid::MouseConfig::BTN_BACK]    ? 1 : 0,
		m.buttons[hid::MouseConfig::BTN_FORWARD] ? 1 : 0,
		m.axes[hid::MouseConfig::X],
		m.axes[hid::MouseConfig::Y],
		m.axes[hid::MouseConfig::V_SCROLL],
		m.axes[hid::MouseConfig::H_SCROLL]);
	return 0;
}

void on_unmount_hid_interface(uint8_t dev_addr, uint8_t hid_interface_num) {
	mounted_mice.erase(map_key(dev_addr, hid_interface_num));
}
```

The above example can be found in the [`test/test.cpp`](./test/test.cpp) file
and can be executed as a test.

Arduino example for rp2040 boards:
[`examples/hid-mouse-host-and-device`](./examples/hid-mouse-host-and-device)
It can be accessed from the Arduino IDE's `File | Examples | HID report parser`
menu after installing this git repo as an Arduino library.


Detecting the type of a USB input device
========================================

One of the early requests sent by a USB host to a newly connected USB device
is `GET_DESCRIPTOR(CONFIGURATION)`. The response of a USB HID device contains
one or more HID interface descriptors. A HID interface descriptor contains a
reference to a HID report descriptor (that can be queried later by the host)
and other pieces of useful information like the `bInterfaceProtocol` field that
has 3 possible values: `None(0)`, `Keyboard(1)`, `Mouse(2)`. Using this field
you can tell if a specific HID interface of a USB device represents a keyboard
or a mouse before querying and parsing the related HID report descriptor.

In theory (according to the HID specification) the `bInterfaceProtocol` field
is populated only by keyboards and mice that support the boot protocol but in
practice it seems to be set even by the fanciest gaming mice that have zero
support for the boot protocol.
The reason for this may be the existence of BIOS implementations that can parse
HID report descriptors to be able to handle input devices that don't support
the boot protocol. The `bInterfaceProtocol` field can still help the BIOS in
picking the main or the simplest HID interface of the USB device.

Real-world example: The Logitech g305 is a popular wireless gaming mouse.
Its wireless USB dongle provides three HID interfaces with the following
`bInterfaceProtocol` values:

1. `bInterfaceProtocol = Keyboard(1)`
2. `bInterfaceProtocol = Mouse(2)`
3. `bInterfaceProtocol = None(0)`

This wireless dongle can be used to connect a Logitech keyboard too along with
the mouse - it has `bInterfaceProtocol = Keyboard(1)` in its first interface
because a BIOS could use this dongle to access a fully featured keyboard too.
The three HID report descriptors that belong to those HID interfaces can be
found in [`test/descriptor_samples.h`](./test/descriptor_samples.h):
`LOGITECH_G305_1`, `LOGITECH_G305_2`, `LOGITECH_G305_3`.

If your input device isn't one of the basic types (keyboard or mouse) then
the `bInterfaceProtocol` field is `None(0)` in all of its HID interface
descriptors so you have to query and scan the report descriptors to see
if you can find something useful.
You can guess the device type by inspecting the top-level application
collection(s) of a descriptor. Application collections that belong to keyboards,
mice or game controllers are usually tagged with one of the following usage values:

- `PAGE_GENERIC_DESKTOP / USAGE_MOUSE`
- `PAGE_GENERIC_DESKTOP / USAGE_KEYBOARD`
- `PAGE_GENERIC_DESKTOP / USAGE_KEYPAD`
- `PAGE_GENERIC_DESKTOP / USAGE_JOYSTICK`
- `PAGE_GENERIC_DESKTOP / USAGE_GAMEPAD`
- `PAGE_CONSUMER / USAGE_CONSUMER_CONTROL` (used for multimedia keys on keyboards)

These are the most common usages but they are far from being the only ones. You
can find less popular usages (for VR, flight/car/etc simulation controls, ...)
in the latest version of the HID Usage Tables pdf.

The `hid::detect_common_input_device_type()` function can tell which of the
above usages are present in a report descriptor. Several of them may be present
in a single descriptor - an example to this is the `HAVIT` mouse in
[`test/descriptor_samples.h`](./test/descriptor_samples.h).
It contains `USAGE_MOUSE`, `USAGE_KEYBOARD` and `USAGE_CONSUMER_CONTROL`.
However, its HID interface descriptor reveals the actual type of the device
with the `bInterfaceProtocol = Mouse(2)` value.
Gaming/productivity mice often add `USAGE_KEYBOARD` and `USAGE_CONSUMER_CONTROL`
interfaces to be able to simulate keyboards through programmable macros.
Unlike the Havit mouse, most devices opt for exposing these usages through
separate HID interfaces instead of cramming everything into a single HID
interface and report descriptor. Separate HID interfaces make it possible to
send the reports that belong to different usages separately without prefixing
them with 8-bit report IDs.

If you are looking for a simple solution that supports only basic keyboard/mouse
input then relying on the `bInterfaceProtocol` field and ignoring HID interfaces
that come with `bInterfaceProtocol = None(0)` is a solid approach. This is how a
typical BIOS implementation handles USB keyboards and mice.
If you want a comprehensive solution you have to do what a desktop operating
system does: enumerate all HID interfaces of the connected USB device, scan
the related HID report descriptors for usages (`USAGE_MOUSE`, `USAGE_JOYSTICK`,
etc...) and mount those HID interfaces that provide at least one usage that
you can handle.

If you go for the simpler approach (like a BIOS) and mount only those HID
interfaces that have `Keyboard(1)` or `Mouse(2)` in the `bInterfaceProtocol`
field then certain features of the mounted keyboards and mice may not work.
The reason for this is that a lot of keyboards and mice expose their features
through multiple HID interfaces and in a situation like that only one of them
is marked as a boot keyboard or mouse in its `bInterfaceProtocol` field.
A mouse that can emulate keyboard shortcuts through programmable macros usually
has an additional HID interface with a report descriptor that declares
`USAGE_KEYBOARD` but the `bInterfaceProtocol` field of the HID interface should
be `None(0)` because a BIOS shouldn't try to mount it as a fully functional
keyboard.
Another good example is the Raspberry PI keyboard that has two HID interfaces.
The first interface comes with `bInterfaceProtocol = Keyboard(1)` and provides
the basic keyboard keys with boot protocol support. The second interface
comes with `bInterfaceProtocol = None(0)` and provides the multimedia keys.
If you skip the second interface (because of its `bInterfaceProtocol = None(0)`
field) and mount only the first one then you can't use the multimedia keys.
You can find the report descriptors of the two Raspberry PI HID interfaces in
[`test/descriptor_samples.h`](./test/descriptor_samples.h)

Be warned: HID devices with incorrect HID report descriptors aren't unheard of.
Such devices aren't functional or aren't fully functional through generic HID
drivers that play by the rules and follow the HID specification (like this
library). They work only through their own drivers that ignore the incorrect
report descriptors and know/assume the correct report format based on the
VID/PID of the device (and perhaps some additional information like firmware
version retrived through vendor-specific queries).

Sometimes seemingly basic features (like extra buttons, NKRO) can be enabled
only through vendor-specific requests or drivers. The presence of a
vendor-specific protocol is bad design if the same functionality could be
provided through the standard HID protocol. An example to such bad design is
the three report descriptors of the CoolerMaster MasterKeys S keyboard: the
NKRO capability of the hardware isn't available through standard HID drivers.
A different type of frequently committed mistake (incorrect logical and
physical maximums) can be observed in the report descriptor of the
Speedlink Phantom Hawk joystick. These report descriptors can be found in
[`test/descriptor_samples.h`](./test/descriptor_samples.h)


Development Environment
=======================

I used Visual Studio 2019 Community Edition (a free C/C++/C# IDE for Windows)
to create and debug this library and its tests. You can find the IDE-specific
config files under the [`vs2019`](./vs2019) folder. The tests depend on the
GoogleTest framework that is downloaded automatically by Visual Studio's NuGet
package manager when you build the project.

There is nothing Windows-specific about this library and its tests so it should
be fairly easy to get them work without Visual Studio on any platform.

I work with Arduino boards on Linux or MacOS but often use the Windows-specific
Visual Studio IDE to edit and debug C/C++ code when I don't have Linux-specific
dependencies. The integrated debugger of Visual Studio is great and a paid
plugin (Visual Assist X) can provide best-in-class C/C++ code completion and
navigation. Unfortunately nothing comes close to this C/C++ dev environment
especially on larger codebases.
