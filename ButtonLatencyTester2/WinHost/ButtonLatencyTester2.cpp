// Pinscape Pico Button Latency Tester II - Windows host tool
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a simple console-mode program for use with the Pinscape Pico
// Button Latency Tester II firmware.  It reads high-level Windows input
// events and relates them back to hardware button presses on the Pico,
// to measure the elapsed time between the physical button press and the
// arrival of the corresponding Windows input event.
//
// We're NOT measuring latency on the BLT-II Pico!  The Windows input that
// we're monitoring DOESN'T come from that Pico.  It comes from a SEPARATE
// device, the "subject" device, which is the device we're timing.  This
// could be another Pico, such as one running Pinscape Pico, or it could
// be some entirely different kind of microcontroller, such as a KL25Z, or
// even a proprietary standalone button encoder like an I-PAC or Zero Delay
// Arcade board.  You could even use this to time a physical keyboard or
// joystick, if you didn't mind some invasive surgery to access its
// internal switch wiring.
// 
// To set up the physical wiring, you have to wire each button to BOTH
// the subject device (e.g., the KL25Z) AND the BLT-II Pico.  The best
// way to do this is to physically wire the button to two optocouplers
// in series, so that pushing the button actives both optocouplers at
// exactly the same time.  Then wire one optocoupler Collector (C) to
// an input port on the subject device, wire the other optocoupler's
// Collector to a BLT-II Pico GPIO port, and wire both optocoupler
// Emitter (E) pins to ground.  
// 
//                  -------------
//  5V---Button---1|(+)       (C)|4---Subject device port
//                 | L           |
//                 | E   PC817   |
//                 | D           |
//              --2|(-)       (E)|3---GND
//             |    --------------
//             |
//             |    -------------
//              --1|(+)       (C)|4---Pico measurement tool GPIO
//                 | L           |
//                 | E   PC817   |
//                 | D           |
//              --2|(-)       (E)|3---GND
//             |    --------------
//             |
//          130 Ohms (based on Vf=1.2V, 5V supply voltage, 20mA)
//             |
//            GND
//
// Principle of operation: When you press the physical button, it
// simultaneously (to within about a microsecond) activates both
// optocouplers, which in turn activate the button inputs on both
// the subject device and the measurement tool Pico.  The subject
// device should respond by sending an input signal to the PC, by
// whatever means it uses to communicate with the PC - USB, serial
// port, keyboard port, etc.  We don't care how the input reaches
// the PC from the subject device, since that's just part of what
// we're measuring.
// 
// At the same instant that the subject device gets the signal from
// the optocoupler, the Button Latency Tester II Pico gets the same
// signal from its optocoupler, and it records the time of the
// button press on its internal clock.  That's accurate to a few
// microseconds.  The BLT-II Pico now knows the exact time when
// the physical button press occurred.
//
// The quantity that we're interested in measuring is the "latency",
// or elapsed time, between the PHYSICAL button press and the
// arrival at the APPLICATION level of the high-level Windows input
// event that the button press translates into.  This might be a
// (virtual) keyboard key press, a joystick button press, an XBox
// controller button press, or an Open Pinball Device button press.
// That's where this HOST program comes in.  The host program's
// job is to monitor for these high-level Windows events.  Using a
// user-supplied configuration, the host program knows which Windows
// input event corresponds to which physical button, identified by
// the BLT-II Pico GPIO port that the physical button is wired to.
// When the host program receives a Windows input event that's in
// the configured set of inputs, it immediately sends a message to
// the Latency Tester Pico to get the elapsed time ON THE PICO
// between NOW and the time it recorded the physical button press
// on the same GPIO.
//
// This is about as close as we can get to the exact amount of
// time it takes from the physical button press to the arrival
// of the corresponding Windows input event at the application.
// The physical button press time measurement is nearly exact,
// since the Pico is physically wired to the button and can
// monitor it with latency on the order of a microsecond.  The
// time that the Pico receives the message from the PC is also
// very precise, and since it's measured against the same clock
// as the button press event (that is, the Pico's internal
// microsecond clock), the elapsed time calculation is exact.
// The only uncertainty in the calculation is the amount of time
// it takes for the USB command from the host to reach the Pico.


#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <Windows.h>
#include <winuser.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <dinput.h>
#include <conio.h>
#include <Xinput.h>
#include <wbemidl.h>
#include <oleauto.h>
#include "../../OpenPinballDevice/OpenPinballDeviceLib/OpenPinballDeviceRawInput.h"
#include "HiResTimer.h"
#include "RefPtr.h"
#include "BLTVendorInterface.h"

// libraries
#pragma comment(lib, "hid")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "OpenPinballDeviceLib")

// character set utilities
#ifdef UNICODE
#define TSTRFMT "ws"
#define TSTRFMTL L"ws"
#else
#define TSTRFMT "hs"
#define TSTRFMTL L"hs"
#endif

using namespace ButtonLatencyTester2;

// --------------------------------------------------------------------------
//
// Mutex for coordinating access to the vendor interfaces between threads.
// We use a single mutex for the whole interface collection (there's no need
// for anything more granular, since the event thread only needs to access
// the one device associated with the current event at any given time).
//
HANDLE mutex;


// --------------------------------------------------------------------------
//
// Window timer IDs
//

static const int TIMER_ID_DI_XI = 1;   // periodic DirectInput/XInput polling timer


// --------------------------------------------------------------------------
//
// Button Latency Tester II device list.  This is the list of all devices
// discovered at initialization.
//
struct Device
{
	Device(VendorInterface *ifc) : ifc(ifc)
	{
		// query the device ID, pull out the hardware ID
		ifc->QueryID(id);
		hwid = id.hwid.ToString();
	}

	// vendor interface
	std::unique_ptr<VendorInterface> ifc;

	// device ID
	DeviceID id;
	std::string hwid;
	
	// latest latency measurements from the device
	std::vector<VendorInterface::MeasurementData> data;
};
std::list<Device> devices;


// --------------------------------------------------------------------------
//
// Timing statistics for the Vendor Interface transactions used to report
// input events to the BLT-II Pico.  This is part of the systematic error
// inherent in our latency measurement procedure, so we'd like to collect
// statistics to characterize it, to help adjust the final figures for a
// more accurate view.
//
// The TRUE latency is defined as the time between the physical button press
// and the arrival of the input event (WM_INPUT message, DirectInput event,
// etc) at the host application interface layer, where the application can
// apply the event to its internal state (e..g, the Visual Pinball physics
// model).
//
// The MEASURED latency is the the time between the physical button press
// (as recorded on the BLT-II Pico) and the arrival of the host input
// notification on the BLT-II Pico.
// 
// We can't directly measure the TRUE latency, because the host and Pico
// don't share a common clock.  The MEASURED latency on the Pico is the
// closest we can come to a direct measurement, because both points in
// time are referenced to the Pico's clock.
//
// The difference between these two times is the sum of the processing
// time in the host application (that is, the time between the application
// receiving the WM_INPUT message in its window procedure and calling
// HostInputEvent() on the BLT-II device interface) and the time spent
// in the outbound leg of the USB transaction.  We can directly measure
// the first component, by noting the difference in Windows system clock
// times between the WM_INPUT arrival in the window procedure and the
// call to HostInputEvent() - those two events are both on the Windows
// machine, and are both within our application code, so we can read the
// Windows clock time at both points and calculate the difference.  This
// isn't perfect, since Windows could still preempt the thread between
// the clock measurement and the other end of the bracket, but that's
// unlikely because the code paths are so short and contain no I/O calls.
// 
// The second component - the outbound USB transaction from host to
// Pico - is impossible to measure directly.  The closest we can come
// is to measure the USB round-trip time.  We can then estimate that
// the outbound time is half of the total time.  That's what this object
// is for: it collects statistics on the USB round-trip time for the
// HostInputEvent() calls.
// 
// We don't collect timing statistics on any other USB transactions
// besides HostInputEvent(), because that's the only one that's
// relevant to the latency statistics.
//
struct USBStats
{
	// number of transactions
	uint64_t nTransactions = 0;

	//
	// All times are in microseconds
	//

	// total time (for average) and sum-of-squares time (for standard deviation)
	uint64_t timeSum = 0;
	uint64_t timeSquaredSum = 0;

	// min/max times
	uint64_t timeMin = 0;
	uint64_t timeMax = 0;

	// get the average
	double GetAvg() const { return nTransactions == 0 ? 0 : static_cast<double>(timeSum) / static_cast<double>(nTransactions); }

	// get the standard deviation
	double GetStdDev() const {
		if (nTransactions == 0) return 0;
		double mean = GetAvg();
		return sqrt(static_cast<double>(timeSquaredSum/nTransactions) - mean*mean);
	}

	// Collect a time (in microseconds)
	void Add(uint64_t dt)
	{
		timeSum += dt;
		timeSquaredSum += dt*dt;

		if (nTransactions == 0)
			timeMin = timeMax = dt;
		else
		{
			timeMin = min(dt, timeMin);
			timeMax = max(dt, timeMax);
		}

		nTransactions += 1;
	}
};
USBStats usbStats;


// set up a high-resolution timer instance
HiResTimer hiResTimer;



// --------------------------------------------------------------------------
//
// Which input subsystems are used?  We set these during button configuration,
// so that the input monitor can initialize only those subsystems that are
// actually needed for this session.  This avoids skewing the statistics with
// unnecessary CPU load or I/O contention from other subsystems.
//

static bool rawInputKbUsed = false;    // Raw Input keyboard
static bool rawInputHidUsed = false;   // Raw Input HID
static bool diKbUsed = false;          // DirectInput keyboard
static bool diJsUsed = false;          // DirectInput joystick/gamepad
static bool opdUsed = false;           // Open Pinball Device HID
static bool xinputUsed = false;        // XInput (XBox controller device)



// --------------------------------------------------------------------------
//
// Button descriptor.  This associates a Windows input event with
// a physical button, by way of the GPIO port that the button is
// wired to on the Pico running the Button Latency Tester II software.
//
class ButtonDesc
{
public:
	// friendly name
	std::string friendlyName;

	// Windows input event type
	enum class Type
	{
		// DirectInput keyboard key
		// id -> DirectInput key number
		DI_KEY,

		// RAW INPUT keyboard key
		// id -> key code
		RI_KEY,

		// DirectInput joystick button
		// id -> button number (we don't differentiate joystick units)
		DI_JSBUTTON,

		// RAW INPUT joystick button
		// id -> button number (we don't differentiate joystick units)
		RI_JSBUTTON,

		// Open Pinball Device button
		// id -> OPD button number
		OPD_BUTTON,

		// XInput button
		// id -> XINPUT_GAMEPAD::wButtons bit mask
		XI_BUTTON,
	};
	Type type;

	// Button ID.  The meaning depends on the type (see above)
	int id;

	// Button Latency Tester II Pico where the button is wired
	Device *device;

	// GPIO on the BLT-II Pico where the button is wired
	int gpio;

	// last state (true -> pressed)
	bool pressed = false;

	// construction
	ButtonDesc(const char *friendlyName, Type type, int id, Device *device, int gpio) :
		friendlyName(friendlyName), type(type), id(id), device(device), gpio(gpio) { }

