// Pinscape Pico - Config Tool - IR Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <list>
#include <iterator>
#include <memory>
#include <algorithm>
#include <regex>
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "PinscapePicoAPI.h"
#include "Dialog.h"
#include "Application.h"
#include "IRTesterWin.h"
#include "Utilities.h"
#include "WinUtil.h"
#include "resource.h"

// the window class is part of the PinscapePico namespace
using namespace PinscapePico;

IRTesterWin::IRTesterWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
	DeviceThreadWindow(hInstance, device, new UpdaterThread())
{
	// Load my menu bar and accelerator
	hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_IRTESTERWIN));
	hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_DEVICEWIN));

	// load the command history and scope context menus
	ctxMenuHist = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_IRCMDHIST));
	ctxMenuScope = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_IRSCOPE));

	// query device information
	QueryDeviceInfo();
}

IRTesterWin::~IRTesterWin()
{
}

bool IRTesterWin::OnDeviceReconnect()
{
	// re-query the device information
	QueryDeviceInfo();

	// tell the container that we've refreshed, so it can keep the window open
	return true;
}

void IRTesterWin::OnEraseDeviceConfig(bool factoryReset)
{
	// re-query the device information
	QueryDeviceInfo();
}

void IRTesterWin::QueryDeviceInfo()
{
	// we don't currently cache any configuration information, so there's
	// nothing to refresh here, but we'll leave this as a placeholder in
	// case it's needed in the future
}

bool IRTesterWin::InstallParentMenuBar(HWND hwndContainer)
{
	SetMenu(hwndContainer, hMenuBar);
	return true;
}

// translate keyboard accelerators
bool IRTesterWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
	return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

void IRTesterWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the layout
	AdjustLayout();

	// do the base class work
	__super::OnSizeWindow(type, width, height);
}

void IRTesterWin::AdjustLayout()
{
	// get the window size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// set the width of the oscilloscope area
	rcScope.right = crc.right - 16;

	// move the history scrollbar
	MoveWindow(sbHist, rcHist.right - cxScrollbar, rcHist.top, cxScrollbar, rcHist.bottom - rcHist.top, TRUE);
}

