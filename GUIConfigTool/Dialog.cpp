// Pinscape Pico Config Tool Dialog box helper 
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdlib.h>
#include <stdio.h>
#include <Windows.h>
#include <shobjidl_core.h>
#include <atlbase.h>
#include "Application.h"
#include "resource.h"
#include "Dialog.h"

std::unordered_map<HWND, Dialog*> Dialog::hwndToDialog;
std::list<Dialog::HookInfo> Dialog::hooks;

Dialog::Dialog(HINSTANCE hInstance) : hInstance(hInstance)
{
}

Dialog::~Dialog()
{
}

bool Dialog::PreTranslate(MSG *msg)
{
	// if we have an accelerator, try letting it process the message
	if (hAccel != NULL && TranslateAccelerator(hDlg, hAccel, msg))
		return true;

	// not handled
	return false;
}

HWND Dialog::OpenModeless(int resourceID, HWND hwndParent)
{
	// create the dialog
	HWND hwnd = CreateDialogParam(hInstance, MAKEINTRESOURCE(resourceID), hwndParent,
		&Dialog::DialogProc, reinterpret_cast<LPARAM>(this));

	// add it to the application modeless dialog list
	if (hwnd != NULL)
		gApp.OnOpenModeless(hwnd);

	// return the window handle
	return hwnd;
}

int Dialog::Show(int resourceID, HWND hwndParent)
{
	// disable active modeless dialogs
	std::list<HWND> reEnableList;
	for (auto hdlg : gApp.modelessDialogs)
	{
		if (IsWindowEnabled(hdlg))
		{
			reEnableList.emplace_back(hdlg);
			EnableWindow(hdlg, FALSE);
		}
	}

	// show the dialog and run its nested modal message loop
	INT_PTR result = DialogBoxParam(hInstance, MAKEINTRESOURCE(resourceID), 
		hwndParent, &Dialog::DialogProc, reinterpret_cast<LPARAM>(this));

	// re-enable the disabled moddless dialogs
	for (auto hdlg : reEnableList)
		EnableWindow(hdlg, TRUE);

	// check for errors
	if (result == 0 || result == -1)
		MessageBox(hwndParent, _T("Error showing dialog"), _T("Error"), MB_OK | MB_ICONERROR);

	// return the result
	return static_cast<int>(result);
}

int Dialog::ShowWithMessageBoxFont(int resourceID)
{
	// get the system UI parameters
	NONCLIENTMETRICSW ncm;
	ncm.cbSize = sizeof(ncm);
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0);

	// show the dialog using the system Message Box font
	return ShowWithFont(resourceID, &ncm.lfMessageFont);
}

// ShowWithFont helper: skip a null-terminated WCHAR string in the
// dialog template (including the trailing null).
static const BYTE *Skip_sz(const BYTE *p)
{
	const WORD *w = (const WORD *)p;
	for (; *w != 0; ++w);
	return (const BYTE *)(w + 1);
}

// ShowWithFont helper: skip an "sz_Or_Ord" field in the dialog
// template, which contains one of the following:
//
//   WORD 0x0000   -> just this one word
//   WORD 0xFFFF   -> two words
//   other         -> null-terminated WCHAR string
//
static const BYTE *Skip_sz_Or_Ord(const BYTE *p)
{
	// it the first WORD is 0x0000, that's all there is
	const WORD *w = reinterpret_cast<const WORD*>(p);
	if (*w == 0)
		return reinterpret_cast<const BYTE*>(w + 1);

	// if the first WORD is 0xFFFF, there's exactly one more WORD 
	// following it
	if (*w == 0xffff)
		return reinterpret_cast<const BYTE*>(w + 2);

	// otherwise, it's a null-terminated WCHAR string
	return Skip_sz(p);
}

