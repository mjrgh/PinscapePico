// Pinscape Pico Button Latency Tester II - USB implementation
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/unique_id.h>

// Tinyusb headers
#include <tusb.h>
#include <bsp/board.h>

// local project headers
#include "../../Firmware/USBCDC.h"
#include "USBImpl.h"
#include "VendorIfc.h"
#include "PicoLED.h"

// global singleton
USBImpl usbImpl;

USBImpl::USBImpl()
{
}

// ----------------------------------------------------------------------------
//
// Initialization
//

// initialize
void USBImpl::Init(uint16_t vid, uint16_t pid)
{
    // set the vid/pid
    this->vid = vid;
    this->pid = pid;
    
    // set up our string descriptors
    AddStringDescriptorBytes(STRDESC_LANG, (const uint8_t[]){ 0x09, 0x04 }, 2);     // HID - Language (English, 0x0409)
    AddStringDescriptorText(STRDESC_MANUF, "Pinscape Labs");                        // HID - Manufacturer Name
    AddStringDescriptorText(STRDESC_PRODUCT, "Pinscape Button Latency Tester II");  // HID - Product Name
    AddStringDescriptorText(STRDESC_VENIFC, "PinscapeBLT2Control");                 // Vendor interface display name
    AddStringDescriptorText(STRDESC_CDCIFC, "ButtonLatencyTester Terminal");        // CDC interface display name

    // Byte-to-hex-digits formatter.  To save a little work later
    // on, we'll build the string in little-Endian 16-bit Unicode
    // format.
    static const auto PutChar = [](uint8_t* &p, char c) {
        *p++ = static_cast<uint8_t>(c);
        *p++ = 0;
    };
    static const auto PutStr = [](uint8_t* &p, const char *s) {
        while (*s != 0)
            PutChar(p, *s++);
    };
    static const auto PutHex = [](uint8_t* &p, uint8_t n) {
        PutChar(p, static_cast<char>(n < 10 ? n + '0' : n - 10 + 'A'));
    };
    static const auto ByteToHex = [](uint8_t* &p, uint8_t b) {
        PutHex(p, (b >> 4) & 0x0F);
        PutHex(p, b & 0x0F);
    };
    
    // buffer to accumulate the serial number under construction
    uint8_t buf[64];
    uint8_t *bufp = buf;

    // start with a device type prefix
    PutStr(bufp, "BLT2-");
    
    // add the Pico's hardware ID
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    for (int i = 0 ; i < 8 ; ++i)
        ByteToHex(bufp, id.id[i]);
    
    // add a two-hex-digit version suffix
    ByteToHex(bufp, DEVICE_USB_VERSION);
    
    // Add the string to the descriptor table.  Since we've been building
    // the string in 16-bit Unicode format all along, we can directly add
    // the byte string without any transcoding.
    AddStringDescriptorBytes(STRDESC_SERIAL, buf, bufp - buf);    // String index 3 - Serial Number

    // initialize tinyusb
    board_init();
    tusb_init();
}

// ----------------------------------------------------------------------------
//
// Bus state events
//

void USBImpl::OnBusSuspended(bool remoteWakeEnabled)
{
    // enter suspend mode, and note whether or not we're allowed to wake the host
    suspended = true;
    this->remoteWakeEnabled = remoteWakeEnabled;

    // handle the state change
    OnDeviceStateChanged();
}

void USBImpl::OnBusResumed()
{
    // exit suspend mode, and assume that remote wakeup will be enabled the next
    // time we find ourselves in suspend mode
    suspended = false;
    remoteWakeEnabled = true;

    // handle the state change
    OnDeviceStateChanged();
}

void USBImpl::OnDeviceStateChanged()
{
    // set standby flash mode
    picoLED.SetBlinkPattern(
        mounted ? (suspended ? PicoLED::Pattern::Suspended : PicoLED::Pattern::Connected) :
        PicoLED::Pattern::Disconnected);
}

// ----------------------------------------------------------------------------
//
// String descriptors
//

void USBImpl::AddStringDescriptorText(uint8_t index, const char *str)
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

void USBImpl::AddStringDescriptorBytes(uint8_t index, const uint8_t *data, size_t nDataBytes)
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

const uint16_t *USBImpl::GetStringDescriptor(uint8_t index, uint16_t /*langId*/)
{
    // If it's in range, return the descriptor from our table, otherwise
    // return null.
    //
    // Note that Tinyusb wants a pointer to a uint16_t array, whereas our
    // table element is a byte vector.  We store the byte vector because we
    // want to be sure that the bytes are in the correct endian order for
    // wire transmission, without further translation every time we get a
    // request.  As it happens, that's what Tinyusb wants, too, even though
    // its interface suggests otherwise.  The uint16_t* return type in the
    // interface says that Tinyusb wants a native uint16_t array, but it
    // doesn't; it actually wants a byte array that it can pass back to
    // the host without translation (the first thing that the Tinusb caller
    // will do on return is reinterpret_cast the result pointer back to
    // uint8_t*).  If Tinyusb really wanted a uint16_t array here, it would
    // translate the local byte order to wire byte order (little Endian)
    // on our return, which it doesn't.  It works out the same way on most
    // device platforms anyway, since almost everyone's little-Endian these
    // days, but it's still a design error in the interface.  The type cast
    // here isn't just safe, but actually *correct*; it just compensates for
    // the error in the API design.  The value we're passing back is exactly
    // what the caller really wants.
    if (auto it = stringDescriptors.find(index) ; it != stringDescriptors.end())
        return reinterpret_cast<const uint16_t*>(it->second.data());
    else
        return nullptr;
}

