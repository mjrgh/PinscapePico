// Pinscape Pico Button Latency Tester II - USB Vendor Interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <malloc.h>
#include <list>

// Pico SDK headers
#include <pico.h>
#include <pico/stdlib.h>
#include <pico/unique_id.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#include <hardware/structs/usb.h>
#include <tusb.h>

// local project headers
#include "../../Firmware/Pinscape.h"
#include "../../Firmware/Utils.h"
#include "../../Firmware/BytePackingUtils.h"
#include "../../Firmware/Logger.h"
#include "../../Firmware/crc32.h"
#include "../../Firmware/Reset.h"
#include "../../Firmware/Main.h"
#include "../../Firmware/Watchdog.h"
#include "../../Firmware/CommandConsole.h"
#include "../USBProtocol.h"
#include "ButtonLatencyTester2.h"
#include "VendorIfc.h"
#include "Version.h"

// Tinyusb has a serious regression in the official 0.17.0 release that
// puts the USB connection into a wedged state after a sleep/resume cycle.
// The error was a regression in 0.17.0, and was fixed in the official
// 0.18.0, so it ONLY affects *exactly* version 0.17.0.  Unfortunately,
// that's the version in the current official Raspberry Pi SDK (2.1.0),
// so anyone building against the official SDK will get the buggy code.
// I added the fix to my UNOFFICIAL build of the 2.1.0 SDK, along with
// a marker macro to let client code know that the fix is included.
//
// So: when we detect version 0.17.0, we'll test for the presence of
// the marker macro, and fail the build if it's not there.  This is
// in the hope of saving the next fellow A LOT of debugging work
// figuring out why sleep/resume is hosing the connection.
//
// DO NOT IGNORE THIS ERROR!  DO NOT #define AWAY THE PROBLEM!
// The test is here to make your life easier, by saving you the trouble
// of building a firmware binary that won't work properly, which is what
// you'll get if you build against the official 2.1.0 SDK.  There are
// two valid ways to satisfy this error check:
//
// 1. Build against some official SDK release OTHER THAN 2.1.0, such
// as 1.5.1.  2.1.0 is the only SDK that includes the buggy TinyUSB
// release.
//
// 2. Get my UNOFFICIAL 2.1.0 SDK snapshot, and build against that
// INSTEAD OF the official 2.1.0.  My SDK snapshot is the same as the
// official 2.1.0, with the addition of the fix for the TinyUSB
// regression.  My unofficial 2.1.0 SDK build is at:
//
// https://github.com/mjrgh/pico-sdk-2.1.0
//
#if (PINSCAPE_TUSB_VERSION_NUMBER == 1700) && !defined(PINSCAPE_PICO_TUSB_VENDOR_DEVICE_FIX_53989A9)
#error You are building against the stock TinyUSB 0.17.0, which contains a serious error that will make the USB connection unstable.  You MUST select a different TinyUSB version.  See the comments at this #error line in VendorIfc.cpp for recommended solutions.
#endif


// $$$ debugging
struct SOFStats
{
    uint64_t n = 0;

    uint64_t tPrv = 0;
    uint64_t dtSum = 0;
    uint64_t dtMin = 0;
    uint64_t dtMax = 0;

    uint16_t numPrv = 0;
    uint64_t dnumSum = 0;
    uint16_t dnumMin = 0;
    uint16_t dnumMax = 0;

    static const int nLog = 256;
    struct Log
    {
        uint64_t t;
        uint64_t dt;
        uint16_t frameNum;
    };
    Log log[nLog];
    int logWriteIndex = 0;
    volatile Log *logWritePtr = &log[0];

    void Add(uint64_t t, uint16_t num) volatile
    {
        volatile uint64_t dt = t - tPrv;
        volatile uint16_t dNum = num < numPrv ? num + 2048 - numPrv : num - numPrv;
        if (n > 1)
        {
            dtSum += dt;
            dtMin = std::min(dt, dtMin);
            dtMax = std::max(dt, dtMax);

            dnumSum += dNum;
            dnumMin = std::min(dNum, dnumMin);
            dnumMax = std::max(dNum, dnumMax);
        }
        else if (n == 1)
        {
            dtMin = dtMax = dt;
            dnumMin = dnumMax = dNum;
        }
        ++n;
        tPrv = t;
        numPrv = num;

        logWritePtr->t = t;
        logWritePtr->dt = dt;
        logWritePtr->frameNum = num;
        if (++logWritePtr, ++logWriteIndex >= nLog)
            logWriteIndex = 0, logWritePtr = &log[0];
    }
};
volatile SOFStats sofStats;

