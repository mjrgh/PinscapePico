// Pinscape Pico - Log Viewer Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <list>
#include <iterator>
#include <memory>
#include <algorithm>
#include <regex>
#include <fstream>
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "../Scintilla/include/Scintilla.h"
#include "../Scintilla/include/ILexer.h"
#include "../Firmware/JSON.h"
#include "PinscapePicoAPI.h"
#include "Dialog.h"
#include "WinUtil.h"
#include "Application.h"
#include "JSONExt.h"
#include "LogViewerWin.h"
#include "resource.h"

// Scintilla indicator assignments
static const int INDICATOR_FIND = INDICATOR_CONTAINER + 0;

// the window class is part of the PinscapePico namespace
using namespace PinscapePico;

LogViewerWin::LogViewerWin(HINSTANCE hInstance,  std::shared_ptr<VendorInterface::Shared> &device) :
	DeviceThreadWindow(hInstance, device, new UpdaterThread())
{
    // load bitmaps
    bmpCtlBarButtons = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_EDITTOOLS));
    BITMAP bmp;
    GetObject(bmpCtlBarButtons, sizeof(bmp), &bmp);
    szCtlBarButtons ={ bmp.bmWidth, bmp.bmHeight };

    // set up control bar buttons
    const static CtlBarButton buttons[]
    {
        { -2, -2, nullptr, "Find text in log" }, // Find box
        { ID_EDIT_FINDNEXT, 0, nullptr, "Find Next" },
        { -1 }, // spacer
        { ID_EDIT_CLEARALL, 1, nullptr, "Clear the log text" }
    };
    for (auto &b : buttons)
        ctlBarButtons.emplace_back(b);
}

LogViewerWin::~LogViewerWin()
{
    // clean up resources
    DeleteFont(editorFont);
}

// ANSI color code to RGB
static const std::unordered_map<int, COLORREF> ansiColorMap
{
    // regular foreground colors
    { 30, RGB(0, 0, 0) },
    { 31, RGB(187, 0, 0) },
    { 32, RGB(0, 140, 0) },
    { 33, RGB(187, 187, 0) },
    { 34, RGB(0, 0, 187) },
    { 35, RGB(187, 0, 187) },
    { 36, RGB(0, 140, 180) },
    { 37, RGB(229, 229, 229) },
    { 39, RGB(0, 0, 0) },

    // regular background colors
    { 40, RGB(0, 0, 0) },
    { 41, RGB(187, 0, 0) },
    { 42, RGB(0, 187, 0) },
    { 43, RGB(255, 130, 0) },
    { 44, RGB(0, 0, 187) },
    { 45, RGB(187, 0, 187) },
    { 46, RGB(0, 187, 187) },
    { 47, RGB(229, 229, 229) },
    { 49, RGB(255, 255, 255) },

    // bright foreground colors
    { 90, RGB(85, 85, 85) },
    { 91, RGB(255, 0, 0) },
    { 92, RGB(0, 180, 0) },
    { 93, RGB(255, 130, 0) },
    { 94, RGB(0, 127, 255) },
    { 95, RGB(245, 0, 245) },
    { 96, RGB(0, 245, 245) },
    { 97, RGB(255, 255, 255) },

    // bright foreground colors
    { 100, RGB(85, 85, 85) },
    { 101, RGB(255, 85, 85) },
    { 102, RGB(85, 255, 85) },
    { 103, RGB(255, 255, 85) },
    { 104, RGB(85, 85, 255) },
    { 105, RGB(255, 85, 255) },
    { 106, RGB(85, 255, 255) },
    { 107, RGB(255, 255, 255) },
};

