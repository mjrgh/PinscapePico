// Pinscape Pico Status LED simulator
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// Displays an on-screen view of the Pinscape Pico RGB status LED.
//
// This is mostly intended as a sample program to demonstrate simple
// use of the Pinscape Pico's Feedback Controller C++ API, but it
// might also be useful as a quick status checker, especially if you
// don't have an RGB LED physically installed on your Pinscape Pico
// hardware, or if it's inconvenient to view the physical LED
// directly because it's locked inside a closed pin cab.
//
// To use the Pinscape Pico C++ API, add the following two lines,
// as shown below in the code preamble:
// 
//    #include "PinscapePicoAPI.h"
//    #pragma comment(lib, "PinscapePicoAPI")
// 
// The first line includes the main Pinscape Pico API header, which
// defines the classes and structures your code uses to access the
// API.  The second line instructs the linker to include the API's
// static-link library (.lib) in your build.  You can accomplish the
// second step by adding the library in the project's property pages
// in Visual Studio, but I find it clearer to embed this instruction
// in the source code, because the other way is pretty well buried in
// the gigantic project options dialog.
// 
// In addition, you'll have to set up the Include and Library paths
// in your Visual Studio project properties to point to the
// corresponding folders in the Pinscape Pico source tree.
//
// Note that the static link library approach isn't the only way to
// build this.  It's clean and convenient, but it has one big snag,
// which is that your build settings must exactly match the library
// build settings that I used to create the API project.  You'll
// get linker errors if you're not using the same settings.  If you
// don't want to copy my settings for some reason, you can avoid the
// link errors by ditching the .lib and instead adding the API's
// .cpp source files directly to your project tree.  If you do that,
// you can remove the #pragma, since the .lib is no longer needed.
//
// WinMain() below shows how to discover the Pinscape Pico devices
// currently connected to the system and access selected device(s).
// The discovery API is:
// 
//   PinscapePico::FeedbackControllerInterface::Enumerate()
// 
// That returns identifying information on each device found,
// including the device's Unit Number, which is the user-configurable
// ID that DOF uses to distinguish the devices.  The Unit Number is 
// what you'll usually want to expose to the user, such as when you 
// ask the user to manually select which device to access.
//
// Once you've enumerated the available units, you can open one or
// more of them via the Open function:
// 
//   PinscapePico::FeedbackControllerInterface::Open()
// 
// That function accepts a descriptor returned from the Enumerate()
// function to specify which device you want to open.  Opening the
// device lets you communicate with the live device over its USB
// connection.  You can open multiple devices simultaneously, and
// multiple applications can access the same device simultaneously
// through the Feedback Controller API without creating any sharing
// conflicts.
// 


#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <list>
#include <memory>
#include <algorithm>
#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <tchar.h>
#include "../WinAPI/FeedbackControllerInterface.h"
#include "resource.h"

#pragma comment(lib, "dwmapi")
#pragma comment(lib, "PinscapePicoAPI")

// application instance handle
HINSTANCE g_hInstance;

// arrow cursor
HCURSOR g_hCursor;

// show unit numbers?
bool showUnitNumbers = true;

// flag: exit the updater thread
volatile bool exitThread = false;

// Pinscape Pico USB interfaces
using FCtl = PinscapePico::FeedbackControllerInterface;
struct Device
{
	Device(int unitNum, FCtl *fctl) : unitNum(unitNum), fctl(fctl), color(RGB(0,0,0)) { }
	int unitNum;
	std::unique_ptr<FCtl> fctl;
	COLORREF color;
};
std::list<Device> g_devices;