static void Command_usbirq(const ConsoleCommandContext *c)
{
    c->Printf("UBS IRQ stats: n=%llu, dt avg %llu (%llu/%llu), dFrame %u/%u\n",
              sofStats.n, sofStats.dtSum/(sofStats.n-1), sofStats.dtMin, sofStats.dtMax, sofStats.dnumMin, sofStats.dnumMax);

    int idx = sofStats.logWriteIndex - sofStats.nLog/2;
    if (idx < 0) idx += sofStats.nLog;
    for (int i = 0 ; i < sofStats.nLog/2 ; ++i)
    {
        auto &l = sofStats.log[idx];
        c->Printf(
            "%12llu (%+12llu) %4u%s", l.t, l.dt, l.frameNum, (i & 3) == 3 ? "\n" : " | ");

        if (++idx >= sofStats.nLog)
            idx = 0;
    }
}

// $$$ end debugging


// global singleton
VendorIfc vendorIfc;

// WinUsb GUID for our vendor interface
const char *VendorIfc::WINUSB_GUID = "{4D7C1DBD-82ED-4886-956F-7DF0B316DBF5}";

// construction
VendorIfc::VendorIfc()
{
}

// initialize
void VendorIfc::Init()
{
    // enable SOF callback
    tud_sof_cb_enable(true);

    // Add our shared USB IRQ handler.  TinyUSB installs a shared handler
    // at highest priority.  We don't really care about the priority, since
    // all we care about is noting the time of SOF changes, which we can
    // detect by comparing to the previous SOF value.
    //
    // Note that we don't have to worry about enabling the interrupt,
    // because TinyUSB has to do that for its own purposes. 
    irq_add_shared_handler(USBCTRL_IRQ, &USBIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);

    // $$$ debugging
    CommandConsole::AddCommand(
        "usbirq", "show USB IRQ statistics",
        "usbirq  (no arguments)",
        Command_usbirq);
}

// USB IRQ handler
void VendorIfc::USBIRQ()
{
    // if the USB hardware frame number has changed, note the timestamp
    uint16_t frameCounter = usb_hw->sof_rd & USB_SOF_RD_BITS;
    if (frameCounter != vendorIfc.usbFrameCounter)
    {
        vendorIfc.usbFrameCounter = frameCounter;
        vendorIfc.tUsbFrameCounter = time_us_64();

        sofStats.Add(vendorIfc.tUsbFrameCounter, vendorIfc.usbFrameCounter);
    }
}

