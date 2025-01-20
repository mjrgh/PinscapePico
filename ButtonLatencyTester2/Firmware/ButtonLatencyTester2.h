// Pinscape Pico Button Latency Tester II
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// Process a button event from the host.
//
// 'gpio' is the GPIO port number corresponding to the input event.  The
// host must figure out the mapping on its own, since we have no way to
// know how the host receives the input events - they could be
// WM_KEYDOWN events, joystick button events read through DirectInput,
// Open Pinball Device HID events, etc.  It's up to the host
// application, probably through a user-supplied configuration file, to
// map those high-level Windows input events to the physical button
// input under test, and to map that to the Pico GPIO it's attached to
// on the measurement tool.
//
// Returns true if the GPIO port is valid, false if not.  On a successful
// return the result struct is populated for passing back to the client.
bool ProcessHostButtonEvent(
    ButtonLatencyTester2::VendorResponse::Args::HostInputResult &result,
    const ButtonLatencyTester2::VendorRequest::Args::HostInputEvent &event);

// Populate a SUBCMD_MEASUREMENTS_GET transfer data block for the vendor
// interface.  Returns the size of the transfer data populated, or zero
// on error.
size_t PopulateMeasurementsList(uint8_t *buf, size_t bufSize);

// reset measurements
void ResetMeasurements();

// Debounce lockout time
extern volatile uint32_t debounceLockoutTime_us;

