// Open Pinball Device Viewer - Main program entrypoint
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <memory>
#include <tchar.h>
#include <Windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <gdiplusinit.h>
#include "Utilities.h"
#include "DeviceList.h"

// Main entrypoint
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	// initialize OLE (for drag/drop)
	HRESULT hr = OleInitialize(NULL);
	if (!SUCCEEDED(hr))
	{
		MessageBoxA(NULL, StrPrintf("Error initializing OLE (HRESULT %08lx)", static_cast<uint32_t>(hr)).c_str(),
			"Open Pinball Device Viewer", MB_ICONERROR | MB_OK);
		return 1;
	}

	// initialize GDI+
	ULONG_PTR gdipTok = 0;
	Gdiplus::GdiplusStartupInput gdipStartupInput;
	Gdiplus::GdiplusStartupOutput gdipStartupOutput;
	Gdiplus::GdiplusStartup(&gdipTok, &gdipStartupInput, &gdipStartupOutput);

	// create the main window object
	auto *mainWin = new DeviceListWin(hInstance);
	std::shared_ptr<BaseWindow> win(mainWin);

	// show the system window
	win->CreateSysWindow(win, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, NULL,
		_T("Open Pinball Device Viewer"), CW_USEDEFAULT, CW_USEDEFAULT, 900, 700, nCmdShow);

	// if that failed, it will have displayed an error message before it returned,
	// so we can simply terminate the program without further explanation
	if (win == nullptr || win->GetHWND() == NULL)
		return 1;

	// since this our main UI window, post a WM_QUIT message and terminate
	// the application when the window closes
	win->CallOnDestroy([]() { PostQuitMessage(0); });

	// run the main message loop
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// shut down GDI
	Gdiplus::GdiplusShutdown(gdipTok);

	// shut down OLE
	OleUninitialize();

	// exit with the parameter from the final WM_QUIT message
	return static_cast<int>(msg.wParam);
}
