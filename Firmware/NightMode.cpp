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
#include "CommandConsole.h"

// Night Mode object singleton
NightModeControl nightModeControl;

NightModeControl::NightModeControl()
{
}

NightModeControl::~NightModeControl()
{
}

void NightModeControl::Init()
{
    // set up our command handler
    CommandConsole::AddCommand(
        "nightmode", "show/set Night Mode status",
        "nightmode [options]\n"
        "options:\n"
        "  -s, --show     show current status (default if no options specified)\n"
        "  --on           activat Night Mode\n"
        "  --off          deactivate Night Mode\n",
        Command);
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

// console command handler
void NightModeControl::Command(const ConsoleCommandContext *c)
{
    static const auto Show = [](const ConsoleCommandContext *c) {
        c->Printf("Night mode is currently %s\n", nightModeControl.state ? "ON" : "OFF");
    };

    // default with no options -> show status
    if (c->argc == 1)
        return Show(c);

    // parse arguments
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--show") == 0)
        {
            Show(c);
        }
        else if (strcmp(a, "--on") == 0)
        {
            nightModeControl.Set(true);
            c->Printf("Night Mode changed to ON\n");
        }
        else if (strcmp(a, "--off") == 0)
        {
            nightModeControl.Set(false);
            c->Printf("Night Mode changed to OFF\n");
        }
        else
        {
            c->Printf("nightmode: unknown option \"%s\"\n", a);
            return;
        }
    }
}
