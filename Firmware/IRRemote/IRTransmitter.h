// Pinscape Pico firmware - IR Transmitter
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class lets you control an IR emitter LED connected to a GPIO
// port to transmit remote control codes using numerous standard and
// proprietary protocols.  You can use this to send remote codes to any
// device with a typical IR remote, such as A/V equipment, home
// automation devices, etc.  You can also use this with the companion IR
// Receiver class running on a separate Pico to send IR commands to the
// other device.
//
// We do all of our transmissions with specific protocols rather than
// raw IR signals.  Every remote control has its own way of representing
// a string of data bits as a series of timed IR flashes.  The exact
// mapping between data bits and IR flashes is the protocol.  There are
// some quasi industry standard protocols, where several companies use
// the same format for their codes, but there are many proprietary
// protocols as well.  We have handlers for the most widely used
// protocols: NEC, Sony, Philips RC5 and RC6, Pioneer, Panasonic, and
// several others.  If your device isn't covered yet, it could probably
// be added, since we've tried to design the system to make it easy to
// add new protocols.
//
// When you transmit a code, you specify it in terms of the protocol to
// use and the "code" value to send.  A "code" is just the data value
// for a particular key on a particular remote control, usually
// expressed as a hex number.  There are published tables of codes for
// many remotes, but unfortunately they're not very consistent in how
// they represent the hex code values, so you'll often see the same key
// represented with different hex codes in different published tables.
// We of course have our own way of mapping the hex codes; we've tried
// to use the format that the original manufacturer uses in their tables
// if they publish them at all, but these may or may not be consistent
// with what you find in any tables you consult.  So your best bet for
// finding the right codes to use here is to "learn" the codes using our
// companion class IRReceiver.  That class has a protocol decoder for
// each protocol transmitter we can use here, so if you set that up and
// point a remote at it, it will tell you the exact code we use for the
// key.
//
// The transmitter class provides a "virtual remote control" interface.
// This gives you an imaginary remote control keypad, with a set of
// virtual buttons programmed for individual remote control commands.
// You specify the protocol and command code for each virtual button.
// You can use different protocols for different buttons.
//
//
// How to use the software
//
// First, configure the transmitter from the JSON config data:
//
//    irTransmitter.Configure(json);
//
// The JSON data can specify a set of virtual remote control buttons,
// each assigned a pre-defined command codes You can also define your
// own codes as follows:
//
//    // program virtual button #0 with Sony 20-bit code 0x123, no dittos
//    irTransmitter.ProgramButton(0, IRPRO_SONY20, false, 0x123);
//
// Now you're set up to transmit.  In your main loop, decide when it's time
// to transmit a button, such as by monitoring a physical pushbutton via a
// GPIO DigitalIn pin.  When you want to transmit a code, just tell the
// transmitter that your virtual button is pressed, by calling pushButton()
// with the virtual button ID (corresponding to a virtual button ID you
// previously programmed wtih programButton()) and a status of 'true',
// meaning that the button is pressed.
//
//    irTransmitter.PushButton(0, true);  // push virtual button #0
//
// This starts the transmission and returns immediately.  The transmission
// proceeds in the background (via timer interrupts), so your main loop can
// go about its other business without waiting for the transmission to
// finish.  Most remote codes take 50ms to 100ms to transmit, and you don't
// usually want to stall an MCU app for that long.
//
// If a prior transmission is still in progress when you call pushButton(), 
// the new transmission doesn't interrupt the previous one.  Every code is
// sent as a complete unit to ensure data integrity, so the old one has to
// finish before the new one starts.  Some protocols have minimum repeat
// counts, and the transmitter takes this into account as well.  For example,
// the Sony protocols require each command to be sent at least three times,
// even if the button is only tapped for a brief instant.  So if you send
// a Sony code, a new command won't start transmitting until the last command
// has been sent completely, not just once, but at least three times.
//
// Once the transmitter starts sending the code for a new button, it keeps
// sending the same code on auto-repeat until you either un-press the
// virtual button or press a new virtual button.  Handling auto-repeat
// in the transmitter like this has an important benefit, besides just making
// the API simpler: it allows the transmitter to use the proper coding for
// the repeats according to the rules of the protocol.  Some protocols use
// a different format for the first code of a key press and auto-repeats
// of the same key.  Some protocols also have other repetition features,
// such as "toggle bits" or sequence counters.  The protocol handlers use
// the appropriate handling for their protocols, so you only have to think
// in terms of when the virtual buttons are pressed and un-pressed, without
// worrying about whether a toggle bit or a "ditto" code or a sequence
// counter is needed.
//
// When the button is no longer pressed, call PushButton() again with a
// status of 'false':
//
//    ifTransmitter.PushButton(0, false);
//
// Multiple button presses use simple PC keyboard-like semantics.  At any
// given time, there can be only one pressed button.  When you call 
// pushButton(N, true), N becomes the pressed button, which means that the
// previous pressed button (if any) is forgotten.  As mentioned above, this
// doesn't cancel the previous transmission if it's still in progress.  The
// transmitter continues with the last code until it's finished.  When it
// finishes with a code, the transmitter looks to see if the same button is
// still pressed.  If so, it starts a new transmission for the same button,
// using the appropriate repeat code.  If a new button is pressed, the
// transmitter starts transmitting the new button's code.  If no button is
// pressed, the transmitter stops sending and becomes idle until you press
// another button.
//
// Note that button presses aren't queued.  Suppose you press button #0
// (while no other code is being sent): this starts transmitting the code
// for button #0 and returns.  Now suppose that a very short time later, 
// while that first send is still in progress, you briefly press and release
// button #1.  Button #1 will never be sent in this case.  When you press
// button #1, the transmitter is still sending the first code, so all it
// does at this point is mark button #1 as the currently pressed button,
// replacing button #0.  But as explained above, this doesn't cancel the
// button #0 code transmission in progress.  That continues until the
// complete code has been sent.  At that point, the transmitter looks to
// see which button is pressed, and discovers that NO button is pressed:
// you already told it button #1 was released.  So the transmitter simply
// stops sending and becomes idle.
//
//
// How to determine command codes and the "ditto" mode
//
// Our command codes are expressed as 64-bit integers.  The code numbers
// are in essence the data bits transmitted in the IR signal, but the mapping
// between the IR data bits and the 64-bit code value is different for each
// protocol.  We've tried to make our codes match the numbers shown in the
// tables published by the respective manufacturers for any given remote,
// but you might also find third-party tables that have completely different
// mappings.  The easiest thing to do, really, is to ignore all of that and
// just treat the codes as arbitrary, opaque identifiers, and identify the
// codes for the remote you want to use by "learning" them.  That is, set up
// a receiver with our companion class IRReceiver, point your remote at it,
// and see what IRReceiver reports as the decoded value for each button. 
// Simply use the same code value for each button when sending.
//
// The "ditto" flag is ignored for most protocols, but it's important for a
// few, such as the various NEC protocols.  This tells the sender whether to
// use the protocol's special repeat code for auto-repeats (true), or to send
// send the same key code repeatedly (false).  The concept of dittos only
// applies to a few protocols; most protocols just do the obvious thing and
// send the same code repeatedly when you hold down a key.  But the NEC
// protocols and a few others have special coding for repeated keys.  It's 
// important to use the special coding for devices that expect it, because 
// it lets them distinguish auto-repeat from multiple key presses, which
// can affect how they respond to certain commands.  The tricky part is that 
// manufacturers aren't always consistent about using dittos even when it's
// a standard part of the protocol they're using, so you have to determine
// whether or not to use it on a per-device basis.  The easiest way to do
// this is just like learning codes: set up a receiever with IRReceiver and
// see what it reports.  But this time, you're interested in what happens
// when you hold down a key.  You'll always get one ordinary report first,
// but check what happens for the repeats.  If IRReceiver reports the same 
// code repeatedly, set dittos = false when sending those codes.  If the
// repeats have the "ditto bit" set, though, set dittos = true when sending.
//
//
// How to wire an IR emitter
//
// Any IR LED should work as the emitter.  I used a Vishay TSAL6400 for
// my reference/testing implementation, but many similar IR LEDs are
// available.  You should choose a device such as TSAL6400 with high IR
// luminosity, so that its signal is strong enough to carry across the
// distances needed for IR remote applications, typically 5-10 meters.
//
// WARNING!  DON'T connect an IR LED directly to a Pico GPIO pin.  GPIO
// pins can't drive high-current devices like this directly.  You need
// some kind of current booster/amplifier circuit.  A simple NPN or
// N-MOSFET transistor circuit will suffice: the GPIO pin drives the
// transistor base/gate, and the transistor collector/drain drives the
// IR LED.
//
// If you want to be able to see the transmitter in action, you can
// connect a visible-light LED in parallel with the IR LED, using a
// suitable current-limiting resistor.  IR remote control codes are slow
// enough that you'll be able to see the visible-light LED flicker
// during each transmission as the coded pulses are sent.

