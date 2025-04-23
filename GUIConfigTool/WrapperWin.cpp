// Pinscape Pico - Config Tool Wrapper Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <list>
#include <string>
#include <iterator>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "PinscapePicoAPI.h"
#include "ButtonTesterWin.h"
#include "OutputTesterWin.h"
#include "PlungerCalWin.h"
#include "ConfigEditorWin.h"
#include "LogViewerWin.h"
#include "NoDeviceWin.h"
#include "OfflineDeviceWin.h"
#include "DeviceOverviewWin.h"
#include "SetUpNewDeviceWin.h"
#include "NudgeDeviceWin.h"
#include "IRTesterWin.h"
#include "BootLoaderWin.h"
#include "AboutBox.h"
#include "Application.h"
#include "Dialog.h"
#include "FlashProgressDialog.h"
#include "JSONExt.h"
#include "WinUtil.h"
#include "WrapperWin.h"
#include "HelpWin.h"
#include "Utilities.h"
#include "resource.h"

using namespace PinscapePico;

// statics
uint64_t WrapperWin::LeftPanelButton::nextUniqueID = 1;

// JSON key for our saved window placement
static const char *windowPlacementJsonKey = "window.placement";

WrapperWin::WrapperWin(HINSTANCE hInstance, const WCHAR *settingsFile,
    bool useDefaultSettings, int initialUnitNum) :
    BaseWindow(hInstance)
{
    // load the applications setting file
    gApp.settingsFile = settingsFile;
    if (!useDefaultSettings)
        gApp.LoadSettings();

    // load button context menus
    deviceButtonContextMenu = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_DEVICEBUTTON));
    bootLoaderButtonContextMenu = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_BOOTLOADERBUTTON));

    // Initialize our device list.  Use the feedback controller HID
    // interface to do the enumeration, since that allows concurrent
    // access from any number of clients.
    std::list<DeviceDesc::ID> descs;
    HRESULT hr = EnumerateDevices(descs);
    if (!SUCCEEDED(hr))
        MessageBoxFmt(NULL, "An error occurred searching for Pinscape Pico devices (error code %08lx)", static_cast<UINT>(hr));

    // add each device to the known device table
    for (auto &desc : descs)
    {
        // add the device
        AddDevice(desc);

        // if this device matches the initial unit number selection,
        // mark it as the initial selection
        if (desc.unitNum == initialUnitNum)
            initialButtonSelection = leftPanelButtons.back().get();
    }

    // enumerate RP2 Boot Loader drives, and add them to the button list
    RP2BootDevice::RP2BootDeviceList drives;
    drives = RP2BootDevice::EnumerateNewRP2BootDrives(drives);
    for (auto &drive : drives)
        AddBootLoaderButton(drive);

    // add the New Device button
    auto &setupBtn = leftPanelButtons.emplace_back(new SetUpNewDeviceButton());
    setupBtn->tabs.emplace_back("Set Up New Device", &WrapperWin::CreateSetUpNewDeviceWin);

    // load bitmaps
    bmpPinscapeDevice = LoadPNG(hInstance, IDB_PINSCAPEDEVICON);
    bmpPinscapeOffline = LoadPNG(hInstance, IDB_PINSCAPEOFFLINEICON);
    bmpBootDrive = LoadPNG(hInstance, IDB_BOOTDRIVEICON);
    bmpNewDevice = LoadPNG(hInstance, IDB_NEWDEVICON);

    // get a representative icon size
    BITMAP bmp{ 0 };
    GetObject(bmpPinscapeDevice, sizeof(bmp), &bmp);
    szBmpIcons ={ bmp.bmWidth, bmp.bmHeight };

    // sort the button list
    SortButtonList();

    // If we didn't make an initial button selection based on a desired
    // initial unit number selection, choose the top button in the list.
    if (initialButtonSelection == nullptr)
        initialButtonSelection = leftPanelButtons.front().get();
}

WrapperWin::~WrapperWin()
{
    // save the settings file
    gApp.SaveSettings();

    // release the drop target helper
    dropTarget->Detach();
    dropTarget->Release();
}

WINDOWPLACEMENT WrapperWin::GetWindowPlacementFromSettings()
{
    return gApp.RestoreWindowPlacement(windowPlacementJsonKey, { 50, 50, 1500, 960 });
}

void WrapperWin::SortButtonList()
{
    using Ele = std::unique_ptr<LeftPanelButton>;
    leftPanelButtons.sort([](const Ele &a, const Ele &b) { return a->SortCompare(b.get()); });
}

HRESULT WrapperWin::EnumerateDevices(std::list<DeviceDesc::ID> &newDeviceList)
{
    // enumerate vendor interfaces
    std::list<VendorInterfaceDesc> vendorIfcDescs;
    HRESULT hr = VendorInterface::EnumerateDevices(vendorIfcDescs);
    if (!SUCCEEDED(hr))
        return hr;

    // The vendor interface list only gives us the USB paths to the devices,
    // because we can't get the Pinscape identifiers (unit number, name, Pico
    // hardware ID) without actually connecting to the device.  WinUsb requires
    // exclusive access (no shared access across processes), so the enumerator
    // explicitly does not open the device, so that the mere act of enumeration
    // doesn't have side effects visible in other processes (specifically,
    // locking out their access to the WinUsb devices) AND so that it succeeds
    // even if some of the devices are currently open in other processes.
    // 
    // But this means that the enumeration doesn't give us all of the
    // information we need.  We need each device's Pinscape unit ID (number
    // and name) to display in the list of available devices in the UI, and we
    // also need the Pico hardware ID so that we can positively identify each
    // physical device, even if its configuration changes while we're running.
    // A change to the Pinscape configuration can change all of the Windows
    // identifiers for the device (Windows device path and device instance
    // ID), because these things are based on the USB configuration that the
    // device presents to the host, which Pinscape allows the user to change
    // on the fly.
    //
    // To get the Pinscape IDs, we have several options:
    //
    // - First, to avoid the exclusive access problem, look for a HID that
    //   we can query for the ID information.  The Feedback Controller
    //   interface, if enabled, provides such a query.  HID is the most
    //   cooperative way to get the information because HIDs allow shared
    //   read/write access.
    // 
    // - Failing that, open the Vendor Interface and query the ID that way.
    //   This is less cooperative because of the exclusive access constraint
    //   for WinUsb, but the Config Tool has to access the vendor ifc anyway
    //   if the user actually wants to do anything with the device in this
    //   session, so they really shouldn't be running any other software
    //   that's tying up the vendor interfaces in the first place.
    // 
    // - If all else fails, we can look to see if we've looked up the IDs
    //   for the same device path previously, and if so, assume they're
    //   the same now.  This could give us faulty information, but it
    //   should be corrected the next time we can actually connect.
    //
    for (const auto &vid : vendorIfcDescs)
    {
        // Try getting associated HIDs
        std::list<VendorInterfaceDesc::AssociatedHID> hids;
        if (SUCCEEDED(vid.GetAssociatedHIDs(hids)))
        {
            // scan for a HID that we can query for device identifiers
            bool foundIDs = false;
            for (auto &hid : hids)
            {
                // look for our Feedback Controller interface
                static const std::wregex fcPat(L"PinscapeFeedbackController/(\\d+)");
                if (std::regex_match(hid.inputReportStringUsage, fcPat))
                {
                    // connect to the Feedback Controller and query IDs
                    std::unique_ptr<FeedbackControllerInterface> fc(FeedbackControllerInterface::Open(hid.path.c_str()));
                    FeedbackControllerInterface::IDReport id;
                    if (fc != nullptr && fc->QueryID(id, 200))
                    {
                        // got it - add the device
                        newDeviceList.emplace_back(vid.Path(), id.unitNum, id.unitName, id.hwid);

                        // mission accomplished
                        foundIDs = true;
                        break;
                    }
                }
            }

            // stop here if we got obtained the identifiers
            if (foundIDs)
                continue;
        }

        // try opening the vendor interface
        std::unique_ptr<VendorInterface> vi;
        DeviceID id;
        if (SUCCEEDED(vid.Open(vi)) && vi->QueryID(id) == PinscapeResponse::OK)
        {
            // success - populate a device list entry
            newDeviceList.emplace_back(vid.Path(), id.unitNum, id.unitName.c_str(), id.hwid);
            continue;
        }

        // We can't open the vendor interface, which could just be
        // because it's already open in the UI (WinUsb only allows
        // exclusive access).  Scan our active device list to see
        // if a device with the same path is already there and is
        // open - if so, we can get the ID from there.
        auto it = std::find_if(devices.begin(), devices.end(),
            [&vid](const std::pair<std::string, DeviceDesc> &d) { return d.second.id.path == vid.Path() && d.second.device != nullptr; });
        if (it != devices.end())
        {
            // got it - add the list entry
            const auto &id = it->second.id;
            newDeviceList.emplace_back(vid.Path(), id.unitNum, id.unitName.c_str(), id.hwId);
            continue;
        }

        // No luck.  Re-enumerate devices to see if the device is
        // still present, in case it was connected briefly and then
        // disconnected again before we could query its identifiers.
        // This can happen if the device crashes to Safe Mode shortly
        // after a reboot, or if the USB connection bounces while
        // the user is plugging in the device.
        std::list<VendorInterfaceDesc> vendorIfcDescs2;
        if (SUCCEEDED(VendorInterface::EnumerateDevices(vendorIfcDescs2)))
        {
            // got a new list - if the current device isn't in the new list, skip it
            if (std::find_if(vendorIfcDescs2.begin(), vendorIfcDescs2.end(),
                [&vid](const VendorInterfaceDesc &vid2) { return wcscmp(vid.Path(), vid2.Path()) == 0; }) == vendorIfcDescs2.end())
                continue;
        }

        // Add it to the list with unknown IDs
        newDeviceList.emplace_back(vid.Path(), 0, StrPrintf("Unknown(%ws)", vid.DeviceInstanceId()).c_str(), PicoHardwareId());
    }

    // success
    return S_OK;
}

WrapperWin::DeviceButton *WrapperWin::AddDevice(const DeviceDesc::ID &desc)
{
    // add the device table entry
    auto &devTabEntry = devices.emplace(desc.hwId.ToString(), desc).first->second;

    // add a left-panel button representing the device
    auto &btn = leftPanelButtons.emplace_back(devTabEntry.button = new DeviceButton(&devTabEntry));
    btn->hContextMenu = deviceButtonContextMenu;

    // initialize the button's associated tab list
    btn->tabs.emplace_back("Overview", &WrapperWin::CreateOverviewWin);
    btn->tabs.emplace_back("Log", &WrapperWin::CreateLogWin);
    btn->tabs.emplace_back("Configuration", &WrapperWin::CreateMainConfigWin);
    btn->tabs.emplace_back("Safe-mode Config", &WrapperWin::CreateSafeModeConfigWin);
    btn->tabs.emplace_back("Buttons", &WrapperWin::CreateButtonTesterWin);
    btn->tabs.emplace_back("Nudge", &WrapperWin::CreateNudgeWin);
    btn->tabs.emplace_back("Output Ports", &WrapperWin::CreateOutputPortTesterWin);
    btn->tabs.emplace_back("Output Hardware", &WrapperWin::CreateOutputDevTesterWin);
    btn->tabs.emplace_back("Plunger", &WrapperWin::CreatePlungerWin);
    btn->tabs.emplace_back("IR && TV ON", &WrapperWin::CreateIRWin);

    // return the button pointer
    return devTabEntry.button;
}

