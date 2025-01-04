// Pinscape Pico firmware - IR Remote Receiver
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a multi-protocol receiver for IR remote control signals.  The IR
// signals are physically received through an external sensor.  Our
// reference device is the TSOP384xx, but most other IR remote sensors are
// similar in design and will proably work.  We have two main requirements
// for the sensor: first, it has to demodulate the IR carrier wave; second,
// it has to present a single-wire digital signal representing the
// demodulated IR status.  We assume active low signaling, where 0V on the
// signal line represents "IR ON" and Vcc represents "IR OFF".  It would be
// fairly easy to adapt the code to the opposite signaling sense, but I
// haven't bothered to parameterize this because I haven't seen any
// active-high sensors.  The sensor also obviously has to be electrically
// compatible with the Pico which mostly means that it runs on a 3.3V
// supply.  If your sensor uses a different supply voltage, it might still
// be workable, but you might need to put a voltage level converter between
// the sensor and the Pico GPIO pin, to adjust the sensor's logical voltage
// to the Pico's 3.3V.
//
//
// How to wire the sensor
//
// To physically wire the sensor, you just need to connect the sensor's Vs
// and GND pins to the Pico's 3.3V and GND pins, respectively, and connect
// the sensor's "OUT" or "data" pin (pin 1 on a TSOP384xx) to a free GPIO on
// the Pico.  You should check your sensor's data sheet to see if any other
// external components are required; e.g., the TSOP384xx data sheet
// recommends a capacitor and resistor for ESD protection and power supply
// conditioning.  The data sheet will include a diagram showing the
// suggested application wiring if there are any special considerations like
// that.  Note that the TSOP384xx data sheet doesn't specify exact values
// for the resistor and capacitor, so I'll mention what I'm using in my test
// setup: 220 ohms for the resistor, 150nF for the capacitor.
//
//
// How to use it in your application
//
// To use the receiver in an application, configure the receiver from a
// JSON object, per the Pinscape Pico conventions:
//
//    irReceiver.Configure(json);
//
// If you're using the companion transmitter class in the same application
// to create a device that's both an IR transmitter and receiver, the
// transmitter class will automatically notify us that it's operating, so
// that we can ignore our own transmissions.  You can do this manually, if
// you don't use IRTransmitter::Configure() to set up the transmitter:
//
//    irReceiver.SetTransmitter(&irTransmitter);
//
// If you're using some unrelated software to send IR transmissions, you
// might instead need to manually disable the receiver while your
// transmitter is sending codes, and then re-enable it when done, using the
// receiver class's Disable() and Enable() method.
//
// After other initialization is complete, enable the receiver:
//
//    irReceiver.Enable();
//
// Note that Enable() will only take effect if the receiver is properly
// configured, so you can safely call Enable() from your main function
// without checking first.
//
// Once you have a receiver set up and enabled, you need to call its Task()
// method on each iteration of your main loop.  This method takes all of the
// signals that have been received since the last call and runs them through
// the protocol decoders.  To minimize time spent in interrupt handlers, the
// interrupt handlers merely queue the messages internally; this makes them
// return extremely quickly, so that they don't add any significant latency 
// for other hardware or timer interrupts your application might use.
//
//    irReceiver.Task();
//
// To handle codes upon receipt, you can set up subscribers that listen for
// all codes or for specific codes.  The subscriber object must implement
// the abstract interface IRReceiver::Subscriber; then it can add itself
// for event notifications via the Subscribe() methods:
//
//    irReceiver.Subscribe(this);   // listen for all codes
//    irReceiver.Subscribe(this, code1, code2, ...);   // listen for specific codes
//
// The event callback is made from the Task() routine in ordinary thread
// context, NOT from IRQ context, so no IRQ restrictions apply within your
// handler.
//
// Alternatively, rather than using subscribers, call EnableNotify(false),
// and then your main loop can read incoming IR remote codes explicitly by
// calling ReadCommand().  If a command is available, this will read it into
// an IRCommand object, which tells you the protocol the sender used (see
// IRProtocolID.h), and provides a "universal" representation of the
// command.  The universal representation is protocol-specific mapping of
// the raw data bits to an integer value.  We try to do this in a way that's
// most useful per protocol, with two main goals in mind.  First, if there
// are any internal bits that are more structural than meaningful, such as
// checksums or other integrity checks, we generally remove them.  Second,
// if there are published tables of codes from a manufacturer, we try to
// match the format used there, to make it easier to verify that codes are
// as expected and to make it easier to construct apps around specific types
// of remotes.
//
//    IRCommandReceived cmd;
//    while (irReceiver.ReadCommand(cmd))
//       { process the command; }
//
// You can also optionally read the raw incoming data, by calling
// ProcessOne() before calling Task().  ProcessOne() runs a reading through
// the protocol decoders but also hands it back to you.  Raw samples are
// simply "IR ON" and "IR OFF" signals with the time the IR was continuously
// on or off.  The raw samples are useful if you want to build something
// like a repeater that only has to replicate the timing of the IR pulses,
// without regard to the underlying data bits.  Raw signals are obviously
// also very useful if you want to analyze an unknown protocol and figure
// out how to write a new encoder/decoder for it.  One thing that raw
// signals aren't great for, somewhat counterintuitively, is for building a
// learning remote.  Many of the protocols have special ways of handling
// repeated codes (e.g., when holding down a key) that make verbatim
// repetition of a signal problematic for learning remote use.  If you just
// repeat a raw code, the receiver might be confused into thinking that one
// key press looks like several key presses, or vice versa.  It's better
// when possible to decode a signal into a recognized protocol, store the
// decoded bit data rather than the raw signals as the "learned" code, and
// then reconstruct the appropriate signal for transmission by re-encoding
// the learned bit code using the protocol's known structure.  Of course,
// that only works if the remote uses one of the protocols we already
// recognize, so the raw pulses could still be useful as a last resort to
// learn codes that don't fit any of the known protocols.
//
//
// Internal architecture
//
// The IRReceiver object is simply a coordinator that manages the sensor
// hardware interface, reads the raw signals from the sensor, and passes the
// raw signals to the protocol objects for decoding.  For each protocol we
// recognize, we define a specialized handler class for the protocol.  Each
// protocol handler implements a state machine for decoding signals using
// its protocol.  When IRReceiver gets a raw signal, it simply passes it to
// each of the protocol handlers in turn.  They all operate as independent
// state machines, so in effect, the receivers for all of the supported
// protocols listen to the signal at the same time, in parallel, trying to
// decode it according to their rules.  When one of the protocol handlers
// successfully decodes a complete "command" (a key press on a remote, in
// most cases), it adds the command to our queue, using a universal command
// format, which is essentially an integer value that's big enough to hold
// any command number from any of the supported protocols, plus a type code
// that indicates which protocol the code came from.  Clients can then read
// the incoming commands from the queue without worrying about the raw
// signal details; they just see the fully decoded commands.
//
// It might sound chaotic to have all of these different protocol decoders
// working on the same data at the same time, but in practice the various
// protocols have enough internal structure that only the "right" handler
// will be able to do anything with a given signal.  All of the "wrong"
// handlers will quickly reject an incoming signal that doesn't match their
// protocol rules, landing in a terminal state that means "no code matched",
// where they'll stay as long as the pulses fail to match the handler's
// protocol rules.
//
// This parallel state machine processing scheme might also sound like a lot
// of overhead, but in practice it's very lightweight: it only takes a few
// percent of CPU time to service the decoding process while IR pulses are
// actively arriving, and essentially 0% in the absence of IR pulses.  The
// CPU time consumed is all in application context, not in interrupt
// context, so it doesn't contribute any latency to any other hardware
// interrupts.  The individual protocol state machines are all very simple,
// typically doing just a few integer compares on the incoming timing data
// per signal.  They also require very little state, usually on the order of
// a few 'int's per decoder, which translates to a small RAM footprint.  The
// decoders operate incrementally and decode in near real time, so decoded
// commands appear essentially at the same time that their signals finish.
// This code was originally written for the KL25Z, which has only 16K of
// RAM, so it was very much designed with a small memory footprint in mind.
//
// If you've looked at any other MCU IR libraries, you've probably seen that
// they have to use special cases and twitchy heuristics to guess where the
// boundaries are between codes at a "global" level (i.e., before they've
// detected which protocol is in use).  That's completely unnecessary with
// the parallel state machine design, because we simply don't have to make
// any guesses at a global level about code boundaries.  All of the boundary
// detection and protocol state is in the individual protocol decoders.
// That neatly eliminates all of the special csses and heuristics that other
// libraries tend to need; since each protocol handler runs independently,
// each can use its own protocol's precise rules about code boundaries.
//
// We receive signals from the sensor via interrupts on the input GPIO pin.
// This allows for the most accurate timing possible, which is important
// because IR coding is entirely in the signal timing.  Interrupts gives us
// much more accurate timing than polling would for obvious reasons.  As
// mentioned above, though, we try to minimize the time we spend in IRQ
// context, since time spent in one interrupt handler translates to added
// latency for any other interrupts that occur at the same time.  To 
// accomplish this, the interrupt handlers don't do any decoding at all.
// They simply add the incoming signal data to an internal queue and then
// return.  We do the decoding work back in application context, by having
// the main loop call our Task() routine periodically.  This takes signal
// readings off of the queue and runs them through the decoders.  This
// introduces a small amount of lag time between physically receiving a
// signal and decoding it, but the lag time is only on the order of the
// main loop run time.  In most MCU applications this is a very short
// time, perhaps only microseconds or perhaps as long as a few millseconds.
// But in any case it's almost always so short that a user can't perceive
// the delay, so for all practical purposes decoding is done in real time.
//
//
// How IR remotes work in general
//
// IR remote controls work by transmitting timed pulses of infrared light.
// These pulses are modulated in two ways: first, with a "carrier", which
// is a PWM signal operating at a fixed, relatively high frequency; and
// second, with a lower frequency data signal superimposed on the PWM
// signal.  (And I suppose you could say there's a third layer of 
// modulation in the IR light itself, since that's an electromagnetic 
// wave operating at an even higher frequency of around 300 THz.)
//
// Carrier: The PWM carrier uses a fixed frequency, usually around 40kHz.  
// The carrier doesn't encode any data, since it's just constant fixed-length
// pulses.  Its function, rather, is to provide a regular oscillating signal
// that receivers can use to distinguish data signals from ambient light.
// This is necessary because the IR light wavelengths are also contained
// in sunlight and ordinary household lighting.  (Fluourescent lights even
// have their own characteristic oscillating frequencies in the IR band, so
// the receiver not only has to distinguish the signal from constant
// amgient light levels but also from other types of oscillating light
// levels.  The PWM carrier frequencies used in remotes are chosen based
// on the practical need to distinguish remote control signals from the
// common household interference sources.)  Receivers can separate the 
// an oscillating PWM signal at a particular frequency from other signals
// through a process known as demodulation, which is the same mechanism
// that radio receivers use to pluck AM or FM signals from the jumble of 
// background noise in the radio spectrum.
//
// For our purposes, we don't worry about demodulation in the software,
// since the sensor hardware does that part of the job.  Each type of sensor
// is designed to demodulate a particular carrier frequency, so you should
// choose a sensor based on the types of remotes you plan to use it with.
// Most CE manufacturers have more or less standardized on 38kHz, which is
// why we recommend the TSOP384xx series.  Not everyone is at exactly 38kHz,
// but most are within 2kHz plus or minus, and the TSOP seems to demodulate
// signals within a few kHz of its nominal frequency very well.  38kHz seems
// to be a good centerpoint for home electronics devices, which is why we
// recommend the 38kHz part as a "universal" receiver.  If your application
// only needs to receive from one specific remote (rather than act as a
// universal receiver), you might be better served with a different TSOP
// part that's tuned to your transmitter's carrier frequency, if that's
// something other than 38kHz.
//
// Data signal: The data signal is superimposed on the PWM carrier by
// turning the PWM'ed IR source on and off at a lower, variable frequency.
// These longer on/off pulses are of different lengths.  The data bits are
// encoded in the varying lengths, although there's no one true way of doing
// this.  Each protocol has its own way of representing bits as combinations
// of on times and off times, which we'll come to shortly.
//
// "On" pulses are called "marks", and "off" pulses are called "spaces".
// The terms come from wired asynchronous protocols, which share many
// properties with IR signals at this level.
//
// Note that each pulse has to be long enough to contain some minimum number
// (maybe 5-10) of PWM pulses, because otherwise the demodulator wouldn't be
// able to detect the presence or absence of the underlying PWM pulses.
// This makes IR remote codes fairly slow in terms of data rate, since the
// absolute minimum time per bit is the length of the shortest data pulse.
// Most codings actually use at least two pulses per bit for the sake of
// signal integrity, so the effective data rate is slower still, only about
// half of the pulse rate.  Fortunately, this is all rather unimportant,
// since IR remotes don't to transmit very much data.  They're mostly used
// to transmit button presses made by hand by a human user, which makes the
// source of the data quite slow to start with; what's more, each button
// press only repersents a few bytes of data, typically 8 to 32 bits.  So
// the inherent data rate, by the nature of the application, is only a few
// bytes per second at most.  This would be a terrible way to transmit video
// streams, but it's quite adequate for manual button presses on small
// keypads.
//
// Bit encodings
//
// The timing of the marks and spaces carries the information, but exactly
// how it does this is a whole separate matter, known as an encoding.  An
// encoding is a mapping from '0' and '1' bits to a pattern of marks and
// spaces, and vice versa.  At first glance, it might seem that you could
// just use a mark as a '1' and a space as a '0', and in fact some protocols
// do something like this.  But that simple approach has some big drawbacks
// arising from the lack of a shared clock between sender and receiver.
// Most encodings therefore do something to embed a timing signal of some
// sort within the data signal, by using the lengths of the pulses to encode 
// bits rather than just the presence of the pulses.
//
// There are probably an infinite number of possible ways to do this in
// principle.  Fortunately, the CE companies have only put a finite number
// of them into practice.  In fact, it seems that we can cover practically
// all of the remotes out there by considering a small handful of encoding 
// schemes.  Here are the main ones, and the ones we can use in this
// receiver library:
//
//  - Async bit stream.  This is basically the IR equivalent of a wired
//    UART.  Each code word consists of a fixed number of bits.  Each bit
//    is represented by IR ON for '1' and IR OFF for '0', transmitted for
//    a fixed time length.  To transmit, simply turn the IR on and off in
//    sequence for the fixed bit time per bit.  To receive and decode,
//    observe whether the IR signal is on or off in each time window. 
//    This type of protocol looks simple, but it presents some difficulties
//    in implementation, because it doesn't provide any cues embedded in
//    the IR signal to help the receiver synchronize with the sender or
//    recognize the boundaries of code words, as all of the other common
//    protocols do.  That might be why this class seems to be rarely used 
//    in real applications.  Protocols based on simple async bits usually
//    add something at the protocol level that helps the reciever detect
//    word boundaries and check signal integrity.
//
//  - Pulse distance coding, also known as space length coding.  In this 
//    scheme, marks are all of equal length, but spaces come in two lengths, 
//    A and B, where B is much longer than A (usually twice as long, but it
//    could be even longer).  A encodes 0 and B encodes 1.  The marks serve
//    as regular clock signals, allowing the receiver to keep in sync with
//    the sender, and the long and short space times (A and B) are different
//    enough that the receiver can easily distinguish them reliably, even
//    with a low-precision clock.  This scheme is probably the most widely
//    used in CE products, because it's the encoding used by the NEC 
//    protocol, which most Japanese CE companies use.
//
//  - Pulse length coding, also known as mark length coding.  This is simply 
//    the inverse of pulse distance coding: spaces are all of equal length, 
//    and marks come in two lengths, with the short mark encoding 0 and the 
//    long mark encoding 1.  This is practically the same in all meaningful
//    ways as the space length coding; the only reason both kinds exist is
//    probably that either someone had a bad case of NIH or they wanted to
//    avoid paying a patent royalty.  Mark length coding is the scheme Sony
//    uses (in their SIRCS protocol).
//
//  - Manchester coding.  The message is divided into time slices of 
//    equal size, one bit per slice.  Within each slice, the IR is on for 
//    half the window and off for half the window.  The 0 and 1 bits are
//    encoded by the direction of the transition: if a bit window starts
//    with a mark (IR ON) and ends with a space (IR OFF), it's a '1'; if it
//    starts with a space and ends with a mark, it's a '0'.  Or vice versa.
//    Each mark or space therefore lasts for either 1/2 or 1 bit time 
//    length, never longer.  This makes it fairly easy for the receiver to 
//    distinguish the two time lengths, even with a fairly low-precision 
//    clock, since they're so different.  It's also easy for the receiver
//    to distinguish each bit, since there's always at least one transition
//    (mark to space or space to mark) per bit.  What's more, '0' and '1' 
//    bits take the same time to transmit (unlike the mark-length and 
//    space-length protocols), so every code word (assuming a fixed bit 
//    count) takes the same time regardless of the bit values within. 
//    Manchester modulation is used in the Philips RC5 and RC6 protocols, 
//    which are widely used among European CE companies.
//
// Protocols
//
// On top of the encoding scheme, there's another level of structure called
// a protocol.  A given protocol uses a given encoding for the data bits,
// but then also adds some extra structure.  
//
// For starters, the IR protocols all work in terms of "code words".  In 
// computer terms, a code word amounts to a datatype with a fixed number 
// of bits.  For example, the NEC protocol uses a 32-bit code word: each 
// button press is represented by a 32-bit transmission.  A single key 
// press usually maps to a single code word, although not always; the 
// Pioneer protocol, for example, transmits two words for certain buttons, 
// using a special "shift" code for the first word to give a second meaning 
// to the second word, to extend the possible number of commands that would
// be otherwise limited by the number of bits in a single code word.
//
// Second, most of the IR protocols add special non-data signals that
// mark the beginning and/or end of a code word.  These are usually just
// extra-long marks or spaces, which are distinguishable from the marks
// and spaces within a code word because they're too long to be valid in
// the data encoding scheme.  These are important to reliable communication
// because the sender and receiver don't have any other way to share state 
// with each other.  Consider what would happen if someone walked in the
// way while you were transmitting a remote code: the receiver would miss
// at least a few data bits, so it would be out of sync with the sender.
// If there weren't some way to distinguish the start of a code word from
// the IR pulses themselves, the receiver would now be permanently out
// of sync from the sender by however many bits it missed.  But with the
// special "header" code, the receiver can sync up again as soon as the
// next code word starts, since it can tell from the timing that the
// header can't possibly be a bit in the middle of a code word.


