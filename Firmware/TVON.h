// Pinscape Pico - TV ON
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This implements the Pinscape TV ON feature, which is designed to help
// make a virtual pinball cabinet's power-up sequence more seamless, by
// switching on any TVs or monitors that don't turn on by themselves
// when power is restored.
//
// Most people set up their pin cabs with a master power switch that
// cuts power at the outlet to all of the secondary devices when the
// main PC is powered down.  When the computer is restarted, power is
// restored at the outlet.  Some TVs and most computer monitors work
// well in this kind of setup, because they remember their power state
// before an outage, and automatically switch back on when power is
// restored.  But many TVs stay in stand-by mode when power is restored,
// and have to be switched back on manually, by pressing an on/off
// button on the TV or by using the remote control.  In a pin cab
// context, it's a nuisance to have to turn on your TVs one by one, and
// many builders think it makes the cab feel unfinished; most of us want
// the cab to feel like a fully integrated appliance rather than a
// collection of miscellaneous parts.
// 
// The TV ON feature helps by automating those manual switching tasks at
// system startup.  It has three elements:
//
// 1. External power-detect circuitry that detects when the overall
// system has just come out of a power cycle.  This is necessary to
// distinguish actual power cycles from mere resets, either to the Pico
// or to the main PC host.  The Pico on its own can't distinguish a
// power cycle from a reset or a host PC reboot.  What's more, in most
// systems, the Pico itself is never actually power-cycled, not even at
// a full system shutdown, because it receives its power through the
// host computer's USB port, which in most systems will continue to
// supply USB power even when the computer is in Soft Shutdown mode.
// The external circuit is designed to monitor the *secondary* power
// supply, which on most systems is fully switched off at the outlet
// when the system has been shut down; and the external circuit has its
// own simple memory that's stable across Pico resets.  This allows the
// Pico to determine when a full system power cycle has occurred,
// independently of the Pico's own reset cycle.
//
// The power-detect circuit is optional.  The TV ON feature can also be
// triggered via USB commands from the host, so you can use one of the
// several Windows run-at-startup mechanisms to invoke the TV ON
// mechanics after each Windows boot.  I think the power-detect
// circuitry is preferable, since it's more reliable at distinguishing
// power cycles from Windows reboots, and it can be timed independently
// of the Windows startup process (which can be quite lengthy), but many
// people might prefer the all-software approach to building yet more
// circuitry.
//
// 2. IR Remote control integration.  Pinscape has a separate subsystem
// that acts as a universal learning IR remote: it can work with a
// physical IR receiver device to learn codes from most IR remotes, and
// it can use a physical IR emitter (which is just an LED that emits
// infrared light rather than visible light) to transmit previously
// learned codes or manually programmed codes.  The TV ON subsystem
// works with the IR subsystem to transmit designated IR codes when a
// power transition event occurs.  This is the recommended way to
// implement the TV ON feature in a pin cab, because virtually all
// modern TVs work with IR remotes, and sending an IR command doesn't
// require any invasive modification of the TV itself.
//
// 3. A "TV Relay", to pulse the TV's physical ON button, if it has one.
// This is the original solution, but I consider it deprecated, because
// it requires physically opening the TV's casing and soldering wires to
// the ON button.  The idea here is that almost all modern TVs have soft
// power switching - they never truly power off, but switch between ON
// and "standby" mode.  Standby mode is a low-power state where the TV
// shuts off the display screen but continues to power its IR receiver
// and button controls, so that it can be turned on via an IR command.
// If the TV has a physical ON button at all (many TVs today don't),
// it's almost always a momentary pushbutton that sends a logic signal
// to the control unit, rather than a physical on/off switch.  The TV
// Relay is designed to work with this sort of physical button, by
// briefly toggling the relay ON to simulate a press of the button.  To
// make this work, you have to physically wire the switch contacts in
// the TV to the wires that run to the Pinscape TV Relay.  When the
// relay momentarily closes, the TV reads it as a momentary press on the
// ON button, and the TV powers up.
//
// The Pinscape Pico expansion boards don't include a dedicated TV relay
// (the original Pinscape KL25Z expansion boards did include one as a
// standard feature).  I didn't think it was worth including the extra
// parts given that I consider the hard-wired approach deprecated; I
// recommend using IR instead.  But you can still implement a TV relay
// if you really want to, by using one of the general-purpose output
// ports.  Connect a small relay to the port, and configure TV ON to use
// the port as the relay output.
// 

#pragma once

// standard headers
#include <stdint.h>
#include <stdarg.h>
#include <vector>

// project headers
#include "JSON.h"
#include "Reset.h"
#include "IRRemote/IRCommand.h"
#include "Buttons.h"
#include "Outputs.h"
#include "../USBProtocol/VendorIfcProtocol.h"

// externals
class ConsoleCommandContext;