WrapperWin::BootLoaderDriveButton *WrapperWin::AddBootLoaderButton(const RP2BootDevice &dev)
{
    // add the left-panel button
    auto *btn = new BootLoaderDriveButton(dev);
    leftPanelButtons.emplace_back(btn);

    // add it to our internal drive list
    bootLoaderDrives.emplace_back(dev, btn);
    btn->hContextMenu = bootLoaderButtonContextMenu;

    // initialize its tab list
    btn->tabs.emplace_back("Install Pinscape", &WrapperWin::CreateBootLoaderDriveWin);

    // return the button pointer
    return btn;
}

// Paint off-screen
void WrapperWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

    // fill the background
    HDCHelper hdc(hdc0);
    HBrush bkgBrush(HRGB(0x5c6d99));
    FillRect(hdc, &crc, bkgBrush);

    // set up a DC for copying bitmaps
    CompatibleDC bdc(hdc);

    // current mouse position, for hot item detection
    POINT ptMouse;
    GetCursorPos(&ptMouse);
    ScreenToClient(hwnd, &ptMouse);

    // figure the left panel button height
    cyDeviceButton = 24 + mainFontMetrics.tmHeight*4 + szBmpIcons.cy;

    // draw the left panel button list
    RECT devrc{ crc.left + 4, crc.top + 4, crc.left + cxLeftPanel - cxScrollbar, crc.bottom - cyStatusline - 4 };
    RECT btnrc{ devrc.left, devrc.top, devrc.right, devrc.top + cyDeviceButton };
    OffsetRect(&btnrc, 0, -yScrollLeftPanel);
    FillRect(hdc, &devrc, HBrush(HRGB(0xf8f8f8)));
    IntersectClipRect(hdc, devrc.left, devrc.top, devrc.right, devrc.bottom);
    for (auto &btn : leftPanelButtons)
    {
        // set its drawing rectangle
        btn->rc = btnrc;

        // fill the background
        bool hot = IsClientRectHot(btnrc) && (!dropTarget->IsDragActive() || dropTarget->IsTargetHot());
        bool selected = (btn.get() == curButton);
        FillRect(hdc, &btnrc, HBrush(selected && hot ? HRGB(0xd0e0ff) : hot ? HRGB(0xcbd9f7) : selected ? HRGB(0xc4d5ff) : HRGB(0xffffff)));

        RECT seprc{ btnrc.left, btnrc.bottom - 1, btnrc.right, btnrc.bottom };
        FillRect(hdc, &seprc, HBrush(HRGB(0x808080)));

        // set up the text drawing position
        int x = btnrc.left + 8;
        int y = btnrc.top + 8;

        // clip to the button interior
        int dcSave = SaveDC(hdc);
        IntersectClipRect(hdc, btnrc.left + 1, btnrc.top + 1, btnrc.right - 1, btnrc.bottom - 1);
            
        // Draw the type-specific button labeling, insetting the drawing area for padding
        RECT innerrc = btnrc;
        InflateRect(&innerrc, -8, -8);
        HBITMAP icon = btn->Draw(this, hdc, innerrc);

        // draw the icon
        if (icon != NULL)
        {
            bdc.Select(icon);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(hdc, (btnrc.left + btnrc.right - szBmpIcons.cx)/2, btnrc.bottom - szBmpIcons.cy - 10,
                szBmpIcons.cx, szBmpIcons.cy, bdc, 0, 0, szBmpIcons.cx, szBmpIcons.cy, bf);
        }

        // remove clipping
        RestoreDC(hdc, dcSave);

        // advance the button rect down one slot vertically
        OffsetRect(&btnrc, 0, cyDeviceButton);
    }
    SelectClipRgn(hdc, NULL);

    // draw the tab control
    static const COLORREF clrTabSel = HRGB(0xf5cc84);
    static const COLORREF clrTabHot = HRGB(0xbbccf1);
    static const COLORREF clrTabNorm = HRGB(0x3b4f81);
    static const COLORREF clrTabTxtSel = HRGB(0x01247a);
    static const COLORREF clrTabTxtNorm = HRGB(0xffffff);
    RECT rctc{ crc.left + cxLeftPanel + cxPanelMargin, crc.top, crc.right - cxPanelMargin, crc.top + cyTabCtl };
    FillRect(hdc, &rctc, bkgBrush);
    RECT rctcGutter{ rctc.left, rctc.bottom - cyTabBottomMargin, rctc.right, rctc.bottom };
    FillRect(hdc, &rctcGutter, HBrush(clrTabSel));
    int xTab = rctc.left + cxTabLeftMargin;
    auto DrawTab = [this, &hdc, &rctc, &xTab, &ptMouse](Tab &tab)
    {
        // get the label text
        const char *label = tab.label;

        // If it's a TabEmbeddableWindow, check the document modified status, and add
        // a "*" to the tab label if it's been modified (this is a common Windows
        // convention for marking a modified document window in the UI).
        std::string labelBuf;
        if (auto *tew = dynamic_cast<TabEmbeddableWindow*>(tab.win.get()); tew != nullptr && tew->IsDocumentModified())
        {
            // build the nominal label plus a "*" marker flag
            labelBuf = label;
            labelBuf.append("*");

            // use this instead of the original label
            label = labelBuf.c_str();
        }

        // figure the tab size
        int padLeft = 8;
        int padRight = 12;
        SIZE sz = hdc.MeasureText(boldFont, label);
        tab.rc ={ xTab, rctc.top + cyTabTopMargin, xTab + padLeft + padRight + sz.cx, rctc.bottom - cyTabBottomMargin };

        // draw slightly shorter for non-selected tabs
        bool selected = (&tab == GetCurTab());
        if (!selected)
            tab.rc.top += 1;

        // figure if the mouse is over the tab
        bool hot = IsClientRectHot(tab.rc) && !dropTarget->IsDragActive();

        // draw it
        FillRect(hdc, &tab.rc, HBrush(selected ? clrTabSel : hot ? clrTabHot : clrTabNorm));
        hdc.DrawText(tab.rc.left + padLeft, tab.rc.bottom - cyTabBottomPadding - sz.cy, 1,
            selected ? boldFont : mainFont, selected || hot ? clrTabTxtSel : clrTabTxtNorm, label);

        // advance the x position past the tab, to set up for the next tab
        xTab = tab.rc.right + 2;
    };
    
    // draw the currently selected button's tabs
    if (curButton != nullptr)
    {
        for (auto &tab : curButton->tabs)
            DrawTab(tab);
    }

    // up up to draw the status line
    RECT rcsl{ crc.left, crc.bottom - cyStatusline, crc.right, crc.bottom };
    int x = crc.left + 8;
    int y = (rcsl.top + rcsl.bottom - szBoldFont.cy)/2;
    COLORREF sfg = HRGB(0xFFFFFF);
    COLORREF sbg = HRGB(0x000080);
    char statusMsg[256] = "Ready";
    const char *statusMsgPtr = statusMsg;
    const char *keyBindingPrefix = nullptr;

    // check for a timed message - these override current status messages
    if (auto *timedMsg = gApp.GetCurrentTimedStatusMessage() ; timedMsg != nullptr)
    {
        // use the timed message
        statusMsgPtr = timedMsg->message.c_str();
        sbg = timedMsg->bg;
        sfg = timedMsg->fg;
    }
    else if (curButton != nullptr)
    {
        // use the message for the currently selected window

        // get the color and main text
        sbg = curButton->GetStatuslineColor();
        curButton->GetStatuslineText(statusMsg, _countof(statusMsg));

        // check for a key-binding prefix to display
        if (curButton->curTab != nullptr)
        {
            if (auto *edWin = dynamic_cast<ConfigEditorWin*>(curButton->curTab->win.get()); edWin != nullptr)
                keyBindingPrefix = edWin->GetKeyBindingPrefix();
        }
    }

    // fill the background and draw the main message text
    FillRect(hdc, &rcsl, HBrush(sbg));
    x += hdc.DrawText(x, y, 1, mainFont, sfg, statusMsgPtr).cx + 32;

    // draw the keyboard state on the right - binding prefix, shift-lock keys
    bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
    bool numLock = (GetKeyState(VK_NUMLOCK) & 1) != 0;
    bool scrlLock = (GetKeyState(VK_SCROLL) & 1) != 0;
    int cx = hdc.MeasureText(mainFont, "CapsLock  NumLock  ScrlLock").cx;
    int x2 = x = crc.right - cx - 16;
    x += hdc.DrawText(x, y, 1, mainFont, HRGB(capsLock ? 0xffffff : 0x808080), "CapsLock  ").cx;
    x += hdc.DrawText(x, y, 1, mainFont, HRGB(numLock ? 0xffffff : 0x808080), "NumLock  ").cx;
    x += hdc.DrawText(x, y, 1, mainFont, HRGB(scrlLock ? 0xffffff : 0x808080), "ScrlLock").cx;
    if (keyBindingPrefix != nullptr)
        hdc.DrawTextF(x2, y, -1, boldFont, HRGB(0xffffff), "%s ...   ", keyBindingPrefix);
}

// draw a device button
HBITMAP WrapperWin::DeviceButton::Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc)
{
    int x = rc.left, y = rc.top;
    y += hdc.DrawText(x, y, 1, win->boldFont, HRGB(0x800080), "Pinscape Pico").cy;
    SIZE sz = hdc.DrawTextF(x, y, 1, win->mainFont, HRGB(0x000000), "Unit #%d", dev->id.unitNum);
    if (!dev->online)
        hdc.DrawText(x + sz.cx + 16, y, 1, win->boldFont, HRGB(0xFF0000), "OFFLINE");

    y += sz.cy;
    y += hdc.DrawTextF(x, y, 1, win->mainFont, HRGB(0x000000), "(%s)", dev->id.unitName.c_str()).cy;
    y += hdc.DrawTextF(x, y, 1, win->mainFont, HRGB(0x000000), "HW ID %s", dev->id.hwId.ToString().c_str()).cy;

    // return the Pinscape Pico Device icon, in its online or offline appearance
    return dev->online ? win->bmpPinscapeDevice : win->bmpPinscapeOffline;
}

// draw a boot loader button
HBITMAP WrapperWin::BootLoaderDriveButton::Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc)
{
    int x = rc.left, y = rc.top;
    y += hdc.DrawText(x, y, 1, win->boldFont, HRGB(0x800080), "Pico Boot Loader").cy;
    y += hdc.DrawTextF(x, y, 1, win->mainFont, HRGB(0x000000), "Drive: %" _TSFMT, dev.path.c_str()).cy;
    y += hdc.DrawTextF(x, y, 1, win->mainFont, HRGB(0x808080), "Pi Pico %s", dev.boardVersion.c_str()).cy;
    y += hdc.DrawTextF(x, y, 1, win->mainFont, HRGB(0x808080), "UF2 Boot Loader v%s", dev.bootloaderVersion.c_str()).cy;

    // return the boot loader drive icon
    return win->bmpBootDrive;
}

