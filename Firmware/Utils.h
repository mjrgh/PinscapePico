// Pinscape Pico - Utilities
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <hardware/sync.h>
#include <hardware/dma.h>
#include <pico/mutex.h>

// ---------------------------------------------------------------------------
//
// array element count
//
#define _countof(array) (sizeof(array)/sizeof((array)[0]))


// ---------------------------------------------------------------------------
//
// Disable interrupts within a scope.  Instantiate this object to block
// interrupts for the duration of the current scope, and automatically
// restore the original interrupt flags on exit.
class IRQDisabler
{
public:
    IRQDisabler() { istat = save_and_disable_interrupts(); }
    ~IRQDisabler() { restore_interrupts(istat); }

    // original interrupt status
    uint32_t istat;

    // explicitly restore interrupts early, before exiting the scope
    void Restore() { restore_interrupts(istat); }
};


// ---------------------------------------------------------------------------
//
// Spin Locker.  This is a wrapper for acquiring a hardware spin lock for
// the duration of the enclosing scope.
//
class SpinLocker
{
public:
    SpinLocker(spin_lock_t *lock) : lock(lock) { state = spin_lock_blocking(lock); }
    ~SpinLocker() { spin_unlock(lock, state); }
    
    spin_lock_t *lock;   // lock
    uint32_t state;      // original interrupt state, restored on unlock
};

// ---------------------------------------------------------------------------
//
// Mutex locker.  This is a wrapper for the Pico SDK mutex object, acquiring
// the mutex for the duration of the current scope.
//
class MutexLocker
{
public:
    MutexLocker(mutex_t *mutex) : mutex(mutex) { mutex_enter_blocking(mutex); }
    MutexLocker(mutex_t *mutex, uint32_t timeout_us) { locked = mutex_enter_timeout_us(mutex, timeout_us); }
    ~MutexLocker() { mutex_exit(mutex); }

    // Do we have the mutex?  This is always true when we're initialized via
    // the no-timeout constructor, but may be false with the timeout constructor.
    bool IsLocked() const { return locked; }

protected:
    // we have the mutex
    bool locked = true;

    // the mutex
    mutex_t *mutex;
};


// ---------------------------------------------------------------------------
//
// 104ns delay.  This supplements the Pico SDK's sleep_us() and sleep_ms()
// functions with a finer granularity sleep time of approximately 100ns.
// There's nothing magical about 100ns other than that it's a round number
// that's significantly finer granularity than the shorted SDK sleep time
// and significantly longer than system clock rate.
//
// We accomplish this by executing 13 NOPs; with the 125MHz standard system
// clock and the ARM M0+'s single-cycle opcode execution, each instruction
// takes 8ns to execute, so 13 NOPs takes approximately 104ns. 
//
// As with all of the SDK's user-mode sleeps, the actual delay time incurred
// in any individual call to this routine might be longer than the nominal
// dleay, since unpredictable external factors (interrupts, caching, etc)
// can add to the total time from start to finish.  Sleep functions in
// general are best used in contexts where a *minimum* delay time is needed,
// and the maximum is somewhat flexible.  This is often the case when
// generating clock or control signals for external peripherals, where the
// chip requires a minimum gap between adjacent signal level changes.
// 
// For cases that require exact signal timing - not just a minimum delay -
// it's better to use the Pico's PIOs.  Those are purpose-built for precise
// timing synchronized exactly to the system clock cycle.
inline void Sleep104ns()
{
    __asm volatile ("nop");  // 13 * 8ns = 104ns
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
}

// 160 ns sleep
inline void Sleep160ns()
{
    __asm volatile ("nop");  // 20 * 8ns = 160ns
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
    __asm volatile ("nop");
}

