// Pinscape Pico - Config Tool - "No Device" window
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
#include "resource.h"
#include "WinUtil.h"
#include "NoDeviceWin.h"

using namespace PinscapePico;

// Paint off-screen
void NoDeviceWin::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // fill the background
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xF0F0F0)));

    // draw a message centered in the window
    static const char *msg1 = "No device is currently selected.";
    static const char *msg2 = "Please select a device to use this tool.";
    
    SIZE sz = hdc.MeasureText(boldFont, msg1);
    int xc = (crc.left + crc.right)/2;
    int yc = (crc.top + crc.bottom)/2;
    hdc.DrawText(xc - sz.cx/2, yc - sz.cy, 1, boldFont, HRGB(0x606060), msg1);

    sz = hdc.MeasureText(boldFont, msg2);
    hdc.DrawText(xc - sz.cx/2, yc, 1, boldFont, HRGB(0x606060), msg2);
}