#pragma once
#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>

#include "../Pinscape.h"
#include "../GPIOManager.h"
#include "../PWMManager.h"
#include "CircBuf.h"
#include "IRRemote.h"
#include "IRCommand.h"
#include "IRProtocols.h"

// forward/external declarations
class JSONParser;


// IR Remote Transmitter
class IRTransmitter
{
public:
    // Construct
    IRTransmitter();

    // destruction
    ~IRTransmitter()
    {
        delete[] buttons;
    }

    // configure from JSON data
    void Configure(JSONParser &json);
    
    // Program the command code for a virtual button
    void ProgramButton(int buttonId, int protocolId, bool dittos, uint64_t cmdCode);
    
    // Push a virtual button.
    // 
    // When this is called, we'll start transmitting the command code
    // associated with the button immediately if no other transmission
    // is already in progress.  On the other hand, if a transmission of
    // a prior command code is already in progress, the previous command
    // isn't interrupted; we always send whole commands, and never
    // interrupt a command in progress.  Instead, the new button is
    // set as pending.  As soon as the prior transmission finishes,
    // the pending button becomes the current button and we start
    // transmitting its code - but only if the button is still pressed
    // when the previous code finishes.  This means that if you both 
    // press and release a button during the time that another 
    // transmission is in progress, the new button will never be 
    // transmitted.  We operate this way to keep things simple and
    // consistent when it comes to more than just one pending button.
    // This way we don't have to consider queues of pending buttons
    // or create mechanisms for canceling pending commands.
    //
    // If the button is still down when its first transmission ends,
    // and no other button has been pressed in the meantime, the button
    // will auto-repeat.  This continues as long as the button is still
    // pressed and no other button has been pressed.
    // 
    // Only one code can be transmitted at a time, obviously.  The
    // semantics for multiple simultaneous button presses are like those
    // of a PC keyboard.  Suppose you press button A, then a while later,
    // while A is still down, you press B.  Then a while later still,
    // you press C, continuing to hold both A and B down.  We transmit
    // A repeatedly until you press B, at which point we finish sending
    // the current repeat of A (we never interrupt a code in the middle:
    // once started, a code is always finished whole) and start sending
    // B.  B continues to repeat until you press C, at which point we
    // finish the last repetition of B and start sending C.  Once A or
    // B have been superseded, it makes no difference whether you continue
    // to hold them down or release them.  They'll never start repeating
    // again, even if you then release C while A and B are still down.
    void PushButton(int id, bool on);

