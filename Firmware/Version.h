// Pinscape Pico - Firmware Version Info
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once

// Major, minor, and patch version numbers.  These are displayed as a
// dotted triplet, MAJOR.MINOR.PATCH, where all fields are rendered as
// decimal values without leading zeroes.  So (1, 2, 3) is displayed
// as "1.2.3" 
//
// There are several common customs in software versioning that differ
// in the details, but generally, the major version is incremented only
// when the software is substantially overhauled or when big new
// features are added; the minor version increases when any functional
// change is made or new features are added; and the patch version is
// incremented at every public release that doesn't have any functional
// or feature changes (just bug fixes, peformance improvements, or ohter
// internal changes that don't affect what the user sees).  The minor
// version resets to zero whenever the minor version changes, and the
// minor version resets to zero when the major version changes.
//
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


// DEVICE_USB_VERSION: Note that this separate version macro is defined
// in usbifc.h rather than here, because it applies only to the USB
// interface.  That should be incremented when and only when the USB
// descriptors are modified in such a way that host machines must be
// forced to discard any cached USB descriptors for the device and
// re-query them.  The USB descriptors are very specific to USB and
// should not be affected by most changes to the rest of the software,
// so DEVICE_USB_VERSION shouldn't need to change very often.  That's
// why we deliberately located it in the USB header rather than here: we
// didn't want to create any implication that the USB version number is
// tied to the overall firmware version numbering or that it needs to
// move in lock-step the firmware version.
