// Pinscape Pico - Config Tool - Device Overview window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <list>
#include <string>
#include <memory>
#include <Windows.h>
#include <windowsx.h>
#include "WinUtil.h"
#include "Utilities.h"
#include "WrapperWin.h"
#include "BaseDeviceWindow.h"
#include "DeviceOverviewWin.h"
#include "resource.h"

using namespace PinscapePico;

DeviceOverviewWin::DeviceOverviewWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
    BaseDeviceWindow(hInstance, device)
{
    // load bitmaps
    bmpBootselButton = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_BOOTSELBUTTON));
    BITMAP bmp{ 0 };
    GetObject(bmpBootselButton, sizeof(bmp), &bmp);
    szBootselButton.cx = bmp.bmWidth;
    szBootselButton.cy = bmp.bmHeight;

    bmpPicoDiagram = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_PICO_DIAGRAM));
    bmpPicoDiagramHot = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_PICO_DIAGRAM_HILITED));
    GetObject(bmpPicoDiagram, sizeof(bmp), &bmp);
    szPicoDiagram.cx = bmp.bmWidth;
    szPicoDiagram.cy = bmp.bmHeight;

    // Load my menu bar and accelerator
    hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_OVERVIEWWIN));
    hMenuCtx = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_OVERVIEWWIN));
    hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_DEVICEWIN));

    // query the GPIO configuration and output port configuration
    QueryGPIOConfig();
    QueryOutputPortConfig();
}

DeviceOverviewWin::~DeviceOverviewWin()
{
    dropTarget->Detach();
    dropTarget->Release();
}

bool DeviceOverviewWin::InstallParentMenuBar(HWND hwndContainer)
{
    SetMenu(hwndContainer, hMenuBar);
    return true;
}

// translate keyboard accelerators
bool DeviceOverviewWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