#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <list>
#include <initializer_list>

#include <pico/stdlib.h>
#include <pico/time.h>

#include "../Pinscape.h"
#include "IRRemote.h"
#include "IRCommand.h"
#include "CircBuf.h"

// external/forward declarations
class JSONParser;

// IR receiver protocol interface.  This contains functions that we only
// want to make accessible to the protocol decoders.
class IRRecvProIfc
{
public:
    // write a command to the command queue
    void WriteCommand(const IRCommandReceived &cmd);

    // Last command queued, and its timestamp.  We keep a separate copy of
    // the last command received for reference by the protocol decoders to
    // detect auto-repeats.
    // 
    // The timestamp lets the decoders set an upper bound on the time
    // between auto-repeats.  Even if a command looks like an auto-repeat
    // based on embedded flags or matching codes, it probably isn't really a
    // repeat if it's been more than a few hundred milliseconds since the
    // previous command - that probably just means that we missed one or
    // more transmissions between the two, so the new command is probably an
    // auto-repeat of a command we *didn't* receive, making it effectively a
    // new key press.
    IRCommandReceived lastCommand;
    uint64_t tLastCommand = 0;

protected:
    // Decoded command queue.  The protocol handlers add commands here
    // as soon as they decode them.
    struct CmdInfo
    {
        IRCommandReceived cmd;    // command received
        uint64_t timestamp;       // timestamp
        uint64_t dt;              // elapsed time since previous command
    };
    CircBuf<CmdInfo, 16> commands;
};