int Dialog::ShowWithFont(int resource_id, const LOGFONTW *fontDesc)
{
	// load the font
	HFONT font = CreateFontIndirect(fontDesc);

	// load the dialog template
	const BYTE *tpl = nullptr;
	HGLOBAL hGlob = NULL;
	DWORD resSize = 0;
	if (HRSRC hRes = FindResource(hInstance, MAKEINTRESOURCE(resource_id), RT_DIALOG); hRes != NULL)
	{
		resSize = SizeofResource(hInstance, hRes);
		if ((hGlob = LoadResource(hInstance, hRes)) != NULL)
			tpl = static_cast<BYTE*>(LockResource(hGlob));
	}
	if (tpl == nullptr)
	{
		// show a hardcoded message, rather than a localizable string table
		// resource as we usually would: if the dialog resource is missing,
		// string resources might be missing as well
		MessageBox(GetActiveWindow(), _T("Missing dialog resource"), _T("Error"), MB_OK | MB_ICONERROR);
		return 0;
	}

	// parse it - start with the 'sz_Or_Ord menu' entry (string or resource ID)
	const BYTE *p = Skip_sz_Or_Ord(tpl + 26);

	// skip the windowClass field
	p = Skip_sz_Or_Ord(p);

	// skip the title field
	p = Skip_sz(p);

	// we're at the pointsize field
	const BYTE *pPointsize = p;
	const BYTE *pTypeface = p + 6;

	// get the length of the old font name
	size_t fontNameLen = (wcslen(reinterpret_cast<const WCHAR *>(pTypeface)) + 1) * sizeof(WCHAR);

	// allocate a new copy of the structure with space for the message 
	// box font name in place of the font in the resource file
	size_t newFontNameLen = (wcslen(fontDesc->lfFaceName) + 1) * sizeof(WCHAR);
	std::unique_ptr<BYTE> newTpl(new BYTE[resSize + newFontNameLen - fontNameLen]);

	// copy the original template up to the font name
	memcpy(newTpl.get(), tpl, pTypeface - tpl);

	// The dialog template wants the new font size to be specified
	// as a point size.  Work backwards from the lfHeight in the font
	// descriptor to a point size.  If lfHeight is positive, it's the
	// character cell height, which is basically the character height
	// plus the leading.  If it's negative, it's just the character
	// height.  Once we have the character height, figure the point
	// size by running the forumula described in the WinSDK docs for
	// LOGFONT.lfHeight backwards.
	LONG charHeight = fontDesc->lfHeight;
	if (charHeight != 0)
	{
		// we'll need a DC for some of the font lookup operations
		HDC dc = GetDC(GetActiveWindow());

		// check if we have a char height or cell height
		if (charHeight < 0)
		{
			// it's the char height - get the absolute value
			charHeight = -charHeight;
		}
		else
		{
			// it's the cell height - get the leading
			TEXTMETRIC tm;
			ZeroMemory(&tm, sizeof(tm));
			HGDIOBJ oldFont = SelectObject(dc, font);
			GetTextMetrics(dc, &tm);
			SelectObject(dc, oldFont);

			// deduct the leading from the cell height to get the char height
			charHeight -= tm.tmInternalLeading;
		}

		// Now we can finally figure the point size by working the WinSDK
		// lfHeight point size formula backwards.
		LONG ptSize = MulDiv(charHeight, 72, GetDeviceCaps(dc, LOGPIXELSY));
		newTpl.get()[pPointsize - tpl] = static_cast<BYTE>(ptSize);

		// done with our temp dc
		ReleaseDC(GetActiveWindow(), dc);
	}

	// store the new font name
	memcpy(newTpl.get() + (pTypeface - tpl), fontDesc->lfFaceName, newFontNameLen);

	// copy the rest of the template
	memcpy(newTpl.get() + (pTypeface - tpl) + newFontNameLen, pTypeface + fontNameLen,
		resSize - ((pTypeface - tpl) + fontNameLen));

	// create the dialog
	INT_PTR result = DialogBoxIndirectParam(hInstance, reinterpret_cast<LPCDLGTEMPLATE>(newTpl.get()),
		GetActiveWindow(), &Dialog::DialogProc, reinterpret_cast<LPARAM>(this));

	// release resources
	UnlockResource(hGlob);
	DeleteObject(font);

	// return the dialog result
	return static_cast<int>(result);
}

