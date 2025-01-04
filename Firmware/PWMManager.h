// Pinscape Pico firmware - PWM manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Utilities for configuring the Pico's PWM controllers and managing
// the hardware resources.
//
// This class helps configure PWM outputs on GPIO pins.  The main thing
// it does beyond what's in the SDK is to detect and resolve conflicts
// in the PWM mappings.  PWM on the Pico has some complex limitations
// that aren't immediately apparent when you look at the pin-out
// diagram, which suggests that you can just use any old pin as a PWM
// output.  It's true that any pin can be used as a PWM output in
// isolation, but once a pin is assigned as a PWM out, it will affect
// which other pins can then be assigned as further PWM outs, and it
// will also affect what frequency and resolution settings are available
// on the other pins.  Understanding the specific constraints requires
// some detailed knowledge of the Pico's internals, which we can't
// reasonably expect end users to master.  So we need a way to detect
// conflicts when they occur, and ideally resolve them automatically, or
// in the worst case, provide troubleshooting guidance to the user to
// help them resolve the problem manually.
//
//
// BACKGROUND: There are two kinds of configuration conflicts that can
// arise due to the design of the Pico's built-in PWM controller
// hardware.
//
// - The first is conflicts between GPIO pins trying to use the same PWM
// channel, which can arise because each physical PWM channel on the
// Pico is mapped to two GPIO pins.  You can thus only use one of the
// two pins for a given channel as a PWM output at any given time.  For
// example, GP0 and GP16 are both connected to PWM0A, so if you
// configure GP0 as a PWM output, you can't use GP16 as PWM, and vice
// versa.
//
// - The second is conflicts between the configured frequency and
// resolution settings for two pins that share the same PWM slice.  A
// "slice" is Raspberry's term for a physical PWM unit, each of which
// has two channels.  Each channel's duty cycle can be independently
// specified, but the frequency and resolution are set in the slice, so
// the two channels on a slice share those settings.  For example, if
// you configure any one of GPIO0, GPIO1, GPIO16, or GPIO17 with a
// specific frequency and resolution, all of the other pins in that
// group are forced to use those same settings, because they're all
// controlled by the same slice (PWM0).  So you can't, for example, set
// GPIO0 to 40 kHz while at the same time setting GPIO1 to 200 Hz - each
// pin on PWM0 gets the same frequency and resolution as all of the
// others.
//
// There are two ways that we can resolve these conflicts.
//
// - The first is to take advantage of the fact that many PWM usages
// don't need specific frequency settings.  This is the case when PWM is
// used for LED dimming, motor speed control, solenoid strength
// modulation, and similar physical device controls.  These usages
// typically only need the PWM frequency to be high enough to avoid
// physical artifacts like LED flicker and acoustic noise from a motor
// or coil.  To take advantage of this inherent flexibility, the API
// lets a caller allocate a PWM channel without setting the frequency or
// resolution, which means that it can accept whatever settings that any
// other caller sharing the slice sets.
//
// - The second way we can resolve conflicts is to bypass the Pico's
// native PWM slices when necessary and use a PIO state machine instead.
// A PIO implementation allows a single GPIO to have its own completely
// independent settings.  We can put this into effect any time a caller
// requests settings that are incompatible with a channel's existing
// configuration.  This sounds so great that you'd think we should just
// it all the time!  But it has to be a last resort, because PIO state
// machines are an even more limited resource than independent native
// PWM channels.  The Pico only has eight PIOs, and some might be needed
// for other purposes besides PWM.  So we try to use native PWM whenever
// possible, and only fall back on PIO if the native PWM has a conflict.

#pragma once

// standard library headers
#include <stdlib.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/pio.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"

// external/forward declarations
class JSONParser;

// PWM manager
class PWMManager
{
public:
    PWMManager();

    // Configure the PWM manager.  This loads default settings for
    // PWM outputs that don't set specific timing parameters.
    void Configure(JSONParser &json);

    // Set up a GPIO pin as a PWM output.  This configures the GPIO and
    // allocates physical resources (either a native PWM channel or a
    // PIO state machine) to the pin.  Returns true on success; on
    // failure, logs an error and returns false.  The reason it's
    // possible for this to fail is that it allocates resources, and the
    // necessary resources might have already been exhausted by previous
    // callers.
    //
    // The channel is initially set to the default PWM cycle frequency
    // and step resolution.
    //
    // The subsystem name is a string with static storage duration that
    // identifies the caller's subsystem, for use in logged error
    // messages to help the user understand the source of the conflict.
    bool InitGPIO(const char *subsystemName, int gp);

    // Set a PWM GPIO pin's frequency, in Hertz.  The GPIO must have
    // been previously configured as a PWM output via InitPGIO().
    //
    // If the GPIO is currently being handled by its native PWM slice,
    // and the other GPIO using the same slice has already set a
    // different frequency, this will attempt to move the GPIO pin to a
    // PIO state machine so that it can set an independent frequency.
    // That can fail if insufficient PIO resources are available.
    // Returns true on success; on failure, logs an error and returns
    // false.
    bool SetFreq(int gp, int frequency);

    // Set the duty cycle, as a fraction from 0 to 1 inclusive.
    void SetLevel(int gp, float level);

    // Get the current level
    float GetLevel(int gp) const;