	// find a MeasurementData record in the device latency data vector
	VendorInterface::MeasurementData *GetMeasurementData() const
	{
		auto it = std::find_if(device->data.begin(), device->data.end(),
			[gpio = this->gpio](const VendorInterface::MeasurementData &d) { return d.gp == gpio; });

		return it != device->data.end() ? &*it : nullptr;
	}

	// Process an input event received for this button
	void OnInput(bool newPressed, uint64_t tEvent)
	{
		// if the button is newly pressed, notify the device
		if (newPressed && !pressed)
		{
			// acquire the mutex
			if (WaitForSingleObject(mutex, 1000) == WAIT_OBJECT_0)
			{
				// time the vendor interface transaction
				uint64_t t0 =  hiResTimer.GetTime_ticks();
				
				// notify the device
				VendorInterface::HostInputEventResult result;
				int stat = device->ifc->HostInputEvent(gpio, tEvent, result);

				// note the elapsed time
				int64_t t1 = hiResTimer.GetTime_ticks();

				// done with the mutex
				ReleaseMutex(mutex);

				// check results
				if (stat == VendorResponse::OK)
				{
					// Collect timing statistics on the USB vendor interface transaction.
					// This isn't part of the latency calculation; it's just performance
					// information for the test system itself.  (Caveat: the performance
					// of the USB interface doesn't affect the latency calculation as long
					// as SOF timestamps are available from the USB hardware driver, since
					// SOF synchronization allows direct, high-precision comparisons
					// between the Pico and Windows system clocks.  The USB transaction
					// time does matter if SOF timing is unavailable on the host, though,
					// because in the absence of SOF synchronization, the Pico has to
					// base the latency calculation on the round-trip time for the event,
					// including the outbound leg of the USB HostInputEvent() command.
					// So knowing the USB timing is actually useful when SOF timing is
					// unavailable, since you'd want to deduct about half of the USB
					// round-trip time from each measured latency.  When SOF timing is
					// working, though, the reported figures can be taken as reliable at
					// face value.)
					usbStats.Add(hiResTimer.TicksToUs64(t1 - t0));

					// debugging
					using Status = VendorInterface::HostInputEventResult::Status;
					static const std::unordered_map<Status, const char*> statusMap{
						{ Status::Unknown, "Unknown" },
						{ Status::Matched, "Matched" },
						{ Status::NotMatched, "Not Matched" },
						{ Status::Duplicate, "Duplicate (not included in stats)" },
					};
					auto it = statusMap.find(result.status);
					printf("Event %s: SOF %s, status %s",
						friendlyName.c_str(),
						result.sofTimeAvailable ? "OK" : "not available",
						it != statusMap.end() ? it->second : "Unknown");

					if (result.status == Status::Matched)
						printf(", measured latency %.3f", static_cast<float>(result.latency) / 1000.0f);

					printf("\n");
				}
			}
			else
			{
				printf("Input event handler: error acquiring device mutex\n");
			}
		}

		// update our internal memory of the button state
		pressed = newPressed;
	}
};
static std::list<ButtonDesc> buttons;

// Joystick button indices, for fast lookup by joystick button number
static ButtonDesc *riJsButtons[33]{ nullptr };
static ButtonDesc *diJsButtons[33]{ nullptr };

// Open Pinball Device indices.  0-31 are the generic numbered buttons;
// 32-63 are the named buttons.
static ButtonDesc *opdButtons[64]{ nullptr };

// --------------------------------------------------------------------------
//
// Keyboard
// 

// Key names to Raw Input and DirectInput ID values
struct KeyDef
{
	KeyDef(const char *desc, int scan, int vkey, int diKey) : desc(desc), scan(scan), vkey(vkey), diKey(diKey) { }

	// friendly name
	const char *desc;

	// Modified scan code, with extended key code (E0, E1) in high byte
	int scan;

	// VK_xxx virtual key code
	int vkey;

	// DIK_xxx DirectInput key code
	int diKey;
};
static const std::unordered_map<std::string, KeyDef> keyMap{
	{ "esc", { "Escape", 0x01, 0x1b, DIK_ESCAPE } },
	{ "1", { "1 !", 0x02, 0x31, DIK_1 } },
	{ "2", { "2 @", 0x03, 0x32, DIK_2 } },
	{ "3", { "3 #", 0x04, 0x33, DIK_3 } },
	{ "4", { "4 $", 0x05, 0x34, DIK_4 } },
	{ "5", { "5 %", 0x06, 0x35, DIK_5 } },
	{ "6", { "6 ^", 0x07, 0x36, DIK_6 } },
	{ "7", { "7 &", 0x08, 0x37, DIK_7 } },
	{ "8", { "8 *", 0x09, 0x38, DIK_8 } },
	{ "9", { "9 (", 0x0a, 0x39, DIK_9 } },
	{ "0", { "0 )", 0x0b, 0x30, DIK_0 } },
	{ "minus", { "- _", 0x0c, 0xbd, DIK_MINUS } },
	{ "equals", { "= +", 0x0d, 0xbb, DIK_EQUALS } },
	{ "backspace", { "Backspace", 0x0e, 0x8, DIK_BACKSPACE } },
	{ "tab", { "Tab", 0x0f, 0x9, DIK_TAB } },
	{ "q", { "Q", 0x10, 0x51, DIK_Q } },
	{ "w", { "W", 0x11, 0x57, DIK_W } },
	{ "e", { "E", 0x12, 0x45, DIK_E } },
	{ "r", { "R", 0x13, 0x52, DIK_R } },
	{ "t", { "T", 0x14, 0x54, DIK_T } },
	{ "y", { "Y", 0x15, 0x59, DIK_Y } },
	{ "u", { "U", 0x16, 0x55, DIK_U } },
	{ "i", { "I", 0x17, 0x49, DIK_I } },
	{ "o", { "O", 0x18, 0x4f, DIK_O } },
	{ "p", { "P", 0x19, 0x50, DIK_P } },
	{ "lbracket", { "[ {", 0x1a, 0xdb, DIK_LBRACKET } },
	{ "rbracket", { "] }", 0x1b, 0xdd, DIK_RBRACKET } },
	{ "enter", { "Enter", 0x1c, 0xd, DIK_RETURN } },
	{ "lctrl", { "Left Ctrl", 0x1d, 0xa2, DIK_LCONTROL } },
	{ "a", { "A", 0x1e, 0x41, DIK_A } },
	{ "s", { "S", 0x1f, 0x53, DIK_S } },
	{ "d", { "D", 0x20, 0x44, DIK_D } },
	{ "f", { "F", 0x21, 0x46, DIK_F } },
	{ "g", { "G", 0x22, 0x47, DIK_G } },
	{ "h", { "H", 0x23, 0x48, DIK_H } },
	{ "j", { "J", 0x24, 0x4a, DIK_J } },
	{ "k", { "K", 0x25, 0x4b, DIK_K } },
	{ "l", { "L", 0x26, 0x4c, DIK_L } },
	{ "semicolon", { "; :", 0x27, 0xba, DIK_SEMICOLON } },
	{ "apostrophe", { "' \"", 0x28, 0xde, DIK_APOSTROPHE } },
	{ "grave", { "` ~", 0x29, 0xc0, DIK_GRAVE } },
	{ "lshift", { "Left Shift", 0x2a, 0xa0, DIK_LSHIFT } },
	{ "backslash", { "\\ |", 0x2b, 0xdc, DIK_BACKSLASH } },
	{ "z", { "Z", 0x2c, 0x5a, DIK_Z } },
	{ "x", { "X", 0x2d, 0x58, DIK_X } },
	{ "c", { "C", 0x2e, 0x43, DIK_C } },
	{ "v", { "V", 0x2f, 0x56, DIK_V } },
	{ "b", { "B", 0x30, 0x42, DIK_B } },
	{ "n", { "N", 0x31, 0x4e, DIK_N } },
	{ "m", { "M", 0x32, 0x4d, DIK_M } },
	{ "comma", { ", <", 0x33, 0xbc, DIK_COMMA } },
	{ "period", { ". >", 0x34, 0xbe, DIK_PERIOD } },
	{ "slash", { "/ ?", 0x35, 0xbf, DIK_SLASH } },
	{ "rshift", { "Right Shift", 0x36, 0xa1, DIK_RSHIFT } },
	{ "numpad-star", { "Numpad *", 0x37, 0x6a, DIK_NUMPADSTAR } },
	{ "lalt", { "Left Alt", 0x38, 0xa4, DIK_LALT } },
	{ "space", { "Space", 0x39, 0x20, DIK_SPACE } },
	{ "capslock", { "Caps Lock", 0x3a, 0x14, DIK_CAPSLOCK } },
	{ "f1", { "F1", 0x3b, 0x70, DIK_F1 } },
	{ "f2", { "F2", 0x3c, 0x71, DIK_F2 } },
	{ "f3", { "F3", 0x3d, 0x72, DIK_F3 } },
	{ "f4", { "F4", 0x3e, 0x73, DIK_F4 } },
	{ "f5", { "F5", 0x3f, 0x74, DIK_F5 } },
	{ "f6", { "F6", 0x40, 0x75, DIK_F6 } },
	{ "f7", { "F7", 0x41, 0x76, DIK_F7 } },
	{ "f8", { "F8", 0x42, 0x77, DIK_F8 } },
	{ "f9", { "F9", 0x43, 0x78, DIK_F9 } },
	{ "f10", { "F10", 0x44, 0x79, DIK_F10 } },
	{ "pause", { "Pause", 0x45, 0x90, DIK_PAUSE } },
	{ "scrlock", { "Scroll Lock", 0x46, 0x91, DIK_SCROLL } },
	{ "numpad7", { "Numpad 7", 0x47, 0x67, DIK_NUMPAD7 } },
	{ "numpad8", { "Numpad 8", 0x48, 0x68, DIK_NUMPAD8 } },
	{ "numpad9", { "Numpad 9", 0x49, 0x69, DIK_NUMPAD9 } },
	{ "numpad-minus", { "Numpad -", 0x4a, 0x6d, DIK_NUMPADMINUS } },
	{ "numpad4", { "Numpad 4", 0x4b, 0x64, DIK_NUMPAD4 } },
	{ "numpad5", { "Numpad 5", 0x4c, 0x65, DIK_NUMPAD5 } },
	{ "numpad6", { "Numpad 6", 0x4d, 0x66, DIK_NUMPAD6 } },
	{ "numpad-plus", { "Numpad +", 0x4e, 0x6b, DIK_NUMPADPLUS } },
	{ "numpad1", { "Numpad 1", 0x4f, 0x61, DIK_NUMPAD1 } },
	{ "numpad2", { "Numpad 2", 0x50, 0x62, DIK_NUMPAD2 } },
	{ "numpad3", { "Numpad 3", 0x51, 0x63, DIK_NUMPAD3 } },
	{ "numpad0", { "Numpad 0", 0x52, 0x60, DIK_NUMPAD0 } },
	{ "numpad-period", { "Numpad .", 0x53, 0x6e, DIK_NUMPADPERIOD } },
	{ "f11", { "F11", 0x57, 0x7a, DIK_F11 } },
	{ "f12", { "F12", 0x58, 0x7b, DIK_F12 } },
	{ "numpad-equals", { "Numpad =", 0x59, 0x59, DIK_NUMPADEQUALS } },
	{ "f13", { "F13", 0x64, 0x7c, DIK_F13 } },
	{ "f14", { "F14", 0x65, 0x7d, DIK_F14 } },
	{ "f15", { "F15", 0x66, 0x7e, DIK_F15 } },
	{ "f16", { "F16", 0x67, 0x7f, -1 } },
	{ "f17", { "F17", 0x68, 0x80, -1 } },
	{ "f18", { "F18", 0x69, 0x81, -1 } },
	{ "f19", { "F19", 0x6a, 0x82, -1 } },
	{ "f20", { "F20", 0x6b, 0x83, -1 } },
	{ "f21", { "F21", 0x6c, 0x84, -1 } },
	{ "f22", { "F22", 0x6d, 0x85, -1 } },
	{ "f23", { "F23", 0x6e, 0x86, -1 } },
	{ "f24", { "F24", 0x76, 0x87, -1 } },
	{ "prev-track", { "Prev Track", 0xE010, 0xb1, DIK_PREVTRACK } },
	{ "next-track", { "Next Track", 0xE019, 0xb0, DIK_NEXTTRACK } },
	{ "numpad-enter", { "Numpad Enter", 0xE01c, -1, DIK_NUMPADENTER } },
	{ "rctrl", { "Right Ctrl", 0xE01d, 0xa3, DIK_RCONTROL } },
	{ "mute", { "Mute", 0xE020, 0xad, DIK_MUTE } },
	{ "play", { "Play/Pause", 0xE022, 0xb3, DIK_PLAYPAUSE } },
	{ "stop", { "Stop", 0xE024, 0xb2, DIK_STOP } },
	{ "vol-down", { "Volume Down", 0xE02e, 0xae, DIK_VOLUMEDOWN } },
	{ "vol-up", { "Volume Up", 0xE030, 0xaf, DIK_VOLUMEUP } },
	{ "numpad-slash", { "Numpad /", 0xE035, 0x6f, DIK_NUMPADSLASH } },
	{ "prtscr", { "Print Screen", 0xE037, -1, -1 } },
	{ "ralt", { "Right Alt", 0xE038, 0xa5, DIK_RALT } },
	{ "numlock", { "Num Lock", 0xE045, 0x90, DIK_NUMLOCK } },
	{ "home", { "Home", 0xE047, 0x24, DIK_HOME } },
	{ "up", { "Up Arrow", 0xE048, 0x26, DIK_UP } },
	{ "pgup", { "Pg Up", 0xE049, 0x21, DIK_PGUP } },
	{ "left", { "Left Arrow", 0xE04b, 0x25, DIK_LEFT } },
	{ "right", { "Right Arrow", 0xE04d, 0x27, DIK_RIGHT } },
	{ "end", { "End", 0xE04f, 0x23, DIK_END } },
	{ "down", { "Down Arrow", 0xE050, 0x28, DIK_DOWN } },
	{ "pgdn", { "Pg Dn", 0xE051, 0x22, DIK_PGDN } },
	{ "ins", { "Insert", 0xE052, 0x2d, DIK_INSERT } },
	{ "del", { "Delete", 0xE053, 0x2e, DIK_DELETE } },
	{ "lwin", { "Left Windows key", 0xE05b, 0x5b, DIK_LWIN } },
	{ "rwin", { "Right Windows key", 0xE05c, 0x5c, DIK_RWIN } },
	{ "application", { "Applications Key", 0xE05d, 0x5d, DIK_APPS } },
};

