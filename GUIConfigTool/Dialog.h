// Pinscape Pico Config Tool - Dialog Box helpers
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Classes and utilities for implementing Windows dialog boxes

#pragma once
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <tchar.h>
#include <memory>
#include <unordered_map>
#include <list>
#include <functional>
#include <string>
#include <Windows.h>
#include <Shlwapi.h>
#include <shobjidl_core.h>
#include "resource.h"


using TSTRING = std::basic_string<TCHAR>;

class Dialog
{
public:
	Dialog(HINSTANCE hInstance);
	virtual ~Dialog();

	// map an HWND to a Dialog object
	static Dialog *FromHWND(HWND hwnd);

	// get the dialog window handle
	HWND GetHWND() const { return hDlg; }

	// Shared pointer reference.  This is useful for modeless dialogs
	// where the owner window keeps its own reference to the dialog.
	// The owner can make a copy of this in its own shared pointer to
	// keep the dialog object alive as long as the owner maintains its
	// reference, OR the dialog window still exists.  We release our
	// own reference when the dialog is destroyed.
	//
	// This isn't populated by default, because we leave it up to the
	// client to decide how the dialog object memory is managed.
	// The client can populate this if desired, and we'll clear it
	// on destroying the window if it's set.
	std::shared_ptr<Dialog> sharedSelfRef;

	// show the dialog modelessly
	virtual HWND OpenModeless(int resourceID, HWND hwndParent = GetActiveWindow());

	// show the dialog
	virtual int Show(int resourceID, HWND hwndParent = GetActiveWindow());

	// Show the dialog, replacing the font specified in the dialog resource
	// with the system Message Box font.
	virtual int ShowWithMessageBoxFont(int resource_id);

	// Show the dialog, replacing the font specified in the dialog resource
	// with the specified dynamically loaded font.
	virtual int ShowWithFont(int resource_id, const LOGFONTW *fontDesc);

	// get a dialog item
	HWND GetDlgItem(int ctlID) const { return ::GetDlgItem(hDlg, ctlID); }

	// Get the client rectangle of a control in global (screen) 
	// coordinates.
	RECT GetCtlScreenRect(HWND ctl) const;

	// Format text for a control, using the control's current text as the
	// printf-style format string.
	void FormatFromDlgItemText(int ctlID, ...);

	// Set a dialog item's text using a printf-style format string
	void SetDlgItemTextFmt(int ctlID, const TCHAR *fmt, ...);

	// get/set a control's text
	std::string GetDlgItemTextA(int ctlID) const;
	std::wstring GetDlgItemTextW(int ctlID) const;
	void SetDlgItemText(int ctlID, const TCHAR *txt) { ::SetDlgItemText(hDlg, ctlID, txt); }

	// Command processing.  These are mostly particularly useful for
	// implementing context menus within the dialog, and for accelerator
	// key handling within the dialog.
    virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom) { return false; }
    virtual bool OnNotify(NMHDR *nm, LRESULT &lresult) { return false; }