// Paint off-screen
void IRTesterWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// fill the background
	HDCHelper hdc(hdc0);
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

	// get a local copy of the data for drawing the update
	if (WaitForSingleObject(updaterThread->dataMutex, 100) == WAIT_OBJECT_0)
    {
        // get the updater thread, downcast to our subclass type
        auto *ut = static_cast<UpdaterThread*>(updaterThread.get());

		// note the current number of IR reports, and the scrollbar position
		size_t nReports = irHist.size();
		SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_POS | SIF_PAGE };
		GetScrollInfo(sbHist, SB_CTL, &si);

		// grab IR pulses
		if (ut->irRaw.size() != 0)
		{
			// If it's been a while since we received the last pulse,
			// clear the buffer.  This will automatically start a new
			// trace when a new command starts after a quiet period.
			UINT64 now = GetTickCount64();
			if (now - tLastPulse > 2500)
				irPulse.clear();

			tLastPulse = now;

			// copy the pulses
			while (ut->irRaw.size() != 0)
			{
				irPulse.emplace_back(ut->irRaw.front());
				ut->irRaw.pop_front();
			}
		}

		// grab accumulated IR reports from the updater thread
		while (ut->irReports.size() != 0)
		{
			// If this is an auto-repeat of the last item, leave the
			// last item in place and increase its repeat count.  Replace
			// the last item's report with the new report, because the
			// presence of dittos can be only be detected in a repeat.
			auto &command = ut->irReports.front();
			if (command.isAutoRepeat && irHist.size() != 0 && irHist.back().command == command)
			{
				// it's a repeat - increment the repeat count, update the command
				auto &h = irHist.back();
				h.nRepeats += 1;
				h.command = command;
			}
			else
			{
				// it's not a repeat - add a new item to the list
				irHist.emplace_back(command);
			}

			// consume the incoming report from the updater queue
			ut->irReports.pop_front();
		}

		// grab the latest TV ON state
		auto tv = ut->tvOnState;

		// done with the data mutex
		ReleaseMutex(updaterThread->dataMutex);

		// adjust the scrollbar if we added new reports
		if (nReports != irHist.size())
		{
			// adjust the range
			AdjustScrollbarRanges();

			// if the scrollbar was previously scrolled to the bottom, keep
			// it scrolled to the bottom
			int oldMax = si.nMax - max(si.nPage - 1, 0);
			if (si.nPos >= oldMax)
			{
				// yes - scroll to bottom - figure the new max
				SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_POS | SIF_PAGE };
				GetScrollInfo(sbHist, SB_CTL, &si);
				int newMax = si.nMax - max(si.nPage - 1, 0);

				// ...and go there
				si.fMask = SIF_POS;
				si.nPos = newMax;
				yScrollHist = newMax;
				SetScrollInfo(sbHist, SB_CTL, &si, TRUE);
			}
		}

		// Layout parameters
        static const int xMargin = 16;
        int y0 = crc.top + 16;
        int x = xMargin;
		int y = y0;

		//
		// IR Oscilloscope
		//

		// label it
		hdc.DrawText(rcScope.left, rcScope.top - boldFontMetrics.tmHeight - 8, 1,
			boldFont, HRGB(0x800080), "IR Oscilloscope");
		FillRect(hdc, &rcScope, HBrush(HRGB(0x005800)));
		FrameRect(hdc, &rcScope, HBrush(HRGB(0x000000)));

		// draw the background and grid lines
		HPen gridPen(HRGB(0x006800), 1);
		HPEN oldPen = SelectPen(hdc, gridPen);
		const int xGrid = 8;
		const int yGrid = 3;
		const int dxGrid = (rcScope.right - rcScope.left)/(xGrid + 1);
		const int dyGrid = (rcScope.bottom - rcScope.top)/(yGrid + 1);
		for (int row = 0, y = rcScope.top + yGrid ; row < yGrid ; ++row, y += dyGrid)
		{
			MoveToEx(hdc, rcScope.left + 1, y, NULL);
			LineTo(hdc, rcScope.right - 1, y);
		}
		for (int col = 0, x = rcScope.left + xGrid ; col < xGrid ; ++col, x += dxGrid)
		{
			MoveToEx(hdc, x, rcScope.top + 1, NULL);
			LineTo(hdc, x, rcScope.bottom - 1);
		}
		SelectPen(hdc, oldPen);

		// draw the latest signal
		if (auto nPulse = irPulse.size(); nPulse != 0)
		{
			// sum up the pulse times, treating overlong pulses as 130us
			int totalTime = 0;
			for (auto &pulse : irPulse)
				totalTime += (pulse.t < 0 ? 130000 : pulse.t);

			// only proceed if we have a non-zero total time
			if (totalTime > 0)
			{
				// figure the x spacing per pulse to almost fill out the width
				float cxPulse = static_cast<float>(max(rcScope.right - rcScope.left - 60, 64)) / static_cast<float>(totalTime);

				// set up a drawing pen
				HPen pulsePen(HRGB(0x80ff80));
				HPen tracePen(HRGB(0x009000));
				oldPen = SelectPen(hdc, pulsePen);

				// figure the vertical layout and the start of the first pulse
				const int ySpace = rcScope.bottom - 10;
				const int yMark = rcScope.top + 10;
				float xPulse = static_cast<float>(rcScope.left + 25);

				// draw a brief lead-in before the first pulse
				MoveToEx(hdc, rcScope.left + 1, ySpace, NULL);
				LineTo(hdc, static_cast<int>(roundf(xPulse)), ySpace);

				// draw the pulses
				for (auto &pulse : irPulse)
				{
					// treat long pulses as 130us
					int t = pulse.t < 0 ? 130000 : pulse.t;

					// draw it
					SelectPen(hdc, tracePen);
					int yPulse = pulse.mark ? yMark : ySpace;
					LineTo(hdc, static_cast<int>(roundf(xPulse)), yPulse);
					
					SelectPen(hdc, pulsePen);
					xPulse += cxPulse * t;
					LineTo(hdc, static_cast<int>(roundf(xPulse)), yPulse);
				}

				// close out the display
				SelectPen(hdc, tracePen);
				LineTo(hdc, static_cast<int>(roundf(xPulse)), ySpace);
				SelectPen(hdc, pulsePen);
				LineTo(hdc, rcScope.right - 1, ySpace);

				// clean up the HDC pen selection
				SelectPen(hdc, oldPen);
			}
		}

		// separator line
		y = rcScope.bottom + 32;
		RECT rcSep{ 16, y, crc.right - 16, y + 1 };
		FillRect(hdc, &rcSep, HBrush(HRGB(0xe0e0e0)));

		//
		// Command history box
		//

		// Label it
		const int yMainIRSection = rcHist.top - boldFontMetrics.tmHeight - 8;
		hdc.DrawText(rcHist.left, yMainIRSection, 1,
			boldFont, HRGB(0x800080), "IR commands received");

		// Draw the history box, clipping to within the box
		IntersectClipRect(hdc, rcHist.left, rcHist.top, rcHist.right, rcHist.bottom);
		int yh = rcHist.top - yScrollHist;
		RECT rcItem{ rcHist.left, yh, rcHist.right, yh + cyHistLine };
		firstHistInView = irHist.end();
		for (auto it = irHist.begin() ; it != irHist.end() ; ++it)
		{
			// set the item rect
			it->rc = rcItem;

			// only draw if we're in view
			if (rcItem.bottom > rcHist.top && rcItem.top < rcHist.bottom)
			{
				// get the item
				auto &h = *it;

				// note if it's the first item in view
				if (firstHistInView == irHist.end())
					firstHistInView = it;

				// highlight if hot
				bool hot = IsClientRectHot(rcItem);
				if (hot)
					FillRect(hdc, &rcItem, HBrush(HRGB(0xe0f0ff)));

				// draw the command string
				int x = rcHist.left + 8;
				int y = rcItem.top + (cyHistLine - mainFontMetrics.tmHeight)/2;
				x += hdc.DrawText(x, y, 1, boldFont, HRGB(0x000000), h.command.ToString().c_str()).cx;

				// add a repeat count
				if (h.nRepeats > 1)
					x += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x404040), "  x%d", h.nRepeats).cx;

				// if it's hot, add the Copy and Transmit buttons
				if (IsClientRectHot(rcItem))
				{
					// draw right-to-left, for right alignment in the box
					int xb = rcItem.right - cxScrollbar - 16;
					auto Button = [this, &hdc, y, &xb](const char *name, RECT &rcButton)
					{
						// calculate the layout and draw
						RECT rc{ xb - hdc.MeasureText(mainFont, name).cx, y, xb, y + mainFontMetrics.tmHeight };
						hdc.DrawText(rc.left, y, 1, mainFont, HRGB(IsClientRectHot(rc) ? 0xff00ff : 0x0000ff), name);
						xb = rc.left - 8;

						// save it in the item history
						rcButton = rc;
					};
					Button("Transmit", h.rcTransmit);
					Button("Copy", h.rcCopy);
				}
			}

			// next line
			OffsetRect(&rcItem, 0, cyHistLine);
		}

		// restore clipping, frame the history box
		SelectClipRgn(hdc, NULL);
		RECT frc = rcHist;
		InflateRect(&frc, 1, 1);
		FrameRect(hdc, &frc, HBrush(HRGB(0xe0e0e0)));

		// add notes
		y = rcHist.bottom + 8;
		y += hdc.DrawTextF(rcHist.left, y, 1, mainFont, HRGB(0x000000),
			"Note: to \"learn\" a code for use with the transmitter, hold down the remote").cy;
		y += hdc.DrawTextF(rcHist.left, y, 1, mainFont, HRGB(0x000000),
			"button long enough to trigger auto-repeat.  This helps identify the coding").cy;
		y += hdc.DrawTextF(rcHist.left, y, 1, mainFont, HRGB(0x000000),
			"that the remote uses to indicate repeats.").cy;

		// separator
		y += 32;
		rcSep ={ 16, y, crc.right - 16, y + 1 };
		FillRect(hdc, &rcSep, HBrush(HRGB(0xe0e0e0)));
		const int yTVONSection = y + 32;

		//
		// Transmitter
		//

		// transmit area label
		x = rcHist.right + mainFontMetrics.tmAveCharWidth*20;
		y = yMainIRSection;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Transmitter Tester").cy + 16;

		// get a control rect, applying new layout if necessary
		auto GetCtlRect = [this, &hdc](HWND ctl, int x, int y, const RECT *centerOnRect = nullptr)
		{
			// get the control area
			RECT rc = GetChildControlRect(ctl);

			// if layout is pending, reposition it to the given x/y coords
			if (layoutPending)
			{
				// if a centering rect was provided, center vertically on the given rect
				if (centerOnRect != nullptr)
					y += ((centerOnRect->bottom - centerOnRect->top) - (rc.bottom - rc.top))/2;
				
				// set the new position
				OffsetRect(&rc, x - rc.left, y - rc.top);
				SetWindowPos(ctl, NULL, x, y, -1, -1, SWP_NOSIZE);
				InvalidateRect(ctl, NULL, TRUE);
			}

			// return the rectangle
			return rc;
		};
		int yCtl = y + mainFontMetrics.tmHeight;
		RECT rcTransmit = GetCtlRect(transmitEditBox, x, yCtl);
		RECT rcRepeat = GetCtlRect(repeatEditBox, rcTransmit.right + 12, yCtl);
		RECT rcSpin = GetCtlRect(repeatSpin, rcRepeat.right, yCtl);
		RECT rcBtn = GetCtlRect(transmitBtn, rcSpin.right + 12, yCtl, &rcTransmit);

		// frame the edit boxes
		auto FrameEdit = [&hdc](int left, int top, int right, int bottom)
		{
			RECT frc{ left - 1, top - 1, right + 1, bottom + 1 };
			FrameRect(hdc, &frc, HBrush(HRGB(0xe0e0e0)));
		};
		FrameEdit(rcTransmit.left, rcTransmit.top, rcTransmit.right, rcTransmit.bottom);
		FrameEdit(rcRepeat.left, rcRepeat.top, rcSpin.right, rcRepeat.bottom);

		// label the edit boxes
		hdc.DrawText(rcTransmit.left, y, 1, mainFont, HRGB(0x000000), "Enter IR Code");
		hdc.DrawText(rcRepeat.left, y, 1, mainFont, HRGB(0x000000), "Repeats");

		// add the status text, if any
		y = rcRepeat.bottom + 16;
		UINT64 now = GetTickCount64();
		const UINT FadeStart = 3000, FadeLen = 2000, VisibleTime = FadeStart + FadeLen;
		if (now < transmitStatus.t + VisibleTime)
		{
			// fade the color after the fade start time
			COLORREF c = transmitStatus.color;
			if (now > transmitStatus.t + FadeStart)
			{
				float frac = static_cast<float>(now - transmitStatus.t - FadeStart) / FadeLen;
				auto Fade = [frac](BYTE b) -> uint8_t {
					int i = static_cast<int>(roundf(b + (255 - b)*frac));
					return i < 0 ? 0 : i > 255 ? 255 : static_cast<uint8_t>(i);
				};
				c = RGB(Fade(GetRValue(c)), Fade(GetGValue(c)), Fade(GetBValue(c)));
			}

			// draw each line
			for (char *p = transmitStatus.msg.data(), *endp = p + transmitStatus.msg.size() ; p < endp ; )
			{
				// convert newlines to nulls
				char *nl = strchr(p, '\n');
				if (nl != nullptr)
					*nl = 0;

				// draw this line
				y += hdc.DrawText(rcTransmit.left, y, 1, mainFont, c, p).cy;

				// advance to the next line
				p += strlen(p) + 1;
			}
		}

		//
		// TV ON state
		//

		// label the section
		x = rcHist.left;
		y = yTVONSection;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "TV ON Status").cy + 16;

		// If this is a new state unknown to us, add it to our map.  This at
		// least shows the name for states added to the firmware since this
		// version.
		if (auto it = tvOnStateNames.find(tv.powerState); it == tvOnStateNames.end())
			tvOnStateNames.emplace(tv.powerState, tv.powerStateName);

		// display the state names in order, highlighting the current one
		for (auto &s : tvOnStateNames)
		{
			if (tv.powerState == s.first)
			{
				SetBkMode(hdc, OPAQUE);
				SetBkColor(hdc, HRGB(0x008000));
				y += hdc.DrawTextF(x + 12, y, 1, boldFont, HRGB(0xffffff), "[ %s ]", s.second.c_str()).cy;
				SetBkMode(hdc, TRANSPARENT);
			}
			else
				y += hdc.DrawTextF(x + 12, y, 1, mainFont, HRGB(0x808080), "  %s  ", s.second.c_str()).cy;
		}

		// Power latch status
		x = rcHist.left + boldFontMetrics.tmAveCharWidth * 24;
		y = yTVONSection;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Power Sense GPIO").cy + 16;
		if (tv.gpioState)
			hdc.DrawText(x + 12, y, 1, boldFont, HRGB(0x008000), "[ HIGH ]");
		else
			hdc.DrawText(x + 12, y, 1, boldFont, HRGB(0x808080), "[ low ]");

		// TV Relay status
		x = rcHist.left + boldFontMetrics.tmAveCharWidth * 48;
		y = yTVONSection;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "TV Relay").cy + 16;
		if (tv.relayState)
			hdc.DrawText(x + 12, y, 1, boldFont, HRGB(0x008000), "[ ON ]");
		else
			hdc.DrawText(x + 12, y, 1, boldFont, HRGB(0x808080), "[ off ]");

		// TV Relay buttons
		y += 32;
		RECT rcPulseBtn = GetCtlRect(tvRelayPulseBtn, x, y);
		RECT rcOnBtn = GetCtlRect(tvRelayOnBtn, rcPulseBtn.right + 16, y);

		// update the label on the RELAY ON button to reflect the current
		// manual control state
		if (tv.relayStateManual != relayOnBtnState)
		{
			relayOnBtnState = tv.relayStateManual;
			SetWindowTextA(tvRelayOnBtn, relayOnBtnState ? "Manual Off" : "Manual On");
		}

		// layout has been completed
		layoutPending = false;
	}
}

void IRTesterWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

    // get the window DC
	HDC hdc = GetWindowDC(hwnd);

	// get the window's current size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// figure the oscilloscope area
	int x = 16;
	int y = 16 + boldFontMetrics.tmHeight + 8;
	rcScope ={ x, y, crc.right - 16, y + 64 };
	y = rcScope.bottom + 48;

	// figure the history area line layout
	cyHistLine = mainFontMetrics.tmHeight + 12;
	cyHist = cyHistLine * 10;
	y += boldFontMetrics.tmHeight + 8;
	rcHist ={ x, y, x + mainFontMetrics.tmAveCharWidth*60, y + cyHist };
	y += cyHist + 16;

	// create the scrollbar
	cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	sbHist = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
		0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_HIST), hInstance, 0);

	// scrollbar range calculation 
	auto GetRangeHist = [this](SCROLLINFO &si)
	{
		// figure the client area height
		int winHt = cyHist;

		// figure the document height
		int docHt = cyHistLine * static_cast<int>(irHist.size());

		// set the range
		si.nMin = 0;
		si.nMax = max(docHt - cyHistLine/2, 0);
		si.nPage = max(winHt - cyHistLine, 20);
	};

	// get the scroll rectangle
	auto GetScrollRectHist = [this](RECT *rc) {	*rc = rcHist; };

	// change the scroll position
	auto SetScrollPosHist = [this](int newPos, int deltaPos) { yScrollHist = newPos; };

	// set up the scrollbar object
	scrollbars.emplace_back(sbHist, SB_CTL, cyHistLine, true, true, GetRangeHist, GetScrollRectHist, SetScrollPosHist);

	// create a spin control
	auto CreateSpin = [this](int id, HWND txtCtl, int minVal, int maxVal) {
		return CreateUpDownControl(
			WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_HOTTRACK | UDS_SETBUDDYINT,
			0, 0, ControlWidth(16), ControlHeight(12), hwnd, id, hInstance, txtCtl, maxVal, minVal, 0);
	};
	transmitEditBox = CreateControl(ID_EDIT_TRANSMIT, WC_EDITA, "", ES_LEFT, 150, 12);
	repeatEditBox = CreateControl(ID_EDIT_REPEAT, WC_EDITA, "1", ES_LEFT | ES_NUMBER, 50, 12);
	repeatSpin = CreateSpin(ID_SPIN_REPEAT, repeatEditBox, 1, 20);
	transmitBtn = CreateControl(ID_BTN_TRANSMIT, WC_BUTTONA, "Transmit", 0, 50, 16);
	tvRelayPulseBtn = CreateControl(ID_BTN_RELAYPULSE, WC_BUTTONA, "Pulse Relay", 0, 50, 16);
	tvRelayOnBtn = CreateControl(ID_BTN_RELAYMANUAL, WC_BUTTONA, "Manual On", 0, 50, 16);

	// done with the window DC for now
	ReleaseDC(hwnd, hdc);

	// adjust the layout
	AdjustLayout();
}

