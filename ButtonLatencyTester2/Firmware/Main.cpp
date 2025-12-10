// Pinscape Pico Button Latency Tester II - Main Program
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Pico firmware for a universal, device-independent, end-to-end button
// latency measurement tool.  The tool helps you precisely measure the
// time (latency) between a physical button press and the arrival of the
// corresponding USB notification in the host application.  It can be
// used with any type of input device that encodes button presses for
// input to a PC.
//
// "End-to-end" means that we measure the ENTIRE data path that the
// input signal travels through, from the physical button press to the
// software event that the APPLICATION actually sees.  This is important
// to get a full understanding of latency, because the data input from
// the device to the PC across the physical connection (USB, etc) is
// only part of the story.  Windows and other modern operating systems
// interpose multiple layers of device drivers and system software
// between the physical input port and the software application logic.
// This tool is designed to measure the full path through the entire
// software stack, by measuring the arrival time of the event at the API
// level where the application actually reads user input.  Windows and
// other systems offer multiple APIs that an application can use to read
// any given input type, and the application's choice of API can made a
// significant difference in the overall latency at the application
// level.  This tool thus can help not only to characterize the latency
// between the subject device and the host's physical input port, but
// can also be useful in optimizing application latency, by providing
// concrete data on the effects of different API choices.
//
// The tool is "universal", meaning that it can be used with ANY input
// device that translates button presses to PC inputs.  It doesn't
// matter whether the device is microcontroller-based or an embedded
// system.  It doesn't even matter what kind of input port the device
// uses to send data to the PC: it could be USB, or a serial port, or a
// keyboard port, or bluetooth, or any other physical PC input method.
// The only requirement for the subject device is that you can
// physically access the switch input port, because you have to be able
// to wire a button to BOTH the subject device AND the Pico running this
// measurement tool.  The entire timing mechanism is based on the
// assumption that the subject device and the Pico can both read the
// PHYSICAL button state in real time, which means that both devices
// must be physically wired to the button.  (So this wouldn't be a
// suitable tool for measuring the latency of a physical PC keyboard,
// for example, because the button switches are inaccessibly sealed
// inside the keyboard body, with no way to tap into the wiring.  If you
// *were* willing to crack open the casing and solder wires to your
// keyboard switches, though, you could use this tool to measure the
// keyboard timing after all.)
//
// No instrumentation or modification is required on the subject device
// That allows the tool to be used with closed-source and proprietary
// devices as well as open-source microcontroller systems like Pinscape
// Pico.
//
// The host application could be a Windows program such as Visual
// Pinball, a Linux application, a MacOS application, etc.  Unlike the
// device being measured, though, the host application must be
// instrumented, because it has to close the loop between the physical
// button input and the host reception of the input event, by sending a
// USB command to the measurement tool on each input event.  We provide
// some simple code templates that can be used to add instrumentation to
// any program that you can build from source, allowing you to measure
// the end-to-end latency to a PARTICULAR application such as Visual
// Pinball.  We also provide a standalone application whose whole job is
// to work with the measurement software, to measure the generic latency
// profile to a basic application.  The standalone program characterizes
// the latency as far as the basic OS-level input APIs go, but adding
// instrumentation to a particular application gives you a more precise
// picture of the latency experienced in that exact application, by
// taking into account the specific code paths that that application
// goes through to handle input events.
//
// This measurement tool firmware runs on its own standalone Pico, NOT
// on the microcontroller whose latency we're measuring.  This
// completely isolates the measurement tool from the device being
// tested, ensuring that there's no unaccounted latency from other CPU
// activity in the subject device that might skew the tool's own timing
// precision.  The measurement tool Pico devotes an entire CPU core to
// timing physical button presses, so it can operate with microsecond
// precision.
//
//
// PHYSICAL SETUP
//
// To set up a measurement, you need three pieces: the subject device,
// such as a Pinscape KL25Z; the host PC; and the Pico running the
// measurement tool.
//
// You can measure up to 26 buttons, one per GPIO port on the
// measurement tool Pico.  Each button must be connected to BOTH the
// subject device and to the measurement tool Pico, but NOT DIRECTLY,
// since we don't want to expose either device to the other device's
// power supply.  Instead, each button should be connected to the
// devices through optocouplers.  The button should be connected to
// the LED side of the optocouplers, so that pressing the button
// activates both optocouplers at the same instant.  Each device
// should then be connected to the collector of one optocoupler,
// with the optocoupler emitter connected to the device's ground.
//
//                  -------------
//  5V---Button---1|(+)       (C)|4---Subject device port
//                 | L           |
//                 | E   PC817   |
//                 | D           |
//              --2|(-)       (E)|3---GND
//             |    --------------
//             |
//             |    -------------
//              --1|(+)       (C)|4---Pico measurement tool GPIO
//                 | L           |
//                 | E   PC817   |
//                 | D           |
//              --2|(-)       (E)|3---GND
//             |    --------------
//             |
//          130 Ohms (based on Vf=1.2V, 5V supply voltage, 20mA)
//             |
//            GND
//
// The "Button" is a Normally Open switch, such as a flipper leaf
// switch or an arcade pushbutton switch.  If using a microswitch
// with three terminals, typically labeled NO NC CMN (or just C),
// connect NO (Normally Open) and CMN/C (Common).
//
// The wiring above assumes that the subject device has active-low
// button ports with internal pull-ups: that is, you would normally wire
// a button to the port by wiring the Normally Open (NO) button switch
// to GND, so that pushing the button grounds the port.  This applies
// to Pinscape on KL25Z, Pinscape Pico, and most commercial key
// encoder devices.  If the subject device requires some other wiring
// arrangement, you should be able to adjust the circuit shown by
// substituting the PC817 Collector/Emitter junction for the physical
// switch in the normal wiring setup for your device.
//
//
// SOFTWARE SETUP
//
// The measurement tool firmware requires no configuration.  Just
// install the firmware on the Pico using the normal Pico Boot Loader
// procedure.
//
// On the PC host side, you must run either the standalone measurement
// tool application provided with this project, OR an instrumented
// version of an application that you want to test.  Application
// instrumentation must be added at a source code level, so you'll need
// access to the subject application's source code and the ability to
// build an executable from the modified source.  Instructions are in
// the Host section of the project.
//
// The standalone application requires some configuration to tell it
// which input events (e.g., "keyboard Q key" or "joystick button 3"
// correspond to which GPIO ports on the measurement tool Pico.
// Instructions are in the program folder.
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>
#include <algorithm>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <pico/unique_id.h>
#include <pico/sync.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>