    // Send an ad hoc command.  This queues the command in an internal
    // buffer and sends it after any existing transmission completes and
    // no buttons are pressed.  Returns true on success, false if the
    // internal queue is full.
    //
    // The optional 'state' variable pointer lets the caller provide a
    // continuously updated On/Off state for the source of the command,
    // in analogy to a physical remote control button.  When the state
    // variable reads true, it means that the imaginary button source is
    // pressed; when it reads false, the button has been released.  If
    // this is provided, the transmitter will check ths state when it
    // finishes sending the code, and if *state is still true, the
    // transmitter will repeat the transmission using the appropriate
    // protocol-specific repeat markers, such as "dittos" or toggle
    // bits.  This will repeat as long as *state remains true.  As soon
    // as *state reads false at the end of a transmission repeat, the
    // transmitter will discard the queued command and move on to the
    // next one.  If state is null, the code will only be sent once.
    //
    // It's up to the caller to ensure that the storage underlying the
    // state variable remains valid until the transmission ends.  The
    // easiest way to do this is to use storage with session duration,
    // such as a static variable, or a member variable of an object
    // that's never going to be deleted.  (This sort of requirement is
    // difficult to fulfill in C++ programs in general, but it's pretty
    // easy in the special context of the Pinscape firmware, because
    // nearly all dynamically allocated objects are created during
    // initialization and never deleted.  They're equivalent to static
    // variables.  This is a design principle that helps ensure that the
    // firmware can't crash due to resource leaks; if you never allocate
    // resources from the main loop, you can't leak anything from the
    // main loop.)
    bool QueueCommand(const IRCommandDesc cmd, int count = 1, volatile bool *state = nullptr);
    