void IRTesterWin::OnNCDestroy()
{
	// do the base class work
	__super::OnNCDestroy();
}

HMENU IRTesterWin::GetContextMenu(POINT pt, HWND &hwndCommand)
{
	// if it's in the IR scope area, show the IR scope menu
	if (PtInRect(&rcScope, pt))
		return ctxMenuScope;

	// if it's in the command list, show the command item menu
	if (PtInRect(&rcHist, pt))
	{
		// note the item selection
		selectedHistItem = nullptr;
		for (auto &it : irHist)
		{
			if (PtInRect(&it.rc, pt))
			{
				selectedHistItem = &it;
				break;
			}
		}

		// return the context menu
		return ctxMenuHist;
	}

	// use the default handling
	return __super::GetContextMenu(pt, hwndCommand);
}

void IRTesterWin::UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply)
{
	apply(ID_EDIT_COPYHISTITEM, selectedHistItem != nullptr);
	apply(ID_EDIT_TRANSMITHISTITEM, selectedHistItem != nullptr);
}

bool IRTesterWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
	switch (notifyCode)
	{
	case 0:
	case 1:
		// menu/accelerator/button click
		switch (ctlCmdId)
		{
        case ID_HELP_HELP:
            // show IR window help
			ShowHelpFile("IRRemote.htm");
			return true;

		case ID_BTN_TRANSMIT:
			// transmit the current edit box command
			ExecTransmit();
			return true;

		case ID_BTN_RELAYMANUAL:
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
				updaterThread->device->device->SetTVRelayManualState(!relayOnBtnState);
			return true;

		case ID_BTN_RELAYPULSE:
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
				updaterThread->device->device->PulseTVRelay();
			return true;

		case ID_IRSCOPE_CLEAR:
			// clear the scope raw pulse list
			irPulse.clear();
			return true;

		case ID_IRSCOPE_COPY:
			// copy the pulse data to the clipboard
			CopyScopeData();
			return true;

		case ID_IRSCOPE_SAVE:
			// save the scope raw pulse list
			SaveScopeData();
			return true;

		case ID_EDIT_CLEARCOMMANDLIST:
			ClearCommandList();
			return true;

		case ID_EDIT_COPYHISTITEM:
			if (selectedHistItem != nullptr)
				CopyHistoryItem(selectedHistItem);
			return true;

		case ID_EDIT_TRANSMITHISTITEM:
			if (selectedHistItem != nullptr)
				TransmitHistoryItem(selectedHistItem);
			return true;
		}
		break;
	}

	// use the base handling
	return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

