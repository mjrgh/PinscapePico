// Pinscape Pico - Output Tester Window
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
#include <Windows.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "PinscapePicoAPI.h"
#include "WinUtil.h"
#include "OutputTesterWin.h"
#include "Application.h"
#include "FlashProgressDialog.h"
#include "Utilities.h"
#include "resource.h"


// the window class is part of the PinscapePico namespace
using namespace PinscapePico;

OutputTesterWin::OutputTesterWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
    DeviceThreadWindow(hInstance, device, new UpdaterThread())
{
    // Load my menu bar and accelerator
    hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_DEVICEWIN));
    hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_DEVICEWIN));

    // load bitmaps
    bmpAttrIcons = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_OUTPUT_ATTR_ICONS));
    BITMAP bmp;
    GetObject(bmpAttrIcons, sizeof(bmp), &bmp);
    cxAttrIcons = cyAttrIcons = bmp.bmHeight;  // multiple square cells - use height as cell width

    // Query device information
    QueryDeviceInfo();
}

bool OutputTesterWin::OnDeviceReconnect()
{
    // re-query the device info
    QueryDeviceInfo();

    // refresh successful - tell the caller to keep this window open
    return true;
}

void OutputTesterWin::OnEraseDeviceConfig(bool factoryReset)
{
    // re-query the device information
    QueryDeviceInfo();
}
    
void OutputTesterWin::QueryDeviceInfo()
{
    // query the output port configuration, for logical ports and device ports
    std::vector<PinscapePico::OutputLevel> portLevels;
    bool testMode = false;
    auto &device = updaterThread->device;
    if (VendorInterface::Shared::Locker l(device); l.locked)
    {
        // query the output port descriptors
        int err;
        if ((err = device->device->QueryLogicalOutputPortConfig(portDescs)) != PinscapeResponse::OK
            || (err = device->device->QueryOutputDeviceConfig(deviceDescs)) != PinscapeResponse::OK
            || (err = device->device->QueryOutputDevicePortConfig(devicePortDescs)) != PinscapeResponse::OK)
            descErrorCode = err;

        // query the initial port level list
        device->device->QueryLogicalOutputLevels(testMode, portLevels);
    }

    // Count TLC5940 and 74HC595 chains.  If there's only one chain
    // of a given type, as is typical, we can elide the chain number
    // when referring to these chips.  That looks cleaner and avoids
    // creating confusion for the user for the common case where
    // there's only one chain.  While we're at it, count up the
    // device ports.
    int numDevPorts = 0;
    for (auto &dev : deviceDescs)
    {
        // count the ports
        numDevPorts += dev.numPorts;

        // if it's a daisy-chain type chip, count the chain
        if (dev.devType == PinscapePico::OutputPortDesc::DEV_TLC5940)
            numTLC5940 += 1;
        else if (dev.devType == PinscapePico::OutputPortDesc::DEV_74HC595)
            num74HC595 += 1;
    }

    // allocate sliders
    logSlider.resize(portDescs.size());
    testModeSlider.resize(numDevPorts);

    // initialize the logical port sliders with the current levels set on the device side
    for (size_t i = 0, n = min(portLevels.size(), logSlider.size()); i < n ; ++i)
        logSlider[i].level = portLevels[i].hostLevel;

    // initialize the test-mode sliders with device information
    {
        int idx = 0;
        auto *ds = testModeSlider.data();
        const auto *portDesc = devicePortDescs.data();
        for (auto &dev : deviceDescs)
        {
            for (unsigned int i = 0 ; i < dev.numPorts && idx < numDevPorts && idx < static_cast<int>(devicePortDescs.size()) ;
                ++i, ++idx, ++ds, ++portDesc)
            {
                ds->devType = dev.devType;
                ds->configIndex = dev.configIndex;
                ds->port = i;

                switch (portDesc->type)
                {
                case portDesc->TYPE_PWM:
                    ds->numSteps = dev.pwmRes;
                    break;

                case portDesc->TYPE_DIGITAL:
                    ds->numSteps = 2;
                    break;

                default:
                    ds->numSteps = 1;
                    ds->enabled = false;
                }
            }
        }
    }
}

OutputTesterWin::~OutputTesterWin()
{
}

bool OutputTesterWin::InstallParentMenuBar(HWND hwndContainer)
{
    SetMenu(hwndContainer, hMenuBar);
    return true;
}

// translate keyboard accelerators
bool OutputTesterWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

void OutputTesterWin::SetLogicalPortMode()
{
    // set normal (logical port) mode
    auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
    ut->uiTestMode = false;

    // show logical mode controls, hide test mode controls
    ShowWindow(sbPortPanel, SW_SHOW);
    ShowWindow(sbDevPanel, SW_SHOW);
    ShowWindow(sbTestMode, SW_HIDE);
    AdjustScrollbarRanges();
}

void OutputTesterWin::SetDeviceTestMode()
{
    // set test mode (direct device access)
    auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
    ut->uiTestMode = true;

    // hide/show mode-specific controls
    ShowWindow(sbPortPanel, SW_HIDE);
    ShowWindow(sbDevPanel, SW_HIDE);
    ShowWindow(sbTestMode, SW_SHOW);
    AdjustScrollbarRanges();
}


