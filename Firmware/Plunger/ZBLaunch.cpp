// Pinscape Pico - ZB Launch Ball
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>
#include <pico/time.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "Outputs.h"
#include "Plunger.h"
#include "ZBLaunch.h"

// global singleton
ZBLaunchBall zbLaunchBall;

// Configure from JSON data
//
// plunger: {
//   // ... plunger device settings...
//
//   zbLaunch: {
//     pulseTime: 50,          // pulse time, in milliseconds; this is the duration of the
//                             // virtual button press triggered when a plunger gesture is
//                             // recognized
//
//     pushThreshold: -2600,   // push threshold, on the abstract -32768..+32767 axis scale
//                             // representing the plunger position; this must be negative
//                             // (a position forward of the rest position, with the plunger
//                             // pushed in against its barrel spring), and should usually
//                             // be around half of the available forward travel distance
//                             // as measured during calibration
//
//     output: 22,             // Output port that triggers ZB Launch mode, as a number or
//                             // a string referring to a named port.  This is only needed
//                             // if you don't explicitly define a port with device type
//                             // "zblaunch" in the outputs[] list.  The setting here lets
//                             // you piggyback the ZB Launch control on a port that also
//                             // controls some physical device.
//
//     action: { ... },        // a button action, as in the button[] setup section; this
//                             // is optional, since the virtual button status can be used
//                             // in other ways besides triggering a button-like action
//   },
// }
void ZBLaunchBall::Configure(JSONParser &json)
{
    if (const auto *val = json.Get("plunger.zbLaunch") ; !val->IsUndefined())
    {
        // get the push threshold
        pushThreshold = val->Get("pushThreshold")->Int(-2600);
        if (pushThreshold >= 0)
        {
            pushThreshold = -2600;
            Log(LOG_ERROR, "ZB Launch: pushThreshold must be a negative value "
                "(forward of the plunger rest position); using default value of %d\n", pushThreshold);
        }

        // get the pulse time
        pulseTime = val->Get("pulseTime")->UInt(50) * 1000;

        // check for an output port assignment
        if (!val->Get("output")->IsUndefined())
        {
            // resolve after the output manager is configured
            OutputManager::AfterConfigure(json, [](JSONParser &json) {
                zbLaunchBall.port = OutputManager::Get(json, json.Get("plunger.zbLaunch.output"));
            });
        }

        // parse the action
        if (auto *actionVal = val->Get("action") ; !actionVal->IsUndefined())
            action.reset(Button::ParseAction("ZB Launch", actionVal));
    }
}

void ZBLaunchBall::SetActive(bool active)
{
    // set the new state
    isActive = active;

    // check to see if this changes the action state, which depends on
    // both the button state and the mode
    UpdateActionState();
}

void ZBLaunchBall::Task()
{
    // if we have an output port, check for status changes
    if (port != nullptr)
    {
        // check for a change since the last time we polled the port
        bool newActive = (port->Get() != 0);
        if (newActive != portActive)
        {
            // activate/deactivate the mode and remember the new port setting
            SetActive(newActive);
            portActive = newActive;
        }
    }

    // Check for launch events
    switch (launchState)
    {
    case LaunchState::Default:
        // No action in progress.  If a launch event has been
        // detected on the plunger, activate a timed pulse and
        // switch to state 1.  If the plunger is pushed forward of
        // the threshold, push the button.
        if (plunger.IsFiring())
        {
            // firing event - start a timed Launch button pulse
            tLaunchState = time_us_64();
            SetButton(true);
            
            // switch to Firing state
            launchState = LaunchState::Firing;
        }
        else if (plunger.GetZ() <= pushThreshold)
        {
            // Pushed forward without a firing event - hold the
            // button as long as we're pushed forward, staying in
            // the default state.
            SetButton(true);
        }
        else
        {
            // not pushed forward - turn off the Launch button
            SetButton(false);
        }
        break;
        
    case LaunchState::Firing:
        // Firing:  A timed Launch button pulse in progress after a
        // firing event.  Wait for the timer to expire.
        if (time_us_64() > tLaunchState + pulseTime)
        {
            // timer expired - turn off the button
            SetButton(false);
            
            // switch to post-firing state
            launchState = LaunchState::PostFiring;
            tLaunchState = time_us_64();
        }
        break;
        
    case LaunchState::PostFiring:
        // Post-Firing: A timed Launch button pulse was fired and
        // timed out.  Wait for the plunger to settle back at the
        // rest position.
        if (!plunger.IsFiring())
        {
            // firing event done - return to default state
            launchState = LaunchState::Default;
            tLaunchState = time_us_64();
        }
        break;
    }
}

void ZBLaunchBall::SetButton(bool newBtnState)
{
    // check for a state change
    if (btnState != newBtnState)
    {
        // note the new button state
        btnState = newBtnState;

        // update the action state accordingly
        UpdateActionState();
    }
}

void ZBLaunchBall::UpdateActionState()
{
    // Figure the new action state.  The action can only be
    // activated when the mode is engaged.
    bool newActionState = isActive && btnState;
    
    // if there's an action, and the state has changed switch its state
    if (action != nullptr && newActionState != actionState)
        action->OnStateChange(newActionState);
    
    // remember the new action state
    actionState = newActionState;
}
