// Pinscape Pico - Led Wiz protocol USB HID interface, for legacy LedWiz software
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <ctype.h>
#include <unordered_map>
#include <vector>
#include <functional>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/unique_id.h>

// project headers
#include "USBIfc.h"
#include "Utils.h"
#include "Pinscape.h"
#include "Main.h"
#include "Logger.h"
#include "Config.h"
#include "JSON.h"
#include "Outputs.h"

// HID device singleton
USBIfc::LedWizIfc ledWizIfc;

// JSON configuration
bool USBIfc::LedWizIfc::Configure(JSONParser &json)
{
    // presume disabled
    bool enabled = false;

    // check for the JSON key
    if (auto *val = json.Get("ledWizProtocol") ; !val->IsUndefined())
    {
        // get the enable status
        enabled = val->Get("enable")->Bool();
    }

    // return the 'enabled' status
    return enabled;
}

// initialize
void USBIfc::LedWizIfc::Init()
{
}

// Report Descriptor.  The LedWiz protocol has an opaque 8-byte output report,
// and no input report.  Importantly, the original LedWiz protocol doesn't use
// HID report IDs, and some legacy software hard-codes the absence of report IDs
// as an assumption, so we can't use them either.
const uint8_t *USBIfc::LedWizIfc::GetReportDescriptor(uint16_t *byteLength)
{
    static const uint8_t desc[] = {
        HID_USAGE_PAGE (HID_USAGE_PAGE_DESKTOP),    // usage page Generic Desktop (0x01)
        HID_USAGE      (0),                         // usage undefined (0x00), for our custom type
        HID_COLLECTION (HID_COLLECTION_APPLICATION),

            // OUTPUT (host-to-device) - 8 bytes of private protocol data
            HID_USAGE         (0),               // undefined (opaque data for application-specific use)
            HID_REPORT_SIZE   (8),               // 8-bit bytes
            HID_REPORT_COUNT  (8),               // x 8 elements
            HID_OUTPUT        (HID_ARRAY),       // output (host-to-device), array

        HID_COLLECTION_END 
    };
    *byteLength = sizeof(desc);
    return desc;
}

uint16_t USBIfc::LedWizIfc::GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // we don't have any IN reports
    return 0;
}

void USBIfc::LedWizIfc::SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen)
{
    // LedWiz protocol commands are 8 bytes
    if (reqLen != 8)
        return;

    // The first byte determines the command code
    //
    //   0-49      -> PBA
    //   64        -> SBA
    //   129-132   -> PBA
    //
    // Others are undefined
    //
    if (buf[0] == 64)
    {
        // SBA
        // 64 <bank0> <bank1> <bank2> <bank3> <period>
        //
        // bankN = on/off state for a bank of 8 ports, starting at port 0
        // for the low bit of bank0
        //
        // period = waveform cycle period, in 250ms units, valid valies are 1..7

        // extract the period, limiting to 1..7
        uint8_t period = std::max(1, (buf[5] & 0x07));

        // process the four 'bank' bytes
        const uint8_t *pBank = &buf[1];
        for (int bank = 0, port = 1 ; bank < 4 ; ++bank)
        {
            // process the 8 ports for this 'bank' byte
            uint8_t b = *pBank++;
            for (int i = 0 ; i < 8 ; ++i, b >>= 1, ++port)
                OutputManager::SetLedWizSBA(port, (b & 0x01) != 0, period);
        }

        // reset the internal PBA index to 0 on each SBA
        pbaIndex = 0;
    }
    else if (buf[0] <= 49 || (buf[0] >= 129 && buf[0] <= 132))
    {
        // PBA - each byte is a profile setting for one port,
        // starting at the current PBA index port
        const uint8_t *p = &buf[0];
        for (int i = 0 ; i < 8 ; ++i)
            OutputManager::SetLedWizPBA(++pbaIndex, *p++);

        // wrap the PBA index after 32 prots
        pbaIndex &= 0x1F;
    }
}
