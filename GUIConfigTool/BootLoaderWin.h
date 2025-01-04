// Pinscape Pico - Config Tool - Boot Loader Drive window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This window represents a Pico in Boot Loader mode, where it presents
// itself as a USB virtual thumb drive.  In this mode, the Pico will
// accept a UF2 file via a Windows file copy operation, and will install
// the Pico program image contained in the file into the Pico's flash.
// This window provides a simple UI for selecting a UF2 file from the
// local hard file system and copying it to the Pico virtual drive.

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
#include "FirmwareDropTarget.h"

using BaseWindow = PinscapePico::BaseWindow;
class BootLoaderWin : public BaseWindow, public FirmwareDropTarget::WindowIfc
{
public:
	// construction
	BootLoaderWin(HINSTANCE hInstance, RP2BootDevice *device);

	// destruction
	~BootLoaderWin();

	// set my menu bar in the host application
	virtual bool InstallParentMenuBar(HWND hwndContainer) override;

	// translate accelerators
	virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

	// command handling
	virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult) override;
	virtual void OnInitMenuPopup(HMENU menu, WORD itemPos, bool isSysMenu) override;

	// FirmwareDropTarget::WindowIfc implementation
	virtual bool IsFirmwareDropLocation(POINTL ptl) override;
	virtual void ExecFirmwareDrop(POINTL pt, const TCHAR *filename) override;

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoBootLoaderWin"); }

	// private messages
	static const UINT MSG_INSTALL_FIRMWARE = WM_USER + 301;

	// Paint off-screen
	virtual void PaintOffScreen(HDC hdc) override;

	// window messages
	virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;
	virtual void OnCreateWindow() override;
	virtual void OnDestroy() override;
	virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;

	// install a firmware file
	void InstallFirmwareFile(const TCHAR *filename);

	// boot device
	RP2BootDevice device;

	// firmware install button rect
	RECT rcFirmwareInstallButton{ 0, 0, 0, 0 };

	// menu/accelerator
	HMENU hMenuBar = NULL;
	HACCEL hAccel = NULL;

	// drag/drop helper
	FirmwareDropTarget *dropTarget{ new FirmwareDropTarget(this) };

	// pending firmware install file
	std::basic_string<TCHAR> pendingFirmwareInstall;
};
