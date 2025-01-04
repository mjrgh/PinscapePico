// Pinscape Pico - PWM Worker Program - PWM GPIO manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements up to 24 channels of PWM outputs, using the 16 native PWM
// channels plus 8 additional channels implemented via PIO state
// machines.  All channels are set to a common PWM frequency, which can
// be changed at any time.
//
// The frequency is global more for the sake of simplifying the user
// model than for the ease of implementation.  It's no problem
// technically to set the frequency on each PIO channel individually,
// and the native PWM channels can also be adjusted, but only in pairs,
// since the frequency is set at the "slice" level (two channels) rather
// than at the individual channel level.  That's the real reason we
// don't expose fine-grained frequency settings: the user would have to
// understand the paired-channel constraint, which will seem arbitrary
// and baffling unless you have a mental model of the PWM slice setup,
// which is a lot to ask a user to absorb.  I think everyone will be
// happier if we just say that the frequency setting is global, because
// most people shouldn't need to adjust the frequency at all, let alone
// on a channel-by-channel basis.  The default 20 kHz setting should be
// great for all device types.  It's high enough that it shouldn't cause
// coils and motors to buzz or whine, and low enough that it won't
// overwhelm optocouplers.  20 kHz yields 6250-step duty cycle
// resolution, which should yield very smooth fades with LEDs.

#pragma once
#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/pio.h>

class PWMManager
{
public:
    // Set up a GPIO pin as a PWM output.  This configures the GPIO and
    // allocates physical resources (either a native PWM channel or a
    // PIO state machine) to the pin.
    //
    // The channel is initially set to the default PWM cycle frequency
    // and step resolution.
    //
    // The physical GPIO port is left in high-impedance state, until
    // outputs are enabled with EnableOutputs().
    void InitGPIO(int gp);

    // Enable/disable all outputs
    void EnableOutputs(bool enable);

    // Set the PWM frequency, in Hertz.  This sets the frequency
    // globally across all ports.
    void SetFreq(int frequency);

    // Set the duty cycle on a port, as a fractional duty cycle, 0-1
    void SetLevel(int gp, float level);

    // Get the current level on a port
    float GetLevel(int gp) const;

protected:
    // Assign a GPIO to a PIO state machine running our PWM program.
    // The channel setup routines invoke this when they can't satisfy
    // the request via the GPIO's associated native PWM channel.
    bool AssignPIO(int gp);

    // Try assigning a GPIO to a specific PIO unit, optionally loading
    // the program if we haven't done so already.
    bool TryAssignPIO(int gp, int smConfigIndex, int unit, bool loadProgram);

    // Slice configurations
    struct SliceConfig
    {
        SliceConfig(int slice) : slice(slice) { }

        // hardware slice number
        int slice;

        // apply the current frequency and resolution settings to the
        // hardware unit
        void ApplyFreq(PWMManager *pwmManager);

        // Frequency setting currently in effect for the slice.  This is
        // initialized to the global default when the first channel on a
        // slice is allocated, and it's set to the last requested value
        // when a channel sets the frequency explicitly.
        int freq = -1;

        // WRAP value for the slice
        uint16_t wrap = 65535;

        // enable/disable outputs for the slice
        void EnableOutputs(bool enable);

        // Channel configuration.  Each native PWM slice controls
        // two channels.
        struct ChannelConfig
        {
            // GPIO claiming the channel
            int gp = -1;

            // Current level setting.  We store this separately from the
            // level set in the hardware so that we can rescale the
            // hardware setting for changes in the WRAP value after a
            // frequency change.
            float level = 0.0f;

            // enable/disable the GPIO
            void EnableOutput(bool enable);
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
        void ApplyFreq(PWMManager *pwmManager);

        // enable/disable the GPIO
        void EnableOutput(bool enable);

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

    // Default frequency setting
    int defaultFreq = 20000;
};
