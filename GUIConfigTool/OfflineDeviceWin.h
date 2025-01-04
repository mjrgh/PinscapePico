// Pinscape Pico - Config Tool "Device Offline" window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Placeholder window for config tool child windows, shown when
// the selected device is currently offline.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <Windows.h>
#include <CommCtrl.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "PinscapePicoAPI.h"
#include "BaseWindow.h"

using BaseWindow = PinscapePico::BaseWindow;
class OfflineDeviceWin : public BaseWindow
{
public:
	// construction
	OfflineDeviceWin(HINSTANCE hInstance);

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoOfflineDevWin"); }

	// Paint off-screen
	virtual void PaintOffScreen(HDC hdc) override;

	// bitmaps
	HBITMAP bmpDisconnect;
	SIZE szDisconnect;
};

