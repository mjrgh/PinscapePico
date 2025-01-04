// Open Pinball Device Viewer - device viewer window
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
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include <Dbt.h>
#include "WinUtil.h"
#include "Utilities.h"
#include "ViewerWin.h"
#include "../OpenPinballDeviceReport.h"
#include "../OpenPinballDeviceLib/OpenPinballDeviceLib.h"
#include "resource.h"

#pragma comment(lib, "OpenPinballDeviceLib")

ViewerWin::ViewerWin(HINSTANCE hInstance, const OpenPinballDevice::DeviceDesc &deviceDesc) : BaseWindow(hInstance)
{
	// load the nudge crosshairs image
	crosshairs = LoadPNG(hInstance, IDB_NUDGE_CROSSHAIRS);
	BITMAP bmp{ 0 };
	GetObject(crosshairs, sizeof(bmp), &bmp);
	szCrosshairs = { bmp.bmWidth, bmp.bmHeight };

	// load the generic numbered button image
	bmpBtnOn = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_BTN_ON));
	bmpBtnOff = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_BTN_OFF));
	GetObject(bmpBtnOn, sizeof(bmp), &bmp);
	szBtn = { bmp.bmWidth, bmp.bmHeight };

	// Load the named pinball buttons image.  This consists of two
	// rows of square cells, with the ON image for a given button in
	// the top row and the OFF image in the bottom row.  The buttons
	// are arranged in the same order as the pinball button bits in
	// the report.  Since the image is two rows high, and the cells
	// are square, the width and height of each cell equal half the
	// image height.
	bmpPinballBtns = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_PINBALL_BUTTONS));
	GetObject(bmpPinballBtns, sizeof(bmp), &bmp);
	szPinballBtn = { bmp.bmHeight/2, bmp.bmHeight/2 };

	// open the reader
	device.reset(OpenPinballDevice::Reader::Open(deviceDesc));

	// load fonts
	barFont = CreateFontA(14,
		0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
		"Segoe UI");
}

ViewerWin::~ViewerWin()
{
	// clean up GDI resources
	DeleteBitmap(crosshairs);
	DeleteFont(barFont);

	// close our device notification handle
	if (deviceNotificationHandle != NULL)
		UnregisterDeviceNotification(deviceNotificationHandle);
}

LRESULT ViewerWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
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

void ViewerWin::OnDeviceChange(WPARAM wparam, LPARAM lparam)
{
	if (wparam == DBT_DEVICEREMOVECOMPLETE)
	{
		// device removed - if it's our device, close the window
		if (reinterpret_cast<DEV_BROADCAST_HDR*>(lparam)->dbch_devicetype == DBT_DEVTYP_HANDLE
			&& device != nullptr
			&& reinterpret_cast<DEV_BROADCAST_HANDLE*>(lparam)->dbch_handle == reinterpret_cast<HANDLE>(device->GetNativeHandle()))
		{
			// immediately release the device, to close the handle
			device.reset();

			// unregister our notification handle
			UnregisterDeviceNotification(deviceNotificationHandle);
			deviceNotificationHandle = NULL;

			// Close the window, deferred to a queued message so that we
			// don't slow down processing the removal notification.  (The
			// notification handler is supposed to be as fast as possible
			// to avoid stalling processing at the device manager level.)
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		}
	}
}



// window creation
void ViewerWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

	// create a tooltip control
	CreateTooltipControl();

	// register for device notifications
	if (device != nullptr)
	{
		DEV_BROADCAST_HANDLE dbh{ sizeof(dbh), DBT_DEVTYP_HANDLE, 0, reinterpret_cast<HANDLE>(device->GetNativeHandle()) };
		deviceNotificationHandle = RegisterDeviceNotification(hwnd, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
	}
}

