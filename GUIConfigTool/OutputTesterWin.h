// Pinscape Pico - Output Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements an interactive window showing the states
// of the Pinscape Pico unit's logical output ports and physical
// output devices.  This is useful for testing and troubleshooting
// output device wiring and the software configuration settings.

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
#include "PinscapePicoAPI.h"
#include "TabEmbeddableWindow.h"
#include "DeviceThreadWindow.h"

namespace PinscapePico
{
	// Output tester  window
	class OutputTesterWin : public DeviceThreadWindow, public TabEmbeddableWindow
	{
	public:
		// factory subclass
		class Factory : public DeviceThreadWindow::Factory
		{
		public:

		protected:
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) {
				return new OutputTesterWin(hInstance, device);
			}
			const TCHAR *WindowTitle() const override { return _T("Pinscape Pico Output Tester"); }
			int Width() const override { return 1280; }
			int Height() const override { return 740; }
		};

		// Destruction.  Note that this blocks on our background updater
		// thread exiting.  If the caller wants to control the timing of
		// that more explicitly, use JoinThread(), which allows a timeout
		// to be specified.
		~OutputTesterWin();
		
		// select Logical Port/Device Test mode
		void SetLogicalPortMode();
		void SetDeviceTestMode();

		// set my menu bar in the host application
		virtual bool InstallParentMenuBar(HWND hwndContainer) override;

		// translate accelerators
		virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