// Pinscape headers
#include "../../Firmware/Pinscape.h"
#include "../../Firmware/Main.h"
#include "../../Firmware/Utils.h"
#include "../../Firmware/Logger.h"
#include "../../Firmware/USBCDC.h"
#include "../../Firmware/CommandConsole.h"
#include "../../Firmware/Watchdog.h"
#include "../../Firmware/Reset.h"

// local project headers
#include "Version.h"
#include "USBImpl.h"
#include "VendorIfc.h"
#include "PicoLED.h"
#include "ButtonLatencyTester2.h"

// Main loop statistics
CoreMainLoopStats mainLoopStats;
volatile CoreMainLoopStats secondCoreLoopStats;
volatile bool secondCoreLoopStatsResetRequested = false;

// Debug/test variables
DebugAndTestVars G_debugAndTestVars;

// Forwards
static void SecondCoreMain();
static void Command_memory(const ConsoleCommandContext *c);
static void Command_loopstats(const ConsoleCommandContext *c);
static void Command_reset(const ConsoleCommandContext *ctx);
static void Command_version(const ConsoleCommandContext *ctx);
static void Command_buttons(const ConsoleCommandContext *ctx);
static void Command_latencyStats(const ConsoleCommandContext *ctx);

// --------------------------------------------------------------------------
//
// Button states
//
struct ButtonState
{
    ButtonState(int gp) : gp(gp), bit(1 << gp) { }
    
    // GPIO port number
    int gp;

    // GPIO vector mask bit (1 << gp)
    uint32_t bit;
    
    // Current GPIO port high/low.  Note that we use the terminology HIGH/LOW
    // to refer to the PHYSICAL state of the GPIO, to avoid confusion with the
    // active-low interpretation of the button's logical on/off state.  We're
    // concerned here strictly with the physical port state.
    volatile bool high = true;

    // timestamp of last state HIGH/LOW transition
    volatile uint64_t tHigh = 0;
    volatile uint64_t tLow = 0;

    // debounce time: physical state changes will be ignored until this timestamp
    uint64_t tDebounce = 0;

    // total number of physical presses recorded (HIGH-to-LOW transitions)
    volatile uint64_t nPresses = 0;

    // reset requested for statistics
    volatile bool nPressesReset = false;

    // tLow (physical button press time) at last host event.  When we
    // record a host event, we set this to the tLow corresponding to the
    // host event.  On the next host event, if tLow is still the same,
    // we ignore the host event, because this indicates that the host
    // received more than one input event for what we recorded as a
    // single physical button press (high->low transition).  This can
    // happen if the subject device (the source of the host input
    // events) doesn't debounce the physical switch input, or debounces
    // it on a shorter time scale than we do, causing it to recognize
    // multiple off->on transitions during an event that we consider a
    // single press after debouncing.  Multiple events per physical
    // press can also be an artifact of the USB protocol, host-side
    // input processing, application processing, or any number of other
    // factors.  Whatever the source, this record of the physical button
    // press time at the last host event helps filter out those
    // multiple-event artifacts, so that they don't skew the overall
    // latency averages.
    uint64_t tLowAtLastHostEvent = 0;

