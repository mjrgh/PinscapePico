// Pinscape Pico - Config Tool Wrapper Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implements the top-level window for the Pinscape Pico GUI Config Tool.
// The window is a wrapper for the various component windows provided in the
// Pinscape Pico Device library, which handle specific device configuration
// and test functions.  We present a tabbed interface that allows selecting
// which tool component window to display, which we show as a child occupying
// the balance of the window under the tab control.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <sstream>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "PinscapePicoAPI.h"
#include "JSONExt.h"
#include "BaseWindow.h"
#include "FirmwareDropTarget.h"

// forward/external declarations
class HDCHelper;

// Wrapper window
using BaseWindow = PinscapePico::BaseWindow;
class WrapperWin : public BaseWindow, public FirmwareDropTarget::WindowIfc
{
public:
	// custom messages
	static const UINT MSG_DEV_REBOOTED = WM_USER + 101;  // device rebooted; WPARAM = 1 for device window, 2 for RP2 boot window
	static const UINT MSG_INSTALL_FIRMWARE = WM_USER + 102;  // firmware drop event on a button; initializes pendingFirmwareInstall with details

	// construction
	WrapperWin(HINSTANCE hInstance, const WCHAR *settingsFile, 
		bool useDefaultSettings = false, int initialUnitNum = -1);

	// destruction
	~WrapperWin();

	// translate accelerators
	virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

	// command handling
	virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult) override;
	virtual void OnInitMenuPopup(HMENU menu, WORD itemPos, bool isSysMenu) override;

	// Install firmware on a given device.  This is exposed as a public static
	// so that it can be invoked from the main window as well as tab windows.
	static void InstallFirmware(
		HINSTANCE hInstance, HWND hwndDialogParent,
		std::shared_ptr<VendorInterface::Shared> device,
		const TCHAR *filename);

	// get the saved window placement from the settings
	WINDOWPLACEMENT GetWindowPlacementFromSettings();

	// Record of last manual device reboot command.  When the user
	// explicitly reboots a device in Boot Loader mode from a Boot
	// Loader window, or reboots a Pinscape device from one of the
	// device windows, we record the time of the command here.  If 
	// we then see a new device of the opposite type attach within
	// a short time, we'll automatically switch to the new device
	// window, on the assumption that this is the same device
	// represented by the boot loader window, now reconnected in
	// the new mode desired by the boot.  When the user explicitly
	// reboots a device into the opposite mode, it's a good bet
	// that they intend to work with the device in its new mode,
	// so the natural workflow is to switch to the new window.
	// This is just an attempt to save that manual step.
	struct LastRebootCommand
	{
		// set from a MSG_DEV_REBOOTED command
		enum class Source;
		void Set(WPARAM wparam) { Set(static_cast<Source>(wparam)); }
		void Set(Source source)
		{
			this->source = static_cast<Source>(source);
			time = GetTickCount64();
		}

		// test for a recent command of the given type
		bool Test(Source source) {
			return (this->source == source && GetTickCount64() < time + 3000);
		}

		// source window type
		enum class Source { Device, BootLoader } source = Source::Device;

		// time of the explicit command
		UINT64 time = 0;
	};

	// FirmwareDropTarget::WindowIfc implementation
	virtual bool IsFirmwareDropLocation(POINTL ptl) override;
	virtual void ExecFirmwareDrop(POINTL pt, const TCHAR *filename) override;