    // Is a transmission in progress?  This is true if the IR TX thread
    // is currently busy.
    bool IsSending() const { return txRunning; }

    // Is a transmission pending or in progress?  This is true if the IR
    // TX thread is busy OR commands are pending in the queue.
    bool IsBusy() const { return txRunning || adHocCommands.IsReadReady(); }

protected:
    // Start the transmitter "thread", if it's not already running.  The
    // thread is actually just a series of timer interrupts; each interrupt
    // sets the next interrupt at an appropriate interval, so the effect is
    // like a thread.
    void TXStart();
    
    // Transmitter "thread" main.  This handles the timer interrupt for each
    // event in a transmission.  This is a Pico SDK alarm callback, so it
    // runs in interrupt context, and the return value indicates the interval
    // to the next alarm time, or zero if the thread is done.
    int64_t TXThread();

    // GPIO output pin controlling the IR LED
    int gpio = -1;
    
    // Virtual button slots.  Each slot represents a virtual remote control
    // button, containing a preprogrammed IR command code to send when the 
    // button is pressed.  Program a button by calling programButton().
    // Press a button by calling pushButton().
    IRCommandDesc *buttons = nullptr;
    
    // Current active virtual button ID.   This is managed in application
    // context and read in interrupt context.  This represents the currently 
    // pushed button.
    int curBtnId = -1;
    
    // Is the transmitter "thread" running?  This is true when a timer is
    // pending, false if not.  The timer interrupt handler clears this
    // before exiting on its last run of a transmission.
    //
    // Synchronization: if txRunning is false, no timer interrupt is either
    // running or pending, so there's no possibility that anyone else will
    // change it, so it's safe for the application to test and set it.  If
    // txRunning is true, only interrupt context can change it, so application
    // context can only read it.
    volatile bool txRunning = false;
    
    // transmitter alarm ID (-1 is the Pico SDK magic number for "invalid")
    alarm_id_t txAlarm = -1;

    // Special button IDs
    static const int TXBTN_NONE = -1;      // no button pressed
    static const int TXBTN_AD_HOC = -2;    // ad hoc command in progress
    
    // Command ID being transmitted in the background "thread".  The thread
    // loads this from curBtnID whenever it's out of other work to do.
    int txBtnId = TXBTN_NONE;
    
    // Protocol for the current transmission
    IRProtocol *txProtocol = nullptr;
    
    // Command value we're currently transmitting
    IRCommandDesc txCmd;
    
    // Protocol state.  This is for use by the individual protocol
    // classes to keep track of their state while the transmission
    // proceeds.
    IRTXState txState;

    // Ad hoc command repeat count
    volatile int txAdHocCount = 0;

    // Pointer to ad hoc command state variable.
    volatile bool *txAdHocStateVar = nullptr;

    // Ad hoc transmission buffer.  This is used to queue commands
    // to send once, in sequence.
    struct AdHocCommand
    {
        IRCommandDesc cmd;      // the command to transmit
        int count;              // number of times to transmit the command
        volatile bool *state;   // caller's state variable; transmission repeats while *state is true
    };
    CircBuf<AdHocCommand, 16> adHocCommands;
};

// global singleton
extern IRTransmitter irTransmitter;