    // number of host events matched against presses
    uint64_t nHostEvents = 0;

    // sum of latency measured from host events, and sum of latency squared (for stddev)
    uint64_t latencySum = 0;
    uint64_t latencySquaredSum = 0;

    // minimum and maximum latency times recorded
    uint64_t latencyMin = 0;
    uint64_t latencyMax = 0;

    // Circular buffer for most recent latency measurements, values in microseconds.
    // Used for calculating the median when showing statistics.
    static const int MaxRecents = 1024;
    uint16_t recents[MaxRecents];

    // number of recents stored
    int nRecents = 0;

    // write pointer to next recent
    int recentsWriteIdx;

    // Add a latency measurement to the recents list
    void AddRecent(uint16_t latency)
    {
        // add it to the buffer
        recents[recentsWriteIdx] = latency;

        // increment and wrap the write index
        if (++recentsWriteIdx >= MaxRecents)
            recentsWriteIdx = 0;

        // if this doesn't overflow the buffer, count it
        if (nRecents < MaxRecents)
            ++nRecents;
    }

    // Calculate the median latency
    uint16_t CalcMedian()
    {
        // there's no median if the list is empty
        if (nRecents == 0)
            return 0;
        
        // calculate the index of the newest and oldest elements
        int newestIdx = recentsWriteIdx == 0 ? MaxRecents - 1 : recentsWriteIdx - 1;
        int oldestIdx = recentsWriteIdx >= nRecents ? recentsWriteIdx - nRecents : recentsWriteIdx + MaxRecents - nRecents;

        // it's trivial if the list is one entry
        if (nRecents == 1)
            return recents[oldestIdx];

        // rearrange the buffer so that it's contiguous
        if (nRecents == MaxRecents)
        {
            // It's completely full, so it can't help but be contiguous,
            // in a single block from 0..Max-1
            oldestIdx = 0;
            newestIdx = MaxRecents - 1;
        }
        else if (oldestIdx < newestIdx)
        {
            // it's in a single block from oldestIdx..newestIdx
        }
        else
        {
            // The buffer is split into two sections, with a gap between:
            // 0..newestIdx and oldestIdx..Max-1.  Move the upper section
            // down so that it's contiguous with the lower section.
            memmove(&recents[newestIdx+1], &recents[oldestIdx], (MaxRecents - oldestIdx)*sizeof(recents[0]));

            // the list is now arranged from 0..nRecents-1
            oldestIdx = 0;
            newestIdx = nRecents - 1;

            // the next available slot is now nRecents
            recentsWriteIdx = nRecents;
        }

        // sort the now-guaranteed-contiguous section
        std::sort(&recents[oldestIdx], &recents[newestIdx + 1]);

        // the median is the middle value if the count is odd, or the mean
        // of the two values straddling the middle if the count is even
        if ((nRecents & 1) == 1)
        {
            return recents[oldestIdx + nRecents/2];
        }
        else
        {
            int a = recents[oldestIdx + nRecents/2 - 1];
            int b = recents[oldestIdx + nRecents/2];
            return static_cast<uint16_t>((a + b)/2);
        }
    }
};

// button states for all externally connected GPIOs
static ButtonState buttonStates[]{
    { 0 },
    { 1 },
    { 2 },
    { 3 },
    { 4 },
    { 5 },
    { 6 },
    { 7 },
    { 8 },
    { 9 },
    { 10 },
    { 11 },
    { 12 },
    { 13 },
    { 14 },
    { 15 },
    { 16 },
    { 17 },
    { 18 },
    { 19 },
    { 20 },
    { 21 },
    { 22 },
    { 26 },
    { 27 },
    { 28 },
};

// map button states to GPIO ports
static ButtonState *const gpioToButton[]{
    &buttonStates[0],
    &buttonStates[1],
    &buttonStates[2],
    &buttonStates[3],
    &buttonStates[4],
    &buttonStates[5],
    &buttonStates[6],
    &buttonStates[7],
    &buttonStates[8],
    &buttonStates[9],
    &buttonStates[10],
    &buttonStates[11],
    &buttonStates[12],
    &buttonStates[13],
    &buttonStates[14],
    &buttonStates[15],
    &buttonStates[16],
    &buttonStates[17],
    &buttonStates[18],
    &buttonStates[19],
    &buttonStates[20],
    &buttonStates[21],
    &buttonStates[22],
    nullptr,             // unused GP23
    nullptr,             // unused GP24
    nullptr,             // unused GP25
    &buttonStates[23],   // GP26
    &buttonStates[24],   // GP27
    &buttonStates[25],   // GP28
    nullptr,             // unused GP29
    nullptr,             // unused GP30
    nullptr,             // unused GP31
    nullptr,             // unused GP32
};

