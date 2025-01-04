// Pinscape Pico - Config Tool - New Device Setup window
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
#include "SetUpNewDeviceWin.h"
#include "resource.h"

using namespace PinscapePico;

SetUpNewDeviceWin::SetUpNewDeviceWin(HINSTANCE hInstance) : BaseWindow(hInstance)
{
    // load the bitmap
    bmpBootselButton = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_BOOTSELBUTTON));
    BITMAP bmp;
    GetObject(bmpBootselButton, sizeof(bmp), &bmp);
    szBootselButton.cx = bmp.bmWidth;
    szBootselButton.cy = bmp.bmHeight;
}


// Paint off-screen
void SetUpNewDeviceWin::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // fill the background
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xffffff)));

    // set up a DC for copying bitmaps
    CompatibleDC bdc(hdc);

    // show the instructions
    int x0 = 16, x = x0;
    int y0 = 16, y = y0;
    y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "How to set up a new Pico with Pinscape").cy + 16;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "1. Unplug the Pico's USB cable (and any other power input)").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "2. PRESS AND HOLD the BOOTSEL button").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "3. While still pressing BOOTSEL, plug in the USB cable").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "4. Release BOOTSEL").cy + 16;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "The Pico should now appear as a Boot Loader drive in the list at left.").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Click the drive button to proceed with firmware installation.").cy + 32;

    // show the diagram
    bdc.Select(bmpBootselButton);
    BitBlt(hdc, x, y, szBootselButton.cx, szBootselButton.cy,
        bdc, 0, 0, SRCCOPY);
    y += szBootselButton.cy + 32;

    // show some extra instructions for expansion boards
    y = y0;
    x = x0 + mainFontMetrics.tmAveCharWidth * 65;
    y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Expansion Board Users").cy + 16;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "If you're using the Pico with an expansion board that features").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "a RESET button, you don't have to unplug the USB cable:").cy + 16;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "1. PRESS AND HOLD the BOOTSEL button").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "2. While still pressing BOOTSEL, press and release RESET").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "3. Release BOOTSEL").cy + 16;
}

