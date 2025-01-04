// Pinscape Pico - Config Tool - Firmware File Drop Target
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Helper class for implementing Windows Shell drag/drop, to drag a Pico
// UF2 firmware file onto one of our UI windows or a region within the
// window.  This lets the user install new firmware by dropping the UF2
// file containing the update onto the UI control.
//
// The class defined here just manages the drag/drop UI operations; the
// window implements the actual installation process.  The class 
// implements the Win32 IDropTarget interface; a window that wishes to
// act as a drop target creates an instance of the object and registers
// it with the shell via RegisterDragDrop().  We call back to the
// containing window when a drop event occurs.
// 

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <Windows.h>
#include "PinscapePicoAPI.h"

// Firmware drop target implementation
class FirmwareDropTarget : public IDropTarget
{
public:
	// Window interface - an abstract interface to be implemented by 
	// UI objects that use our services.
	class WindowIfc
	{
	public:
		// Hit-test a cursor position, in screen coordinates.  Returns
		// true if this is valid drop location.
		virtual bool IsFirmwareDropLocation(POINTL ptl) = 0;

		// Execute a drag/drop operation
		virtual void ExecFirmwareDrop(POINTL pt, const TCHAR *filename) = 0;
	};

	// construction; the new object has one counted ref added on behalf of the caller
	FirmwareDropTarget(WindowIfc *win) : win(win) { }

	// IDropTarget interface
	ULONG STDMETHODCALLTYPE AddRef();
	ULONG STDMETHODCALLTYPE Release();
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj);
	HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *pDataObj, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect);
	HRESULT STDMETHODCALLTYPE DragLeave();
	HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD *pdwEffect);
	HRESULT STDMETHODCALLTYPE Drop(IDataObject *pDataObj, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect);

	// is a file drag active?
	bool IsDragActive() const { return isDragActive; }

	// Is the current drop target "hot"?  This indicates if the last
	// drag/drop update was dragging a UF2-looking file over a hot
	// spot in the window.  The window can use this to highlight the
	// UI to indicate that a drop at the current location would be
	// effective.
	bool IsTargetHot() const { return isTargetHot; }

	// Detach the parent window.  The parent window must call this before
	// it's destroyed to let us know that we can't send it any further 
	// events.  Note that the parent is also responsible for releasing 
	// its COM reference on the object.
	void Detach();

protected:
	// test a drop at the given point
	bool TestDrop(POINTL ptl, DWORD *pdwEffect);

	// COM reference count -s tart at one, for our caller
	ULONG refCnt = 1;

	// parent window
	WindowIfc *win;

	// current drag/drop source file name
	std::vector<TCHAR> dragDropFile;

	// is a drag in progress?
	bool isDragActive = false;

	// is the drop target hot?
	bool isTargetHot = false;
};