void LogViewerWin::OnCreateWindow()
{
    // do the base class work
    __super::OnCreateWindow();

    // Load my menus and accelerators
    hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_LOGWIN));
    hCtxMenu = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_LOGWIN));
    hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_LOGWIN));

    // load fonts
    const char *editorFontName = "Courier New";
    {
        // get the window DC
        HDC hdc = GetWindowDC(hwnd);

        // create the fonts
        editorFont = CreateFontA(
            -MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
            editorFontName);

        // get metrics
        HFONT oldFont = SelectFont(hdc, mainFont);
        GetTextMetrics(hdc, &tmEditorFont);
        SelectFont(hdc, oldFont);

        // done with the window DC for now
        ReleaseDC(hwnd, hdc);
    }

    // figure the height of the control bar and error panel
    cyControlBar = boldFontMetrics.tmHeight + 6;

    // create the find box: single-line edit control with a cue banner saying "Find..."
    findBox = CreateControl(ID_EDIT_FIND, WC_EDITA, "", 
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        mainFontMetrics.tmAveCharWidth*20, mainFontMetrics.tmHeight + 2);
    SendMessage(findBox, WM_SETFONT, reinterpret_cast<WPARAM>(mainFont), MAKELPARAM(FALSE, 0));
    SendMessage(findBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find..."));

    // if we haven't loaded Scintilla yet, do so now
	static bool scintillaLoaded = false;
	if (!scintillaLoaded)
	{
        // only attempt this once (even if it fails, since it will presumably just 
        // fail again in the same way on repeats - the usual problem is that the
        // DLL file is missing)
        scintillaLoaded = true;
        if (LoadLibraryA("Scintilla.dll") == NULL)
		{
            MessageBoxFmt(hwnd, "An error occurred trying to load the Scintilla text editing component DLL. "
                "You might need to reinstall the Config Tool application files.");
		}
	}

    // create the scintilla window
    RECT crc;
    GetClientRect(hwnd, &crc);
    sciWin = CreateWindowExA(0, "Scintilla", "Config Editor",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
        crc.left, crc.top, crc.right - crc.left, crc.bottom - crc.top,
        hwnd, reinterpret_cast<HMENU>(ID_SCINTILLA), hInstance, NULL);

    // if that succeeded, configure the editor
    if (sciWin != NULL)
    {
        // get the direct-call functions
        sciFunc = reinterpret_cast<ScintillaFunc_t*>(SendMessage(sciWin, SCI_GETDIRECTFUNCTION, 0, 0));
        sciFuncCtx = reinterpret_cast<void*>(SendMessage(sciWin, SCI_GETDIRECTPOINTER, 0, 0));

        // set read-only mode
        CallSci(SCI_SETREADONLY, 1);

        // get rid of the side margins
        CallSci(SCI_SETMARGINWIDTHN, 0, 0);
        CallSci(SCI_SETMARGINWIDTHN, 1, 0);

        // set the default font
        CallSci(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<INT_PTR>(editorFontName));
        CallSci(SCI_STYLESETSIZE, STYLE_DEFAULT, 10);

        // set the default color scheme
        CallSci(SCI_STYLESETFORE, STYLE_DEFAULT, ansiColorMap.at(39));
        CallSci(SCI_STYLESETBACK, STYLE_DEFAULT, ansiColorMap.at(49));
        CallSci(SCI_STYLECLEARALL);

        // set up find indicators
        CallSci(SCI_INDICSETSTYLE, INDICATOR_FIND, INDIC_ROUNDBOX);

        // disable the built-in context menu (so that we can use our custom menu instead)
        CallSci(SCI_USEPOPUP, SC_POPUP_NEVER);
    }
}

// set my menu bar in the host application
bool LogViewerWin::InstallParentMenuBar(HWND hwndContainer)
{
    SetMenu(hwndContainer, hMenuBar);
    return true;
}

// translate keyboard accelerators
bool LogViewerWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

// Window UI activation.  Set focus on the Scintilla control.
void LogViewerWin::OnActivateUI(bool isAppActivate)
{
    SetFocus(sciWin);
}

LRESULT LogViewerWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Process the message
    switch (msg)
    {
    case MSG_NEW_DATA:
        // append new data to the scintilla control
        AppendLog(reinterpret_cast<std::vector<BYTE>*>(lparam));
        return 0;
    }

    // not intercepted - inherit the default handling
    return __super::WndProc(msg, wparam, lparam);
}

void LogViewerWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the layout
	AdjustLayout();

    // do the base class work
    __super::OnSizeWindow(type, width, height);
}

