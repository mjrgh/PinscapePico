// Pinscape Pico - USB Vendor Interface operations
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class handles operations for our USB Vendor Interface, which
// provides configuration and control functions.  Applications on the
// host PC access the vendor interface through the host operating
// system's "generic vendor class driver" - typically WinUsb on Windows
// and libusb on Linux.
//
// For the USB wire protocol that we use for this interface, refer to
// ../USBProtocol/VendorIfcProtocol.h.

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include "../../Firmware/Pinscape.h"
#include "../USBProtocol.h"

// forwards/externals
class JSONParser;

// Vendor interface handler object
class VendorIfc
{
public:
    VendorIfc();

    // Initialize.  This must be called after USB initialization so that we can
    // take over the USB IRQ.
    void Init();

    // Device Interface GUID for our WinUSB vendor interface.  This is a private
    // GUID, created for this project, that uniquely identifies our WinUSB device
    // driver interface on the Windows host side.  Client software (for example,
    // our Configuration Manager program) uses this identifier to select the
    // appropriate device driver to connect to our device over the WinUSB
    // interface.
    //
    // This GUID is private to this project - it doesn't refer to any Microsoft
    // object, but rather uniquely identifies this device for the purposes of
    // WinUSB connections.  Forks of the project that modify the WinUSB protocol
    // part of the project should choose new GUIDs (by randomly rolling up a new
    // GUID using a GUID generator) so that their clients will be able to
    // distinguish their modified device interfaces from this version.
    //
    // (WinUSB is the Windows generic USB driver for devices that don't use any
    // of the standard USB class drivers [HID, CDC, etc] and instead define their
    // own custom wire protocols.  WinUSB interfaces on the device side are often
    // called "Vendor" interfaces because they're defined by the device's maker
    // rather than conforming to one of the standard interfaces.  In general,
    // a device with a vendor interface requires a custom device driver on the
    // Windows side, because by definition these devices don't work with any of
    // the standard class drivers that are built in to Windows.  In many cases,
    // the vendor satisfies this custom device driver requirement by writing a
    // brand new device driver, which must be installed on Windows in order to
    // use the device.  WinUSB provides a hybrid of "custom" and "built-in":
    // WinUSB is a built-in driver that comes with Windows, directly exposing
    // a device's low-level data connection to user-mode client programs.  The
    // work that the custom device driver would normally perform or parsing the
    // wire protocol can thus be shifted up to the client program, eliminating
    // the need for the vendor to provide a kernel-mode driver.  This is easier
    // for the vendor, since kernel-mode drivers are inherently more difficult
    // to write than regular application programs, and it's easier for the user,
    // since there's no need to install any new Windows device drivers.  Any
    // device can opt in to using WinUSB by providing some special descriptor
    // data during the device's USB connection setup.  We use a WinUSB interface
    // for our control interface, which doesn't fit into any of the standard
    // USB device classes.  Note that we *also* expose some standard USB
    // class interfaces, specifically HID and CDC.  But we also need a custom
    // interface for our unique functions that don't fit into any standard
    // USB device classes.)
    static const char *WINUSB_GUID;

    // Current vendor interface protocol state
    enum class ProtocolState
    {
        Ready,    // ready for new command input
        XferIn,   // awaiting request data transfer in
        XferOut,  // awaiting completion of request data transfer out
        Replying, // awaiting completion of reply send
    };
    ProtocolState protocolState = ProtocolState::Ready;

    // short-hand names for the ButtonLatencyTester2 namespace objects
    using Request = ButtonLatencyTester2::VendorRequest;
    using Response = ButtonLatencyTester2::VendorResponse;

    // Current request, if any.  When a request has additional host-to-device
    // data, we stash the pending request here until the additional data
    // transfer completes.
    Request curRequest;

    // Previous request type and count, for logging.  This lets us consolidate
    // logging for repeated requests to avoid excessive reporting.
    uint8_t prvRequestCmd = 0;
    uint8_t prvRequestCnt = 0;

    // Current request time.  This is the time (in system clock microseconds)
    // that the we received the request in curRequest.  We use this to cancel
    // the request if it doesn't complete within a timeout period, so that the
    // pipe doesn't get stuck if the host fails to complete the additional
    // incoming data transfer for a request.
    //
    // This reflects the time that we actually started servicing the request
    // in user mode.  This might be somewhat later than the actual hardware
    // arrival time, since tinyusb's IRQ handler only queues incoming packets
    // for the client to handle in user mode.
    uint64_t curRequestTime = 0;