protected:
	// Pre-translate a message within the dialog's modal message loop.
	// Returns true if the message was fully handled, false if it should
	// be run through the normal Windows message translation and dispatch.
	virtual bool PreTranslate(MSG *msg);

	// dialog box procedure - static entrypoint
	static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT message, WPARAM wparam, LPARAM lparam);

	// GetMessage hook procedure.  We use this to intercept dialog
	// messages before dispatch for processing through PreTranslate().
	static LRESULT CALLBACK GetMsgHook(int code, WPARAM wparam, LPARAM lparam);

	// dialog box procedure - virtual method entrypoint
	virtual INT_PTR Proc(UINT message, WPARAM wParam, LPARAM lParam);

	// Handle WM_INITDIALOG.  By default, this simply returns TRUE
	// to allow the system dialog proc to set the default focus.
	virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) { return TRUE; }

	// destroy
	virtual void OnDestroy();

	// initialize a menu when it first appears
    virtual void OnInitMenuPopup(HMENU hmenu, UINT itemIndex, bool isWindowMenu) { }

	// Owner-draw operations.  If these return true, the window proc returns 0
	// immediately; otherwise it proceeds to the inherited handling and default
	// system window proc.
    virtual bool OnMeasureItem(WPARAM ctlId, MEASUREITEMSTRUCT *mi) { return false; }
    virtual bool OnDrawItem(WPARAM ctlId, DRAWITEMSTRUCT *di) { return false; }
    virtual bool OnDeleteItem(WPARAM ctlId, DELETEITEMSTRUCT *di) { return false; }

	// Resize a static text element vertically so that it's tall enough
	// to fit its text contents.  Returns the change in height from the
	// original control height.
	int ResizeStaticToFitText(HWND ctl, const TCHAR *txt);

	// Move a control by the given distance
	void MoveCtlBy(int ctlID, int dx, int dy);

	// Expand the window by the given delta
	void ExpandWindowBy(int dx, int dy);

	// application instance handle
	HINSTANCE hInstance;

 	// Dialog window handle
    HWND hDlg = NULL;

    // Accelerator table handle.  If desired, the caller can provide
    // an accelerator that we'll use to translate keystrokes into 
    // commands while the dialog is active.
    HACCEL hAccel = NULL;

	// HWND -> Dialog* mapping table
	static std::unordered_map<HWND, Dialog*> hwndToDialog;

	// GetMessage hook stack.  Each time we show a modal dialog, we create
	// a GetMessage hook to intercept messages going to the dialog window's
	// nested modal message loop, so that we can do our own processing
	// on selected messages before the modal loop dispatches them.  The
	// win32 hook mechanism is pretty archaic in design and doesn't provide
	// any context information to the hook callback, so we have to infer
	// which hook is being invoked from global static information. 
	// Fortunately, this is straightforward for dialog boxes, in that
	// they're inherently nested.  That means we can simply keep a stack
	// of the hooks, and the top of the stack is always always the current
	// one.
	struct HookInfo
	{
		HookInfo(HHOOK hook, Dialog *dlg) : hook(hook), dlg(dlg) { }
		HHOOK hook;
		Dialog *dlg;
	};
	static std::list<HookInfo> hooks;
};


// --------------------------------------------------------------------------
// 
// Message Box-like dialogs.  These dialogs have the following fixed
// resource IDs:
//
//    IDOK           - OK/Close button
//    IDC_TXT_ERROR  - main error text
//    IDC_ERROR_ICON - error icon
//    IDC_BOTTOM_BAR - bottom bar containing the buttons
//


class MessageBoxLikeDialog : public Dialog
{
public:
	MessageBoxLikeDialog(HINSTANCE hInstance, int bitmap_id, bool useSystemMessageBoxFont);

	virtual int Show(int resource_id)
	{
		return useSystemMessageBoxFont ? ShowWithMessageBoxFont(resource_id) : __super::Show(resource_id);
	}

	virtual ~MessageBoxLikeDialog()
	{
		DeleteObject(icon);
		DeleteObject(bkgBrush);
		DeleteObject(faceBrush);
	}

protected:
	virtual INT_PTR Proc(UINT message, WPARAM wParam, LPARAM lParam);

	// use the system message box font?
	bool useSystemMessageBoxFont;

	// error/warning icon
	HBITMAP icon;

	// background color brush - uses the system window background color
	HBRUSH bkgBrush;

	// 3D face brush, per system parameters
	HBRUSH faceBrush;
};

// --------------------------------------------------------------------------
//
// Custom message box.  Looks like a system message box, but the caller
// can specify the font and override the dialog proc.
//
class CustomMessageBox : public MessageBoxLikeDialog
{
public:
	CustomMessageBox(HINSTANCE hInstance, int iconID, int messageStringID, bool useSysMsgBoxFont);
	CustomMessageBox(HINSTANCE hInstance, int iconID, const TCHAR *message, bool useSysMsgBoxFont) :
		MessageBoxLikeDialog(hInstance, iconID, useSysMsgBoxFont),
		message(message)
	{
	}

	// show an error alert using our custom message box
	static int ErrorMsg(HINSTANCE hInstance, const TCHAR *fmt, ...)
	{
		// format the message
		TCHAR buf[4096];
		va_list va;
		va_start(va, fmt);
		_vstprintf_s(buf, fmt, va);
		va_end(va);

		// display the message box
		return RunOK(hInstance, IDB_ERROR, buf);
	}

	// show a message box using our custom font
	static int RunOK(HINSTANCE hInstance, int iconID, const TCHAR *message) { 
		return Run(hInstance, IDD_MSGBOX, iconID, message); 
	}
	static int Run(HINSTANCE hInstance, int dialogID, int iconID, const TCHAR *message)
	{
		CustomMessageBox m(hInstance, iconID, message, false);
		return m.Show(dialogID);
	}