// --------------------------------------------------------------------------
//
// Raw Input joysticks
// 

// maximum joystick button number
static const int MAX_JOYSTICK_BUTTON = 32;

class RawInputJoystick
{
public:
	RawInputJoystick(HANDLE hDevice, const RID_DEVICE_INFO_HID *info) : hDevice(hDevice)
	{
		// for debugging purposes, get the device product name and serial number
		TCHAR devPath[512];
		UINT sz = _countof(devPath);
		GetRawInputDeviceInfo(hDevice, RIDI_DEVICENAME, &devPath, &sz);
		HANDLE fp = CreateFile(
			devPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			0, OPEN_EXISTING, 0, 0);
		WCHAR prodName[128] = L"";
		WCHAR serial[128] = L"";
		if (fp != INVALID_HANDLE_VALUE)
		{
			// query the product name
			if (!HidD_GetProductString(fp, prodName, _countof(prodName)))
				prodName[0] = 0;

			// query the serial number string
			if (!HidD_GetSerialNumberString(fp, serial, _countof(serial)))
				serial[0] = 0;

			// done with the HidD device object handle
			CloseHandle(fp);
		}

		// If we weren't able to get a product name out of HidD, 
		// synthesize a semi-friendly name from the VID/PID codes.
		// It's rare to have more than one device of the same type 
		// in a system, and most devices have hard-coded VID/PID 
		// codes, so this should give us a unique name and stable
		// name that we can use to correlate config records to the
		// same device in a future session even if the "device name"
		// path changes (due to reinstallation, e.g.).
		if (prodName[0] == 0)
		{
			swprintf_s(prodName, L"Joystick %04lx:%04lx",
				info->dwVendorId, info->dwProductId);
		}

		// If we didn't get a serial, synthesize a placeholder serial number
		if (serial[0] == 0)
			wcscpy_s(serial, L"00000000");

		// store the identifiers
		this->prodName = prodName;
		this->serial = serial;
		this->vid = info->dwVendorId;
		this->pid = info->dwProductId;

		// retrieve the preparsed data size
		UINT ppdSize = 0;
		GetRawInputDeviceInfo(hDevice, RIDI_PREPARSEDDATA, 0, &ppdSize);
		if (ppdSize != 0)
		{
			// allocate space for the preparsed data
			ppdBuf.reset(new (std::nothrow) BYTE[ppdSize]);
			if (ppdBuf.get() != nullptr)
			{
				// retrieve the preparsed data
				UINT ppdActual = ppdSize;
				GetRawInputDeviceInfo(hDevice, RIDI_PREPARSEDDATA, ppdBuf.get(), &ppdActual);
				this->ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(ppdBuf.get());
			}
		}
	}

	// device information
	HANDLE hDevice;
	std::wstring prodName;
	std::wstring serial;
	DWORD vid, pid;

	// preparsed data
	PHIDP_PREPARSED_DATA ppd = NULL;
	std::unique_ptr<BYTE> ppdBuf;

	// Current buttons states.  We recognize joystick buttons 1-32 (so
	// the array size is 33, to make room for the unused element #0).
	bool buttonState[MAX_JOYSTICK_BUTTON + 1]{ false };

	// add a device
	static void AddDevice(HANDLE hDevice, const RID_DEVICE_INFO_HID *info)
	{
		// if we already have an entry for this device, ignore it
		if (devices.find(hDevice) != devices.end())
			return;

		// add the device
		devices.emplace(std::piecewise_construct,
			std::forward_as_tuple(hDevice),
			std::forward_as_tuple(hDevice, info));
	}

	// remove a device
	static void RemoveDevice(HANDLE hDevice)
	{
		// remove the device from our table
		devices.erase(hDevice);
	}

	// list of active devices
	static std::unordered_map<HANDLE, RawInputJoystick> devices;
};
std::unordered_map<HANDLE, RawInputJoystick> RawInputJoystick::devices;


// --------------------------------------------------------------------------
//
// Open Pinball Device
// 

// OPD pinball button names
struct OPDButtonDesc
{
	OPDButtonDesc(const char *desc, int buttonNum) : desc(desc), buttonNum(buttonNum) { }
	const char *desc;
	int buttonNum;
};
static const std::unordered_map<std::string, OPDButtonDesc> opdButtonNames{
	{ "start", { "Start", 0 } },
	{ "exit", { "Exit", 1 } },
	{ "extra-ball", { "Extra Ball", 2 } },
	{ "coin1", { "Coin slot 1", 3 } },
	{ "coin2", { "Coin slot 2", 4 } },
	{ "coin3", { "Coin slot 3", 5 } },
	{ "coin4", { "Coin slot 4", 6 } },
	{ "launch", { "Launch Ball", 7 } },
	{ "fire", { "Lockbar Fire button", 8 } },
	{ "lflipper", { "Left Flipper", 9 } },
	{ "rflipper", { "Right Flipper", 10 } },
	{ "lflipper2", { "Left Flipper 2nd switch", 11 } },
	{ "rflipper2", { "Right Flipper 2nd switch", 12 } },
	{ "lmagna", { "Left MagnaSave", 13 } },
	{ "rmagna", { "Right MagnaSave", 14 } },
	{ "tilt", { "Tilt bob", 15 } },
	{ "slam-tilt", { "Slam tilt switch", 16 } },
	{ "coin-door", { "Coin door position switch", 17 } },
	{ "service-cancel", { "Service Cancel/Esc", 18 } },
	{ "service-down", { "Service Down/-", 19 } },
	{ "service-up", { "Service Up/+", 20 } },
	{ "service-enter", { "Service Enter/Select", 21 } },
	{ "lnudge", { "Left simulated nudge", 22 } },
	{ "cnudge", { "Center simulated nudge", 23 } },
	{ "rnudge", { "Right simulated nudge", 24 } },
	{ "volup", { "Volume Up", 25 } },
	{ "voldown", { "Volume Down", 26 } },
};

// Custom Open Pinball Device Raw Input reader subclass, defining
// our event handler callbacks
class CustomOpenPinballDeviceReader : public OpenPinballDevice::RawInputReader 
{
public:
	CustomOpenPinballDeviceReader(HWND hwnd) : RawInputReader(hwnd) { }

	// process an input event at a given timestamp
	bool ProcessInput(HRAWINPUT hRawInput, int64_t tEvent)
	{
		// remember the event time, for use in the button callbacks
		this->tEvent = tEvent;

		// process the event via the base class
		return RawInputReader::ProcessInput(hRawInput);
	}

	// current event time
	int64_t tEvent = 0;

	// generic button event handler
	virtual void OnGenericButton(int button, bool pressed)
	{
		if (button >= 0 && button <= 31)
		{
			// generic buttons are numbered 0..31 in the opdButtons[] array,
			// so the nominal button number is also the opdButtons[] index
			if (auto *bp = opdButtons[button]; bp != nullptr)
				bp->OnInput(pressed, tEvent);
		}
	}

	// pinball-function button event handler
	virtual void OnPinballButton(int button, bool pressed)
	{
		if (button >= 0 && button <= 31)
		{
			// the pinball-function buttons are numbered 32..64 in the opdButtons[]
			// array, so the array index is the nominal button number plus 32
			if (auto *bp = opdButtons[button + 32]; bp != nullptr)
				bp->OnInput(pressed, tEvent);
		}
	}
};


// reader object - instantiated in the monitor thread if required
std::unique_ptr<CustomOpenPinballDeviceReader> opdReader;

// initialize
void InitOpenPinballDevice(HWND hwnd)
{
	if (opdUsed)
	{
		// create the Open Pinball Device reader
		opdReader.reset(new CustomOpenPinballDeviceReader(hwnd));
	}
}

// terminate
void TerminateOpenPinballDevice()
{
}


// --------------------------------------------------------------------------
//
// Raw Input
//