// Debounce lockout time.  After we detect a state change on a GPIO port,
// we'll ignore further state changes for this interval, in microseconds.
volatile uint32_t debounceLockoutTime_us = 3000;


// ---------------------------------------------------------------------------
//
// Populate a SUBCMD_MEASUREMENTS_GET buffer for the vendor interface
//
size_t PopulateMeasurementsList(uint8_t *buf, size_t bufSize)
{
    // make sure there's room
    using List = ButtonLatencyTester2::MeasurementsList;
    using Data = ButtonLatencyTester2::MeasurementData;
    size_t n = _countof(buttonStates);
    size_t neededSize = sizeof(List) + n*sizeof(Data);
    if (bufSize < neededSize)
        return 0;

    // zero the whole return area, so that we return default zero bytes
    // in any fields we don't explicitly fill
    memset(buf, 0, neededSize);

    // populate the list header
    List *list = reinterpret_cast<List*>(buf);
    list->cb = sizeof(List);
    list->cbData = sizeof(Data);
    list->nData = static_cast<uint16_t>(n);

    // populate the per-port entries
    Data *data = reinterpret_cast<Data*>(list + 1);
    for (auto &b : buttonStates)
    {
        // populate this element
        data->gp = b.gp;
        data->nPresses = b.nPresses;
        data->nHostEvents = b.nHostEvents;
        data->latencySum = b.latencySum;
        data->latencySquaredSum = b.latencySquaredSum;
        data->latencyMin = b.latencyMin;
        data->latencyMax = b.latencyMax;
        data->latencyMedian = b.CalcMedian();

        // advance to the next output slot
        ++data;
    }

    // success - return the populated size
    return neededSize;
}

// ---------------------------------------------------------------------------
//
// Console command - show button states
//
static void Command_buttons(const ConsoleCommandContext *c)
{
    c->Printf("Button states:\n");
    uint64_t now = time_us_64();
    for (auto &b : buttonStates)
    {
        c->Printf("  GP%-2d   %s   ", b.gp, b.high ? "HIGH" : "LOW ");

        static const auto ShowTime = [](const ConsoleCommandContext *c, const char *name, uint64_t t, uint64_t now)
        {
            c->Printf("%s ", name);
            if (t == 0)
            {
                c->Printf("-");
            }
            else
            {
                int64_t dt = now - t;
                c->Printf("%llu ", t);
                if (dt < 0)
                    c->Printf("(now)", t);
                if (dt < 100000)
                    c->Printf("(%.2f ms ago)", static_cast<float>(dt)/1000.0f);
                else if (dt < 60000000)
                    c->Printf("(%.2f seconds ago)", static_cast<float>(dt/1000)/1000.0f);
                else
                    c->Printf("(%d:%02d minutes ago)", static_cast<int>(dt / 60000000), static_cast<int>(dt / 1000000) % 60);
            }
        };
        ShowTime(c, "tHi", b.tHigh, now);
        ShowTime(c, ", tLo", b.tLow, now);
        c->Printf("\n");
    }
}

// 
// Console command - Show latency stats
//
static void Command_latencyStats(const ConsoleCommandContext *c)
{
    bool showStats = (c->argc == 1);  // show by default if no arguments
    bool showAll = false;
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-r") == 0 || strcmp(a, "--reset") == 0)
        {
            ResetMeasurements();
        }
        else if (strcmp(a, "-s") == 0 || strcmp(a, "--show") == 0)
        {
            showStats = true;
        }
        else if (strcmp(a, "-a") == 0 || strcmp(a, "--all") == 0)
        {
            showAll = true;
            showStats = true;  // implies we want to show stats
        }
        else
        {
            return c->Printf("latstats: invalid option \"%s\"\n", a);
        }
    }

    if (showStats)
    {
        c->Printf("GP #Presses #HostEvt AvgLat(ms)   Median     StdDev    Min Lat    Max Lat\n"
                  "== ======== ======== ========== ========== ========== ========== ==========\n");
        uint64_t now = time_us_64();
        int nListed = 0;
        for (auto &b : buttonStates)
        {
            // skip buttons with no events, unless -a specified
            if (b.nHostEvents == 0 && !showAll)
                continue;

            // get the stats; use all zeroes if there are no events
            float mean = 0.0f, med = 0.0f, stddev = 0.0f, lmin = 0.0f, lmax = 0.0f;
            if (b.nHostEvents != 0)
            {
                double mean_us = static_cast<double>(b.latencySum)/static_cast<double>(b.nHostEvents);
                mean = static_cast<float>(mean_us)/1000.0f;
                med = b.CalcMedian()/1000.0f;
                stddev = sqrtf(static_cast<float>(
                    static_cast<double>(b.latencySquaredSum)/static_cast<double>(b.nHostEvents) - mean_us*mean_us))/1000.0f;
                lmin = static_cast<float>(b.latencyMin)/1000.0f;
                lmax = static_cast<float>(b.latencyMax)/1000.0f;
            }
            c->Printf(
                "%2d %8llu %8llu %10.3f %10.3f %10.3f %10.3f %10.3f\n",
                b.gp, b.nPresses, b.nHostEvents, mean, med, stddev, lmin, lmax);

            ++nListed;
        }
        if (nListed == 0)
            c->Printf("\nNo events recorded (use -a to show all buttons anyway)\n");
    }
}


