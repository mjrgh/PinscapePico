// Pinscape Pico - Config Tool Version Number
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Program version string, displayed in the About Box
extern const char *gVersionString;

// Get the build timestamp in our canonical YYMMDDhhmmss<MODE>/<CPU> format.
// 
// <MODE> specifies the build mode (D for debug, R for release)
// <CPU> specifies the CPU type (x86, iA64)
//
const char *GetBuildTimestamp();

// build mode code
#ifdef _DEBUG
#define VSN_BUILD_MODE_CODE 'D'
#else
#define VSN_BUILD_MODE_CODE 'R'
#endif

#if defined(_M_IX86)
#define VSN_BUILD_CPU "x86"
#elif defined(_M_X64)
#define VSN_BUILD_CPU "x64"
#endif
