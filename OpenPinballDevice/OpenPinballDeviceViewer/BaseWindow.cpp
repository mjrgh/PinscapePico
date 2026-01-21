// Open Pinball Device Viewer - Base window class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <io.h>
#include <cstdlib>
#include <math.h>
#include <list>
#include <iterator>
#include <memory>
#include <ctime>
#include <algorithm>
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include <Shlwapi.h>
#include "WinUtil.h"
#include "Utilities.h"
#include "BaseWindow.h"
#include "resource.h"

// include the common controls library and UX theme library
#pragma comment(lib, "Comctl32")
#pragma comment(lib, "uxtheme")
#pragma comment(lib, "Shlwapi")

// add common controls 6.0 to the manifest
#pragma comment(linker, \
	"\"/ManifestDependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' \
    version='6.0.0.0' \
    processorArchitecture='*' \
    publicKeyToken='6595b64144ccf1df' \
    language='*'\"")

// statistics
std::unordered_set<std::basic_string<TCHAR>> BaseWindow::classesRegistered;
HCURSOR BaseWindow::arrowCursor = NULL;

// show an error message box
void BaseWindow::MessageBoxFmtV(HWND parent, DWORD mbIcon, const char *fmt, va_list va)
{
	// make a copy to measure the formatted length
	va_list va2;
	va_copy(va2, va);
	int len = _vscprintf(fmt, va2);
	va_end(va2);

	// allocate space and format the message
	std::unique_ptr<char, void(*)(void*)> buf(reinterpret_cast<char*>(_malloca(len + 1)), [](void *ptr) { _freea(ptr); });
	vsprintf_s(buf.get(), static_cast<size_t>(len) + 1, fmt, va);

	// if 'parent' is itself a child window, walk up the tree until we
	// find a non-child to use as the dialog owner
	while ((GetWindowStyle(parent) & WS_CHILD) != 0)
	{
		if (HWND grandparent = GetParent(parent); grandparent == NULL)
			break;
		else
			parent = grandparent;
	}

	// display the message box
	MessageBoxA(parent, buf.get(), "Open Pinball Device Viewer", MB_OK | mbIcon);
}

void BaseWindow::MessageBoxFmtV(HWND parent, const char *fmt, va_list va)
{
	MessageBoxFmtV(parent, MB_ICONERROR, fmt, va);
}

void BaseWindow::MessageBoxFmt(HWND parent, const char *fmt, ...)
{
	// set up varargs
	va_list va;
	va_start(va, fmt);
	MessageBoxFmtV(parent, fmt, va);
	va_end(va);
}

void BaseWindow::MessageBoxFmt(HWND parent, DWORD mbIcon, const char *fmt, ...)
{
	// set up varargs
	va_list va;
	va_start(va, fmt);
	MessageBoxFmtV(parent, mbIcon, fmt, va);
	va_end(va);
}

void BaseWindow::CopyTextToClipboard(const char *txt, size_t len)
{
	// open the clipboard
	if (OpenClipboard(hwnd))
	{
		// allocate an HGLOBAL for the text plus a null terminator
		if (HGLOBAL hglobal = GlobalAlloc(GMEM_MOVEABLE, len + 1); hglobal != NULL)
		{
			// lock it
			if (char *dst = reinterpret_cast<char*>(GlobalLock(hglobal)) ; dst != nullptr)
			{
				// copy the text plus a trailing null
				memcpy(dst, txt, len);
				dst[len] = 0;

				// unlock the HGLOBAL
				GlobalUnlock(hglobal);

				// delete the previous clipboard contents
				EmptyClipboard();

				// put our text on the clipboard in ANSI text format
				SetClipboardData(CF_TEXT, hglobal);
			}
		}

		// done with the clipboard
		CloseClipboard();
	}
}

BaseWindow::BaseWindow(HINSTANCE hInstance) :
	hInstance(hInstance)
{
}

BaseWindow::~BaseWindow()
{
	// delete any off-screen bitmap
	if (offScreenBitmap != NULL)
		DeleteBitmap(offScreenBitmap);
}

HWND BaseWindow::GetOverlappedParent() const
{
	// traverse the parent chain until we find a non-child window
	for (HWND hwnd = this->hwnd, hwndDesktop = GetDesktopWindow() ; 
		hwnd != NULL && hwnd != hwndDesktop ; hwnd = GetParent(hwnd))
	{
		// If it's not a WS_CHILD window, it's a WS_OVERLAPPED window.
		// (Note that we can't test for the WS_OVERLAPPED bit, because
		// there isn't one - WS_OVERLAPPED is defined as 0 because it's
		// really just the opposite of WS_CHILD.)
		if ((GetWindowStyle(hwnd) & WS_CHILD) == 0)
			return hwnd;
	}

	// no overlapped parent found - use my own handle as the last resort
	return hwnd;
}

HWND BaseWindow::CreateSysWindow(std::shared_ptr<BaseWindow> &self,
	DWORD style, DWORD exStyle, HWND hwndParent,
	const TCHAR *title, int x, int y, int width, int height, int nCmdShow)
{
	// register the window class if this is the first time through
	RegisterWindowClass();

	// Create the window, passing the shared_ptr as the LPARAM.  This
	// will convey the object to the WM_NCCREATE message, allowing the
	// window proc to associate the object with the window handle.
	hwnd = CreateWindowEx(
		exStyle, GetWindowClassName(), title, style, 
		x, y, width, height,
		hwndParent, NULL, hInstance, &self);

	// Show the window
	ShowWindow(hwnd, nCmdShow);

	// return the window handle
	return hwnd;
}

