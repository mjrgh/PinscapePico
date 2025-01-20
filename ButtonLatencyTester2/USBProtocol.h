// Pinscape Pico Button Latency Tester II - USB Vendor Interface Protocol
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file defines the protocol for the button latency tester's USB Vendor
// Interface, which provides the application interface to the device.
//
// A USB Vendor Interface is a custom application-specific protocol not
// based on any of the pre-defined USB interface classes, thus requiring
// a custom host-side driver.  We rely on the "generic" drivers provided
// on most operating systems, which provide user-space applications with
// direct access to the USB endpoints exposed by the device interface.
// On Windows, the native generic driver is WinUsb.  On Linux and other
// platforms, libusb provides similar access.  (libusb also works on
// Windows, but WinUsb is preferable because it provides plug-and-play
// installation, without any manual user action to install a device
// driver.  libusb requires manual device driver installation.)
// 
// This interface is based on Pinscape Pico's vendor interface.  It uses
// the same basic protocol and message structure, with a different
// command set (obviously) designed for this application.

#pragma once
#include <stdint.h>
#include "../USBProtocol/CompilerSpecific.h"

namespace ButtonLatencyTester2
{
    // Host-to-device request format.  The host sends this struct to
    // invoke a command on the device.
    struct __PackedBegin VendorRequest
    {
        VendorRequest()
        {
            cmd = CMD_NULL;
            checksum = 0;
            token = 0;
            memset(args.argBytes, 0, sizeof(args.argBytes));
        }

        VendorRequest(uint32_t token, uint8_t cmd, uint16_t xferBytes)
        { 
            this->cmd = cmd;
            this->token = token;
            this->checksum = ComputeChecksum(token, cmd, xferBytes);
            this->xferBytes = xferBytes;
            memset(args.argBytes, 0, sizeof(args.argBytes));
        }

        // Token.  This is an arbitrary 32-bit int supplied by the host
        // to identify the request.  It has no meaning to us other than
        // as an opaque ID for the request.  We echo this back in the
        // response, so that the host can correlate responses to requests.
        uint32_t token;

        // Checksum.  This is a simple integrity check on the packet,
        // to help ensure that the device doesn't attempt ot execute
        // ill-formed commands coming from unrelated applications.  Since
        // we use the generic WinUSB driver on Windows, any user-mode
        // application can send raw data over our endpoints.  (That's
        // the one major drawback of using WinUSB instead of a custom
        // device driver; a custom driver insulates the USB pipes from
        // direct access from application software, which helps ensure
        // that data on the wire is well-formed.)
        //
        // To compute the checksum, compute ~(token + cmd + xferBytes)
        // as a uint32_t, ignoring any overflow.
        uint32_t checksum;

        // figure the correct checksum for the given parameters
        static uint32_t ComputeChecksum(uint32_t token, uint8_t cmd, uint16_t xferBytes) {
            return ~(token + cmd + xferBytes);
        }

        // validate the stored checksum
        bool ValidateChecksum() const { return ComputeChecksum(token, cmd, xferBytes) == checksum; }

        // Command code
        uint8_t cmd;

        //
        // Command codes
        //

        // Null command - this can be used to represent an empty,
        // invalid, or uninitialized request structure.
        static const uint8_t CMD_NULL = 0x00;

        // Ping.  This can be used to test that the connection is working
        // and the device is responsive.  This takes no arguments, and
        // simply sends back an OK status with no other effects.
        static const uint8_t CMD_PING = 0x01;

        // Query version.  No arguments.  The reply reports the version
        // information via the args.version field.
        static const uint8_t CMD_QUERY_VERSION = 0x02;

