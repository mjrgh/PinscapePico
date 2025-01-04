// Pinscape Pico - Base window class for config tool device access windows
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <io.h>
#include <cstdlib>
#include <math.h>
#include <list>
#include <iterator>
#include <memory>
#include <ctime>
#include <algorithm>
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include <Shlwapi.h>
#include "PinscapePicoAPI.h"
#include "Utilities.h"
#include "BaseWindow.h"
#include "DeviceThreadWindow.h"

using namespace PinscapePico;


// --------------------------------------------------------------------------
//
// Device window factory
//

// Create a window for a given target device
std::shared_ptr<BaseWindow> DeviceThreadWindow::Factory::Create(
	HINSTANCE hInstance, HWND hwndParent, int nCmdShow,
	std::shared_ptr<VendorInterface::Shared> &device,
	int x, int y, DWORD style, DWORD exStyle)
{
	// initialize the common controls we use
	INITCOMMONCONTROLSEX icx{ sizeof(icx), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_BAR_CLASSES };
	InitCommonControlsEx(&icx);

	// create the window object
	DeviceThreadWindow *devWin = New(hInstance, device);

	// create the system window
	std::shared_ptr<BaseWindow> win(devWin);
	win->CreateSysWindow(win, style, exStyle, hwndParent, WindowTitle(), x, y, Width(), Height(), nCmdShow);

	// Updater thread static entrypoint.  This takes a pointer to the shared_ptr
	// to the UpdaterThread object as the LPVOID thread context argument, and 
	// creates its own separate shared_ptr to the UpdaterThread on the thread's
	// stack.  The stacked shared_ptr ensures that the UpdaterThread remains
	// referenced as long as the thread is running.
	static auto ThreadMain = [](void *pv) -> DWORD {
		auto &ctx = *reinterpret_cast<std::shared_ptr<UpdaterThread>*>(pv);
		std::shared_ptr<UpdaterThread> myRef(ctx);
		SetEvent(ctx->threadStartedEvent);
		return myRef->Main();
	};

	// link the updater thread to the window
	devWin->updaterThread->hwnd = devWin->hwnd;

	// launch the thread
	DWORD threadID;
	devWin->hThread = CreateThread(NULL, 0, ThreadMain, &devWin->updaterThread, 0, &threadID);
	if (devWin->hThread == NULL)
	{
		MessageBox(NULL, _T("Unable to create device monitor thread"),
			_T("Pinscape Pico"), MB_OK | MB_ICONERROR);
		return nullptr;
	}

	// Wait for the thread to start up.  This is an essential bit of
	// handshaking to guarantee that the thread has claimed its reference
	// on the thread context shared_ptr.  If we didn't wait for that, it
	// would be possible for the main thread to return to the caller, and
	// for the caller to continue on to release its reference on the window
	// object, which would in turn release its reference on the thread
	// context, before the thread even started running.  If that happened,
	// the thread context would have been deleted before the thread could
	// claim its shared_ptr reference to keep the context alive.
	WaitForSingleObject(devWin->updaterThread->threadStartedEvent, INFINITE);

	// hand off the shared_ptr to the window to the caller
	return win;
}

// --------------------------------------------------------------------------
//
// Device window
//

DeviceThreadWindow::DeviceThreadWindow(HINSTANCE hInstance,
	std::shared_ptr<VendorInterface::Shared> &device, 
	UpdaterThread *updater) :
	BaseDeviceWindow(hInstance, device), updaterThread(updater)
{
	// give the thread context a device reference
	updaterThread->device = device;

	// query device IDs
	if (VendorInterface::Shared::Locker l(device); l.locked)
		device->device->QueryID(deviceID);
}

DeviceThreadWindow::~DeviceThreadWindow()
{
	// tell the thread to exit, with a short wait
	JoinThread(500);
}

bool DeviceThreadWindow::JoinThread(DWORD timeout)
{
	// if we've already cleaned up the thread (or never created one),
	// there's nothing to do
	if (hThread == NULL)
		return true;

	// set the flag to tell the thread to terminate, and signal
	// the flag change event in case it's in suspend mode
	updaterThread->exitThread = true;
	SetEvent(updaterThread->hEvent);

	// wait for the thread to exit, up to the specified timeout
	if (WaitForSingleObject(hThread, timeout) == WAIT_OBJECT_0)
	{
		CloseHandle(hThread);
		hThread = NULL;
		return true;
	}

	// failed
	return false;
}

LRESULT DeviceThreadWindow::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
	// Process the message
	switch (msg)
	{
	case MSG_NEW_DATA:
		// redraw the window whenever we get new data
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	}

	// not intercepted - inherit the default handling
	return __super::WndProc(msg, wparam, lparam);
}

void DeviceThreadWindow::OnDestroy()
{
	// tell the updater thread to exit by setting the flag
	updaterThread->exitThread = true;

	// wake up the thread if it's suspended
	SetEvent(updaterThread->hEvent);
}

void DeviceThreadWindow::SuspendUpdaterThread()
{
	// set the suspend flag
	updaterThread->suspendThread = true;

	// wait briefly for the thread to acknowledge
	for (auto timeout = GetTickCount64() + 100 ;
		!updaterThread->inSuspendWait && GetTickCount64() < timeout ;
		Sleep(10)) ;
}

void DeviceThreadWindow::ResumeUpdaterThread()
{
	if (updaterThread != nullptr)
	{
		// clear the suspend flag
		updaterThread->suspendThread = false;

		// wake up the thread by signaling the flag status change event
		SetEvent(updaterThread->hEvent);
	}
}


// --------------------------------------------------------------------------
//
// Updater Thread
//

// thread entrypoint
DWORD DeviceThreadWindow::UpdaterThread::Main()
{
	// loop until the exit flag is set
	for (;;)
	{
		// pause between updates if desired
		if (timeBetweenUpdates != 0)
		{
			// wait for the time to expire, or for the parent window to signal 
			// an event; if the wait fails, abort the thread
			if (WaitForSingleObject(hEvent, timeBetweenUpdates) == WAIT_FAILED)
				break;
		}

		// if thread exit was signaled, abort the thread
		if (exitThread)
			break;

		// if thread suspension is requested, stop and wait for an event 
		// notification
		if (suspendThread)
		{
			// flag that we're in a suspend wait
			inSuspendWait = true;

			// wait for the event object; if that fails, abort the thread
			if (WaitForSingleObject(hEvent, INFINITE) == WAIT_FAILED)
				break;

			// resume at the top of the loop, to check the new flags
			continue;
		}

		// we're no longer in a suspend wait
		inSuspendWait = false;

		// hold the device mutex while running the query
		switch (DWORD waitResult = WaitForSingleObject(device->mutex, 100))
		{
		case WAIT_OBJECT_0:
			// successfully acquired the mutex - we can now access the device safely
			{
				// do the update
				bool releasedMutex = false;
				bool ok = Update(releasedMutex);

				// if the callee didn't release the mutex, do so now
				if (!releasedMutex)
					ReleaseMutex(device->mutex);

				// on error, pause before retrying, so that we don't saturate
				// the CPU if the error is persistent and failing without I/O
				// waits (for example, the USB connection is broken)
				if (!ok)
					Sleep(100);
			}
			break;

		case WAIT_TIMEOUT:
			// timeout state - try again later
			break;

		case WAIT_FAILED:
		default:
			// wait failed or other error - abort the thread
			OutputDebugStringA(StrPrintf("DeviceThreadWindow::UpdaterThread::Main: Wait failed with code %d; aborting thread\n", waitResult).c_str());
			exitThread = true;
			break;
		}
	}

	// successful completion
	return 0;
}
