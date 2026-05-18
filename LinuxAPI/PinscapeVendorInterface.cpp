// Pinscape Pico Config Tool - Linux Vendor Interface Implementation
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include "PinscapeVendorInterface.h"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <unistd.h>  // for usleep

// Include the real protocol definitions from the shared USBProtocol folder
#include "VendorIfcProtocol.h"

using namespace PinscapePico;

namespace PinscapePico::Linux {

PinscapeVendorInterface::PinscapeVendorInterface() : requestId_(0) {}

PinscapeVendorInterface::~PinscapeVendorInterface() {
    Close();
}

PinscapeVendorInterface::PinscapeVendorInterface(PinscapeVendorInterface&& other) noexcept
    : device_(std::move(other.device_)), requestId_(other.requestId_) {
    other.requestId_ = 0;
}

PinscapeVendorInterface& PinscapeVendorInterface::operator=(PinscapeVendorInterface&& other) noexcept {
    if (this != &other) {
        Close();
        device_ = std::move(other.device_);
        requestId_ = other.requestId_;
        other.requestId_ = 0;
    }
    return *this;
}

bool PinscapeVendorInterface::Open(int unitNumber) {
    // Try to open device with specified VID/PID (try both PID variants)
    USBResult result = device_.Open(PINSCAPE_VID, PINSCAPE_PID_1);
    if (result != USBResult::Success) {
        result = device_.Open(PINSCAPE_VID, PINSCAPE_PID_2);
    }

    if (result != USBResult::Success) {
        std::cerr << "Error: Failed to open Pinscape Pico device: " << USBResultToString(result) << std::endl;
        return false;
    }

    // Claim the vendor interface (interface 2)
    result = device_.ClaimInterface(2);
    if (result != USBResult::Success) {
        std::cerr << "Error: Failed to claim vendor interface: " << USBResultToString(result) << std::endl;
        device_.Close();
        return false;
    }

    return true;
}

bool PinscapeVendorInterface::OpenBySerial(const std::string& serial) {
    // Try to open device with specified serial number
    USBResult result = device_.Open(PINSCAPE_VID, PINSCAPE_PID_1, serial.c_str());
    if (result != USBResult::Success) {
        result = device_.Open(PINSCAPE_VID, PINSCAPE_PID_2, serial.c_str());
    }

    if (result != USBResult::Success) {
        std::cerr << "Error: Failed to open device with serial " << serial << ": "
                  << USBResultToString(result) << std::endl;
        return false;
    }

    // Claim the vendor interface (interface 2)
    result = device_.ClaimInterface(2);
    if (result != USBResult::Success) {
        std::cerr << "Error: Failed to claim vendor interface: " << USBResultToString(result) << std::endl;
        device_.Close();
        return false;
    }

    return true;
}

void PinscapeVendorInterface::Close() {
    if (device_.IsOpen()) {
        device_.ReleaseInterface(2);
        device_.Close();
    }
}

bool PinscapeVendorInterface::SendRequest(const void* request, size_t requestSize,
                                  void* response, size_t responseSize,
                                  const void* sendData, size_t sendDataSize,
                                  void* recvData, size_t recvDataSize,
                                  size_t* actualRecvSize) {
    if (!device_.IsOpen()) {
        std::cerr << "Error: Device not connected" << std::endl;
        return false;
    }

    // Send request
    int transferred;
    USBResult result = device_.BulkWrite(VENDOR_BULK_OUT,
                                        static_cast<const uint8_t*>(request),
                                        requestSize, &transferred);

    if (result != USBResult::Success || transferred != (int)requestSize) {
        std::cerr << "Error: Failed to send request: " << USBResultToString(result) << std::endl;
        return false;
    }

    // Send additional data if provided
    if (sendData && sendDataSize > 0) {
        result = device_.BulkWrite(VENDOR_BULK_OUT,
                                  static_cast<const uint8_t*>(sendData),
                                  sendDataSize, &transferred);

        if (result != USBResult::Success) {
            std::cerr << "Error: Failed to send request data: " << USBResultToString(result) << std::endl;
            return false;
        }
    }

    // Read response header - use a USB packet-sized buffer (64 bytes is common packet size)
    uint8_t respBuffer[64];
    memset(respBuffer, 0, sizeof(respBuffer));

    // Sometimes USB devices send zero-length packets, so retry once if we get 0 bytes
    int retryCount = 0;
    do {
        result = device_.BulkRead(VENDOR_BULK_IN,
                                 respBuffer,
                                 sizeof(respBuffer), &transferred,
                                 5000);  // 5 second timeout

        if (result != USBResult::Success) {
            std::cerr << "Error: Failed to read response: " << USBResultToString(result) << std::endl;
            return false;
        }

        // If we got data, break out
        if (transferred > 0) {
            break;
        }

        // Zero-length packet received, retry once
        retryCount++;

    } while (retryCount < 2 && transferred == 0);

    // We need at least enough for a basic response header
    if (transferred < 8) {  // Minimum response is token(4) + cmd(1) + argsSize(1) + status(2)
        std::cerr << "Error: Response too short (" << transferred << " bytes) after "
                  << retryCount + 1 << " attempts" << std::endl;
        return false;
    }

    // Copy response header to caller's buffer
    memcpy(response, respBuffer, std::min(responseSize, (size_t)transferred));

    // Check if there's additional data to read based on response xferBytes field
    VendorResponse* resp = reinterpret_cast<VendorResponse*>(respBuffer);

    if (resp->xferBytes > 0 && recvData) {
        // Check if data is already in the buffer we received
        size_t headerSize = sizeof(VendorResponse);
        size_t bytesReceived = 0;

        if (transferred > (int)headerSize) {
            // Some data came with the response
            size_t dataInBuffer = transferred - headerSize;
            size_t dataToCopy = std::min(dataInBuffer, recvDataSize);
            memcpy(recvData, respBuffer + headerSize, dataToCopy);
            bytesReceived = dataToCopy;
        }

        // Check if we need to read more data
        if (bytesReceived < resp->xferBytes && bytesReceived < recvDataSize) {
            // Need to read additional data - use a large buffer to avoid overflow
            size_t bytesRemaining = std::min((size_t)(resp->xferBytes - bytesReceived), recvDataSize - bytesReceived);
            uint8_t dataBuffer[4096];  // Large buffer for remaining data

            result = device_.BulkRead(VENDOR_BULK_IN,
                                     dataBuffer,
                                     sizeof(dataBuffer), &transferred,
                                     5000);  // 5 second timeout

            if (result != USBResult::Success) {
                std::cerr << "Error: Failed to read remaining response data: " << USBResultToString(result) << std::endl;
                return false;
            }

            // Copy the data we actually wanted
            size_t dataToCopy = std::min((size_t)transferred, bytesRemaining);
            memcpy(static_cast<uint8_t*>(recvData) + bytesReceived, dataBuffer, dataToCopy);
            bytesReceived += dataToCopy;
        }

        if (actualRecvSize) {
            *actualRecvSize = bytesReceived;
        }
    } else if (actualRecvSize) {
        *actualRecvSize = 0;
    }

    return true;
}

bool PinscapeVendorInterface::QueryDeviceInfo(DeviceInfo& info) {
    VendorRequest req(++requestId_, VendorRequest::CMD_QUERY_IDS, 0);
    VendorResponse resp;

    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Device returned status " << resp.status << std::endl;
        return false;
    }

