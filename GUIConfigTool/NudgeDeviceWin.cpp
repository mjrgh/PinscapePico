// Pinscape Pico - Config Tool - Nudge Device Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

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
#include "NudgeDeviceWin.h"
#include "WinUtil.h"
#include "Utilities.h"
#include "Application.h"
#include "resource.h"

// the window class is part of the PinscapePico namespace
using namespace PinscapePico;

NudgeDeviceWin::NudgeDeviceWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
	DeviceThreadWindow(hInstance, device, new UpdaterThread())
{
	// Load my menu bar and accelerator
	hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_NUDGEWIN));
	hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_NUDGEWIN));
    hCtxMenu = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_NUDGEWIN));

	// query device information
	QueryDeviceInfo();
	
	// load our bitmaps
	crosshairs = LoadPNG(hInstance, IDB_NUDGE_CROSSHAIRS);

	BITMAP bmp{ 0 };
	GetObject(crosshairs, sizeof(bmp), &bmp);
	szCrosshairs ={ bmp.bmWidth, bmp.bmHeight };
}

NudgeDeviceWin::~NudgeDeviceWin()
{
	// clean up GDI resources
	DeleteBitmap(crosshairs);
	DeleteFont(simDataFont);

	// Before we exit, revert in-memory settings on the device to the
	// last saved settings.  This provides a more document-like model for
	// the user, by making the settings in the window act like the working
	// copy of a document in a traditional Windows program.  When closing
	// a window with unsaved changes in a traditional application, the
	// unsaved changes are discarded (after asking the user to confirm
	// that this is okay, which we also do here).  
	auto &dev = updaterThread->device;
	if (VendorInterface::Shared::Locker l(dev); l.locked)
		dev->device->RevertNudgeSettings();
}

bool NudgeDeviceWin::OnDeviceReconnect()
{
	// re-query the device information
	QueryDeviceInfo();

	// tell the container that we've refreshed, so it can keep the window open
	return true;
}

void NudgeDeviceWin::OnEraseDeviceConfig(bool factoryReset)
{
	// re-query the device information
	QueryDeviceInfo();
}

void NudgeDeviceWin::QueryDeviceInfo()
{
	// reload nudge parameters, overwriting the UI contents unconditionally
	ReloadNudgeParams(true);
}

bool NudgeDeviceWin::InstallParentMenuBar(HWND hwndContainer)
{
	SetMenu(hwndContainer, hMenuBar);
	return true;
}

// translate keyboard accelerators
bool NudgeDeviceWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
	return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