LRESULT IRTesterWin::ControlSubclassProc(
	HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass)
{
	// treat a return key in the transmit text boxes as a Transmit button click
	if (msg == WM_CHAR && (hwnd == transmitEditBox || hwnd == repeatEditBox))
	{
		if (wparam == '\n' || wparam == '\r')
		{
			// enter key -> click on transmit button
			ExecTransmit();
			return 0;
		}
	}

	// use the default handling
	return __super::ControlSubclassProc(hwnd, msg, wparam, lparam, idSubclass);
}


bool IRTesterWin::OnLButtonDown(WPARAM keys, int x, int y)
{
	// search for a click in a history item
	POINT pt{ x, y };
	if (PtInRect(&rcHist, pt))
	{
		// search the in-view items for a hit on one of the item buttons
		for (decltype(firstHistInView) it = firstHistInView ;
			it != irHist.end() && it->rcCopy.top < rcHist.bottom ; ++it)
		{
			auto &h = *it;
			if (PtInRect(&h.rcCopy, pt))
			{
				// copy button - copy the IR command to the clipboard as a string
				CopyHistoryItem(&h);
				return true;
			}
			else if (PtInRect(&h.rcTransmit, pt))
			{
				// transmit button - populate the transmit box with the code, and
				// execute a transmit command
				TransmitHistoryItem(&h);
				return true;
			}
		}
	}

	// use the inherited handling
	return __super::OnLButtonDown(keys, x, y);
}