// Paint off-screen
void OutputTesterWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

    HDCHelper hdc(hdc0);
    auto DrawSeparator = [this, &hdc](int x0, int x1, int y)
    {
        HPen SepPen(RGB(0xE0, 0xE0, 0xE0));
        HPEN savepen = SelectPen(hdc, SepPen);
        MoveToEx(hdc, x0, y, NULL);
        LineTo(hdc, x1, y);
        SelectPen(hdc, savepen);
    };

    // get the color for a PWM level, 0-255
    static const auto LevelFill = [](uint8_t level) { return RGB(0, level, 0); };
    static const auto LevelText = [](uint8_t level) { return (level < 215 ? RGB(0xff, 0xff, 0xff) : RGB(0, 0, 0)); };

    // draw a slider bar
    auto DrawSlider = [this, &hdc](SliderCtl &slider, int index)
    {
        // refigure the level on an 8-bit scale, for the fill color
        float levelf = static_cast<float>(slider.level) / (slider.numSteps - 1);
        uint8_t level8 = static_cast<uint8_t>(roundf(levelf * 255.0f));

        // draw the bar
        int yBar = (slider.rc.top + slider.rc.bottom - cySliderBar)/2;
        int x = slider.rc.left;
        RECT src{ x, yBar, x + cxSliderBar, yBar + cySliderBar };
        FillRect(hdc, &src, HBrush(slider.enabled ? LevelFill(level8) : HRGB(0xC0C0C0)));
        if (slider.enabled)
            FrameRect(hdc, &src, GetStockBrush(BLACK_BRUSH));

        // draw the thumb
        if (slider.enabled)
        {
            int xThumb = x + static_cast<int>(roundf(levelf * static_cast<float>(cxSliderBar - cxSliderThumb)));
            int yThumb = (slider.rc.top + slider.rc.bottom - cySliderThumb)/2;
            RECT trc{ xThumb, yThumb, xThumb + cxSliderThumb, yThumb + cySliderThumb };
            HBrush br(HRGB(index == focusSlider ?
                (static_cast<uint32_t>(GetTickCount64()) % 1000) < 500 ? 0x80ffff : 0xff8000 :
                0xffffff));
            FillRect(hdc, &trc, br);
            FrameRect(hdc, &trc, GetStockBrush(BLACK_BRUSH));

            // draw the numeric entry in progress, if any
            if (index == focusSlider && focusNumberEntry >= 0)
            {
                char buf[32];
                sprintf_s(buf, "%d", focusNumberEntry);
                SIZE sz = hdc.MeasureText(mainFont, buf);
                int xn = xThumb - sz.cx - 8;
                int yn = yThumb + (cySliderThumb - sz.cy - 8)/2;
                RECT rcn{ xn, yn, xn + sz.cx + 8, yn + sz.cy + 8 };
                FillRect(hdc, &rcn, HBrush(HRGB(0xffff80)));
                FrameRect(hdc, &rcn, HBrush(HRGB(0x808040)));
                hdc.DrawText(xn + 4, yn + 4, 1, mainFont, HRGB(0x000000), buf);
            }
        }
    };

    // Describe a device.  The port is the actual port number or an example of any
    // port on the chip, which lets us identify the specific chip if it's part of
    // a daisy chain.  Returns the chip-relative port number.
    const auto GetDevName = [this](char *buf, size_t bufSize, int devType, int devId, int devPort)
    {
        static const char *devName[] ={ "Invalid", "Virtual", "Pico GPIO", "TLC59116", "TLC5940", "PCA9685", "PCA9555", "74HC595", "ZBLaunch" };
        using PortDesc = PinscapePico::OutputPortDesc;
        switch (devType)
        {
        case PortDesc::DEV_VIRTUAL:
        case PortDesc::DEV_ZBLAUNCH:
        case PortDesc::DEV_GPIO:
            // these are unique devices, so don't include chain/chip information
            sprintf_s(buf, bufSize, "%s", devName[devType]);
            return devPort;

        case PortDesc::DEV_TLC5940:
            // show the chain (if there's more than one), chip, and port number relative to the chip
            if (numTLC5940 > 1)
                sprintf_s(buf, bufSize, "TLC5940 %c#%d", devId + 'A', devPort/16);
            else
                sprintf_s(buf, bufSize, "TLC5940 #%d", devPort/16);
            return devPort % 16;

        case PortDesc::DEV_74HC595:
            // show the chain (if there's more than one), chip, and port number relative to the chip
            if (num74HC595 > 1)
                sprintf_s(buf, bufSize, "74HC595 %c%d", devId + 'A', devPort/8);
            else
                sprintf_s(buf, bufSize, "74HC595 %d", devPort/8);
            return devPort % 8;

        case PortDesc::DEV_TLC59116:
        case PortDesc::DEV_PCA9555:
        case PortDesc::DEV_PCA9685:
            // chip number & port number
            sprintf_s(buf, bufSize, "%s #%d", devName[devType], devId);
            return devPort;

        case PortDesc::DEV_PWMWORKER:
            // unit number & port number
            sprintf_s(buf, bufSize, "Worker Pico #%d", devId);
            return devPort;

        default:
            sprintf_s(buf, bufSize, "Unknown(0x%02x)", devType);
            return devPort;
        }
    };

    // describe a device port
    const auto GetDevPortName = [this, GetDevName](char *buf, size_t bufSize, int devType, int devId, int devPort)
    {
        // get the device name
        char devName[40];
        devPort = GetDevName(devName, _countof(devName), devType, devId, devPort);

        // add the port number, if applicable for the device type
        using PortDesc = PinscapePico::OutputPortDesc;
        switch (devType)
        {
        case PortDesc::DEV_VIRTUAL:
        case PortDesc::DEV_ZBLAUNCH:
            // no port information - just use the device name unchanged
            strcpy_s(buf, bufSize, devName);
            break;

        case PortDesc::DEV_GPIO:
            // add the port number without any extra verbiage
            sprintf_s(buf, bufSize, "%s %d", devName, devPort);
            break;

        case PortDesc::DEV_PCA9555:
            // use the NXP notation for the port names
            sprintf_s(buf, bufSize, "%s IO%d_%d", devName, devPort / 8, devPort % 8);
            break;

        default:
            // for everything else, add "port N"
            sprintf_s(buf, bufSize, "%s port %d", devName, devPort);
            break;
        }
    };

	// fill with white background
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

    // select a gray pen for drawing separator lines
    HPen grayPen(RGB(0xC0, 0xC0, 0xC0));
    HPEN oldpen = SelectPen(hdc, grayPen);

    // set up a DC for copying bitmaps
    CompatibleDC bdc(hdc);

    // get the updater thread object, cast to our subclass
    auto *ut = static_cast<UpdaterThread*>(updaterThread.get());

    // draw the tab control
    static const COLORREF clrTabSel = HRGB(0xf5cc84);
    static const COLORREF clrTabHot = HRGB(0xbbccf1);
    static const COLORREF clrTabNorm = HRGB(0x3b4f81);
    static const COLORREF clrTabTxtSel = HRGB(0x01247a);
    static const COLORREF clrTabTxtNorm = HRGB(0xffffff);
    RECT rctc{ crc.left, crc.top, crc.right, crc.top + cyTabCtl };
    FillRect(hdc, &rctc, HBrush(RGB(0x5c, 0x6d, 0x99)));
    RECT rctcGutter{ rctc.left, rctc.bottom - cyTabBottomMargin, rctc.right, rctc.bottom };
    FillRect(hdc, &rctcGutter, HBrush(clrTabSel));
    auto DrawTab = [this, &hdc, &rctc](int &x, const char *label, bool selected, RECT &tabrc)
    {
        // figure the tab size
        int padLeft = 8;
        int padRight = 12;
        SIZE sz = hdc.MeasureText(boldFont, label);
        tabrc ={ x, rctc.top + cyTabTopMargin, x + padLeft + padRight + sz.cx, rctc.bottom - cyTabBottomMargin };

        // draw slightly shorter for non-selected tabs
        if (!selected)
            tabrc.top += 1;

        // figure if the mouse is over the tab
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        bool hot = PtInRect(&tabrc, pt);

        // draw it
        FillRect(hdc, &tabrc, HBrush(selected ? clrTabSel : hot ? clrTabHot : clrTabNorm));
        hdc.DrawText(x + padLeft, tabrc.bottom - cyTabBottomPadding - sz.cy, 1,
            selected ? boldFont : mainFont, selected || hot ? clrTabTxtSel : clrTabTxtNorm, label);

        // advance the x position past the tab, to set up for the next tab
        x = tabrc.right;
    };
    int xTab = rctc.left + cxTabLeftMargin;
    if (cyTabCtl != 0)
    {
        DrawTab(xTab, "Logical Port Mode", !ut->uiTestMode, rcTabNormalMode);
        DrawTab(xTab, "Direct Device Test Mode", ut->uiTestMode, rcTabTestMode);
    }

    // lock the mutex so that we can access the current state data
	if (WaitForSingleObject(updaterThread->dataMutex, 100) == WAIT_OBJECT_0)
	{
        // metrics
        const int xMargin = 16;

        // draw the panel for normal or test mode
        std::list<IconTip> newIconTips;
        if (!ut->uiTestMode)
        {
            //
            // Normal operating mode.  Show the logical port list on the
            // left, and the physical device states on the right.
            //

            // metrics
            const int xPortCol = crc.left + xMargin;
            const int xDevCol = xPortCol + szBoldFont.cx*3;
            const int xAttrCol = xDevCol + szBoldFont.cx*12;
            const int xLevelCol = xAttrCol + cxAttrIcons*5 + 24;
            const int cxLevelBox = 32;
            const int levelBoxSpacing = 8;
            const int xLevelCol2 = xLevelCol + cxLevelBox + levelBoxSpacing;
            const int xLevelCol3 = xLevelCol2 + cxLevelBox + levelBoxSpacing;
            const int xSliderCol = xLevelCol3 + cxLevelBox + levelBoxSpacing;

            // if the left panel width is too narrow, refigure it
            int cxPanelNew = xSliderCol + cxSliderBar + xMargin;
            if (cxPanelNew > cxPanel)
            {
                cxPanel = cxPanelNew;
                AdjustLayout();
            }

            // draw the logical output port list header title
            RECT hrc{ crc.left, crc.top + cyTabCtl, crc.left + cxPanel + cxScrollbar, crc.top + cyHeaderLeft + cyTabCtl };
            FillRect(hdc, &hrc, HBrush(RGB(0xF0, 0xF0, 0xF0)));
            int y = hrc.top + 8;
            y += hdc.DrawText(xPortCol, y, 1, boldFont, RGB(0x80, 0, 0x80), "Logical (DOF) Ports").cy;

            // separator bar
            DrawSeparator(crc.left, cxPanel + cxScrollbar, cyHeaderRight + cyTabCtl - 1);

            // draw the column headers
            y = hrc.bottom - szBoldFont.cy - 4;
            hdc.DrawText(xPortCol, y, 1, boldFont, RGB(0, 0, 0), "Port");
            hdc.DrawText(xDevCol, y, 1, boldFont, RGB(0, 0, 0), "Device Port");
            hdc.DrawText(xAttrCol, y, 1, boldFont, RGB(0, 0, 0), "Attributes");
            hdc.DrawText(xLevelCol, y, 1, boldFont, RGB(0, 0, 0), "Host");
            hdc.DrawText(xLevelCol2, y, 1, boldFont, RGB(0, 0, 0), "Calc");
            hdc.DrawText(xLevelCol3, y, 1, boldFont, RGB(0, 0, 0), "Out");

            // draw a PWM level box
            auto DrawLevelBox = [this, &hdc](const RECT &rc, uint8_t level)
            {
                // fill and frame the rect
                FillRect(hdc, &rc, HBrush(LevelFill(level)));
                FrameRect(hdc, &rc, HBrush(RGB(0, 0, 0)));

                // draw the level number centered in the box
                char label[20];
                sprintf_s(label, "%d", level);
                SIZE sz = hdc.MeasureText(boldFont, label);
                hdc.DrawText((rc.right + rc.left - sz.cx)/2, (rc.bottom + rc.top - sz.cy)/2, 1, boldFont, LevelText(level), label);
            };

            // draw the output ports
            IntersectClipRect(hdc, crc.left, hrc.bottom, hrc.right, crc.bottom);
            RECT portrc{ crc.left, hrc.bottom - yScrollPorts, crc.left + cxPanel, hrc.bottom - yScrollPorts + cyOutput - 1 };
            const auto *pLevel = ut->portLevels.data();
            const auto *pDesc = portDescs.data();
            for (unsigned int i = 0 ; i < ut->portLevels.size() && i < portDescs.size() ;
                ++i, ++pLevel, ++pDesc, OffsetRect(&portrc, 0, cyOutput))
            {
                // get this row's slider control object
                auto &slider = logSlider[i];

                // skip drawing if we're out of bounds
                if (portrc.bottom < crc.top + hrc.bottom || portrc.top > crc.bottom)
                {
                    // clear the slider coordinates, as this slider isn't visible
                    slider.rc ={ 0,0,0,0 };

                    // skip drawing
                    continue;
                }

                // start at left, with margins
                int yTxt = (portrc.top + portrc.bottom - szBoldFont.cy)/2;

                // draw the port number label
                hdc.DrawTextF(xPortCol, yTxt, 1, boldFont, RGB(0, 0, 0), "#%d", i+1);

                // draw the device information
                char devDesc[40];
                GetDevPortName(devDesc, _countof(devDesc), pDesc->devType, pDesc->devId, pDesc->devPort);
                hdc.DrawText(xDevCol, yTxt, 1, mainFont, RGB(0, 0, 0), devDesc);

                // draw the attribute icons
                POINT ptAttrs{ xAttrCol, (portrc.top + portrc.bottom - cyAttrIcons)/2 };
                bdc.Select(bmpAttrIcons);
                auto DrawAttrIcon =[this, &hdc, &ptAttrs, pDesc, &bdc, &newIconTips](
                    int iconIndex, unsigned int flagBit, const char *text)
                {
                    // each icon takes up two cells in the image - one for ON, one for off
                    int bmpPixOffset = cxAttrIcons * iconIndex * 2;

                    // select the ON or OFF icon - the ON icon is first, the OFF icon is the next cell to the right
                    bool isOn = ((pDesc->flags & flagBit) != 0);
                    if (!isOn)
                        bmpPixOffset += cxAttrIcons;

                    // draw the cell
                    BitBlt(hdc, ptAttrs.x, ptAttrs.y, cxAttrIcons, cyAttrIcons,
                        bdc, bmpPixOffset, 0, SRCCOPY);

                    // add a tooltip
                    newIconTips.emplace_back(ptAttrs, cxAttrIcons, cyAttrIcons, isOn ? text : text + strlen(text) + 1);

                    // advance the drawing position
                    ptAttrs.x += cxAttrIcons;
                };
                DrawAttrIcon(0, pDesc->F_GAMMA, "Gamma Correction Enabled\0No Gamma Correction");
                DrawAttrIcon(1, pDesc->F_NOISY, "Noisy Device (disabled in Night Mode)\0Normal device, not affected by Night Mode");
                DrawAttrIcon(2, pDesc->F_INVERTED, "Inverted Polarity/Active Low Output\0Normal Active-High Output");
                DrawAttrIcon(3, pDesc->F_FLIPPERLOGIC, "Flipper Logic Enabled\0Flipper Logic timers not used on this port");
                DrawAttrIcon(4, pDesc->F_COMPUTED, "Computed-Value Output Port\0Normal Host-Controlled Output");

                // draw the level boxes (Host, Calc, Out)
                RECT rclvl{ xLevelCol, portrc.top + 2, xLevelCol + 32, portrc.bottom - 2 };
                DrawLevelBox(rclvl, pLevel->hostLevel);
                OffsetRect(&rclvl, xLevelCol2 - xLevelCol, 0);

                DrawLevelBox(rclvl, pLevel->calcLevel);
                OffsetRect(&rclvl, xLevelCol3 - xLevelCol2, 0);

                DrawLevelBox(rclvl, pLevel->outLevel);
                OffsetRect(&rclvl, xLevelCol3 - xLevelCol2, 0);

                // figure the slider control's screen coordinates
                int xSlider = rclvl.left;
                slider.rc ={ xSlider, portrc.top + 2, xSlider + cxSliderBar, portrc.bottom - 2 };

                // draw it
                DrawSlider(slider, i);

                // separator
                MoveToEx(hdc, portrc.left, portrc.bottom, NULL);
                LineTo(hdc, portrc.right, portrc.bottom);
            }
            SelectClipRgn(hdc, NULL);

            // devices panel header
            hrc.left = hrc.right + 1;
            hrc.right = crc.right;
            hrc.bottom = crc.top + cyHeaderRight + cyTabCtl;
            FillRect(hdc, &hrc, HBrush(RGB(0xf0, 0xf0, 0xf0)));
            y = hrc.top + 8;
            const int xPhysCol = hrc.left + xMargin;
            y += hdc.DrawText(xPhysCol, y, 1, boldFont, RGB(0x80, 0, 0x80), "Physical Device Ports").cy;

            // clip to the devices scrolling area
            IntersectClipRect(hdc, cxPanel, hrc.bottom, crc.right - cxScrollbar, crc.bottom);

            // set up coordinates
            int x0 = hrc.left + xMargin;
            int x = x0;
            int y0 = hrc.bottom - yScrollDev;
            y = y0 + 8;

            // draw a device port
            static const int cxPortBox = min(36, szBoldFont.cx*3);
            static const int cyPortBox = min(18, szBoldFont.cy+2);
            auto DrawDevPort = [this, &hdc, &x, &y](uint8_t devType, int portNum, uint16_t level, uint16_t pwmRes, bool enabled = true)
            {
                // treat disabled ports as level 0
                if (!enabled)
                    level = 0;

                // figure the level on the 8-bit scale
                uint8_t level8 = static_cast<uint8_t>(roundf(255.0f * level / (pwmRes - 1.0f)));

                // label the port number
                static const int cxLabel = szMainFont.cx * 2;
                static const int cxGap = 4;
                using PortDesc = PinscapePico::OutputPortDesc;
                switch (devType)
                {
                case PortDesc::DEV_PCA9555:
                    // use the abbreviated IOn_m notation, leaving off the IO prefix
                    hdc.DrawTextF(x + cxLabel, y + (cyPortBox - szMainFont.cy)/2, -1, boldFont, HRGB(0x000000), "%d_%d",
                        portNum / 8, portNum % 8);
                    break;

                default:
                    // for everything else, just show the port number as a decimal value
                    hdc.DrawTextF(x + cxLabel, y + (cyPortBox - szMainFont.cy)/2, -1, boldFont, HRGB(0x000000), "%d", portNum);
                    break;
                }
                x += cxLabel + cxGap;

                // draw the level box
                RECT rc{ x, y, x + cxPortBox, y + cyPortBox };
                FillRect(hdc, &rc, enabled ? LevelFill(level8) : HBrush(HRGB(0xE0E0E0)));
                FrameRect(hdc, &rc, GetStockBrush(BLACK_BRUSH));
                x += cxPortBox + cxGap;

                // label the level, or "X" if the port is disabled
                char levelLabel[32];
                if (!enabled)
                    strcpy_s(levelLabel, "X");
                else if (pwmRes == 2)
                    sprintf_s(levelLabel, "%s", level != 0 ? "ON" : "OFF");
                else
                    sprintf_s(levelLabel, "%d", level);
                SIZE sz = hdc.MeasureText(boldFont, levelLabel);
                hdc.DrawText((rc.left + rc.right - sz.cx)/2, (rc.bottom + rc.top - sz.cy)/2, 1,
                    boldFont, LevelText(level8), levelLabel);
            };

            // draw the device ports
            workerUpdateButtons.clear();
            auto itDesc = deviceDescs.begin();
            auto itPort = devicePortDescs.begin();
            auto itLevel = ut->devLevels.begin();
            for (int idx = 0 ; itDesc != deviceDescs.end() && itLevel != ut->devLevels.end() ; ++itDesc, ++idx)
            {
                // show each port for this device
                for (int port = 0 ; port < itDesc->numPorts && itPort != devicePortDescs.end() ; ++port, ++itLevel, ++itPort)
                {
                    // For port 0, and the start of each port on the chip, label 
                    // a new chip section.  Otherwise, start a new row every 8 ports.
                    if ((port % itDesc->numPortsPerChip) == 0)
                    {
                        // add spacing, except at the first entry
                        if (idx != 0)
                        {
                            x = x0;
                            y += cyPortBox + 12;
                        }

                        // show the label
                        char devName[40];
                        GetDevName(devName, sizeof(devName), itDesc->devType, itDesc->configIndex, port);
                        SIZE sz = hdc.DrawText(x, y, 1, boldFont, HRGB(0x000000), devName);

                        // add the Update Firmware button if this is a PWMWorker Pico
                        if (itDesc->devType == OutputPortDesc::DEV_PWMWORKER)
                        {
                            int xb = x + sz.cx + 16;
                            const char *label = "Update Firmware";
                            SIZE szb = hdc.MeasureText(boldFont, label);
                            RECT rcb{ xb, y, xb + szb.cx, y + szb.cy };
                            hdc.DrawText(xb, y, 1, boldFont, IsClientRectHot(rcb) ? HRGB(0xA000A0) : HRGB(0x00000FF), label);

                            // add the button tracker
                            workerUpdateButtons.emplace_back(rcb, itDesc->configIndex);
                        }

                        y += sz.cy + 4;
                    }
                    else if ((port % 8) == 0)
                    {
                        x = x0;
                        y += cyPortBox + 4;
                    }

                    // draw the port
                    DrawDevPort(itDesc->devType, port % itDesc->numPortsPerChip, itLevel->level, 
                        itPort->type == itPort->TYPE_PWM ? itDesc->pwmRes : 2, 
                        itPort->type != itPort->TYPE_UNUSED);
                }
            }

            // set the document height, and readjust the scrollbars if necessary
            int newDevPanelDocHeight = y - y0;
            if (devPanelDocHeight != newDevPanelDocHeight)
            {
                devPanelDocHeight = newDevPanelDocHeight;
                AdjustScrollbarRanges();
            }

            // remove the clipping area
            SelectClipRgn(hdc, NULL);
        }
        else
        {
            //
            // Test Mode
            //

            // show the warning at the bottom
            RECT rcWarn{ crc.left, crc.bottom - cyWarning, crc.right, crc.bottom };
            FillRect(hdc, &rcWarn, HBrush(HRGB(0xffff00)));

            int y = rcWarn.top + cyWarningPad;
            int x = xMargin;
            y += hdc.DrawText(x, y, 1, boldFont, HRGB(0xA00000), "Warning: Flipper Logic timer protection is disabled while in direct hardware test mode.").cy;
            y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Use caution with devices that might overheat if activated too long.").cy;

            // clip to the scrolling area
            IntersectClipRect(hdc, crc.left, crc.top + cyTabCtl, crc.right - cxScrollbar, crc.bottom - cyWarning);

            // draw the device sliders
            int y0 = cyTabCtl - yScrollTestMode;
            y = y0 + 8;
            int yMax = y;
            int yPortMargin = 8;
            int cyPort = cySliderThumb + yPortMargin;
            auto itDesc = deviceDescs.begin();
            auto itPort = devicePortDescs.begin();
            auto itLevel = ut->devLevels.begin();
            const int cxCol = cxSliderBar + szBoldFont.cx*12 + 16;
            int colNum = 0;
            int xCol = xMargin;
            int yColTop = y;
            for (int idx = 0 ; itDesc != deviceDescs.end() && itLevel != ut->devLevels.end() ; ++itDesc)
            {
                // if we're not on the first item, start a new column or row
                if (idx != 0)
                {
                    xCol += cxCol;
                    yMax = max(y, yMax);
                    if (xCol + cxCol > crc.right - cxScrollbar)
                    {
                        // no room for a new column - advance to the next row
                        y = yMax + 8;
                        DrawSeparator(crc.left, crc.right, y);
                        y += 8;

                        // this is the new column top
                        yColTop = y;
                        x = xCol = xMargin;
                    }
                    else
                    {
                        // there's room for a column - note the high-water mark, and wrap to the top
                        y = yColTop;
                    }
                }

                // show each port for this device
                for (int port = 0 ; port < itDesc->numPorts ; ++port, ++itLevel, y += cyPort, ++idx)
                {
                    // Label the section at the start of each new chip
                    if ((port % itDesc->numPortsPerChip) == 0)
                    {
                        // add some spacing, except for the first chip in a group
                        x = xCol;
                        if (port != 0)
                            y += 16;

                        // show the label
                        char devName[40];
                        GetDevName(devName, sizeof(devName), itDesc->devType, itDesc->configIndex, port);
                        x += hdc.DrawText(x, y, 1, boldFont, HRGB(0x000000), devName).cx + 16;

                        // add the PWM resolution
                        if (itDesc->pwmRes == 2)
                            hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x606060), "%d ports, digital (on/off)", itDesc->numPortsPerChip);
                        else
                            hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x606060), "%d ports, %d-step PWM", itDesc->numPortsPerChip, itDesc->pwmRes);

                        x = xCol;
                        y += szBoldFont.cy + 8;
                    }

                    // skip sliders that aren't in view
                    auto &slider = testModeSlider[idx];
                    if (y + cyPort < crc.top + cyTabCtl || y > crc.bottom - cyWarning)
                    {
                        slider.rc ={ 0, 0, 0, 0 };
                        continue;
                    }

                    // figure the slider's new screen coordinates
                    int xLabel = xCol + 16;
                    int cxLabel = szBoldFont.cx*2;
                    int xSlider = xLabel + cxLabel + 8;
                    slider.rc ={ xSlider, y, xSlider + cxSliderBar, y + cySliderThumb };

                    // label it with the port number/name
                    int yLabel = y + (cySliderThumb - szBoldFont.cy)/2;
                    using PortDesc = PinscapePico::OutputPortDesc;
                    switch (itDesc->devType)
                    {
                    case PortDesc::DEV_PCA9555:
                        // use the abbreviated IOn_m notation, leaving off the IO prefix
                        hdc.DrawTextF(xLabel + cxLabel, yLabel, -1, boldFont, HRGB(0x000000), "%d_%d",
                            (port % itDesc->numPortsPerChip) / 8, port % 8);
                        break;

                    default:
                        // for everything else, show the port number as a decimal value
                        hdc.DrawTextF(xLabel + cxLabel, yLabel, -1, boldFont, slider.enabled ? HRGB(0x000000) : HRGB(0xc0c0c0),
                            "%d", port % itDesc->numPortsPerChip);
                        break;
                    }

                    // draw the slider
                    DrawSlider(slider, idx);
                    x = slider.rc.right + 16;

                    // label the current level
                    if (slider.enabled)
                        hdc.DrawTextF(x, yLabel, 1, boldFont, HRGB(0x000000), "%d/%d", slider.level, slider.numSteps - 1);
                }
            }

            // remove the clipping area
            SelectClipRgn(hdc, NULL);

            // figure the final document height
            if (int newDocHt = max(y, yMax) - y0; newDocHt != testModeDocHeight)
            {
                testModeDocHeight = newDocHt;
                AdjustScrollbarRanges();
            }
        }

        // done with the struct mutex
		ReleaseMutex(updaterThread->dataMutex);

        // update all changed tips
        TOOLINFOA tt{ sizeof(tt), TTF_SUBCLASS, hwnd };
        bool diff = false;
        for (auto itOld = iconTips.begin(), itNew = newIconTips.begin() ;
            itNew != newIconTips.end() ; ++itNew, ++tt.uId)
        {
            if (itOld == iconTips.end()
                || (memcmp(&itOld->rc, &itNew->rc, sizeof(RECT)) != 0 || itOld->text != itNew->text))
            {
                tt.rect = itNew->rc;
                tt.lpszText = const_cast<char*>(itNew->text);
                SendMessage(tooltips, TTM_ADDTOOLA, 0, reinterpret_cast<LPARAM>(&tt));
                diff = true;
            }

            if (itOld != iconTips.end())
                ++itOld;
        }

        // erase any old tips beyond the end of the list
        if (newIconTips.size() > iconTips.size())
        {
            tt.uId = 0;
            for (auto &tip : iconTips)
            {
                if (tt.uId >= newIconTips.size())
                {
                    diff = true;
                    SendMessage(tooltips, TTM_DELTOOLA, 0, reinterpret_cast<LPARAM>(&tt));
                }

                tt.uId++;
            }
        }

        // remember the new list if there were any differences
        if (diff)
            iconTips = newIconTips;
	}

    // clean up drawing resources
    SelectPen(hdc, oldpen);
}

