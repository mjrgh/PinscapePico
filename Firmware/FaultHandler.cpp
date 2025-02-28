// Pinscape Pico - Fault Handler
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/platform.h>
#include <hardware/exception.h>
#include <hardware/watchdog.h>

// local project headers
#include "Pinscape.h"
#include "Reset.h"
#include "Logger.h"
#include "USBIfc.h"
#include "FaultHandler.h"
#include "Watchdog.h"
#include "CommandConsole.h"
#include "Logger.h"
#include "m0FaultDispatch.h"

// fault handler singleton
FaultHandler faultHandler;

// m0FaultDispatch cause code names
static const char *causeName[]{
    "Invalid",
    "Read access fault",
    "Write access fault",
    "Bad CPU mode",
    "Unaligned access",
    "Undefined instruction 16",
    "Undefined instruction 32",
    "Breakpoint",
    "Fault handler error",
    "Unknown",
};

// Fault data in un-cleared RAM.  This is static memory that's preserved
// across a CPU reset.
struct FaultData
{
    // Checksum.  This is a simple sum mod 2^32 of the bytes in the
    // struct, calculated with this element and the next set to zero.
    // This allows us to determine if the struct was populated before
    // the last CPU reset.  On power-on, uninitialized RAM contains
    // random data, so the probability of this accidentally containing
    // the correct checksum is nearly zero; so if it DOES contain the
    // correct checksum, the struct was almost certainly initialized
    // before the most recent CPU reset.
    uint32_t checksum;

    // Bitwise negation of checksum.  This contains ~checksum when
    // the struct is properly initialized.
    uint32_t bnotChecksum;

    // Is the crash data valid for this session?  This is set at
    // initialization according to the validity of the crash log
    // carried over from the PREVIOUS session, before the reset.
    // This must not be tested before Init() is called, since it
    // contains random data on initial power-on.
    bool isCrashLogValid;

    // "Seal" the struct - populate the checksum fields to indicate
    // that the struct is valid.
    void Seal()
    {
        // calculate and store the checksum and its bitwise inverse
        checksum = crashLog.CalcChecksum();
        bnotChecksum = ~checksum;
    }

    // test for a valid seal
    bool TestSeal()
    {
        uint32_t c = crashLog.CalcChecksum();
        return checksum == c && bnotChecksum == ~c;
    }

    // Log the data
    void Log()
    {
        crashLog.Log();
    }

    // crash log
    struct CrashLog
    {
        // faulting core number
        int core;

        // caller's stack pointer ('sp' register) when exception occurred
        uint32_t sp;

        // fault handler processor state data
        uint32_t cause;
        uint32_t extraData;
        CortexExcFrame exc;
        CortexPushedRegs regs;
        
        // stack capture
        uint32_t stackData[256];
        uint32_t stackDataBytes;

        // Calculate the checksum
        uint32_t CalcChecksum()
        {
            // calculate the sum of the bytes in the struct mod 2^32
            uint32_t acc = 0;
            for (const uint8_t *p = reinterpret_cast<const uint8_t*>(this), *pEnd = reinterpret_cast<uint8_t*>(this + 1) ; p < pEnd ; )
                acc += *p++;
            
            // return the result
            return acc;
        }

        // write the crash data to the system log
        void Log()
        {
            // log the CPU state
            ::Log(LOG_INFO, "CPU Hard Fault exception data: core: %d, cause: %s (code %d), extra data: %08lX, pc: %08lX\n",
                  core, cause > 0 && cause < _countof(causeName) ? causeName[cause] : "Invalid", cause, extraData, exc.pc);
            ::Log(LOG_INFO, ". r0: %08lX, r1: %08lX, r2:  %08lX, r3:  %08lX, r4:  %08lX, r5: %08lX, r6: %08lX, r7: %08lX\n",
                  exc.r0, exc.r1, exc.r2, exc.r3, regs.regs4_7[0], regs.regs4_7[1], regs.regs4_7[2], regs.regs4_7[3]);
            ::Log(LOG_INFO, ". r8: %08lX, r9: %08lX, r10: %08lX, r11: %08lX, r12: %08lX, sp: %08lX, lr: %08lX, sr: %08lX\n",
                  regs.regs8_11[0], regs.regs8_11[1], regs.regs8_11[2], regs.regs8_11[3], exc.r12, sp, exc.lr, exc.sr);

            // log the captured stack
            ::Log(LOG_INFO, "Stack dump at fault:\n");
            for (int ofs = 0 ; ofs < stackDataBytes/4 ; )
            {
                ::Log(LOG_INFO, ". %08lX ", sp + ofs);
                for (int i = 0 ; i < 8 && ofs < stackDataBytes/4 ; ++i, ++ofs)
                    ::Log(LOG_INFO, " %08lX", stackData[ofs]);
                ::Log(LOG_INFO, "\n");
            }
        }