HBITMAP WrapperWin::SetUpNewDeviceButton::Draw(WrapperWin *win, HDCHelper &hdc, const RECT &rc)
{
    int x = rc.left, y = rc.top;
    y += hdc.DrawText(x, y, 1, win->boldFont, HRGB(0x800080), "New Device Setup").cy;
    y += hdc.DrawText(x, y, 1, win->mainFont, HRGB(0x000000), "Click to set up a new Pico").cy;
    return win->bmpNewDevice;
}


void WrapperWin::SelectButton(LeftPanelButton *button)
{
    if (button != curButton)
    {
        // deactivate the old window
        DeactivateTab();

        // make the button current
        curButton = button;

        // activate it
        button->Activate(this);

        // activate its current tab, or its first tab if there's no current tab
        SelectTab(button->curTab != nullptr ? *button->curTab : curButton->tabs.front());
    }
}

// activate a device button
void WrapperWin::DeviceButton::Activate(WrapperWin *win)
{
    // Create the shared device object if this is the first time we've
    // activated this device.  Skip this when the device is offline.
    if (dev->device == nullptr)
        dev->device = std::make_shared<VendorInterface::Shared>();

    // if we're not connected, connect
    if (HRESULT hr = dev->Connect(win, false); hr != S_OK && dev->online)
        MessageBoxFmt(win->GetHWND(), "Unable to connect to device (error code %08lx)", static_cast<UINT>(hr));
}

HRESULT WrapperWin::DeviceDesc::Connect(WrapperWin *win, bool reconnect)
{
    // lock the device
    HRESULT hr = S_OK;
    if (VendorInterface::Shared::Locker l(device); l.locked)
    {
        // connect if there's no device or if we're explicitly reconnecting
        if (device->device == nullptr || !device->device->IsDeviceHandleValid() || reconnect)
        {
            // connect
            VendorInterface *newDevice = nullptr;
            hr = VendorInterface::Open(newDevice, id.hwId);

            // if successful, stash the new device pointer
            if (hr == S_OK && newDevice != nullptr)
            {
                // save the new device handle
                device->Set(newDevice, win->hwnd);

                // Update the device with the time of day.  This is something
                // that most applications should do as a matter of routine when
                // connecting, to refresh the device with the wall clock time
                // in case the Pico has been reset recently.  The Pico doesn't
                // have a way to remember the time across resets, and USB
                // doesn't provide a way for the device to ask, so the Pico
                // depends upon the host sending updates.  We can satisfy this
                // need simply by sending the time incidentally whenever we
                // connect.  If the Pico already has the current time, this
                // won't do any harm, and it'll even do a little good in that
                // it'll sync up the Pico to correct any clock drift since the
                // last update.
                device->device->PutWallClockTime();

                // get the uptime, so that we can detect future resets
                PinscapePico::Statistics stats;
                if (device->device->QueryStats(&stats, sizeof(stats), false) == PinscapeResponse::OK)
                {
                    upTime = stats.upTime;
                    upTimeTimestamp = GetTickCount64() * 1000;
                }
            }
        }
    }
    
    // return the result
    return hr;
}

void WrapperWin::DeactivateTab()
{
    auto *curTab = GetCurTab();
    if (curTab != nullptr && curTab->win != nullptr)
    {
        // explicitly suspend updates if it's a device window
        if (auto *devWin = dynamic_cast<DeviceThreadWindow*>(curTab->win.get()); devWin != nullptr)
            devWin->SuspendUpdaterThread();

        // hide the window
        ShowWindow(curTab->win->GetHWND(), SW_HIDE);
    }
}

void WrapperWin::SelectTab(Tab &tab)
{
    // deactivate the old tab
    DeactivateTab();

    // select the new tab
    curButton->curTab = &tab;

    // make sure the offline window is hidden, unless we explicitly show it
    ShowWindow(offlineDeviceWin->GetHWND(), SW_HIDE);

    // check for a pending UI refresh
    if (tab.uiRefreshPending)
    {
        // clear the flag - it only needs to be processed once per reconnect
        tab.uiRefreshPending = false;

        // if there's a window, refresh it
        if (tab.win != nullptr)
        {
            // If the window exposes the TabEmbeddableWindow interface, notify it.
            // If it doesn't expose the interface, brute-force a refresh by
            // destroying the window, which will trigger creation of a new one.
            if (auto tewin = dynamic_cast<TabEmbeddableWindow*>(tab.win.get()) ;
                tewin == nullptr || !tewin->OnDeviceReconnect())
            {
                // the window didn't update, so close it, which will
                // trigger creation of a fresh window the next time
                // it's displayed
                DestroyWindow(tab.win->GetHWND());
                tab.win.reset();
            }
        }
    }

    // if it doesn't have a window yet, create one
    bool showTabWindow = false;
    if (tab.win == nullptr)
    {
        // the tab doesn't have a window yet - create one
        (this->*tab.createWindow)();
    }
    else if (auto *devBtn = dynamic_cast<DeviceButton*>(curButton); devBtn != nullptr && !devBtn->dev->online)
    {
        // This is a device tab, and the device is offline.  Ask the window
        // for advice if it exposes TabEmbeddableWindow.
        if (auto *tewin = dynamic_cast<TabEmbeddableWindow*>(tab.win.get()); tewin != nullptr && tewin->IsVisibleOffline())
        {
            // this window remains visible when the device is offline
            showTabWindow = true;
        }
        else
        {
            // this window does not stay visible offline - show the Offline Device window in its place
            ShowWindow(tab.win->GetHWND(), SW_HIDE);
            ShowWindow(offlineDeviceWin->GetHWND(), SW_SHOW);

            // suspend device polling while the window is hidden
            if (auto *devwin = dynamic_cast<DeviceThreadWindow*>(tab.win.get()); devwin != nullptr)
                devwin->SuspendUpdaterThread();
        }
    }
    else
    {
        // there's an existing window, and the device is online - show the tab window
        showTabWindow = true;
    }

    if (showTabWindow)
    {
        // the tab already has a window - show it
        ShowWindow(tab.win->GetHWND(), SW_SHOW);

        // explicitly resume updates if it's a device window
        if (auto *devWin = dynamic_cast<DeviceThreadWindow*>(tab.win.get()); devWin != nullptr)
            devWin->ResumeUpdaterThread();
    }

    // activate the tab's UI
    if (tab.win != nullptr)
    {
        // activate the tab window UI
        tab.win->OnActivateUI(false);

        // select its menu
        if (!tab.win->InstallParentMenuBar(hwnd))
            SetMenu(hwnd, defaultMenuBar);
    }

    // adjust layout
    AdjustLayout();
}

bool WrapperWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    // if a tab is active, and it has a window, use its accelerators
    auto *curTab = GetCurTab();
    if (curTab != nullptr && curTab->win != nullptr && curTab->win->TranslateAccelerators(hwndMenu, msg))
        return true;

    // translate through our own accelerators instead
    return ::TranslateAccelerator(hwnd, defaultAccel, msg);
}

void WrapperWin::AdjustLayout()
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

    // position the left panel scrollbar
    SetWindowPos(sbLeftPanel, NULL, 
        cxLeftPanel - cxScrollbar, crc.top + 4, 
        cxScrollbar, crc.bottom - crc.top - cyStatusline - 8, SWP_NOZORDER);

    auto SetTabWindowPos = [&crc, this](HWND hwnd)
    {
        SetWindowPos(hwnd, HWND_TOP,
            crc.left + cxLeftPanel + cxPanelMargin, crc.top + cyTabCtl,
            crc.right - crc.left - cxLeftPanel - cxPanelMargin * 2,
            crc.bottom - crc.top - cyTabCtl - cyStatusline - 4,
            SWP_NOACTIVATE);
    };

    // position the active child window
    if (auto *curTab = GetCurTab(); curTab != nullptr && curTab->win != nullptr)
        SetTabWindowPos(curTab->win->GetHWND());

    // position the offline placeholder window
    if (offlineDeviceWin != nullptr)
        SetTabWindowPos(offlineDeviceWin->GetHWND());
}

void WrapperWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

    // load my menu bar and accelerator
    defaultMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_WRAPPERWIN_MAIN));
    defaultAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_WRAPPERWIN_MAIN));
    SetMenu(hwnd, defaultMenuBar);

    // set up for layout computation - get the client area size and window DC
    RECT crc;
    GetClientRect(hwnd, &crc);
    HDC dc = GetWindowDC(hwnd);

    // get the main and bold font sizes
    HFONT oldFont = SelectFont(dc, mainFont);
    GetTextExtentPoint32A(dc, "M", 1, &szMainFont);
    SelectFont(dc, boldFont);
    GetTextExtentPoint32A(dc, "M", 1, &szBoldFont);

    // figure the top tab control metrics
    cyTabCtl = szBoldFont.cy + cyTabTopMargin + cyTabBottomMargin + cyTabTopPadding + cyTabBottomPadding + 1;
    cyStatusline = min(szBoldFont.cy + 16, 24);

    // Create the placeholder window to display in lieu of an actual tool
    // window when the current tab's device is offline.  Create the window
    // initially hidden; we'll show it if and when we need it.
    offlineDeviceWin.reset(new OfflineDeviceWin(hInstance));
    offlineDeviceWin->CreateSysWindow(offlineDeviceWin, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0, hwnd, _T("Device Offline"),
            cxLeftPanel + cxPanelMargin, cyTabCtl, crc.right - crc.left, crc.bottom - crc.top - cyTabCtl - cyStatusline, SW_HIDE);

	// done with the window DC - clean it up and release it
	SelectFont(dc, oldFont);
	ReleaseDC(hwnd, dc);

    // create the left-panel scrollbar
    sbLeftPanel = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
        0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_LEFTPANEL), hInstance, 0);

    // range calculation for the scrollbar
    auto GetRangeLeftPanel = [this](SCROLLINFO &si)
    {
        // figure the client area height
        RECT crc;
        GetClientRect(hwnd, &crc);
        int winHt = crc.bottom - crc.top - cyStatusline - 8;

        // figure the document height - number of devices x device button height
        int docHt = cyDeviceButton * static_cast<int>(leftPanelButtons.size());

        // set the range
        si.nMin = 0;
        si.nMax = max(docHt - cyLeftPanelScrollLine, 0);
        si.nPage = max(winHt - cyLeftPanelScrollLine, cyLeftPanelScrollLine);
    };

    // get the output scroll region
    auto GetScrollRectLeftPanel = [this](RECT *rc)
    {
        rc->right = rc->left + cxLeftPanel;
    };

    // change the scroll position for the output scroll panel
    auto SetScrollPosLeftPanel = [this](int newPos, int deltaPos) { yScrollLeftPanel = newPos; };

    // set up the scrollbar object
    scrollbars.emplace_back(sbLeftPanel, SB_CTL, cyLeftPanelScrollLine, true, true, 
        GetRangeLeftPanel, GetScrollRectLeftPanel, SetScrollPosLeftPanel);

    // activate the initial left panel button
    SelectButton(initialButtonSelection);

    // adjust the layout
    AdjustLayout();

    // register as a drop target
    RegisterDragDrop(hwnd, dropTarget);
}

