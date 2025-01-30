// Pinscape Pico - USB CDC (virtual COM port)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Our USB interface includes a virtual COM port that we use primarily
// for logging human-readable status messages to the PC, for debugging
// and troubleshooting purposes.

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <malloc.h>
#include <vector>

// Pico SDK headers
#include <pico/stdlib.h>
#include <class/cdc/cdc_device.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "Main.h"
#include "Logger.h"
#include "USBIfc.h"
#include "USBCDC.h"

// global singleton
USBCDC usbcdc;

USBCDC::USBCDC()
{
}

// Configure the port
void USBCDC::Configure(const JSONParser::Value *val)
{
    // There's nothing to configure at the moment beyond the existence
    // of the port, which means that we include the CDC interface in the
    // collection of USB interfaces we report to the host during
    // connection setup.
    Configure();
}

void USBCDC::Configure()
{
    // mark the interface as configured
    configured = true;
}

// Handle output CDC data (host-to-device).  (The term "output" is a
// little confusing, because it's incoming data on our side - but the
// USB naming convention is ALWAYS from the HOST's perspective, so
// host-to-device messages are always OUTPUT.)
void USBCDC::OnOutput(const uint8_t *buf, size_t len)
{
    // send the data to our associated logger's console
    usbCdcLogger.console.ProcessInputStr(reinterpret_cast<const char*>(buf), len);
}

// handle bus suspend/resume
void USBCDC::OnSuspendResume(bool suspend)
{
    // No action is currently required.  I added this to try to work
    // around a problem that occurs in the Windows CDC driver if the
    // host sends a USB Bus Reset signal shortly after coming out of
    // suspend mode, which will happen if the XInput interface is
    // active (because the Windows-side XInput device driver triggers
    // the bus reset on resume).  In particular, the Windows CDC driver
    // has a known bug where it stalls the CDC bulk IN endpoint if a
    // bus reset occurs while an application connoection is open on
    // the virtual COM port.  The problem isn't actually caused by
    // the suspend/resume directly, but rather by the bus reset that
    // the Windows XInput driver does on resume.  But TinyUSB doesn't
    // give us any callback notice on Bus Reset, and even if it did,
    // it would probably be too late to apply a workaround.  My
    // thinking was that we might be able to work around the problem
    // by ensuring that no packets were in flight when the bus reset
    // occurred, hence we should suppress IN traffic from the time
    // a SUSPEND event occurs until some heuristic delay time after
    // the corresponding RESUME, to out-wait the bus reset that the
    // XInput driver initiates on resume.  But this doesn't seem to
    // help; the IN stall seems to occur whether or not a packet is
    // actually being sent when the reset occurs.  I haven't found
    // anything that helps, so there's nothing to do here, but I'm
    // leaving the event point in place in case anyone wants to
    // experiment further or if a workaround becomes available in
    // the future.
}

// TinyUSB CDC event callback for line state changes
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    usbcdc.OnLineStateChange(dtr, rts);
}

void USBCDC::OnLineStateChange(bool dtr, bool rts)
{
    // remember the new state
    lineState.dtr = dtr;
    lineState.rts = rts;
}