void IRTesterWin::CopyHistoryItem(IRHistItem *item)
{
	std::string s = item->command.ToString();
	CopyTextToClipboard(s);
	gApp.AddTimedStatusMessage(StrPrintf("%s copied to clipboard", s.c_str()).c_str(), HRGB(0xFFFFFF), HRGB(0x008000), 2500);
}

void IRTesterWin::TransmitHistoryItem(IRHistItem *item)
{
	SetWindowTextA(transmitEditBox, item->command.ToString().c_str());
	SetWindowTextA(repeatEditBox, "1");
	ExecTransmit();
}

void IRTesterWin::ExecTransmit()
{
	// get the code
	char code[128];
	GetWindowTextA(transmitEditBox, code, _countof(code));

	// if the code is empty, do nothing
	std::regex blank("\\s*");
	if (std::regex_match(code, blank))
		return;

	// Get the repeat count.  Limit it to 1-20 repeats.  The upper bound
	// is arbitrary; it's just to prevent inadvertently queueing a huge
	// repeat count that ties up the transmitter for a long time.
	char repeatTxt[32];
	GetWindowTextA(repeatEditBox, repeatTxt, _countof(repeatTxt));
	int repeatCnt = atoi(repeatTxt);
	repeatCnt = (repeatCnt < 1 ? 1 : repeatCnt > 20 ? 20 : repeatCnt);

	// parse it
	PinscapePico::IRCommand cmd;
	if (!cmd.Parse(code))
	{
		transmitStatus.SetError(
			"Invalid IR command format.  The command must use the format XX.XX.XXXXXXXX,\n"
			"where each X represents a hexadecimal digit (0-9 and A-F).  The final section can\n"
			"be from 4 to 16 hex digits long.  Codes should be determined using the receiver.\n"
			"Note that codes taken from other sources (such as IR database Web sites) usually\n"
			"use different formats that Pinscape doesn't recognize.");
		return;
	}

	// execute
	auto &dev = updaterThread->device;
	int stat = PinscapeResponse::ERR_FAILED;
	if (VendorInterface::Shared::Locker l(dev); l.locked)
		stat = dev->device->SendIRCommand(cmd, repeatCnt);

	// check the status
	if (stat == PinscapeResponse::OK)
	{
		transmitStatus.SetOK("Command transmitted OK");
	}
	else
	{
		char buf[256];
		sprintf_s(buf, "Command transmission failed: %s (code %d)", VendorInterface::ErrorText(stat), stat);
		transmitStatus.SetError(buf);
	}
}

