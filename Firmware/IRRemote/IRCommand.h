// Pinscape Pico firmware - IR Command Descriptor
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file defines a common representation for command codes across
// all of our supported IR protocols.  A "command code" generally maps
// to the data sent for one key press on a remote control.  We define
// two struct variations here:
//
// - IRCommandDesc - an IR Command Descriptor.  This specifies a command
//   in a format suitable for transmitting, and for comparing against
//   received commands.  We provide a canonical string format for this
//   object, so that it can be exported to and imported from external
//   data sources, such as configuration files.
//
//   The Command Descriptor is a sort of abstract specification for the
//   command that the protocol engine can translate into IR pulses for
//   transmission, and match against an incoming IR pulse sequence to
//   recognize the command on receipt.  A single descriptor might map
//   to one or more IR pulse streams, because the pulse stream for a
//   given code is also dependent upon ephemeral state in some codes.
//   For example, some codes alter the bit stream for the second and
//   later codes when the code is auto-repeated (such as by holding
//   down the button on the remote).
//
//   The command descriptor contains the following:
//
//   - The protocol ID.  This represents the mapping between data bits
//     and the physical IR signals.  Each protocol has its own rules for
//     how the individual bits are represented and how the bits making
//     up a single command are arranged into a "command word" unit for
//     transmission.  See IRProtocolID.h for a list of protocol codes.
//  
//   - An integer with the unique key code.  For most protocols, this is
//     simply the sequence of bits sent by the remote.  This is the
//     "base" code, with any toggle bit position set to zero in the
//     canonical representation.  (Some codes have a "toggle" bit that
//     gets flipped on each new key press, to distinguish auto-repeats
//     from physically separate button pushes.)  The canonical format
//     lets us match a received code to a descriptor regardless of the
//     toggle state.
//
//     If the protocol has a published spec with a defined bit ordering
//     (LSB first or MSB first), we'll use the same bit ordering to
//     construct the key code value, otherwise the bit order is
//     arbitrary.  We use the canonical bit order when one exists to
//     make it more likely that our codes will match those in published
//     tables for commercial remotes using the protocol.  We'll also try
//     to use the same treatment as published tables for any meaningless
//     structural bits, such as start and stop bits, again so that our
//     codes will more closely match published codes.

// - IRCommandReceived - a concrete received command code.  This
//   contains all of the information in an Command Descriptor, plus some
//   additional information about the concrete pulse sequence received
//   for this specific transmission:
//
//   - "Toggle bit" state.  Some protocols have the notion of a toggle
//     bit, which is a bit that gets flipped on each key press, but
//     stays the same when the same code is sent repeatedly while the
//     user is holding down a key.  This lets the receiver distinguish
//     two distinct key presses from one long key press, which is
//     important for non-idempotent commands like "power toggle" and
//     "keypad 5".  The toggle bit state in the Command Received struct
//     specifies the current toggle state in the bit stream.
//  
//   - "Ditto" flag.  Some protocols use a special code to indicate an
//     auto-repeat.  Like the toggle bit, this serves to distinguish
//     repeatedly pressing the same key from holding the key down.
//     Ditto codes in most protocols that use them don't contain any
//     data bits, so the key code will usually be zero if the ditto bit
//     is set.  The ditto flag indicates whether dittos were detected
//     in the received pulse stream.
//
// Note that most of the published protocols have more internal
// structure than this to the bit stream.  E.g., there's often an
// "address" field of some kind that specifies the type of device the
// code is for (TV, DVD, etc), and a "command" field with a key code
// scoped to the device type.  We don't try to break down the codes into
// subfields like this.  (With the exception of "toggle" bits, which
// does get special treatment because they'd otherwise make each unique
// command look like it two separate codes, one with each toggle state.)
//

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <pico/stdlib.h>
#include "../Pinscape.h"
#include "IRProtocolID.h"

// IR Command Descriptor.  This contains the abstract, canonical
// representation of a unique IR command code.  This format has a
// corresponding string representation that can be used to store command
// codes externally in arbitrary data stoers, such as configuration
// files.
//
// This format is "abstract" in that it doesn't have any of the
// ephemeral state bits that are embedded in the pulse stream in some
// protocols, such as dittos or toggles.  This is a specification of the
// command code that can be used to generate the concrete pulse stream
// for a transmission, or to recognize the code for a given sequence of
// pulses received.
struct IRCommandDesc
{
    // instantiate an empty descriptor
    IRCommandDesc() { }

