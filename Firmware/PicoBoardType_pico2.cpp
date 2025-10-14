// Pinscape Pico - Pico Board Type for official Pico 2 (RP2350)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implements tyhe PicoBoardType interface for the official Pico 2
// (RP2350) from Rasbperry Pi.  This file is included in the build when PICO_BOARD
// is set to "pico2" in the build configuration.


#include "PicoBoardType.h"


// Additional target-board-specific intialization
void PicoBoardType::Init()
{
    // no additional initialization required
}

// LED initialization
void PicoBoardType::LED::Init()
{
    // set up the standard Pico LED
    gpioManager.Claim("Pico LED", PICO_DEFAULT_LED_PIN);
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

// LED write
void PicoBoardType::LED::Write(bool state)
{
    gpio_put(PICO_DEFAULT_LED_PIN, state);
}

uint32_t PicoBoardType::LED::GetResetUSBBootMask()
{
    return 1UL << PICO_DEFAULT_LED_PIN;
}
