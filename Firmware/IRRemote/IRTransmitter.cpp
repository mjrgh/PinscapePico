// Pinscape Pico firmware - IR Transmitter
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard headers
#include <stdlib.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "../Pinscape.h"
#include "../GPIOManager.h"
#include "../PWMManager.h"
#include "../Logger.h"
#include "CircBuf.h"
#include "IRRemote.h"
#include "IRCommand.h"
#include "IRProtocols.h"
#include "IRTransmitter.h"
#include "IRReceiver.h"


// global singleton
IRTransmitter irTransmitter;

// construction
IRTransmitter::IRTransmitter()
{
}

// Configure from JSON data
//
// irTx: {
//    gpio: <number>,           // GPIO port number
//    commandSlots: <number>,   // maximum number of command slots (optional)
//    commands: [               // predefined commands
//      {
//        name: "string",       // name of the code (optional, used to refer to it elsewhere in the configuration)
//        code: "string",       // command descriptor string: "<protocol>.<flags>.<commandCode>", see IRCommand.h
//      },
//      { second code },
//      ...
//    ],
// }
//
// The command slots define virtual remote control buttons that can be
// activated from other subsystems, such as from a physical button
// input.  This is a holdover from the KL25Z Pinscape software, and it's
// probably no longer necessary for any of the existing use cases,
// because callers can use the more convenient QueueCommand() instead.
// That function lets a caller queue an ad hoc command, and the caller
// can supply its own data source for the repeat status.  This has the
// advantage that a stored command can be kept with the logical
// subsystem that sends the command, rather than stored in the central,
// global list here.  For example, if you want to configure a physical
// button to send an IR command when pressed, the IR command to send can
// be configured directly in the config section defining the button
// input; likewise for a feedback output port.
//
// The reason that KL25Z Pinscape needed the pre-defined command list
// is that its USB protocol had an extremely small payload per command
// (8 bytes), so it was impossible to pass one of our universal remote
// control codes across the wire in a single message.  Separately, we
// were extremely memory-constrained because of the 16K RAM complement
// on the KL25Z, so we couldn't spare the bytes to reserve space for a
// command code per button.  The solution to both problems was to use
// a single list of codes that both USB clients and buttons could
// select from by index, so only a one-byte index had to be stored or
// passed across the USB connection.  Pinscape Pico has much more
// ample RAM available, and its USB protocols can easily handle full
// IR codes as parameters, so there's no longer any need to keep a
// table.  It greatly simplifies things for the client as well as
// for the firmware itself to just embed IR codes where they're
// needed and dispense with the central table.  So even though the
// central table is still here, I'm not planning to document it; it's
// a deprecated feature that might be removed at some point.
void IRTransmitter::Configure(JSONParser &json)
{
    // check for the configuration key
    if (auto *val = json.Get("irTx") ; !val->IsUndefined())
    {
        // get the parameters
        int gpio = val->Get("gpio")->Int(-1);
        int nCmdSlots = val->Get("commandSlots")->Int(32);

        // presume success
        bool ok = true;

        // validate the GPIO
        if (!IsValidGP(gpio))
        {
            Log(LOG_ERROR, "irTx: invalid or undefined IR LED GPIO pin\n");
            ok = false;
        }
        if (ok && (!gpioManager.Claim("IR TX", gpio) || !pwmManager.InitGPIO("IR Transmitter", gpio)))
            ok = false;

        // proceed with initialization if configuration succeeded
        if (ok)
        {
            // remember the GPIO
            this->gpio = gpio;

            // Configure the GPIO for high drive strength and fast slew.
            // The GPIO is meant to be used as the input to a transistor
            // switch that drives a high-current IRED, typically aiming
            // for around 1000 mA IRED drive current.  If it's an BJT,
            // it will require a relatively high base current to drive
            // the collector current that high, so we want to give it as
            // much current as we can.  If it's a MOSFET, the relatively
            // high carrier signal rate (around 38 kHz) will require
            // high instantaneous current to charge and discharge the
            // MOSFET gate quickly enough to maintain a clean square
            // wave at the drain.  So in either case, the IRED driver
            // will benefit from a high-current drive.  This also means
            // that we expect a substantial load on the GPIO, so we
            // don't need the Pico's slew-rate limiter, which is meant
            // mainly to reduce EMI noise generation with light loads.
            // We *want* the output to maintain sharp square-wave edges
            // so that they pass through to the IR signal.
            gpio_set_drive_strength(gpio, GPIO_DRIVE_STRENGTH_12MA);
            gpio_set_slew_rate(gpio, GPIO_SLEW_RATE_FAST);
            
            // make sure the protocol singletons are allocated
            IRProtocol::AllocProtocols();

            // get the command list
            auto *commands = val->Get("commands");
            
            // Allocate the command slot array.  Make sure that there are
            // at least enough for the predefined commands.
            if (nCmdSlots < commands->Length())
                nCmdSlots = commands->Length();
            if (nCmdSlots != 0)
                buttons = new IRCommandDesc[nCmdSlots];

            // populate the command list, if defined
            commands->ForEach([](int index, const JSONParser::Value *command)
            {
                // get the basics
                IRCommandDesc &b = irTransmitter.buttons[index];
                std::string s = command->Get("code")->String();
                if (!b.Parse(s.c_str()))
                    Log(LOG_ERROR, "irTx.commands[%d]: code string \"%s\" isn't a valid IR code\n", index, s.c_str());
            });

            // let the IR receiver know that we're operating, so that it
            // can suppress reception of our own transmissions
            irReceiver.SetTransmitter(this);
            
            // done
            Log(LOG_CONFIG, "IR Transmitter configured on GP%d\n", gpio);
        }
    }
}

