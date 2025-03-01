// Pinscape Pico - Config Tool About Box
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
#include <shlwapi.h>
#include "WinUtil.h"
#include "VersionNumber.h"
#include "AboutBox.h"
#include "resource.h"

using namespace PinscapePico;

AboutBox::AboutBox(HINSTANCE hInstance) : BaseWindow(hInstance)
{
    // load the bitmap
    bmpLogo = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_LOGO));
    BITMAP bmp;
    GetObject(bmpLogo, sizeof(bmp), &bmp);
    szLogo.cx = bmp.bmWidth;
    szLogo.cy = bmp.bmHeight;
}

// control IDs
static const int ID_BTN_OK = 101;
static const int ID_EDITBOX = 102;

void AboutBox::OnCreateWindow()
{
    // do the base class work
    __super::OnCreateWindow();

    // get the window DC
    HDC hdc = GetWindowDC(hwnd);

    // load fonts
    titleFont = CreateFontA(
        -MulDiv(16, GetDeviceCaps(hdc, LOGPIXELSY), 72),
        0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
        "Segoe UI");

    // done with the DC
    ReleaseDC(hwnd, hdc);

    // get the client rect
    RECT crc;
    GetClientRect(hwnd, &crc);

    // OK button
    okBtn = CreateControl(ID_BTN_OK, WC_BUTTONA, "OK", WS_CHILD | WS_VISIBLE | BS_FLAT, 50, 16);
    RECT brc;
    GetWindowRect(okBtn, &brc);
    SIZE bsz{ brc.right - brc.left, brc.bottom - brc.top };
    int yOK = crc.bottom - bsz.cy - 8;
    SetWindowPos(okBtn, NULL, (crc.right + crc.left - bsz.cx)/2, yOK,
        -1, -1, SWP_NOZORDER | SWP_NOSIZE);

    // edit box text
    const char *editBoxText =
        "This program incorporates the following third-party open-source components:\r\n"
        "\r\n"
        "CRC++, Copyright(c) 2022, Daniel Bahr. "
        "Distributed under a BSD 3-clause license.\r\n"
        "\r\n"
        "Scintilla Edit Control, Copyright 1998-2021 by Neil Hodgson "
        "<neilh@scintilla.org>.  Distributed under a Python-like license.\r\n"
        "\r\n";

    // edit box
    editBox = CreateControl(ID_EDITBOX, WC_EDITA, editBoxText,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        100, 10);
    int cyEdit = 50;
    int yEdit = yOK - cyEdit - 8;
    SetWindowPos(editBox, NULL, crc.left, yEdit, crc.right - crc.left, cyEdit, SWP_NOZORDER);
    SendMessage(editBox, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(16, 16));
}

AboutBox::~AboutBox()
{
    // delete fonts
    DeleteFont(titleFont);
}

// Paint off-screen
void AboutBox::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // fill the background
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xFFFFFF)));

    // DC for bitmap drawing
    CompatibleDC bdc(hdc);

    // draw the logo, filling the top rect to the full width and centering
    // the logo horizontally
    RECT lrc{ crc.left, crc.top, crc.right, crc.top + szLogo.cy };
    FillRect(hdc, &lrc, HBrush(HRGB(0x800080)));
    bdc.Select(bmpLogo);
    BitBlt(hdc, (crc.left + crc.right - szLogo.cx)/2, 0, 
        (crc.left + crc.right - szLogo.cx)/2 + szLogo.cx, szLogo.cy,
        bdc, 0, 0, SRCCOPY);

    // draw the text
    int x = 16;
    int y = szLogo.cy + 16;
    y += hdc.DrawText(x, y, 1, titleFont, HRGB(0x800080), "Pinscape Pico Config Tool").cy + 4;
    y += hdc.DrawTextF(x, y, 1, boldFont, HRGB(0x000000), "Version %s | Build %s", gVersionString, GetBuildTimestamp()).cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Copyright 2024, 2025 Michael Roberts").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Released under an open source license (BSD-3-Clause) | NO WARRANTY").cy;

    // the line about the license text is a hot spot
    const char *licenseTxt = "See the accompanying License.txt file for the full text.";
    SIZE szLicenseTxt = hdc.MeasureText(mainFont, licenseTxt);
    rcLicenseTxt = { x, y, x + szLicenseTxt.cx, y + szLicenseTxt.cy };
    bool hot = IsClientRectHot(rcLicenseTxt);
    y += hdc.DrawText(x, y, 1, mainFont, hot ? HRGB(0x0000FF) : HRGB(0x000000), licenseTxt).cy;
    if (hot)
    {
        RECT rcu = rcLicenseTxt;
        rcu.top = rcu.bottom - 1;
        FillRect(hdc, &rcu, HBrush(HRGB(0x0000ff)));
    }

    // separator line
    y += 16;
    HBrush lnbr(HRGB(0xf0f0f0));
    RECT lnrc{ crc.left, y, crc.right, y + 1 };
    FillRect(hdc, &lnrc, lnbr);
    y += 1;

    // OK button bar at the bottom
    RECT brc = GetChildControlRect(okBtn);
    RECT bbrc{ crc.left, brc.top - 8, crc.right, crc.bottom };
    FillRect(hdc, &bbrc, HBrush(HRGB(0xffffff)));

    // separator at top of bar
    lnrc.top = bbrc.top;
    lnrc.bottom = bbrc.top + 1;
    FillRect(hdc, &lnrc, lnbr);

    // position the edit box if we haven't already
    if (!editBoxPositioned)
    {
        editBoxPositioned = true;
        SetWindowPos(editBox, NULL, crc.left, y, crc.right - crc.left, bbrc.top - y, SWP_NOZORDER);
    }
}

bool AboutBox::OnCtlColor(UINT msg, HDC hdc, HWND hwndCtl, HBRUSH &hbrush)
{
    // The edit box is in read-only mode, which makes it ask for static text
    // colors.  Use black text on a white background instead of the "disabled"
    // looking gray background that the system control uses by default, so
    // that it harmonizes better with the rest of the window graphics.
    if (msg == WM_CTLCOLORSTATIC && hwndCtl == editBox)
    {
        SetBkColor(hdc, HRGB(0xffffff));
        hbrush = GetStockBrush(WHITE_BRUSH);
        return true;
    }

    // use the default handling for anything else
    return __super::OnCtlColor(msg, hdc, hwndCtl, hbrush);
}

bool AboutBox::OnLButtonDown(WPARAM keys, int x, int y)
{
    // check for a click on the license text hotspot
    POINT pt{ x, y };
    if (PtInRect(&rcLicenseTxt, pt))
    {
        // open the license file
        char path[MAX_PATH];
        GetModuleFileNameA(hInstance, path, _countof(path));
        PathRemoveFileSpecA(path);
        PathAppendA(path, "License.txt");
        ShellExecuteA(hwnd, "open", path, NULL, NULL, SW_SHOW);

        // handled
        return true;
    }

    // use the default handling
    return __super::OnLButtonDown(keys, x, y);
}


bool AboutBox::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    // check the message
    switch (ctlCmdId)
    {
    case ID_BTN_OK:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return true;
    }

    // not handled - use default handling
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

bool AboutBox::OnKeyDown(WPARAM vkey, LPARAM flags)
{
    // dismiss on Return or Escape
    switch (vkey)
    {
    case VK_RETURN:
    case VK_ESCAPE:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return true;
    }

    return __super::OnKeyDown(vkey, flags);
}



