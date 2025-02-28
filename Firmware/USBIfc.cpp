// Pinscape Pico - USB interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines the Pinscape Pico's USB interface, which includes a
// configurable set of HID devices (keyboard, gamepad, feedback
// controller), a configuration XBox controller emulation (as an
// alternative to the HID gamepad interface), a custom vendor interface
// for configuration and control functions, and a USB CDC (virtual COM
// port) for logging, debugging, and troubleshooting.
//
// USB functionality is implemented via the Tinyusb library that's
// included in the Pico SDK.


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
#include <tusb.h>
#include <device/usbd_pvt.h>
#include <class/hid/hid_device.h>

// project headers
#include "../OpenPinballDevice/OpenPinballDeviceReport.h"
#include "USBIfc.h"
#include "Utils.h"
#include "Pinscape.h"
#include "Logger.h"
#include "XInput.h"
#include "USBCDC.h"
#include "VendorIfc.h"
#include "Config.h"
#include "JSON.h"
#include "PicoLED.h"
#include "NightMode.h"
#include "Version.h"
#include "Outputs.h"
#include "TimeOfDay.h"
#include "TVON.h"
#include "Nudge.h"
#include "Plunger/Plunger.h"
#include "CommandConsole.h"



// global singleton instance of the USB interface
USBIfc usbIfc;

// USB main interface construction
USBIfc::USBIfc()
{
    // USB statistics console command
    CommandConsole::AddCommand(
        "hidstats", "show HID status and statistics",
        "hidstats [options]\n"
        "Options:\n"
        "   -a, --all     show stats for all devices\n"
        "\n"
        "With no options, shows a summary for each interface.\n",
        &Command_usbstats);
}

// USB main interface destruction
USBIfc::~USBIfc()
{
}

// 'usbstats' console command
void USBIfc::Command_usbstats(const ConsoleCommandContext *c)
{
    if (c->argc == 1)
    {
        // show a per-interface summary
        c->Printf(
            "USB status:  %s\n",
            usbIfc.IsMounted() ? (usbIfc.IsSuspended() ? "Suspended (host sleep mode)" : "Active") : "Dismounted");
        
        uint64_t now = time_us_64();
        auto &ifcs = usbIfc.GetHIDIfcs();
        if (ifcs.size() != 0)
        {
            c->Printf("HID interfaces:\n");
            for (auto &ifc : ifcs)
            {
                c->Printf(
                    "  HID %d:\n"
                    "  Devices:           ",
                    ifc->instance);
                
                for (auto &dev : ifc->devices)
                    c->Printf("%s ", dev->name);
                c->Printf("\n");
                
                c->Printf(
                    "    Status:          %s\n"
                    "    Device Sending:  %s\n"
                    "    Send start time: %llu us (%llu ms ago)\n"
                    "    Send completion: %llu us (%llu ms ago)\n",
                    tud_hid_n_ready(ifc->instance) ? "Ready" : "Busy",
                    ifc->deviceSending < 0 ? "None" : ifc->devices[ifc->deviceSending]->name,
                    ifc->tSendStart, (now - ifc->tSendStart) / 1000,
                    ifc->tSendComplete, (now - ifc->tSendComplete) / 1000);
            }
        }
        else
            c->Print("No HID interfaces\n");
    }
    else
    {
        for (int i = 1 ; i < c->argc ; ++i)
        {
            const char *a = c->argv[i];
            if (strcmp(a, "-a") == 0 || strcmp(a, "--all") == 0)
            {
                auto &ifcs = usbIfc.GetHIDIfcs();
                int n = 0;
                for (auto &ifc : ifcs)
                {
                    c->Printf("%s=== HID %d ===\n", n++ != 0 ? "\n" : "", ifc->instance);
                    for (auto &dev : ifc->devices)
                    {
                        c->Printf("%s:\n", dev->name);
                        dev->stats.Log(c, dev);
                    }
                }

                if (ifcs.size() == 0)
                    c->Printf("No USB interfaces\n");
            }
            else
            {
                return c->Printf("usbstats: unknown option \"%s\"\n", a);
            }
        }
    }   
}


// Configure the USB interface from the JSON config data
void USBIfc::Configure(JSONParser &json)
{
    // get the VID and PID from the configuration
    vid = json.Get("usb.vid")->UInt16(0x1209);  // pid.codes open-source VID
    pid = json.Get("usb.pid")->UInt16(0xEAEB);  // pid.codes assignment for Pinscape Pico
}

// Initialize the USB interface.  This completes initialization of our
// internal structures based on the loaded configuration data, so the
// configuration for all related classes (such as the various HID device
// types) must be completed first.  The routine also initializes the
// Tinyusb library, which will set up the USB connection to the host.
// No communications with the host can be attempted before this
// initialization is done.
void USBIfc::Init()
{
    // set up our string descriptors
    AddStringDescriptorBytes(STRDESC_LANG, (const uint8_t[]){ 0x09, 0x04 }, 2);     // HID - Language (English, 0x0409)
    AddStringDescriptorText(STRDESC_MANUF, "Pinscape Labs");                        // HID - Manufacturer Name
    AddStringDescriptorText(STRDESC_PRODUCT, "PinscapePico");                       // HID - Product Name
    AddStringDescriptorText(STRDESC_VENIFC, "PinscapePicoControl");                 // Vendor interface display name
    AddStringDescriptorText(STRDESC_CDCIFC, "Pinscape Pico Terminal");              // CDC interface display name
    AddStringDescriptorText(STRDESC_FEEDBACK_LBL, "PinscapeFeedbackController/1");  // Feedback controller report usage label
    AddStringDescriptorText(STRDESC_OPENPINDEV_LBL, OPENPINDEV_STRUCT_STRDESC);     // OpenPinballDeviceReport struct usage label

    // add the XInput strings, if enabled
    if (xInput.enabled)
        AddStringDescriptorText(STRDESC_XINPUTIFC, "PinscapePicoXInput");           // XInput interface display name

    // Generate the serial number.  We want to use a unique serial number
    // for the device itself, and within the device, a unique serial number
    // for each possible configuration.  The unique serial per configuration
    // is necessary because the host might cache the HID descriptors for a
    // device it's seen before, using the device serial number as a cache key.
    // This caching is perfectly appropriate for most USB devices, since most
    // devices only do one thing and always send the same report format every
    // time they're connected.  But we can be dynamically reconfigured to
    // expose differents sets of virtual devices, each with their own HID
    // report formats, so we need to make sure the host thinks we're a
    // physically separate device in each possible virtual device
    // configuration.  Since the serial number is the cache key, we can
    // accomplish this unique appearance per configuration by using a unique
    // serial number per configuration.  Note that we don't want to just use
    // a unique number on every connection - we want the number to be unique
    // per configuration, but also *stable* per configuration, so that the
    // host *can* cache the descriptors a given configuration, and reuse
    // them whenever the device is connected again in that same configuration.
    //
    // Concretely, what we mean by a "configuration" here is the particular
    // combination of virtual devices that are currently in effect.  Each
    // virtual device has a stable reporting format, so the report format
    // only changes if you change the combination of virtual devices that are
    // exposed on the interface.
    {
        // Byte-to-hex-digits formatter.  To save a little work later
        // on, we'll build the string in little-Endian 16-bit Unicode
        // format.
        static auto ByteToHex = [](uint8_t *buf, int &idx, uint8_t b)
        {
            // foramt the high nybble as a little-endian 16-bit Unicode character
            auto bh = static_cast<unsigned int>((b >> 4) & 0x0F);
            buf[idx++] = static_cast<uint8_t>(bh < 10 ? bh + '0' : bh - 10 + 'A');
            buf[idx++] = 0;

            // format the low nybble
            auto bl = static_cast<unsigned int>(b & 0x0F);
            buf[idx++] = static_cast<uint8_t>(bl < 10 ? bl + '0' : bl - 10 + 'A');
            buf[idx++] = 0;
        };

        // buffer to accumulate the serial number under construction
        uint8_t buf[64];
        int cch = 0;

        // Start with a unique ID for the device itself, based on the Pico's
        // unique hardware ID.  (This actually retrieves the 64-bit unique ID
        // for the Pico's on-board flash memory chip.)
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        for (int i = 0 ; i < 8 ; ++i)
            ByteToHex(buf, cch, id.id[i]);

        // Add a suffix for the subdevice type bits.  Combine the unique bits
        // for all of the active subdevices, and encode the result as two hex
        // digits.  This gives us a unique serial number for each combination
        // of device types, so that the host won't try to use cached descriptors
        // from one configuration with a different device configuration.
        uint8_t typeBits = 0;
        for (auto &ifc : hidIfcs)
        {
            for (auto &d : ifc->devices)
                typeBits |= d->serialNoBit;
        }

        // Add the type bit for the XInput interface, if selected
        if (xInput.enabled)
            typeBits |= SerialBitXInput;

        // add the device selection suffix as two more hex digits, with a
        // separator for readability
        buf[cch++] = '.';
        buf[cch++] = 0;
        ByteToHex(buf, cch, typeBits);

        // finally, add a two-hex-digit version suffix, with another courtesy
        // separator
        buf[cch++] = '.';
        buf[cch++] = 0;
        ByteToHex(buf, cch, DEVICE_USB_VERSION);

        // Add the string to the descriptor table.  Since we've been building
        // the string in 16-bit Unicode format all along, we can directly add
        // the byte string without any transcoding.
        AddStringDescriptorBytes(STRDESC_SERIAL, buf, cch);    // String index 3 - Serial Number
    }

    // initialize tinyusb
    board_init();
    tusb_init();
}

