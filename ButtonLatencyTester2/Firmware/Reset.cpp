// Pinscape Pico - Pico reset/reboot routines
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>
#include <hardware/watchdog.h>
#include <hardware/resets.h>

// local project headers
#include "../../Firmware/Pinscape.h"
#include "../../Firmware/Reset.h"

// global singleton
PicoReset picoReset;

// Reboot the Pico
void PicoReset::Reboot(bool bootLoaderMode, BootMode mode)
{
    // set the pending firmware mode
    SetNextBootMode(mode);

    // reboot into the desired mode
    if (bootLoaderMode)
    {
        // halt the second core
        multicore_reset_core1();
        multicore_launch_core1([](){
            // sleep for 2s, and repeat the Boot Loader mode reset from
            // this core if we're still running
            for (int i = 0 ; i < 2000 ; ++i) sleep_us(1000);
            reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
        });

        // Reset into the USB boot loader
        sleep_us(1000);
        reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
    }
    else
    {    
        // Reset the Pico and restart the resident flash program.  The
        // official way to do this on Pico is to enable the watchdog
        // timer with a zero timeout, which immediately triggers a
        // system reset.  Going through the watchdog (rather than just
        // triggering a CPU reset) ensures that all of the Pico's
        // on-board peripherals are reset and then started up in the
        // correct order.
        watchdog_enable(0, false);
    }

    // we shouldn't reach this point, but just in case...
    while (true) { }
}

// Magic numbers for the scratch registers, used as an integrity
// check.  Garbage or conflicting use of the registers is unlikely
// to match these bit patterns.
const uint32_t SCRATCH0_MAGIC = 0x76E8C136;
const uint32_t SCRATCH1_MAGIC = 0xD1C76856;

// Set the next boot mode in the watchdog scratch registers
void PicoReset::SetNextBootMode(BootMode mode)
{
    // store the uint32 value of the mode enum, XOR'd with the magic
    // numbers, in our two scratch registers
    uint32_t n = static_cast<int>(mode);
    watchdog_hw->scratch[0] = n ^ SCRATCH0_MAGIC;
    watchdog_hw->scratch[1] = n ^ SCRATCH1_MAGIC;        
}

// Get the boot mode from the scratch registers
PicoReset::BootMode PicoReset::GetBootMode()
{
    // Get the two scratch register values, and make sure they
    // match each other.  If they don't, the integrity check
    // fails, so return Unknown.
    uint32_t a0 = watchdog_hw->scratch[0] ^ SCRATCH0_MAGIC;
    uint32_t a1 = watchdog_hw->scratch[1] ^ SCRATCH1_MAGIC;
    return a0 == a1 ? static_cast<BootMode>(a0) : BootMode::Unknown;
}