    // instantiate a descriptor with a given code and protocol
    IRCommandDesc(uint8_t proId, uint64_t code, bool useDittos) :
        code(code), proId(proId), useDittos(useDittos) { }

    // instantiate from another descriptor
    IRCommandDesc(const IRCommandDesc &desc) :
        code(desc.code), proId(desc.proId), useDittos(desc.useDittos) { }

    // Instantiate a descriptor from the canonical string representation:
    //
    //   "<protocol>.<flags>.<code>"
    //
    // All fields are given as hex numbers:
    //
    //   <protocol> is two hex digits, specifying the protocol ID
    //   number (see IRProtocolList.h)
    //
    //   <flags> is two hex digits, specifying flags:
    //      0x02 = use dittos
    //
    //   <code> is a hex number, 4 to 16 digits, specifying the
    //   command code number.
    //
    IRCommandDesc(const char *str) { Parse(str); }

    // Parse a command string, and load the object's fields from the
    // result.  Returns true on success, false if the string isn't
    // well-formed.
    bool Parse(const char *str);

    // Format the descriptor into the canonical string format.  Returns
    // a pointer to the caller's buffer for convenience.  The string is
    // always null-terminated, even if it overflows the buffer.  A
    // buffer 24 bytes long is sufficient for any current code.
    char *ToString(char *buf, size_t len) const;

    // Compare for equality.  Two commands are equivalent if they have
    // the same protocol and command code.
    bool operator==(const IRCommandDesc &b) {
        return b.code == code && b.proId == proId;
    }

    // 64-bit command code, containing the decoded bits of the command.
    // The bits are arranged in LSB-first or MSB-first order, relative
    // to the order of IR transmission, according to the conventions of
    // the protocol.  This includes all bits from the transmission,
    // including things like error detection bits, except for
    // meaningless fixed structural elements like header marks, start
    // bits, and stop bits.  
    //
    // If there's a "toggle" bit in the code, its bit position in 'code'
    // is ALWAYS set to zero, but we store the actual bit value in
    // 'toggle'.  This ensures that clients who don't care about toggle
    // bits will see code value every time for a given key press, while
    // still preserving the toggle bit information for clients who can
    // use it.
    //
    // See the individual protocol encoder/decoder classes for the exact
    // mapping between the serial bit stream and the 64-bit code value
    // here.
    uint64_t code = 0;

    // Protocol ID - a PRO_xxx value
    uint8_t proId = IRPRO_NONE;

    // Use dittos?  The transmitter uses this for protocols that have
    // variants with and without dittos, to determine which variant to
    // use.  This is ignored on reception, because the presence of
    // dittos is detected directly from the pulse stream, and it's
    // ignored when comparing commands.
    bool useDittos = false;
};


// Three-state logic for reporting dittos and toggle bits.  "Null"
// means that the bit isn't used at all, which isn't quite the same
// as false.
class Bool3
{
public:
    enum val { null3, false3, true3 } __attribute__ ((packed));

    static const Bool3 null;

    Bool3() : v(null3) { }
    Bool3(val v) : v(v) { }
    Bool3(bool v) : v(v ? true3 : false3) { }
    Bool3(int v) : v(v ? true3 : false3) { }
    Bool3(const Bool3 &b) : v(b.v) { }

    operator int() const { return v == true3; }
    operator bool() const { return v == true3; }

    bool IsNull() const { return v == null3; }

private:
    val v;
};


// IR Command Received.  This describes a command received, which
// consists of a decoded Command Descriptor, plus the ephemeral state
// bits detected in the concrete transmission received.
struct IRCommandReceived : IRCommandDesc
{
    // Position codes.  Some protocols (Ortek MCE in particular) encode
    // a three-state position marker in each code for auto-repeat sensing.
    // The first code in a repeating sequence is marked as First, the
    // last is marked as Last, and all repeats in between are Middle.
    // Codes that don't use position markers should use Null.
    enum class Position
    {
        Null,    // no position marker used in this code
        First,   // first of a repeated sequence
        Middle,  // second+
        Last,    // last; key was released
    };
    
    IRCommandReceived() :
        IRCommandDesc(),
        toggle(false), hasToggle(false),
        ditto(false), hasDittos(false),
        position(Position::Null),
        isAutoRepeat(false)
    {
    }

    IRCommandReceived(const IRCommandReceived &cmd) :
        IRCommandDesc(cmd),
        toggle(cmd.toggle), hasToggle(cmd.hasToggle),
        ditto(cmd.ditto), hasDittos(cmd.hasDittos),
        position(cmd.position),
        isAutoRepeat(cmd.isAutoRepeat)
    {
    }