// add a HID interface container
USBIfc::HIDIfc *USBIfc::AddHIDInterface()
{
    // add the new interface object
    auto ifc = new HIDIfc(static_cast<int>(hidIfcs.size()), nextHidEpOut++, nextHidEpIn++);
    hidIfcs.emplace_back(ifc);

    // return a pointer to the new object
    return ifc;
}

const uint8_t *USBIfc::GetDeviceDescriptor()
{
    // initialize the device descriptor fields
    deviceDescriptor = {
        sizeof(tusb_desc_device_t), // bLength - Size of this descriptor in bytes
        TUSB_DESC_DEVICE,           // bDescriptorType - DEVICE Descriptor Type

        0x210,                      // bcdUSB - USB Specification Release Number in Binary-Coded Decimal
                                    // (e.g., 2.10 is 210H). This field identifies the release of the USB
                                    // Specification with which the device and its descriptors are compliant

        0xEF,                       // bDeviceClass - Class code: 0xEF = multi-interface function
        0x02,                       // bDeviceSubClass - Subclass code: 0x02 for IAD descriptor
        0x01,                       // bDeviceProtocol - Protocol code: device protocol 0x01 for IAD descriptor

        CFG_TUD_ENDPOINT0_SIZE,     // bMaxPacketSize0 - Maximum packet size for endpoint zero (only 8, 16, 32, or 64
                                    // are valid). For HS devices is fixed to 64

        vid,                        // idVendor - Vendor ID (assigned by the USB-IF) - dynamic data to be filled in
        pid,                        // idProduct - Product ID (assigned by the manufacturer) - dyanmic data to be filled in
        0x0100,                     // bcdDevice - Device release number in binary-coded decimal

        0x01,                       // iManufacturer - Index of string descriptor describing manufacturer
        0x02,                       // iProduct - Index of string descriptor describing product
        0x03,                       // iSerialNumber - Index of string descriptor describing the device's serial number

        0x01                        // bNumConfigurations - Number of possible configurations
    };

    // return the descriptor
    return reinterpret_cast<const uint8_t*>(&deviceDescriptor);
}