void LogViewerWin::AdjustLayout()
{
    // get the client area
    RECT crc;
    GetClientRect(hwnd, &crc);

    // get the window DC
    HDCHelper hdc(hwnd);

    // start with the scintilla control getting the whole thing, minus the
    // control bar
    RECT src = crc;
    src.top += cyControlBar;

    // set up the buttons
    int cxBtn = szCtlBarButtons.cy;  // not cx - buttons are square, bitmap width is sum of all cells
    int cyBtn = szCtlBarButtons.cy;
    int x = crc.left + 4;
    int y = (crc.top + cyControlBar - cyBtn)/2;
    RECT rcBtn{ x, y, x + cxBtn, y + cyBtn };
    for (auto &b : ctlBarButtons)
    {
        // check special items
        switch (b.cmd)
        {
        case 0:
            // unpopulated
            break;

        case -1:
            // spacer - add padding on either side, and set the button rect to a
            // one-pixel vertical line where we draw a visual separator
            b.rc ={ rcBtn.left + 6, rcBtn.top, rcBtn.left + 7, rcBtn.bottom };
            OffsetRect(&rcBtn, 13, 0);
            break;

        case -2:
            // Find Next box
            {
                // vertically center the text box within the control bar, with a little margin to the left
                POINT ptFindBox{ rcBtn.left + 2, (y + cyControlBar - mainFontMetrics.tmHeight)/2 };
                SIZE szFindBox{ mainFontMetrics.tmAveCharWidth * 32, mainFontMetrics.tmHeight };
                SetWindowPos(findBox, NULL, ptFindBox.x, ptFindBox.y, szFindBox.cx, szFindBox.cy, SWP_NOZORDER);

                // set a tooltip for the edit box's whole client area
                RECT rc;
                GetClientRect(findBox, &rc);
                SetTooltip(rc, ID_EDIT_FIND, b.tooltip, findBox);

                // advance the next control rect
                OffsetRect(&rcBtn, ptFindBox.x + szFindBox.cx - rcBtn.left + 2, 0);
            }
            break;

        default:
            // Regular button - set the button rect
            b.rc = rcBtn;

            // if it has text, add the text size into the button rect
            if (b.label != nullptr)
                b.rc.right += hdc.MeasureText(mainFont, b.label).cx + 8;

            // set the tooltip
            SetTooltip(b.rc, b.cmd, b.tooltip);

            // advance the rect to the next button, if it's not specially positioned
            if (!b.specialPos)
                OffsetRect(&rcBtn, b.rc.right - rcBtn.left, 0);
            break;
        }
    }

    // position the Scintilla window
    SetWindowPos(sciWin, NULL, src.left, src.top, src.right - src.left, src.bottom - src.top, SWP_NOZORDER);
}

void LogViewerWin::UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply)
{
    // enable copy if the selection is non-empty
    INT_PTR s1 = CallSci(SCI_GETSELECTIONSTART);
    INT_PTR s2 = CallSci(SCI_GETSELECTIONEND);
    apply(ID_EDIT_COPY, s1 != s2);

    // enable Find Next if there's a search term to repeat
    apply(ID_EDIT_FINDNEXT, lastSearchTerm.size() != 0);

    // enable Select All and Clear All if there's any text
    INT_PTR len = CallSci(SCI_GETLENGTH);
    apply(ID_EDIT_SELECTALL, len != 0);
    apply(ID_EDIT_CLEARALL, len != 0);
}

void LogViewerWin::SaveToFile(const WCHAR *filename)
{
    // get a pointer to the Scintilla text
    const char *text = reinterpret_cast<const char*>(CallSci(SCI_GETCHARACTERPOINTER));
    INT_PTR len = CallSci(SCI_GETLENGTH);

    // set up the file writer
    std::ofstream f(filename);
    if (f)
        f.write(text, len);
    if (f)
        f.close();

    // report errors
    if (!f)
        MessageBoxFmt(hwnd, "Error saving log to file (%" _TSFMT ")", filename);
}