// IR Remote Receiver
class IRReceiver : protected IRRecvProIfc
{
public:
    // construction
    IRReceiver();

    // destruction
    ~IRReceiver();

    // Configure
    void Configure(JSONParser &json);

    // Is the receiver configured?  Returns true if the receiver has
    // been configured with a GPIO input.
    bool IsConfigured() const { return gpio >= 0; }

    // IR event subscriber.  Implement this abstract interface to define
    // an object that can subscribe for notifications when
    class Subscriber
    {
    public:
        // Receive notification of an IR command received.  This is called
        // on each object that subscribed for the specific command received,
        // or that subscribed for any command.  'dt' represents the elapsed
        // time since the preceding IR command was received.
        //
        // This is called in ordinary thread context (from the receiver's
        // Task() routine), NOT IRQ context, so no special IRQ restrictions
        // apply.
        virtual void OnIRCommandReceived(const IRCommandReceived &command, uint64_t dt) = 0;

        // Receive notification of raw IR pulse timing.  This is called from
        // ordinary thread context (not IRQ context) as the main loop
        // processes queued pulses, so this is called some time after pulses
        // arrive, not in real time.  If 'mark' is true, the pulse is a
        // "mark" (the IR signal is ON), otherwise it's a "space" (the IR
        // signal is OFF).  'time_us' is the duration of the pulse in
        // microseconds.  The special value -1 means that the pulse was
        // longer than the longest meaningful pulse time, so it really
        // represents a dead interval between transmissions rather than part
        // of a transmission.
        //
        // We provide a no-op default so that subscriber implementations
        // don't have to provide an override if this won't be used.
        virtual void OnIRPulseReceived(int time_us, bool mark) { }
    };

