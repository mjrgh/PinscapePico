// Pinscape Pico - Config Tool - IR Remote Control Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements an interactive window for exercising the IR
// remote control receiver and transmitter subsystems of the Pinscape
// firmware.
//
// For the receiver, the window continuously monitors the Feedback
// Controller USB interface for reports of incoming IR codes, and
// displays them in the window.  This is useful to identify the codes
// associated with specific remote control buttons, so it can be used as
// a sort of "learning universal remote" mode to learn commands that you
// either want to recognize when received or to transmit as outgoing
// commands.
//
// For the transmitter, the window provides a UI for manually entering
// commands to send on the emitter.  This can be used to test the
// physical emitter setup, and to test the effects of specific codes
// when transmitted.


#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <map>
#include <Windows.h>
#include <CommCtrl.h>
#include "PinscapePicoAPI.h"
#include "TabEmbeddableWindow.h"
#include "DeviceThreadWindow.h"

namespace PinscapePico
{
	// IR tester window
	class IRTesterWin : public DeviceThreadWindow, public TabEmbeddableWindow
	{
	public:
		// factory subclass
		class Factory : public DeviceThreadWindow::Factory
		{
		public:

		protected:
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) {
				return new IRTesterWin(hInstance, device);
			}
			const TCHAR *WindowTitle() const override { return _T("Pinscape Pico IR Tester"); }
			int Width() const override { return 960; }
			int Height() const override { return 740; }
		};

		// set my menu bar in the host application
		virtual bool InstallParentMenuBar(HWND hwndContainer) override;

		// command handling
		virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult) override;
		virtual void UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply) override;

		// translate accelerators
		virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

		// TabEmbeddableWindow interface
		virtual bool OnDeviceReconnect() override;
		virtual void OnEraseDeviceConfig(bool factoryReset) override;

		// Destruction.  Note that this blocks on our background updater
		// thread exiting.  If the caller wants to control the timing of
		// that more explicitly, use JoinThread(), which allows a timeout
		// to be specified.
		~IRTesterWin();

	protected:
		// protected constructor - clients use the factory
		IRTesterWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// query device information - called during construction and on device reconnect
		void QueryDeviceInfo();

		// transmit the code in the transmit box
		void ExecTransmit();

		// control IDs
		const static INT_PTR ID_SB_HIST = 101;
		const static INT_PTR ID_EDIT_TRANSMIT = 102;
		const static INT_PTR ID_EDIT_REPEAT = 103;
		const static INT_PTR ID_SPIN_REPEAT = 104;
		const static INT_PTR ID_BTN_TRANSMIT = 105;
		const static INT_PTR ID_BTN_RELAYMANUAL = 106;
		const static INT_PTR ID_BTN_RELAYPULSE = 107;

		// menu/accelerator
		HMENU hMenuBar = NULL;
		HACCEL hAccel = NULL;

		// oscilloscope area
		RECT rcScope{ 0 };

		// context menus for the command list and scope area
		HMENU ctxMenuHist = NULL;
		HMENU ctxMenuScope = NULL;

		// save the scope data
		void SaveScopeData();

		// copy scope data to the clipboard
		void CopyScopeData();

		// write scope data to a stream in a selected format
		void FormatScopeDataText(std::ostream &os);
		void FormatScopeDataCSV(std::ostream &os);
		void FormatScopeDataHTML(std::ostream &os);
		void FormatScopeDataHTMLFile(std::ostream &os);
		void FormatScopeDataCFHTML(std::ostream &os);

		// history area layout
		int cyHistLine = 0;
		int cyHist = 0;
		RECT rcHist{ 0 };

		// received command history scrollbar
		int cxScrollbar = 0;
		HWND sbHist = NULL;
		int yScrollHist = 0;

		// clear the command list
		void ClearCommandList();

		// transmit controls
		HWND transmitEditBox = NULL;
		HWND repeatEditBox = NULL;
		HWND repeatSpin = NULL;
		HWND transmitBtn = NULL;

		// TV ON controls
		HWND tvRelayOnBtn = NULL;
		HWND tvRelayPulseBtn = NULL;

		// last RELAY ON button state
		bool relayOnBtnState = false;

		// initial layout needed
		bool layoutPending = true;

		// window class name
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoIRTester"); }

		// window message handlers
		virtual void OnCreateWindow() override;
		virtual void OnNCDestroy() override;
		virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
		virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
		virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) override;

		// control subclass customizations
		virtual LRESULT ControlSubclassProc(
			HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass) override;

		// adjust layout
		void AdjustLayout();

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// IR report history
		struct IRHistItem
		{
			IRHistItem(IRCommandReceived command) : command(command) { }

			// the original command
			IRCommandReceived command;

			// number of repeats
			int nRepeats = 1;

			// overall list item area
			RECT rc{ 0 };

			// Copy/Transmit buttons
			RECT rcCopy{ 0 };
			RECT rcTransmit{ 0 };
		};
		std::list<IRHistItem> irHist;

		// selected history menu for context menu
		IRHistItem *selectedHistItem = nullptr;

		// history item operations
		void CopyHistoryItem(IRHistItem *item);
		void TransmitHistoryItem(IRHistItem *item);

		// IR oscilloscope pulse list
		std::list<VendorInterface::IRRawPulse> irPulse;

		// time of last pulse buffered
		UINT64 tLastPulse = 0;

		// Status message to display in the transmitter box, color, and timestamp
		struct TransmitStatus
		{
			void SetError(const char *msg) { Set(msg, HRGB(0xff0000)); }
			void SetOK(const char *msg) { Set(msg, HRGB(0x008000)); }
			void Set(const char *msg, COLORREF color)
			{
				this->msg = msg;
				this->color = color;
				t = GetTickCount64();
			}

			std::string msg;
			COLORREF color = RGB(0, 0, 0);
			UINT64 t = 0;
		} transmitStatus;

		// TV ON state names
		std::map<int, std::string> tvOnStateNames{
			{ 0, "Power Off" },
			{ 1, "Pulsing Latch" },
			{ 2, "Testing Latch" },
			{ 3, "Countdown" },
			{ 4, "Relay On" },
			{ 5, "IR Ready" },
			{ 6, "IR Waiting" },
			{ 7, "IR Sending" },
			{ 8, "Power On" },
			{ 9, "Pico Reboot" },
		};

		// first history item currently in view, as an iterator
		decltype(irHist)::iterator firstHistInView{ irHist.end() };
		
		// Thread context object.  This object is shared between the 
		// window and the thread.
		struct UpdaterThread : DeviceThreadWindow::UpdaterThread
		{
			// update
			virtual bool Update(bool &releasedMutex) override;

			// Incoming IR reports.  We queue incoming reports here for
			// consumption in the main thread.  Hold the data mutex before
			// accessing this.
			std::list<IRCommandReceived> irReports;

			// Incoming raw IR pulses
			std::list<VendorInterface::IRRawPulse> irRaw;

			// last TV ON status
			VendorInterface::TVONState tvOnState;
		};

	};

}