// Paint off-screen
void NudgeDeviceWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// fill the background
	HDCHelper hdc(hdc0);
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

	int cxSep = crc.right - crc.left - 32;
	auto DrawSep = [&hdc, &cxSep](int x, int y)
	{
		HPen sepPen(HRGB(0xf0f0f0), 1);
		HPEN oldPen = SelectPen(hdc, sepPen);
		MoveToEx(hdc, x, y, NULL);
		LineTo(hdc, x + cxSep, y);
		SelectPen(hdc, oldPen);
	};

	// if we're tracking a ball, synthesize a mouse move even if the mouse
	// hasn't moved, to hold the ball at the current location as long as the
	// button is pressed
	if (trackingSimBall != nullptr && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
	{
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		OnSimMouseEvent(WM_MOUSEMOVE, 0, pt.x, pt.y);
	}

	// refresh the nudge parameters from the device periodically
	if (GetTickCount64() > tNudgeParamCheck + 1000)
	{
		ReloadNudgeParams(false);
		tNudgeParamCheck = GetTickCount64();
	}

	// get a local copy of the data for drawing the update
	if (WaitForSingleObject(updaterThread->dataMutex, 100) == WAIT_OBJECT_0)
	{
		// get the updater thread object, downcast to our subclass type
		auto *ut = static_cast<UpdaterThread*>(updaterThread.get());

		// get a local copy of the nudge data
		PinscapePico::NudgeStatus s = ut->nudgeStatus;

		// get a local copy of the mini pinball model ball and flipper states
		// (everything else is static, so we can go straight to the source
		// for the rest)
		auto balls = ut->pinModel.GetBallSnapshot();
		auto flippers = ut->pinModel.GetFlipperSnapshot();

		// done with the data mutex
		ReleaseMutex(ut->dataMutex);

		// update our internal copy of the modified flag
		isSettingModified = ((s.flags & s.F_MODIFIED) != 0);

		// edit controls
		RECT rcEdit = GetChildControlRect(dcTimeEdit);
		int dyEdits = rcEdit.bottom - rcEdit.top + 10;
		auto DrawEditBase = [&rcEdit, dyEdits, &hdc, this](HWND edit, const char *label, bool alignOnLabel = false)
		{
			// if desired, align on the label, by shifting the control right
			// by the label size
			if (alignOnLabel)
				OffsetRect(&rcEdit, hdc.MeasureText(mainFont, label).cx + 8, 0);

			// frame it
			RECT frc = rcEdit;
			InflateRect(&frc, 1, 2);
			FrameRect(hdc, &frc, HBrush(HRGB(0xD0D0D0)));

			// label it
			hdc.DrawText(rcEdit.left - 8, (rcEdit.top + rcEdit.bottom - mainFontMetrics.tmHeight)/2, -1,
				mainFont, HRGB(0x000000), label);

			// move the edit control if doing layout
			if (layoutPending)
				MoveWindow(edit, rcEdit.left, rcEdit.top, rcEdit.right - rcEdit.left, rcEdit.bottom - rcEdit.top, TRUE);

		};

		// draw an edit with vertical advance
		auto DrawEditAdvanceY = [&rcEdit, dyEdits, &hdc, this, &DrawEditBase](HWND edit, const char *label, bool alignOnLabel = false)
		{
			// draw and then advance vertically
			DrawEditBase(edit, label, alignOnLabel);
			OffsetRect(&rcEdit, 0, dyEdits);
		};

		// draw an edit with horizontal advance
		auto DrawEditAdvanceX = [&rcEdit, dyEdits, &hdc, this, &DrawEditBase](HWND edit, const char *label, bool alignOnLabel = false)
		{
			// draw and then advance horizontally
			DrawEditBase(edit, label, alignOnLabel);
			OffsetRect(&rcEdit, rcEdit.right - rcEdit.left, 0);
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
		auto LabelXY = [&hdc, this](const RECT &rcPlot, int xVal, int yVal)
		{
			int yPos = rcPlot.bottom + mainFontMetrics.tmHeight + 8;
			int xCenter = (rcPlot.left + rcPlot.right)/2;

			int cxNum = mainFontMetrics.tmAveCharWidth * 7;
			hdc.DrawText(xCenter - 8 - cxNum, yPos, -1, boldFont, HRGB(0x0000FF), "x:");
			hdc.DrawTextF(xCenter - 8, yPos, -1, boldFont, HRGB(0x0000FF), "% 5d", xVal);
			int cx = hdc.DrawText(xCenter + 8, yPos, 1, boldFont, HRGB(0x0000FF), "y:").cx;
			hdc.DrawTextF(xCenter + 8 + cx + cxNum, yPos, -1, boldFont, HRGB(0x0000FF), "% 5d", yVal);
		};

		// draw the X/Y axis visualization
		int x = 48;
		int y = 48;
		int cxPlot = 256, cyPlot = cxPlot;
		RECT rcPlot{ x, y, x + cxPlot, y + cyPlot };
		hdc.DrawText(POINT{ x + cxPlot/2, y - mainFontMetrics.tmHeight }, POINT{ 0, 4 }, POINT{ 0, -1 },
			boldFont, HRGB(0x0000FF), "Accelerometer X/Y (Raw)");

		// draw the main axes
		DrawAxes(rcPlot, cxPlot/2, cyPlot/2, HRGB(0xc0c0c0), "0", "0", 0);

		// draw the auto-center axes, if auto-centering is enabled
		if ((nudgeParams.flags & nudgeParams.F_AUTOCENTER) != 0)
		{
			DrawAxes(rcPlot, cxPlot/2 + s.xCenter * cxPlot / 65536, cyPlot/2 - s.yCenter * cyPlot / 65536, HRGB(0x8080FF),
				StrPrintf("CY=%d", s.yCenter).c_str(), StrPrintf("CX=%d", s.xCenter).c_str(), 3);
		}

		// plot the current reading
		CompatibleDC bdc(hdc);
		bdc.Select(crosshairs);
		BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		AlphaBlend(hdc,
			rcPlot.left + cxPlot/2 + s.xRaw * cxPlot / 65536 - szCrosshairs.cx/2, rcPlot.top + cyPlot/2 - s.yRaw * cyPlot / 65536 - szCrosshairs.cy/2,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically at the bottom
		int yLabel = rcPlot.bottom + mainFontMetrics.tmHeight + 8;
		LabelXY(rcPlot, s.xRaw, s.yRaw);

		//
		// Raw Z axis
		//
		int xz = rcPlot.right + mainFontMetrics.tmAveCharWidth*12;
		int cxPlotZ = 32;
		RECT rcPlotZ{ xz, rcPlot.top, xz + cxPlotZ, rcPlot.bottom };

		hdc.DrawText(POINT{ xz + cxPlotZ/2, y - mainFontMetrics.tmHeight }, POINT{ 0, 4 }, POINT{ 0, -1 },
			boldFont, HRGB(0x0000FF), "Z (Raw)");

		DrawAxes(rcPlotZ, cxPlotZ/2, cyPlot/2, HRGB(0xC0C0C0), "0", "", 0);
		if ((nudgeParams.flags & nudgeParams.F_AUTOCENTER) != 0)
		{
			DrawAxes(rcPlotZ, cxPlotZ/2, cyPlot/2 - s.zCenter * cyPlot / 65536, HRGB(0x8080FF),
				StrPrintf("CZ=%d", s.zCenter).c_str(), "", 3);
		}

		AlphaBlend(hdc,
			xz + cxPlotZ/2 - szCrosshairs.cx/2, rcPlot.top + cyPlot/2 - s.zRaw * cyPlot / 65536 - szCrosshairs.cy/2,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically
		hdc.DrawTextF((rcPlotZ.left + rcPlotZ.right)/2, yLabel, 0, boldFont, HRGB(0x0000FF),
			"z: %-5d", s.zRaw);

		//
		// Filtered X/Y
		//
		int xf = rcPlotZ.right + mainFontMetrics.tmAveCharWidth*12;
		int cxPlotF = cxPlot;
		RECT rcPlotF{ xf, rcPlot.top, xf + cxPlotF, rcPlot.bottom };

		hdc.DrawText(POINT{ xf + cxPlotF/2, y - mainFontMetrics.tmHeight },
			POINT{ 0, 4 }, POINT{ 0,-1 }, boldFont, HRGB(0x0000FF), "Filtered X/Y");

		DrawAxes(rcPlotF, cxPlotF/2, cyPlot/2, HRGB(0x0000FF), "X", "Y", 1);

		AlphaBlend(hdc,
			xf + cxPlotF/2 - szCrosshairs.cx/2 + s.xFiltered * cxPlotF / 65536,
			rcPlotF.top + cyPlot/2 - szCrosshairs.cy/2 - s.yFiltered * cyPlot / 65536,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically at the bottom
		LabelXY(rcPlotF, s.xFiltered, s.yFiltered);

		// arrange and draw the filter parameter controls
		rcEdit = GetChildControlRect(dcTimeEdit);
		int xEdits = xf + cxPlotF/2;
		OffsetRect(&rcEdit, xEdits - rcEdit.left, y + cyPlot + mainFontMetrics.tmHeight + 48 - rcEdit.top);
		DrawEditAdvanceY(dcTimeEdit, "DC Blocker time constant:");
		DrawEditAdvanceY(jitterXEdit, "X axis jitter window:");
		DrawEditAdvanceY(jitterYEdit, "Y axis jitter window:");
		DrawEditAdvanceY(jitterZEdit, "Z axis jitter window:");

		//
		// Filtered Z
		//
		int xzf = rcPlotF.right + mainFontMetrics.tmAveCharWidth*8;
		int cxPlotZf = 32;
		RECT rcPlotZf{ xzf, rcPlot.top, xzf + cxPlotZ, rcPlot.bottom };

		hdc.DrawText(POINT{ xzf + cxPlotZf/2, y - mainFontMetrics.tmHeight }, POINT{ 0, 4 }, POINT{ 0, -1 },
			boldFont, HRGB(0x0000FF), "Z (Filtered)");

		DrawAxes(rcPlotZf, cxPlotZf/2, cyPlot/2, HRGB(0xC0C0C0), "0", "", 0);

		AlphaBlend(hdc,
			xzf + cxPlotZ/2 - szCrosshairs.cx/2, rcPlot.top + cyPlot/2 - s.zFiltered * cyPlot / 65536 - szCrosshairs.cy/2,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically
		hdc.DrawTextF((rcPlotZf.left + rcPlotZf.right)/2, yLabel, 0, boldFont, HRGB(0x0000FF),
			"z: %-5d", s.zFiltered);


		//
		// Velocity X/Y
		//
		int xVel = rcPlotZf.right + mainFontMetrics.tmAveCharWidth*12;
		int cxPlotVel = cxPlot;
		RECT rcPlotVel{ xVel, rcPlot.top, xVel + cxPlotVel, rcPlot.bottom };

		hdc.DrawText(POINT{ xVel + cxPlotVel/2, y - mainFontMetrics.tmHeight },
			POINT{ 0, 4 }, POINT{ 0,-1 }, boldFont, HRGB(0x0000FF), "Velocity X/Y");

		DrawAxes(rcPlotVel, cxPlotVel/2, cyPlot/2, HRGB(0x0000FF), "X", "Y", 1);

		AlphaBlend(hdc,
			xVel + cxPlotVel/2 - szCrosshairs.cx/2 + s.vx * cxPlotVel / 65536,
			rcPlotVel.top + cyPlot/2 - szCrosshairs.cy/2 - s.vy * cyPlot / 65536,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically at the bottom
		LabelXY(rcPlotVel, s.vx, s.vy);

		// arrange and draw the velocity parameter controls
		rcEdit = GetChildControlRect(velocityDecayEdit);
		xEdits = xVel + cxPlotVel/2;
		OffsetRect(&rcEdit, xEdits - rcEdit.left, y + cyPlot + mainFontMetrics.tmHeight + 48 - rcEdit.top);
		DrawEditAdvanceY(velocityDecayEdit, "Velocity decay time (ms):");
		DrawEditAdvanceY(velocityScaleEdit, "Velocity scaling factor:");

		//
		// Velocity Z
		//
		int xzv = rcPlotVel.right + mainFontMetrics.tmAveCharWidth*8;
		int cxPlotZV = 32;
		RECT rcPlotZV{ xzv, rcPlot.top, xzv + cxPlotZV, rcPlot.bottom };

		hdc.DrawText(POINT{ xzv + cxPlotZV/2, y - mainFontMetrics.tmHeight }, POINT{ 0, 4 }, POINT{ 0, -1 },
			boldFont, HRGB(0x0000FF), "Velocity Z");

		DrawAxes(rcPlotZV, cxPlotZV/2, cyPlot/2, HRGB(0xC0C0C0), "0", "", 0);

		AlphaBlend(hdc,
			xzv + cxPlotZ/2 - szCrosshairs.cx/2, rcPlot.top + cyPlot/2 - s.vz * cyPlot / 65536 - szCrosshairs.cy/2,
			szCrosshairs.cx, szCrosshairs.cy,
			bdc, 0, 0, szCrosshairs.cx, szCrosshairs.cy, bf);

		// label the current position numerically
		hdc.DrawTextF((rcPlotZV.left + rcPlotZV.right)/2, yLabel, 0, boldFont, HRGB(0x0000FF),
			"z: %-5d", s.vz);

		//
		// Mini pinball model
		//
		int xMini = rcPlotVel.left;
		int yMini = rcEdit.bottom + mainFontMetrics.tmHeight + 4;
		int cyMini = mainFontMetrics.tmAveCharWidth*40;
		simScale = static_cast<float>(cyMini) / 152.0f;
		int cxMini = cyMini;
		rcSim ={ xMini, yMini, xMini + cxMini, yMini + cyMini };

		// headline text
		hdc.DrawText(POINT{ xMini + cxMini/2, yMini }, POINT{ 0, 4 }, POINT{ 0,-1 },
			boldFont, HRGB(0x0000FF), "Physics Tester");

		// draw it
		MicroPinSim::DrawingCtx simDrawCtx{ hdc, rcSim, simScale, simDataView, simDataFont, tmSimData };
		ut->pinModel.Draw(simDrawCtx, balls, flippers);

		// end separators at the model
		cxSep = xMini - 32;

		//
		// auto-centering
		//

		y += cyPlot + mainFontMetrics.tmHeight + 48;
		x = 16;
		auto now = GetTickCount64();
		if (now > lastAutoCenterDisplayTime + 750)
			lastAutoCenterDisplayTime = now, lastAutoCenterTime = s.lastCenteringTime;

		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "All axis ranges are -32768 to +32767").cy;
		if ((nudgeParams.flags & nudgeParams.F_AUTOCENTER) != 0)
			y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Last auto-centered: %.2lf seconds ago",
				static_cast<double>(lastAutoCenterTime)/1000000.0f).cy;
		else
			y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Last auto-centered: N/A (disabled)").cy;
		y += 16;

		// auto-center parameter controls
		RECT rcCk = GetChildControlRect(autoCenterCk);
		if (rcCk.top != y)
			MoveWindow(autoCenterCk, x, y, rcCk.right - rcCk.left, rcCk.bottom - rcCk.top, TRUE);

		y += rcCk.bottom - rcCk.top + 8;
		rcEdit = GetChildControlRect(autoCenterTimeEdit);
		const char *editLabel = "Auto-centering interval:";
		OffsetRect(&rcEdit, x - rcEdit.left, y - rcEdit.top);
		DrawEditAdvanceY(autoCenterTimeEdit, editLabel, true);
		y = rcEdit.top;

		editLabel = "Quiet threshold: X:";
		rcEdit = GetChildControlRect(quietXEdit);
		OffsetRect(&rcEdit, x - rcEdit.left, y - rcEdit.top);
		DrawEditAdvanceX(quietXEdit, editLabel, true);

		x = rcEdit.left;
		rcEdit = GetChildControlRect(quietYEdit);
		OffsetRect(&rcEdit, x - rcEdit.left, y - rcEdit.top);
		DrawEditAdvanceX(quietYEdit, " Y:", true);

		x = rcEdit.left;
		rcEdit = GetChildControlRect(quietZEdit);
		OffsetRect(&rcEdit, x - rcEdit.left, y - rcEdit.top);
		DrawEditAdvanceX(quietZEdit, " Z:", true);
		y = rcEdit.bottom + 10;
		x = 16;

		// auto-center-now button
		RECT rcb = GetChildControlRect(centerBtn);
		if (layoutPending)
			MoveWindow(centerBtn, x, y, rcb.right - rcb.left, rcb.bottom - rcb.top, TRUE);
		y += rcb.bottom - rcb.top;
		EnableWindow(centerBtn, (nudgeParams.flags & nudgeParams.F_AUTOCENTER) != 0);

		//
		// Capture button
		//
		y = max(y, rcEdit.top);
		y += 16;
		DrawSep(x, y);
		y += 16;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x0000FF), "Capture readings to file").cy + 8;

		rcb = GetChildControlRect(btnCapture);
		OffsetRect(&rcb, x - rcb.left, y - rcb.top);
		if (layoutPending)
			MoveWindow(btnCapture, rcb.left, rcb.top, rcb.right - rcb.left, rcb.bottom - rcb.top, TRUE);

		if (captureFile != nullptr)
		{
			hdc.DrawText(rcb.right + 16, (rcb.top + rcb.bottom - boldFontMetrics.tmHeight)/2, 1, boldFont,
				HRGB((GetTickCount64() % 800) < 400 ? 0xFF00FF : 0x00FF00), "*** Capture in progress ***");
		}

		y += rcb.bottom - rcb.top;

		//
		// Noise calibration section
		//
		y += 16;
		DrawSep(x, y);
		y += 16;

		int ySect = y;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x0000FF), "Noise Calibration").cy + 8;
		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "This measures the random background vibrations that the accelerometer reads").cy;
		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "when the device is at rest.  Run the calibration for more accurate detection of").cy;
		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "stillness for auto-centering.  During calibration, don't move the accelerometer,").cy;
		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "and avoid any large vibrations in the surrounding area that could be picked up.").cy;
		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Calibration runs for about 15 seconds and goes into effect automatically at the").cy;
		y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "end of the period.").cy;
		y += 16;
		rcb = GetChildControlRect(calBtn);
		if (layoutPending)
			MoveWindow(calBtn, x, y, rcb.right - rcb.left, rcb.bottom - rcb.top, TRUE);

		if ((s.flags & s.F_CALIBRATING) != 0)
		{
			hdc.DrawText(POINT{ x + rcb.right - rcb.left + 32, y + (rcb.bottom - rcb.top)/2 }, POINT{ 0, 0 }, POINT{ 1, 0 },
				boldFont, HRGB((GetTickCount64() % 800) < 400 ? 0xFF0000 : 0x0080FF), "CALIBRATION IN PROGRESS");
		}
		int yMax = rcb.bottom;

		y = ySect;
		x += mainFontMetrics.tmAveCharWidth * 80;
		y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x0000FF), "Current auto-centering noise thresholds:").cy + 8;
		y += hdc.DrawTextF(x + 16, y, 1, mainFont, HRGB(0x000000), "X: +/- %d", s.xThreshold).cy;
		y += hdc.DrawTextF(x + 16, y, 1, mainFont, HRGB(0x000000), "Y: +/- %d", s.yThreshold).cy;
		y += hdc.DrawTextF(x + 16, y, 1, mainFont, HRGB(0x000000), "Z: +/- %d", s.zThreshold).cy;

		//
		// Save/revert buttons
		//
		RECT brc;
		GetWindowRect(saveBtn, &brc);
		y = max(yMax + 10, crc.bottom - (brc.bottom - brc.top) - 16);
		OffsetRect(&brc, -brc.left + 16, 0);
		if (brc.top != y)
		{
			OffsetRect(&brc, 0, y - brc.top);
			MoveWindow(saveBtn, brc.left, brc.top, brc.right - brc.left, brc.bottom - brc.top, TRUE);
			OffsetRect(&brc, brc.right - brc.left + 8, 0);
			MoveWindow(revertBtn, brc.left, brc.top, brc.right - brc.left, brc.bottom - brc.top, TRUE);
		}

		// enable/disable the save/restore buttons according to the modified state
		EnableWindow(saveBtn, isSettingModified);
		EnableWindow(revertBtn, isSettingModified);

		// layout has been completed
		layoutPending = false;
	}
}

void NudgeDeviceWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

    // get the window DC
	WindowDC hdc(hwnd);

	// get the window's current size
	RECT crc;
	GetClientRect(hwnd, &crc);

    // create controls
	auto cbNudge = [this](const char*) { this->OnNudgeParamChange(); };
	autoCenterTimeEdit = CreateEdit(ID_EDIT_AUTOCENTERTIME, 50, 8, nudgeParams.autoCenterInterval, cbNudge);
	autoCenterCk = CreateControl(ID_CK_AUTOCENTER, WC_BUTTONA, "Enable auto-centering", BS_AUTOCHECKBOX | BS_NOTIFY, 90, 8);
	quietXEdit = CreateEdit(ID_EDIT_QUIETX, 25, 8, nudgeParams.xThresholdAutoCenter, cbNudge);
	quietYEdit = CreateEdit(ID_EDIT_QUIETY, 25, 8, nudgeParams.yThresholdAutoCenter, cbNudge);
	quietZEdit = CreateEdit(ID_EDIT_QUIETZ, 25, 8, nudgeParams.zThresholdAutoCenter, cbNudge);
	dcTimeEdit = CreateEdit(ID_EDIT_DCTIME, 50, 8, nudgeParams.dcTime, cbNudge);
	jitterXEdit = CreateEdit(ID_EDIT_JITTERX, 50, 8, nudgeParams.xJitterWindow, cbNudge);
	jitterYEdit = CreateEdit(ID_EDIT_JITTERY, 50, 8, nudgeParams.yJitterWindow, cbNudge);
	jitterZEdit = CreateEdit(ID_EDIT_JITTERZ, 50, 8, nudgeParams.zJitterWindow, cbNudge);
	velocityDecayEdit = CreateEdit(ID_EDIT_VDECAY, 50, 8, nudgeParams.velocityDecayTime_ms, cbNudge);
	velocityScaleEdit = CreateEdit(ID_EDIT_VSCALE, 50, 8, nudgeParams.velocityScalingFactor, cbNudge);
	centerBtn = CreateControl(ID_BTN_CENTER, WC_BUTTONA, "Center Now", 0, 50, 16);
	btnCapture = CreateControl(ID_BTN_CAPTURE, WC_BUTTONA, "Start Capture", 0, 50, 16);
	calBtn = CreateControl(ID_BTN_CALIBRATE, WC_BUTTONA, "Calibrate", 0, 50, 16);
	saveBtn = CreateControl(ID_BTN_SAVE, WC_BUTTONA, "Save Settings", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);
	revertBtn = CreateControl(ID_BTN_REVERT, WC_BUTTONA, "Restore Settings", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);

	// load the nudge params
	ReloadNudgeParams(true);

	// create the simulator data font
	simDataFont = CreateFontA(
		-MulDiv(8, GetDeviceCaps(hdc, LOGPIXELSY), 72),
		0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
		"Segoe UI");

	// get the font metrics
	HFONT oldFont = SelectFont(hdc, simDataFont);
	GetTextMetrics(hdc, &tmSimData);
	SelectFont(hdc, boldFont);
}

