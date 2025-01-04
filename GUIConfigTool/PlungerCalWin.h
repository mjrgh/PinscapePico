// Pinscape Pico - Plunger Calibration Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements an interactive window that performs plunger
// calibration on a Pinscape Pico device.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <fstream>
#include <Windows.h>
#include <CommCtrl.h>
#include "PinscapePicoAPI.h"
#include "TabEmbeddableWindow.h"
#include "DeviceThreadWindow.h"

namespace PinscapePico
{
	// Plunger calibration window
	class PlungerCalWin : public DeviceThreadWindow, public TabEmbeddableWindow
	{
	public:
		// factory subclass
		class Factory : public DeviceThreadWindow::Factory
		{
		public:

		protected:
			virtual DeviceThreadWindow *New(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) {
				return new PlungerCalWin(hInstance, device);
			}
			const TCHAR *WindowTitle() const override { return _T("Pinscape Pico Plunger Calibration"); }
			int Width() const override { return 960; }
			int Height() const override { return 740; }

			// filter for devices with plungers attached
			virtual FeedbackDeviceList FilterDevices(const FeedbackDeviceList list) override;
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
		~PlungerCalWin();

	protected:
		// protected constructor - clients use the factory
		PlungerCalWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// query device information - called during construction and on device reconnect
		void QueryDeviceInfo();

		// Perform an operation while holding the device mutex.  The
		// function returns a PinscapeResponse::OK / ERR_xxx status code.
		// Returns the status code returned from the function, or
		// ERR_TIMEOUT if we couldn't acquire the mutex.
		int WithDeviceLock(const char *desc, std::function<int()> func, DWORD timeout_ms = 1000);

		// capture file, when active
		std::unique_ptr<std::fstream> captureFile;

		// frame capture file, when active
		std::unique_ptr<std::fstream> frameCaptureFile;

		// modification flag - cached from the latest USB update
		bool isSettingModified = false;

		// menu/accelerator
		HMENU hMenuBar = NULL;
		HACCEL hAccel = NULL;

		// window class name
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoPlunger"); }

		// window message handlers
		virtual void OnCreateWindow() override;
		virtual void OnNCDestroy() override;
		virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override { layoutPending = true; }
		virtual void OnTimer(WPARAM timerId) override;

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// integer parameter - a text box and an internal representation
		struct IntEditBox
		{
			using Setter = std::function<int(VendorInterface *device, uint32_t val)>;

			IntEditBox(uint32_t minVal, uint32_t maxVal, const char *name, Setter send) :
				minVal(minVal), maxVal(maxVal), name(name), send(send) { }

			// set the UI value
			void Set(uint32_t val);

			// update the UI value
			void Update(uint32_t val);

			// check for changes and send to device as needed
			void CheckChanges(PlungerCalWin *win);

			// send the current values to the device
			void SendToDevice(PlungerCalWin *win);

			// current value
			uint32_t curVal = 0;

			// is the current value valid?
			bool valid = false;

			// original value loaded from the device side or last sent to the device
			uint32_t origVal = 0;

			// name, for error reporting
			const char *name;

			// value range
			uint32_t minVal;
			uint32_t maxVal;

			// setter function
			Setter send;

			// edit box and optional spinner control
			HWND edit = NULL;
			HWND spin = NULL;
		};

		// byte parameter combo box
		struct ByteComboBox
		{
			using Setter = std::function<int(VendorInterface *device, uint8_t val)>;

			ByteComboBox(const char *name, Setter send) :
				name(name), send(send) { }

			// set the UI value
			void Set(uint8_t val);

			// check for changes in the UI, send to device as needed
			void CheckChanges(PlungerCalWin *win);

			// send the current values to the device
			void SendToDevice(PlungerCalWin *win);

			// current value
			uint8_t curVal = 0;

			// name, for error reporting
			const char *name;

			// setter function
			Setter send;

			// combo box
			HWND combo = NULL;
		};

		// checkbox parameter
		struct Checkbox
		{
			using Setter = std::function<int(VendorInterface *device, bool)>;

			Checkbox(const char *name, Setter send) : name(name), send(send) { }

			// set the UI value
			void Set(bool val);

			// check for changes, send to device as needed
			void CheckChanges(PlungerCalWin *win);

			// send the current values to the device
			void SendToDevice(PlungerCalWin *win);

			// current value
			bool curVal = false;

			// setter function
			Setter send;

			// name, for error reporting
			const char *name;

			// checkbox control
			HWND ck = NULL;
		};

		// jitter filter edit field
		IntEditBox jitter{ 0, UINT32_MAX, "jitter filter window",
			[](VendorInterface *device, uint32_t val) { return device->SetPlungerJitterFilter(static_cast<int>(val)); } };