void WrapperWin::OnDestroy()
{
    // unregister our drop target
    RevokeDragDrop(hwnd);

    // do the base work
    __super::OnDestroy();
}

void WrapperWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the layout
	AdjustLayout();

    // do the base class work
    __super::OnSizeWindow(type, width, height);
}

void WrapperWin::OnActivate(WPARAM code, HWND other)
{
    // on activation, give focus to the current tab window
    if (code == WA_ACTIVE || code == WA_CLICKACTIVE)
    {
        if (auto *curTab = GetCurTab(); curTab != nullptr && curTab->win != nullptr)
            curTab->win->OnActivateUI(true);
    }
    else if (code == WA_INACTIVE)
    {
        if (auto *curTab = GetCurTab(); curTab != nullptr && curTab->win != nullptr)
            curTab->win->OnDeactivateUI();
    }
}

bool WrapperWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (notifyCode)
    {
    case 0:
    case 1:
        // Menu command, accelerator, or button click.  First, check for
        // commands that the main window always handles, overriding any
        // handler in the tab children.  (The tab children might have
        // their own handlers for these that are intended for when they're
        // running in standalone mode as their own top-level windows.
        // Those are superseded when in a container app like this one.)
        switch (ctlCmdId)
        {
        case ID_HELP_ABOUT:
            // show the About box as a modal dialog
            {
                // get the main window screen location
                RECT wrc;
                GetWindowRect(hwnd, &wrc);

                // show the about box window centered over the main window
                int cx = 600, cy = 500;
                std::shared_ptr<BaseWindow> aboutBox(new AboutBox(hInstance));
                aboutBox->CreateSysWindow(aboutBox, 
                    WS_OVERLAPPED | WS_SYSMENU | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 
                    0, hwnd, _T("About the Pinscape Pico Config Tool"),
                    (wrc.left + wrc.right - cx)/2, (wrc.top + wrc.bottom - cy)/2, cx, cy,
                    SW_SHOW);

                // disable the main window to make the About box modal
                EnableWindow(hwnd, FALSE);
                aboutBox->CallOnClose([hwnd = hwnd]() { EnableWindow(hwnd, TRUE); });
            }
            return true;

        case ID_FILE_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return true;
        }

        // if there's an active tab window, forward the message to its handler
        if (auto *curTab = GetCurTab(); curTab != nullptr && curTab->win != nullptr
            && curTab->win->OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult))
            return true;

        // Now check for commands that we handle only if the tab child 
        // window doesn't handle it first.  These provide defaults for
        // commands that some child windows might handle, but which they
        // aren't required to handle.
        switch (ctlCmdId)
        {
        case ID_HELP_HELP:
            // Show the generic program-wide help.  Note that the child
            // windows might provide their own context-specific help, which
            // is why we handle this in the default handler section.
            ShowHelpFile("ConfigTool.htm");
            return true;

        case ID_DEVICE_REBOOT:
            Reboot();
            return true;

        case ID_DEVICE_SAFEMODE:
            RebootSafeMode();
            return true;

        case ID_DEVICE_ENTERFACTORYMODE:
            RebootFactoryMode();
            return true;

        case ID_DEVICE_BOOTLOADERMODE:
            BootLoaderMode();
            return true;

        case ID_DEVICE_INSTALLFIRMWARE:
        case ID_BACKUP_RESTOREEXPORTEDIMAGE:
            InstallFirmware(ctlCmdId);
            return true;

        case ID_BACKUP_EXPORTDATAAREAIMAGE:
        case ID_BACKUP_EXPORTFULLFLASHIMAGE:
            ExportFlash(ctlCmdId);
            return true;

        case ID_DEVICE_CLEARCONFIGURATION:
            ClearConfig();
            return true;

        case ID_DEVICE_FULLFACTORYRESET:
            FactoryReset();
            return true;

        case ID_DEVICE_REPAIRJOYCPL:
            RepairJoyCpl();
            return true;

        case ID_DEVICE_I2CBUSSCAN:
            I2CBusScan();
            return true;

        case ID_VIEW_REFRESHWINDOW:
            // if the current tab exposes TabEmbeddableWindow, refresh it
            if (curButton != nullptr && curButton->curTab != nullptr)
            {
                if (auto *tew = dynamic_cast<TabEmbeddableWindow*>(curButton->curTab->win.get()); tew != nullptr)
                    tew->OnDeviceReconnect();
            }
            return true;
        }
        break;
    }

    // not handled
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

void WrapperWin::OnInitMenuPopup(HMENU menu, WORD itemPos, bool isSysMenu)
{
    // handle device-related commands, according to device selection and online status
    UINT flags = MF_BYCOMMAND | ((CanExecDeviceCommand(true) != nullptr) ? MF_ENABLED : MF_DISABLED);
    EnableMenuItem(menu, ID_DEVICE_BOOTLOADERMODE, flags);
    EnableMenuItem(menu, ID_DEVICE_REBOOT, flags);
    EnableMenuItem(menu, ID_DEVICE_SAFEMODE, flags);
    EnableMenuItem(menu, ID_DEVICE_INSTALLFIRMWARE, flags);
    EnableMenuItem(menu, ID_DEVICE_CLEARCONFIGURATION, flags);
    EnableMenuItem(menu, ID_DEVICE_FULLFACTORYRESET, flags);
    EnableMenuItem(menu, ID_BACKUP_EXPORTDATAAREAIMAGE, flags);
    EnableMenuItem(menu, ID_BACKUP_EXPORTFULLFLASHIMAGE, flags);
    EnableMenuItem(menu, ID_BACKUP_RESTOREEXPORTEDIMAGE, flags);
    EnableMenuItem(menu, ID_DEVICE_REPAIRJOYCPL, flags);

    // forward it to the active tab for window-specific commands
    if (auto *curTab = GetCurTab(); curTab != nullptr && curTab->win != nullptr)
        curTab->win->OnInitMenuPopup(menu, itemPos, isSysMenu);
}

bool WrapperWin::OnLButtonDown(WPARAM keys, int x, int y)
{
    // check tab controls
    POINT pt{ x, y };
    if (curButton != nullptr)
    {
        for (auto &tab : curButton->tabs)
        {
            // check for a click in the tab
            if (PtInRect(&tab.rc, pt))
            {
                // click in tab - select the tab if it's not already active
                if (&tab != curButton->curTab)
                    SelectTab(tab);

                // handled
                return true;
            }
        }
    }

    // check left panel buttons
    for (auto &btn : leftPanelButtons)
    {
        if (PtInRect(&btn->rc, pt))
        {
            SelectButton(btn.get());
            return true;
        }
    }

    // not handled
    return false;
}

bool WrapperWin::OnRButtonDown(WPARAM keys, int x, int y)
{
    // treat the button-down phase the same way as a left-click, to
    // activate a tab or device button
    OnLButtonDown(keys, x, y);

    // allow default right-click handling to proceed, to activate
    // the context menu if applicable
    return false;
}

bool WrapperWin::OnMouseMove(WPARAM keys, int x, int y)
{
    // handled
    return true;
}

bool WrapperWin::OnLButtonUp(WPARAM keys, int x, int y)
{
    // handled
    return true;
}

bool WrapperWin::OnCaptureChange(HWND hwnd)
{
    // handled
    return true;
}

bool WrapperWin::OnNCHitTest(int x, int y, LRESULT &lresult)
{
    // adjust to client coordinates
    POINT pt{ x, y };
    ScreenToClient(hwnd, &pt);

    // check if it's in the lower right corner of the status bar - treat
    // this as a sizing box
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.left = rc.right - cyStatusline;
    rc.top = rc.bottom - cyStatusline;
    if (PtInRect(&rc, pt))
    {
        lresult = HTBOTTOMRIGHT;
        return true;
    }

    // no special handling required - use the default hit testing
    return false;
}

HMENU WrapperWin::GetContextMenu(POINT pt, HWND &hwndCommand)
{
    // check left panel buttons
    for (auto &btn : leftPanelButtons)
    {
        if (PtInRect(&btn->rc, pt))
            return btn->hContextMenu;
    }

    // no menu for this location
    return NULL;
}


LRESULT WrapperWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    // check for custom messages
    switch (msg)
    {
    case MSG_DEV_REBOOTED:
        // A Device or Boot Loader window explicitly triggered a device reboot. 
        // Flag it so that we can switch to the next new complementary window
        // that appears.  We'll take the proximity in time as evidence that the
        // new device is the same physical device as that represented by the
        // window that triggered the reboot.  The WPARAM indicates which type
        // of window triggered the reboot, as a LastRebootCommand::Source enum.
        lastRebootCommand.Set(wparam);
        return 0;

    case MSG_INSTALL_FIRMWARE:
        ProcessPendingFirmwareInstall();
        return 0;
    }

    // use the default handling
    return __super::WndProc(msg, wparam, lparam);
}