        // Query the device's ID information.  No arguments.
        //
        // The extra transfer data consists of a series of null-terminated
        // strings, composed of single-byte characters, appended
        // back-to-back (so that the next string starts immediately after
        // the terminating null byte of the prior string):
        //
        // - Target board ID: the name string used in the Pico SDK to
        //   identify the target board in the firmware build process.
        //   The Pico is an open-source design with many clones, some
        //   compatible with the original reference design and some
        //   with different on-board peripherals.  Boards with custom
        //   hardware changes generally require the firmware to be
        //   rebuilt with different parameters.  That's what this ID
        //   indicates: the board configuration that the running
        //   firmware was built for.  This isn't a "live" board ID;
        //   it only indicates which configuration the firmware was
        //   compiled for.  So it won't distinguish between an
        //   original Raspberry Pi Pico and a compatible clone,
        //   for example.
        //
        // - Pico SDK version string: the version of the Pico SDK
        //   used to build the firmware.
        //
        // - TinyUSB library version string: the version of TinyUSB
        //   used to build the firmware.  (TinyUSB is the Pico SDK's
        //   official USB layer, but it's a separate project with its
        //   own versioning.  The Pico SDK version doesn't necessarily
        //   imply a particular TinyUSB version.)
        //
        // - Compiler version string: the name and version of the
        //   C++ compiler used to build the firmware, typically
        //   of the form "GNUC x.y.z".
        //    
        // The hardware ID is a unique identifier for the physical Pico
        // unit, programmed permanently into ROM at the factory during the
        // Pico manufacturing process.  It's unique and immutable, so it
        // serves as positive identification for a particular physical
        // Pico device.  It's not affected by resets or firmware updates
        // or anything else, since it's etched into the silicon.
        static const uint8_t CMD_QUERY_IDS = 0x03;

        // Reset the Pico and restart the flash program.  The first byte
        // of the arguments contains a subcommand that specifies the mode
        // to activate after the reset.
        //
        // The USB connection will be dropped across the reset, so the
        // host side will generally need to close and re-open its USB
        // connection.
        //
        // Resetting into Boot Loader mode can be used to install a
        // firmware update.  After the reset, the Pico will reconnect as
        // an RP2 Boot device, with its associated USB virtual disk
        // drive.  The RP2 Boot device doesn't expose any form of unique
        // ID information for the Pico, so there's no way to know with
        // certainty which RP2 Boot device corresponds to the same
        // physical Pico that you were connected to via the vendor
        // interface.  However, since the reboot into RP2 Boot Loader
        // mode is fast (typically less than 1 second), it's a good bet
        // that the next new RP2 Boot Loader device to attach to the
        // host after issuing this command is the same physical device
        // that you just explicitly rebooted.  So an easy algorithm is
        // to scan for RP2 Boot Loader devices just *before* sending the
        // reboot command, then scan again every second or so until a
        // new Boot Loader device appears that wasn't in the "before"
        // list.
        static const uint8_t CMD_RESET = 0x04;
        static const uint8_t SUBCMD_RESET_NORMAL = 0x01;      // run firmware program in standard mode
        static const uint8_t SUBCMD_RESET_BOOTLOADER = 0x02;  // reset into the Pico's native Boot Loader mode

        // Read data from the in-memory message logger.  The firmware
        // saves logging messages in memory for later retrieval via the
        // vendor interface.  This retrieves as much logging information
        // as is available and can fit in the returned message.  The log
        // text is returned as plain 8-bit text in the reply data
        // transfer.  If no logging text is available, returns ERR_EOF
        // status.
        //
        // The log.avail field in the reply contains the total number of
        // bytes currently available in the log data on the device.
        // This lets the client distinguish log data that was already
        // available when it started retrieving the log from new log
        // messages that were added during the retrieval loop.  A client
        // that runs interactively can ignore this, since an interactive
        // client will usually want to just keep showing new messages as
        // they arrive until the user closes the window (or equivalent).
        // A client that runs as a one-shot check on the log can use the
        // available size at the start of its polling loop to stop after
        // it has retrieved all of the messages that were available when
        // it first looked, so that it doesn't keep looping forever if
        // new messages are added in the course of the retrieval loop.
        static const uint8_t CMD_QUERY_LOG = 0x05;

        // Statistics commands.  The first byte of the request arguments
        // is a sub-command code selecting the action to perform.  Additional
        // argument bytes vary by sub-command.
        //
        // SUBCMD_STATS_QUERY_STATS
        //   Query global statistics.  Returns data in the additional transfer
        //   bytes, as a Statistics struct.  The second byte of the arguments
        //   contains bit flags, as a combination of QUERYSTATS_FLAG_xxx bits
        //   defined below:
        //
        //   QUERYSTATS_FLAG_RESET_COUNTERS
        //     If set, the rolling-average counters are reset to start a
        //     new averaging window.
        //
        static const uint8_t CMD_STATS = 0x06;
        static const uint8_t SUBCMD_STATS_QUERY_STATS = 0x01;

