// Pinscape Pico - Config Tool - Button Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements an interactive window showing the states
// of the Pinscape Pico unit's logical buttons and physical button 
// input devices.  This is useful for testing and troubleshooting
// the physical button wiring and the software configuration settings.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <Windows.h>
#include <CommCtrl.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "../OpenPinballDevice/OpenPinballDeviceLib/OpenPinballDeviceLib.h"
#include "PinscapePicoAPI.h"
#include "TabEmbeddableWindow.h"
#include "DeviceThreadWindow.h"

namespace PinscapePico
{
	// Button tester  window
	class ButtonTesterWin : public DeviceThreadWindow, public TabEmbeddableWindow
	{
	public:
		// factory subclass
		class Factory : public DeviceThreadWindow::Factory
		{
		public:

		protected:
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) {
				return new ButtonTesterWin(hInstance, device);
			}
			const TCHAR *WindowTitle() const override { return _T("Pinscape Pico Button Tester"); }
			int Width() const override { return 1200; }
			int Height() const override { return 740; }
		};

		// Destruction.  Note that this blocks on our background updater
		// thread exiting.  If the caller wants to control the timing of
		// that more explicitly, use JoinThread(), which allows a timeout
		// to be specified.
		~ButtonTesterWin();

		// set my menu bar in the host application
		virtual bool InstallParentMenuBar(HWND hwndContainer) override;

		// translate accelerators
		virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

		// command handler
		virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult);

		// TabEmbeddableWindow interface
		virtual bool OnDeviceReconnect() override { ReQueryDeviceInfo(); return true; }
		virtual void OnEraseDeviceConfig(bool factoryReset) override { ReQueryDeviceInfo(); }

	protected:
		// protected constructor - clients use the factory
		ButtonTesterWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// query device information
		void QueryDeviceInfo();

		// re-query the device info - clears the old cached device info and re-queries
		void ReQueryDeviceInfo();

		// window class name
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoButtonTester"); }

		// main menu bar and accelerator
		HMENU hMenuBar = NULL;
		HACCEL hAccel = NULL;

		// window message handlers
		virtual void OnCreateWindow() override;
		virtual void OnActivate(WPARAM code, HWND other) override;
		virtual bool OnKeyDown(WPARAM vkey, LPARAM flags) override;
		virtual bool OnKeyUp(WPARAM vkey, LPARAM flags) override;
		virtual bool OnSysKeyDown(WPARAM vkey, LPARAM flags) override;
		virtual bool OnSysKeyUp(WPARAM vkey, LPARAM flags) override;
		virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
		virtual bool OnSysCommand(WPARAM id, WORD x, WORD y, LRESULT &lresult) override;

		// device removal notification handler
		virtual bool OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr) override;

		// common Key Up/Down event handler
		bool OnKeyEvent(WPARAM vkey, LPARAM flags, bool down);

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// adjust the window layout
		void AdjustLayout();

		// initial layout pending
		bool layoutPending = false;

		// button configuration data
		std::vector<PinscapePico::ButtonDesc> buttonDescs;
		std::vector<PinscapePico::ButtonDevice> buttonDevices;

		// shift bits used
		uint32_t shiftBitsUsed = 0;

		// Thread context object.  This object is shared between the 
		// window and the thread.
		struct UpdaterThread : DeviceThreadWindow::UpdaterThread
		{
			// latest button and device state readings
			uint32_t shiftState = 0;
			std::vector<BYTE> buttonState;
			std::vector<BYTE> gpioState;
			std::vector<BYTE> pca9555State;
			std::vector<BYTE> hc165State;

			// update
			virtual bool Update(bool &releasedMutex) override;
		};

		// main/bold font "M" size (literally the size of the text "M")
		SIZE szMainFont;
		SIZE szBoldFont;

		// shift-state diagram metrics
		const int marginShiftState = 4;

		// keyboard background image
		HBITMAP bmpKeyboard = NULL;
		int cxkb = 0, cykb = 0;
		const int marginKb = 16;

		// physical input port status boxes
		const int portBoxSize = 16;
		const int portBoxSpacing = 4;

		// XBox controller background image
		HBITMAP bmpXBox = NULL;
		HBITMAP bmpNoXBox = NULL;
		int cxXBox = 0, cyXBox = 0;

		// gamepad button image
		HBITMAP bmpGPBtnOn = NULL;
		HBITMAP bmpGPBtnOff = NULL;
		HBITMAP bmpGPBtnNA = NULL;
		int cxGPBtn = 0, cyGPBtn = 0;

		// Open Pinball Device predefined-button images
		HBITMAP bmpOpenPinDevButtons = NULL;
		int cxOPDBtn = 0, cyOPDBtn = 0;

		// shift icon - actually a 1x2 matrix of icons, [Shift, Unshift]
		HBITMAP bmpShiftIcon = NULL;
		int cxShiftIcon = 0, cyShiftIcon = 0;

		// the current set of activated keys to draw in the keyboard diagram,
		// indexed by scan code combined with KF_EXTENDED in the 9th bit
		bool keysDown[512]{ false };

		// Refresh the keysDown array from the current key states
		void RefreshKeysDown();

		// Register notifiers for USB handles.  This sets up device
		// removal notifications for the gamepad and OPD handles.
		void RegisterDeviceNotifiers();

		// gamepad file handle, preparsed data, and notification handle
		HANDLE hGamepad = INVALID_HANDLE_VALUE;
		PHIDP_PREPARSED_DATA ppdGamepad = NULL;
		HANDLE hGamepadNotifier = NULL;
		OverlappedObject ovGamepad;

		// gamepad button usage
		std::vector<USAGE> gamepadButtonUsages;

		// gamepad report buffer
		std::vector<BYTE> gamepadReport;

		// gamepad button states, as a bit vector
		uint32_t gamepadButtonStates = 0;

		// Release the gamepad resources
		void ReleaseGamepadResources();

		// Open Pinball Device file handle, notification handle
		std::unique_ptr<OpenPinballDevice::Reader> openPinDevReader;
		HANDLE hOpenPinDevNotifier = NULL;

		// OpenPinDev button states
		uint32_t opdGenericButtonStates = 0;
		uint32_t opdPinballButtonStates = 0;

		// release Open Pinball Device resources
		void ReleaseOpenPinDevResources();

		// XInput controller ID
		int xInputId = -1;

		// control/command IDs
		static const UINT_PTR ID_SB_MAIN = 101;
		static const UINT_PTR ID_SB_DEVS = 102;
		static const UINT ID_HELP = 0x2010;  // bottom 4 bits are used by Windows, must be zero

		// button panel (left panel) metrics
		int cxPanel = 0;
		int cyHeader = 0;
		int cxPanelMin = 0;
		int cyButton = 0;
		const int yMarginButton = 5;

		// height of the keyboard/joystick layout area
		int cyKeyJoyPanel = 0;

		// button panel scrollbar
		int cxScrollbar = 0;
		HWND sbBtnPanel = NULL;
		int yScrollBtns = 0;

		// physical input panel scrollbar
		HWND sbPhysPanel = NULL;
		int yScrollPhys = 0;
		int physPanelDocHeight = 0;
		const int cyLineScrollPhys = 16;
	};

}
