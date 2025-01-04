// Pinscape Pico - Utilities
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
#include <stdlib.h>
#include <stdio.h>
#include <functional>
#include <string>
#include <regex>
#include <stdarg.h>
#include "Utilities.h"


std::string StrPrintf(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	std::string s = StrPrintfV(fmt, va);
	va_end(va);
	return s;
}

std::string StrPrintfV(const char *fmt, va_list va)
{
	// make a copy to measure the formatted length
	va_list va2;
	va_copy(va2, va);
	int len = _vscprintf(fmt, va2);
	va_end(va2);

	// allocate space and format the message
	std::unique_ptr<char, void(*)(void*)> buf(reinterpret_cast<char*>(_malloca(len + 1)), [](void *ptr) { _freea(ptr); });
	vsprintf_s(buf.get(), static_cast<size_t>(len) + 1, fmt, va);

	// return the formatted string
	return buf.get();
}

std::basic_string<wchar_t> StrPrintf(const wchar_t *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	std::basic_string<wchar_t> s = StrPrintfV(fmt, va);
	va_end(va);
	return s;
}

std::basic_string<wchar_t> StrPrintfV(const wchar_t *fmt, va_list va)
{
	// make a copy to measure the formatted length
	va_list va2;
	va_copy(va2, va);
	int len = _vscwprintf(fmt, va2);
	va_end(va2);

	// allocate space and format the message
	std::wstring buf;
	buf.resize(len);
	vswprintf_s(buf.data(), static_cast<size_t>(len) + 1, fmt, va);

	// return the formatted string
	return buf;
}
