// Pinscape Pico - Config Tool "No Device" window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Placeholder window for config tool child windows, shown when no device
// is available or no device is selected.

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
class NoDeviceWin : public BaseWindow
{
public:
	// construction
	NoDeviceWin(HINSTANCE hInstance) : BaseWindow(hInstance) { }

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoNoDevWin"); }

	// Paint off-screen
	virtual void PaintOffScreen(HDC hdc) override;
};