void NudgeDeviceWin::ReloadNudgeParams(bool force)
{
	// retrieve the current nudge model parameters
	NudgeParams newParams;
	if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
		updaterThread->device->device->QueryNudgeParams(&newParams, sizeof(newParams));

	// ignore notifications while updating controls
	loadingControlValues = true;

	// update an edit box
	bool changed = false;
	auto UpdateEdit = [this, force, &changed](HWND edit, unsigned int val, unsigned int orig)
	{
		if (force || (val != orig && GetFocus() != edit))
		{
			SetWindowTextA(edit, StrPrintf("%u", val).c_str());
			changed = true;
		}
	};
	auto UpdateCk = [this, force, &changed](HWND ck, bool val, bool orig)
	{
		if (force || (val != orig && GetFocus() != ck))
		{
			Button_SetCheck(ck, val ? BST_CHECKED : BST_UNCHECKED);
			changed = true;
		}
	};

	// update the control values from the device params
	UpdateCk(autoCenterCk, (newParams.flags & newParams.F_AUTOCENTER) != 0, (nudgeParams.flags & nudgeParams.F_AUTOCENTER) != 0);
	UpdateEdit(autoCenterTimeEdit, newParams.autoCenterInterval, nudgeParams.autoCenterInterval);
	UpdateEdit(quietXEdit, newParams.xThresholdAutoCenter, nudgeParams.xThresholdAutoCenter);
	UpdateEdit(quietYEdit, newParams.yThresholdAutoCenter, nudgeParams.yThresholdAutoCenter);
	UpdateEdit(quietZEdit, newParams.zThresholdAutoCenter, nudgeParams.zThresholdAutoCenter);
	UpdateEdit(dcTimeEdit, newParams.dcTime, nudgeParams.dcTime);
	UpdateEdit(jitterXEdit, newParams.xJitterWindow, nudgeParams.xJitterWindow);
	UpdateEdit(jitterYEdit, newParams.yJitterWindow, nudgeParams.yJitterWindow);
	UpdateEdit(jitterZEdit, newParams.zJitterWindow, nudgeParams.zJitterWindow);
	UpdateEdit(velocityDecayEdit, newParams.velocityDecayTime_ms, nudgeParams.velocityDecayTime_ms);
	UpdateEdit(velocityScaleEdit, newParams.velocityScalingFactor, nudgeParams.velocityScalingFactor);

	// done updating controls
	loadingControlValues = false;

	// save the new parameters on any change
	if (changed)
	{
		// store the last parameters
		nudgeParams = newParams;

		// pass the velocity scaling factor to the updater thread
		static_cast<UpdaterThread*>(updaterThread.get())->OnNudgeParamsChange(&nudgeParams);
	}
}

