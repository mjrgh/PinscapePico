// Pinscape Pico - USB Vendor Interface operations
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
#include <pico/platform.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#include <hardware/structs/usb.h>
#include <tusb.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "BytePackingUtils.h"
#include "USBIfc.h"
#include "VendorIfc.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "Version.h"
#include "Logger.h"
#include "USBCDC.h"
#include "crc32.h"
#include "Buttons.h"
#include "XInput.h"
#include "Outputs.h"
#include "NightMode.h"
#include "TimeOfDay.h"
#include "Config.h"
#include "Reset.h"
#include "Main.h"
#include "FlashStorage.h"
#include "JSON.h"
#include "Nudge.h"
#include "Plunger/Plunger.h"
#include "TVON.h"
#include "I2C.h"
#include "IRRemote/IRCommand.h"
#include "IRRemote/IRTransmitter.h"
#include "IRRemote/IRReceiver.h"
#include "Devices/GPIOExt/PCA9555.h"
#include "Devices/ShiftReg/74HC165.h"
#include "Devices/PWM/PWMWorker.h"
#include "Devices/ADC/PicoADC.h"
#include "Watchdog.h"

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
// two valid ways to satify this error check:
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


// global singleton
PinscapeVendorIfc psVendorIfc;

// WinUsb GUID for our vendor interface
const char *PinscapeVendorIfc::WINUSB_GUID = "{D3057FB3-8F4C-4AF9-9440-B220C3B2BA23}";

// construction
PinscapeVendorIfc::PinscapeVendorIfc()
{
}