void LogViewerWin::SaveToFile()
{
    GetFileNameDlg dlg(_T("Save log to file"),
        OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT,
        _T("Text/log files\0*.txt;*.log\0All Files\0*.*"), _T("log"));
    if (dlg.Save(GetDialogOwner()))
        SaveToFile(dlg.GetFilename());
}

bool LogViewerWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (notifyCode)
    {
    case 0:
    case 1:
        // menu command, accelerator, or button click
        switch (ctlCmdId)
        {
        case ID_FILE_SAVETOFILE:
            SaveToFile();
            return true;

        case ID_EDIT_COPY:
            CallSci(SCI_COPY);
            return true;

        case ID_EDIT_SELECTALL:
            CallSci(SCI_SELECTALL);
            return true;

        case ID_EDIT_FIND:
            // restore the previous search term to the find box
            SetWindowTextA(findBox, lastSearchTerm.c_str());
            SendMessage(findBox, EM_SETSEL, 0, lastSearchTerm.size());

            // set focus on the find box
            SetFocus(findBox);
            return true;

        case ID_EDIT_FINDNEXT:
            // If the edit box isn't empty, treat this as a Find, otherwise
            // repeat the last saved search
            {
                char buf[256];
                GetWindowTextA(findBox, buf, _countof(buf));
                if (buf[0] != 0)
                {
                    UpdateSearch(buf);
                    CommitSearch(CommitSearchReason::Accept);
                    lastSearchTerm = buf;
                }
                else if (lastSearchTerm.size() != 0 && UpdateSearch(lastSearchTerm.c_str()))
                {
                    CommitSearch(CommitSearchReason::Accept);
                }
            }
            return true;

        case ID_FILE_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return true;

        case ID_EDIT_CLEARALL:
            // clear the editor window
            CallSci(SCI_SETREADONLY, 0);
            CallSci(SCI_CLEARALL);
            CallSci(SCI_SETREADONLY, 1);
            return true;

        case ID_HELP_HELP:
            // note - this applies when we're running as a child window under a
            // container app like the Config Tool, where the parent frame window
            // provides a standard Help menu with this item
            ShowHelpFile("LogViewer.htm");
            return true;
        }
        break;

    case EN_CHANGE:
        // edit box change
        if (hwndCtl == findBox)
        {
            // get the new search term
            char buf[256];
            GetWindowTextA(findBox, buf, _countof(buf));

            // update the search
            UpdateSearch(buf);
        }
        break;

    case EN_SETFOCUS:
        // open the search
        searchOpen = true;
        break;

    case EN_KILLFOCUS:
        // if the search is still open, end it
        if (searchOpen)
            CommitSearch(CommitSearchReason::KillFocus);
        break;
    }

    // not handled
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

bool LogViewerWin::OnNotify(WPARAM id, NMHDR *nmhdr, LRESULT &lresult)
{
    // skip a newline sequence backwards
    auto SkipNewlineBack = [this](int pos)
    {
        if (pos > 0 && CallSci(SCI_GETCHARAT, pos) == '\n' && CallSci(SCI_GETCHARAT, pos-1) == '\r')
            pos -= 1;
        return pos;
    };

    // indentation increment, in columns
    int indentSize = 4;

    // handle Scintilla notifications
    if (nmhdr->hwndFrom == sciWin)
    {
        auto *nm = reinterpret_cast<SCNotification*>(nmhdr);
        switch (nmhdr->code)
        {
        case SCN_UPDATEUI:
            // UI change - selection position, styling, or contents changed

            // done
            break;
        }
    }

    // inherit default handling
    return __super::OnNotify(id, nmhdr, lresult);
}

