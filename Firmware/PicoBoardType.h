// Pinscape Pico firmware - Pico board type abstraction layer
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module defines an abstract interface to functions that vary across
// Pico hardware implementations.  The Pico has many variations, including
// official variants from Raspberry Pi (Pico W, Pico 2, Pico 2W) and numerous
// third-party versions.  All Pico variants are *mostly* the same, since
// they're all based on the RP2040 or RP2350 system-on-chip, which integrates
// nearly all system components, including RAM and many I/O peripherals.
// But there are a couple of key components that the SOC lacks and that all
// Pico boards must thus provide off-chip, including flash memory and power
// management.  In addition, nearly all Pico boards include some kind of
// status indicator LED, and some have additional unique peripherals, such
// as the wireless transponder chip on the Pico W and 2W.  Most of these
// external peripherals require Pico GPIO pin connections for control and
// status I/O - and that's where the different boards manifest differences
// in the programming model.  The official Pico SDK provides abstractions
// for *some* of these differences, but not all of them.  That's where this
// file comes in: this is where we define our own abstractions to handle
// variations in the programming model that matter to the Pinscape software.
//
// This abstraction layer is divided into an INTERFACE, which is common
// across all board variants, and an IMPLEMENTATION, which must be written
// separately for each board variant that has differences in how it handles
// the functionality defined in the interface.  All of the Pinscape code
// works in terms of the abstract interface, so that the Pinscape code
// doesn't contain any dependencies on different boards.  As a matter of
// policy, we try to avoid EVER writing code of the form "if the board is
// a Pico 2W, then...".  Instead, the Pinscape code should always work in
// terms of the abstractions defined here, so that it doesn't contain any
// assumptions about differences among the boards, and leave it to the
// per-board implementation layer to carry out the function defined in the
// abstraction in the correct way for the actual board targeted in the
// build.  The implementation layer is baked into the build at the LINKER
// level, so all of the testing along the lines of "if the board is a 2W
// then..." should be isolated to CMakeLists.txt.  So the interfaces
// defined below will be implemented in more than one .cpp file.  Each
// target board will have its own .cpp file that implements the interfaces,
// and the build will link in the one appropriate .cpp file based on the
// target board selected in the build configuration.
//
// The implementation .cpp file is selected by NAMING CONVENTION in
// the build.  The standard SDK build system defines a cmake variable
// named PICO_BOARD, which is how the build system figures out which
// version of the official SDK library to include in the build.  We
// use the same variable to select our implementation file, by naming
// it in this form:
//
//   PicoBoardType_$(PICO_BOARD).cpp
//
// For the official base Pico RP2040, PICO_BOARD is set to "pico", so
// the implementation file is PicoBoardType_pico.cpp.  New boards can
// be added to the build by creating a file using the same naming
// convention, based on the PICO_BOARD setting in the SDK build that
// corresponds to that board.
//

// standard library headers
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// Board type abstraction object.  This collects the abstraction interfaces
// under a single name.
class PicoBoardType
{
public:
    // Initialize the target board.  This carries out any special
    // startup code required for the target.  This is called early
    // in the main Pinscape firmware startup code on the primary
    // core.
    static void Init();

    // Status LED.  This is one of the major differences in target
    // boards.  Many of the third-party boards thought it would be
    // nice to add a WS2812B RGB LED in place of the original Pico's
    // boring green LED, and some boards (notably the W and 2W)
    // repurposed the GPIO that's assigned to the boring green LED
    // on the original Pico to control a different peripheral.  So
    // you can't control the status LED just by writing to GPIO 25
    // (the GPIO where the original boring green LED is connected).
    // This abstracts the LED functionality that Pinscape uses.
    class LED
    {
    public:
        // Initialize the LED.  For the original Pico, this claims
        // GPIO 25 as a digital output.  For a W or 2W, this sets
        // things up so that we can access the wireless chip pin
        // where the LED is connected on that platform.  For third-
        // party devices with RGB LEDs, this should do whatever
        // I/O initialization is needed to access the LED.
        static void Init();

        // Update the LED state to ON (true) or OFF (false).
        static void Write(bool state);

        // Get the GPIO mask for the activity LED for reset_usb_boot().
        // For platforms such as the original Pico where an LED is
        // hardwired to a GPIO, this should return a mask of the
        // form (1 << PICO_DEFAULT_LED_PIN).  For platforms where
        // the LED can't be accessed directly through a GPIO, this
        // should simply return 0.
        static uint32_t GetResetUSBBootMask();
    };
};
