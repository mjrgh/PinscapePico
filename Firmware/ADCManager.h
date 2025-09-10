// Pinscape Pico - ADC management
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module provides a hardware abstraction interface for ADCs, designed
// to provide a generic interface that ADC input-consuming subsystems can
// use to access the ADC, and that can be implemented for different physical
// sensor types.  The idea is to make it possible for ADC input consumers to
// be written independently of physical ADC type, so that the user can
// choose a specific device in the configuration.  We provide an
// implementation for the Pico's on-board ADC.
//
// Some subsystems have special requirements that the generic interface
// can't meet, particularly the use of DMA or PIO to read from the device.
// When necessary, subsystems can program directly to the target device's
// hardware interface, bypassing the generic interface.  The generic
// interface's "Claim" mechanism should still be used in these cases, to
// flag configuration conflicts that try to access the ADC from other
// subsystems.
//
// Some ADCs accept differential or double-ended inputs, where the voltage
// quantized can vary over a negative and positive voltage range.  The
// generic interface represents this by allowing negative numbers in the
// readings.  If the device only accepts single-ended inputs at the hardware
// level, where the voltage quantized can range from 0V to a positive analog
// reference voltage, the reports should always use positive numbers.


#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <list>

#include <pico/stdlib.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"

// foward/external declrations
class ADCManager;
class ADC;

// ---------------------------------------------------------------------------
//
// ADC Manager.  This manages a global list of available ADC devices,
// so that subsystems can find devices during configuration.
//

// global singleton
extern ADCManager adcManager;

// ADC Manager class
class ADCManager
{
public:
    // Construction
    ADCManager();

    // Claim the Pico's on-board ADC for exclusive use by a subsystem.  The
    // subsystem name is recorded, and used in any error messages to help
    // the user identify the source of the conflict.
    //
    // The on-board ADC is uniquely "claimable" as an exclusive resource
    // because of its tight integration with the RP2040 DMA controller.  The
    // abstract ADC interface defined here doesn't have any provision for
    // DMA usage, because it's simply not applicable to outboard ADCs, so
    // any device driver that requires DMA mode has to program directly to
    // the ADC hardware.  That's the point of the "claim" mechanism: it
    // tells the ADC manager that the ADC can't be configured for general
    // access.  It also lets us generate troubleshooting messages in case
    // two or more device driver try to claim exclusive access.
    //
    // Returns true if the claim succeeds, false if another subsystem has
    // already claimed the device.  On failure, we'll log a message naming
    // the old and new claimants to help the user identify the source of the
    // conflict.
    bool ClaimPicoADC(const char *subsystemName);
    
    // Configure.  This configures the supported ADC device types.
    void Configure(JSONParser &json);

    // Add an ADC to the list.  Device implementations call this
    // during configuration to become discoverable by subsystem
    // configurations.  We take ownership of the object.
    void Add(ADC *adc);

    // iterate over the configured devices
    void Enumerate(std::function<void(ADC*)> callback);

    // Iterate over the configured devices by config key.  This enumerats
    // each channel of each device, under both the primary and alternate
    // config keys.
    //
    // Each channel is enumerated under its channel number using the
    // naming convention "key[n]", where 'n' is the channel number,
    // starting at zero.  The zeroeth channel is ALSO enumerated with no
    // '[0]' suffix, so that the user can refer to a single-channel
    // device without a channel number suffix, AND still allowing the
    // same notation for the first channel in the event the user later
    // reconfigures the device to use multiple channels.  (It could be
    // confusing for a user to have a plunger reference to 'pico_adc'
    // break just because they configured a second Pico ADC channel,
    // which would happen if we required the [n] suffix for multi-
    // channel devices but not for single-channel devices.  And it would
    // be annoying to require the [0] suffix for single-channel devices.
    // So we really have to accept 'key' and 'key[0]' as synonmous in
    // all cases to avoid the confusing edge case where we go from
    // single-channel to multi-channel after a config change.)
    //
    // All channel keys (including the [n] suffix keys and the default
    // key for channel 0) are repeated for the primary and alternate
    // keys, if the device has an alternate key.
    void EnumerateChannelsByConfigKey(std::function<void(const char *key, ADC *adc, int channelNum)> callback);

protected:
    // List of configured ADC devices
    std::list<std::unique_ptr<ADC>> adcs;

    // subsystem claiming on-board Pico ADC
    const char *picoAdcOwner = nullptr;
};