// Paint off-screen
void DeviceOverviewWin::PaintOffScreen(HDC hdc0)
{
    // periodically update the device ID information
    UINT64 now = GetTickCount64();
    if (now >= nextUpdateTime)
    {
        // try querying the device ID information
        if (VendorInterface::Shared::Locker l(device); l.locked)
        {
            // check the connection
            if (device->device->Ping() == PinscapeResponse::OK)
            {
                // online - fetch ID and statistics information
                online = true;
                device->device->QueryID(devID);
                device->device->QueryVersion(devVersion);
                device->device->QueryStats(&devStats, sizeof(devStats), true);
                device->device->QueryUSBInterfaceConfig(&usbIfcs, sizeof(usbIfcs));
                device->device->QueryConfigFileExists(PinscapeRequest::CONFIG_FILE_MAIN, hasConfig);

                if ((usbIfcs.flags & usbIfcs.F_CDC_CONF) == 0 || !device->device->GetCDCPort(cdcPortName))
                    cdcPortName = _T("Unknown COM Port");
            }
            else
            {
                // ping failed - device is offline
                online = false;
            }
        }

        // set the next update time
        nextUpdateTime = now + 2000;
    }

    // update GPIO states if applicable
    if (now >= nextGpioTime)
    {
        // get the new states
        if (VendorInterface::Shared::Locker l(device); l.locked)
            device->device->QueryButtonGPIOStates(gpioStates);

        // set the next update time; update frequently to show live status
        nextGpioTime = now + 100;
    }
    
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // update button layout
    int cxUpdateButton = 100;
    int cyUpdateButton = 60;

    // figure the left/right division - stats on the left, pinout diagram on the right
    int cxLeftSide = max(mainFontMetrics.tmAveCharWidth*80, 16 + cxUpdateButton + mainFontMetrics.tmAveCharWidth*24);
    int xPinoutDiagram = cxLeftSide + mainFontMetrics.tmAveCharWidth*24;

    // fill the background
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xffffff)));

    // set up a DC for copying bitmaps
    CompatibleDC bdc(hdc);

    // Check for a fresh configuration
    int x = 16;
    int y = 16;
    if (online && !hasConfig)
    {
        // draw a highlight box, and clip to it
        RECT rc{ x, y, x + cxLeftSide - mainFontMetrics.tmAveCharWidth*8, y + mainFontMetrics.tmHeight*8 };
        FillRect(hdc, &rc, HBrush(HRGB(0x008000)));
        IntersectClipRect(hdc, rc.left, rc.top, rc.right, rc.bottom);

        // explain
        int x1 = x + mainFontMetrics.tmHeight;
        int y1 = y + mainFontMetrics.tmHeight;
        y1 += hdc.DrawText(x1, y1, 1, boldFont, HRGB(0xffffff), "Factory default settings in effect").cy + 8;
        y1 += hdc.DrawText(x1, y1, 1, mainFont, HRGB(0xffffff), "This device is brand new or has been reset to factory settings.  No").cy;
        y1 += hdc.DrawText(x1, y1, 1, mainFont, HRGB(0xffffff), "configuration file is present on the device.  To create a configuration,").cy;
        y1 += hdc.DrawText(x1, y1, 1, mainFont, HRGB(0xffffff), "select the Config tab.").cy;

        y1 += 8;
        const char *helpLabel = "Click here for help with setting up a new device";
        rcHelpNew ={ x1, y1, x1 + hdc.MeasureText(mainFont, helpLabel).cx, y1 + mainFontMetrics.tmHeight };
        RECT rcul = rcHelpNew; rcul.top = rcul.bottom - 1;
        bool hot = IsClientRectHot(rcHelpNew);
        if (hot) FillRect(hdc, &rcHelpNew, HBrush(0xffffff));
        COLORREF txtclr = HRGB(hot ? 0x0080FF : 0xffffff);
        hdc.DrawText(x1, y1, 1, mainFont, txtclr, helpLabel);
        FillRect(hdc, &rcul, HBrush(txtclr));

        // reset clipping, advance vertically
        SelectClipRgn(hdc, NULL);
        y = rc.bottom + 16;
    }
    else
    {
        rcHelpNew ={ 0, 0, 0, 0 };
    }

    // Show the identifiers.  Note that we do this whether online or offline,
    // since we still "remember" the device even when it's offline and can at
    // least show its ID.  The only ID feature that's guaranteed to be stable
    // across resets and reconnects is the hardware ID (that's in ROM in the
    // Pico's soldered-on flash chip, so it's immutable for all practical
    // purposes; you'd have to solder on a new flash chip to change it).
    // But show the other IDs as well, since they're more meaningful to the
    // user, and even if the user reconfigures them at some point, they're
    // still a good immediate reference point.
    y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Pinscape Pico").cy;
    y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Pinscape Unit #%d, %s", devID.unitNum, devID.unitName.c_str()).cy;
    if (devID.ledWizUnitMask == 0)
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Virtual LedWiz disabled").cy;
    else
    {
        std::string lw;
        int nUnitsPopulated = 0;
        int nUnitsNeeded = static_cast<int>((outputPortConfig.size() + 31)/32);
        for (int bit = 0x0001, unit = 1 ; unit <= 16 ; bit <<= 1, ++unit)
        {
            // is it set?
            if ((devID.ledWizUnitMask & bit) != 0)
            {
                // add it to the string
                if (lw.size() != 0) lw += ", ";
                lw += StrPrintf("%d", unit);

                // count the unit; if that satisfies the number of units needed, stop here
                if (++nUnitsPopulated >= nUnitsNeeded)
                    break;
            }
        }
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Virtual LedWiz ID %s", lw.c_str()).cy;
    }
    y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Pinscape firmware version %d.%d.%d, build %s",
        devVersion.major, devVersion.minor, devVersion.patch, devVersion.buildDate).cy;
    y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x00000), "Target board: %s", devID.FriendlyBoardName().c_str()).cy;
    y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Hardware ID: %s", devID.hwid.ToString().c_str()).cy;
    y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "CPU: %s, Version: %d, ROM: %d (%s)",
        devID.cpuType == 2040 ? "RP2040" : devID.cpuType == 2350 ? "RP2350" : "Unknown",
        devID.cpuVersion, devID.romVersion, devID.romVersionName.c_str()).cy;
    y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Build environment: Pico SDK %s, TinyUSB %s, %s", 
        devID.picoSDKVersion.c_str(), devID.tinyusbVersion.c_str(), devID.compilerVersion.c_str()).cy;
    y += 16;

    // show details if online, otherwise just show offline status
    if (online)
    {
        // format a large number with commas
        const static auto FormatNum = [](uint64_t n) {
            char tmp[32];
            char tmp2[64];
            sprintf_s(tmp, "%I64u", n);
            size_t l = strlen(tmp);
            const char *src = tmp;
            char *dst = tmp2;
            for (; dst + 1 < tmp2 + sizeof(tmp2) && *src != 0 ; )
            {
                *dst++ = *src++;
                --l;
                if (l != 0 && (l % 3) == 0)
                    *dst++ = ',';
            }
            *dst = 0;
            return std::string(tmp2);
        };

        // convert the time since reboot into a nice format
        char uptime[256];
        {
            // figure the number of days, if over 24 hours
            char days[32] = "";
            UINT uptime_s = static_cast<UINT>(devStats.upTime / 1000000ULL);
            UINT nDays = uptime_s / (24*60*60);
            if (uptime_s > 24*60*60)
            {
                // format a prefix with the number of days
                sprintf_s(days, "%u day%s, ", nDays, nDays != 1 ? "s" : "");

                // figure the remaining time in excess of whole days
                uptime_s %= (24*60*60);
            }

            // figure the hours, minutes, and seconds
            int hh = uptime_s / (60*60);
            int mm = (uptime_s % (60*60)) / 60;
            int ss = uptime_s % 60;
            auto us = FormatNum(devStats.upTime);

            // show "[n days] hh:mm:ss hours", or "mm:ss minutes", or just "ss seconds"
            if (hh > 0 || nDays > 0)
                sprintf_s(uptime, "%s us (%s%u:%02u:%02u hours)", us.c_str(), days, hh, mm, ss);
            else if (mm > 0)
                sprintf_s(uptime, "%s us (%u:%02u minutes)", us.c_str(), mm, ss);
            else
                sprintf_s(uptime, "%s us (%u second%s)", us.c_str(), ss, ss != 1 ? "s" : "");
        }

        // uptime
        y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Uptime").cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Time since Pico reset: %s", uptime).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Main loop iterations since reset: %s", FormatNum(devStats.nLoopsEver).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Secondary core loops since reset: %s", FormatNum(devStats.nLoopsEver2).c_str()).cy + 8;

        // USB interfaces
        y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "USB Interfaces").cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "USB device identification: VID %04X, PID %04X", usbIfcs.vid, usbIfcs.pid).cy;
        auto ShowUSB =[this, &hdc, x0 = x, &y](const char *name, uint32_t fConf, uint32_t fEna, std::function<void(int x)> extraInfo = [](int) { })
        {
            int x = x0;
            x += hdc.DrawTextF(x0, y, 1, mainFont, HRGB(0x000000), "%s: ", name).cx;
            if ((usbIfcs.flags & fConf) != 0)
            {
                // it's configured
                x += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Configured").cx;

                // show reporting status, if it's configurable
                if (fEna != 0)
                    x += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "; Reporting %s",
                        (usbIfcs.flags & fEna) != 0 ? "Enabled" : "Disabled").cx;

                // show any extra per-type info
                extraInfo(x);
            }
            else
            {
                // not configured
                hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Not Configured").cy;
            }
            y += mainFontMetrics.tmHeight;
        };
        y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Pinscape Vendor Interface: Configured (always present)").cy;
        y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Pinscape Feedback Controller: Configured (always present)").cy;
        ShowUSB("Virtual COM port (CDC)", usbIfcs.F_CDC_CONF, 0, [this, y, &hdc](int x) {
            hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), " (%" _TSFMT ")", cdcPortName.c_str());
        });
        ShowUSB("Keyboard", usbIfcs.F_KEYBOARD_CONF, usbIfcs.F_KEYBOARD_ENA);
        ShowUSB("Gamepad", usbIfcs.F_GAMEPAD_CONF, usbIfcs.F_GAMEPAD_ENA);
        ShowUSB("Xbox Controller", usbIfcs.F_XINPUT_CONF, usbIfcs.F_XINPUT_ENA, [this, y, &hdc](int x) {
            hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "; Player #%d", devID.xinputPlayerIndex + 1);
        });
        ShowUSB("Open Pinball Device", usbIfcs.F_PINDEV_CONF, usbIfcs.F_PINDEV_ENA);
        y += 16;

        // statistics
        y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Recent main loop counters").cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Iterations (recent rolling window): %s", FormatNum(devStats.nLoops).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Avg main loop time: %u us", devStats.avgLoopTime).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Max main loop time: %u us", devStats.maxLoopTime).cy + 8;

        y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Recent secondary core loop counters").cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Iterations (recent rolling window): %s", FormatNum(devStats.nLoops2).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Avg main loop time: %u us", devStats.avgLoopTime2).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Max main loop time: %u us", devStats.maxLoopTime2).cy + 8;

        y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Memory").cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Heap memory size: %s bytes", FormatNum(devStats.heapSize).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Heap unused: %s bytes", FormatNum(devStats.heapUnused).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Malloc arena size: %s", FormatNum(devStats.arenaSize).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Arena in use: %s", FormatNum(devStats.arenaAlloc).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Arena unused: %s", FormatNum(devStats.arenaFree).c_str()).cy;
        y += hdc.DrawTextF(x, y, 1, mainFont, HRGB(0x000000), "Total free memory: %s", FormatNum(devStats.arenaFree + devStats.heapUnused).c_str()).cy;

        //
        // Drop/Select firmware update button
        //

        // set up the layout
        const char *label1 = "Update Firmware";
        const char *label2 = "Drop a .UF2 file here or click to select a file";
        SIZE sz = hdc.MeasureText(mainFont, label2);
        y += 40;
        RECT brc{ x, y, x + sz.cx + cxUpdateButton, y + sz.cy*3/2 + cyUpdateButton };
        rcFirmwareInstallButton = brc;
        bool hot = IsClientRectHot(brc) && (!dropTarget->IsDragActive() || dropTarget->IsTargetHot());

        // draw the rounded rect
        HBrush br(hot ? HRGB(0xd0ffff) : HRGB(0xf0f0f0));
        HPen pen(hot ? HRGB(0xff00ff) : HRGB(0x808080));
        HBRUSH oldbr = SelectBrush(hdc, br);
        HPEN oldpen = SelectPen(hdc, pen);
        RoundRect(hdc, brc.left, brc.top, brc.right, brc.bottom, 24, 24);
        SelectBrush(hdc, oldbr);
        SelectPen(hdc, oldpen);

        // draw the label
        COLORREF txtColor = (hot ? HRGB(0xa000a0) : HRGB(0x000000));
        sz = hdc.MeasureText(mainFont, label1);
        int yTxt = (brc.top + brc.bottom - sz.cy*2)/2;
        yTxt += hdc.DrawText((brc.left + brc.right - sz.cx)/2, yTxt, 1, boldFont, txtColor, label1).cy;
        sz = hdc.MeasureText(mainFont, label2);
        yTxt += hdc.DrawText((brc.left + brc.right - sz.cx)/2, yTxt, 1, mainFont, txtColor, label2).cy;

        //
        // Pinout diagram
        //

        // figure the layout location
        int x = xPinoutDiagram;
        int y = 32;
        rcPicoDiagram ={ x, y, x + szPicoDiagram.cx, y + szPicoDiagram.cy };

        // the image is hot if we're dragging a file over it
        bool pdhot = IsClientRectHot(rcPicoDiagram) && dropTarget->IsTargetHot();

        // paint it
        PaintPinoutDiagram(hdc, xPinoutDiagram, y, pdhot, false, false);
    }
    else
    {
        // device is offline
        y += hdc.DrawText(x, y, 1, boldFont, HRGB(0xA00000), "This Pinscape unit is currently offline").cy;
        y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "The Pico is either disconnected from USB or is in Boot Loader mode.").cy;
        y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "To restore the connection, plug it back into USB, or reset the Pico by").cy;
        y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "unplugging the USB cable (and any power inputs) and plugging it back in.").cy;
    }
}

