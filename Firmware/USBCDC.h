// Pinscape Pico - USB CDC (virtual COM port)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Our USB interface includes a virtual COM port that we use primarily
// for logging human-readable status messages to the PC, for debugging
// and troubleshooting purposes.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "Logger.h"
#include "JSON.h"

// global isngleton
class USBCDC;
extern USBCDC usbcdc;

// USB CDC (virtual COM port)
class USBCDC
{
public:
    USBCDC();

    // Handle incoming CDC port output text (host-to-device)
    void OnOutput(const uint8_t *buf, size_t len);

    // configure the port from the serialPort.usb configuration item
    void Configure(const JSONParser::Value *val);

    // configure from fixed parameters; currently just enables the interface
    void Configure();

    // is the USB CDC port configured?
    bool IsConfigured() const { return configured; }

    // Is a terminal connected?  This returns true if TinyUSB indicates
    // that the host has set the virtual DTR (Data Terminal Ready) signal,
    // which host CDC drivers generally do when a client program is
    // connected to the virtual COM port.
    //
    // Note that we can query this directly from TinyUSB, since it also
    // tracks this state internally.  However, TinyUSB's conception of
    // it can get out of sync with the host's if a USB bus reset occurs,
    // because TinyUSB internally clears its memory of the state, while
    // the host's connection can continue across the reset without
    // interruption.  The host doesn't re-send the line state in this
    // case, so OUR version of the line state can more accurately reflect
    // the host's notion of the line state than TinyUSB's internal state
    // does.
    bool IsTerminalConnected() { return lineState.dtr; }

    // handle bus suspend/resume
    void OnSuspendResume(bool suspend);

    // Handle line state change notifications from TinyUSB
    void OnLineStateChange(bool dtr, bool rts);

protected:
    // USB CDC port configured
    bool configured = false;

    // Last line state sent from TinyUSB.
    struct LineState
    {
        bool dtr = false;       // Data Terminal Ready
        bool rts = false;       // Request To Send
    };
    LineState lineState;
};
