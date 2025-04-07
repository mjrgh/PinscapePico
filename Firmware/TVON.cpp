// Pinscape Pico - TV ON
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The TV ON feature uses an external power detection circuit to detect
// when the overall system power has been switched from OFF to ON.  The
// external circuit monitors the power to the secondary power supply,
// which is assumed to switch off entirely when the system is off.  This
// lets us detect system power transitions even if the Pico itself is
// powered continuously from the USB port when the PC is in Soft-Off
// mode.
//
// When an OFF-to-ON system power transition occurs, the TV ON feature
// starts a countdown timer (with a time interval that's configurable by
// the user).  When the countdown finishes, the TV ON system can pulse a
// designated output port ON briefly (the port and pulse time are both
// configurable), with the intention of triggering an external relay
// that is in turn hard-wired to the TV's Soft-On button, to switch the
// TV on.  The TV ON system can also send a list of command codes
// through the IR transmitter system; by listing the TV's POWER ON
// command, the IR transmission will turn the TV on.  The IR list can
// contain multiple commands, so it can turn on more than one TV and
// perform any other necessary start-up commands.  Both the relay and IR
// features are optional; either, both, or neither can be used.


// standard library headers
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/watchdog.h>

// local project headers
#include "JSON.h"
#include "TVON.h"
#include "GPIOManager.h"
#include "Logger.h"
#include "Outputs.h"
#include "Reset.h"
#include "IRRemote/IRTransmitter.h"
#include "CommandConsole.h"
#include "../USBProtocol/VendorIfcProtocol.h"


// global singleton
TVON tvOn;

// Magic numbers for watchdog SCRATCHn register state encodings.  These
// are arbitrary bit patterns that we use check the integrity of the
// scratch registers after a reboot, on the theory that it's unlikely
// that either random garbage or intentional conflicting use of the
// registers (by other subsystems or Pico SDK components) would
// accidentally match these bit patterns.
static const int SCRATCH2_MAGIC = 0x96DAD899;
static const int SCRATCH3_MAGIC = 0x864ABF16;

// Construction
TVON::TVON()
{
}