static const int PinoutDiagramLabelPadding = 6;
int DeviceOverviewWin::CalcPinoutXMargin(HDCHelper &hdc)
{
    // figure the margin requirements based on the widest pin label
    int xMargin = 0;
    for (auto &gp : gpio)
    {
        // figure the width of the label plus the label padding
        int cx = hdc.MeasureText(boldFont, gp.usage.c_str()).cx + PinoutDiagramLabelPadding;

        // keep the widest result so far
        xMargin = max(cx, xMargin);
    }

    // return the result
    return xMargin;
}

void DeviceOverviewWin::PaintPinoutDiagram(HDCHelper &hdc, int xOffset, int yOffset, 
    bool hot, bool forPrinter, bool monochrome)
{
    //
    // Pico pinout diagram
    //

    int x = xOffset;
    int y = yOffset;

    // draw the image diagram
    CompatibleDC bdc(hdc);
    bdc.Select(hot ? bmpPicoDiagramHot : bmpPicoDiagram);
    BitBlt(hdc, x, y, szPicoDiagram.cx, szPicoDiagram.cy, bdc, 0, 0, SRCCOPY);

    // GPIO y locations on the diagram
    const static int yGPIO[] ={
        40,  73,  138, 171, 204, 237, 302, 336,  // left side, GP0-GP7
        368, 401, 466, 499, 532, 565, 630, 663,  // left side, GP8-GP15
        662, 629, 563, 531, 498, 465, 400, -1,   // right side, GP16-GP23 (no pin for GP23)
        -1,  -1,  334, 301, 236, -1,  -1,  -1,   // right side, GP24-GP31 (no pins for GP24, GP25, GP29, GP30, GP31)
    };

    // label the pins with assigned functions
    const auto *gp = &gpio[0];
    int cxLabelMargin = 0;
    int xLabel = x - cxLabelMargin;
    int yLabel = y - mainFontMetrics.tmHeight/2;
    int xPin = x;
    int yPin = y;
    int dirLabel = -1;
    int gpioPinOffset = 66;
    for (size_t i = 0 ; i < _countof(gpio) ; ++i, ++gp)
    {
        // switch to the right side at GP16
        if (i == 16)
        {
            xLabel = x + szPicoDiagram.cx + cxLabelMargin;
            xPin = x + szPicoDiagram.cx;
            dirLabel = 1;
            gpioPinOffset = -gpioPinOffset;
        }

        // skip unexposed GP's
        if (yGPIO[i] < 0)
            continue;

        // figure the color scheme, based on the pin function
        COLORREF bg = HRGB(0x7ac943);
        COLORREF fg = HRGB(0xFFFFFF);
        using Func = VendorInterface::GPIOConfig::Func;
        switch (gp->func)
        {
        case Func::I2C: bg = HRGB(0x1d6089); break;
        case Func::SPI: bg = HRGB(0xa22c6f); break;
        case Func::UART: bg = HRGB(0x613da5); break;
        case Func::PWM: bg = HRGB(0xc65800); break;
        case Func::PIO0:
        case Func::PIO1: bg = HRGB(0xaa0004); break;
        case Func::SIO: bg = HRGB(0x3d9800); break;
        case Func::GPCK: bg = HRGB(0x404040); break;
        case Func::USB: bg = HRGB(0x606060); break;
        case Func::NONE: bg = HRGB(0x808080); break;
        }

        // if it's for the printer, adjust the label colors
        if (forPrinter)
        {
            if (monochrome)
            {
                // monochrome - always use black text on white background
                fg = 0x000000;
                bg = 0xffffff;
            }
            else
            {
                // full-color printer - draw opaque instead of using a fill rect,
                // since printers usually intentionally remove background fill
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, bg);
            }
        }

        // draw this item
        if (gp->usage.size() != 0)
        {
            // draw the background rect
            int y = yLabel + yGPIO[i];
            int x1 = xLabel;
            int x2 = xLabel + dirLabel*(PinoutDiagramLabelPadding + hdc.MeasureText(boldFont, gp->usage.c_str()).cx);
            RECT rc{ min(x1, x2), y, max(x1, x2), y + boldFontMetrics.tmHeight };
            InflateRect(&rc, 0, 2);
            FillRect(hdc, &rc, HBrush(bg));

            // draw the text
            hdc.DrawText(xLabel + dirLabel*3, y, dirLabel, boldFont, fg, gp->usage.c_str());
        }

        // draw the state, if not printing
        if (!forPrinter && i < gpioStates.size())
        {
            POINT pt{ xPin + gpioPinOffset, yPin + yGPIO[i] };
            HBrush br(gpioStates[i] != 0 ? HRGB(0x00ff00) : HRGB(0x600000));
            HBRUSH oldBr = SelectBrush(hdc, br);
            HPEN oldPen = SelectPen(hdc, NULL_PEN);
            Ellipse(hdc, pt.x - 8, pt.y - 8, pt.x + 8, pt.y + 8);
                
            SelectBrush(hdc, oldBr);
            SelectPen(hdc, oldPen);
        }
    }

    // restore transparent text drawing
    SetBkMode(hdc, TRANSPARENT);
}

