// Pinscape Pico - BOOTSEL button reader
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Reads the state of the BOOTSEL button.  Based on sample code provided by
// Raspberry Pi, at github.com/raspberrypi/pico-examples/

#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/flash.h>
#include <hardware/gpio.h>
#include <hardware/sync.h>
#include <hardware/structs/ioqspi.h>
#include <hardware/structs/sio.h>
#include "BootselButton.h"
#include "MultiCore.h"

// Important: this routine must be called from a "flash-safe" context:
// interrupts must be disabled, and the other CPU core must be idling in
// a non-flash spin loop for the duration of the call.  The BOOTSEL
// button is wired to the on-board flash's /CS (Chip Select) line, so
// reprogramming the BOOTSEL GPIO as an input (which we must do to read
// the button state) prevents the Pico's XIP (execute-in-place) module
// from accessing the flash for opcode fetches, hence the CPU would fault
// if it tried.  To prevent this, we can only execute code from RAM for
// the duration of the test.
static bool __no_inline_not_in_flash_func(ReadBOOTSELInternal)()
{
    const uint CS_PIN_INDEX = 1;

    // Set the flash Chip Select to Hi-Z by making it an input
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Wait for that to settle - use a spin loop rather than an SDK sleep
    // routine, to make absolutely sure we don't enter flash code space
    for (volatile int i = 0; i < 1000; ++i);

    // The HI GPIO registers in SIO can observe and control the 6 QSPI pins.
    // The BOOTSEL button pulls the pin low when pressed.
    bool buttonState = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    // Restore GPIO output control over the flash Chip Select pin
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Return the button state
    return buttonState;
}

// Statistics
BootselButton::Stats BootselButton::stats;

// Read the BOOTSEL button state
bool BootselButton::Read()
{
    // note the start time
    uint64_t t0 = time_us_64();

    // read the button state in a flash-safe context
    bool buttonState = false;
    if (FlashSafeExecute([&buttonState]() { buttonState = ReadBOOTSELInternal(); }, 1) != PICO_OK)
        buttonState = false;

    // count it for the statistics
    stats.tSum += (time_us_64() - t0);
    stats.nCalls += 1;

    // return the result
    return buttonState;
}
