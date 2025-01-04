// Open Pinball Device Viewer - Device List Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <list>
#include <memory>
#include <tchar.h>
#include <Windows.h>
#include "../OpenPinballDeviceLib/OpenPinballDeviceLib.h"
#include "BaseWindow.h"

// Device selection dialog window
class DeviceListWin : public BaseWindow
{
public:
	DeviceListWin(HINSTANCE hInstance);
	~DeviceListWin();

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("OPDViewerDevListWindow"); }

	// window message handlers
	virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;
	virtual void OnCreateWindow() override;
	virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
	virtual bool OnKeyDown(WPARAM vkey, LPARAM flags) override;
	virtual bool OnMouseMove(WPARAM keys, int x, int y) override;
	virtual bool OnMouseLeave() override;
	virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
	virtual bool OnLButtonUp(WPARAM keys, int x, int y) override;
	virtual bool OnCaptureChange(HWND hwnd) override;

	// device change notifications
	virtual void OnDeviceChange(WPARAM wparam, LPARAM lparam);

	// control IDs
	static const INT_PTR ID_SB_MAIN = 201;

	// scrollbar
	Scrollbar *sb = nullptr;

	// adjust window layout
	void AdjustLayout();

	// painting
	virtual void PaintOffScreen(HDC hdc) override;

	// open a viewer window on a given list item
	void OpenViewerWindow(int selectionIndex);

	std::list<OpenPinballDevice::DeviceDesc> deviceList;

	// Current selected device
	int selIndex = -1;
	int hotIndex = -1;
	int clickedIndex = -1;

	// button height and text height
	int btnHeight = 0;
	int btnPaddingX = 16;
	int btnPaddingY = 4;

	// header section height
	int cyHeader = 0;

	// display font
	HFONT headlineFont = NULL;
	TEXTMETRIC headlineFontMetrics{ 0 };

	// vertical scrolling offset
	int yScrollBtns = 0;

	// update the hot button for a given mouse location
	void UpdateHotButton(int x, int y);

	// set the selected item
	void SetSelection(int index);

	// find the button for a given mouse location; returns the 
	// button index
	int FindButton(int x, int y);

	// invalidate a button's on-screen area
	void InvalButton(int idx);

	// scroll the selection into view
	void ScrollIntoView();

	// build/rebuild the device list
	void RebuildDeviceList();
};
