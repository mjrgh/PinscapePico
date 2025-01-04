// Pinscape Pico - Pico reset/reboot routines
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <list>
#include <pico/stdlib.h>

class PicoReset
{
public:
    // Set the NEXT boot mode in the scratch registers.  This stores
    // distinctive values in watchdog scratch registers 0 and 1 to
    // control the boot mode on the next reset.  The startup code uses
    // this to go into safe mode if we crash too soon after startup.
    //
    // Note that we deliberately use large, random-ish numbers to
    // represent the modes, rather than a simple 0-1-2-3 sequence, to
    // make it less likely that we'll match values in the scratch
    // registers that came from some other source, such as hardware
    // randomness at power-on, or because some Pico SDK module or
    // another of our own subsystems intentionally wrote something to
    // the scratch registers for its own purposes.  Small values near
    // zero aren't necessarily more likely than anything else due to any
    // law of physics, but they might be slightly more likely if a human
    // programmer was involved in choosing them, or if they were
    // mechanically assigned by a compiler (as in an enum).  It seems
    // less likely that we'd collide with another intentionally chosen
    // value elsewhere in the UINT32 range, even if (maybe especially
    // if) the other guy is applying the same thinking about random
    // distribution.  Eliminating the extra-likely values near zero, we
    // merely have the Birthday Problem to contend with, which says that
    // our odds of a collision are somewhere around 1 in 64K, or .0015%,
    // which seems acceptable in a self-contained system like this.
    enum class BootMode
    {
        // Invalid/Unknown - reported when reading the scratch registers
        // and they don't contain a valid mode setting.
        Unknown = 0x1175AA01,

        // Factory Mode - use factory settings, ignoring all user
        // configuration files.  We use this mode if we crash early
        // after launching in Safe Mode, either during initialization or
        // shortly after entering the main loop.  An early crash is
        // likely to happen again after a reboot if we apply the same
        // settings as last time, since loading the same settings will
        // likely re-create the same conditions that caused the crash
        // last time.  We might be able to get out of this crash/reset
        // loop by bypassing the settings we used last time and
        // reverting to the basic factory settings.
        FactoryMode = 0x2289BB02,

        // Safe Mode - use the user's Safe Mode configuration, if
        // provided, or revert to factory settings if not.  This mode is
        // selected if we crashed during initialization or early in the
        // main loop after a boot in Normal mode, with the user's
        // configuration file loaded.  As with Factory Mode, the theory
        // is that loading the full settings will likely re-create the
        // conditions that led to the early crash, so we'll just keep
        // crashing and resetting if we keep trying to load the same
        // settings.  The point of having a separate Safe Mode user
        // configuration is that it lets the user create a basic
        // environment that's been tested as safe, but that still has a
        // minimal complement of hardware peripherals enabled.
        SafeMode = 0x3362CC03,

        // Normal mode - load the user's configuration as normal.
        // This is set as soon as the main loop has been running for
        // long enough to consider the configuration stable.
        Normal = 0x4457DD04,
    };
    void SetNextBootMode(BootMode mode);

    // Get the boot mode scratch register settings
    BootMode GetBootMode();

    // Reboot the Pico.  If it's safe to reboot, this will immediately
    // perform the reboot, which will never return to the caller.  If
    // another subsystem has set a reboot lock, this will only set
    // internal flags to remember that the reboot has been requested,
    // and then return to the caller; the actual reboot will happen in
    // the Task() handler after all locks have been lifted.  The caller
    // must therefore be prepared for both possibilities: the function
    // might return immediately, OR never return.
    //
    // A software set on the Pico has essentially the same effect as
    // power cycling the device.  After the reset, we'll boot into the
    // Pico's native ROM boot loader if bootLoaderMode is true,
    // otherwise we'll simply restart the resident flash program, as
    // though the device had been power cycled.
    void Reboot(bool bootLoaderMode, BootMode mode = BootMode::Normal);

    // Prepare for a software reset.  This should be called before
    // resetting the Pico either to restart the firmware program or to
    // invoke the ROM boot loader.  This routine attempts to switch all
    // controlled feedback devices off to ensure that nothing is left
    // activated through the reset.  This is especially important when
    // resetting into the ROM boot loader, since the boot loader has no
    // knowledge of our controlled devices and thus no way to switch
    // them off.
    //
    // It's not necessary (but it won't hurt) to call this routine prior
    // to calling Reboot(), since the reboot routine will call this
    // itself before performing the reset.  The main reason this is
    // exposed as a public interface is for cases where a subsystem is
    // about to start a potentially long-running process that isn't
    // guaranteed to succeed, in which case the user might need to reset
    // the Pico manually.
    void PrepareForReboot();

    // Periodic task handler.  The main loop should call this
    // periodically to check the reboot status and apply any pending
    // reboot when safe.  Reboots can't always be performed immediately,
    // because some subsystems perform tasks that should be completed
    // before the reboot if possible.  RebootPico() therefore doesn't
    // actually carry out the reboot, but just sets flags indicating
    // that a reboot has been requested.  This task routine actually
    // performs a reboot when it's safe to do so.
    inline void Task()
    {
        if (resetPending)
            TryReset();
    }

    // Create a reset lock.  Outside subsystems that need, at times, to
    // lock the system against discretionary resets can acquire one of
    // these objects, and then set its status as needed to indicate when
    // it's safe to reboot.
    class Lock
    {
        friend class PicoReset;
        
    public:
        // Set/clear the locked state.  When set, the reset class won't
        // allow scheduled resets until all this lock (and all other
        // reset locks) are cleared.
        void SetLocked(bool locked);

        // get the current locked state
        bool IsLocked() const { return isLocked; }

    protected:
        // are we currently locked?
        bool isLocked = false;
        
        // for use by the parent class only
        Lock() { }
    };
    Lock *CreateLock();

protected:
    // Try performing a reset.  If no reset locks are in effect,
    // this carries out the pending reset immediately.
    void TryReset();
    
    // is a reboot pending?
    bool resetPending = false;

    // reset mode of pending request - boot into the Pico's ROM boot
    // loader mode if true, or into the current flash-resident user
    // program if false
    bool bootLoaderMode = false;

    // list of active reset locks
    std::list<Lock*> locks;
};

// global singleton instance
extern PicoReset picoReset;
