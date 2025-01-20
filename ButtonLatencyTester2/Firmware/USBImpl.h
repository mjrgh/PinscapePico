// Pinscape Pico Button Latency Tester II - USB implementation
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is the low-level USB implementation, based on Tinyusb.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>

// Tinyusb headers
#include <tusb.h>
#include <bsp/board.h>

// USB implementation class
class USBImpl
{
public:
    USBImpl();

    // initialize
    void Init(uint16_t vid, uint16_t pid);

    // periodic tasks
    void Task();

    // Device USB version.  This is meant to reflect changes to the USB
    // interface that might be significant to the host, particularly
    // changes that would require the host to reload any descriptors it
    // had previously cached.  We include this in the version string,
    // because Windows (and perhaps this is common among other hosts as
    // well) seems to use the serial number as a primary key for cached
    // descriptor data; changing the serial number thus organically
    // forces a reload of the descriptors.  This doesn't need to be
    // updated just because the firmware itself was updated; it's meant
    // to be specific to the USB interface structure.
    static const uint8_t DEVICE_USB_VERSION = 0x01;

    // Interface numbers
    static const uint8_t IFCNUM_CDC = 0;       // note - CDC counts as two interfaces (data + notification)
    static const uint8_t IFCNUM_VENDOR = 2;    // Pinscape vendor interface, for our control & configuration interface

    // Vendor device request codes, for BOS descriptor requests
    static const uint8_t VENDOR_REQUEST_WINUSB = 1;

    // Endpoints
    static const uint8_t EndpointOutCDC = 0x01;     // CDC data out
    static const uint8_t EndpointInCDC = 0x81;      // CDC data in
    static const uint8_t EndpointNotifyCDC = 0x82;  // CDC notification in
    static const uint8_t EndpointOutVendor = 0x03;  // Vendor interface (WinUSB) data out
    static const uint8_t EndpointInVendor = 0x83;   // Vendor interface (WinUSB) data in

    // Vendor interface instance numbers.  These are the indices of our respective
    // vencor interfaces in Tinyusb's internal list.  Note that these are NOT the
    // same as the USB interface IDs; these are just the internal array indices
    // that tusb assigns, starting at 0, in the order in which the interfaces are
    // listed in the configuration descriptor.  These must be kept in sync with
    // the configuration descriptor.
    static const int VENDORIFC_IDX_CONFIG = 0;

    // Get a string descriptor
    const uint16_t *GetStringDescriptor(uint8_t index, uint16_t langId);

    // Get the USB device descriptor
    const uint8_t *GetDeviceDescriptor();

    // Get the USB configuration descriptor
    const uint8_t *GetConfigurationDescriptor(int index);

    // Get the MS OS 2.0 WinUSB descriptor
    const uint8_t *GetMSOSDescriptor(size_t *byteSize = nullptr);

    // Get the USB BOS descriptor
    const uint8_t *GetBOSDescriptor();

    // has the device EVER been mounted (since the last reset)?
    bool WasEverMounted() const { return everMounted; }

    // is the device mounted (connected to host)?
    bool IsMounted() const { return mounted; }

    // active connection status
    bool IsConnectionActive() const { return mounted && !suspended; }

    // suspended status
    bool IsSuspended() const { return suspended; }

    // Handle a vendor-interface control transfer
    bool VendorControlXfer(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request);

    // device mount state change events
    void OnMountDevice() { mounted = everMounted = true; suspended = false; OnDeviceStateChanged(); }
    void OnDismountDevice() { mounted = false; OnDeviceStateChanged(); }

    // bus suspend/resume (low-power state) events
    void OnBusSuspended(bool remoteWakeEnabled);
    void OnBusResumed();

    // mounting status and/or bus suspend/resume state changed
    void OnDeviceStateChanged();

protected:
    // Add a string descriptor from ASCII text or from pre-formatted
    // binary data.  We call these internally during initialization
    // to populate the string descriptor table.  Each call adds
    // a string at the next available index.  For the text version,
    // we convert from ASCII to the Unicode wire format; for the byte
    // version, we copy the bytes exactly as submitted, so the buffer
    // must already be in the correct format to send on the wire.
    void AddStringDescriptorText(uint8_t index, const char *str);
    void AddStringDescriptorBytes(uint8_t index, const uint8_t *data, size_t nBytes);

    // device VID and PID
    uint16_t vid = 0;
    uint16_t pid = 0;

    // is the device mounted?
    bool mounted = false;

    // has the device EVER been mounted (since the last reset)?
    bool everMounted = false;

    // is the bus suspended?
    bool suspended = false;

    // remote wake enabled by host at last suspend
    bool remoteWakeEnabled = false;

    // String descriptor indices
    static const uint8_t STRDESC_LANG = 0;          // USB standard entry for language identifier code
    static const uint8_t STRDESC_MANUF = 1;         // USB standard entry for manufacturer name string
    static const uint8_t STRDESC_PRODUCT = 2;       // USB standard entry for product name string
    static const uint8_t STRDESC_SERIAL = 3;        // USB standard entry for device serial number string
    static const uint8_t STRDESC_VENIFC = 4;        // display name of our Configuration & Control vendor interface
    static const uint8_t STRDESC_CDCIFC = 5;        // display name for our CDC interface

    // String descriptors, keyed by string index.  The descriptors are stored
    // in the USB wire format: a two-byte descriptor prefix, followed by the text
    // of the string as little-Endian UCS-2 (16-bit Unicode) characters.  The
    // prefix looks like this:
    //
    //   byte [0] = length of the entire object, in bytes, including the two-byte prefix
    //   byte [1] = 0x03 (TUSB_DESC_STRING, HID desriptor type code for String)
    //
    std::unordered_map<uint8_t, std::vector<uint8_t>> stringDescriptors;

    // Configuration descriptor.  This is built on the first call to
    // GetConfigurationDescriptor() and then cached.  All HID interfaces must
    // be added and populated with their logical devices before the first call
    // to GetConfigurationDescriptor(), to ensure that everything is included
    // in the cached descriptor.
    std::unique_ptr<uint8_t> cfgDesc;

    // HID interface list
    // device descriptor
    tusb_desc_device_t deviceDescriptor;
};

// global singleton
extern USBImpl usbImpl;
