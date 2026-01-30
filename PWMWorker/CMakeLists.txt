# PWMWorker build instructions
# Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
#
# This file is part of Pinscape Pico.  Build using the normal Pico SDK
# build procedures.


cmake_minimum_required(VERSION 3.12)
include(pico_sdk_import.cmake)

# project declaration
project(PWMWorker C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Disable -Wpsabi.  This is a warning about possible library ABI
# incompatibilities that's some std:: library calls trigger.  It's
# always spurious for our setup because we don't use any pre-built
# libraries (which is what could trigger the underlying problem that
# the compiler is trying to warn us about), but the compiler can't
# know that, so we just have to disable the warning entirely.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-psabi")

# set up the pico SDK
pico_sdk_init()

# main executable and list of sources
add_executable(
    PWMWorker
    PWMWorker.cpp
    Logger.cpp
    PicoLED.cpp
    PWMManager.cpp
    i2c_slave.cpp
)

# PIO headers
pico_generate_pio_header(PWMWorker PWM_pio.pio)

# Explicitly add the source directory to the #include list.  This seems
# redundant, but it's required because some tinyusb headers #include
# headers that have to be provided by the client program, as a way of
# injecting client definitions into library headers.
target_include_directories(PWMWorker PUBLIC .)

# libraries
target_link_libraries(
    PWMWorker
    pico_stdlib
    pico_time
    hardware_exception
    hardware_i2c
    hardware_gpio
    hardware_pwm
    hardware_watchdog
    hardware_dma
    hardware_pio
    pico_bootsel_via_double_reset
)

# create map/bin/hex files
pico_add_extra_outputs(PWMWorker)

# Disable stdio on UART, enable on USB.  We'll use USB for debug output.
# We assign all of the pins as either PWM outputs or I2C ports, so there
# are no pins left over to assign to the UART.
pico_enable_stdio_uart(PWMWorker 0)
pico_enable_stdio_usb(PWMWorker 0)
target_compile_definitions(PWMWorker PRIVATE PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=30000)
