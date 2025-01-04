// Pinscape Pico - Device API includer
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Includes the device API files, and brings the classes into our global
// namespace.

#pragma once
#include "../WinAPI/PinscapeVendorInterface.h"
#include "../WinAPI/FeedbackControllerInterface.h"
#include "../WinAPI/RP2BootLoaderInterface.h"

// bring the main interfaces into our global namespace
using VendorInterface = PinscapePico::VendorInterface;
using FeedbackControllerInterface = PinscapePico::FeedbackControllerInterface;
using RP2BootDevice = PinscapePico::RP2BootDevice;
