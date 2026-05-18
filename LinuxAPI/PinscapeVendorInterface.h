// Pinscape Pico Config Tool - Linux Vendor Interface
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Linux implementation of the Pinscape Pico Vendor Interface using libusb

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "USBDevice.h"

namespace PinscapePico::Linux {

// Pinscape Pico USB VID/PID
constexpr uint16_t PINSCAPE_VID = 0x1209;
constexpr uint16_t PINSCAPE_PID_1 = 0xEAEA;
constexpr uint16_t PINSCAPE_PID_2 = 0xEAEB;

// RP2 Bootloader PIDs
constexpr uint16_t RP2_VID = 0x2E8A;
constexpr uint16_t RP2_BOOTLOADER_PID = 0x0003;

// Vendor Interface endpoints (interface 2)
constexpr uint8_t VENDOR_BULK_OUT = 0x03;
constexpr uint8_t VENDOR_BULK_IN = 0x83;

// Device info struct
struct DeviceInfo {
    uint32_t unitNumber;
    std::string unitName;
    std::string serialNumber;
    std::string manufacturerName;
    std::string deviceName;
};

// Vendor Interface class
class PinscapeVendorInterface {
public:
    PinscapeVendorInterface();
    ~PinscapeVendorInterface();

    // Delete copy constructor and assignment
    PinscapeVendorInterface(const PinscapeVendorInterface&) = delete;
    PinscapeVendorInterface& operator=(const PinscapeVendorInterface&) = delete;

    // Move constructor and assignment
    PinscapeVendorInterface(PinscapeVendorInterface&& other) noexcept;
    PinscapeVendorInterface& operator=(PinscapeVendorInterface&& other) noexcept;

    // Open device by unit number
    bool Open(int unitNumber = -1);

    // Open device by serial number
    bool OpenBySerial(const std::string& serial);

    // Close connection
    void Close();

    // Check if connected
    bool IsConnected() const { return device_.IsOpen(); }

    // Query device info
    bool QueryDeviceInfo(DeviceInfo& info);

    // Get configuration file
    bool GetConfig(std::vector<uint8_t>& buffer, uint8_t fileID = 0);

    // Put configuration file
    bool PutConfig(const std::vector<uint8_t>& buffer, uint8_t fileID = 0);

    // Enter bootloader mode
    bool EnterBootLoader();

    // Get device statistics
    bool GetStatistics(std::vector<uint8_t>& buffer);

    // Enumerate available devices
    static bool EnumerateDevices(std::vector<DeviceInfo>& devices);

    // Send IR code
    bool SendIRCode(uint8_t protocol, uint8_t flags, uint64_t code);

    // Reboot device (normal mode)
    bool Reboot();

    // Reboot into safe mode
    bool RebootSafeMode();

    // Erase configuration file(s)
    bool EraseConfig(uint8_t fileID = 0xFF);

    // Factory reset (erase all config data)
    bool FactoryReset();

    // Query log data
    bool QueryLog(std::vector<uint8_t>& buffer, uint32_t& avail);

    // TV relay control
    bool PulseTVRelay();
    bool SetTVRelay(bool on);

private:
    USBDevice device_;
    uint32_t requestId_;

    // Helper to send request and receive response
    bool SendRequest(const void* request, size_t requestSize,
                    void* response, size_t responseSize,
                    const void* sendData = nullptr, size_t sendDataSize = 0,
                    void* recvData = nullptr, size_t recvDataSize = 0,
                    size_t* actualRecvSize = nullptr);

    // CRC-32 calculation for config uploads
    uint32_t ComputeCRC32(const uint8_t* data, size_t length);
};

} // namespace PinscapePico::Linux