    if (resp.token != req.token) {
        std::cerr << "Error: Response token mismatch" << std::endl;
        return false;
    }

    info.unitNumber = resp.args.id.unitNum;
    info.serialNumber = "Unknown";  // TODO: Get from device descriptor
    info.manufacturerName = "Pinscape";
    info.deviceName = "Pinscape Pico";

    return true;
}

bool PinscapeVendorInterface::GetConfig(std::vector<uint8_t>& buffer, uint8_t fileID) {
    buffer.clear();

    // Read config page by page (4K pages)
    const size_t pageSize = 4096;  // VendorRequest::CONFIG_PAGE_SIZE
    uint16_t page = 0;

    while (true) {
        // Delay between requests to let device prepare next page
        if (page > 0) {
            usleep(200000);  // 200ms delay - device needs time to prepare page
        }

        VendorRequest req(++requestId_, VendorRequest::CMD_CONFIG, 0);
        req.args.config.subcmd = VendorRequest::SUBCMD_CONFIG_GET;
        req.args.config.fileID = fileID;
        req.args.config.page = page;
        req.argsSize = sizeof(req.args.config);

        VendorResponse resp;
        std::vector<uint8_t> pageData(pageSize);
        size_t actualSize = 0;

        if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp),
                        nullptr, 0, pageData.data(), pageData.size(), &actualSize)) {
            return false;
        }

        if (resp.status == VendorResponse::ERR_EOF) {
            // End of file
            break;
        }

        if (resp.status == VendorResponse::ERR_NOT_FOUND) {
            std::cerr << "Error: Config file not found" << std::endl;
            return false;
        }

        if (resp.status != VendorResponse::OK) {
            std::cerr << "Error: Failed to read config page " << page
                     << " (status=" << resp.status << ")" << std::endl;
            return false;
        }

        // Append page data to buffer
        buffer.insert(buffer.end(), pageData.begin(), pageData.begin() + actualSize);
        page++;
    }

    return true;
}

