// Pinscape Pico - top-level definitions
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once

// debug/test
struct DebugAndTestVars
{
    // Put Config delay time, microseconds.  Simulates a delayed response
    // to Put Config on the vendor interface, to help debug the client.
    uint64_t putConfigDelay = 0;
};
extern DebugAndTestVars G_debugAndTestVars;