	static int RunFmt(HINSTANCE hInstance, int dialogID, int iconID, const TCHAR *fmt, ...)
	{
		// format the message
		TCHAR buf[4096];
		va_list va;
		va_start(va, fmt);
		_vstprintf_s(buf, fmt, va);
		va_end(va);

		// display the message box
		return Run(hInstance, dialogID, iconID, buf);
	}

protected:
	virtual INT_PTR Proc(UINT message, WPARAM wParam, LPARAM lParam);

	// main text message
	TSTRING message;
};

// --------------------------------------------------------------------------
//
// Message box with checkbox.  This simulates a message box, but adds a
// check box at the bottom.  These are usually used for a message like
// "Don't show me this message again".
//
class MessageBoxWithCheckbox : public MessageBoxLikeDialog
{
public:
	MessageBoxWithCheckbox(HINSTANCE hInstance, int iconID, const TCHAR *message, const TCHAR *checkboxLabel, bool useSysMsgBoxFont) :
		MessageBoxLikeDialog(hInstance, iconID, useSysMsgBoxFont),
		message(message), checkboxLabel(checkboxLabel), isCheckboxChecked(false)
	{
	}

	// is the checkbox checked?
	bool IsCheckboxChecked() const { return isCheckboxChecked; }

protected:
	virtual INT_PTR Proc(UINT message, WPARAM wParam, LPARAM lParam);

	// main text message
	TSTRING message;

	// checkbox label, set at construction
	TSTRING checkboxLabel;

	// is the checkbox checked?
	bool isCheckboxChecked;
};

// -----------------------------------------------------------------------
//
// GetOpenFileName dialog helper
//
class GetFileNameDlg
{
public:
	GetFileNameDlg(const TCHAR *title, DWORD flags,
		const TCHAR *filters = _T("All Files\0*.*\0"), const TCHAR *defaultExtension = _T(""),
		const TCHAR *initialFilename = nullptr, const TCHAR *initialDir = nullptr,
		size_t customFilterBufferSize = 0);

	// Vista new-style Open/Save dialog
	bool Open(HWND hwndOwner);
	bool Save(HWND hwndOwner);

	// folder picker dialog
	bool SelectFolder(HWND hwndOwner);

	// get the filename result
	const TCHAR *GetFilename() const { return filename.get(); }

protected:
	// common dialog handler for Open and Save
	bool ShowV(HWND hwndOwner, IFileDialog *pdf);

	// show an error
	virtual bool Error(HWND hwndOwner, HRESULT hr);