        // flags for CMD_STATE + SUBCMD_STATS_QUERY_STATS
        static const uint8_t QUERYSTATS_FLAG_RESET_COUNTERS = 0x01;

        // Process a button-press event on the host.  The host populates
        // args.hostInputEvent with the BLT-II Pico GPIO number where
        // the button corresponding to the event is wired, the host-side
        // timestamp when it received the event, and the current USB
        // hardware frame number and SOF (Start-of-frame) timestamp on
        // the same host system clock used to record the event time.
        //
        // The Pico matches up the event against the last OFF-to-ON
        // transition on the GPIO (which is physically a transition from
        // HIGH logic level to LOW on the Pico GPIO, since the button
        // inputs are active-low).  The Pico populates args.hostInputResult
        // in the return arguments with flags giving the event status,
        // and the measured total latency from the matched button press.
        //
        // For accurate time synchronization between the host and Pico
        // system clocks, the host must provide information on the
        // current USB frame timing from the USB hardware adapter.
        // On Windows, this information is available through the WinUsb
        // user-mode API via WinUsb_GetCurrentFrameNumberAndQpc().
        // The host-side input event time passed to the Pico is expressed
        // in terms of an offset from the current USB SOF timestamp,
        // T[Start-of-frame] - T[event], in microseconds.
        //
        // The Pico records the event in its latency statistics memory,
        // which can be retrieved via CMD_MEASUREMENTS.
        static const uint8_t CMD_HOST_INPUT_EVENT = 0x07;

        // Get/set the debounce lockout time.  The first byte of the
        // arguments is a subcommand code.
        //
        // The device debounces button inputs by latching a button's
        // "logical" state immediately upon detecting a change in the
        // physical high/low logic level, and locking out changes to the
        // logical state until the debounce lockout time has elapsed.
        // This prevents recording the voltage oscillations after a
        // switch "make" or "break" event as separate button presses, by
        // treating all oscillations for the defined lockout period as
        // part of the same overall state change.
        //
        // This debouncing method adds zero latency for each transition
        // event, since it detects the state change in the button at the
        // first physical edge detected within the Pico's time
        // resolution.  In exchange, it limits how quickly the button
        // can be cycled - that is, it limits the minimum time that a
        // button can be considered ON or OFF, and thus limits the
        // frequency at which the button can switch states.  This isn't
        // much of a limitation in practice, because switches physically
        // can't cycle faster than their "bounce" time.  A fast
        // microswitch tends to bounce for at least 1-2 ms, so it's
        // simply not meaningful to try to measure switch state
        // transitions on a shorter time scale - you just can't
        // distinguish anything shorter from switch bounce.
        //
        // The Pico GPIO inputs are reasonably immune to physical noise
        // due to their use of Schmitt trigger inputs.  So it's not
        // generally necessary to also filter for false edges.  That's
        // why we can get away with this zero-latency algorithm, where
        // we interpret the first edge we see as an actual physical
        // transition.  If we had to filter for noise as well, we'd have
        // to apply a low-pass filter to the input signal, which would
        // add latency of the filter's characteristic time.
        //
        // SUBCMD_DEBOUNCE_TIME_GET
        //   Retrieve the current debounce time in microseconds, encoded
        //   in the first four bytes of the reply arguments as a little-
        //   endian UINT32.
        //
        // SUBCMD_DEBOUNCE_TIME_SET
        //   The new debounce time in microseconds is encoded as a
        //   little-endian UINT32 in the next four bytes of the arguments
        //   after the subcommand byte.
        //
        static const uint8_t CMD_DEBOUNCE_TIME = 0x08;
        static const uint8_t SUBCMD_DEBOUNCE_TIME_GET = 0x01;
        static const uint8_t SUBCMD_DEBOUNCE_TIME_SET = 0x02;