// initialize Raw Input
void InitRawInput(HWND hwnd)
{
	// raw input device registration list
	RAWINPUTDEVICE d[4];
	int i = 0;

	// if Raw Input keyboard is used, add keyboard input to the registration list
	if (rawInputKbUsed)
	{
		d[i++] = { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK, hwnd };
	}

	// if Raw Input HID input is used, register for HID input, and build the device list
	if (rawInputHidUsed)
	{
		// add HID joysticks and gamepads to the Raw Input registration list
		d[i++] = { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd };
		d[i++] = { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd };
	}

	// if Open Pinball Device is used, register for its input
	if (opdUsed)
	{
		d[i++] = { HID_USAGE_PAGE_GAME, HID_USAGE_GAME_PINBALL_DEVICE, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd };
	}

	// If any Raw Input HID input is used, build the device list
	if (rawInputHidUsed || opdUsed)
	{
		// build the Raw Input HID device list
		UINT numDevices = 0;
		GetRawInputDeviceList(0, &numDevices, sizeof(RAWINPUTDEVICELIST));

		// Allocate space for the list
		std::unique_ptr<RAWINPUTDEVICELIST> buf(new (std::nothrow) RAWINPUTDEVICELIST[numDevices]);
		if (buf.get() != nullptr)
		{
			// Retrieve the device list; fail if that doesn't return the expected device count
			UINT numActual = numDevices;
			if (GetRawInputDeviceList(buf.get(), &numActual, sizeof(RAWINPUTDEVICELIST)) == numDevices)
			{
				// process the list
				RAWINPUTDEVICELIST *r = buf.get();
				for (UINT i = 0; i < numDevices; ++i, ++r)
				{
					RID_DEVICE_INFO info;
					UINT sz = info.cbSize = sizeof(info);
					if (GetRawInputDeviceInfo(r->hDevice, RIDI_DEVICEINFO, &info, &sz) != (UINT)-1)
					{
						switch (info.dwType)
						{
						case RIM_TYPEHID:
							// HID device - check the type
							if (rawInputHidUsed
								&& info.hid.usUsagePage == HID_USAGE_PAGE_GENERIC
								&& (info.hid.usUsage == HID_USAGE_GENERIC_JOYSTICK || info.hid.usUsage == HID_USAGE_GENERIC_GAMEPAD))
							{
								// joystick - add it to our joystick list
								RawInputJoystick::AddDevice(r->hDevice, &info.hid);
							}
							break;
						}
					}
				}
			}
		}
	}

	// if we need input from any Raw Input devices, register for WM_INPUT messages
	if (i != 0)
		RegisterRawInputDevices(d, i, sizeof(d[0]));
}

// de-initialize Raw Input
void TerminateRawInput()
{
	// build the list of devices we registered
	RAWINPUTDEVICE d[4];
	int i = 0;

	// add the keyboard, if used
	if (rawInputKbUsed)
		d[i++] = { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_REMOVE, NULL };

	// add HID joysticks/gamepads, if used
	if (rawInputHidUsed)
	{
		d[i++] = { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK, RIDEV_REMOVE, NULL };
		d[i++] = { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD, RIDEV_REMOVE, NULL };
	}

	// if Open Pinball Device is used, register for its input
	if (opdUsed)
		d[i++] = { HID_USAGE_PAGE_GAME, HID_USAGE_GAME_PINBALL_DEVICE, RIDEV_REMOVE, NULL };

	// if any devices were registered, de-register
	if (i != 0)
		RegisterRawInputDevices(d, i, sizeof(d[0]));
}

// process a WM_INPUT messages
void ProcessRawInput(HRAWINPUT hRawInput, int64_t tEvent)
{
	// get the data size
	UINT sz = 0;
	std::vector<BYTE> buf;
	if (GetRawInputData(hRawInput, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) == 0)
	{
		// allocate space and retrieve the data
		buf.resize(sz);
		if (GetRawInputData(hRawInput, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) != sz)
			return;
	}

	// parse it
	auto &ri = *reinterpret_cast<RAWINPUT*>(buf.data());
	if (ri.header.dwType == RIM_TYPEKEYBOARD)
	{
		// Keyboard input
		auto &rk = ri.data.keyboard;
		bool keyDown = ((rk.Flags & RI_KEY_BREAK) == 0);
		uint16_t scanCode = rk.MakeCode;
		if (scanCode != 0)
		{
			// scan code is available - set the high byte to E0 or E1 for extended keys
			if ((rk.Flags & RI_KEY_E0) != 0) scanCode |= 0xE000;
			if ((rk.Flags & RI_KEY_E1) != 0) scanCode |= 0xE100;
		}
		else
		{
			// no scan code - try mapping a scan code from the Virtual Key code
			scanCode = LOWORD(MapVirtualKey(rk.VKey, MAPVK_VK_TO_VSC_EX));
		}

		// look up the key in the mapped button list
		auto it = std::find_if(buttons.begin(), buttons.end(), [scanCode](const ButtonDesc &b) {
			return b.type == ButtonDesc::Type::RI_KEY && b.id == scanCode; });
		if (it != buttons.end())
			it->OnInput(keyDown, tEvent);
	}
	else if (ri.header.dwType == RIM_TYPEHID)
	{
		// HID input - gamepad, joystick
		auto &rh = ri.data.hid;

		// search for the device in our joystick and OPD lists
		if (auto it = RawInputJoystick::devices.find(ri.header.hDevice); it != RawInputJoystick::devices.end())
		{
			// get our internal joystick object
			auto *js = &it->second;

			// The raw input data consists of one or more reports from the HID device.
			// Each report is in the device's custom reporting format.  Fortunately, we
			// don't have to parse every random device's custom HID report format
			// ourselves, since the HidP API can do that for us to extract the button
			// states contained in the report.
			BYTE *pRawData = rh.bRawData;
			for (DWORD i = 0 ; i < rh.dwCount ; ++i, pRawData += rh.dwSizeHid)
			{
				// Get the buttons that are marked as ON in this report (up
				// to 32 buttons).  Note that the joystick reports are "status",
				// not "events": they don't tell us what changed, but rather
				// just what the instantaneous state is.  It's up to us to
				// determine what's changed since our last update.  Start by
				// extracting the new ON button list.
				USAGE onUsage[MAX_JOYSTICK_BUTTON]{ 0 };
				ULONG onCnt = _countof(onUsage);
				if (HidP_GetButtons(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, onUsage, &onCnt, js->ppd,
					reinterpret_cast<PCHAR>(pRawData), rh.dwSizeHid) == HIDP_STATUS_SUCCESS)
				{
					// HidP_GetButtons only tells us which buttons are ON; everything
					// that doesn't appear in the list is OFF.
					bool newState[MAX_JOYSTICK_BUTTON + 1]{ false };
					for (USAGE *pOn = &onUsage[0] ; onCnt != 0 ; --onCnt)
					{
						// *pOn contains the Usage code for the button, which is the same
						// as the button number.  If it's in range (1..32), mark the button
						// as ON in our new-state array.
						USAGE btnUsage = *pOn++;
						if (btnUsage >= 1 && btnUsage <= MAX_JOYSTICK_BUTTON)
							newState[btnUsage] = true;
					}

					// Now go through the buttons to detect which ones have changed on
					// this controller since the last update.
					for (UINT btnNum = 1 ; btnNum <= MAX_JOYSTICK_BUTTON ; ++btnNum)
					{
						// if we're tracking this input, and the state has changed, process the event
						bool pressed = newState[btnNum];
						if (auto *bp = riJsButtons[btnNum]; bp != nullptr && pressed != js->buttonState[btnNum])
							bp->OnInput(pressed, tEvent);

						// record the new state
						js->buttonState[btnNum] = pressed;
					}
				}
			}
		}
		else if (opdReader != nullptr && opdReader->ProcessInput(hRawInput, tEvent))
		{
			// handled - the reader processed any button changes through our callbacks
			// in the custom input handler
		}
	}
}

// process a Raw Input device change notification
void ProcessRawInputDeviceChange(USHORT what, HANDLE hDevice)
{
	// if we have an Open Pinball Device reader, let it try handling the event
	if (opdReader != nullptr && opdReader->ProcessDeviceChange(what, hDevice))
	{
		// handled - no further processing is required
		return;
	}

	// check the event type
	switch (what)
	{
	case GIDC_ARRIVAL:
		{
			RID_DEVICE_INFO info{ sizeof(info) };
			UINT sz = sizeof(info);
			if (GetRawInputDeviceInfo(hDevice, RIDI_DEVICEINFO, &info, &sz) != static_cast<UINT>(-1))
			{
				switch (info.dwType)
				{
				case RIM_TYPEHID:
					// check the device type
					if (info.hid.usUsagePage == HID_USAGE_PAGE_GENERIC
						&& (info.hid.usUsage == HID_USAGE_GENERIC_JOYSTICK || info.hid.usUsage == HID_USAGE_GENERIC_GAMEPAD))
					{
						// joystick/gamepad
						RawInputJoystick::AddDevice(hDevice, &info.hid);
					}
					break;
				}
			}
		}
		break;

	case GIDC_REMOVAL:
		// remove the device from the joystick and OPD lists
		RawInputJoystick::RemoveDevice(hDevice);
		break;
	}
}


// --------------------------------------------------------------------------
//
// DirectInput
//

// DirectInput interface object
RefPtr<IDirectInput8> piDirectInput8;

// input buffer size
static const int DI_INPUT_BUFFER_SIZE = 32;

// DI joysticks
struct DIJoystick
{
	DIJoystick(IDirectInputDevice8 *dev, int nButtons, const WCHAR *prodName, const WCHAR *path, const GUID &guidInstance) :
		dev(dev), nButtons(nButtons), prodName(prodName), path(path), guidInstance(guidInstance)
	{
	}

	// IDI8 device interface
	RefPtr<IDirectInputDevice8> dev;

	// number of buttons associated with the device
	int nButtons;

	// product name
	std::wstring prodName;

	// system device path
	std::wstring path;

	// instance GUID
	GUID guidInstance;
};
std::list<DIJoystick> diJoysticks;

// DI keyboard interface
RefPtr<IDirectInputDevice8> diKeyboard;