	// hook procedure
	static UINT_PTR CALLBACK HookProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		// On initialization, we get the OPENFILENAME struct used to create
		// the dialog, which includes our hook data value, which is our 'this'
		// pointer (as an LPARAM).  We can thus recover our 'this' pointer
		// and save it in the dialog DWLP_USER slot.
		if (msg == WM_INITDIALOG)
		{
			// retrieve 'this' from the OPENFILENAME struct, and save it in
			// the dialog window's DWLP_USER slot
			auto ofn = reinterpret_cast<OPENFILENAME*>(lparam);
			auto self = reinterpret_cast<GetFileNameDlg*>(ofn->lCustData);
			SetWindowLongPtr(hwnd, DWLP_USER, ofn->lCustData);

			// Subclass the parent window (hwnd is a child dialog that the
			// real dialog creates for us, so the real dialog is the parent).
			// This is the only way we can set the window position when it's
			// first shown, since the dialog will undo any position we set
			// here or in the CDN_INITDONE handler by calling SetWindowPos()
			// on its own later on.  The only good way to customize the
			// dialog position is to hook into WM_WINDOWPOSCHANGED in the
			// subclassed window proc.
			HWND hwndDlg = GetParent(hwnd);
			SetProp(hwndDlg, _T("OrigWndProc"), reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwndDlg, GWLP_WNDPROC)));
			SetWindowLongPtr(hwndDlg, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassWindowProc));
		}

		// get the 'this' pointer from DWLP_USER, and invoke the virtual hook proc
		auto self = reinterpret_cast<GetFileNameDlg*>(GetWindowLongPtr(hwnd, DWLP_USER));
		if (self != nullptr)
			return self->Hook(hwnd, msg, wparam, lparam);
		else
			return 0;
	}

	// virtual hook procedure
	virtual UINT_PTR Hook(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	// Approve a file selection.  This is called when the user clicks the OK
	// button to select a file through the dialog.  Return true to allow the
	// selection and dismiss the dialog, false to prevent the dialog from
	// closing.  If returning false, you should also do something to let the
	// user know what's wrong - typically just display a message box with an
	// error message.
	virtual bool ApproveFile(OFNOTIFY *ofn)
	{
		// by default, approve all selections
		return true;
	}


	// subclassed dialog window proc
	static LRESULT CALLBACK SubclassWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		// get the original window proc
		auto wndproc = reinterpret_cast<WNDPROC>(GetProp(hwnd, _T("OrigWndProc")));

		// check for custom messages
		if (msg == WM_WINDOWPOSCHANGED)
		{
			// We only want to do this on the initial positioning, so remove
			// our hook.
			SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc));

			// get the work area for the monitor containing the window
			HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi ={ sizeof(mi) };
			GetMonitorInfo(hmon, &mi);

			// center the dialog window in the monitor area
			RECT rc;
			GetWindowRect(hwnd, &rc);
			int x = ((mi.rcWork.right - mi.rcWork.left) - (rc.right - rc.left)) / 2;
			int y = ((mi.rcWork.bottom - mi.rcWork.top) - (rc.bottom - rc.top)) / 2;
			SetWindowPos(hwnd, NULL, x, y, -1, -1, SWP_NOZORDER | SWP_NOSIZE);
		}

		// call the old window proc
		return wndproc(hwnd, msg, wparam, lparam);
	}

	// handle custom initialization after the system dialog has started up
	virtual void OnInitDone(HWND hwnd)
	{
	}

	// dialog title
	TSTRING title;

	// filter strings - e.g., "C++ Files\0.cxx\0All Files\0*.*\0"
	TSTRING filters;

	// default extension
	TSTRING defExt;

	// filename buffer
	std::unique_ptr<TCHAR> filename;
	size_t filenameBufSize;

	// Initial folder.  This is the UNCONDITIONAL initial folder, which
	// overrides any system memory of the last folder for this dialog.
	TSTRING initialDir;

	// flags (OFN_xxx)
	DWORD flags;

	// Custom filters.  This is allocated, if desired, with space to hold
	// user-defined file filters across dialog invocations.
	std::unique_ptr<TCHAR> customFilter;

	// maximum size of custom filter buffer
	size_t maxCustomFilter;
};


// --------------------------------------------------------------------------
//
// Common Dialog helper
//
// The dialog templates for the common dialogs are published, so they're
// effectively part of the API.  Each control in each dialog has a well-
// defined ID that user programs can use to manipulate the controls to
// customize the dialogs.  The SDK headers define the IDs for these
// controls in a very problematic way, though: the IDs are short symbols
// like "chx1" and "edt2", and they're defined as #define macros.  It's
// really annoying to have this huge bunch of short symbol names in the
// global namespace.  To work around this, we define proper C++ const
// int symbols, scoped to a namespace, corresponding to the dlgs.h
// macros.
//

// Include the dlgs.h macros.  This must be done AFTER including Dialog.h,
// because Dialog.h wants to use the macro names for C++ symbols, and we
// can't do that if they're defined in the preprocessor first.
#include <dlgs.h>