// ---------------------------------------------------------------------------
//
// Abstract ADC base class
//
// To accommodate a range of devices, we use a simple polling model.
// Once enabled, the device takes samples continuously at a selected
// rate, and the samples are available to the caller via polling.  We
// don't provide an IRQ model in the abstract interface, because some
// devices might not have interrupt hardware capabilities.  A concrete
// subclass can still use interrupts internally when the underlying
// device provides IRQ features, though (and it's generally better to
// take advantage of that when available, since interrupt-based access
// usually has lower latency and less CPU overhead).
//
class ADC
{
public:
    // Construction.  The strings must have static storage duration,
    // since we keep references to them.
    ADC(int32_t nativeMin, int32_t nativeMax, const char *configKey, const char *altConfigKey, const char *displayName) :
        nativeMin(nativeMin), nativeMax(nativeMax), configKey(configKey), altConfigKey(altConfigKey), displayName(displayName) { }

    // get the configuration key
    const char *ConfigKey() const { return configKey; }

    // Alternate configuration key.  For devices that allow multiple
    // instances to be configured, the first unit's config key is usually
    // the unardorned chip name, and additional units have names like
    // "ads1115_2", "ads1115_3", etc.  In these cases, we'd like the
    // first unit to go by TWO names - the plain base name that would
    // apply if only a single unit were configured ("ads1115"), AND the
    // numbered version when multiple units are present ("ads1115_0").
    // The alt key is how we provide this aliasing.  The point of the
    // alias is that the user doesn't have to know about the numbering
    // scheme in the common case where only one unit is present, and
    // doesn't have to change references to the single unit if they
    // add more units later (since the original first unit still
    // matches the base name with no number suffix), while also
    // maintaining consistency in the multi-unit case, by allowing
    // the numbering notation to be used for every unit.
    const char *AltConfigKey() const { return altConfigKey; }

    // get the display name for messages
    const char *DisplayName() const { return displayName; }

    // Start sampling on all configured channels.  Once enabled, the device
    // collects samples continuously for the rest of the session.  We don't
    // have a "disable" function so that the ADC can be shared among
    // readers without the readers having to coordinate their activity; for
    // that to work, the ADC has to be effectively read-only, so we can't
    // allow one reader to stop the ADC when another reader might still be
    // using it.
    virtual void EnableSampling() = 0;

    // Get the device's native scale limits.  For single-ended devices,
    // the minimum is always zero.
    virtual uint32_t GetNativeMin() const { return nativeMin; }
    virtual uint32_t GetNativeMax() const { return nativeMax; }

    // Get the number of logical channels configured for this device.  If
    // the ADC has multiple physical inputs that it can read from while in
    // continuous sampling mode, the driver can allow the configuration to
    // enable multiple channels.  The "logical" channel count reflects the
    // number of *configured* channels, not the number of physical channels.
    //
    // Some devices have multiple physical inputs, but can only read from
    // one channel while in continuous sampling mode (e.g., ADS1115).  When
    // that restriction applies, the driver can only support a single
    // logical channel, so this will report 1.
    virtual int GetNumLogicalChannels() const = 0;

    // Read the latest sample, in the device's native units.  The channel
    // is the *logical* channel number, which doesn't necessarily correspond
    // to any physical channel numbering or pin numbering in the chip.  This
    // is always zero for a device with only one configured channel.
    struct Sample
    {
        int32_t sample;
        uint64_t timestamp;
    };
    virtual Sample ReadNative(int channel) = 0;

    // Read a sample in normalized units, 0..65535.  Double-ended inputs
    // should be re-centered at +32767 so that all reported values fit in
    // the UINT16 range.
    //
    // Callers can do their own normalization based on GetNativeMin/Max, but
    // it's usually more efficient to do the calculation at the device
    // level, because most devices have fixed native scales that allow doing
    // the normalization calculation with constants and bit shifts.
    virtual Sample ReadNorm(int channel) = 0;

protected:
    // Native scale.  These give the mininum and maximum values that the
    // device reports at the hardware level.  For a device that only accepts
    // single-ended inputs (i.e., voltages between DC GND and an analog
    // reference voltage), the minimum must be zero.  A device that accepts
    // double-ended or differential inputs can report negative values, so
    // nativeMin will be negative.
    int32_t nativeMin;
    int32_t nativeMax;

    // Configuration key.  This is a string that we match to the 'type'
    // key in an subsystem's 'adc:' configuration.
    const char *configKey;

    // alternate config key
    const char *altConfigKey;

    // Display name, for use in logging messages
    const char *displayName;

    // The subsystem that owns this ADC; null if it's not claimed yet
    const char *owner = nullptr;
};

