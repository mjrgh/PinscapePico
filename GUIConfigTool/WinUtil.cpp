// Pinscape Pico - Windows utilities and helpers
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <memory>
#include <vector>
#include <Windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <atlbase.h>
#include "WinUtil.h"

#pragma comment(lib, "gdiplus")
#pragma comment(lib, "Msimg32")

// -----------------------------------------------------------------------
//
// Load a PNG resource into a GDI+ Bitmap
//
Gdiplus::Bitmap *GPBitmapFromPNG(HINSTANCE hInstance, int resid)
{
    // load and lock the PNG resource
    ResourceLocker res(hInstance, resid, _T("PNG"));
    if (res.GetData() == nullptr)
        return nullptr;

    // create a stream on the HGLOBAL
    CComPtr<IStream> pstr(SHCreateMemStream(static_cast<const BYTE*>(res.GetData()), res.GetSize()));
    if (pstr == nullptr)
        return nullptr;

    // create the bitmap
    return Gdiplus::Bitmap::FromStream(pstr);
}

// -----------------------------------------------------------------------
//
// Load a PNG resource into an HBITMAP
//

HBITMAP LoadPNG(HINSTANCE hInstance, int resid)
{
    // load the PNG into a GDI+ bitmap
    std::unique_ptr<Gdiplus::Bitmap> bmp(GPBitmapFromPNG(hInstance, resid));
    if (bmp == nullptr)
        return NULL;

    // get its HBITMAP
    HBITMAP hbitmap;
    bmp->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbitmap);

    // return the HBITMAP
    return hbitmap;
}

// -----------------------------------------------------------------------
//
// HDC helper
//

// measure text
SIZE HDCHelper::MeasureText(HFONT hfont, const char *str)
{
	// measure the width and line height
	SelectFont(hdc, hfont);
	SIZE sz;
	GetTextExtentPoint32A(hdc, str, static_cast<int>(strlen(str)), &sz);

	// GetTextExtentPoint32A ignores newlines, so look for additional lines and
	// multiply the height accordingly
	int lineHeight = sz.cy;
	for (const char *p = str ; *p != 0 ; ++p)
	{
		if (*p == '\n' && *(p+1) != 0)
			sz.cy += lineHeight;
	}

	// return the result
	return sz;
}

// Draw text.  'dir' specifies which horizontal direction to draw from
// the starting position x: 1 -> draw to the right, -1 -> draw to the left,
// 0 -> center on x.  Returns the size of the text drawn.
SIZE HDCHelper::DrawText(int x, int y, int dir, HFONT hfont, COLORREF color, const char *txt)
{
	// figure the text size
	SIZE sz = MeasureText(hfont, txt);

	// figure the drawing area, left, right, or center relative to the origin
	SetTextColor(hdc, color);
	RECT rc = (dir == 0) ? RECT{ x - sz.cx/2, y, x + sz.cx/2 + 16, y + sz.cy } :
		(dir > 0) ? RECT{ x, y, x + sz.cx, y + sz.cy } :
		RECT{ x - sz.cx, y, x, y + sz.cy };
	::DrawTextA(hdc, txt, -1, &rc, DT_LEFT | DT_TOP);

	// return the text size
	return sz;
}

// Draw text with printf-like formatting
SIZE HDCHelper::DrawTextV(int x, int y, int dir, HFONT hfont, COLORREF color, const char *fmt, va_list va)
{
	// measure the required buffer size
	va_list va2;
	va_copy(va2, va);
	size_t bufsize = _vscprintf(fmt, va2) + 1;
	va_end(va2);

	// format the text into a temp buffer
	std::vector<char> buf;
	buf.resize(bufsize);
	vsprintf_s(buf.data(), bufsize, fmt, va);

	// draw it
	return DrawText(x, y, dir, hfont, color, buf.data());
}

SIZE HDCHelper::DrawTextF(int x, int y, int dir, HFONT hfont, COLORREF color, const char *fmt, ...)
{
	// draw the text
	va_list va;
	va_start(va, fmt);
	auto ret = DrawTextV(x, y, dir, hfont, color, fmt, va);
	va_end(va);

	// return the text size from DrawTextV
	return ret;
}

SIZE HDCHelper::DrawText(POINT pt, POINT margin, POINT dir, HFONT hfont, COLORREF color, const char *txt)
{
	// figure the text size
	SIZE sz = MeasureText(hfont, txt);

	// figure the drawing area relative to the origin
	SetTextColor(hdc, color);
	RECT rc ={ pt.x, pt.y, pt.x + sz.cx, pt.y + sz.cy };
	if (dir.x < 0)
		OffsetRect(&rc, -sz.cx - margin.x, 0);
	else if (dir.x == 0)
		OffsetRect(&rc, -sz.cx/2, 0);
	else
		OffsetRect(&rc, margin.x, 0);

	if (dir.y < 0)
		OffsetRect(&rc, 0, -sz.cy - margin.y);
	else if (dir.y == 0)
		OffsetRect(&rc, 0, -sz.cy/2);
	else
		OffsetRect(&rc, 0, margin.y);

	::DrawTextA(hdc, txt, -1, &rc, DT_LEFT | DT_TOP);

	// return the text size
	return sz;
}

SIZE HDCHelper::DrawTextV(POINT pt, POINT margin, POINT dir, HFONT hfont, COLORREF color, const char *fmt, va_list va)
{
	// measure the required buffer size
	va_list va2;
	va_copy(va2, va);
	size_t bufsize = _vscprintf(fmt, va2) + 1;
	va_end(va2);

	// format the text into a temp buffer
	std::vector<char> buf;
	buf.resize(bufsize);
	vsprintf_s(buf.data(), bufsize, fmt, va);

	// draw it
	return DrawText(pt, margin, dir, hfont, color, buf.data());
}


SIZE HDCHelper::DrawTextF(POINT pt, POINT margin, POINT dir, HFONT hfont, COLORREF color, const char *fmt, ...)
{
	// draw the text
	va_list va;
	va_start(va, fmt);
	auto ret = DrawTextV(pt, margin, dir, hfont, color, fmt, va);
	va_end(va);

	// return the text size from DrawTextV
	return ret;
}

// Get the y position for vertically centering text within a rectangle
int HDCHelper::VCenterText(const RECT &rc, HFONT hfont)
{
	// figure the text height
	SelectFont(hdc, hfont);
	SIZE szTxt;
	GetTextExtentPoint32A(hdc, "X", 1, &szTxt);

	// figure the centering position
	return (rc.top + rc.bottom - szTxt.cy)/2;
}