void OutputTesterWin::AdjustLayout()
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// position the left panel scrollbar
	MoveWindow(sbPortPanel, cxPanel, crc.top + cyHeaderLeft + cyTabCtl, cxScrollbar, crc.bottom - crc.top - cyHeaderLeft - cyTabCtl, TRUE);

    // position the devices panel scrollbar
    MoveWindow(sbDevPanel, crc.right - cxScrollbar, crc.top + cyHeaderRight + cyTabCtl, cxScrollbar, crc.bottom - crc.top - cyHeaderRight - cyTabCtl, TRUE);

    // position the test mode scrollbar
    MoveWindow(sbTestMode, crc.right - cxScrollbar, crc.top + cyTabCtl, cxScrollbar, crc.bottom - crc.top - cyTabCtl - cyWarning, TRUE);
}

void OutputTesterWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

    // add our system menu items
    HMENU sysMenu = GetSystemMenu(hwnd, FALSE);

    MENUITEMINFOA mii{ sizeof(MENUITEMINFOA), MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_CHECKMARKS };
    mii.fType = MFT_STRING;
    mii.dwTypeData = const_cast<LPSTR>("Help");
    mii.wID = ID_HELP;
    mii.hbmpChecked = mii.hbmpUnchecked = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_MENUICON_HELP));
    InsertMenuItemA(sysMenu, SC_CLOSE, FALSE, &mii);

    mii.fMask = MIIM_FTYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemA(sysMenu, SC_CLOSE, FALSE, &mii);

    // create the tooltips control
    tooltips = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, NULL, hInstance, NULL);

    // set up for layout computation - get the client area size and window DC
    RECT crc;
    GetClientRect(hwnd, &crc);
    HDC dc = GetWindowDC(hwnd);

    // figure the width of the logical port panel and height of a port cell
    HFONT oldfont = SelectFont(dc, mainFont);
    GetTextExtentPoint32A(dc, "M", 1, &szMainFont);
    cxPanelMin = szMainFont.cx * 42;
    cyHeaderLeft = szMainFont.cy*2 + 20;
    cyOutput = max(szMainFont.cy + yMarginOutput + 1, cyAttrIcons + 4);

    // right panel metrics
    cyHeaderRight = szMainFont.cy + 12;

    // get the bold font size
    SelectFont(dc, boldFont);
    GetTextExtentPoint32A(dc, "M", 1, &szBoldFont);

    // Figure the top tab control metrics.  The tab control is only
    // shown if we're a top-level window; when we're embedded, we
    // rely on the parent to show a UI to select the mode.
    if ((GetWindowStyle(hwnd) & WS_CHILD) == 0)
        cyTabCtl = szBoldFont.cy + cyTabTopMargin + cyTabBottomMargin + cyTabTopPadding + cyTabBottomPadding + 1;

    // create the logical port panel scrollbar
	cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	sbPortPanel = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
		0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_PORTS), hInstance, 0);

    // range calculation for the output scroll panel
    auto GetRangePorts = [this](SCROLLINFO &si)
    {
        // figure the client area height
        RECT crc;
        GetClientRect(hwnd, &crc);
        int winHt = crc.bottom - crc.top - cyHeaderLeft - cyTabCtl;

        // figure the document height - number of ports x port height
        int docHt = cyOutput * static_cast<int>(portDescs.size());

        // set the range
        si.nMin = 0;
        si.nMax = max(docHt - cyOutput/2, 0);
        si.nPage = max(winHt - cyOutput, 20);
    };

    // get the output scroll region
    auto GetScrollRectPorts = [this](RECT *rc)
    {
        rc->top += cyHeaderLeft + cyTabCtl;
        rc->right = rc->left + cxPanel;
    };

    // change the scroll position for the output scroll panel
    auto SetScrollPosPorts = [this](int newPos, int deltaPos) { yScrollPorts = newPos; };

    // set up the scrollbar object
    scrollbars.emplace_back(sbPortPanel, SB_CTL, cyOutput, true, true, GetRangePorts, GetScrollRectPorts, SetScrollPosPorts);

    // Output devices panel scrollbar
    sbDevPanel = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
        0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_DEVS), hInstance, 0);

    // range calculation for the devices panel
    auto GetRangeDevs = [this](SCROLLINFO &si)
    {
        // figure the client area height
        RECT crc;
        GetClientRect(hwnd, &crc);
        int panelHt = crc.bottom - crc.top - cyHeaderRight - cyTabCtl;

        // set the range
        si.nMin = 0;
        si.nMax = max(devPanelDocHeight, 0);
        si.nPage = max(panelHt - cyLineScrollDev, 20);
    };

    // get the devices panel scrolling area
    auto GetScrollRectDevs = [this](RECT *rc)
    {
        rc->top += cyHeaderRight + cyTabCtl;
        rc->left += cxPanel;
        rc->right -= cxScrollbar;
    };

    // change the scroll position for the devices panel
    auto SetScrollPosDevs = [this](int newPos, int deltaPos) { yScrollDev = newPos; };

    // set up the scrollbar object
    scrollbars.emplace_back(sbDevPanel, SB_CTL, cyLineScrollDev, true, true, GetRangeDevs, GetScrollRectDevs, SetScrollPosDevs);

    // test mode panel metrics
    cyWarning = szMainFont.cy*2 + cyWarningPad*2;

    // Test mode panel scrollbar
    sbTestMode = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
        0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_DEVS), hInstance, 0);

    // range calculation for the devices panel
    auto GetRangeTestMode = [this](SCROLLINFO &si)
    {
        // figure the client area height
        RECT crc;
        GetClientRect(hwnd, &crc);
        int panelHt = crc.bottom - crc.top - cyTabCtl - cyWarning;

        // set the range
        si.nMin = 0;
        si.nMax = max(testModeDocHeight, 0);
        si.nPage = max(panelHt - cyLineScrollTestMode, 20);
    };

    // get the devices panel scrolling area
    auto GetScrollRectTestMode = [this](RECT *rc)
    {
        rc->top += cyTabCtl;
        rc->right -= cxScrollbar;
        rc->bottom -= cyWarning;
    };

    // change the scroll position for the devices panel
    auto SetScrollPosTestMode = [this](int newPos, int deltaPos) { yScrollTestMode = newPos; };

    // set up the scrollbar object
    scrollbars.emplace_back(sbTestMode, SB_CTL, cyLineScrollTestMode, true, true, GetRangeTestMode, GetScrollRectTestMode, SetScrollPosTestMode);

    // adjust the layout
	AdjustLayout();
	
	// done with the window DC - clean it up and release it
	SelectFont(dc, oldfont);
	ReleaseDC(hwnd, dc);

    // Note descriptor errors if we're running as a top-level window.
    // Don't do this in embedded mode; we'll count on the container
    // application to track device connection status.
    if (descErrorCode != PinscapeResponse::OK && (GetWindowStyle(hwnd) & WS_CHILD) == 0)
        MessageBoxFmt(hwnd, "An error occurred retrieving the output port information from the "
            "Pinscape Pico device (error code %d).  The tester won't be able to show port "
            "information during this session.  Please exit and restart the program.", descErrorCode);

}