// Register the window class
void BaseWindow::RegisterWindowClass()
{
	const WCHAR *wcname = GetWindowClassName();
	if (classesRegistered.find(wcname) == classesRegistered.end())
	{
		// load the arrow cursor
		arrowCursor = LoadCursor(NULL, IDC_ARROW);

		// Register our window class
		WNDCLASSEX wc;
		wc.cbSize        = sizeof(WNDCLASSEX);
		wc.style         = CS_VREDRAW | CS_HREDRAW;
		wc.lpfnWndProc   = WndProcS;
		wc.cbClsExtra    = 0;
		wc.cbWndExtra    = 0;
		wc.hInstance     = hInstance;
		wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PROGRAM_ICON));
		wc.hCursor       = arrowCursor;
		wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
		wc.lpszMenuName  = NULL;
		wc.lpszClassName = wcname;
		wc.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PROGRAM_ICON));
		RegisterClassEx(&wc);
	}
}

// Window UI activation.  Set focus on the window by default.
void BaseWindow::OnActivateUI(bool isAppActivate)
{
	SetFocus(hwnd);
}

// Paint off-screen.  This creates a bitmap of the screen image for
// copying to the actual window during WM_PAINT.  To maintain a fast
// refresh rate for animation, we invalidate the window on a timer.
// The layout process is fairly time-consuming, and can take so long
// that doing it inline with WM_PAINT can end up saturating the
// window message queue, preventing the control child windows from
// getting redrawn.  (The Windows message queue has a priority scheme,
// and empirically, it seems that parent window drawing can starve
// child window drawing if it's slow enough.) 
// 
// The solution is to do the drawing outside of WM_PAINT, saving the
// result to a memory bitmap.  WM_PAINT can then blt the bitmap to
// the window, which is very fast.  We do the drawing on our periodic
// animation update timer, which invalidates the window as soon as
// we're ready with a new bitmap.  Windows skips missed interval
// timer events, so when the drawing is slower than the timer, this
// arrangement has the natural effect of throttling the animation to
// a pace that we can keep up with, without starving any other message
// receivers.
void BaseWindow::PaintOffScreen()
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// get the window DC
	HDC whdc = GetWindowDC(hwnd);

	// set up an off-screen bitmap and DC
	HDC hdc = CreateCompatibleDC(whdc);
	HBITMAP bitmap = CreateCompatibleBitmap(whdc, crc.right, crc.bottom);
	HGDIOBJ oldBitmap = SelectObject(hdc, bitmap);

	// initialize the DC with default GDI settings and objects
	HBRUSH oldBrush = SelectBrush(hdc, GetStockBrush(WHITE_BRUSH));
	HPEN oldPen = SelectPen(hdc, GetStockPen(NULL_PEN));
	HFONT oldFont = SelectFont(hdc, mainFont);
	COLORREF oldTxColor = SetTextColor(hdc, RGB(0, 0, 0));
	COLORREF oldBkColor = SetBkColor(hdc, RGB(255, 255, 255));
	int oldBkMode = SetBkMode(hdc, TRANSPARENT);

	// do the subclass-specific drawing
	PaintOffScreen(hdc);

	// delete any previous bitmap and save the new one
	if (offScreenBitmap != NULL)
		DeleteBitmap(offScreenBitmap);
	offScreenBitmap = bitmap;

	// restore DC properties and release the window DC
	SelectFont(hdc, oldFont);
	SetBkMode(hdc, oldBkMode);
	SetBkColor(hdc, oldBkColor);
	SetTextColor(hdc, oldTxColor);
	SelectPen(hdc, oldPen);
	SelectBrush(hdc, oldBrush);
	SelectObject(hdc, oldBitmap);
	DeleteDC(hdc);
	ReleaseDC(hwnd, whdc);
}

// painting
void BaseWindow::OnPaint()
{
	// start painting
	PAINTSTRUCT ps;
	HDC whdc = BeginPaint(hwnd, &ps);

	// draw the off-screen bitmap
	if (offScreenBitmap != NULL)
	{
		// get the client rect
		RECT crc;
		GetClientRect(hwnd, &crc);

		// create a compatible DC for the blt source
		HDC hdc = CreateCompatibleDC(whdc);
		HBITMAP oldbmp = SelectBitmap(hdc, offScreenBitmap);

		// copy the off-screen bitmap into the window
		BitBlt(whdc, 0, 0, crc.right, crc.bottom, hdc, 0, 0, SRCCOPY);

		// done with the compatible DC
		SelectBitmap(hdc, oldbmp);
		DeleteDC(hdc);
	}

	// end painting
	EndPaint(hwnd, &ps);
}

// timer handler
void BaseWindow::OnTimer(WPARAM timerID)
{
	switch (timerID)
	{
	case TIMER_ID_REFRESH:
        // update toolbar buttons for the command status
        UpdateCommandStatus([this](int cmd, bool enabled) {
            if (auto it = ctlBarButtonsByCmd.find(cmd); it != ctlBarButtonsByCmd.end())
                it->second->enabled = enabled;
        });

		// if we're not minimized, refresh drawing
		if (!IsIconic(hwnd))
		{
			// rebuild the off-screen bitmap
			PaintOffScreen();

			// invalidate for redraw
			InvalidateRect(hwnd, NULL, FALSE);
		}
		break;
	}
}

