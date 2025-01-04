// Pinscape Pico - Log Viewer Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements a GUI window displaying the Pinscape Pico
// device-side message log.  This uses the Scintilla editor control
// to display the styled text.  We parse ANSI color control sequences
// before populating the text in Scintilla.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "../Scintilla/include/Scintilla.h"
#include "PinscapePicoAPI.h"
#include "TabEmbeddableWindow.h"
#include "DeviceThreadWindow.h"

namespace PinscapePico
{
	class LogViewerWin : public DeviceThreadWindow, public TabEmbeddableWindow
	{
	public:
		// factory subclass
		class Factory : public DeviceThreadWindow::Factory
		{
		public:

		protected:
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) {
				return new LogViewerWin(hInstance, device);
			}
			const TCHAR *WindowTitle() const override { return _T("Pinscape Pico Log Viewer"); }
			int Width() const override { return 900; }
			int Height() const override { return 900; }
		};

		// Destruction
		~LogViewerWin();

		// set my menu bar in the host application
		virtual bool InstallParentMenuBar(HWND hwndContainer) override;

		// translate accelerators
		virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

		// UI activation
		virtual void OnActivateUI(bool isAppActivate) override;

		// thread activity controls
		virtual void SuspendUpdaterThread() override;
		virtual void ResumeUpdaterThread() override;

