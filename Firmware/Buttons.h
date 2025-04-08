// Pinscape Pico firmware - buttons
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines the button interface.  A button represents a two-state input
// control.  The most straightforward incarnation of a button is a physical
// pushbutton that's mapped to send a keyboard key or joystick, but this class
// attempts to abstract both the input and output side of that into a logical
// control that still acts like a pushbutton conceptually, but could be
// implemented with different sorts of inputs and outputs.
//
// Shift Buttons
// -------------
// The original Pinscape firmware provided one Shift button, which acted like
// a keyboard modifier key by selecting s second function for each other
// button when held down.  This version generalizes the idea to allow for up
// to 31 modifier buttons, which can be used individually or in chords to
// select alternate functions.  In practice, I think two modifier buttons will
// be sufficient, since that allows for up to four functions per button
// (unshifted, shift 1, shift 2, shift 1+2).  As with the original Pinscape
// software, the shift buttons themselves can be assigned functions that are
// activated either immediately when pressed ("Shift-AND" mode - shift
// function AND the base function on every press), or only when released and
// only if no shifted button was pressed ("Shift-OR")
//
// The original Pinscape software's button model had two actions per button,
// one for Unshifted and one for Shifted.  This version uses a more flexible
// model where each logical button only has one action, and specifies the
// shift combination that selects it.  To create a shifted and unshifted
// version of a given physical input, you create two separate logical inputs
// with the same physical input source and different shift masks.  A logical
// button can also have its shift mask set to match ANY shift state, so that
// the physical input always activates the same action regardless of which
// shift keys are pressed.
//
//
// Variations on the input side
// ----------------------------
//    - Physical GPIOs wired to a switch, active high or active low input
//    - GPIO extender chips wired to pushbuttons
//    - Shift register chips wired to pushbuttons
//    - IR remote control receiver, receiving specific codes
//    - USB input from the host, exposed on the host as virtual DOF
//      devices (for control via DOF-aware software)
//    - USB input from the host, exposed on the host as abstract binary
//      inputs (for control via Pinscape-aware software)
//    - Timer events, such as periodic timers, countdown timers, or
//      time-of-day timers (e.g., turn on night mode between 9PM and 10AM)
//    - Analog inputs crossing thresholds, such as accelerometer nudging
//      or plunger motion
//    - Software inputs from other subsystems, such as Night Mode status,
//      USB connection status, or secondary power supply status
//
// Variations on the output side
// -----------------------------
//    - Keyboard key press
//    - Gamepad button press
//    - XInput button press
//    - IR remote control code transmission
//    - Night mode enable
//    - Feedback device activation
//    - Pico hard reset (into boot loader or into flash firmware)
//    - Macro (a timed series of one or more of the above)
//    - Engage shift mode (selects alternate function of other buttons)
//
// Types of logical buttons
// ------------------------
//    - Pushbutton: standard pushbutton mode: logical button equals
//      state of source; suitable for most of the basic pinball
//      machine buttons
//    - ToggleButton: logical button flips state on each OFF->ON
//      transition on source, for things like a Night Mode button
//    - On/Off button: a toggle button with two inputs, one that
//      turns the logical control on and one that turns it off,
//      in analogy to separate Power On/Power Off buttons on a TV
//      remote control.
//    - PulseButton: each state change on the source triggers a
//      timed pulse on the logical control, for things like a coin
//      door position switch wired to the older PinMame scheme that
//      required a key press to signal when the door changed from
//      open to closed or vice versa
//    - Shift-OR button: activates shifted function of other buttons
//      when held, generates a pulse when released if no other
//      button was pressed while it was being held
//    - Shift-AND button: activates shifted function of other buttons
//      when held, AND acts like a momentary pushbutton itself while
//      behind held
//
// To create a logical button, you separately create the desired input source
// object (a subclass of Button::Source), the action event handler (a
// Button::Action subclass), and a concrete subclass of Button.  The Button
// constructor takes the Source and Action objects as parameters, plus any
// other parameters specific to the Button subclass.  Add the new Button
// object to the global list via Button::Add(), and the automatic polling will
// take it from there.  (The main loop simply has to call Button::Task() as
// often as possible to poll the physical control inputs and process pending
// asynchronous events.)

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <list>
#include <unordered_map>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/mutex.h>

// project headers
#include "USBIfc.h"
#include "IRRemote/IRReceiver.h"
#include "../USBProtocol/VendorIfcProtocol.h"

// external classes
class PCA9555;
class C74HC165;
class JSONParser;
class IRCommandDesc;


// Abstract button class.  This defines a logical button, which can be
// mapped to a physical or logical input that controls its state, and an
// output action to perform when the state changes.  This is an abstract
// base class; subclasses define the behavior variations.
class Button
{
public:
    // forward declarations for our scoped classes
    class Source;
    class Action;

    // look up a button by name or by configuration index
    static Button *Get(const char *name);
    static Button *Get(int configIndex);

    // get the current logical state
    bool GetLogicalState() const { return logicalState; }

    // Configure buttons from the JSON configuration data
    static void Configure(JSONParser &json);

    // Parse a source.  jsonLocus is the JOSN config location to report in
    // logged error messages; pinoutLabel is the name to report in host
    // queries for the pin out diagram label for the item.
    //
    // activeDefault is the default value for "active", either "high" or
    // "low".
    //
    // If allowBareGPIO is true, the source value can be a simple number
    // instead of an object, giving a GPIO port number.  This creates a
    // standard GPIO pin input source with default settings.
    //
    // Returns a null object if the configuration is invalid.
    static Source *ParseSource(
        const char *jsonLocus, const char *pinoutLabel,
        JSONParser &json, const JSONParser::Value *srcVal,
        const char *activeDefault, bool allowBareGPIO);

    // Parse an action.  "jsonLocus" is the JSON config location to report in
    // logged error messages.
    static Action *ParseAction(const char *jsonLocus, const JSONParser::Value *actionVal, bool inMacro = false);