INT_PTR CALLBACK Dialog::DialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	Dialog *self;

	switch (message)
	{
	case WM_INITDIALOG:
		// The lParam has our object pointer.  Store it in the DWLP_USER
		// window long so that we can retrieve it on subsequent calls.
		SetWindowLongPtr(hDlg, DWLP_USER, lParam);

		// get the 'this' pointer
		self = reinterpret_cast<Dialog*>(lParam);

		// set the window handle internally
		self->hDlg = hDlg;

		// add the HWND mapping table entry
		hwndToDialog.emplace(hDlg, self);

		// set up our window hook
		hooks.emplace_back(
			SetWindowsHookEx(WH_GETMESSAGE, GetMsgHook, self->hInstance, GetCurrentThreadId()), 
			self);

		// invoke the virtual method
		return self->Proc(message, wParam, lParam);

	case WM_ENTERIDLE:
		// Forward this up to our parent window.  Note that WM_ENTERIDLE is
		// sent to the *parent* of a dialog, not the dialog itself, so 
		// we're not handling this here for the sake of the present dialog.
		// Rather, we're handling it for the sake of any nested dialogs we
		// invoke, including MessageBox dialogs.  Note that we won't get
		// this at all for nested MessageBox dialogs unless they're created
		// using our specialized cover, MessageBoxWithIdleMsg().  The point
		// passing this to the parent window is to allow the parent window
		// to continue performing idle-time activity while a dialog is
		// running.  Dialogs run in nested message loops, so any idle-time
		// activity that the parent would normally do in its own message
		// loop would otherwise be suspended while the dialog's nested
		// message loop is in effect.
		return SendMessage(GetParent(hDlg), message, wParam, lParam);

	default:
		// For other messages, the DWLP_USER window long should have our
		// object pointer.  Retrieve it, make sure it's valid, and call
		// our virtual dialog proc method.
		if (self = reinterpret_cast<Dialog*>(GetWindowLongPtr(hDlg, DWLP_USER)); self != nullptr)
		{
			std::shared_ptr<Dialog> stackRef(self->sharedSelfRef);
			return self->Proc(message, wParam, lParam);
		}
		break;
	}

	// not handled - ignore the message
	return FALSE;
}

LRESULT CALLBACK Dialog::GetMsgHook(int code, WPARAM wparam, LPARAM lparam)
{
	// the current hook is always the top of the hook stack
	auto &hook = hooks.back();

	// process HC_ACTION messages
	if (code == HC_ACTION)
	{
		// decode the parameters
		UINT pmCode = static_cast<UINT>(wparam);  // PM_NOREMOVE or PM_REMOVE
		MSG *msg = reinterpret_cast<MSG*>(lparam);

		// pre-translate the message
		if (hook.dlg->PreTranslate(msg))
		{
			// The message was intercepted and handled, so we don't want to allow
			// the dialog box message loop to see it.  We can't remove the message
			// from the queue, but we're allowed to modify it in place, so change
			// it into a null message.
			msg->message = WM_NULL;
			msg->hwnd = NULL;
			msg->wParam = 0;
			msg->lParam = 0;
			return 0;
		}
	}

	// call the next hook procedure
	return CallNextHookEx(hook.hook, code, wparam, lparam);
}

INT_PTR Dialog::Proc(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		// Perform subclassed create handling
		return OnInitDialog(wParam, lParam);

	case WM_NCDESTROY:
		// my window handle is no longer valid after this, so remove it from
		// the HWND -> Dialog mapping table
		hwndToDialog.erase(hDlg);

		// remove our windows hook
		UnhookWindowsHookEx(hooks.back().hook);
		hooks.pop_back();

		// release our shared self-reference
		sharedSelfRef.reset();
		break;

	case WM_INITMENUPOPUP:
		OnInitMenuPopup(reinterpret_cast<HMENU>(wParam), LOWORD(lParam), HIWORD(wParam) != 0);
		break;

    case WM_COMMAND:
        // try processing it through the virtual command handler
        if (OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam)))
            return 0;

		// Check for the standard dismissal buttons - OK, CANCEL, YES, NO.  On
		// a click in any of these, end the dialog with the button ID as the
		// dialog exit code.
		switch (HIWORD(wParam))
		{
		case BN_CLICKED:
			switch (LOWORD(wParam))
			{
			case IDOK:
			
			case IDCANCEL:
			case IDYES:
			case IDNO:
				EndDialog(hDlg, LOWORD(wParam));
				return TRUE;
			}
		}
		break;

    case WM_NOTIFY:
        if (LRESULT lresult = 0; OnNotify(reinterpret_cast<NMHDR*>(lParam), lresult))
            return lresult;
		break;

	case WM_MEASUREITEM:
		if (OnMeasureItem(wParam, reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)))
			return 0;
		break;

	case WM_DRAWITEM:
		if (OnDrawItem(wParam, reinterpret_cast<DRAWITEMSTRUCT*>(lParam)))
			return 0;
		break;

	case WM_DELETEITEM:
		if (OnDeleteItem(wParam, reinterpret_cast<DELETEITEMSTRUCT*>(lParam)))
			return 0;
		break;

	case WM_DESTROY:
		OnDestroy();
		break;
	}

	// not handled
	return FALSE;
}