        // write the crash data to a command console
        void LogTo(CommandConsole *c)
        {
            c->Print("Hard Fault exception data: Core: %d, Cause: %s (code %d), Extra data: %08lX, pc: %08lX\n",
                     core, cause > 0 && cause < _countof(causeName) ? causeName[cause] : "Invalid", cause, extraData, exc.pc);
            c->Print("CPU state at exception:\n"
                     "  r0: %08lX, r1: %08lX, r2:  %08lX, r3:  %08lX, r4:  %08lX, r5: %08lX, r6: %08lX, r7: %08lX\n",
                     exc.r0, exc.r1, exc.r2, exc.r3, regs.regs4_7[0], regs.regs4_7[1], regs.regs4_7[2], regs.regs4_7[3]);
            c->Print("  r8: %08lX, r9: %08lX, r10: %08lX, r11: %08lX, r12: %08lX, sp: %08lX, lr: %08lX, sr: %08lX\n",
                     regs.regs8_11[0], regs.regs8_11[1], regs.regs8_11[2], regs.regs8_11[3], exc.r12, sp, exc.lr, exc.sr);
        }

        bool LogStackTo(CommandConsole *c, int lineNum)
        {
            // check if there's more data available - 32 bytes/8 DWORDs per line
            if (lineNum*32 < stackDataBytes)
            {
                c->Print("%08lX ", sp + lineNum*32);
                for (int i = lineNum*8, end = i + 8 ; i < end && i < stackDataBytes/4 ; ++i)
                    c->Print(" %08lX", stackData[i]);
                c->Print("\n");

                return true;
            }
            else
            {
                // no more data
                return false;
            }
        }
    };
    CrashLog crashLog;
};
FaultData __uninitialized_ram(faultData);

// initialize
void FaultHandler::Init()
{
    // set up our exception handler
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, HardFault_Handler);

    // mark whether or not the crash log contains valid data from before the reset
    faultData.isCrashLogValid = faultData.TestSeal();
}

// is the crash log data valid?
bool FaultHandler::IsCrashLogValid()
{
    return faultData.isCrashLogValid;
}

// Stack limit markers, defined in Pico SDK link script.  These are symbols
// defined at the assembler level that serve to give us the memory locationss;
// they're not actually variables in the usual C/C++ sense, but just byte
// locations in memory.
extern "C" {
    extern uint8_t __StackTop, __StackBottom, __StackOneTop, __StackOneBottom;
}

// Fault handler callback.  The hard fault handler decodes the processor state
// information on the stack and invokes this callback.
extern "C" void faultHandlerWithExcFrame(struct CortexExcFrame *exc, uint32_t cause, uint32_t extraData, struct CortexPushedRegs *pushedRegs)
{
    // collect the data into the persistent buffer
    auto &c = faultData.crashLog;
    c.core = get_core_num();
    c.sp = static_cast<uint32_t>(reinterpret_cast<intptr_t>(exc + 1));
    c.cause = cause;
    c.extraData = extraData;
    memcpy(&c.exc, exc, sizeof(c.exc));
    memcpy(&c.regs, pushedRegs, sizeof(c.regs));

    // Capture stack data.  'exc' is the actual exception stack frame pointer,
    // so the caller's stack starts just above the frame.  The stack grows downwards
    // on Cortex platforms, so the active stack consists of the region between the
    // stack pointer and top-of-stack.
    const uint8_t *stackTop = get_core_num() == 0 ? &__StackTop : &__StackOneTop;
    const uint8_t *callerStackPtr = reinterpret_cast<const uint8_t*>(exc + 1);
    c.stackDataBytes = static_cast<uint32_t>(std::min(sizeof(c.stackData), static_cast<size_t>(stackTop - callerStackPtr)));
    memcpy(c.stackData, callerStackPtr, c.stackDataBytes);

    // seal the struct (write checksums) so that we can tell it's valid after reboot
    faultData.Seal();

    // reboot into safe mode
    picoReset.Reboot(false, PicoReset::BootMode::SafeMode);
}

// Log the crash data if present
void FaultHandler::LogCrashData()
{
    // check for a valid seal
    if (faultData.isCrashLogValid || faultData.TestSeal())
    {
        // mark the captured data as valid for this session
        faultData.isCrashLogValid = true;
        
        // it's valid - log the crash data
        faultData.Log();

        // Invalidate the crash data so that we don't log it again on the next
        // session if no new crash occurs.  The crash log is deliberately configured
        // to survive resets, so it won't just survive the reset that got us here,
        // but also the next reset, even if that reset is intentional and orderly.
        // To invalidate the data, it's sufficient to set both checksum fields to
        // zero - that will always yield an invalid seal, because the inverted
        // checksum won't match the inverse of the checksum if they're both zero.
        faultData.checksum = 0;
        faultData.bnotChecksum = 0;
    }
}

void FaultHandler::LogCrashDataTo(CommandConsole *console)
{
    faultData.crashLog.LogTo(console);
}

bool FaultHandler::LogStackDataTo(CommandConsole *console, int lineNum)
{
    return faultData.crashLog.LogStackTo(console, lineNum);
}