        // Timing measurements commands.  The first byte of the
        // arguments is the subcommand code.
        //
        // SUBCMD_MEASUREMENTS_GET
        //   Retrieves timing measurement data collected since the Pico
        //   CPU reset or the last SUBCMD_MEASUREMENT_RESET command.
        //   The measurements are returned in the extra transfer data.
        //   The transfer starts with a MeasurementsList struct as a
        //   header, which mostly provides information on the byte
        //   layout of the per-button structures.  The header is
        //   followed by a packed array of MeasurementData structs,
        //   one per GPIO.  This only includes entries for GPIOs that
        //   are exposed as pins on the Pico: 0-22, 26, 27, 28.
        //
        // SUBCMD_MEASUREMENTS_RESET
        //   Resets the internal counters for all timing measurements.
        //
        static const uint8_t CMD_MEASUREMENTS = 0x09;
        static const uint8_t SUBCMD_MEASUREMENTS_GET = 0x01;
        static const uint8_t SUBCMD_MEASUREMENTS_RESET = 0x02;

        // Length of the arguments union data, in bytes.  This is the number
        // of bytes of data in the arguments union that are actually used.
        // At the USB level, the request packet is of fixed length, so the
        // whole args union is always transmitted, but this specifies how
        // many bytes of it are actually meaningful for the command.
        uint8_t argsSize = 0;

        // Transfer length.  If the request requires additional data beyond
        // what can fit in the arguments to be transmitted host-to-device,
        // this specifies the length of the additional data, which the host
        // sends as another packet immediately following the request.  If
        // this is zero, there's not another packet.
        uint16_t xferBytes = 0;

        // Arguments.  This allows a small amount of parameter data to be
        // provided directly with the request packet, rather than as an
        // additional transfer.  The meaning of this section is specific
        // to each command code, and many commands don't use it at all.
        // The host must set argsSize to the number of bytes of the union
        // that are actually used for the request; if no arguments are
        // used, set argsSize to 0.
        union Args
        {
            // raw byte view of the argument data
            uint8_t argBytes[16];

            // Host Input Event arguments
            struct __PackedBegin HostInputEvent
            {
                // Host system clock time at SOF, in microseconds.  This
                // is purely for debugging, to compare system clock time
                // offsets calculated using the USB SOF method.
                uint64_t sofTimestamp;
                
                // Time interval, in microseconds, from the event
                // arrival at the application's event ingestion point to
                // the USB Start of Frame for the hardware frame number
                // in usbFrameCounter.
                //
                // To calculate this time interval, the host application
                // must record the time on the host system clock at the
                // moment it receives the event in application code, and
                // then must get the system clock time of the last SOF
                // signal from the USB hardware just before sending this
                // request.  The value here is the difference between
                // those two timestamps, T[SOF] - T[event].  This is a
                // signed value because the last start of frame might
                // have occurred before the event was received.
                //
                // The SOF timestamp must be for the most recent
                // available frame.  This is important because the
                // hardware frame counter rolls over every 1.024
                // seconds, which makes the frame counter a valid
                // reference point only during a 1-second window.  The
                // caller must therefore obtain the SOF timestamp just
                // before sending the request to ensure that it doesn't
                // roll over before the Pico receives the packet.
                //
                // HOW TO IMPLEMENT ON WINDOWS WITH WINUSB:
                // On Windows, the high-precision system clock time can
                // be obtained from QueryPerformanceCounter(), and the
                // USB SOF time via WinUsb_GetCurrentFrameNumberAndQpc().
                //
                // IF SOF TIMESTAMPS ARE NOT AVAILABLE:
                // Obtaining the SOF timestamp requires access to the
                // USB hardware on the host system, so it might not be
                // possible on some OSes.  If the SOF time isn't
                // available:
                //
                // - Set this field to the elapsed time from the event
                //   ingestion to just before sending this request
                //
                // - Set the frame number field to 0xFFFF
                //
                // That will allow the latency calculation to exclude
                // the host application processing time up to *but not
                // including* the USB transfer.  It's impossible to
                // measure the time of the USB transfer in the absence
                // of a reliable timestamp at SOF, because the Pico and
                // host have no other way to synchronize their clocks to
                // adequate precision.  The latency measurements will
                // therefore overstate the actual latency by the average
                // USB transfer time (including staging time for the
                // requests in the host USB driver), which is inherently
                // unpredictable, because it depends on how each event
                // time aligns with the USB hardware's 125us microframe
                // cycle.
                int32_t dtEventToSof;

                // USB 10-bit hardware frame counter in effect at the
                // SOF timestamp used to calculate dtEventToSof.
                uint16_t usbFrameCounter;
                