namespace CommDlgCtl
{
	// Now define constants for all of the dlgs.h macros, and then undef
	// all of the dlgs.h macros.  This lets us use the macro names as C++
	// symbols again.  Needless to say, this was mechanically generated.
	// The dlgs.h SDK header tends to be very stable, since they defined
	// a large set of generic controls that can be reused across all of the
	// common dialogs, so I doubt this will ever need to be expanded; but if
	// new control IDs are ever added, and we need to address those IDs, they
	// can easily be added here by repeating the same pattern for the new IDs.
	const int DLGS_H_ctlFirst = ctlFirst;
#undef ctlFirst
	const int DLGS_H_ctlLast = ctlLast;
#undef ctlLast
	const int DLGS_H_psh1 = psh1;
#undef psh1
	const int DLGS_H_psh2 = psh2;
#undef psh2
	const int DLGS_H_psh3 = psh3;
#undef psh3
	const int DLGS_H_psh4 = psh4;
#undef psh4
	const int DLGS_H_psh5 = psh5;
#undef psh5
	const int DLGS_H_psh6 = psh6;
#undef psh6
	const int DLGS_H_psh7 = psh7;
#undef psh7
	const int DLGS_H_psh8 = psh8;
#undef psh8
	const int DLGS_H_psh9 = psh9;
#undef psh9
	const int DLGS_H_psh10 = psh10;
#undef psh10
	const int DLGS_H_psh11 = psh11;
#undef psh11
	const int DLGS_H_psh12 = psh12;
#undef psh12
	const int DLGS_H_psh13 = psh13;
#undef psh13
	const int DLGS_H_psh14 = psh14;
#undef psh14
	const int DLGS_H_pshHelp = pshHelp;
#undef pshHelp
	const int DLGS_H_psh16 = psh16;
#undef psh16
	const int DLGS_H_chx1 = chx1;
#undef chx1
	const int DLGS_H_chx2 = chx2;
#undef chx2
	const int DLGS_H_chx3 = chx3;
#undef chx3
	const int DLGS_H_chx4 = chx4;
#undef chx4
	const int DLGS_H_chx5 = chx5;
#undef chx5
	const int DLGS_H_chx6 = chx6;
#undef chx6
	const int DLGS_H_chx7 = chx7;
#undef chx7
	const int DLGS_H_chx8 = chx8;
#undef chx8
	const int DLGS_H_chx9 = chx9;
#undef chx9
	const int DLGS_H_chx10 = chx10;
#undef chx10
	const int DLGS_H_chx11 = chx11;
#undef chx11
	const int DLGS_H_chx12 = chx12;
#undef chx12
	const int DLGS_H_chx13 = chx13;
#undef chx13
	const int DLGS_H_chx14 = chx14;
#undef chx14
	const int DLGS_H_chx15 = chx15;
#undef chx15
	const int DLGS_H_chx16 = chx16;
#undef chx16
	const int DLGS_H_rad1 = rad1;
#undef rad1
	const int DLGS_H_rad2 = rad2;
#undef rad2
	const int DLGS_H_rad3 = rad3;
#undef rad3
	const int DLGS_H_rad4 = rad4;
#undef rad4
	const int DLGS_H_rad5 = rad5;
#undef rad5
	const int DLGS_H_rad6 = rad6;
#undef rad6
	const int DLGS_H_rad7 = rad7;
#undef rad7
	const int DLGS_H_rad8 = rad8;
#undef rad8
	const int DLGS_H_rad9 = rad9;
#undef rad9
	const int DLGS_H_rad10 = rad10;
#undef rad10
	const int DLGS_H_rad11 = rad11;
#undef rad11
	const int DLGS_H_rad12 = rad12;
#undef rad12
	const int DLGS_H_rad13 = rad13;
#undef rad13
	const int DLGS_H_rad14 = rad14;
#undef rad14
	const int DLGS_H_rad15 = rad15;
#undef rad15
	const int DLGS_H_rad16 = rad16;
#undef rad16
	const int DLGS_H_grp1 = grp1;
#undef grp1
	const int DLGS_H_grp2 = grp2;
#undef grp2
	const int DLGS_H_grp3 = grp3;
#undef grp3
	const int DLGS_H_grp4 = grp4;
#undef grp4
	const int DLGS_H_frm1 = frm1;
#undef frm1
	const int DLGS_H_frm2 = frm2;
#undef frm2
	const int DLGS_H_frm3 = frm3;
#undef frm3
	const int DLGS_H_frm4 = frm4;
#undef frm4
	const int DLGS_H_rct1 = rct1;
#undef rct1
	const int DLGS_H_rct2 = rct2;
#undef rct2
	const int DLGS_H_rct3 = rct3;
#undef rct3
	const int DLGS_H_rct4 = rct4;
#undef rct4
	const int DLGS_H_ico1 = ico1;
#undef ico1
	const int DLGS_H_ico2 = ico2;
#undef ico2
	const int DLGS_H_ico3 = ico3;
#undef ico3
	const int DLGS_H_ico4 = ico4;
#undef ico4
	const int DLGS_H_stc1 = stc1;
#undef stc1
	const int DLGS_H_stc2 = stc2;
#undef stc2
	const int DLGS_H_stc3 = stc3;
#undef stc3
	const int DLGS_H_stc4 = stc4;
#undef stc4
	const int DLGS_H_stc5 = stc5;
#undef stc5
	const int DLGS_H_stc6 = stc6;
#undef stc6
	const int DLGS_H_stc7 = stc7;
#undef stc7
	const int DLGS_H_stc8 = stc8;
#undef stc8
	const int DLGS_H_stc9 = stc9;
#undef stc9
	const int DLGS_H_stc10 = stc10;
#undef stc10
	const int DLGS_H_stc11 = stc11;
#undef stc11
	const int DLGS_H_stc12 = stc12;
#undef stc12
	const int DLGS_H_stc13 = stc13;
#undef stc13
	const int DLGS_H_stc14 = stc14;
#undef stc14
	const int DLGS_H_stc15 = stc15;
#undef stc15
	const int DLGS_H_stc16 = stc16;
#undef stc16
	const int DLGS_H_stc17 = stc17;
#undef stc17
	const int DLGS_H_stc18 = stc18;
#undef stc18
	const int DLGS_H_stc19 = stc19;
#undef stc19
	const int DLGS_H_stc20 = stc20;
#undef stc20
	const int DLGS_H_stc21 = stc21;
#undef stc21
	const int DLGS_H_stc22 = stc22;
#undef stc22
	const int DLGS_H_stc23 = stc23;
#undef stc23
	const int DLGS_H_stc24 = stc24;
#undef stc24
	const int DLGS_H_stc25 = stc25;
#undef stc25
	const int DLGS_H_stc26 = stc26;
#undef stc26
	const int DLGS_H_stc27 = stc27;
#undef stc27
	const int DLGS_H_stc28 = stc28;
#undef stc28
	const int DLGS_H_stc29 = stc29;
#undef stc29
	const int DLGS_H_stc30 = stc30;
#undef stc30
	const int DLGS_H_stc31 = stc31;
#undef stc31
	const int DLGS_H_stc32 = stc32;
#undef stc32
	const int DLGS_H_lst1 = lst1;
#undef lst1
	const int DLGS_H_lst2 = lst2;
#undef lst2
	const int DLGS_H_lst3 = lst3;
#undef lst3
	const int DLGS_H_lst4 = lst4;
#undef lst4
	const int DLGS_H_lst5 = lst5;
#undef lst5
	const int DLGS_H_lst6 = lst6;
#undef lst6
	const int DLGS_H_lst7 = lst7;
#undef lst7
	const int DLGS_H_lst8 = lst8;
#undef lst8
	const int DLGS_H_lst9 = lst9;
#undef lst9
	const int DLGS_H_lst10 = lst10;
#undef lst10
	const int DLGS_H_lst11 = lst11;
#undef lst11
	const int DLGS_H_lst12 = lst12;
#undef lst12
	const int DLGS_H_lst13 = lst13;
#undef lst13
	const int DLGS_H_lst14 = lst14;
#undef lst14
	const int DLGS_H_lst15 = lst15;
#undef lst15
	const int DLGS_H_lst16 = lst16;
#undef lst16
	const int DLGS_H_cmb1 = cmb1;
#undef cmb1
	const int DLGS_H_cmb2 = cmb2;
#undef cmb2
	const int DLGS_H_cmb3 = cmb3;
#undef cmb3
	const int DLGS_H_cmb4 = cmb4;
#undef cmb4
	const int DLGS_H_cmb5 = cmb5;
#undef cmb5
	const int DLGS_H_cmb6 = cmb6;
#undef cmb6
	const int DLGS_H_cmb7 = cmb7;
#undef cmb7
	const int DLGS_H_cmb8 = cmb8;
#undef cmb8
	const int DLGS_H_cmb9 = cmb9;
#undef cmb9
	const int DLGS_H_cmb10 = cmb10;
#undef cmb10
	const int DLGS_H_cmb11 = cmb11;
#undef cmb11
	const int DLGS_H_cmb12 = cmb12;
#undef cmb12
	const int DLGS_H_cmb13 = cmb13;
#undef cmb13
	const int DLGS_H_cmb14 = cmb14;
#undef cmb14
	const int DLGS_H_cmb15 = cmb15;
#undef cmb15
	const int DLGS_H_cmb16 = cmb16;
#undef cmb16
	const int DLGS_H_edt1 = edt1;
#undef edt1
	const int DLGS_H_edt2 = edt2;
#undef edt2
	const int DLGS_H_edt3 = edt3;
#undef edt3
	const int DLGS_H_edt4 = edt4;
#undef edt4
	const int DLGS_H_edt5 = edt5;
#undef edt5
	const int DLGS_H_edt6 = edt6;
#undef edt6
	const int DLGS_H_edt7 = edt7;
#undef edt7
	const int DLGS_H_edt8 = edt8;
#undef edt8
	const int DLGS_H_edt9 = edt9;
#undef edt9
	const int DLGS_H_edt10 = edt10;
#undef edt10
	const int DLGS_H_edt11 = edt11;
#undef edt11
	const int DLGS_H_edt12 = edt12;
#undef edt12
	const int DLGS_H_edt13 = edt13;
#undef edt13
	const int DLGS_H_edt14 = edt14;
#undef edt14
	const int DLGS_H_edt15 = edt15;
#undef edt15
	const int DLGS_H_edt16 = edt16;
#undef edt16
	const int DLGS_H_scr1 = scr1;
#undef scr1
	const int DLGS_H_scr2 = scr2;
#undef scr2
	const int DLGS_H_scr3 = scr3;
#undef scr3
	const int DLGS_H_scr4 = scr4;
#undef scr4
	const int DLGS_H_scr5 = scr5;
#undef scr5
	const int DLGS_H_scr6 = scr6;
#undef scr6
	const int DLGS_H_scr7 = scr7;
#undef scr7
	const int DLGS_H_scr8 = scr8;
#undef scr8
	const int DLGS_H_ctl1 = ctl1;
#undef ctl1

