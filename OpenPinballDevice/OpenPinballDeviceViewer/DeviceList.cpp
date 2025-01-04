// Open Pinball Device Viewer - Device List Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <list>
#include <memory>
#include <algorithm>
#include <regex>
#include <Windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <Dbt.h>
#include "DeviceList.h"
#include "ViewerWin.h"

// link OpenPinballDeviceLib.lib
#pragma comment(lib, "OpenPinballDeviceLib")


DeviceListWin::DeviceListWin(HINSTANCE hInstance) : BaseWindow(hInstance)
{
}

DeviceListWin::~DeviceListWin()
{
	// clean up GDI objects
	DeleteFont(headlineFont);
}

LRESULT DeviceListWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_DEVICECHANGE:
		// device change notification
		OnDeviceChange(wparam, lparam);
		break;
	}

	// use the base class handling
	return __super::WndProc(msg, wparam, lparam);
}

void DeviceListWin::OnDeviceChange(WPARAM wparam, LPARAM lparam)
{
	if (wparam == DBT_DEVNODES_CHANGED)
	{
		// Device Nodes Changed.  This is a generic notification
		// that top-level windows get when a USB device is added
		// or removed.  It doesn't provide any detail about which
		// device or devices were affected or what kind of change
		// (addition or removal) happened, so simply use this as
		// a signal to refresh our list.
		RebuildDeviceList();
	}
}

void DeviceListWin::OnCreateWindow()
{
	// run the base class handling
	__super::OnCreateWindow();

	// get our window DC
	WindowDC hdc(hwnd);

	// create fonts
	headlineFont = CreateFontA(-MulDiv(11, GetDeviceCaps(hdc, LOGPIXELSY), 72),
		0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
		"Segoe UI");

	// get font metrics
	HFONT oldFont = SelectFont(hdc, headlineFont);
	GetTextMetrics(hdc, &headlineFontMetrics);
	SelectFont(hdc, oldFont);

	// figure the button height
	btnHeight = headlineFontMetrics.tmHeight + 3*mainFontMetrics.tmHeight + btnPaddingY*2;

	// set up the scrollbar
	int cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	HWND hwndScrollbar = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
		0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_MAIN), hInstance, 0);

	// scrollbar callbacks
	auto GetRangeSB = [this](SCROLLINFO &si)
	{
		// figure the client area
		RECT crc;
		GetClientRect(hwnd, &crc);
		int winHt = crc.bottom - crc.top;

		// figure the document height - number of buttons * button height
		int docHt = btnHeight * static_cast<int>(deviceList.size()) + 8;

		// set the range
		si.nMin = 0;
		si.nMax = max(docHt, 0);
		si.nPage = max(winHt - btnHeight, 20);
	};
	auto GetScrollRect = [this](RECT *rc) { };
	auto SetScrollPos = [this](int newPos, int deltaPos) { yScrollBtns = newPos; };

	// set up the scrollbar object
	sb = &scrollbars.emplace_back(hwndScrollbar, SB_CTL, btnHeight, true, true, GetRangeSB, GetScrollRect, SetScrollPos);

	// adjust the layout
	AdjustLayout();

	// build the initial device list
	RebuildDeviceList();
}

void DeviceListWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the internal layout
	AdjustLayout();

	// do the base class work
	__super::OnSizeWindow(type, width, height);
}

void DeviceListWin::AdjustLayout()
{
	// get the client area
	RECT crc;
	GetClientRect(hwnd, &crc);

	// figure the header height
	cyHeader = mainFontMetrics.tmHeight * 5;
	
	// move the scrollbar
	int cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	MoveWindow(sb->hwnd, crc.right - cxScrollbar, cyHeader, cxScrollbar, crc.bottom - cyHeader, TRUE);
}

// update the host item
void DeviceListWin::UpdateHotButton(int x, int y)
{
	// figure the button under the cursor
	int newHot = FindButton(x, y);

	// is the hot button changing?
	if (newHot != hotIndex)
	{
		// redraw the new and old buttons
		InvalButton(hotIndex);
		InvalButton(newHot);

		// set the new hot index
		hotIndex = newHot;

		// if a button is now highlighted, track mouse events to make sure that
		// we see an event (and un-highlight the button) if the cursor leaves
		// the window
		if (newHot >= 0)
		{
			TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
			TrackMouseEvent(&tme);
		}
	}
}

