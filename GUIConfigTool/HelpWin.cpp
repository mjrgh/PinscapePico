// Pinscape Pico - Help Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <regex>
#include <Windows.h>
#include <windowsx.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include "WinUtil.h"
#include "Utilities.h"
#include "Application.h"
#include "Dialog.h"
#include "resource.h"
#include "HelpWin.h"

using namespace PinscapePico;
using Microsoft::WRL::Callback;

// global singleton
std::shared_ptr<BaseWindow> HelpWindow::inst;

// statics
bool HelpWindow::isWebView2Installed = false;

// JSON key for our saved window placement
static const char *windowPlacementJsonKey = "helpWindow.placement";

// check WebView2 runtime installation
void HelpWindow::CheckWebView2Install(HWND hwndDialogParent, HINSTANCE hInstance)
{
	// get the WebView2 version string
	WCHAR *versionInfo = NULL;
	if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(NULL, &versionInfo)))
	{
		// if the version string is non-null, WebView2 runtime is installed
		if (versionInfo != nullptr)
		{
			// flag that it's installed
			isWebView2Installed = true;
			
			// we're responsible for cleaning up the version string
			CoTaskMemFree(versionInfo);

			// our work here is done
			return;
		}
	}

	// WebView2 is not installed.  Offer to install it, unless they've already
	// told us not to keep asking.
	if (gApp.settingsJSON.Get("skipWebView2Prompt")->Bool(false))
		return;

	// prompt
	class PromptDlg : public Dialog
	{
	public:
		PromptDlg(HINSTANCE hInstance) : Dialog(hInstance) { }
		virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom)
		{
			switch (command)
			{
			case IDC_CK_STOP_ASKING:
				// remember the setting
				gApp.settingsJSON.SetBool("skipWebView2Prompt",
					IsDlgButtonChecked(hDlg, IDC_CK_STOP_ASKING) == BST_CHECKED);
				break;

			case IDOK:
				// show the auto update opt-in warning
				if (MessageBoxA(hDlg, "WebView2 sets up an automatic updater that runs in "
					"the background and downloads updates from Microsoft Internet servers "
					"from time to time, without any action on your part and without notice. "
					"Unfortunately, Microsoft doesn't make this optional, so installing the "
					"component requires your consent to these automatic updates.\r\n"
					"\r\n"
					"Click Yes to accept this requirement and proceed with the installation, "
					"or No to cancel the installation.",
					"Automatic Updates", MB_YESNO | MB_ICONQUESTION) != IDYES)
				{
					// rejected - ignore the OK click and keep the dialog open
					return true;
				}
				break;
			}

			// use the base handling
			return __super::OnCommand(command, code, ctlFrom);
		}
	};
	PromptDlg dlg(hInstance);
	if (dlg.Show(IDD_INSTALL_WEBVIEW2, hwndDialogParent) != IDOK)
		return;
	
	// launch the installer
	char exe[MAX_PATH];
	GetModuleFileNameA(hInstance, exe, _countof(exe));
	PathRemoveFileSpecA(exe);
	PathAppendA(exe, "MicrosoftEdgeWebview2Setup.exe");
	if (auto result = reinterpret_cast<INT_PTR>(ShellExecuteA(NULL, NULL, exe, NULL, NULL, SW_SHOW)); result < 32)
		MessageBoxFmt(NULL, "WebView2 runtime installer launch failed (error code %d). "
			"You might try manually running this program, found in the Pinscape Pico "
			"Config Tool program folder:\r\n"
			"\r\n"
			"MicrosoftEdgeWebview2Setup.exe", static_cast<int>(result));

	// assume that the installation succeeded
	isWebView2Installed = true;
}