// paint
static void OnPaint(HWND hwnd)
{
	// get the client size
	RECT rc;
	GetClientRect(hwnd, &rc);

	// set up the painting context
	PAINTSTRUCT ps;
	HDC whdc = BeginPaint(hwnd, &ps);

	// draw into an off-screen bitmap, to minimize flashing
	HDC hdc = CreateCompatibleDC(whdc);
	HBITMAP bitmap = CreateCompatibleBitmap(whdc, rc.right, rc.bottom);
	HGDIOBJ oldBitmap = SelectObject(hdc, bitmap);

	// white text
	COLORREF oldTxColor = SetTextColor(hdc, RGB(255, 255, 255));
	int oldBkMode = SetBkMode(hdc, TRANSPARENT);

	// divide the window into horizontal/vertical slices for the devices
	LONG width = rc.right - rc.left, height = rc.bottom - rc.top;
	float aspect = static_cast<float>(width) / static_cast<float>(height);
	int ncols, nrows;
	int n = static_cast<int>(g_devices.size());
	if (n >= 4 && aspect >= 0.667f && aspect <= 1.5f)
	{
		// at least four items, roughly squarish - use a grid, sqrt(n) cols
		ncols = static_cast<int>(ceilf(sqrtf(static_cast<float>(n))));
		nrows = (n + ncols - 1) / ncols;
	}
	else if (aspect < 1.0f)
	{
		// taller than wide - arrange in a single column
		ncols = 1;
		nrows = n;
	}
	else
	{
		// wider than tall - arrange in a single row
		ncols = n;
		nrows = 1;
	}

	// figure the row and column sizes
	float dx = static_cast<float>(width) / static_cast<float>(ncols);
	float dy = static_cast<float>(height) / static_cast<float>(nrows);

	// create the unit number font if needed
	HFONT hfont = NULL;
	HGDIOBJ oldFont = NULL;
	if (showUnitNumbers)
	{
		hfont = CreateFont(static_cast<int>(dy*2.0f/3.0f), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
			_T("Tahoma"));
		oldFont = SelectObject(hdc, hfont);
	}

	// draw
	int row = 0, col = 0;
	float x = 0.0f, y = 0.0f;
	for (auto &dev : g_devices)
	{
		// draw it
		int left = static_cast<int>(roundf(x));
		int right = static_cast<int>(roundf(x + dx));
		int top = static_cast<int>(roundf(y));
		int bottom = static_cast<int>(roundf(y + dy));
		RECT lrc{ left, top, col + 1 == ncols ? rc.right : right, row + 1 == nrows ? rc.bottom : bottom };
		HBRUSH br = CreateSolidBrush(dev.color);
		FillRect(hdc, &lrc, br);
		DeleteObject(br);

		// draw the unit number if desired
		if (showUnitNumbers)
		{
			TCHAR buf[16];
			_stprintf_s(buf, _T("%d"), dev.unitNum);
			DrawText(hdc, buf, -1, &lrc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}

		// advance to the next slot
		x += dx;
		if (++col >= ncols)
			col = 0, x = 0.0f, ++row, y += dy;
	}

	// copy the off-screen bitmap into the window
	BitBlt(whdc, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);

	// restore DC properties and close the painting context
	if (showUnitNumbers)
	{
		SelectObject(hdc, oldFont);
		DeleteObject(hfont);
	}
	SetBkMode(hdc, oldBkMode);
	SetTextColor(hdc, oldTxColor);
	SelectObject(hdc, oldBitmap);
	DeleteBitmap(bitmap);
	DeleteDC(hdc);
	EndPaint(hwnd, &ps);
}

// window timer IDs
const int TIMER_ID_REFRESH = 1;

// timer handler
static void OnTimer(HWND hwnd, WPARAM timerID)
{
	switch (timerID)
	{
	case TIMER_ID_REFRESH:
		// invalidate the window so that it refreshes
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	}
}

// tracking move
bool tracking = false;
POINT trackpos;

// main window proc
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	// Pass the message to the DWM handler, and note whether it handled it
	LRESULT dwmResult;
	bool callDWP = !DwmDefWindowProc(hwnd, msg, wparam, lparam, &dwmResult);

	// Process the message
	switch (msg)
	{
	case WM_CREATE:
		// Set up a timer to refresh fast enough to keep up with the
		// LED's flash rates.  This doesn't really have to be quite at 
		// the full video refresh rate, since the flash effects are
		// no faster than about 4 Hz, but it helps maintain consistent
		// timing to use a fairly high refresh rate.
		SetTimer(hwnd, TIMER_ID_REFRESH, 32, NULL);

		// Force a frame change notification via SetWindowPos().  This
		// is the standard formula required when using a custom frame
		// via DwmExtendFrameIntoClientArea() - it triggers the initial
		// non-client area size calculation that sets up the custom
		// frame.
		{
			RECT rc;
			GetWindowRect(hwnd, &rc);
			SetWindowPos(hwnd, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_FRAMECHANGED);
		}

		// proceed to the normal default window handling
		break;

	case WM_ACTIVATE:
		// On activation, tell DWM to extend the frame into the client
		// area.  This lets us paint into the portions of the window
		// that would normally be DWM framing.  For Windows 10/11, this
		// lets us remove the ugly little 8-pixel blank white border at
		// the top that Windows adds for top-level caption-less windows.
		// (The ugly little border is there because Win 10/11's frameless
		// window design sneakily incorporates the top sizing border into
		// the caption bar.  When we remove the caption bar, we still get
		// 8 pixels of top sizing border, which is rendered by default in
		// plain window frame color, white by default.  This looks ugly
		// with this particular application, where we simply want to fill
		// the entire window with the blinking status color.)
		{
			// we don't want any visible framing -> 0 margins
			MARGINS margins{ 0, 0, 0, 0 };
			DwmExtendFrameIntoClientArea(hwnd, &margins);
		}
		break;

	case WM_NCCALCSIZE:
		// Calculate the non-client framing size.  Extend the left, right,
		// and bottom non-client framing into the window for the sizing
		// borders.  The top border doesn't need this treatment since the
		// top sizing border on Win 10/11 is aligned within the visible
		// framing area, overlapping the caption bar area.
		if (wparam != 0)
		{
			auto &rgrc = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
			rgrc.left += 8;
			rgrc.right -= 8;
			rgrc.top += 0;    // NB: the top sizing border is inside the nominal client area
			rgrc.bottom -= 8;
			return 0;
		}
		break;

	case WM_NCHITTEST:
		// Non-client hit testing.  This is necessary because of our
		// customized framing.  This is just the basic formulaic code
		// Microsoft provides in their DWM examples in the SDK.  I don't
		// know why this isn't provided as a system API; I guess MSFT
		// likes to make their programmer customers feel useful by
		// assigning them this kind of rote busywork.
		{
			// get the mouse position
			POINT ptMouse { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

			// Get the window rectangle
			RECT rcWindow;
			GetWindowRect(hwnd, &rcWindow);

			// Get the frame rectangle, adjusted for the style without a caption
			RECT rcFrame ={ 0 };
			AdjustWindowRectEx(&rcFrame, WS_OVERLAPPEDWINDOW & ~WS_CAPTION, FALSE, NULL);

			// Determine if the hit test is for resizing
			USHORT uRow = 1;
			USHORT uCol = 1;
			bool fOnResizeBorder = false;

			// Determine if the point is at the top or bottom of the window
			if (ptMouse.y >= rcWindow.top && ptMouse.y < rcWindow.top + 8)
				uRow = 0;
			else if (ptMouse.y < rcWindow.bottom && ptMouse.y >= rcWindow.bottom - 8)
				uRow = 2;

			// Determine if the point is at the left or right of the window
			if (ptMouse.x >= rcWindow.left && ptMouse.x < rcWindow.left + 8)
				uCol = 0;
			else if (ptMouse.x < rcWindow.right && ptMouse.x >= rcWindow.right - 8)
				uCol = 2;

			// Hit test (HTTOPLEFT, ... HTBOTTOMRIGHT)
			LRESULT hitTests[3][3] =
			{
				{ HTTOPLEFT,    HTTOP,     HTTOPRIGHT    },
				{ HTLEFT,       HTCLIENT,  HTRIGHT       },
				{ HTBOTTOMLEFT, HTBOTTOM,  HTBOTTOMRIGHT },
			};

			return hitTests[uRow][uCol];
		}

	case WM_PAINT:
		OnPaint(hwnd);
		return 0;

	case WM_ERASEBKGND:
		// do nothing - our paint routine covers the whole window, so
		// there's no need to do a separate erase, which can cause 
		// visible artifacts
		return 0;

	case WM_TIMER:
		OnTimer(hwnd, wparam);
		return 0;

	case WM_LBUTTONDOWN:
		tracking = true;
		trackpos.x = GET_X_LPARAM(lparam);
		trackpos.y = GET_Y_LPARAM(lparam);
		SetCapture(hwnd);
		return 0;

	case WM_LBUTTONUP:
		if (tracking)
			ReleaseCapture();
		return 0;

	case WM_CAPTURECHANGED:
		tracking = false;
		break;

	case WM_MOUSEMOVE:
		if (tracking)
		{
			RECT rc;
			int dx = GET_X_LPARAM(lparam) - trackpos.x;
			int dy = GET_Y_LPARAM(lparam) - trackpos.y;
			GetWindowRect(hwnd, &rc);
			SetWindowPos(hwnd, NULL, rc.left + dx, rc.top + dy, -1, -1, SWP_NOZORDER | SWP_NOSIZE | SWP_NOREDRAW);
		}
		return 0;

	case WM_CONTEXTMENU:
		if (HMENU hmenu = LoadMenu(g_hInstance, MAKEINTRESOURCE(IDR_CTXMENU)) ; hmenu != NULL)
		{
			POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			TrackPopupMenu(GetSubMenu(hmenu, 0), TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, NULL);
		}
		return 0;

	case WM_INITMENU:
		CheckMenuItem(reinterpret_cast<HMENU>(wparam), ID_SHOW_UNIT_NUMBERS,
			MF_BYCOMMAND | (showUnitNumbers ? MF_CHECKED : MF_UNCHECKED));
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case ID_FILE_EXIT:
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;

		case ID_SHOW_UNIT_NUMBERS:
			showUnitNumbers = !showUnitNumbers;
			return 0;
		}
		break;

	case WM_KEYDOWN:
		// close on Ctrl+W or Ctrl+Q
		if ((wparam == 'W' || wparam == 'Q') && (GetKeyState(VK_CONTROL) & 0x8000) != 0)
		{
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}
		break;

	case WM_CLOSE:
		// destroy the window on close
		DestroyWindow(hwnd);
		return 0;

	case WM_SETCURSOR:
		// show the standard cursor
		if (!IsIconic(hwnd))
			SetCursor(g_hCursor);
		break;

	case WM_DESTROY:
		// terminate when the window is destroyed
		PostQuitMessage(0);
		return 0;
	}

	// do the normal system work if we didn't handle it ourselves
	return callDWP ? DefWindowProc(hwnd, msg, wparam, lparam) : dwmResult;
}