// Calculate the Pico time for a given host time
uint64_t VendorIfc::HostTimeToPicoTime(int32_t hostTimeToSof, uint16_t hostFrameNum, uint64_t hostSofTimestamp)
{
    // Snapshot the Pico-side USB hardware frame number and time, so
    // that we're working with coherent values even if a new SOF
    // interrupt occurs while we're working
    uint16_t picoCurFrameNum;
    uint64_t picoCurFrameTime;
    {
        IRQDisabler irqd;
        picoCurFrameNum = usbFrameCounter;
        picoCurFrameTime = tUsbFrameCounter;
    }
    
    // The frame counter is only an 11-bit value that's incremented every
    // 1ms, so it rolls over every 2.048 seconds.  Fortunately, we
    // know for certain that the host frame number must always be EQUAL
    // TO OR LESS THAN the current Pico frame number, because the host
    // can't send us messages from the future.  If the host frame number
    // is nominally higher than the Pico frame number, it means that the
    // Pico number rolled over from 2047 to 0 since the host frame
    // number was recorded.  (This of course assumes that the frame is
    // within the past 1.024 seconds, so that it hasn't rolled over more
    // than once.  That's always true as long as the caller captures
    // the USB frame information immediatly before sending the request.)
    int dFrame = static_cast<int>(picoCurFrameNum) - static_cast<int>(hostFrameNum);
    if (dFrame < 0)
        dFrame += 2048;

    // Frames start precisely every 1ms, so the elapsed time between the
    // two frames is 1000us per frame.  The frame counter is incremented
    // by one at each interval, so we can determine the number of frames
    // between the two counter values simply by taking the arithmetic
    // difference of the counters.
    int dt = dFrame * 1000;

    // Now we can figure the Pico system clock time at the host SOF
    // time: it's the Pico SOF time for the current frame, minus the
    // elapsed time between the Pico frame and the host frame.
    uint64_t picoPastFrameTime = picoCurFrameTime - dt;

    // For debugging, log the correlation between Pico time and host
    // system time at SOF.  This lets us check how accurate this method
    // is by checking the stability of the offset value calculated.
    static int64_t prvOffset = 0;
    int64_t offset = static_cast<int64_t>(hostSofTimestamp - picoPastFrameTime);
    Log(LOG_DEBUG, "HostTimeToPicoTime: dNum=%d, tHost=%llu, tPico=%llu, ofs=%lld (%+lld), IRQ dt %llu/%llu, dn %u/%u\n",
        dFrame, hostSofTimestamp, picoPastFrameTime, offset, offset - prvOffset,
        sofStats.dtMin, sofStats.dtMax, sofStats.dnumMin, sofStats.dnumMax);
    prvOffset = offset;

    // We can now calculate the Pico time of the host event, as the Pico
    // time of the SOF for the host frame number MINUS the elapsed time
    // from event to this SOF.
    return picoPastFrameTime - hostTimeToSof;
}

// periodic task handler
void VendorIfc::Task()
{
    // continue working on the current pending request
    TryProcessRequest();

    // check for a pending reboot
    if (deferredRebootSubcmd != 0 && time_us_64() >= deferredRebootTime)
    {
        // execute the command
        switch (deferredRebootSubcmd)
        {
        case Request::SUBCMD_RESET_NORMAL:
            // Reset the Pico and restart the flash-resident software in normal mode
            picoReset.Reboot(false, PicoReset::BootMode::Normal);
            break;

        case Request::SUBCMD_RESET_BOOTLOADER:
            // Reboot the Pico into the ROM Boot Loader
            picoReset.Reboot(true);
            break;
        }

        // Clear the request.  Note that the reboot might not happen
        // immediately, since other subsystems might have placed a reset
        // lock.
        deferredRebootSubcmd = 0;
        deferredRebootTime = UINT64_MAX;
    }
}

// process output (host-to-device) on our interface
void VendorIfc::OnOutput(const uint8_t *buf, size_t len)
{
    // Snapshot the time before we do any other processing, so that we
    // have the most accurate tiem possible for the actual USB packet
    // arrival time.  Since the main job of this program is to measure
    // latency between events, we want to minimize the artificial
    // latency added in the measuring process itself, so we want to
    // "stop the clock" as early as possible in the USB message
    // processing.  That would be right now.
    uint64_t t0 = time_us_64();
    
    // Check to see if we're awaiting a data transfer in.  If the
    // tail of the queue is expecting data in that hasn't been
    // received yet, treat this as the data transfer.  Otherwise,
    // it's a new request.
    if (curRequest.xferBytes != 0)
    {
        // There's an incoming data transfer for this request, so the
        // new input goes into the incoming data buffer.
        xferIn.Append(buf, len);

        // process the request if it's now complete
        TryProcessRequest();
    }
    else
    {
        // We're not awaiting an incoming data transfer for a past
        // request, so this is a new request.  Check that it's
        // the correct length for a request and that the checksum
        // is valid; if not, ignore it, since it's either an ill-formed
        // request or something else entirely.
        auto const *req = reinterpret_cast<const Request*>(buf);
        if (len >= sizeof(Request) && req->ValidateChecksum())
        {
            // copy the request data to the current request store
            memcpy(&curRequest, req, sizeof(curRequest));

            // any extra data beyond the Request struct size is extra transfer data
            if (int extra = len - sizeof(curRequest); extra > 0)
                xferIn.Append(buf + sizeof(curRequest), static_cast<size_t>(extra));

            // mark the time of the request (the time we entered
            // this routine)
            curRequestTime = t0;

            // process the request if it's complete
            TryProcessRequest();
        }
        else
        {
            Log(LOG_VENDOR, "Bad vendor request received, wrong size or checksum (size=%d, checksum=%08lx, expected=%08lx)\n",
                len, req->checksum, req->ComputeChecksum(req->token, req->cmd, req->xferBytes));
        }
    }
}