    // subscribe for notifications for all command codes, and optionally for
    // raw pulse events
    void Subscribe(Subscriber *sub, bool rawPulses = false);

    // subscribe for notifications for specific codes only
    void Subscribe(Subscriber *sub, std::initializer_list<IRCommandDesc> commands, bool rawPulses = false);

    // Enable/disable subscriber notifications.  Notifications are enabled
    // by default.  When notifications are enabled, Task() reads and
    // consumes incoming events, passing them to enrolled subscribers.
    // Disable notifications if you wish to process commands manually via
    // ReadCommand() instead.  (It's not sufficient to empty the command
    // queue via ReadCommand() before calling Task(), because commands can
    // be added to the queue asynchronously from interrupt context, so a new
    // command could arrive between the last ReadCommand() call and the call
    // to Task().  Disabling notifications ensures that Task() won't attempt
    // to read the queue itself, leaving all commands available for manual
    // consumption via ReadCommand().)
    void EnableNotify(bool enable) { notifyEnabled = enable; }
    
    // Periodic task handler
    void Task();
    
    // Optionally connect to a transmitter, to suppress reception while
    // we're transmitting.  This prevents spuriously receiving our own
    // transmissions, if our IR LED and sensor are physically close enough
    // to one another that our sensor would pick up light from our LED.
    // If the two are physically isolated so that we can't receive our
    // own transmissions, it's not necessary to connect the transmitter
    // here, as there's no restriction on the software side on sending
    // and receiving simultaneously - the suppression is only needed to
    // avoid self-interference with the physical IR signals.
    void SetTransmitter(class IRTransmitter *transmitter) { this->transmitter = transmitter; }

