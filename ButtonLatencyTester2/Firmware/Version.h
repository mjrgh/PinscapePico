// Pinscape Pico Button Latency Tester II - Firmware Version Info
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once

// Version number
#define VERSION_MAJOR  0
#define VERSION_MINOR  1
#define VERSION_PATCH  0

// Build timestamp.  This is a printable 12-character string, in the
// format YYYYMMDDhhmmss, giving the compile timestamp for the main
// module, which serves as a proxy for the build date of the whole
// firmware image.  This is exactly 12 characters long, plus a null
// terminator byte.  The storage is defined in main.cpp and
// initialized at program startup (it obviously can't change during
// a session, by its nature).  The timestamp is derived from the
// __DATE__ and __TIME__ macros defined by the C++ compiler, which
// use the local time zone on the build machine.
extern char buildTimestamp[];

// compiler version
#define STRINGIZE_EXPAN(x) STRINGIZE_LIT(x)
#define STRINGIZE_LIT(x) #x
#define COMPILER_VERSION_STRING "GNUC " STRINGIZE_EXPAN(__GNUC__) "." STRINGIZE_EXPAN(__GNUC_MINOR__) "." STRINGIZE_EXPAN(__GNUC_PATCHLEVEL__)

