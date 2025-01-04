// Pinscape Pico - Button helper
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include "ButtonHelper.h"

// Report button states
uint32_t ButtonHelper::Report()
{
    // remember the buttons sent, and snapshot the live button
    // state as the starting point for the next report
    reported = next;
    next = live;

    // return the buttons for this report
    return reported;
}

// Handle a button event
void ButtonHelper::OnButtonEvent(int buttonNum, bool isDown)
{
    // Get the bit mask in the button bit vector corresponding to the button.
    // Buttons are numbered 1..32, so the bit position is one less than the
    // nominal button number.
    uint32_t mask = (1 << (buttonNum - 1));

    // set or clear the bit in the live button mask
    isDown ? (live |= mask) : (live &= ~mask);

    // If the bit hasn't changed in the pending next report, update
    // it now to match.  The pending report bits are "sticky", meaning
    // that when a bit changes there with respect to the last report,
    // it doesn't change back.  That ensures that short button taps
    // persist long enough to make it into at least one HID report,
    // so that the host sees a HID report button press event for
    // every physical button press, even if the physical press was
    // so brief that it started and ended within the same USB cycle.
    if ((next & mask) == (reported & mask))
        isDown ? (next |= mask) : (next &= ~mask);
}

