// Pinscape Pico - Pico reset/reboot routines
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <hardware/watchdog.h>
#include <hardware/resets.h>

// local project headers
#include "Pinscape.h"
#include "Reset.h"
#include "PicoBoardType.h"
#include "Outputs.h"
#include "Config.h"
#include "TimeOfDay.h"
#include "StatusRGB.h"


// global singleton
PicoReset picoReset;

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


// Reboot the Pico.  This resets the Pico hardware, and re-enters the
// firmware program at our main entrypoint.  If a reset lock has been
// set, this only sets internal flags and returns; the actual reset
// will be performed in the Task() handler.
//
// The user can always manually override a regular boot-into-flash reset
// and invoke the ROM boot loader instead by holding down the BOOTSEL button
// (the small pushbutton located on the top of the Pico) during the reset.
// There's no way for the software to prevent this (by design: this makes
// the Pico un-brickable by ensuring that the user can always invoke the
// boot loader, no matter what kind of errnat or malicious code is loaded
// into the flash.)  BOOTSEL can't *initiate* a reset, though; it just
// controls which program (ROM boot loader or the program installed in
// flash) starts when a reset occurs, and the state of the button only
// matters at the moment of a reset (so pushing BOOTSEL while the flash
// program is running has no effect at all; the Pico must be in the
// process of rebooting for BOOTSEL to do anything).  A user can manually
// initiate a reset simply by power-cycling the Pico (by unplugging it
// from USB and plugging it back in; to manually invoke the boot loader,
// then, unplug the Pico and plug it back in while holding down BOOTSEL).
// The Pinscape Pico expansion board (as of the current design iteration)
// also features a manual reset button, so pressing that button while
// holding down BOOTSEL on the Pico will also invoke the boot loader.
void PicoReset::Reboot(bool bootLoaderMode, BootMode mode)
{
    // set the reboot flags
    this->resetPending = true;
    this->bootLoaderMode = bootLoaderMode;

    // set the pending firmware mode
    SetNextBootMode(mode);

    // check if it's safe to reboot immediately
    TryReset();
}

// Try performing the pending reset now.  We assume that the caller has
// already determined that a reset is indeed pending, so we don't check
// again.
void PicoReset::TryReset()
{
    // check for reset locks - if any locks are currently asserted,
    // skip the reset for now
    for (auto *lock : locks)
    {
        if (lock->IsLocked())
            return;
    }

    // Perform pre-reboot actions
    PrepareForReboot();

    // Save the time-of-day information, so that we can restore it
    // immediately after the reset.
    //
    // It's unlikely that the information will survive a Boot Loader
    // traversal, but it doesn't do any harm to try.  (It's unlikely to
    // survive the Boot Loader for two reasons.  The first is that the
    // boot loader program might scribble over the RAM area where we
    // save the data, since the boot loader is itself an RP2040 program
    // that has access to the same RAM space.  The second is that
    // invoking the boot loader usually means we're going to install a
    // new version of *this* program, and it's unlikely that the updated
    // version will be linked with the saved time data in exactly the
    // same RAM location.)
    timeOfDay.SaveBeforeReset();

    // reboot into the desired mode
    if (bootLoaderMode)
    {
        // Reset into the USB boot loader
        reset_usb_boot(PicoBoardType::LED::GetResetUSBBootMask(), 0);
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
        while (true) { }
    }
}

// Prepare for a software reset.  This should be called before resetting
// the Pico either to restart the firmware program or to invoke the ROM
// boot loader.  This routine should attempt to switch all controlled
// feedback devices off to ensure that nothing is left activated through
// the reset.  This is especially important when resetting into the ROM
// boot loader, since the boot loader has no knowledge of our controlled
// devices and thus no way to switch them off.
void PicoReset::PrepareForReboot()
{
    // turn off all outputs and physically disable peripheral output ports
    OutputManager::AllOff();
    OutputManager::EnablePhysicalOutputs(false);

    // turn off the status LED
    statusRGB.Enable(false);
}

// create a reset lock
PicoReset::Lock *PicoReset::CreateLock()
{
    return locks.emplace_back(new Lock());
}

// --------------------------------------------------------------------------
//
// Reset locker
//

void PicoReset::Lock::SetLocked(bool locked)
{
    // if we're changing from locked to unlocked, check to see if
    // a reset is pending and if we can finally apply it, by calling
    // the periodic task routine
    if (isLocked && !locked)
        picoReset.Task();

    // update the lock status
    isLocked = locked;
}