// find the button at a given mouse position
int DeviceListWin::FindButton(int x, int y)
{
	// if it's outside the window, it's not a button
	RECT crc;
	GetClientRect(hwnd, &crc);
	int cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	if (y < cyHeader || x < 0 || x > crc.right - cxScrollbar)
		return -1;

	// find the button index, adjusting for scrolling
	int i = (y - cyHeader + yScrollBtns) / btnHeight;

	// if it's a valid button, return the index, otherwise return -1
	return (i >= 0 && i < static_cast<int>(deviceList.size())) ? i : -1;
}

// invalidate a button's on-screen area
void DeviceListWin::InvalButton(int idx)
{
	if (idx >= 0)
	{
		RECT rc;
		GetClientRect(hwnd, &rc);
		int y = (idx * btnHeight) - yScrollBtns;
		rc.top = y;
		rc.bottom = y + btnHeight;
		InvalidateRect(hwnd, &rc, FALSE);
	}
}

// paint
void DeviceListWin::PaintOffScreen(HDC hdc0)
{
	// set up an HDC helper
	HDCHelper hdc(hdc0);

	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// black text on white background
	COLORREF oldTxColor = SetTextColor(hdc, HRGB(0x000000));
	COLORREF oldBkColor = SetBkColor(hdc, HRGB(0xFFFFFF));
	int oldBkMode = SetBkMode(hdc, TRANSPARENT);

	// clear the background
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

	// draw the header area
	int y = 8;
	RECT rcHeader{ 0, 0, crc.right, cyHeader };
	FillRect(hdc, &rcHeader, HBrush(HRGB(0xf8f8f8)));
	y += hdc.DrawText(16, y, 1, boldFont, HRGB(0x000000), "Open Pinball Device Viewer").cy;
	y += hdc.DrawText(16, y, 1, mainFont, HRGB(0x000000),
		"This tool lists the Open Pinball Device-compatible I/O controllers currently connected to your PC.").cy;
	y += hdc.DrawText(16, y, 1, mainFont, HRGB(0x000000),
		"Click on a device in the list below to view its input status.").cy;

	// separator
	RECT rcSep{ 0, cyHeader - 1, crc.right, cyHeader };
	FillRect(hdc, &rcSep, HBrush(HRGB(0xf0f0f0)));

	// if the list is empty, say so explicitly, to avoid confusion
	if (deviceList.size() == 0)
	{
		y = cyHeader + 16;
		y += hdc.DrawText(16, y, 1, boldFont, HRGB(0x800000),
			"No Open Pinball Device instances detected").cy;
		y += hdc.DrawTextF(16, y, 1, mainFont, HRGB(0x606060),
			"Devices will appear here when connected").cy;
	}

	// draw the list
	int cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	RECT btnrc{ crc.left, crc.top + cyHeader - yScrollBtns, crc.right - cxScrollbar, crc.top + cyHeader - yScrollBtns + btnHeight };
	int idx = 0;
	COLORREF textColor = HRGB(0x000000);
	for (auto &dev : deviceList)
	{
		// draw the button colors according to selected and hot status
		if (idx == clickedIndex && idx == hotIndex)
		{
			// clicked item, mouse is still over it
			FillRect(hdc, &btnrc, HBrush(HRGB(0xFFFF80)));
			FrameRect(hdc, &btnrc, HBrush(HRGB(0x8000FF)));
			textColor = HRGB(0x8000FF);
		}
		else if (idx == hotIndex && clickedIndex < 0)
		{
			// hot item (mouse is hovering over the item), and not tracking a click
			FillRect(hdc, &btnrc, HBrush(HRGB(0xFFFF80)));
			FrameRect(hdc, &btnrc, HBrush(HRGB(0x8000FF)));
			textColor =  HRGB(0x8000FF);
		}
		else if (idx == selIndex)
		{
			// selected item
			FillRect(hdc, &btnrc, HBrush(HRGB(0xF0F0F0)));
			FrameRect(hdc, &btnrc, HBrush(HRGB(0x0000FF)));
			textColor = HRGB(0x0000FF);
		}
		else
		{
			// no highlighting
			FrameRect(hdc, &btnrc, HBrush(HRGB(0xC8C8C8)));
			textColor = HRGB(0x000000);
		}

		// figure the text location, centering it within the button
		int x = btnrc.left + btnPaddingX;
		int y = btnrc.top + btnPaddingY;

		// format the button text
		y += hdc.DrawTextF(x, y, 1, headlineFont, textColor, "%ws", dev.productName.c_str()).cy;
		y += hdc.DrawTextF(x, y, 1, mainFont, textColor, "Open Pinball Device %ws", dev.versionStr.c_str()).cy;
		y += hdc.DrawTextF(x, y, 1, mainFont, textColor, "Manufacturer: %ws", dev.manufacturer.c_str()).cy;
		y += hdc.DrawTextF(x, y, 1, mainFont, textColor, "VID/PID: %04X/%04X, Serial: %ws", dev.vid, dev.pid, dev.serial.c_str()).cy;

		// advance the containing rectangle
		OffsetRect(&btnrc, 0, btnHeight);

		// count the item
		++idx;
	}

	// restore DC properties and close the painting context
	SetBkMode(hdc, oldBkMode);
	SetBkColor(hdc, oldBkColor);
	SetTextColor(hdc, oldTxColor);
}

