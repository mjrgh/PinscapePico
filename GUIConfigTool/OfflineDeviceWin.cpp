// Pinscape Pico - Config Tool - Offline Device window
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
#include "OfflineDeviceWin.h"
#include "resource.h"

using namespace PinscapePico;

OfflineDeviceWin::OfflineDeviceWin(HINSTANCE hInstance) : BaseWindow(hInstance)
{
    // load the bitmap
    bmpDisconnect = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_DISCONNECT));
    BITMAP bmp;
    GetObject(bmpDisconnect, sizeof(bmp), &bmp);
    szDisconnect.cx = bmp.bmWidth;
    szDisconnect.cy = bmp.bmHeight;
}

// Paint off-screen
void OfflineDeviceWin::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // fill the background
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xFFFFFF)));

    // show the message
    int x = 16;
    int y = 16;
    y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "This Pinscape unit is currently offline").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "The Pico is either disconnected from USB or is in Boot Loader mode.").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "If it's in Boot Loader mode, you can restart Pinscape by unplugging").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "the USB cable (and any other power source) and plugging it back in.").cy + 80;

    // draw the image
    CompatibleDC bdc(hdc);
    bdc.Select(bmpDisconnect);
    BitBlt(hdc, x, y, szDisconnect.cx, szDisconnect.cy,
        bdc, 0, 0, SRCCOPY);
}