void NudgeDeviceWin::OnNudgeParamChange()
{
	// populate a Put Params struct from the edit fields
	static auto Get = [](HWND edit) {
		char buf[128];
		GetWindowTextA(edit, buf, _countof(buf));
		return std::string(buf);
	};
	static auto GetUI16 = [](HWND edit) { return static_cast<uint16_t>(atoi(Get(edit).c_str())); };
	auto &np = nudgeParams;
	memset(&np, 0, sizeof(np));
	np.cb = sizeof(np);
	if (Button_GetCheck(autoCenterCk) == BST_CHECKED) np.flags |= np.F_AUTOCENTER;
	np.autoCenterInterval = GetUI16(autoCenterTimeEdit);
	np.xThresholdAutoCenter = GetUI16(quietXEdit);
	np.yThresholdAutoCenter = GetUI16(quietYEdit);
	np.zThresholdAutoCenter = GetUI16(quietZEdit);
	np.dcTime = GetUI16(dcTimeEdit);
	np.xJitterWindow = GetUI16(jitterXEdit);
	np.yJitterWindow = GetUI16(jitterYEdit);
	np.zJitterWindow = GetUI16(jitterZEdit);
	np.velocityDecayTime_ms = GetUI16(velocityDecayEdit);
	np.velocityScalingFactor = GetUI16(velocityScaleEdit);

	// update the velocity scaling factor in the updater thread
	static_cast<UpdaterThread*>(updaterThread.get())->OnNudgeParamsChange(&np);

	// send the update
	int stat = PinscapeResponse::ERR_FAILED;
	if (VendorInterface::Shared::Locker l(updaterThread->device.get()); l.locked)
		stat = updaterThread->device->device->PutNudgeParams(&np, sizeof(np));

	// flag errors
	if (stat != PinscapeResponse::OK)
	{
		gApp.AddTimedStatusMessage(StrPrintf(
			"Error sending nudge parameter update to device (%s, code %d)", VendorInterface::ErrorText(stat), stat).c_str(),
			HRGB(0xFFFFFF), HRGB(0xC00000), 5000);
	}
}