// Configuration
//
// tvon: {
//   delay: <number>,           // delay time in milliseconds after system startup before sending TV ON signals
//   powerDetect: {
//     sense: <number>,         // GPIO port for the SENSE line from power detect circuit
//     set: <number>,           // GPIO port for the SET line from power detect circuit
//   },
//   relay: {                   // optional relay, hard-wired to the TV's Soft-ON button
//     port: <number>|<string>, // output port (by number or name) where the TV relay is connected; must be configured as a virtual output port
//     pulseTime: <number>,     // duration in millseconds of TV ON relay pulse
//     mode: "pulse",           // "pulse" -> pulse at system power on
//                              // "switch" -> stay on continuously after system power on
//                              // "manual" -> relay is controlled only by host commands
//   },
//   IR: [                      // optional IR commands to transmit when the timer expiers
//     "xx.xx.xxxxxx",          // first command code, in our universal IR code format (protocol.flags.code, as hex digits)
//     5000,                    // optionally, a number gives a time delay in milliseconds before sending the next command
//     "xx.xx.xxxxxx",          // second command, sent after delay above
//     "xx.xx.xxxxxx",          // third command, sent immediately after the second (since no numeric delay time is included)
//   ], 
// }
//
// The TV relay port can be configured either here or in the outputs[]
// list:
//
// - To configure it here, set relayPort to the port number or port name
//   of a port defined as type "virtual" in the outputs[] array.
//
// - To configure it in the outputs[] list instead, set the output
//   source to "tvon".  The effect is the same either way.  The
//   outputs[] list approach can also be used to configure multiple
//   ports to trigger on the relay, if desired.
//
void TVON::Configure(JSONParser &json)
{
    // check for a configuration
    if (auto *tvon = json.Get("tvon") ; !tvon->IsUndefined())
    {
        // check for the power detect parameters
        if (auto *detect = tvon->Get("powerDetect") ; !detect->IsUndefined())
        {
            // parse the sense port as a button source
            sensePort.reset(Button::ParseSource("tvon.powerDetect.sense", "TV ON Sense", json, detect->Get("sense"), "high", true));

            // Parse the set port as an output device.  Use digital mode (not PWM)
            // by default, since this is a digital signal to the power-sense circuit.
            setPort.reset(OutputManager::ParseDevice("tvon.powerDetect.set", "TV ON Set", json, detect->Get("set"), false, true));

            // if both are valid, proceed with TV ON configuration
            if (sensePort != nullptr && setPort != nullptr)
            {
                // log the successful setup
                char sensePortName[40], setPortName[40];
                Log(LOG_CONFIG, "TV ON Power Sense circuit configured; Sense port=%s, Set port=%s\n",
                    sensePort->FullName(sensePortName, sizeof(sensePortName)),
                    setPort->FullName(setPortName, sizeof(setPortName)));
            }
        }

        // check for relay parameters
        if (auto *relay = tvon->Get("relay") ; !relay->IsUndefined())
        {
            // read the mode
            if (auto *modeVal = relay->Get("mode"); !modeVal->IsUndefined())
            {
                std::string mode = modeVal->String();
                std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                if (mode == "pulse")
                    relayMode = RelayMode::Pulse;
                else if (mode == "switch")
                    relayMode = RelayMode::Switch;
                else
                    Log(LOG_ERROR, "TV ON: unknown relay mode \"%s\"; using default \"pulse\" mode\n", mode.c_str());
            }
            
            // set the pulse time
            relayPulseTime = relay->Get("pulseTime")->UInt32(250);

            // If an output port is specified, parse it.  Use digital mode (not PWM)
            // by default, since the relay doesn't derive any benefit from PWM.
            if (const auto *port = relay->Get("port"); !port->IsUndefined())
            {
                relayPort.reset(OutputManager::ParseDevice("tvon.relay.port", "TV Relay", json, port, false, true));
                if (relayPort != nullptr)
                {
                    char buf[40];
                    Log(LOG_CONFIG, "TV ON relay control port configured on %s\n", relayPort->FullName(buf, sizeof(buf)));
                }
            }
        }

        // check for an IR command list
        if (auto *ir = tvon->Get("IR") ; !ir->IsUndefined())
        {
            // make sure it'a an array
            if (!ir->IsArray())
                Log(LOG_ERROR, "tvon.IR must be an array; ignored\n");

            // populate the list
            uint32_t delay = 0;
            ir->ForEach([this, &delay](int index, const JSONParser::Value *ele)
            {
                // check the type
                if (ele->IsNumber())
                {
                    // A number represents a delay time before the next
                    // command.  Add this into the delay accumulator,
                    // which we'll apply to the next code we find.
                    delay += ele->UInt32();
                }
                else
                {
                    // A string is a command code.  Parse it.
                    IRCommandDesc cmd;
                    std::string str = ele->String();

                    // Check for a repeat count prefix - "number *".  Start
                    // by skipping spaces and then scanning leading decimal
                    // digits.
                    int acc = 0;
                    int repeatCount = 1;
                    const char *p = str.c_str();
                    for ( ; isspace(*p) ; ++p) ;
                    for ( ; isdigit(*p) ; ++p)
                    {
                        // accumulate leading digits
                        acc *= 10;
                        acc += *p - '0';
                    }

                    // now skip spaces and check for a '*'
                    for ( ; isspace(*p) ; ++p) ;
                    if (*p == '*')
                    {
                        // Found it - apply the repeat count and skip the '*'
                        repeatCount = acc < 1 ? 1 : acc;
                        ++p;
                    }
                    else
                    {
                        // no '*', so there's no reepat count after all; treat
                        // the whole string as the command portion
                        p = str.c_str();
                    }

                    // parse the command portion
                    if (cmd.Parse(p))
                    {
                        // success - add it to our list, with any delay
                        // that was queued up in previous elements
                        irList.emplace_back(delay * 1000, repeatCount, cmd);

                        // this consumes the accumulated delay
                        delay = 0;
                    }
                    else
                    {
                        // parse error
                        Log(LOG_ERROR, "tvon.IR[%d]: \"%s\" is not a valid IR command code\n",
                            index, str.c_str());
                    }
                }
            });
        }

        // Allocate a system reset locker, so that we can lock out
        // discretionary resets at delicate points in the power status
        // checker.  This doesn't preclude hard resets, but we can at
        // least prevent self-inflicted interruptions at critical points.
        resetLock = picoReset.CreateLock();

        // If we're coming out of a soft reset via the watchdog, check
        // to see if the watchdog scratch registers contain a valid
        // saved state.  To be valid, SCRATCH2 and SCRATCH3 must match
        // after applying the respective XOR magic numbers, and the
        // value must be a valid enum State element (when reinterpreted
        // as integer).
        if (watchdog_caused_reboot())
        {
            uint32_t a = watchdog_hw->scratch[2] ^ SCRATCH2_MAGIC;
            uint32_t b = watchdog_hw->scratch[3] ^ SCRATCH3_MAGIC;
            if (a == b && a >= 0 && a < static_cast<uint32_t>(State::LAST))
            {
                // restore the state from the previous session
                state = static_cast<State>(a);
            }

            Log(LOG_DEBUG, "TV ON: initial state %d set from watchdog memory\n", static_cast<int>(state));
        }
        else
        {
            // keep the default initial "Pico Reboot" state
            Log(LOG_DEBUG, "TV ON: no watchdog memory found, initial state is %d\n", static_cast<int>(state));
        }

        // set up the console command
        CommandConsole::AddCommand(
            "tvon", "test TV ON functions",
            "tvon [options]\n"
            "   -s, --status      show current status\n"
            "   --pulse-relay     pulse the TV relay\n"
            "   --relay-on        activate the TV relay in manual mode\n"
            "   --relay-off       deactivate the TV relay in manual mode\n"
            "   --simulate-on     simulate a Power On condition on the power sense circuit\n"
            "   --simulate-off    simulate a Power Off condition on the power sense circuit\n"
            "   --simulate-end    end the power sense circuit simulation\n",
            &TVON::Command_main);
        
        // get the basic parameters
        startupDelayTime = tvon->Get("delay")->UInt32(7000);
        Log(LOG_CONFIG, "TV ON configured; startup delay time %d ms\n", startupDelayTime);
    }
}

