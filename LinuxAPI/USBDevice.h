// Pinscape Pico Config Tool - Linux USB Device Abstraction
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This provides platform-independent abstractions for USB device access
// using libusb on Linux.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <libusb-1.0/libusb.h>

namespace PinscapePico::Linux {

// USB Device Descriptor - platform-independent information about a USB device
struct USBDeviceInfo {
    std::string path;
    std::string serial;
    std::string manufacturer;
    std::string product;
    uint16_t vid;
    uint16_t pid;
    uint8_t bus;
    uint8_t port;
};

// Result codes for USB operations
enum class USBResult {
    Success = 0,
    NotFound = 1,
    AccessDenied = 2,
    DeviceLost = 3,
    InvalidParam = 4,
    Timeout = 5,
    IoError = 6,
    UnknownError = 7
};

// Convert libusb error codes to our result codes
inline USBResult LibUSBToResult(int libusb_err) {
    switch (libusb_err) {
        case LIBUSB_SUCCESS:
            return USBResult::Success;
        case LIBUSB_ERROR_NO_DEVICE:
            return USBResult::DeviceLost;
        case LIBUSB_ERROR_ACCESS:
            return USBResult::AccessDenied;
        case LIBUSB_ERROR_TIMEOUT:
            return USBResult::Timeout;
        case LIBUSB_ERROR_INVALID_PARAM:
            return USBResult::InvalidParam;
        case LIBUSB_ERROR_IO:
            return USBResult::IoError;
        default:
            return USBResult::UnknownError;
    }
}

inline const char* LibUSBErrorString(int libusb_err) {
    switch (libusb_err) {
        case LIBUSB_SUCCESS: return "Success";
        case LIBUSB_ERROR_IO: return "I/O error";
        case LIBUSB_ERROR_INVALID_PARAM: return "Invalid parameter";
        case LIBUSB_ERROR_ACCESS: return "Access denied";
        case LIBUSB_ERROR_NO_DEVICE: return "No device";
        case LIBUSB_ERROR_NOT_FOUND: return "Not found";
        case LIBUSB_ERROR_BUSY: return "Resource busy";
        case LIBUSB_ERROR_TIMEOUT: return "Timeout";
        case LIBUSB_ERROR_OVERFLOW: return "Overflow";
        case LIBUSB_ERROR_PIPE: return "Pipe error";
        case LIBUSB_ERROR_INTERRUPTED: return "Interrupted";
        case LIBUSB_ERROR_NO_MEM: return "No memory";
        case LIBUSB_ERROR_NOT_SUPPORTED: return "Not supported";
        default: return "Unknown error";
    }
}

inline const char* USBResultToString(USBResult r) {
    switch (r) {
        case USBResult::Success: return "Success";
        case USBResult::NotFound: return "Device not found";
        case USBResult::AccessDenied: return "Access denied";
        case USBResult::DeviceLost: return "Device lost";
        case USBResult::InvalidParam: return "Invalid parameter";
        case USBResult::Timeout: return "Operation timeout";
        case USBResult::IoError: return "I/O error";
        case USBResult::UnknownError: return "Unknown error";
    }
    return "Unknown error";
}

// USB Device Handle - wraps libusb device handle
class USBDevice {
public:
    USBDevice();
    ~USBDevice();

    // Delete copy constructor and assignment
    USBDevice(const USBDevice&) = delete;
    USBDevice& operator=(const USBDevice&) = delete;

    // Move constructor and assignment
    USBDevice(USBDevice&& other) noexcept;
    USBDevice& operator=(USBDevice&& other) noexcept;

    // Open device by VID/PID
    USBResult Open(uint16_t vid, uint16_t pid, const char* serial = nullptr);

    // Close device
    void Close();

    // Check if device is open
    bool IsOpen() const { return handle_ != nullptr; }

    // Claim interface
    USBResult ClaimInterface(int interface);

    // Release interface
    USBResult ReleaseInterface(int interface);

    // Send control transfer
    USBResult ControlTransfer(
        uint8_t request_type,
        uint8_t request,
        uint16_t value,
        uint16_t index,
        uint8_t* data,
        uint16_t length,
        unsigned int timeout = 1000);

    // Bulk write
    USBResult BulkWrite(
        uint8_t endpoint,
        const uint8_t* data,
        int length,
        int* bytes_transferred,
        unsigned int timeout = 1000);

    // Bulk read
    USBResult BulkRead(
        uint8_t endpoint,
        uint8_t* data,
        int length,
        int* bytes_transferred,
        unsigned int timeout = 1000);

    // Interrupt write
    USBResult InterruptWrite(
        uint8_t endpoint,
        const uint8_t* data,
        int length,
        int* bytes_transferred,
        unsigned int timeout = 1000);

    // Interrupt read
    USBResult InterruptRead(
        uint8_t endpoint,
        uint8_t* data,
        int length,
        int* bytes_transferred,
        unsigned int timeout = 1000);

    // Get device string descriptor
    std::string GetStringDescriptor(uint8_t desc_index);

private:
    libusb_device_handle* handle_;
};

// USB Context - manages libusb library initialization
class USBContext {
public:
    USBContext();
    ~USBContext();

    // Delete copy constructor and assignment
    USBContext(const USBContext&) = delete;
    USBContext& operator=(const USBContext&) = delete;

    // Get the libusb context
    libusb_context* Get() { return context_; }

    // Check if initialized successfully
    bool IsInitialized() const { return context_ != nullptr; }

    static USBContext& Instance();

private:
    libusb_context* context_;
};

} // namespace PinscapePico::Linux