// ---------------------------------------------------------------------------
//
// Process a host button event
//
bool ProcessHostButtonEvent(
    ButtonLatencyTester2::VendorResponse::Args::HostInputResult &result,
    const ButtonLatencyTester2::VendorRequest::Args::HostInputEvent &event)
{
    // assume no timing information is available
    ButtonState *b = nullptr;
    auto gpio = event.gp;
    if (gpio >= 0 && gpio < _countof(gpioToButton) && (b = gpioToButton[gpio]) != nullptr)
    {
        // Figure the Pico time of the host event:
        //
        // - If the host provided a frame counter (frame counter != 0xFFFF),
        //   calculate the time based on the host frame counter and elapsed
        //   time to start of frame.  This determines the host event time on
        //   the Pico system clock to high precision, using the USB SOF
        //   signal as a shared time reference point.
        //
        // - If no frame counter is available (frame counter == 0xFFFF),
        //   use the current Pico system clock minus the elapsed time on
        //   the host to where it sent the USB request.  This doesn't
        //   account for the USB transit time, so it results in much
        //   less accurate latency calculations, but it's a necessary
        //   alternative for hosts that don't provide the USB hardware
        //   access required to obtain the SOF timing information.
        //
        uint64_t tEvent = (event.usbFrameCounter == 0xFFFF) ?
            time_us_64() - event.dtEventToSof :
            vendorIfc.HostTimeToPicoTime(event.dtEventToSof, event.usbFrameCounter, event.sofTimestamp);
        
        // figure the elapsed time
        uint64_t dt = tEvent - b->tLow;

        // If the time is less than 50ms, count the latency in the total.  Don't
        // count longer times, since that's probably either a mismatch between our event
        // and the host event, or reflects a CPU preemption on the host that's due to
        // something entirely separate from input latency.  A mismatch can occur if the
        // same high-level event on the host (a keyboard key press, joystick button press,
        // etc) that the host is mapping to the subject input device could also come from
        // a separate source, such as an actual keyboard or another joystick.  CPU
        // preemption can occur for any number of reasons, such as a higher-priority task
        // taking the foreground or a device driver stall.  In any case, it's not
        // meaningful to count such outliers against the latency total, so just throw
        // them out if they go above this threshold.
        //
        // Don't count the event if we've already recorded a host event for the same
        // tLow.  A second event at the same tLow means that the subject device sent
        // the host more than one input event for what we recorded as a single physical
        // button press.  This can happen if the subject device doesn't debounce its
        // switch input, or debounces it on a shorter time scale than we do, so that
        // it interprets what we consider switch bounce as separate button presses.
        // Multiple events could also happen due to details of the subject device's
        // USB protocol, or due to host-side processing.  In any case, a second or
        // Nth host input event for the same physical button press isn't meaningful
        // for measuring the latency, since the host knows about the button press
        // as of the first event corresponding the press.
        const char *statusName = "Unknown";
        if (dt > 50000)
        {
            // too long - not matched
            result.status = result.STAT_NO_MATCH;
            result.latency = 0;
            statusName = "No Match";
        }
        else if (b->tLow == b->tLowAtLastHostEvent)
        {
            // we've already processed a host event for the same physical button press
            result.status = result.STAT_DUPLICATE;
            result.latency = dt < UINT32_MAX ? static_cast<uint32_t>(dt) : 0xFFFFFFFFUL;
            statusName = "Duplicate";
        }
        else
        {
            // update the running sum (for the average) and sum-of-squares (for
            // standard deviation)
            b->latencySum += dt;
            b->latencySquaredSum += dt*dt;

            // update the min/max
            if (b->nHostEvents == 0)
            {
                // this is the first event, so it's the min and max so far
                b->latencyMin = b->latencyMax = dt;
            }
            else
            {
                // one or more events recorded -> keep the highest and lowest so far
                b->latencyMin = std::min(dt, b->latencyMin);
                b->latencyMax = std::max(dt, b->latencyMax);
            }

            // count the host event
            b->nHostEvents += 1;

            // Record the tLow for the event, so that we'll know to ignore
            // any redundant events the host sends us for the same physical
            // button press.
            b->tLowAtLastHostEvent = b->tLow;

            // Add it to the recent samples list for median calculations.  Note
            // that truncation to uint16_t is safe because we've already rejected
            // anything over 50000 us (which is safely less than UINT16_MAX) on
            // the basis that a host-side input event recorded so long after the
            // last button press probably came from a source other than the
            // button press, and so isn't our event to record.
            b->AddRecent(static_cast<uint16_t>(dt));

            // successfully matched
            result.latency = static_cast<uint32_t>(dt);
            result.status = result.STAT_MATCHED;
            statusName = "Matched";
        }

        // log for debugging
        Log(LOG_DEBUG, "Event: GP%d, dt=%llu, USB frame %u, eventToSof %ld, button tLow %llu, status %s\n",
            event.gp, dt, event.usbFrameCounter, event.dtEventToSof, b->tLow, statusName);

        // success
        return true;
    }
    else
    {
        // invalid GPIO
        result.latency = 0;
        result.status = result.STAT_NO_MATCH;
        return false;
    }
}