protected:
	// drag/drop helper
	FirmwareDropTarget *dropTarget{ new FirmwareDropTarget(this) };

	// pending firmware install file and button
	struct PendingFirmwareInstall
	{
		TSTRING filename;
		uint64_t buttonID = 0;
	};
	PendingFirmwareInstall pendingFirmwareInstall;

	// process the pending firmware installation
	void ProcessPendingFirmwareInstall();

	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoConfigTool"); }

	// window message handlers
	virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;
	virtual void OnCreateWindow() override;
	virtual bool OnClose() override;
	virtual void OnDestroy() override;
	virtual void OnActivate(WPARAM code, HWND other) override;
	virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
	virtual bool OnMouseMove(WPARAM keys, int x, int y) override;
	virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
	virtual bool OnLButtonUp(WPARAM keys, int x, int y) override;
	virtual bool OnRButtonDown(WPARAM keys, int x, int y) override;
	virtual bool OnCaptureChange(HWND hwnd) override;
	virtual bool OnNCHitTest(int x, int y, LRESULT &lresult) override;
	virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) override;

	// Device node change notification.  This handles the Windows broadcast
	// message sent to top-level windows when devices are plugged or unplugged.
	// We use this to refresh our current device list.
	virtual bool OnDevNodesChanged() override;

	// Device removal notification.  This handles messages for the shared
	// devices that we've registered to receive notifications.
	virtual bool OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr) override;

	// Paint off-screen.  This prepares a bitmap with the window
	// contents filled in, for display in the next WM_PAINT.
	virtual void PaintOffScreen(HDC hdc) override;

	// adjust the window layout
	void AdjustLayout();

	// main/bold font "M" size (literally the size of the text "M")
	SIZE szMainFont{ 0, 0 };
	SIZE szBoldFont{ 0, 0 };

	// default menu bar and accelerator table
	HMENU defaultMenuBar = NULL;
	HACCEL defaultAccel = NULL;

	// context menus for device buttons
	HMENU deviceButtonContextMenu = NULL;
	HMENU bootLoaderButtonContextMenu = NULL;

	// control/command IDs
	static const UINT_PTR ID_SB_LEFTPANEL = 101;
	static const UINT ID_HELP = 1001;

	// Device menu command handlers
	void Reboot();
	void RebootSafeMode();
	void RebootFactoryMode();
	void BootLoaderMode();
	void InstallFirmware(int cmd);
	void ExportFlash(int cmd);
	void ClearConfig();
	void FactoryReset();
	void RepairJoyCpl();
	void I2CBusScan();

	// Check with open windows prior to a config erase/factory reset 
	// action, and prompt the user for confirmation.  Returns true if
	// the user clicks through the warning, false if the user cancels.
	bool PreEraseDeviceConfig(bool factoryReset, const char *warning, const char *question);

	// notify windows of a configuration erase/factory reset action
	void OnEraseDeviceConfig(bool factoryReset);

	// Execute a command on the currently selected device.  If a device
	// is selected in the UI, and the device is connected, this acquires
	// the device connection lock and invokes the callback, then displays
	// the success message, or constructs an error message based on the
	// command description.  Returns true on success, false on failure.
	// The callback takes a device handle and returns a PinscapeResponse::
	// status code (OK or ERR_xxx).
	bool ExecDeviceCommand(std::function<int(VendorInterface*)> callback,
		const char *cmdDesc, const char *successMsg);

	// tab window creation functions - devices
	struct Tab;
	void CreateNoDevWin();
	void CreateOverviewWin();
	void CreateLogWin();
	void CreateMainConfigWin();
	void CreateSafeModeConfigWin();
	void CreateButtonTesterWin();
	void CreateOutputPortTesterWin();
	void CreateOutputDevTesterWin();
	void CreatePlungerWin();
	void CreateNudgeWin();
	void CreateIRWin();

	// switch to the log window
	void SwitchToLogWin();

	// Test if a device is offline, and display the Offline Device
	// placeholder window for it if so.  Returns true if the placeholder
	// window was displayed.  On a false return, the caller can create the
	// actual tool window for the tab as desired.
	bool TestShowOfflineDeviceWin(Tab *tab);

	// tab window creation - boot loader drives
	void CreateBootLoaderDriveWin();

	// tab window creation - new device setup
	void CreateSetUpNewDeviceWin();

	// common handler for config editor tabs
	void CreateConfigWin(uint8_t fileID);

	// A common "Offline Device" window, to display in lieu of the
	// actual tool window when the current tab is associated with
	// a device that's currently offline.
	std::shared_ptr<BaseWindow> offlineDeviceWin;

	// tab window list
	struct Tab
	{
		Tab(const char *label, void (WrapperWin::*createWindow)()) :
			label(label), createWindow(createWindow) { }

		// tab label
		const char *label;

		// window creation callback
		void (WrapperWin::*createWindow)();

		// child window, displayed when this tab is selected
		std::shared_ptr<BaseWindow> win;

		// UI refresh pending.  We set this flag when we detect that
		// the device has been reconnected after a period of being
		// disconnected.  When the window is next activated in the
		// UI, if this flag is set, we notify it of the reconnect so
		// that it can refresh its UI from the possibly updated device
		// configuration.
		bool uiRefreshPending = false;

		// on-screen tab rectangle
		RECT rc{ 0, 0, 0, 0 };
	};

	// select a tab
	void SelectTab(Tab &tab);

	// Left panel button.  This represents a button in the device selector
	// panel on the left side of the main window.  We have three kinds
	// of buttons: device buttons, which correspond to attached Pinscape
	// Pico units; boot loader drive buttons, which correspond to virtual
	// disk drives presented by Picos in RP2 Boot Loader mode; and an
	// "new device" button, which brings up a help screen explaining
	// how to get a Pico into Boot Loader mode for firmware installation.
	struct LeftPanelButton
	{
		LeftPanelButton(int sortGroup) : sortGroup(sortGroup) { }
		virtual ~LeftPanelButton() { }

		// context menu for the button
		HMENU hContextMenu = NULL;

		// Each button gets a unique identifier at creation, so that we
		// can find it again across a PostMessage event.  We use this
		// instead of a pointer in PostMessage so that we don't keep
		// pointers to objects that are destroyed before the message
		// is received.
		uint64_t uniqueID = nextUniqueID++;
		static uint64_t nextUniqueID;

		// firmware drop target
		virtual bool IsFirmwareDropTarget() const { return false; }
		virtual void ExecFirmwareDrop(WrapperWin *win, const TCHAR *filename) { }

		// Sorting group.  The buttons are arranged in groups: Pinscape
		// Pico units first, then RP2 Boot Loader drives, then the 
		// New Device button.
		int sortGroup;
		static const int DEVICE_SORT_GROUP = 1;
		static const int BOOTLOADER_SORT_GROUP = 2;
		static const int NEWDEV_SORT_GROUP = 3;

		// Draw the button.  Returns the bitmap to display in the icon 
		// area, or NULL for no icon.
		virtual HBITMAP Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc) = 0;

		// status line color/text
		virtual COLORREF GetStatuslineColor() const = 0;
		virtual void GetStatuslineText(char *buf, size_t bufSize) = 0;

		// get my display name, for messages
		virtual std::string GetDisplayName() const = 0;

		// Compare to another device for sorting the list.  Returns
		// true if this device sorts ahead of the other device.
		// For devices in different sort groups, this is determined
		// by the relative order of the sort groups.  For devices
		// in the same sort group, this uses the virtual comparer.
		bool SortCompare(const LeftPanelButton *other) const {
			return sortGroup != other->sortGroup ?
				sortGroup < other->sortGroup :
				SortCompareLikeKind(other);
		}
		virtual bool SortCompareLikeKind(const LeftPanelButton *other) const = 0;

		// Activate the button.  The main window calls this when
		// the user makes the button active.
		virtual void Activate(WrapperWin *win) = 0;

		// on-screen (clint-relative) rectangle
		RECT rc{ 0 };

		// the list of tab windows associated with the button
		std::list<Tab> tabs;

		// currently selected tab
		Tab *curTab = nullptr;
	};
	std::list<std::unique_ptr<LeftPanelButton>> leftPanelButtons;

	// initial button selection, if any
	LeftPanelButton *initialButtonSelection = nullptr;

	// currently selected button
	LeftPanelButton *curButton = nullptr;

	// get the currently selected tab
	Tab *GetCurTab() const { return curButton != nullptr ? curButton->curTab : nullptr; }

	// deactivate the current tab
	void DeactivateTab();

	// sort the button list
	void SortButtonList();

	// select a button
	void SelectButton(LeftPanelButton *button);

	// Current known devices.  We track devices by hardware ID.  We
	// keep track of devices that we've ever seen even if they go
	// offline during the session.
	struct DeviceButton;
	struct DeviceDesc
	{
		// Device information
		using PicoHardwareId = PinscapePico::PicoHardwareId;
		struct ID
		{
			ID() { }
			ID(const wchar_t *path, int unitNum, const char *unitName, const PicoHardwareId &hwId) :
				path(path), unitNum(unitNum), unitName(unitName), hwId(hwId) { }

			// Windows device path
			std::wstring path;

			// Pinscape Pico unit number (user-assigned in the config)
			int unitNum = 0;

			// Unit name (user-assigned in the config)
			std::string unitName;

			// Pico hardware ID
			PicoHardwareId hwId;
		};

		// construction
		DeviceDesc(const ID &id) : id(id) { }
		DeviceDesc(const wchar_t *path, int unitNum, const char *unitName, const PicoHardwareId &hwId) :
			id(path, unitNum, unitName, hwId) { }

		// connect or reconnect the device
		HRESULT Connect(WrapperWin *win, bool reconnect);

		// device identifiers
		ID id;

		// Vendor interface handle.  This is populated when we first
		// connect to the device.  We use a shared-access struct so
		// that we can share this among multiple tabbed windows and
		// their background threads.
		std::shared_ptr<VendorInterface::Shared> device;

		// is the device currently online?
		bool online = true;

		// Was the device online at the last check?  The periodic
		// device re-scan uses this to detect when devices are
		// added and removed, so that it can update the window
		// contents on reconnection.
		bool wasOnline = true;

		// Time since boot, as of last check, and local system timestamp
		// at last check.  A device reset sometimes leaves the existing 
		// WinUsb device handle alive after the reset (it seems to be a
		// matter of how long the reset takes: the connection sometimes
		// survives a reset that completes within a few hundred
		// milliseconds).  So we can't always detect a reset by device
		// handle validity.  But if the projected time-since-boot is less
		// now than the last time we checked, we can be sure the device
		// has gone through a reset.  Note that we need to figure the
		// proleptic (projected) uptime from the last check, so we need
		// to record both the uptime reading and the time we took the
		// reading.
		uint64_t upTime = 0;
		UINT64 upTimeTimestamp = 0;

		// UI button for the device
		DeviceButton *button = nullptr;
	};
	std::unordered_map<std::string, DeviceDesc> devices;

		// handle device list updates
	struct DeviceDesc;
	void OnNewDeviceList(const std::list<DeviceDesc::ID> &newDevList);
	void OnNewBootDriveList(const RP2BootDevice::RP2BootDeviceList &newDriveList);

	// Enumerate devices
	HRESULT EnumerateDevices(std::list<DeviceDesc::ID> &devices);

	// get the device for the active tab, if any
	DeviceDesc *GetActiveDevice();

	// add a device
	DeviceButton *AddDevice(const DeviceDesc::ID &desc);

	// validate that a device command is possible; returns the device
	// descriptor so, shows an error message and returns nullptr if not
	DeviceDesc *CanExecDeviceCommand(bool silent);

	// Device button.  This is a left-panel button that corresponds
	// to an attached Pinscape Pico unit.
	struct DeviceButton : LeftPanelButton
	{
		DeviceButton(DeviceDesc *dev) : LeftPanelButton(DEVICE_SORT_GROUP), dev(dev) { }
		virtual HBITMAP Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc) override;
		virtual COLORREF GetStatuslineColor() const override { return HRGB(dev->online ? 0x204080 : 0xA00000); }
		virtual void GetStatuslineText(char *buf, size_t bufSize) override {
			sprintf_s(buf, bufSize, "Pinscape Pico unit %d (%s) | %s",
				dev->id.unitNum, dev->id.unitName.c_str(), dev->online ? "ONLINE" : "OFFLINE");
		}

		// devices are valid firmware drop targets
		virtual bool IsFirmwareDropTarget() const { return true; }
		virtual void ExecFirmwareDrop(WrapperWin *win, const TCHAR *filename);

		// device descriptor
		DeviceDesc *dev;

		// use the device name as the display name 
		virtual std::string GetDisplayName() const override {
			char buf[128];
			sprintf_s(buf, "Unit #%d (%s)", dev->id.unitNum, dev->id.unitName.c_str());
			return buf;
		}

		// sort relative to other device buttons according to unit number
		virtual bool SortCompareLikeKind(const LeftPanelButton *other) const override {
			return dev->id.unitNum < static_cast<const DeviceButton*>(other)->dev->id.unitNum;
		}

		// Activate.  This establishes a connection to the device if
		// we don't already have one, and makes the device current.
		virtual void Activate(WrapperWin *win) override;
	};

	// New Device Setup button
	struct SetUpNewDeviceButton : LeftPanelButton
	{
		SetUpNewDeviceButton() : LeftPanelButton(NEWDEV_SORT_GROUP) { }
		virtual HBITMAP Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc) override;
		virtual COLORREF GetStatuslineColor() const override { return HRGB(0x208020); }
		virtual void GetStatuslineText(char *buf, size_t bufSize) override {
			strcpy_s(buf, bufSize, "Select a Pinscape Pico unit in the left panel to access configuration tools");
		}

		virtual std::string GetDisplayName() const override { return "New Device Setup"; }

		// sort - this button is unique, so this will never be called and
		// thus only needs a trivial implementation
		virtual bool SortCompareLikeKind(const LeftPanelButton *) const override { return true; }

		// activation - no action required
		virtual void Activate(WrapperWin *win) override { }
	};

	// Boot Loader Drive button
	struct BootLoaderDriveButton : LeftPanelButton
	{
		BootLoaderDriveButton(const RP2BootDevice &dev) :
			LeftPanelButton(BOOTLOADER_SORT_GROUP), dev(dev) { }
		virtual HBITMAP Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc) override;
		virtual COLORREF GetStatuslineColor() const override { return HRGB(0x902890); }
		virtual void GetStatuslineText(char *buf, size_t bufSize) override {
			sprintf_s(buf, bufSize, "Pico Boot Loader drive %" _TSFMT "selected", dev.path.c_str());
		}

		// devices are valid firmware drop targets
		virtual bool IsFirmwareDropTarget() const { return true; }
		virtual void ExecFirmwareDrop(WrapperWin *win, const TCHAR *filename);

		// drive letter/path
		RP2BootDevice dev;

		virtual std::string GetDisplayName() const override { 
			char buf[128];
			sprintf_s(buf, "Boot Loader drive %" _TSFMT, dev.path.c_str());
			return buf;
		}

		// sort by drive letter
		virtual bool SortCompareLikeKind(const LeftPanelButton *other) const override {
			return _wcsicoll(dev.path.c_str(), static_cast<const BootLoaderDriveButton*>(other)->dev.path.c_str());
		}

		// activate - no action required
		virtual void Activate(WrapperWin *win) override { }
	};

	// add a boot loader button
	BootLoaderDriveButton *AddBootLoaderButton(const RP2BootDevice &dev);

	// list of RP2 boot loader drives found at last enumeration
	struct BootLoaderDrive
	{
		BootLoaderDrive(const RP2BootDevice &dev, BootLoaderDriveButton *button) :
			dev(dev), button(button) { }

		// drive descriptor
		RP2BootDevice dev;

		// button
		BootLoaderDriveButton *button;
	};
	std::list<BootLoaderDrive> bootLoaderDrives;

	// last reboot command source
	LastRebootCommand lastRebootCommand;
	
	// left panel - device list
	static const int cxLeftPanel = 220;
	int cyDeviceButton = 120;

	// left panel scrollbar
	HWND sbLeftPanel = NULL;
	const int cxScrollbar{ GetSystemMetrics(SM_CXVSCROLL) };
	int yScrollLeftPanel = 0;
	static const int cyLeftPanelScrollLine = 20;

	// left panel icons
	HBITMAP bmpPinscapeDevice = NULL;
	HBITMAP bmpPinscapeOffline = NULL;
	HBITMAP bmpBootDrive = NULL;
	HBITMAP bmpNewDevice = NULL;
	SIZE szBmpIcons{ 0, 0 };

	// margin to right panel
	static const int cxPanelMargin = 5;

	// top tab control
	int cyTabCtl = 0;
	static const int cyTabTopMargin = 6;
	static const int cyTabBottomMargin = 2;
	static const int cxTabLeftMargin = 0;
	static const int cyTabTopPadding = 4;
	static const int cyTabBottomPadding = 3;

	// status line
	int cyStatusline = 32;

	// status line data
	struct StatusLineMsg
	{
		COLORREF fg = HRGB(0xffffff);      // foreground (text) color
		COLORREF bg = HRGB(0x0000E0);      // background color
		std::string msg;                   // text
	};

	// Status line data from the config editor window.  The
	// config editor uses the status line to display error
	// messages after JSON syntax checks.
	StatusLineMsg configEditorStatus;
};
