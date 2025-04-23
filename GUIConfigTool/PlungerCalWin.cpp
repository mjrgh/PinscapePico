// Pinscape Pico - Plunger Calibration Window
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
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "PinscapePicoAPI.h"
#include "WinUtil.h"
#include "Utilities.h"
#include "PlungerCalWin.h"
#include "resource.h"

// the window class is part of the PinscapePico namespace
using namespace PinscapePico;

PlungerCalWin::Factory::FeedbackDeviceList PlungerCalWin::Factory::FilterDevices(const FeedbackDeviceList list)
{
	// get the sublist of items with plungers configured
	using Desc = PinscapePico::FeedbackControllerInterface::Desc;
	FeedbackDeviceList withPlunger;
	std::copy_if(list.begin(), list.end(), std::back_inserter(withPlunger), [](const Desc &desc) {
		return desc.plungerType != PinscapePico::FeedbackControllerReport::PLUNGER_NONE; });

	// return the filtered list
	return withPlunger;
}

PlungerCalWin::PlungerCalWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
	DeviceThreadWindow(hInstance, device, new UpdaterThread())
{
	// Load my menu bar and accelerator
	hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_PLUNGERWIN));
	hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_DEVICEWIN));

	// query device information
	QueryDeviceInfo();
}

PlungerCalWin::~PlungerCalWin()
{
	// Before we exit, revert in-memory settings on the device to the
	// last saved settings.  This provides a more document-like model for
	// the user, by making the settings in the window act like the working
	// copy of a document in a traditional Windows program.  When closing
	// a window with unsaved changes in a traditional application, the
	// unsaved changes are discarded (after asking the user to confirm
	// that this is okay, which we also do here).  
	auto &dev = updaterThread->device;
	if (VendorInterface::Shared::Locker l(dev); l.locked)
		dev->device->RevertPlungerSettings();
}

void PlungerCalWin::OnEraseDeviceConfig(bool factoryReset)
{
	// re-query the device information
	QueryDeviceInfo();

	// reset UI controls to the updated/erased device settings
	InitControls();
	lastCalDataValid = false;
}


bool PlungerCalWin::OnDeviceReconnect()
{
	// remember the old plunger type, and re-query the device information
	auto oldSensorType = sensorType;
	QueryDeviceInfo();

	// Sync settings on the device with the UI.  This provides a document-like
	// model for the user, in that changes made in the UI remain in effect as
	// long as the window is open.  With a standard document model, the working
	// copy viewed and edited through the UI would be in local memory, but in
	// this case we have a weird distributed setup where the "document" contents
	// are also stored in working memory on the Pico.  Resetting the Pico makes
	// it forget the old memory contents, which is why we have to explicitly
	// restore the UI settings here, to get the Pico's copy of the working
	// in-memory document back in sync with the UI.
	//
	// Only do this if the plunger type didn't change.  If the plunger type
	// changed since we last checked, it's better to forget the old settings,
	// since the various sensors have different enough characteristics that
	// the settings have to be fine-tuned to the particular setup.  That's
	// really the whole point of these settings in the first place, to
	// optimize for the installed sensor's response characteristics and the
	// physical install environment.
	if (sensorType == oldSensorType)
	{
		// no sensor type change - carry over the old settings
		jitter.SendToDevice(this);
		firingTime.SendToDevice(this);
		integrationTime.SendToDevice(this);
		scalingFactor.SendToDevice(this);
		reverseOrientation.SendToDevice(this);
		scanMode.SendToDevice(this);

		// if we captured a calibration, restore it
		if (lastCalDataValid && !lastCalDataPending)
		{
			if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
				updaterThread->device->device->SetPlungerCalibrationData(&lastCalData, sizeof(lastCalData));
		}

		// Clear the calibration-pending flag, in case the device reset
		// while a calibration was in progress - we don't need to wait
		// for it any longer.
		lastCalDataPending = false;
	}
	else
	{
		// sensor type changed - reinitialize the UI settings from the
		// current settings on the device
		InitControls();
		lastCalDataValid = false;
	}

	// tell the container that we've refreshed, so it can keep the window open
	return true;
}


void PlungerCalWin::QueryDeviceInfo()
{
	// query the plunger configuration, and look up the plunger sensor display name
	auto &device = updaterThread->device;
	if (VendorInterface::Shared::Locker l(device); l.locked)
	{
		if (device->device->QueryPlungerConfig(plungerConfig) == PinscapeResponse::OK)
		{
			// check for a change of sensor type
			if (sensorType != plungerConfig.sensorType)
			{

				// save the type code and friendly name
				sensorType = plungerConfig.sensorType;
				sensorTypeName = PinscapePico::FeedbackControllerInterface::Desc::GetPlungerTypeName(plungerConfig.sensorType);

				// note if it's an imaging sensor
				isImageSensor = (
					sensorType == FeedbackControllerReport::PLUNGER_TCD1103
					|| sensorType == FeedbackControllerReport::PLUNGER_TSL1410R
					|| sensorType == FeedbackControllerReport::PLUNGER_TSL1412S);

				// populate the scan mode combo
				PopulateScanModeCombo();
			}
		}
	}
}

void PlungerCalWin::PopulateScanModeCombo()
{
	// only proceed if the combo exists (as this routine can be called
	// during the initial device info query, before the UI is initialized)
	if (auto cb = scanMode.combo; cb != NULL)
	{
		// clear out the old strings
		while (ComboBox_GetCount(scanMode.combo) != 0)
			ComboBox_DeleteString(scanMode.combo, ComboBox_GetCount(scanMode.combo) - 1);

		// add the appropriate strings for the new combo
		if (sensorType == FeedbackControllerReport::PLUNGER_TSL1410R
			|| sensorType == FeedbackControllerReport::PLUNGER_TSL1412S)
		{
			ComboBox_AddString(scanMode.combo, _T("Steady Slope"));
			ComboBox_AddString(scanMode.combo, _T("Steepest Slope"));
			ComboBox_AddString(scanMode.combo, _T("Speed Gap"));
		}
	}
}

bool PlungerCalWin::InstallParentMenuBar(HWND hwndContainer)
{
	SetMenu(hwndContainer, hMenuBar);
	return true;
}

// translate keyboard accelerators
bool PlungerCalWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
	return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