// Refresh our internal device list with a new device enumeration.
// Cross-checks the list against our current list of devices, adding
// a new entry for each device that we didn't know about before, and
// marking existing devices that aren't in the new list as offline.
void WrapperWin::OnNewDeviceList(const std::list<DeviceDesc::ID> &newDevLst)
{
    // Start by marking all of our devices as offline.  We'll
    // restore online status to each we find in the new list.
    for (auto &dev : devices)
    {
        dev.second.wasOnline = dev.second.online;
        dev.second.online = false;
    }

    // process the new list
    bool needSort = false;
    DeviceButton *newDeviceButton = nullptr;
    for (auto &newDev : newDevLst)
    {
        // search for an existing copy of the device
        if (auto it = devices.find(newDev.hwId.ToString()); it != devices.end())
        {
            // We already know about this device.  Mark it as online.
            auto &dev = it->second;
            dev.online = true;

            // Check to see if we need to re-open the connection.  If the
            // device was previously connected, and the handle isn't working,
            // open a new connection.
            //
            // The reason we have to check for a working connection is that
            // Windows in practice will tolerate a brief interruption to the
            // connection at the USB level without invalidating the WinUsb
            // handle.  This allows the USB connection to survive a software
            // reset that completes within a few hundred milliseconds.  But
            // this effect seems unpredictable; the WinUsb handle will survive
            // some brief resets but will drop on others.  So we just have to
            // test it.
            if (dev.device != nullptr && dev.device->device != nullptr)
            {
                bool reconnect = true;
                if (VendorInterface::Shared::Locker l(dev.device, 100); l.locked)
                {
                    // Get device statistics.  This serves as a test to see if the
                    // connection is working, and if so, lets us check the device's
                    // uptime to see if it has rebooted since we last checked in.
                    PinscapePico::Statistics stats;
                    if (dev.device->device->IsDeviceHandleValid()
                        && dev.device->device->QueryStats(&stats, sizeof(stats), false) == PinscapeResponse::OK)
                    {
                        // the connection is working - no need to reconnect
                        reconnect = false;

                        // Check for a reset cycle on the device, by comparing its
                        // uptime to the projected uptime since our last check.  The 
                        // WinUsb connection sometimes survives brief interrupts, such
                        // as a software reset on the Pico side.
                        // 
                        // Allow a margin of error in calculating the projected uptime,
                        // since the USB report is slightly out of date by the time we
                        // receive it (plus, the Pico and PC clocks aren't synchronized,
                        // so we should expect some slight drift between checks).
                        UINT64 now_us = GetTickCount64() * 1000;
                        uint64_t projectedUptime = dev.upTime + (now_us - dev.upTimeTimestamp);
                        if (stats.upTime + 50000 < projectedUptime)
                        {
                            // The device has been up less than expected from the
                            // last check-in, so take this as a reset.  Mark the
                            // device as newly online by setting wasOnline to false.
                            dev.wasOnline = false;
                        }

                        // remember the new up-time
                        dev.upTime = stats.upTime;
                        dev.upTimeTimestamp = now_us;
                    }

                    // if the connection is broken, reconnect
                    if (reconnect)
                    {
                        // reconnect
                        dev.Connect(this, true);

                        // flag the device as requiring a UI update
                        dev.wasOnline = false;
                    }
                }
            }

            // make sure we have its current Pinscape IDs, in case the
            // configuration was updated since we last checked
            dev.id = newDev;
        }
        else
        {
            // we don't know about this device - add a new button for it
            newDeviceButton = AddDevice(newDev);

            // we'll have to re-sort the button list
            needSort = true;
        }
    }

    // Now check for devices that are newly online, so that we
    // can refresh their windows for the possibly updated configuration
    // after a device reset.
    for (auto &itDev : devices)
    {
        // check for a state transition
        auto *curTab = curButton != nullptr ? curButton->curTab : nullptr;
        auto &desc = itDev.second;
        if (desc.online != desc.wasOnline)
        {
            // Set or clear the UI refresh flag in each tab, according
            // to the connection status.
            for (auto &tab : desc.button->tabs)
            {
                // set the UI refresh if online, clear if offline
                tab.uiRefreshPending = desc.online;

                // if this is the current tab, reactivate it immediately
                // to refresh the UI window
                if (&tab == curTab)
                    SelectTab(tab);
            }

            // If it's newly online, update our cached descriptor, in
            // case anything in it changed (unit number, name, etc).
            // We can positively identify the same physical device by 
            // its Pico hardware ID, which is immutable.
            if (desc.online)
            {
                auto it = std::find_if(newDevLst.begin(), newDevLst.end(),
                    [&desc](const DeviceDesc::ID &d) { return d.hwId == desc.id.hwId; });
                if (it != newDevLst.end())
                    desc.id = *it;

                // remember it as the latest new device we've seen
                newDeviceButton = desc.button;
            }
        }
    }

    // If the user performed a manual reboot from a Boot Loader window,
    // within the last few seconds, assume that a new device that was
    // just added represents the same physical device, and switch to its
    // window.
    if (newDeviceButton != nullptr
        && lastRebootCommand.Test(LastRebootCommand::Source::BootLoader))
        SelectButton(newDeviceButton);

    // if necessary, re-sort the button list
    if (needSort)
        SortButtonList();
}

void WrapperWin::OnNewBootDriveList(const RP2BootDevice::RP2BootDeviceList &newList)
{
    // scan the existing button list for buttons to delete
    for (auto itOld = bootLoaderDrives.begin() ; itOld != bootLoaderDrives.end() ; )
    {
        // get the next position, in case we delete this one (which
        // will invalidate the current item iterator, but leave the
        // next one intact)
        decltype(itOld) itNxtOld = itOld;
        ++itNxtOld;

        // find it in the new list
        if (auto it = std::find_if(newList.begin(), newList.end(),
            [&itOld](const RP2BootDevice &newDev) { return newDev.path == itOld->dev.path; });
            it == newList.end())
        {
            // No match - delete the old item.  If its button is
            // currently selected, switch to the New Device button
            // (since that one is always present).
            if (curButton == itOld->button)
            {
                // close the current tab window
                if (curButton->curTab != nullptr && curButton->curTab->win != nullptr)
                {
                    DestroyWindow(curButton->curTab->win->GetHWND());
                    curButton->curTab->win.reset();
                }

                // select the New Device button (which is always in last
                // position in the sorted list)
                SelectButton(leftPanelButtons.back().get());
            }

            // delete the button
            leftPanelButtons.remove_if([&itOld](const std::unique_ptr<LeftPanelButton> &ele) {
                return ele.get() == itOld->button; });

            // delete the list item
            bootLoaderDrives.erase(itOld);
        }

        // on to the next item
        itOld = itNxtOld;
    }

    // scan the new list for buttons to add
    bool needSort = false;
    BootLoaderDriveButton *newBootLoaderDriveButton = nullptr;
    for (auto &itNew : newList)
    {
        // look for an existing button for this drive
        if (auto it = std::find_if(bootLoaderDrives.begin(), bootLoaderDrives.end(),
            [&itNew](const BootLoaderDrive &d) { return d.dev.path == itNew.path; });
            it == bootLoaderDrives.end())
        {
            // not found - add it
            newBootLoaderDriveButton = AddBootLoaderButton(itNew);
            needSort = true;
        }
    }

    // sort the button list if necessary
    if (needSort)
        SortButtonList();

    // If we're currently displaying the Set Up New Device button, and
    // a new boot loader drive just appeared in the list, automatically
    // activate the new drive as the selected button.  The instructions
    // in the Set Up New Device window explain how to get a Pico into
    // boot loader mode manually, so it's a good bet that if a new boot
    // loader device just appeared, it's because the user followed the
    // instructions and intends to set up the new device.  The natural
    // next step is to go to the new drive's window to install firmware.
    // So let's cut out the manual step of selecting the button.
    //
    // Do the same if the user just issued a command to manually reboot
    // a device from Pinscape mode to boot loader mode.
    if (newBootLoaderDriveButton != nullptr
        && (dynamic_cast<SetUpNewDeviceButton*>(curButton) != nullptr
            || lastRebootCommand.Test(LastRebootCommand::Source::Device)))
        SelectButton(newBootLoaderDriveButton);
}

bool WrapperWin::OnClose()
{
    // check all open tabs in all device sets for unsaved work
    std::list<std::string> unsavedTabs;
    for (auto &b : leftPanelButtons)
    {
        // check all tabs in this device/button
        auto *curTab = curButton != nullptr ? curButton->curTab : nullptr;
        for (auto &t : b->tabs)
        {
            // if the window has the modifiable document interface, check for unsaved work
            if (auto *w = dynamic_cast<TabEmbeddableWindow*>(t.win.get());
                w != nullptr && w->IsDocumentModified())
            {
                // add this to the unsaved work list
                char buf[128];
                std::string uclabel = t.label;
                std::transform(uclabel.begin(), uclabel.end(), uclabel.begin(), ::toupper);
                sprintf_s(buf, "%s - %s", uclabel.c_str(), b->GetDisplayName().c_str());
                unsavedTabs.emplace_back(buf);

                // activate the first unsaved window
                if (unsavedTabs.size() == 1 && curTab != &t)
                {
                    SelectButton(b.get());
                    SelectTab(t);
                }
            }
        }
    }

    // if there's any unsaved work, ask what to do about it
    if (unsavedTabs.size() != 0)
    {
        std::string lst;
        for (auto &u : unsavedTabs)
        {
            lst.append("* ");
            lst.append(u.c_str());
            lst.append("\r\n");
        }

        class SaveDialog : public Dialog 
        {
        public:
            SaveDialog(WrapperWin *win, const char *modList) : Dialog(win->hInstance), win(win), modList(modList) { }
            WrapperWin *win;
            const char *modList;

            ~SaveDialog() { DeleteFont(boldFont); }

            virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override
            {
                // create a bold version of the dialog font
                LOGFONT lf{ 0 };
                GetObject(GetWindowFont(hDlg), sizeof(lf), &lf);
                lf.lfWeight = FW_BOLD;
                boldFont = CreateFontIndirect(&lf);

                // set the mod list to the bold font
                SetDlgItemTextA(hDlg, IDC_TXT_MODLIST, modList);
                SetWindowFont(GetDlgItem(IDC_TXT_MODLIST), boldFont, FALSE);

                // do the base class work
                return __super::OnInitDialog(wparam, lparam);
            }

            HFONT boldFont = NULL;
        };
        SaveDialog dlg(this, lst.c_str());
        if (dlg.Show(IDD_SAVEDISCARD, hwnd) != IDYES)
            return true;
    }

    // if the help window is open, explicitly close it so that we save
    // its window placement before we proceed
    if (auto *helpWin = HelpWindow::GetInstance(); helpWin != nullptr)
        SendMessage(helpWin->GetHWND(), WM_CLOSE, 0, 0);

    // save the window placement
    gApp.SaveWindowPlacement(hwnd, windowPlacementJsonKey);

    // proceed to the default handling to allow the window to close
    return __super::OnClose();
}

// --------------------------------------------------------------------------
//
// Get the device associated with the active tab, if any
//
WrapperWin::DeviceDesc *WrapperWin::GetActiveDevice()
{
    if (auto *db = dynamic_cast<DeviceButton*>(curButton) ; db != nullptr)
        return db->dev;
    else
        return nullptr;
}

WrapperWin::DeviceDesc *WrapperWin::CanExecDeviceCommand(bool silent)
{
    if (auto *dd = GetActiveDevice(); dd == nullptr || dd->device == nullptr)
    {
        // no device selected
        if (!silent)
            MessageBoxFmt(hwnd, "No device is currently selected.");
        return nullptr;
    }
    else if (dd->device->device == nullptr)
    {
        // device isn't connected
        if (!silent)
            MessageBoxFmt(hwnd, "Unable to connect to device.");
        return nullptr;
    }
    else if (!dd->online)
    {
        // device is offline
        if (!silent)
            MessageBoxFmt(hwnd, "The device is currently offline.");
        return nullptr;
    }
    else
    {
        // success
        return dd;
    }
}

bool WrapperWin::ExecDeviceCommand(std::function<int(VendorInterface*)> callback,
    const char *cmdDesc, const char *successMsg)
{
    if (auto *dev = CanExecDeviceCommand(false))
    {
        auto &sd = dev->device;
        if (VendorInterface::Shared::Locker l(sd); l.locked)
        {
            if (int status = callback(sd->device.get()); status == PinscapeResponse::OK)
            {
                gApp.AddTimedStatusMessage(successMsg, HRGB(0xffffff), HRGB(0x008000), 5000);
                return true;
            }
            else
            {
                MessageBoxFmt(hwnd, "%s: failed (error code %d)", VendorInterface::ErrorText(status), status);
                return false;
            }
        }
    }

    // failed
    return false;
}

