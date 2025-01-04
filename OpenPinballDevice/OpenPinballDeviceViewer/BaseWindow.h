// Open Pinball Device Viewer - Base window class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <string>
#include <list>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include "WinUtil.h"

class BaseWindow
{
public:
	// construction
	BaseWindow(HINSTANCE hInstance);

	// destruction
	virtual ~BaseWindow();

	// window handle
	HWND GetHWND() const { return hwnd; }

	// Set an action to perform when the window is closed/destroyed.  
	// The host application can use to exit any mode created when the
	// window is shown, such as re-enabling a parent window (if this
	// window is being presented as a modal dialog) or terminating
	// the application (if this is the only window).
	void CallOnClose(std::function<void()> func) { callOnClose = func; }
	void CallOnDestroy(std::function<void()> func) { callOnDestroy = func; }

	// Create the system window for this object.  The caller must
	// provide a shared pointer to 'this', so that the window can
	// add its own reference to the same pointer.
	HWND CreateSysWindow(std::shared_ptr<BaseWindow> &self,
		DWORD style, DWORD exStyle, HWND hwndParent,
		const TCHAR *title, int x, int y, int width, int height, int nCmdShow);

	// Install this window's menu bar in the specified window.  The 
	// parent application calls this when activating the window's
	// UI.  If this window is used as the main UI window, and is 
	// styled as an overlapped window, the application should simply
	// pass GetHWND() as the window to use for the menu installation. 
	// If this window is used as a child window within a wrapper
	// provided by the application, the host application should pass
	// a handle to its own main window.
	// 
	// Returns true if the window installed a menu, false if not.
	// On a false return, the host application can install its own
	// default menu, or run with no menu.
	virtual bool InstallParentMenuBar(HWND hwndContainer) { return false; }

	// Activate the window's user interface.  If this is used as
	// a child window, the container application should call this
	// when selecting this child window, and when the application
	// itself switches into the foreground while this child window
	// is already active.  The child can use this opportunity to
	// set focus as needed.  isAppActivate means that the whole
	// application is being switched into the foreground.
	virtual void OnActivateUI(bool isAppActivate);
	virtual void OnDeactivateUI() { }

	// Translate keyboard accelerators.  The application's main
	// message loop should call this when this window is active,
	// to allow it to provide keyboard accelerators.  Returns true
	// if the message was translated, false if not.  This should
	// simply return false if the window doesn't provide its own
	// accelerators.
	//
	// hwndMenu is the window that contains the main menu.  This
	// should be used as the command target for a matching
	// accelerator WM_COMMAND.
	virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) { return false; }

	// Process a WM_COMMAND message.  Note that this is public,
	// so that related windows (e.g., parents) and application
	// wrappers can bubble commands through focus chains, parent/
	// child relations. etc.
	virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult);

	// initialize popup menu status
	virtual void OnInitMenuPopup(HMENU menu, WORD itemPos, bool isSysMenu);