    IRCommandReceived(uint8_t proId, uint64_t code) :
        IRCommandDesc(proId, code, false),
        toggle(false), hasToggle(false),
        ditto(false), hasDittos(false),
        position(Position::Null),
        isAutoRepeat(false)
    {
    }

    IRCommandReceived(uint8_t proId, uint64_t code, Bool3 toggle, Bool3 ditto, Position position) :
        IRCommandDesc(proId, code, !ditto.IsNull()),
        toggle(toggle), hasToggle(!toggle.IsNull()),
        ditto(ditto), hasDittos(!ditto.IsNull()),
        position(position)
    {
    }

    // convenience ctor for ditto protocols
    struct Ditto { Ditto(bool ditto) : ditto(ditto) { } bool ditto; };
    IRCommandReceived(uint8_t proId, uint64_t code, Ditto ditto) :
        IRCommandDesc(proId, code, ditto.ditto),
        toggle(false), hasToggle(false),
        ditto(ditto.ditto), hasDittos(true),
        position(Position::Null)
    {
    }

    // convenience ctor for toggle protocols
    struct Toggle { Toggle(bool toggle) : toggle(toggle) { } bool toggle; };
    IRCommandReceived(uint8_t proId, uint64_t code, Toggle toggle) :
        IRCommandDesc(proId, code, false),
        toggle(toggle.toggle), hasToggle(true),
        ditto(false), hasDittos(false),
        position(Position::Null)
    {
    }

    // convenience ctor for position-code protocols
    IRCommandReceived(uint8_t proId, uint64_t code, Position position) :
        IRCommandDesc(proId, code, false),
        toggle(false), hasToggle(false),
        ditto(false), hasDittos(false),
        position(position)
    {
    }
    
    // Format the command as a string.  This adds the toggle and ditto
    // flags to the basic descriptor format.  Unlike ToString() on the
    // base class, this ISN'T a canonical string expression of the
    // command object; rather, this is just a human-readable display
    // format showing the contents of the object.
    char *Format(char *buf, size_t len) const;
    
    // Toggle bit.  Some protocols have a "toggle bit", which the sender
    // flips each time a new key is pressed.  This allows receivers to
    // distinguish auto-repeat from separate key presses.  For protocols
    // that define a toggle bit, we'll store the bit here, and set the
    // bit position in 'code' to zero.  That way, the client always sees
    // the same code for every key press, regardless of the toggle state,
    // but callers who want to make use of the toggle bit can still get
    // at the transmitted value by inspecting this field.
    bool toggle;

    // Does the protocol use toggle bits?  This is a fixed feature of 
    // the protocol, so it doesn't tell us whether the sender is
    // actually using toggles properly, only that the bit exists in
    // the protocol.  If you want to determine if the sender is using
    // toggles for learning remote purposes, ask the user to press the
    // same key several times in a row, and observe if the reported
    // toggle bits alternate.
    bool hasToggle;

    // Ditto bit.  Some protocols send a distinct code to indicate auto-
    // repeat when a key is held down.  These protocols will send the 
    // normal code for the key first, then send the special "ditto" code
    // repeatedly as long as the key is held down.  If this bit is set,
    // the command represents one of these auto-repeat messages.  Ditto
    // codes usually don't have any data bits, so the 'code' value will
    // usually be zero if this is set.
    bool ditto;

    // Does the protocol have a ditto format?  This only indicates if
    // the protocol has a defined ditto format, not if the sender is
    // actually using it.  If you want to determine if the sender uses
    // dittos for learning remote purposes, ask the user to hold a key
    // down long enough to repeat, and observe the reported codes to
    // see if the ditto bit is set after the first repeat.
    bool hasDittos;

    // Code position marker (Position::Null if not used in this code)
    Position position;

    // Is this an auto-repeated command?  Auto-repeat means that the
    // command was repeated because the user was continuously holding
    // down the button on the remote.  This case is distinct from the
    // user pressing the button repeatedly, because some commands are
    // edge-sensitive.  That makes it important to replicate a
    // continuous hold in our local representation of the command state.
    //
    // Some protocols encode extra information specifically for
    // distinguishing between continuous hold and repeated presses:
    // toggle bits, dittos, and position markers.  For those, we infer
    // the auto-repeat state from the extra information.  For protocols
    // that don't encode any auto-repeat information explicitly, we have
    // to rely on timing; if a code is repeated within a certain time
    // window, we assume that it's auto-repeating.
    bool isAutoRepeat = false;
};