void Dialog::OnDestroy()
{
	// remove me from the application modeless list, if we're in it
	gApp.OnCloseModeless(hDlg);
}

int Dialog::ResizeStaticToFitText(HWND ctl, const TCHAR *txt)
{
	// get the current layout
	const RECT rc = GetCtlScreenRect(ctl);
	const int width = rc.right - rc.left;
	const int height = rc.bottom - rc.top;

	// get the font and select it into the DC
	HFONT hfont = reinterpret_cast<HFONT>(SendMessage(ctl, WM_GETFONT, 0, 0));
	HDC hdc = GetDC(hDlg);
	HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hfont));

	// set the text
	SetWindowText(ctl, txt);

	// measure the text
	RECT rcTxt = rc;
	int newht = DrawText(hdc, txt, -1, &rcTxt, DT_CALCRECT | DT_TOP | DT_LEFT | DT_WORDBREAK);

	// if the height didn't increase, there's nothing to do
	if (newht < height)
		return 0;

	// increase the height to fit, keeping the width and location unchanged
	// and keeping the width
	MoveWindow(ctl, rc.left, rc.top, rc.right - rc.left, newht, TRUE);

	// restore the old font
	SelectObject(hdc, oldFont);

	// return the change in height
	return newht - height;
}

// Get the client rect for a dialog control relative to the dialog window
RECT Dialog::GetCtlScreenRect(HWND ctl) const
{
	RECT rc;
	GetWindowRect(ctl, &rc);
	ScreenToClient(hDlg, (POINT *)&rc);
	ScreenToClient(hDlg, ((POINT *)&rc) + 1);
	return rc;
}

void Dialog::MoveCtlBy(int ctlID, int dx, int dy)
{
	// get the control
	HWND ctl = GetDlgItem(ctlID);
	if (ctl != 0)
	{
		// get its current location
		RECT rc = GetCtlScreenRect(ctl);

		// move it to the new location
		MoveWindow(ctl, rc.left + dx, rc.top + dy, rc.right - rc.left, rc.bottom - rc.top, TRUE);
	}
}

void Dialog::ExpandWindowBy(int dx, int dy)
{
	// get the current window area
	RECT rcw;
	GetWindowRect(hDlg, &rcw);

	// expand it by the given amount
	MoveWindow(hDlg, rcw.left - dx/2, rcw.top - dy/2, rcw.right - rcw.left + dx, rcw.bottom - rcw.top + dy, TRUE);
}

std::wstring Dialog::GetDlgItemTextW(int ctlID) const
{
	// prepare a string buffer big enough to hold the window text
	std::vector<WCHAR> s;
	s.resize(GetWindowTextLength(GetDlgItem(ctlID)) + 1);

	// Get the text and return the string
	::GetDlgItemTextW(hDlg, ctlID, s.data(), static_cast<int>(s.size()));
	return std::wstring(s.data());
}

std::string Dialog::GetDlgItemTextA(int ctlID) const
{
	// prepare a string buffer big enough to hold the window text
	std::vector<char> s;
	s.resize(GetWindowTextLength(GetDlgItem(ctlID)) + 1);

	// Get the text and return the string
	::GetDlgItemTextA(hDlg, ctlID, s.data(), static_cast<int>(s.size()));
	return std::string(s.data());
}

