// Pinscape Pico - PIO helpers
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/pio.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "PIOHelper.h"

// global singletons for the helpers per physical Pico PIO unit
PIOHelper pioHelper0(pio0);
PIOHelper pioHelper1(pio1);

// initialize
void PIOHelper::Init()
{
    // install our exclusive interrupt handlers
    irq_set_exclusive_handler(PIO0_IRQ_0, &PIOHelper::PIO0_IRQ0);
    irq_set_exclusive_handler(PIO0_IRQ_1, &PIOHelper::PIO0_IRQ1);
    irq_set_exclusive_handler(PIO1_IRQ_0, &PIOHelper::PIO1_IRQ0);
    irq_set_exclusive_handler(PIO1_IRQ_1, &PIOHelper::PIO1_IRQ1);
}

void PIOHelper::RegisterIRQ0(PIOInterruptHandler *handler)
{
    // add to the handler to the list, with interrupts disabled in
    // case an interrupt occurs during this routine (in which case
    // the IRQ handler would inspect the list, which could be in
    // an inconsistent state during the manipulation)
    IRQDisabler irqDisabler;
    intHandlersIRQ0.emplace_back(handler);
}

void PIOHelper::RegisterIRQ1(PIOInterruptHandler *handler)
{
    IRQDisabler irqDisabler;
    intHandlersIRQ1.emplace_back(handler);
}


// interrupt handler
void PIOHelper::IRQ(IntHandlerList &handlers)
{
    // note entry time for statistics
    uint64_t t0 = time_us_64();
    
    // Clear the PIO interrupt flags for this PIO.  We clear ALL of the
    // flags up front, before dispatching to any of the registered
    // handlers.  Each handler is responsible for checking the
    // underlying condition that would trigger its interrupt, so the
    // handlers don't need to know the flags.  Indeed, they can't rely
    // on the IRQ flags anyway, because the flags are in the PIO, and
    // thus shared among all of the state machines on the PIO, which
    // might be running unrelated programs that don't know about each
    // other.  So when an interrupt occurs, the handler can only tell if
    // it actually has work to do (as opposed to the interrupt coming
    // from some other PIO program that happens to also use the same
    // IRQ flag) by checking the underlying condition.  For example, it
    // could check its FIFO status, or a DMA channel's status, or a GPIO
    // pin state.
    //
    // Clearing the IRQ flags FIRST, before dispatching to the
    // handlers, avoids a race between the state machines and the
    // interrupt dispatcher.  Remember that the PIO state machines are
    // freely running while the IRQ handler is active - they're separate
    // cores with their own clocks, and an interrupt on the main CPU
    // doesn't interrupt the PIOs.  If one of the state machines raises
    // a NEW interrupt while the interrupt handler is running, the new
    // flag will remain in effect when the handler exits, which will
    // immediately trigger another call to the interrupt handler.  If a
    // state machine raises a new interrupt JUST BEFORE we clear the
    // flags, the new interrupt will *still* be handled properly,
    // because we haven't even started to check for actual work at that
    // point.  That's why it's so vital to clear the IRQ flags as the
    // first step - that way, it's guaranteed that we'll either service
    // the new work on THIS invocation of the handler, because the work
    // was already available at the time we cleared the IRQ flags, OR
    // we'll service it on an immdiate re-invocation, because the work
    // became available between the time we checked for it and the time
    // we returned, during which we leave any new flags as-is.
    //
    // The PIO IRQ register uses the set-to-clear hardware idiom: i.e.,
    // 'write a 1' bit to clear the corresponding register bit.
    pio->irq = 0xFFFFFFFF;

    // now dispatch to the registered instance handlers
    for (auto handler : handlers)
        handler->PIO_IRQ();

    // collect statistics
    stats.n += 1;
    stats.t += (time_us_64() - t0);
}

