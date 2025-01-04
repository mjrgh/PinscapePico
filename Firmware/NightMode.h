// Pinscape Pico - Night Mode Control
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Night Mode lets the user disable designated feedback controls as
// a group.  The main purpose, as suggested by the name, is to allow
// for quieter play during late hours when housemates and neighbors
// are more sensitive to noise.
//
// This class coordinates night mode, mostly just by serving as the
// storage location for the current Night Mode/Day Mode state.  The
// feedback device output controller classes are responsbile for
// actually enforcing the current mode (by selectively enabling
// devices according to the device settings and current Night Mode
// state), and they also handle the details of designating which
// devices are affected.  Likewise, the details of how the user
// actually activates Night Mode are left to the other parts of
// the system that handle user input.
//

#pragma once

#include <list>

class NightModeControl
{
public:
    NightModeControl();
    ~NightModeControl();

    // Turn night mode on or off
    void Set(bool state);

    // Get the current mode
    bool Get() const { return state; }

    // Subscribe for state change notifications.  Note that the night
    // mode object retains the pointer, so the subscriber must unsubscribe
    // before being destroyed.  We provide a virtual destructor in the
    // interface class to make that automatic.
    class NightModeEventSink
    {
    public:
        virtual ~NightModeEventSink();
        virtual void OnNightModeChange(bool state) = 0;
    };
    void Subscribe(NightModeEventSink *eventSink);
    void Unsubscribe(NightModeEventSink *eventSink);
    
protected:
    // Current state.  true -> Night Mode is on, false -> day mode
    bool state = false;

    // event subscribers
    std::list<NightModeEventSink*> eventSinks;
};

// global singleton instance
extern NightModeControl nightModeControl;
