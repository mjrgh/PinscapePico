// Pinscape Pico - Config Tool - Nudge Device Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements an interactive window for testing the Pinscape
// Pico's nudge device.  The nudge device interprets readings from a
// 3-axis accelerometer into nudge input to pinball simulators on the
// PC.  This window provides a visualization of the accelerometer data
// and the nudge device status.


#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <map>
#include <fstream>
#include <Windows.h>
#include <CommCtrl.h>
#include "PinscapePicoAPI.h"
#include "TabEmbeddableWindow.h"
#include "DeviceThreadWindow.h"
#include "MicroPinSim.h"
#include "HiResTimer.h"

// Enable extra controls for debugging/tuning the mini simulator?
// #define MINI_SIM_MODEL
// #define SIM_DEBUG_CONTROLS

namespace PinscapePico
{
	// Nudge device window
	class NudgeDeviceWin : public DeviceThreadWindow, public TabEmbeddableWindow
	{
	public:
		// factory subclass
		class Factory : public DeviceThreadWindow::Factory
		{
		public:

		protected:
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) {
				return new NudgeDeviceWin(hInstance, device);
			}
			const TCHAR *WindowTitle() const override { return _T("Pinscape Pico Nudge Device Viewer"); }
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
		virtual bool IsDocumentModified() const override { return isSettingModified; }
		virtual bool OnDeviceReconnect() override;
		virtual bool PreEraseDeviceConfig(bool factoryReset) const override { return isSettingModified; }
		virtual void OnEraseDeviceConfig(bool factoryReset) override;

		// Destruction.  Note that this blocks on our background updater
		// thread exiting.  If the caller wants to control the timing of
		// that more explicitly, use JoinThread(), which allows a timeout
		// to be specified.
		~NudgeDeviceWin();

	protected:
		// protected constructor - clients use the factory
        NudgeDeviceWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// query device information - called during construction and on device reconnect
		void QueryDeviceInfo();

		// capture file, when active
		std::unique_ptr<std::fstream> captureFile;

		// last parameters read from device
		PinscapePico::NudgeParams nudgeParams{ 0 };

		// reload the nudge params from the device
		void ReloadNudgeParams(bool force);

		// last nudge parameter poll time
		UINT64 tNudgeParamCheck = 0;

		// menu/accelerator
        HMENU hMenuBar = NULL;
        HMENU hCtxMenu = NULL;
		HACCEL hAccel = NULL;

		// fonts
		HFONT simDataFont = NULL;
		TEXTMETRIC tmSimData{ 0 };

		// control IDs
		static const int ID_BTN_CALIBRATE = 320;
		static const int ID_BTN_CENTER = 321;
		static const int ID_BTN_CAPTURE = 322;
		static const int ID_CK_AUTOCENTER = 323;
		static const int ID_EDIT_AUTOCENTERTIME = 324;
		static const int ID_EDIT_QUIETX = 325;
		static const int ID_EDIT_QUIETY = 326;
		static const int ID_EDIT_QUIETZ = 327;
		static const int ID_EDIT_DCTIME = 328;
		static const int ID_EDIT_JITTERX = 329;
		static const int ID_EDIT_JITTERY = 330;
		static const int ID_EDIT_JITTERZ = 331;
		static const int ID_EDIT_VDECAY = 332;
		static const int ID_EDIT_VSCALE = 333;
		static const int ID_BTN_SAVE = 334;
		static const int ID_BTN_REVERT = 335;

		// control windows
		HWND calBtn = NULL;
		HWND centerBtn = NULL;
		HWND btnCapture = NULL;
		HWND autoCenterCk = NULL;
		HWND autoCenterTimeEdit = NULL;
		HWND quietXEdit = NULL;
		HWND quietYEdit = NULL;
		HWND quietZEdit = NULL;
		HWND dcTimeEdit = NULL;
		HWND jitterXEdit = NULL;
		HWND jitterYEdit = NULL;
		HWND jitterZEdit = NULL;
		HWND velocityDecayEdit = NULL;
		HWND velocityScaleEdit = NULL;
		HWND saveBtn = NULL;
		HWND revertBtn = NULL;