    // Enable/disable our interrupt handlers.  If the main program
    // doesn't need IR input, it can disable the receiver so that
    // it doesn't consume any CPU time handling interrupts.
    void Enable();
    void Disable();

    // Is a code being received?  If we're currently receiving a "mark"
    // (meaning that a modulated IR pulse is being received) or we have any
    // pulses queued in the raw buffer, reception is in progress.
    bool IsReceiving() const { return pulseState || rawbuf.IsReadReady() ; }
    
    // Read a command.  Returns true if a command is available, filling in
    // 'cmd'.  Returns false (without blocking) if no commands are
    // available.
    bool ReadCommand(IRCommandReceived &cmd, uint64_t &dt);
    
    // Is a command ready to read?
    bool IsCommandReady() { return commands.IsReadReady(); }
    
    // Process and retrieve one raw pulse.  The application main loop can
    // optionally call this, instead of Task(), if it wants to retrieve
    // each raw sample for its own purposes in addition to running them 
    // through the protocol state machines.  If no sample is available, we
    // immediately return false - the routine doesn't block waiting for a
    // sample.  If a sample is available, we fill in 'sample' with the pulse 
    // time in microseconds, and set 'mark' to true if the sample was a mark, 
    // false if it's a space.  
    //
    // To use this instead of process(), on each main loop iteration, call 
    // this function in an inner loop until it returns false.  That'll ensure 
    // that all pending samples have been processed through the protocol 
    // state machines and that maximum buffer space is available for the next 
    // main loop iteration.
    bool ProcessOne(uint32_t &sample, bool &mark);
    