LRESULT DeviceOverviewWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case MSG_INSTALL_FIRMWARE:
        // deferred firmware install from drag/drop
        WrapperWin::InstallFirmware(hInstance, GetDialogOwner(), device, pendingFirmwareInstall.c_str());
        pendingFirmwareInstall.clear();
        return 0;
    }

    // use the default handling
    return __super::WndProc(msg, wparam, lparam);
}


void DeviceOverviewWin::OnCreateWindow()
{
    // do the base class work
    __super::OnCreateWindow();

    // register as a drop target
    RegisterDragDrop(hwnd, dropTarget);
}

void DeviceOverviewWin::OnDestroy()
{
    // remove the drop target registration
    RevokeDragDrop(hwnd);

    // do the base class work
    __super::OnDestroy();
}

bool DeviceOverviewWin::OnDeviceReconnect()
{
    // re-query the GPIO port configuration
    QueryGPIOConfig();

    // re-query the output port configuration
    QueryOutputPortConfig();

    // keep the window open
    return true;
}

void DeviceOverviewWin::OnEraseDeviceConfig(bool factoryReset)
{
    // re-query the GPIO port configuration
    QueryGPIOConfig();
}


void DeviceOverviewWin::QueryGPIOConfig()
{
    if (VendorInterface::Shared::Locker l(device); l.locked)
        device->device->QueryGPIOConfig(gpio);
}