// XINPUT/DIRECTINPUT DEVICE FILTERING
//
// The XInput device driver on Windows automatically creates a fake HID
// Gamepad interface for each XInput device, so that the XInput device
// appears in HID Gamepad enumerations, both in the low-level HID APIs and
// in DirectInput.  They did this for the sake of legacy DirectInput programs
// that aren't XInput aware, so that XBox controller units would work in old
// DI programs.
// 
// As often happens with Microsoft's well-intentioned attempts at universal
// backward compatibility, this creates a headache for modern programs that
// are XInput-aware, in that every XInput device will appear TWICE in that
// program's device enumeration, once in the DirectInput device list and
// then again in the XInput device list.
// 
// Microsoft recognized this problem after the fact, so they provided the
// lovely code example shown below as a workaround.  The code checks a
// DirectInput gamepad object to see if it's actually associated with an
// XInput device.  It does this by parsing the VID/PID out of the DirectInput
// GUID, then scanning the system device database for a device with the
// same VID/PID entry, and checking the DeviceID key for that device to see
// if it's of the form used by the "fake HID gamepad" device entry that the
// XInput driver creates.  The fake gamepad always (according to the code)
// has a particular substring, "IG_", in the DeviceID string.  (I personally
// would be more selective about this by matching the regex "\\bIG_\\d",
// since that appears to be the real pattern they're looking for.)
// 
// But the Microsoft code contains an invalid assumption that makes it a
// bad way to do this: the Microsoft code assumes that a USB device can
// only expose ONE USB interface.  This isn't the case; a core feature of
// USB is that a device can expose any number of interfaces.  So if a
// device exposes an actual HID Gamepad interface alongside an XInput
// interface, the Microsoft code will incorrectly filter the HID Gamepad
// out of the DirectInput device enumeration.
// 
// So I'm providing my own alternative implementation.  My version has a
// fundamental flaw of its own, but it also has the advantage that it works.
// My approach is to inspect the Device Path for the HID that DirectInput
// locates, and search for the same "fake interface" marker substring that
// the Microsoft code searches for.  The difference between this approach
// and the Microsoft approach is that my approach is appropriately selective:
// by looking only at the Device Path, instead of the DeviceID, there's no
// confusion about which interface of a multi-interface device is involved.
// The device path is specific to the single interface we're inspecting.  So
// this code will correctly distinguish between the fake HID gamepad that
// XInput creates for a given physical device, and a true HID gamepad
// interface that the same device exposes directly.  The fundamental flaw
// in my code, though, is that you're not actually allowed to make the
// assumption I'm making about the structure of the Device Path.  The Device
// ID has a publicly documented syntax structure; the Device Path does not,
// and applications are intended to treat it as opaque.  In practice,
// though, the Device Path DOES use a predictable syntax structure that's
// been the same from Windows XP through 11; it's basically the device ID
// with some added information identifying the sub-interface of a multi-
// interface device.  So the parsing assumptions that we're explicitly
// allowed to make about a Device also HAPPEN to work with a Device Path,
// for all extant Windows versions, even though we're not SUPPOSED to
// assume that this is true.  Because of this fundamental flaw, I don't
// recommend using this technique in production code for a game program
// that uses DirectInput - but then again, Microsoft doesn't recommend
// using DirectInput itself in production code, so if you're going to
// violate Microsoft's advice on what you can and can't use in your
// production code, you're probably not going to give much weight to my
// warnings either.  So to summarize: Microsoft's code is correct but
// doesn't work; my code is fundamentally wrong but works; and this
// entire program is a Developer-Use-Only Testing Tool with very
// limited use cases that specifically involve devices that might
// expose multiple interfaces, breaking the Microsoft code.  Therefore
// I'm choosing my flawed-but-working code to implement this.
bool IsDirectInputDeviceAFakeXInputHID(const WCHAR *devicePathFromDirectInput)
{
	std::wregex pat(L"\\bIG_\\d", std::regex_constants::icase);
	return std::regex_search(devicePathFromDirectInput, pat);
}

// MICROSOFT EXAMPLE OF XINPUT FAKE-HID-GAMEPAD DEVICE DETECTION - see
// comments above about why we're NOT using this code.  This is retained
// for reference only.
BOOL MicrosoftExampleImpl_IsXInputDevice(const GUID *pGuidProductFromDirectInput)
{
	// Create WMI
	RefPtr<IWbemLocator> pIWbemLocator;
	HRESULT hr = CoCreateInstance(__uuidof(WbemLocator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWbemLocator), (LPVOID*)&pIWbemLocator);
	if (FAILED(hr) || pIWbemLocator == nullptr)
		return false;

	class BStr
	{
	public:
		BStr() : bstr(nullptr) { }
		BStr(const wchar_t *s) : bstr(SysAllocString(s)) { }
		~BStr() { if (bstr != nullptr) SysFreeString(bstr); }

		BSTR bstr;

		operator BSTR() { return bstr; }
		BSTR* operator&() { return &bstr; }
	};
	class VariantHolder
	{
	public:
		VariantHolder() { VariantInit(&var); }
		~VariantHolder() { VariantClear(&var); }

		VARIANT var{ };

		VARIANT* operator&() { return &var; }
	};

	BStr bstrNamespace(L"\\\\.\\root\\cimv2");
	BStr bstrClassName(L"Win32_PNPEntity");
	BStr bstrDeviceID(L"DeviceID");
	
	if (bstrNamespace == nullptr || bstrClassName == nullptr || bstrDeviceID == nullptr)
		return false;

	// Connect to WMI 
	RefPtr<IWbemServices> pIWbemServices;
	hr = pIWbemLocator->ConnectServer(bstrNamespace, nullptr, nullptr, 0L, 0L, nullptr, nullptr, &pIWbemServices);
	if (FAILED(hr) || pIWbemServices == nullptr)
		return false;

	// Switch security level to IMPERSONATE. 
	hr = CoSetProxyBlanket(pIWbemServices,
		RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
		RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
		nullptr, EOAC_NONE);
	if (FAILED(hr))
		return false;

	RefPtr<IEnumWbemClassObject> pEnumDevices;
	hr = pIWbemServices->CreateInstanceEnum(bstrClassName, 0, nullptr, &pEnumDevices);
	if (FAILED(hr) || pEnumDevices == nullptr)
		return false;

	// Loop over all devices
	for (;;)
	{
		class IWebmClassObjectArray
		{
		public:
			~IWebmClassObjectArray()
			{
				for (auto &pDevice : arr)
					if (pDevice != nullptr) pDevice->Release();
			}
			IWbemClassObject *arr[20] = {};
		};
		IWebmClassObjectArray devices;

		ULONG uReturned = 0;
		hr = pEnumDevices->Next(10000, _countof(devices.arr), devices.arr, &uReturned);
		if (FAILED(hr))
			return false;
		if (uReturned == 0)
			break;

		for (size_t iDevice = 0; iDevice < uReturned; ++iDevice)
		{
			SAFEARRAY *sa = NULL;
			devices.arr[iDevice]->GetNames(NULL, WBEM_FLAG_ALWAYS, NULL, &sa);

			// For each device, get its device ID
			VariantHolder var;
			hr = devices.arr[iDevice]->Get(bstrDeviceID, 0L, &var, nullptr, nullptr);
			if (SUCCEEDED(hr) && var.var.vt == VT_BSTR && var.var.bstrVal != nullptr)
			{
				// Check if the device ID contains "IG_".  If it does, then it's an XInput device
				// This information cannot be found from DirectInput 
				if (wcsstr(var.var.bstrVal, L"IG_"))
				{
					// If it does, then get the VID/PID from var.bstrVal
					DWORD dwPid = 0, dwVid = 0;
					WCHAR* strVid = wcsstr(var.var.bstrVal, L"VID_");
					if (strVid && swscanf_s(strVid, L"VID_%4X", &dwVid) != 1)
						dwVid = 0;
					WCHAR* strPid = wcsstr(var.var.bstrVal, L"PID_");
					if (strPid && swscanf_s(strPid, L"PID_%4X", &dwPid) != 1)
						dwPid = 0;

					// Compare the VID/PID to the DInput device
					DWORD dwVidPid = MAKELONG(dwVid, dwPid);
					if (dwVidPid == pGuidProductFromDirectInput->Data1)
						return true;
				}
			}
		}
	}

	// not an XInput device
	return false;
}


// initialize DirectInput
void InitDirectInput(HWND hwnd)
{
	if (diKbUsed || diJsUsed)
	{
		// create our DirectInput instance
		HRESULT hr;
		if (!SUCCEEDED(hr = DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8,
			reinterpret_cast<void**>(&piDirectInput8), NULL)))
		{
			printf("DirectInput initialization failed (HRESULT %08lX); DI key/joystick buttons won't be used for this session\n", hr);
			return;
		}

		// DI enumeration callback
		struct CallbackContext
		{
			IDirectInput8 *idi8;
			HWND hwnd;
		};
		CallbackContext ctx{ piDirectInput8, hwnd };

		// set up DI keyboard input
		if (diKbUsed)
		{
			// create a DI keyboard reader
			if (SUCCEEDED(hr = piDirectInput8->CreateDevice(GUID_SysKeyboard, &diKeyboard, NULL)))
			{
				// set the data format to "keyboard"
				diKeyboard->SetDataFormat(&c_dfDIKeyboard);

				// set non-exclusive foreground/background operation
				diKeyboard->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);

				// set the input buffer size
				DIPROPDWORD bufSizeProp{ sizeof(bufSizeProp), sizeof(DIPROPHEADER), 0, DIPH_DEVICE, DI_INPUT_BUFFER_SIZE };
				diKeyboard->SetProperty(DIPROP_BUFFERSIZE, &bufSizeProp.diph);
			}
			else
			{
				printf("DirectInput keyboard initialization failed (HRESULT %08lX); DI keyboard input won't be used for this session\n", hr);
			}
		}

		// set up DI joystick input
		if (diJsUsed)
		{
			static auto cbEnumGameCtl = [](LPCDIDEVICEINSTANCE ddi, LPVOID pvRef) -> BOOL
			{
				// recover our context struct
				auto *ctx = reinterpret_cast<CallbackContext*>(pvRef);

				// open the device
				RefPtr<IDirectInputDevice8> js;
				if (SUCCEEDED(piDirectInput8->CreateDevice(ddi->guidInstance, &js, NULL)))
				{
					// enumerate the buttons
					struct ButtonCallbackContext
					{
						int nButtons = 0;
					} btnCtx;
					auto cbEnumObj = [](LPCDIDEVICEOBJECTINSTANCE ddo, LPVOID pvRef) -> BOOL
					{
						// recover our context struct
						auto *ctx = reinterpret_cast<ButtonCallbackContext*>(pvRef);

						// count the button
						ctx->nButtons += 1;

						// continue the enumeration
						return DIENUM_CONTINUE;
					};
					js->EnumObjects(cbEnumObj, &btnCtx, DIDFT_BUTTON);

					// get the device instance information
					DIDEVICEINSTANCE didi{ sizeof(didi) };
					js->GetDeviceInfo(&didi);

					// if it's a joystick or gamepad, and it has any buttons, keep it in our list
					if (btnCtx.nButtons != 0
						&& (didi.wUsagePage == HID_USAGE_PAGE_GENERIC
							&& didi.wUsage == HID_USAGE_GENERIC_GAMEPAD || didi.wUsage == HID_USAGE_GENERIC_JOYSTICK))
					{
						// get the product name
						DIPROPSTRING prodName{ sizeof(prodName), sizeof(DIPROPHEADER), 0, DIPH_DEVICE };
						if (!SUCCEEDED(js->GetProperty(DIPROP_PRODUCTNAME, &prodName.diph)))
							wcscpy_s(prodName.wsz, L"(Unknown Device)");

						// retrieve its device path
						DIPROPGUIDANDPATH devIdent{ sizeof(devIdent), sizeof(DIPROPHEADER), 0, DIPH_DEVICE };
						if (!SUCCEEDED(js->GetProperty(DIPROP_GUIDANDPATH, &devIdent.diph)))
							devIdent.wszPath[0] = 0;

						// get its device instance data
						DIDEVICEINSTANCE didi{ sizeof(didi) };
						js->GetDeviceInfo(&didi);

						// If it's an XInput device, and we have XInput buttons mapped
						// explicitly through the XInput API, filter out this device.  The 
						// XInput driver adds a fake HID gamepad for each XBox controller,
						// for the sake of legacy DirectInput applications, so that XBox
						// controllers will work with old code that don't use the newer
						// XInput API.  This creates a headache for applications that ARE
						// XInput aware, like this one, because we'll see every XInput
						// device twice, once in the XInput enumeration and again in the
						// DirectInput enumeration.  The fake gamepad input could overlap
						// with true gamepad/joystick inputs we're also monitoring, which
						// could confuse the timing data.
						if (xinputUsed && IsDirectInputDeviceAFakeXInputHID(devIdent.wszPath))
							return DIENUM_CONTINUE;

						// set joystick input format
						js->SetDataFormat(&c_dfDIJoystick);

						// set input with or without focus
						js->SetCooperativeLevel(ctx->hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);

						// set the input buffer size
						DIPROPDWORD bufSizeProp{ sizeof(bufSizeProp), sizeof(DIPROPHEADER), 0, DIPH_DEVICE, DI_INPUT_BUFFER_SIZE };
						js->SetProperty(DIPROP_BUFFERSIZE, &bufSizeProp.diph);

						// add it to our list
						diJoysticks.emplace_back(js.Detach(), btnCtx.nButtons, prodName.wsz, devIdent.wszPath, didi.guidInstance);
					}
				}

				// continue the enumeration
				return DIENUM_CONTINUE;
			};
				
			// enumerate DI8 Game Controller devices
			piDirectInput8->EnumDevices(DI8DEVCLASS_GAMECTRL, cbEnumGameCtl, &ctx, DIEDFL_ATTACHEDONLY);
		}

		// If we enabled DI keyboard or joystick, set up a timer.  DI
		// uses a polling model, so we have to periodically ask it if
		// any input is available.  To minimize latency, poll at the
		// minimum 1ms intervals.
		if (diKeyboard != nullptr || diJoysticks.size() != 0)
			SetTimer(hwnd, TIMER_ID_DI_XI, 1, NULL);
	}
}