void OutputTesterWin::OnActivateUI(bool isAppActivate)
{
    // remove slider focus
    focusSlider = -1;
    focusNumberEntry = -1;

    // reset controls to the device settings
    auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
    if (VendorInterface::Shared::Locker l(ut->device); l.locked)
    {
        // query the new levels
        int stat = -1;
        if (ut->uiTestMode)
        {
            ut->device->device->SetOutputTestMode(true, 2500);
            stat = ut->device->device->QueryPhysicalOutputDeviceLevels(ut->devLevels);
        }
        else
        {
            stat = ut->device->device->QueryLogicalOutputLevels(ut->devTestMode, ut->portLevels);
        }

        // update the sliders
        if (stat == PinscapeResponse::OK)
        {
            if (ut->uiTestMode)
            {
                for (size_t i = 0, nSlider = testModeSlider.size(), nPorts = ut->devLevels.size() ;
                    i < nSlider && i < nPorts ; ++i)
                    testModeSlider[i].level = ut->devLevels[i].level;
            }
            else
            {
                for (size_t i = 0, nSlider = logSlider.size(), nPorts = ut->portLevels.size() ;
                    i < nSlider && i < nPorts ; ++i)
                    logSlider[i].level = ut->portLevels[i].hostLevel;
            }
        }
    }

    // inherit the default handling
    __super::OnActivateUI(isAppActivate);
}

