// Pinscape Pico - Config Tool - flash file copy progress dialog
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <windowsx.h>
#include "PinscapePicoAPI.h"
#include "Dialog.h"

class FlashProgressDialog : public Dialog, public RP2BootDevice::IProgressCallback
{
public:
	// Show the dialog.  Once the dialog is on screen, invokes the 'exec'
	// callback on a background thread to carry out the copy.
	FlashProgressDialog(HINSTANCE hInstance, std::function<void(FlashProgressDialog*)> exec);

	// set the title bar caption
	void SetCaption(const TCHAR *caption);

	// set the main banner text
	void SetBannerText(const TCHAR *text);

	// destruction
	~FlashProgressDialog();

	// IProgressCallback implementation
	virtual void ProgressInit(const TCHAR *from, const TCHAR *to, uint32_t fileSizeBytes) override;
	virtual void ProgressUpdate(uint32_t bytesCopied) override;
	virtual bool IsCancelRequested() override { return isCancelRequested; }

protected:
	virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override;
	virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom) override;

	// window caption and main banner text
	TSTRING caption;
	TSTRING banner;

	// expected file size
	uint32_t fileSizeBytes = 0;

	// cancellation requested - user clicked Cancel button
	bool isCancelRequested = false;

	// user function to carry out the copy operation
	std::function<void(FlashProgressDialog*)> execFunc;

	// background thread
	HANDLE hThread = NULL;
};