protected:
	// Get the top-level ("overlapped") parent window.  This traverses 
	// the parent chain until we find a non-child window.
	HWND GetOverlappedParent() const;

	// Get the dialog box owner to use for dialogs displayed by this window
	HWND GetDialogOwner() const { return GetOverlappedParent(); }

	// static window message handler entrypoint
	static LRESULT CALLBACK WndProcS(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	// member function window handler
	virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);

	// register my window class
	virtual void RegisterWindowClass();
	virtual const TCHAR *GetWindowClassName() const = 0;

	// Get the context menu to handle a click (given in client coordinates).
	// Returns a menu handle is a context menu should be displayed, NULL
	// to do nothing.  hwndCommand can optionally be filled in with the
	// command target window, if different from the current window.  (It's
	// sometimes useful to process commands through a parent or child window,
	// such as when this window is a child of a container window that shows
	// the main UI wrapper.)
	virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) { return NULL; }

	// window classes registered
	static std::unordered_set<std::basic_string<TCHAR>> classesRegistered;

	// message handlers
	virtual void OnCreateWindow();
	virtual bool OnClose();
	virtual void OnDestroy() { }
	virtual void OnNCDestroy();
	virtual void OnActivate(WPARAM code, HWND cause) { }
	virtual void OnShowWindow(bool show, LPARAM source);
	virtual void OnSizeWindow(WPARAM type, WORD width, WORD height);
	virtual bool OnWindowPosChanged(WINDOWPOS *wp) { return false; }
	virtual bool OnNCHitTest(int x, int y, LRESULT &lresult) { return false; }
	virtual void OnPaint();
	virtual void OnTimer(WPARAM timerId);
	virtual LRESULT OnCustomDraw(NMCUSTOMDRAW *);
	virtual bool OnKeyDown(WPARAM vkey, LPARAM flags);
	virtual bool OnKeyUp(WPARAM vkey, LPARAM flags) { return false; }
	virtual bool OnSysKeyDown(WPARAM vkey, LPARAM flags) { return false; }
	virtual bool OnSysKeyUp(WPARAM vkey, LPARAM flags) { return false; }
	virtual void OnTabKey(bool shift);
	virtual void OnEnterKey();
	virtual bool OnNotify(WPARAM id, NMHDR *nmhdr, LRESULT &lresult);
	virtual bool OnSysCommand(WPARAM id, WORD x, WORD y, LRESULT &lresult) { return false; }
	virtual void OnVScroll(HWND sb, UINT sbCode, UINT thumbPos);
	virtual void OnHScroll(HWND sb, UINT sbCode, UINT thumbPos);
	virtual void OnMouseWheel(WPARAM keys, int delta, int x, int y);
	virtual bool OnMouseMove(WPARAM keys, int x, int y);
	virtual bool OnMouseLeave() { return false; }
	virtual bool OnLButtonDown(WPARAM keys, int x, int y);
	virtual bool OnLButtonUp(WPARAM keys, int x, int y);
	virtual bool OnRButtonDown(WPARAM keys, int x, int y) { return false; }
	virtual bool OnRButtonUp(WPARAM keys, int x, int y) { return false; }
	virtual bool OnMButtonDown(WPARAM keys, int x, int y) { return false; }
	virtual bool OnMButtonUp(WPARAM keys, int x, int y) { return false; }
	virtual bool OnCaptureChange(HWND hwnd);
	virtual void OnContextMenu(HWND hwndClick, int x, int y);
	virtual bool OnCtlColor(UINT msg, HDC hdc, HWND hwndCtl, HBRUSH &hbrush) { return false; }

	// Update command status.  Subclasses should override this to invoke
	// the callback for each command that has an enabled/disabled status.
	// This is called on WM_INITMENUPOPUP, and periodically (on the UI
	// refresh timer) to update the control bar buttons.
	virtual void UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply);

	// common scrollbar handler
	virtual void OnScroll(HWND sbHwnd, UINT sbType, UINT sbCode, UINT thumbPos);

	// Get the name of the print document, for use in the print job setup
	virtual std::basic_string<TCHAR> GetPrintDocName() const { return _T(""); }

	// host-provided function to call when the window is closed/destroyed
	std::function<void()> callOnClose{ []() { } };
	std::function<void()> callOnDestroy{ []() { } };

	// Paint off-screen.  This prepares a bitmap with the window
	// contents filled in, for display in the next WM_PAINT.
	//
	// The plain no-arguments version sets up an off-screen bitmap
	// and DC and calls the HDC-argument version.  The subclass
	// only needs to implement the (HDC) function, but can override
	// the plain function as well if it needs to customize the bitmap
	// setup or do other work before or after the bitmap fill.
	virtual void PaintOffScreen();
	virtual void PaintOffScreen(HDC hdc) = 0;

	// off-screen bitmap
	HBITMAP offScreenBitmap = NULL;

	// timer IDs
	static const UINT TIMER_ID_REFRESH = 1;

	// application instance handle
	HINSTANCE hInstance = NULL;

	// window handle
	HWND hwnd = NULL;

	// fonts
	HFONT mainFont = NULL;
	HFONT boldFont = NULL;

	// font metrics
	TEXTMETRIC mainFontMetrics{ 0 };
	TEXTMETRIC boldFontMetrics{ 0 };

	// Tab order.  If the window has child controls, it should add their
	// window handles here, in tab order.  This lets the main window
	// implement Windows dialog tab key processing to move focus through
	// the controls.
	std::vector<HWND> tabOrder;

	// Create a child control.  This sets up the control with our custom
	// subclass proc, to handle dialog-like tab key processing.
	HWND CreateControl(UINT_PTR id, const char *winClass, const char *title,
		DWORD style, int width, int height);

	// next CreateControl x offset
	int xCreateControl = 0;

	// Translate control width/heights from Dialog Units to pixels.  This
	// uses the standard Windows conventions for dialog units: 1 VDU = 1/8
	// character cell height of dialog font (our mainFont); 1 HDU = 1/4
	// average character width.
	int ControlWidth(int hdu) const { return hdu * mainFontMetrics.tmAveCharWidth/4; }
	int ControlHeight(int vdu) const { return vdu * mainFontMetrics.tmHeight/8; }

	// Control window subclass proc.  If desired, child controls can be
	// subclassed with this routine to implement dialog-like tab key processing.
	static LRESULT CALLBACK ControlSubclassProcS(
		HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass, DWORD_PTR refData);

	// virtual handler for the control subclass proc
	virtual LRESULT ControlSubclassProc(
		HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass);

	// shared_ptr self-reference, as a proxy for the SetWindowLongPtr
	// ref we keep in the window object.  This is set throughout the
	// lifetime of the system window, and cleared when the window is
	// destroyed.
	std::shared_ptr<BaseWindow> windowLongPtrRef;

	// arrow cursor
	static HCURSOR arrowCursor;

	// Scrollbar holder.  This can be used to implement a
	// window-frame scrollbar (SB_VERT, SB_HORZ) or a separate
	// scrollbar control (SB_CTL).
	class Scrollbar
	{
	public:
		//
		// User callbacks
		//

		// Get the current range.  Called during adjustment.  This
		// must fill in si.nMin, si.nMax, and si.nPage.
		using GetRangeFunc = std::function<void(SCROLLINFO &si)>;

		// Get the scrolling rectangle within the window client area.
		// On entry, the rectangle is populated with the client area,
		// so if this represents the entire scrolling region, the
		// function can return without making any changes.
		//
		// The scrolling area rectangle is used to determine if the
		// mouse is within the scrolling area for wheel events when
		// handleMouseWheel is true, and sets the graphic area to
		// update on scrolling events when autoScrollWindow is true.
		using GetScrollRectFunc = std::function<void(RECT*)>;

		// Set the new scroll position.  Called on scroll events.
		using SetPosFunc = std::function<void(int newPos, int deltaPos)>;

		// hwnd = parent window for SB_VERT or SB_HORZ, control window for SB_CTL
		// 
		// type = SB_VERT, SB_HORZ, or SB_CTL
		// 
		// cyLine = scroll position change per scrollbar arrow click
		//
		// handleMouseWheel = true if this scrollbar handles mouse-wheel events
		// 
		// autoScrollWindow = true to automatically scroll the window contents
		// (via ScrollWindowEx) on scroll events
		//
		Scrollbar(HWND hwnd, WORD type, int cyLine,
			bool handleMouseWheel, bool autoScrollWindow,
			GetRangeFunc getRange, GetScrollRectFunc getScrollRect, SetPosFunc setPos) :
			hwnd(hwnd), type(type), cyLine(cyLine),
			handleMouseWheel(handleMouseWheel), autoScrollWindow(autoScrollWindow),
			getRange(getRange), getScrollRect(getScrollRect), setPos(setPos) { }

		// adjust the range
		void AdjustRange();

		// set the new scrolling position
		void SetPos(int newPos, int deltaPos);

		// Get the scrolling content window handle.  For SB_VERT and SB_HORZ,
		// the nominal scrollbar hwnd is actually the content window, since
		// the real scrollbar control is part of the window; for SB_CTL, hwnd
		// is a separate child control for the scrollbar, so the content window
		// is the scrollbar control's parent window.
		HWND GetScrollWin() const { return type == SB_CTL ? GetParent(hwnd) : hwnd; }

		// Try handling a mouse wheel event.  Returns true if the event was
		// handled, false if not.  We handle the event if handleMouseWheel
		// is true and the mouse pointer is within our scrolling region or 
		// within the scrollbar itself.
		bool HandleWheelEvent(HWND hwndEvent, WPARAM keys, int delta, int x, int y);

		// Get the rectangle of a scrollbar in terms of its own client
		// area.  For child scrollbars (SB_CTL) this is simply the whole
		// client area of the scrollbar.  For embedded window scrollbars
		// (SB_VERT, SB_HORZ), we figure the inset of the scrollbar from
		// the right or bottom edge.
		void GetScrollbarClientRect(RECT *rc);

		// Scrollbar HWND - parent window for SB_VERT/SB_HORZ, 
		// scrollbar control for SB_CTL
		HWND hwnd;

		// Scrollbar type - SB_VERT, SB_HORZ, SB_CTL
		WORD type;

		// Scroll position change per up/down arrow click
		int cyLine;

		// Does this scrollbar handle mouse-wheel events for the parent window?
		bool handleMouseWheel;

		// Automatically update the window drawing area on scroll events?
		bool autoScrollWindow;

		// User callbacks
		GetRangeFunc getRange;
		GetScrollRectFunc getScrollRect;
		SetPosFunc setPos;
	};

	// scrollbar controls
	std::list<Scrollbar> scrollbars;

	// adjust the ranges for all scrollbars
	void AdjustScrollbarRanges();


	//
	// Control/tool bars.  We have some basic support for custom
	// toolbar buttons.
	//

	// control bar button definition
	struct CtlBarButton
	{
		// Command ID, or:
		//
		//      0  = unpopulated (no button)
		//     -1  = spacer (v
		//  <= -2  = subclass-defined special meaning
		//
		int cmd;

		// image in the 
		int imageIndex;

		// label to draw on the button
		const char *label;

		// tooltip text
		const char *tooltip;

		// true -> this is specially positioned, not by the auto layout
		bool specialPos = false;

		// layout rect; assigned during layout
		RECT rc;

		// button enabled
		bool enabled = true;
	};

	// Control bar button list.  Subclasses using this mechanism can
	// populate this list during construction.
	std::list<CtlBarButton> ctlBarButtons;

	// Map of control bar buttons by ID.  We automatically populate
	// this from ctlBarButtons during OnCreateWindow().
	std::unordered_map<int, CtlBarButton*> ctlBarButtonsByCmd;

	// Button bitmap, arranged in square cells, with four cells per button
	// (normal, hover, pressed, disabled).  The subclass must load this
	// (typically in its constructor or OnCreateWindow()).
	HBITMAP bmpCtlBarButtons = NULL;
	SIZE szCtlBarButtons{ 0, 0 };

	// the button we're currently tracking, as a command ID
	int trackingButton = -1;
	RECT rcTrackingButton{ 0, 0, 0, 0 };

	// Draw a control bar button
	void DrawCtlBarButton(HDCHelper &hdc, CtlBarButton &button, int &xLastBtn);

	// Create the tooltips control.  The base window does this automatically
	// in OnCreateWindow() if there's a control bar; subclasses that don't
	// have control bars but want to use tooltips for other purposes can
	// create one explicitly via this method.
	void CreateTooltipControl();

	// set/update a tooltip
	void SetTooltip(RECT &rc, int id, const char *msg, HWND hwnd = NULL);

	// control bar tooltips
	HWND tooltips = NULL;


	//
	// Utilities
	//

	// Copy plain text to the clipboard
	void CopyTextToClipboard(const std::string &str) { CopyTextToClipboard(str.c_str(), str.size()); }
	void CopyTextToClipboard(const char *txt, size_t len);

	// Determine if the mouse is within a given client rectangle
	// within this window.  This returns true if the mouse is
	// inside this rectangle AND there isn't another window in
	// front of our window.
	bool IsClientRectHot(const RECT &rc) const;

	// display a message box with a printf-style formatted messages
	static void MessageBoxFmtV(HWND parent, DWORD mbIcon, const char *fmt, va_list va);
	static void MessageBoxFmtV(HWND parent, const char *fmt, va_list va);
	static void MessageBoxFmt(HWND parent, DWORD mbIcon, const char *fmt, ...);
	static void MessageBoxFmt(HWND parent, const char *fmt, ...);

	// Get the window rect for a child control relative to our own client rect
	RECT GetChildControlRect(HWND child) const;
};