void TerminateDirectInput()
{
	// release the keyboard object
	if (diKeyboard != nullptr)
	{
		diKeyboard->Unacquire();
		diKeyboard = nullptr;
	}

	// release joystick acquisition
	for (auto &js : diJoysticks)
	{
		js.dev->Unacquire();
		js.dev = nullptr;
	}
}

void PollDirectInput(HWND hwnd)
{
	// poll for keyboard input
	if (diKbUsed)
	{
		// repeat in case we have to acquire and retry
		for (int tries = 0 ; ; )
		{
			DWORD nEle = 1;
			DIDEVICEOBJECTDATA dod;
			HRESULT hr = diKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &dod, &nEle, 0);
			if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
			{
				// if we haven't already tried this, acquire the keyboard and go back
				// for another attempt; if we've already done this, stop here, as it
				// will probably keep failing the same way
				if (tries++ == 0)
				{
					diKeyboard->Acquire();
					continue;
				}
				else
				{
					printf("Error acquiring DirectInput keyboard\n");
					return;
				}
			}

			if ((hr == S_OK || hr == DI_BUFFEROVERFLOW) && nEle != 0)
			{
				// note the time we read the key
				uint64_t tEvent = VendorInterface::GetMicrosecondTime();

				// get the DIK_xxx code and pressed/released state
				int dik = dod.dwOfs;
				bool pressed = (dod.dwData & 0x80) != 0;

				// look for a key in the button list
				auto it = std::find_if(buttons.begin(), buttons.end(), [diKeyCode = dod.dwOfs](const ButtonDesc &b) {
					return b.type == ButtonDesc::Type::DI_KEY && b.id == diKeyCode; });
				if (it != buttons.end())
					it->OnInput(pressed, tEvent);

				// go back for another key
				continue;
			}

			// stop if we ran out of work
			break;
		}
	}

	// poll for joystick input
	if (diJsUsed)
	{
		// scan each joystick
		for (auto &js : diJoysticks)
		{
			// acquire the joystick
			if (auto hr = js.dev->Acquire(); hr == S_OK || hr == S_FALSE)
			{
				DIDEVICEOBJECTDATA od[DI_INPUT_BUFFER_SIZE];
				DWORD n = DI_INPUT_BUFFER_SIZE;
				hr = js.dev->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), od, &n, 0);
				auto tEvent = VendorInterface::GetMicrosecondTime();
				if (hr == S_OK || hr == DI_BUFFEROVERFLOW)
				{
					auto *odp = &od[0];
					for (DWORD i = 0 ; i < n ; ++i, ++odp)
					{
						// check for button presses
						if (odp->dwOfs >= DIJOFS_BUTTON0 && odp->dwOfs <= DIJOFS_BUTTON31)
						{
							// get our nominal button number - DI numbers the buttons 0..31, and
							// we use the HID convention 1..32
							int btnNum = odp->dwOfs - DIJOFS_BUTTON0 + 1;
							if (btnNum < _countof(diJsButtons) && diJsButtons[btnNum] != nullptr)
							{
								bool pressed = (odp->dwData & 0x80);
								diJsButtons[btnNum]->OnInput(pressed, tEvent);
							}
						}
					}
				}
			}
		}
	}
}

// --------------------------------------------------------------------------
//
// XInput (XBox controller input API)
//

// XInput button names
struct XInputButtonDesc
{
	XInputButtonDesc(const char *desc, int idx, uint32_t bit) : desc(desc), idx(idx), bit(bit) { }

	// description for button listings and event messages
	const char *desc;

	// button index in our internal array (equal to the bit number)
	int idx;

	// button bit in XINPUT_GAMEPAD::wButtons
	uint32_t bit;
};
static const std::unordered_map<std::string, XInputButtonDesc> xiButtonMap{
	{ "x-dpad-up", { "DPad Up", 0, 0x0001 } },
	{ "x-dpad-down", { "DPad Down", 1, 0x0002 } },
	{ "x-dpad-left", { "DPad Left", 2, 0x0004 } },
	{ "x-dpad-right", { "DPad Right", 3, 0x0008 } },
	{ "x-start", { "Start", 4, 0x0010 } },
	{ "x-back", { "Back", 5, 0x0020 } },
	{ "x-left-thumb", { "Left Thumb", 6, 0x0040 } },
	{ "x-right-thumb", { "Right Thumb", 7, 0x0080 } },
	{ "x-left-bumper", { "Right Bumper", 8, 0x0100 } },
	{ "x-right-bumper", { "Right Bumper", 9, 0x0200 } },
	{ "x-a", { "A", 12, 0x1000 } },
	{ "x-b", { "B", 13, 0x2000 } },
	{ "x-x", { "X", 14, 0x4000 } },
	{ "x-y", { "Y", 15, 0x8000 } },
};

// XInput buttons being monitored
static ButtonDesc *xiButtons[16]{ nullptr };

// Controller states.  XInput exposes a fixed maximum number of controllers,
// so we just have to keep one entry per controller.
struct XInputDeviceState
{
	// Last observed packet number.  XInput controllers mark each
	// packet they send with a new serial number, so that an
	// application can detect if any new input has arrived since
	// the last polling check.
	DWORD packetNum = 0;

	// Button states from the controller.  These are packed into a
	// bit array in a fixed order defined in the XInput API.  See
	// the xiButtonMap list above for the mappings from bits to
	// button names.
	WORD buttonBits = 0;
};
static const int MAX_NUMBER_OF_XINPUT_CONTROLLERS = 4;
static XInputDeviceState xiButtonState[MAX_NUMBER_OF_XINPUT_CONTROLLERS];

void InitXInput(HWND hwnd)
{
	if (xinputUsed)
	{
		// set up the polling timer (note that our DI subsystem will also start 
		// this timer if DI is in use, but it's harmless to start it twice; it
		// just has the effect of rescheduling the first call for 1ms from now,
		// which is fine because we're still in initialization anyway)
		SetTimer(hwnd, TIMER_ID_DI_XI, 1, NULL);
	}
}

void TerminateXInput()
{
}

void PollXInput(HWND hwnd)
{
	if (xinputUsed)
	{
		// If we haven't retrieved any initial states yet, do so now.  On
		// Windows 10+, the XInput API takes care of app activation internally,
		// and hides all XInput functions from us when the app is in the
		// background.  Since we don't get a say in when the activation
		// occurs, we'll just monitor for active controllers until we see
		// one.
		static bool initializedStates = false;
		if (!initializedStates)
		{
			// scan controllers
			XInputDeviceState *pst = &xiButtonState[0];
			for (int i = 0 ; i < MAX_NUMBER_OF_XINPUT_CONTROLLERS ; ++i, ++pst)
			{
				// get capabilities
				XINPUT_CAPABILITIES caps;
				DWORD result = XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &caps);
				if (result == ERROR_SUCCESS)
				{
					// get the initial button states
					XINPUT_STATE st;
					if (XInputGetState(i, &st) == ERROR_SUCCESS)
					{
						// record the initial state
						pst->packetNum = st.dwPacketNumber;
						pst->buttonBits = st.Gamepad.wButtons;

						// flag that we've initialized at least one controller state
						initializedStates = true;
					}
				}
			}
		}

		// check each controller for new input
		XInputDeviceState *pst = &xiButtonState[0];
		for (int i = 0 ; i < MAX_NUMBER_OF_XINPUT_CONTROLLERS ; ++i, ++pst)
		{
			// get the new controller state
			XINPUT_STATE st;
			if (XInputGetState(i, &st) == ERROR_SUCCESS)
			{
				// note the event time
				auto tEvent = VendorInterface::GetMicrosecondTime();

				// check for button changes since the last update
				if (pst->buttonBits != st.Gamepad.wButtons)
				{
					// check for changes in monitored buttons
					for (unsigned int btnNum = 0, bit = 1 ; btnNum < _countof(xiButtons) ; ++btnNum, bit <<= 1)
					{
						if (xiButtons[btnNum] != nullptr)
						{
							// update it if the state has changed on this controller
							bool oldPressed = (pst->buttonBits & bit) != 0;
							bool newPressed = (st.Gamepad.wButtons & bit) != 0;
							if (oldPressed != newPressed)
							{
								// state changed on controller - handle the change on the monitored input
								xiButtons[btnNum]->OnInput(newPressed, tEvent);
							}
						}
					}
				}

				// record the new state
				pst->packetNum = st.dwPacketNumber;
				pst->buttonBits = st.Gamepad.wButtons;
			}
		}
	}
}


// --------------------------------------------------------------------------
//
// Monitor thread
//

