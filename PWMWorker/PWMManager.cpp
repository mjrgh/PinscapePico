// Pinscape Pico - PWM Worker Program - PWM GPIO manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/pio.h>
#include <hardware/watchdog.h>
#include "Util.h"
#include "Logger.h"
#include "PWM_pio.pio.h"
#include "PWMManager.h"

// set up a GPIO as a PWM output
void PWMManager::InitGPIO(int gp)
{
    // validate the GPIO
    if (gp < 0 || gp > 31)
    {
        Log(LOG_ERROR, "InitGPIO: Invalid GPIO #%d\n", gp);
        return;
    }

    // make sure this same GP isn't already configured as PWM
    auto &gc = gpioConfig[gp];
    if (gc.slice != -1 || gc.smIndex != -1)
    {
        Log(LOG_ERROR, "InitGPIO: GP%d is already configured\n", gp);
        return;
    }

    // initialize the GPIO as a plain SIO input, so that it remains
    // in high-impedance state until outputs are enabled
    gpio_init(gp);
    gpio_set_function(gp, GPIO_FUNC_SIO);
    gpio_set_dir(gp, GPIO_IN);
    
    // Get the PWM slice and channel corresponding to the GPIO
    int slice = pwm_gpio_to_slice_num(gp);
    int ch = pwm_gpio_to_channel(gp);

    // Check if this slot is already in use.  Each PWM channel is tied
    // to two fixed GPIO ports, so if the other GPIO connected to this
    // channel has already been configured as a PWM out, we can't use
    // the channel for the new GPIO.
    auto &sc = sliceConfig[slice];
    auto &cc = sc.channelConfig[ch];
    if (cc.gp >= 0)
    {
        // This native PWM channel is already reserved, so we need to
        // set up on a PIO instead.
        if (!AssignPIO(gp))
        {
            Log(LOG_ERROR, "InitGPIO: can't assign GP%d as PWM because PWM%d-%c is in use (on GP%d) and no PIO resources are available\n",
                gp, slice, ch == 0 ? 'A' : 'B', cc.gp);
            return;
        }
    }
    else
    {
        // the native PWM channel is available - claim it
        cc.gp = gp;
        gc.slice = slice;
        gc.channel = ch;

        // if the slice doesn't already have a frequency set, use the
        // global default
        if (sc.freq == -1)
        {
            // set the default frequency
            sc.freq = defaultFreq;

            // configure the slice
            sc.ApplyFreq(this);
        }

        // set the duty cycle to 0 and enable the slice
        pwm_set_chan_level(slice, ch, 0);
        pwm_set_enabled(slice, true);
    }
}

// enable/disable all outputs
void PWMManager::EnableOutputs(bool enable)
{
    // update all of the slices
    for (int i = 0 ; i < _countof(sliceConfig) ; ++i)
        sliceConfig[i].EnableOutputs(enable);

    // update all of the PIO programs
    for (int i = 0 ; i < _countof(smConfig) ; ++i)
        smConfig[i].EnableOutput(enable);
}

// set the level for a GPIO
void PWMManager::SetLevel(int gp, float level)
{
    // make sure it's in range
    if (gp < 0 || gp > 31)
        return;

    // set the level according to the pin configuration
    auto &gc = gpioConfig[gp];
    if (gc.slice != -1)
    {
        // It's on a native PWM controller channel.  Set the level in
        // the PWM hardware unit.
        auto &sc = sliceConfig[gc.slice];
        auto &cc = sc.channelConfig[gc.channel];
        if (cc.level != level)
        {
            pwm_set_chan_level(gc.slice, gc.channel, static_cast<int>(roundf(level * sc.wrap)));
            cc.level = level;
        }
    }
    else if (gc.smIndex != -1)
    {
        // It's on a PIO state machine running the PWM program.  Set
        // the level by sending it to the PIO TX FIFO.
        auto &sc = smConfig[gc.smIndex];
        if (sc.level != level)
        {
            pio_sm_put_blocking(sc.pio, sc.sm, static_cast<uint16_t>(roundf(level * sc.wrap)));
            sc.level = level;
        }
    }
}

// get the current level setting for a GPIO
float PWMManager::GetLevel(int gp) const
{
    // make sure it's in range
    if (gp < 0 || gp > 31)
        return 0.0f;

    // set the level according to the pin configuration
    auto &gc = gpioConfig[gp];
    if (gc.slice != -1)
    {
        // It's on a native PWM controller channel.  Get the level on
        // the PWM hardware unit.
        auto &sc = sliceConfig[gc.slice];
        auto &cc = sc.channelConfig[gc.channel];
        return cc.level;
    }
    else if (gc.smIndex != -1)
    {
        // It's on a PIO state machine running the PWM program.  Set
        // the level by sending it to the PIO TX FIFO.
        auto &sc = smConfig[gc.smIndex];
        return sc.level;
    }
    else
    {
        // not configured as a PWM manager output - return 0
        return 0.0f;
    }
}