void NudgeDeviceWin::OnNCDestroy()
{
	// do the base class work
	__super::OnNCDestroy();
}

void NudgeDeviceWin::UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply)
{
	// enable save/revert according to the modified flag
	apply(ID_NUDGE_SAVESETTINGS, isSettingModified);
	apply(ID_NUDGE_REVERTSETTINGS, isSettingModified);
}

HMENU NudgeDeviceWin::GetContextMenu(POINT pt, HWND &hwndCommand)
{
    return hCtxMenu;
}

bool NudgeDeviceWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
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
			ShowHelpFile("NudgeWin.htm");
			return true;

        case ID_NUDGE_CALIBRATE:
		case ID_BTN_CALIBRATE:
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
				updaterThread->device->device->StartNudgeCalibration(false);
			return true;

		case ID_NUDGE_CENTER:
		case ID_BTN_CENTER:
			lastAutoCenterDisplayTime = 0;
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
				updaterThread->device->device->SetNudgeCenterPoint();
			return true;

		case ID_NUDGE_SAVESETTINGS:
		case ID_BTN_SAVE:
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
			{
				int stat = updaterThread->device->device->CommitNudgeSettings();
				if (stat == PinscapeResponse::OK)
					gApp.AddTimedStatusMessage("Nudge device settings saved to Pico flash memory", HRGB(0xffffff), HRGB(0x00C000), 5000);
				else
					MessageBoxFmt(hwnd, "Error saving settings: %s (code %d)", VendorInterface::ErrorText(stat), stat);
			}
			return true;

		case ID_NUDGE_REVERTSETTINGS:
		case ID_BTN_REVERT:
			// revert settings
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
			{
				int stat = updaterThread->device->device->RevertNudgeSettings();
				if (stat == PinscapeResponse::OK)
					gApp.AddTimedStatusMessage("Nudge device settings restored to values last saved in flash", HRGB(0xffffff), HRGB(0x00C000), 5000);
				else
					MessageBoxFmt(hwnd, "Error restoring nudge device settings: %s (code %d)", VendorInterface::ErrorText(stat), stat);
			}

			// reload device settings into controls
			ReloadNudgeParams(true);
			return true;

		case ID_BTN_CAPTURE:
			// start/stop capture
			if (captureFile == nullptr)
			{
				// start capture
				GetFileNameDlg dlg(
					_T("Select capture output file"), 0,
					_T("CSV Files\0*.csv\0All Files\0*.*\0"), _T("csv"));
				
				if (dlg.Save(GetDialogOwner()))
				{
					// open the file
					captureFile.reset(new std::fstream(dlg.GetFilename(), std::ios_base::out));

					// write the CSV header
					*captureFile << "timestamp,xRaw,yRaw,zRaw,xFiltered,yFiltered,zFiltered,xAvg,yAvg,zAvg,xCenter,yCenter,zCenter\n";

					// make sure the file is valid
					if (!*captureFile)
					{
						MessageBoxFmt(GetDialogOwner(), "Error opening file \"" TCHAR_FMT "\"", dlg.GetFilename());
						captureFile.reset();
						return true;
					}

					// notify the updater thread
					auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
					if (WaitForSingleObject(ut->dataMutex, 1000) == WAIT_OBJECT_0)
					{
						ut->captureFile = captureFile.get();
						ReleaseMutex(ut->dataMutex);
					}
						
					// change the button text to show a capture is in progress
					SetWindowTextA(btnCapture, "End Capture");
				}
			}
			else
			{
				// notify the updater thread
				auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
				if (WaitForSingleObject(ut->dataMutex, 1000) == WAIT_OBJECT_0)
				{
					ut->captureFile = nullptr;
					ReleaseMutex(ut->dataMutex);
				}

				// close and forget the file
				captureFile->close();
				captureFile.reset();

				// reset the button text
				SetWindowTextA(btnCapture, "Start Capture");
			}
			return true;

		case ID_CK_AUTOCENTER:
			// send the change to the device
			OnNudgeParamChange();
			return true;
		}
		break;

	case EN_CHANGE:
		if (OnEditChange(hwndCtl))
			return true;
		break;
	}

	// use the base handling
	return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