    // Create a GPIO source
    class GPIOSource;
    static GPIOSource *CreateGPIOSource(
        const char *jsonLocus, const char *pinoutLabel,
        int gp, bool activeHigh, bool usePull, bool enableLogging,
        int lpFilterRiseTime_us, int lpFilterFallTime_us,
        int debounceOnTime_us, int debounceOffTime_us);

    // construction/destruction
    Button(Source *source, uint32_t shiftMask, uint32_t shiftBits, Action *action) :
        source(source), shiftMask(shiftMask), shiftBits(shiftBits), action(action) { }
    virtual ~Button() { }

    // Match the global shift state to the button's shift mask.  Returns
    // true if the button is selected in the current shift state, false if
    // not.  This also has the side effect of notifying the current shift
    // buttons that their shift effect has been activated, if appropriate.
    bool MatchShiftState();
    
    // Add a button to the global button list, for inclusion in the periodic
    // Task() polling.  The list takes ownership of the object, so the caller
    // need not and must not delete it after adding it to the list.
    static void Add(Button *button);
    
    // Handle periodic button tasks.  The main loop must call this when
    // convenient, as frequently as possible.  This polls the current button
    // states and fires events on state changes.
    static void Task();

    // Receive notification that a shifted function has been activated
    // in the current shift state.  A Shift-OR button uses this to record
    // that the current press was a shift activation.  'maskedBits' gives
    // the set of bits actually used to match the button press.
    virtual void OnShiftUsage(uint32_t maskedBits) { }


    // -----------------------------------------------------------------------
    //
    // USB Vendor Interface functions.  These handle diagnostic commands
    // through our private Vendor Interface, whcih is used to access the
    // device from the Config Tool and similar host-side tools.
    //

    // Query logical button descriptors.  Populates the buffer with a
    // PinscapePico::ButtonList header struct, followed by an array of
    // PinscapePico::ButtonDesc structs describing the assigned logical
    // buttons.  Returns the size in bytes of the populated data, or 0
    // if an error occurs.  Note that a successful call will return a
    // non-zero size even if no logical buttons are defined, since the
    // fixed header struct is always present, so zero buttons isn't a
    // special case.
    static size_t QueryDescs(uint8_t *buf, size_t bufSize);

    // Query logical button states.  Populates the buffer with an array
    // of bytes equal in size to the number of defined buttons.  Returns
    // the number of bytes populated, or 0xFFFFFFFF on error.  Note that
    // it's valid to have zero logical buttons configured, in which case
    // the return value is zero, which isn't an error.
    static size_t QueryStates(uint8_t *buf, size_t bufSize, uint32_t &globalShiftState);

    // Query GPIO input states.  Populates the buffer with an array of
    // bytes, one per Pico GPIO port, giving the current live input
    // states of the pins.
    static size_t QueryGPIOStates(uint8_t *buf, size_t bufSize);

    // Populate a PinscapePico::ButtonDesc for this individual button
    void PopulateDesc(PinscapePico::ButtonDesc *desc) const;

    // Get the vendor interface button type (PinscapePico::ButtonDesc::TYPE_xxx)
    virtual uint8_t GetVendorIfcType() const = 0;


    // -----------------------------------------------------------------------
    //
    // Input sources
    //
    class Source
    {
    public:
        // Format the name into a buffer, for messages.  Returns a pointer
        // to the buffer.
        virtual const char *FullName(char *buf, size_t buflen) const = 0;

        // Poll the source.  Returns the current logical state of the
        // control.  The "logical" state is the state after applying any
        // necessary debouncing or filtering to the physical state.
        virtual bool Poll() = 0;

        // Populate a vendor interface descriptor struct with details
        // on the source.  This fills in at least the type code; some
        // source types also fill in the detail items.
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const = 0;
    };