// set a 16-bit little-endian field from any int type
template<typename T> void Set16(uint8_t *ele, T val) {
    ele[0] = static_cast<uint8_t>(val & 0xFF);
    ele[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

// Get the MS OS descriptor.  This is a Microsoft-specific BOS descriptor,
// which tells Windows to automatically install its built-in WinUsb kernel
// device driver on a USB device's vendor interface.  WinUsb allows user-
// mode application software to directly read and write the vendor interface
// endpoints to implement a custom communications protocol with the device.
// A vendor interfaces would otherwise require a custom device driver on
// the Windows side to implement its protocol, which is more work for the
// vendor and more hassle for the user.
const uint8_t *USBIfc::GetMSOSDescriptor(size_t *byteSize)
{
    // MS OS 2.0 Descriptor Set header, for WinUSB access to our private vendor-defined interface.
    // Most fields have fixed values defined in the Microsoft spec.
    struct MS_OS_DESC_SET_HEADER
    {
        uint8_t wLength[2] = { 0x0A, 0x00 };
        uint8_t wDescriptorType[2] = { 0x00, 0x00 };
        uint8_t dwWindowsVersion[4] = { 0x06, 0x03, 0x00, 0x00 };
        uint8_t wTotalLength[2];      // sizeof the entire descriptor set, including this header and all contained structs
    };

    // MS OS 2.0 Configuration Subset header
    struct MS_CONFIG_SUBSET_HEADER
    {
        uint8_t wLength[2] = { 0x08, 0x00 };
        uint8_t wDescriptorType[2] = { 0x01, 0x00 };
        uint8_t bConfigIndex = 0x00;   // USB configuration number that this header applies to
        uint8_t bReserved = 0x00;
        uint8_t wTotalLength[2];      // sizeof the entire configuration set, including this header and all contained structs
    };

    // MS OS 2.0 Function Subset header
    struct MS_FUNC_SUBSET_HEADER
    {
        uint8_t wLength[2] = { 0x08, 0x00};
        uint8_t wDescriptorType[2] = { 0x02, 0x00 };
        uint8_t bFirstInterface = 0x00; // USB interface number that this header applies to
        uint8_t bReserved = 0x00;
        uint8_t wSubsetLength[2];       // sizeof the entire function set, including this header and all contained structs
    };

    // Feature description for WINUSB
    struct MS_COMP_ID_FEAT_DESC_WINUSB
    {
        uint8_t wLength[2] = { 0x14, 0x00 };
        uint8_t wDescriptorType[2] = { 0x03, 0x00 };
        uint8_t CompatibleID[8] = { 'W', 'I', 'N', 'U', 'S', 'B', 0, 0 };
        uint8_t SubCompatibleID[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    };

    // GUID property descriptor
    struct MS_REG_PROP_DESC_GUID
    {
        uint8_t wLength[2] = { 0x0A + 0x2A + 0x50, 0x00 };
        uint8_t wDescriptorType[2] = { 0x04, 0x00 };
        uint8_t wStringType[2] = { 0x07, 0x00 };
        uint8_t wPropertyNameLength[2] = { 0x2A, 0 };
        uint8_t PropertyName[0x2A] = {
            'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
            'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
            'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0,
            0, 0
        };
        uint8_t wPropertyDataLength[2] = { 0x50, 0x00 };
        uint8_t PropertyData[0x50] = {
            '{', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, '-', 0,
            'x', 0, 'x', 0, 'x', 0, 'x', 0, '-', 0,
            'x', 0, 'x', 0, 'x', 0, 'x', 0, '-', 0,
            'x', 0, 'x', 0, 'x', 0, 'x', 0, '-', 0,
            'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, '}', 0, 
            0, 0,   // GUID null terminator
            0, 0    // additional null terminator for the GUID list
        };
    };

    // Full MS OS struct for a basic WInUSB device with a custom GUID
    struct MS_OS_DESC_SET
    {
        // Note: provide the GUID in the canonical hex-digit format, all letters in
        // upper case, with the enclosing braces.
        MS_OS_DESC_SET(uint8_t configIndex, uint8_t interfaceIndex, const char *guid)
        {
            // set the configuration number and interface index
            config.bConfigIndex = configIndex;
            func.bFirstInterface = interfaceIndex;
            
            // set the set header size to the entire outer struct size
            Set16(setHeader.wTotalLength, sizeof(*this));
            
            // set the config subset header to the rest of the struct excluding the set header
            Set16(config.wTotalLength, sizeof(*this) - sizeof(setHeader));
            Set16(func.wSubsetLength, sizeof(*this) - sizeof(setHeader) - sizeof(config));
            
            // set the GUID, converting ASCII to little-endian wide characters
            uint8_t *dst = prop_guid.PropertyData;
            for (const char *src = guid ; *src != 0 ; *dst++ = static_cast<uint8_t>(*src++), *dst++ = 0) ;
        }

        MS_OS_DESC_SET_HEADER setHeader;          // set header {
        MS_CONFIG_SUBSET_HEADER config;           //   config subset header {
        MS_FUNC_SUBSET_HEADER func;               //      func subset header {
        MS_COMP_ID_FEAT_DESC_WINUSB feat_winusb;  //           compatible feature - winusb
        MS_REG_PROP_DESC_GUID prop_guid;          //           property descriptor - GUID
                                                  //      }
                                                  //   }
                                                  // }
    };

    // Build our WinUSB device's MS OS struct as a static const object.
    // Note that (per normal C++ static initialization rules) this is
    // populated from the dynamic elements on the first invocation, and
    // simply remains in memory unchanged after that, so (a) second and
    // later accesses don't incur any runtime cost, and (b) a pointer to
    // the object can be freely passed around and retained outside this
    // function (since the object has session lifetime).
    static const MS_OS_DESC_SET ms_os_desc_set(0, USBIfc::IFCNUM_VENDOR, PinscapeVendorIfc::WINUSB_GUID);

    // pass back the size if desired
    if (byteSize != nullptr)
        *byteSize = sizeof(ms_os_desc_set);

    // return the descriptor
    return reinterpret_cast<const uint8_t*>(&ms_os_desc_set);
}

// Get the BOS descriptor (used for WinUSB setup).  Returns the MS OS
// descriptor that instructs a Windows host to automatically install
// the generic WinUsb device driver on our vendor interface.  BOS is
// a standard USB descriptor type, so non-Windows hosts will accept
// and ignore MS-specific BOS data.
const uint8_t *USBIfc::GetBOSDescriptor()
{
    // get the MSOS descriptor's size
    size_t msosDescSize;
    static_cast<void>(GetMSOSDescriptor(&msosDescSize));

    // USB BOS descriptor - this wraps the MS OS descriptor in a standard USB format
    static const uint8_t bosDescriptor[] =
    {
        // total length, number of device capabilities
        TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),

        // MS OS 2.0 descriptor
        TUD_BOS_MS_OS_20_DESCRIPTOR(msosDescSize, USBIfc::VENDOR_REQUEST_WINUSB),
    };

    // return the BOS descriptor
    return bosDescriptor;
}

// Interface descriptor collection for XInput (xbox controller emulator).  This
// whole collection of descriptors is included in the configuration descriptor
// set when XInput is enabled.
static const uint8_t xinputDescriptors[] = {
    // Interface descriptor
    // Length, type, interface number, alternate setting, num endpoints, ifc class, ifc subclass, ifc protocol, string index
    9, TUSB_DESC_INTERFACE, USBIfc::IFCNUM_XINPUT, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, 0x5D, 0x01, USBIfc::STRDESC_XINPUTIFC,
    
    // Class device descriptor; see:
    // https://www.partsnotincluded.com/understanding-the-xbox-360-wired-controllers-usb-data/
    // https://github.com/fluffymadness/tinyusb-xinput/blob/master/descriptor_xinput.h
    //
    // The two sources above use slightly different versions of the descriptor, perhaps
    // for different controller versions.  The descriptor contents were discovered by
    // wire-sniffing the setup data sent by an authentic controller unit, but as far as
    // I can see, no one has reverse-engineered the full meaning of the descriptor.
    // One detail that's known is that the endpoint addresses and report lengths are
    // embedded, as commented below.  The rest of the contents are opaque.
    0x11, TUSB_DESC_CS_DEVICE,         // descriptor length and type
    0x00, 0x01, 0x01,                  // uknown
    0x25,                              // unknown
    USBIfc::EndpointInXInput, 0x14,    // endpoint in, maximum data size
    0x00, 0x00, 0x00, 0x00, 0x13,      // unknown
    USBIfc::EndpointOutXInput, 0x08,   // endpoint out, maximum data size
    0x00, 0x00,                        // unknown
    
    // Endpoint descriptor (in - device to host)
    // Length, descriptor type (endpoint), EP address, attrs, packet size, interval (ms)
    0x07, TUSB_DESC_ENDPOINT, USBIfc::EndpointInXInput, 0x03, 0x20,0x00, 0x01,
    
    // Endpoint descriptor (out - host to device)
    // Length, descriptor type (endpoint), EP address, attrs, packet size, interval (ms)
    0x07, TUSB_DESC_ENDPOINT, USBIfc::EndpointOutXInput, 0x03, 0x20,0x00, 0x01,
};

const uint8_t *USBIfc::GetConfigurationDescriptor(int /*index*/)
{
    // if we haven't built the configuration descriptor yet, do so now
    if (cfgDesc == nullptr)
    {
        // Figure the number of interfaces included
        uint8_t nCDC = usbcdc.IsConfigured() ? 2 : 0;         // CDC (virtual COM port); each counts as two interfaces, for notification & data
        uint8_t nVen = 1;                                     // Pinscape Configuration & Control vendor interface (accessible via WinUsb or libusb)
        uint8_t nXInput = xInput.enabled ? 1 : 0;             // XInput interface (a Microsoft-specific vendor interface)
        uint8_t nHID = static_cast<uint8_t>(hidIfcs.size());  // HID interfaces
        uint8_t nIfc = nCDC + nVen + nXInput + nHID;          // total number of interfaces
        
        // Figure the size of the configuration descriptor.  The descriptor consists
        // of the config descriptor header plus each interface descriptor.
        uint16_t cfgDescLen =
            TUD_CONFIG_DESC_LEN         // the config descriptor itself
            + TUD_VENDOR_DESC_LEN;      // our vendor interface descriptor

        // add the CDC descriptor length, if enabled
        if (usbcdc.IsConfigured())
            cfgDescLen += TUD_CDC_DESC_LEN;

        // add the HID descriptor sizes, which vary depending on whether they're input-only
        // or input/output devices
        for (auto &ifc : hidIfcs)
            cfgDescLen += ifc->isInOut ? TUD_HID_INOUT_DESC_LEN : TUD_HID_DESC_LEN;

        // Add the XInput class interface descriptor collection, if XInput is enabled
        if (xInput.enabled)
            cfgDescLen += sizeof(xinputDescriptors);

        // allocate space
        cfgDesc.reset(new uint8_t[cfgDescLen]);
        uint8_t *p = cfgDesc.get();
        static auto Append = [](uint8_t* &p, const uint8_t *desc, size_t size) {
            memcpy(p, desc, size);
            p += size;
        };

        // Add the configuration descriptor header
        // Config number, interface count, string index, total length, attributes, power in mA.
        const uint8_t hdr[] = { TUD_CONFIG_DESCRIPTOR(1, nIfc, 0, cfgDescLen, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500) };
        Append(p, hdr, sizeof(hdr));
        static_assert(sizeof(hdr) == TUD_CONFIG_DESC_LEN);

        // Add the CDC interface descriptor, if configured
        // Interface number, string index, EP notification in address, EP notification size, EP data address out, EP data address in, EP data size
        const uint8_t cdc[] = { TUD_CDC_DESCRIPTOR(IFCNUM_CDC, STRDESC_CDCIFC, EndpointNotifyCDC, 10, EndpointOutCDC, EndpointInCDC, CFG_TUD_CDC_EP_BUFSIZE) };
        static_assert(sizeof(cdc) == TUD_CDC_DESC_LEN);
        if (usbcdc.IsConfigured())
            Append(p, cdc, sizeof(cdc));

        // Add our Configuration & Control vendor interface.  This interface is
        // accessed on the host by custom application software that speaks our
        // special protocol.  On Windows, we use WinUsb to access the interface.
        // Interface number, string index, EP Out & IN address, EP size
        const uint8_t ven[] = { TUD_VENDOR_DESCRIPTOR(IFCNUM_VENDOR, STRDESC_VENIFC, EndpointOutVendor, EndpointInVendor, CFG_TUD_VENDOR_EP_BUFSIZE) };
        static_assert(sizeof(ven) == TUD_VENDOR_DESC_LEN);
        Append(p, ven, sizeof(ven));

        // Add the XInput interface, if configured
        if (xInput.enabled)
            Append(p, xinputDescriptors, sizeof(xinputDescriptors));

        // Add each HID interface
        uint8_t ifcNum = IFCNUM_HID0;
        for (auto &ifc : hidIfcs)
        {
            // initialize the interface's combined report descriptor, since we need the
            // final size of the combined descriptor for the interface descriptor
            static_cast<void>(ifc->GetCombinedReportDescriptor());
            
            // build the HID interface descriptor
            if (ifc->isInOut)
            {
                // IN/OUT interface
                // interface number, string index, protocol, report descriptor length, EP-Out address, EP-In address, size, polling interval in milliseconds
                const uint8_t hidInOut[] = { TUD_HID_INOUT_DESCRIPTOR(
                    ifcNum, 0, HID_ITF_PROTOCOL_NONE, ifc->combinedReportDescriptor.size(),
                    ifc->epOut, ifc->epIn, CFG_TUD_HID_EP_BUFSIZE, static_cast<uint8_t>(ifc->pollingInterval/1000))
                };
                
                // add the interface
                Append(p, hidInOut, sizeof(hidInOut));
                static_assert(sizeof(hidInOut) == TUD_HID_INOUT_DESC_LEN);
            }
            else
            {
                // INPUT ONLY interface
                // interface number, string index, protocol, report descriptor len, EP In address, size, polling interval in milliseconds
                const uint8_t hidIn[] = { TUD_HID_DESCRIPTOR(
                    ifcNum, 0, HID_ITF_PROTOCOL_NONE, ifc->combinedReportDescriptor.size(),
                    ifc->epIn, CFG_TUD_HID_EP_BUFSIZE, static_cast<uint8_t>(ifc->pollingInterval/1000))
                };

                // add the interface
                Append(p, hidIn, sizeof(hidIn));
                static_assert(sizeof(hidIn) == TUD_HID_DESC_LEN);
            }

            // advance the interface counter
            ++ifcNum;
        };
    }

    // return the cached descriptor
    return cfgDesc.get();
}

const uint8_t *USBIfc::GetHIDReportDescriptor(int instance)
{
    // make sure the instance is in range
    if (instance < 0 || instance >= static_cast<int>(sizeof(hidIfcs)))
        return nullptr;

    // get the combined report descriptor for the requested instance
    return hidIfcs[instance]->GetCombinedReportDescriptor();
}

void USBIfc::AddStringDescriptorText(uint8_t index, const char *str)
{
    // add a new element to the descriptor vector
    auto &ele = stringDescriptors.emplace(
        std::piecewise_construct, std::forward_as_tuple(index), std::forward_as_tuple()).first->second;

    // figure the descriptor length: two bytes for the prefix plus
    // two bytes for each character in the string
    size_t charLen = strlen(str);
    size_t descByteLen = 2 + charLen*2;

    // allocate space in the descriptor's byte vector
    ele.resize(descByteLen);

    // set the prefix bytes: length, descriptor type code
    uint8_t *dst = ele.data();
    *dst++ = static_cast<uint8_t>(descByteLen);
    *dst++ = TUSB_DESC_STRING;

    // Translate the ASCII characters to 16-bit Unicode; this is a
    // straightforward matter of expanding each 8-bit source character
    // to 16 bits with zero extension, since we define the source
    // character set as the first Unicode page, hence all of the 16-bit
    // Unicode code points are the same as the 8-bit input code points.
    for (const char *p = str ; *p != 0 ; ++p)
    {
        // add the character in little-Endian 16-bit format (low byte first)
        *dst++ = *p;
        *dst++ = 0;
    }
}   

void USBIfc::AddStringDescriptorBytes(uint8_t index, const uint8_t *data, size_t nDataBytes)
{
    // add a new element
    auto &ele = stringDescriptors.emplace(
        std::piecewise_construct, std::forward_as_tuple(index), std::forward_as_tuple()).first->second;

    // reserve space for the two-byte prefix plus the data bytes
    size_t descByteLen = 2 + nDataBytes;
    ele.resize(descByteLen);

    // set the prefix bytes: length, descriptor type code
    uint8_t *dst = ele.data();
    *dst++ = static_cast<uint8_t>(descByteLen);
    *dst++ = TUSB_DESC_STRING;

    // Copy the data bytes.  Note that we copy the bytes exactly as
    // given - the caller has already formatted them as desired for
    // wire transfer.  We explicitly don't do any character set
    // translation here; these are just opaque bytes to be sent
    // exactly as-is.
    for (size_t i = 0 ; i < nDataBytes ; ++i)
        *dst++ = *data++;
}

const uint16_t *USBIfc::GetStringDescriptor(uint8_t index, uint16_t /*langId*/)
{
    // If it's in range, return the descriptor from our table, otherwise
    // return null.
    //
    // Note that TinyUSB wants a pointer to a uint16_t array, whereas our
    // table element is a byte vector.  We store the byte vector because we
    // want to be sure that the bytes are in the correct endian order for wire
    // transmission, without further translation every time we get a request.
    // As it happens, that's what TinyUSB wants, too, even though its
    // interface suggests otherwise.  The uint16_t* return type in the
    // interface says that TinyUSB wants a native uint16_t array, but it
    // doesn't; it actually wants a byte array that it can pass back to the
    // host without translation (the first thing that the TinyUSB caller will
    // do on return is reinterpret_cast the result pointer back to uint8_t*).
    //
    // If TinyUSB really wanted a uint16_t array here, it would translate the
    // local byte order to wire byte order (little Endian) on our return,
    // which it doesn't.  It accidentally works on Pico, since Pico is
    // little-endian, and it accidentally works on most other devices, since
    // almost everyone's little-Endian these days, but it's still a design
    // error in the interface.  The type cast here isn't just safe, but
    // actually *correct*; it just compensates for the error in the API
    // specification.  The value we're passing back is exactly what the
    // caller really wants.
    if (auto it = stringDescriptors.find(index) ; it != stringDescriptors.end())
        return reinterpret_cast<const uint16_t*>(it->second.data());
    else
        return nullptr;
}

void USBIfc::EnableSOFInterrupt(int clientID, bool enable)
{
    // add/remove the client bit in the client status bit vector
    uint32_t clientBit = 1UL << clientID;
    if (enable)
        sofInterruptClientEnable |= clientBit;
    else
        sofInterruptClientEnable &= ~clientBit;

    // enable in TinyUSB if it's enabled in ANY client
    tud_sof_cb_enable(sofInterruptClientEnable != 0);
}

bool USBIfc::VendorControlXfer(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    switch (stage)
    {
    case CONTROL_STAGE_SETUP:
        // Setup - check the request type
        switch (request->bmRequestType_bit.type)
        {
        case TUSB_REQ_TYPE_VENDOR:
            // vendor request
            switch (request->bRequest)
            {
            case VENDOR_REQUEST_WINUSB:
                // WinUSB request - check the request type
                switch (request->wIndex)
                {
                case 7:
                    // WinUSB request type 7 = Get MS OS 2.0 compatible descriptor.  This
                    // only applies to our Pinscape vendor interface, so check the endpoint.
                    {
                        size_t msosDescSize;
                        const uint8_t *msosDesc = GetMSOSDescriptor(&msosDescSize);
                        return tud_control_xfer(rhport, request, const_cast<uint8_t*>(msosDesc), msosDescSize);
                    }
                    
                default:
                    // not handled
                    return false;
                }
                
            default:
                // unhandled request
                return false;
            }

        case TUSB_REQ_TYPE_CLASS:
            // not handled
            return false;
        }

    case CONTROL_STAGE_DATA:
    case CONTROL_STAGE_ACK:
        // no extra work required
        return true;

    default:
        // uknown stage
        return false;
    }
}


void USBIfc::Task()
{
    // run pending tinyusb device tasks
    tud_task();

    // check for remote wakeup
    if (wakePending)
    {
        // only wake the host if remote wake is enabled, and the host is actually
        // asleep, as indicated by "suspended" mode on the USB connection
        if (remoteWakeEnabled && tud_suspended())
            tud_remote_wakeup();

        // consume the pending wake event, even if we didn't actually wake the host
        // (as we don't want the event to remain pending into the next sleep if the
        // host is currently awake)
        wakePending = false;
    }

    // run HID interface tasks
    for (auto &ifc : hidIfcs)
        ifc->Task();

    // Read CDC host-to-device output if available.  Note that we do most of
    // this work in-line rather than in the USBCDC class because it makes the
    // checks a little faster, which is important since this is invoked on
    // every main loop iteration.
    if (usbcdc.IsConfigured() && tud_cdc_n_available(0) != 0)
    {
        // read and process the output from the host
        uint8_t cdcBuf[64];
        uint32_t nBytes = tud_cdc_n_read(0, cdcBuf, sizeof(cdcBuf));
        if (nBytes != 0)
            usbcdc.OnOutput(cdcBuf, nBytes);
    }

    // Read host-to-device output on our Pinscape Control & Config
    // vendor interface, if available.  As with the CDC, we do a lot of the
    // work in-line to speed up the main loop slightly.
    if (tud_vendor_n_available(VENDORIFC_IDX_CONFIG) != 0)
    {
        // read and process the output from the host
        uint8_t venbuf[64];
        uint32_t nBytes = tud_vendor_n_read(VENDORIFC_IDX_CONFIG, venbuf, sizeof(venbuf));
        if (nBytes != 0)
            psVendorIfc.OnOutput(venbuf, nBytes);

        // if that put us in polling mode, poll for additional input for a few milliseconds
        if (psVendorIfc.enterPollingMode)
            psVendorIfc.RunPollingMode();
    }
}

void USBIfc::OnBusSuspended(bool remoteWakeEnabled)
{
    // enter suspend mode, and note whether or not we're allowed to wake the host
    suspended = true;
    this->remoteWakeEnabled = remoteWakeEnabled;

    // update the CDC COM port
    usbcdc.OnSuspendResume(true);

    // handle the state change
    OnDeviceStateChanged();
}

void USBIfc::OnBusResumed()
{
    // exit suspend mode, and assume that remote wakeup will be enabled the next
    // time we find ourselves in suspend mode
    suspended = false;
    remoteWakeEnabled = true;

    // update the CDC COM port
    usbcdc.OnSuspendResume(false);

    // handle the state change
    OnDeviceStateChanged();
}

void USBIfc::OnDeviceStateChanged()
{
    // if dismounted and/or suspended, turn off all output ports
    if (!mounted || suspended)
        OutputManager::AllOff();

    // update the diagnostic LEDs
    picoLED.SetUSBStatus(mounted, suspended);
}

// Get Report event handler
uint16_t USBIfc::OnGetHIDReport(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    // check that the instance is in range
    if (instance < hidIfcs.size())
    {
        // get the interface
        HIDIfc *ifc = hidIfcs[instance].get();
    
        // find the device that handles this type of report
        for (auto &device : ifc->devices)
        {
            // match on the report ID
            if (device->reportID == id)
            {
                switch (type)
                {
                case HID_REPORT_TYPE_INPUT:
                    // get the report and return the result
                    return device->GetReport(type, buf, reqLen);
                    
                default:
                    // we don't handle other types
                    return 0;
                }
            }
        }
    }

    // not found
    return 0;
}

// Set Report event handler
void USBIfc::OnSetHIDReport(uint8_t instance, uint8_t reportID, hid_report_type_t type, const uint8_t *buf, uint16_t reqLen)
{
    // report type names
    static_assert(static_cast<int>(HID_REPORT_TYPE_INVALID) == 0);
    static_assert(static_cast<int>(HID_REPORT_TYPE_INPUT) == 1);
    static_assert(static_cast<int>(HID_REPORT_TYPE_OUTPUT) == 2);
    static_assert(static_cast<int>(HID_REPORT_TYPE_FEATURE) == 3);
    static const char *typeName[]{ "INVALID", "INPUT", "OUTPUT", "FEATURE" };

    // check that the instance is in range
    if (instance < hidIfcs.size())
    {
        // get the interface
        HIDIfc *ifc = hidIfcs[instance].get();

        // Tinyusb calls this in two contexts, with inconsistent arguments:
        //
        // Control transfer:
        //    - report type set to type from control request
        //    - 'reportID' set to first byte of raw buffer
        //    - first byte of raw buffer skipped, buffer length reduced by 1
        //
        // OUT Endpoint transfer
        //    - Tinyusb < 0.17.0:
        //      - report type set to HID_REPORT_TYPE_INVALID
        //      - reportID set to 0
        //      - raw buffer passed exactly as sent, with no length change
        //
        //    - Tinyusb >= 0.17.0:
        //      - report type set to HID_REPORT_TYPE_OUTPUT
        //      - reportID set to 0
        //      - raw buffer passed exactly as sent, with no length change
        //
        // If reportID != 0, we know this is a control transfer, and we know that
        // the report ID prefix has already been removed from the buffer.
        //
        // If reportID == 0, we have either:
        //   - a control transfer with ID==0, buffer has no ID prefix
        //   - an OUT transfer with ANY ID:
        //     - buffer has an ID prefix if and only if the reports use IDs
        //     - buffer does not contain an ID prefix if the reports don't use IDs
        //
        // As far as I can see, there's no way to distinguish these cases
        // from what TinyUSB sends us.  So we have to go by what we EXPECT
        // the host to send, and assume that the host is sending us well-
        // formed packets that include report IDs when required and don't
        // include report IDs where not required:
        //
        //   - If we have exactly one interface, and its report ID == 0,
        //     there are no report IDs, so assume the buffer has no report
        //     ID prefix bytes
        //
        //   - If we have more than one interface, or our one interface
        //     uses a non-zero report ID, all of our OUT reports should
        //     come with a report ID prefix, which is still in the buffer.
        //
        if (reportID == 0 && (ifc->devices.size() != 1 || ifc->devices.front()->reportID != 0))
        {
            // We have multiple interfaces (-> we MUST use report IDs
            // for all interfaces), OR we have just one interface, but
            // it uses a non-zero report ID anyway.  Therefore this is
            // an OUT report with the report ID still in the buffer as
            // the prefix byte.
            reportID = *buf++;
            reqLen -= 1;
        }

        // find the device that handles this type of report
        for (auto &device : ifc->devices)
        {
            // match on the report ID
            if (device->reportID == reportID)
            {
                switch (type)
                {
                case HID_REPORT_TYPE_OUTPUT:
                    // set the report, then we're done
                    device->SetReport(type, buf, reqLen);
                    return;
                    
                default:
                    // we don't handle other types
                    Log(LOG_DEBUGEX, "OnSetHIDReport: unhandled report type %d (%s) on HIDIfc[%d], report ID=%d (%s)\n",
                        type, typeName[type], instance, reportID, device->name);
                    return;
                }
            }
        }

        // not handled
        Log(LOG_DEBUGEX, "OnSetHIDReport: unhandled report type %d (%s), report ID=%d [no device match]\n", type, typeName[type], reportID);
    }
    else
    {
        // out of range
        Log(LOG_DEBUGEX, "OnSetHIDReport: no such HID interface [%d]\n", instance);
    }
}

// Send Report Complete event handler
void USBIfc::OnSendHIDReportComplete(uint8_t instance, const uint8_t *report, uint16_t len)
{
    // forward the notification to the interface instance
    if (instance < static_cast<uint8_t>(hidIfcs.size()))
        hidIfcs[instance]->OnSendReportComplete(report, len);
}


// ----------------------------------------------------------------------------
//
// Tinyusb callbacks.  We refer most of these to the singleton USBIfc instance,
// if one exists.
//

// Tinyusb Get Configuration Descriptor callback
const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return usbIfc.GetConfigurationDescriptor(index);
}