// message window proc
LRESULT CALLBACK MonitorWinProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	// Mark the time we received the message at the application level.
	// Note that we deliberately use the current time, NOT the event
	// time (GetEventTime()), because we want to measure the time to
	// the actual reception of the event in application code.  The
	// time that the operating system queued the event isn't as relevant,
	// since any similar event-loop-based application will have to go
	// through this same procedure to read events out of the queue, and
	// thus couldn't react to an event any faster than this.  The whole
	// point of this exercise is to measure the true time it takes for
	// the button input to reach the application-level input processor,
	// where the application will apply the event to its live state
	// (e.g., where Visual Pinball feeds the event into the physics
	// simulation).
	uint64_t tEvent = VendorInterface::GetMicrosecondTime();

	// process the message
	switch (msg)
	{
	case WM_CREATE:
		// initialize Raw Input if needed
		InitRawInput(hwnd);

		// initialize DirectInput if needed
		InitDirectInput(hwnd);

		// initialize Open Pinball Device input if needed
		InitOpenPinballDevice(hwnd);

		// initialize XInput
		InitXInput(hwnd);
		break;

	case WM_INPUT:
		// process the raw input message
		ProcessRawInput(reinterpret_cast<HRAWINPUT>(lparam), tEvent);
		break;

	case WM_INPUT_DEVICE_CHANGE:
		// process the device change
		ProcessRawInputDeviceChange(static_cast<USHORT>(wparam), reinterpret_cast<HANDLE>(lparam));
		break;

	case WM_ACTIVATE:
		// Enable XInput when in the foreground.  This is only required for 
		// Windows < 10.  Windows 10 and later, the XInput API handles activation
		// internally, and XInputEnable() has no effect. (It's also deprecated on
		// Win10+, but for now it's harmless to call it anyway, so we can just call
		// it unconditionally for the sake of older systems.)
		if (wparam == WA_ACTIVE || wparam == WA_CLICKACTIVE)
		{
			// allow deprecated XInputEnable call, for compatibility with older systems
			#pragma warning(push)
			#pragma warning(disable: 4995) 
	
			// enable XInput (only required for Windows < 10)
			XInputEnable(TRUE);

			#pragma warning(pop)
		}
		break;


	case WM_TIMER:
		switch (wparam)
		{
		case TIMER_ID_DI_XI:
			// poll for DirectInput and XInput events
			PollDirectInput(hwnd);
			PollXInput(hwnd);
			return 0;
		}
		break;

	case WM_PAINT:
		// for XInput, we need a real window, so do nominal painting
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);

			RECT crc;
			GetClientRect(hwnd, &crc);
			HBRUSH hbr = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
			FillRect(hdc, &crc, hbr);

			int oldBkMode = SetBkMode(hdc, TRANSPARENT);
			COLORREF oldColor = SetTextColor(hdc, RGB(0, 0, 0));

			const char *msg = "This window must be in the foreground to receive XInput events";
			SIZE sz;
			GetTextExtentPointA(hdc, msg, static_cast<int>(strlen(msg)), &sz);
			RECT txrc{ (crc.right - sz.cx)/2, (crc.bottom - sz.cy)/2, crc.right, crc.bottom };
			DrawTextA(hdc, msg, -1, &txrc, DT_LEFT | DT_TOP | DT_SINGLELINE);

			SetTextColor(hdc, oldColor);
			SetBkMode(hdc, oldBkMode);
			EndPaint(hwnd, &ps);
		}
		return 0;

	case WM_DESTROY:
		// terminate input subsystems
		TerminateRawInput();
		TerminateDirectInput();
		TerminateXInput();
		TerminateOpenPinballDevice();
		break;

	default:
		break;
	}

	// pass the message to the system default window proc
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

// exit thread requested
bool exitThreadRequested = false;

// thread entrypoint
DWORD WINAPI MonitorThread(void *lparam)
{
	// initialize COM on the thread
	CoInitialize(NULL);

	// set up the message window
	WNDCLASSEX wc{ sizeof(wc) };
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = _T("ButtonLatencyTester2.MsgWin");
	wc.lpfnWndProc = MonitorWinProc;
	RegisterClassEx(&wc);

	// create the window
	HWND hwnd;
	if (xinputUsed)
	{
		// the XInput API only works when we have focus (on Windows 10 and
		// later, anyway; earlier platforms can manage their own activation,
		// but they made it "easier" in Win10 by hiding activation inside
		// the API, and that depends upon a UI window that can take focus)
		hwnd = CreateWindow(wc.lpszClassName, _T("Button Latency Tester II"),
			WS_VISIBLE | WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			NULL, NULL, wc.hInstance, 0);
	}
	else
	{
		// other APIs only need a window for message-loop purposes, so create
		// an invisible Message-Only window
		hwnd = CreateWindow(wc.lpszClassName, _T("ButtonLatencyTester2"), 0,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			HWND_MESSAGE, NULL, wc.hInstance, 0);
	}

	// run the message loop
	MSG msg;
	for (BOOL ret = FALSE; (ret = GetMessage(&msg, NULL, 0, 0)) != 0 && ret != -1 ; )
	{
		// process the message
		TranslateMessage(&msg);
		DispatchMessage(&msg);

		// stop if thread exit requested
		if (exitThreadRequested)
		{
			DestroyWindow(hwnd);
			PostQuitMessage(0);
		}
	}

	// clean up COM on the thread
	CoUninitialize();

	// return value not used
	return 0;
}

// --------------------------------------------------------------------------
//
// Show statistics
//
void ShowLatencyMeasurements()
{
	// acquire the mutex
	if (WaitForSingleObject(mutex, 1000) != WAIT_OBJECT_0)
	{
		printf("Unable to acquire device mutex\n");
		return;
	}

	// get current statistics from all of the BLT-II Picos
	for (auto &dev : devices)
	{
		if (int stat = dev.ifc->QueryMeasurements(dev.data); stat != VendorResponse::OK)
		{
			printf("Error retrieving latency measurements from %s: %s (error code %d)\n",
				dev.hwid.c_str(), VendorInterface::ErrorText(stat), stat);
		}
	}

	// done with the interfaces - release the mutex
	ReleaseMutex(mutex);

	// list stats for the configured buttons
	printf(
		"Button Name              %sGP #Presses #HostEvt AvgLat(ms)   Median     StdDev     Min Lat    Max Lat\n"
		"======================== %s== ======== ======== ========== ========== ========== ========== ==========\n",
		devices.size() > 1 ? "BLT-II           " : "", devices.size() > 1 ? "================ " : "");
	
	for (const auto &b : buttons)
	{
		// show button ID, unit name, and GP
		printf("%-24.24s %s%s%2d ", 
			b.friendlyName.c_str(), 
			devices.size() > 1 ? b.device->hwid.c_str() : "",
			devices.size() > 1 ? " " : "",
			b.gpio);

		// find the button's measurement data record by GPIO
		if (auto *data = b.GetMeasurementData(); data != nullptr)
		{
			printf("%8llu %8llu %10.3lf %10.3lf %10.3lf %10.3lf %10.3lf\n",
				data->nPresses, data->nHostEvents, data->GetAvg()/1000.0f, 
				data->latencyMedian/1000.0f, data->GetStdDev()/1000.0f,
				data->latencyMin/1000.0f, data->latencyMax/1000.0f);
		}
		else
		{
			printf("No data from device\n");
		}
	}

	// show USB timing statistics
	printf("\nBLT-II request round-trip times: avg %.2lfus, std dev %.2lfus, min %lluus, max %lluus\n\n",
		usbStats.GetAvg(), usbStats.GetStdDev(), usbStats.timeMin, usbStats.timeMax);
}

// --------------------------------------------------------------------------
//
// Show usage and exit
//
void UsageExit()
{
	printf("Usage: ButtonLatencyTester [options] <button> ...\n"
		"\nOptions:\n"
		"   -d, --devices          list all detected Button Latency Tester II Picos\n"
		"   --bootloader <hwid>    reset unit(s) matching hardware ID to Boot Loader mode\n"
		"                          <hwid> can be any substring of the ID, or '*' for all\n"
		"   -b, --buttons          show a list of valid button names\n"
		"   --api <api>            select the API for keyboard and joystick input; <api>\n"
		"                          can be 'RawInput' or 'DirectInput'\n"
		"   --direct-input         use the DirectInput 8 API for joystick and keyboard inputs\n"
		"   /?, --help             show usage\n"
		"\n"
		"If no input API is specified, Raw Input is used by default.  Only one may be\n"
		"used during a session, because DirectInput takes exclusive control when enabled.\n"
		"This option applies ONLY to keyboard and gamepad/joystick button input; XInput\n"
		"and Open Pinball Device buttons are always read through their own APIs.\n"
		"\n"
		"If any XInput buttons are mapped by name (X-DPAD-UP, etc), all XInput devices\n"
		"are hidden from DirectInput for the session.  Normally, DirectInput reads from\n"
		"XInput devices as though they were USB gamepads/joysticks, mapping the XInput\n"
		"buttons to joystick buttons #1-10.  That's a built-in feature of DirectInput,\n"
		"but this program overrides it when XInput is being used explicitly, to avoid\n"
		"confusion with normal joystick/gamepad inputs.\n"
		"\n"
		"The <button> list specifies the inputs to monitor.  Each entry is of the\n"
		"form \"<button-name>:<gpio>[:<hwid>], where:\n"
		"\n"
		"   <button-name>          name of a keyboard key or joystick button; use the\n"
		"                          --buttons option to show a list of the names accepted\n"
		"   <gpio>                 GPIO number on the BLT-II Pico where the button is wired\n"
		"   <hwid>                 the hardware ID of the BLE-II Pico where the button is\n"
		"                          connected, or any unique substring; required only if more\n"
		"                          than one BLT-II Pico is present\n"
		"\n"
		"All input types (keyboard, joystick, etc) are combined across like devices, so\n"
		"'gamepad7' will respond to button 7 on any joystick or gamepad, 'A' will respond\n"
		"to the \"A\" key on any keyboard, etc.\n");
	exit(1);
}

// --------------------------------------------------------------------------
//
// Main program
//

// forwards
static int MainProgram(int argc, const char *const *argv);

// entrypoint
int main(int argc, char **argv)
{
	// initialize COM
	CoInitialize(NULL);

	// run the main program
	int ret = MainProgram(argc, argv);

	// uninitialize COM
	CoUninitialize();

	// return the result from the main handler
	return ret;
}