void OutputTesterWin::OnTimer(WPARAM timerId)
{
    if (timerId == TIMER_ID_NUMBER_ENTRY)
    {
        // implicitly complete the number entry when the time expires
        CommitNumberEntry();

        // this is a one-shot
        KillTimer(hwnd, timerId);

        // handled)
        return;
    }

    // use the default handling
    __super::OnTimer(timerId);
}

bool OutputTesterWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (ctlCmdId)
    {
    case ID_HELP_HELP:
        // note - this applies when we're running as a child window under a
        // container app like the Config Tool, where the parent frame window
        // provides a standard Help menu with this item
        ShowHelpFile(static_cast<UpdaterThread*>(updaterThread.get())->uiTestMode ?
            "PhysicalOutputTester.htm" : "LogicalOutputTester.htm");
        return true;
    }

    // not handled
    return false;
}

bool OutputTesterWin::OnSysCommand(WPARAM id, WORD x, WORD y, LRESULT &lresult)
{
    switch (id)
    {
    case ID_HELP:
        // note - this applies when we're running in standalone mode, where we
        // install our Help command in the system window menu
        ShowHelpFile("OutputTester.htm");
        return true;
    }

    // not handled
    return false;
}

void OutputTesterWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the layout
	AdjustLayout();

    // do the base class work
    __super::OnSizeWindow(type, width, height);
}

