// Pinscape Pico firmware - IR Remote Receiver
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
#include <stdlib.h>
#include <stdint.h>
#include <list>

#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>

#include "../Pinscape.h"
#include "../JSON.h"
#include "../Logger.h"
#include "../GPIOManager.h"
#include "IRReceiver.h"
#include "IRTransmitter.h"
#include "IRProtocols.h"

// global singleton
IRReceiver irReceiver;

// Constructor
IRReceiver::IRReceiver()
{
}

// Destructor
IRReceiver::~IRReceiver()
{
}

void IRRecvProIfc::WriteCommand(const IRCommandReceived &cmd)
{
    // calculate the time since the last command
    uint64_t now = time_us_64();
    uint64_t dt = now - tLastCommand;
    
    // save the new command for auto-repeat checks on the next command
    lastCommand = cmd;
    tLastCommand = now;

    // add it to the queue
    commands.Write({ lastCommand, tLastCommand, dt });
}

// Configure from JSON data
//
// irRx: {
//   gpio: <number>,         // GPIO port for TSOP384xx (or equivalent) IR receiver DATA/OUT pin
//   bufferSize: <number>,   // pulse buffer size, in entries (optional)
// }
//
void IRReceiver::Configure(JSONParser &json)
{
    // get the JSON key for the IR receiver
    bool ok = false;
    int rawBufSize = 128;
    int gpio = -1;
    if (auto *val = json.Get("irRx") ; !val->IsUndefined())
    {
        // get the parameters
        gpio = val->Get("gpio")->Int(-1);
        rawBufSize = val->Get("bufferSize")->Int(rawBufSize);

        // presume success
        ok = true;

        // validate the GPIO
        if (!IsValidGP(gpio))
        {
            Log(LOG_ERROR, "irRx: invalid or undefined gpio for IR receiver\n");
            ok = false;
        }

        // Claim it as an input.  The TSOP receivers have their own
        // internal pull-ups on the the DATA line, so we don't need
        // any extra pulls on the Pico.
        if (ok && !gpioManager.ClaimSharedInput("IR RX", gpio, false, false, true))
            ok = false;

        // make sure the raw buffer size is within reason
        if (rawBufSize < 16 || rawBufSize > 1024)
        {
            Log(LOG_ERROR, "irRx: invalid buffer size; must be 16..1024\n");
            ok = false;
        }
    }

    // if configuration succeeded, set up the receiver
    if (ok)
    {
        // remember the GPIO
        this->gpio = gpio;
        
        // allocate the raw pulse buffer
        rawbuf.Alloc(rawBufSize);
        
        // allocate the protocol singletons
        IRProtocol::AllocProtocols();

        // set our interrupt handler
        gpio_add_raw_irq_handler(gpio, [](){ irReceiver.IRQHandler(); });

        // done
        Log(LOG_CONFIG, "IR Receiver configured on GP%d\n", gpio);
    }
}

// Periodic tasks
void IRReceiver::Task()
{
    // check for a pulse timeout
    if (time_us_64() >= pulseTimeout)
        OnPulseTimeout();

    // process pulses from the raw buffer through the state machines
    uint16_t t;
    while (rawbuf.Read(t))
    {
        // Decode the pulse.  The low bit is the mark/space indicator
        // ('1' == mark), and the high 15 bits are the pulse time
        // divided by 2, in 2us units.
        int time_us = (t & ~0x0001) << 1;
        bool mark = (t & 0x0001) != 0;

        // if the time stored is the maximum value of 0xFFFE, pass
        // the special value -1 to the subscribers, to indicate that
        // it's something longer than 0xFFFE*2 us
        int subscriberTime_us = ((t & 0xFFFE) == 0xFFFE) ? -1 : time_us;
        
        // pass the raw pulse data to subscribers
        for (auto &sub : subscribers)
        {
            if (sub.rawPulses)
                sub.sub->OnIRPulseReceived(subscriberTime_us, mark);
        }

        // Process it through the protocol handlers
        ProcessProtocols(time_us, mark);
    }

    // if notifications are enabled, pass queued commands to event
    // subscribers
    if (notifyEnabled)
    {
        // process all queued commands
        IRCommandReceived cmd;
        uint64_t dt;
        while (ReadCommand(cmd, dt))
        {
            // notify all subscribers
            for (auto &sub : subscribers)
            {
                // If there's a filter, only notify this subscriber for commands
                // listed.  A zero-length filter matches all commands.
                bool matched = true;
                if (sub.filter.size() != 0)
                {
                    // there's a filter - look for a matching command, and only
                    // notify if there's a match
                    matched = false;
                    for (auto &f : sub.filter)
                    {
                        if (f == cmd)
                        {
                            matched = true;
                            break;
                        }
                    }
                }

                // Call the callback if we matched the filter
                if (matched)
                    sub.sub->OnIRCommandReceived(cmd, dt);
            }
        }
    }
}