// Tinyusb Get BOS descriptor callback
const uint8_t *tud_descriptor_bos_cb()
{
    return usbIfc.GetBOSDescriptor();
}

// Tinyusb Get HID Descriptor Report callback
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return usbIfc.GetHIDReportDescriptor(instance);
}

// Tinyusb Get HID Report client callback
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t reqLen)
{
    return usbIfc.OnGetHIDReport(instance, id, type, buf, reqLen);
}

// Tinyusb Set HID Report client callback
void tud_hid_set_report_cb(uint8_t instance, uint8_t reportId, hid_report_type_t type, const uint8_t *buf, uint16_t bufSize)
{
    usbIfc.OnSetHIDReport(instance, reportId, type, buf, bufSize);
}

// Tinyusb Send Complete callback - invoked when a 'send report' operation finishes
void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
    usbIfc.OnSendHIDReportComplete(instance, report, len);
}

// get the device descriptor
const uint8_t *tud_descriptor_device_cb()
{
    return usbIfc.GetDeviceDescriptor();
}

// get a string descriptor
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langId)
{
    return usbIfc.GetStringDescriptor(index, langId);
}

// vendor control transfer callback - invoked when a control transfer occurs on a vendor interface
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    return usbIfc.VendorControlXfer(rhport, stage, request);
}

