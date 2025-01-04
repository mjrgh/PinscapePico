// Pinscape Pico - Command Line Config Tool Version Number
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Program version string, displayed in the About Box
extern const char *gVersionString;

// Get the build timestamp in our canonical YYMMDDhhmmss format
const char *GetBuildTimestamp();