		// handle a change to the nudge parameters
		void OnNudgeParamChange();

		// flag: loading control values -> ignore EN_CHANGE
		bool loadingControlValues = false;

		// process an EN_CHANGE
		bool OnEditChange(HWND edit);

		// edit control EN_CHANGE processing routines
		using EditChangeProc = std::function<void(const char*)>;
		std::unordered_map<UINT_PTR, EditChangeProc> editChangeProcs;

		// Set up an edit control - create its window, and set its change processing
		// routine and initial value
		HWND CreateEdit(UINT id, int cx, int cy, const char *initialValue, EditChangeProc changeProc);
		HWND CreateEdit(UINT id, int cx, int cy, float initialValue, EditChangeProc changeProc);

		// bitmaps
		HBITMAP crosshairs = NULL;
		SIZE szCrosshairs;

		// initial layout needed
		bool layoutPending = true;

		// Last auto-centering time-ago update.  The display is too busy when
		// we update this continuously, so we only update it periodically.
		UINT64 lastAutoCenterDisplayTime = 0;
		uint64_t lastAutoCenterTime;

		// Are any settings modified?  We set this from the modified flag when
		// we process a nudge status report from the device.
		bool isSettingModified = false;

		// window class name
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoNudgeWin"); }

		// window message handlers
		virtual void OnCreateWindow() override;
		virtual void OnNCDestroy() override;
		virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
		virtual bool OnLButtonUp(WPARAM keys, int x, int y) override;
		virtual bool OnMouseMove(WPARAM keys, int x, int y) override;
		virtual bool OnCaptureChange(HWND hwnd) override;
		virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) override;
		virtual bool OnKeyDown(WPARAM vkey, LPARAM flags);

		// control subclass customizations
		virtual LRESULT ControlSubclassProc(
			HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass) override;

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// physics simulator on-scree drawing area
		RECT rcSim{ 0 };

		// physics simulator scaling factor - pix/mm (on-screen pixels, simulation millimeters)
		float simScale = 1.0f;

		// show numerical visualizations in simulation
		bool simDataView = false;

		// tracking a mouse click in the simulation
		bool trackingSimClick = false;
		MicroPinSim::Ball *trackingSimBall = nullptr;

		// Handle a click or drag event in the simulator
		void OnSimMouseEvent(UINT event, WPARAM keys, int x, int y);

		// Thread context object.  This object is shared between the 
		// window and the thread.
		struct UpdaterThread : DeviceThreadWindow::UpdaterThread
		{
			UpdaterThread();

			// update
            virtual bool Update(bool &releasedMutex) override;

            // last accelerometer query data - written only by the updater thread
			PinscapePico::NudgeStatus nudgeStatus{ 0 };

			// handle a change in nudge parameters from the main thread
			void OnNudgeParamsChange(const PinscapePico::NudgeParams *np);

			// Velocity scaling factor, to convert from INT16 device units
			// to mm/s.  This is set from the main thread whenever it updates
			// the nudge parameters.
			float velocityScalingFactor = 1.0f;

			// capture file
			std::fstream *captureFile = nullptr;

			// Mini pinball model.  This aids in tuning the position model
			// parameters by letting you see the effects on a simple physics
			// simulation without switching to a different program.
			MicroPinSim::Table pinModel;

			// run-to-collision mode
			void SimSetRunToCollisionMode(bool f) { pinModel.SetCollisionStepMode(f); }

			// simulator time step (seconds)
			const float simTimeStep = 0.0005f;
			UINT simTimeStepsPerSecond = static_cast<UINT>(roundf(1.0f / simTimeStep));

			// last model update time, as a system tick count
			UINT64 lastPinModelUpdateTime = 0;

			// high-resolution system timer
			HiResTimer hrt;
		};

	};
}