    // request timeout, in microseconds
    static const uint64_t REQUEST_TIMEOUT = 250000;

    // USB hardware frame counter, and time of last frame counter
    // change.  The host maintains an 11-bit frame counter, which is
    // incremented every 1ms exactly.  On each 1ms frame boundary, the
    // host sends an SOF (Start Of Frame) packet with the new frame
    // number.  The host can correlate frame boundaries with the host
    // system clock to order of microsecond precision, and we can do the
    // same thing with the Pico clock by way of the USB IRQ, which fires
    // on every SOF packet received.  The frame counter is shared across
    // the connection, so knowing the microsecond time on the host clock
    // and the Pico clock for a given frame number lets us synchronize
    // the clocks to high precision.  Note that we don't need to know
    // EVERY frame number, just a single RECENT frame number, because we
    // can count on the frames starting at precise 1000us intervals.  If
    // the host tells us that frame Fw has host timestamp Tw, and we
    // know that frame F' has Pico timestamp Tp, we know that the time
    // between frames F and F' is 1000us * (F - F'), so we can determine
    // that the Pico clock at frame F was Tp+1000us*(F - F').  Thus we
    // know that Tw on the host clock equals Tp+1000us(F - F') on the
    // Pico clock, which gives us the offset between the two clocks and
    // thus allows us to calculate the Pico clock time for any given
    // host clock time.
    volatile uint16_t usbFrameCounter = 0;
    volatile uint64_t tUsbFrameCounter = 0;

    // USB IRQ handler
    static void USBIRQ();

    // Calculate the Pico timestamp for a host timestamp, given the USB
    // hardware frame number on the host and the elapsed time in
    // microseconds between the host-side event timestamp and the
    // host-side SOF timestamp.
    //
    // hostTimeToSof = T[event] - T[SOF], where T[event] is the host
    // timestamp of the event, and T[SOF] is the host timestamp of the
    // SOF signal for the given USB hardware frame number.  The interval
    // is in microseconds, and it's a signed value because the event
    // time could have occurred before or after the SOF.
    uint64_t HostTimeToPicoTime(int32_t hostTimeToSof, uint16_t hostFrameNum, uint64_t hostSofTimestamp);

    // Buffer for host-to-device and device-to-host data transfers
    struct XferBuf
    {
        // buffer for the transfer bytes
        uint8_t data[4096];

        // size of current contents
        uint16_t len = 0;

        // clear the data
        void Clear() { len = 0; }

        // append data
        void Append(const uint8_t *src, size_t copylen);
    };
    XferBuf xferIn;    // host to device transfer
    XferBuf xferOut;   // device to host transfer

    // copy a transfer out
    const uint8_t *SetXferOut(Response &resp, const void *data, size_t nBytes)
    {
        memcpy(xferOut.data, data, nBytes);
        resp.xferBytes = xferOut.len = nBytes;
        return xferOut.data;
    }
    
    // Deferred reboot subcommand.  This is one of the Request::SUBCMD_RESET_xxx
    // codes for a reboot command.  These commands are processed asynchronously
    // to allow time to send the USB reply back to the host and to allow for an
    // orderly shutdown of the output controller.
    uint8_t deferredRebootSubcmd = 0;

    // System clock time for processing pending reboot.  Set this initially to
    // "infinity" to indicate that no reboot is currently pending.
    uint64_t deferredRebootTime = UINT64_MAX;

    // periodic task processing
    void Task();

    // handle incoming host output (host to device)
    void OnOutput(const uint8_t *buf, size_t len);

    // Try processing the current request.  If we've received all of the
    // transfer data for the request, we can execute it, otherwise we have
    // to wait for the incoming transfer to complete.
    void TryProcessRequest();

    // Process the current request.  This routine assumes that all host-to-device
    // transfer data has been received.
    void ProcessRequest();

    // Clear the current request.  This resets our protocol state to
    // awaiting a new request.
    void ClearCurRequest();

    // Translate __DATE__ and __TIME__ into our YYYYMMDDhhmm timestamp format.
    // Fills a 12-character buffer, with no trailing null terminator.
    void GetBuildTimestamp(char buf[12]);
};

// global singleton
extern VendorIfc vendorIfc;