static int MainProgram(int argc, const char *const *argv)
{
	// banner
	printf("Pinscape Pico Button Latency Tester II  Version 0.1.0\n\n");

	// enumerate devices
	std::list<VendorInterfaceDesc> descs;
	HRESULT hr = VendorInterface::EnumerateDevices(descs);
	if (!SUCCEEDED(hr))
		return printf("Error enumerating latency tester devices (HRESULT %08lx)\n", hr), 1;
	if (descs.size() == 0)
		return printf("No Button Latency Tester II devices found\n"), 1;

	// open each device's vendor interface
	for (const auto &d : descs)
	{
		// try opening the interface
		VendorInterface *ifc;
		if (!SUCCEEDED(hr = d.Open(ifc)))
			return printf("Error connecting to device %ws (HRESULT %08lx)\n", d.Name(), hr), 1;

		// add it to our device list
		devices.emplace_back(ifc);
	}

	// API selected
	enum class API {
		None,
		RawInput,
		DirectInput,
	};
	API api = API::None;

	// parse arguments
	bool buttonsRequired = true;
	for (int i = 1 ; i < argc ; ++i)
	{
		const char *a = argv[i];
		if (strcmp(a, "--devices") == 0 || strcmp(a, "-d") == 0)
		{
			// list units
			printf("Button Latency Tester II devices detected:\n");
			for (const auto &dev : devices)
			{
				TSTRING comPort;
				if (!dev.ifc->GetCDCPort(comPort))
					comPort = _T("No COM port");
				printf("  %s   (%" TSTRFMT ")\n", dev.hwid.c_str(), comPort.c_str());
			}
			printf("\n%zd device%s found\n", devices.size(), devices.size() == 1 ? "" : "s");

			// allow just --list with no other options
			buttonsRequired = false;
		}
		else if (strcmp(a, "--buttons") == 0 || strcmp(a, "-b") == 0)
		{
			// list the keyboard keys, in alphabetical order
			printf("Keyboard key names:\n");
			std::map<std::string, const KeyDef*> sortedKeys;
			for (const auto &k : keyMap)
				sortedKeys.emplace(k.first, &k.second);
			for (const auto &k : sortedKeys)
			{
				std::string name = k.first;
				std::transform(name.begin(), name.end(), name.begin(), ::toupper);
				printf("    %-14s  %s\n", name.c_str(), k.second->desc);
			}

			// joystick buttons
			printf("\nJoystick/gamepad buttons:\n"
				"    JS1             Joystick button 1 (on any joystick/gamepad)\n"
				"    JS2             Joystick button 2\n"
				"    ...\n"
				"    JS32            Joystick button 32\n");

			// Open Pinball Device buttons
			printf("\nOpen Pinball Device \"generic\" numbered buttons:\n"
				"    OPD1            Open pinball device generic button #1\n"
				"    OPD2            Generic button #2\n"
				"    ...\n"
				"    OPD32           Generic button #32\n");

			printf("\nOpen Pinball Device named-function buttons:\n");
			std::map<std::string, const OPDButtonDesc*> sortedOpdButtons;
			for (const auto &b : opdButtonNames)
				sortedOpdButtons.emplace(b.first, &b.second);
			for (const auto &pair : sortedOpdButtons)
			{
				auto *b = pair.second;
				std::string name = pair.first;
				std::transform(name.begin(), name.end(), name.begin(), ::toupper);
				printf("    %-14s  %s\n", name.c_str(), b->desc);
			}

			printf("\nXInput buttons:\n");
			std::map<std::string, const XInputButtonDesc*> sortedXIButtons;
			for (const auto &b : xiButtonMap)
				sortedXIButtons.emplace(b.first, &b.second);
			for (const auto &pair : sortedXIButtons)
			{
				auto *b = pair.second;
				std::string name = pair.first;
				std::transform(name.begin(), name.end(), name.begin(), ::toupper);
				printf("    %-14s  %s\n", name.c_str(), b->desc);
			}

			buttonsRequired = false;
		}
		else if (strcmp(a, "/?") == 0 || strcmp(a, "-?") == 0 || strcmp(a, "--help") == 0)
		{
			UsageExit();
		}
		else if (strcmp(a, "--bootloader") == 0)
		{
			// get the ID, converted to capitals
			if (++i >= argc)
			{
				printf("Missing hardware ID argument for \"--bootloader\" option\n");
				return 1;
			}
			std::string hwid = argv[i];
			std::transform(hwid.begin(), hwid.end(), hwid.begin(), ::toupper);

			// scan the device list for matches
			int nFound = false;
			for (auto &d : devices)
			{
				// match '*' (wildcard) or any substring of the live device ID
				if (hwid == "*" || strstr(d.hwid.c_str(), hwid.c_str()) != nullptr)
				{
					// it's a match - reboot this device
					++nFound;
					printf("Resetting %s to Boot Loader mode: ", d.hwid.c_str());
					if (auto stat = d.ifc->EnterBootLoader(); stat == VendorResponse::OK)
						printf("OK\n");
					else
						printf("Error: %s (code %d)\n", d.ifc->ErrorText(stat), stat);
				}
			}

			// if we didn't find anything to reboot, say so
			if (nFound == 0)
			{
				printf("No matching devices found for \"--bootloader %s\"\n", argv[i]);
				return 1;
			}

			// no buttons required for this operation
			buttonsRequired = false;
		}
		else if (strcmp(a, "--api") == 0)
		{
			if (++i >= argc)
			{
				printf("Missing argument for --api\n");
				return 1;
			}
			if (api != API::None)
			{
				printf("Error: --api can only be used once\n");
				return 1;
			}

			if (_stricmp(argv[i], "rawinput") == 0)
				api = API::RawInput;
			else if (_stricmp(argv[i], "directinput") == 0)
				api = API::DirectInput;
			else
			{
				printf("Error: invalid --api argument \"%s\"\n", argv[i]);
				return 1;
			}
		}
		else if (a[0] == '-')
		{
			printf("Invalid option \"%s\"; use --help for usage\n", a);
			return 1;
		}
		else
		{
			// Anything else is a button mapping.  If the API hasn't been defined
			// yet, select Raw Input as the default.
			if (api == API::None)
				api = API::RawInput;

			// match the <button-name>:<gpio>[:<hwid>] syntax
			static const std::regex pat("([\\w-]+):(\\d+)(:[0-9A-Fa-f]+)?");
			std::match_results<const char*> m;
			if (std::regex_match(a, m, pat))
			{
				// pull out the tokens
				std::string buttonOrKeyName = m[1].str();
				int gpio = atoi(m[2].str().c_str());
				std::string deviceId = m[3].str();

				// convert the button name to all lower-case, for matching to the maps
				std::transform(buttonOrKeyName.begin(), buttonOrKeyName.end(), buttonOrKeyName.begin(), ::tolower);

				// convert the device ID to upper-case (since the ID from the live device comes 
				// back in all caps)
				std::transform(deviceId.begin(), deviceId.end(), deviceId.begin(), ::toupper);

				// find the interface
				Device *device = nullptr;
				if (deviceId.size() == 0)
				{
					// no ID specified
					if (devices.size() == 1)
					{
						// one device present -> no ambiguity, just pick the one
						device = &devices.front();
					}
					else
					{
						printf("Button mapping \"%s\" must specify a device ID, since multiple devices are present\n", a);
						return 1;
					}
				}
				else
				{
					// scan for a match to the ID
					int nMatches = 0;
					for (auto &dev : devices)
					{
						if (strstr(dev.hwid.c_str(), deviceId.c_str()) != nullptr)
						{
							device = &dev;
							++nMatches;
						}
					}

					// make sure we found a match, and that it's unique
					if (nMatches == 0)
						return printf("Button mapping \"%s\": no device ID matched\n", a), 1;
					else if (nMatches > 1)
						return printf("Button mapping \"%s\": device ID is ambiguous\n", a), 1;
				}

				// match the name
				static const std::regex jsPat("js(\\d+)");
				static const std::regex opdGenericPat("opd(\\d+)");
				std::match_results<std::string::const_iterator> m2;
				char friendlyName[64];
				if (auto it = keyMap.find(buttonOrKeyName); it != keyMap.end())
				{
					// keyboard key
					sprintf_s(friendlyName, "Keyboard %s", it->second.desc);
					switch (api)
					{
					case API::RawInput:
						rawInputKbUsed = true;
						buttons.emplace_back(friendlyName, ButtonDesc::Type::RI_KEY, it->second.scan, device, gpio);
						break;

					case API::DirectInput:
						if (it->second.diKey < 0)
						{
							printf("Button mapping \"%s\": this key has no DirectInput mapping (use --api RawInput instead)\n", a);
							return 1;
						}
						buttons.emplace_back(friendlyName, ButtonDesc::Type::DI_KEY, it->second.diKey, device, gpio);
						diKbUsed = true;
						break;
					}
				}
				else if (std::regex_match(buttonOrKeyName, m2, jsPat))
				{
					// gamepad/joystick button number - 1 to 32
					int btnNum = atoi(m2[1].str().c_str());
					if (btnNum < 1 || btnNum > 32)
					{
						printf("Invalid joystick button number %s; must be 1 to 32\n", m2[1].str().c_str());
						return 1;
					}

					sprintf_s(friendlyName, "Joystick button #%d", btnNum);
					switch (api)
					{
					case API::RawInput:
						riJsButtons[btnNum] = &buttons.emplace_back(friendlyName, ButtonDesc::Type::RI_JSBUTTON, btnNum, device, gpio);
						rawInputHidUsed = true;
						break;

					case API::DirectInput:
						diJsButtons[btnNum] = &buttons.emplace_back(friendlyName, ButtonDesc::Type::DI_JSBUTTON, btnNum, device, gpio);
						diJsUsed = true;
						break;
					}
				}
				else if (auto it = opdButtonNames.find(buttonOrKeyName); it != opdButtonNames.end())
				{
					// Open Pinball Device named button
					
					// for our internal numbering, use the range 32-63, beyond the end
					// of the generic numbered buttons 0-31
					int btnNum = it->second.buttonNum + 32;
					sprintf_s(friendlyName, "OPD %s", it->second.desc);
					opdButtons[btnNum] = &buttons.emplace_back(friendlyName, ButtonDesc::Type::OPD_BUTTON, btnNum, device, gpio);
					opdUsed = true;
				}
				else if (std::regex_match(buttonOrKeyName, m2, opdGenericPat))
				{
					// Open Pinball Device generic numbered button
					int btnNum = atoi(m2[1].str().c_str());
					if (btnNum < 1 || btnNum > 32)
					{
						printf("Invalid Open Pinball Device numbered button %s; must be 1 to 32\n", m2[1].str().c_str());
						return 1;
					}

					sprintf_s(friendlyName, "OPD generic #%d", btnNum);
					opdButtons[btnNum] = &buttons.emplace_back(friendlyName, ButtonDesc::Type::OPD_BUTTON, btnNum, device, gpio);
					opdUsed = true;
				}
				else if (auto it = xiButtonMap.find(buttonOrKeyName); it != xiButtonMap.end())
				{
					// XInput button
					const auto &b = it->second;
					sprintf_s(friendlyName, "XInput %s", b.desc);
					xiButtons[b.idx] = &buttons.emplace_back(friendlyName, ButtonDesc::Type::XI_BUTTON, b.bit, device, gpio);
					xinputUsed = true;
				}
				else
				{
					printf("Invalid button name \"%s\" (in \"%s\"); use --buttons to list accepted names\n", buttonOrKeyName.c_str(), a);
					return 1;
				}
			}
			else
			{
				printf("Invalid option or key specification \"%s\"; use --help for usage\n", a);
				return 1;
			}
		}
	}

	// if there are no buttons defined, there's nothing to do
	if (buttons.size() == 0)
	{
		// if they didn't ask for some other option, mention that
		// we need a button list to do anything
		if (buttonsRequired)
		{
			printf("No buttons configured.  One or more buttons must be configured via\n"
				"command arguments to run timing tests.  See usage (--help).\n");
		}
		return 1;
	}

	// clear any latency measurements gathered on the device in past sessions
	for (auto &dev : devices)
		dev.ifc->ClearMeasurements();

	// create the mutex for coordinating device access among the threads
	mutex = CreateMutex(NULL, FALSE, NULL);

	// launch the monitor thread
	DWORD tid;
	HANDLE hThread = CreateThread(NULL, 0, MonitorThread, nullptr, 0, &tid);
	if (hThread == NULL)
		return printf("Error creating monitor thread\n"), 1;

	// run until the user exits
	const char *helpMsg = "'X' = Exit, 'S' = Show statistics";
	printf("Monitoring input events - perform manual button press tests now\n");
	printf("%s\n\n", helpMsg);
	for (;;)
	{
		int ch = _getch();
		if (ch == 'x' || ch == 'X')
			break;
		else if (ch == 's' || ch == 'S')
			ShowLatencyMeasurements();
		else if (ch == '?')
			printf("%s\n\n", helpMsg);
	}

	// show statistics before exiting
	ShowLatencyMeasurements();

	// wait for the monitor thread to exit
	exitThreadRequested = true;
	WaitForSingleObject(hThread, 1000);
	CloseHandle(hThread);

	// normal completion
	return 0;
}