// set the frequency for a GPIO
void PWMManager::SetFreq(int frequency)
{
    // update all of the slices
    for (int i = 0 ; i < _countof(sliceConfig) ; ++i)
    {
        // check for a change on this slice
        auto &sc = sliceConfig[i];
        if (sc.freq != frequency)
        {
            // store the new frequency, and update the PWM unit
            sc.freq = frequency;
            sc.ApplyFreq(this);

            // still alive
            watchdog_update();
        }
    }

    // update all of the PIO programs
    for (int i = 0 ; i < _countof(smConfig) ; ++i)
    {
        // check for a change for this state machine
        auto &sm = smConfig[i];
        if (sm.freq != frequency)
        {
            // record the new frequency, and update the PIO program
            sm.freq = frequency;
            sm.ApplyFreq(this);

            // still alive
            watchdog_update();
        }
    }
}

// assign a PIO
bool PWMManager::AssignPIO(int gp)
{
    // find a free GPIO-to-PIO association entry
    int idx = 0;
    for ( ; idx < _countof(smConfig) && smConfig[idx].gp != -1 ; ++idx) ;
    if (idx >= _countof(smConfig))
    {
        Log(LOG_ERROR, "PWM setup for GP%d requires PIO assignment, but all PIOs are already in use\n", gp);
        return false;
    }

    // Try assigning a PIO state machine on an existing unit where we've
    // already loaded the program
    if (TryAssignPIO(gp, idx, 0, false)
        || TryAssignPIO(gp, idx, 1, false))
        return true;

    // No luck there, so either we haven't loaded the program into either
    // unit yet, or we're out of state machines on the unit(s) where we've
    // loaded the program.  Try again, this time loading the program (if
    // possible) into any unit where it hasn't been loaded yet.  This
    // might open access to more state machines.
    if (TryAssignPIO(gp, idx, 0, true)
        || TryAssignPIO(gp, idx, 1, true))
        return true;

    // We're out of things to try
    Log(LOG_ERROR, "PWM setup for GP%d requires PIO assignment, but no PIO state machines are available\n", gp);
    return false;
}

// assign a PIO
bool PWMManager::TryAssignPIO(int gp, int smConfigIndex, int unit, bool load)
{
    // check if the program has been loaded into this PIO
    int sm = -1;
    auto &pcfg = pioConfig[unit];
    PIO pio = pcfg.pio;
    if (pio == nullptr)
    {
        // It hasn't been loaded yet.  If we're not allowed to load the
        // program, silently fail to let the caller know that it should
        // look elsewhere, or try again with loading enabled.  The
        // caller always tries first to find a unit where the program
        // has already been loaded, to minimize our PIO resource usage.
        if (!load)
            return false;

        // get the PIO unit
        pio = unit == 0 ? pio0 : pio1;

        // check if there's space to load the program
        if (!pio_can_add_program(pio, &pwm_program))
            return false;

        // Before actually loading the program, make sure that we'll
        // be able to allocate a state machine, since otherwise there's
        // no point in tying up the instruction space on a unit we
        // won't be able to use anyway.
        if ((sm = pio_claim_unused_sm(pio, false)) < 0)
            return false;

        // there's room for the program and there's an available SM,
        // so go ahead and load it
        pcfg.offset = pio_add_program(pio, &pwm_program);

        // we're now established on this PIO - set the hardware pointer
        // in our internal configuration struct
        pcfg.pio = pio;
    }
    else
    {
        // the program is already loaded on this PIO, so the only thing
        // we need to allocate is a new state machine on the same PIO
        pio = pioConfig[unit].pio;
        if ((sm = pio_claim_unused_sm(pio, false)) < 0)
            return false;
    }

    // success - set up the state machine configuration slot
    auto &sc = smConfig[smConfigIndex];
    sc.gp = gp;
    sc.pio = pio;
    sc.pioUnit = unit;
    sc.sm = sm;

    // set up the GPIO configuration slot
    auto &gc = gpioConfig[gp];
    gc.slice = -1;
    gc.channel = -1;
    gc.smIndex = smConfigIndex;

    // configure the GPIO program
    pio_sm_set_consecutive_pindirs(pio, sm, gp, 1, true);
    pio_sm_config cfg = pwm_program_get_default_config(pcfg.offset);
    sm_config_set_sideset_pins(&cfg, gp);
    pio_sm_init(pio, sm, pcfg.offset, &cfg);

    // set the frequency; this will also enable the state machine
    sc.freq = defaultFreq;
    sc.ApplyFreq(this);

    // success
    Log(LOG_INFO, "PWM for GP%d set up on PIO%d.%d\n", gp, unit, sm);
    return true;
}