void OutputTesterWin::ForEachSlider(std::function<bool(SliderCtl&, int)> callback)
{
    auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
    if (ut->uiTestMode)
    {
        int i = 0;
        for (auto &s : testModeSlider)
        {
            if (!callback(s, i++))
                break;
        }
    }
    else
    {
        int i = 0;
        for (auto &s : logSlider)
        {
            if (!callback(s, i++))
                break;
        }
    }
}

bool OutputTesterWin::OnLButtonDown(WPARAM keys, int x, int y)
{
    // Check if we're over a slider control.  If we're in test mode,
    // the device sliders are active; otherwise the logical port
    // sliders are active.
    auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
    POINT pt{ x, y };
    bool foundSlider = false;
    ForEachSlider([this, pt, &foundSlider](SliderCtl &slider, int index)
    {
        if (PtInRect(&slider.rc, pt))
        {
            // start tracking slider, capturing the mouse for the duration of the drag
            SetCapture(hwnd);
            trackingSlider = &slider;
            focusSlider = index;
            focusNumberEntry = -1;
            TrackSlider(pt.x, pt.y);

            // stop here
            foundSlider = true;
            return false;
        }

        // keep searching
        return true;
    });

    // stop if we found a slider to handle it
    if (foundSlider)
        return true;

    // clear slider focus
    focusSlider = -1;
    focusNumberEntry = -1;

    // check tab controls
    if (PtInRect(&rcTabNormalMode, pt))
    {
        // Logical Port Mode tab
        SetLogicalPortMode();
        return true;
    }
    else if (PtInRect(&rcTabTestMode, pt))
    {
        // set Direct Device Output Test mode
        SetDeviceTestMode();
        return true;
    }

    // check PWMWorker firmware update buttons
    for (auto &b : workerUpdateButtons)
    {
        if (PtInRect(&b.rc, pt))
        {
            UpdatePWMWorkerFirmware(b.id);
            return true;
        }
    }

    // not handled
    return false;
}