bool NudgeDeviceWin::OnEditChange(HWND edit)
{
	// ignore during control loading
	if (loadingControlValues)
		return false;

	// look for a change handler for the control
	if (auto p = editChangeProcs.find(reinterpret_cast<UINT_PTR>(edit)) ; p != editChangeProcs.end())
	{
		// retrieve the text
		char buf[256];
		GetWindowTextA(edit, buf, _countof(buf));

		// call the change processor
		p->second(buf);
	}

	// not handled
	return false;
}

HWND NudgeDeviceWin::CreateEdit(UINT id, int cx, int cy, float initialValue, EditChangeProc changeProc)
{
	char buf[64];
	sprintf_s(buf, "%g", initialValue);
	return CreateEdit(id, cx, cy, buf, changeProc);
}

HWND NudgeDeviceWin::CreateEdit(UINT id, int cx, int cy, const char *initialValue, EditChangeProc changeProc)
{
	// create the system control
	HWND edit = CreateControl(id, WC_EDITA, "", ES_LEFT, cx, cy);

	// set the change proc
	editChangeProcs.emplace(reinterpret_cast<UINT_PTR>(edit), changeProc);

	// Set the initial value.  Set the loading flag while we do this so that
	// the control doesn't try to interpret the change as a user action that
	// requires calling the change proc.
	loadingControlValues = true;
	SetWindowTextA(edit, initialValue);
	loadingControlValues = false;

	// return the control window
	return edit;
}


LRESULT NudgeDeviceWin::ControlSubclassProc(
	HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass)
{
	// use the default handling
	return __super::ControlSubclassProc(hwnd, msg, wparam, lparam, idSubclass);
}


bool NudgeDeviceWin::OnLButtonDown(WPARAM keys, int x, int y)
{
	// check for a simulator click, inside by the ball radius
	POINT pt{ x, y };
	if (PtInRect(&rcSim, pt))
	{
		OnSimMouseEvent(WM_LBUTTONDOWN, keys, x, y);
		return true;
	}

	// use the inherited handling
	return __super::OnLButtonDown(keys, x, y);
}

void NudgeDeviceWin::OnSimMouseEvent(UINT event, WPARAM keys, int x, int y)
{
	// get the point in simulator coordinates
	POINT pt{ x, y };
	MicroPinSim::Point simPt(
		static_cast<float>((pt.x - rcSim.left)/simScale),
		static_cast<float>((rcSim.bottom - pt.y)/simScale));

	// lock the model
	auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
	if (WaitForSingleObject(ut->dataMutex, 100) == WAIT_OBJECT_0)
	{
		// if we're not already tracking a ball, search for a click on a ball
		auto &sim = ut->pinModel;
		if (!trackingSimClick)
		{
			// search for a ball
			trackingSimBall = nullptr;
			sim.ForEachBall([this, &simPt](MicroPinSim::Ball *ball)
			{
				if (simPt.Dist(ball->c) < ball->r || trackingSimBall == nullptr)
					trackingSimBall = ball;
			});

			// start tracking
			trackingSimClick = true;
			SetCapture(hwnd);
		}

		// move the ball
		if (trackingSimBall != nullptr)
		{
			trackingSimBall->c = simPt;
			trackingSimBall->v ={ 0, 0 };
		}

		// unlock the model
		ReleaseMutex(ut->dataMutex);
	}
}

