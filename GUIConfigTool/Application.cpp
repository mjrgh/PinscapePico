// Pinscape Pico Config Tool - Application Object
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Global application-level functions and data

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <fstream>
#include <tchar.h>
#include <Windows.h>
#include <windowsx.h>
#include "BaseWindow.h"
#include "Application.h"

using namespace PinscapePico;

// global singleton
Application gApp;

Application::Application()
{
}

Application::~Application()
{
}

// load settings
void Application::LoadSettings()
{
	// load the file into a string stream
	std::ifstream f(settingsFile);
	std::stringstream s;
	s << f.rdbuf();

	// now get the loaded data as a string
	settingsFileText = s.str();

	// parse the settings
	settingsJSON.Parse(settingsFileText.c_str(), settingsFileText.size());
}

// save settings
void Application::SaveSettings()
{
	// generate the JSON tree to the settings file
	std::ofstream f(gApp.settingsFile);
	gApp.settingsJSON.Generate(f);
	f.close();
}

// top-level message loop
int Application::MessageLoop()
{
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
		// run it through any dialog window handlers
		bool claimedByDialog = false;
		for (auto hDlg : modelessDialogs)
		{
			if (IsDialogMessage(hDlg, &msg))
			{
				claimedByDialog = true;
				break;
			}
		}

		// if a dialog claimed the message, no further processing is required
		if (claimedByDialog)
			continue;
		
		// run it through the accelerator window, if any; if that handles it, no further
		// processing is necessary
		if (accelWin != nullptr && accelWin->TranslateAccelerators(accelWin->GetHWND(), &msg))
			continue;

		// no one claimed the message - use the normal translate/dispatch processing
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// the GetMessage() loop stops on a WM_QUIT message, whose WPARAM
	// is the exit code for the caller
	return static_cast<int>(msg.wParam);
}

void Application::AddTimedStatusMessage(const char *message, COLORREF fg, COLORREF bg, uint32_t displayTime_ms)
{
	// figure the start time - the end time of the last message in the queue,
	// or now if the queue is empty
	UINT64 startTime = timedStatusMessages.size() != 0 ? timedStatusMessages.back().endTime : GetTickCount64();

	// queue it
	timedStatusMessages.emplace_back(message, fg, bg, startTime + displayTime_ms);
}

const Application::TimedStatusMessage *Application::GetCurrentTimedStatusMessage()
{
	// time out expired messages
	auto now = GetTickCount64();
	while (timedStatusMessages.size() != 0 && now >= timedStatusMessages.front().endTime)
		timedStatusMessages.pop_front();

	// return the front message, if there's anything in the queue
	return timedStatusMessages.size() != 0 ? &timedStatusMessages.front() : nullptr;
}

void Application::SaveWindowPlacement(HWND hwnd, const char *jsonKey)
{
	WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
	auto &rcnp = wp.rcNormalPosition;
	GetWindowPlacement(hwnd, &wp);

	auto &s = gApp.settingsJSON;
	auto *jwp = s.SetObject(jsonKey);
	auto *jrcnp = s.SetObject(jwp, "rcNormalPosition");
	s.SetNum(jrcnp, "left", rcnp.left);
	s.SetNum(jrcnp, "top", rcnp.top);
	s.SetNum(jrcnp, "right", rcnp.right);
	s.SetNum(jrcnp, "bottom", rcnp.bottom);

	auto *jpt = s.SetObject(jwp, "ptMaxPosition");
	s.SetNum(jpt, "x", wp.ptMaxPosition.x);
	s.SetNum(jpt, "y", wp.ptMaxPosition.y);

	jpt = s.SetObject(jwp, "ptMinPosition");
	s.SetNum(jpt, "x", wp.ptMinPosition.x);
	s.SetNum(jpt, "y", wp.ptMinPosition.y);

	s.SetNum(jwp, "flags", wp.flags);
	s.SetNum(jwp, "showCmd", wp.showCmd);
}

WINDOWPLACEMENT Application::RestoreWindowPlacement(const char *jsonKey, RECT rcDefault)
{

	// set up the WINDOWPLACEMENT struct
	WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };

	// read the settings from the JSON tree
	auto &s = gApp.settingsJSON;
	auto *jwp = s.Get(jsonKey);

	auto *jrcnp = jwp->Get("rcNormalPosition");
	wp.rcNormalPosition ={
		jrcnp->Get("left")->Int32(rcDefault.left),
		jrcnp->Get("top")->Int32(rcDefault.top),
		jrcnp->Get("right")->Int32(rcDefault.right),
		jrcnp->Get("bottom")->Int32(rcDefault.bottom)
	};

	auto *jpt = jwp->Get("ptMaxPosition");
	wp.ptMaxPosition ={ jpt->Get("x")->Int32(0), jpt->Get("y")->Int32(0) };

	jpt = jwp->Get("ptMinPosition");
	wp.ptMinPosition ={ jpt->Get("x")->Int32(0), jpt->Get("y")->Int32(0) };

	wp.flags = jwp->Get("flags")->Int32(0);
	wp.showCmd = jwp->Get("showCmd")->Int32(SW_SHOW);

	// return the result
	return wp;
}