    // Process and retrieve one raw pulse.  This works the same as the
    // two-argument version above, but returns the sample in our internal
    // format: the sample value is a time reading in 2us units, and the low
    // bit is 1 for a mark, 0 for a space.  To convert to a time reading in
    // microseconds, mask out the low bit and multiply by 2.
    bool ProcessOne(uint16_t &sample);

    // Maximum pulse length in microseconds.  Anything longer will simply be
    // represented with this value.  This is long enough that anything
    // longer has equivalent meaning in any of our protocols.  Generally, a
    // space longer than this will only occur in a silent interval between
    // transmissions (that is, while no one is sending any codes), and a
    // mark longer than this could only be interference or noise.
    //
    // This value should be chosen so that it's high enough to be readily
    // distinguishable (in terms of our error tolerance) from the longest
    // meaningful space or pulse in any protocol we need to handle, but not
    // much higher than that.  It shouldn't be too long because it has a
    // role as an inactivity timeout on receive: we can't always know that a
    // signal has ended until there's inactivity for this amount of time.
    // If the timeout is too long, it can become noticable as lag time in
    // recognizing signals.  In practice, the longest gap time between
    // repeating signals in commonly used protocols is in the neighboorhood
    // of 100ms.
    //
    // This value is chosen to be the largest we can fit into a 16-bit int,
    // taking into account our 2X scaling and our use of the low bit for a
    // mark/space indicator.
    static const uint32_t MAX_PULSE = 131068;    

private:
    // Input pin.  Reads from a TSOP384xx or similar sensor.  Any
    // sensor should work that demodulates the carrier wave and 
    // gives us an active-low input on the pin.
    int gpio = -1;