    // Null source.  We use this for buttons where source configuration
    // fails, so that we can at least set up a vector entry for the button
    // to keep the indexing right.
    class NullSource : public Source
    {
    public:
        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "None"); return buf; }
        virtual bool Poll() { return false; }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;
    };

    // Debounced input source.  This can be used as the base class for
    // sources connected to physical inputs that require software
    // debouncing.
    //
    // We debounce inputs using a minimum hold time for each state change.
    // When we observe a physical state change, it passes through to the
    // logical state immediately, as long as the prior logical state has
    // been in effect for the minimum hold time.  That starts a new hold
    // period for the new logical state.  Physical switch inputs typically
    // "bounce" for a brief time after the moment of contact, meaning that
    // the voltage level on the input oscillates rapidly before stabilizing
    // at the new on/off level.  The hold time is designed to ignore these
    // rapid oscillations, but without adding any latency at the actual
    // transition points.
    class DebouncedSource : public Source
    {
    public:
        DebouncedSource(uint32_t dtOn, uint32_t dtOff) :
            dtOn(dtOn), dtOff(dtOff) { }

        // Poll the physical state of the switch
        virtual bool PollPhysical() = 0;

        // poll the logical state of the switch, applying debouncing
        virtual bool Poll() override;

        // Minimum times to hold each state, in microseconds.  After a
        // logical state transition, new physical state transitions will
        // be ignored until the hold time elapses.
        uint32_t dtOn;
        uint32_t dtOff;

        // current logical state
        bool logicalState = false;

        // System time of last physical state change
        uint64_t tLastLogicalChange = 0;
    };

    // Second-core debounced input source.  This is the base class for
    // devices that we can poll at extremely high speed in the second-core
    // thread: GPIO ports, 74HC165 ports.
    class SecondCoreDebouncedSource : public Source
    {
    public:
        // second-core per-button polling
        static void SecondCoreTask();

        // poll the logical state of the button
        virtual bool Poll() override { return activeHigh ? debouncedPhysicalState : !debouncedPhysicalState; }

    protected:
        SecondCoreDebouncedSource(bool activeHigh, uint32_t lpFilterRise, uint32_t lpFilterFall, uint32_t dtOn, uint32_t dtOff);

        // poll the button on the second-core thread
        void SecondCorePoll();

        // poll the physical hardware source (runs on second-core thread)
        virtual bool PollPhysical() = 0;

        // Process a change to the debounced state (runs on second-core thread).
        // Does nothing by default.  This is provided primarily as a hook for
        // event logging in the GPIO button handler.
        virtual void OnDebouncedStateChange(bool newState) { }

        // active high/low
        bool activeHigh;

        // Low-pass filter times for rising and falling edges, in
        // microseconds.  The low-pass filter delays recognition of an
        // edge on the physical input until the input has remained at
        // the new level for the lp filter time.  This adds latency
        // equal to the filter time.
        uint32_t lpFilterRise;
        uint32_t lpFilterFall;

        // Minimum hold times for each state, in microseconds.  After the low-pass
        // filter recognizes a valid state transition, the new state will remain
        // in effect for a minimum of the hold time.  New edges will be ignored
        // until the hold time has passed.
        uint32_t dtOn;
        uint32_t dtOff;

        // button list for the second core task handler
        static std::list<SecondCoreDebouncedSource*> all;

        // Time of the last physical edge
        uint64_t tEdge = 0;

        // time of last change to debounced physical state
        uint64_t tLastDebouncedStateChange = 0;

        // Last physical state, before any debouncing
        bool lastPhysicalState = false;

        // Debounced physical state of the GPIO pin.  This is written by
        // the second-core polling routine, and read by the main button
        // task routine on the primary core.  It's volatile because it's
        // accessed by both cores, but no locking is needed since only
        // the second core has write access.
        //
        // This is the post-debouncing state.  The second-core thread
        // applies debouncing, so the main task routine sees the virtual
        // GPIO state with debouncing applied.  This allows the main
        // thread to recognize state changes on this value immediately,
        // with no further filtering required.
        volatile bool debouncedPhysicalState = false;
    };

    // GPIO input source
    class GPIOSource : public SecondCoreDebouncedSource
    {
    public:
        // Set up the button
        GPIOSource(int gpNumber, bool activeHigh = false, bool usePull = true,
                   int lpFilterRiseTime = 0, int lpFilterFallTime = 0,
                   int dtOn = 1000, int dtOff = 1000, bool enableEventLogging = false);

        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "GP%d", gpNumber); return buf; }

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // poll the hardware input
        virtual bool PollPhysical() override { return gpio_get(gpNumber); }

        // process a debounced state change - logs the event
        virtual void OnDebouncedStateChange(bool newState) override;

        // query the event log for a given GPIO
        static size_t QueryEventLog(uint8_t *buf, size_t buflen, int gpNum);

        // clear the event log
        static bool ClearEventLog(int gpNum);
        
        // Pico GP number of the input
        int gpNumber;

        // true -> enable an internal Pico pull up/down to the OFF voltage;
        // false -> leave input floating.  Floating should be used when the
        // external circuit drives the pin in both states.  Internal pull
        // should be used when the external circuit is "open" in the off
        // state, such as with an SPST switch or an open collector/open
        // drain electronic switch (e..g, an optocoupler).
        bool usePull;

        // list of all GPIOSource objects
        static std::list<Button::GPIOSource*> allgs;

        // IRQ event logging.  When a button is enabled for event logging,
        // we record its state transitions in a log that the host can query
        // via the Vendor Interface, to measure the latency between a physical
        // button press and its appearance in a HID report on the host.
        struct EventLog
        {
            EventLog(bool enable);

            // logging enabled
            bool enable;

            // mutex - the event log is written from the second core and read
            // on the primary core
            mutex_t mutex;

            // Implied state after last edge.  This lets us sequence rise/fall
            // events in the log properly when an IRQ call signals both a rising
            // and falling edge simultaneously.  That actually happens quite a
            // lot, because switch bounce at the moment of contact is so fast
            // that the Pico can register a rising and falling edge within the
            // time it takes to invoke the IRQ handler.  The Pico GPIO hardware
            // that senses the edges is much faster than the CPU IRQ servicing
            // latency.  When we see both edges at once, this lets us sequence
            // the log events in the proper order, os that logged events always
            // alternate between PRESS and RELEASE.
            bool impliedPostEdgeState = false;

            // Event log, as a ring buffer
            struct Event
            {
                Event() { }
                void Init(int type)
                {
                    this->t = time_us_64();
                    this->type = type;
                }

                // event timestamp
                uint64_t t = 0;

                // Event type.  These are abstracted to represent the BUTTON state,
                // rather than the raw voltage level, according to the active high/low
                // settings, so that a PRESS event always represents the button being
                // pressed (the switch being closed).  Note that the constants here
                // must match the ones defined in USBProtocol/VendorIfcProtocol.h
                // (semantically).
                static const int TypeRelease = 0;     // button released (switching from on to off)
                static const int TypePress = 1;       // button pressed (switching from off to on)
                int type = TypeRelease;
            };
            static const int MaxEvents = 64;
            Event event[MaxEvents];

            // event log read/write pointer
            volatile int read = 0;
            volatile int write = 0;

            // clear the log
            void Clear();

            // Add an event
            void AddEvent(int type);

            // Get the oldest event, removing it from the queue; returns false if the queue is empty.
            bool GetEvent(Event &e);
        };
        EventLog eventLog;
    };

    // BOOTSEL button source
    class BOOTSELSource : public Source
    {
    public:
        BOOTSELSource(int checkInterval_ms) :
            checkInterval(checkInterval_ms*1000) { }
        
        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "BOOTSEL"); return buf; }

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->sourceType = PinscapePico::ButtonDesc::SRC_BOOTSEL; }

        // Poll the switch
        virtual bool Poll() override;

    protected:
        // state at last check (true -> button pressed)
        bool state = false;

        // time of last check
        uint64_t tCheck = 0;

        // check interval in microseconds
        uint32_t checkInterval = 100000;
    };

    // PCA9555 port extender source.  The PCA9555 is an I2C chip providing
    // 16 digital in/out ports.  The button source is specified by the chip
    // instance and port number.
    class PCA9555Source : public DebouncedSource
    {
    public:
        // Instantiate the source.  Note that the I2C bus must be configured
        // before the constructor is called, since the constructor sends I2C
        // commands to the selected PCA9555 to configure the port as an input
        // with the specified pull up/down properties.
        PCA9555Source(PCA9555 *chip, uint8_t port, bool activeHigh = false, int debounceTime_us = 1000);
        virtual bool PollPhysical() override;
        
        virtual const char *FullName(char *buf, size_t buflen) const override;

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // PCA9555 device interface
        PCA9555 *chip;

        // port number
        uint8_t port;

        // true -> active high (3.3V = ON), false = active low (GND = ON)
        bool activeHigh;
    };

    // 74HC165 shift-register source.  The 74HC165 is an 8-port
    // parallel-in, serial-out shift register that can be daisy-chained
    // to add an arbitrary number of ports with only three GPIO ports
    // consumed on the Pico.
    class C74HC165Source : public SecondCoreDebouncedSource
    {
    public:
        C74HC165Source(C74HC165 *chain, int port, bool activeHigh = false,
                       int lpFilterRiseTime = 0, int lpFilterFallTime = 0,
                       int debounceTimeOn_us = 1500, int debounceTimeOff_us = 1000);
        
        virtual bool PollPhysical() override;

        virtual const char *FullName(char *buf, size_t buflen) const override;

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // device interface
        C74HC165 *chain;

        // port number
        int port;
    };

    // IR Remote Receiver source.  This source latches on when a
    // specified IR code is received on the IR remote receiver.  The
    // source then remains on until polled.  Polling resets the latch.
    // This type of source is normally used with a Toggle Button
    // (sending the IR code toggles the state of the logical control
    // on and off) or a Pulse Button (sending the IR code acts like
    // a momentary button push of a given duration).
    class IRSource : public Source, public IRReceiver::Subscriber
    {
    public:
        IRSource(const IRCommandDesc &cmd,
            uint32_t latchingInterval_ms, uint32_t firstRepeatDelay_ms);

        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "IR receiver"); return buf; }

        // poll for input
        virtual bool Poll() override;

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->sourceType = PinscapePico::ButtonDesc::SRC_IR; }

        // IR command event.  The IR receiver calls this when it receives
        // a transmission matching our command code.
        virtual void OnIRCommandReceived(const IRCommandReceived &command, uint64_t dt) override;

        // command code
        IRCommandDesc cmd;

        // Latching interval in microseconds
        uint32_t latchingInterval_us;

        // First repeat delay interval in microseconds
        uint32_t firstRepeatDelay_us;

        // flag: the code has been received since the last polling pass
        bool received = false;

        // flag: the last code received was an auto-repeat
        bool repeating = false;

        // Current receive latching end time.  When we receive our code, we'll
        // set this to the current time plus the latching interval, and we'll
        // report our state as ON until reaching the end of the latching time.
        // This is an attempt to replicate the physical button state on the
        // sending remote control, by staying continuously activated for as
        // long as the user is holding the button down.  The remote will
        // persumbaly send the same code on auto-repeat for as long as the
        // button is down, so we'll receive a series of activations as the
        // repeated commands arrive.  As long as our latching timeout is
        // longer than the time between repeats, our state will read as
        // continuously ON the whole time, replicating the state of the
        // sender's physical button.
        uint64_t tLatchingEnd = 0;

        // First repeat delay end time
        uint64_t tFirstRepeatDelayEnd = 0;
    };

    // Nudge device source.  This source pulses ON for a specified
    // interval when a specified accelerometer axis enters a specified
    // range.  The activate zone is specified via x and y ranges; when
    // a reading occurs within the given range, the logical source
    // pulses on for the specified pulse time.  After the pulse, the
    // control waits a minimum spacing interval before taking more
    // readings.
    class NudgeDeviceSource : public Source
    {
    public:
        NudgeDeviceSource(
            char axis, bool plus, bool minus, int16_t threshold, uint32_t onTIme_ms, uint32_t resetTime_ms);

        virtual bool Poll() override;

        virtual const char *FullName(char *buf, size_t buflen) const override {
            snprintf(buf, buflen, "Nudge %s%s%c > %d", plus ? "+" : "", minus ? "-" : "", toupper(axis), threshold);
            return buf;
        }

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->sourceType = PinscapePico::ButtonDesc::SRC_ACCEL; }

        // axis - 'x', 'y', 'z'
        char axis;

        // axis data fetcher
        int16_t (*ReadDevice)();

        // axis directions
        bool plus;
        bool minus;

        // threshold level
        int16_t threshold;

        // on time, reset interval in microseconds
        uint32_t onTime_us;
        uint32_t resetTime_us;

        // On-until and off-until times.  We set these each time a new
        // firing event occurs.
        uint64_t onUntilTime = 0;
        uint64_t offUntilTime = 0;
    };

    // Plunger source.  This source reads ON when the plunger position
    // is inside or outside a given range.
    class PlungerSource : public Source
    {
    public:
        PlungerSource(bool fire, int fireOnTime_ms, bool inside, int rangeMin, int rangeMax) :
            fire(fire), fireOnTime_us(fireOnTime_ms*1000), inside(inside), rangeMin(rangeMin), rangeMax(rangeMax) { }
        virtual bool Poll() override;

        virtual const char *FullName(char *buf, size_t buflen) const override;

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->sourceType = PinscapePico::ButtonDesc::SRC_PLUNGER; }

        // Trigger on firing gestures, for the specified time interval
        bool fire;
        uint32_t fireOnTime_us;

        // when a firing event occurs, we set this to the ending time for the ON interval
        uint64_t fireOnUntil = 0;

        // Range triggering type:
        //   true -> trigger when inside the range
        //   false -> trigger when outside the range
        bool inside;

        // range 
        int rangeMin;
        int rangeMax;
    };

    // ZB Launch source.  This source reads ON when ZB Launch detects a
    // plunger gesture.  This can be set to trigger only when ZB Launch
    // Mode is engaged, or always.
    class ZBLaunchSource : public Source
    {
    public:
        ZBLaunchSource(bool modal) : modal(modal) { }
        virtual bool Poll() override;

        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "ZBLaunch"); return buf; }

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->sourceType = PinscapePico::ButtonDesc::SRC_ZBLAUNCH; }

        // modal -> only trigger when ZB Launch Mode is engaged
        bool modal;
    };

    // DOF input source.  This source reads as ON when the associated
    // DOF port is ON.
    class DOFSource : public Source
    {
    public:
        DOFSource(bool rangeInside, int rangeMin, int rangeMax) :
            range(rangeInside, rangeMin, rangeMax) { }
        virtual bool Poll() override;

        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "DOF port %d", portNum); return buf; }

        // populate a vendor interface button descriptor
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // active range
        struct Range
        {
            Range(bool inside, int min, int max) : inside(inside), min(min), max(max) { }
            bool inside;    // true -> active when DOF level is in min..max
            int min;        // low end of range (inclusive)
            int max;        // high end of range (inclusive)
        } range;

        // output port number
        int portNum = 0;

        // Output port pointer.  This has to be stored as a void* because
        // OutputManager::Port is an incomplete nested class at this point,
        // and C++ doesn't currently have any way to declare such a thing.
        void *port = nullptr;
    };

    // -----------------------------------------------------------------------
    //
    // Output actions
    //
    class Action
    {
    public:
        // Handle a change in the logical button state.  This is called
        // each time the logical state changes during input polling.
        virtual void OnStateChange(bool state) = 0;

        // Generate a descriptive name string for logging.  This is
        // meant to be a low-effort string routine using a static buffer
        // that can only has to survive until the next call.  The
        // default implementation calls GenName() to generate the name
        // into the shared static buffer, and returns a pointer to the
        // buffer.  Subclasses that don't need to generate a dynamic
        // name can override Name() to just return a fixed string.
        virtual const char *Name() { GenName(); return nameBuf; }
        virtual void GenName() { }

        // shared static buffer for Name() generation
        static const int NAME_BUF_LEN = 64;
        static char nameBuf[NAME_BUF_LEN];

        // Periodic task processing.  This is called on the button task
        // loop whether or not the state has changed.  Does nothing by
        // default.
        virtual void Task() { }

        // Populate a vendor interface button descriptor with information
        // on the action.  This fills in at least the action type; some
        // subclasses also fill in the detail field.
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const = 0;
    };

    // Null action
    class NullAction : public Action
    {
    public:
        virtual void OnStateChange(bool) override { }
        virtual const char *Name() override { return "Null"; }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->actionType = PinscapePico::ButtonDesc::ACTION_NONE; }
    };

    // Keyboard key action.  This sends a keyboard key-down event to the
    // host PC when the button is pressed.
    class KeyboardKeyAction : public Action
    {
    public:
        KeyboardKeyAction(uint8_t usbKeyCode) : usbKeyCode(usbKeyCode) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "Key(0x%02X)", usbKeyCode); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // The USB key code that we report through the keyboard interface.
        // See the USB specification's HID Usage Tables, Keyboard/Keypad
        // Usage Page 0x07.
        uint8_t usbKeyCode;
    };

    // Media key action.  This reports the assigned media key as pressed
    // when the source button is pressed.
    class MediaKeyAction : public Action
    {
    public:
        MediaKeyAction(USBIfc::MediaControl::Key key) : key(key) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "MediaKey(%d)", static_cast<int>(key)); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // The media key ID
        USBIfc::MediaControl::Key key;
    };

    // Gamepad button action.  This reports the assigned gamepad button
    // as pressed when the logical control is on.
    class GamepadButtonAction : public Action
    {
    public:
        GamepadButtonAction(uint8_t buttonNum) : buttonNum(buttonNum) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "GamepadButton(%d)", buttonNum); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // the gamepad button number that we report, 1-32
        uint8_t buttonNum;
    };

    // Gamepad hat switch action.  This reports the assigned gamepad hat
    // switch button as pressed when the logical control is on.  The hat
    // switches are numbered 0=UP, 1=DOWN, 2=LEFT, 3=RIGHT.
    class GamepadHatSwitchAction : public Action
    {
    public:
        GamepadHatSwitchAction(uint8_t buttonNum) : buttonNum(buttonNum) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "GamepadHatSwitch(%c)", "UDLR?"[buttonNum < 4 ? buttonNum : 4]); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // the hat switch number that we report, 0-3
        uint8_t buttonNum;
    };

    // XInput button action.  This reports the assigned xbox controller
    // button as pressed when the logical control is on.
    class XInputButtonAction : public Action
    {
    public:
        XInputButtonAction(uint8_t buttonNum) : buttonNum(buttonNum) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "XInputButton(%d)", buttonNum); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // the xbox controller button we report, 0-15 (see the button
        // identifiers in USBIfc::XInputInReport for mappings to the
        // button labels on the controller)
        uint8_t buttonNum;
    };

    // Open Pinball Device - generic button action.  This is for buttons
    // mapeed to the numbered generic buttons, 1-32.
    class OpenPinDevGenericButtonAction : public Action
    {
    public:
        OpenPinDevGenericButtonAction(int buttonNum) : buttonNum(buttonNum) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "OpenPinDevGenericButton(%d)", buttonNum); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // generic button number, 1-32
        int buttonNum;
    };

    // Open Pinball Device - pre-defined pinball button action.  The button number
    // is the pinball button index defined in the specification, 0-31.
    class OpenPinDevPinballButtonAction : public Action
    {
    public:
        OpenPinDevPinballButtonAction(int buttonNum) : buttonNum(buttonNum) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "OpenPinDevPinballButton(%d)", buttonNum); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // pinball button index, 0-31
        int buttonNum;
    };

    // Open Pinbal Device - flipper button action.  The button number
    // indicates which flipper button is connected: 0 lower left, 1 lower
    // right, 2 upper left, 3 upper right.
    // Open Pinball Device - generic button action.  This is for buttons
    // mapeed to the numbered generic buttons, 1-32.
    class OpenPinDevFlipperButtonAction : public Action
    {
    public:
        OpenPinDevFlipperButtonAction(int buttonNum) : buttonNum(buttonNum) { }
        virtual void OnStateChange(bool state) override;
        virtual void GenName() override { sprintf(nameBuf, "OpenPinDevFlipperButton(%d)", buttonNum); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override;

        // 0 lower left, 1 lower right, 2 upper left, 3 upper right
        int buttonNum;
    };

        // IR remote control action.  This sends a remote-control code on the
    // IR transmitter when the logical control switches on.
    class IRAction : public Action
    {
    public:
        IRAction(const IRCommandDesc &cmd, bool autoRepeatEnabled) :
            cmd(cmd), autoRepeatEnabled(autoRepeatEnabled) { }
        virtual void OnStateChange(bool state) override;
        virtual const char *Name() override { return "IRSend"; }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->actionType = PinscapePico::ButtonDesc::ACTION_IR; }

        // IR command to send
        IRCommandDesc cmd;

        // is auto-repeat enabled?
        bool autoRepeatEnabled;

        // current logical state
        bool state = false;
    };

    // Night Mode action.  This engages Night Mode globally when the
    // logical control switches on, and disengages when the logical
    // control switches off.
    class NightModeAction : public Action
    {
    public:
        NightModeAction() { }
        virtual const char *Name() override { return "NightMode"; }
        virtual void OnStateChange(bool state) override;
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->actionType = PinscapePico::ButtonDesc::ACTION_NIGHTMODE; }
    };

    // Hardware reset action.  This resets the Pico, either in ROM Boot
    // Loader mode or simply restarting the flash firmware.
    class ResetAction : public Action
    {
    public:
        ResetAction(bool bootLoaderMode, uint16_t holdTime_ms) :
            bootLoaderMode(bootLoaderMode), holdTime_us(static_cast<uint32_t>(holdTime_ms) * 1000) { }
        virtual void GenName() { sprintf(nameBuf, "Reset(%shold=%dms)", bootLoaderMode ? "BootLoader," : "", holdTime_us/1000); }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->actionType = PinscapePico::ButtonDesc::ACTION_RESET; }
        virtual void OnStateChange(bool state) override;
        virtual void Task() override;

        // true -> reset to the ROM boot loader; false -> restart the firmware
        bool bootLoaderMode;

        // button hold time in microseconds - the reset occurs after the
        // button has been held down continuously for this long
        uint32_t holdTime_us;

        // last observed control state
        bool lastState = false;

        // hold start time
        uint64_t t0 = 0;
    };

    // Plunger calibration action.  This activates the virtual plunger
    // calibration button, which must be held down for about 2 seconds
    // to activate calibration mode.
    class PlungerCalAction : public Action
    {
    public:
        PlungerCalAction() { }
        virtual const char *Name() override { return "PlungerCal"; }
        virtual void OnStateChange(bool state) override;
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->actionType = PinscapePico::ButtonDesc::ACTION_PLUNGERCAL; }
    };

    // Macro action.  This executes a timed series of other actions when
    // the button is pressed.
    class MacroAction : public Action
    {
    public:
        MacroAction(bool runToCompletion, bool repeat) :
            runToCompletion(runToCompletion), repeat(repeat) { }
        virtual const char *Name() override { return "Macro"; }
        virtual void PopulateDesc(PinscapePico::ButtonDesc *desc) const override { desc->actionType = PinscapePico::ButtonDesc::ACTION_MACRO; }

        virtual void OnStateChange(bool state) override;
        virtual void Task() override;

        // list of steps - each step is an action, with a start time and duration
        struct Step
        {
            Step(uint32_t start_ms, uint32_t duration_ms, bool hold, Action *action) :
                tStart(start_ms * 1000), tEnd((start_ms + duration_ms) * 1000), hold(hold), action(action) { }
            
            // Start time of this step, microseconds, relative to the MACRO
            // start time.
            //
            // Note that the stored value differs from the nominal value that
            // appears in the macro's JSON data, which specifies the start
            // time relative to the start time of the previous step.  The
            // stored value is the computed offset from the overall macro
            // start time.  The JSON stores the step-to-step relative time
            // because that seems like a more human-friendly way to look at
            // it, and it also avoids confusion by making it structurally
            // impossible for the user to schedule steps out of order.
            uint32_t tStart;

            // End time of this step, microseconds, relative to the macro
            // start time.  This is start_us plus the nominal duration.  This
            // isn't meaningful for a "hold" step.
            uint32_t tEnd;

            // true -> duration:"hold"
            bool hold;

            // action
            std::unique_ptr<Action> action;

            // Current on/off state for the action.  This switches ON at the
            // step's start time when the macro is running, and switches OFF
            // after the step's duration has elapsed.
            bool state = false;
        };
        std::list<Step> steps;

    protected:
        // start the macro running from the beginning
        void StartMacro(bool newSourceState);

        // stop the macro - cancels any curerntly active step
        void StopMacro(bool newSourceState);

        // Internal step scheduler
        void ScheduleSteps(bool newSourceState);

        // Run the macro to completion?  If true, the macro continues to
        // execute until completed, even if the underlying button source
        // is released early.
        bool runToCompletion;

        // Repeat the macro while the buton is down?  If true, the macro
        // starts again each time it finishes with the source button
        // still reading ON.  If false, the macro stops after one run,
        // and won't start again until the next off->on transition.
        // On/off/on changes while the macro is running have no effect;
        // it's the state of the button at the end of the macro playback
        // that matters.
        bool repeat;

        // Latest source state
        bool sourceState = false;

        // Is the macro currently running?
        bool isRunning = false;

        // Start time on the system clock.  This is used to determine
        // when steps start and stop, since the step times are expressed
        // relative to the macro start time.
        uint64_t tStart = 0;

        // Next event start time
        uint64_t tNext = 0;
    };