// ---------------------------------------------------------------------------
//
// Static string pool.  This is a place to stash strings that must be
// created dynamically, and then left in memory permanently.  It can be
// used when a caller must create such a string but doesn't need to
// retain its own reference to the string, leaving the string without a
// clear "container".  This pool serves as a container of last resort
// for such cases.  This is useful especially for the resource manager
// classes that require "friendly name" strings to store for error
// logging.
//
// Background and motivation: Some of our interfaces take string pointer
// arguments that are required to point to strings with static storage
// duration, because the callee will stash the string pointer in a
// struct without making a private copy of the string text.  The
// retained pointer requires that the caller maintain the text in memory
// for the lifetime of the struct in the callee.  We only use this
// convention when the callee's retained reference has session duration,
// which means that the caller's string must likewise have session
// duration.  We use this idiom as a deliberate efficiency measure for
// the special application environment we're targeting: an embedded
// microcontroller program that runs on bare metal.  The thing that
// makes this application environment special is that the embedded
// program serves as its own operating system, and thus never
// terminates.  A program that never terminates never needs to release
// resources. That allows us to make a simplifying assumption that we
// don't need to use any of the typical formal means of managing dynamic
// C++ string objects, such as std::string or std::unique_ptr.  Those
// mechanisms all come with some overhead cost.  This static string pool
// can be made a bit lower overhead than the standard mecahnisms.
//
// Note that the simplifying asusmption that we never terminate also
// means that we could simply malloc() these static strings and then
// forget the pointers when we're done with them.  Formally, that's
// pretty much the definition of a memory leak, which is really the
// entire point of the static string pool mechanism: it makes explicit
// the cases where we're allocating a string that won't be tracked by
// its creator.  (The string won't *actually* leak, by the way: a
// permanent reference does exist, in the callee that required the
// retained pointer.  But since C++ doesn't have garbage collection or
// reference tracing, the relationship between the retained pointer and
// its source can't be inferred from looking at the struct that contains
// the retained pointer.)  The string pool's purpose is to formally mark
// such cases in the code, so that every such string has an explicit
// owner, even if the owner is just a global pool of miscellanous text.
//
// Note that it's important that this mechanism ONLY be used for cases
// that are inherently one-time-only.  For the most part, this means
// that it should only be used for initialization and setup, and never
// during normal ongoing operations.  The memory allocated for pool
// strings is tied up for the rest of the session, by design; using them
// for any operation that could be repeated indefinitely will fill up
// all available memory and crash the program.  Don't use this as a fig
// leaf to hide what's really a memory leak!  It's only for cases where
// the string will deterministically remain referenced for the entire
// remainder of the session.
class StringPool
{
public:
    // Add a string to the pool.  This copies the given string (which is
    // presumably in some sort of temporary storage, such as on the
    // stack) into the pool, and returns a pointer to the static copy.
    // The string can be given as a conventional null-terminated string
    // or as a counted-length string (which need not have a null
    // terminator).  In either case, the returned pool copy is
    // null-terminated.
    const char *Add(const char *buf) { return Add(buf, strlen(buf)); }
    const char *Add(const char *buf, size_t len);

    // Add a string via sprintf-style formatting
    const char *Format(const char *fmt, ...);
    const char *FormatV(const char *fmt, va_list va);
};

// global singleton
extern StringPool stringPool;

// shorthand for stringPool.Format() and stringPool.FormatV()
const char *Format(const char *fmt, ...);
inline const char *FormatV(const char *fmt, va_list va) { return stringPool.FormatV(fmt, va); }


// ---------------------------------------------------------------------------
//
// Additional DMA operations not in the SDK
//

// set a DMA channel's chain_to register
static inline void SetDMAChainTo(int channel, int chainTo)
{
    // go through the all_ctrl alias (not ctrl_trigger), so that we don't trigger the channel
    // when we write the register
    auto &ctrl = dma_channel_hw_addr(channel)->al1_ctrl;

    // replace the chain_to bits in the CTRL register
    ctrl = (ctrl & ~DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) | (chainTo << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
}

// disable a DMA channel's chain_to (by setting pointing the chain_to to its own channel)
static inline void DisableDMAChainTo(int channel) { SetDMAChainTo(channel, channel); }