// read a command from the queue
bool IRReceiver::ReadCommand(IRCommandReceived &cmd, uint64_t &dt)
{
    // read the queue
    IRRecvProIfc::CmdInfo cmdInfo;
    if (commands.Read(cmdInfo))
    {
        // retrieve the command
        cmd = cmdInfo.cmd;
        dt = cmdInfo.dt;

        // log it 
        char buf[128];
        uint64_t dt = cmdInfo.dt;
        Log(LOG_INFO, "IR command received: %s, dt=%llu ms\n", cmd.Format(buf, sizeof(buf)), dt / 1000);

        // success
        return true;
    }

    // no command available
    return false;
}

// add a subscriber, with no filter
void IRReceiver::Subscribe(Subscriber *sub, bool rawPulses)
{
    subscribers.emplace_back(sub, rawPulses);
}

// add a subscriber, with a filter for specific command codes
void IRReceiver::Subscribe(Subscriber *sub, std::initializer_list<IRCommandDesc> filters, bool rawPulses)
{
    subscribers.emplace_back(sub, filters, rawPulses);
}

// Enable reception
void IRReceiver::Enable()
{
    // only proceed if we have a valid GPIO pin
    if (gpio != -1)
    {
        // start the pulse timers
        StartPulse(gpio_get(gpio) ? 0 : 1);
        
        // enable rising-edge and falling-edge interrupts on the input pin
        gpio_set_irq_enabled(gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
        irq_set_enabled(IO_IRQ_BANK0, true);
    }
}

// Disable reception
void IRReceiver::Disable()
{
    if (gpio != -1)
    {
        // Shut down all of our asynchronous handlers: disable the pin edge
        // interrupts, stop the pulse timer, and cancel the maximum pulse 
        // length timeout.
        gpio_set_irq_enabled(gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
        ClearPulseTimeout();
    }
}

// Start a new pulse of the given type.
void IRReceiver::StartPulse(bool newPulseState)
{
    // set the new state
    pulseState = newPulseState;
    
    // set the pulse timer
    SetPulseStartTime();
    pulseAtMax = false;

    // set the new pulse timeout time
    pulseTimeout = time_us_64() + MAX_PULSE;
}

// End the current pulse
void IRReceiver::EndPulse(bool lastPulseState)
{
    // Add the pulse to the buffer.  If the pulse already timed out,
    // we already wrote it, so there's no need to write it again.
    if (!pulseAtMax)
    {
        // get the time of the ending space
        uint32_t t = GetPulseLength();
        
        // Scale by 2X to give us more range in a 16-bit int.  Since we're
        // also discarding the low bit (for the mark/space indicator below),
        // round to the nearest 4us by adding 2us before dividing.
        t += 2;
        t >>= 1;
        
        // limit the stored value to the uint16 maximum value
        if (t > 65535)
            t = 65535;
            
        // set the low bit if it's a mark, clear it if it's a space
        t &= ~0x0001;
        t |= lastPulseState;

        // add it to the buffer
        rawbuf.Write(uint16_t(t));
    }

    // no pulse is active, so clear the pulse timeout
    ClearPulseTimeout();
}

// GPIO IRQ handler
void IRReceiver::IRQHandler()
{
    // get the event mask
    uint32_t mask = gpio_get_irq_event_mask(gpio);

    // Call the edge handler(s) for the bits in the mask
    if ((mask & (GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE)) == (GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE))
    {
        // Both bits are set, so a rise and fall occurred within the
        // time it took to service the first of the pair.  Infer the
        // order from the last state: if we were in a space (pulseState
        // == false), a falling edge should be next, otherwise a rising
        // edge should be next.
        if (!pulseState)
        {
            // curently in a space - expect fall -> rise
            OnFallingEdge();
            OnRisingEdge();
        }
        else
        {
            // currently in a mark - expect rise -> fall
            OnRisingEdge();
            OnFallingEdge();
        }
    }
    else if ((mask & GPIO_IRQ_EDGE_FALL) != 0)
    {
        // falling edge only
        OnFallingEdge();
    }
    else if ((mask & GPIO_IRQ_EDGE_RISE) != 0)
    {
        // rising edge only
        OnRisingEdge();
    }

    // acknowledge the IRQ to clear the mask flags
    gpio_acknowledge_irq(gpio, mask);
}

// Falling-edge interrupt.  The sensors we work with use active-low 
// outputs, so a high->low edge means that we're switching from a "space"
// (IR off) to a "mark" (IR on).
void IRReceiver::OnFallingEdge() 
{
    // If the transmitter is sending, ignore new ON pulses, so that we
    // don't try to read our own transmissions.
    if (transmitter != nullptr && transmitter->IsSending())
        return;

    // if we were in a space, end the space and start a mark
    if (!pulseState)
    {
        EndPulse(false);
        StartPulse(true);
    }
}

// Rising-edge interrupt.  A low->high edge means we're switching from
// a "mark" (IR on) to a "space" (IR off).
void IRReceiver::OnRisingEdge() 
{
    // if we were in a mark, end the mark and start a space
    if (pulseState)
    {
        EndPulse(true);
        StartPulse(false);
    }
}

// Pulse timeout
void IRReceiver::OnPulseTimeout()
{
    // End the current pulse, even though it hasn't physically ended,
    // so that the protocol processor can read it.  Pulses longer than
    // the maximum are all the same to the protocols, so we can process
    // these as soon as we reach the timeout.  However, don't start a
    // new pulse yet; we'll wait to do that until we get an actual
    // physical pulse.
    EndPulse(pulseState);

    // note that we've reached the pulse timeout
    pulseAtMax = true;
}

// Process one buffer pulse
bool IRReceiver::ProcessOne(uint16_t &sample)
{
    // try reading a sample
    if (rawbuf.Read(sample))
    {
        // Process it through the protocols - convert to microseconds
        // by masking out the low bit and mulitplying by the 2us units
        // we use in the sample buffer, and pull out the low bit as
        // the mark/space type.
        ProcessProtocols((sample & ~0x0001) << 1, sample & 0x0001);
        
        // got a sample
        return true;
    }
    
    // no sample
    return false;
}

// Process one buffer pulse
bool IRReceiver::ProcessOne(uint32_t &t, bool &mark)
{
    // try reading a sample
    uint16_t sample;
    if (rawbuf.Read(sample))
    {
        // it's a mark if the low bit is set
        mark = sample & 0x0001;
        
        // remove the low bit, as it's not actually part of the time value,
        // and multiply by 2 to get from the 2us units in the buffer to
        // microseconds
        t = (sample & ~0x0001) << 1;
        
        // process it through the protocol handlers
        ProcessProtocols(t, mark);
        
        // got a sample
        return true;
    }
    
    // no sample
    return false;
}

// Process a pulse through the protocol handlers
void IRReceiver::ProcessProtocols(uint32_t t, bool mark)
{
    // generate a call to each sender in the RX list
    #define IR_PROTOCOL_RX(cls) IRProtocol::protocols->s_##cls.RxPulse(this, t, mark);
    #include "IRProtocolList.h"
}