protected:
    // Poll the button.  The static Task() routine calls this on each button
    // to poll its logical state and fire events as needed on state changes.
    virtual void Poll() = 0;

    // Global button list.  This is a static singleton containing all
    // of the buttons created through Add().
    static std::vector<std::unique_ptr<Button>> buttons;

    // button name index
    static std::unordered_map<std::string, Button*> namedButtons;

    // Button name.  This is an optional arbitrary identifier, assigned
    // in the configuration file, that can be used to refer to the
    // button elsewhere in the configuration.  For example, this can be
    // used to tie a button to an output port via a 'source' expression
    // in the output port.
    std::string name;

    // the button's input source
    std::unique_ptr<Source> source;

    // The button's shift mask and shift bits.  When the underlying
    // input source switches from OFF to ON, we check the current live
    // shift state against the mask and bits:
    //
    //   liveShiftState & shiftMask == shiftBits
    //
    // In other words, the MASK selects which shift bits are included
    // in the test, and BITS indicates the state of each selected bit.
    uint32_t shiftMask;
    uint32_t shiftBits;

    // Remote Wake flag.  If set, the button triggers a USB wakeup
    // packet when pressed (off->on transition).
    bool remoteWake = false;

    // the button's action handler
    std::unique_ptr<Action> action;

    // Set the state of a shift button
    static void SetShiftState(uint32_t bits, bool state);

    // Shift buttons pressed.  This is a bit mask representing the shift
    // buttons currently being held down.
    static uint32_t shiftButtonsPressed;

    // The button's current logical state.  This records the underlying
    // source's logical state as of the last polling cycle.
    bool logicalState = false;
};