LRESULT LogViewerWin::ControlSubclassProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass)
{
    // do some special handling for some of the controls
    if (hwnd == findBox)
    {
        // Find Box
        switch (msg)
        {
        case WM_KEYDOWN:
            switch (wparam)
            {
            case VK_ESCAPE:
                // abort the search and return focus to the Scintilla window
                CommitSearch(CommitSearchReason::Cancel);

                // restore the scroll position
                CallSci(SCI_SCROLLCARET);

                // forget the search term
                lastSearchTerm.clear();
                return true;

            case VK_RETURN:
                // save the search term for next time
                {
                    char buf[256];
                    GetWindowTextA(findBox, buf, _countof(buf));
                    lastSearchTerm = buf;
                }

                // end the search, selecting the result
                CommitSearch(CommitSearchReason::Accept);
                return true;
            }
            break;

        case WM_CHAR:
            // suppress the bell on an escape key press
            if (wparam == 27)
                return true;
            break;
        }
    }

    // use the base class handling
    return __super::ControlSubclassProc(hwnd, msg, wparam, lparam, idSubclass);
}

void LogViewerWin::SetFindHighlight(INT_PTR start, INT_PTR end)
{
    // select the Find indicator
    CallSci(SCI_SETINDICATORCURRENT, INDICATOR_FIND);

    // remove any existing range
    if (findStart != findEnd)
        CallSci(SCI_INDICATORCLEARRANGE, findStart, findEnd - findStart);

    // fill the new range
    findStart = start;
    findEnd = end;
    if (start != end)
    {
        // set highlighting
        CallSci(SCI_INDICATORFILLRANGE, start, end - start);

        // make sure it's in range
        INT_PTR startLine = CallSci(SCI_LINEFROMPOSITION, start);
        INT_PTR endLine = CallSci(SCI_LINEFROMPOSITION, end);
        INT_PTR topVisLine = CallSci(SCI_GETFIRSTVISIBLELINE);
        CallSci(SCI_SCROLLRANGE, start, end);
    }
}

bool LogViewerWin::UpdateSearch(const char *term)
{
    // try searching from the current position to end of document
    CallSci(SCI_SETTARGETRANGE, CallSci(SCI_GETCURRENTPOS), CallSci(SCI_GETLENGTH));
    if (CallSci(SCI_SEARCHINTARGET, strlen(term), reinterpret_cast<INT_PTR>(term)) >= 0)
    {
        // success - highlight the result
        SetFindHighlight(CallSci(SCI_GETTARGETSTART), CallSci(SCI_GETTARGETEND));
        return true;
    }

    // not found; try wrapping, from beginning of document to current position
    CallSci(SCI_SETTARGETRANGE, 0, CallSci(SCI_GETCURRENTPOS));
    if (CallSci(SCI_SEARCHINTARGET, strlen(term), reinterpret_cast<INT_PTR>(term)) >= 0)
    {
        // success - highlight the result
        SetFindHighlight(CallSci(SCI_GETTARGETSTART), CallSci(SCI_GETTARGETEND));
        return true;
    }

    // not found anywhere - remove any search highlighting
    SetFindHighlight(-1, -1);
    return false;
}

void LogViewerWin::CommitSearch(CommitSearchReason reason)
{
    // If ending by Enter key, and the search was successful, set the selection 
    // to the search result.  Otherwise, restore the original scroll position,
    // except when exiting by focus change, in which case the user probably
    // clicked in the Scintilla window and expects the scrolling to stay put.
    if (reason == CommitSearchReason::Accept && findStart >= 0 && findEnd >= 0)
        CallSci(SCI_SETSEL, findStart, findEnd);
    else if (reason != CommitSearchReason::KillFocus)
        CallSci(SCI_SCROLLCARET);

    // clear the find box
    SetWindowTextA(findBox, "");

    // remove search result highlighting
    SetFindHighlight(-1, -1);

    // set focus back in the Scintilla window
    SetFocus(sciWin);

    // mark the search as closed
    searchOpen = false;
}

