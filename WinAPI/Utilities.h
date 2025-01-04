// Pinscape Pico API - helper utilities
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <functional>
#include <Windows.h>
#include <windowsx.h>

// Generic Windows handle holder
template<typename H> class HandleHolder
{
public:
	HandleHolder(std::function<void(H)> deleter) : handle(NULL), deleter(deleter) { }
	HandleHolder(H handle, std::function<void(H)> deleter) :
		handle(handle), deleter(deleter) { }

	~HandleHolder() {
		if (handle != NULL)
			deleter(handle);
	}

	void reset(H h)
	{
		if (handle != NULL)
			deleter(handle);
		handle = h;
	}

	H get() { return handle; }
	H release()
	{
		H h = handle;
		handle = NULL;
		return h;
	}

	H* operator &() { return &handle; }

	H handle;
	std::function<void(H)> deleter;
};

// Windows OVERLAPPED struct holder
class OVERLAPPEDHolder
{
public:
	OVERLAPPEDHolder(HANDLE hFile, WINUSB_INTERFACE_HANDLE winusbHandle) : hFile(hFile), winusbHandle(winusbHandle)
	{
		ZeroMemory(&ov, sizeof(ov));
		ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	}

	~OVERLAPPEDHolder()
	{
		CloseHandle(ov.hEvent);
	}

	template<typename SizeType> HRESULT Wait(DWORD timeout, SizeType &bytesTransferred)
	{
		switch (WaitForSingleObject(ov.hEvent, timeout))
		{
		case WAIT_TIMEOUT:
			// timeout - cancel the I/O and return ABORT status
			Cancel(timeout);
			return E_ABORT;

		case WAIT_OBJECT_0:
			// I/O completed - return the result from the OVERLAPPED struct
			{
				ULONG sz;
				if (WinUsb_GetOverlappedResult(winusbHandle, &ov, &sz, FALSE))
				{
					bytesTransferred = static_cast<SizeType>(sz);
					return S_OK;
				}
				else
					return HRESULT_FROM_WIN32(GetLastError());
			}

		case WAIT_FAILED:
			// error in the wait - cancel the I/O and return the underlying error code
			Cancel(timeout);
			CancelIoEx(hFile, &ov);
			return HRESULT_FROM_WIN32(GetLastError());

		default:
			// other error - cancel the I/O and return generic FAIL status
			Cancel(timeout);
			return E_FAIL;
		}
	}

	// cancel an I/O request, waiting for up to the timeout for the response to clear
	void Cancel(DWORD timeout)
	{
		// cancel the I/O
		CancelIoEx(hFile, &ov);

		// Cancellation isn't necessarily synchronous, so allow some time
		// for the request to clear, up to the timeout.  This effectively
		// doubles the timeout period the caller requested, but that's
		// preferable to leaving the I/O hanging, because an incomplete
		// I/O tends to break the protocol flow for future requests. 
		// Allowing some extra time for the I/O to clear is better for
		// application stability.  In practice, WinUsb seems to resolve
		// cancellations quickly (order of milliseconds), but not
		// immediately, so the extra wait is worthwhile: it improves
		// the chances that the pipe will recover and we can keep making
		// requests, without incurring much actual wait time.
		//
		// We don't care what the outcome of the wait is, because there's
		// nothing more we can do here to resolve the problem if the wait
		// times out or fails with an error.  The purpose of the wait is
		// to allow time for WinUsb to recover *when that's possible*,
		// with the timeout to prevent a freeze when recovery isn't
		// possible.  In the event that the cancellation doesn't complete
		// within the timeout, the caller is responsible for keeping the
		// OVERLAPPED struct's memory valid, because the WinUsb driver
		// will hang onto the pointer to it until the I/O actually does
		// complete, and could write into the memory at any time until
		// then.
		WaitForSingleObject(ov.hEvent, timeout);
	}

	// the original device handle
	HANDLE hFile;

	// the WinUsb handle layered on the device handle
	WINUSB_INTERFACE_HANDLE winusbHandle;
	
