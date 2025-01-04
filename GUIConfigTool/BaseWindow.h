// Pinscape Pico - Base window class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a base class for interactive Pinscape Pico configuration
// tool windows.

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
#include <Dbt.h>
#include "Dialog.h"
#include "WinUtil.h"

namespace PinscapePico
{
	// Base window
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
		virtual bool OnNCHitTest(int x, int y, LRESULT &lResult) { return false; }
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
		virtual bool OnSysCommand(WPARAM id, WORD x, WORD y, LRESULT &lResult) { return false; }
		virtual void OnVScroll(HWND sb, UINT sbCode, UINT thumbPos);
		virtual void OnHScroll(HWND sb, UINT sbCode, UINT thumbPos);
		virtual void OnMouseWheel(WPARAM keys, int delta, int x, int y);
		virtual bool OnSetCursor(HWND hwndCursor, UINT hitTest, UINT msg);
		virtual bool OnMouseMove(WPARAM keys, int x, int y);
		virtual bool OnLButtonDown(WPARAM keys, int x, int y);
		virtual bool OnLButtonUp(WPARAM keys, int x, int y);
		virtual bool OnRButtonDown(WPARAM keys, int x, int y) { return false; }
		virtual bool OnRButtonUp(WPARAM keys, int x, int y) { return false; }
		virtual bool OnMButtonDown(WPARAM keys, int x, int y) { return false; }
		virtual bool OnMButtonUp(WPARAM keys, int x, int y) { return false; }
		virtual bool OnCaptureChange(HWND hwnd);
		virtual void OnContextMenu(HWND hwndClick, int x, int y);
		virtual bool OnCtlColor(UINT msg, HDC hdc, HWND hwndCtl, HBRUSH &hBrush) { return false; }
		virtual bool OnDeviceChange(WPARAM eventType, void *param, LRESULT &lresult);

		// WM_DEVICECHANGE notification handlers for specific event types
		virtual bool OnDeviceArrival(DEV_BROADCAST_HDR *hdr) { return false; }
		virtual bool OnDeviceQueryRemoveFailed(DEV_BROADCAST_HDR *hdr) { return false; }
		virtual bool OnDeviceRemovePending(DEV_BROADCAST_HDR *hdr) { return false; }
		virtual bool OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr) { return false; }
		virtual bool OnDevNodesChanged() { return false; }

		// Query device removal, for WM_DEVICECHANGE + DBT_DEVICEQUERYREMOVE.
		// Return true to allow the device removal, false to protest it.
		virtual bool OnDeviceQueryRemove(DEV_BROADCAST_HDR *hdr) { return true; }

		// Update command status.  Subclasses should override this to invoke
		// the callback for each command that has an enabled/disabled status.
		// This is called on WM_INITMENUPOPUP, and periodically (on the UI
		// refresh timer) to update the control bar buttons.
		virtual void UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply);

		// common scrollbar handler
		virtual void OnScroll(HWND sbHwnd, UINT sbType, UINT sbCode, UINT thumbPos);

		// Printing/page setup commands.  These invoke the printing framework
		// interfaces below.
		virtual void PrintDocument();
		virtual void PageSetup();

		// Is printing enabled?  This is used to update the visual status
		// of the Print and Page Setup commands in the menus, and to determine
		// if the print dialog should be shown on a Print command.  Always
		// returns false by default, since the base window only provides the
		// framework for printing, and doesn't actually do any printing itself.
		virtual bool IsPrintingEnabled() const { return false; }

		// Get the name of the print document, for use in the print job setup
		virtual std::basic_string<TCHAR> GetPrintDocName() const { return _T(""); }

		// Print the next page.  When printing is enabled, PrintDocument()
		// calls this sequentially for each page, until it returns false to
		// indicate that we've reached the end of the document.  The page
		// numbering starts at 1 for the first page.
		// 
		// If 'skip' is true, it means that this page has been filtered out
		// by the page range selection in the print dialog.  The print loop
		// still invokes the method to print it so that the callee can
		// advance internal print state as needed, but the callee shouldn't
		// actually paint anything into the DC.
		//
		// The base class routine sets up printingContext, draws the header
		// and footer, and calls PrintPageContents() to fill in the main
		// content area.  Most subclasses should be able to use the base
		// PrintPage() and only override PrintPageContents().
		virtual bool PrintPage(HDCHelper &hdc, int pageNumber, bool skip);

		// Print the page contents.  Returns true if there are more pages
		// to print, false if this is the last page.
		virtual bool PrintPageContents(HDCHelper &hdc, int pageNumber, bool skip) { return false; }

		// Create a PageSetupOptions object for the subclass
		struct PageSetupOptions;
		virtual PageSetupOptions *CreatePageSetupOptions() const { return new PageSetupOptions("pageSetup"); }

		// Paginate the document.  This is called from PrintPage() on the
		// first page if we need the page count, such as for the $(numPages)
		// header/footer expansion variable.
		virtual int Paginate(HDCHelper &hdc) { return 1; }

		// Expand a substitution variable (of the form "$(name)") in a printer
		// header/footer string.  The base PrintPage() expands the basic set
		// of variables generic to all documents (date, time, page number,
		// number of pages).  Subclasses can add document-specific variables
		// by overriding this.  Return true if the variable was matched, false
		// if it's unknown.
		virtual bool ExpandHeaderFooterVar(const std::string &varName, std::string &expansion) { return false; }

		// Basic printing context.  This keeps track of page-to-page information
		// during the printing process.
		struct PrintingContext
		{
			// Page setup options, created by the subclass so that it can
			// store a subclassed options object
			std::unique_ptr<PageSetupOptions> opts;

			// number of pages for the current print job, from Paginate()
			int numPages = 1;

			// scaling factors - printer/screen, which is usually >1 (e.g., for
			// a 600 dpi printer and 96 dpi screen, it's 600/96)
			float xScale = 1.0f;
			float yScale = 1.0f;

			// physical paper rectangle (0, 0, HORZRES, VERTRES)
			RECT rcPaper{ 0 };

			// print area rectangle, after deducting space for margins and headers
			RECT rcPrint{ 0 };
		};
		PrintingContext printingContext;

		// Page setup options.  This provides the basic set of options
		// common to most document types, and it can extended with
		// additional properties as needed.
		struct PageSetupOptions
		{
			PageSetupOptions(const char *jsonKey) : jsonKey(jsonKey) { Load(); }

			// the top-level JSON key for storing the configuration settings
			const char *jsonKey;

			// load options from JSON
			virtual void Load();

			// store options to JSON
			virtual void Store();

			// margins
			struct Margin
			{
				float left = 1.0f;
				float right = 1.0f;
				float top = 1.0f;
				float bottom = 1.0f;
			};
			Margin margin;

			// monochrome
			bool monochrome = false;

			// header/footer text
			std::string header;
			std::string footer;
		};

		// Page setup dialog
		class PageSetupDialog : public Dialog
		{
		public:
			PageSetupDialog(HINSTANCE hInstance, PageSetupOptions &opts) : Dialog(hInstance), opts(opts) { }
			virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override;
			virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom) override;

			// options object
			PageSetupOptions &opts;

			// load UI controls from the JSON config/store controls in JSON config
			virtual void LoadControls();
			virtual void StoreControls();
		};

		// host-provided function to call when the window is closed/destroyed
		std::function<void()> callOnClose{ []() { } };
		std::function<void()> callOnDestroy{ []() { } };

		// Show a help file.  This opens the given HTML file in a
		// browser window.  The files are stored the Help\ subfolder
		// of the program executable folder.
		void ShowHelpFile(const char *filename, const char *anchor = nullptr);

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

}