void IRTesterWin::ClearCommandList()
{
	selectedHistItem = nullptr;
	irHist.clear();
}

void IRTesterWin::CopyScopeData()
{
	// generate text in various formats
	std::stringstream csv;
	FormatScopeDataCSV(csv);

	std::stringstream txt;
	FormatScopeDataText(txt);

	// add the formats to the clipboard
	if (OpenClipboard(hwnd))
	{
		// remove previous contents
		EmptyClipboard();

		// add an item
		using Formatter = void (IRTesterWin::*)(std::ostream &);
		auto Add = [this](Formatter formatter, UINT fmtCode)
		{
			// format the text to a string stream
			std::stringstream s;
			(this->*formatter)(s);

			// copy it into an HGLOBAL for the clipboard
			if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, s.str().size() + 1); hg != NULL)
			{
				// lock the global
				if (void *hgp = GlobalLock(hg); hgp != nullptr)
				{
					// copy the string data
					memcpy(hgp, s.str().data(), s.str().size() + 1);

					// done with the lock
					GlobalUnlock(hg);

					// add it to the clipboard
					if (SetClipboardData(fmtCode, hg) != NULL)
						hg = NULL;
				}

				// if we didn't transfer the HGLOBAL to the clipboard, delete it
				if (hg != NULL)
					GlobalFree(hg);
			}
		};

		// add each format
		Add(&IRTesterWin::FormatScopeDataText, CF_TEXT);
		Add(&IRTesterWin::FormatScopeDataCSV, RegisterClipboardFormatA("CSV"));
		Add(&IRTesterWin::FormatScopeDataCFHTML, RegisterClipboardFormatA("HTML Format"));

		// done with the clipboard
		CloseClipboard();
	}
}

void IRTesterWin::SaveScopeData()
{
	GetFileNameDlg d(_T("Save IR raw pulse data to file"), 0,
		_T("Text Files\0*.txt\0HTML Documents\0*.htm;*.html\0CSV Files\0*.csv\0All Files\0*.*"), _T("txt"));
	if (d.Save(GetDialogOwner()))
	{
		// open the file
		std::fstream f(d.GetFilename(), std::ios::out | std::ios::trunc);

		// search for a format based on the filename
		struct Format
		{
			const TCHAR *filePat;
			void (IRTesterWin::*func)(std::ostream&);
		};
		Format formats[]{
			{ _T(".*\\.csv$"), &IRTesterWin::FormatScopeDataCSV },
			{ _T(".*\\.html?$"), &IRTesterWin::FormatScopeDataHTMLFile },
			{ _T(".*"), &IRTesterWin::FormatScopeDataText },
		};
		for (const auto &format : formats)
		{
			if (std::regex_match(d.GetFilename(), std::basic_regex<TCHAR>(format.filePat, std::regex_constants::icase)))
			{
				(this->*format.func)(f);
				break;
			}
		}

		// done with the file - close it and check that the write was successful
		f.close();
		if (f)
			MessageBoxFmt(hwnd, MB_ICONINFORMATION, "Success - Pulse timing data written to %" _TSFMT, d.GetFilename());
		else
			MessageBoxFmt(hwnd, "An error occurred writing the file.");
	}
}