// device mounted
void tud_mount_cb()
{
    usbIfc.OnMountDevice();
}

// device dismounted
void tud_umount_cb()
{
    usbIfc.OnDismountDevice();
}

// bus suspended, entering low power mode
void tud_suspend_cb(bool remoteWakeEnabled)
{
    usbIfc.OnBusSuspended(remoteWakeEnabled);
}

// bus resumed from low-power mode
void tud_resume_cb()
{
    usbIfc.OnBusResumed();
}

// SOF (Start-of-Frame) callback.  If enabled via tud_sof_cb_enable(),
// TinyUSB enables the SOF interrupt at the hardware level and invokes
// this callback, always in user space (during task processing), after
// each SOF interrupt.  TinyUSB only calls this when the client
// explicitly enables the callback, which allows the library to disable
// the SOF interrupt at the hardware level when it's not needed,
// reducing CPU load from handling unnecessary interrupts.
void tud_sof_cb(uint32_t frame_count)
{
}

// ---------------------------------------------------------------------------
//
// Application-supplied class driver initialization callback.  Tinyusb calls
// this during initialization to allow the application to install its own
// custom class drivers, for USB classes that Tinyusb doesn't provide as
// built-ins.  We use this to add our XInput class driver if XInput is
// enabled.
//
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driverCount)
{
    static const usbd_class_driver_t applicationClassDrivers[] = {
        {
            TUSB_DRIVER_NAME("XINPUT")     // driver name
            &XInput::Driver::Init,         // initialize driver
            IF_TUSB_001600(&XInput::Driver::DeInit,)  // deinitialize driver
            &XInput::Driver::Reset,        // reset driver
            &XInput::Driver::Open,         // open driver
            &XInput::Driver::ControlXfer,  // device control request
            &XInput::Driver::Xfer,         // transfer completion callback
            nullptr                        // USB Start Of Frame (SOF) event handler (optional)
        }
    };

    // add the XInput class driver only if XInput is enabled for this session
    if (xInput.enabled)
    {
        *driverCount = static_cast<uint8_t>(_countof(applicationClassDrivers));
        return &applicationClassDrivers[0];
    }

    // no custom class drivers are needed for this session
    *driverCount = 0;
    return nullptr;
}