// Periodic tasks
void TVON::Task()
{
    // end the manual relay pulse, if in effect
    uint64_t now = time_us_64();
    if ((relayState & RELAY_STATE_MANUAL_PULSE) != 0 && now >= tManualRelayPulseEnd)
        UpdateRelayState(RELAY_STATE_MANUAL_PULSE, false);

    // If the power-detect circuit isn't configured, there's nothing
    // more to do.
    if (setPort == nullptr || sensePort == nullptr)
        return;

    // if the next state check time hasn't arrived yet, there's nothing
    // to do on this pass
    if (now < tNextState)
        return;

    // set the default next state check time for 1/4 second from now
    tNextState = now + 250000;

    // continue from the current state
    switch (state)
    {
    case State::PowerOff:
        // Power was off at last check.  Try pulsing the latch SET pin
        // to see if the latch sticks.
        state = State::PulseLatch;
        WriteSetPin(true);

        // We only have to toggle the pin for a few microseconds, but
        // leave it on for long enough for a user to see in a PC-side
        // UI, to make it easier to troubleshoot.
        tNextState = now + 100000;
        break;

    case State::PulseLatch:
        // We pulsed the latch SET pin to test the latch status.  End
        // the pulse and proceed to testing the latch.
        WriteSetPin(false);
        state = State::TestLatch;

        // We can test again almost immediately, since the new state
        // will only appear on the test pin if PUS2 power is on.  But
        // stay here long enough to show up in the Config Tool
        // visualization, so that it's easier to troubleshoot when
        // it's not working.
        tNextState = now + 100000;
        break;

    case State::TestLatch:
        // We pulsed the latch to see if power has switched on.  Test
        // the latch via the SENSE input.  If it's now ON, an OFF->ON
        // power transition has occurred.
        if (ReadSensePin())
        {
            // SENSE line is on -> power-detect latch memory is working
            // -> power is on -> start countdown
            state = State::Countdown;
            tCountdownEnd = now + (startupDelayTime * 1000);
        }
        else
        {
            // SENSE line is off -> system power is still off -> return
            // to Power Off state and repeat the whole power sensing test
            // process
            state = State::PowerOff;
            tNextState = now + 100000;
        }
        break;

    case State::Countdown:
        // TV timer countdown in progress.  The latch has to stay on
        // throughout the countdown; if it's off again, power was cut
        // before the timer expired, and we need to start over from
        // scratch.
        if (!ReadSensePin())
        {
            // the latch pin went low, so the power is now oww
            WriteSetPin(true);
            state = State::PowerOff;
            tNextState = now + 100000;
            break;
        }

        // check for timer expiration
        if (now >= tCountdownEnd)
        {
            // Startup countdown ended.  It's time to fire the TV relay and
            // IR commands.
            
            // fire the relay if it's in PULSE or SWITCH mode
            if (relayMode == RelayMode::Pulse || relayMode == RelayMode::Switch)
                UpdateRelayState(RELAY_STATE_POWERON, true);

            // advance to Relay On state, and stay there for the relay pulse time
            state = State::RelayOn;
            tNextState = now + (relayPulseTime * 1000);
        }
        break;

    case State::RelayOn:
        // Relay pulse state.  If the relay is set to PULSE mode, switch it off to
        // end the pulse.  (We just leave it on forever in SWITCH mode, and we leave
        // it unchanged in MANUAL Mode.)
        if (relayMode == RelayMode::Pulse)
            UpdateRelayState(RELAY_STATE_POWERON, false);

        // advance to IR Ready state
        state = State::IRReady;
        irIndex = 0;
        break;

    case State::IRReady:
        // Ready to send the next IR command.
        if (irIndex < irList.size())
        {
            // advance to IRWaiting, and schedule the next delay time
            state = State::IRWaiting;
            irTime = now + irList[irIndex].delay_us;

            // check back immediately
            tNextState = 0;
        }
        else
        {
            // There are no more IR commands to send, so we've reached
            // the end of the power-on procedure.  Go to Power On state.
            state = State::PowerOn;
        }
        break;

    case State::IRWaiting:
        // IR wait between commands.  Check if we've reached the delay time,
        // and the IR transmitter is available.
        if (now >= irTime && !irTransmitter.IsBusy())
        {
            // ready to send
            auto &l = irList[irIndex];
            irTransmitter.QueueCommand(l.cmd, l.repeatCount);

            // we're now waiting for the command to complete
            state = State::IRSending;
        }

        // check back soon
        tNextState = now + 1000;
        break;

    case State::IRSending:
        // Sending IR signals.  If the IR transmitter is idle, advance
        // to the next step.
        if (!irTransmitter.IsSending())
        {
            // transmission done - ready for the next IR command
            ++irIndex;

            // if we've reached the last command, this is the end of
            // the power-up procedure, so we've reached the Power On
            // state; otherwise go back for the next transmission
            state = (irIndex < irList.size()) ? State::IRReady : State::PowerOn;
        }
        break;

    case State::PowerOn:
        // Power was on at last check.  Monitor the power latch to
        // detect an ON->OFF transition.  When that occurs, to go
        // Power Off state.
        if (!ReadSensePin())
        {
            // latch is off - power has been removed
            state = State::PowerOff;
            tNextState = now + 100000;
        }
        else
        {
            // power is still on - remain in the current state and
            // check back in a bit
            tNextState = now + 250000;
        }
        break;

    case State::PicoBoot:
        // The Pico just rebooted, so the external state is not yet
        // known.  Read the sense pin to determine the state.  The
        // sense pin retains its memory across Pico boots, so if it's
        // currently reading ON, we must go directly to POWER ON mode
        // without going through an off-to-on transition (sending the
        // power-on TV ON commands), since we presumably already
        // completed that in a prior session.
        if (ReadSensePin())
        {
            // external power was already detected on - go straight
            // to POWER ON mode
            state = State::PowerOn;
            tNextState = now + 250000;
        }
        else
        {
            // external power is off - go to POWER OFF mode
            state = State::PowerOff;
            tNextState = now + 100000;
        }
        Log(LOG_DEBUG, "TV ON: reboot transition to state %d\n", static_cast<int>(state));
        break;
    }

    // Stash the TV ON state in watchdog scratch registers SCRATCH0
    // and SCRATCH1, encoded with the magic XOR patterns.  The magic
    // numbers are just a way to check that values we find in the
    // registers after a reboot were put there intentionally, since
    // it's unlikely that random values put there would agree after
    // applying the separate XOR patterns.
    //
    // Note that only works in limited situations.  The scratch
    // registers only survive soft resets, not BOOTSEL resets, hard
    // resets via the RUN pin, or power cycles.  But we might as well
    // try to preserve the state across resets when we can.
    watchdog_hw->scratch[2] = static_cast<uint32_t>(state) ^ SCRATCH2_MAGIC;
    watchdog_hw->scratch[3] = static_cast<uint32_t>(state) ^ SCRATCH3_MAGIC;

    // Lock out discretionary reboots while we're in any transitional
    // power state.  We can't reliably figure out which transitional
    // state we were in after a reset, because the power-sense latch
    // will remain latched if power was on.  The On and Off states
    // are the only ones we can return to reliably.
    switch (state)
    {
    case State::PowerOn:
    case State::PowerOff:
        // we can return to these states by testing the latch
        resetLock->SetLocked(false);
        break;

    default:
        // we can't return to other states, so lock out resets
        resetLock->SetLocked(true);
        break;
    }
}