void DeviceListWin::RebuildDeviceList()
{
	// new device list
	auto newList = OpenPinballDevice::EnumerateDevices();

	// sort the list by device name
	newList.sort([](const OpenPinballDevice::DeviceDesc &a, const OpenPinballDevice::DeviceDesc &b) 
	{
		// sort first by product name
		int c = _wcsicmp(a.productName.c_str(), b.productName.c_str());
		if (c != 0)
			return c < 0;

		// product names are the same; sort by serial number
		return wcscmp(a.serial.c_str(), b.serial.c_str()) < 0;
	});

	// find the old selection in the new list
	if (selIndex >= 0)
	{
		// get the old item
		auto &oldSel = *std::next(deviceList.begin(), selIndex);

		// search for the same device in the new list, matching by path
		selIndex = -1;
		int i = 0;
		for (const auto &d : newList)
		{
			if (strcmp(d.path.c_str(), oldSel.path.c_str()) == 0)
			{
				selIndex = i;
				break;
			}
			++i;
		}
	}

	// put the new list into effect
	deviceList = newList;

	// update the scrollbar
	AdjustScrollbarRanges();
}

void DeviceListWin::SetSelection(int index)
{
	// only proceed if it's changing
	if (index == selIndex)
		return;

	// redraw the old and new selected buttons, to update highlighting
	InvalButton(selIndex);
	InvalButton(index);

	// set the new selection
	selIndex = index;

	// if an item is selected, note its hardware ID, for resyncing
	// with the same item if the list is rebuilt because a device
	// is newly plugged or unplugged
	if (index >= 0)
	{
		// find the item and stash its hardware ID
		auto it = deviceList.begin();
		std::advance(it, index);

		// scroll the selection into view
		ScrollIntoView();
	}
}

void DeviceListWin::ScrollIntoView()
{
	// only proceed if the selection is valid
	if (selIndex < 0)
		return;

	// find the bounds of the selection
	int top = (btnHeight * selIndex) - yScrollBtns;
	int bottom = top + btnHeight;

	// get the client area
	RECT rc;
	GetClientRect(hwnd, &rc);
	int winHt = rc.bottom - rc.top;

	// check if it's outside the client area
	int newScroll = yScrollBtns;
	if (top < 0)
	{
		// it's out of view above - put it at the top
		newScroll += top;
	}
	else if (bottom > winHt)
	{
		// it's out of view below - try aligning at the bottom
		int dy = (bottom - winHt);
		newScroll += dy;
		top -= dy;
		bottom -= dy;

		// if that puts the top out of view, align at the top
		if (top < 0)
			newScroll += top;
	}

	// if it changed, set the new scrollbar location
	if (newScroll != yScrollBtns)
	{
		// update the scroll position
		sb->SetPos(newScroll, newScroll - yScrollBtns);

		// adjust the scrollbar info
		SCROLLINFO si{ sizeof(si), SIF_POS };
		si.nPos = newScroll;
		SetScrollInfo(sb->hwnd, SB_CTL, &si, TRUE);

		// update the hot item
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		UpdateHotButton(pt.x, pt.y);
	}
}