// initialize settings from a fresh plunger reading
void PlungerCalWin::InitControls()
{
	PinscapePico::PlungerReading pd;
	if (VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
	{
		std::vector<uint8_t> sd;
		updaterThread->device->device->QueryPlungerReading(pd, sd);
	}

	// initialize the controls
	InitControls(pd);
}

// initialize settings
void PlungerCalWin::InitControls(const PlungerReading &pd)
{
	jitter.Set(pd.jfWindow);
	firingTime.Set(pd.firingTimeLimit / 1000);
	integrationTime.Set(pd.integrationTime);
	scalingFactor.Set(pd.manualScalingFactor);
	reverseOrientation.Set((pd.flags & pd.F_REVERSE) != 0);
	scanMode.Set(pd.scanMode);
}

// update controls with the current reading
void PlungerCalWin::UpdateControls(const PlungerReading &pd)
{
	jitter.Update(pd.jfWindow);
	firingTime.Update(pd.firingTimeLimit / 1000);
	integrationTime.Update(pd.integrationTime);
	scalingFactor.Update(pd.manualScalingFactor);
}

// Perform an operation while holding the device mutex
int PlungerCalWin::WithDeviceLock(const char *desc, std::function<int()> func, DWORD timeout_ms)
{
	// acquire the device mutex
	if (WaitForSingleObject(updaterThread->device->mutex, timeout_ms) == WAIT_OBJECT_0)
	{
		// invoke the callback
		int stat = func();

		// done with the device
		ReleaseMutex(updaterThread->device->mutex);

		// check for errors
		if (stat != PinscapeResponse::OK)
			MessageBoxFmt(hwnd, "Error %s (%s, code %d)", desc, VendorInterface::ErrorText(stat), stat);

		// return the status
		return stat;
	}
	else
	{
		MessageBoxFmt(hwnd, "Error %s: unable to acquire access to device connection", desc);
		return PinscapeResponse::ERR_TIMEOUT;
	}
}

// Set the UI value
void PlungerCalWin::IntEditBox::Set(uint32_t val)
{
	// set the window text
	char buf[32];
	sprintf_s(buf, "%lu", val);
	SetWindowTextA(edit, buf);

	// set the internal control value
	curVal = origVal = val;
	valid = true;
}

void PlungerCalWin::IntEditBox::Update(uint32_t val)
{
	// update if it's different and we don't currently have focus
	if (curVal == origVal && origVal != val && GetFocus() != edit)
		Set(val);
}

// Check for changes in an edit box
void PlungerCalWin::IntEditBox::CheckChanges(PlungerCalWin *win)
{
	// get the current int value in the box
	char buf[128];
	GetWindowTextA(edit, buf, _countof(buf));
	long newVal = atol(buf);

	// check that it's in range; if not, don't send it to the device
	valid = (newVal >= static_cast<long>(minVal) && static_cast<uint32_t>(newVal) <= maxVal);
	if (!valid)
		return;

	// if it's different from the current stored value, update it
	if (newVal != curVal)
	{
		curVal = origVal = newVal;
		SendToDevice(win);
	}
}

void PlungerCalWin::IntEditBox::SendToDevice(PlungerCalWin *win)
{
	char desc[128];
	sprintf_s(desc, "sending new %s setting", name);
	win->WithDeviceLock(desc, [this, win]() { return send(win->updaterThread->device->device.get(), curVal); });
}

void PlungerCalWin::ByteComboBox::Set(uint8_t val)
{
	ComboBox_SetCurSel(combo, val);
	curVal = val;
}

void PlungerCalWin::ByteComboBox::CheckChanges(PlungerCalWin *win)
{
	// get the current value
	int sel = ComboBox_GetCurSel(combo);
	if (sel >= 0 && sel != curVal)
	{
		curVal = static_cast<uint8_t>(sel);
		SendToDevice(win);
	}
}

void PlungerCalWin::ByteComboBox::SendToDevice(PlungerCalWin *win)
{
	char desc[128];
	sprintf_s(desc, "sending new %s setting", name);
	win->WithDeviceLock(desc, [this, win]() { return send(win->updaterThread->device->device.get(), curVal); });
}

// Set the UI value
void PlungerCalWin::Checkbox::Set(bool val)
{
	// set the checkbox state
	Button_SetCheck(ck, val ? BST_CHECKED : BST_UNCHECKED);

	// set the internal value
	curVal = val;
}

// Check for changes in a checkbox
void PlungerCalWin::Checkbox::CheckChanges(PlungerCalWin *win)
{
	// get the current checkbox value
	bool newVal = (Button_GetCheck(ck) == BST_CHECKED);

	// check for a change
	if (newVal != curVal)
	{
		curVal = newVal;
		SendToDevice(win);
	}
}

void PlungerCalWin::Checkbox::SendToDevice(PlungerCalWin *win)
{
	// send the change to the device
	char desc[128];
	sprintf_s(desc, "sending new %s setting", name);
	win->WithDeviceLock(desc, [win, this]() { return this->send(win->updaterThread->device->device.get(), curVal); });
}

// Check the controls for changes
void PlungerCalWin::CheckControlChanges()
{
	jitter.CheckChanges(this);
	firingTime.CheckChanges(this);
	integrationTime.CheckChanges(this);
	scalingFactor.CheckChanges(this);
	reverseOrientation.CheckChanges(this);

	// DON'T check certain controls on the timer; use change events for these instead
	// scanMode.CheckChanges(this);
}

// Paint off-screen
void PlungerCalWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// redo the control layout on scroll changes
	if (yScrollPos != yScrollPosAtLastDraw)
	{
		yScrollPosAtLastDraw = yScrollPos;
		layoutPending = true;
	}

	// layout changes, for layoutPending mode
	POINT ptCalibrate, ptTxtJitter, ptSpinJitter, ptTxtFiringTime, ptSpinFiringTime,
		ptCbReverse, ptTxtIntegrationTime, ptSpinIntegrationTime, ptCapture, ptFrameCapture,
		ptTxtScalingFactor, ptSpinScalingFactor, ptScanMode, ptBtnSave, ptBtnRevert, ptBtnHelp;
	bool integrationTimeVisible = false;
	bool scanModeVisible = false;

	// fill the background
	HDCHelper hdc(hdc0);
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

	// get a local copy of the data for drawing the update
	if (WaitForSingleObject(updaterThread->dataMutex, 100) == WAIT_OBJECT_0)
	{
		// stash a local copy so that we can release the mutex
		PlungerReading pd;
		std::vector<BYTE> sensorData;
		auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
		pd = ut->reading;
		bool hasSensor = (sensorType != FeedbackControllerReport::PLUNGER_NONE);
		isSettingModified = hasSensor && ((pd.flags & pd.F_MODIFIED) != 0);
		sensorData = ut->sensorData;
		auto zeroCrossing = ut->zeroCrossing;
		auto newTimestamp = ut->timestamp;

		// done with the mutex
		ReleaseMutex(updaterThread->dataMutex);

		// update fields with the new settings, if applicable
		UpdateControls(pd);

		// determine if this is a new reading
		bool isNewReading = newTimestamp != lastReadingTimestamp;
		lastReadingTimestamp = newTimestamp;

		// note the new orientation
		bool isReverseOrientation = ((pd.flags & pd.F_REVERSE) != 0);

		// Layout parameters
		static const int xMargin = 16;
		static const int xMarginBar = 16;
		const int barWidth = max(400, crc.right - crc.left - xMarginBar*2), barHeight = 16;
		int xBar = crc.left + xMarginBar;
		int y0 = crc.top + 16 - yScrollPos;
		int y = y0;

		// Draw a bar label
		// 
		// dir = 
		//   +1 to draw right of origin
		//   -1 to draw left of origin
		//   +2 to draw right of origin, but move left if it doesn't fit
		//   -2 to draw left of origin, but move right if it doesn't fit
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

		// draw a gray line at the current y, with some vertical margin space around it
		auto DrawSeparator = [hdc, &crc, &y]()
		{
			y += 16;
			HPen grayPen(RGB(0xE0, 0xE0, 0xE0));
			HPEN oldPen = SelectPen(hdc, grayPen);
			MoveToEx(hdc, crc.left + xMargin, y, NULL);
			LineTo(hdc, crc.right - xMargin, y);
			y += 16;
			SelectObject(hdc, oldPen);
		};

		// draw the jitter window
		auto DrawJitterFilter = [this, &pd, &hdc, xBar, barWidth]
		(int y, int height, int yOfsText, COLORREF boxColor, COLORREF outerBoxColor, std::function<int(int)> ToScreenX)
		{
			// if the lo/hi gap is smaller than the window, expand it to
			// the window size, adding the excess equally to either side
			uint32_t lo = pd.jfLo;
			uint32_t hi = pd.jfHi;
			uint32_t win = pd.jfWindow;
			uint32_t gap = max(hi - lo, 0);
			if (gap < win)
			{
				int excess = win - gap;
				lo -= excess/2;
				hi += (excess+1)/2;
			}

			// Get screen coordinates - swap them if they're in right-to-left order
			auto Clip = [xBar, barWidth](int x) { return x < xBar ? xBar : x > xBar + barWidth ? xBar + barWidth : x; };
			int screenJfLo = Clip(ToScreenX(lo));
			int screenJfHi = Clip(ToScreenX(hi));
			if (screenJfLo > screenJfHi)
				std::swap(screenJfLo, screenJfHi);

			// If desired, drawn an outer box first
			RECT jrc{ screenJfLo - 1, y, screenJfHi + 1, y + height };
			if (outerBoxColor != boxColor)
			{
				FrameRect(hdc, &jrc, HBrush(outerBoxColor));
				InflateRect(&jrc, -1, -1);
			}

			// Draw a box around the jitter window
			FrameRect(hdc, &jrc, HBrush(boxColor));

			// label it
			char buf[128];
			sprintf_s(buf, "Dejittered: %lu (%lu..%lu)", pd.jfLastPost, pd.jfLo, pd.jfHi);
			SIZE sz = hdc.MeasureText(barFont, buf);
			SetBkMode(hdc, OPAQUE);
			SetBkColor(hdc, RGB(0xFF, 0xFF, 0xFF));
			if (screenJfHi + sz.cx >= barWidth)
				hdc.DrawText(screenJfLo - 4, y + yOfsText, -1, barFont, RGB(0x00, 0x00, 0xFF), buf);
			else
				hdc.DrawText(screenJfHi + 4, y + yOfsText, 1, barFont, RGB(0x00, 0x00, 0xFF), buf);

			SetBkMode(hdc, TRANSPARENT);
		};

		//
		// Configuration information section
		//
		y += hdc.DrawTextF(xMargin, y, 1, mainFont, RGB(0x00, 0x00, 0x00),
			"Pinscape unit number: %d (%s)", deviceID.unitNum, deviceID.unitName.c_str()).cy;

		y += hdc.DrawTextF(xMargin, y, 1, mainFont, RGB(0x00, 0x00, 0x00),
			"Sensor type: %s", sensorTypeName.c_str()).cy;

		y += 8;
		y += hdc.DrawText(xMargin, y, 1, mainFont, RGB(0x60, 0x60, 0x60),
			"(You can change the sensor setup via the JSON configuration)").cy;

		DrawSeparator();


		//
		// Sensor visualization section.   This draws an image snapshot for
		// imaging sensors, a simulation of the encoder strip bars for quadrature
		// encoders, or other sensor-specific visualizations.
		//

		// label it
		int sensorImageHeight = 36;
		y += hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Live Sensor View").cy;

		// get the native scale; use the full range if the scale is zero (which will be
		// the case if no sensor is configured), and use '1' if the range is zero
		int nativeScale = plungerConfig.nativeScale;
		if (nativeScale == 0)
			nativeScale = pd.calMax - pd.calMin;
		if (nativeScale == 0)
			nativeScale = 1;

		// Figure the calibration points on screen
		double rawToScreen = 1.0 / max(nativeScale, 10) * barWidth;
		int screenMin = static_cast<int>(round(pd.calMin * rawToScreen));
		int screenZero = static_cast<int>(round(pd.calZero * rawToScreen));
		int screenMax = static_cast<int>(round(pd.calMax * rawToScreen));

		// Get the base struct information for the sensor data, if any
		struct __PackedBegin SensorBaseData
		{
			uint16_t cb;
			uint16_t sensorType;
		} __PackedEnd;
		const SensorBaseData *sensorBaseData = (sensorData.size() >= sizeof(SensorBaseData)) ?
			reinterpret_cast<const SensorBaseData*>(sensorData.data()) : nullptr;

		// get the mouse position in local coordinates
		POINT mousePos;
		GetCursorPos(&mousePos);
		ScreenToClient(hwnd, &mousePos);

		// Draw the live sensor view/visualization
		int ySensor = y;
		RECT src{ xBar, y, xBar + barWidth, y + sensorImageHeight };
		bool sensorVisualizationDrawn = false;
		bool jitterFilterVisualizationDrawn = false;
		if (sensorBaseData != nullptr && sensorBaseData->sensorType == plungerConfig.sensorType)
		{
			if (isImageSensor)
			{
				// Imaging sensors.  These sensors provide a snapshot of the
				// one-dimensional optical image.
				const auto *sd = reinterpret_cast<const PinscapePico::PlungerReadingImageSensor*>(sensorBaseData);
				int nPix = sd->nPix;

				// pixel -> grayscale function
				std::function<UINT32(uint8_t)> pixToGray;
				static auto RGBA = [](int r, int g, int b) { return static_cast<UINT32>((r << 16) | (g << 8) | b); };
				if (plungerConfig.sensorType == FeedbackReport::PLUNGER_TCD1103)
				{
					// This sensor produces a negative image, with about a 1.4V
					// dynamic range, or about 110 units on the 8-bit scale.
					// We can figure the dark level from pixels [16..28], which
					// are live sensor pixels that are physically shaded on the
					// sensor to provide a dark reference voltage.
					int darkSum = 0;
					for (int i = 16 ; i <= 28 ; darkSum += sd->pix[i++]) ;
					uint8_t dark = static_cast<uint8_t>(max(darkSum / 13, 110));
					uint8_t bright = static_cast<uint8_t>(dark - 110);
					float norm = 1.0f/(dark - bright);
					pixToGray = [dark, bright, norm](uint8_t pix) -> COLORREF 
					{
						// apply a power curve, similar to gamma correction, and scale to 3x the 8-bit range
						int gray = pix < bright ? 0xFF : pix > dark ? 0x00 :
							static_cast<int>(roundf(powf((dark - pix)*norm, 0.6f)*255.0f*3.0f));

						// show 0-255 as shades of red, 256-511 as shades of orange to yellow, and 512+ as bright yellow to white
						return (gray < 255) ? RGBA(static_cast<BYTE>(gray), 0, 0) :
							(gray < 255*2) ? RGBA(255, static_cast<BYTE>(gray - 255), 0) :
							RGBA(255, 255, static_cast<BYTE>(gray - 255*2));
					};
				}
				else
				{
					// For other sensors, assume the analog level is rail-to-rail,
					// so it quantizes to the full 0..255 range of the UINT8.  Show
					// the level linearly, using a red-yellow-white colorization.
					pixToGray = [](uint8_t pix) -> COLORREF {
						int gray = static_cast<int>(roundf(pix*3.0f));
						return (gray < 255) ? RGBA(static_cast<BYTE>(gray), 0, 0) :
							(gray < 255*2) ? RGBA(255, static_cast<BYTE>(gray - 255), 0) :
							RGBA(255, 255, static_cast<BYTE>(gray - 255*2));
					};
				}

				// get the pixel array
				const uint8_t *pix = sd->pix;

				// If the actual image is wider (in pixels) than our drawing
				// area, build a bitmap at the image width, and stretch-blt it
				// onto the drawing area.  This will scale it down to the
				// drawing resolution with nice dithering.  If the actual
				// image is narrow, the automatic stretch-blt upscaling doesn't
				// look very nice; we can do better by drawing the individual
				// pixels as rectangles.
				if (true) // change to if (nPix >= barWidth) if specializing small-sensor drawing
				{
					// create an RGB grayscale version of the pixel array
					std::unique_ptr<COLORREF> rgb(new COLORREF[nPix*3]);
					const uint8_t *src = pix;
					COLORREF *dst = rgb.get();
					int iStart = 0, iEnd = nPix, di = 1;
					if (isReverseOrientation)
						iStart = nPix - 1, iEnd = -1, di = -1, src = pix + nPix - 1;
					for (int i = iStart ; i != iEnd ; i += di, src += di)
						*dst++ = pixToGray(*src);

					// create a DIB from the 32-bit COLORREF array
					BITMAPINFOHEADER bmi{ sizeof(BITMAPINFOHEADER) };
					bmi.biWidth = nPix;
					bmi.biHeight = -1;
					bmi.biPlanes = 1;
					bmi.biBitCount = 32;
					bmi.biCompression = BI_RGB;
					bmi.biSizeImage = 0;
					HBITMAP bmp = CreateDIBitmap(hdc, &bmi, CBM_INIT, rgb.get(), reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS);

					// select it into a compatible DC
					HDC hdc2 = CreateCompatibleDC(hdc);
					HBITMAP oldbmp2 = SelectBitmap(hdc2, bmp);

					// stretch-blt it into the drawing area
					StretchBlt(hdc, xMarginBar, ySensor, barWidth, sensorImageHeight,
						hdc2, 0, 0, nPix, 1, SRCCOPY);

					// release resources
					SelectBitmap(hdc2, oldbmp2);
					DeleteBitmap(bmp);
					DeleteDC(hdc2);
				}
				else
				{
					// TO DO (if needed) - add special case drawing for small pixel 
					// arrays.
					// 
					// The currently supported sensors all have large pixel arrays
					// (over 1000 pixels), so this isn't currently needed.  If we
					// add a sensor in the future with a small pixel array (under
					// 100 pixels), it would benefit from custom drawing where
					// we fill out rectangles that divide the window space, rather
					// than stretch-blt'ing single pixels.  Stretch-blt'ing works
					// well when we're shrinking a large array to fit the window,
					// because it smooths out the detail nicely, but it doesn't
					// work well for enlarging a small pixel array to fit a big
					// display window.  That ends up looking like a low-res photo
					// being over-enlarged, making big fuzzy-edged pixels.  For
					// enlargement, it works better to draw the pixels individually
					// as rectangles, so that we show sharp edges between sensor
					// pixels, rather than smoothing them out.  Regular stretching
					// is intended to hide the pixel structure from the eye, but
					// in this case we want to make the pixel structure apparent,
					// since the whole point is to show the user the native image
					// that the sensor sees.
				}

				// if the mouse is over the bitmap area, add a numeric display
				// of the intensity at the point under the mouse
				if (PtInRect(&src, mousePos))
				{
					// figure the point in bitmap coordinates
					int pixIdx = static_cast<int>(roundf(
						static_cast<float>(mousePos.x - src.left)/static_cast<float>(barWidth)
						* static_cast<float>(nPix)));
					int pixVal = pix[isReverseOrientation ? nPix - 1 - pixIdx : pixIdx];
					SetBkMode(hdc, OPAQUE);
					SetBkColor(hdc, HRGB(0xFFFFFF));
					hdc.DrawTextF(POINT{ mousePos.x, src.top }, POINT{ 10, 0 }, POINT{ mousePos.x < barWidth/2 ? 1 : -1, 1 },
						barFont, HRGB(0x0000FF), " Pixel[%d] = %d ", pixIdx, pixVal);
					SetBkMode(hdc, TRANSPARENT);
				}

				// visualization drawn
				sensorVisualizationDrawn = true;
			}
			else if (plungerConfig.sensorType == FeedbackReport::PLUNGER_VCNL4010)
			{
				// VCNL4010 proximity sensor - show a visualization of the sensor's
				// raw proximity count, which is proportional to the intensity of
				// the reflected light received at the sensor.  Show this as a red
				// bar from the right end of the visualization area.  The prox count
				// is an unsigned 16-bit value, 0..65535.  Figure the color based
				// on the square root of the prox count.
				int proxCount = static_cast<int>(pd.jfLastPre);
				float proxCountf = static_cast<float>(proxCount);
				const auto *sd = reinterpret_cast<const PinscapePico::PlungerReadingVCNL4010*>(sensorBaseData);
				int proxScreen = static_cast<int>(floorf(proxCountf/65535.0f * barWidth));
				RECT rc{ src.right - proxScreen, src.top, src.right, src.bottom };
				FillRect(hdc, &src, HBrush(RGB(0xE0, 0xE0, 0xE0)));
				BYTE red = static_cast<BYTE>(fminf(sqrtf(proxCountf), 255.0f));
				FillRect(hdc, &rc, HBrush(RGB(red, 0x00, 0x00)));

				// The VCNL4010 applies jitter filtering to the prox count rather
				// than the distance, since the prox count is the linear analog
				// measurement being made.  Show the jitter filter window here
				// rather than in the raw reading.
				if (pd.jfWindow != 0)
				{
					int fontHt = hdc.MeasureText(barFont, "X").cy;
					int boxHt = src.bottom - src.top - fontHt - 2;
					DrawJitterFilter(rc.top + fontHt + 2, boxHt, (boxHt - fontHt)/2, RGB(0xFF, 0x00, 0xFF), RGB(0xFF, 0xFF, 0xFF),
						[&src, barWidth](int x) { return src.right - static_cast<int>(floorf(x/65535.0f * barWidth)); });
				}

				// flag that the jitter filter visualization has been drawn, so
				// that we don't draw it again in the raw position graph (the
				// green bar)
				jitterFilterVisualizationDrawn = true;

				// draw a label showing the numeric reading
				SetBkMode(hdc, OPAQUE);
				SetBkColor(hdc, RGB(red, 0x00, 0x00));
				DrawBarLabelF(rc.left, rc.top, RGB(0xFF, 0xFF, 0xFF), 2, "Prox count: %u", proxCount);
				SetBkMode(hdc, TRANSPARENT);

				// visualization drawn
				sensorVisualizationDrawn = true;
			}
			else if (plungerConfig.sensorType == FeedbackReport::PLUNGER_AEDR8300)
			{
				// Quadrature sensor - show a visualization of the sensor's scale
				// position.  The sensor extra data gives us the current "A" and "B"
				// channel states, where each channel state is a bool indicating
				// whether the sensor on that channel is looking at a "black" or
				// "white" bar.  The scare quotes because the actual quadrature
				// sensor isn't necessarily optical; it could be magnetic,
				// capacitive, or something else entirely.  But the principle is
				// always the same: the sensor runs along a scale that's encoded
				// with alternating on/off marks at regular intervals, which might
				// be black/white stripes, north/south poles, or whatever else the
				// sensor reads.  For the visualization, we draw the states as black
				// and white bars regardless of the underlying physical media.
				auto *sd = reinterpret_cast<const PinscapePico::PlungerReadingQuadrature*>(sensorBaseData);
				bool chA = (sd->state & 0x01) != 0;
				bool chB = (sd->state & 0x02) != 0;

				// Figure the stripe layout.  In the visualization, the current
				// sensor reading is positioned at the center of the bar box.  Each
				// sensor covers half a stripe width.  In the standard orientation,
				// A is on the left and B is on the right; flip that for reverse
				// orientation.  If A and B are both reading the same value, one
				// full stripe covers the whole A/B sensor zone.  Otherwise there's
				// a stripe boundary at the center of the bar area.  Once we figure
				// that, we just draw the stripes out in each direction from there.
				const int nStripes = 16;
				float stripeWidth = static_cast<float>(barWidth) / nStripes;
				float halfway = barWidth / 2.0f;
				float startOfs = halfway;
				if (chA == chB)
					startOfs -= stripeWidth / 2.0f;

				// set up black/white brushes
				HBrush black(RGB(0x00, 0x00, 0x00));
				HBrush white(RGB(0xff, 0xff, 0xff));

				// draw stripes to the right
				bool rightColor = isReverseOrientation ? chA : chB;
				bool color = rightColor;
				for (float x = startOfs ; x < barWidth ; x += stripeWidth, color = !color)
				{
					RECT rc{ xMarginBar + static_cast<int>(roundf(x)), ySensor,
						xMarginBar + min(static_cast<int>(roundf(x + stripeWidth)), barWidth), ySensor + sensorImageHeight };
					FillRect(hdc, &rc, color ? black : white);
				}

				// draw stripes to the left
				color = !rightColor;
				for (float x = startOfs - stripeWidth ; x > -stripeWidth ; x -= stripeWidth, color = !color)
				{
					RECT rc{ xMarginBar + max(static_cast<int>(roundf(x)), 0), ySensor,
						xMarginBar + static_cast<int>(roundf(x + stripeWidth)), ySensor + sensorImageHeight };
					FillRect(hdc, &rc, color ? black : white);
				}

				// outline the sensor position
				RECT rcl{ static_cast<int>(xMarginBar + halfway - stripeWidth/2), ySensor,
					static_cast<int>(xMarginBar + halfway), ySensor + sensorImageHeight };
				RECT rcr{ static_cast<int>(xMarginBar + halfway), ySensor,
					static_cast<int>(xMarginBar + halfway + stripeWidth/2), ySensor + sensorImageHeight };
				HBrush brFr(RGB(0x00, 0xff, 0x00));
				FrameRect(hdc, &rcl, brFr);
				FrameRect(hdc, &rcr, brFr);

				// caption the A and B sensor points
				int xTxt = xBar + static_cast<int>(halfway);
				hdc.DrawText(xTxt, ySensor, -1, boldFont, RGB(0x00, 0xFF, 0x00), isReverseOrientation ? "B " : "A ");
				hdc.DrawText(xTxt, ySensor, 1, boldFont, RGB(0x00, 0xFF, 0x00), isReverseOrientation ? " A" : " B");

				// visualization drawn
				sensorVisualizationDrawn = true;
			}
		}

		// if we didn't provide a custom sensor visualization, use the default
		// gray fill
		if (!sensorVisualizationDrawn)
		{
			// no additional sensor-specific data; fill the sensor 
			// visualization area with gray
			FillRect(hdc, &src, HBrush(RGB(0xE0, 0xE0, 0xE0)));
		}
		y += sensorImageHeight;

		// Get the raw position reading.  If we've already drawn the jitter
		// filter visualization as part of the sensor visualization, just
		// show the final position reading.  Otherwise, show the pre-filtered
		// raw reading, so that we can superimpose the jitter window bounds,
		// to show the live effect of the filtering.
		uint32_t rawPos = (jitterFilterVisualizationDrawn ? pd.rawPos : pd.jfLastPre);

		// Draw the raw sensor reading, extending left from the right side.  Note
		// that we want to show the pre-jitter-filtered value here for sensors
		// that apply the jitter filter at this stage, so that we can show the
		// jitter filter window.
		int screenRaw = static_cast<int>(round((nativeScale - rawPos) * rawToScreen));
		screenRaw = screenRaw < 0 ? 0 : screenRaw > barWidth ? barWidth : screenRaw;
		RECT rc{ xBar + screenRaw, y, xBar + barWidth, y + barHeight };
		HBrush rawBrush(RGB(0x00, 0xB0, 0x00));
		DrawBar(barWidth, -screenRaw, y, rawBrush, -1, 0, "Raw=%u", rawPos);

		// Add the jitter window, if active
		if (pd.jfWindow != 0 && !jitterFilterVisualizationDrawn)
		{
			DrawJitterFilter(y - barHeight, barHeight*2, 0, RGB(0x00, 0x00, 0xff), RGB(0x00, 0x00, 0xff),
				[xBar, rawToScreen](int x) { return static_cast<int>(round(x * rawToScreen)) + xBar; });
		}

		// advance past the bar
		y += barHeight;

		// frame the sensor visualization + raw position rectangle
		rc ={ xBar, ySensor, xBar + barWidth, y };
		FrameRect(hdc, &rc, GetStockBrush(BLACK_BRUSH));

		// draw the zero marker
		COLORREF zeroMarkerColor = RGB(0x00, 0xA0, 0x00);
		DrawMarker(screenZero, y, zeroMarkerColor);
		hdc.DrawTextF(xBar + screenZero + markerLen, y, 1, barFont, zeroMarkerColor, "Cal Rest=%u", pd.calZero);

		// Draw the max marker.  In the special case that max <= min, which only
		// happens if the plunger isn't functioning or the calibration is screwed
		// up, just omit the max.  It'll end up as a jumble on top of the min,
		// and it's not meaningful anyway.
		if (screenMax > screenMin)
		{
			COLORREF maxMarkerColor = RGB(0xFF, 0x00, 0xFF);
			DrawMarker(screenMax, y, maxMarkerColor);
			hdc.DrawTextF(xBar + screenMax + markerLen/2, y + markerLen, -1, barFont, maxMarkerColor, "Cal Max=%u", pd.calMax);
		}

		// draw the min marker
		COLORREF minMarkerColor = RGB(0xFF, 0x80, 0x00);
		DrawMarker(screenMin, y, minMarkerColor);
		y += hdc.DrawTextF(xBar + screenMin - markerLen/2, y + markerLen, 1, barFont, minMarkerColor, "Cal Min=%u", pd.calMin).cy;
		y += markerLen;

		//
		// Z Axis visualization
		//

		// label it
		y += 12;
		y += hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Joystick Z Axis View").cy;

		// Draw the Z axis bar, extending left from the right end
		int screenZ = static_cast<int>(((32767 - pd.z) / 32767.0) * (barWidth - screenZero));
		HBrush zBrush(RGB(0x00, 0x00, 0xD0));
		DrawBar(barWidth, -screenZ, y, zBrush, -1, 0, "Z=%d", pd.z);
		y += barHeight;

		// Draw the Z0 axis bar
		int screenZ0 = static_cast<int>(((32767 - pd.z0) / 32767.0) * (barWidth - screenZero));
		HBrush z0Brush(RGB(0x00, 0x40, 0xD0));
		DrawBar(barWidth, -screenZ0, y, z0Brush, -1, 0, "Z0=%d", pd.z0);
		y += barHeight;

		// Frame the Z bar area
		rc ={ xBar, y - barHeight*2, xBar + barWidth, y };
		FrameRect(hdc, &rc, zBrush);

		// draw the zero marker (again - this time for the Z axis view)
		DrawMarker(screenZero, y, zeroMarkerColor);
		y += hdc.DrawText(xBar + screenZero, y + markerLen, 0, barFont, zeroMarkerColor, "Z=0").cy;

		//
		// Speedometer section
		//
		y += 12;
		y += hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Speedometer (Z Axis logical units per 10ms)").cy;

		// Check for a new peak forward speed; forget the old one after
		// a few seconds
		UINT64 now = GetTickCount64();
		if (pd.speed < 0 && pd.speed < peakForwardSpeed)
		{
			peakForwardSpeed = pd.speed;
			tPeakForwardSpeed = now;
		}
		else if (now - tPeakForwardSpeed > 3000)
		{
			peakForwardSpeed = pd.speed < 0 ? pd.speed : 0;
			tPeakForwardSpeed = now;
		}

		// Draw the peak speed bar
		HBrush peakBrush(RGB(0xFF, 0xE0, 0xE0));
		HBrush speedBrush(RGB(0xFF, 0x00, 0x00));
		int screenPeak = static_cast<int>((peakForwardSpeed / 32767.0) * barWidth/2);
		DrawBar(barWidth/2, screenPeak, y, peakBrush, barWidth/2 + screenPeak, -1, nullptr);
		DrawBarLabelF(xBar + barWidth/2 + screenPeak, y, speedBrush.color, -1, "Peak: %d", peakForwardSpeed);

		// Draw the speed bar
		int screenSpeed = static_cast<int>((pd.speed / 32767.0) * barWidth/2);
		DrawBar(barWidth/2, screenSpeed, y, speedBrush, barWidth/2 + max(screenSpeed, 0), 1, "Speed: %d", pd.speed);

		// frame the speed bar
		rc ={ xBar, y, xBar + barWidth, y + barHeight };
		FrameRect(hdc, &rc, speedBrush);
		y += barHeight;

		//
		// Firing state section
		//
		{
			// figure the size of the whole indicator area, and left-align it
			SIZE sz = hdc.MeasureText(boldFont, "Firing State:    None    Moving    Fired    Settling  ");
			POINT pt{ xMargin, y + 16 };
			y = pt.y + sz.cy + 6;

			// draw a state button
			using FS = PinscapePico::PlungerFiringState;
			auto DrawSel = [sz, hdc, this, &pt, &crc](const char *txt, COLORREF color, bool selected)
			{
				// get the string length
				int len = static_cast<int>(strlen(txt));

				// use the bold font
				SelectFont(hdc, boldFont);

				// measure the text and set up the drawing area
				SIZE tsz;
				GetTextExtentPoint32A(hdc, txt, len, &tsz);
				RECT rc{ pt.x, pt.y - 2, pt.x + tsz.cx, pt.y + tsz.cy + 2 };

				// set the text color - gray by default, white if selected
				SetTextColor(hdc, selected ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xB0, 0xB0, 0xB0));

				// fill the background if this mode is selected
				if (selected)
					FillRect(hdc, &rc, HBrush(color));

				// draw the text
				rc.top += 2;
				DrawTextA(hdc, txt, static_cast<int>(strlen(txt)), &rc, DT_LEFT | DT_TOP);

				// advance past it horizontally
				pt.x += tsz.cx;
			};
			auto DrawState = [DrawSel, &pd](const char *txt, COLORREF color, FS state) {
				DrawSel(txt, color, pd.firingState == static_cast<uint16_t>(state));
			};

			// draw the label, then the state buttons
			pt.x += hdc.DrawText(pt.x, pt.y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Firing State:  ").cx;
			DrawState("  None  ", RGB(0x00, 0x00, 0x80), FS::None);
			DrawState("  Moving  ", RGB(0x00, 0xFF, 0x00), FS::Moving);
			DrawState("  Fired  ", RGB(0xFF, 0x00, 0x00), FS::Fired);
			DrawState("  Settling  ", RGB(0xFF, 0x80, 0x00), FS::Settling);

			// show the ZB Launch trigger state
			const char *zbStateLabel = "  ZB Launch  ";
			pt.x = crc.right - xMargin - hdc.MeasureText(boldFont, zbStateLabel).cx;
			DrawSel(zbStateLabel, RGB(0x80, 0x00, 0x00), (pd.flags & pd.F_ZBLAUNCH) != 0);

			// Show the last zero-crossing speed.  Fade it from bright red to black
			// to gray to white as it ages.
			UINT64 dt = (GetTickCount64() - zeroCrossing.time);
			auto Fade = [dt](int startTime, int duration, int startColor, int endColor) {
				float dColor = static_cast<float>(endColor - startColor);
				float frac = static_cast<float>(dt - startTime)/static_cast<float>(duration);
				float newColor = startColor + (frac * dColor);
				return static_cast<BYTE>(newColor);
			};
			auto Gray = [](BYTE c) { return RGB(c, c, c); };
			COLORREF zcolor = dt < 1000 ? RGB(0xFF, 0x00, 0x00) :
				dt < 3000 ? RGB(Fade(1000, 2000, 0xFF, 0x00), 0x00, 0x00) :
				dt < 5000 ? Gray(Fade(3000, 2000, 0x00, 0xFF)) :
				RGB(0xff, 0xff, 0xff);
			hdc.DrawTextF(xMargin, y, 1, mainFont, zcolor,
				"Last zero-crossing speed: %d, delta Z: %d (%I64u seconds ago)",
				zeroCrossing.speed, zeroCrossing.deltaZ, dt/1000);
			y += sz.cy;
		}

		//
		// Calibration section
		//
		{
			DrawSeparator();

			// arrange the button if layout is pending
			ptCalibrate ={ xBar, y };

			// disable the button if no sensor is attached and calibration isn't in progress
			bool calibrationInProgress = ((pd.flags & pd.F_CALIBRATING) != 0);
			EnableWindow(btnCalibrate, hasSensor && !calibrationInProgress);

			// draw the calibration status
			RECT rcBtn;
			GetWindowRect(btnCalibrate, &rcBtn);
			OffsetRect(&rcBtn, xBar - rcBtn.left, y - rcBtn.top);
			int xTxtBase = rcBtn.right + 16;
			int yTxt = hdc.VCenterText(rcBtn, mainFont);
			if (calibrationInProgress)
			{
				// calibrating - show the calibration status above a message explaining
				// the procedure
				yTxt -= hdc.MeasureText(mainFont, "X").cy;

				// blink the status message
				const UINT PERIOD = 1000;
				UINT64 phase = GetTickCount64() % PERIOD;
				COLORREF color = (phase > PERIOD/2) ? RGB(0xFF, 0x80, 0x00) : RGB(0x00, 0x40, 0xFF);
				yTxt += hdc.DrawText(xTxtBase, yTxt, 1, boldFont, color, "CALIBRATION IN PROGRESS").cy;

				// show the explanation
				hdc.DrawText(xTxtBase, yTxt, 1, mainFont, RGB(0x00, 0x00, 0x00),
					"Wait a couple of seconds, then pull the plunger all the way back.\n"
					"Hold for a second, then release.  Let it fully come to rest.  Repeat.");
			}
			else if (!hasSensor)
			{
				// no sensor - calibration not applicable
				int xTxt = xTxtBase;
				xTxt += hdc.DrawText(rcBtn.right + 16, yTxt, 1, mainFont, RGB(0x00, 0x00, 0x00), "Calibration Status:  N/A").cx;
			}
			else if ((pd.flags & pd.F_CALIBRATED) != 0)
			{
				// calibrated - show the status and the observed firing time
				yTxt -= (hdc.MeasureText(mainFont, "X").cy + 1) / 2;
				int xTxt = xTxtBase;
				xTxt += hdc.DrawText(rcBtn.right + 16, yTxt, 1, mainFont, RGB(0x00, 0x00, 0x00), "Calibration Status:  ").cx;
				yTxt += hdc.DrawText(xTxt, yTxt, 1, boldFont, RGB(0x00, 0x00, 0xFF), "Calibrated").cy;
				hdc.DrawTextF(xTxtBase, yTxt, 1, mainFont, RGB(0x00, 0x00, 0x00),
					"Average firing time during last calibration: %.2lf ms", pd.firingTimeMeasured/1000.0);

				// if we were waiting for a calibration to complete, capture the
				// calibration data, for restoration to the device if it reboots
				// while the window is still open
				if (lastCalDataPending)
				{
					// save the calibration data from the reading
					lastCalData.calMin = pd.calMin;
					lastCalData.calZero = pd.calZero;
					lastCalData.calMax = pd.calMax;
					lastCalData.firingTimeMeasured = pd.firingTimeMeasured;
					lastCalData.flags = 0;
					if ((pd.flags & pd.F_CALIBRATED) != 0) lastCalData.flags |= lastCalData.F_CALIBRATED;
					memcpy(lastCalData.sensorData, pd.calSensorData, min(sizeof(lastCalData.sensorData), sizeof(pd.calSensorData)));

					// set the struct size and mark it valid
					lastCalData.cb = sizeof(lastCalData);
					lastCalDataValid = true;

					// no longer waiting for the calibration to complete
					lastCalDataPending = false;
				}
			}
			else
			{
				// not calibrated
				int xTxt = xTxtBase;
				xTxt += hdc.DrawText(rcBtn.right + 16, yTxt, 1, mainFont, RGB(0x00, 0x00, 0x00), "Calibration Status:  ").cx;
				hdc.DrawText(xTxt, yTxt, 1, boldFont, RGB(0xFF, 0x80, 0x00), "Not Calibrated");
			}
			y = rcBtn.bottom;
		}


		//
		// Capture buttons
		//
		{
			// section separator
			DrawSeparator();
			
			// readings capture button
			SIZE szTitle1 = hdc.DrawText(xBar, y, 1, boldFont, HRGB(0x0000FF), "Capture readings to file");
			int yBtn = y + szTitle1.cy + 8;
			int xCaptureMsg = xBar + szTitle1.cx;
			ptCapture = { xBar, yBtn };

			// pixel capture button - show only for image sensors
			ptFrameCapture = { 0, 0 };
			if (isImageSensor)
			{
				int xfc = xBar + szTitle1.cx + 24;
				SIZE szTitle2 = hdc.DrawText(xfc, y, 1, boldFont, HRGB(0x0000FF), "Capture pixels to file");
				ptFrameCapture = { xfc, yBtn };
				xCaptureMsg = xfc + szTitle2.cx;
			}
			else
				ShowWindow(btnFrameCapture, SW_HIDE);

			// advance to the button row
			y = yBtn;

			// disable the buttons if no sensor is attached
			EnableWindow(btnCapture, hasSensor);
			EnableWindow(btnFrameCapture, hasSensor);

			// draw the status
			RECT rcb;
			GetWindowRect(btnFrameCapture, &rcb);
			OffsetRect(&rcb, xBar - rcb.left, y - rcb.top);
			int xTxtBase = rcb.right + 16;
			int yTxt = hdc.VCenterText(rcb, mainFont);
			if (captureFile != nullptr || frameCaptureFile != nullptr)
			{
				// capturing
				hdc.DrawText(xCaptureMsg + 16, (rcb.top + rcb.bottom - boldFontMetrics.tmHeight)/2, 1, boldFont,
					HRGB((GetTickCount64() % 800) < 400 ? 0xFF00FF : 0x00FF00), "*** Capture in progress ***");
			}

			// advance past the buttons
			y += rcb.bottom - rcb.top;
		}

		//
		// Configurable parameters section
		//
		{
			DrawSeparator();

			//
			// Orientation
			//

			// get the checkbox size
			RECT cbrc;
			GetWindowRect(reverseOrientation.ck, &cbrc);
			OffsetRect(&cbrc, -cbrc.left, -cbrc.top);

			// set the layout
			ptCbReverse ={ xBar, y };
			y += cbrc.bottom + 6;

			//
			// Jitter filter window
			//

			// get the text/spinner sizes
			RECT trc, src;
			GetWindowRect(jitter.edit, &trc);
			GetWindowRect(jitter.spin, &src);
			OffsetRect(&trc, -trc.left, -trc.top);
			OffsetRect(&src, -src.left, -src.top);

			// draw the caption
			int xCtls = xBar + hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Jitter filter window:  ").cx;

			// position the text box and spinner if necessary
			ptTxtJitter ={ xCtls, y };
			ptSpinJitter ={ xCtls + trc.right, y };

			// frame the text box and spinner
			RECT frc{ xCtls - 1, y - 1, xCtls + trc.right + src.right + 1, y + trc.bottom + 1 };
			FrameRect(hdc, &frc, GetStockBrush(BLACK_BRUSH));

			y += max(trc.bottom, src.bottom) + 6;

			//
			// Firing time 
			//
			GetWindowRect(firingTime.edit, &trc);
			OffsetRect(&trc, -trc.left, -trc.top);
			xCtls = xBar + hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Maximum firing time (milliseconds):  ").cx;
			ptTxtFiringTime ={ xCtls, y };
			ptSpinFiringTime ={ xCtls + trc.right, y };

			if (!firingTime.valid)
				hdc.DrawText(xCtls + trc.right + src.right - src.left + 8, y, 1, mainFont, RGB(0xFF, 0x00, 0x00), "(Invalid)");

			frc ={ xCtls - 1, y - 1, xCtls + trc.right + src.right + 1, y + trc.bottom + 1 };
			FrameRect(hdc, &frc, GetStockBrush(BLACK_BRUSH));
			y += trc.bottom + 6;

			//
			// Manual scaling factor
			//
			GetWindowRect(scalingFactor.edit, &trc);
			OffsetRect(&trc, -trc.left, -trc.top);
			xCtls = xBar + hdc.DrawText(xBar, y, 1, mainFont, RGB(0x00, 0x00, 0x00), "Manual scaling factor (percent):  ").cx;
			ptTxtScalingFactor ={ xCtls, y };
			ptSpinScalingFactor ={ xCtls + trc.right, y };

			if (!scalingFactor.valid)
				hdc.DrawText(xCtls + trc.right + src.right - src.left + 8, y, 1, mainFont, RGB(0xFF, 0x00, 0x00), "(Invalid)");

			frc ={ xCtls - 1, y - 1, xCtls + trc.right + src.right + 1, y + trc.bottom + 1 };
			FrameRect(hdc, &frc, GetStockBrush(BLACK_BRUSH));
			y += trc.bottom + 6;

			//
			// Integration time, if it's an applicable imaging sensor
			//
			GetWindowRect(integrationTime.edit, &trc);
			OffsetRect(&trc, -trc.left, -trc.top);
			ptTxtIntegrationTime ={ xBar, y };
			ptSpinIntegrationTime ={ xCtls + trc.right, y };

			// only display the integration time window if it's an imaging sensor
			// that accepts this parameter
			if (plungerConfig.sensorType == FeedbackReport::PLUNGER_TCD1103
				|| plungerConfig.sensorType == FeedbackReport::PLUNGER_TSL1410R
				|| plungerConfig.sensorType == FeedbackReport::PLUNGER_TSL1412S)
			{
				// draw the caption
				xCtls = xBar + hdc.DrawText(xBar, y, 1, mainFont, HRGB(0x000000), "Integration time (microseconds): ").cx;
				ptTxtIntegrationTime ={ xCtls, y };

				if (!integrationTime.valid)
					hdc.DrawText(xCtls + trc.right + src.right - src.left + 8, y, 1, mainFont, RGB(0xFF, 0x00, 0x00), "(Invalid)");

				// frame it
				frc ={ xCtls - 1, y - 1, xCtls + trc.right + src.right + 1, y + trc.bottom + 1 };

				// set the text box position
				FrameRect(hdc, &frc, GetStockBrush(BLACK_BRUSH));
				y += trc.bottom + 6;

				// do show the window
				integrationTimeVisible = true;
			}

			//
			// Scan mode drop list, for selected imaging sensors
			//
			GetWindowRect(scanMode.combo, &cbrc);
			OffsetRect(&cbrc, -cbrc.left, -cbrc.top);
			ptScanMode = { xBar, y };
			if (plungerConfig.sensorType == FeedbackReport::PLUNGER_TSL1410R
				|| plungerConfig.sensorType == FeedbackReport::PLUNGER_TSL1412S)
			{
				// draw the caption
				int dy = (cbrc.bottom - cbrc.top - mainFontMetrics.tmHeight)/2;
				xCtls = xBar + hdc.DrawTextF(xBar, y + dy, 1, mainFont, HRGB(0x00000), "Scan mode: ").cx;

				// set the combo position
				ptScanMode = { xCtls, y };
				y += cbrc.bottom + 6;

				// do show the window
				scanModeVisible = true;
			}

			//
			// Save/revert buttons
			//
			RECT brc;
			GetWindowRect(btnSaveSettings, &brc);
			OffsetRect(&brc, -brc.left, -brc.top);
			y += 10;
			ptBtnSave ={ xBar, y };
			ptBtnRevert ={ xBar + brc.right + 8, y };

			// enable/disable the save button
			BOOL enable = ((pd.flags & pd.F_MODIFIED) != 0);
			EnableWindow(btnSaveSettings, enable);
			EnableWindow(btnRevertSettings, enable);

			//
			// Help button - standalone (top-level window) mode only
			//
			if (btnSaveSettings != NULL)
			{
				ptBtnHelp ={ max(xBar + brc.right*2 + 16, crc.right - xMargin - brc.right), y };
				y += brc.bottom;
			}
		}

		// save the document height, if it changed
		int newDocHeight = y + 16 + yScrollPos;
		if (newDocHeight != docHeight)
		{
			docHeight = newDocHeight;
			AdjustScrollbarRanges();
		}
	}

	// do control layout
	if (layoutPending)
	{
		// Set positions and visibilities
		HDWP hdwp = BeginDeferWindowPos(16);
		auto SetPos = [hdwp](HWND ctl, POINT pt, bool visible = true) 
		{
			// move and show/hide the window
			DeferWindowPos(hdwp, ctl, NULL, pt.x, pt.y, -1, -1, SWP_NOSIZE | SWP_NOZORDER | (visible ? 0 : SWP_HIDEWINDOW));

			// If it's visible, invalidate it.  The interaction of the batch of
			// moves and the off-screen drawing in the main window makes Windows
			// miss updates in some cases if we don't do this explicitly.
			if (visible)
				InvalidateRect(ctl, NULL, TRUE);
		};

		SetPos(btnCalibrate, ptCalibrate);
		SetPos(jitter.edit, ptTxtJitter);
		SetPos(jitter.spin, ptSpinJitter);
		SetPos(firingTime.edit, ptTxtFiringTime);
		SetPos(firingTime.spin, ptSpinFiringTime);
		SetPos(reverseOrientation.ck, ptCbReverse);
		SetPos(scalingFactor.edit, ptTxtScalingFactor);
		SetPos(scalingFactor.spin, ptSpinScalingFactor);
		SetPos(integrationTime.edit, ptTxtIntegrationTime, integrationTimeVisible);
		SetPos(integrationTime.spin, ptSpinIntegrationTime, integrationTimeVisible);
		SetPos(scanMode.combo, ptScanMode, scanModeVisible);
		SetPos(btnSaveSettings, ptBtnSave);
		SetPos(btnRevertSettings, ptBtnRevert);
		SetPos(btnCapture, ptCapture);
		SetPos(btnFrameCapture, ptFrameCapture);

		if (btnHelp != NULL)
			SetPos(btnHelp, ptBtnHelp);

		// apply the deferred window positioning
		EndDeferWindowPos(hdwp);

		// done
		layoutPending = false;
	}
}

void PlungerCalWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

	// get the window DC
	WindowDC hdc(hwnd);

	// create additional fonts
	barFont = CreateFontA(14,
		0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
		"Segoe UI");

	// create a spin control
	auto CreateSpin = [this](int id, HWND txtCtl, int minVal, int maxVal) {
		return CreateUpDownControl(
			WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_HOTTRACK | UDS_SETBUDDYINT,
			0, 0, ControlWidth(16), ControlHeight(12), hwnd, id, hInstance, txtCtl, maxVal, minVal, 0);
	};
	btnCalibrate = CreateControl(ID_CALIBRATE, WC_BUTTONA, "Calibrate", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);
	btnCapture = CreateControl(ID_CAPTURE_BTN, WC_BUTTONA, "Start Capture", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);
	btnFrameCapture = CreateControl(ID_CAPTURE_FRAME_BTN, WC_BUTTONA, "Start Capture", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);
	reverseOrientation.ck = CreateControl(ID_REVERSE_CB, WC_BUTTONA, "Reverse Orientation",
		BS_AUTOCHECKBOX | BS_LEFT | BS_FLAT | BS_NOTIFY, 86, 10);
	jitter.edit = CreateControl(ID_JITTER_TXT, WC_EDITA, "", ES_LEFT | ES_NUMBER, 50, 12);
	jitter.spin = CreateSpin(ID_JITTER_SPIN, jitter.edit, 0, 32767);
	firingTime.edit = CreateControl(ID_FIRING_TIME_TXT, WC_EDITA, "", ES_LEFT | ES_NUMBER, 50, 12);
	firingTime.spin = CreateSpin(ID_FIRING_TIME_SPIN, firingTime.edit, 0, 1000);
	scalingFactor.edit = CreateControl(ID_SCALING_FACTOR_TXT, WC_EDITA, "", ES_LEFT | ES_NUMBER, 50, 12);
	scalingFactor.spin = CreateSpin(ID_SCALING_FACTOR_SPIN, scalingFactor.edit, 0, 1000);
	integrationTime.edit = CreateControl(ID_INT_TIME_TXT, WC_EDITA, "", ES_LEFT | ES_NUMBER, 50, 12);
	integrationTime.spin = CreateSpin(ID_INT_TIME_SPIN, integrationTime.edit, 0, 4000);
	scanMode.combo = CreateControl(ID_SCAN_MODE_CB, WC_COMBOBOXA, "", CBS_DROPDOWNLIST, 80, 80);
	btnSaveSettings = CreateControl(ID_SAVE_BTN, WC_BUTTONA, "Save Settings", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);
	btnRevertSettings = CreateControl(ID_REVERT_BTN, WC_BUTTONA, "Restore Settings", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);

	// populate the scan mode combo list box
	PopulateScanModeCombo();

	// set up the Help button in standalone mode (i.e., as a top-level window)
	if ((GetWindowStyle(hwnd) & WS_CHILD) == 0)
		btnHelp = CreateControl(ID_HELP_BTN, WC_BUTTONA, "Help", BS_CENTER | BS_FLAT | BS_NOTIFY, 60, 16);

	// initialize the controls
	InitControls();

	// range calculation for the scrollbar
	auto GetScrollRange = [this](SCROLLINFO &si)
	{
		// figure the client area height
		RECT crc;
		GetClientRect(hwnd, &crc);
		int winHeight = crc.bottom - crc.top;

		// set the range
		si.nMin = 0;
		si.nMax = max(docHeight - mainFontMetrics.tmHeight, 0);
		si.nPage = max(winHeight - mainFontMetrics.tmHeight, 20);
	};

	// scrolling region
	auto GetScrollRect = [](RECT *rc)
	{
		// no adjustment required - scroll the whole client rect
	};

	// set the scroll position
	auto SetScrollPos = [this](int newPos, int deltaPos) { yScrollPos = newPos; };

	// set up the scrollbar logic
	scrollbars.emplace_back(hwnd, SB_VERT, mainFontMetrics.tmHeight, true, true, GetScrollRange, GetScrollRect, SetScrollPos);
}

