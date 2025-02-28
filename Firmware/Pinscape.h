// Pinscape Pico - top-level definitions
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once

// debug/test
struct DebugAndTestVars
{
    // Put Config delay time, microseconds.  Simulates a delayed response
    // to Put Config on the vendor interface, to help debug the client.
    uint64_t putConfigDelay = 0;

    // Memory usage before and after loading the JSON configuration file,
    // for monitoring usage.  Config files can get quite large due to the
    // large number of buttons and output ports allowed, so we have to be
    // careful to keep the parsed representation of the JSON as memory-
    // efficient as possible.  This helps us keep tabs on that.
    struct
    {
        uint32_t before = 0;
        uint32_t after = 0;
    } jsonMemUsage;
};
extern DebugAndTestVars G_debugAndTestVars;
