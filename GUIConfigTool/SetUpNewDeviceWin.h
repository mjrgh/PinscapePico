// Pinscape Pico - Config Tool - New Device Setup window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This window is displayed when the "Set Up A New Device" button is
// selected in the drive selection panel.  That button is always
// present, to provide guidance in the UI even if the user hasn't
// set up any Pinscape devices yet.  This window simply displays
// instructions for getting a Pico into Boot Loader mode so that
// the user can proceed with firmware installation.

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
class SetUpNewDeviceWin : public BaseWindow
{
public:
	// construction
    SetUpNewDeviceWin(HINSTANCE hInstance);

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoSetUpNewDevWin"); }

	// Paint off-screen
    virtual void PaintOffScreen(HDC hdc) override;

    // bitmaps
    HBITMAP bmpBootselButton;
    SIZE szBootselButton;
};