uint32_t PinscapeVendorInterface::ComputeCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

bool PinscapeVendorInterface::PutConfig(const std::vector<uint8_t>& buffer, uint8_t fileID) {
    // Calculate CRC-32 of entire file
    uint32_t crc32 = ComputeCRC32(buffer.data(), buffer.size());

    // Send start marker (page 0xFFFF)
    {
        VendorRequest req(++requestId_, VendorRequest::CMD_CONFIG, 0);
        req.args.config.subcmd = VendorRequest::SUBCMD_CONFIG_PUT;
        req.args.config.fileID = fileID;
        req.args.config.page = 0xFFFF;
        req.args.config.crc = crc32;
        req.argsSize = sizeof(req.args.config);

        VendorResponse resp;
        if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
            return false;
        }

        if (resp.status != VendorResponse::OK) {
            std::cerr << "Error: Failed to start config upload (status="
                     << resp.status << ")" << std::endl;
            return false;
        }
    }

    // Send config data in 4K pages
    const size_t pageSize = 4096;  // VendorRequest::CONFIG_PAGE_SIZE
    size_t totalPages = (buffer.size() + pageSize - 1) / pageSize;

    for (size_t page = 0; page < totalPages; page++) {
        size_t offset = page * pageSize;
        size_t thisPageSize = std::min(pageSize, buffer.size() - offset);

        VendorRequest req(++requestId_, VendorRequest::CMD_CONFIG, thisPageSize);
        req.args.config.subcmd = VendorRequest::SUBCMD_CONFIG_PUT;
        req.args.config.fileID = fileID;
        req.args.config.page = page;
        req.args.config.nPages = totalPages;

        // Set CRC on last page only
        if (page == totalPages - 1) {
            req.args.config.crc = crc32;
        }

        req.argsSize = sizeof(req.args.config);

        VendorResponse resp;
        if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp),
                        buffer.data() + offset, thisPageSize)) {
            return false;
        }

        if (resp.status != VendorResponse::OK && resp.status != VendorResponse::ERR_RETRY_OK) {
            std::cerr << "Error: Failed to send config page " << page
                     << " (status=" << resp.status << ")" << std::endl;
            return false;
        }
    }

    return true;
}

bool PinscapeVendorInterface::EnterBootLoader() {
    VendorRequest req(++requestId_, VendorRequest::CMD_RESET, 0);
    req.args.argBytes[0] = VendorRequest::SUBCMD_RESET_BOOTLOADER;
    req.argsSize = 1;

    VendorResponse resp;

    // Note: Device will disconnect, so we may not get a response
    SendRequest(&req, sizeof(req), &resp, sizeof(resp));

    // Close connection as device will disconnect
    Close();
    return true;
}

bool PinscapeVendorInterface::GetStatistics(std::vector<uint8_t>& buffer) {
    VendorRequest req(++requestId_, VendorRequest::CMD_STATS, 0);
    req.args.argBytes[0] = VendorRequest::SUBCMD_STATS_QUERY_STATS;
    req.argsSize = 1;

    VendorResponse resp;
    buffer.resize(4096);  // Max size for stats
    size_t actualSize = 0;

    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp),
                    nullptr, 0, buffer.data(), buffer.size(), &actualSize)) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to get statistics (status=" << resp.status << ")" << std::endl;
        return false;
    }

    buffer.resize(actualSize);
    return true;
}

bool PinscapeVendorInterface::EnumerateDevices(std::vector<DeviceInfo>& devices) {
    devices.clear();

    // Simple implementation: Try to open a device and if successful, add it
    USBDevice testDevice;
    USBResult result = testDevice.Open(PINSCAPE_VID, PINSCAPE_PID_1);
    if (result == USBResult::Success) {
        DeviceInfo info;
        info.serialNumber = "Unknown";
        info.manufacturerName = "Pinscape";
        info.deviceName = "Pinscape Pico";
        info.unitNumber = 0;
        devices.push_back(info);
        testDevice.Close();
        return true;
    }

    result = testDevice.Open(PINSCAPE_VID, PINSCAPE_PID_2);
    if (result == USBResult::Success) {
        DeviceInfo info;
        info.serialNumber = "Unknown";
        info.manufacturerName = "Pinscape";
        info.deviceName = "Pinscape Pico";
        info.unitNumber = 0;
        devices.push_back(info);
        testDevice.Close();
        return true;
    }

    return devices.size() > 0;
}

