// Pinscape Pico - Project Version Number
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines the version number for all subprojects.

#define PINSCAPE_PICO_VERSION_MAJOR  0
#define PINSCAPE_PICO_VERSION_MINOR  1
#define PINSCAPE_PICO_VERSION_PATCH  5

#define PINSCAPE_PICO_STRING(x) #x
#define PINSCAPE_PICO_XSTRING(x) PINSCAPE_PICO_STRING(x)
#define PINSCAPE_PICO_VERSION_STRING PINSCAPE_PICO_XSTRING(PINSCAPE_PICO_VERSION_MAJOR) "." PINSCAPE_PICO_XSTRING(PINSCAPE_PICO_VERSION_MINOR) "." PINSCAPE_PICO_XSTRING(PINSCAPE_PICO_VERSION_PATCH)
