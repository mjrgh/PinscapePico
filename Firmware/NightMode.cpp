// Pinscape Pico - Night Mode
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <pico/flash.h>
#include <pico/unique_id.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>

// project headers
#include "NightMode.h"

// Night Mode object singleton
NightModeControl nightModeControl;

NightModeControl::NightModeControl()
{
}

NightModeControl::~NightModeControl()
{
}

void NightModeControl::Set(bool newState)
{
    // check for a state change
    if (newState != state)
    {
        // udpate the internal state
        state = newState;
        
        // notify subscribers
        for (auto &e : eventSinks)
            e->OnNightModeChange(newState);
    }
}

void NightModeControl::Subscribe(NightModeEventSink *eventSink)
{
    // add the subscriber to the list
    eventSinks.emplace_back(eventSink);
}

void NightModeControl::Unsubscribe(NightModeEventSink *eventSink)
{
    // Check that 'this' is non-null, to allow for safe shutdown when the
    // NightModeControl destructor is called before one or more event sink
    // object destructors.  In principle, such a thing should never happen,
    // because constructor and destructor calls should always be in nested
    // order, but there are some valid idioms in C++ where the compiler
    // chooses the order and it's not easy for the programmer to override
    // it.  We can be more robust against such cases by allowing for the
    // possibility that our singleton was deleted before all of its
    // subscribers were.  There's no harm in ignoring the unsubscribe
    // call in this case, because it's just removing an entry from a list
    // that no longer exists.
    if (this != nullptr)
        eventSinks.remove(eventSink);
}

NightModeControl::NightModeEventSink::~NightModeEventSink()
{
    // automatically unsubscribe
    nightModeControl.Unsubscribe(this);
}