void LogViewerWin::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // get the mouse position in client coordinates, to detect hot items
    POINT mouse; 
    GetCursorPos(&mouse);
    ScreenToClient(hwnd, &mouse);

    // fill the background in the control bar color
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xF0F0F0)));

    // frame the find box
    RECT erc = GetChildControlRect(findBox);
    InflateRect(&erc, 1, 1);
    FrameRect(hdc, &erc, HBrush(HRGB(0xB0B0B0)));

    // bitmap drawing DC
    CompatibleDC dcb(hdc);

    // draw control bar buttons
    int xLastBtn = 0;
    for (auto &b : ctlBarButtons)
        DrawCtlBarButton(hdc, b, xLastBtn);

    // draw a message centered in the window, in case the Scintilla window couldn't be created
    static const char *msg1 = "The Scintilla editor control component isn't available.";
    static const char *msg2 = "You might need to reinstall the application's program files.";

    SIZE sz = hdc.MeasureText(boldFont, msg1);
    int xc = (crc.left + crc.right)/2;
    int yc = (crc.top + crc.bottom)/2;
    hdc.DrawText(xc - sz.cx/2, yc - sz.cy, 1, boldFont, HRGB(0x606060), msg1);

    sz = hdc.MeasureText(boldFont, msg2);
    hdc.DrawText(xc - sz.cx/2, yc, 1, boldFont, HRGB(0x606060), msg2);
}

void LogViewerWin::AppendLog(const std::vector<BYTE> *text)
{
    // scan for escape sequences
    const BYTE *start = text->data();
    const BYTE *p = start;
    const BYTE *end = start + text->size();
    using State = EscapeSequenceState::State;
    for (; p < end ; ++p)
    {
        // check the escape state
        switch (esc.state)
        {
        case State::Plain:
            // Plain text mode
            if (*p == 27)
            {
                // enter escape state
                esc.state = State::Esc;

                // append the current run of plain text
                AppendLogPlain(start, p);
                start = p + 1;
            }
            break;

        case State::Esc:
            // We've parsed an Escape character; check for '['
            if (*p == '[')
            {
                // it's an escape sequence; enter the number section
                esc.state = State::Number;
                esc.acc = 0;
                esc.numbers.clear();
            }
            else
            {
                // it's not an escape sequence; return to plain text mode, with
                // the new plain-text run starting here
                esc.state = State::Plain;
                start = p;
            }
            break;

        case State::Number:
            // Number section; check for the next digit, or a letter ending the sequence
            if (isdigit(*p))
            {
                // digit - accumulate the digit into the current number
                esc.acc *= 10;
                esc.acc += *p - '0';
            }
            else if (*p == ';')
            {
                // semicolon - start the next number section
                esc.numbers.emplace_back(esc.acc);
                esc.acc = 0;
            }
            else
            {
                // anything else ends the escape sequence and resume plain
                // text mode starting at the next character
                esc.state = State::Plain;
                start = p + 1;

                // close out the last number
                esc.numbers.emplace_back(esc.acc);

                // check the letter code
                if (*p == 'm')
                {
                    // Esc [ <numbers> m
                    // SGR (Select Graphic Rendition) code
                                    //
                    // We accept the follow numeric codes:
                    //
                    //   0        reset
                    //   30-37    set foreground color
                    //   30-37;1  set bright foreground color (same as 90-97)
                    //   39       set default foreground color
                    //   40-47    set background color
                    //   40-47;1  set bright background color (same as 100-107)
                    //   49       set default background color
                    //   90-97    set bright foreground color
                    //   100-107  set bright background color

                    // get the first number
                    int color = esc.numbers.size() > 0 ? esc.numbers[0] : 0;

                    // If we have two numbers, and the second is a '1', add 50
                    // to the first to get the "bright" color code
                    if (esc.numbers.size() == 2 && esc.numbers[1] == 1)
                        color += 60;

                    // set the current foreground or background color, according
                    // to the color range
                    if (color == 0)
                        esc.fg = 39, esc.bg = 49;
                    else if ((color >= 30 && color < 40) || (color >= 90 && color < 100))
                        esc.fg = color;
                    else if ((color >= 40 && color < 50) || (color >= 100 && color < 110))
                        esc.bg = color;
                }
            }
            break;
        }
    }

    // append the last bit if we're in plain text mode
    if (esc.state == State::Plain)
        AppendLogPlain(start, p);
}