// ----------------------------------------------------------------------------
//
// HID Interface container class
//

// add a device to a HID interface
USBIfc::HIDIfc *USBIfc::HIDIfc::AddDevice(HID *dev)
{
    // add the device to our list
    devices.emplace_back(dev);

    // set its interface instance
    dev->ifcInstance = instance;

    // mark the device as configured
    dev->configured = true;

    // if this any device is in/out, the whole interface is in/out
    if (dev->isInOut)
        isInOut = true;

    // Reduce the report interval to the device's desired report interval,
    // if shorter than the current setting.  This ensures that the interface
    // will be polled at the fastest rate of any device it aggregates.
    pollingInterval = std::min(pollingInterval, dev->GetMaxPollingInterval());

    // Update the polling refractory interval.  Make this equal to the
    // new polling interval, minus a couple of milliseconds for padding, to
    // leave time to set up the new send before the next host polling request
    // actually occurs.  The refractory interval can never be less than zero.
    pollingRefractoryInterval = std::max(0, pollingInterval - RefractoryIntervalPadding);

    // return 'this' for call chaining
    return this;
}

// get the combined HID report descriptors for this interface
const uint8_t *USBIfc::HIDIfc::GetCombinedReportDescriptor()
{
    // if we haven't generated the concatenated report yet, do so now
    if (combinedReportDescriptor.size() == 0)
    {
        // figure the total space required
        uint16_t totalLen = 0;
        for (auto &d : devices)
        {
            // get this report's length and add it to the total
            uint16_t curLen;
            d->GetReportDescriptor(&curLen);
            totalLen += curLen;
        }

        // allocate space
        combinedReportDescriptor.resize(totalLen);

        // build the combined report
        uint8_t *dst = combinedReportDescriptor.data();
        for (auto &d : devices)
        {
            // get this report
            uint16_t curLen;
            const uint8_t *src = d->GetReportDescriptor(&curLen);

            // concatenate it into the combined buffer
            memcpy(dst, src, curLen);
            dst += curLen;
        }
    }

    // return the concacenated descriptor byte array
    return combinedReportDescriptor.data();
}

void USBIfc::HIDIfc::Task()
{
    // Check if it's time to send our next report.  Skip reports while
    // the connection is suspended (meaning the host is in sleep mode).
    //
    // HID report transmission is a little counterintuitive.  In most
    // other networking contexts, "send" means that we start sending out
    // the data on the wire.  In HID, "send" doesn't actually send
    // anything.  It only places the report into an internal buffer in
    // the USB controller hardware, awaiting a request from the host to
    // read the report.  The host periodically polls for these reports,
    // so that's when the send actually occurs: when the host asks for
    // what's already in the buffer.  These HID poll requests must be
    // answered *immediately*, so it's not possible for the USB
    // controller to go out and ask the CPU to provide the report live
    // at the moment the host requests it; the report must already be
    // sitting in the buffer for immediate transmission on demand, or
    // the controller will simply answer that there's nothing to send.
    // This isn't even a case where we can intervene with an IRQ
    // response; it's entirely handled in the USB controller hardware,
    // with no CPU involvement whatsoever.
    //
    // The requirement to buffer a transmission in advance of the host's
    // request means that there's some unavoidable latency, because the
    // report we send here is going to sit in the hardware buffer until
    // the host asks for it.  The best we can do is to try to time
    // things on our side so that we wait until as late in the current
    // polling cycle as possible to generate the report, so that the
    // report is as up-to-date as possible when it finally travels
    // across the wire.  If we generate a report early in the polling
    // cycle, it doesn't do anything to speed up the polling cycle, and
    // thus only makes the latency between report preparation and
    // transmission longer.  We paradoxically get lower latency by
    // waiting *longer* to prepare the report, because that means the
    // report will have been sitting in the hardware staging buffer for
    // a shorter time when the host picks it up.  Also note that there's
    // no way to "update" a report that was previously staged: once the
    // CPU hands a buffer over to the controller, the controller "owns"
    // the buffer until the IN transaction completes.
    //
    // We use two factors to try to time things to generate reports late
    // in the cycle.  First, we keep track of the time the PREVIOUS
    // report completed, meaning that the host polled for the last
    // report and the USB hardware finished transmitting it.  Second, we
    // define a "refractory interval" for the connection.  This is
    // initialized at startup to a few milliseconds less than the
    // requested polling interval set on the interface.  The idea is
    // that it's long enough lead time that we'll beat the next polling
    // request from the host, ensuring that a report is ready to go when
    // the host asks for one, but still late enough in the polling cycle
    // that the report we enqueue is still fresh when the host asks for
    // it.  We try to keep the main loop time below 1ms, so a lead time
    // of perhaps 2ms should be adequate to ensure timely delivery.  For
    // interfaces with polling times shorter than this, the refractory
    // time is set to zero, so that we generate reports every time the
    // buffer is available.
    uint64_t t = time_us_64();
    if (!usbIfc.suspended
        && (pollingRefractoryInterval == 0 || t >= tSendComplete + pollingRefractoryInterval)
        && tud_hid_n_ready(instance))
    {
        // Scan for a device with a report to send.  Start at the
        // next device after the last device that actually sent a
        // report, to ensure that no one device will starve the other
        // devices on the interface.
        for (int idx = lastSender, cntr = 0, n = GetNDevices() ; cntr < n ; ++cntr)
        {
            // increment and wrap
            idx = (idx + 1) % n;

            // check if this device has data to send
            auto *pdev = devices[idx];
            t = time_us_64();
            if (pdev->enabled && pdev->SendReport())
            {
                // report sent - note the device with a send in progress
                deviceSending = idx;
                lastSender = idx;

                // collect statistics
                pdev->stats.StartReport(t);

                // note the start time
                tSendStart = t;

                // stop here, return to the main task loop
                break;
            }
        }
    }
}