void IRTesterWin::FormatScopeDataCSV(std::ostream &os)
{
	// CSV format
	os << "PulseTime_us,Type,Value\n";
	for (auto &p : irPulse)
		os << (p.t < 0 ? 131068 : p.t) << "," << (p.mark ? "Mark,1\n" : "Space,0\n");
}

void IRTesterWin::FormatScopeDataHTML(std::ostream &os)
{
	// HTML fragment format - a table containing the pulse time and type data
	os << "<table>\r\n";
	os << "   <tr><th>Pulse Time(us)</th><th>Type</th></tr>\r\n";
	for (auto &p : irPulse)
	{
		os << "   <tr><td>" ;
		if (p.t < 0) os << "&gt;131068"; else os << p.t;
		os << "</td><td>" << (p.mark ? "Mark" : "Space") << "</td></tr>\r\n";
	}
	os << "</table>\r\n";
}

void IRTesterWin::FormatScopeDataCFHTML(std::ostream &os)
{
	// clipboard format HTML - requires a special header, followed by the HTML
	// document contents

	// start by formatting the contents into a string
	std::stringstream ss;
	FormatScopeDataHTML(ss);

	// build a template header
	std::string hdr = 
		"Version:0.9\r\n"
		"StartHTML:00000000\r\n"
		"EndHTML:00000000\r\n"
		"StartFragment:00000000\r\n"
		"EndFragment:00000000\r\n";

	// fix up the header elements
	auto Fix = [&hdr](const char *key, size_t val)
	{
		char buf[10];
		sprintf_s(buf, "%08u", static_cast<unsigned int>(val));
		char *p = strstr(hdr.data(), key);
		if (p != nullptr)
			memcpy(p + strlen(key) + 1, buf, 8);
	};
	Fix("StartHTML", hdr.size());
	Fix("EndHTML", hdr.size() + ss.str().size());
	Fix("StartFragment", hdr.size());
	Fix("EndFragment", hdr.size() + ss.str().size());

	// write the final stream: header + contents
	os << hdr;
	os << ss.str();
}

void IRTesterWin::FormatScopeDataHTMLFile(std::ostream &os)
{
	// HTML file format - a complete HTML file with header

	// header
	os << "<!DOCTYPE html>\r\n"
		"<html>\n<head>\r\n"
		"   <title>Pinscape IR Pulse Data</title>\r\n"
		"</head>\r\n"
		"<body>\r\n";

	// add the body
	FormatScopeDataHTML(os);

	// footer
	os << "</body>\r\n"
		"</html>\r\n";

	
}

void IRTesterWin::FormatScopeDataText(std::ostream &os)
{
	os << "Time(us)\tType\n";
	os << "========\t=====\n\n";

	for (auto &p : irPulse)
	{
		if (p.t < 0)
			os << ">131067";
		else
			os << p.t;

		os << "\t\t" << (p.mark ? "Mark" : "Space") << "\n";
	}
}


// Updater thread
bool IRTesterWin::UpdaterThread::Update(bool &releasedMutex)
{
	// query IR commands
	std::vector<IRCommandReceived> cmds;
	int cmdStat = device->device->QueryIRCommandsReceived(cmds);
	
	// query raw IR pulses
	std::vector<VendorInterface::IRRawPulse> pulses;
	int pulseStat = device->device->QueryIRRawPulsesReceived(pulses);

	// read the TV ON status
	VendorInterface::TVONState tv;
	int tvOnStat = device->device->QueryTVONState(tv);

	// done with the mutex
	ReleaseMutex(device->mutex);
	releasedMutex = true;

	// if we have new items, append them to the list
	if ((cmdStat == PinscapeResponse::OK && cmds.size() != 0)
		|| (pulseStat == PinscapeResponse::OK && pulses.size() != 0)
		|| (tvOnStat == PinscapeResponse::OK))
	{
		// acquire the data lock
		if (WaitForSingleObject(dataMutex, 100) == WAIT_OBJECT_0)
		{
			// queue the new commands
			if (cmdStat == PinscapeResponse::OK)
			{
				for (size_t i = 0 ; i < cmds.size() ; ++i)
					irReports.emplace_back(cmds[i]);
			}

			// queue new raw pulses
			if (pulseStat == PinscapeResponse::OK)
			{
				for (size_t i = 0 ; i < pulses.size() ; ++i)
					irRaw.emplace_back(pulses[i]);
			}

			// set the new TV ON status
			tvOnState = tv;

			// done with the data lock
			ReleaseMutex(dataMutex);
		}
	}

	// success
	return true;
}
