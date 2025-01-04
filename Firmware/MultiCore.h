// Pinscape Pico - Flash Data Storage
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Dual-Core operations

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <functional>
#include <pico/stdlib.h>
#include <pico/multicore.h>

// Launch the second core thread.  This initializes the second core
// program for flash_safe_execute cooperation and invokes the caller's
// main() function.
void LaunchSecondCore(void (*main)());

// Invoke a callback in a flash-safe execution context.  If the second
// core has been enabled, this uses flash_safe_execute().  Otherwise, it
// simply disables interrupts and invokes the callback.  Returns the
// PICO_xxx status code from flash_safe_execute() if that's used,
// otherwise simply returns PICO_OK.
//
// All Pinscape Pico code should use this instead of calling the SDK's
// flash_safe_execute() function directly.  The SDK function has no way
// to know if we've launched the second core thread yet, so it will wait
// for the other core unconditionally, and fail with a timeout if the
// other core isn't running.  In contrast, our version knows whether or
// not to wait for the other core, so it's safe to call at any time.
int FlashSafeExecute(std::function<void()> callback, uint32_t timeout_ms);
