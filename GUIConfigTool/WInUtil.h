// Pinscape Pico - Windows utilities and helpers
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <Windows.h>
#include <windowsx.h>
#include <gdiplus.h>

// -----------------------------------------------------------------------
// 
// PNG helpers
//

// Load a PNG resource into an HBITMAP object.  Note that the
// caller must initialize GDI+ prior to calling this.
HBITMAP LoadPNG(HINSTANCE hInstance, int resid);

// Load a PNG resource into a GDI+ Bitmap object.  The caller
// must initialize GDI+ prior to calling this.
Gdiplus::Bitmap *GPBitmapFromPNG(int resid);


// -----------------------------------------------------------------------
//
// Resource locker
//
class ResourceLocker
{
public:
	ResourceLocker(HINSTANCE hInstance, int id, const TCHAR *rt) : hres(NULL), hglobal(NULL), data(nullptr), sz(0)
	{
		// find the resource
		if ((hres = FindResource(hInstance, MAKEINTRESOURCE(id), rt)) != NULL)
		{
			// load it
			if ((hglobal = LoadResource(hInstance, hres)) != NULL)
			{
				// get its size and lock it
				sz = SizeofResource(hInstance, hres);
				data = LockResource(hglobal);
			}
		}
	}

	~ResourceLocker()
	{
		if (hres != NULL)
			UnlockResource(hres);
	}

	// get the data pointer; null if the resource wasn't found
	const void *GetData() const { return data; }

	// get the data size in bytes
	DWORD GetSize() const { return sz; }

	// get the resource handle
	HRSRC GetHRes() const { return hres; }

	// get the HGLOBAL handle
	HGLOBAL GetHGlobal() const { return hglobal; }

protected:
	HRSRC hres;          // resource handle
	HGLOBAL hglobal;     // hglobal with loaded resource
	const void *data;    // pointer to data
	DWORD sz;            // size of the data
};


// -----------------------------------------------------------------------
//
// HGLOBAL locker.  Creates and locks an HGLOBAL memory object.
//
class HGlobalLocker
{
public:
	// allocate and lock an HGLOBAL
	HGlobalLocker(UINT flags, SIZE_T size) : p(nullptr)
	{
		if ((h = GlobalAlloc(flags, size)) != NULL)
			p = GlobalLock(h);
	}

	~HGlobalLocker()
	{
		if (h != NULL)
		{
			if (p != nullptr)
				GlobalUnlock(h);
			GlobalFree(h);
		}
	}

	// get the handle
	HGLOBAL GetHandle() const { return h; }

	// release the handle - takes ownership of the handle object, preventing
	// deletion when the HGlobalLocker goes out of scope
	HGLOBAL Release()
	{
		HGLOBAL ret = h;
		h = NULL;
		return ret;
	}

	// get the locked buffer
	void *GetBuf() const { return p; }

protected:
	// object handle
	HGLOBAL h;

	// locked buffer pointer
	void *p;
};

// -----------------------------------------------------------------------
//
// Compatible DC, for bitmap sourcing
//
class CompatibleDC
{
public:
	CompatibleDC(HDC srcdc) : hdc(CreateCompatibleDC(srcdc)) { }
	~CompatibleDC() { 
		Reset();
		DeleteDC(hdc);
	}

	// select a new bitmap into the DC; saves the original if
	// we haven't already, for restoration prior to DC destruction
	void Select(HBITMAP newbmp)
	{
		HBITMAP prv = SelectBitmap(hdc, newbmp);
		if (oldbmp == NULL)
			oldbmp = prv;
	}

	void Reset()
	{
		if (oldbmp != NULL)
		{
			SelectBitmap(hdc, oldbmp);
			oldbmp = NULL;
		}
	}

	operator HDC() { return hdc; }

	HDC hdc;
	HBITMAP oldbmp = NULL;
};

// -----------------------------------------------------------------------
//
// Window DC
//
class WindowDC
{
public:
	WindowDC(HWND hwnd) : hdc(GetWindowDC(hwnd)), hwnd(hwnd) { }
	~WindowDC() { ReleaseDC(hwnd, hdc); }

	operator HDC() { return hdc; }

	HDC hdc;
	HWND hwnd;
};

// -----------------------------------------------------------------------
//
// HDC helper.  Encapsulates an HDC, and provides some higher-level
// covers for common GDI functions.
//
class HDCHelper
{
public:
	HDCHelper() : hdc(NULL) { }
	HDCHelper(HDC hdc) : hdc(hdc) { }

	~HDCHelper() 
	{
		// if the DC came from GetWindowDC(), release the window DC when done
		if (hwnd != NULL)
			ReleaseDC(hwnd, hdc);
	}

	// create from a window DC
	HDCHelper(HWND hwnd) : hwnd(hwnd), hdc(GetWindowDC(hwnd)) { }

	// when used in a context requiring an HDC, pass back the HDC
	operator HDC() const { return hdc; }

	// measure text
	SIZE MeasureText(HFONT hfont, const char *str);

	// Draw text.  'dir' specifies which horizontal direction to draw from
	// the starting position x: 1 -> draw to the right, -1 -> draw to the left,
	// 0 -> center on x.  Returns the size of the text drawn.
	SIZE DrawText(int x, int y, int dir, HFONT hfont, COLORREF color, const char *txt);

	// Draw text with printf-like formatting
	SIZE DrawTextV(int x, int y, int dir, HFONT hfont, COLORREF color, const char *fmt, va_list va);
	SIZE DrawTextF(int x, int y, int dir, HFONT hfont, COLORREF color, const char *fmt, ...);

	// Draw text relative to a given point.  dir is {0,0} for center, {1, 0} for left/center, etc.
	SIZE DrawText(POINT pt, POINT margin, POINT dir, HFONT font, COLORREF color, const char *txt);
	SIZE DrawTextV(POINT pt, POINT margin, POINT dir, HFONT font, COLORREF color, const char *fmt, va_list va);
	SIZE DrawTextF(POINT pt, POINT margin, POINT dir, HFONT font, COLORREF color, const char *fmn, ...);

	// Get the y position for vertically centering text within a rectangle
	int VCenterText(const RECT &rc, HFONT hfont);

	// the underlying Windows HDC
	HDC hdc;

	// window, when the DC came from GetWindowDC() 
	HWND hwnd = NULL;
};

// -----------------------------------------------------------------------
//
// Colors
//

// HTML color notation (0xRRGGBB) to COLORREF
inline COLORREF HRGB(DWORD c) { return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }



// -----------------------------------------------------------------------
//
// GDI brush
//
class HBrush
{
public:
	HBrush(COLORREF color) : hBrush(CreateSolidBrush(color)), color(color) { }
	~HBrush() { DeleteBrush(hBrush); }
	HBRUSH hBrush;
	COLORREF color;
	operator HBRUSH() const { return hBrush; }
	void SetTextColor(HDC hdc) const { ::SetTextColor(hdc, color); }
};

// -----------------------------------------------------------------------
//
// GDI pen
//
class HPen
{
public:
	HPen(COLORREF color, int width = 1) : hPen(CreatePen(PS_SOLID, width, color)), color(color), width(width) { }
	~HPen() { DeletePen(hPen); }
	HPEN hPen;
	COLORREF color;
	int width;
	operator HPEN() const { return hPen; }
	void SetTextColor(HDC hdc) const { ::SetTextColor(hdc, color); }
};

