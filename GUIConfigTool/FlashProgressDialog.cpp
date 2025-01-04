// Pinscape Pico - Config Tool - flash file copy progress dialog
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <windowsx.h>
#include "PinscapePicoAPI.h"
#include "FlashProgressDialog.h"
#include "CommCtrl.h"
#include "resource.h"

FlashProgressDialog::FlashProgressDialog(HINSTANCE hInstance, std::function<void(FlashProgressDialog*)> exec) :
	Dialog(hInstance), execFunc(exec)
{
}

FlashProgressDialog::~FlashProgressDialog()
{
	// wait for the thread to exit before allowing the dialog to be destroyed,
	// since the thread has a reference to the dialog object
	if (hThread != NULL)
	{
		WaitForSingleObject(hThread, INFINITE);
		CloseHandle(hThread);
	}
}


// format a large number with commas
static std::string FormatNum(uint64_t n) 
{
	char tmp[32];
	char tmp2[64];
	sprintf_s(tmp, "%I64u", n);
	size_t l = strlen(tmp);
	const char *src = tmp;
	char *dst = tmp2;
	for (; dst + 1 < tmp2 + sizeof(tmp2) && *src != 0 ; )
	{
		*dst++ = *src++;
		--l;
		if (l != 0 && (l % 3) == 0)
			*dst++ = ',';
	}
	*dst = 0;
	return std::string(tmp2);
};

void FlashProgressDialog::SetCaption(const TCHAR *caption)
{
	this->caption = caption;
	if (hDlg != NULL)
		SetWindowText(hDlg, caption);
}

void FlashProgressDialog::SetBannerText(const TCHAR *banner)
{
	this->banner = banner;
	if (hDlg != NULL)
		SetDlgItemText(IDC_TXT_BANNER, banner);
}

void FlashProgressDialog::ProgressInit(const TCHAR *from, const TCHAR *to, uint32_t fileSizeBytes)
{
	SetDlgItemTextFmt(IDC_TXT_TRANSFER_DESC, _T("%s -> %s"), from, to);
	SetDlgItemTextFmt(IDC_TXT_TRANSFER_PCT, _T("0/%hs bytes"), FormatNum(fileSizeBytes).c_str());

	this->fileSizeBytes = fileSizeBytes;
	SendMessage(GetDlgItem(IDC_PROGRESS), PBM_SETRANGE32, 0, fileSizeBytes);
}

void FlashProgressDialog::ProgressUpdate(uint32_t bytesCopied)
{
	if (isCancelRequested)
	{
		// cancellation requested - say so in the UI 
		SetDlgItemText(IDC_TXT_TRANSFER_PCT, _T("Cancelling transfer..."));
	}
	else
	{
		// show the copy progress
		SetDlgItemTextFmt(IDC_TXT_TRANSFER_PCT, _T("%hs/%hs bytes"), 
			FormatNum(bytesCopied).c_str(), FormatNum(fileSizeBytes).c_str());
		SendMessage(GetDlgItem(IDC_PROGRESS), PBM_SETPOS, bytesCopied, 0);
	}
}

INT_PTR FlashProgressDialog::OnInitDialog(WPARAM wParam, LPARAM lParam) 
{
	// disable the close box
	HMENU sysMenu = GetSystemMenu(hDlg, FALSE);
	EnableMenuItem(sysMenu, SC_CLOSE, MF_BYCOMMAND | MF_DISABLED);

	// set the window caption and banner text, if customized
	if (caption.size() != 0) SetWindowText(hDlg, caption.c_str());
	if (banner.size() != 0) SetDlgItemText(IDC_TXT_BANNER, banner.c_str());

	// launch the thread to carry out the copy
	DWORD tid;
	hThread = CreateThread(NULL, 0, [](void *param) -> DWORD
	{
		// get 'this' from the param
		auto *self = reinterpret_cast<FlashProgressDialog*>(param);

		// execute the copy
		self->execFunc(self);
		
		// close the dialog
		SendMessage(self->hDlg, WM_COMMAND, IDOK, 0);

		// done
		return 0;
	}, this, 0, &tid);

	// do the base class work
	auto ret = __super::OnInitDialog(wParam, lParam);

	// make sure the thread started
	if (hThread == NULL)
	{
		MessageBoxA(hDlg, "Error launching flash copy thread", 
			"Pinscape Pico Config Tool", MB_OK | MB_ICONERROR);
		PostMessage(hDlg, WM_COMMAND, IDOK, 0);
	}

	// return the result
	return ret;
}

bool FlashProgressDialog::OnCommand(UINT command, UINT code, HWND ctlFrom)
{
	// check for cancellation request
	if (command == IDCANCEL)
	{
		// set the cancellation flag
		isCancelRequested = true;

		// Disable the Cancel button in the UI, to indicate that clicking it
		// again won't have any effect.
		EnableWindow(GetDlgItem(IDCANCEL), FALSE);

		// set the progress text to indicate that cancellation is requested
		SetDlgItemText(IDC_TXT_TRANSFER_PCT, _T("Cancelling transfer..."));

		// Handled.  Note that we explicitly bypass the normal handling,
		// which would dismiss the dialog.  We can only request canellation;
		// it's up to the task to decide when it can safely exit.  It's
		// better for UI feedback to keep the dialog on-screen until the
		// task actually completes.
		return true;
	}

	// use the default handling
	return __super::OnCommand(command, code, ctlFrom);
}