// --------------------------------------------------------------------------
//
// Install firmware
//
void WrapperWin::InstallFirmware(
    HINSTANCE hInstance, HWND hwndDialogParent,
    std::shared_ptr<VendorInterface::Shared> device,
    const TCHAR *filename)
{
    // run the operation via the flash progress dialog
    HRESULT hr = E_FAIL;
    const static HRESULT E_AMBIGUOUS_DRIVE = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x1001);
    FlashProgressDialog dlg(hInstance, [&device, filename, &hr](FlashProgressDialog *dlg)
    {
        // Step 1: get a list of Boot Loader drives currently present,
        // so that we can identify the target device's boot drive by
        // its newness.  We're targeting an active Pinscape unit, so
        // it's not showing its native boot loader drive currently,
        // but will after we reset it into boot loader mode.
        auto oldDrives = RP2BootDevice::EnumerateRP2BootDrives();

        // Step 1: reboot the Pico into the Boot Loader mode
        if (VendorInterface::Shared::Locker l(device); l.locked)
            device->device->EnterBootLoader();

        // Step 2: wait for a new Boot Loader drive to appear
        TSTRING bootPath;
        for (;;)
        {
            // enumerate new drives since our pre-reboot check
            auto newDrives = RP2BootDevice::EnumerateNewRP2BootDrives(oldDrives);
            if (newDrives.size() == 1)
            {
                // one new drive appeared - assume it's our target device
                bootPath = newDrives.front().path;
                break;
            }
            else if (newDrives.size() > 1)
            {
                // more than one new drive appeared - don't try to guess which one;
                // just fail with an error
                hr = E_AMBIGUOUS_DRIVE;
                return;
            }

            // wait a little bit before polling again
            Sleep(100);

            // check for cancellation
            if (dlg->IsCancelRequested())
            {
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                return;
            }
        }

        // Step 3: copy the firmware to the Boot Loader drive
        dlg->SetBannerText(_T("Sending firmware to Pico"));
        hr = RP2BootDevice::InstallFirmware(filename, bootPath.c_str(), dlg);
    });
    dlg.SetCaption(_T("Installing Firmware Update"));
    dlg.SetBannerText(_T("Entering Pico Boot Loader"));
    dlg.Show(IDD_FLASH_COPY_PROGRESS, hwndDialogParent);

    // announce the result
    if (SUCCEEDED(hr))
    {
        // acknowledge the successful installation
        gApp.AddTimedStatusMessage("Success - firmware update installed", HRGB(0xffffff), HRGB(0x008000), 5000);
    }
    else if (hr == E_AMBIGUOUS_DRIVE)
    {
        // E_AMBIGUOUS_DRIVE is our custom error code for multiple drives appearing at once
        MessageBoxFmt(hwndDialogParent, "Error: multiple new Boot Loader drives were detected. "
            "The Config Tool can't determine which one corresponds to the original device. "
            "Please either perform the installation manually on the boot drive, or reset "
            "the device back into Pinscape mode and try again.");
    }
    else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        // other error (besides "user canceled", which requires no explanation)
        MessageBoxFmt(hwndDialogParent, "Error updating firmware (error code %08lx)", static_cast<unsigned long>(hr));
    }
}

// --------------------------------------------------------------------------
//
// Device menu command handlers
//


// reset the device
void WrapperWin::Reboot()
{
    ExecDeviceCommand([](VendorInterface *dev) { return dev->ResetPico(); },
        "Rebooting device", "Success - the Pico has been rebooted");
}

// reboot into safe mode
void WrapperWin::RebootSafeMode()
{
    ExecDeviceCommand([](VendorInterface *dev) { return dev->EnterSafeMode(); },
        "Rebooting device", "Success - the Pico has been rebooted to Safe Mode");
}

// reboot into factory mode
void WrapperWin::RebootFactoryMode()
{
    ExecDeviceCommand([](VendorInterface *dev) { return dev->EnterFactoryMode(); },
        "Rebooting device", "Success - the Pico has been rebooted to Factory Mode");
}

// reboot into boot loader mode
void WrapperWin::BootLoaderMode()
{
    // record the reboot command, so that we know to automatically switch
    // to the new boot loader device window when the device connects
    lastRebootCommand.Set(WrapperWin::LastRebootCommand::Source::Device);
    
    // send the command to the device
    ExecDeviceCommand([](VendorInterface *dev) { return dev->EnterBootLoader(); },
        "Entering boot loader", "Success - the Pico has been rebooted into Boot Loader mode");
}

void WrapperWin::InstallFirmware(int cmd)
{
    // make sure we have a valid target device selected
    if (auto *dev = CanExecDeviceCommand(false); dev != nullptr)
    {
        // Select a dialog title based on the command.  "Install Firmware" and
        // "Restore image" are actually identical operations, since they both
        // just load a UF2 file onto the Pico via its Boot Loader mode.  But
        // we expose them as separate commands to make them easier to find in
        // the respective contexts.  Users are likely to conceptualize them as
        // distinct operations and will thus want to look for each command in
        // a related menu group - in particular, they'll expect to see a Restore
        // command grouped with the Backup commands that create the exported
        // UF2 files.  There's no equivalent "generate" command for a firmware
        // file, since that's a matter of compiling the firmware program from
        // its C++ source, which isn't within the scope of the Config Tool,
        // so it makes more sense to group the Install Firmware command with
        // the other global device-level functions such as resetting the
        // device.  Anyway, to maintain consistency, we'll present the file
        // selection dialog under a title reflecting the conceptual command
        // we're performing, even though both just amount to picking a UF2 
        // file.
        const TCHAR *title =
            (cmd == ID_DEVICE_INSTALLFIRMWARE) ? _T("Select a Pinscape Pico firmware file") :
            (cmd == ID_BACKUP_RESTOREEXPORTEDIMAGE) ? _T("Select a backup image file to restore") :
            _T("Select a Pico UF2 file");

        // get the file to install
        GetFileNameDlg dlg(title, OFN_ENABLESIZING, _T("UF2 Files\0*.uf2\0All Files\0*.*\0"), _T("uf2"));
        if (dlg.Open(hwnd))
        {
            // run the installation
            InstallFirmware(hInstance, hwnd, dev->device, dlg.GetFilename());
        }
    }
}

void WrapperWin::ExportFlash(int cmd)
{
    // make sure we have a valid target device selected
    if (auto *dev = CanExecDeviceCommand(false); dev != nullptr)
    {
        // retrieve flash storage info from the device
        PinscapePico::FlashFileSysInfo fsInfo;
        int status = PinscapeResponse::ERR_FAILED;
        if (VendorInterface::Shared::Locker l(dev->device); l.locked)
            status = dev->device->device->QueryFileSysInfo(&fsInfo, sizeof(fsInfo));

        if (status != PinscapeResponse::OK)
        {
            MessageBoxFmt(hwnd, "Error retrieving flash layout information: %s (error code %d)",
                VendorInterface::ErrorText(status), status);
            return;
        }

        // Figure the sector range
        uint32_t startOfs, endOfs;
        switch (cmd)
        {
        case ID_BACKUP_EXPORTFULLFLASHIMAGE:
            // export the whole flash space
            startOfs = 0;
            endOfs = fsInfo.flashSizeBytes;

            // If the firmware doesn't know the flash size for sure, prompt the user
            // to select the desired export size
            if ((fsInfo.flags & fsInfo.F_FLASH_SIZE_KNOWN) == 0)
            {
                class SizeDialog : public Dialog
                {
                public:
                    SizeDialog(HINSTANCE hInstance) : Dialog(hInstance) { }
                    virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override
                    {
                        CheckDlgButton(hDlg, IDC_RB_2MB, BST_CHECKED);
                        return __super::OnInitDialog(wparam, lparam);
                    }
                    virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom) override 
                    {
                        if (command == IDOK)
                        {
                            // note the selected size button
                            static const struct { int id; int mb; } btnToMB[]{
                                { IDC_RB_1MB, 1 },
                                { IDC_RB_2MB, 2 },
                                { IDC_RB_4MB, 4 },
                                { IDC_RB_8MB, 8 },
                                { IDC_RB_16MB, 16 },
                            };
                            for (auto &b : btnToMB)
                            {
                                if (IsDlgButtonChecked(hDlg, b.id) == BST_CHECKED)
                                {
                                    sizeMB = b.mb;
                                    break;
                                }
                            }
                        }

                        return __super::OnCommand(command, code, ctlFrom);
                    }

                    uint32_t sizeMB = 2;
                };
                SizeDialog dlg(hInstance);
                if (dlg.Show(IDD_FLASH_SIZE, hwnd) == IDOK)
                {
                    // OKed - update the size
                    endOfs = dlg.sizeMB * 1024*1024;
                }
                else
                {
                    // canceled - abort
                    return;
                }
            }
            break;

        case ID_BACKUP_EXPORTDATAAREAIMAGE:
            // export just the file system data region
            startOfs = fsInfo.fileSysStartOffset;
            endOfs = fsInfo.fileSysStartOffset + fsInfo.fileSysByteLength;
            break;

        default:
            // unrecognized command
            return;
        }
    
        // ask for a filename
        GetFileNameDlg dlg(_T("Export Pico flash memory data to local file"), OFN_ENABLESIZING,
            _T("UF2 (Pico boot loader format)\0*.uf2\0Raw Binary Format\0*.bin\0All Files\0*.*\0"), _T("uf2"));
        if (dlg.Save(hwnd))
        {
            // open the file 
            std::ofstream f(dlg.GetFilename(), std::ios_base::out | std::ios_base::binary);
            if (!f)
            {
                MessageBoxFmt(hwnd, "Unable to open output file");
                return;
            }

            // run the export from a progress dialog
            bool ok = false;
            TCHAR resultMsg[256] = _T("OK");
            FlashProgressDialog prog(hInstance, [this, filename = dlg.GetFilename(), &dev, startOfs, endOfs, &f, &ok, &resultMsg](
                FlashProgressDialog *prog)
            {
                // figure the file format based on the name - if it ends in .UF2,
                // export it in UF2 format, otherwise export it as raw binary
                bool isUF2 = (_tcslen(filename) >= 4 && _tcsicmp(filename + _tcslen(filename) - 4, _T(".uf2")) == 0);

                // For UF2 format, each block is labeled with a sequence number, and
                // each block must specify the total number of blocks in the overall 
                // file.  For the Pico, each UF2 block contains 256 bytes of flash data
                // (that's a requirement of the Pico native boot loader), so the number
                // of blocks is the total export size divided by 256.
                static const uint32_t UF2_BLOCK_DATA_SIZE = 256;
                uint32_t uf2BlockNo = 0;
                uint32_t uf2NumBlocks = (endOfs - startOfs) / UF2_BLOCK_DATA_SIZE;

                // initialize the progress dialog
                prog->SetCaption(_T("Exporting flash data"));
                prog->SetBannerText(isUF2 ? _T("Transferring data from Pico in UF2 format") : 
                    _T("Transferring data from Pico in raw binary format"));
                prog->ProgressInit(_T("Pico"), filename, endOfs - startOfs);

                // export the sectors
                for (uint32_t ofs = startOfs, retries = 0 ; ofs < endOfs ; )
                {
                    // read the next sector
                    std::vector<uint8_t> data;
                    int status = PinscapeResponse::ERR_FAILED;
                    if (VendorInterface::Shared::Locker l(dev->device); l.locked)
                        status = dev->device->device->ReadFlashSector(ofs, data);

                    // if the status is BAD TRANSFER DATA, retry a few times, in case we just
                    // garbled some data across the wire
                    if (status == PinscapeResponse::ERR_BAD_REPLY_DATA && retries < 3)
                    {
                        ++retries;
                        continue;
                    }

                    // fail on error
                    if (status != PinscapeResponse::OK)
                    {
                        _stprintf_s(resultMsg, _T("Error reading sector from device (flash offset %d; error code %d, %hs)"),
                            ofs, status, VendorInterface::ErrorText(status));
                        prog->ProgressFinish(false);
                        ok = false;
                        return;
                    }

                    // success - reset the retry counter
                    retries = 0;

                    // write the data to the file
                    if (isUF2)
                    {
                        // UF2 format.  Break the transmission into 256-byte blocks.  Since
                        // the Pico flash sector size (4KB) is an even multiple of the UF2
                        // page size (256 bytes), there's no need to carry over partial
                        // buffers.  But check to make sure this assumption is true, in 
                        // case something unforeseen changes in the protocol; better to
                        // fail with an explanation than to mysteriously lose data.
                        if ((data.size() % UF2_BLOCK_DATA_SIZE) != 0)
                        {
                            _stprintf_s(resultMsg, _T("Flash sector transfer size %ul bytes is not a multiple ")
                                _T("of the UF2 block size (%u bytes).  This program can't generate a UF2 from ")
                                _T("this device.  You can still export in Raw Binary format (select the *.bin ")
                                _T("file type) and convert the file to UF2 with external tools if desired."),
                                static_cast<uint32_t>(data.size()), UF2_BLOCK_DATA_SIZE);
                            ok = false;
                            prog->ProgressFinish(false);
                            return;
                        }

                        // Generate UF2 blocks.  The UF2 file encodes the "target address" for
                        // each block, which is the location in the microcontroller's memory-mapped
                        // address space where the block is written during a transfer from PC to
                        // device.  For the Pico, the flash memory is mapped into the CPU address
                        // space starting at 0x10000000, known symbolically as XIP_BASE.  For the 
                        // purposes of the USB transfer, the offset is expressed as a byte offset 
                        // from the start of the flash space.  So the target address corresponding 
                        // to a given offset is (XIP_BASE + offset).
                        static const uint32_t XIP_BASE = 0x10000000;
                        uint32_t targetAddr = XIP_BASE + ofs;
                        const char *src = reinterpret_cast<const char*>(data.data());
                        for (size_t remaining = data.size(); remaining != 0 && f ; 
                            remaining -= UF2_BLOCK_DATA_SIZE, targetAddr += UF2_BLOCK_DATA_SIZE, 
                            src += UF2_BLOCK_DATA_SIZE, ++uf2BlockNo)
                        {
                            // format a UF2 block and write it to the file
                            RP2BootDevice::UF2_Block blk(targetAddr, uf2BlockNo, uf2NumBlocks, src);
                            f.write(reinterpret_cast<const char*>(&blk), sizeof(blk));
                        }
                    }
                    else
                    {
                        // binary format - just write the bytes exactly as they appear in flash
                        f.write(reinterpret_cast<const char*>(data.data()), data.size());
                    }

                    // check for file errors
                    if (!f)
                    {
                        ok = false;
                        _stprintf_s(resultMsg, _T("Error writing to file"));
                        prog->ProgressFinish(false);
                        return;
                    }
                
                    // advance by the amount read
                    ofs += static_cast<uint32_t>(data.size());

                    // update the progress dialog
                    prog->ProgressUpdate(ofs - startOfs);

                    // set the result message and status to success
                    _stprintf_s(resultMsg, _T("Success - the flash contents have been saved to \"%s\""), filename);
                    ok = true;
                }

                // finish the progress dialog
                prog->ProgressFinish(true);
            });
            prog.Show(IDD_FLASH_COPY_PROGRESS, hwnd);

            // close the file
            if (f)
                f.close();

            // announce the result
            if (!f)
                MessageBoxFmt(hwnd, "Error writing to file - check file permissions and space available on the destination drive");
            else
                MessageBox(hwnd, resultMsg, _T("Export Flash"), MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        }
    }
}