// periodic task handler
void PinscapeVendorIfc::Task()
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

        case Request::SUBCMD_RESET_SAFEMODE:
            // Reset the Pico and restart the flash-resident software in Safe Mode
            picoReset.Reboot(false, PicoReset::BootMode::SafeMode);
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
void PinscapeVendorIfc::OnOutput(const uint8_t *buf, size_t len)
{
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

            // mark the time of the request
            curRequestTime = time_us_64();

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

// polling mode
void PinscapeVendorIfc::RunPollingMode()
{
    while (enterPollingMode)
    {
        // clear the polling mode
        enterPollingMode = false;
        
        // poll for new input with a timeout
        for (uint64_t tStop = time_us_64() + 2000 ; time_us_64() < tStop ; )
        {
            // check for a new request packet
            tud_task();
            if (tud_vendor_n_available(USBIfc::VENDORIFC_IDX_CONFIG) != 0)
            {
                // read and process the request
                uint8_t venbuf[64];
                if (uint32_t nBytes = tud_vendor_n_read(USBIfc::VENDORIFC_IDX_CONFIG, venbuf, sizeof(venbuf)); nBytes != 0)
                    OnOutput(venbuf, nBytes);
                
                // done with the extra polling
                break;
            }
        }
    }
}

// Check the current buffered request to determine if it's complete, and
// process it if so.
void PinscapeVendorIfc::TryProcessRequest()
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
void PinscapeVendorIfc::ProcessRequest()
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
            
            // set the unit number
            ids.unitNum = unitID.unitNum;

            // set the LedWiz unit number
            ids.ledWizUnitMask = unitID.ledWizUnitMask;

            // set the XInput player number, if known
            int xpn = xInput.GetPlayerNumber();
            ids.xinputPlayerIndex = xpn >= 0 ? static_cast<uint8_t>(xpn) : 0xFF;

            // copy the unit name into the transfer data
            uint8_t *dst = xferOut.data;
            uint8_t *dstMax = dst + sizeof(xferOut.data);
            for (const char *p = unitID.unitName.c_str() ; *p != 0 && dst+2 < dstMax ; *dst++ = *p++);
            *dst++ = 0;

            // copy the board name into the transfer data
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

        // Suspend output management.  This will turn off all ports
        // immediately, and will prevent turning them back on via port
        // control commands from the host or computed port updates.
        // This is an attempt to ensure that we don't leave any
        // timer-protected ports activated through the reboot.
        OutputManager::Suspend(0);

        // These commands must be deferred, to allow time for the reply
        // to be sent back to the host, and for output port updates to
        // complete.
        deferredRebootSubcmd = curRequest.args.argBytes[0];
        deferredRebootTime = time_us_64() + 100000;
        break;

    case Request::CMD_CONFIG:
        // Configuration commands
        {
            // proceed according to the subcommand
            auto &args = curRequest.args.config;
            switch (args.subcmd)
            {
            case Request::SUBCMD_CONFIG_GET:
                // Get configuration (device-to-host).  Transmits the current config
                // data to the client as the response transfer block.
                if (const uint8_t *p = ProcessGetConfig(resp); p != nullptr)
                    pXferOut = p;
                break;

            case Request::SUBCMD_CONFIG_PUT:
                // Put configuration (host-to-device).  Receives updated config data
                // via the request transfer block, and overwrites the current flash
                // contents with the updated config.
                ProcessPutConfig(resp);
                break;

            case Request::SUBCMD_CONFIG_EXISTS:
                resp.status = (config.ConfigFileExists(args.fileID) ? Response::OK : Response::ERR_NOT_FOUND);
                break;

            case Request::SUBCMD_CONFIG_ERASE:
                // erase the device-side configuration file
                if (!config.EraseConfig(args.fileID))
                    resp.status = Response::ERR_FAILED;
                break;

            case Request::SUBCMD_CONFIG_RESET:
                // Perform a factory reset by clearing the config data area in flash.
                resp.status = (config.FactoryReset() ? Response::OK : Response::ERR_FAILED);
                break;

            case Request::SUBCMD_CONFIG_TEST_CHECKSUM:
                // test a config file checksum
                ProcessTestConfig(resp);
                break;

            default:
                // invalid subcommand
                resp.status = Response::ERR_BAD_SUBCMD;
                break;
            }
        }
        break;

    case Request::CMD_TVON:
        // TV-ON commands - get the subcommand from the first byte
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_TVON_QUERY_STATE:
            // query TV ON state - populate the reply arguments
            resp.argsSize = sizeof(resp.args.tvon);
            tvOn.Populate(&resp.args.tvon);
            break;

        case Request::SUBCMD_TVON_SET_RELAY:
            // set the TV relay state, according to the second argument byte
            switch (curRequest.args.argBytes[1])
            {
            case Request::TVON_RELAY_OFF:
                tvOn.ManualSetRelay(false);
                break;
                
            case Request::TVON_RELAY_ON:
                tvOn.ManualSetRelay(true);
                break;
                
            case Request::TVON_RELAY_PULSE:
                tvOn.ManualPulseRelay();
                break;

            default:
                // ignore other argument values; this allows new values to be added
                // in the future with predictable results ("do nothing") when sent
                // to devices still running older firmware
                break;
            }
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
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

    case Request::CMD_FLASH_STORAGE:
        // Flash storage access commands.  Process according to the
        // sub-command code.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_FLASH_READ_SECTOR:
            // read a flash sector
            {
                // figure the transfer size
                size_t copySize = std::min(FLASH_SECTOR_SIZE, sizeof(xferOut.data));
                
                // validate the address - it has to be within the flash space
                auto &argsIn = curRequest.args.flash;
                uint32_t ofs = argsIn.ofs;
                if (ofs + copySize > PICO_FLASH_SIZE_BYTES)
                {
                    resp.status = Response::ERR_OUT_OF_BOUNDS;
                    break;
                }

                // copy the data
                resp.xferBytes = copySize;
                pXferOut = xferOut.data;
                const void *flashPtr = reinterpret_cast<const void*>(XIP_BASE + ofs);
                memcpy(xferOut.data, flashPtr, copySize);

                // calculate the CRC-32 of the data
                auto &argsOut = resp.args.flash;
                argsOut.crc32 = CRC::Calculate(flashPtr, copySize, CRC::CRC_32());
            }
            break;

        case Request::SUBCMD_FLASH_QUERY_FILESYS:
            // reply with a FlashFileSysInfo struct
            pXferOut = xferOut.data;
            resp.xferBytes = flashStorage.Populate(reinterpret_cast<PinscapePico::FlashFileSysInfo*>(xferOut.data), sizeof(xferOut.data));

            // a zero return means there wasn't enough space in our buffer
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_NUDGE:
        // Nudge commands.  Process according to the sub-command code
        // in the first argument byte.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_NUDGE_CALIBRATE:
            // begin calibration
            nudgeDevice.StartCalibration(curRequest.args.argBytes[1] == 1);
            break;

        case Request::SUBCMD_NUDGE_CENTER:
            // set the center point now
            nudgeDevice.RequestManualCentering();
            break;

        case Request::SUBCMD_NUDGE_QUERY_STATUS:
            // reply with a NudgeStatus struct
            pXferOut = xferOut.data;
            resp.xferBytes = nudgeDevice.Populate(reinterpret_cast<PinscapePico::NudgeStatus*>(xferOut.data), sizeof(xferOut.data));

            // a zero return means there wasn't enough space in our buffer
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_NUDGE_QUERY_PARAMS:
            // query nudge device parameters
            pXferOut = xferOut.data;
            resp.xferBytes = nudgeDevice.GetParams(reinterpret_cast<PinscapePico::NudgeParams*>(xferOut.data), sizeof(xferOut.data));
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_NUDGE_PUT_PARAMS:
            // set nudge device parameters
            if (!nudgeDevice.SetParams(reinterpret_cast<PinscapePico::NudgeParams*>(xferIn.data), xferIn.len))
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_NUDGE_COMMIT:
            // commit in-memory nudge settings to flash
            if (!nudgeDevice.CommitSettings())
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_NUDGE_REVERT:
            // reload nudge settings from flash
            if (!nudgeDevice.RestoreSettings())
                resp.status = Response::ERR_FAILED;
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_PLUNGER:
        // Plunger commands.  Process according to the sub-command code
        // in the first argument byte.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_PLUNGER_CALIBRATE:
            // Start plunger calibration.  Run a timed calibration, and auto-save
            // according to the low bit of the first argument byte.
            plunger.SetCalMode(true, (curRequest.args.plungerByte.b & 0x01) != 0);
            break;

        case Request::SUBCMD_PLUNGER_SET_JITTER_FILTER:
            // set a new jitter filter size
            plunger.SetJitterWindow(curRequest.args.jitterFilter.windowSize);
            break;

        case Request::SUBCMD_PLUNGER_SET_FIRING_TIME_LIMIT:
            // set a new jitter filter size
            plunger.SetFiringTimeLimit(curRequest.args.plungerInt.u);
            break;

        case Request::SUBCMD_PLUNGER_SET_INTEGRATION_TIME:
            // set a new integration time
            plunger.SetIntegrationTime(curRequest.args.plungerInt.u);
            break;

        case Request::SUBCMD_PLUNGER_SET_ORIENTATION:
            // set forward/reverse orientation
            plunger.SetReverseOrientation(curRequest.args.plungerByte.b != 0);
            break;

        case Request::SUBCMD_PLUNGER_SET_SCALING_FACTOR:
            // set the manual caliration scaling factor
            plunger.SetManualScalingFactor(curRequest.args.plungerInt.u);
            break;

        case Request::SUBCMD_PLUNGER_SET_SCAN_MODE:
            plunger.SetScanMode(curRequest.args.plungerByte.b);
            break;

        case Request::SUBCMD_PLUNGER_SET_CAL_DATA:
            // set the calibration data from the struct in the extra transfer data
            if (!plunger.SetCalibrationData(reinterpret_cast<PinscapePico::PlungerCal*>(xferIn.data), xferIn.len))
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_PLUNGER_COMMIT_SETTINGS:
            // commit the in-memory settings
            if (!plunger.CommitSettings())
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_PLUNGER_REVERT_SETTINGS:
            // Revert changes to the in-memory settings to the saved
            // settings.  Only report an error if the load fails and the
            // file exists, which indicates some actual file error occurred
            // trying to load it.  If the file doesn't exist, count it as a
            // success, since the absence of the file just means that we're
            // still using defaults, which isn't an error.
            {
                bool fileExists = false;
                if (!plunger.RestoreSettings(&fileExists) && fileExists)
                    resp.status = Response::ERR_FAILED;
            }
            break;

        case Request::SUBCMD_PLUNGER_QUERY_READING:
            // reply with a PlungerReading struct
            pXferOut = xferOut.data;
            resp.xferBytes = plunger.Populate(reinterpret_cast<PinscapePico::PlungerReading*>(xferOut.data), sizeof(xferOut.data));
            
            // a zero return means there wasn't enough space in our buffer
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_PLUNGER_QUERY_CONFIG:
            // reply with a PlungerConfig struct
            pXferOut = xferOut.data;
            resp.xferBytes = plunger.Populate(reinterpret_cast<PinscapePico::PlungerConfig*>(xferOut.data), sizeof(xferOut.data));

            // a zero return means there wasn't enough space in our buffer
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;
        
    case Request::CMD_BUTTONS:
        // Button commands.  The first argument byte is a subcommand
        // code specifying which action to perform.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_BUTTON_QUERY_DESCS:
            // query logical button descriptors through the button manager
            pXferOut = xferOut.data;
            if ((resp.xferBytes = Button::QueryDescs(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;
            
        case Request::SUBCMD_BUTTON_QUERY_STATES:
            // query logical button states through the button manager
            pXferOut = xferOut.data;
            {
                // query states
                uint32_t shiftState;
                if ((resp.xferBytes = Button::QueryStates(xferOut.data, sizeof(xferOut.data), shiftState)) == 0xFFFFFFFF)
                    resp.status = Response::ERR_FAILED;

                // fill in return arguments
                resp.argsSize = sizeof(resp.args.buttonState);
                resp.args.buttonState.globalShiftState = shiftState;
            }
            break;
            
        case Request::SUBCMD_BUTTON_QUERY_GPIO_STATES:
            // query GPIO physical port states
            pXferOut = xferOut.data;
            if ((resp.xferBytes = Button::QueryGPIOStates(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;
            
        case Request::SUBCMD_BUTTON_QUERY_PCA9555_STATES:
            // query PCA9555 physical port states
            pXferOut = xferOut.data;
            if ((resp.xferBytes = PCA9555::QueryInputStates(xferOut.data, sizeof(xferOut.data))) == 0xFFFFFFFF)
                resp.status = Response::ERR_FAILED;
            break;
            
        case Request::SUBCMD_BUTTON_QUERY_74HC165_STATES:
            // query 74HC165 physical port states
            pXferOut = xferOut.data;
            if ((resp.xferBytes = C74HC165::QueryInputStates(xferOut.data, sizeof(xferOut.data))) == 0xFFFFFFFF)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_BUTTON_QUERY_EVENT_LOG:
            // query the GPIO button event log for a given GPIO
            pXferOut = xferOut.data;
            if ((resp.xferBytes = Button::GPIOSource::QueryEventLog(xferOut.data, sizeof(xferOut.data), curRequest.args.argBytes[1])) == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_BUTTON_CLEAR_EVENT_LOG:
            // clear the GPIO button event log for a given GPIO
            if (!Button::GPIOSource::ClearEventLog(curRequest.args.argBytes[1]))
                resp.status = Response::ERR_FAILED;
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_OUTPUTS:
        // Button commands.  The first argument byte is a subcommand
        // code specifying which action to perform.
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_OUTPUT_SET_PORT:
            // set logical output port level
            OutputManager::Set(curRequest.args.argBytes[1], curRequest.args.argBytes[2]);
            break;

        case Request::SUBCMD_OUTPUT_TEST_MODE:
            // enter/exit output manager test mode (test mode suspends output
            // management, allowing the host to directly address device-level
            // ports via SUBCMD_OUTPUT_SET_DEVICE_PORT)
            {
                const auto &a = curRequest.args.outputTestMode;
                if (a.enable == 1)
                    OutputManager::Suspend(a.timeout_ms);
                else
                    OutputManager::Resume();
            }
            break;

        case Request::SUBCMD_OUTPUT_SET_DEVICE_PORT:
            // set a physical device output port level, bypassing the output
            // manager; test mode must be enabled
            {
                const auto &a = curRequest.args.outputDevPort;
                OutputManager::SetDevicePortLevel(a.devType, a.configIndex, a.port, a.pwmLevel);
            }
            break;

        case Request::SUBCMD_OUTPUT_PWMWORKER_RESET:
        case Request::SUBCMD_OUTPUT_PWMWORKER_BOOTLOADER:
            // reboot a PWMWorker Pico
            PWMWorker::RebootRemotePico(curRequest.args.argBytes[1], curRequest.args.argBytes[0] == Request::SUBCMD_OUTPUT_PWMWORKER_BOOTLOADER);
            break;

        case Request::SUBCMD_OUTPUT_QUERY_LOGICAL_PORTS:
            // query logical output port descriptors
            pXferOut = xferOut.data;
            if ((resp.xferBytes = OutputManager::QueryLogicalPortDescs(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_OUTPUT_QUERY_LOGICAL_PORT_NAME:
            // query logical port name
            pXferOut = xferOut.data;
            if ((resp.xferBytes = OutputManager::QueryLogicalPortName(xferOut.data, sizeof(xferOut.data), curRequest.args.argBytes[1])) == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_OUTPUT_QUERY_DEVICES:
            // query physical output device descriptors
            pXferOut = xferOut.data;
            if ((resp.xferBytes = OutputManager::QueryDeviceDescs(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_OUTPUT_QUERY_DEVICE_PORTS:
            // query physical output port descriptors
            pXferOut = xferOut.data;
            if ((resp.xferBytes = OutputManager::QueryDevicePortDescs(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;
            
        case Request::SUBCMD_OUTPUT_QUERY_LOGICAL_PORT_LEVELS:
            // query logical output port levels
            pXferOut = xferOut.data;
            if ((resp.xferBytes = OutputManager::QueryLogicalPortLevels(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_OUTPUT_QUERY_DEVICE_PORT_LEVELS:
            // query physical output port levels
            pXferOut = xferOut.data;
            if ((resp.xferBytes = OutputManager::QueryDevicePortLevels(xferOut.data, sizeof(xferOut.data))) == 0)
                resp.status = Response::ERR_FAILED;
            break;
            
        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_QUERY_USBIFCS:
        // get USB Interface configuration
        {
            // set up the reporting struct
            PinscapePico::USBInterfaces u{ sizeof(u) };

            // set the VID/PID
            u.vid = usbIfc.GetVID();
            u.pid = usbIfc.GetPID();

            // set the flag bits
            if (keyboard.configured) u.flags |= u.F_KEYBOARD_CONF;
            if (keyboard.enabled) u.flags |= u.F_KEYBOARD_ENA;
            if (gamepad.configured) u.flags |= u.F_GAMEPAD_CONF;
            if (gamepad.enabled) u.flags |= u.F_GAMEPAD_ENA;
            if (xInput.enabled) u.flags |= u.F_XINPUT_CONF;
            if (xInput.reportsEnabled) u.flags |= u.F_XINPUT_ENA;
            if (openPinballDevice.configured) u.flags |= u.F_PINDEV_CONF;
            if (openPinballDevice.enabled) u.flags |= u.F_PINDEV_ENA;
            if (feedbackController.configured) u.flags |= u.F_FEEDBACK_CONF;
            if (ledWizIfc.configured) u.flags |= u.F_LEDWIZ_CONF;
            if (usbcdc.IsConfigured()) u.flags |= u.F_CDC_CONF;

            // copy the struct to the transfer data
            pXferOut = SetXferOut(resp, &u, sizeof(u));
        }
        break;

    case Request::CMD_QUERY_GPIO_CONFIG:
        // get GPIO configuration
        {
            // write directly into the transfer buffer
            pXferOut = xferOut.data;
            
            // Set up the GPIOConfig struct.  This will go at the start of the transfer
            // buffer, followed immediately by the name strings.
            PinscapePico::GPIOConfig gc{ sizeof(gc), sizeof(gc.port[0]), _countof(gc.port) };
            uint16_t ofs = sizeof(gc);

            // populate the ports
            for (int gpnum = 0 ; gpnum < static_cast<int>(_countof(gc.port)) ; ++gpnum)
            {
                // set the function
                auto func = static_cast<uint8_t>(gpio_get_function(gpnum));
                gc.port[gpnum].func = func;

                // if it's a GPIO (function code SIO), set the OUT flag if it's set as an output port
                if (func == GPIO_FUNC_SIO && gpio_is_dir_out(gpnum))
                    gc.port[gpnum].flags |= PinscapePico::GPIOConfig::Port::F_DIR_OUT;

                // Check for ADC input mode.  The hardware multiplexer doesn't have a
                // function code for ADC; the ADC will simply read from whatever pin
                // in the ADC range you direct it to.  So we have to ask our own ADC
                // resource manager if the pin is assigned as an ADC input.
                if (PicoADC::GetInst()->IsADCGPIO(gpnum))
                    gc.port[gpnum].flags |= PinscapePico::GPIOConfig::Port::F_ADC;

                // Get the usage string.  If it's not null, add it to the transfer
                // in the string pool section after the end of the struct.  If it's
                // null, leave the offset in the struct as zero to indicate that
                // there's no string here.
                const char *usage = gpioManager.GetUsage(gpnum);
                size_t rem = sizeof(xferOut.data) - ofs;
                if (usage != nullptr)
                {
                    // make sure there's room for it
                    if (size_t len = strlen(usage) + 1; len <= rem)
                    {
                        // set the offset
                        gc.port[gpnum].usageOfs = ofs;
                        
                        // copy the string, advance the offset past it
                        memcpy(&xferOut.data[ofs], usage, len);
                        ofs += len;
                    }
                }
            }

            // copy the main struct to the start of the transfer buffer
            memcpy(xferOut.data, &gc, sizeof(gc));

            // set the transfer length to include all of the strings
            resp.xferBytes = ofs;
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
                PinscapePico::Statistics s{ sizeof(s) };
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

        case Request::SUBCMD_STATS_QUERY_CLOCK:
            // Get the Pico system clock time, returned in the reply arguments as a
            // little-endian UINT64.
            {
                // returning 16 bytes (2 x packed UINT64)
                resp.argsSize = 16;

                // Pack the start time of the request, and the reply time.
                // We send the reply immediately after we finish building the
                // arguments, so the reply time is "now", to a few microseconds
                // precision.
                uint8_t *p = resp.args.argBytes;
                PutUInt64(p, curRequestTime);
                PutUInt64(p, time_us_64());
            }
            break;

        case Request::SUBCMD_STATS_PREP_QUERY_CLOCK:
            // Prepare for an immediate QUERY CLOCK command.  This enters polling
            // mode for a few milliseconds, polling for a new vendor command, so
            // that we can handle it immediately without traversing the main loop.
            enterPollingMode = true;
            break;

        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_SEND_IR:
        // send an ad hoc IR command
        {
            auto &ir = curRequest.args.sendIR;
            if (!irTransmitter.QueueCommand(IRCommandDesc(ir.protocol, ir.code, ir.flags), ir.count))
                resp.status = Response::ERR_FAILED;
        }
        break;

    case Request::CMD_SET_CLOCK:
        // set the wall clock time
        {
            auto &c = curRequest.args.clock;
            timeOfDay.SetTime(DateTime{ c.year, c.month, c.day, c.hour, c.minute, c.second }, true);
        }
        break;

    case Request::CMD_QUERY_IR:
        // IR queries - process according to the subcommand byte
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_QUERY_IR_CMD:
            // query recent decoded commands
            pXferOut = xferOut.data;
            resp.xferBytes = IRQueryCmd(xferOut.data, sizeof(xferOut.data));

            // a zero return means there wasn't enough space in our buffer
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;

        case Request::SUBCMD_QUERY_IR_RAW:
            // query recent raw IR pulses
            pXferOut = xferOut.data;
            resp.xferBytes = IRQueryRaw(xferOut.data, sizeof(xferOut.data));

            // a zero return means there wasn't enough space in our buffer
            if (resp.xferBytes == 0)
                resp.status = Response::ERR_FAILED;
            break;
            
        default:
            // invalid subcommand
            resp.status = Response::ERR_BAD_SUBCMD;
            break;
        }
        break;

    case Request::CMD_SYNC_CLOCKS:
        // clock sync request
        resp.status = ProcessClockSync(curRequest.args.timeSync, resp.args.timeSync);
        break;

    case Request::CMD_DEBUG:
        // miscellaneous debug commands
        switch (curRequest.args.argBytes[0])
        {
        case Request::SUBCMD_DEBUG_I2C_BUS_SCAN:
            // initiate a bus scan on each valid bus
            I2C::ForEach([](I2C *i2c) { i2c->StartBusScan(); });
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

// Process a TEST CONFIG CHECKSUM request
void PinscapeVendorIfc::ProcessTestConfig(Response &resp)
{
    // Compute the checksum of the config data area, and test it against
    // the stored checksum.

    // presume failure, with a zero checksum
    resp.status = Response::ERR_CONFIG_INVALID;
    resp.argsSize = sizeof(resp.args.checksum);
    resp.args.checksum = 0;

    // config file to check
    uint8_t fileID = curRequest.args.config.fileID;
    
    // if the file doesn't exist, return status NOT FOUND
    if (!config.ConfigFileExists(fileID))
    {
        resp.status = Response::ERR_NOT_FOUND;
        return;
    }
    
    // pass back the stored checksum, and run an integrity check
    uint32_t ck = 0;
    if (config.IsConfigFileValid(fileID, &ck))
    {
        // valid - store the returned checksum
        resp.args.checksum = ck;
        resp.status = Response::OK;
    }
}

// Process a GET CONFIG request
const uint8_t *PinscapeVendorIfc::ProcessGetConfig(Response &resp)
{
    // get the arguments
    auto &args = curRequest.args.config;

    // test if the file exists
    if (!config.ConfigFileExists(args.fileID))
    {
        resp.status = Response::ERR_NOT_FOUND;
        return nullptr;
    }

    // get the config file pointer
    size_t size;
    const char *p = config.GetConfigFileText(size, args.fileID);
    if (p == nullptr)
    {
        // no config data available
        resp.status = Response::ERR_CONFIG_INVALID;
        return nullptr;
    }
    
    // figure the requested offset
    size_t ofs = args.page * Request::CONFIG_PAGE_SIZE;
    if (ofs >= size)
    {
        // this is past the end of the file - return EOF
        resp.status = Response::ERR_EOF;
        return nullptr;
    }

    // Transfer back the page.  The config file text has session
    // lifetime, so we can safely pass back a pointer directly
    // into it for the transfer.
    resp.status = Response::OK;
    resp.xferBytes = std::min(static_cast<uint32_t>(size - ofs), Request::CONFIG_PAGE_SIZE);
    return reinterpret_cast<const uint8_t*>(p + ofs);
}

// Process a PUT CONFIG request
void PinscapeVendorIfc::ProcessPutConfig(Response &resp)
{
    // extend the watchdog time - this can be fairly time-consuming
    WatchdogTemporaryExtender wte(2500);

    // if it's been too long since the last Put Config call, forget the state
    // of the last attempt
    uint64_t now = time_us_64();
    if (now > tPutConfig + 30000000)
    {
        // it's been too long - forget the previous state
        putConfigPrevPageNo = -1;
        putConfigPrevPageChecksum = 0;
    }

    // update the timestamp of the last Put Config request
    tPutConfig = now;

    // get the arguments
    auto &args = curRequest.args.config;

    // If the page number is set to 0xFFFF, this is the special START OF FILE
    // packet, which doesn't have any file data associated with it.  This
    // packet's purpose is to reset the protocol, so that we know that this
    // is a new file rather than a resend of the first page of a prior file.
    if (args.page == 0xFFFF)
    {
        // reset the protocol state and return success
        putConfigPrevPageNo = -1;
        putConfigPrevPageChecksum = 0;
        return;
    }

    // calculate the page checksum
    uint32_t pageChecksum = CRC::Calculate(xferIn.data, xferIn.len, CRC::CRC_32());

    // If this is a repeat of the same page as last time, return success
    // without actually writing the page.  The host is allowed to send the
    // same page multiple times, to allow for the possibility that the
    // host doesn't receive our acknowledgment (or times out waiting for
    // the reply) and decides to send a retry.
    if (args.page == putConfigPrevPageNo && pageChecksum == putConfigPrevPageChecksum)
    {
        // it's a repeat - return RETRY_OK indication
        Log(LOG_INFO, "Vendorifc: PUT_CONFIG page retry OK (page %d, CRC32 %08lx)\n", args.page, pageChecksum);
        resp.status = Response::ERR_RETRY_OK;
        return;
    }

    // if the page is out of order, it's an error
    int expectedPageNo = (putConfigPrevPageNo < 0 ? 0 : putConfigPrevPageNo + 1);
    if (args.page != expectedPageNo)
    {
        Log(LOG_ERROR, "VendorIfc: PUT_CONFIG pages out of order; host sent page %d, expected %d\n", args.page, expectedPageNo);
        resp.status = Response::ERR_OUT_OF_BOUNDS;
        return;
    }

    // add this page into the CRC-32 calculation
    putConfigChecksum = (args.page == 0) ?
        CRC::Calculate(xferIn.data, xferIn.len, CRC::CRC_32()) :
        CRC::Calculate(xferIn.data, xferIn.len, CRC::CRC_32(), putConfigChecksum);
                         
    // update the internal counters for the page
    putConfigPrevPageNo = args.page;
    putConfigPrevPageChecksum = pageChecksum;

    // check for the last page
    if (args.page + 1 == args.nPages)
    {
        // we have the full stream now, so validate the checksum
        if (putConfigChecksum != args.crc)
        {
            // checksum mismatch
            Log(LOG_ERROR, "Vendorifc: PUT_CONFIG CRC32 mismatch; host %08lx, device %08lx\n", args.crc, putConfigChecksum);

            // Clear the page counters, since there's no point in allowing a retry.
            // A retry can only send the same page contents, and the page contents
            // add up to an invalid overall file, so a retry will necessarily have
            // the same mismatch.
            putConfigPrevPageNo = -1;
            putConfigPrevPageChecksum = 0;

            // return failure
            resp.status = Response::ERR_CONFIG_INVALID;
            return;
        }
    }

    // check for PUT CONFIG debug mode
    if (auto delay = G_debugAndTestVars.putConfigDelay; delay != 0)
    {
        // delay for the specified time
        Log(LOG_DEBUG, "Simulating PUT_CONFIG delay of %llu us for page %d\n", delay, args.page);
        for (uint64_t tEnd = time_us_64() + delay ; time_us_64() < tEnd ; )
        {
            // delay a bit, and let the watchdog know this is intention
            sleep_us(1000);
            watchdog_update();
        }

        // return a success indication, but without actually storing anything,
        // so that the host can run this test without risk of overwriting the
        // actual config file in flash
        Log(LOG_DEBUG, "PUT_CONFIG delay done; returning success with no data written to flash\n");
        return;
    }

    // write the page to flash
    if (!config.SavePage(xferIn.data, xferIn.len, args.page, args.nPages, putConfigChecksum, args.fileID))
    {
        // return failure status
        resp.status = Response::ERR_FAILED;

        // clear the internal state - a retry for this page won't work, since
        // it wasn't written successfully in the first place
        putConfigPrevPageNo = -1;
        putConfigPrevPageChecksum = 0;
    }
}

// clear the current request
void PinscapeVendorIfc::ClearCurRequest()
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
// Host clock synchronization
//

// Process a clock sync request (CMD_SYNC_CLOCKS)
int PinscapeVendorIfc::ProcessClockSync(const Request::Args::TimeSync &req, Response::Args::TimeSync &resp)
{
    // check for special frame numbers, which indicate subcommands
    // rather than regular clock synchronization requests
    switch (req.usbFrameNumber)
    {
    case Request::Args::TimeSync::ENABLE_FRAME_TRACKING:
        // enable frame tracking - install the interrupt handler if not already enabled
        if (!irqInstalled)
        {
            // Add our shared USB IRQ handler.  TinyUSB installs a shared handler
            // at highest priority.  We don't really care about the priority, since
            // all we care about is noting the time of SOF changes, which we can
            // detect by comparing to the previous SOF value.
            //
            // Note that we don't have to worry about enabling the interrupt,
            // because TinyUSB has to do that for its own purposes. 
            irq_add_shared_handler(USBCTRL_IRQ, &USBIRQ, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
            irqInstalled = true;

            // Enable the client user-mode SOF callback.  We don't actually use
            // this to intercept the SOF events, since the user-mode callback
            // is handled in the task queue processing, which can have latency
            // up to our main loop time.  We instead intercept the interrupt
            // directly so that we can capture the time with about 1us accuracy.
            // HOWEVER, TinyUSB optimizes its own interrupt overhead by disabling
            // the SOF interrupt at the hardware level when no one is using it.
            // And the way to tell TinyUSB that we depend upon the interrupt is
            // to enable the callback.
            usbIfc.EnableSOFInterrupt(USBIfc::SOFClientClockSync, true);
        }
        return Response::OK;

    case Request::Args::TimeSync::DISABLE_FRAME_TRACKING:
        // disable frame tracking - uninstall the interrupt handler if installed
        if (irqInstalled)
        {
            // Remove the handler.  DON'T disable the IRQ, because TinyUSB still
            // needs it for its own handler.
            irq_remove_handler(USBCTRL_IRQ, &USBIRQ);
            irqInstalled = false;

            // We no longer need the SOF callback.  This lets TinyUSB selectively
            // disable the SOF interrupt at the hardware level when it's not using
            // the signal for anything internally, which reduces the CPU load of
            // handling unnecessary interrupts.
            usbIfc.EnableSOFInterrupt(USBIfc::SOFClientClockSync, false);
        }
        return Response::OK;
        
    default:
        // Regular request
        {
            // validate the frame number
            if (req.usbFrameNumber > 2047)
                return Response::ERR_BAD_PARAMS;

            // if we're not tracking SOFs (i.e., the IRQ handler isn't
            // installed), we can't satisfy the request
            if (!irqInstalled)
                return Response::ERR_NOT_READY;

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
            // 1ms, so it rolls over every 2.048 seconds.  Fortunately, we know for
            // certain that the host frame number must always be EQUAL TO OR LESS
            // THAN the current Pico frame number, because the host can't send us
            // messages from the future.  If the host frame number is nominally
            // higher than the Pico frame number, it means that the Pico number
            // rolled over from 2047 to 0 since the host frame number was recorded.
            // (This of course assumes that the frame is within the past 2.048
            // seconds, so that it hasn't rolled over more than once.  That's
            // always true as long as the caller captures the USB frame information
            // immediatly before sending the request.)
            int dFrame = static_cast<int>(picoCurFrameNum) - static_cast<int>(req.usbFrameNumber);
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
            resp.picoClockAtSof = picoCurFrameTime - dt;

            // Calculate the host clock offset, such that:
            //
            //   T[Host] = T[Pico] + hostClockOffset
            //
            // This allows us to the calculate the time on the host clock
            // corresponding to any given time on the Pico clock.
            hostClockOffset = static_cast<int64_t>(req.hostClockAtSof - resp.picoClockAtSof);
        }
        return Response::OK;
    }
}

// USB IRQ handler - installed when start-of-frame time tracking
// is enabled via CMD_SYNC_CLOCKS
void PinscapeVendorIfc::USBIRQ()
{
    // if the USB hardware frame number has changed, note the timestamp
    uint16_t frameCounter = usb_hw->sof_rd & USB_SOF_RD_BITS;
    if (frameCounter != psVendorIfc.usbFrameCounter)
    {
        psVendorIfc.usbFrameCounter = frameCounter;
        psVendorIfc.tUsbFrameCounter = time_us_64();
    }
}

// ---------------------------------------------------------------------------
// 
// IR capture
//

void PinscapeVendorIfc::ConfigureIR(JSONParser &json)
{
    // if the IR receiver is configured, allocate space for raw pulse capture
    if (irReceiver.IsConfigured())
        irPulse.resize(128);

    // subscribe for notifications
    irReceiver.Subscribe(this, true);
}

void PinscapeVendorIfc::OnIRCommandReceived(const IRCommandReceived &command, uint64_t dt)
{
    // add the command to our circular buffer
    irCmd[irCmdWrite] = { command, dt };

    // wrap the pointer
    if (++irCmdWrite >= IRCMD_BUF_SIZE)
        irCmdWrite = 0;

    // bump the read pointer if we collided
    if (irCmdWrite == irCmdRead)
    {
        if (++irCmdRead >= IRCMD_BUF_SIZE)
            irCmdRead = 0;
    }
}

void PinscapeVendorIfc::OnIRPulseReceived(int time_us, bool mark)
{
    // Clear the pulse buffer if it's been a while since the last pulse.
    // That suggests that this is a new command after a quiescent period.
    uint64_t now = time_us_64();
    if (now - tLastIRPulse > 2500000)
        irPulseWrite = 0;

    // record the time
    tLastIRPulse = now;

    // if the buffer is full, discard the new pulse
    if (irPulseWrite >= irPulse.size())
        return;

    // re-encode the pulse in the IRReceiver format: mark/space in the
    // low bit, time in 4us intervals in the high 15 bits
    irPulse[irPulseWrite++] =
        ((time_us < 0 || time_us > 0x7FFF) ? 0xFFFE : (((time_us + 2) >> 1)  & 0xFFFE))
        | (mark ? 0x0001 : 0x0000);
}

size_t PinscapeVendorIfc::IRQueryCmd(uint8_t *buf, size_t bufsize)
{
    // make sure the buffer is big enough for at least the header
    using IRCommandList = PinscapePico::IRCommandList;
    using IRCommandListEle = PinscapePico::IRCommandListEle;
    if (bufsize < sizeof(IRCommandList))
        return 0;

    // figure how many elements we can fit in the buffer
    size_t nMax = (bufsize - sizeof(IRCommandList)) / sizeof(IRCommandListEle);

    // figure the number of elements present
    size_t nAvail = (irCmdWrite >= irCmdRead) ?
                    irCmdWrite - irCmdRead :
                    irCmdWrite + IRCMD_BUF_SIZE - irCmdRead;

    // copy as many pulses as we can fit, up to the number we actually have
    size_t nCopy = std::min(nMax, nAvail);

    // figure the result size
    size_t resultSize = sizeof(IRCommandList);

    // clear the buffer
    memset(buf, 0, resultSize);

    // pouplate the header
    IRCommandList *lst = reinterpret_cast<IRCommandList*>(buf);
    lst->cb = sizeof(IRCommandList);
    lst->cbEle = sizeof(IRCommandListEle);;
    lst->numEle = 0;

    // populate the commands
    IRCommandListEle *dst = reinterpret_cast<IRCommandListEle*>(lst + 1);
    for (size_t i = 0 ; i < nCopy && irCmdRead != irCmdWrite ;
         ++i, ++dst, ++lst->numEle, resultSize += sizeof(IRCommandListEle))
    {
        auto &src = irCmd[irCmdRead];
        memset(dst, 0, sizeof(IRCommandListEle));

        dst->dt = src.dt;
        dst->cmd = src.cmd.code;
        dst->protocol = src.cmd.proId;
        if (src.cmd.useDittos) dst->proFlags |= dst->FPRO_DITTOS;

        if (src.cmd.hasToggle) dst->cmdFlags |= dst->F_HAS_TOGGLE;
        if (src.cmd.toggle) dst->cmdFlags |= dst->F_TOGGLE_BIT;
        if (src.cmd.hasDittos) dst->cmdFlags |= dst->F_HAS_DITTO;
        if (src.cmd.ditto) dst->cmdFlags |= dst->F_DITTO_FLAG;
        if (src.cmd.isAutoRepeat) dst->cmdFlags |= dst->F_AUTOREPEAT;

        switch (src.cmd.position)
        {
        case IRCommandReceived::Position::First:  dst->cmdFlags |= dst->F_POS_FIRST; break;
        case IRCommandReceived::Position::Middle: dst->cmdFlags |= dst->F_POS_MIDDLE; break;
        case IRCommandReceived::Position::Last:   dst->cmdFlags |= dst->F_POS_LAST; break;
        }

        // bump the read pointer, wrap at end
        if (++irCmdRead >= IRCMD_BUF_SIZE)
            irCmdRead = 0;
    }

    // return the populated size
    return resultSize;
}

size_t PinscapeVendorIfc::IRQueryRaw(uint8_t *buf, size_t bufsize)
{
    // make sure the buffer is big enough for at least the header
    using IRRawList = PinscapePico::IRRawList;
    using IRRaw = PinscapePico::IRRaw;
    if (bufsize < sizeof(IRRawList))
        return 0;

    // figure how many pulses we can fit in the buffer
    size_t nMax = (bufsize - sizeof(IRRawList)) / sizeof(IRRaw);

    // copy as many pulses as we can fit, up to the number we actually have
    size_t nCopy = std::min(nMax, irPulseWrite);

    // figure the result size
    size_t resultSize = sizeof(IRRawList) + nCopy*sizeof(IRRaw);

    // clear the buffer
    memset(buf, 0, resultSize);

    // pouplate the header
    IRRawList *lst = reinterpret_cast<IRRawList*>(buf);
    lst->cb = sizeof(IRRawList);
    lst->cbRaw = sizeof(IRRaw);
    lst->numRaw = static_cast<uint16_t>(nCopy);

    // populate the pulses
    IRRaw *raw = reinterpret_cast<IRRaw*>(lst + 1);
    for (size_t i = 0 ; i < nCopy ; ++i, ++raw)
    {
        auto t = irPulse[i];
        raw->t = (t & 0xFFFE);
        if (raw->t == 0xFFFE) raw->t = 0xFFFF;  // translate our internal max to 0xFFFF on the wire
        raw->type = static_cast<uint8_t>(t & 0x0001);
    }

    // if we didn't copy everything, move the remaining samples to the bottom of the buffer
    if (nCopy < irPulseWrite)
        memcpy(irPulse.data(), irPulse.data() + nCopy, (irPulseWrite - nCopy)*sizeof(irPulse[0]));

    // deduct the samples sent from the remaining samples
    irPulseWrite -= nCopy;

    // return the populated size
    return resultSize;
}


// ---------------------------------------------------------------------------
// 
// Vendor interface transfer buffer
//

void PinscapeVendorIfc::XferBuf::Append(const uint8_t *src, size_t copyLen)
{
    // limit the copy to the remaining buffer space
    size_t avail = sizeof(data) - len;
    if (copyLen > avail)
        copyLen = avail;

    // copy the data and update the length
    memcpy(data + len, src, copyLen);
    len += copyLen;
}