// Updater thread
static DWORD WINAPI UpdaterThread(void *)
{
	// loop until the exit flag is set
	while (!exitThread)
	{
		// update all devices
		for (auto &dev : g_devices)
		{
			// get this device's status
			FCtl::StatusReport sr;
			COLORREF color = RGB(0, 0, 0);
			if (dev.fctl->QueryStatus(sr, 20))
				dev.color = sr.led;
		}
	}

	// done
	return 0;
}

// Main entrypoint
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	// remember the instance handle globally
	g_hInstance = hInstance;

	// discover Pico devices
	std::list<FCtl::Desc> descs;
	HRESULT hr = FCtl::Enumerate(descs);
	if (!SUCCEEDED(hr))
	{
		TCHAR buf[128];
		_stprintf_s(buf, _T("An error occurred enumerating Pinscape Pico devices (HRESULT %08lx)."), hr);
		MessageBox(NULL, buf, _T("Pinscape Pico"), MB_OK | MB_ICONERROR);
		return 1;
	}
	if (descs.size() == 0)
	{
		MessageBox(NULL, _T("No Pinscape Pico devices were found."), _T("Pinscape Pico"), MB_OK | MB_ICONERROR);
		return 1;
	}

	// parse the command line
	for (int i = 1 ; i < __argc ; ++i)
	{
		const char *p = __argv[i];
		if (strcmp(p, "--no-unit-numbers") == 0)
		{
			showUnitNumbers = false;
		}
		else
		{
			MessageBox(NULL, _T("Command line error - options:\n\n")
				_T("  --no-unit-numbers      - don't show unit numbers\n\n"),
				_T("Pinscape Pico"), MB_OK | MB_ICONERROR);
			return 1;
		}
	}

	// sort the list in ascending unit number order
	descs.sort([](const FCtl::Desc &a, const FCtl::Desc &b) { return a.unitNum < b.unitNum; });

	// open the devices
	for (auto &desc : descs)
	{
		if (auto *dev = FCtl::Open(desc) ; dev != nullptr)
		{
			// add the device to our list
			g_devices.emplace_back(desc.unitNum, dev);
		}
		else
		{
			TCHAR buf[128];
			_stprintf_s(buf, _T("Unable to open USB connection to Pinscape Pico unit %d\n"), desc.unitNum);
			MessageBox(NULL, buf, _T("Pinscape Pico"), MB_OK | MB_ICONERROR);
			return 1;
		}
	}

	// start the updater thread
	DWORD threadID;
	HANDLE hThread = CreateThread(NULL, 0, UpdaterThread, nullptr, 0, &threadID);
	if (hThread == NULL)
	{
		MessageBox(NULL, _T("Unable to create device status monitor thread"), _T("Pinscape Pico"), MB_OK | MB_ICONERROR);
		return 1;
	}

	// load the arrow cursor
	g_hCursor = LoadCursor(NULL, IDC_ARROW);

	// Register our window class
	WNDCLASSEX wc;
	wc.cbSize        = sizeof(WNDCLASSEX);
	wc.style         = 0;
	wc.lpfnWndProc   = WndProc;
	wc.cbClsExtra    = 0;
	wc.cbWndExtra    = 0;
	wc.hInstance     = hInstance;
	wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	wc.hCursor       = g_hCursor;
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszMenuName  = NULL;
	wc.lpszClassName = _T("PinscapePicoStatus");
	wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
	RegisterClassEx(&wc);

	// Create window
	HWND hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, _T("PinscapePicoStatus"), _T("Pinscape Pico Status"), 
		WS_POPUP | WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT,
		300, 200, NULL, NULL, hInstance, NULL);

	// Show window
	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// Main message Loop
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// tell the updater thread to exit, and wait for it to terminate
	exitThread = true;
	WaitForSingleObject(hThread, 1000);

	// exit with the parameter from the final WM_QUIT message
	return static_cast<int>(msg.wParam);
}
