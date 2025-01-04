// Pinscape Pico - BOOTSEL button access
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The BOOTSEL button is the small pushbutton on top of the standard Pico boards.
// The design function of BOOTSEL is to force the Pico to enter its ROM Boot
// Loader mode upon CPU reset, which it accomplishes by pulling the flash memory
// Chip Select line low (overriding the drive signal from the QSPI_SS CPU pin).
// With the flash chip disabled at system reset, the RP2040 loads the ROM boot
// loader code instead of whatever's stored in the flash.
//
// It's also possible for the software to read the BOOTSEL state, because the
// CPU QSPI_SS pin is really just an ordinary GPIO by another name, which means
// that it can be configured as a digital input to sample the current state of
// the BOOTSEL button.  Since the BOOTSEL button pulls the line to ground, the
// GPIO reads as low (logic 0) when the button is pressed and high (logic 1)
// when the button isn't pressed.  However, care must be taken during this
// procedure, because the CPU can't access flash memory while the QSPI_SS pin
// is configured as an input, hence the CPU can't attempt to execute any
// instructions in flash-resident code segments during the procedure.  This
// means that the read routine itself must be RAM-resident, but that's not
// sufficient: we also have to block interrupts, since an interrupt handler
// could be flash-resident, and we have to temporarily halt the other core,
// since the other core could be executing flash-resident code or could jump
// to flash-resident code while the read routine is running.  Fortunately,
// the Pico SDK provides a mechanism ("flash safe execution") that covers
// all of those bases, so we can expose a function that hides all of those
// details from the caller.
//
// PERFORMANCE WARNING: the Read function is fairly slow, about 100us (0.1ms)
// per call.   Interrupts and other-core execution will be blocked while a
// Read call is in progress.  Interrupt latency and time-critical loops on
// the other core might be extended slightly for any events that happen to
// occur during a Read call.  BOOTSEL reading shouldn't be used if interrupt
// latency is required to be less than around 100us.

class BootselButton
{
public:
    // Read the BOOTSEL button status.  This can be called from any context,
    // including when code is running on both CPU cores, as long as the code on
    // the other core has called flash_safe_execute_core_init() at some point.
    static bool Read();

    // timing statistics
    struct Stats
    {
        // cumulative time spent in Read(), in microseconds
        uint64_t tSum = 0;

        // number of calls to Read()
        uint32_t nCalls = 0;

        // average time spent in Read() per call, in microseconds
        int AvgTime() const { return static_cast<int>(tSum / nCalls); }
    };
    static Stats stats;
};

