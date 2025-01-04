// Pinscape Pico - Help Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <Windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include "PinscapePicoAPI.h"
#include "BaseWindow.h"

using BaseWindow = PinscapePico::BaseWindow;
class HelpWindow : public BaseWindow
{
public:
	// Check if the WebView2 runtime is installed
	static void CheckWebView2Install(HWND hwndDialogParent, HINSTANCE hInstance);

	// Is WebView2 installed?
	static bool IsWebView2Installed() { return isWebView2Installed; }

	// Create the window, if it doesn't already exist, and return the
	// global singleton instance
	static HelpWindow *CreateInstance(HINSTANCE hInstance);

	// get the global singleton instance, or null if the window isn't open
	static HelpWindow *GetInstance() { return static_cast<HelpWindow*>(inst.get()); }

	// get a reference to the shared instance
	static std::shared_ptr<BaseWindow>& GetSharedInstance() { return inst; }

	// construction
	HelpWindow(HINSTANCE hInstance);

	// destruction
	~HelpWindow();

	// navigate to a link
	void Navigate(const char *link);

	// command handler
	virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult);

protected:
	// Global singleton.  There's one help window open at any given time.
	static std::shared_ptr<BaseWindow> inst;

	// Is WebView2 installed on the computer?  We test for the installation
	// at startup.  If it's not installed, we can't use our integrated help
	// viewer, and must use the default Web browser program instead, running
	// as a separate process.
	static bool isWebView2Installed;

	// Current URI
	std::wstring currentUri;

	// Pending URI.  When we first open the window, the WebView control
	// will be created asynchronously, so we need to stash the initial
	// URL until the control comes into existence.
	std::string pendingUri;

	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoHelpWin"); }

	// window message handlers
	virtual void OnCreateWindow() override;
	virtual void OnDestroy() override;
	virtual bool OnClose() override;
	virtual void OnSizeWindow(WPARAM type, WORD width, WORD height);

	// update command status
	virtual void UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply) override;

	// Paint off-screen
	virtual void PaintOffScreen(HDC hdc) override;

	// Web View controller
	wil::com_ptr<ICoreWebView2Controller> webViewController;

	// Web View window
	wil::com_ptr<ICoreWebView2> webView;

	// control bar height
	int cyControlBar = 0;
};