    // Is the given GPIO configured as a PWM output?  Note that this
    // only returns true if the GPIO was configured via the PWM Manager;
    // a pin configured directly via the hardware doesn't count here.
    bool IsConfigured(int gp) const { return gp >= 0 && gp < 32 && gpioConfig[gp].IsConfigured(); }

protected:
    // Assign a GPIO to a PIO state machine running our PWM program.
    // The channel setup routines invoke this when they can't satisfy
    // the request via the GPIO's associated native PWM channel.
    // Returns true on success; on failure, logs an error and returns
    // false.
    bool AssignPIO(const char *subsystemName, int gp);

    // Try assigning a GPIO to a specific PIO unit, optionally loading
    // the program if we haven't done so already.
    bool TryAssignPIO(const char *subsystemName, int gp, int smConfigIndex, int unit, bool loadProgram);

    // Slice configurations
    struct SliceConfig
    {
        SliceConfig(int slice) : slice(slice) { }

        // hardware slice number
        int slice;
        
        // apply the current frequency and resolution settings to the
        // hardware unit
        void ApplyFreq();

        // Frequency setting currently in effect for the slice.  This is
        // initialized to the global default when the first channel on a
        // slice is allocated, and it's set to the last requested value
        // when a channel sets the frequency explicitly.
        int freq = -1;

        // WRAP value for the slice
        uint16_t wrap = 65535;

        // Channel configuration.  Each native PWM slice controls
        // two channels.
        struct ChannelConfig
        {
            // name of subsystem that reserved the channel
            const char *reserver = nullptr;

            // GPIO claiming the channel
            int gp = -1;

            // Current level setting.  We store this separately from the
            // level set in the hardware so that we can rescale the
            // hardware setting for changes in the WRAP value after a
            // frequency change.
            float level = 0.0f;

            // Explicit frequency setting for this channel.  This is set
            // when the GPIO owner of the channel sets a specific
            // frequency.  When set, the other channel on the slice
            // isn't allowed to change the frequency, since that would
            // also change our frequency without our consent.  -1 means
            // that we're using the slice default, and will accept any
            // future changes the other channel wants to make.
            int freq = -1;
        };
        ChannelConfig channelConfig[2];
    };
    SliceConfig sliceConfig[8]{ 0, 1, 2, 3, 4, 5, 6, 7 };

    // PIO state machine configuration.  This records assignments of PIO
    // state machines to GPIO PWM outputs, which we use when the GPIO
    // can't be configured to use its native PWM unit due to a conflict
    // with previously reserved parameters on the unit.  We need at most
    // eight of these assignments, since we need one PIO state machine
    // per associated GPIO, and the Pico has 8 state machines total (two
    // PIO units, four state machines per unit)
    struct SMConfig
    {
        // Apply the current frequency setting to the PIO
        void ApplyFreq();

        // GPIO; -1 if unassigned
        int gp = -1;

        // PIO unit number
        int pioUnit = -1;
        
        // PIO hardware for the unit assigned to the GPIO
        PIO pio = nullptr;

        // PIO state machine assigned
        int sm = -1;

        // current level setting
        float level = 0.0f;

        // frequency setting
        int freq = -1;

        // total length of PWM cycle in counts
        uint16_t wrap = 65535;
    };
    SMConfig smConfig[8];

    // PIO state machine configuration.  This keeps track of the PIO
    // configuration for each unit where we load the PWM program.  We
    // can load the PWM program into one or both PIO units according to
    // how many individual state machines we need to assign.  The index
    // in this array corresponds to the physical PIO unit number.
    struct PIOConfig
    {
        // PIO hardware object; null if not loaded in this unit
        PIO pio = nullptr;
        
        // program load offset
        int offset = -1;
    };
    PIOConfig pioConfig[2];

    // GPIO assignments
    struct GPIOConfig
    {
        // PWM slice number and channel (0=A, 1=B). -1 means that the
        // pin isn't configured at all or is configured on a PIO state
        // machine instead of a native PWM controller.
        int slice = -1;
        int channel = -1;

        // State machine configuration index (in smConfig[]). -1 means
        // that the pin isn't configured at all or is configured on a
        // native PWM slice instead of a PIO.
        int smIndex = -1;

        // is this pin configured as a PWM output?
        bool IsConfigured() const { return slice != -1 || smIndex != -1; }
    };
    GPIOConfig gpioConfig[32];

    // Default frequency setting used for new ports.  This can be set by
    // the user via the JSON configuration.  The factory default is
    // chosen to be just above the human hearing range, which tends to
    // eliminate audible acoustic noise from solenoids and motors, and
    // is plenty fast enough to eliminate LED flicker.
    //
    // (Using PWM with motors and solenoids can make the mechanical
    // parts vibrate at the PWM frequency, which can be audible as
    // acoustic noise, especially at low duty cycles.  Since the
    // mechanical vibration is at the PWM frequency, raising the
    // frequency to at least 20 kHz tends to make it inaudible by
    // pushing it outside the human hearing range.  The noise is still
    // there, but only dogs can hear it; sorry, dogs.  LED flicker is
    // much less demanding; 200 Hz or above is will completely eliminate
    // it.)
    //
    // On the other hand, the default shouldn't be any higher than
    // necessary, because optocoupler-based output amplifiers have
    // relatively low limits on switching frequency, often less than 100
    // kHz.
    //
    // A value of 20kHz satisfies both requirements - it's above most
    // people's hearing range, and almost an order of magnitude below
    // the point where optocouplers might stop working.
    // 
    int defaultFreq = 20000;
};

// global singleton
extern PWMManager pwmManager;