// Ordinary pushbutton.  The logical state of the button simply follows
// the state of the underlying source device.  This is suitable for most
// of the physical buttons on a pin cab.
class Pushbutton : public Button
{
public:
    Pushbutton(Source *source, uint32_t shiftMask, uint32_t shiftBits, Action *action) :
        Button(source, shiftMask, shiftBits, action) { }
    virtual uint8_t GetVendorIfcType() const override { return PinscapePico::ButtonDesc::TYPE_PUSH; }

    virtual void Poll() override;

    // last source state
    bool lastSourceState = false;
};

// Push-and-hold button.  This is like a regular pushbutton, but it
// doesn't turn on until it's been held down for a minimum hold time.
class HoldButton : public Button
{
public:
    HoldButton(Source *source, uint32_t shiftMask, uint32_t shiftBits, Action *action, int holdTime_ms) :
        Button(source, shiftMask, shiftBits, action), holdTime_us(holdTime_ms * 1000) { }
    virtual uint8_t GetVendorIfcType() const override { return PinscapePico::ButtonDesc::TYPE_HOLD; }

    virtual void Poll() override;

    // hold time, microseconds
    uint32_t holdTime_us;

    // last source state
    bool lastSourceState = false;

    // current "raw" logical state - this is the logical state before
    // applying the hold time delay
    bool rawLogicalState = false;