// Check the current buffered request to determine if it's complete, and
// process it if so.
void VendorIfc::TryProcessRequest()
{
    // if no request is active, there's nothing to do
    if (curRequest.cmd == Request::CMD_NULL)
        return;

    // if we have enough data, process the request
    if (xferIn.len >= curRequest.xferBytes)
    {
        ProcessRequest();
        return;
    }

    // check for timeout
    if (time_us_64() - curRequestTime > REQUEST_TIMEOUT)
    {
        Log(LOG_VENDOR, "Request timeout; pending request canceled\n");
        ClearCurRequest();
    }
}

// Process the current buffered request
void VendorIfc::ProcessRequest()
{
    // Set up a default OK response packet, copying the request's token
    // and command code, and setting the arguments and additional transfer
    // lengths to zero.
    Response resp{ curRequest.token, curRequest.cmd, 0, Response::OK, 0 };
    const uint8_t *pXferOut = nullptr;

    // Log it to the CDC for debugging, except for requests for the log,
    // since those will rapidly and uselessly grow the log if the client
    // is viewing the log interactively.  And if a request is being
    // repeated, just count it.
    if (curRequest.cmd == prvRequestCmd)
    {
        // count it but don't log it for now
        prvRequestCnt += 1;
    }
    else if (curRequest.cmd != Request::CMD_QUERY_LOG)
    {
        // log the previous request repeat count, if applicable
        if (prvRequestCnt > 0)
            Log(LOG_DEBUGEX, "Vendor request repeated: cmd=%d, repeats=%d\n", prvRequestCmd, prvRequestCnt);
        
        // log the new request
        Log(LOG_DEBUGEX, "Vendor request received: cmd=%d, token=%ld, args=%d, xferLen=%d\n",
            curRequest.cmd, curRequest.token, curRequest.argsSize, curRequest.xferBytes);

        // remember it for next time
        prvRequestCmd = curRequest.cmd;
        prvRequestCnt = 0;
    }

    // check the command
    switch (curRequest.cmd)
    {
    case Request::CMD_NULL:
        // Null command - nothing to do
        break;

    case Request::CMD_PING:
        // Ping - just send back an OK response
        break;

    case Request::CMD_QUERY_VERSION:
        // Firmware version query

        // store the version numbers
        resp.argsSize = sizeof(resp.args.version);
        resp.args.version.major = VERSION_MAJOR;
        resp.args.version.minor = VERSION_MINOR;
        resp.args.version.patch = VERSION_PATCH;

        // store the build timestamp
        memcpy(resp.args.version.buildDate, buildTimestamp, 12);
        break;

    case Request::CMD_QUERY_IDS:
        // query device identifiers
        {
            // get the 8-byte unique ID from the flash chip
            pico_unique_board_id_t hwid;
            pico_get_unique_board_id(&hwid);

            // copy it to the response
            auto &ids = resp.args.id;
            resp.argsSize = sizeof(ids);
            static_assert(sizeof(ids.hwid) == sizeof(hwid));
            memcpy(ids.hwid, &hwid, sizeof(hwid));

            // set the Pico hardware version information
            ids.romVersion = rp2040_rom_version();
#ifdef PICO_RP2040
            ids.cpuType = 2040;
            ids.cpuVersion = rp2040_chip_version();
#elif PICO_RP2350
            ids.cpuType = 2350;
            ids.cpuVersion = rp2350_chip_version();
#else
#error Unknown target
#endif
            // copy the board name into the transfer data
            uint8_t *dst = xferOut.data;
            uint8_t *dstMax = dst + sizeof(xferOut.data);
            for (const char *p = PICO_BOARD ; *p != 0 && dst+1 < dstMax ; *dst++ = *p++);
            *dst++ = 0;

            // copy the Pico SDK version string into the transfer data
            for (const char *p = PICO_SDK_VERSION_STRING ; *p != 0 && dst+1 < dstMax ; *dst++ = *p++);
            *dst++ = 0;

            // Copy the tinyusb version number into the transfer data.  (Note
            // that TUSB_VERSION_STRING is defined erroneously in some versions
            // of the library headers to not expand the macros, so it comes out
            // as "TUSB_VERSION_MAJOR.TUSB_VERSION_MINOR.TUSB_VERSION_REVISION".
            // So don't use that; construct our own string instead.)
#define PSVI_STRING(x) #x
#define PSVI_XSTRING(x) PSVI_STRING(x)
#define PVSI_TUSB_VERSION_STRING PSVI_XSTRING(TUSB_VERSION_MAJOR) "." PSVI_XSTRING(TUSB_VERSION_MINOR) "." PSVI_XSTRING(TUSB_VERSION_REVISION)
            for (const char *p = PVSI_TUSB_VERSION_STRING ; *p != 0 && dst+1 < dstMax ; *dst++ = *p++);
            *dst++ = 0;

            // copy the compiler version into the transfer data
            for (const char *p = COMPILER_VERSION_STRING ; *p != 0 && dst+1 < dstMax ; *dst++ = *p++);
            *dst++ = 0;

            // set the transfer data
            pXferOut = xferOut.data;
            resp.xferBytes = dst - xferOut.data;
        }
        break;

    case Request::CMD_RESET:
        // Reboot the Pico
        Log(LOG_VENDOR, "Deferred boot command, subcommand code %d\n", curRequest.args.argBytes[0]);

        // These commands must be deferred, to allow time for the reply
        // to be sent back to the host, and for output port updates to
        // complete.
        deferredRebootSubcmd = curRequest.args.argBytes[0];
        deferredRebootTime = time_us_64() + 100000;
        break;

    case Request::CMD_QUERY_LOG:
        // Get in-memory logging data
        resp.argsSize = sizeof(resp.args.log);
        if (size_t avail = vendorInterfaceLogger.Available(); avail != 0)
        {
            // transfer up to one transfer buffer full, or whatever remains in the log buffer
            size_t size = avail < sizeof(xferOut.data) ? avail : sizeof(xferOut.data);

            // copy the data
            resp.xferBytes = xferOut.len = vendorInterfaceLogger.Get(reinterpret_cast<char*>(xferOut.data), size);
            pXferOut = xferOut.data;

            // set the full available size in the response
            resp.args.log.avail = avail;
        }
        else
        {
            // no data available - return EOF
            resp.status = Response::ERR_EOF;
            resp.args.log.avail = 0;
        }
        break;

    case Request::CMD_STATS:
        // statistics request
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_STATS_QUERY_STATS:
            // get and reset main loop timing stats
            {
                // get the main loop stats
                auto &m = mainLoopStats;
                auto &m2 = secondCoreLoopStats;

                // get the malloc statistics
                extern char __StackLimit, __bss_end__;
                size_t totalHeap = &__StackLimit - &__bss_end__;
                struct mallinfo mi = mallinfo();

                // populate the Statistics struct to send back
                ButtonLatencyTester2::Statistics s{ sizeof(s) };
                s.upTime = time_us_64();

                s.nLoops = m.nLoops;
                s.nLoopsEver = m.nLoopsEver;
                s.avgLoopTime = m.totalTime / m.nLoops;
                s.maxLoopTime = m.maxTime;
                
                s.nLoops2 = m2.nLoops;
                s.nLoopsEver2 = m2.nLoopsEver;
                s.avgLoopTime2 = m2.totalTime / m2.nLoops;
                s.maxLoopTime2 = m2.maxTime;

                s.heapSize = static_cast<uint32_t>(totalHeap);
                s.heapUnused = static_cast<uint32_t>(totalHeap - mi.arena);
                s.arenaSize = mi.arena;
                s.arenaAlloc = mi.uordblks;
                s.arenaFree = mi.fordblks;

                // if desired, reset the main loop counters
                uint8_t flags = curRequest.args.argBytes[1];
                if ((flags & Request::QUERYSTATS_FLAG_RESET_COUNTERS) != 0)
                {
                    // reset the primary core loop counters
                    m.Reset();

                    // request reset on the secondary core counters (to be carried
                    // out by the second core thread, since only the second core has
                    // write access to the struct)
                    secondCoreLoopStatsResetRequested = true;
                }

                // copy the struct to the transfer data
                pXferOut = SetXferOut(resp, &s, sizeof(s));
            }
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_HOST_INPUT_EVENT:
        // Host input event
        resp.argsSize = sizeof(resp.args.hostInputResult);
        if (!ProcessHostButtonEvent(resp.args.hostInputResult, curRequest.args.hostInputEvent))
            resp.status = Response::ERR_BAD_PARAMS;
        break;

    case Request::CMD_DEBOUNCE_TIME:
        // Debounce time get/set.  The first byte is the subcommand code.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_DEBOUNCE_TIME_GET:
            // get the current debounce time
            {
                resp.argsSize = 4;
                uint8_t *p = resp.args.argBytes;
                PutUInt32(p, debounceLockoutTime_us);
            }
            break;

        case Request::SUBCMD_DEBOUNCE_TIME_SET:
            // set the debounce time
            {
                const uint8_t *p = &curRequest.args.argBytes[1];
                debounceLockoutTime_us = GetUInt32(p);
            }
            break;
            
        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_MEASUREMENTS:
        // Latency measurements.  The first byte is the subcommand code.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_MEASUREMENTS_GET:
            // retrieve measurements
            pXferOut = xferOut.data;
            resp.xferBytes = PopulateMeasurementsList(xferOut.data, sizeof(xferOut.data));

            // a zero return indicates failure
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_MEASUREMENTS_RESET:
            // reset measurements
            ResetMeasurements();
            break;
            
        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;
        
    default:
        // bad command request
        resp.status = Response::ERR_BAD_CMD;
        break;
    }

    // Send the response packet; flush it so that the reply goes to the
    // host immediately, even if we ended on a partial endpoint buffer.
    // Tinyusb only transmits by default on endpoint buffer boundaries
    // (64 bytes for the Pico's Full Speed interface).
    tud_vendor_write(&resp, sizeof(resp));
    tud_vendor_flush();

    // Send the additional response data, if any
    if (resp.xferBytes != 0)
    {
        // Send the transfer data.  The endpoint FIFO should always be big
        // enough to handle the response header plus the maximum extra transfer
        // data size, so it's an error if we couldn't write the whole thing.
        // Since the protocol is synchronous, there can't be old data sitting
        // in the buffer, and we explicitly size the buffer in the tusb
        // configuration to be big enough for a maximum reply, so the only
        // way we should have a partial write is if our tusb configuration
        // is wrong.  If this error occurs, CFG_TUD_VENDOR_TX_BUFSIZE in
        // tusb_config.h is probably set too small - it must be large enough
        // to accommodate sizeof(resp) plus the maximum xferOut data size.
        if (uint32_t written = tud_vendor_write(pXferOut, resp.xferBytes); written < resp.xferBytes)
        {
            Log(LOG_ERROR, "VendorIfc: incomplete transfer data write: xferBytes=%u, written=%u (check tusb_config.h -> CFG_TUD_VENDOR_TX_BUFSIZE)\n",
                resp.xferBytes, written);
        }

        // flush immediately, in case we didn't fill the endpoint buffer
        tud_vendor_flush();
    }

    // we're done with the request - clear it
    ClearCurRequest();
}

// clear the current request
void VendorIfc::ClearCurRequest()
{
    // clear the command ID in the request
    curRequest.cmd = Request::CMD_NULL;

    // clear the expected-transfer-in counter
    curRequest.xferBytes = 0;

    // clear the incoming transfer data
    xferIn.Clear();
}

// ---------------------------------------------------------------------------
// 
// Vendor interface transfer buffer
//

void VendorIfc::XferBuf::Append(const uint8_t *src, size_t copyLen)
{
    // limit the copy to the remaining buffer space
    size_t avail = sizeof(data) - len;
    if (copyLen > avail)
        copyLen = avail;

    // copy the data and update the length
    memcpy(data + len, src, copyLen);
    len += copyLen;
}

