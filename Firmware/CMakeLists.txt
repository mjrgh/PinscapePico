# Pinscape Pico - firmware build instructions
# Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
#
# This file contains the main build instructions for the Pinscape Pico build
# process, following the build model for C++ firmware described in Getting
# Started with Raspberry Pi Pico-series, available at:
#
# (https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf)
#
# CMAKE is a meta-build tool; its job is to create build scripts for an
# underlying build system that will execute the build.  For Windows, this
# is typically NMAKE, Microsoft's typically idiosyncratic take on the
# venerable Unix 'make' tool.
#
# Before running CMAKE, you must set up your build environment for the
# Pico SDK and build tools.  See the ReadMe.md file in the PinscapePico
# root directory for instructions.  Before running CMAKE, you must run
# the local environment/path variable setup script from the Pico SDK
# tools folder:
#
#   <path to Pico SDK folder>\pico-env.cmd
#
# Now switch working directory here:
#
#   cd PinscapePico\Firmware
#
# ...and if necessary, delete any remnants of a prior build:
#
#   del CMakeCache.txt
#   rmdir /s CMakeFiles
#
# Now you can run the CMAKE build as follows:
#
#   cmake -S . -G "NMake Makefiles"
#
# That doesn't actually build any software; it only builds the scripts
# that build the software.  Now you can run NMAKE:
#
#   nmake
#
# If all goes well, that should compile all of the Pinscape Pico C++
# files and produce PinscapePico.uf2, which is the actual binary file
# containing the Pinscape Pico firmware that you can copy onto a live
# Pico.
#
# If you're actively working on the Pinscape source code, you don't
# need to repeat the whole CMAKE process on every build.  Recompiling
# after a code change is just a matter of typing 'nmake' again.  The
# build scripts are very good about inferring which files need to be
# recompiled after touching any source or header files (kudos to CMAKE
# for solid automatic dependency management), so you don't even have
# to specify which files to build; NMAKE will figure that out and
# recompile what needs to be recompiled, without wasting your time
# by recompiling more than it has to.
#
# You CAN repeat the CMAKE step at any time, by deleting the old
# CMAKE output first as shown above, but the only time you should
# really have to do this is when updating to a new version of the
# Pico SDK, or changing the locations of Pico SDK files.  However,
# it's perfectly safe to start from scratch with CMAKE if you're
# ever in doubt about the integrity of the dependency tracking.


set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.12)
include(pico_sdk_import.cmake)

# project declaration
project(PinscapePico C CXX ASM)
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
    PinscapePico
    Main.cpp
    Version.cpp
    Utils.cpp
    ButtonHelper.cpp
    MultiCore.cpp
    ThunkManager.cpp
    FlashStorage.cpp
    ExpansionBoard.cpp
    TimeOfDay.cpp
    USBIfc.cpp
    Keyboard.cpp
    Gamepad.cpp
    OpenPinballDevice.cpp
    FeedbackController.cpp
    Logger.cpp
    CommandConsole.cpp
    Config.cpp
    JSON.cpp
    Buttons.cpp
    Outputs.cpp
    Accel.cpp
    Nudge.cpp
    NightMode.cpp
    BootselButton.cpp
    Watchdog.cpp
    PicoLED.cpp
    StatusRGB.cpp
    XInput.cpp
    USBCDC.cpp
    VendorIfc.cpp
    Reset.cpp
    I2C.cpp
    SPI.cpp
    PIOHelper.cpp
    GPIOManager.cpp
    PWMManager.cpp
    ADCManager.cpp
    TVON.cpp
    Devices/Accel/LIS3DH.cpp
    Devices/Accel/LIS3DSH.cpp
    Devices/Accel/MC3416.cpp
    Devices/Accel/MXC6655XA.cpp
    Devices/ADC/ADS1115.cpp
    Devices/ADC/PicoADC.cpp
    Devices/DistanceSensor/VL6180X.cpp
    Devices/GPIOExt/PCA9555.cpp
    Devices/LinearPhotoSensor/TCD1103.cpp
    Devices/LinearPhotoSensor/TSL1410R.cpp
    Devices/ProxSensor/VCNL4010.cpp
    Devices/PWM/TLC59116.cpp
    Devices/PWM/TLC5940.cpp
    Devices/PWM/PCA9685.cpp
    Devices/PWM/PWMWorker.cpp
    Devices/Quadrature/QuadratureEncoder.cpp
    Devices/Quadrature/AEDR8300.cpp
    Devices/RTC/I2CRTCBase.cpp
    Devices/RTC/DS1307.cpp
    Devices/RTC/DS3231M.cpp
    Devices/RTC/RV3032C7.cpp
    Devices/ShiftReg/74HC595.cpp
    Devices/ShiftReg/74HC165.cpp
    IRRemote/IRCommand.cpp
    IRRemote/IRProtocols.cpp
    IRRemote/IRReceiver.cpp
    IRRemote/IRTransmitter.cpp
    Plunger/Plunger.cpp
    Plunger/ZBLaunch.cpp
    Plunger/LinearPhotoSensorPlunger.cpp
    Plunger/PotPlunger.cpp
    Plunger/ProxPlunger.cpp
    Plunger/QuadraturePlunger.cpp
)

# PIO headers
pico_generate_pio_header(PinscapePico PWM_pio.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TCD1103_FMpos.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TCD1103_FMneg.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TCD1103_SHpos.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TCD1103_SHneg.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TCD1103_ICGpos.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TCD1103_ICGneg.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TSL1410R_SI.pio)
pico_generate_pio_header(PinscapePico Devices/LinearPhotoSensor/TSL1410R_CLK.pio)
pico_generate_pio_header(PinscapePico Devices/PWM/TLC5940_pio.pio)
pico_generate_pio_header(PinscapePico Devices/ShiftReg/74HC165_pio.pio)
pico_generate_pio_header(PinscapePico Devices/ShiftReg/74HC595_pwm_pio.pio)
pico_generate_pio_header(PinscapePico Devices/ShiftReg/74HC595_dig_pio.pio)

# Explicitly add the source directory to the #include list.  This seems
# redundant, but it's required because some tinyusb headers #include
# headers that have to be provided by the client program, as a way of
# injecting client definitions into library headers.
target_include_directories(PinscapePico PUBLIC .)


# libraries
target_link_libraries(
    PinscapePico
    pico_stdlib
    pico_stdio_uart
    pico_unique_id
    pico_multicore
    pico_time
    pico_flash
    pico_platform
    hardware_adc
    hardware_exception
    hardware_i2c
    hardware_spi
    hardware_uart
    hardware_gpio
    hardware_pwm
    hardware_flash
    hardware_watchdog
    hardware_dma
    hardware_pio
    hardware_sync
    hardware_rtc
    pico_sync
    tinyusb_device
    tinyusb_board
)


# create map/bin/hex files
pico_add_extra_outputs(PinscapePico)

# Disable stdio on all device interfaces (UART and USB).  We use our
# own custom logging mechanism in place of stdio, and we don't want
# stdio claiming either type of device.
#pico_enable_stdio_uart(PinscapePico 0)
#pico_enable_stdio_usb(PinscapePico 0)