    // start time (time_us_64() timestamp) of last off->on transition
    uint64_t tOn = 0;
};
// Toggle Button.  This maps a physical momentary switch into a toggle
// toggle control, where you tap the physical button to flip the state
// of the logical toggle control (push the button to switch the logical
// control ON, push it again to switch it OFF).  This can be used to
// implement a pushbutton Night Mode toggle, for example.
class ToggleButton : public Button
{
public:
    ToggleButton(Source *source, uint32_t shiftMask, uint32_t shiftBits, Action *action) :
        Button(source, shiftMask, shiftBits, action) { }
    virtual uint8_t GetVendorIfcType() const override { return PinscapePico::ButtonDesc::TYPE_TOGGLE; }

    virtual void Poll() override;

    // Underlying control state at last reading.  The toggle state
    // flips when the underlying control changes state from OFF to ON,
    // so we need to record the control state and monitor for changes.
    bool lastSourceState = false;
};

// On/Off Button.  This is an alternate version of the Toggle Button
// that has separate inputs that turn the logical control ON and OFF.
// This is useful for IR inputs, since it lets you assign separate
// ON and OFF buttons on the IR remote.
//
// The ON and OFF sources both trigger on a CHANGE to their state
// from off to on.  If you assign both inputs to pushbutton switches,
// for example, pressing the ON button will turn the logical control
// ON, but after that, it doesn't matter how long you continue holding
// the ON button, because the only thing that counts is the change in
// state when you first pushed it.  If you continue holding the ON
// button, and press the OFF button, it will turn the logical control
// OFF even though you're still holding the ON button.
class OnOffButton : public Button
{
public:
    OnOffButton(Source *onSource, Source *offSource, uint32_t shiftMask, uint32_t shiftBits, Action *action) :
        Button(onSource, shiftMask, shiftBits, action), offSource(offSource) { }
    virtual uint8_t GetVendorIfcType() const override { return PinscapePico::ButtonDesc::TYPE_ONOFF; }