// Report completed notification (callback from tinyusb)
void USBIfc::HIDIfc::OnSendReportComplete(const uint8_t * /*report*/, size_t /*len*/)
{
    // collect statistics for the send
    uint64_t t = time_us_64();
    if (deviceSending >= 0 && deviceSending < devices.size())
    {
        // log the completion time in the stats
        devices[deviceSending]->stats.CompleteReport(t);

        // no device sending
        deviceSending = -1;
    }

    // update the interface-level send completion time marker
    tSendComplete = t;
}


// ----------------------------------------------------------------------------
//
// HID base class
//

bool USBIfc::HID::SendReport()
{
    // build an input report (device-to-host) into a local buffer
    uint8_t buf[64];
    uint16_t len = GetReport(HID_REPORT_TYPE_INPUT, buf, static_cast<uint16_t>(sizeof(buf)));

    // send it on the wire
    if (len != 0)
    {
        tud_hid_n_report(ifcInstance, reportID, buf, len);
        return true;
    }

    // no report generated
    return false;
}

void USBIfc::HID::Stats::Log(const ConsoleCommandContext *ctx, USBIfc::HID *hid)
{
    ctx->Printf(
        "  Configured:          %s\n", hid->configured ? "Yes" : "No");
    if (!hid->configured)
        return;

    ctx->Printf(
        "  Reports:             %s\n"
        "  HID interface:       %d\n"
        "  HID report ID:       %d\n"
        "  Num reports started: %llu\n"
        "  Num completed:       %llu\n",
        hid->enabled ? "Enabled" : "Disabled",
        hid->ifcInstance, hid->reportID,
        nReportsStarted, nReportsCompleted);

    if (nReportsCompleted != 0)
    {
        ctx->Printf(
            "  Avg completion time: %.2lf ms\n"
            "  Report interval:     %.2lf ms\n",
            static_cast<double>(totalCompletionTime)/nReportsCompleted/1000.0,
            static_cast<double>(totalTimeBetweenReports)/nReportsStarted/1000.0);
    }
    else
    {
        ctx->Print(
            "  Avg completion time: n/a\n"
            "  Report interval:     n/a\n");
    }
    if (nReportsCompletedWithEvents != 0)
    {
        ctx->Printf(
            "  Event latency:       %.2lf ms (%llu events)\n",
            static_cast<double>(totalEventLatency)/nReportsCompletedWithEvents/1000.0,
            nReportsCompletedWithEvents);
    }
    else
    {
        ctx->Printf(
            "  Event latency:       n/a\n");
    }
}

// ---------------------------------------------------------------------------
//
// Logical Axis.  This is a helper object that maps USB analog input
// axes, such as a joystick axis, to an underlying physical data source,
// such as an accelerometer axis or the plunger sensor.
//

// null axis source singleton
NullAxisSource nullAxisSource;

// logical axis source name/creation function map
using Strs = std::vector<std::string>;
std::unordered_map<std::string, LogicalAxis::CreateFunc> LogicalAxis::nameMap{
    { "null",           [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return &nullAxisSource; } },
    { "nudge.x",        [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NudgeXAxisSource(params); } },
    { "nudge.y",        [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NudgeYAxisSource(params); } },
    { "nudge.z",        [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NudgeZAxisSource(params); } },
    { "nudge.vx",       [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NudgeVXAxisSource(params); } },
    { "nudge.vy",       [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NudgeVYAxisSource(params); } },
    { "nudge.vz",       [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NudgeVZAxisSource(params); } },
    { "plunger.sensor", [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new PlungerSensorRawAxisSource(params); } },
    { "plunger.z",      [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new PlungerZAxisSource(params); } },
    { "plunger.z0",     [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new PlungerZ0AxisSource(params); } },
    { "plunger.speed",  [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new PlungerSpeedAxisSource(params); } },
    { "sine",           [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new SineAxisSource(params, strs); } },
    { "negate",         [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new NegativeAxisSource(params, strs); } },
    { "offset",         [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new OffsetAxisSource(params, strs); } },
    { "scale",          [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new ScaleAxisSource(params, strs); } },
    { "abs",            [](const CtorParams &params, Strs &strs) -> LogicalAxis* { return new AbsAxisSource(params, strs); } },
};

// Configure a logical axis from JSON data.  This is designed to be called
// from a device's configuration function to configure its components that
// can be connected to analog controls like accelerometers and plungers.
LogicalAxis *LogicalAxis::Configure(
    const CtorParams &params, const JSONParser::Value *obj, const char *prop, const char *defaultValue)
{
    // If the property is defined, interpret it as a string and parse it.
    // Otherwise just return a null axis source.
    if (auto *val = obj->Get(prop) ; !val->IsUndefined())
        return Configure(params, val->String().c_str(), prop);
    else if (defaultValue != nullptr)
        return Configure(params, defaultValue, prop);
    else
        return &nullAxisSource;
}

