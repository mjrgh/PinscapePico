// Open Pinball Device - Win32 Raw Input API support
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Provides support for Open Pinball Device controllers input through
// the Windows Raw Input API.  This is the preferred API for use with
// Win32 applications that use an event-based input model - it should
// fit easily into the structure of event-based programs, and Raw
// Input has extremely low latency and minimal performance impact.
// 
// Programs structured around a physics-and-video rendering loop,
// which is common in DirectX video games, might prefer to use the
// polling interface defined in OpenPinballDeviceLib.h.  Polling fits
// well into a render loop structure, since such a program can usually
// only accept input between rendering cycles anyway.  In addition,
// the polling interface is based on the portable hidapi library, so
// it's more readily portable to non-Windows platforms.  Raw Input is
// a strictly Win32 API.
// 
// 
// About Raw Input
//
// Raw Input is a Windows API for event-based input from HID devices.
// It supplements the standard Windows input event model to provide
// applications with more direct access to input from the system's
// HID device driver, which has two major benefits to applications
// compared to the standard input events (WM_KEYDOWN, WM_MOUSEMOVE,
// etc): it greatly reduces latency, and it extends coverage to all
// types of HID devices, not just keyboards and mice.
// 
// When Raw Input was introduced, Microsoft strongly recommended it
// as a replacement for all older input APIs, especially the old
// Multimedia Joystick API and DirectInput, because of its greatly
// reduced latency.  Even on Windows 11, it remains the lowest-latency
// input API (by far) that I've measured.  Raw Input can achieve
// end-to-end latencies in the 2ms range from fast devices, with
// extremely high consistency (small standard deviation).
// 
// 
// Using this interface
// 
// Raw Input is a bit more complicated to use than the basic input
// APIs.  It requires the following steps.  If your application is
// already using Raw Input to process keyboard, mouse, and/or
// joystick input, it will already be handling all of these tasks,
// so adding Open Pinball Device support will be a very simple
// matter of adding a few method calls at key places, identified
// in the instructions below.  If you're not already using Raw
// Input for other devices, it's still pretty easy to add it just
// for Open Pinball Device input, but you'll need to look at the
// Microsoft Raw Input documentation for details on the specific
// API calls you'll have to add to your program.
// 
// 1. During window creation (WM_CREATE) for your main input
// window, call the Win32 API function RegisterRawInputDevices().
// Specify all of the HID device types that you wish to include in
// Raw Input messages.  To receive input from Open Pinball Device
// controllers, include the following in the array of RAWINPUTDEVICE
// structs passed to the function.  Note that 'hwnd' is the handle
// to the application window that will receive the Raw Input
// messages.
// 
//   { HID_USAGE_PAGE_GAME, HID_USAGE_GAME_PINBALL_DEVICE, 
//     RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd }
// 
// We leave it up to the application to do this registration,
// because the Win32 documentation discourages library developers
// from calling it at the library level, to avoid conflicts over
// which window receives the messages.  The application can also
// register other HID usage types that it will read on its own,
// such as keyboards, mice, and joysticks, by adding entries to
// the RAWINPUTDEVICE array passed to the API call.
// 
// 2. Also during WM_CREATE handling, create an
// OpenPinballDevice::RawInputReader object.  We'll refer to this
// object instance below as 'pRawInputReader', which is notionally
// a RawInputReader* pointer variable.
// 
// 3. In the window procedure for your main input window, handle
// WM_INPUT messages by calling pRawInputReader->ProcessInput().
// If that function returns true, it means that the event was
// an Open Pinball Device event and was fully processed.  If it
// returns false, the event is from some other device that the
// application registered in the RegisterRawInputDevices() call
// during initialization, such as a keyboard or mouse.  If
// Open Pinball Device is the only type of Raw Input device
// you're using, the call to ProcessInput() is the only thing
// you have to do; otherwise, if ProcessInput() returns false,
// you'll need to handle the input, since it's for one of your
// other device types.
// 
// 4. Also in the window proc for your input window, handle
// WM_INPUT_DEVICE_CHANGE messages by calling
// pRawInputReader->ProcessDeviceChange().  This returns true
// if the event was for an Open Pinball Device, in which case
// it was fully handled.  It returns false if the event is for
// some other device type, in which case you should do whatever
// processing you need to do for that other device type.  The
// purpose of this message is to notify you of a new device
// being plugged in or an existing device being removed from
// the system, so if you're tracking a list of active devices,
// you should revise your list accordingly.
// 
// 5. At window destruction (WM_DESTROY), call
// RegisterRawInputDevices() again, with the same array of HID
// usages passed at the call during WM_CREATE, but this time
// pass RIDEV_REMOVE in the flags section for each array element.
// This un-registers the window for raw input messages, which
// is a required step at window destruction.
//
// 6. OPTIONAL: In keeping with the event-oriented Raw Input
// model, the application can implement event-oriented handling
// for Open Pinball Device status changes by overriding the
// OnXxx() methods in the RawInputReader class.  Those will be
// called from ProcessInput() whenever a button or axis change
// occurs.  The RawInputReader will ONLY call these methods
// from ProcessInput(), so the application has complete control
// over the threading context where they'll be invoked.
//
// If you don't wish to use the event callback option, you
// can instead simply check the current states of the Open
// Pinball Device control via the 'state' member of the
// RawInputReader instead.  That tracks the instantaneous
// state of all controls, combined across all physical Open
// Pinball Device instances.  It's equivalent to the Combined
// Reader in the polling-oriented API.

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <unordered_map>
#include <Windows.h>
#include "../OpenPinballDeviceReport.h"

