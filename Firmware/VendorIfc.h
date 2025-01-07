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
#include "Pinscape.h"
#include "../USBProtocol/VendorIfcProtocol.h"

// forwards/externals
class JSONParser;

// Vendor interface handler object
class PinscapeVendorIfc : public IRReceiver::Subscriber
{
public:
    PinscapeVendorIfc();

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

    // short-hand names for the PinscapePico namespace objects
    using Request = PinscapePico::VendorRequest;
    using Response = PinscapePico::VendorResponse;

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

    // Enter Polling Mode flag.  This set by CMD_STATS + SUBCMD_STATUS_PREP_QUERY_CLOCK
    // to signal the polling loop that it should continue polling for new
    // input on our endpoint for a few milliseconds, because a clock query
    // command is expected immediately.
    bool enterPollingMode = false;

    // execute polling mode
    void RunPollingMode();

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

    // handle incoming host output (host to device) on our Pinscape Configuration
    // and Control vendor interface
    void OnOutput(const uint8_t *buf, size_t len);

    // Try processing the current request.  If we've received all of the
    // transfer data for the request, we can execute it, otherwise we have
    // to wait for the incoming transfer to complete.
    void TryProcessRequest();

    // Process the current request.  This routine assumes that all host-to-device
    // transfer data has been received.
    void ProcessRequest();

    // Process a PUT CONFIG request.  The host uses this request to send one
    // 4K section of a new config file for storage in flash.
    void ProcessPutConfig(Response &resp);

    // Process a GET CONFIG request.  Returns a pointer to the reply transfer
    // data, which is a pointer directly into the config file data in flash.
    const uint8_t *ProcessGetConfig(Response &resp);

    // Process a TEST CONFIG CHECKSUM request
    void ProcessTestConfig(Response &resp);

    // For PUT_CONFIG requests, the CRC-32 accumulator.  This tracks the
    // intermediate checksum value for the whole stream, updated as we
    // receive config pages.
    uint32_t putConfigChecksum = 0;

    // Previous PUT_CONFIG page number and checksum.  The caller must send
    // us the file's pages in consecutive page order, *except* that the
    // caller is allowed to repeat a page any number of times.  Repeats
    // are allowed because it's possible for our acknowledgment of a
    // successful page write to be lost in the USB transmission, so the
    // caller might not know that a page was actually written
    // successfully, and so might retry the operation.  We thus have to
    // keep track of the last page sent to compare against each new page.
    // The checksum lets us confirm that it's the same contents that we
    // received last time.
    int putConfigPrevPageNo = -1;
    uint32_t putConfigPrevPageChecksum = 0;

    // Time of last Put Config call.  We reset the internal counters if
    // the time between calls is too long, to ensure that old state from a
    // previous connection that didn't properly finish a Put Config
    // operation won't confuse things for a new session.
    uint64_t tPutConfig = 0;

    // Clear the current request.  This resets our protocol state to
    // awaiting a new request.
    void ClearCurRequest();

    // Translate __DATE__ and __TIME__ into our YYYYMMDDhhmm timestamp format.
    // Fills a 12-character buffer, with no trailing null terminator.
    void GetBuildTimestamp(char buf[12]);

    // IR Receiver Subscriber
    virtual void OnIRCommandReceived(const IRCommandReceived &command, uint64_t dt) override;
    virtual void OnIRPulseReceived(int time_us, bool mark) override;

    // Configure IR capture capabilities
    void ConfigureIR(JSONParser &json);

    // Query recent IR commands/pulses.  These populate the buffer with
    // codes/pulses for the CMD_IR_QUERY requests.
    size_t IRQueryCmd(uint8_t *buf, size_t bufsize);
    size_t IRQueryRaw(uint8_t *buf, size_t bufsize);

    // IR command buffer.  This is a circular buffer of recent IR commands
    // received.
    struct IRCmd
    {
        IRCommandReceived cmd;
        uint64_t dt;
    };
    static const int IRCMD_BUF_SIZE = 16;
    IRCmd irCmd[IRCMD_BUF_SIZE];

    // ring buffer pointers; write == read -> empty
    int irCmdRead = 0;
    int irCmdWrite = 0;

    // IR pulse buffer.  This is a linear buffer of recent IR pulses
    // received.  Unlike the command buffer, we don't run this in a circle;
    // instead, we stop when it's full, and we clear it automatically when
    // no pulses have been received in a while.
    //
    // We use the same compact encoding as the IRReceiver class: the low
    // bit is the mark/space indicator (1 = mark, 0 = space), and the high
    // 15 bits are the time in 2us units (i.e., time is us divided by 2).
    std::vector<uint16_t> irPulse;
    size_t irPulseWrite = 0;

    // Time of last IR pulse
    uint64_t tLastIRPulse = 0;
};

// global singleton
extern PinscapeVendorIfc psVendorIfc;