void PlungerCalWin::OnNCDestroy()
{
	// clean up GDI resources
	DeleteFont(barFont);
	
	// do the base class work
	__super::OnNCDestroy();
}

void PlungerCalWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// redo the control layout
	layoutPending = true;

	// do the base class work
	__super::OnSizeWindow(type, width, height);
}

void PlungerCalWin::OnTimer(WPARAM timerId)
{
	switch (timerId)
	{
	case TIMER_ID_REFRESH:
		// piggyback on the UI refresh timer to check for control changes
		CheckControlChanges();
		break;
	}

	// use the base class handling
	__super::OnTimer(timerId);
}

void PlungerCalWin::UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply)
{
	// if we don't have a sensor configured, disable all plunger commands
	bool hasSensor = (sensorType != FeedbackControllerReport::PLUNGER_NONE);
	apply(ID_PLUNGER_CALIBRATE, hasSensor);

	// enable save/revert according to the modified flag
	apply(ID_PLUNGER_SAVESETTINGSTOPICO, isSettingModified && hasSensor);
	apply(ID_PLUNGER_RESTORESETTINGSFROMPICO, isSettingModified && hasSensor);
}

bool PlungerCalWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lResult)
{
	switch (notifyCode)
	{
	case 0:
	case 1:
		// menu/accelerator/button click
		switch (ctlCmdId)
		{
		case ID_CALIBRATE:            // calibrate button
		case ID_PLUNGER_CALIBRATE:    // Plunger > Calibrate menu item
			// Begin calibration.  This requires access to the device, so
			// acquire the device mutex.
			if (WithDeviceLock("sending calibration request", [this]() {
				return updaterThread->device->device->StartPlungerCalibration(false);
			}) == PinscapeResponse::OK)
			{
				// make a note that we're waiting to collect the results of the calibration
				lastCalDataPending = true;
			}
			return true;

		case ID_CAPTURE_BTN:
			// Start/end capture
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
					*captureFile << "timestamp,sensor,z0,z,speed,firingState,firingStateName\n";

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

		case ID_CAPTURE_FRAME_BTN:
			// Start/end capture
			if (frameCaptureFile == nullptr)
			{
				// start capture
				GetFileNameDlg dlg(
					_T("Select pixel data capture output file"), 0,
					_T("Text Files\0*.txt\0All Files\0*.*\0"), _T("txt"));

				if (dlg.Save(GetDialogOwner()))
				{
					// open the file
					frameCaptureFile.reset(new std::fstream(dlg.GetFilename(), std::ios_base::out));

					// make sure the file is valid
					if (!*frameCaptureFile)
					{
						MessageBoxFmt(GetDialogOwner(), "Error opening file \"" TCHAR_FMT "\"", dlg.GetFilename());
						frameCaptureFile.reset();
						return true;
					}

					// write the header line
					*frameCaptureFile << "rawPos,z0,z,speed,firingState,firingStateName,pixels\n\n";

					// notify the updater thread
					auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
					if (WaitForSingleObject(ut->dataMutex, 1000) == WAIT_OBJECT_0)
					{
						ut->frameCaptureFile = frameCaptureFile.get();
						ReleaseMutex(ut->dataMutex);
					}

					// change the button text to show a capture is in progress
					SetWindowTextA(btnFrameCapture, "End Capture");
				}
			}
			else
			{
				// notify the updater thread
				auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
				if (WaitForSingleObject(ut->dataMutex, 1000) == WAIT_OBJECT_0)
				{
					ut->frameCaptureFile = nullptr;
					ReleaseMutex(ut->dataMutex);
				}

				// close and forget the file
				frameCaptureFile->close();
				frameCaptureFile.reset();

				// reset the button text
				SetWindowTextA(btnFrameCapture, "Start Capture");
			}
			return true;

		case ID_SAVE_BTN:
		case ID_PLUNGER_SAVESETTINGSTOPICO:
			// Save the updated settings
			WithDeviceLock("saving settings", [this]() { return updaterThread->device->device->CommitPlungerSettings(); });
			return true;

		case ID_REVERT_BTN:
		case ID_PLUNGER_RESTORESETTINGSFROMPICO:
			// Revert settings
			WithDeviceLock("restoring saved settings", [this]() {
				// send the revert request
				int stat = updaterThread->device->device->RevertPlungerSettings();

				// refresh our copy of the settings
				if (stat == PinscapeResponse::OK)
				{
					// take a reading to get the new settings
					PlungerReading reading;
					std::vector<BYTE>sensorData;
					stat = updaterThread->device->device->QueryPlungerReading(reading, sensorData);

					// update the UI to reflect the new settings
					if (stat == PinscapeResponse::OK)
						InitControls(reading);
				}
				return stat;
			});
			return true;

		case ID_HELP_BTN:
		case ID_HELP_HELP:
			// Help.  ID_HELP_BTN is sent by our in-window button control; ID_HELP_HELP
			// is sent from the container application menu bar when we're running in
			// child mode, such as within the Config Tool.
			ShowHelpFile("PlungerCal.htm");
			return true;

		case ID_SCAN_MODE_CB:
			// scan mode combo - check for a selection change
			if (notifyCode == CBN_SELCHANGE)
				scanMode.CheckChanges(this);
			break;
		}
		break;
	}

	// use the base handling
	return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lResult);
}