// Paint off-screen
void ViewerWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// fill the background
	HDCHelper hdc(hdc0);
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

	// margins
	int xMargin = 16;
	int yMargin = 16;

	// separator bar
	int cxSep = crc.right - crc.left - 32;
	auto DrawSep = [&hdc, &cxSep](int x, int y)
	{
		HPen sepPen(HRGB(0xf0f0f0), 1);
		HPEN oldPen = SelectPen(hdc, sepPen);
		MoveToEx(hdc, x, y, NULL);
		LineTo(hdc, x + cxSep, y);
		SelectPen(hdc, oldPen);
	};

	// axis drawing
	auto DrawAxes = [&hdc, this](const RECT &rc, int centerXOfs, int centerYOfs, COLORREF color,
		const char *xLbl, const char *yLbl, int quadrant)
	{
		HPen pen(color, 1);
		HPEN oldPen = SelectPen(hdc, pen);

		MoveToEx(hdc, rc.left, rc.top + centerYOfs, NULL);
		LineTo(hdc, rc.right, rc.top + centerYOfs);

		MoveToEx(hdc, rc.left + centerXOfs, rc.top, NULL);
		LineTo(hdc, rc.left + centerXOfs, rc.bottom);

		SelectPen(hdc, oldPen);

		// quadrant: 0=top left, 1=top right, 2=bottom left, 3 = bottom right
		int cyx = hdc.MeasureText(mainFont, xLbl).cy;
		int cxy = hdc.MeasureText(mainFont, yLbl).cx;
		if (quadrant == 0 || quadrant == 1)
		{
			// top
			hdc.DrawText(rc.left + centerXOfs - cxy/2, rc.top - cyx - 4, 1, mainFont, color, yLbl);
		}
		else
		{
			// bottom
			int cxy = hdc.MeasureText(mainFont, yLbl).cx;
			hdc.DrawText(rc.left + centerXOfs - cxy/2, rc.bottom + 4, 1, mainFont, color, yLbl);
		}
		if (quadrant == 0 || quadrant == 2)
		{
			// left
			hdc.DrawText(rc.left - 4, rc.top + centerYOfs - cyx/2, -1, mainFont, color, xLbl);
		}
		else
		{
			// right
			int cyx = hdc.MeasureText(mainFont, xLbl).cy;
			hdc.DrawText(rc.right + 4, rc.top + centerYOfs - cyx/2, 1, mainFont, color, xLbl);
		}
	};

	// label X/Y axes
	auto LabelXY = [&hdc, this](const RECT &rcPlot, int xVal, int yVal)
	{
		int yPos = rcPlot.bottom + 8;
		int xCenter = (rcPlot.left + rcPlot.right)/2;

		int cxNum = mainFontMetrics.tmAveCharWidth * 7;
		hdc.DrawText(xCenter - 8 - cxNum, yPos, -1, boldFont, HRGB(0x0000FF), "x:");
		hdc.DrawTextF(xCenter - 8, yPos, -1, boldFont, HRGB(0x0000FF), "% 5d", xVal);
		int cx = hdc.DrawText(xCenter + 8, yPos, 1, boldFont, HRGB(0x0000FF), "y:").cx;
		return hdc.DrawTextF(xCenter + 8 + cx + cxNum, yPos, -1, boldFont, HRGB(0x0000FF), "% 5d", yVal);
	};
	
	// Draw a bar label
	// 
	// dir = 
	//   +1 to draw right of origin
	//   -1 to draw left of origin
	//   +2 to draw right of origin, but move left if it doesn't fit
	//   -2 to draw left of origin, but move right if it doesn't fit
	int xBar = xMargin;
	const int barWidth = max(400, crc.right - crc.left - xMargin*2);
	const int barHeight = 16;
	auto DrawBarLabel = [this, xBar, barWidth, &hdc, &crc](int xOrigin, int yTop, COLORREF color, int dir, const char *str)
	{
		// figure the text size
		SelectFont(hdc, barFont);
		SetTextColor(hdc, color);
		SIZE sz = hdc.MeasureText(barFont, str);
		int yText = yTop + (barHeight - sz.cy)/2;
		int xMargin = 4;

		// for +2/-2, switch sides if it doesn't fit on the chosen side
		int cxText = sz.cx + xMargin;
		if (dir == 2 && xOrigin + cxText > xBar + barWidth && xOrigin - cxText >= xBar)
			dir = -1;
		else if (dir == -2 && xOrigin - cxText < xBar && xOrigin + cxText <= xBar + barWidth)
			dir = 1;

		// draw left or right of the X origin
		if (dir < 0)
		{
			// draw left of the origin (right-align at the origin)
			int limit = xBar + sz.cx + xMargin;
			RECT rc{ crc.left, yText, max(xOrigin - xMargin, limit), yText + sz.cy };
			DrawTextA(hdc, str, -1, &rc, DT_TOP | DT_RIGHT);
		}
		else
		{
			// draw right of the origin (left-align at the origin)
			int limit = xBar + barWidth - sz.cx - xMargin;
			RECT rc{ min(xOrigin + xMargin, limit), yText, crc.right, yText + sz.cy };
			DrawTextA(hdc, str, -1, &rc, DT_TOP | DT_LEFT);
		}
	};
	auto DrawBarLabelV = [DrawBarLabel](int xOrigin, int yTop, COLORREF color, int dir, const char *fmt, va_list va)
	{
		// format the text
		char buf[128];
		vsprintf_s(buf, fmt, va);

		// draw the label
		DrawBarLabel(xOrigin, yTop, color, dir, buf);
	};
	auto DrawBarLabelF =[DrawBarLabelV](int xOrigin, int yTop, COLORREF color, int dir, const char *fmt, ...)
	{
		va_list va;
		va_start(va, fmt);
		DrawBarLabelV(xOrigin, yTop, color, dir, fmt, va);
		va_end(va);
	};

	// Draw a bar.  Set xLabel to -1 for automatic label positioning; otherwise,
	// this gives the origin of the label, on the side indicated by labelDir.
	// labelDir > 0 -> label to the right of origin, else left of origin.
	auto DrawBar = [this, xBar, &hdc, DrawBarLabel, DrawBarLabelV]
	(int origin, int width, int y, const HBrush &br, int xLabel, int labelDir, const char *label, ...)
	{
		// set up varargs
		va_list va;
		va_start(va, label);

		// draw the bar
		int x0 = origin + xBar;
		int x1 = origin + xBar + width;
		RECT rc{ min(x0, x1), y, max(x0, x1), y + barHeight };
		if (width != 0)
			FillRect(hdc, &rc, br);

		// draw the label, if provided
		if (label != nullptr)
		{
			// check for an explicit label position
			if (xLabel >= 0)
			{
				// explicit label positioning
				DrawBarLabelV(xBar + xLabel, y, br.color, labelDir, label, va);
			}
			else
			{
				// automatic label positioning - figure out where there's room
				// for the label, favoring inside the bar

				// format the text
				char buf[128];
				vsprintf_s(buf, label, va);

				// figure the text size
				SIZE sz = hdc.MeasureText(barFont, buf);
				int yText = y + (barHeight - sz.cy)/2;
				int xMargin = 4;

				// check if there's room inside the bar
				if (abs(width) >= sz.cx + xMargin*2)
				{
					// There's room inside the bar - draw white text.  The bar extends
					// to the left if the origin is zero, so the end of the bar is at
					// the right, and the text goes to the left (-1).
					DrawBarLabel(x1, y, RGB(0xff, 0xff, 0xff), origin == 0 ? -1 : 1, buf);
				}
				else
				{
					// no room inside the bar -> draw outside, in the bar color
					DrawBarLabel(x1, y, br.color, origin == 0 ? 1 : -1, buf);
				}
			}
		}

		// done with the argument list
		va_end(va);
	};

	// draw an arrow marker
	const int markerLen = 10;
	auto DrawMarker = [hdc, xBar](int x, int y, COLORREF color)
	{
		// trace out the triangle path
		BeginPath(hdc);
		MoveToEx(hdc, xBar + x, y, NULL);
		LineTo(hdc, xBar + x + markerLen/2, y + markerLen);
		LineTo(hdc, xBar + x - markerLen/2, y + markerLen);
		EndPath(hdc);

		// fill with the selected color
		HBrush markerBrush(color);
		HBRUSH oldBr = SelectBrush(hdc, markerBrush);
		FillPath(hdc);
		SelectBrush(hdc, oldBr);
	};


	// If we have a HID device, read a report
	if (device != nullptr)
	{
		// read a report
		OpenPinballDeviceReport report{ 0 };
		device->Read(report);

		// ------------------------------------------------------------------------
		//
		// Nudge Acceleration visualization
		//
		int x = xMargin;
		int y = yMargin + mainFontMetrics.tmHeight;
		int cxPlot = 256, cyPlot = cxPlot;
		RECT rcPlot{ x, y + mainFontMetrics.tmHeight, x + cxPlot, y + cyPlot + mainFontMetrics.tmHeight };
		hdc.DrawText(POINT{ x + cxPlot/2, y }, POINT{ 0, 4 }, POINT{ 0, -1 },
			boldFont, HRGB(0x0000FF), "Nudge Acceleration");

		// draw the main axes
		DrawAxes(rcPlot, cxPlot/2, cyPlot/2, HRGB(0x0000FF), "X", "Y", 1);

		// plot the current reading
		CompatibleDC bdc(hdc);
		bdc.Select(crosshairs);
		BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		AlphaBlend(hdc,
			rcPlot.left + cxPlot/2 + report.axNudge * cxPlot / 65536 - szCrosshairs.cx/2, 
			rcPlot.top + cyPlot/2 - report.ayNudge * cyPlot / 65536 - szCrosshairs.cy/2,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically at the bottom
		int yLabel = rcPlot.bottom + 8;
		int yPlotBottom = yLabel + LabelXY(rcPlot, report.axNudge, report.ayNudge).cy;

		// ------------------------------------------------------------------------
		//
		// Nudge Velocity visualization
		//
		int xVel = rcPlot.right + mainFontMetrics.tmAveCharWidth*12;
		int cxPlotVel = cxPlot;
		RECT rcPlotVel{ xVel, rcPlot.top, xVel + cxPlotVel, rcPlot.bottom };

		hdc.DrawText(POINT{ xVel + cxPlotVel/2, y }, POINT{ 0, 4 }, POINT{ 0,-1 }, 
			boldFont, HRGB(0x0000FF), "Nudge Velocity");

		DrawAxes(rcPlotVel, cxPlotVel/2, cyPlot/2, HRGB(0x0000FF), "X", "Y", 1);

		AlphaBlend(hdc,
			xVel + cxPlotVel/2 - szCrosshairs.cx/2 + report.vxNudge * cxPlotVel / 65536,
			rcPlotVel.top + cyPlot/2 - szCrosshairs.cy/2 - report.vyNudge * cyPlot / 65536,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically at the bottom
		LabelXY(rcPlotVel, report.vxNudge, report.vyNudge);

		// ------------------------------------------------------------------------
		//
		// Plunger visualization
		// 

		// separator
		y = yPlotBottom + 8;
		DrawSep(xMargin, y);
		y += 8;

		// label it
		x = xMargin;
		y += 12;
		y += hdc.DrawText(x, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Plunger Position").cy;

		// place the zero point at 1/6 from left
		int screenZero = barWidth/6;

		// Draw the Z axis bar, extending left from the right end
		int screenZ = static_cast<int>(((32767 - report.plungerPos) / 32767.0) * (barWidth - screenZero));
		HBrush zBrush(RGB(0x00, 0x00, 0xD0));
		DrawBar(barWidth, -screenZ, y, zBrush, -1, 0, "Position=%d", report.plungerPos);
		y += barHeight;

		// Frame the position area
		RECT rcBar ={ xBar, y - barHeight, xBar + barWidth, y };
		FrameRect(hdc, &rcBar, zBrush);

		// draw the zero marker (again - this time for the Z axis view)
		COLORREF zeroMarkerColor = HRGB(0x008000);
		DrawMarker(screenZero, y, zeroMarkerColor);
		y += hdc.DrawText(xBar + screenZero, y + markerLen, 0, barFont, zeroMarkerColor, "0 (Rest Position)").cy;

		//
		// Speedometer section
		//
		y += 12;
		y += hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Plunger Speed").cy;

		// Check for a new peak forward speed; forget the old one after
		// a few seconds
		UINT64 now = GetTickCount64();
		if (report.plungerSpeed < 0 && report.plungerSpeed < peakForwardSpeed)
		{
			peakForwardSpeed = report.plungerSpeed;
			tPeakForwardSpeed = now;
		}
		else if (now - tPeakForwardSpeed > 3000)
		{
			peakForwardSpeed = report.plungerSpeed < 0 ? report.plungerSpeed : 0;
			tPeakForwardSpeed = now;
		}

		// Draw the peak speed bar
		HBrush peakBrush(RGB(0xFF, 0xE0, 0xE0));
		HBrush speedBrush(RGB(0xFF, 0x00, 0x00));
		int screenPeak = static_cast<int>((peakForwardSpeed / 32767.0) * barWidth/2);
		DrawBar(barWidth/2, screenPeak, y, peakBrush, barWidth/2 + screenPeak, -1, nullptr);
		DrawBarLabelF(xBar + barWidth/2 + screenPeak, y, speedBrush.color, -1, "Peak: %d", peakForwardSpeed);

		// Draw the speed bar
		int screenSpeed = static_cast<int>((report.plungerSpeed / 32767.0) * barWidth/2);
		DrawBar(barWidth/2, screenSpeed, y, speedBrush, barWidth/2 + max(screenSpeed, 0), 1, "Speed: %d", report.plungerSpeed);

		// frame the speed bar
		rcBar ={ xBar, y, xBar + barWidth, y + barHeight };
		FrameRect(hdc, &rcBar, speedBrush);
		y += barHeight;


		// ------------------------------------------------------------------------
		// 
		// Buttons section
		// 
		

		// separator
		y += 8;
		DrawSep(xMargin, y);
		y += 8;

		//
		// Generic buttons
		// 
		const char *caption = " Generic Buttons  ";
		SIZE szCaption = hdc.MeasureText(boldFont, caption);
		x = xMargin;
		int btnSpacing = 4;
		int btnMargin = 8;
		int cxBtns = szBtn.cx*8 + btnSpacing*7 + btnMargin*2;
		int cyBtns = szBtn.cy*4 + btnSpacing*3 + btnMargin*2 + szCaption.cy;
		RECT frc = { x, y + szCaption.cy/2, x + cxBtns, y + cyBtns };
		FrameRect(hdc, &frc, HBrush(RGB(0xf0, 0xf0, 0xf0)));
		SetBkMode(hdc, OPAQUE);
		hdc.DrawText((frc.right + frc.left - szCaption.cx)/2, y, 1, boldFont, HRGB(0x202020), caption);
		SetBkMode(hdc, TRANSPARENT);
		int y0 = y + szCaption.cy + btnMargin;
		int x0 = x + btnMargin;
		for (int i = 0, bit = 1, bx = x0, by = y0 ; i < 32 ; ++i, bit <<= 1)
		{
			// draw the button circle
			bool on = ((report.genericButtons & bit) != 0);
			bdc.Select(on ? bmpBtnOn : bmpBtnOff);
			BitBlt(hdc, bx, by, szBtn.cx, szBtn.cy, bdc, 0, 0, SRCCOPY);

			// label it
			char label[5];
			sprintf_s(label, "%d", i + 1);
			SIZE sz = hdc.MeasureText(boldFont, label);
			hdc.DrawText(bx + (szBtn.cx - sz.cx)/2, by + (szBtn.cy - sz.cy)/2 - 1, 1, boldFont, RGB(0xFF, 0xFF, 0xFF), label);

			// advance to the next row every 8 buttons
			if ((i % 8) == 7)
			{
				bx = x0;
				by += szBtn.cy + btnSpacing;
			}
			else
			{
				bx += szBtn.cx + btnSpacing;
			}
		}

		// Draw the named buttons.  These have the same basic layout as the
		// generic buttons.
		caption = " Pinball Buttons ";
		szCaption = hdc.MeasureText(boldFont, caption);
		x += cxBtns + xMargin;
		frc = { x, y + szCaption.cy/2, x + cxBtns, y + cyBtns };
		FrameRect(hdc, &frc, HBrush(RGB(0xf0, 0xf0, 0xf0)));
		SetBkMode(hdc, OPAQUE);
		hdc.DrawText((frc.right + frc.left - szCaption.cx)/2, y, 1, boldFont, HRGB(0x202020), caption);
		SetBkMode(hdc, TRANSPARENT);
		x0 = x + 2;
		y0 = y + szCaption.cy + btnMargin;
		bdc.Select(bmpPinballBtns);
		for (int i = 0, bit = 1, bx = x0, by = y0 ; i < 27 ; ++i, bit <<= 1)
		{
			// Get the button state
			bool on = (report.pinballButtons & bit) != 0;

			// draw the button circle; the bitmap's top row is ON, bottom row is OFF
			BitBlt(hdc, bx, by, szPinballBtn.cx, szPinballBtn.cy, bdc, i*szPinballBtn.cx, on ? 0 : szPinballBtn.cy, SRCCOPY);

			// set the tooltip on the first layout pass
			if (layoutPending)
			{
				static const char *buttonName[] = {
					"Start", "Exit", "Extra Ball", "Coin 1", "Coin 2", "Coin 3", "Coin 4", "Launch Ball", 
					"Lockbar Fire", "Left Flipper", "Right Flipper", "Left Flipper 2", "Right Flipper 2", "Left MagnaSave", "Right MagnaSave", "Tilt Bob",
					"Slam Tilt", "Coin Door", "Service Cancel", "Service Down", "Service Up", "Service Enter", "Left Nudge", "Forward Nudge",
					"Right Nudge", "Vol Up", "Vol Down"
				};

				RECT rctt{ bx, by, bx + szPinballBtn.cx, by + szPinballBtn.cy };
				SetTooltip(rctt, i + 100, buttonName[i]);
			}

			// advance to the next row every 8 buttons
			if ((i % 8) == 7)
			{
				bx = x0;
				by += szBtn.cy + btnSpacing;
			}
			else
			{
				bx += szBtn.cx + btnSpacing;
			}
		}
	}
	else
	{
		// device not available - just show a message instead of the data visualizations
		hdc.DrawText({ crc.right/2, crc.bottom/2 }, { 0, 0 }, { 0, 0 }, boldFont, HRGB(0x000000),
			"This device isn't currently available");
	}

	// layout completed
	layoutPending = false;
}
