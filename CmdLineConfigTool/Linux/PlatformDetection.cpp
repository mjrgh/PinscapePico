// Pinscape Pico - Platform Detection Implementation
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include "PlatformDetection.h"
#include <cstdio>

namespace PinscapePico {

const char* PlatformInfo::GetPlatformName() {
    return PINSCAPE_PLATFORM_NAME;
}

const char* PlatformInfo::GetCompilerName() {
    return PINSCAPE_COMPILER_NAME;
}

const char* PlatformInfo::GetArchitectureName() {
    return PINSCAPE_ARCH_NAME;
}

bool PlatformInfo::IsLittleEndian() {
#ifdef PINSCAPE_LITTLE_ENDIAN
    return true;
#else
    return false;
#endif
}

const char* PlatformInfo::GetVersionString() {
    static char buffer[256];
    static bool initialized = false;

    if (!initialized) {
        snprintf(buffer, sizeof(buffer),
                "Pinscape Pico Config Tool\n"
                "  Platform: %s\n"
                "  Compiler: %s\n"
                "  Architecture: %s\n"
                "  Endianness: %s\n",
                GetPlatformName(),
                GetCompilerName(),
                GetArchitectureName(),
                IsLittleEndian() ? "Little Endian" : "Big Endian");
        initialized = true;
    }

    return buffer;
}

} // namespace PinscapePico