    virtual void Poll() override;

    // prior ON source state
    bool lastOnSourceState = false;

    // The OFF source, and its prior state.  (The inherited source
    // in the base class is used as the ON source.)
    std::unique_ptr<Source> offSource;
    bool lastOffSourceState = false;
};

// Pulse button.  This maps a physical toggle switch (such as a slide
// switch or paddle switch) into a pulse generator.  Each time the
// physical switch changes state, the logical control sends a brief
// pulse to the host.  This is useful in cases where the physical
// installation makes it most convenient to use a toggle switch, but
// where the software requires a key press to signal each state
// change.
//
// If the pulse button uses a shift mask, then the shift buttons
// control the pulse generation.  Pulses are only generated when the
// shift button state matches.  I can't see any sensible uses cases
// for shifted pulse buttons with the ordinary type of shift buttons,
// but it might be useful to enable/disable a pulse button according
// to a mode, using an abstract shift state to represent the mode.
// For example, you could associate a shift bit with a DOF signal
// that's activated when certain games are loaded on the PC side, and
// then use that shift bit to enable/disable a pulse button assigned
// to the coin door switch.  Games that need the pulse signal would
// activate the DOF signal, enabling the coin door switch pulse
// signal; other games would skip the DOF signal, leaving the coin
// door pulse signal disabled.
//
// The classic pin cab use case for the pulse button is the Coin Door
// switch.  Real pinball machines implement the coin door switch with
// a spring-loaded plunger switch, similar to the door light switch in
// a typical refrigerator, where the door pushes down on the plunger
// when the door is closed, closing the switch.  That's also the best
// way to implement a physical coin door switch in a virtual pin cab.
// However, that physical switch arrangement clashes with older
// versions of PinMame, which were designed only with desktop PC play
// in mind, and thus used a keyboard key to toggle PinMame's notion of
// the coin door open/closed status.  The pulse switch bridges the gap
// by generating the required key press each time the physical coin
// door switch changes from CLOSED to OPEN, or vice versa.  Newer
// versions of PinMame allow for a more direct mapping to a physical
// switch, so most people no longer need a pulse button for this
// particular use case.  But it might still be useful for other
// situations.
//
// The control can send the same action for both pulses (the "on"
// pulse that's sent when the underlying control switches on, and the
// "off" pulse that's sent when the underlying control switches off),
// or it can send separate actions.  Use the same action for cases
// where the host uses a toggle key, such as the old PinMame coin
// door input.  Use separate actions where the host has separate
// ON and OFF commands for the target operation, such as a TV with
// an IR remote that has separate Power On and Power Off buttons.
class PulseButton : public Button
{
public:
    // Set up the button.  Note that all time intervals are
    // expressed in milliseconds.
    PulseButton(Source *source, uint32_t shiftMask, uint32_t shiftBits,
                Action *action, Action *offAction,
                uint16_t onPulseTime_ms, uint16_t offPulseTime_ms,
                uint16_t minPulseSpacing_ms);
    virtual uint8_t GetVendorIfcType() const override { return PinscapePico::ButtonDesc::TYPE_PULSE; }