// create the singleton instance
HelpWindow *HelpWindow::CreateInstance(HINSTANCE hInstance)
{
	if (inst == nullptr)
	{
		// create the instance
		inst.reset(new HelpWindow(hInstance));

		// get the saved window placement
		WINDOWPLACEMENT wp = gApp.RestoreWindowPlacement(windowPlacementJsonKey, { 100, 100, 1000, 800 });
		auto &wprc = wp.rcNormalPosition;

		// open the system window
		inst->CreateSysWindow(inst,
			WS_OVERLAPPEDWINDOW | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, 0,
			NULL, _T("Help - Pinscape Pico Config Tool"),
			wprc.left, wprc.top, wprc.right - wprc.left, wprc.bottom - wprc.top, SW_SHOW);

		// restore the window placement read from the settings
		SetWindowPlacement(inst->GetHWND(), &wp);
	}

	return static_cast<HelpWindow*>(inst.get());
}

HelpWindow::HelpWindow(HINSTANCE hInstance) : BaseWindow(hInstance)
{
	// load the control bar button bitmap
	bmpCtlBarButtons = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_HELPTOOLS));
	BITMAP bmp;
	GetObject(bmpCtlBarButtons, sizeof(bmp), &bmp);
	szCtlBarButtons ={ bmp.bmWidth, bmp.bmHeight };

	// populate the control bar button list
	const static CtlBarButton buttons[]
	{
		{ ID_NAVIGATE_BACK, 0, "Back", "Go back to previous page" },
		{ ID_NAVIGATE_FORWARD, 2, nullptr, "Go forward to next page" },
		{ -1 },  // separator
		{ ID_NAVIGATE_HOME, 1, "Config Tool Help", "Go to main help page" },
	};
	for (auto &b : buttons)
		ctlBarButtons.emplace_back(b);
}

HelpWindow::~HelpWindow()
{
}

