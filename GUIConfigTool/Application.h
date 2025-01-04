// Pinscape Pico Config Tool - Application Object
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Global application-level functions and data

#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <list>
#include <string>
#include <tchar.h>
#include <Windows.h>
#include <windowsx.h>
#include "JSONExt.h"

// forwards/externals
class Application;
namespace PinscapePico { 
	class BaseWindow; 
}
 
// application object - global singleton
extern Application gApp;

class Application
{
public:
	Application();
	~Application();

	// Run the outer message loop.  Returns the WM_QUIT exit code.
	int MessageLoop();

	// Add/remove an active modeless dialog
	void OnOpenModeless(HWND hwnd) { modelessDialogs.emplace_back(hwnd); }
	void OnCloseModeless(HWND hwnd) { modelessDialogs.remove(hwnd); }

	// load/save the settings file, using the name in settingsFile
	void LoadSettings();
	void SaveSettings();

	// instance handle
	HINSTANCE hInstance = NULL;

	// window that current handles accelerators
	PinscapePico::BaseWindow *accelWin = nullptr;

	// modeless dialog windows
	std::list<HWND> modelessDialogs;

	// save/restore a window placement to/from the JSON tree
	void SaveWindowPlacement(HWND hwnd, const char *jsonKey);
	WINDOWPLACEMENT RestoreWindowPlacement(const char *jsonKey, RECT rcDefault);

	// Status line temporary messages.  Windows can place messages
	// here to display for a short time.  This is especially useful
	// for success and confirmation messages that the user might want
	// to see to know that an action was successfully executed, but
	// which don't warrant a modal alert.
	struct TimedStatusMessage
	{
		TimedStatusMessage(const char *message, COLORREF fg, COLORREF bg, UINT64 endTime) :
			message(message), fg(fg), bg(bg), endTime(endTime) { }

		std::string message;    // message text
		COLORREF fg;            // foreground color
		COLORREF bg;			// background color
		UINT64 endTime;			// ending time for message, on system GetTickCount64() clock
	};
	std::list<TimedStatusMessage> timedStatusMessages;
	void AddTimedStatusMessage(const char *message, COLORREF fg, COLORREF bg, uint32_t displayTime_ms);

	// get the current timed status message; returns null if there's nothing to show
	const TimedStatusMessage *GetCurrentTimedStatusMessage();

	// Print dialog DEVMODE and DEVNAMES objects.  These are normally
	// left null so that they can be allocated by the printer dialog
	// on the first invocation.  Once allocated, they're stored here
	// so that we remember the printer settings across print commands
	// during the session.
	struct PrintDialogState
	{
		~PrintDialogState() { 
			Replace(hDevMode, NULL); 
			Replace(hDevNames, NULL);
		}

		void Update(HGLOBAL hDevMode, HGLOBAL hDevNames) {
			Replace(this->hDevMode, hDevMode);
			Replace(this->hDevNames, hDevNames);
		}

		void Replace(HGLOBAL &ele, HGLOBAL val)	{
			if (ele != NULL && ele != val) GlobalFree(ele);
			ele = val;
		}

		HGLOBAL hDevMode = NULL;
		HGLOBAL hDevNames = NULL;
	};
	PrintDialogState printDialogState;

	// Settings file name
	std::wstring settingsFile;

	// settings file contents and JSON parse tree
	std::string settingsFileText;
	JSONParserExt settingsJSON;
};

