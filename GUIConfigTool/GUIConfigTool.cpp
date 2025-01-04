// Pinscape Pico - GUI Config Tool
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a wrapper window for the various testing and configuration
// component windows provided in the PinscapePicoAPI library.  The
// goal is to organize the tools for easy access from a single window.
//

#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <list>
#include <memory>
#include <algorithm>
#include <tchar.h>
#include <Windows.h>
#include <windowsx.h>
#include <Shlwapi.h>
#include <gdiplus.h>
#include <gdiplusinit.h>
#include "PinscapePicoAPI.h"
#include "ButtonTesterWin.h"
#include "OutputTesterWin.h"
#include "PlungerCalWin.h"
#include "WrapperWin.h"
#include "HelpWin.h"
#include "Utilities.h"
#include "Application.h"
#include "ConfigEditorWin.h"

// link the PinscapePicoAPI static library with the project
#pragma comment(lib, "PinscapePicoAPI")

// Main entrypoint
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	// remember the instance handle in the application object
	gApp.hInstance = hInstance;

	// initialize OLE (for drag/drop)
	HRESULT hr = OleInitialize(NULL);
	if (!SUCCEEDED(hr))
	{
		MessageBoxA(NULL, StrPrintf("Error initializing OLE (HRESULT %08lx)", static_cast<uint32_t>(hr)).c_str(),
			"Pinscape Pico Config Tool", MB_ICONERROR | MB_OK);
		return 1;
	}

	// initialize GDI+
	ULONG_PTR gdipTok = 0;
	Gdiplus::GdiplusStartupInput gdipStartupInput;
	Gdiplus::GdiplusStartupOutput gdipStartupOutput;
	Gdiplus::GdiplusStartup(&gdipTok, &gdipStartupInput, &gdipStartupOutput);

	// use the settings file in the executable folder by default
	WCHAR defaultSettingsFile[MAX_PATH];
	GetModuleFileNameW(hInstance, defaultSettingsFile, _countof(defaultSettingsFile));
	PathRemoveFileSpecW(defaultSettingsFile);
	PathAppendW(defaultSettingsFile, L"GUIConfigToolSettings.json");
	const WCHAR *settingsFile = defaultSettingsFile;

	// parse the command line, in case the user wanted to pre-select a unit
	int unitNum = -1;
	bool useDefaultSettings = false;
	for (int i = 1 ; i < __argc ; ++i)
	{
		const WCHAR *p = __wargv[i];
		if (wcscmp(p, L"--unit") == 0 && i + 1 < __argc)
		{
			// note the initially selected unit number
			unitNum = _wtoi(__wargv[++i]);
			if (unitNum == 0)
			{
				MessageBox(NULL, _T("Invalid unit number; must be 1 or greater"),
					_T("Pinscape Pico Config Tool"), MB_OK | MB_ICONERROR);
				return 1;
			}
		}
		else if (wcscmp(p, L"--default-settings") == 0)
		{
			// use default settings (ignore settings file contents)
			useDefaultSettings = true;
		}
		else if (wcscmp(p, L"--settings") == 0 && i + 1 < __argc)
		{
			// save the new settings file
			settingsFile = __wargv[++i];
		}
		else
		{
			// invalid option - show usage
			MessageBox(NULL, _T("Command line error - options are:\r\n\r\n")
				_T("  --default-settings    - ignore settings file contents and use defaults\r\n")
				_T("  --settings <file>     - use <file> for program settings\r\n")
				_T("  --unit <number>       - select the Pinscape Pico to configure, by unit number\r\n"),
				_T("Pinscape Pico Config Tool"), MB_OK | MB_ICONERROR);
			return 1;
		}
	}

	// create the main window object
	auto *mainWin = new WrapperWin(hInstance, settingsFile, useDefaultSettings, unitNum);
	std::shared_ptr<BaseWindow> win(mainWin);

	// get the saved placement
	auto wp = mainWin->GetWindowPlacementFromSettings();
	auto &wprc = wp.rcNormalPosition;

	// show the system window
	win->CreateSysWindow(win, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0,
		NULL, L"Pinscape Pico Config Tool",
		wprc.left, wprc.top, wprc.right - wprc.left, wprc.bottom - wprc.top, nCmdShow);

	// if that failed, it will have displayed an error message before it returned,
	// so we can simply terminate the program without further explanation
	if (win == nullptr || win->GetHWND() == NULL)
        return 1;

	// restore the window placement read from the settings
	SetWindowPlacement(win->GetHWND(), &wp);

    // since this our main UI window, post a WM_QUIT message and terminate
    // the application when the window closes
    win->CallOnDestroy([]() { PostQuitMessage(0); });

	// the wrapper window provides accelerator translation
	gApp.accelWin = win.get();

	// Check if the WebView2 runtime is installed.  This is required for our
	// help window; if it's not installed, we can offer to install it.
	HelpWindow::CheckWebView2Install(win->GetHWND(), hInstance);

	// clean up old auto-backup config files (on a background thread)
	PinscapePico::ConfigEditorWin::CleanUpAutoBackup();

	// run the main message loop
	int exitCode = gApp.MessageLoop();

	// shut down GDI
	Gdiplus::GdiplusShutdown(gdipTok);
	
	// shut down OLE
	OleUninitialize();

	// exit with the parameter from the final WM_QUIT message
	return exitCode;
}