                // Pico GPIO number where the button corresponding to
                // the input input is connected.  This is the GPIO on
                // the Button Latency Tester II Pico - don't confuse
                // this with the GPIO on the subject device being
                // tested.
                uint8_t gp;
            } __PackedEnd hostInputEvent;
        } args;

    } __PackedEnd;



    // Device-to-host response format.  The device responds to each request
    // with this struct to indicate the result of the request.
    struct __PackedBegin VendorResponse
    {
        // Token.  This is the token from the host's request packet, so that
        // the host can correlate the response to the original request.
        uint32_t token;

        // Command code.  This is the same command specified in the
        // corresponding request.
        uint8_t cmd;

        // Argument size.  This is the number of bytes of the arguments
        // union that are populated in the response.  This is set to 0
        // if the response has no arguments.
        uint8_t argsSize;

        // Status code
        uint16_t status;

        //
        // Status codes
        //

        // success
        static const uint16_t OK = 0;

        // general failure
        static const uint16_t ERR_FAILED = 1;

        // client-side timeout on USB transaction
        static const uint16_t ERR_TIMEOUT = 2;

        // transfer length out too long
        static const uint16_t ERR_BAD_XFER_LEN = 3;

        // USB transfer failed
        static const uint16_t ERR_USB_XFER_FAILED = 4;

        // parameter error (missing or bad argument in function call)
        static const uint16_t ERR_BAD_PARAMS = 5;

        // invalid command code
        static const uint16_t ERR_BAD_CMD = 6;

        // invalid subcommand code (for commands that have subcommand variations)
        static const uint16_t ERR_BAD_SUBCMD = 7;

        // mismatched reply for request
        static const uint16_t ERR_REPLY_MISMATCH = 8;

        // config transfer timeout
        static const uint16_t ERR_CONFIG_TIMEOUT = 9;

        // config data invalid
        static const uint16_t ERR_CONFIG_INVALID = 10;

        // out of bounds
        static const uint16_t ERR_OUT_OF_BOUNDS = 11;

        // not ready for current operation
        static const uint16_t ERR_NOT_READY = 12;

        // end of file
        static const uint16_t ERR_EOF = 13;

        // data or data format error in request/reply data
        static const uint16_t ERR_BAD_REQUEST_DATA = 14;
        static const uint16_t ERR_BAD_REPLY_DATA = 15;

        // file (or other object type, depending on context) not found
        static const uint16_t ERR_NOT_FOUND = 16;

        // Transfer length.  If the response requires additional data to
        // be sent device-to-host, this indicates the length of the data,
        // which the device sends as another packet immediately following
        // the response.  If this is zero, there's not another packet.
        uint16_t xferBytes;

        // reserved (currently just here for padding to a 32-bit boundary)
        uint16_t reserved = 0;

        // Response arguments.  This allows the response to send back a
        // small amount of parameter data directly with the response
        // packet, without the need for an additional transfer.  If
        // the response contains any argument data, argsSize is set to
        // the number of bytes populated.  The meaning of the arguments
        // is specified to the command code.
        union Args
        {
            // raw byte view of the arguments
            uint8_t argBytes[16];

            // Version number, for CMD_QUERY_VERSION
            struct __PackedBegin Version
            {
                uint8_t major;       // major version number
                uint8_t minor;       // minor version number
                uint8_t patch;       // patch version number
                char buildDate[12];  // build date, ASCII, YYYYMMDDhhmm, no null terminator
            } __PackedEnd version;

            // ID numbers, for CMD_QUERY_IDS
            struct __PackedBegin ID
            {
                uint8_t hwid[8];      // the Pico's native hardware ID; permanent and universally unique per device
                uint16_t cpuType;     // CPU type (2040 -> RP2040, 2350 -> RP2350)
                uint8_t cpuVersion;   // CPU version (RP2040: 1 -> B0/B1, 2 -> B2)
                uint8_t romVersion;   // Pico ROM version (1 -> B0, 2 -> B1, 3 -> B2)
            } __PackedEnd id;

            // CMD_GET_LOG reply arguments
            struct __PackedBegin
            {
                uint32_t avail;      // number of bytes of log data currently available on the device side
            } __PackedEnd log;

            // HOST_INPUT_EVENT response
            struct __PackedBegin HostInputResult
            {
                // Latency result, in microseconds.  Valid only if the status is STATUS_MATCHED.
                uint32_t latency;

