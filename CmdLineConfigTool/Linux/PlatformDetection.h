// Pinscape Pico - Platform Detection and Abstraction
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define PINSCAPE_PLATFORM_WINDOWS 1
    #define PINSCAPE_PLATFORM_NAME "Windows"
#elif defined(__APPLE__)
    #define PINSCAPE_PLATFORM_MACOS 1
    #define PINSCAPE_PLATFORM_NAME "macOS"
#elif defined(__linux__)
    #define PINSCAPE_PLATFORM_LINUX 1
    #define PINSCAPE_PLATFORM_NAME "Linux"
#elif defined(__unix__)
    #define PINSCAPE_PLATFORM_UNIX 1
    #define PINSCAPE_PLATFORM_NAME "Unix"
#else
    #define PINSCAPE_PLATFORM_UNKNOWN 1
    #define PINSCAPE_PLATFORM_NAME "Unknown"
#endif

// Compiler detection
#if defined(_MSC_VER)
    #define PINSCAPE_COMPILER_MSVC 1
    #define PINSCAPE_COMPILER_NAME "MSVC"
#elif defined(__clang__)
    #define PINSCAPE_COMPILER_CLANG 1
    #define PINSCAPE_COMPILER_NAME "Clang"
#elif defined(__GNUC__)
    #define PINSCAPE_COMPILER_GCC 1
    #define PINSCAPE_COMPILER_NAME "GCC"
#else
    #define PINSCAPE_COMPILER_UNKNOWN 1
    #define PINSCAPE_COMPILER_NAME "Unknown"
#endif

// Architecture detection
#if defined(__x86_64__) || defined(_M_X64)
    #define PINSCAPE_ARCH_X64 1
    #define PINSCAPE_ARCH_NAME "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
    #define PINSCAPE_ARCH_X86 1
    #define PINSCAPE_ARCH_NAME "x86"
#elif defined(__ARM_ARCH) || defined(_M_ARM)
    #define PINSCAPE_ARCH_ARM 1
    #define PINSCAPE_ARCH_NAME "ARM"
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define PINSCAPE_ARCH_ARM64 1
    #define PINSCAPE_ARCH_NAME "ARM64"
#else
    #define PINSCAPE_ARCH_UNKNOWN 1
    #define PINSCAPE_ARCH_NAME "Unknown"
#endif

// Endianness detection
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define PINSCAPE_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    #define PINSCAPE_BIG_ENDIAN 1
#elif defined(_MSC_VER)
    // MSVC is always little-endian on Windows
    #define PINSCAPE_LITTLE_ENDIAN 1
#else
    // Default to little-endian (most common)
    #define PINSCAPE_LITTLE_ENDIAN 1
#endif

// Common assertions for compile-time checks
static_assert(sizeof(char) == 1, "char must be 1 byte");
static_assert(sizeof(short) == 2, "short must be 2 bytes");
static_assert(sizeof(int) == 4, "int must be 4 bytes");
static_assert(sizeof(long long) == 8, "long long must be 8 bytes");

namespace PinscapePico {

// Runtime platform information
struct PlatformInfo {
    static const char* GetPlatformName();
    static const char* GetCompilerName();
    static const char* GetArchitectureName();
    static bool IsLittleEndian();
    static const char* GetVersionString();
};

} // namespace PinscapePico