void LogViewerWin::AppendLogPlain(const BYTE *start, const BYTE *end)
{
    // make sure we have text to add
    if (end > start)
    {
        // check if the last line is currently in view
        INT_PTR lastVisibleLineNum = CallSci(SCI_GETFIRSTVISIBLELINE) + CallSci(SCI_LINESONSCREEN);
        INT_PTR numDocLines = CallSci(SCI_GETLINECOUNT);
        bool wasLastLineVisible = lastVisibleLineNum >= numDocLines - 1;

        // enable modification
        CallSci(SCI_SETREADONLY, 0);

        // set styling to start at the current position
        CallSci(SCI_STARTSTYLING, CallSci(SCI_GETLENGTH));

        // add the text
        INT_PTR len = static_cast<INT_PTR>(end - start);
        CallSci(SCI_APPENDTEXT, len, reinterpret_cast<INT_PTR>(start));

        // Look up the style code for the current fg/bg combination
        int style;
        int styleKey = MakeStyleKey(esc.fg, esc.bg);
        if (auto it = ansiCodeToSciStyle.find(styleKey); it != ansiCodeToSciStyle.end())
        {
            // use the existing style for this color combination
            style = it->second;
        }
        else
        {
            // add a new style for this color combination
            CallSci(SCI_STYLESETFORE, nextSciStyle, ansiColorMap.at(esc.fg));
            CallSci(SCI_STYLESETBACK, nextSciStyle, ansiColorMap.at(esc.bg));
            ansiCodeToSciStyle.emplace(styleKey, nextSciStyle);

            // use the new style code
            style = nextSciStyle++;
        }

        // set the style for the newly added text
        CallSci(SCI_SETSTYLING, len, style);

        // restore readonly mode
        CallSci(SCI_SETREADONLY, 1);

        // if the last line was visible when we started, scroll the new
        // last line into view
        if (wasLastLineVisible)
        {
            INT_PTR lastPos = CallSci(SCI_GETLENGTH);
            CallSci(SCI_SCROLLRANGE, lastPos, lastPos);
        }
    }
}

void LogViewerWin::SuspendUpdaterThread()
{
    // Don't completely suspend the thread when we're in the background.
    // Instead, just reduce the update frequency.  It's useful to keep
    // gathering log data even when we're in the background because of
    // the limited log storage available on the device side; polling it
    // even when we're in the background helps ensure that we don't miss
    // anything if we're backgrounded for a long time.
    updaterThread->timeBetweenUpdates = 1000;
}

void LogViewerWin::ResumeUpdaterThread()
{
    // restore immediate updates, and wake it immediately in case it's
    // in a wait
    updaterThread->timeBetweenUpdates = 0;
    SetEvent(updaterThread->hEvent);
}



// --------------------------------------------------------------------------
//
// Updater thread
//

bool LogViewerWin::UpdaterThread::Update(bool &releasedMutex)
{
    // query log text
    std::vector<uint8_t> text;
    if (device->device->QueryLog(text) != PinscapeResponse::OK)
        return false;

    // done with the mutex
    ReleaseMutex(device->mutex);
    releasedMutex = true;

    // send it to Scintilla
    SendMessage(hwnd, MSG_NEW_DATA, 0, reinterpret_cast<LPARAM>(&text));

    // success
    return true;
}


// --------------------------------------------------------------------------
//
// Printing
//

void LogViewerWin::PageSetupOptions::Load()
{
    __super::Load();
    const auto *val = gApp.settingsJSON.Get(jsonKey);
    wrap = val->Get("wrap")->Bool(true);
}

void LogViewerWin::PageSetupOptions::Store()
{
    __super::Store();
    auto &js = gApp.settingsJSON;
    auto *val = gApp.settingsJSON.SetObject(jsonKey);
    js.SetBool(val, "wrap", wrap);
}