bool TVON::ReadSensePin()
{
    return sim.active ? sim.latch :
        sensePort != nullptr ? sensePort->Poll() :
        false;
}

void TVON::WriteSetPin(bool state)
{
    if (sim.active)
    {
        // the latch clears when power is off
        if (!sim.powerOn)
            sim.latch = false;

        // setting the state sets the latch if power is on
        if (sim.powerOn && state)
            sim.latch = true;
    }
    else if (setPort != nullptr)
    {
        // set the latch port
        setPort->Set(state ? 255 : 0);
    }
}

void TVON::ManualSetRelay(bool on)
{
    // set/clear the manual relay bit
    UpdateRelayState(RELAY_STATE_MANUAL, on);
}

void TVON::ManualPulseRelay()
{
    // set the manual pulse relay bit, and set the timeout
    UpdateRelayState(RELAY_STATE_MANUAL_PULSE, true);
    tManualRelayPulseEnd = time_us_64() + relayPulseTime * 1000;
}

void TVON::UpdateRelayState(uint8_t mask, bool set)
{
    // set or clear the mask bits
    if (set)
        relayState |= mask;
    else
        relayState &= ~mask;

    // If there's a relay port configured, set the port state ON if any bits
    // are set, OFF otherwise.  Since the port uses DOF-port semantics, it's
    // nominally a PWM port, but the relay is just a digital on/off device,
    // so set the port to either 0% or 100% (level 255) duty cycle.
    if (relayPort != nullptr)
        relayPort->Set(relayState != 0 ? 255 : 0);
}