// --------------------------------------------------------------------------
//
// Reset latency statistics
//
void ResetMeasurements()
{
    // reset all button states
    for (auto &b : buttonStates)
    {
        // these can be safely reset, as they're only written on the main core
        b.nHostEvents = 0;
        b.latencySum = 0;
        b.latencySquaredSum = 0;
        b.latencyMin = 0;
        b.latencyMax = 0;
        b.recentsWriteIdx = 0;
        b.nRecents = 0;
        
        // request second-core update resets
        b.nPressesReset = true;
    }
}

// --------------------------------------------------------------------------
//
// Second core main entrypoint
//
static void SecondCoreMain()
{
    for (uint64_t t0 = time_us_64() ; ; )
    {
        // read all GPIO states as a bit vector
        uint32_t all = gpio_get_all();

        // update button states
        for (auto &b : buttonStates)
        {
            // reset nPresses if desired
            if (b.nPressesReset)
            {
                b.nPressesReset = false;
                b.nPresses = 0;
            }
            
            // if the debounce lockout period has expired, check for a state change
            uint64_t now = time_us_64();
            if (now > b.tDebounce)
            {
                // get the current physical state for this GPIO from the bit vector
                bool high = ((all & b.bit) != 0);

                // if the state has changed, update it
                if (high != b.high)
                {
                    // note the new high/low transition time
                    b.high = high;
                    if (high)
                    {
                        // low-to-high - switch BREAK
                        b.tHigh = now;
                    }
                    else
                    {
                        // high-to-low - switch MAKE
                        b.tLow = now;
                        b.nPresses += 1;
                    }

                    // set the debounce lockout time
                    b.tDebounce = now + debounceLockoutTime_us;
                }
            }
        }

        // update statistics
        uint64_t now = time_us_64();
        secondCoreLoopStats.AddSample(now - t0);
        t0 = now;

        // reset statistics if desired
        if (secondCoreLoopStatsResetRequested)
        {
            secondCoreLoopStatsResetRequested = false;
            secondCoreLoopStats.Reset();
        }
    }
}