void WrapperWin::ClearConfig()
{
    // make sure they're serious
    if (!PreEraseDeviceConfig(false,
        "You're about to erase the Main and Safe Mode configuration files "
        "from the Pico's flash memory.  Pinscape will revert to defaults for all settings. "
        "This operation can't be undone - the old config files stored on the Pico will be "
        "permanently lost.",
        "Are you sure you want to erase config files from the Pico?"))
        return;

    // clear the configuration device-side
    ExecDeviceCommand(
        [](VendorInterface *dev) { 
            int stat = dev->EraseConfig(PinscapeRequest::CONFIG_FILE_ALL); 
            if (stat == PinscapeResponse::OK) stat = dev->ResetPico();
            return stat;
        },
        "Erasing device configuration files",
        "Success - the device configuration files have been erased.");

    // notify windows of the change
    OnEraseDeviceConfig(false);
}

void WrapperWin::FactoryReset()
{
    // get the list of affected windows, if any
    if (!PreEraseDeviceConfig(true,
        "You're about to erase all settings information stored in the Pico's "
        "flash memory, including the Main and Safe Mode configuration files, as well "
        "as all plunger calibration data and settings.  The Pico will revert to "
        "factory defaults for all settings and calibrations.  This operation can't "
        "be undone.",
        "Are you sure you want to erase config files from the Pico?"))
        return;

    // clear the configuration device-side
    ExecDeviceCommand(
        [](VendorInterface *dev) {
            int stat = dev->FactoryResetSettings(); 
            if (stat == PinscapeResponse::OK) stat = dev->ResetPico();
            return stat; 
        },
        "Resetting to factory defaults",
        "Success - all device settings have been erased and restored to factory defaults");

    // notify windows of the change
    OnEraseDeviceConfig(true);
}

// Check with windows prior to a factory reset or configuration erase command
bool WrapperWin::PreEraseDeviceConfig(
    bool factoryReset, const char *warning, const char *question)
{
    // construct a message showing the tabs where work will be lost
    std::string s;
    if (curButton != nullptr)
    {
        // make a list of work that will be lost
        for (auto &tab : curButton->tabs)
        {
            if (auto *teWin = dynamic_cast<TabEmbeddableWindow*>(tab.win.get());
                teWin != nullptr && teWin->PreEraseDeviceConfig(factoryReset))
            {
                char buf[128];
                sprintf_s(buf, "  * %s\r\n", tab.label);
                s.append(buf);
            }
        }

        // if the window list string is non-empty, add a bracketing message
        if (s.size() != 0)
            s = "In addition, the following changes made in open windows will be lost:\r\n\r\n" + s + "\r\n";
    }

    // construct the overall string
    std::vector<char> buf;
    buf.resize(strlen(warning) + strlen(question) + s.size() + 128);
    sprintf_s(buf.data(), buf.size(), "%s\r\n\r\n%s%s", warning, s.c_str(), question);

    // show the warning, and return true if they click Yes
    return (MessageBoxA(GetDialogOwner(), buf.data(), "Warning - Erasing Configuration",
        MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON2) == IDYES);
}

// Notify device windows that the configuration has been externally reset
void WrapperWin::OnEraseDeviceConfig(bool factoryReset)
{
    if (curButton != nullptr)
    {
        for (auto &t : curButton->tabs)
        {
            if (auto *teWin = dynamic_cast<TabEmbeddableWindow*>(t.win.get()); teWin != nullptr)
                teWin->OnEraseDeviceConfig(factoryReset);
        }
    }
}

void WrapperWin::RepairJoyCpl()
{
    int btn = MessageBoxA(GetDialogOwner(),
        "This will attempt to repair the Game Controllers control panel (JOY.CPL) "
        "display for this device.  If the JOY.CPL Properties window shows the wrong "
        "axis or button layout for the device, it's usually due a bug in JOY.CPL related "
        "to some registry keys that track attached joystick devices.  This command "
        "deletes the keys, which forces the system to rebuild them from scratch, "
        "which usually fixes the JOY.CPL display."
        "\r\n\r\n"
        "Please close all applications that might be connected to the device before "
        "proceeding.  The JOY.CPL bug seems more likely to occur when one or more "
        "applications are actively accessing the device during a reset."
        "\r\n\r\n"
        "Do you wish to proceed with deleting the keys?",
        "Pinscape Pico Config Tool",
        MB_ICONQUESTION | MB_YESNO);
    if (btn != IDYES)
        return;

    // get a list of current boot devices
    auto origBootDrives = RP2BootDevice::EnumerateRP2BootDrives();

    // Get the key name, and reboot the device to boot loader mode
    std::string keyName;
    bool ok = ExecDeviceCommand(
        [&keyName](VendorInterface *dev) -> int
    {
        // get the device descriptor, for the VID/PID information
        USB_DEVICE_DESCRIPTOR dd;
        if (int stat = dev->GetDeviceDescriptor(dd); stat != PinscapeResponse::OK)
            return stat;

        // Form the key name:
        // HKEY_CURRENT_USER\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\DirectInput\VID_vid&PID_pid\Calibration
        keyName = StrPrintf(
            "System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\DirectInput\\VID_%04X&PID_%04X\\Calibration",
            dd.idVendor, dd.idProduct);

        // put the device in Boot Loader mode, to make sure it's disconnected
        return dev->EnterBootLoader();
    }, "Repairing JOY.CPL display", "Device reset to Boot Loader mode");

    if (ok)
    { 
        // wait for the device to reappear as a boot drive
        RP2BootDevice::RP2BootDeviceList newBootDrives;
        for (UINT64 tEnd = GetTickCount64() + 2000 ; newBootDrives.size() == 0 && GetTickCount64() < tEnd ;)
            newBootDrives = RP2BootDevice::EnumerateNewRP2BootDrives(origBootDrives);

        // delete the key
        ok = (RegDeleteTreeA(HKEY_CURRENT_USER, keyName.c_str()) == ERROR_SUCCESS);

        // if we found one boot drive, reboot it
        bool okBooted = false;
        if (newBootDrives.size() == 1)
            okBooted = newBootDrives.front().RebootPico();

        // announced the result
        std::string msg;
        if (ok)
            msg = "Success - the registry keys have been deleted.";
        else
            msg = "Failed - the registry keys could not be deleted.  You might not have "
            "the necessary permissions on this account to delete the keys.  See the Help "
            "for instructions on a manual procedure you can try using RegEdit.";

        if (!okBooted)
            msg += "\r\n\r\n"
            "The device has been placed into Boot Loader mode, but the Config Tool "
            "was unable to reset it back to normal operation.  You can manually reset "
            "the device by unplugging it and reconnecting it.";

        MessageBoxA(GetDialogOwner(), msg.c_str(), "Pinscape Pico Config Tool", MB_OK);
    }
}

