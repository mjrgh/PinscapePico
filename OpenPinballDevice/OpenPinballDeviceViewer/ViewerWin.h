// Open Pinball Device Viewer - device viewer window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <map>
#include <string>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include "../OpenPinballDeviceLib/OpenPinballDeviceLib.h"
#include "BaseWindow.h"


class ViewerWin : public BaseWindow
{
public:
	ViewerWin(HINSTANCE hInstance, const OpenPinballDevice::DeviceDesc &desc);
	~ViewerWin();

protected:
	// window class name
	const TCHAR *GetWindowClassName() const override { return _T("OPDViewerWin"); }

	// window message handlers
	virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;
	virtual void OnCreateWindow() override;

	// device change notifications
	virtual void OnDeviceChange(WPARAM wparam, LPARAM lparam);

	// Paint off-screen.  This prepares a bitmap with the window
	// contents filled in, for display in the next WM_PAINT.
	virtual void PaintOffScreen(HDC hdc) override;

	// flag: initial layout pending
	bool layoutPending = true;

	// fonts
	HFONT barFont = NULL;

	// bitmaps
	HBITMAP crosshairs = NULL;
	SIZE szCrosshairs{ 0, 0 };

	HBITMAP bmpBtnOn = NULL;
	HBITMAP bmpBtnOff = NULL;
	SIZE szBtn{ 0, 0 };

	HBITMAP bmpPinballBtns = NULL;
	SIZE szPinballBtn{ 0, 0 };

	// device reader
	std::unique_ptr<OpenPinballDevice::Reader> device;

	// device notification registration handle
	HANDLE deviceNotificationHandle = NULL;

	// plunger - peak recent forward (negative) plunger speed, and time observed
	int peakForwardSpeed = 0;
	UINT64 tPeakForwardSpeed = 0;
};