// --------------------------------------------------------------------------
//
// Main program entrypoint
//
int main()
{
    // set the LED to solid on while initializing
    picoLED.SetBlinkPattern(PicoLED::Pattern::On);
    
    // If the watchdog initiated the reboot, reset to Boot Loader mode.
    // That will serve as our approximation of "Safe Mode" after a
    // crash.  We don't have a proper Safe Mode of our, since this
    // program isn't configurable enough for the concept to apply; the
    // point of a Safe Mode is to enable the minimum operating system
    // core, with optional features and device drivers disabled, in the
    // hope that the crash was caused by one of the optional features
    // that's enabled in the full configuration, and thus can be avoided
    // by loading just the OS core.  This program really only has its
    // core functionality, so there's nothing optional to disable to
    // create a Safe Mode.  Switching to Boot Loader mode doesn't help
    // with diagnostics, which is the main point of booting into a Safe
    // Mode after a crash (to inspect whatever remnants of system state
    // carried over from just before the crash, in an effort to identify
    // the cause), but it at least puts the Pico into a usable state
    // where the user can try installing a firmware update.  This might
    // be especially useful during development, when an updated build
    // turns out to crash the Pico, creating the need to revise the
    // update to fix the newly introduced error, and try again.
    if (watchdog_caused_reboot() && picoReset.GetBootMode() == PicoReset::BootMode::SafeMode)
        picoReset.Reboot(true);

    // enable the watchdog to reboot after a few seconds of inactivity
    WatchdogEnable(3000);

    // if the watchdog resets us unexpectedly, boot to safe mode
    picoReset.SetNextBootMode(PicoReset::BootMode::SafeMode);

    // add console commands
    CommandConsole::AddCommand(
        "loopstats", "show main loop timing statistics",
        "loopstats [options]\n"
        "options:\n"
        "  -l, --list     list statistics (default if no options specified)\n"
        "  -r, --reset    reset the statistics counters",
        Command_loopstats);

    CommandConsole::AddCommand(
        "mem", "show memory usage statistics", "mem  (no options)\n",
        Command_memory);

    CommandConsole::AddCommand(
        "reboot", "hardware-reset the Pico",
        "reboot [mode]\n"
        "  -r, --reset      reset the Pico and restart the latency tester firmware program\n"
        "  -b, --bootsel    launch the Pico ROM Boot Loader, for installing new firmware",
        Command_reset);

    CommandConsole::AddCommand(
        "version", "show firmware and hardware information",
        "version (no arguments)",
        Command_version);

    CommandConsole::AddCommand(
        "buttons", "show live button states",
        "buttons (no arguments)",
        Command_buttons);

    CommandConsole::AddCommand(
        "latstats", "show current latency statistics",
        "latstats [options]\n"
        "options:\n"
        "  -s, --show       show statistics\n"
        "  -a, --all        show stats for all buttons, even those with no events recorded\n"
        "  -r, --reset      reset statitics\n"
        "\n"
        "With no option flags, shows statistics (same as --show).\n",
        Command_latencyStats);

    // add the logger control command
    logger.InstallConsoleCommand();

    // show the in
    logger.SetDisplayModes(false, false, true);
    logger.SetFilterMask((1 << LOG_ERROR) | (1 << LOG_WARNING) | (1 << LOG_INFO));
    Log(LOG_INFO, "\n\n"
        "===========================================\n"
        "Pinscape Button Latency Tester II Version %d.%d.%d, build %s\n"
        "\n",
        VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, buildTimestamp);

    // configure the logger to show type codes and colors
    logger.SetDisplayModes(false, true, true);

    // initialize the USB layer; sets VID, PID
    usbImpl.Init(0x1209, 0xEAEC);

    // initialize the vendor interface (must be after UBS initialization)
    vendorIfc.Init();

    // set up USB CDC logging and console access
    usbcdc.Configure();
    usbCdcLogger.Configure(true, true, 16384, 1024);

    // Initialize all GPIOs as inputs, hysteresis (Schmitt trigger)
    // enabled, with pull-up enabled.
    //
    // Each physical button is to be wired as a Normally Open switch
    // that connects to GND when the button is pressed.  GPIO logic LOW
    // is Button On, logic HIGH is button Off.
    for (auto &b : buttonStates)
    {
        int gp = b.gp;
        gpio_init(gp);
        gpio_set_dir(gp, false);
        gpio_set_input_hysteresis_enabled(gp, true);
        gpio_set_pulls(gp, true, false);
    }

    // Get the initial input states of all GPIOs, so that we don't
    // record any spurious low->high or high->low events at startup.
    // Note that gpio_get() within the loop above records everything
    // as LOW, regardless of the actual state, perhaps because it
    // takes non-zero time for the pull-up to physically take effect
    // at the input pad.  They read properly if we add a brief wait.
    sleep_us(1000);
    for (auto &b : buttonStates)
    {
        b.high = gpio_get(b.gp);
        Log(LOG_INFO, "GP%d high=%d, tHigh=%llu\n", b.gp, b.high, b.tHigh);
    }

    // launch the second-core thread
    multicore_launch_core1(SecondCoreMain);

    // main loop
    for (uint64_t tTopOfLoop = time_us_64() ; ; )
    {
        // blink the LED
        picoLED.Task();

        // run USB tasks
        usbImpl.Task();

        // run logger tasks
        logger.Task();

        // run vendor interface tasks
        vendorIfc.Task();

        // collect loop timing statistics for this loop iteration
        uint64_t tEndOfLoop = time_us_64();  
        mainLoopStats.AddSample(static_cast<uint32_t>(tEndOfLoop - tTopOfLoop));
        tTopOfLoop = tEndOfLoop;

        // let the watchdog know we're running
        watchdog_update();
    }

    // not reached
    return 0;
}


// ---------------------------------------------------------------------------
//
// Command console welcome banner
//
void CommandConsole::ShowBanner()
{
    PutOutputFmt(
        "\n\033[0;1mPinscape Pico Button Latency Tester II command console\n"
        "Firmware %d.%d.%d, build %s\033[0m\n",
        VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, buildTimestamp);
}