// Start an I2C bus scan
void WrapperWin::I2CBusScan()
{
    // initiate the scan
    bool ok = ExecDeviceCommand(
        [](VendorInterface *dev) { return dev->I2CBusScan(); }, "Starting I2C bus scan",
        "I2C bus scan started; check log window for results");

    // switch to the log window
    if (ok)
        SwitchToLogWin();
}

// device change notification handler
bool WrapperWin::OnDevNodesChanged()
{
    // enumerate Pinscape devices, and send the list to the main
    // thread for comparison against its existing device list
    std::list<DeviceDesc::ID> descs;
    HRESULT hr = EnumerateDevices(descs);
    if (SUCCEEDED(hr))
        OnNewDeviceList(descs);

    // enumerate RP2 Boot Loader drives, and send it to the main
    // thread for comparison to its drive list
    RP2BootDevice::RP2BootDeviceList bootList;
    bootList = RP2BootDevice::EnumerateNewRP2BootDrives(bootList);
    OnNewBootDriveList(bootList);

    // let the default handling proceed
    return __super::OnDevNodesChanged();
}

bool WrapperWin::OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr)
{
    // if our device was just removed, close its handle
    if (hdr->dbch_devicetype == DBT_DEVTYP_HANDLE)
    {
        // get the device handle
        HANDLE hDev = reinterpret_cast<DEV_BROADCAST_HANDLE*>(hdr)->dbch_handle;

        // search our devices for the handle being removed
        for (auto &dev : devices)
        {
            // match on device handle
            auto &shared = dev.second.device;
            if (shared != nullptr && shared->device != nullptr && shared->device->GetDeviceHandle() == hDev)
            {
                // matched - close our handle and unregister notifications
                if (VendorInterface::Shared::Locker l(shared); l.locked)
                {
                    shared->device->CloseDeviceHandle();
                    shared->UnregisterNotify();
                }
            }
        }
    }

    // proceed with default handling
    return __super::OnDeviceRemoveComplete(hdr);
}

// --------------------------------------------------------------------------
//
// Tab window creation
//

void WrapperWin::CreateNoDevWin()
{
    RECT crc;
    GetClientRect(hwnd, &crc);

    auto *curTab = GetCurTab();
    curTab->win.reset(new NoDeviceWin(hInstance));
    curTab->win->CreateSysWindow(curTab->win, WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, hwnd, _T("No Device"),
        cxLeftPanel + cxPanelMargin, cyTabCtl, crc.right - crc.left, crc.bottom - crc.top - cyTabCtl - cyStatusline, SW_SHOW);
}

bool WrapperWin::TestShowOfflineDeviceWin(Tab *tab)
{
    // if the device is marked as offline, create the offline window
    auto *dev = static_cast<DeviceButton*>(curButton)->dev;
    if (!dev->online || dev->device == nullptr || dev->device->device == nullptr)
    {
        // display the offline device window
        ShowWindow(offlineDeviceWin->GetHWND(), SW_SHOW);
        return true;
    }
    
    // if the device descriptor is currently marked as online, and the device
    // object has been created, test to see if the connection is working
    if (dev->online && dev->device != nullptr && dev->device->device != nullptr)
    {
        if (VendorInterface::Shared::Locker l(dev->device, 100); l.locked)
        {
            // test the connection
            PinscapePico::DeviceID id;
            if (dev->device->device->Ping() == PinscapeResponse::OK)
            {
                // the connection is working - no need for an offline window
                return false;
            }
        }
    }

    // mark the device as offline
    dev->online = false;

    // use the offline window
    ShowWindow(offlineDeviceWin->GetHWND(), SW_SHOW);
    return true;
}

void WrapperWin::CreateOverviewWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    DeviceOverviewWin *ovwin = new DeviceOverviewWin(hInstance, dev);
    curTab->win.reset(ovwin);
    ovwin->CreateSysWindow(curTab->win,
        WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL, 0, hwnd, _T("Config Editor"),
        cxLeftPanel + cxPanelMargin, cyTabCtl,
        crc.right - cxLeftPanel - cxPanelMargin, crc.bottom - cyTabCtl,
        SW_SHOW);
}

void WrapperWin::SwitchToLogWin()
{
    if (curButton != nullptr)
    {
        for (auto &tab : curButton->tabs)
        {
            if (tab.createWindow == &WrapperWin::CreateLogWin)
            {
                SelectTab(tab);
                break;
            }
        }
    }
}

void WrapperWin::CreateLogWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the log window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::LogViewerWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0);
}

void WrapperWin::CreateConfigWin(uint8_t fileID)
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // create the config editor window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    ConfigEditorWin *cfgWin = new ConfigEditorWin(hInstance, dev, fileID);
    curTab->win.reset(cfgWin);
    cfgWin->CreateSysWindow(curTab->win,
        WS_CHILDWINDOW | WS_CLIPCHILDREN, 0, hwnd, _T("Config Editor"),
        cxLeftPanel + cxPanelMargin, cyTabCtl, 
        crc.right - cxLeftPanel - cxPanelMargin, crc.bottom - cyTabCtl,
        SW_SHOW);
}

void WrapperWin::CreateMainConfigWin()
{
    CreateConfigWin(VendorRequest::CONFIG_FILE_MAIN);
}

void WrapperWin::CreateSafeModeConfigWin()
{
    CreateConfigWin(VendorRequest::CONFIG_FILE_SAFE_MODE);
}

void WrapperWin::CreateButtonTesterWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the button tester window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::ButtonTesterWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0);
}

void WrapperWin::CreateOutputPortTesterWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the output tester window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::OutputTesterWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0);

    // set logical port mode
    auto *outputWin = static_cast<OutputTesterWin*>(curTab->win.get());
    outputWin->SetLogicalPortMode();
}

void WrapperWin::CreateOutputDevTesterWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the output tester window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::OutputTesterWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0);

    // put it in device tester mode
    auto *outputWin = static_cast<OutputTesterWin*>(curTab->win.get());
    outputWin->SetDeviceTestMode();
}

void WrapperWin::CreateNudgeWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the plunger setup window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::NudgeDeviceWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL, 0);
}

void WrapperWin::CreatePlungerWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the plunger setup window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::PlungerCalWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL, 0);
}

void WrapperWin::CreateIRWin()
{
    // use an offline window if necessary
    auto *curTab = GetCurTab();
    if (TestShowOfflineDeviceWin(curTab))
        return;

    // create the IR setup window
    auto &dev = static_cast<DeviceButton*>(curButton)->dev->device;
    PinscapePico::IRTesterWin::Factory factory;
    curTab->win = factory.Create(hInstance, hwnd, SW_SHOW, dev,
        cxLeftPanel + cxPanelMargin, cyTabCtl, WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL, 0);
}

void WrapperWin::CreateSetUpNewDeviceWin()
{
    // get the client area
    RECT crc;
    GetClientRect(hwnd, &crc);

    // set up the new window
    auto *curTab = GetCurTab();
    curTab->win.reset(new SetUpNewDeviceWin(hInstance));
    curTab->win->CreateSysWindow(curTab->win, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0, hwnd, _T("Setup New Device"),
        cxLeftPanel + cxPanelMargin, cyTabCtl, crc.right - crc.left, crc.bottom - crc.top - cyTabCtl - cyStatusline, SW_SHOW);
}


void WrapperWin::CreateBootLoaderDriveWin()
{
    // get the client area
    RECT crc;
    GetClientRect(hwnd, &crc);

    // get the boot loader device currently selected
    RP2BootDevice *dev = nullptr;
    if (auto *b = reinterpret_cast<BootLoaderDriveButton*>(curButton); b != nullptr)
        dev = &b->dev;

    if (dev == nullptr)
        return;

    // set up the new window
    auto *curTab = GetCurTab();
    curTab->win.reset(new BootLoaderWin(hInstance, dev));
    curTab->win->CreateSysWindow(curTab->win, WS_CHILDWINDOW | WS_CLIPCHILDREN, 0, hwnd, _T("Boot Loader"),
        cxLeftPanel + cxPanelMargin, cyTabCtl, crc.right - crc.left, crc.bottom - crc.top - cyTabCtl - cyStatusline, SW_SHOW);
}


// --------------------------------------------------------------------------
//
// Firmware drag/drop support
//

bool WrapperWin::IsFirmwareDropLocation(POINTL ptl)
{
    // adjust to client coordinates
    POINT pt{ ptl.x, ptl.y };
    ScreenToClient(hwnd, &pt);

    // some buttons are drop targets
    for (auto &btn : leftPanelButtons)
    {
        // if it's in this button, ask the button if it's a drop target
        if (PtInRect(&btn->rc, pt))
            return btn->IsFirmwareDropTarget();
    }

    // not a drop target
    return false;
}

void WrapperWin::ExecFirmwareDrop(POINTL ptl, const TCHAR *filename)
{
    // adjust to client coordinates
    POINT pt{ ptl.x, ptl.y };
    ScreenToClient(hwnd, &pt);

    // some buttons are drop targets
    for (auto &btn : leftPanelButtons)
    {
        // if it's in this button, pass the drop to the button
        if (PtInRect(&btn->rc, pt))
        {
            // Post a deferred drop event to myself.  We can't do the install
            // here, because the Drop handler is called from a nested event
            // loop on the source application's UI thread, so we'll lock out
            // the other application's UI for the duration of the firmware
            // copy to the Pico if do it here.
            pendingFirmwareInstall.filename = filename;
            pendingFirmwareInstall.buttonID = btn->uniqueID;
            PostMessage(hwnd, MSG_INSTALL_FIRMWARE, 0, 0);
            return;
        }
    }
}

void WrapperWin::ProcessPendingFirmwareInstall()
{
    // find the button
    for (auto &btn : leftPanelButtons)
    {
        if (btn->uniqueID == pendingFirmwareInstall.buttonID)
        {
            btn->ExecFirmwareDrop(this, pendingFirmwareInstall.filename.c_str());
            return;
        }
    }
}

void WrapperWin::DeviceButton::ExecFirmwareDrop(WrapperWin *win, const TCHAR *filename)
{
    win->InstallFirmware(win->hInstance, win->GetDialogOwner(), dev->device, filename);
}

void WrapperWin::BootLoaderDriveButton::ExecFirmwareDrop(WrapperWin *win, const TCHAR *filename)
{
    HRESULT hr = S_OK;
    FlashProgressDialog dlg(win->hInstance, [this, filename, &hr](FlashProgressDialog *dlg)
    {
        dlg->SetBannerText(_T("Sending firmware to Pico"));
        hr = RP2BootDevice::InstallFirmware(filename, WSTRINGToTSTRING(dev.path).c_str(), dlg);
    });
    dlg.SetCaption(_T("Installing Firmware Update"));
    dlg.Show(IDD_FLASH_COPY_PROGRESS, win->GetDialogOwner());

    // announce the result
    if (SUCCEEDED(hr))
    {
        // acknowledge the successful installation
        gApp.AddTimedStatusMessage("Success - firmware update installed", HRGB(0xffffff), HRGB(0x008000), 5000);
    }
    else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        // error (other than "user canceled", which requires no explanation
        MessageBoxFmt(win->GetDialogOwner(), "Error updating firmware (error code %08lx)", static_cast<unsigned long>(hr));
    }
}

