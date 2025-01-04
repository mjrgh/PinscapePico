// Pinscape Pico - PIO helpers
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/pio.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"


// global singletons for the helpers per physical Pico PIO unit
class PIOHelper;
extern PIOHelper pioHelper0;
extern PIOHelper pioHelper1;

// PIO interrupt handler interface.  Classes that make use of the PIO
// helper's features must implement this interface.
class PIOInterruptHandler
{
public:
    virtual void PIO_IRQ() = 0;
};

// PIO helper object
class PIOHelper
{
public:
    PIOHelper(PIO pio) : pio(pio) { }

    // get the helper instance for a given hardware PIO unit by hw struct or unit number
    static PIOHelper *For(PIO pio) { return pio == pio0 ? &pioHelper0 : pio == pio1 ? &pioHelper1 : nullptr; }
    static PIOHelper *For(int unitNo) { return unitNo == 0 ? &pioHelper0 : unitNo == 1 ? &pioHelper1 : nullptr; }

    // Initialization.  The main loop must call this at startup to install
    // the PIO interrupt handlers.
    static void Init();

    // Register an interrupt handler
    void RegisterIRQ0(PIOInterruptHandler *handler);
    void RegisterIRQ1(PIOInterruptHandler *handler);

    // registered interrupt handler objects
    using IntHandlerList = std::list<PIOInterruptHandler*>;
    IntHandlerList intHandlersIRQ0;
    IntHandlerList intHandlersIRQ1;

    // Static interrupt handlers
    static void PIO0_IRQ0() { pioHelper0.IRQ(pioHelper0.intHandlersIRQ0); }
    static void PIO0_IRQ1() { pioHelper0.IRQ(pioHelper0.intHandlersIRQ1); }
    static void PIO1_IRQ0() { pioHelper1.IRQ(pioHelper1.intHandlersIRQ0); }
    static void PIO1_IRQ1() { pioHelper1.IRQ(pioHelper1.intHandlersIRQ1); }
    void IRQ(IntHandlerList &handlers);

    // underlying SDK hardware interface
    PIO pio;

    // interrupt time statistics
    struct
    {
        uint64_t n = 0;   // number of calls to IRQ handler
        uint64_t t = 0;   // cumulative time spent in IRQ, in microseconds
        float AvgTime() const { return static_cast<float>(static_cast<double>(t) / static_cast<double>(n)); }
    } stats;
};
