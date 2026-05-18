// Pinscape Pico Config Tool - Linux Feedback Controller HID Interface
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Linux implementation of the Pinscape Pico Feedback Controller interface using hidraw

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace PinscapePico::Linux {

// Feedback Controller HID interface for night mode and IR learning
class FeedbackControllerInterface {
public:
    // IR report structure
    struct IRReport {
        uint8_t protocol;
        uint8_t flags;
        uint8_t command_lo;
        uint8_t command_mid;
        uint8_t command_hi;
        uint8_t command_hi2;
        uint32_t elapsedTime_us;

        std::string ToString() const;
    };

    // Status report structure
    struct StatusReport {
        bool plungerEnabled = false;
        bool calibrated = false;
        bool nightMode = false;
        bool clockSet = false;
        bool safeMode = false;
        bool configLoaded = false;
    };

    FeedbackControllerInterface();
    ~FeedbackControllerInterface();

    // Delete copy constructor and assignment
    FeedbackControllerInterface(const FeedbackControllerInterface&) = delete;
    FeedbackControllerInterface& operator=(const FeedbackControllerInterface&) = delete;

    // Move constructor and assignment
    FeedbackControllerInterface(FeedbackControllerInterface&& other) noexcept;
    FeedbackControllerInterface& operator=(FeedbackControllerInterface&& other) noexcept;

    // Open device (finds first Pinscape Pico HID interface)
    bool Open(uint16_t vid = 0x1209, uint16_t pid = 0xEAEA);

    // Close connection
    void Close();

    // Check if connected
    bool IsConnected() const { return fd_ >= 0; }

    // Set Night Mode
    bool SetNightMode(bool on, int timeout_ms = 3000);

    // Query Night Mode status
    bool QueryStatus(StatusReport& status, int timeout_ms = 3000);

    // Read IR report (for learning mode)
    bool ReadIRReport(IRReport& report, int timeout_ms = 50);

    // Write a request to the device
    bool WriteRequest(const uint8_t* data, size_t length, int timeout_ms = 3000);

    // Read a report from the device
    bool ReadReport(uint8_t* buffer, size_t max_length, int& bytesRead, int timeout_ms = 3000);

private:
    int fd_;  // File descriptor for hidraw device
    std::string device_path_;
};

} // namespace PinscapePico::Linux