// ----------------------------------------------------------------------------
//
// Device descriptor
//

const uint8_t *USBImpl::GetDeviceDescriptor()
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

// ----------------------------------------------------------------------------
//
// Configuration descriptor
//

const uint8_t *USBImpl::GetConfigurationDescriptor(int /*index*/)
{
    // if we haven't built the configuration descriptor yet, do so now
    if (cfgDesc == nullptr)
    {
        // Figure the number of interfaces included
        uint8_t nCDC = 2;               // CDC (virtual COM port); each counts as two interfaces, for notification & data
        uint8_t nVen = 1;               // Private vendor interface; uses WinUsb on Windows
        uint8_t nIfc = nCDC + nVen;     // total number of interfaces

        // Figure the size of the configuration descriptor.  The descriptor consists
        // of the config descriptor header plus each interface descriptor.
        uint16_t cfgDescLen =
            TUD_CONFIG_DESC_LEN         // the config descriptor itself
            + TUD_CDC_DESC_LEN          // CDC descriptor
            + TUD_VENDOR_DESC_LEN;      // our vendor interface descriptor

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

        // Add the CDC interface descriptor
        // Interface number, string index, EP notification in address, EP notification size, EP data address out, EP data address in, EP data size
        const uint8_t cdc[] = { TUD_CDC_DESCRIPTOR(IFCNUM_CDC, STRDESC_CDCIFC, EndpointNotifyCDC, 8, EndpointOutCDC, EndpointInCDC, CFG_TUD_CDC_EP_BUFSIZE) };
        static_assert(sizeof(cdc) == TUD_CDC_DESC_LEN);
        Append(p, cdc, sizeof(cdc));

        // Add our vendor interface descriptor
        // Interface number, string index, EP Out & IN address, EP size
        const uint8_t ven[] = { TUD_VENDOR_DESCRIPTOR(IFCNUM_VENDOR, STRDESC_VENIFC, EndpointOutVendor, EndpointInVendor, CFG_TUD_VENDOR_EP_BUFSIZE) };
        static_assert(sizeof(ven) == TUD_VENDOR_DESC_LEN);
        Append(p, ven, sizeof(ven));
    }

    // return the cached descriptor
    return cfgDesc.get();
}

// ----------------------------------------------------------------------------
//
// Microsoft descriptors - for automatic WinUsb driver setup
//

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
const uint8_t *USBImpl::GetMSOSDescriptor(size_t *byteSize)
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
    static const MS_OS_DESC_SET ms_os_desc_set(0, USBImpl::IFCNUM_VENDOR, VendorIfc::WINUSB_GUID);

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
const uint8_t *USBImpl::GetBOSDescriptor()
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
        TUD_BOS_MS_OS_20_DESCRIPTOR(msosDescSize, USBImpl::VENDOR_REQUEST_WINUSB),
    };

    // return the BOS descriptor
    return bosDescriptor;
}

// ----------------------------------------------------------------------------
//
// Periodic tasks
//
void USBImpl::Task()
{
    // run pending tinyusb device tasks
    tud_task();

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
            vendorIfc.OnOutput(venbuf, nBytes);
    }
}

// ----------------------------------------------------------------------------
//
// Vendor interface operations
//

bool USBImpl::VendorControlXfer(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
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


// ----------------------------------------------------------------------------
//
// Tinyusb callbacks.  We refer most of these to the singleton USBImpl
// instance.
//

// Tinyusb Get Configuration Descriptor callback
const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return usbImpl.GetConfigurationDescriptor(index);
}

// Tinyusb Get BOS descriptor callback
const uint8_t *tud_descriptor_bos_cb()
{
    return usbImpl.GetBOSDescriptor();
}

// get the device descriptor
const uint8_t *tud_descriptor_device_cb()
{
    return usbImpl.GetDeviceDescriptor();
}

// get a string descriptor
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langId)
{
    return usbImpl.GetStringDescriptor(index, langId);
}

// vendor control transfer callback - invoked when a control transfer occurs on a vendor interface
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    return usbImpl.VendorControlXfer(rhport, stage, request);
}

// device mounted
void tud_mount_cb()
{
    usbImpl.OnMountDevice();
}

// device dismounted
void tud_umount_cb()
{
    usbImpl.OnDismountDevice();
}

// bus suspended, entering low power mode
void tud_suspend_cb(bool remoteWakeEnabled)
{
    usbImpl.OnBusSuspended(remoteWakeEnabled);
}

// bus resumed from low-power mode
void tud_resume_cb()
{
    usbImpl.OnBusResumed();
}

// User-mode SOF callback.  We don't do anything here, but we provide
// it because we have to enable the SOF callback in order to let TinyUSB
// know that we want the SOF interrupt enabled at the hardware level.
// The callback must be explicitly enabled via tud_sof_cb_enable(true).
void tud_sof_cb(uint32_t frame_count)
{
}