	// first/last control
	const int ctlFirst = DLGS_H_ctlFirst;
	const int ctlLast = DLGS_H_ctlLast;

	// pushbuttons
	const int psh1 = DLGS_H_psh1;
	const int psh2 = DLGS_H_psh2;
	const int psh3 = DLGS_H_psh3;
	const int psh4 = DLGS_H_psh4;
	const int psh5 = DLGS_H_psh5;
	const int psh6 = DLGS_H_psh6;
	const int psh7 = DLGS_H_psh7;
	const int psh8 = DLGS_H_psh8;
	const int psh9 = DLGS_H_psh9;
	const int psh10 = DLGS_H_psh10;
	const int psh11 = DLGS_H_psh11;
	const int psh12 = DLGS_H_psh12;
	const int psh13 = DLGS_H_psh13;
	const int psh14 = DLGS_H_psh14;
	const int pshHelp = DLGS_H_pshHelp;
	const int psh16 = DLGS_H_psh16;

	// Checkboxes
	const int chx1 = DLGS_H_chx1;
	const int chx2 = DLGS_H_chx2;
	const int chx3 = DLGS_H_chx3;
	const int chx4 = DLGS_H_chx4;
	const int chx5 = DLGS_H_chx5;
	const int chx6 = DLGS_H_chx6;
	const int chx7 = DLGS_H_chx7;
	const int chx8 = DLGS_H_chx8;
	const int chx9 = DLGS_H_chx9;
	const int chx10 = DLGS_H_chx10;
	const int chx11 = DLGS_H_chx11;
	const int chx12 = DLGS_H_chx12;
	const int chx13 = DLGS_H_chx13;
	const int chx14 = DLGS_H_chx14;
	const int chx15 = DLGS_H_chx15;
	const int chx16 = DLGS_H_chx16;