    virtual void Poll() override;

protected:
    // The "off" action, activated during the "off" pulse period.
    // The inherited action is used as the "on" action.  If this
    // is null, the same action is used for both pulses.
    std::unique_ptr<Action> offAction;

    // ON pulse time in microseconds.  This is the length of the
    // pulse generated when the underlying physical control switches
    // from OFF to ON.  This can be set to zero if no ON pulse is
    // needed.
    uint32_t onPulseTime_us;

    // OFF Pulse time in microseconds.  This is the length of the
    // pulse generated when the underlying physical control switches
    // from ON to OFF.  This can be set to zero if no OFF pulse i
    // needed.
    uint32_t offPulseTime_us;

    // Minimum time between pulses, in microseconds.  A new pulse
    // will be delayed until this minimum time elapses after the
    // end of the previous pulse, to ensure that the pulses are
    // spaced out enough that the host recognizes each pulse as
    // a distinct input.
    uint32_t minPulseSpacing_us;

    // Current pulse state
    enum class PulseState
    {
        None,       // no pulse in progress
        Pulse,      // sending a pulse
        Space,      // sending a space between pulses
    };
    PulseState pulseState = PulseState::None;

    // current pulse action
    Action *pulseAction = nullptr;

    // ending time of current pulse
    uint64_t tEnd = 0;
};

// Shift button.  When this button is held down, pressing another
// button activates the other button's "shifted" function instead of
// its main function.
//
// The shift button can also have an action of its own, which it
// can carry out immediately when the button is pushed, OR only when
// the button is released and no other button's shifted function was
// selected while the shift button was being pressed.  The first
// mode, where the action is carried out immediately, is what we
// call the "Shift-AND" function, meaning that the button always
// performs both its Shift function AND its regular action function
// on every push.  The second mode, where the button's own action
// only occurs if the Shift function wasn't engaged (by pressing
// some other button at the same time), is what we call "Shift-OR",
// since only one of the button's two functions is executed on any
// given push.
//
// The same button object handles both modes.  If the button has a
// non-zero pulse time, the Shift-OR mode is used, with the action
// performed only as a timed pulse after the button is released,
// and then only if no other button was pressed while the button
// was being held.  If the pulse time is zero, the Shift-AND mode
// is used, with the action immediately engaged when the button is
// pushed, and held on as long as the button is held down.
class ShiftButton : public Button
{
public:
    ShiftButton(Source *source, uint32_t shiftMask, uint32_t shiftBits, Action *action, uint16_t pulseTime_ms);
    virtual uint8_t GetVendorIfcType() const override { return PinscapePico::ButtonDesc::TYPE_SHIFT; }

    virtual void Poll() override;
    virtual void OnShiftUsage(uint32_t maskedBits) override;

    // pulse times in microseconds
    uint32_t pulseTime_us;

    // Flag: some other button's shifted button function was
    // engaged during the current push on this shift button
    bool shiftUsed = false;

    // is a post-press pulse in progress?
    bool pulseActive = false;

    // ending time of current pulse
    uint64_t tPulseEnd = 0;
};