bool OutputTesterWin::OnMouseMove(WPARAM keys, int x, int y)
{
    // track the active slider if appropriate
    if (trackingSlider != nullptr)
        TrackSlider(x, y);

    // handled
    return true;
}

bool OutputTesterWin::OnLButtonUp(WPARAM keys, int x, int y)
{
    // release slider capture
    if (trackingSlider != nullptr)
        ReleaseCapture();

    // handled
    return true;
}

bool OutputTesterWin::OnCaptureChange(HWND hwnd)
{
    // end slider tracking
    trackingSlider = nullptr;

    // handled
    return true;
}

bool OutputTesterWin::OnKeyDown(WPARAM vkey, LPARAM flags)
{
    switch (vkey)
    {
    case VK_TAB:
        // commit the current number entry, if one is in progerss
        CommitNumberEntry();

        // move focus to the next/previous slider
        {
            auto ut = static_cast<UpdaterThread*>(updaterThread.get());
            bool testMode = ut->uiTestMode;
            bool shift = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);

            int nMax = static_cast<int>(testMode ? testModeSlider.size() : logSlider.size()) - 1;
            for (int i = 0 ; i < nMax ; ++i)
            {
                focusSlider += shift ? -1 : 1;
                focusSlider = (focusSlider < 0 ? nMax : focusSlider > nMax ? 0 : focusSlider);
                SliderCtl *sc = testMode ? &testModeSlider[focusSlider] : &logSlider[focusSlider];
                if (sc->enabled)
                    break;
            }

            focusNumberEntry = -1;
        }
        return true;

    case VK_ESCAPE:
        // end numeric entry, or kill focus if no numeric entry
        if (focusNumberEntry >= 0)
            focusNumberEntry = -1;
        else if (focusSlider >= 0)
            focusSlider = -1;
        return true;

    case VK_RETURN:
        // commit numeric entry
        CommitNumberEntry();
        return true;

    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
        // bump focus slider value
        {
            auto ut = static_cast<UpdaterThread*>(updaterThread.get());
            bool testMode = ut->uiTestMode;
            int nMax = static_cast<int>(testMode ? testModeSlider.size() : logSlider.size()) - 1;
            bool shift = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            bool ctrl = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);

            if (focusSlider >= 0 && focusSlider <= nMax)
            {
                SliderCtl *sc = (testMode ? &testModeSlider[focusSlider] : &logSlider[focusSlider]);
                int delta = shift ? sc->numSteps / 10 : ctrl ? sc->numSteps / 25 : 1;
                sc->level += (vkey == VK_LEFT || vkey == VK_DOWN) ? -delta : delta;
                sc->level = (sc->level < 0 ? 0 : sc->level > sc->numSteps - 1 ? sc->numSteps - 1 : sc->level);
                QueueSliderChange(sc);
            }
        }
        return true;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        // numeric entry
        if (focusSlider != -1)
        {
            // accumulate the digit into the number entry in progress
            if (focusNumberEntry == -1)
                focusNumberEntry = static_cast<int>(vkey - '0');
            else
                focusNumberEntry = focusNumberEntry*10 + static_cast<int>(vkey - '0');
            
            // check range
            if (focusNumberEntry > 65535)
            {
                // out of range - abort number entry
                focusNumberEntry = -1;
                KillTimer(hwnd, TIMER_ID_NUMBER_ENTRY);
            }
            else
            {
                // start or extend the automatic entry timer
                SetTimer(hwnd, TIMER_ID_NUMBER_ENTRY, 1500, NULL);
            }
        }
        return true;

    case VK_BACK:
        if (focusSlider != -1 && focusNumberEntry != -1)
        {
            focusNumberEntry /= 10;
            if (focusNumberEntry == 0)
                focusNumberEntry = -1;
        }
        return true;
    }

    // use the default handling
    return __super::OnKeyDown(vkey, flags);
}

void OutputTesterWin::CommitNumberEntry()
{
    // if a number entry is in progress, apply it to the focus slider
    if (focusSlider >= 0 && focusNumberEntry >= 0)
    {
        // get the slider
        auto ut = static_cast<UpdaterThread*>(updaterThread.get());
        bool testMode = ut->uiTestMode;
        SliderCtl *sc = (testMode ? &testModeSlider[focusSlider] : &logSlider[focusSlider]);

        // update the slider
        sc->level = focusNumberEntry < sc->numSteps ? focusNumberEntry : sc->numSteps - 1;
        QueueSliderChange(sc);

        // clear the number entry, kill the pending timer
        focusNumberEntry = -1;
        KillTimer(hwnd, TIMER_ID_NUMBER_ENTRY);
    }
}


void OutputTesterWin::TrackSlider(int x, int y)
{
    if (trackingSlider != nullptr)
    {
        // update the UI
        auto *ut = static_cast<UpdaterThread*>(updaterThread.get());
        bool testMode = ut->uiTestMode;
        auto &s = *trackingSlider;
        int x0 = s.rc.left + cxSliderThumb/2;
        int x1 = s.rc.right - cxSliderThumb/2;
        float frac = static_cast<float>(x - x0) / static_cast<float>(x1 - x0);
        uint16_t range = s.numSteps - 1;
        uint16_t newLevel = frac < 0.0f ? 0 : frac > 1.0f ? range : static_cast<uint16_t>(roundf(frac * range));

        // if it's changed, update the UI and device
        if (newLevel != s.level)
        {
            s.level = newLevel;
            QueueSliderChange(&s);
        }
    }
}

void OutputTesterWin::QueueSliderChange(SliderCtl *s)
{
    // make sure we redraw
    InvalidateRect(hwnd, &s->rc, FALSE);

    // queue the change to the device
    auto ut = static_cast<UpdaterThread*>(updaterThread.get());
    if (WaitForSingleObject(ut->dataMutex, 100) == WAIT_OBJECT_0)
    {
        if (!ut->uiTestMode)
        {
            ut->portLevelChanges.insert_or_assign(static_cast<uint8_t>(s - &logSlider[0]), static_cast<uint8_t>(s->level));
        }
        else
        {
            auto *ds = static_cast<DevSliderCtl*>(s);
            int idx = static_cast<int>(ds - &testModeSlider[0]);
            ut->devLevelChanges.insert_or_assign(idx, UpdaterThread::DevLevelChange{ 
                ds->devType, ds->configIndex, ds->port, static_cast<uint16_t>(s->level) });
        }
        ReleaseMutex(ut->dataMutex);
    }
}