		// command handling
		virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult) override;
		virtual void UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply) override;

		// save the contents to a file
		void SaveToFile();
		void SaveToFile(const WCHAR *filename);

		//
		// TabEmbeddableWindow implementation
		//

		// keep showing the log when offline
		virtual bool IsVisibleOffline() const override { return true; }

		// no action required on a reconnect; we'll simply resume appending log output
		virtual bool OnDeviceReconnect() override { return true; }

	protected:
		// construction
		LogViewerWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// window class
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoLogViewer"); }

		// custom window proc override
		virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;

		// get my context menu
		virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) override { return hCtxMenu; }

		// window message handlers
		virtual void OnCreateWindow() override;
		virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
		virtual bool OnNotify(WPARAM id, NMHDR *nmhdr, LRESULT &lresult) override;

		// append log text, parsing ANSI escape codes
		void AppendLog(const std::vector<BYTE> *text);

		// append plain text to the log
		void AppendLogPlain(const BYTE *start, const BYTE *end);

		// Map of ANSI color combination to Scintilla style number.
		// This is keyed by the ANSI codes for foreground and background
		// color combined into a 16-bit value as ((bg << 8) | fg).  The
		// key formation is arbitrary - it's just meant to produce a
		// unique integer value per color combination so that we have a
		// convenient key for the map.  The entry in the map is in turn
		// a Scintilla style code.  We allocate a new style code each
		// time we encounter a new fg/bg combination, and enter it in
		// the map.  
		// 
		// Scintilla has a limited number of style codes available.
		// ANSI has 15 foreground color codes and 15 background color
		// codes, so we'd need 225 Scintilla styles to represent every
		// combination.  That would exceed Scintilla's space, which has
		// 216 user codes available as of this writing (and could shrink
		// in the future if Scintilla adds more reserved styles for its
		// own internal use).  Fortunately, we don't need all of the
		// possible combinations; we only need the ones that are used
		// by the Pinscape Pico firmware to generate log messages.  The
		// firmware currently only uses about a dozen combinations.
		// We also don't want to hard-code that set of colors here,
		// since we'd have to maintain it in parallel with the firmware.
		// The solution to all of these constraints is to simply add
		// new styles as we encounter new color combinations in the
		// log text we receive.
		std::unordered_map<int, int> ansiCodeToSciStyle;
		static int MakeStyleKey(int fg, int bg) { return (bg << 8) | fg; }
		int nextSciStyle = STYLE_LASTPREDEFINED + 1;

		// control subclass handler
		virtual LRESULT ControlSubclassProc(
			HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass) override;

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// adjust the window layout
		void AdjustLayout();

		// editor font
		HFONT editorFont = NULL;
		TEXTMETRIC tmEditorFont{ 0 };

		// top control bar
		int cyControlBar = 0;

		// control IDs
		static const UINT_PTR ID_SCINTILLA = 101;

		// main menu bar and accelerator
		HMENU hMenuBar = NULL;
		HACCEL hAccel = NULL;

		// context menu
		HMENU hCtxMenu = NULL;

		// control bar controls
		HWND findBox = NULL;

		// last search term
		std::string lastSearchTerm;

		// flag: search is in progress
		bool searchOpen = false;

		// current incremental search result range
		INT_PTR findStart = 0;
		INT_PTR findEnd = 0;

		// set the incremental search result range
		void SetFindHighlight(INT_PTR start, INT_PTR end);

		// update the search in progress
		bool UpdateSearch(const char *term);

		// end search mode
		enum class CommitSearchReason
		{
			Accept,       // accept the results of the search (Enter key from box, F3)
			Cancel,       // cancel the search
			KillFocus,    // exit by losing focus without an accept/cancel action
		};
		void CommitSearch(CommitSearchReason reason);

		// Scintilla window
		HWND sciWin = NULL;

		// ANSI escape sequence parsing state
		struct EscapeSequenceState
		{
			// state
			enum class State
			{
				Plain,		// plain text mode, not in an escape
				Esc,		// escape character parsed
				Number,		// in a number section
			} state = State::Plain;

			// accumulator for current number, while in Number state
			int acc = 0;

			// Sequence of accumulated numbers.  An escape sequence
			// can have zero or more numeric prefixes, separated by
			// semicolons.
			std::vector<int> numbers;

			// Current foreground and background colors, as ANSI codes.
			// 39 and 49 are the ANSI codes for the default colors.
			COLORREF fg = 39;
			COLORREF bg = 49;
		} esc;

		// Scintilla direct access function pointer
		typedef INT_PTR __cdecl ScintillaFunc_t(void*, int, INT_PTR, INT_PTR);
		ScintillaFunc_t *sciFunc = nullptr;
		void *sciFuncCtx = nullptr;
		INT_PTR CallSci(int msg, INT_PTR param1 = 0, INT_PTR param2 = 0) {
			return sciFunc != nullptr ? sciFunc(sciFuncCtx, msg, param1, param2) : 0; }

		// Printing
		virtual void PageSetup() override;
		virtual bool IsPrintingEnabled() const override { return true; }
		virtual std::basic_string<TCHAR> GetPrintDocName() const override { return _T("Pinscape Pico Log Viewer"); }
		virtual PageSetupOptions *CreatePageSetupOptions() const { return new PageSetupOptions(); }
		virtual bool ExpandHeaderFooterVar(const std::string &varName, std::string &expansion) override;
		virtual int Paginate(HDCHelper &hdc) override;
		virtual bool PrintPageContents(HDCHelper &hdc, int pageNum, bool skip) override;

		// Page setup options.  This provides the basic set of options
		// common to most document types, and it can extended with
		// additional properties as needed.
		struct PageSetupOptions : BaseWindow::PageSetupOptions
		{
			PageSetupOptions() : BaseWindow::PageSetupOptions("logViewer.pageSetup") { Load(); }
			virtual void Load() override;
			virtual void Store() override;

			// wrap long lines - true -> wrap, false -> truncate
			bool wrap = true;

			// show line numbers
			bool showLineNumbers = true;
		};

		// print context
		struct PrintingContext
		{
			// current print range
			Sci_RangeToFormatFull rtf{ 0 };
		};
		PrintingContext printingContext;

		// Updater thread context object.  This object is shared between 
		// the window and the thread.
		struct UpdaterThread : DeviceThreadWindow::UpdaterThread
		{
			// update
			virtual bool Update(bool &releasedMutex) override;
		};
	};
}
