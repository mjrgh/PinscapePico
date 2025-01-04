// Pinscape Pico - PWM Worker Program - Utilities
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#pragma once
#include <pico/sync.h>
#include <hardware/irq.h>

// array element count
#define _countof(x) (sizeof(x)/sizeof((x)[0]))

//
// Disable interrupts within a scope.  Instantiate this object to block
// interrupts for the duration of the current scope, and automatically
// restore the original interrupt flags on exit.
class IRQDisabler
{
public:
    IRQDisabler() { istat = save_and_disable_interrupts(); }
    ~IRQDisabler() { restore_interrupts(istat); }

    // original interrupt status
    uint32_t istat;
    // explicitly restore interrupts early, before exiting the scope
    void Restore() { restore_interrupts(istat); }
};

