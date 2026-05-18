// Pinscape Pico Config Tool - Linux USB Device Implementation
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include "USBDevice.h"
#include <cstring>
#include <iostream>

namespace PinscapePico::Linux {

// Global libusb context
static USBContext* g_usb_context = nullptr;

USBContext::USBContext() : context_(nullptr) {
    libusb_init(&context_);
}

USBContext::~USBContext() {
    if (context_) {
        libusb_exit(context_);
    }
}

USBContext& USBContext::Instance() {
    static USBContext instance;
    return instance;
}

USBDevice::USBDevice() : handle_(nullptr) {}

USBDevice::~USBDevice() {
    Close();
}

USBDevice::USBDevice(USBDevice&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

USBDevice& USBDevice::operator=(USBDevice&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

USBResult USBDevice::Open(uint16_t vid, uint16_t pid, const char* serial) {
    Close();

    libusb_context* ctx = USBContext::Instance().Get();
    if (!ctx) {
        return USBResult::UnknownError;
    }

    // Find device with matching VID/PID
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &device_list);
    if (count < 0) {
        return LibUSBToResult(static_cast<int>(count));
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* dev = device_list[i];
        libusb_device_descriptor desc;
        int r = libusb_get_device_descriptor(dev, &desc);
        if (r != LIBUSB_SUCCESS) {
            continue;
        }

        if (desc.idVendor != vid || desc.idProduct != pid) {
            continue;
        }

        // If serial is specified, check it
        if (serial) {
            libusb_device_handle* temp_handle;
            r = libusb_open(dev, &temp_handle);
            if (r != LIBUSB_SUCCESS) {
                continue;
            }

            std::string device_serial = GetStringDescriptor(desc.iSerialNumber);
            libusb_close(temp_handle);

            if (device_serial != serial) {
                continue;
            }
        }

        // Try to open the device
        int r_open = libusb_open(dev, &handle_);
        libusb_free_device_list(device_list, 1);

        if (r_open == LIBUSB_SUCCESS) {
            return USBResult::Success;
        } else {
            handle_ = nullptr;
            return LibUSBToResult(r_open);
        }
    }

    libusb_free_device_list(device_list, 1);
    return USBResult::NotFound;
}

void USBDevice::Close() {
    if (handle_) {
        libusb_close(handle_);
        handle_ = nullptr;
    }
}

USBResult USBDevice::ClaimInterface(int interface) {
    if (!handle_) return USBResult::InvalidParam;

    // First try to detach kernel driver if attached
    libusb_detach_kernel_driver(handle_, interface);

    int r = libusb_claim_interface(handle_, interface);
    if (r != LIBUSB_SUCCESS) {
        // Try other common interfaces if the primary one fails
        if (interface == 0) {
            r = libusb_claim_interface(handle_, 1);
            if (r != LIBUSB_SUCCESS) {
                r = libusb_claim_interface(handle_, 2);
            }
        }
    }

    return r == LIBUSB_SUCCESS ? USBResult::Success : LibUSBToResult(r);
}

USBResult USBDevice::ReleaseInterface(int interface) {
    if (!handle_) return USBResult::InvalidParam;

    int r = libusb_release_interface(handle_, interface);
    return r == LIBUSB_SUCCESS ? USBResult::Success : LibUSBToResult(r);
}

USBResult USBDevice::ControlTransfer(
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t* data,
    uint16_t length,
    unsigned int timeout) {

    if (!handle_) return USBResult::InvalidParam;

    int r = libusb_control_transfer(
        handle_,
        request_type,
        request,
        value,
        index,
        data,
        length,
        timeout);

    if (r < 0) {
        return LibUSBToResult(r);
    }
    return USBResult::Success;
}

USBResult USBDevice::BulkWrite(
    uint8_t endpoint,
    const uint8_t* data,
    int length,
    int* bytes_transferred,
    unsigned int timeout) {

    if (!handle_) return USBResult::InvalidParam;

    int r = libusb_bulk_transfer(
        handle_,
        endpoint,
        const_cast<uint8_t*>(data),
        length,
        bytes_transferred,
        timeout);

    if (r < 0) {
        return LibUSBToResult(r);
    }
    return USBResult::Success;
}

USBResult USBDevice::BulkRead(
    uint8_t endpoint,
    uint8_t* data,
    int length,
    int* bytes_transferred,
    unsigned int timeout) {

    if (!handle_) return USBResult::InvalidParam;

    int r = libusb_bulk_transfer(
        handle_,
        endpoint,
        data,
        length,
        bytes_transferred,
        timeout);

    if (r < 0) {
        return LibUSBToResult(r);
    }

    return USBResult::Success;
}

USBResult USBDevice::InterruptWrite(
    uint8_t endpoint,
    const uint8_t* data,
    int length,
    int* bytes_transferred,
    unsigned int timeout) {

    if (!handle_) return USBResult::InvalidParam;

    int r = libusb_interrupt_transfer(
        handle_,
        endpoint,
        const_cast<uint8_t*>(data),
        length,
        bytes_transferred,
        timeout);

    if (r < 0) {
        return LibUSBToResult(r);
    }
    return USBResult::Success;
}

USBResult USBDevice::InterruptRead(
    uint8_t endpoint,
    uint8_t* data,
    int length,
    int* bytes_transferred,
    unsigned int timeout) {

    if (!handle_) return USBResult::InvalidParam;

    int r = libusb_interrupt_transfer(
        handle_,
        endpoint,
        data,
        length,
        bytes_transferred,
        timeout);

    if (r < 0) {
        return LibUSBToResult(r);
    }
    return USBResult::Success;
}

std::string USBDevice::GetStringDescriptor(uint8_t desc_index) {
    if (!handle_ || desc_index == 0) {
        return std::string();
    }

    unsigned char buffer[256];
    int len = libusb_get_string_descriptor_ascii(
        handle_,
        desc_index,
        buffer,
        sizeof(buffer));

    if (len < 0) {
        return std::string();
    }

    return std::string(reinterpret_cast<char*>(buffer), len);
}

} // namespace PinscapePico::Linux