                // Measurement status - one of the STAT_xxx codes below
                uint8_t status;

                // Status: event matched.  The host input event was matched against
                // a recent physical button press on the same GPIO, and the latency
                // field is valid.
                static const uint8_t STAT_MATCHED = 0x01;

                // Status: no match.  The Pico hasn't observed a button press on
                // the GPIO recently enough to match the input event.  No latency
                // measurement is possible for this event.
                static const uint8_t STAT_NO_MATCH = 0x02;

                // Status: repeated event.  The Pico has already matched the same
                // button press against a prior host input event.  It's possible (and
                // common) for the host to receive multiple events for a single
                // physical button press, because switch bounce can cause the device
                // generating the events to mistake an electrical fluctuation on the
                // switch to a new button press.  Most devices use software filtering
                // to reduce false events due to switch bounce, and the BLT-II Pico
                // does as well, but they might not always agree due to differences
                // in their filtering algorithms and filter timing.  So BLT-II
                // simply ignores host input events that appear to be duplicates,
                // and reports those cases with this code.
                static const uint8_t STAT_DUPLICATE = 0x03;
                
            } __PackedEnd hostInputResult;

        } args;
    } __PackedEnd;

    // CMD_QUERY_STATS response data struct, returned as the additional
    // transfer data.
    // 
    // The structure size member (cb) is a version marker.  The caller
    // should check this to make sure the version returned contains the
    // fields being accessed.  Any newer version will start with the
    // identical field layout as the next older version, with additional
    // fields added at the end, so a struct with a larger size can
    // always be interpreted using an older version of the struct.
    struct __PackedBegin Statistics
    {
        uint16_t cb;             // structure size, to identify structure version
        uint16_t reserved0;      // reserved/padding
        uint32_t reserved1;      // reserved/padding
        uint64_t upTime;         // time since reboot, in microseconds

        uint64_t nLoops;         // number of iterations through main loop since stats reset
        uint64_t nLoopsEver;     // number of main loop iterations since startup
        uint32_t avgLoopTime;    // average main loop time, microseconds
        uint32_t maxLoopTime;    // maximum main loop time, microseconds

        uint64_t nLoops2;        // number of iterations through secondary core main loop since stats reset
        uint64_t nLoopsEver2;    // number of secondary core main loop iterations since startup
        uint32_t avgLoopTime2;   // average secondary core main loop time, microseconds
        uint32_t maxLoopTime2;   // maximum secondary core main loop time, microseconds

        uint32_t heapSize;       // total heap size
        uint32_t heapUnused;     // heap space not in use
        uint32_t arenaSize;      // arena size
        uint32_t arenaAlloc;     // arena space allocated
        uint32_t arenaFree;      // arena space not in use
    } __PackedEnd;

    // SUBCMD_MEASUREMENTS_GET list header.  The transfer data starts
    // with this structure, which is followed immediately by a packed
    // array of MeasurementData structs.
    struct __PackedBegin MeasurementsList
    {
        // size of this header struct, in bytes
        uint16_t cb;

        // size of each MeasurementData struct, in bytes
        uint16_t cbData;

        // number of MeasurementData structs
        uint16_t nData;
        
    } __PackedEnd;

    // SUBCMD_MEASUREMENTS_GET list element.  This provides the
    // timing measurement data for one GPIO port on the Pico.
    struct __PackedBegin MeasurementData
    {
        // GPIO port number that this element covers
        uint8_t gp;

        // padding/reserved
        uint8_t reserved[7];

        // number of physical presses recorded on the button
        uint64_t nPresses;

        // number of host events matched against presses
        uint64_t nHostEvents;

        // Sum of latency measured for host events, in microseconds, and
        // sum of latency squared.  The mean latency can be calculated
        // as latencySum/nHostEvents.  The standard deviation is
        // sqrt(latencySum2/nHostEvents - mean*mean).
        uint64_t latencySum;
        uint64_t latencySquaredSum;

        // minimum and maximum latency recorded
        uint64_t latencyMin;
        uint64_t latencyMax;

        // Median latency in microseconds.  This is calculated from the
        // most recent set of samples up to a maximum number set in the
        // firmware (currently 1000).
        uint16_t latencyMedian;
        
    } __PackedEnd;
}