class TVON
{
public:
    // construction
    TVON();

    // configure
    void Configure(JSONParser &json);

    // perform periodic timed tasks
    void Task();

    // is the TV relay pulsed on?
    bool IsRelayOn() const { return relayState != 0; }

    // TV on state
    enum class State
    {
        PowerOff = 0,      // power was off at least check
        PulseLatch = 1,    // pulsing the latch SET pin to test the latch status
        TestLatch = 2,     // test the latch SENSE pin to see if the pulse stuck
        Countdown = 3,     // off-to-on power transition detected, delay countdown in progress
        RelayOn = 4,       // pulsing relay
        IRReady = 5,       // ready to send next IR command
        IRWaiting = 6,     // waiting for IR pause between commands
        IRSending = 7,     // IR command sent, waiting for transmission to complete
        PowerOn = 8,       // power is on
        PicoBoot = 9,      // Pico boot state - external power state is not yet known

        // last state marker - not an actual state, just a high-water mark for the enum int values
        LAST
    };

    // get the current state
    State GetState() const { return state; }

    // characterize the state
    bool IsInCountdown() const { return state == State::Countdown; }
    bool IsSendingCommands() const {
        return state == State::RelayOn || state == State::IRReady
            || state == State::IRWaiting || state == State::IRSending;
    }

    // Set the manual TV relay state (for the USB interface)
    void ManualSetRelay(bool on);

    // Pulse the TV relay manually (for the USB interface)
    void ManualPulseRelay();

    // Populate a Vendor Interface TV-ON state query result
    void Populate(PinscapePico::VendorResponse::Args::TVON *state);

protected:
    // update the TV relay state by setting or clearing a bit
    void UpdateRelayState(uint8_t mask, bool set);

    // delay time after system startup before TV ON commands begin
    uint32_t startupDelayTime = 0;

    // relay pulse time
    uint32_t relayPulseTime = 250;

    // Power Detect "Sense" port, as a button source port
    std::unique_ptr<Button::Source> sensePort;

    // Power Detect "Set" port, as an output device
    std::unique_ptr<OutputManager::Device> setPort;

    // Simulation state.  The command console has a simulation mode to
    // test the logic.  When the simulation is active, we override the
    // physical GPIO pins and use a state simulator instead.
    struct Simulation
    {
        // simulation is active
        bool active = false;

        // simulated power state
        bool powerOn = false;

        // simulated latch state ("sense" pin)
        bool latch = false;
    };
    Simulation sim;

    // GPIO interface - uses the simulator if active, otherwise the physical pins
    bool ReadSensePin();
    void WriteSetPin(bool state);

    // Current state
    State state = State::PicoBoot;

    // Next state update time.  Each step in the state machine has a
    // delay time until the next step; this records the scheduled next
    // state update time.
    uint64_t tNextState = 0;

    // Countdown end time.  This is valid during State::Countdown.
    uint64_t tCountdownEnd = 0;

    // TV relay port
    std::unique_ptr<OutputManager::Device> relayPort;

    // Relay mode
    enum class RelayMode
    {
        Manual,      // manual mode - relay is controlled only by host commands
        Pulse,       // pulse mode - timed pulse at system power on
        Switch,      // switch mode - stay on continuously after system power on
    };
    RelayMode relayMode = RelayMode::Pulse;
    
    // TV relay state.  The relay can be controlled from the power-on
    // timer and manually from the PC via USB commands.  We keep
    // separate states for each source.
    static const uint8_t RELAY_STATE_POWERON = 0x01;
    static const uint8_t RELAY_STATE_MANUAL = 0x02;
    static const uint8_t RELAY_STATE_MANUAL_PULSE = 0x04;
    uint8_t relayState = 0;

    // End time for a manual relay pulse
    uint64_t tManualRelayPulseEnd = 0;

    // system reset locker
    PicoReset::Lock *resetLock = nullptr;

    // IR commands
    struct IR
    {
        IR(uint32_t delay_us, int repeatCount, const IRCommandDesc &cmd) : delay_us(delay_us), repeatCount(repeatCount), cmd(cmd) { }
        uint32_t delay_us;      // delay time before this command, microseconds
        int repeatCount;        // number of repeats (1 = send once)
        IRCommandDesc cmd;      // the command to send
    };
    std::vector<IR> irList;

    // IR command index.  This is used when the main state is
    // State::SendIR, to keep track of the current IR command we're
    // sending.  Multiple IR commands can be designated as TV ON
    // commands, and each one takes long enough to send that we have to
    // send it asynchronously, so this keeps track of which one we're
    // currently working on.
    int irIndex = 0;

    // Last IR completion time
    uint64_t irTime = 0;

    // command handler
    static void Command_main(const ConsoleCommandContext *ctx);
};

// global singleton
extern TVON tvOn;