bool NudgeDeviceWin::OnLButtonUp(WPARAM keys, int x, int y)
{
	// end sim capture
	if (trackingSimClick)
	{
		ReleaseCapture();
		return true;
	}

	// proceed to the handling
	return __super::OnLButtonUp(keys, x, y);
}

bool NudgeDeviceWin::OnMouseMove(WPARAM keys, int x, int y)
{
	// handle sim click tracking if applicable
	if (trackingSimClick)
	{
		OnSimMouseEvent(WM_MOUSEMOVE, keys, x, y);
		return true;
	}

	// no special handling
	return __super::OnMouseMove(keys, x, y);
}

bool NudgeDeviceWin::OnCaptureChange(HWND hwnd)
{
	// end sim tracking
	trackingSimClick = false;
	trackingSimBall = nullptr;

	// use the default handling
	return __super::OnCaptureChange(hwnd);
}


bool NudgeDeviceWin::OnKeyDown(WPARAM vkey, LPARAM flags)
{
	auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
	switch (vkey)
	{
	case VK_F8:
		// F8 - toggle numeric data visualization in mini pin model
		simDataView = !simDataView;
		if (WaitForSingleObject(updaterThread->dataMutex, 100) == WAIT_OBJECT_0)
		{
			ut->pinModel.SetDebugMode(simDataView);
			ReleaseMutex(updaterThread->dataMutex);
		}
		break;
	}

	// use the inherited handling
	return __super::OnKeyDown(vkey, flags);
}


// --------------------------------------------------------------------------
//
// Updater thread
//

NudgeDeviceWin::UpdaterThread::UpdaterThread()
{
	// Set up the mini pinball simulator model.  This is just a
	// little square with elastic walls and one ball in the middle.
	pinModel.AddBall(76.0f, 76.0f);
	pinModel.AddWall(0.75f, 0.0f, 0.0f, 0.0f, 152.0f);
	pinModel.AddWall(0.75f, 0.0f, 0.0f, 152.0f, 0.0f);
	pinModel.AddWall(0.75f, 152.0f, 0.0f, 152.0f, 152.0f);
	pinModel.AddWall(0.75f, 0.0f, 152.0f, 152.0f, 152.0f);

	// disable gravity, to show the effects of the nudge input in
	// isolation
	pinModel.EnableGravity(false);
}

void NudgeDeviceWin::UpdaterThread::OnNudgeParamsChange(const PinscapePico::NudgeParams *np)
{
	// acquire the data mutex while updating values
	if (WaitForSingleObject(dataMutex, 100) == WAIT_OBJECT_0)
	{
		// Set the new velocity scale, device units to mm/s
		velocityScalingFactor = np->velocityScalingFactor == 0 ? 1.0f : 1.0f / np->velocityScalingFactor;

		// done with the mutex
		ReleaseMutex(dataMutex);
	}
}

// process periodic updates
bool NudgeDeviceWin::UpdaterThread::Update(bool &releasedMutex)
{
	// read a nudge status report
	PinscapePico::NudgeStatus s;
	int cmdStat = device->device->QueryNudgeStatus(&s, sizeof(s));

	// done with the mutex
	ReleaseMutex(device->mutex);
	releasedMutex = true;

	// save the new status
	if (cmdStat == PinscapeResponse::OK)
	{
		// acquire the data lock
		if (WaitForSingleObject(dataMutex, 100) == WAIT_OBJECT_0)
		{
			// check for new data, as indicated by the timestamp
			if (s.timestamp != nudgeStatus.timestamp)
			{
				// capture new readings to the file, if desired
				if (captureFile != nullptr)
				{
					*captureFile << s.timestamp
						<< "," << s.xRaw << "," << s.yRaw << "," << s.zRaw
						<< "," << s.xFiltered << "," << s.yFiltered << "," << s.zFiltered
						<< "," << s.xAvg << "," << s.yAvg << "," << s.zAvg
						<< "," << s.xCenter << "," << s.yCenter << "," << s.zCenter
						<< "\n";
				}

				// save the status data
				nudgeStatus = s;
			}

			// update the model with the current live accelerometer velocity
			pinModel.SetNudgeVelocity(s.vx * velocityScalingFactor, s.vy * velocityScalingFactor);

			// figure the time since the last mini pin model update, in units of
			// time steps
			uint64_t now = hrt.GetTime_ticks();
			uint64_t dt = now - lastPinModelUpdateTime;
			uint64_t dtInSteps = (dt * simTimeStepsPerSecond) / hrt.GetFreq();
			if (dtInSteps != 0)
			{
				// Limit the to 25ms of simulation time,to avoid extremely long 
				// computation runs after switching into the foreground.  This 
				// will freeze the simulation when the thread is blocked, which
				// is fine; it should evolve in something close to real time
				// when the UI is active, but there's no reason that simulation
				// time has to remain in sync with wall clock time globally 
				// across a whole run.
				int maxSteps = 25 * simTimeStepsPerSecond / 1000;
				int steps;
				if (dtInSteps > maxSteps)
				{
					steps = 1;
					lastPinModelUpdateTime = now;
				}
				else
				{
					steps = static_cast<int>(dtInSteps);
					lastPinModelUpdateTime += (steps * hrt.GetFreq() / simTimeStepsPerSecond);
				}

				// evolve the model
				for (int i = 0 ; i < steps ; ++i)
					pinModel.Evolve(simTimeStep);
			}

			// done with the data lock
			ReleaseMutex(dataMutex);
		}
	}

	// success
	return true;
}