// main window proc
LRESULT CALLBACK BaseWindow::WndProcS(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	// on WM_NCREATE, grab the window handle from the CREATESTRUCT
	if (msg == WM_NCCREATE)
	{
		// lpCreateParams from the CREATESTRUCT in the LPARAM contains
		// a pointer to self
		auto *shared = reinterpret_cast<std::shared_ptr<BaseWindow>*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);

		// remember our window handle
		(*shared)->hwnd = hwnd;

		// stash a pointer to our window object in the system window's first
		// user data slot
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shared->get()));

		// SetWindowLongPtr counts as an owner for the shared_ptr, since the
		// system window will hold this reference for the system window's
		// lifetime.  But SetWindowLongPtr can only store the naked pointer,
		// so it doesn't increment the shared_ptr reference count.  To fix
		// this deficiency, add our own explicit self-reference as a proxy
		// for the SetWindowLongPtr ref.  Note that this *looks* like a
		// circular reference, but only superficially; it's stored as part
		// of 'this', which is what makes it circular, but it's really a
		// proxy for the SetWindowLongPtr reference, and we'll explicitly
		// remove the proxy reference when the window is destroyed.  The
		// existence of this shared_ptr reference won't do anything to
		// prevent the system window from being destroyed, which is what
		// breaks the apparent circularity.
		(*shared)->windowLongPtrRef = *shared;
	}

	// get our self pointer from the window's user data area
	auto *self = reinterpret_cast<BaseWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

	// if we don't a 'self' object, go straight to the default system handler
	if (self == nullptr)
		return DefWindowProc(hwnd, msg, wparam, lparam);

	// Keep a self-reference during dispatch, since some message processing
	// can recursively destroy the window, which might otherwise release
	// the last reference.  The reference we add here is on behalf of the
	// locally stacked 'this' pointer, guaranteeing that the object stays
	// alive through the virtual WndProc() and everything it calls.
	std::shared_ptr<BaseWindow> selfRef(self->windowLongPtrRef);

	// dispatch the message to our virtual handler
	return self->WndProc(msg, wparam, lparam);
}

void BaseWindow::OnTabKey(bool shift)
{
	// next/previous valid control
	auto FocusToNext = [this](int i, int di) -> bool
	{
		// do nothing if there are no controls in the tab order list
		if (tabOrder.size() == 0)
			return false;

		// search for a matching control
		for (int j = i + di, n = static_cast<int>(tabOrder.size()) ; j != i ; j += di)
		{
			j = (j < 0 ? n - 1 : j >= n ? 0 : j);
			auto ctl = tabOrder[j];
			if (IsWindowEnabled(ctl) && IsWindowVisible(ctl))
			{
				// set focus here
				SetFocus(ctl);

				// redraw, to show focus
				InvalidateRect(ctl, NULL, TRUE);

				// special handling for some control types
				char cls[32];
				if (RealGetWindowClassA(ctl, cls, _countof(cls)))
				{
					if (strcmp(cls, WC_EDITA) == 0)
					{
						// text box - select all text
						SendMessage(ctl, EM_SETSEL, 0, -1);
					}
				}
				OutputDebugStringA(StrPrintf("Focus to %lx (%s)\n", static_cast<long>(reinterpret_cast<UINT_PTR>(ctl)), cls).c_str());

				// found
				return true;
			}
		}

		// note found
		return false;
	};

	// find the current focus control in the tab order
	HWND focus = GetFocus();
	bool found = false;
	for (int i = 0, n = static_cast<int>(tabOrder.size()) ; i < n ; ++i)
	{
		if (tabOrder[i] == focus)
		{
			// this is the one - seek the next/previous enabled+visible window
			found = FocusToNext(i, shift ? -1 : 1);
			break;
		}
	}

	// if we didn't find a focus control, set focus on the first control
	if (!found)
		FocusToNext(-1, 1);
}

void BaseWindow::OnEnterKey()
{
}