void DeviceOverviewWin::QueryOutputPortConfig()
{
    if (VendorInterface::Shared::Locker l(device); l.locked)
        device->device->QueryLogicalOutputPortConfig(outputPortConfig);
}

bool DeviceOverviewWin::OnLButtonDown(WPARAM keys, int x, int y)
{
    // check for a click in the firmware install button
    POINT pt{ x, y };
    if (PtInRect(&rcFirmwareInstallButton, pt))
    {
        // this is a device-level command handled in the wrapper window
        PostMessage(GetParent(hwnd), WM_COMMAND, ID_DEVICE_INSTALLFIRMWARE, 0);
        return true;
    }

    // check for a click on the new-device-help button
    if (PtInRect(&rcHelpNew, pt))
    {
        ShowHelpFile("NewDeviceSetup.htm");
        return true;
    }

    // inherit the base handling
    return __super::OnLButtonDown(keys, x, y);
}

bool DeviceOverviewWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (notifyCode)
    {
    case 0:
    case 1:
        // menu/accelerator command/button click
        switch (ctlCmdId)
        {
        case ID_HELP_HELP:
            // note - this applies when we're running as a child window under a
            // container app like the Config Tool, where the parent frame window
            // provides a standard Help menu with this item
            ShowHelpFile("DeviceOverview.htm");
            return true;

        case ID_EDIT_COPY:
            // Copy the pinout diagram to the clipboard
            CopyPinoutDiagram();
            return true;
        }
        break;
    }

    // use the default handling
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

