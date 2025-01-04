// Pinscape Pico - Base class for quadrature encoders
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "GPIOManager.h"
#include "ThunkManager.h"
#include "QuadratureEncoder.h"

// reset the counter to zero
void QuadratureEncoder::ZeroCounter()
{
    // Note that it's not necessary to disable IRQs here, even though
    // the interrupt handlers also access the count, because a single
    // memory write is inherently atomic with respect to interrupts.
    count = 0;
}

// base class configuration
bool QuadratureEncoder::ConfigureBase(const char *name, const JSONParser::Value *val)
{
    // get the two encoder channel GPIOs
    int a = val->Get("channelA")->Int(-1);
    int b = val->Get("channelB")->Int(-1);
    
    // validate them and claim them as inputs
    if (!IsValidGP(a) || !IsValidGP(b))
    {
        Log(LOG_ERROR, "%s: invalid or missing channelA/channelB GPIO port assignment", name);
        return false;
    }
    
    // claim the GPIOs in shared input mode
    if (!gpioManager.ClaimSharedInput(Format("%s (ChA)", name), a, false, false, true)
        || !gpioManager.ClaimSharedInput(Format("%s (ChB)", name), b, false, false, true))
        return false;

    // set the GP ports in the chip
    this->gpA = a;
    this->gpB = b;

    // successul configuration - initialize the device and return the result
    return Init();
}

// Initialize
bool QuadratureEncoder::Init()
{
    // Set up interrupt handlers on the A and B channels.  Set these to
    // the highest priority among GPIO handlers, since the quadrature
    // interrupts are extremely sensitive to latency when used with a
    // fast encoder.
    const auto priority = PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY;
    gpio_add_raw_irq_handler_with_order_priority_masked(
        (1 << gpA) | (1 << gpB), thunkManager.Create(&QuadratureEncoder::IRQ, this), priority);
    gpio_set_irq_enabled(gpA, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(gpB, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    // Set the GPIO IRQ to top priority.  A quadrature encoder can send
    // interrupts at very high speeds, and every missed interrupt puts
    // us further out of sync with the physical system we're measuring.
    // The Pico can easily keep up with GPIO interrupts at up to about
    // 300kHz - but only when interrupts are enabled.  Our GPIO IRQ will
    // be blocked any time another IRQ of equal or higher priority is
    // being serviced.  Since the quadrature interrupts are the last
    // tolerant of latency in the whole Pinscape system, they qualify
    // for the highest interrupt priority as a way to minimize their
    // exposure to latency.  In addition, the quadrature IRQ service
    // routines are fast, and have consistent run time, so prioritizing
    // them will add very little latency to any *other* interrupts.
    //
    // Note that all GPIO IRQs share the same hardware vector, so
    // elevating the priority of the quadrature GPIO IRQs will also
    // elevate all other GPIO IRQs.  
    irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);

    // enable the interrupt handlers
    irq_set_enabled(IO_IRQ_BANK0, true);

    // success
    return true;
}

// IRQ handler for channels A and B
//
// Note that this is explicitly placed in RAM (instead of the
// default linkage in flash), because RAM-resident code on Pico
// runs about 10x faster than flash-resident code.  Some quadrature
// chips are capable of high-speed channel transitions; e.g.,
// AEDR-8300 is spec'd to generate transitions at up to 30 kHz,
// which is 33us between interrupts.  We need these handlers to be
// fast enough to avoid missed interrupts at the highest rates
// coming from the physical sensors.
void __not_in_flash_func(QuadratureEncoder::IRQ)()
{
    // get the interrupt event mask for each channel
    uint32_t maskA = gpio_get_irq_event_mask(gpA) & (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    uint32_t maskB = gpio_get_irq_event_mask(gpB) & (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);

    // Update the state.  Note that the state updater won't do anything if
    // neither of our pins is involved in the interrupt, so it's faster to
    // just call it unconditionally.  This is important because we *will*
    // get called many times with no relevant pin change, because the Pico
    // SDK calls EVERY raw GPIO IRQ handler on EVERY GPIO interrupt,
    // regardless of which pin(s) a handler is nominally associated with.
    UpdateState();

    // acknowledge the IRQ to clear the mask flags
    gpio_acknowledge_irq(gpA, maskA);
    gpio_acknowledge_irq(gpB, maskB);
}