void Dialog::FormatFromDlgItemText(int ctlID, ...)
{
	// get the original control text to use as the format string
	TSTRING baseText = GetDlgItemText(ctlID);

	// format the text with the variadic arguments
	TCHAR buf[1024];
	va_list va;
	va_start(va, ctlID);
	_vstprintf_s(buf, baseText.c_str(), va);
	va_end(va);

	// plug the formatted text back in as the control text 
	SetDlgItemText(ctlID, buf);
}

void Dialog::SetDlgItemTextFmt(int ctlID, const TCHAR *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	TCHAR buf[1024];
	_vstprintf_s(buf, fmt, va);
	SetDlgItemText(ctlID, buf);
	va_end(va);
}

UINT_PTR GetFileNameDlg::Hook(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_NOTIFY:
		switch (reinterpret_cast<NMHDR*>(lparam)->code)
		{
		case CDN_FILEOK:
			// check the file for approval
			if (!ApproveFile(reinterpret_cast<OFNOTIFY*>(lparam)))
			{
				// tell the system dialog handler that we're rejecting the
				// selection - to do this, we have to set DWL_MSGRESULT to
				// a non-zero value AND return non-zero value from the hook
				// procedure
				SetWindowLong(hwnd, DWLP_MSGRESULT, 1);
				return TRUE;
			}
			break;
		}
	}

	// Return 0 to indicate that the system default handling should proceed.
	// For some (but not all) notifications, we can return a non-zero value
	// to override the system defaults.
	return 0;
}


// --------------------------------------------------------------------------
//
// Message-box-like dialog
//

MessageBoxLikeDialog::MessageBoxLikeDialog(HINSTANCE hInstance, int bitmap_id, bool useSystemMessageBoxFont) :
	Dialog(hInstance),
	useSystemMessageBoxFont(useSystemMessageBoxFont)
{
	// load the icon
	icon = (HBITMAP)LoadImage(
		hInstance, MAKEINTRESOURCE(bitmap_id),
		IMAGE_BITMAP, 0, 0, LR_DEFAULTSIZE);

	// create the brushes
	bkgBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
	faceBrush = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
}

INT_PTR MessageBoxLikeDialog::Proc(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		// set the bitmap ID for the icon
		SendDlgItemMessage(hDlg, IDC_ERROR_ICON, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(icon));
		break;

	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORDLG:
		// use the 3D face brush for the bottom bar
		if ((HWND)lParam == GetDlgItem(IDC_BOTTOM_BAR))
			return reinterpret_cast<INT_PTR>(faceBrush);

		// use the white brush for other controls
		return reinterpret_cast<INT_PTR>(bkgBrush);
	}

	// return the inherited handling
	return __super::Proc(message, wParam, lParam);
}

// --------------------------------------------------------------------------
//
// Custom message box
//

CustomMessageBox::CustomMessageBox(HINSTANCE hInstance, int iconID, int messageStringID, bool useSysMsgBoxFont) :
	MessageBoxLikeDialog(hInstance, iconID, useSysMsgBoxFont)
{
	std::vector<TCHAR> buf;
	buf.resize(LoadString(hInstance, messageStringID, NULL, 0) + 1);
	LoadString(hInstance, messageStringID, buf.data(), static_cast<int>(buf.size()));
	message = buf.data();
}