	// system overlapped I/O tracking struct
	OVERLAPPED ov;
};

// Overlapped I/O object - encapsulates an OVERLAPPED struct and event handle
struct OverlappedObject
{
	~OverlappedObject() { CloseHandle(hEvent); }

	OVERLAPPED *Clear()
	{
		memset(&ov, 0, sizeof(ov));
		ov.hEvent = hEvent;
		return &ov;
	}

	OVERLAPPED *SetOffset(uint64_t offset)
	{
		memset(&ov, 0, sizeof(ov));
		ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
		ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
		ov.hEvent = hEvent;
		return &ov;
	}

	OVERLAPPED *SetAppend()
	{
		memset(&ov, 0, sizeof(ov));
		ov.Offset = ov.OffsetHigh = 0xFFFFFFFF;
		ov.hEvent = hEvent;
		return &ov;
	}

	HANDLE hEvent{ CreateEvent(NULL, TRUE, FALSE, NULL) };
	OVERLAPPED ov{ 0 };
};


// --------------------------------------------------------------------------
//
// TCHAR utilities
// 

// convenience typedefs for our conditional and explicit-wide string types
using TSTRING = std::basic_string<TCHAR>;
using WSTRING = std::basic_string<WCHAR>;

// convert a fixed-type string to TCHAR
inline TSTRING ToTCHAR(const char *str)
{
#ifdef _UNICODE
	// TCHAR == wchar_t -> convert 8-bit to wide char
	size_t size = strlen(str) + 1;
	TSTRING ts(size, 0);
	size_t ret;
	mbstowcs_s(&ret, ts.data(), size, str, _TRUNCATE);
	return ts;
#else
	// TCHAR == char -> we're already the right type
	return str;
#endif
}

inline TSTRING ToTCHAR(const wchar_t *str)
{
#ifdef _UNICODE
	// TCHAR == wchar_t -> already the right type
	return str;
#else
	// TCHAR == char -> convert wide char to 8-bit
	size_t size = wcslen(str) + 1;
	TSTRING ts(size, 0);
	size_t ret;
	wcstombs_s(&ret, ts.data(), size, str, _TRUNCATE);
	return ts;
#endif
}

#ifdef _UNICODE
inline TSTRING STRINGToTSTRING(const std::string &str) { return ToTCHAR(str.c_str()); }
#define WSTRINGToTSTRING(str) (str)
#else
#define STRINGToTSTRING(str) (str)
inline TSTRING WSTRINGToTSTRING(WSTRING &wstr) { return ToTCHAR(wstr.c_str()); }
#endif

// "%s" format variations to use in format strings for TCHAR* string
// arguments - Microsoft provides tchar.h macros for just about everything
// else TCHAR-related, but not this.  You don't need this when using a
// one of the _txxxprintf functions, because those treat %s as whichever
// form TCHAR is currently using.  You DO need this when using either
// explicit narrow-character xxxprintf or explicit wide-char wxxxprintf,
// because each of those expects that %s means their native type (that is,
// printf() thinks %s means char*, and wprintf() thinks %s means wchar_t*).
// And you don't need it if you're mixing printf() with wchar_t* arguments,
// because there you write "%ws", OR when mixing wprintf() with char* args,
// where you write "%hs".  This is just for the one remaining oddball
// scenario that MSFT apparently missed, where you're using an explicit-
// width function (NOT a _txxx function) with a TCHAR* string.  In that
// case, we need to use "%ws" if TCHAR==wchar_t and "%hs" if TCHAR==char,
// which we can do via a macro switched on _UNICODE.
//
// Use these like so:
//
//    printf("Filename = %" _TSFMT "\n", filename);
//
// Note that the "%" is NOT part of the format string - this is deliberate,
// so that each use can add format modifiers as needed (e.g., "%-7" _TSFMT).
#ifdef _UNICODE
#define _TSFMT "ws"
#else
#define _TSFMT "hs"
#endif