bool PinscapeVendorInterface::SendIRCode(uint8_t protocol, uint8_t flags, uint64_t code) {
    VendorRequest req(++requestId_, VendorRequest::CMD_SEND_IR, 0);
    req.args.sendIR.protocol = protocol;
    req.args.sendIR.flags = flags;
    req.args.sendIR.code = code;
    req.args.sendIR.count = 1;
    req.argsSize = sizeof(req.args.sendIR);

    VendorResponse resp;
    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to send IR code (status=" << resp.status << ")" << std::endl;
        return false;
    }

    return true;
}

bool PinscapeVendorInterface::Reboot() {
    VendorRequest req(++requestId_, VendorRequest::CMD_RESET, 0);
    req.args.argBytes[0] = VendorRequest::SUBCMD_RESET_NORMAL;
    req.argsSize = 1;

    VendorResponse resp;

    // Device will disconnect, so we may not get a response
    SendRequest(&req, sizeof(req), &resp, sizeof(resp));

    Close();
    return true;
}

bool PinscapeVendorInterface::RebootSafeMode() {
    VendorRequest req(++requestId_, VendorRequest::CMD_RESET, 0);
    req.args.argBytes[0] = VendorRequest::SUBCMD_RESET_SAFEMODE;
    req.argsSize = 1;

    VendorResponse resp;

    // Device will disconnect
    SendRequest(&req, sizeof(req), &resp, sizeof(resp));

    Close();
    return true;
}

bool PinscapeVendorInterface::EraseConfig(uint8_t fileID) {
    VendorRequest req(++requestId_, VendorRequest::CMD_CONFIG, 0);
    req.args.config.subcmd = VendorRequest::SUBCMD_CONFIG_ERASE;
    req.args.config.fileID = fileID;
    req.argsSize = sizeof(req.args.config);

    VendorResponse resp;
    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to erase config (status=" << resp.status << ")" << std::endl;
        return false;
    }

    return true;
}

bool PinscapeVendorInterface::FactoryReset() {
    VendorRequest req(++requestId_, VendorRequest::CMD_CONFIG, 0);
    req.args.config.subcmd = VendorRequest::SUBCMD_CONFIG_RESET;
    req.argsSize = sizeof(req.args.config);

    VendorResponse resp;
    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to factory reset (status=" << resp.status << ")" << std::endl;
        return false;
    }

    return true;
}

bool PinscapeVendorInterface::QueryLog(std::vector<uint8_t>& buffer, uint32_t& avail) {
    VendorRequest req(++requestId_, VendorRequest::CMD_QUERY_LOG, 0);

    VendorResponse resp;
    buffer.resize(4096);
    size_t actualSize = 0;

    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp),
                    nullptr, 0, buffer.data(), buffer.size(), &actualSize)) {
        return false;
    }

    if (resp.status == VendorResponse::ERR_EOF) {
        buffer.clear();
        avail = 0;
        return true;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to query log (status=" << resp.status << ")" << std::endl;
        return false;
    }

    buffer.resize(actualSize);
    avail = resp.args.log.avail;
    return true;
}

bool PinscapeVendorInterface::PulseTVRelay() {
    VendorRequest req(++requestId_, VendorRequest::CMD_TVON, 0);
    req.args.argBytes[0] = VendorRequest::SUBCMD_TVON_SET_RELAY;
    req.args.argBytes[1] = VendorRequest::TVON_RELAY_PULSE;
    req.argsSize = 2;

    VendorResponse resp;
    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to pulse TV relay (status=" << resp.status << ")" << std::endl;
        return false;
    }

    return true;
}

bool PinscapeVendorInterface::SetTVRelay(bool on) {
    VendorRequest req(++requestId_, VendorRequest::CMD_TVON, 0);
    req.args.argBytes[0] = VendorRequest::SUBCMD_TVON_SET_RELAY;
    req.args.argBytes[1] = on ? VendorRequest::TVON_RELAY_ON : VendorRequest::TVON_RELAY_OFF;
    req.argsSize = 2;

    VendorResponse resp;
    if (!SendRequest(&req, sizeof(req), &resp, sizeof(resp))) {
        return false;
    }

    if (resp.status != VendorResponse::OK) {
        std::cerr << "Error: Failed to set TV relay (status=" << resp.status << ")" << std::endl;
        return false;
    }

    return true;
}

} // namespace PinscapePico::Linux