bool DeviceListWin::OnKeyDown(WPARAM vkey, LPARAM flags)
{
	switch (vkey)
	{
	case VK_UP:
		// move selection up
		if (selIndex > 0)
			SetSelection(selIndex - 1);
		else
			ScrollIntoView();
		return true;

	case VK_DOWN:
		// move selection down
		if (selIndex + 1 < static_cast<int>(deviceList.size()))
			SetSelection(selIndex + 1);
		else
			ScrollIntoView();
		return true;

	case VK_HOME:
		// move selection to top of list, and explicitly scroll into view
		SetSelection(0);
		ScrollIntoView();
		return true;

	case VK_END:
		// move selection to end of list and explicitly scroll into view
		SetSelection(static_cast<int>(deviceList.size()) - 1);
		ScrollIntoView();
		return true;

	case VK_RETURN:
		// open the current button's device in a viewer window
		OpenViewerWindow(selIndex);
		return true;

	case VK_ESCAPE:
		// clear the selection
		selIndex = -1;
		return true;

	case 'W':
		// cancel on Ctrl+W
		if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		return true;
	}

	// use default handling
	return false;
}

bool DeviceListWin::OnLButtonDown(WPARAM keys, int x, int y)
{
	// start tracking a click on the active item
	if (int i = FindButton(x, y) ; i >= 0)
	{
		// set the clicked item and update it
		clickedIndex = i;
		InvalButton(i);

		// capture the mouse until the button is released
		SetCapture(hwnd);
	}
	else
	{
		clickedIndex = -1;
		selIndex = -1;
	}

	// handled
	return true;
}

bool DeviceListWin::OnLButtonUp(WPARAM keys, int x, int y)
{
	// if tracking a click, and the mouse is still in the clicked item,
	// select the item
	if (clickedIndex >= 0)
	{
		// remember the clicked item, and end capture
		int i = clickedIndex;
		ReleaseCapture();

		// redraw the clicked item, to account for the highlighting change
		// now that it's no longer clicked
		InvalButton(i);

		// redraw the hot button, since we suppress normal hot button drawing
		// while tracking a click
		InvalButton(hotIndex);

		// check to see if it's in the same item as originally clicked
		if (FindButton(x, y) == i)
		{
			// select the item
			SetSelection(i);

			// open the viewer window
			OpenViewerWindow(i);
		}
	}

	// handled
	return true;
}

bool DeviceListWin::OnMouseMove(WPARAM keys, int x, int y)
{
	// refresh the hot selection
	UpdateHotButton(x, y);

	// handled
	return true;
}

bool DeviceListWin::OnMouseLeave()
{
	hotIndex = -1;
	return __super::OnMouseLeave();
}

bool DeviceListWin::OnCaptureChange(HWND hwnd)
{
	// no more clicked item
	clickedIndex = -1;

	// handled
	return true;
}

void DeviceListWin::OpenViewerWindow(int selectionIndex)
{
	// ignore it if there's no selection
	if (selectionIndex < 0)
		return;

	// get the selected item's descriptor from the list
	auto &dev = *std::next(deviceList.begin(), selectionIndex);

	// get my window location, to use as the basis for the new window location
	RECT wrc;
	GetWindowRect(hwnd, &wrc);

	// open a window on the device
	ViewerWin *viewer = new ViewerWin(hInstance, dev);
	std::shared_ptr<BaseWindow> win(viewer);

	// show the system window
	win->CreateSysWindow(win, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, hwnd,
		dev.productName.c_str(), wrc.left + 32, wrc.top + 32, 700, 700, SW_SHOW);

	// disable the list window while the device viewer is open, so that the
	// viewer acts like a modal dialog
	EnableWindow(hwnd, FALSE);

	// re-enable the list window when the child closes
	viewer->CallOnClose([hwnd = this->hwnd]() { EnableWindow(hwnd, TRUE); });
}