// program the command code for a virtual button slot
void IRTransmitter::ProgramButton(int buttonId, int protocolId, bool dittos, uint64_t cmdCode)
{
    IRCommandDesc &btn = buttons[buttonId];
    btn.proId = protocolId;
    btn.code = cmdCode;
    btn.useDittos = dittos;
}

// push a virtual button
void IRTransmitter::PushButton(int id, bool on)
{
    if (on)
    {
        // make this the current command
        curBtnId = id;

        // start the transmitter
        TXStart();
    }
    else
    {
        // if this is the current command, cancel it
        if (id == curBtnId)
            curBtnId = TXBTN_NONE;
    }
}

// queue a command
bool IRTransmitter::QueueCommand(IRCommandDesc cmd, int count, volatile bool *state)
{
    // ignore if not configured
    if (gpio < 0)
        return false;

    // add it to the pending command queue; return failure if the
    // queue is full
    if (!adHocCommands.Write({ cmd, count, state }))
        return false;

    // start a new transmission if necessary
    TXStart();

    // success
    return true;
}

// start a transmission
void IRTransmitter::TXStart()
{
    if (!txRunning)
    {
        // The thread isn't running.  Note that this means that there's
        // no possibility that txRunning will change out from under us
        // asynchronously, since there's no pending interrupt handler
        // to change it.  Mark the thread as running.
        txRunning = true;
        
        // Directly invoke the thread handler for the first call.  It
        // will normally run in interrupt context, but since there's
        // no pending interrupt yet that would re-enter it, we can
        // launch it first in application context.  If there's work
        // pending, it'll kick off the transmission and return the
        // time in microseconds until the next alarm interrupt.
        if (uint64_t us = TXThread() ; us != 0)
        {
            txAlarm = add_alarm_in_us(us, [](alarm_id_t, void *self){
                return reinterpret_cast<IRTransmitter*>(self)->TXThread();
            }, this, true);
        }
    }
}