INT_PTR CustomMessageBox::Proc(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		// inherit the default handling first
		__super::Proc(message, wParam, lParam);

		// set the main message
		SetDlgItemText(IDC_TXT_ERROR, this->message.c_str());

		// resize the dialog to accommodate the message text
		{
			// adjust the main static text to fit the message
			int dy = ResizeStaticToFitText(GetDlgItem(IDC_TXT_ERROR), this->message.c_str());

			// resize the window to match
			ExpandWindowBy(0, dy);

			// moe the bottom bar
			MoveCtlBy(IDC_BOTTOM_BAR, 0, dy);

			// move all of the buttons
			for (HWND chi = GetWindow(hDlg, GW_CHILD); chi != NULL; chi = GetWindow(chi, GW_HWNDNEXT))
			{
				WCHAR cls[64] = L"";
				GetClassNameW(chi, cls, _countof(cls));
				DWORD style = GetWindowLong(chi, GWL_STYLE);
				DWORD bt = style & BS_TYPEMASK;
				if (wcscmp(cls, L"Button") == 0 && (bt == BS_PUSHBUTTON || bt == BS_DEFPUSHBUTTON))
				{
					RECT rc = GetCtlScreenRect(chi);
					MoveWindow(chi, rc.left, rc.top + dy, rc.right - rc.left, rc.bottom - rc.top, TRUE);
				}
			}
		}

		// Set focus to the first button with the DEFPUSHBUTTON style
		for (HWND chi = GetWindow(hDlg, GW_CHILD); chi != NULL; chi = GetWindow(chi, GW_HWNDNEXT))
		{
			WCHAR cls[64] = L"";
			GetClassNameW(chi, cls, _countof(cls));
			DWORD style = GetWindowLong(chi, GWL_STYLE);
			DWORD bt = style & BS_TYPEMASK;
			if (IsWindowEnabled(chi) && (style & WS_TABSTOP) != 0
				&& wcscmp(cls, L"Button") == 0 && bt == BS_DEFPUSHBUTTON)
			{
				// set focus to this control, and return false to tell the
				// system handler not to set its own default focus, which
				// would just go to the first focusable child control
				SetFocus(chi);
				return FALSE;
			}
		}
		
		// allow the system to set the default focus
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		case IDCANCEL:
			// it's one of the standard dialog dismiss buttons - inherit
			// the standard dialog box handling, which will dismiss the
			// dialog
			break;

		case IDYES:
		case IDNO:
		case IDCONTINUE:
		case IDIGNORE:
		case IDRETRY:
		case IDTRYAGAIN:
			// It's one of the standard buttons from the system MessageBox()
			// API.  Also treat these as automatic dismiss buttons.
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;

		default:
			// Check the originating control to determine if it's a
			// pushbutton.  If so, treat it as a dismiss button, since
			// this is almost always the function of every pushbutton
			// in a message box.  Exceptions can always be handled by
			// subclassing.
			if (lParam != 0)
			{
				HWND hwndCtl = reinterpret_cast<HWND>(lParam);
				WCHAR buf[64] = L"";
				GetClassNameW(hwndCtl, buf, _countof(buf));
				DWORD buttonType = GetWindowLong(hwndCtl, GWL_STYLE) & BS_TYPEMASK;
				if (wcscmp(buf, L"Button") == 0 
					&& (buttonType == BS_PUSHBUTTON || buttonType == BS_DEFPUSHBUTTON))
				{
					EndDialog(hDlg, LOWORD(wParam));
					return TRUE;
				}
			}
			break;
		}
		break;
	}

	// return the inherited handling
	return __super::Proc(message, wParam, lParam);
}



// --------------------------------------------------------------------------
//
// Message box with checkbox
//

INT_PTR MessageBoxWithCheckbox::Proc(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		// set the main message
		SetDlgItemText(IDC_TXT_ERROR, this->message.c_str());

		// resize it to accommodate the message text
		{
			int dy = ResizeStaticToFitText(GetDlgItem(IDC_TXT_ERROR), this->message.c_str());
			MoveCtlBy(IDC_MESSAGE_CHECKBOX, 0, dy);
			MoveCtlBy(IDC_BOTTOM_BAR, 0, dy);
			MoveCtlBy(IDOK, 0, dy);
			MoveCtlBy(IDCANCEL, 0, dy);
			ExpandWindowBy(0, dy);
		}

		// set the checkbox text
		SetDlgItemText(IDC_MESSAGE_CHECKBOX, checkboxLabel.c_str());
		break;

	case WM_COMMAND:
		// record checkbox changes
		if (LOWORD(wParam) == IDC_MESSAGE_CHECKBOX)
			isCheckboxChecked = IsDlgButtonChecked(hDlg, IDC_MESSAGE_CHECKBOX) == BST_CHECKED;
		break;

	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORDLG:
		// use the 3D face brush for the checkbox, which is in the bottom bar
		if ((HWND)lParam == GetDlgItem(IDC_MESSAGE_CHECKBOX))
			return reinterpret_cast<INT_PTR>(faceBrush);
		break;
	}

	// return the inherited handling
	return __super::Proc(message, wParam, lParam);
}