// Populate a Vendor Interface TV-ON state query result
void TVON::Populate(PinscapePico::VendorResponse::Args::TVON *state)
{
    // Make sure that the constant defintions in the USB interface definition
    // match our own.  It's bad form to define the same constants in two places,
    // but we have conflicting constraints that make it seem like the least bad
    // option.  It's critical for USBProtocol/VendorIfcProtocol.h to be self-
    // contained, because that's for inclusion by external clients, and I don't
    // want to create the usual open-source build nightmare where every client
    // has to include every dependency of the main project.  But it makes no
    // sense for VendorIfcProtocol.h to serve as the source-of-truth for the
    // state constants, since they're integral to the inner workings of the
    // class; they must be defined in our own header.  I think the right way
    // to look at it is that the public API (the Vendor Interface protocol)
    // exposes a *description* of our internal state, which need not use the
    // same binary representation, but *just happens to*.  So PWR_DEFAULT
    // could actually be a different number, or even a string constant, and
    // we'd just map to it here.  The simplest mapping, and the one we use at
    // the moment, is to simply make PWR_DEFAULT == State::Default after the
    // nominal C++ type conversion (enum vs uint8_t).  So the mapping function
    // is simply = (native assignment).  But we do have to make sure that the
    // mapping will work as expected, thus the asserts.
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_OFF == static_cast<uint8_t>(State::PowerOff));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_PULSELATCH == static_cast<uint8_t>(State::PulseLatch));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_TESTLATCH == static_cast<uint8_t>(State::TestLatch));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_COUNTDOWN == static_cast<uint8_t>(State::Countdown));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_RELAYON == static_cast<uint8_t>(State::RelayOn));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_IRREADY == static_cast<uint8_t>(State::IRReady));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_IRWAITING == static_cast<uint8_t>(State::IRWaiting));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_IRSENDING == static_cast<uint8_t>(State::IRSending));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_ON == static_cast<uint8_t>(State::PowerOn));
    static_assert(PinscapePico::VendorResponse::Args::TVON::PWR_PICOBOOT == static_cast<uint8_t>(State::PicoBoot));
    static_assert(PinscapePico::VendorResponse::Args::TVON::RELAY_STATE_POWERON == RELAY_STATE_POWERON);
    static_assert(PinscapePico::VendorResponse::Args::TVON::RELAY_STATE_MANUAL == RELAY_STATE_MANUAL);
    static_assert(PinscapePico::VendorResponse::Args::TVON::RELAY_STATE_MANUAL_PULSE == RELAY_STATE_MANUAL_PULSE);

    // populate the struct
    state->powerState = static_cast<uint8_t>(this->state);
    state->gpioState = (ReadSensePin() ? 1 : 0);
    state->relayState = relayState;
    state->irCommandIndex = static_cast<uint8_t>(irIndex);
    state->irCommandCount = static_cast<uint8_t>(irList.size());
}

