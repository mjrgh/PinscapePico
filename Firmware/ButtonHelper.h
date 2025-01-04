// Pinscape Pico - Button helper
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Helper class for USB interfaces that maintain On/Off pushbutton
// states.  The helper object keeps track of changes to button states
// between USB reports, so that each report reflects updates since
// the last report.  We use this in our gamepad, XInput, and Pinball
// Device interfaces.

#pragma once
#include <stdint.h>

// Button helper
struct ButtonHelper
{
    // Live button state.  This stores the latest button state as
    // reported by the logical button controls (see class Button
    // in Buttons.h).
    uint32_t live = 0;
    
    // Last reported button state.  We save the button state of each
    // report so that we can detect quick on/off/on or off/on/off
    // pulses that occur within the space of a single USB polling
    // cycle.  When we detect a state change, we record it as a
    // sticky change to the "next" state, so that we send the change
    // for at least one reporting cycle.  This ensures that quick
    // button taps result in at least one report to the host showing
    // the state change.
    uint32_t reported = 0;
    
    // Next button report state.  This is where we store sticky state
    // changes that occur within a polling period.
    uint32_t next;

    // Handle a button event.  Buttons are labeled 1 to 32.
    void OnButtonEvent(int buttonNum, bool isDown);
    
    // Get the button states to report for the current cycle, and
    // update the internal state for the next reporting cycle.
    uint32_t Report();
};