// ---------------------------------------------------------------------------
//
// Native PWM hardware interface
//

// apply the current frequency and step settings to the hardware unit
void PWMManager::SliceConfig::ApplyFreq(PWMManager *pwmManager)
{
    // Remember the original wrap value
    uint16_t oldWrap = wrap;

    // Figure the clock divider and highest WRAP value.  The clock
    // divider sets the PWM counter clock in terms of the system clock.
    // The system clock is (usually) 125 MHz, and the divider can range
    // from 1 to 256, so we can have a counter clock from about 488 kHz
    // to 125 MHz.  That's the *counter* clock; a full PWM cycle is WRAP
    // counter clocks long, so the PWM frequency is the system clock
    // divided by the product of the clock divider and (WRAP+1).  The
    // WRAP value is a uint16_t, so it can range from 0 to 65535.  High
    // frequencies are incompatible with high WRAP values because the
    // product will exceed the system clock, which would require a clock
    // divider less then 1, which isn't allowed.  We'd like as high a
    // WRAP value as possible, so figure it as follows:
    //
    // - Figure the clock divider based on the maximum wrap value.
    //
    // - If that yields a value in the valid range, use it and stop.
    //
    // - Otherwise, figure the WRAP value for the highest possible
    // clock speed - that is, with clock divider set to 1.
    //
    float sysClock = static_cast<float>(clock_get_hz(clk_sys));
    float divider = sysClock / (static_cast<float>(freq) * 65536.0f);
    wrap = 65535;
    if (divider < 1.0f)
    {
        // The frequency is too high to use the maximum WRAP value.
        // Calculate the maximum WRAP value that we can use at the
        // maximum counter frequency (i.e., with the clock divider
        // set to 1.0).
        divider = 1.0;
        wrap = static_cast<uint16_t>(ceilf(sysClock / freq) - 1);

        // The minimum wrap value is 1
        if (wrap < 1)
            wrap = 1;
    }
    else if (divider > 255.9375f)
    {
        // The frequency is too low, even with the maximum WRAP value -
        // the limit inherent in the hardware design is about 7 Hz with
        // the standard Pico 125MHz system clock.  There's nothing we
        // can do to make the physical PWM unit go slower than this, so
        // we can't satisfiy the caller's request.  Simply set the
        // slowest available speed, with WRAP and the clock divide both
        // maxed out, and give up.  But at least log an error, in case
        // the lower speed is actually imporrtant to the user: if it's
        // important, then they should notice the timing error, and
        // hopefully that'll lead them to look at the log to see what's
        // going on, where they'll find the error message.
        //
        // It would be possible to implement slower speeds by some other
        // mechanism.  One possibility would be a system timer interupt.
        // At 7 Hz cycle time and 256 steps, we'd only need about 550us
        // timer resolution, and system timers have 1us resolution.  We
        // could easily set up a single system timer that handles all
        // ultra-slow PWM channels by figuring the time until the next
        // transition and setting an interrupt, setting up again on each
        // interrupt.  But I don't think it's worth the trouble, since I
        // can't think of any serious use cases for such ultra-slow PWM
        // in a virtual pinball context.  If a use case does pop up at
        // some point, we can always add something later.
        divider = 255.9375f;
        Log(LOG_ERROR, "PWM frequency %d Hz is too low; minimum is 8 Hz\n", freq);
    }

    // disable the PWM slice while making changes
    pwm_set_enabled(slice, false);

    // update the clock divider and wrap value
    pwm_set_clkdiv(slice, divider);
    pwm_set_wrap(slice, wrap);

    // If the WRAP value changed, update the level settings in the
    // hardware units to rescale them to the new cycle length.
    if (wrap != oldWrap)
    {
        // Rescale the level on both channels.  Note that we don't
        // bother checking to see if the channels are assigned to GPIOs,
        // since it does no harm to update the level if they're not;
        // it's just a register update.
        pwm_set_chan_level(slice, 0, static_cast<uint16_t>(roundf(channelConfig[0].level * wrap)));
        pwm_set_chan_level(slice, 1, static_cast<uint16_t>(roundf(channelConfig[1].level * wrap)));
    }

    // re-enable the slice
    pwm_set_enabled(slice, true);
}

void PWMManager::SliceConfig::EnableOutputs(bool enable)
{
    channelConfig[0].EnableOutput(enable);
    channelConfig[1].EnableOutput(enable);
}

void PWMManager::SliceConfig::ChannelConfig::EnableOutput(bool enable)
{
    if (gp >= 0)
    {
        if (enable)
        {
            // enable the GPIO by setting it an output in PWM mode
            gpio_set_dir(gp, GPIO_OUT);
            gpio_set_function(gp, GPIO_FUNC_PWM);
        }
        else
        {
            // disable the GPIO by setting it as an SIO in INPUT mode
            gpio_set_function(gp, GPIO_FUNC_SIO);
            gpio_set_dir(gp, GPIO_IN);
        }
    }
}