		// command handler
		virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult);

		// TabEmbeddableWindow interface
		virtual bool OnDeviceReconnect() override;
		virtual void OnEraseDeviceConfig(bool factoryReset) override;

	protected:
		// protected constructor - clients use the factory
		OutputTesterWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// query device information - called during construction and on device reconnect
		void QueryDeviceInfo();

		// window class name
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoOutputTester"); }

		// window message handlers
		virtual void OnCreateWindow() override;
		virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
		virtual bool OnSysCommand(WPARAM id, WORD x, WORD y, LRESULT &lresult) override;
		virtual bool OnMouseMove(WPARAM keys, int x, int y) override;
		virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
		virtual bool OnLButtonUp(WPARAM keys, int x, int y) override;
		virtual bool OnCaptureChange(HWND hwnd) override;
		virtual bool OnKeyDown(WPARAM vkey, LPARAM flags) override;
		virtual void OnTimer(WPARAM timerId) override;

		// timer IDs
		static const int TIMER_ID_NUMBER_ENTRY = 301;

		// UI activation
		virtual void OnActivateUI(bool isAppActivate) override;

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// adjust the window layout
		void AdjustLayout();

		// menu/accelerator
		HMENU hMenuBar = NULL;
		HACCEL hAccel = NULL;

		// tooltips
		HWND tooltips = NULL;

		// output port status tooltip icon descriptors
		struct IconTip
		{
			IconTip(POINT pt, int cx, int cy, const char *text) : 
				rc({ pt.x, pt.y, pt.x + cx, pt.y + cy }), text(text) { }
			IconTip(const RECT &rc, const char *text) : rc(rc), text(text) { }
			RECT rc;
			const char *text;  // always a string with static storage duration
		};
		std::list<IconTip> iconTips;

		// output port configuration data
		int descErrorCode = PinscapeResponse::OK;
		std::vector<PinscapePico::OutputPortDesc> portDescs;
		std::vector<std::string> portNames;
		std::vector<PinscapePico::OutputDevDesc> deviceDescs;
		std::vector<PinscapePico::OutputDevPortDesc> devicePortDescs;

		// do any ports have non-empty names?
		bool havePortNames = false;

		// Number of daisy chains for TLC5940, TLC5947, and 74HC595.
		// We use the count to determine if we need to bother mentioning
		// which chain a particular chip belongs to.  Most commonly,
		// there's only one daisy chain per type, in which case it's
		// better to omit any mention of the chain ID.
		int numTLC5940 = 0;
		int numTLC5947 = 0;
		int num74HC595 = 0;

		// Thread context object.  This object is shared between the 
		// window and the thread.
		struct UpdaterThread : DeviceThreadWindow::UpdaterThread
		{
            virtual ~UpdaterThread();

            // test mode, as set in the UI
            bool uiTestMode = false;

            // test mode, as reported by the device
            bool devTestMode = false;

			// Next test mode update time.  We use a short timeout when
			// setting the device to test mode, so that the device will
			// exit test mode automatically if we crash or otherwise exit
			// without disabling test mode.  That means we have to keep
			// sending test mode commands to the device to extend the
			// timeout as long we want the mode to remain active.
			UINT64 nextTestModeCmdTime = 0;

            // latest output port states as reported by the device
            std::vector<PinscapePico::OutputLevel> portLevels;

			// pending logical port changes to send to the device
			std::unordered_map<uint8_t, uint8_t> portLevelChanges;

			// latest device port states as reported by the device
            std::vector<PinscapePico::OutputDevLevel> devLevels;

			// Pending device port changes.  These are indexed by
			// slider number.
			struct DevLevelChange
			{
				DevLevelChange(uint8_t devType, uint8_t configIndex, uint8_t port, uint16_t level) :
					devType(devType), configIndex(configIndex), port(port), level(level) { }

				uint8_t devType;
				uint8_t configIndex;
				uint8_t port;
				uint16_t level;
			};
			std::unordered_map<int, DevLevelChange> devLevelChanges;

			// update
			virtual bool Update(bool &releasedMutex) override;
		};

		// main/bold font "M" size (literally the size of the text "M")
		SIZE szMainFont{ 0, 0 };
		SIZE szBoldFont{ 0, 0 };

		// attribute icons bitmap
		HBITMAP bmpAttrIcons = NULL;
		int cxAttrIcons = 0;
		int cyAttrIcons = 0;

		// LedWiz waveform icons
		HBITMAP bmpLedWizWaveIcons = NULL;
		int cxLedWizWaveIcons = 0;
		int cyLedWizWaveIcons = 0;

		// control/command IDs
        static const UINT_PTR ID_SB_PORTS = 101;
        static const UINT_PTR ID_SB_DEVS = 102;
		static const UINT ID_HELP = 0x2010;  // leave low-order 4 bits empty, since Windows hijacks them in WM_SYSCOMMAND

		// output port panel metrics
		int cxPanel = 0;
		int cyHeaderLeft = 0;
		int cyHeaderRight = 0;
		int cxPanelMin = 0;
		int cyOutput = 0;
		const int yMarginOutput = 5;

		// test mode panel metrics
		int cyWarning = 0;
		int cyWarningPad = 8;

        // output panel scrollbar
		int cxScrollbar = 0;
		HWND sbPortPanel = NULL;
		int yScrollPorts = 0;

		// device panel scrollbar
		HWND sbDevPanel = NULL;
		int yScrollDev = 0;
		int devPanelDocHeight = 0;
		const int cyLineScrollDev = 16;

		// test mode panel scrollbar
		HWND sbTestMode = NULL;
		int yScrollTestMode = 0;
		int testModeDocHeight = 0;
		const int cyLineScrollTestMode = 16;

		// slider control
		struct SliderCtl
		{
			// current window client coordinates
			RECT rc{ 0, 0, 0, 0 };

			// number of steps
			int numSteps = 256;

			// current level setting, 0..numSteps
			int level = 0;

			// is this slider enabled?
			bool enabled = true;
		};

		// queue a slider change
		void QueueSliderChange(SliderCtl *ctl);

		// slider control with focus, as an index into logSlider 
		// or testModeSlider (according to the current mode), or -1
		// if no slider has focus
		int focusSlider = -1;

		// numeric entry for focus slider
		int focusNumberEntry = -1;

		// commit the number entry
		void CommitNumberEntry();

		// logical port slider controls
		std::vector<SliderCtl> logSlider;

		// device port slider
		struct DevSliderCtl : SliderCtl
		{
			// device information
			uint8_t devType;
			uint8_t configIndex;
			uint8_t port;
		};

		// test mode slider controls
		std::vector<DevSliderCtl> testModeSlider;

		// call a callback for each slider active in the current mode
		void ForEachSlider(std::function<bool(SliderCtl&, int index)> callback);

		// Track the active slider
		void TrackSlider(int x, int y);
		SliderCtl *trackingSlider = nullptr;

		// slider control metrics
		static const int cxSliderBar = 160;
		static const int cySliderBar = 8;
		static const int cxSliderThumb = 12;
		static const int cySliderThumb = 20;

		// PWMWorker Pico "update firmware" buttons
		struct PWMWorkerUpdateButton
		{
			PWMWorkerUpdateButton(const RECT &rc, int id) : rc(rc), id(id) { }
			RECT rc;		// screen area
			int id;			// worker Pico configuration index
		};
		std::list<PWMWorkerUpdateButton> workerUpdateButtons;

		// update a PWMWorker Pico's firmware
		void UpdatePWMWorkerFirmware(int id);

		// top tab controls
		int cyTabCtl = 0;
		static const int cyTabTopMargin = 6;
		static const int cyTabBottomMargin = 2;
		static const int cxTabLeftMargin = 4;
		static const int cyTabTopPadding = 4;
		static const int cyTabBottomPadding = 3;

		RECT rcTabNormalMode{ 0, 0, 0, 0 };
		RECT rcTabTestMode{ 0, 0, 0, 0 };
	};

}
