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
# Optionally, you can also define the target board to build for:
#
#   cmake -D PICO_BOARD:STRING=pico -S . -G "NMake Makefiles"
#   cmake -D PICO_BOARD:STRING=pico2 -S . -G "NMake Makefiles"
#
# cmake doesn't actually build the Pinscape firmware.  It's just
# a "metabuild" program, whose job is to create the scripts tha
# build the software.  The actual build is performed by NMAKE,
# which you run by typing simply:
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

# Note: this can be overridden on the CMAKE command line using
# the -D option, as in:
#
#   cmake -D PICO_BOARD:STRING=pico2 ...
#
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.12)

# ------ SDK pioasm build "environment leakage" workaround ----------
set(pioasm_FOUND TRUE CACHE INTERNAL "Force bypass early check" FORCE)
set(Pioasm_FOUND TRUE CACHE INTERNAL "Force bypass early check" FORCE)

find_program(PORTABLE_PIOASM_EXE 
    NAMES pioasm pioasm.exe
    PATHS 
        "$ENV{PICO_SDK_PATH}/tools/pioasm"
    NO_DEFAULT_PATH # Prioritize our adjacent local project targets first
)

if(PORTABLE_PIOASM_EXE)
    if(NOT TARGET Pioasm)
        add_executable(Pioasm IMPORTED GLOBAL)
        set_property(TARGET Pioasm PROPERTY IMPORTED_LOCATION "${PORTABLE_PIOASM_EXE}")
        add_executable(pioasm ALIAS Pioasm)
    endif()
    message(STATUS "Portable SDK Bypass: Linked local pioasm binary from: ${PORTABLE_PIOASM_EXE}")
else()
    message(FATAL_ERROR "Portable pioasm bypass failed: No compiled pioasm binary found in adjacent paths.")
endif()
# ------- end SDK workaround ---------

include(pico_sdk_import.cmake)

# project declaration
project(PinscapePico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(PICO_FIRMWARE_BUILD=1)
add_compile_definitions(PICO_MAX_SHARED_IRQ_HANDLERS=8)
add_compile_definitions(PICO_PANIC_FUNCTION=PinscapePanic)

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
    PicoBoardType_${PICO_BOARD}.cpp
    Version.cpp
    Utils.cpp
    ButtonHelper.cpp
    MultiCore.cpp
    FaultHandler.cpp
    m0FaultDispatch.c
    ThunkManager.cpp
    FlashStorage.cpp
    ExpansionBoard.cpp
    TimeOfDay.cpp
    TimeRange.cpp
    USBIfc.cpp
    Keyboard.cpp
    Gamepad.cpp
    OpenPinballDevice.cpp
    FeedbackController.cpp
    LedWizIfc.cpp
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
    Devices/Accel/MMA8451Q.cpp
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
    Devices/PWM/TLC5947.cpp
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
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/PWM_pio.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TCD1103_FMpos.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TCD1103_FMneg.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TCD1103_SHpos.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TCD1103_SHneg.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TCD1103_ICGpos.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TCD1103_ICGneg.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TSL1410R_SI.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/LinearPhotoSensor/TSL1410R_CLK.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/PWM/TLC5940_pio.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/PWM/TLC5947_pio.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/ShiftReg/74HC165_pio.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/ShiftReg/74HC595_pwm_pio.pio)
pico_generate_pio_header(PinscapePico ${CMAKE_CURRENT_LIST_DIR}/Devices/ShiftReg/74HC595_dig_pio.pio)

# Explicitly add the source directory to the #include list.  This seems
# redundant, but it's required because some tinyusb headers #include
# headers that have to be provided by the client program, as a way of
# injecting client definitions into library headers.
target_include_directories(PinscapePico PUBLIC .)


# libraries
target_link_libraries(
    PinscapePico
    pico_stdlib
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
    pico_sync
    tinyusb_device
    tinyusb_board
)

# Disable the RP2350-E10 erratum workaround, which the SDK cmake
# scripts apply by default.  E10 is a significant bug in early
# steppings of the Pico 2 ROM bootloader that made the Pico UF2
# ignore image downloads when a partition table is present on
# the device, making it impossible to install normal UF2 files.
# The workaround inserts some extra UF2 data that happens to get
# the errant bootloader into the right initial state to avoid
# the bug and properly install the UF2 data.
#
# The workaround has a seriously bad side effect, though, which
# is that it erases the last sector of flash.  That's bad for us
# because that's where we store our mini file system's central
# directory data, so when the E10 workaround is enabled in the
# build, it produces a UF2 that deletes all of our in-flash
# configuration data (JSON, plunger calibration, etc) on every
# firmware update.
#
# Fortunately, WE DON'T NEED THE WORKAROUND AT ALL, because we
# don't use the Pico 2 partition table scheme.  The E10 bug
# only occurs when a partition table is present AND the UF2
# file targets a partition.  We don't need partitions, so we
# can prepare our file in "absolute" format, which doesn't
# trigger the bug.
#
# Unfortunately, though, the SDK cmake scripts don't have a way
# to turn off the workaround in isolation.  The workaround is
# just hard-coded into the SDK 'picotool' command that generates
# the UF2 file.  So to disable the workaround, we have to disable
# the entire 'picotool' UF2 generator command that we'd normally
# inherit from the SDK cmake scripts, and manually insert our
# own version of the command at the end of the build process.
set(PICO_NO_PICOTOOL 1)

# Add the commands to create the create map/bin/hex files.
# Normally, these would include the 'picotool' commands that
# converts the .bin file to the final .uf2 file, but we disabled
# picotool above, so this will only gets us as far as the .bin
# file.
pico_add_extra_outputs(PinscapePico)

# Now add our custom UF2 generator, to convert the .bin file
# containing the program image to the UF2 format that we can use
# with the Pico bootloader.  This command would normally be included
# in the commands added to the build by pico_add_extra_outputs()
# above, but we intentionally disabled the default UF2 generator
# command that so that we could customize the command line to
# remove the bug E10 workaround, as described above.
#
# For the original Pico, the UF2 family is always 'rp2040'.
#
# For Pico 2, we must store the UF2 sectors in 'absolute' format
# so that we won't trigger the E10 bug if a sector table is present.
# And having done that, we don't need the "--abs-block" part of the
# default SDK command that would work around the E10 bug by
# erasing the last sector of flash (and all of the user's stored
# config data along with it).
add_custom_command(TARGET PinscapePico POST_BUILD
    COMMAND picotool
    ARGS uf2 convert
        --quiet
        ${uf2_package_args}
        PinscapePico.bin
        PinscapePico.uf2
        --family $<IF:$<STREQUAL:${PICO_BOARD},pico2>,absolute,rp2040>
    COMMAND_EXPAND_LISTS
    VERBATIM
    BYPRODUCTS "PinscapePico.uf2")


# Disable stdio on all device interfaces (UART and USB).  The Pico
# SDK provides a stdio implementation that can optionally present a
# terminal-style interface on a UART and/or a USB virtual COM (CDC)
# port.  We don't need or want the SDK console interface, so disable
# it in the build.
#
# Note that we must EXPLICITLY DISABLE the stdio console options,
# because the SDK's base build configuration enables them by default.
# The console features are actively harmful when enabled, because
# they'll claim certain resources (GP0, GP1, and UART0), interfering
# with our own use of the resources.  This can happen EVEN IF we never
# call any stdio console functions, because some SDK libraries (Tinyusb
# in particular) will initialize stdio on their own if stdio is enabled.
# Disabling the console features at build time prevents this.
pico_enable_stdio_uart(PinscapePico 0)
pico_enable_stdio_usb(PinscapePico 0)