// ---------------------------------------------------------------------------
//
// PIO interface
//

void PWMManager::SMConfig::ApplyFreq(PWMManager *pwmManager)
{
    // do nothing if there's no PIO
    if (pio == nullptr || sm < 0)
        return;

    // disable the PIO
    pio_sm_set_enabled(pio, sm, false);

    // Figure the new state machine clock divider.  Each overall PWM
    // cycle consists of three instructions at the top of the loop, plus
    // the loop.  The loop is executed once per resolution step, and
    // consists of two instructions per iteration.  Start with the
    // maximum wrap value; we'll reduce this later as needed.
    wrap = 65535;
    int opsPerCycle = ((wrap + 1) * 2) + 2;
    int pioClockHz = freq * opsPerCycle;
    float sysClockHz = static_cast<float>(clock_get_hz(clk_sys));
    float divider = sysClockHz / static_cast<float>(pioClockHz);
    if (divider < 1.0f)
    {
        // The divider is too small, which means that WRAP value is too
        // large.  Refigure the wrap value with the smallest possible
        // divider value of 1.0.
        divider = 1.0f;
        wrap = static_cast<uint16_t>(ceilf(((sysClockHz / static_cast<float>(freq)) - 2.0f) / 2.0f) - 1.0f);

        // the minimum wrap value is 1
        if (wrap < 1)
            wrap = 1;
    }
    else if (divider > 65535.99609375f)
    {
        // The frequency is set too low - peg it to the maximum divider
        // and log an error.  Note that the PIO implementation can go as
        // low as .01 Hz, but report the native PWM minimum of 8 Hz,
        // since we don't want to expose direct configuration controls
        // for selecting PIO vs native PWM.  As such, treat the nominal
        // valid range as the intersection of the physical ranges of PWM
        // and PIO, so that the user will always be okay staying within
        // the nominal range.
        divider = 65535.99609375f;
        Log(LOG_ERROR, "PWM frequency %d Hz is too low; minimum is 8 Hz\n", freq);
    }

    // set the new divider
    pio_sm_set_clkdiv(pio, sm, divider);
    Log(LOG_INFO, "PIO%d.%d freq=%d, divider=%.2f, wrap=%u\n", pioUnit, sm, freq, divider, wrap);

    // clear the FIFOs
    pio_sm_clear_fifos(pio, sm);

    // Load the period in steps into the state machine's ISR.  Write the
    // value to the TX FIFO, then execute a little synthesized program
    // on the SM:
    //
    //   PULL ifempty noblock    ; load OSR from FIFO
    //   OUT ISR, 32             ; shift 32 bits from OSR to ISR
    //
    pio_sm_put_blocking(pio, sm, wrap);
    pio_sm_exec(pio, sm, pio_encode_pull(false, false));
    pio_sm_exec(pio, sm, pio_encode_out(pio_isr, 32));

    // Reset the state machine to the top of the main loop.  If the
    // state machine was already running, this aborts the remainder of
    // its current loop, so that it immediately starts a new cycle with
    // the new parameters.  This seems like the least glitchy way to do
    // the transition, although even this might cause a brief flash or
    // dimming if the output is driving an LED, since the aborted cycle
    // will have the wrong duty cycle.  I don't think this will ever be
    // a practical problem in a Pinscape context, because I don't think
    // on-the-fly frequency changes will ever happen.  The only case I
    // can think of where a PWM output might change frequency at all
    // during a session is an IR remote control transmitter, which might
    // use different carrier frequencies for different codes, but in
    // this case the frequency change would always happen between
    // transmissions, when the output is fully off.
    pio_sm_exec(pio, sm, pio_encode_jmp(pwmManager->pioConfig[pioUnit].offset));

    // Write the new level value into the TX FIFO.  We have to set
    // a new level in case the WRAP value changed, since the level
    // in the state machine is scaled by the WRAP value.
    pio_sm_put_blocking(pio, sm, static_cast<uint16_t>(roundf(level * wrap)));

    // Re-enable the state machine
    pio_sm_set_enabled(pio, sm, true);
}

void PWMManager::SMConfig::EnableOutput(bool enable)
{
    if (gp >= 0)
    {
        if (enable)
        {
            // enable the output by connecting the pin to the PIO
            pio_gpio_init(pio, gp);
            gpio_set_input_enabled(gp, false);
        }
        else
        {
            // disable the GPIO by setting it as an SIO INPUT
            gpio_set_function(gp, GPIO_FUNC_SIO);
            gpio_set_dir(gp, GPIO_IN);
        }
    }
}