void HelpWindow::OnCreateWindow()
{
    // do the base class work
    __super::OnCreateWindow();

	// get the client area
	RECT crc;
	GetClientRect(hwnd, &crc);

	// get the window DC
	HDCHelper hdc(hwnd);

	// figure the height of the control bar and error panel
	cyControlBar = max(boldFontMetrics.tmHeight + 6, szCtlBarButtons.cy);

	// lay out the buttons
	int cxBtn = szCtlBarButtons.cy;  // not cx - buttons are square, bitmap with is sum of all cells
	int cyBtn = szCtlBarButtons.cy;
	int x = crc.left + 4;
	int y = (crc.top + cyControlBar - cyBtn)/2;
	RECT rcBtn{ x, y, x + cxBtn, y + cyBtn };
	for (auto &b : ctlBarButtons)
	{
		switch (b.cmd)
		{
		case 0:
			// unpopulated
			break;
			
		case -1:
			// spacer - add padding on either side, and set the button rect to
			// a one-pixel vertical line where we draw a visual spacer
			b.rc ={ rcBtn.left + 6, rcBtn.top, rcBtn.left + 7, rcBtn.bottom };
			OffsetRect(&rcBtn, 13, 0);
			break;

		default:
			// regular button - set the button rect
			b.rc = rcBtn;

			// if it has text, add the text size into the button rect
			if (b.label != nullptr)
				b.rc.right += hdc.MeasureText(mainFont, b.label).cx + 8;

			// set the tooltip
			SetTooltip(b.rc, b.cmd, b.tooltip);

			// advance the rect to the next button, if it's not specially positioned
			if (!b.specialPos)
				OffsetRect(&rcBtn, b.rc.right - rcBtn.left, 0);
			break;
		}
	}


	//
	// WebView2 environment options setup
	//
	 
	// Disable "MS Smart Screen".  Smart Screen is Microsoft's online malicious URL
	// filtering tool, which in principle is a good thing to have with a Web browser,
	// but it's also a gigantic privacy concern because it sends every URL loaded to
	// Microsoft.  The privacy implications are so significant that Microsoft requires
	// third-party software based on WebView2 to display a privacy warning if Smart
	// Screen is enabled.  For this application, we're only loading local file URLs
	// that are integral to the application, so they're 100% known and trusted.  So
	// we gain nothing from having Smart Screen enabled.
	auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
	options->put_AdditionalBrowserArguments(L"--disable-features=msSmartScreenProtection");

	// Set up the WebView2 control.  Note that a lot of this is boilerplate from 
	// Microsoft's WebView2 sample application:
	//
	// https://github.com/MicrosoftEdge/WebView2Samples/tree/main/GettingStartedGuides/Win32_GettingStarted
	//
	CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, options.Get(),
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT 
	{
		// Create a CoreWebView2Controller
		env->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
			[this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT 
		{
			if (controller != nullptr) 
			{
				// remember the controller
				webViewController = controller;

				// get the WebView2 object
				webViewController->get_CoreWebView2(&webView);
			}

			// put settings
			wil::com_ptr<ICoreWebView2Settings> settings;
			webView->get_Settings(&settings);
			settings->put_IsScriptEnabled(TRUE);
			settings->put_AreDefaultScriptDialogsEnabled(TRUE);
			settings->put_IsWebMessageEnabled(TRUE);

			// fit to the host window
			RECT bounds;
			GetClientRect(hwnd, &bounds);
			bounds.top += cyControlBar;
			webViewController->put_Bounds(bounds);

			// if there's a pending URI, navigate there
			if (pendingUri.size() != 0)
			{
				webView->Navigate(StrPrintf(L"%hs", pendingUri.c_str()).c_str());
				pendingUri.clear();
			}

			// Open a URL in an external browser process, using the system default browser.
			// We use this for all external links, for the sake of a nicer user experience
			// with external material.  The UI frame for the help window is intentionally
			// stripped down to just the basics we need for our own help material, which
			// isn't adequate for real Web pages from the outside world.  For those, we
			// need to give the user the normal full-browser experience, and the best way
			// to do that is to actually launch the material in the user's normal browser.
			auto LaunchBrowser = [hwnd = this->hwnd, hInstance = this->hInstance](const wchar_t *uri)
			{
				// Get the default browser executable; use our main .html help
				// file as a proxy, since FindExectuableW doesn't work with URIs.
				WCHAR helpMain[MAX_PATH];
				WCHAR exe[MAX_PATH];
				bool ok = false;
				GetModuleFileName(hInstance, helpMain, _countof(helpMain));
				PathRemoveFileSpec(helpMain);
				PathAppendW(helpMain, L"help\\ConfigTool.htm");
				if (FindExecutableW(helpMain, NULL, exe))
				{
					// launch the browser
					if (reinterpret_cast<UINT_PTR>(ShellExecuteW(hwnd, NULL, exe, uri, NULL, SW_SHOW)) > 32)
						ok = true;
				}

				return ok;
			};

			// register for navigation events
			EventRegistrationToken token;
			webView->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
				[this, LaunchBrowser](ICoreWebView2* webView, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT 
			{
				// get the URI string
				wil::unique_cotaskmem_string uri;
				args->get_Uri(&uri);

				// if it's not one of our local file URIs, open it in an external browser
				HRESULT result = S_OK;
				if (!std::regex_match(uri.get(), std::wregex(L"file?:.*", std::regex_constants::icase)))
				{
					// launch it externally
					if (!LaunchBrowser(uri.get()))
						result = E_FAIL;

					// cancel navigation
					args->put_Cancel(true);
				}
				else
				{
					// remember the current page
					currentUri = uri.get();
				}

				return result;
			}).Get(), &token);

			// register for New Window events
			webView->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
				[this, LaunchBrowser](ICoreWebView2* webView, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
			{
				// get the URI string
				wil::unique_cotaskmem_string uri;
				args->get_Uri(&uri);

				// if it's not one of our local file URIs, open it in an external browser
				HRESULT result = S_OK;
				if (!std::regex_match(uri.get(), std::wregex(L"file?:.*", std::regex_constants::icase)))
				{
					// launch it externally
					if (!LaunchBrowser(uri.get()))
						result = E_FAIL;

					// mark the event as handled
					args->put_Handled(true);
				}

				return result;
			}).Get(), &token);

			// Schedule an async task to add initialization script that freezes the Object object.
			// This is to prevent malicious script manipulation of the browser core objects; it
			// should be unnecessary for this setup, since we only load our own local help files,
			// but it's a good just-in-case.
			webView->AddScriptToExecuteOnDocumentCreated(L"Object.freeze(Object);", nullptr);

			// Schedule an async task to get the document URL
			webView->ExecuteScript(L"window.document.URL;", Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
				[](HRESULT errorCode, LPCWSTR resultObjectAsJson) -> HRESULT 
			{
				LPCWSTR URL = resultObjectAsJson;
				return S_OK;
			}).Get());

			// set up an event handler to receive messages from javascript on the page
			webView->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[](ICoreWebView2* webView, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT 
			{
				wil::unique_cotaskmem_string message;
				args->TryGetWebMessageAsString(&message);
				// processMessage(&message);
				webView->PostWebMessageAsString(message.get());
				return S_OK;
			}).Get(), &token);

			// on load page, post a message to the host application with the document URL
			webView->AddScriptToExecuteOnDocumentCreated(
				L"window.chrome.webview.postMessage(window.document.URL);",
				nullptr);

			return S_OK;
		}).Get());
		return S_OK;
	}).Get());
}

