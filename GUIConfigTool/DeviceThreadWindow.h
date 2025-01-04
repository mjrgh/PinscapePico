// Pinscape Pico - Device window class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a base window for Pinscape Pico config tool windows that
// access the device through a vendor interface handle.  This window
// has its own background thread handler that polls the device for
// updates, according to the information that the subclass displays.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <string>
#include <list>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <Windows.h>
#include <CommCtrl.h>
#include "PinscapePicoAPI.h"
#include "BaseWindow.h"
#include "BaseDeviceWindow.h"

namespace PinscapePico
{
	class DeviceThreadWindow : public BaseDeviceWindow
	{
	public:
		// Destruction.  Note that this blocks on our background updater
		// thread exiting.  If the caller wants to control the timing of
		// that more explicitly, use JoinThread(), which allows a timeout
		// to be specified.
		virtual ~DeviceThreadWindow();

		// Suspend/resume the updater thread
		virtual void SuspendUpdaterThread();
		virtual void ResumeUpdaterThread();

	protected:
		// custom messages
		static const UINT MSG_NEW_DATA = WM_USER + 16;   // new data available from updater thread

		// Factory class.  Each concrete window subclass must implement a
		// corresponding concrete factory subclass.  This creates the group
		// of subclassed objects for the window, including the window itself
		// and the updater thread.
		class Factory
		{
		public:
			// Create and display a window, attaching to an given open Pinscape
			// Pico device.  The device object is provided as a shared pointer
			// so that the newly created window can safely hold a reference for
			// its lifetime without affecting the caller's ability to keep its
			// own reference.
			//
			// If an error occurs, displays an error message (via MessageBox)
			// and returns a null object.
			std::shared_ptr<BaseWindow> Create(
				HINSTANCE hInstance, HWND parent, int nCmdShow,
				std::shared_ptr<VendorInterface::Shared> &device,
				int x = CW_USEDEFAULT, int y = CW_USEDEFAULT,
				DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
				DWORD exStyle = WS_EX_CLIENTEDGE);

		protected:
			// create the instance
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, 
				std::shared_ptr<VendorInterface::Shared> &device) = 0;

			// window title
			virtual const TCHAR *WindowTitle() const = 0;

			// window width and height for creation
			virtual int Width() const = 0;
			virtual int Height() const = 0;

			// Filter devices.  This is called during window creation if we
			// find more than one device available, and the user didn't specify
			// a single device by ID.  This lets the subclass filter the list
			// for subclass-specific properties, so that we don't have to ask
			// the user to make a selection when the selection should be
			// obvious from context.  For example, the plunger calibration
			// window implements this to filter for devices that have plungers
			// attached, since it's unlikely that the user would want to run
			// plunger calibration on a plungerless device.  This is only called
			// when the list is ambiguous, so it won't prevent the user from
			// overriding the filter criteria by making an explicit selection
			// prior to opening the window.
			using FeedbackDeviceList = std::list<PinscapePico::FeedbackControllerInterface::Desc>;
			virtual FeedbackDeviceList FilterDevices(const FeedbackDeviceList list) { return list; }
		};

		// protected constructor - clients use the Create function
		struct UpdaterThread;

		// construct
		DeviceThreadWindow(HINSTANCE hInstance, 
			std::shared_ptr<VendorInterface::Shared> &device, 
			UpdaterThread *updater);

		// wait for the thread to exit
		bool JoinThread(DWORD timeout = INFINITE);
		
		// member function window handler
		virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);

		// window message handlers
		virtual void OnDestroy() override;

		// Pinscape unit identifiers
		PinscapePico::DeviceID deviceID;

		// Thread context base class.  This object is shared between 
		// the window and the thread.  The updater thread stores live
		// data received from the device here, and the main UI thread
		// can access it as needed, such as drawing window updates.
		//
		// Each concrete window class should subclass this to add the
		// desired shared data members.
		struct UpdaterThread
		{
			virtual ~UpdaterThread() { }

			// Time between Update() calls, in milliseconds.  By
			// default, we simply run nonstop, so that the thread
			// can poll the device for updates as frequently as
			// possible.  Most of the device windows need continuous
			// polling so that they can display real-time information
			// on the device status.  Some tool windows don't need
			// frequent updates, so they can tell us to take a more
			// leisurely approach.
			uint32_t timeBetweenUpdates = 0;

			// Thread Started event.  We use this for a startup
			// handshake between the main thread and updater thread,
			// to let the main thread know that the updater thread
			// has claimed its references on shared resources.
			HANDLE threadStartedEvent{ CreateEvent(NULL, TRUE, FALSE, NULL) };

			// UI window handle
			HWND hwnd = NULL;

			// Target device.  This is a shared pointer because the
			// device object might have been created by the containing
			// application, which might wish to keep its own reference
			// to the object.  The shared pointer ensures that both
			// references (and any others) will remain valid for the
			// lifetimes of the respective owners.
			std::shared_ptr<VendorInterface::Shared> device;

			// flag: thread exit requested
			volatile bool exitThread = false;

			// flag: thread suspension requested
			volatile bool suspendThread = false;

			// flag: the thread is currently in a suspend wait
			volatile bool inSuspendWait = false;

			// Event object.  We use this when we're in suspend mode,
			// awaiting notifications from the parent window.
			HANDLE hEvent{ CreateEvent(NULL, FALSE, FALSE, NULL) };

			// Mutex for accessing our shared data
			HANDLE dataMutex{ CreateMutex(NULL, FALSE, NULL) };

			// Thread entrypoint
			DWORD Main();

			// Thread data updater.  The main thread calls this
			// in a loop, acquiring the device handle mutex before 
			// each call.  This should retrieve the desired device 
			// data, populate local fields with the results, and 
			// return.  This routine should always acquire the
			// data mutex before writing local fields, since the
			// local fields are intended to be shared with the UI
			// thread.
			//
			// Important: this routine must not use SendMessage(),
			// to its own window or any other window.  The caller
			// is holding the device mutex for the duration of this
			// routine.  The main UI thread, where the message loop
			// is located, ALSO acquires the device mutex from time
			// to time.  SendMessage() blocks waiting for the main
			// thread to process the message, so if the main thread
			// happens to be trying to acquire the device mutex when
			// we call SendMessage(), the main thread will get stuck
			// waiting for us to release the mutex we're holding,
			// thus the deadlock.  If you need to send a message to
			// a window, either use PostMessage(), or explicitly
			// release the device mutex first.
			//
			// Return true on success, false if a device error
			// occurs.  On device error, we'll pause before the
			// next call, to ensure that we don't saturate the
			// CPU if we're failing repeatedly on I/O errors on
			// the device connection.
			//
			// The routine is allowed to explicit release the
			// device mutex on its own, if it wishes.  If it does,
			// it must set releasedMutex = true before returning.
			virtual bool Update(bool &releasedMutex) = 0;
		};
		std::shared_ptr<UpdaterThread> updaterThread;

		// updater thread handle
		HANDLE hThread = NULL;
	};

}