namespace OpenPinballDevice
{
	// Raw Input reader.  This is for use by applications that use the
	// Windows Raw Input API for event-based device input.
	class RawInputReader
	{
	public:
		// Create the reader.  The application should create the reader
		// during initialization of the window where it will receive
		// WM_INPUT and WM_INPUT_DEVICE_CHANGE messages.
		RawInputReader(HWND hwnd);

		// destruction
		~RawInputReader();

		// Process a WM_INPUT message.  Returns true if the event was
		// from an Open Pinball Device controller, in which case the
		// event is fully processed and the application doesn't have
		// to do anything more with it.  Returns false if the event
		// was from some other type of device, such as a keyboard,
		// mouse, or joystick, in which case the application is
		// responsible for processing the input.  'lparam' is the
		// LPARAM from the WM_INPUT event, which is the HRAWINPUT
		// handle to the event data.
		bool ProcessInput(LPARAM lparam) { return ProcessInput(reinterpret_cast<HRAWINPUT>(lparam)); }

		// WM_INPUT handler, for use when the caller has already
		// recovered the HRAWINPUT from the LPARAM.  Callers that
		// handle other device types alongside Open Pinball Device
		// will already have the handle for their own purposes, so
		// it's clearer to let them pass it directly without another
		// coercion.
		bool ProcessInput(HRAWINPUT hRawInput);

		// Process a WM_INPUT_DEVICE_CHANGE message.  Returns true
		// if the event was for an Open Pinball Device controller, in
		// which case the event is fully processed and the application
		// doesn't have to do anything more with it.  Returns false if
		// the event was for some other device type, in which case the
		// application must process it.  'wparam' and 'lparam' are
		// the window message parameters.
		bool ProcessDeviceChange(WPARAM wparam, LPARAM lparam) 
			{ return ProcessDeviceChange(wparam, reinterpret_cast<HANDLE>(lparam)); }
		bool ProcessDeviceChange(WPARAM wparam, HANDLE hRawInput);

		//
		// EVENT ORIENTED CALLBACKS.  The application can override
		// these to receive notifications whenever a button or axis
		// state changes.  The RawInputReader will only call these
		// from ProcessInput(), so the application has complete
		// control over the threading context - there's no danger
		// that the reader object will call these at any other time
		// when the application might not be expecting them.  (The
		// application itself could separately invoke them on its
		// own terms and in other contexts, course, but it doesn't
		// have to worry about the library making calls from any
		// other contexts.)
		//

		// Generic button change event, buttons 0-31
		virtual void OnGenericButton(int button, bool pressed) { }

		// Pinball-function button change event (OPENPINDEV_BTN_xxx)
		virtual void OnPinballButton(int button, bool pressed) { }

		// Axis changes
		virtual void OnNudge(int16_t ax, int16_t ay, int16_t vx, int16_t vy) { }
		virtual void OnPlunger(int16_t pos, int16_t speed) { }

		// Current report state.  This represents the latest live
		// state, with inputs combined from all devices.  The timestamp
		// is the Windows tick time for the latest report from any one
		// device.
		OpenPinballDeviceReport state{ 0 };

	protected:
		// Add a device to our internal list.  This is called during
		// the initial device search at construction, and again any
		// time we get a WM_INPUT_DEVICE_CHANGE event that indicates
		// that a new HID with Game::Pinball Device usage has been
		// added to the system.  This checks the device to determine
		// if it's actually an Open Pinball Device, and if so adds
		// it to our internal list.  Ignores non-OPD devices, even
		// if they have Game::Pinball Device usage, so the caller
		// doesn't have to make any further checks on the device
		// type before calling this.  Returns true if it's an OPD,
		// false if not.
		bool AddDevice(HANDLE hDevice, const RID_DEVICE_INFO_HID *info);

		// Remove a device from the internal list.  Called when a
		// WM_INPUT_DEVICE_CHANGE event indicates that an interface
		// with Game::Pinball Device has been removed from the system.
		// Returns true if it's a known device, false if not.
		bool RemoveDevice(HANDLE hDevice);

		// Individual active Open Pinball Device object
		class Device
		{
		public:
			Device(HANDLE hDevice, const WCHAR *prodName, const WCHAR *serial) :
				hDevice(hDevice), prodName(prodName), serial(serial) { }

			// device handle
			HANDLE hDevice;

			// device identification, mostly for debugging
			std::wstring prodName;
			std::wstring serial;

			// state from last raw input report
			OpenPinballDeviceReport state{ 0 };
		};

		// active devices
		std::unordered_map<HANDLE, Device> devices;

	};

}