// Firing state names
static const char *GetFiringStateName(uint16_t stateNum)
{
	static const char *name[]{ "None", "Moving", "Firing", "Settling" };
	return stateNum < _countof(name) ? name[stateNum] : "Unknown";
}

// Updater thread
bool PlungerCalWin::UpdaterThread::Update(bool &releasedMutex)
{
	// get the next plunger report
	PinscapePico::PlungerReading reading;
	std::vector<BYTE> sensorData;
	bool ok = (device->device->QueryPlungerReading(reading, sensorData) == PinscapeResponse::OK);

	// done with the mutex
	ReleaseMutex(device->mutex);
	releasedMutex = true;

	// check the query result
	if (ok)
	{
		// success - update the data in the context, holding the mutex while updating
		if (WaitForSingleObject(dataMutex, 100) == WAIT_OBJECT_0)
		{
			// note zero crossings
			if (reading.z <= 0 && this->reading.z > 32767/6)
			{
				zeroCrossing.time = GetTickCount64();
				zeroCrossing.speed = reading.speed;
				zeroCrossing.deltaZ = this->reading.z - reading.z;
			}

			// capture new readings to the file, if desired
			if (captureFile != nullptr && reading.timestamp != this->reading.timestamp)
			{
				*captureFile << reading.timestamp
					<< "," << reading.rawPos
					<< "," << reading.z0
					<< "," << reading.z
					<< "," << reading.speed
					<< "," << reading.firingState
					<< "," << GetFiringStateName(reading.firingState)
					<< "\n";
			}

			// capture pixel data to the file, if desired
			if (frameCaptureFile != nullptr && reading.timestamp != this->reading.timestamp)
			{
				// make sure we have a valid sensor data struct, with at least the sensor type field
				struct __PackedBegin SensorBaseData
				{
					uint16_t cb;
					uint16_t sensorType;
				} __PackedEnd;
				if (sensorData.size() >= sizeof(PinscapePico::PlungerReadingImageSensor))
				{
					// interpret it as an image sensor struct, and make sure it has a valid
					// sensor type and matching pixel count
					const auto *sb = reinterpret_cast<const PinscapePico::PlungerReadingImageSensor*>(sensorData.data());
					static const std::unordered_map<int, int> sensorTypeToNPix{
						{ PinscapePico::FeedbackControllerReport::PLUNGER_TCD1103, 1546 },
						{ PinscapePico::FeedbackControllerReport::PLUNGER_TSL1410R, 1280 },
						{ PinscapePico::FeedbackControllerReport::PLUNGER_TSL1412S, 1536 },
					};
					if (auto it = sensorTypeToNPix.find(sb->sensorType);
						it != sensorTypeToNPix.end() && sb->nPix == it->second)
					{
						// all tests passed - build a text version of the pixel array
						std::unique_ptr<char> buf(new char[sb->nPix*3 + 1]);
						char *p = buf.get();
						auto *pix = sb->pix;
						for (unsigned int i = 0 ; i < sb->nPix ; ++i)
						{
							static const auto HexChar = [](uint8_t b) { return static_cast<char>(b < 10 ? b + '0' : b + 'A' - 10); };
							uint8_t curPix = *pix++;
							*p++ = HexChar((curPix >> 4) & 0x0F);
							*p++ = HexChar(curPix & 0x0F);
							*p++ = ' ';
						}
						*p = 0;

						// write the position readings and pixel file
						*frameCaptureFile << reading.timestamp
							<< "," << reading.rawPos
							<< "," << reading.z0
							<< "," << reading.z
							<< "," << reading.speed
							<< "," << reading.firingState
							<< "," << GetFiringStateName(reading.firingState)
							<< ",   " << buf.get()
							<< "\n";
					}
				}
			}

			// copy the data
			memcpy(&this->reading, &reading, sizeof(this->reading));
			this->sensorData = sensorData;

			// record the time
			timestamp = GetTickCount64();

			// done accessing the shared data
			ReleaseMutex(dataMutex);

			// let the main thread know about the update
			PostMessage(hwnd, DeviceThreadWindow::MSG_NEW_DATA, 0, 0);
		}
	}

	// return the result
	return ok;
}