    // IR raw data buffer.  The interrupt handlers store the pulse
    // timings here as they arrive, and the ProcessOne() routine reads
    // from the buffer.
    //
    // Samples here are limited to 16 bits, so the longest time that
    // can be represented is 65535us.  Anything longer is capped to this.
    //
    // To keep track of marks vs spaces, we set the low-order bit of
    // each sample time to 1 for a mark and 0 for a space.  That means
    // that the times are only good to 2us precision, but that's plenty
    // of precision for all of the IR protocols, since the shortest time 
    // bases are around 250us.
    CircBufV<uint16_t> rawbuf;
    
    // Start time of current pulse.  We reset the time at the start of each
    // pulse, so it tells us the duration thus far of the current pulse at
    // any given time.
    volatile uint64_t pulseStartTime = 0;

    // reset the pulse time to the current system time
    void SetPulseStartTime() { pulseStartTime = time_us_64(); }

    // get the time since the start of the pulse
    uint32_t GetPulseLength() { return static_cast<uint32_t>(time_us_64() - pulseStartTime); }
    
    // flag: the pulse timer has reached IR_MAX_PULSE
    bool pulseAtMax = false;
    
    // current pulse state: mark = 1, space = 0
    bool pulseState = false;
    
    // start the pulse timers with the new pulse state (1=mark, 0=space)
    void StartPulse(bool newPulseState);
    
    // end the current pulse, checking that the pulse state matches the
    // current state
    void EndPulse(bool lastPulseState);

    // process a pulse through our protocol handlers
    void ProcessProtocols(uint32_t t, bool mark);

    // Base interrupt handler.  This reads the interrupt mask and
    // calls the appropriate rising-edge or falling-edge handler(s).
    void IRQHandler();
    
    // rise and fall interrupt handlers for the input pin
    void OnFallingEdge();
    void OnRisingEdge();
    
    // pulse timeout - system clock time for maximum pulse duration
    uint64_t pulseTimeout = ~0ULL;

    // clear/reset the pulse timeout (set to a time impossibly far in the future)
    void ClearPulseTimeout() { pulseTimeout = ~0ULL; }

    // timeout handler for a pulse (mark or space)
    void OnPulseTimeout();
    
    // Connected transmitter.  If this is set, we'll suppress reception
    // while the transmitter is sending a signal, to avoid receiving our
    // own transmissions.
    class IRTransmitter *transmitter = nullptr;

    // Event subscribers
    struct EventSub
    {
        EventSub(Subscriber *sub, bool rawPulses) : sub(sub), rawPulses(rawPulses) { }
        EventSub(Subscriber *sub, std::initializer_list<IRCommandDesc> filters, bool rawPulses) :
            sub(sub), filter(filters), rawPulses(rawPulses) { }

        // subscriber callback interface
        Subscriber *sub;

        // command filter; if non-empty, the subscriber is only notified for
        // commands that match a command listed in the filter; if empty, the
        // subscriber is notified on all command codes
        std::list<IRCommandDesc> filter;

        // does the subscriber want raw pulse notifications?
        bool rawPulses;
    };
    std::list<EventSub> subscribers;

    // are subscriber notifications enabled?
    bool notifyEnabled = true;
};

// Global singleton
extern IRReceiver irReceiver;
