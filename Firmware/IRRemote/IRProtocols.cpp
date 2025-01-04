// Pinscape Pico firmware - IR Protocol Handlers
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdio.h>
#include <stdint.h>

#include <pico/stdlib.h>

#include "IRCommand.h"
#include "IRReceiver.h"
#include "IRProtocols.h"
#include "../PWMManager.h"


// ---------------------------------------------------------------------------
// 
// IRProtocol base class implementation
//

// protocol handler singletons
IRProtocols *IRProtocol::protocols;


// Look up a protocol by ID
IRProtocol *IRProtocol::SenderForId(int id)
{
    // try each protocol singleton in the sender list
    #define IR_PROTOCOL_TX(className) \
        if (protocols->s_##className.IsSenderFor(id)) \
            return &protocols->s_##className;
    #include "IRProtocolList.h"
    
    // not found    
    return 0;
}

// allocate the protocol singletons
void IRProtocol::AllocProtocols()
{
    if (protocols == 0)
        protocols = new IRProtocols();
}

// report code with a specific protocol
void IRProtocol::ReportCode(IRRecvProIfc *receiver, const IRCommandReceived &cmd)
{
    receiver->WriteCommand(cmd);
}

// check for auto-repeat using dittos
void IRProtocol::CheckDittos(IRRecvProIfc *receiver, IRCommandReceived cmd, bool isEmptyDitto)
{
    // For this to be an auto-repeat, the previous command must have
    // come from the same protocol (technically, from the same physical
    // sender, but there's no way to determine that), and must be
    // relatively recent (within a few hundred milliseconds).  If those
    // conditions are met, the command is a repeat if its ditto flag is
    // set, indicating that the packet was encoded with the protocol's
    // special ditto format.
    uint64_t now = time_us_64();
    uint64_t dt = now - receiver->tLastCommand;
    auto &last = receiver->lastCommand;
    if (dt < 300000 && cmd.proId == last.proId && cmd.ditto)
    {
        // it's an auto-repeat
        cmd.isAutoRepeat = true;

        // If the protocol explicitly uses empty dittos, or the new code
        // is zero, copy the command code from the previous command.
        if (isEmptyDitto || cmd.code == 0)
            cmd.code = last.code;
    }

    // report the code
    ReportCode(receiver, cmd);
}

// check for auto-repeat using the toggle bit
void IRProtocol::CheckToggles(IRRecvProIfc *receiver, IRCommandReceived cmd)
{
    // For this to be an auto-repeat, the previous command must have
    // come from the same protocol (technically, from the same physical
    // sender, but there's no way to determine that), and must be
    // relatively recent (within a few hundred milliseconds), and must
    // match the command code from the new command.  If those conditions
    // are met, the command is a repeat if its toggle bit is the same as
    // the previous command's toggle bit, indicating that the user has
    // been holding down the same button on the remote the whole time.
    uint64_t now = time_us_64();
    uint64_t dt = now - receiver->tLastCommand;
    auto &last = receiver->lastCommand;
    if (dt < 300000 && cmd.proId == last.proId && cmd.code == last.code
        && cmd.toggle == last.toggle)
    {
        // it's an auto-repeat
        cmd.isAutoRepeat = true;
    }

    // report the code
    ReportCode(receiver, cmd);
}

// check for auto-repeat using position codes
void IRProtocol::CheckPositionCodes(IRRecvProIfc *receiver, IRCommandReceived cmd, bool codeMustMatch)
{
    // For this to be an auto-repeat, the previous command must have
    // come from the same protocol (technically, from the same physical
    // sender, but there's no way to determine that), and must be
    // relatively recent (within a few hundred milliseconds).
    uint64_t now = time_us_64();
    uint64_t dt = now - receiver->tLastCommand;
    auto &last = receiver->lastCommand;
    if (dt < 300000 && cmd.proId == last.proId)
    {
        // It can only be a match if the previous and new codes match, OR
        // the caller didn't require matching codes
        if (cmd.code == last.code || !codeMustMatch)
        {
            // All other requirements are met, so it's now up to the
            // position codes.  If the NEW code is a FIRST, it's not an
            // auto-repeat (by definition of FIRST).  If the PREVIOUS
            // code was a LAST, the new code can't be a repeat, since
            // LAST means that it's the end of a repeating sequence.
            // Any other combination is a repeat.
            if (cmd.position == IRCommandReceived::Position::First)
            {
                // the new code is a FIRST, so it can't be a repeat
            }
            else if (last.position == IRCommandReceived::Position::Last)
            {
                // the previous code is a LAST, so the new code can't be a repeat
            }
            else
            {
                // any other combination is a repeat
                cmd.isAutoRepeat = true;
            }
        }
    }

    // report the code
    ReportCode(receiver, cmd);
}

// check for auto-repeat by elapsed time between commands received
void IRProtocol::CheckRepeatByTime(IRRecvProIfc *receiver, IRCommandReceived cmd, int maxTime_us)
{
    // For this to be an auto-repeat, the previous command must have
    // come from the same protocol (technically, from the same physical
    // sender, but there's no way to determine that), and must be
    // relatively recent (within a few hundred milliseconds), and must
    // have the same command code as the prior command.
    uint64_t now = time_us_64();
    uint64_t dt = now - receiver->tLastCommand;
    auto &last = receiver->lastCommand;
    if (dt < maxTime_us && cmd.proId == last.proId && cmd.code == last.code)
    {
        // consider it an auto-repeat
        cmd.isAutoRepeat = true;
    }

    // report the code
    ReportCode(receiver, cmd);
}


// -------------------------------------------------------------------------
//
// Kaseikyo class implementation.
//

// OEM <-> subprotocol map
const IRPKaseikyo::OEMMap IRPKaseikyo::oemMap[] = {
    { 0x0000, IRPRO_KASEIKYO48, 48 },
    { 0x0000, IRPRO_KASEIKYO56, 56 },
    { 0x5432, IRPRO_DENONK, 48 },
    { 0x1463, IRPRO_FUJITSU48, 48 },
    { 0x1463, IRPRO_FUJITSU56, 56 },
    { 0x0301, IRPRO_JVC48, 48 },
    { 0x0301, IRPRO_JVC56, 56 },
    { 0x23CB, IRPRO_MITSUBISHIK, 48 },
    { 0x0220, IRPRO_PANASONIC48, 48 },
    { 0x0220, IRPRO_PANASONIC56, 56 },
    { 0xAA5A, IRPRO_SHARPK, 48 },
    { 0x4353, IRPRO_TEACK, 48 }
};
const int IRPKaseikyo::nOemMap = sizeof(oemMap)/sizeof(oemMap[0]);