// console command handler
void TVON::Command_main(const ConsoleCommandContext *c)
{
    if (c->argc < 2)
        return c->Usage();

    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--status") == 0)
        {
            char sensePortName[32] = "None";
            bool sensePortState = false;
            if (tvOn.sensePort != nullptr)
            {
                tvOn.sensePort->FullName(sensePortName, sizeof(sensePortName));
                sensePortState = tvOn.sensePort->Poll();
            }

            c->Printf(
                "State: %d\n"
                "Physical SENSE pin (%s): %s\n"
                "Logical SENSE pin: %s\n"
                "Simulation Mode: %s\n",
                tvOn.state,
                sensePortName, sensePortState ? "High" : "Low",
                tvOn.ReadSensePin() ? "High" : "Low",
                tvOn.sim.active ? "Yes" : "No");
        }
        else if (strcmp(a, "--pulse-relay") == 0)
        {
            c->Printf("Pulsing relay\n");
            tvOn.ManualPulseRelay();
        }
        else if (strcmp(a, "--relay-on") == 0)
        {
            c->Printf("Relay -> Manual On\n");
            tvOn.ManualSetRelay(true);
        }
        else if (strcmp(a, "--relay-off") == 0)
        {
            c->Printf("Relay -> Manual Off\n");
            tvOn.ManualSetRelay(false);
        }
        else if (strcmp(a, "--simulate-on") == 0)
        {
            c->Printf("Activating simulation; simulated power state is now ON\n");
            tvOn.sim.active = true;
            tvOn.sim.powerOn = true;
        }
        else if (strcmp(a, "--simulate-off") == 0)
        {
            c->Printf("Activating simulation; simulated power state is now OFF\n");
            tvOn.sim.active = true;
            tvOn.sim.powerOn = false;
            tvOn.sim.latch = false;
        }
        else if (strcmp(a, "--simulate-end") == 0)
        {
            c->Printf("Deactivating simulation mode\n");
            tvOn.sim.active = false;
        }
        else
        {
            return c->Printf("tvOn: unknown option \"%s\"\n", a);
        }
    }
}

