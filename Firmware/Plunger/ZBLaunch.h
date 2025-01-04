// Pinscape Pico - ZB Launch Ball
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// ZB Launch Ball is a system that monitors the analog plunger, and
// generates a virtual button press when it detects certain gestures:
//
//   - The plunger is pulled and released, as though launching a ball
//     in a regular table with a plunger.  In this case, ZB Launch
//     generates a brief pulse on the virtual button, as though the
//     user briefly pushed the launch button.
//
//   - The plunger is pushed firmly forward from its rest position, as
//     though treating the plunger knob as a button by pushing it.
//     ZB Launch shows the virtual button as pressed as long as the
//     plunger is held forward like this.
//
// The ZB Launch button has two appearances: the raw button state, which
// always detects the gestures above, and a mode-sensitive state, which
// only detects gestures when software on the PC side specifically
// enables ZB Launch Mode.  Either appearance can be selected when
// configuring a button that uses the ZB Launch source.
//
// The mode-sensitive appearance lets the PC engage and disengage the
// mode according to what kind of pinball table is loaded in VP or any
// other simulator.  Normally, the PC host will enable ZB Launch Mode
// when a plunger-less table is loaded in the simulator (that is, any
// pinball table that has a Launch Ball button or other non-plunger
// launch control), and will disable it otherwise.  This prevents
// spurious Launch Ball commands from being sent to tables with
// traditional plungers.
//
// We provide multiple ways to engage and disengage the mode.  One way
// is via a DOF command to a port designated in the configuration.  When
// this port is ON (set to any PWM level above 0), ZB Launch mode is
// engaged, otherwise the mode is disengaged.  This is the traditional
// way to engage the mode, and it's simple to implement on the PC side
// for any table that uses DOF, since it can usually be set up entirely
// within the DOF configuration file, without any table scripting
// needed.  A second way is via a special USB command to our Vendor
// Interface or Feedback Controller HID interface; this method can be
// used manually, or from a Windows batch file, so it's a good option
// for tables or simulators that aren't DOF-aware.  Another method is to
// control the mode manually through a button press, using the ZB Launch
// action in the button configuration.

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <memory>
#include <pico/stdlib.h>
#include "Pinscape.h"
#include "Utils.h"
#include "Outputs.h"
#include "Buttons.h"

// forwards/externals
class JSONParser;
class ZBLaunchBall;

// global singleton
extern ZBLaunchBall zbLaunchBall;

// XZ Launch Ball controller
class ZBLaunchBall
{
public:
    ZBLaunchBall() { }

    // Configure from JSON data
    void Configure(JSONParser &json);

    // Period task handler
    void Task();

    // Is ZB Launch mode active?  This returns true if the mode is
    // engaged by the associated DOF control port or by a manual
    // command.
    bool IsActive() const { return isActive; }

    // Set the mode.  Mode changes from all sources are edge-sensitive,
    // so a SetActive() overrides the current setting from the DOF
    // output, if any.  This allows mode changes under program control
    // from the PC side, or via manual controls on the device side
    // (e.g., a button mapped to toggle the mode).
    void SetActive(bool active);

    // Is the ZB Launch virtual button "pressed"?  This returns true if
    // the virtual button state is on.  This is independent of the mode;
    // this detects firing actions even when the mode isn't engaged.
    bool IsFiring() const { return btnState; }

private:
    // Handle a Launch Ball button state change
    void SetButton(bool state);

    // Push threshold.  This is the plunger logical axis position where
    // the plunger acts like the Launch Ball button when pressed.  This
    // must be a negative value, which represents a position in front of
    // the plunger's rest position.  The default threshold is about 8%
    // of the positive range.  A standard modern ball shooter has a
    // travel range of about 80mm total, with about 85% of the total
    // range in the retraction zone and the remaining 15% forward of the
    // rest position.  The button-action threshold has to be forward of
    // the rest position because otherwise the button would just
    // constantly fire when the plunger is at rest.  It also shouldn't
    // be too close to the rest position - you need a little padding,
    // because there's enough friction in a typical plunger that it
    // won't always return to exactly the same rest position; there's a
    // bit of a range.  The threshold has to be set far enough in front
    // of the rest position to account for this variability.  It needs
    // to be tuned for each system, which is why we make this a
    // configurable variable.
    int16_t pushThreshold = -2600;  // about half of the typical forward travel range

    // Output port that serves as the source of the ZB Launch status
    OutputManager::Port *port = nullptr;

    // port on/off status as of our last check
    bool portActive = false;

    // Action to fire when virtual launch button is activated
    std::unique_ptr<Button::Action> action;

    // Pulse time, microseconds
    uint32_t pulseTime = 50000;

    // ZB Launch mode is active
    bool isActive = false;

    // Simulated Launch Ball button state.  If a "ZB Launch Ball" port is
    // defined for our LedWiz port mapping, any time that port is turned ON,
    // we'll simulate pushing the Launch Ball button if the player pulls 
    // back and releases the plunger, or simply pushes on the plunger from
    // the rest position.  This allows the plunger to be used in lieu of a
    // physical Launch Ball button for tables that don't have plungers.
    enum class LaunchState
    {
        Default,     // default state
        Firing,      // button pulse started
        PostFiring,  // button pulse ended; waiting for plunger to settle at rest position
    };
    LaunchState launchState = LaunchState::Default;

    // Logical button state.  This represents the virtual ZB Launch
    // button's pressed/not pressed state.  This is independent of
    // whether or not ZB Launch mode is engaged; ZB Launch monitors the
    // plunger state and sets the button state at all times, whether or
    // not the mode is active.
    bool btnState = false;

    // Action state.  This is the state of the underlying action
    // assigned to the virtual button.  The action is only fired when
    // the button is pressed while ZB Launch Mode is active.
    bool actionState = false;

    // Update the action state.  This figures the new action state based
    // on the current virtual button state and ZB Launch Mode: the
    // action is activated when the button is pressed and the mode is
    // engaged.  Fires the action if the state changed.
    void UpdateActionState();

    // Time of last state transition (as a system timestamp).  Some of
    // the states are time-dependent; this lets us determine how long
    // we've been in the current state.
    uint64_t tLaunchState = 0;
};