// --------------------------------------------------------------------------
//
// PWMWorker firmware update
//

void OutputTesterWin::UpdatePWMWorkerFirmware(int id)
{
    // prompt to plug in the Pico
    int btn = MessageBoxA(GetDialogOwner(),
        "This will update the PWMWorker firmware program installed on the "
        "selected Worker Pico.  Before proceeding, make sure that the Pico "
        "to be updated is plugged into a USB port on the PC.",
        "Worker Pico Firmware Update", MB_ICONINFORMATION | MB_OKCANCEL);
    if (btn != IDOK)
        return;

    // we might have to select more than once
    std::wstring filename;
    for (;;)
    {
        // select the firmware file
        GetFileNameDlg fd(_T("Select PWMWorker Firmware"), OFN_ENABLESIZING, _T("UF2 Files\0*.uf2\0All Files\0*.*\0"), _T("uf2"));
        if (!fd.Open(GetDialogOwner()))
            return;
        
        // get the filename
        filename = fd.GetFilename();

        // if the filename matches the expected naming pattern, accept it
        static const std::wregex fnPat(L".*pwmworker.*\\.uf2", std::regex_constants::icase);
        if (std::regex_match(filename, fnPat))
            break;

        // warn in case they accidentally picked the wrong file
        btn = MessageBoxA(GetDialogOwner(),
            StrPrintf("The selected file (%" _TSFMT ") doesn't match the expected name, PWMWorker.uf2. "
                "Are you sure you want to install this file?", filename.c_str()).c_str(), 
            "Check File Selection", MB_ICONWARNING | MB_YESNO);

        // if they answered Yes, proceed
        if (btn == IDYES)
            break;
    }

    // run the operation via the flash progress dialog
    HRESULT hr = E_FAIL;
    const static HRESULT E_AMBIGUOUS_DRIVE = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x1001);
    const static HRESULT E_REBOOT_ERROR = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x1002);
    int bootStat = PinscapeResponse::OK;
    FlashProgressDialog dlg(hInstance, [this, &filename, &hr, &bootStat](FlashProgressDialog *dlg)
    {
        // get a list of boot drives
        auto oldDrives = RP2BootDevice::EnumerateRP2BootDrives();

        // reboot the Worker Pico
        if (VendorInterface::Shared::Locker l(device); l.locked)
        {
            uint8_t args = PinscapeRequest::SUBCMD_OUTPUT_PWMWORKER_BOOTLOADER;
            bootStat = device->device->SendRequestWithArgs(PinscapeRequest::CMD_OUTPUTS, args);
            if (bootStat != PinscapeResponse::OK)
            {
                hr = E_REBOOT_ERROR;
                return;
            }
        }

        // wait for the new Boot Loader drive to appear
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

        // Copy the firmware to the Boot Loader drive
        dlg->SetBannerText(_T("Sending firmware to Worker Pico"));
        hr = RP2BootDevice::InstallFirmware(filename.c_str(), bootPath.c_str(), dlg);
    });
    dlg.SetCaption(_T("Installing Firmware Update"));
    dlg.SetBannerText(_T("Entering Boot Loader Mode"));
    dlg.Show(IDD_FLASH_COPY_PROGRESS, GetDialogOwner());

    // announce the result
    if (SUCCEEDED(hr))
    {
        // acknowledge the successful installation
        Dialog dlg(hInstance);
        int btn = dlg.Show(IDD_WORKER_UPDATE_SUCCESS, GetDialogOwner());
        if (btn == IDOK)
            PostMessage(GetParent(hwnd), WM_COMMAND, ID_DEVICE_REBOOT, 0);
    }
    else if (hr == E_REBOOT_ERROR)
    {
        MessageBoxFmt(GetDialogOwner(), "Error rebooting the Pico into Boot Loader mode: %s (code %d)",
            VendorInterface::ErrorText(bootStat), bootStat);
    }
    else if (hr == E_AMBIGUOUS_DRIVE)
    {
        // E_AMBIGUOUS_DRIVE is our custom error code for multiple drives appearing at once
        MessageBoxFmt(GetDialogOwner(), "Error: multiple new Boot Loader drives were detected. "
            "The Config Tool can't determine which one corresponds to the original device. "
            "Please either perform the installation manually, or reset the Pico manually "
            "and try again.");
    }
    else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        // other error (besides "user canceled", which requires no explanation)
        MessageBoxFmt(GetDialogOwner(), "Error updating firmware (error code %08lx)", static_cast<unsigned long>(hr));
    }
}


// --------------------------------------------------------------------------
//
// Updater thread
//

// Destroy updater thread
OutputTesterWin::UpdaterThread::~UpdaterThread()
{
    // before exiting, turn off test mode if it's active
    if (uiTestMode)
    {
        if (VendorInterface::Shared::Locker l(device); l.locked)
            device->device->SetOutputTestMode(false, 0);
    }
}

// Updater thread main entrypoint
bool OutputTesterWin::UpdaterThread::Update(bool &releasedMutex)
{
	// update logical port levels
	bool testMode;
	std::vector<PinscapePico::OutputLevel> portLevels;
	bool ok = (device->device->QueryLogicalOutputLevels(testMode, portLevels) == PinscapeResponse::OK);

	// update device port levels
	std::vector<PinscapePico::OutputDevLevel> devLevels;
	ok = ok && (device->device->QueryPhysicalOutputDeviceLevels(devLevels) == PinscapeResponse::OK);

	// if the queries succeeded, update our internal records
	if (ok)
	{
		// success - update the data in the context, holding the mutex while updating
		if (WaitForSingleObject(dataMutex, 100) == WAIT_OBJECT_0)
		{
            // update our saved copies of the port and device arrays
            this->devTestMode = testMode;
            this->portLevels = portLevels;
            this->devLevels = devLevels;

            // Check if we need to send a test mode command.  We need to send
            // a command if the test mode has changed, or it's time for the
            // next timeout extension.  The timeout extension is only necessary
            // when test mode is active.
            UINT64 now = GetTickCount64();
            bool newTestMode = uiTestMode;
            if (newTestMode != devTestMode
                || (devTestMode && now >= nextTestModeCmdTime))
            {
                // update test mode
                int timeout = 2500;
                device->device->SetOutputTestMode(newTestMode, timeout);

                // set the next update time, to extend the timeout
                nextTestModeCmdTime = now + timeout/2;

                // clear requests that are inconsistent with the new mode
                devTestMode ? portLevelChanges.clear() : portLevelChanges.clear();
            }

            // send any pending logical port level changes
            for (auto &it : portLevelChanges)
                device->device->SetLogicalOutputPortLevel(it.first + 1, it.second);

            // clear the pending requests
            portLevelChanges.clear();

            // send any pending direct device port level changes
            for (auto &it : devLevelChanges)
            {
                const auto &s = it.second;
                device->device->SetPhysicalOutputPortLevel(s.devType, s.configIndex, s.port, s.level);
            }

            // clear pending requests
            devLevelChanges.clear();

            // done accessing the shared data
            ReleaseMutex(dataMutex);

			// let the main thread know about the update
			PostMessage(hwnd, DeviceThreadWindow::MSG_NEW_DATA, 0, 0);
		}
	}

    // return the result
	return ok;
}