	// Radio buttons
	const int rad1 = DLGS_H_rad1;
	const int rad2 = DLGS_H_rad2;
	const int rad3 = DLGS_H_rad3;
	const int rad4 = DLGS_H_rad4;
	const int rad5 = DLGS_H_rad5;
	const int rad6 = DLGS_H_rad6;
	const int rad7 = DLGS_H_rad7;
	const int rad8 = DLGS_H_rad8;
	const int rad9 = DLGS_H_rad9;
	const int rad10 = DLGS_H_rad10;
	const int rad11 = DLGS_H_rad11;
	const int rad12 = DLGS_H_rad12;
	const int rad13 = DLGS_H_rad13;
	const int rad14 = DLGS_H_rad14;
	const int rad15 = DLGS_H_rad15;
	const int rad16 = DLGS_H_rad16;

	// Groups, frames, rectangles, and icons
	const int grp1 = DLGS_H_grp1;
	const int grp2 = DLGS_H_grp2;
	const int grp3 = DLGS_H_grp3;
	const int grp4 = DLGS_H_grp4;
	const int frm1 = DLGS_H_frm1;
	const int frm2 = DLGS_H_frm2;
	const int frm3 = DLGS_H_frm3;
	const int frm4 = DLGS_H_frm4;
	const int rct1 = DLGS_H_rct1;
	const int rct2 = DLGS_H_rct2;
	const int rct3 = DLGS_H_rct3;
	const int rct4 = DLGS_H_rct4;
	const int ico1 = DLGS_H_ico1;
	const int ico2 = DLGS_H_ico2;
	const int ico3 = DLGS_H_ico3;
	const int ico4 = DLGS_H_ico4;