HWND BaseWindow::CreateControl(UINT_PTR id, const char *winClass, const char *title,
	DWORD style, int width, int height)
{
	// Translate the width and height from dialog units to pixels
	height = ControlHeight(height);
	width = ControlWidth(width);

	// create the window
	static const DWORD baseStyles = WS_CHILD | WS_VISIBLE;
	HWND hwndCtl = CreateWindowA(winClass, title, baseStyles | style, xCreateControl, 0, width, height,
		hwnd, reinterpret_cast<HMENU>(id), hInstance, NULL);

	// Advance the x position - this ensures that the controls aren't initially
	// piled up on top of each other, which can cause drawing anomalies during
	// the initial layout.
	xCreateControl += width + 1;

	// set the font
	SendMessage(hwndCtl, WM_SETFONT, reinterpret_cast<WPARAM>(mainFont), FALSE);

	// add to the tab order
	tabOrder.emplace_back(hwndCtl);

	// intercept tab and enter keys in the control
	SetWindowSubclass(hwndCtl, ControlSubclassProcS, 1, reinterpret_cast<DWORD_PTR>(this));

	// enable focus state display for checkboxes and radio buttons
	if (strcmp(winClass, WC_BUTTONA) == 0)
	{
		DWORD btnType = style & BS_TYPEMASK;
		switch (btnType)
		{
		case BS_CHECKBOX:
		case BS_AUTOCHECKBOX:
		case BS_RADIOBUTTON:
		case BS_AUTORADIOBUTTON:
		case BS_PUSHBUTTON:
		case BS_DEFPUSHBUTTON:
			PostMessage(hwndCtl, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
			break;
		}
	}

	// return the window
	return hwndCtl;
}


// control window subclass proc - static entrypoint
LRESULT CALLBACK BaseWindow::ControlSubclassProcS(
	HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass, DWORD_PTR refData)
{
	// get the 'this' pointer from the reference data, and invoke the virtual handler
	auto *self = reinterpret_cast<BaseWindow*>(refData);
	return self->ControlSubclassProc(hwnd, msg, wparam, lparam, idSubclass);
}

// control window subclass proc - virtual handler
LRESULT BaseWindow::ControlSubclassProc(
	HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass)
{
	// check the message
	switch (msg)
	{
	case WM_KEYDOWN:
		switch (wparam)
		{
		case VK_TAB:
			// forward Tab keys to the parent
			return SendMessage(this->hwnd, msg, wparam, lparam);

		case VK_RETURN:
			// for buttons, treat this as a click
			{
				char cls[32];
				RealGetWindowClassA(hwnd, cls, _countof(cls));
				if (strcmp(cls, WC_BUTTONA) == 0)
				{
					DWORD btnType = GetWindowStyle(hwnd) & BS_TYPEMASK;
					if (btnType == BS_PUSHBUTTON || btnType == BS_DEFPUSHBUTTON)
						return SendMessage(hwnd, BM_CLICK, 0, 0);
				}
			}
			break;

		case VK_ESCAPE:
			// set focus on parent window
			SetFocus(GetParent(hwnd));
			return 0;
		}
		break;

	case WM_CHAR:
		switch (wparam)
		{
		case '\t':
		case '\n':
		case '\r':
		case 27:
			// Ignore tabs, escapes, and enter key characters.  We process
			// these as WM_KEYDOWNs instead, but Windows still queues the
			// WM_CHAR anyway thanks to TranslateMessage().  The control
			// message loop annoyingly beeps if we let it see these.
			return 0;
		}
	}

	// forward other messages to the original handler
	return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void BaseWindow::OnCreateWindow()
{
	// initialize the control bar button by-command map
	for (auto &b : ctlBarButtons)
		ctlBarButtonsByCmd.emplace(b.cmd, &b);

	// if there are any control bar buttons, create a tooltip control for them
	if (ctlBarButtons.size() != 0)
		CreateTooltipControl();

	// create our common fonts
	{
		// get the window DC
		WindowDC hdc(hwnd);

		// create the fonts
		mainFont = CreateFontA(
			-MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSY), 72),
			0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
			"Segoe UI");
		boldFont = CreateFontA(
			-MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSY), 72),
			0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
			"Segoe UI");
		
		// get the main font text metrics
		HFONT oldFont = SelectFont(hdc, mainFont);
		GetTextMetrics(hdc, &mainFontMetrics);
		SelectFont(hdc, boldFont);
		GetTextMetrics(hdc, &boldFontMetrics);
		SelectFont(hdc, oldFont);

		// If this is a top-level (overlapped) window, start our refresh
		// timer.  We normally do this in WM_SHOW, so that the timer only
		// runs when the window is visible, but WM_SHOW has some weird
		// special cases for overlapped windows.  In particular, WM_SHOW
		// is never sent to an overlapped window that's initially maximized
		// or minimized.  Fortunately, we never hide the main window, so
		// it's enough to know that the window has been created to consider
		// it visible, and we can thus do our normal WM_SHOW work here
		// instead.
		if ((GetWindowStyle(hwnd) & WS_CHILD) == 0)
			SetTimer(hwnd, TIMER_ID_REFRESH, 32, NULL);
	}
}

void BaseWindow::CreateTooltipControl()
{
	// create the tooltips control if we haven't already
	if (tooltips == NULL)
	{
		tooltips = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL,
			WS_POPUP | TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			hwnd, NULL, hInstance, NULL);
	}
}

void BaseWindow::OnShowWindow(bool show, LPARAM cause)
{
	// start or stop the drawing refresh timer
	if (show)
		SetTimer(hwnd, TIMER_ID_REFRESH, 32, NULL);
	else
		KillTimer(hwnd, TIMER_ID_REFRESH);
}

bool BaseWindow::OnNotify(WPARAM id, NMHDR *nmhdr, LRESULT &lresult)
{
	switch (nmhdr->code)
	{
	case NM_CUSTOMDRAW:
		// custom drawing
		lresult = OnCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(nmhdr));
		return true;
	}

	// not handled
	return false;
}

