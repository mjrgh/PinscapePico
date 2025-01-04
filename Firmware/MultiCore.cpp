// Pinscape Pico - Flash Data Storage
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Dual-Core operations

#include <stdlib.h>
#include <stdint.h>
#include <functional>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <pico/flash.h>
#include "Utils.h"
#include "MultiCore.h"

// Flag: the second core is running
static volatile bool secondCoreRunning = false;

// Invoke a callback in a flash-safe execution context.  If the second
// core has been enabled, this uses flash_safe_execute().  Otherwise, it
// simply disables interrupts and invokes the callback.  Returns the
// PICO_xxx status code from flash_safe_execute() if that's used,
// otherwise simply returns PICO_OK.
int FlashSafeExecute(std::function<void()> callback, uint32_t timeout_ms)
{
    // check if the second core is running
    if (secondCoreRunning)
    {
        // The second core is running - use flash_safe_execute()
        return flash_safe_execute([](void *pv)
        {
            std::function<void()> *callback = reinterpret_cast<std::function<void()>*>(pv);
            (*callback)();
        }, &callback, timeout_ms);
    }
    else
    {
        // this is the only active core, so just call with interrupts
        // disabled
        IRQDisabler irqd;
        callback();

        // success
        return PICO_OK;
    }
}

// Launch the second core thread.  This initializes the second core
// program for flash_safe_execute cooperation and invokes the caller's
// main() function.
void LaunchSecondCore(void (*main)())
{
    // Stash the callback in a static that we can access from our second
    // thread wrapper routine.  Note that we can get away with a simple
    // static variable here because the RP2040 has exactly two cores,
    // hence there's only one secondary core.  If we wanted to
    // generalize this to N cores, we could use ThunkManager instead to
    // dynamically create a new context for each additional core.  But
    // that's not necessary for this CPU.
    static void (*s_main)() = main;

    // launch the second core using our wrapper
    multicore_launch_core1([]()
    {
        // Initialize flash memory access safety for this core.  This allows
        // the primary core to temporarily lock out this (secondary) core in
        // order to write to flash memory, without any danger that the
        // secondary core will access flash while the write is occurring.
        // Writing to flash requires exclusive access, which precludes either
        // core attempting to fetch instructions from flash, which can happen
        // if either core tries to execute within code mapped into flash space
        // by the linker.  The lockout ensures that the secondary core halts
        // while the flash write is processing.  This initializaing call sets
        // up an interrupt handler on this core that allows the main core to
        // enforce a lockout at any time, without any further coordination
        // in the secondary core program code.
        flash_safe_execute_core_init();

        // flag that the second core is running
        secondCoreRunning = true;

        // invoke the caller's thread entrypoint function
        s_main();

        // deinitialization the flash lockout handler
        flash_safe_execute_core_deinit();

        // flag that the second core is no longer running
        secondCoreRunning = false;
    });

    // Spin until the second core starts up and initializes its flash-
    // safe-execute cooperation.  This ensures that we can't have a race
    // condition if the caller immediately attempts a flash operation on
    // return, which could conceivably happen before the second finishes
    // its setup if we didn't wait.  Don't wait forever, though, just in
    // case the other core immediately crashes out.
    uint64_t timeout = time_us_64() + 10000;
    while (!secondCoreRunning && time_us_64() < timeout)
        sleep_us(50);
}