void LogViewerWin::PageSetup()
{
    class PSDlg : public PageSetupDialog
    {
    public:
        PSDlg(HINSTANCE hInstance) : PageSetupDialog(hInstance, opts) { }

        PageSetupOptions opts;

        virtual void LoadControls()
        {
            __super::LoadControls();
            CheckRadioButton(hDlg, IDC_RB_WRAP, IDC_RB_TRUNC, opts.wrap ? IDC_RB_WRAP : IDC_RB_TRUNC);
        }

        virtual void StoreControls()
        {
            opts.wrap = (IsDlgButtonChecked(hDlg, IDC_RB_WRAP) == BST_CHECKED);
            __super::StoreControls();
        }
    };
    PSDlg d(hInstance);
    d.Show(IDD_LOGVIEWER_PAGESETUP);
}


bool LogViewerWin::PrintPageContents(HDCHelper &hdc, int pageNum, bool skip)
{
    // set up the Scintilla print context
    auto &rtf = printingContext.rtf;
    rtf.hdc = hdc;
    rtf.hdcTarget = hdc;
    auto ToSciRect = [](const RECT &rc) -> Sci_Rectangle { return { rc.left, rc.top, rc.right, rc.bottom }; };
    rtf.rcPage = ToSciRect(BaseWindow::printingContext.rcPaper);
    rtf.rc = ToSciRect(BaseWindow::printingContext.rcPrint);

    // get my subclassed options object
    auto &opts = *static_cast<PageSetupOptions*>(BaseWindow::printingContext.opts.get());

    // on page 1, initialize the print range to cover the whole document
    if (pageNum == 1)
    {
    // set the print range to the whole document
        rtf.chrg.cpMin = 0;
        rtf.chrg.cpMax = CallSci(SCI_GETLENGTH);

        // set the wrap mode
        CallSci(SCI_SETPRINTWRAPMODE, opts.wrap ? SC_WRAP_WORD : SC_WRAP_NONE);

        // set the color mode
        CallSci(SCI_SETPRINTCOLOURMODE, opts.monochrome ? SC_PRINT_BLACKONWHITE : SC_PRINT_COLOURONWHITE);
    }

    // have Scintilla print its content area
    rtf.chrg.cpMin = CallSci(SCI_FORMATRANGEFULL, !skip, reinterpret_cast<INT_PTR>(&rtf));

    // figure if there's more
    return (rtf.chrg.cpMin < rtf.chrg.cpMax);
}

int LogViewerWin::Paginate(HDCHelper &hdc)
{
    // set up the Scintilla print context
    auto &rtf = printingContext.rtf;
    rtf.hdc = hdc;
    rtf.hdcTarget = hdc;
    auto ToSciRect = [](const RECT &rc) -> Sci_Rectangle { return { rc.left, rc.top, rc.right, rc.bottom }; };
    rtf.rcPage = ToSciRect(BaseWindow::printingContext.rcPaper);
    rtf.rc = ToSciRect(BaseWindow::printingContext.rcPrint);

    // set the print range to the whole document
    rtf.chrg.cpMin = 0;
    rtf.chrg.cpMax = CallSci(SCI_GETLENGTH);

    // run through all the pages in non-drawing mode to get a page count
    int nPages = 0;
    for (; rtf.chrg.cpMin < rtf.chrg.cpMax ; ++nPages)
        rtf.chrg.cpMin = CallSci(SCI_FORMATRANGEFULL, FALSE, reinterpret_cast<INT_PTR>(&rtf));

    // return the number of pages calculated
    return nPages;
}

bool LogViewerWin::ExpandHeaderFooterVar(const std::string &varName, std::string &expansion)
{
    auto FromInt = [](int n) {
        char buf[32];
        sprintf_s(buf, "%d", n);
        return std::string(buf);
    };
    auto DeviceInfo = [this](std::function<std::string(const DeviceID&)> func)
    {
        DeviceID id;
        if (PinscapePico::VendorInterface::Shared::Locker l(updaterThread->device); l.locked)
            updaterThread->device->device->QueryID(id);
        return func(id);
    };
    if (varName == "unitNum")
        return expansion = DeviceInfo([FromInt](const DeviceID &id) { return FromInt(id.unitNum); }), true;
    if (varName == "unitName")
        return expansion = DeviceInfo([](const DeviceID &id) { return id.unitName; }), true;

    // unknown
    return __super::ExpandHeaderFooterVar(varName, expansion);
}