LRESULT BaseWindow::OnCustomDraw(NMCUSTOMDRAW *cd)
{
	// check the window class
	char cls[32];
	RealGetWindowClassA(cd->hdr.hwndFrom, cls, _countof(cls));
	if (strcmp(cls, WC_BUTTONA) == 0)
	{
		// button - check the type
		DWORD btnType = GetWindowStyle(cd->hdr.hwndFrom) & BS_TYPEMASK;
		switch (btnType)
		{
		case BS_CHECKBOX:
		case BS_AUTOCHECKBOX:
		case BS_RADIOBUTTON:
		case BS_AUTORADIOBUTTON:
			// Override ERASE stage for checkboxes and radio buttons to fill with
			// our window background color, so that we the text label with the
			// window color as background.
			if (cd->dwDrawStage == CDDS_PREERASE)
			{
				FillRect(cd->hdc, &cd->rc, GetStockBrush(WHITE_BRUSH));
				return CDRF_NOTIFYPOSTERASE;
			}
			break;
		}
	}

	// no special override - use the default handling
	return CDRF_DODEFAULT;
}

void BaseWindow::OnNCDestroy()
{
	// clean up GDI objects
	DeleteFont(mainFont);
	DeleteFont(boldFont);

	// call the on-destroy handler
	callOnDestroy();

	// clear the WindowLongPtr storing our self-reference, and release
	// the corresponding std::shared_ptr reference
	SetWindowLongPtr(hwnd, GWLP_USERDATA, NULL);
	windowLongPtrRef.reset();
}

void BaseWindow::UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply)
{
}

void BaseWindow::OnInitMenuPopup(HMENU hMenu, WORD itemPos, bool isSysMenu)
{
	// update the menu with the current computed command status
	UpdateCommandStatus([this, hMenu](int cmd, bool enabled) {
		EnableMenuItem(hMenu, cmd, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_DISABLED));
	});
}

// set a tooltip item
void BaseWindow::SetTooltip(RECT &rc, int id, const char *msg, HWND hwndCtl)
{
	TOOLINFOA ti{ sizeof(ti) };
	ti.uFlags = TTF_SUBCLASS;
	ti.hwnd = hwndCtl != NULL ? hwndCtl : this->hwnd;
	ti.uId = id;
	ti.rect = rc;
	ti.lpszText = const_cast<char*>(msg);
	SendMessage(tooltips, TTM_ADDTOOLA, 0, reinterpret_cast<LPARAM>(&ti));
};


bool BaseWindow::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
	switch (notifyCode)
	{
	case BN_SETFOCUS:
	case BN_KILLFOCUS:
		// button focus change - update the default button styling
		{
			DWORD style = GetWindowStyle(hwndCtl);
			DWORD btnType = style & BS_TYPEMASK;
			if (btnType == BS_PUSHBUTTON || btnType == BS_DEFPUSHBUTTON)
			{
				if (notifyCode == BN_SETFOCUS)
					style |= BS_DEFPUSHBUTTON;
				else
					style &= ~BS_DEFPUSHBUTTON;
				SendMessage(hwndCtl, BM_SETSTYLE, style, TRUE);
			}
		}
		break;
	}

	// use the default handling
	return false;
}

bool BaseWindow::OnKeyDown(WPARAM vkey, LPARAM flags)
{
	// close on Ctrl+W or Ctrl+Q
	switch (vkey)
	{
	case 'W':
	case 'Q':
		if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
		{
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return true;
		}
		break;

	case VK_TAB:
		OnTabKey((GetKeyState(VK_SHIFT) & 0x8000) != 0);
		return 0;

	case VK_RETURN:
		OnEnterKey();
		return true;
	}

	return false;
}

void BaseWindow::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust scrollbar ranges
	AdjustScrollbarRanges();
}

void BaseWindow::AdjustScrollbarRanges()
{
	for (auto &sb : scrollbars)
		sb.AdjustRange();
}

void BaseWindow::OnVScroll(HWND sb, UINT sbCode, UINT thumbPos)
{
	// invoke the common scroll handler for SB_VERT or SB_CTL, as appropriate
	OnScroll(sb, sb == hwnd ? SB_VERT : SB_CTL, sbCode, thumbPos);
}

void BaseWindow::OnHScroll(HWND sb, UINT sbCode, UINT thumbPos)
{
	// invoke the common scroll handler for SB_HORZ or SB_CTL, as appropriate
	OnScroll(sb, sb == hwnd ? SB_HORZ : SB_CTL, sbCode, thumbPos);
}

void BaseWindow::OnScroll(HWND sbHwnd, UINT sbType, UINT sbCode, UINT thumbPos)
{
	// find the scrollbar control in our list
	for (auto &sb : scrollbars)
	{
		if (sb.type == sbType && sb.hwnd == sbHwnd)
		{
			// get the current scroll info
			SCROLLINFO si{ sizeof(si), SIF_TRACKPOS | SIF_RANGE | SIF_POS | SIF_PAGE };
			GetScrollInfo(sbHwnd, sbType, &si);
			int oldPos = si.nPos;
			const int maxPos = si.nMax - max(si.nPage - 1, 0);
			switch (sbCode)
			{
			case SB_BOTTOM:
				si.nPos = si.nMax;
				break;

			case SB_TOP:
				si.nPos = si.nMin;
				break;

			case SB_LINEDOWN:
				si.nPos = min(si.nPos + sb.cyLine, maxPos);
				break;

			case SB_PAGEDOWN:
				si.nPos = min(static_cast<int>(si.nPos + si.nPage), maxPos);
				break;

			case SB_PAGEUP:
				si.nPos = max(static_cast<int>(si.nPos - si.nPage), si.nMin);
				break;

			case SB_LINEUP:
				si.nPos = max(si.nPos - sb.cyLine, si.nMin);
				break;

			case SB_THUMBTRACK:
				si.nPos = si.nTrackPos;
				break;
			}

			// set the new position
			si.fMask = SIF_POS | SIF_DISABLENOSCROLL;
			SetScrollInfo(sbHwnd, sbType, &si, TRUE);

			// update
			sb.SetPos(si.nPos, oldPos - si.nPos);

			// immediately redraw if tracking the thumb; thumb tracking is a modal
			// event loop that doesn't deliver regular paint messages until the
			// user releases the mouse
			if (sbCode == SB_THUMBTRACK)
				UpdateWindow(hwnd);

			// handled - look no further for a handler
			break;
		}
	}
}