// ---------------------------------------------------------------------------
//
// Console command - show memory statistics in a console session
//
static void Command_memory(const ConsoleCommandContext *c)
{
    // get the malloc statistics
    extern char __StackLimit, __bss_end__;
    size_t totalHeap = &__StackLimit - &__bss_end__;
    struct mallinfo mi = mallinfo();

    // display on the console
    c->Printf(
        "Memory stats:\n"
        "  Heap size (bytes):  %lu\n"
        "  Heap unused:        %lu\n"
        "  Malloc arena size:  %lu\n"
        "  Arena in use:       %lu\n"
        "  Arena free:         %lu\n"
        "  Total free space:   %lu\n",
        static_cast<uint32_t>(totalHeap),
        static_cast<uint32_t>(totalHeap - mi.arena),
        mi.arena,
        mi.uordblks,
        mi.fordblks,
        static_cast<uint32_t>(totalHeap - mi.arena) + mi.fordblks);
}

// ---------------------------------------------------------------------------
//
// Console command - show main loop statistics
//
static void Command_loopstats(const ConsoleCommandContext *c)
{
    const static auto Stats = [](const ConsoleCommandContext *c)
    {
        auto &s = mainLoopStats;
        auto &s2 = secondCoreLoopStats;
        uint64_t now = time_us_64();
        int days = static_cast<int>(now / (24*60*60*1000000ULL));
        int timeOfDay = static_cast<int>((now - (days * (24*60*60*1000000ULL))) / 1000000);
        int hh = timeOfDay / 3600;
        int mm = (timeOfDay - hh*3600) / 60;
        int ss = timeOfDay % 60;
        c->Printf(
            "Main loop statistics:\n"
            "  Uptime:       %llu us (%d days, %d:%02d:%02d)\n"
            "  Primary core: %llu iterations (since startup)\n"
            "  Second core:  %llu iterations (since startup)\n"
            "\n"
            "Recent main loop counters:\n"
            "  Iterations:   %llu (since last stats reset)\n"
            "  Average time: %llu us\n"
            "  Max time:     %lu us\n"
            "\n"
            "Recent second-core loop counters:\n"
            "  Iterations:   %llu (since last stats reset)\n"
            "  Average time: %llu us\n"
            "  Max time:     %lu us\n",
            now, days, hh, mm, ss,
            s.nLoopsEver, s2.nLoopsEver,
            s.nLoops, s.totalTime / s.nLoops, s.maxTime,
            s2.nLoops, s2.totalTime / s2.nLoops, s2.maxTime);
    };

    // with no arguments, just show the stats
    if (c->argc == 1)
        return Stats(c);

    // parse options
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0)
        {
            Stats(c);
        }
        else if (strcmp(a, "-r") == 0 || strcmp(a, "--reset") == 0)
        {
            mainLoopStats.Reset();
            secondCoreLoopStatsResetRequested = true;            
            c->Print("Main loop statistics reset\n");
        }
        else
        {
            return c->Usage();
        }
    }
}

// ---------------------------------------------------------------------------
//
// Console command - show version information
//
static void Command_version(const ConsoleCommandContext *c)
{
    if (c->argc != 1)
        return c->Usage();

    c->Printf("Pinscape Pico Button Latency Tester II v%d.%d.%d (build %s)\n",
              VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, buildTimestamp);

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    c->Printf("Pico hardware ID:   %02X%02X%02X%02X%02X%02X%02X%02X\n",
              id.id[0], id.id[1], id.id[2], id.id[3],
              id.id[4], id.id[5], id.id[6], id.id[7]);

    uint8_t cpuVsn = rp2040_chip_version();
    uint8_t romVsn = rp2040_rom_version();
    char romVsnStr[16];
    sprintf(romVsnStr, "B%d", romVsn - 1);
    c->Printf(
        "Target board type:  %s\n"
        "RP2040 CPU version: %d\n"
        "RP2040 ROM version: %d (%s)\n"
        "Pico SDK version:   %s\n"
        "TinyUSB library:    %d.%d.%d\n"
        "Compiler:           %s\n",
        PICO_BOARD,
        cpuVsn,
        romVsn, romVsn >= 1 ? romVsnStr : "Unknown",
        PICO_SDK_VERSION_STRING,
        TUSB_VERSION_MAJOR, TUSB_VERSION_MINOR, TUSB_VERSION_REVISION,
        COMPILER_VERSION_STRING);
}

// ---------------------------------------------------------------------------
//
// Console command - reset the Pico
//
static void Command_reset(const ConsoleCommandContext *c)
{
    if (c->argc != 2)
        return c->Usage();

    if (strcmp(c->argv[1], "-r") == 0 || strcmp(c->argv[1], "--reboot") == 0 || strcmp(c->argv[1], "--reset") == 0)
    {
        c->Print("Resetting\n");
        picoReset.Reboot(false);
    }
    else if (strcmp(c->argv[1], "-b") == 0 || strcmp(c->argv[1], "--bootsel") == 0)
    {
        c->Print("Entering Pico ROM Boot Loader\n");
        picoReset.Reboot(true);
    }
    else
        c->Usage();
}