// -----------------------------------------------------------------------
//
// MessageBox() specialization with idle processing
//
int MessageBoxWithIdleMsg(HINSTANCE hInstance, HWND hWnd, LPCTSTR lpText, LPCTSTR lpCaption, UINT uType)
{
	// install our hook
	static HHOOK hhook;
	hhook = SetWindowsHookEx(WH_CBT, [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT
		{
			if (nCode == HCBT_CREATEWND)
			{
				// creating a window - get the hook message parameters
				HWND hwndCreating = reinterpret_cast<HWND>(wParam);
				CBT_CREATEWND *createStruct = reinterpret_cast<CBT_CREATEWND*>(lParam);

				// Check the class name of the new window.  The MessageBox 
				// class starts with "#32770".
				TCHAR cls[256];
				GetClassName(hwndCreating, cls, _countof(cls));
				if (_tcsncmp(cls, _T("#32770"), 6) == 0)
				{
					// it's the message box - clear the DS_NOIDLEMSG style
					SetWindowLong(hwndCreating, GWL_STYLE,
						GetWindowLong(hwndCreating, GWL_STYLE) & ~DS_NOIDLEMSG);
				}
			}

			// call the next hook
			return CallNextHookEx(hhook, nCode, wParam, lParam);

		}, hInstance, GetCurrentThreadId());

	// now run the native system message box
	int ret = MessageBox(hWnd, lpText, lpCaption, uType);

	// remove the hook
	UnhookWindowsHookEx(hhook);

	// return the MessageBox result
	return ret;
}

// --------------------------------------------------------------------------
//
// New-style dialog helpers
//

GetFileNameDlg::GetFileNameDlg(
	const TCHAR *title, DWORD flags, const TCHAR *filters, const TCHAR *defaultExtension,
	const TCHAR *initialFilename, const TCHAR *initialDir, size_t customFilterBufferSize) :
	title(title),
	flags(flags),
	defExt(defaultExtension),
	maxCustomFilter(customFilterBufferSize)
{
	if (customFilterBufferSize != 0)
		customFilter.reset(new TCHAR[customFilterBufferSize]);

	if (filters != nullptr)
	{
		// figure the length of the filters list, which consists of a series
		// of consecutive null-terminated strings, ended with an extra null
		const TCHAR *endp;
		for (endp = filters ; *endp != 0; endp += _tcslen(endp) + 1);

		// save the filter list
		this->filters.assign(filters, endp - filters);
	}

	// Allocate the filename buffer.  For regular single selection, just
	// use MAX_PATH.  If OFN_ALLOWMULTISELECT is set, there's no hard
	// upper bound to how big a buffer might be needed, so the best we
	// can do is allocate a buffer big enough for a few sigmas out from
	// a typical case.  The most typical case even for a multi-select
	// dialog is probably a single file, and anything beyond tens of
	// files probably becomes too unwieldy for the user in most cases.
	filenameBufSize = (flags & OFN_ALLOWMULTISELECT) != 0 ? 32 * MAX_PATH : MAX_PATH;
	filename.reset(new TCHAR[filenameBufSize]);

	if (initialFilename != nullptr)
		_tcscpy_s(filename.get(), filenameBufSize, initialFilename);
	else
		filename.get()[0] = 0;

	if (initialDir != nullptr)
		this->initialDir = initialDir;
}


bool GetFileNameDlg::Open(HWND hwndOwner)
{
	CComPtr<IFileDialog> pfd;
	if (HRESULT hr = pfd.CoCreateInstance(__uuidof(FileOpenDialog)); SUCCEEDED(hr))
		return ShowV(hwndOwner, pfd);
	else
		return Error(hwndOwner, hr);
}

bool GetFileNameDlg::Save(HWND hwndOwner)
{
	CComPtr<IFileDialog> pfd;
	if (HRESULT hr = pfd.CoCreateInstance(__uuidof(FileSaveDialog)); SUCCEEDED(hr))
		return ShowV(hwndOwner, pfd);
	else
		return Error(hwndOwner, hr);
}

bool GetFileNameDlg::SelectFolder(HWND hwndOwner)
{
	CComPtr<IFileDialog> pfd;
	if (HRESULT hr = pfd.CoCreateInstance(__uuidof(FileOpenDialog)); SUCCEEDED(hr))
	{
		// set the Pick Folder flag
		DWORD flags = 0;
		if (!SUCCEEDED(hr = pfd->GetOptions(&flags))
			|| !SUCCEEDED(hr = pfd->SetOptions(flags | FOS_PICKFOLDERS)))
			return Error(hwndOwner, hr);

		// show the dialog
		return ShowV(hwndOwner, pfd);
	}
	else
		return Error(hwndOwner, hr);
}

bool GetFileNameDlg::Error(HWND hwndOwner, HRESULT hr)
{
	TCHAR buf[128];
	_stprintf_s(buf, _T("Error showing file dialog (HRESULT %08lx)"), static_cast<unsigned long>(hr));
	MessageBox(hwndOwner, buf, _T("Error"), MB_ICONERROR | MB_OK);
	return false;
}

bool GetFileNameDlg::ShowV(HWND hwndOwner, IFileDialog *pfd)
{
	// shorthand error exit
	HRESULT hr = S_OK;
	auto Error = [this, hwndOwner, &hr]() { return this->Error(hwndOwner, hr); };

	// set options
	DWORD flags = 0;
	if (!SUCCEEDED(hr = pfd->GetOptions(&flags)))
		return Error();

	// Our internal flags are OFN_xxx flags.  Many of these have the
	// same meanings for the new dialogs - carry them over.
	flags |= (this->flags & (
		OFN_ALLOWMULTISELECT | OFN_CREATEPROMPT | OFN_DONTADDTORECENT
		| OFN_FILEMUSTEXIST | OFN_FORCESHOWHIDDEN | OFN_NOCHANGEDIR
		| OFN_NODEREFERENCELINKS | OFN_NOTESTFILECREATE | OFN_NOVALIDATE
		| OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_SHAREAWARE));

	// certain flags have new bits
	if ((this->flags & OFN_HIDEREADONLY) != 0)
		flags |= FOS_NOREADONLYRETURN;

	// file system objects only
	if (!SUCCEEDED(hr = pfd->SetOptions(flags | FOS_FORCEFILESYSTEM)))
		return Error();

	// set the title
	if (!SUCCEEDED(hr = pfd->SetTitle(title.c_str())))
		return Error();

	// set file types
	std::vector<COMDLG_FILTERSPEC> comFilters;
	for (const TCHAR *p = filters.c_str() ; *p != 0 ; )
	{
		// add the filter
		auto &f = comFilters.emplace_back();
		f.pszName = p;
		p += _tcslen(p) + 1;
		f.pszSpec = p;
		p += _tcslen(p) + 1;
	}
	if (filters.size() != 0 && !SUCCEEDED(hr = pfd->SetFileTypes(static_cast<UINT>(comFilters.size()), comFilters.data())))
		return Error();

	// set the default extension
	if (defExt.size() != 0 && !SUCCEEDED(hr = pfd->SetDefaultExtension(defExt.c_str())))
		return Error();

	// set the initial folder
	if (initialDir.size() != 0)
	{
		// Create an IShellItem for the path, and set it as the folder.  Note that
		// we have to use SetFolder(), not SetDefaultFolder(), as the latter only
		// takes effect if 
		CComPtr<IShellItem> isi;
		if (!SUCCEEDED(hr = SHCreateItemFromParsingName(initialDir.c_str(), NULL, IID_IShellItem, reinterpret_cast<void**>(&isi)))
			|| !SUCCEEDED(hr = pfd->SetFolder(isi)))
			return Error();
	}

	// show the dialog
	if (!SUCCEEDED(hr = pfd->Show(hwndOwner)))
	{
		// check for user cancellation (user clicked the Cancel button, pressed Escape, etc)
		if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
			return false;

		// otherwise the dialog probably didn't show, so show an error box instead
		return Error();
	}

	// get the result
	CComPtr<IShellItem> result;
	if (!SUCCEEDED(pfd->GetResult(&result)))
		return Error();

	// get the path
	PWSTR path = nullptr;
	if (!SUCCEEDED(hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
		return Error();

	// set the path
	size_t pathBufSize = wcslen(path) + 1;
	filename.reset(new TCHAR[pathBufSize]);
	_tcscpy_s(filename.get(), pathBufSize, path);

	// free the COM memory
	CoTaskMemFree(path);

	// success
	return true;
}