// Configure from a string
LogicalAxis *LogicalAxis::Configure(
    const CtorParams &params, const char *str, const char *prop)
{
    // Parse the property string:
    //
    //    <name> ( <arg>, ... )
    //
    // An optional argument list in parentheses can pass comma-delimited
    // strings to the source constructor.  Some sources take arguments,
    // and some just ignore them.
    //
    // Some sources take sub-sources as arguments.  E.g., "negate(nudge.x)"
    // reverses the sign of the nudge device X axis source.
    const char *p = str;

    // skip leading spaces, extract the name portion
    for ( ; isspace(*p) ; ++p) ;
    const char *start = p;
    for ( ; *p != 0 && !(isspace(*p) || *p == '(') ; ++p) ;
    std::string name(start, p - start);

    // skip spaces and check for an argument list
    for ( ; isspace(*p) ; ++p) ;
    std::vector<std::string> argStrings;
    if (*p == '(')
    {
        // skip the '(' and parse arguments, until we reach ')' or end of string
        for (++p ; *p != 0 && *p != ')' ; )
        {
            // Skip spaces and scan this argument.  If the argument
            // is enclosed in quotes, scan to the closing quote,
            // otherwise scan to the next space, comma, or right
            // paren.
            for ( ; isspace(*p) ; ++p) ;
            if (*p == '"' || *p == '\'')
            {
                // quoted string - skip the quote and scan for the
                // matching close quote
                char qu = *p++;
                for (start = p ; *p != 0 && *p != qu ; ++p) ;

                // add the argument
                argStrings.emplace_back(start, p - start);

                // if we found a close quote, skip it, otherwise
                // it's an error
                if (*p == qu)
                    ++p;
                else
                    Log(LOG_ERROR, "%s.%s: unterminated string in argument list\n",
                        params.deviceName, prop);
            }
            else
            {
                // Scan it as a plain token, ending at a space or
                // the next argument list delimiter.  If there are
                // any nested parentheses, scan the contents as a
                // single unit.
                start = p;
                for ( ; *p != 0 && !(isspace(*p) || *p == ',' || *p == ')'); ++p)
                {
                    if (*p == '(')
                    {
                        // scan for the matching paren
                        ++p;
                        for (int level = 1 ; *p != 0 ; ++p)
                        {
                            if (*p == ')')
                            {
                                if (--level == 0)
                                    break;
                            }
                            else if (*p == '(')
                            {
                                ++level;
                            }
                            else if (*p == '"' || *p == '\'')
                            {
                                for (char qu = *p++ ; *p != 0 && *p != qu ; ++p) ;
                                if (*p == 0)
                                    break;
                            }
                        }

                        // if we ran out of string in a nested expression
                        // or string, stop the outer scan here
                        if (*p == 0)
                            break;
                    }
                }

                // add this argument to the list
                argStrings.emplace_back(start, p - start);
            }

            // skip spaces after the token
            for ( ; isspace(*p) ; ++p) ;

            // end the whole argument list at ')'
            if (*p == ')')
                break;

            // continue on ','
            if (*p == ',')
            {
                ++p;
                continue;
            }

            // anything else is an error - log it, skip the rest (so
            // that we don't report another error about extraneous
            // text at the end), and stop here
            Log(LOG_ERROR, "%s.%s: expected ',' or ')' in argument list, found \"%.16s\"; extra text ignored\n",
                params.deviceName, prop, p, strlen(p) > 16 ? "..." : "");
            p += strlen(p);
            break;
        }

        // skip the closing ')' and any trailing spaces
        if (*p == ')')
            for (++p ; isspace(*p) ; ++p) ;
    }

    // we should be at the end of the string now
    if (*p != 0)
    {
        // extraneous text - log a warning and ignore it
        Log(LOG_WARNING, "%s.%s: unexpected text after axis source type, \"%.16s%s\"\n",
            params.deviceName, prop, p, strlen(p) > 16 ? "..." : "");
    }

    // look for a match to the name
    if (auto it = nameMap.find(name) ; it != nameMap.end())
    {
        // call the creation function to create the source
        return it->second(CtorParams(params, prop), argStrings);
    }
    else
    {
        // not found - log an error and return the null source
        Log(LOG_ERROR, "%s.%s: unrecognized axis source type \"%s\"\n",
            params.deviceName, prop, name.c_str());

        return &nullAxisSource;
    }
}

NudgeDevice::View *LogicalAxis::CtorParams::GetNudgeDeviceView() const
{
    // if the parent device doesn't already have a view object, create one
    if (*ppNudgeDeviceView == nullptr)
        *ppNudgeDeviceView = nudgeDevice.CreateView();

    // return the object
    return *ppNudgeDeviceView;
}

// ---------------------------------------------------------------------------
//
// Plunger logical axis sources
//

// Raw and normalized sensor readings.  Both use a UINT16
// representation, so we have to divide by two to rescale it to the
// positive half of the INT16 logical axis.
int16_t PlungerSensorRawAxisSource::Read() { return static_cast<int16_t>(plunger.GetRawSensorReading() / 2); }

// Processed logical Z axis value, with launch-event corrections applied
int16_t PlungerZAxisSource::Read() { return plunger.GetZ(); }

// Logical Z axis value, base reading without launch-event corrections applied
int16_t PlungerZ0AxisSource::Read() { return plunger.GetZ0(); }

// Plunger speed axis value
int16_t PlungerSpeedAxisSource::Read() { return plunger.GetSpeed(); }


// ---------------------------------------------------------------------------
//
// Sine wave logical axis source
//

SineAxisSource::SineAxisSource(const CtorParams &params, std::vector<std::string> &args)
{
    // args[0] (required) = period in milliseconds
    if (args.size() >= 1)
        period = atoi(args[0].c_str()) * 1000;
    else
        Log(LOG_ERROR, "%s.%s: sine() axis period parameter is missing\n", params.deviceName, params.propName);

    // args[1] (optional) = phase offset in milliseconds
    if (args.size() >= 2)
        phase = atoi(args[1].c_str()) * 1000;

    // warn on additional arguments
    if (args.size() >= 3)
        Log(LOG_WARNING, "%s.%s: extra arguments to sine() ignored, expected (period,phase)\n", params.deviceName, params.propName);
}

// ---------------------------------------------------------------------------
//
// Negative logical axis source
//

NegativeAxisSource::NegativeAxisSource(const CtorParams &params, std::vector<std::string> &args)
{
    // args[0] (required) = underlying source expression
    if (args.size() >= 1)
        source = LogicalAxis::Configure(params, args[0].c_str(), params.propName);
    else
        Log(LOG_ERROR, "%s.%s: negative() source argument missing\n", params.deviceName, params.propName);

    // warn on additional arguments
    if (args.size() >= 2)
        Log(LOG_WARNING, "%s.%s: extra arguments to negative() ignored\n", params.deviceName, params.propName);
}

// ---------------------------------------------------------------------------
//
// Offset logical axis source
//

OffsetAxisSource::OffsetAxisSource(const CtorParams &params, std::vector<std::string> &args)
{
    // args[0] (required) = underlying source expression
    if (args.size() >= 1)
        source = LogicalAxis::Configure(params, args[0].c_str(), params.propName);
    else
        Log(LOG_ERROR, "%s.%s: offset() source argument missing\n", params.deviceName, params.propName);

    // args[1] (required) = offset
    if (args.size() >= 2)
        offset = atoi(args[1].c_str());
    else
        Log(LOG_ERROR, "%s.%s: offset() offset-amount argument missing\n", params.deviceName, params.propName);

    // warn on additional arguments
    if (args.size() >= 3)
        Log(LOG_WARNING, "%s.%s: extra arguments to offset() ignored\n", params.deviceName, params.propName);
}

// ---------------------------------------------------------------------------
//
// Scaling logical axis source
//

ScaleAxisSource::ScaleAxisSource(const CtorParams &params, std::vector<std::string> &args)
{
    // args[0] (required) = underlying source expression
    if (args.size() >= 1)
        source = LogicalAxis::Configure(params, args[0].c_str(), params.propName);
    else
        Log(LOG_ERROR, "%s.%s: scale() source argument missing\n", params.deviceName, params.propName);

    // args[1] (required) = scale factor
    if (args.size() >= 2)
        scale = atof(args[1].c_str());
    else
        Log(LOG_ERROR, "%s.%s: scale() scaling fator argument missing\n", params.deviceName, params.propName);

    // warn on additional arguments
    if (args.size() >= 3)
        Log(LOG_WARNING, "%s.%s: extra arguments to scale() ignored\n", params.deviceName, params.propName);
}

// ---------------------------------------------------------------------------
//
// Absolute-value logical axis source
//

AbsAxisSource::AbsAxisSource(const CtorParams &params, std::vector<std::string> &args)
{
    // args[0] (required) = underlying source expression
    if (args.size() >= 1)
        source = LogicalAxis::Configure(params, args[0].c_str(), params.propName);
    else
        Log(LOG_ERROR, "%s.%s: abs() source argument missing\n", params.deviceName, params.propName);

    // warn on additional arguments
    if (args.size() >= 2)
        Log(LOG_WARNING, "%s.%s: extra arguments to abs() ignored\n", params.deviceName, params.propName);
}