void BaseWindow::OnContextMenu(HWND hwndClick, int x, int y)
{
	// get the click location in client coordinates
	POINT pt{ x, y };
	ScreenToClient(hwnd, &pt);

	// check for a context menu
	HWND hwndCommand = hwnd;
	if (HMENU hMenu = GetContextMenu(pt, hwndCommand); hMenu != NULL)
	{
		// track the popup menu
		TrackPopupMenu(GetSubMenu(hMenu, 0),
			TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
			x, y, 0, hwndCommand, NULL);
	}
}

void BaseWindow::OnMouseWheel(WPARAM keys, int delta, int x, int y)
{
	// search for a scrollbar that handles mouse wheel events
	for (auto &sb : scrollbars)
	{
		// try handling it
		if (sb.HandleWheelEvent(hwnd, keys, delta, x, y))
			break;
	}
}



LRESULT BaseWindow::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
	// Process the message
	switch (msg)
	{
	case WM_CREATE:
		OnCreateWindow();
		break;

	case WM_NCCALCSIZE:
		break;

	case WM_NCHITTEST:
		if (LRESULT lresult = 0; OnNCHitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), lresult))
			return lresult;
		break;

	case WM_SIZE:
		OnSizeWindow(wparam, LOWORD(lparam), HIWORD(lparam));
		break;

	case WM_PAINT:
		OnPaint();
		return 0;

	case WM_ERASEBKGND:
		// do nothing - our paint routine covers the whole window, so
		// there's no need to do a separate erase, which can cause 
		// visible artifacts
		return 0;

	case WM_SHOWWINDOW:
		OnShowWindow(wparam != 0, lparam);
		break;

	case WM_WINDOWPOSCHANGED:
		if (OnWindowPosChanged(reinterpret_cast<WINDOWPOS*>(lparam)))
			return 0;
		break;

	case WM_ACTIVATE:
		OnActivate(wparam, reinterpret_cast<HWND>(lparam));
		return 0;

	case WM_TIMER:
		OnTimer(wparam);
		return 0;

	case WM_VSCROLL:
		OnVScroll(reinterpret_cast<HWND>(lparam), LOWORD(wparam), HIWORD(wparam));
		break;

	case WM_HSCROLL:
		OnHScroll(reinterpret_cast<HWND>(lparam), LOWORD(wparam), HIWORD(wparam));
		break;

	case WM_MOUSEWHEEL:
		OnMouseWheel(wparam, static_cast<INT16>(HIWORD(wparam)), GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		break;

	case WM_MOUSEMOVE:
		if (OnMouseMove(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_MOUSELEAVE:
		if (OnMouseLeave())
			return 0;
		break;

	case WM_LBUTTONDOWN:
		if (OnLButtonDown(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_LBUTTONUP:
		if (OnLButtonUp(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_MBUTTONDOWN:
		if (OnMButtonDown(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_MBUTTONUP:
		if (OnMButtonUp(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_RBUTTONDOWN:
		if (OnRButtonDown(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_RBUTTONUP:
		if (OnRButtonUp(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
			return 0;
		break;

	case WM_CAPTURECHANGED:
		if (OnCaptureChange(reinterpret_cast<HWND>(lparam)))
			return 0;
		break;

	case WM_CONTEXTMENU:
		OnContextMenu(reinterpret_cast<HWND>(wparam), GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return 0;

	case WM_INITMENUPOPUP:
		OnInitMenuPopup(reinterpret_cast<HMENU>(wparam), LOWORD(lparam), HIWORD(lparam));
		return 0;

	case WM_NOTIFY:
		if (LRESULT lret = 0; OnNotify(wparam, reinterpret_cast<NMHDR*>(lparam), lret))
			return lret;
		break;

	case WM_COMMAND:
		if (LRESULT lret = 0; OnCommand(HIWORD(wparam), LOWORD(wparam), reinterpret_cast<HWND>(lparam), lret))
			return lret;
		break;

	case WM_SYSCOMMAND:
		if (LRESULT lret = 0; OnSysCommand(wparam, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), lret))
			return lret;
		break;

	case WM_KEYDOWN:
		if (OnKeyDown(wparam, lparam))
			return 0;
		break;

	case WM_KEYUP:
		if (OnKeyUp(wparam, lparam))
			return 0;
		break;

	case WM_SYSKEYDOWN:
		if (OnSysKeyDown(wparam, lparam))
			return 0;
		break;

	case WM_SYSKEYUP:
		if (OnSysKeyUp(wparam, lparam))
			return 0;
		break;

	case WM_CLOSE:
		if (OnClose())
			return 0;
		break;

	case WM_SETCURSOR:
		// show the standard cursor
		if (!IsIconic(hwnd))
			SetCursor(arrowCursor);
		break;

	case WM_DESTROY:
		// window is being destroyed
		OnDestroy();
		break;

	case WM_NCDESTROY:
		OnNCDestroy();
		break;

	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLORSCROLLBAR:
		if (HBRUSH hbr; OnCtlColor(msg, reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam), hbr))
			return reinterpret_cast<LPARAM>(hbr);
		break;
	}

	// do the normal system work if we didn't handle it ourselves
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

bool BaseWindow::OnClose()
{
	// call the host notifier callback
	callOnClose();

	// destroy the window
	DestroyWindow(hwnd);

	// handled
	return true;
}

bool BaseWindow::OnLButtonDown(WPARAM keys, int x, int y)
{
	// check control bar buttons
	POINT pt{ x, y };
	trackingButton = -1;
	for (const auto &b : ctlBarButtons)
	{
		// if it's a click in an enabled button with a valid (positive) command ID,
		// start tracking the button
		if (b.enabled && b.cmd > 0 && PtInRect(&b.rc, pt))
		{
			// remember the button we're tracking, and capture the mouse as long as
			// the mouse button is down
			trackingButton = b.cmd;
			rcTrackingButton = b.rc;
			SetCapture(hwnd);

			// handled
			return true;
		}
	}

	// proceed to the default system handling
	return false;
}


bool BaseWindow::OnLButtonUp(WPARAM keys, int x, int y)
{
    if (trackingButton != -1)
    {
        // if the mouse is still within the selected button, activate the command
        POINT pt{ x, y };
        if (PtInRect(&rcTrackingButton, pt))
            PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(trackingButton, BN_CLICKED), 0);

        // no more tracking command
        trackingButton = -1;
        rcTrackingButton={ 0, 0, 0, 0 };

        // end capture
        ReleaseCapture();
    }

    // proceed to the default system handling
	return false;
}

bool BaseWindow::OnMouseMove(WPARAM keys, int x, int y)
{
	// no special handling
	return false;
}

bool BaseWindow::OnCaptureChange(HWND hwnd)
{
	// cancel any button tracking
	trackingButton = -1;

	// continue with the default handling
	return false;
}

RECT BaseWindow::GetChildControlRect(HWND child) const
{
	// get the child window rect in screen coordinates
	RECT rcChild;
	GetWindowRect(child, &rcChild);

	// get our own client area origin in screen coordinates
	POINT origin{ 0, 0 };
	ClientToScreen(hwnd, &origin);

	// adjust the child window rect to our client coordinates
	OffsetRect(&rcChild, -origin.x, -origin.y);
	return rcChild;
}

bool BaseWindow::IsClientRectHot(const RECT &rc) const
{
	// don't show hotspots if the window is disabled
	if (!IsWindowEnabled(hwnd))
		return false;

	// if a menu window is open, it's not in our window
	HWND hwndPopupMenu = FindWindowEx(NULL, NULL, MAKEINTATOM(0x8000), NULL);
	if (hwndPopupMenu != NULL)
		return false;

	// get the window containing the mouse
	POINT pt;
	GetCursorPos(&pt);
	HWND hwndPt = WindowFromPoint(pt);

	// if it's not this window or one of its children, another window
	// is in the way, so nothing in our own window is "hot"
	for (; hwndPt != NULL && hwndPt != hwnd && (GetWindowStyle(hwnd) & WS_CHILD) != 0; hwndPt = GetParent(hwndPt)) ;
	if (hwndPt != hwnd)
		return false;

	// adjust the point to client coordinates, and text if it's in the rect
	ScreenToClient(hwnd, &pt);
	return PtInRect(&rc, pt);
}

// -------------------------------------------------------------------------
//
// Control bar buttons
//

void BaseWindow::DrawCtlBarButton(HDCHelper &hdc, CtlBarButton &b, int &xLastBtn)
{
	// check for spacers (-1) and placeholders for other controls (other negative values)
	if (b.cmd <= 0)
	{
		// check for visual spacers
		if (b.cmd == -1)
			FillRect(hdc, &b.rc, HBrush(HRGB(0x808080)));

		// skip
		return;
	}

	// get the status
	bool hot = IsClientRectHot(b.rc);
	bool track = (trackingButton == b.cmd);

	// four images per button: normal, hot, track, disabled
	int imageCell = b.imageIndex * 4;
	imageCell += !b.enabled ? 3 : (hot && track) ? 2 : (hot || track) ? 1 : 0;
	int colorIndex = imageCell % 4;

	// if there's a label, extend the background into the text area
	static const DWORD bkg[]{ 0xf0f0f0, 0xffffff, 0xffffc0, 0xf0f0f0 };
	static const DWORD fr[]{ 0xf0f0f0, 0x0080ff, 0x0080ff, 0xf0f0f0 };
	static const DWORD txt[]{ 0x000000, 0x0080ff, 0x0080ff, 0x606060 };
	if (b.label != nullptr)
		FillRect(hdc, &b.rc, HBrush(HRGB(bkg[colorIndex])));

	// draw the icon
	int szBtn = szCtlBarButtons.cy;
	CompatibleDC dcb(hdc);
	dcb.Select(bmpCtlBarButtons);
	BitBlt(hdc, b.rc.left, b.rc.top, szBtn, szBtn, dcb, imageCell*szBtn, 0, SRCCOPY);

	// draw the label, if applicable
	if (b.label != nullptr)
	{
		// draw the label text
		hdc.DrawText((b.rc.left + szBtn + b.rc.right)/2, (b.rc.top + b.rc.bottom - mainFontMetrics.tmHeight)/2,
			0, mainFont, HRGB(txt[colorIndex]), b.label);

		// frame the whole button
		FrameRect(hdc, &b.rc, HBrush(HRGB(fr[colorIndex])));
	}

	// note the last button's right location
	xLastBtn = max(xLastBtn, b.rc.right);
}

// -------------------------------------------------------------------------
//
// Scrollbar helper
//

void BaseWindow::Scrollbar::AdjustRange()
{
	// get the current scroll info
	SCROLLINFO si{ sizeof(SCROLLINFO) };
	si.fMask = SIF_POS;
	GetScrollInfo(hwnd, type, &si);
	int oldPos = si.nPos;

	// set the scroll limits
	si.fMask = SIF_PAGE | SIF_POS | SIF_RANGE | SIF_DISABLENOSCROLL;
	getRange(si);
	SetScrollInfo(hwnd, type, &si, TRUE);

	// get the new position
	si.fMask = SIF_POS;
	GetScrollInfo(hwnd, type, &si);
	if (si.nPos != oldPos)
		SetPos(si.nPos, oldPos - si.nPos);
}

void BaseWindow::Scrollbar::SetPos(int newPos, int deltaPos)
{
	// call the user callback to update their drawing metrics as needed
	setPos(newPos, deltaPos);

	// if automatic window updates are enabled, scroll the affected display region
	if (autoScrollWindow)
	{
		// get the scrolling area relative to the client area
		HWND hwndScroll = GetScrollWin();
		RECT rc;
		GetClientRect(hwndScroll, &rc);
		getScrollRect(&rc);

		// scroll the region
		ScrollWindowEx(hwndScroll, 0, deltaPos, &rc, &rc, NULL, NULL, SW_ERASE | SW_INVALIDATE);
	}
}

void BaseWindow::Scrollbar::GetScrollbarClientRect(RECT *rc)
{
	// start with my own client rect
	GetClientRect(hwnd, rc);

	// for embedded window scrollbars (SB_VERT, SB_HORZ), hwnd is actually
	// the parent window, and the scrollbar attaches to the inside right
	// or bottom edge
	switch (type)
	{
	case SB_VERT:
		// the scrollbar is along the right inside edge of the client area
		rc->left = rc->right - GetSystemMetrics(SM_CXVSCROLL);
		break;

	case SB_HORZ:
		// the scrollbar is along the bottom inside edge of the client area
		rc->top = rc->bottom - GetSystemMetrics(SM_CYHSCROLL);
		break;
	}
}

bool BaseWindow::Scrollbar::HandleWheelEvent(HWND hwndEvent, WPARAM keys, int delta, int x, int y)
{
	// ignore it if we're not configured to handle wheel events, or if
	// the scrollbar is currently disabled or not visible
	if (!this->handleMouseWheel || !IsWindowEnabled(hwnd) || !IsWindowVisible(hwnd))
		return false;

	// Check to see if the point is within the scrolling area
	POINT pt{ x, y };
	HWND hwndScroll = GetScrollWin();
	ScreenToClient(hwndScroll, &pt);
	RECT rc;
	GetClientRect(hwndScroll, &rc);
	getScrollRect(&rc);
	bool inScrollRect = PtInRect(&rc, pt);

	// Check to see if it's within the scrollbar itself.
	pt ={ x, y };
	ScreenToClient(hwnd, &pt);
	GetScrollbarClientRect(&rc);
	bool inScrollbar = PtInRect(&rc, pt);

	// if it's not in either rectangle, ignore the event
	if (!inScrollRect && !inScrollbar)
		return false;

	// get the current scroll position
	SCROLLINFO si{ sizeof(si), SIF_POS | SIF_RANGE | SIF_PAGE };
	GetScrollInfo(hwnd, type, &si);

	// scroll by <wheel> lines
	int oldPos = si.nPos;
	si.nPos -= static_cast<int>(static_cast<float>(delta) / WHEEL_DELTA * cyLine);
	si.fMask = SIF_POS | SIF_DISABLENOSCROLL;

	// force it into range
	si.nPos = max(si.nPos, si.nMin);
	int maxPos = si.nMax - max(si.nPage - 1, 0);
	si.nPos = min(si.nPos, maxPos);

	// only set the new scroll position if it's changed
	if (si.nPos != oldPos)
	{
		// update the position in the scrollbar
		si.fMask = SIF_POS;
		SetScrollInfo(hwnd, type, &si, TRUE);

		// get the final position, in case the scrollbar forced it into range
		GetScrollInfo(hwnd, type, &si);

		// if the position changed, update the window
		if (si.nPos != oldPos)
			SetPos(si.nPos, oldPos - si.nPos);
	}

	// handled
	return true;
}
