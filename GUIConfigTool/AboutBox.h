// Pinscape Pico - Config Tool About box
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

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
class AboutBox : public BaseWindow
{
public:
	// construction
	AboutBox(HINSTANCE hInstance);

	// destruction
	~AboutBox();

	// command handler
	virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult);

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoOfflineDevWin"); }

	// window message handlers
	virtual void OnCreateWindow() override;
	virtual bool OnKeyDown(WPARAM vkey, LPARAM flags) override;
	virtual bool OnCtlColor(UINT msg, HDC hdc, HWND hwndCtl, HBRUSH &hbrush) override;
	virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;

	// Paint off-screen
	virtual void PaintOffScreen(HDC hdc) override;

	// license text hot spot
	RECT rcLicenseTxt{ 0, 0, 0, 0 };

	// fonts
	HFONT titleFont = NULL;

	// controls
	HWND okBtn = NULL;
	HWND editBox = NULL;
	bool editBoxPositioned = false;

	// bitmaps
	HBITMAP bmpLogo = NULL;
	SIZE szLogo;
};

