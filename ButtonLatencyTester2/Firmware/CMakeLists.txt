# Pinscape Pico Button Latency Tester II - firmware build instructions
# Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.12)
include(pico_sdk_import.cmake)

# project declaration
project(ButtonLatencyTester2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(PICO_FIRMWARE_BUILD=1)
add_compile_definitions(PICO_MAX_SHARED_IRQ_HANDLERS=8)

# Disable -Wpsabi.  This is a warning about possible library ABI
# incompatibilities in code that passes structs by value across
# library call boundaries; in particular, it's always triggered in
# std::vector::emplace_back(), regardless of what the constructor
# parameters look like, because emplace_back() itself passes the
# newly constructed element around internally by value.  The
# warning is for people who are linking pre-built third-party
# libraries into their program, since the external libraries
# could have been built with earlier gcc versions.  There's no
# way for gcc to determine at compile time if the mixed ABI
# situation applies to a given call, since the source of the
# callee isn't known until link time, so the compiler has to
# barf out the warning every time it *could* apply, whether it
# does or not.  In our case, the warning is always spurious,
# because the only pre-built libraries we use are from gcc
# itself and so should be sync'ed on version.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-psabi")

# set up the pico SDK
pico_sdk_init()

# main executable and list of sources
add_executable(
    ButtonLatencyTester2
    Main.cpp
    VendorIfc.cpp
    Reset.cpp
    USBImpl.cpp
    PicoLED.cpp
    Version.cpp
    ../../Firmware/Utils.cpp
    ../../Firmware/Logger.cpp
    ../../Firmware/USBCDC.cpp
    ../../Firmware/CommandConsole.cpp
    ../../Firmware/TimeOfDay.cpp
    ../../Firmware/Watchdog.cpp
)

# Explicitly add the source directory to the #include list.  This seems
# redundant, but it's required because some tinyusb headers #include
# headers that have to be provided by the client program, as a way of
# injecting client definitions into library headers.
target_include_directories(ButtonLatencyTester2 PUBLIC .)


# libraries
target_link_libraries(
    ButtonLatencyTester2
    pico_stdlib
    pico_unique_id
    pico_multicore
    pico_time
    pico_platform
    pico_sync
    hardware_exception
    hardware_gpio
    hardware_watchdog
    hardware_sync
    hardware_dma
    tinyusb_device
    tinyusb_board
)


# create map/bin/hex files
pico_add_extra_outputs(ButtonLatencyTester2)

# Disable stdio on all device interfaces (UART and USB).  We use our
# own custom logging mechanism in place of stdio, and we don't want
# stdio claiming either type of device.
#pico_enable_stdio_uart(PinscapePico 0)
#pico_enable_stdio_usb(PinscapePico 0)