	// Static text
	const int stc1 = DLGS_H_stc1;
	const int stc2 = DLGS_H_stc2;
	const int stc3 = DLGS_H_stc3;
	const int stc4 = DLGS_H_stc4;
	const int stc5 = DLGS_H_stc5;
	const int stc6 = DLGS_H_stc6;
	const int stc7 = DLGS_H_stc7;
	const int stc8 = DLGS_H_stc8;
	const int stc9 = DLGS_H_stc9;
	const int stc10 = DLGS_H_stc10;
	const int stc11 = DLGS_H_stc11;
	const int stc12 = DLGS_H_stc12;
	const int stc13 = DLGS_H_stc13;
	const int stc14 = DLGS_H_stc14;
	const int stc15 = DLGS_H_stc15;
	const int stc16 = DLGS_H_stc16;
	const int stc17 = DLGS_H_stc17;
	const int stc18 = DLGS_H_stc18;
	const int stc19 = DLGS_H_stc19;
	const int stc20 = DLGS_H_stc20;
	const int stc21 = DLGS_H_stc21;
	const int stc22 = DLGS_H_stc22;
	const int stc23 = DLGS_H_stc23;
	const int stc24 = DLGS_H_stc24;
	const int stc25 = DLGS_H_stc25;
	const int stc26 = DLGS_H_stc26;
	const int stc27 = DLGS_H_stc27;
	const int stc28 = DLGS_H_stc28;
	const int stc29 = DLGS_H_stc29;
	const int stc30 = DLGS_H_stc30;
	const int stc31 = DLGS_H_stc31;
	const int stc32 = DLGS_H_stc32;

	// Listboxes
	const int lst1 = DLGS_H_lst1;
	const int lst2 = DLGS_H_lst2;
	const int lst3 = DLGS_H_lst3;
	const int lst4 = DLGS_H_lst4;
	const int lst5 = DLGS_H_lst5;
	const int lst6 = DLGS_H_lst6;
	const int lst7 = DLGS_H_lst7;
	const int lst8 = DLGS_H_lst8;
	const int lst9 = DLGS_H_lst9;
	const int lst10 = DLGS_H_lst10;
	const int lst11 = DLGS_H_lst11;
	const int lst12 = DLGS_H_lst12;
	const int lst13 = DLGS_H_lst13;
	const int lst14 = DLGS_H_lst14;
	const int lst15 = DLGS_H_lst15;
	const int lst16 = DLGS_H_lst16;

	// Combo boxes
	const int cmb1 = DLGS_H_cmb1;
	const int cmb2 = DLGS_H_cmb2;
	const int cmb3 = DLGS_H_cmb3;
	const int cmb4 = DLGS_H_cmb4;
	const int cmb5 = DLGS_H_cmb5;
	const int cmb6 = DLGS_H_cmb6;
	const int cmb7 = DLGS_H_cmb7;
	const int cmb8 = DLGS_H_cmb8;
	const int cmb9 = DLGS_H_cmb9;
	const int cmb10 = DLGS_H_cmb10;
	const int cmb11 = DLGS_H_cmb11;
	const int cmb12 = DLGS_H_cmb12;
	const int cmb13 = DLGS_H_cmb13;
	const int cmb14 = DLGS_H_cmb14;
	const int cmb15 = DLGS_H_cmb15;
	const int cmb16 = DLGS_H_cmb16;

	// Edit controls
	const int edt1 = DLGS_H_edt1;
	const int edt2 = DLGS_H_edt2;
	const int edt3 = DLGS_H_edt3;
	const int edt4 = DLGS_H_edt4;
	const int edt5 = DLGS_H_edt5;
	const int edt6 = DLGS_H_edt6;
	const int edt7 = DLGS_H_edt7;
	const int edt8 = DLGS_H_edt8;
	const int edt9 = DLGS_H_edt9;
	const int edt10 = DLGS_H_edt10;
	const int edt11 = DLGS_H_edt11;
	const int edt12 = DLGS_H_edt12;
	const int edt13 = DLGS_H_edt13;
	const int edt14 = DLGS_H_edt14;
	const int edt15 = DLGS_H_edt15;
	const int edt16 = DLGS_H_edt16;

	// Scroll bars
	const int scr1 = DLGS_H_scr1;
	const int scr2 = DLGS_H_scr2;
	const int scr3 = DLGS_H_scr3;
	const int scr4 = DLGS_H_scr4;
	const int scr5 = DLGS_H_scr5;
	const int scr6 = DLGS_H_scr6;
	const int scr7 = DLGS_H_scr7;
	const int scr8 = DLGS_H_scr8;

	// Controls
	const int ctl1 = DLGS_H_ctl1;
};

