// Pinscape Pico - Compiler-Specific Definitions
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// This header defines some macros for special language features that
// vary by compiler, so that we can write common code that works on both
// the Pico firmware side (using gcc) and the Windows application side
// (using MSVC).  This is useful especially for the protocol structure
// definitions, so that those can be shared across device and host.

#pragma once

// Macros for attribute declarations, for GCC and MSVC.  Note that
// the Pico SDK provides its own __packed macro, but we need a slightly
// different incarnation that takes the whole struct declaration as an
// argument in order to make this file shareable with MSVC.  Note the
// slightly different name we use for this macro (capital 'P') so that
// we don't conflict with the Pico SDK definitions, which are also
// needed when including SDK headers on the firmware side.
#ifdef __GNUC__
#define __PackedBegin __attribute__((__packed__))
#define __PackedEnd
#endif
#ifdef _MSC_VER
#define __PackedBegin __pragma(pack(push,1))
#define __PackedEnd __pragma(pack(pop))
#endif