void HelpWindow::OnDestroy()
{
	// clear the singleton instance pointer
	if (inst.get() == this)
		inst.reset();

	// do the normal work
	__super::OnDestroy();
}

void HelpWindow::Navigate(const char *link)
{
	if (webViewController != nullptr)
	{
		// navigate to the new link
		webView->Navigate(StrPrintf(L"%hs", link).c_str());

		// bring the window to the front
		BringWindowToTop(hwnd);
	}
	else
	{
		// no control yet - set the pending link
		pendingUri = link;
	}
}

// Paint off-screen
void HelpWindow::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // fill the control bar background
    HDCHelper hdc(hdc0);
	RECT rcBar{ crc.left, crc.top, crc.right, crc.top + cyControlBar };
    FillRect(hdc, &rcBar, HBrush(HRGB(0xF0F0F0)));
	RECT rcBorder = rcBar;
	rcBorder.top = rcBorder.bottom - 1;
	FillRect(hdc, &rcBorder, HBrush(HRGB(0xD0D0D0)));

	// draw the control bar buttons
	int xLastBtn = 0;
	for (auto &b : ctlBarButtons)
		DrawCtlBarButton(hdc, b, xLastBtn);
}

bool HelpWindow::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
	switch (ctlCmdId)
	{
	case ID_NAVIGATE_BACK:
		// go to the previous page
		if (webView != nullptr)
			webView->GoBack();
		break;

	case ID_NAVIGATE_FORWARD:
		// go to the previous page
		if (webView != nullptr)
			webView->GoForward();
		break;

	case ID_NAVIGATE_HOME:
		// go to the main doc page
		ShowHelpFile("ConfigTool.htm");
		break;
	}

    // not handled - use default handling
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

void HelpWindow::UpdateCommandStatus(std::function<void(int, bool)> apply)
{
	if (webView != nullptr)
	{
		BOOL canGoBack = false;
		BOOL canGoForward = false;
		webView->get_CanGoBack(&canGoBack);
		webView->get_CanGoForward(&canGoForward);
		apply(ID_NAVIGATE_BACK, canGoBack);
		apply(ID_NAVIGATE_FORWARD, canGoForward);
	}

	// inherit the base handling
	__super::UpdateCommandStatus(apply);
}

void HelpWindow::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// do the base class work
	__super::OnSizeWindow(type, width, height);

	// resize the web view control
	if (webViewController != nullptr)
	{
		RECT bounds;
		GetClientRect(hwnd, &bounds);
		bounds.top += cyControlBar;
		webViewController->put_Bounds(bounds);
	}
}

bool HelpWindow::OnClose()
{
	// save the window position at close
	gApp.SaveWindowPlacement(hwnd, windowPlacementJsonKey);

	// inherit the default handling
	return __super::OnClose();
}