		// firing time edit field
		IntEditBox firingTime{ 0, 2000, "maximum firing time",
			[](VendorInterface *device, uint32_t val) { return device->SetPlungerFiringTime(val * 1000); } };

		// integration time edit field
		IntEditBox integrationTime{ 0, 4000000, "integration time",
			[](VendorInterface *device, uint32_t val) { return device->SetPlungerIntegrationTime(val); } };

		// scaling factor edit field
		IntEditBox scalingFactor{ 0, 500, "manual scaling factor",
			[](VendorInterface *device, uint32_t val) { return device->SetPlungerScalingFactor(val); } };

		// reverse orientation checkbox
		Checkbox reverseOrientation{ "reverse orientation",
			[](VendorInterface *device, bool val) { return device->SetPlungerOrientation(val); } };

		// scan mode combo
		ByteComboBox scanMode{ "scan mode",
			[](VendorInterface *device, uint8_t val) { return device->SetPlungerScanMode(val); } };

		// populate the scan mode combo
		void PopulateScanModeCombo();

		// plunger configuration data
		PinscapePico::PlungerConfig plungerConfig{ 0 };

		// plunger sensor type name
		int sensorType = FeedbackControllerReport::PLUNGER_NONE;
		std::string sensorTypeName;

		// is this an image sensor?
		bool isImageSensor = false;

		// Post-calibration reading.  When the user initiates calibration
		// through this window, we capture the results of the calibration
		// here, so that we can restore it if the device resets while the
		// window is still open but before the user commits the new
		// calibration to flash.
		PinscapePico::PlungerCal lastCalData{ sizeof(lastCalData) };
		bool lastCalDataValid = false;
		bool lastCalDataPending = false;

		// Thread context object.  This object is shared between the 
		// window and the thread.
		struct UpdaterThread : DeviceThreadWindow::UpdaterThread
		{
			// latest plunger data
			PinscapePico::PlungerReading reading{ 0 };
			std::vector<BYTE> sensorData;

			// timestamp of last reading
			UINT64 timestamp = 0;

			// last observed zero-crossing
			struct ZeroCrossing
			{
				UINT64 time = 0;
				int16_t speed = 0;
				int32_t deltaZ = 0;
			} zeroCrossing;

			// capture files (reference only; controlling copy kept in main window)
			std::fstream *captureFile = nullptr;
			std::fstream *frameCaptureFile = nullptr;

			// update
			virtual bool Update(bool &releasedMutex) override;
		};

		// fonts
		HFONT barFont = NULL;

		// control windows
		HWND btnCalibrate = NULL;
		HWND btnSaveSettings = NULL;
		HWND btnRevertSettings = NULL;
		HWND btnHelp = NULL;
		HWND btnCapture = NULL;
		HWND btnFrameCapture = NULL;

		// check controls for changes, send to the device as needed
		void CheckControlChanges();

		// initialize the UI controls from a current device reading
		void InitControls();
		void InitControls(const PlungerReading &pd);

		// update controls with the latest reading
		void UpdateControls(const PlungerReading &pd);

		// timestamp of last reading taken from updater thread
		UINT64 lastReadingTimestamp = 0;

		// control IDs
		static const UINT_PTR ID_CALIBRATE = 101;
		static const UINT_PTR ID_JITTER_TXT = 102;
		static const UINT_PTR ID_JITTER_SPIN = 103;
		static const UINT_PTR ID_FIRING_TIME_TXT = 104;
		static const UINT_PTR ID_FIRING_TIME_SPIN = 105;
		static const UINT_PTR ID_INT_TIME_TXT = 106;
		static const UINT_PTR ID_INT_TIME_SPIN = 107;
		static const UINT_PTR ID_REVERSE_CB = 108;
		static const UINT_PTR ID_SCALING_FACTOR_TXT = 109;
		static const UINT_PTR ID_SCALING_FACTOR_SPIN = 110;
		static const UINT_PTR ID_SCAN_MODE_CB = 111;
		static const UINT_PTR ID_SAVE_BTN = 200;
		static const UINT_PTR ID_REVERT_BTN = 201;
		static const UINT_PTR ID_HELP_BTN = 202;
		static const UINT_PTR ID_CAPTURE_BTN = 203;
		static const UINT_PTR ID_CAPTURE_FRAME_BTN = 204;

		// flag: layout pending in paint
		bool layoutPending = true;

		// peak recent forward (negative) plunger speed, and time observed
		int peakForwardSpeed = 0;
		UINT64 tPeakForwardSpeed = 0;
	};

}