// transmission thread handler
int64_t IRTransmitter::TXThread()
{
    // if we're working on a command, process the next step
    if (txProtocol != nullptr)
    {
        // Determine if the virtual button for the current transmission
        // is still pressed.  It's still pressed if we have a valid 
        // transmitting button ID, and the current pressed button is the 
        // same as the transmitting button.
        txState.pressed = false;
        if (txBtnId == TXBTN_AD_HOC)
        {
            // We're processing an ad hoc command.  If it has an
            // associated state variable, read the repeat state from the
            // state variable; otherwise it's a one-off with no repeat
            // state.  Otherwise, if it has a counter, decrement the
            // count, and repeat until the count reaches zero.
            if (txAdHocStateVar != nullptr)
            {
                // it has a state variable - continue while the state
                // variable is reading true
                txState.pressed = *txAdHocStateVar;
            }
            else if (txAdHocCount > 1)
            {
                // The ad hoc state has a repeat counter, so treat the
                // button as pressed until the state's repeat counter
                // reaches the ad hoc repeat count.
                txState.pressed = (txState.repeatNumber + 1 < txAdHocCount);
            }
        }
        else if (txBtnId != TXBTN_NONE)
        {
            // we're processing a virtual button press - keep repeating
            // as long as same button ID is still selected
            txState.pressed = (txBtnId == curBtnId);
        }
        
        // Perform the next step via the protocol handler.  The handler
        // returns a positive time value for the next timeout if it still
        // has more work to do.
        for (;;)
        {
            // perform the next step
            int t = txProtocol->TXStep(&txState);

            // A positive value means that the protocol handler has
            // more work to do after 't' microseconds.  Simply return
            // the same value from the thread handler to reschedule
            // the alarm.
            if (t > 0)
                return t;

            // A negative value means that the protocol handler is
            // finished.
            if (t < 0)
                break;

            // A zero return from TXStep() means that it has more work
            // to do immediately, with no delay.  Just keep iterating.
        }
    }
    
    // If we made it here, the transmitter is now idle
    txBtnId = TXBTN_NONE;
    txProtocol = nullptr;
    txAdHocCount = 0;
    txAdHocStateVar = nullptr;

    // Check to see if we have a new virtual button press, or a queued
    // ad hoc command.
    txProtocol = nullptr;
    AdHocCommand adHoc;
    if (curBtnId != TXBTN_NONE)
    {
        // load the command
        txBtnId = curBtnId;
        txCmd = buttons[curBtnId];
        txProtocol = IRProtocol::SenderForId(txCmd.proId);
    }
    else if (adHocCommands.Read(adHoc))
    {
        // load the command
        txCmd = adHoc.cmd;
        txBtnId = TXBTN_AD_HOC;
        txAdHocCount = adHoc.count;
        txAdHocStateVar = adHoc.state;
        txProtocol = IRProtocol::SenderForId(txCmd.proId);
    }
        
    // If we found a protocol handler, start the transmission
    if (txProtocol != nullptr)
    {
        // fill in the transmission state object with the new command
        // details
        txState.cmdCode = txCmd.code;
        txState.protocolId = txCmd.proId;
        txState.dittos = txCmd.useDittos;
        txState.gpio = gpio;
        txState.pressed = true;

        // reset the transmission step counters
        txState.step = 0;
        txState.bit = 0;
        txState.bitstep = 0;
        txState.repeatPhase = 0;
        txState.repeatNumber = 0;

        // this is a new transmission, so toggle the toggle bit
        txState.toggle ^= 1;

        // Turn off the IR and set the PWM frequency of the IR LED to
        // the carrier frequency for the chosen protocol
        pwmManager.SetLevel(gpio, 0);
        pwmManager.SetFreq(gpio, txProtocol->PWMFreq(&txState));

        // start the transmission timer
        txState.ResetTXTime();

        // initiate the transmission; returns the time to the next step
        int t = txProtocol->TXStart(&txState);

        // tell the caller to reschedule the alarm for 't' microseconds
        return t;
    }

    // If we made it here, there's no transmission in progress,
    // so the thread is no longer running.
    txRunning = false;
    txAlarm = -1;
    
    // return 0 to indicate that the alarm is not to be rescheduled
    return 0;
}