void DeviceOverviewWin::CopyPinoutDiagram()
{
    // set up an off-screen drawing context
    WindowDC wdc(hwnd);
    CompatibleDC hdc0(wdc);
    HDCHelper hdc(hdc0);

    // figure out how much space we need for the label margins
    int xMargin = CalcPinoutXMargin(hdc) + 16;
    int yMargin = mainFontMetrics.tmHeight;

    // create a bitmap big enough to cover the Pico diagram plus pin labels
    RECT rcBmp{ 0, 0, szPicoDiagram.cx + xMargin*2, szPicoDiagram.cy + yMargin*2 };
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rcBmp.right, rcBmp.bottom);
    hdc0.Select(bmp);

    // initialize the DC with default GDI settings and objects
    HBRUSH oldBrush = SelectBrush(hdc, GetStockBrush(WHITE_BRUSH));
    HPEN oldPen = SelectPen(hdc, GetStockPen(NULL_PEN));
    HFONT oldFont = SelectFont(hdc, mainFont);
    COLORREF oldTxColor = SetTextColor(hdc, RGB(0, 0, 0));
    COLORREF oldBkColor = SetBkColor(hdc, RGB(255, 255, 255));
    int oldBkMode = SetBkMode(hdc, TRANSPARENT);

    // paint the diagram
    FillRect(hdc, &rcBmp, GetStockBrush(WHITE_BRUSH));
    PaintPinoutDiagram(hdc, xMargin, yMargin, false, false, false);

    // restore DC properties and release the window DC
    SelectFont(hdc, oldFont);
    SetBkMode(hdc, oldBkMode);
    SetBkColor(hdc, oldBkColor);
    SetTextColor(hdc, oldTxColor);
    SelectPen(hdc, oldPen);
    SelectBrush(hdc, oldBrush);

    // make sure drawing operations are completed
    GdiFlush();
    hdc0.Reset();

    // acquire clipboard access
    if (OpenClipboard(hwnd))
    {
        // clear the clipboard
        EmptyClipboard();

        // put the extracted image on the clipboard
        SetClipboardData(CF_BITMAP, bmp);

        // release the clipboard
        CloseClipboard();
    }

    // discard the temporary bitmaps
    DeleteBitmap(bmp);
}

