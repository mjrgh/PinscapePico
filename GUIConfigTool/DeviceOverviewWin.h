// Pinscape Pico - Config Tool - Device Overview window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This window displays basic information on a Pinscape Pico unit,
// and provides a UI for installing new firmware onto the device
// (without the need to go through the manual BOOTSEL reset
// procedure).

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
#include "BaseDeviceWindow.h"
#include "TabEmbeddableWindow.h"
#include "FirmwareDropTarget.h"

using BaseDeviceWindow = PinscapePico::BaseDeviceWindow;
class DeviceOverviewWin : public BaseDeviceWindow, public TabEmbeddableWindow, public FirmwareDropTarget::WindowIfc
{
public:
	// construction
    DeviceOverviewWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

    // destruction
    ~DeviceOverviewWin();

    // set my menu bar in the host application
    virtual bool InstallParentMenuBar(HWND hwndContainer) override;

    // translate accelerators
    virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

    // command handling
    virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult) override;

    // FirmwareDropTarget::WindowIfc implementation
    virtual bool IsFirmwareDropLocation(POINTL ptl) override;
    virtual void ExecFirmwareDrop(POINTL pt, const TCHAR *filename) override;

    // TabEmbeddableWindow interface
    virtual bool IsVisibleOffline() const override { return true; }
    virtual bool OnDeviceReconnect() override;
    virtual void OnEraseDeviceConfig(bool factoryReset) override;

protected:
	// window class
	const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoDevOverviewWin"); }

    // private messages
    static const UINT MSG_INSTALL_FIRMWARE = WM_USER + 201;

    // window message handlers
    virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;
    virtual void OnCreateWindow() override;
    virtual void OnDestroy() override;
    virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
    virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) override { 
        hwndCommand = GetParent(hwnd);
        return hMenuCtx;
    }

    // Copy the pinout diagram to the clipboard
    void CopyPinoutDiagram();

	// Paint off-screen
    virtual void PaintOffScreen(HDC hdc) override;

    // Paint the pinout diagram into the DC.  xOffset is the location
    // of the left edge of the Pico image.  Pinout labels are drawn to
    // the left of this (and to the right of the right side of the
    // image), so the caller needs to figure the desired margins when
    // selecting the x offset.
    void PaintPinoutDiagram(HDCHelper &hdc, int xOffset, int yOffset,
        bool hot, bool forPrinter, bool monochrome);
    
    // figure the required horizontal margins for the pinout diagram
    int CalcPinoutXMargin(HDCHelper &hdc);

    // menu/accelerator
    HMENU hMenuBar = NULL;
    HMENU hMenuCtx = NULL;
    HACCEL hAccel = NULL;

    // bitmaps
    HBITMAP bmpBootselButton = NULL;
    SIZE szBootselButton{ 0, 0 };

    HBITMAP bmpPicoDiagram = NULL;
    HBITMAP bmpPicoDiagramHot = NULL;
    SIZE szPicoDiagram{ 0, 0 };
    RECT rcPicoDiagram{ 0 };

    // device query information
    bool online = true;
    bool hasConfig = false;
    PinscapePico::DeviceID devID;
    VendorInterface::Version devVersion{ 0 };
    PinscapePico::Statistics devStats{ 0 };
    PinscapePico::USBInterfaces usbIfcs{ 0 };
    TSTRING cdcPortName;
    std::vector<BYTE> gpioStates;

    // new-device-help button
    RECT rcHelpNew{ 0 };

    // query the GPIO configuration
    void QueryGPIOConfig();

    // GPIO port configuration
    VendorInterface::GPIOConfig gpio[30];

    // query the output port configuration
    void QueryOutputPortConfig();

    // output port configuration
    std::vector<PinscapePico::OutputPortDesc> outputPortConfig;

    // time for next device status update and GPIO read time
    UINT64 nextUpdateTime = 0;
    UINT64 nextGpioTime = 0;

    // firmware install button
    RECT rcFirmwareInstallButton{ 0, 0, 0, 0 };

    // drag/drop helper
    FirmwareDropTarget *dropTarget{ new FirmwareDropTarget(this) };

    // pending firmware installation file, for MSG_INSTALL_FIRMWARE
    std::basic_string<TCHAR> pendingFirmwareInstall;

    // Printing
    virtual bool IsPrintingEnabled() const override { return true; }
    virtual std::basic_string<TCHAR> GetPrintDocName() const override { return _T("Pinscape Pico Pinout"); }
    virtual PageSetupOptions *CreatePageSetupOptions() const { return new PageSetupOptions("pinout.pageSetup"); }
    virtual bool ExpandHeaderFooterVar(const std::string &varName, std::string &expansion) override;
    virtual bool PrintPageContents(HDCHelper &hdc, int pageNum, bool skip) override;
};
