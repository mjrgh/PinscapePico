// Pinscape Pico Config Tool - Linux Feedback Controller HID Interface Implementation
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#include "FeedbackControllerInterface.h"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>

namespace PinscapePico::Linux {

std::string FeedbackControllerInterface::IRReport::ToString() const {
    char buf[64];
    uint32_t code = (static_cast<uint32_t>(command_hi2) << 24) |
                   (static_cast<uint32_t>(command_hi) << 16) |
                   (static_cast<uint32_t>(command_mid) << 8) |
                   command_lo;
    snprintf(buf, sizeof(buf), "%02X.%02X.%08X", protocol, flags, code);
    return std::string(buf);
}

FeedbackControllerInterface::FeedbackControllerInterface()
    : fd_(-1) {}

FeedbackControllerInterface::~FeedbackControllerInterface() {
    Close();
}

FeedbackControllerInterface::FeedbackControllerInterface(FeedbackControllerInterface&& other) noexcept
    : fd_(other.fd_), device_path_(std::move(other.device_path_)) {
    other.fd_ = -1;
}

FeedbackControllerInterface& FeedbackControllerInterface::operator=(FeedbackControllerInterface&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        device_path_ = std::move(other.device_path_);
        other.fd_ = -1;
    }
    return *this;
}

bool FeedbackControllerInterface::Open(uint16_t vid, uint16_t pid) {
    // Scan /dev/hidraw* devices
    DIR* dir = opendir("/dev");
    if (!dir) {
        std::cerr << "Error: Could not open /dev directory" << std::endl;
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "hidraw", 6) == 0) {
            std::string device_path = std::string("/dev/") + entry->d_name;

            int test_fd = open(device_path.c_str(), O_RDWR | O_NONBLOCK);
            if (test_fd < 0)
                continue;

            // Test if this is the right device by sending a status query
            uint8_t req[64];
            memset(req, 0, sizeof(req));
            req[0] = 0x04;  // Report ID for feedback controller
            req[1] = 0x02;  // REQ_QUERY_STATUS

            if (write(test_fd, req, 64) > 0) {
                // Try to read a response
                uint8_t resp[64];
                struct pollfd pfd = {test_fd, POLLIN, 0};
                if (poll(&pfd, 1, 100) > 0) {
                    if (read(test_fd, resp, sizeof(resp)) > 0) {
                        // Found the device!
                        device_path_ = device_path;
                        fd_ = test_fd;
                        closedir(dir);
                        return true;
                    }
                }
            }

            close(test_fd);
        }
    }

    closedir(dir);
    std::cerr << "Error: Could not find Pinscape Feedback Controller HID interface" << std::endl;
    std::cerr << "Ensure the hidraw device is accessible (udev rules may be needed)" << std::endl;
    return false;
}

void FeedbackControllerInterface::Close() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool FeedbackControllerInterface::SetNightMode(bool on, int timeout_ms) {
    if (fd_ < 0)
        return false;

    uint8_t req[64];
    memset(req, 0, sizeof(req));
    req[0] = 0x04;  // Report ID
    req[1] = 0x10;  // REQ_NIGHT_MODE
    req[2] = on ? 1 : 0;

    if (write(fd_, req, 64) <= 0) {
        std::cerr << "Error: Failed to write night mode request" << std::endl;
        return false;
    }

    return true;
}

bool FeedbackControllerInterface::QueryStatus(StatusReport& status, int timeout_ms) {
    if (fd_ < 0)
        return false;

    uint8_t req[64];
    memset(req, 0, sizeof(req));
    req[0] = 0x04;  // Report ID
    req[1] = 0x02;  // REQ_QUERY_STATUS

    if (write(fd_, req, 64) <= 0)
        return false;

    // Try up to 10 times to get the response
    for (int attempt = 0; attempt < 10; ++attempt) {
        uint8_t resp[64];
        struct pollfd pfd = {fd_, POLLIN, 0};

        if (poll(&pfd, 1, timeout_ms) <= 0)
            continue;

        int n = read(fd_, resp, sizeof(resp));
        if (n > 0 && resp[0] == 0x02) {  // RPT_STATUS report type
            // Parse status flags from resp[1]
            uint8_t flags = resp[1];
            status.plungerEnabled = (flags & 0x01) != 0;
            status.calibrated = (flags & 0x02) != 0;
            status.nightMode = (flags & 0x04) != 0;
            status.clockSet = (flags & 0x08) != 0;
            status.safeMode = (flags & 0x10) != 0;
            status.configLoaded = (flags & 0x20) != 0;
            return true;
        }
    }

    return false;
}

bool FeedbackControllerInterface::ReadIRReport(IRReport& report, int timeout_ms) {
    if (fd_ < 0)
        return false;

    uint8_t buffer[64];
    struct pollfd pfd = {fd_, POLLIN, 0};

    if (poll(&pfd, 1, timeout_ms) <= 0)
        return false;

    int n = read(fd_, buffer, sizeof(buffer));
    if (n > 0 && buffer[0] == 0xF0) {  // RPT_IR_COMMAND report type
        // Decode IR command from buffer
        report.protocol = buffer[2];
        report.flags = buffer[3];
        report.command_lo = buffer[4];
        report.command_mid = buffer[5];
        report.command_hi = buffer[6];
        report.command_hi2 = buffer[7];

        // Decode elapsed time (little-endian 32-bit)
        report.elapsedTime_us =
            (static_cast<uint32_t>(buffer[8]) << 0) |
            (static_cast<uint32_t>(buffer[9]) << 8) |
            (static_cast<uint32_t>(buffer[10]) << 16) |
            (static_cast<uint32_t>(buffer[11]) << 24);

        return true;
    }

    return false;
}

bool FeedbackControllerInterface::WriteRequest(const uint8_t* data, size_t length, int timeout_ms) {
    if (fd_ < 0)
        return false;

    if (write(fd_, data, length) <= 0) {
        std::cerr << "Error: Failed to write request" << std::endl;
        return false;
    }

    return true;
}

bool FeedbackControllerInterface::ReadReport(uint8_t* buffer, size_t max_length, int& bytesRead, int timeout_ms) {
    if (fd_ < 0)
        return false;

    struct pollfd pfd = {fd_, POLLIN, 0};

    if (poll(&pfd, 1, timeout_ms) <= 0) {
        bytesRead = 0;
        return false;
    }

    int n = read(fd_, buffer, max_length);
    if (n < 0) {
        bytesRead = 0;
        return false;
    }

    bytesRead = n;
    return true;
}

} // namespace PinscapePico::Linux