bool DeviceOverviewWin::PrintPageContents(HDCHelper &hdc, int pageNum, bool skip)
{
    if (!skip)
    {
        // set the window so that the diagram fills 3/4 of the height
        int pinoutMargin = CalcPinoutXMargin(hdc);
        int cx = szPicoDiagram.cx + pinoutMargin * 2;
        int cy = szPicoDiagram.cy;
        SetMapMode(hdc, MM_ISOTROPIC);
        SetWindowExtEx(hdc, cx, cy, NULL);

        // get the options
        const auto *opts = printingContext.opts.get();

        // set viewport scaling
        auto &rc = printingContext.rcPrint;
        SetViewportExtEx(hdc, rc.right - rc.left, rc.bottom - rc.top, NULL);
        float scale = static_cast<float>(cx) / static_cast<float>((rc.right - rc.left));

        // figure the offsets to center it in the print area
        int xMargin = static_cast<int>(((rc.right + rc.left)*scale - cx))/2;
        int yMargin = static_cast<int>(((rc.bottom + rc.top)*scale - cy))/2;

        // paint the pinout diagram
        PaintPinoutDiagram(hdc, xMargin + pinoutMargin, yMargin, false, true, opts->monochrome);
    }

    // there's only one page, so we're done now
    return false;
}

bool DeviceOverviewWin::ExpandHeaderFooterVar(const std::string &varName, std::string &expansion)
{
    auto FromInt = [](int n) {
        char buf[32];
        sprintf_s(buf, "%d", n);
        return std::string(buf);
    };
    auto DeviceInfo = [this](std::function<std::string(const DeviceID&)> func)
    {
        DeviceID id;
        if (PinscapePico::VendorInterface::Shared::Locker l(device); l.locked)
            device->device->QueryID(id);
        return func(id);
    };
    if (varName == "unitNum")
        return expansion = DeviceInfo([FromInt](const DeviceID &id) { return FromInt(id.unitNum); }), true;
    if (varName == "unitName")
        return expansion = DeviceInfo([](const DeviceID &id) { return id.unitName; }), true;

    // unknown
    return __super::ExpandHeaderFooterVar(varName, expansion);
}

bool DeviceOverviewWin::IsFirmwareDropLocation(POINTL ptl)
{
    // adjust to client coordinates
    POINT pt{ ptl.x, ptl.y };
    ScreenToClient(hwnd, &pt);

    // check if it's in the firmware install button or the Pico diagram
    return PtInRect(&rcFirmwareInstallButton, pt) || PtInRect(&rcPicoDiagram, pt);
}

void DeviceOverviewWin::ExecFirmwareDrop(POINTL, const TCHAR *filename)
{
    // execute the install via a posted message, so that we can
    // immediately return from the drag/drop nested event loop
    // (without going another level deeper with the nested event
    // loop for the modal progress dialog)
    pendingFirmwareInstall = filename;
    PostMessage(hwnd, MSG_INSTALL_FIRMWARE, 0, 0);
}
