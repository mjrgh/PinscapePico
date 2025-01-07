// Pinscape Pico - Main program entrypoint and main loop
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
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

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <pico/flash.h>
#include <pico/unique_id.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>

// local project headers
#include "Pinscape.h"
#include "Main.h"
#include "MultiCore.h"
#include "Utils.h"
#include "Config.h"
#include "FlashStorage.h"
#include "USBIfc.h"
#include "VendorIfc.h"
#include "Version.h"
#include "Logger.h"
#include "CommandConsole.h"
#include "Buttons.h"
#include "Outputs.h"
#include "Nudge.h"
#include "Accel.h"
#include "NightMode.h"
#include "crc32.h"
#include "Watchdog.h"
#include "PicoLED.h"
#include "StatusRGB.h"
#include "VendorIfc.h"
#include "Reset.h"
#include "I2C.h"
#include "SPI.h"
#include "PIOHelper.h"
#include "PWMManager.h"
#include "ADCManager.h"
#include "TimeOfDay.h"
#include "TVON.h"
#include "ExpansionBoard.h"
#include "Plunger/Plunger.h"
#include "Plunger/ZBLaunch.h"
#include "IRRemote/IRTransmitter.h"
#include "IRRemote/IRReceiver.h"
#include "Devices/Accel/LIS3DH.h"
#include "Devices/Accel/LIS3DSH.h"
#include "Devices/Accel/MXC6655XA.h"
#include "Devices/Accel/MC3416.h"
#include "Devices/DistanceSensor/VL6180x.h"
#include "Devices/GPIOExt/PCA9555.h"
#include "Devices/LinearPhotoSensor/TCD1103.h"
#include "Devices/LinearPhotoSensor/TSL1410R.h"
#include "Devices/ProxSensor/VCNL4010.h"
#include "Devices/PWM/PWMWorker.h"
#include "Devices/PWM/TLC59116.h"
#include "Devices/PWM/PCA9685.h"
#include "Devices/PWM/TLC5940.h"
#include "Devices/Quadrature/AEDR8300.h"
#include "Devices/RTC/DS1307.h"
#include "Devices/RTC/DS3231M.h"
#include "Devices/RTC/RV3032C7.h"
#include "Devices/ShiftReg/74HC595.h"
#include "Devices/ShiftReg/74HC165.h"


// Unit identifiers singleton
UnitID unitID;

// Main loop statistics
CoreMainLoopStats mainLoopStats;
volatile CoreMainLoopStats secondCoreLoopStats;
volatile bool secondCoreLoopStatsResetRequested = false;

// Debug/test variables
DebugAndTestVars G_debugAndTestVars;

// Forwards
static void SecondCoreMain();
static void MainLoop();
static void Configure(PicoReset::BootMode bootMode);
static void Command_memory(const ConsoleCommandContext *c);
static void Command_loopstats(const ConsoleCommandContext *c);
static void Command_reset(const ConsoleCommandContext *ctx);
static void Command_version(const ConsoleCommandContext *ctx);
static void Command_devtest(const ConsoleCommandContext *ctx);

// ---------------------------------------------------------------------------
// 
// Main program entrypoint.  This is where the primary core starts running
// after power-on or a soft reboot.
//
int main()
{
    // Add a startup banner at the top of the logger, with a couple of
    // blank lines for separation from previous material in the same window.
    // Turn off all of the decorations for this, so that it stands out.
    logger.SetDisplayModes(false, false, false);
    Log(LOG_INFO, "\n\n"
        "===========================================\n"
        "PinscapePico v%d.%d.%d, build %s\n"
        "\n",
        VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, buildTimestamp);

    // enable logger timestamps and type codes by default
    logger.SetDisplayModes(true, true, false);

    // restore the time-of-day information stashed before the reset, if possible
    timeOfDay.RestoreAfterReset();

    // Check for a watchdog reboot.  This indicates that the software
    // crashed or froze, and the watchdog intervened by resetting the
    // Pico.  We use the watchdog scratch registers to store our status
    // at the time of the crash so that we can take corrective action.
    auto lastBootMode = picoReset.GetBootMode();
    if (watchdog_caused_reboot()
        && (lastBootMode == PicoReset::BootMode::SafeMode || lastBootMode == PicoReset::BootMode::FactoryMode))
    {
        Log(LOG_INFO, "*** %s Mode engaged (due to unexpected watchdog reset, or by user request) ***\n",
            lastBootMode == PicoReset::BootMode::SafeMode ? "Safe" : "Factory");
    }

    // add our console commands
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
        "  -r, --reset      reset the Pico and restart the Pinscape firmware program\n"
        "  -s, --safe-mode  reset the Pico and restart Pinscape in Safe Mode\n"
        "  -b, --bootsel    launch the Pico ROM Boot Loader, for installing new firmware",
        Command_reset);

    CommandConsole::AddCommand(
        "version", "show firmware and hardware information",
        "version (no arguments)",
        Command_version);

    CommandConsole::AddCommand(
        "devtest", "special development testing functions",
        "devtest [options]\n"
        "  --block-irq <t>            block IRQs for <t> milliseconds\n"
        "  --config-put-timeout <t>   simulate config write delay of <t> milliseconds\n",
        Command_devtest);

    // Set the boot mode to apply to the NEXT hardwaer reset, in case a
    // watchdog reset occurs during initialization or early in the main
    // loop.  Watchdog intervention indicates a hardware or software
    // fault that froze the software.
    //
    // If we're currently booted in NORMAL mode, meaning that we loaded
    // the full configuration, select SAFE MODE as the next mode after a
    // crash reset.  An early crash from NORMAL mode likely means that
    // we crashed trying to configure some external hardware peripheral
    // or an optional feature enabled in the configuration, so it's
    // likely that we'll crash again immediately if we reboot and
    // restore the same configuration again.  SAFE MODE skips loading
    // the full user configuration, and loads only the user's Safe Mode
    // configuration, which is a backup config that's meant to be
    // stripped down to a basic, known-safe set of options.
    //
    // If we're current in SAFE MODE, downgrade the next boot to FACTORY
    // MODE.  A crash from safe mode means that even the stripped-down
    // safe mode config is crashing, so we shouldn't even try to load
    // that again.  Factory Mode doesn't load a config at all, which
    // makes all of the configurable subsystems use their default
    // settings.  That disables most external peripherals and optional
    // features, which should put the Pinscape software in a stable
    // state where it can at least present the USB Vendor Interface.
    switch (lastBootMode)
    {
    case PicoReset::BootMode::SafeMode:
    case PicoReset::BootMode::FactoryMode:
        // factory mode or safe mode - boot into factory mode
        picoReset.SetNextBootMode(PicoReset::BootMode::FactoryMode);
        break;

    default:
        // normal mode or unknown (probably a power-on reset) - boot into safe mode
        picoReset.SetNextBootMode(PicoReset::BootMode::SafeMode);
        break;
    }

    // Set the watchdog with a relatively long timeout, so that we'll
    // eventually reboot if we crash during initialization.  Unlike in
    // the main loop, where we want the watchdog to intervene in a
    // fraction of a second, we need a fairly leisurely timeout here.
    // Some intialization tasks necessarily spin for a while waiting for
    // external hardware devices to start up.  We need a short timeout
    // in the main loop because there might be solenoids depending on
    // short time limits being enforced to prevent overheating.  We're
    // not going to activate any of those during initialization, so this
    // timeout is just to prevent the Pico from freezing up entirely.
    WatchdogEnable(5000);
    
    // Initialize flash-safe execution.  This sets up an interrupt handler
    // that allows the second core to interrupt this core and put it into
    // a temporary spin loop so that the other core can safely take
    // exclusive control of the flash memory controller, such as to write
    // data to flash memory.
    flash_safe_execute_core_init();

    // initialize our PIO helpers
    PIOHelper::Init();

    // Initialize the miniature flash file system
    flashStorage.Initialize();
    flashStorage.Mount(FLASHSTORAGE_CENTRAL_DIRECTORY_SIZE);

    // Configure everything
    Configure(lastBootMode);

    // enable IRQ handling on the I2C bus controllers
    I2C::EnableIRQs(true);

    // enable the IR receiver (if configured)
    irReceiver.Enable();

    // Start the second core thread.  Note that we wait until after
    // configuration has completed, because this allows the second core
    // thread to safely access "configure-then-read-only" structures
    // without any concurrency protections.  Most of the structures set
    // up during configuration are only written during configuration,
    // and are read-only from that point forward, so there's no risk of
    // anything changing out from underneath the second thread when it
    // reads them, and thus no need for locks or mutexes to protect them
    // against concurrent access.  So the ordering of the startup here
    // is critical, and this one small detail eliminates the need for
    // extra complexity and overhead elsewhere.
    LaunchSecondCore(SecondCoreMain);

    // Run the output manager task to apply the logical output state on
    // all ports to the physical device port states, then enable the
    // physical outputs.  All of the output peripherals that have Output
    // Enable controls initialize with output ports disabled, to ensure
    // that connected devices remain off during initialization (to
    // prevent solenoids randomly firing during startup, for example).
    // Now that the output manager is initialized, we can have it
    // compute the correct physical port setting corresponding to the
    // initial logical setting for each port, which takes into account
    // Active High/Low and the like.  Once the ports are all in their
    // proper initial conditions, we can enable outputs.  This should
    // ensure glitch-free startup, with no ports randomly firing in the
    // brief time window between power-on and the completion of software
    // initialization.
    OutputManager::Task();
    OutputManager::EnablePhysicalOutputs(true);

    // Set the main loop watchdog to a solenoid-friendly short timeout.  The
    // watchdog is an on-board Pico peripheral that runs a countdown timer,
    // and executes a cold-boot reset on the whole Pico if the timer ever
    // reaches zero.  The program must periodically intervene by calling a
    // "keepalive" function, which restarts the countdown each time it's
    // called.  This is a fail-safe against software faults that are difficult
    // or impossible for the software itself to prevent with absolute
    // certainty, such as infinite loops, jumps to random locations, corrupted
    // program storage, and unhandled exceptions.  Note that the watchdog is
    // meant as a last resort: it's not a substitute for robustness in the
    // software's design and implementation.  Ideally, the software would
    // always work correctly and the watchdog would never have to intervene.
    // But it's still nice to have it there as a last resort, since this
    // program is specifically designed to act as a controller for high-power
    // devices, some of which could be damaged if a software fault were to
    // leave them in an "on" condition for an extended period.  The main
    // reason to enable the watchdog is to prevent that from happening if the
    // software ever gets stuck in an infinite loop or any other bad state
    // that prevents the main loop from running.  If the software does get
    // stuck, whatever the reason, the watchdog will reboot the whole Pico.
    // That will rapidly invoke the startup code, which should as one of its
    // first steps turn all of the controlled devices off.  The hardware reset
    // should ensure that whatever bad state caused the freeze has been
    // cleared and that ths post-boot startup code should be able to run
    // reliably.
    //
    // Note that we use a much longer timeout in the initialization section.
    // The section of code between the early intialization (where we should
    // have decisively turned off all output ports) and the main loop consists
    // of intializations, of the software system and the external peripherals.
    // Some of that setup code could be relatively time-consuming, and we
    // don't want the watchdog to trigger because we're sitting in a wait loop
    // waiting for some peripheral or other to initialize.  Many hardware
    // devices explicitly require prescribed delay times at startup.  We can
    // be cavalier about burning up time in wait loops during initialization,
    // because we're confident that everything sensitive got switched off in
    // the early stages of startup, and won't get switched on again until the
    // main loop gets going.  So the initialization code uniquely doesn't need
    // quick watchdog protection.  The only need for the watchdog during
    // startup is as a convenience to the user to automatically reset the Pico
    // if startup crashes, so that they don't have to reset it manually.
    WatchdogEnable(100);

    // Run the main loop
    MainLoop();

    // The main loop never terminates, so nothing after this point will ever
    // be reached in practice.  In a more conventional program running on a
    // machine with an operating system, the main loop would eventually
    // exit, and we'd have to release resources here, shut down any active
    // background tasks, and otherwise prepare for normal program exit.
    // None of that applies in a bare-metal program like this that acts as
    // its own operating system and thus never exits (the only way to stop
    // the program is to reset the CPU or remove power).  However, for the
    // sake of documentation, we'll show some steps below that we would need
    // to perform if the main loop ever did exit and we were going to return
    // to a calling container program.

    // clean up flash-safe execution
    flash_safe_execute_core_deinit();

    // done
    return 0;
}

// ---------------------------------------------------------------------------
//
// Configure all subsystems
//

static void Configure(PicoReset::BootMode bootMode)
{
    // load the configuration file from flash
    JSONParser json;
    bool configValid = config.Load(bootMode, json);

    // Configure the logger.  This is the first thing we configure
    // (after loading the configuration itself) so that the logger can
    // capture any error messages that occur during initialization.
    logger.Configure(json);

    // note the config status for the log
    Log(LOG_CONFIG, "Config file %s\n", configValid ? "loaded OK" : "not loaded, using defaults");

    // get the Pinscape Pico unit identifiers
    auto const *id = json.Get("id");
    unitID.unitNum = id->Get("unitNum")->UInt8(1);
    unitID.unitName = id->Get("unitName")->String("Pinscape");

    // assign the LedWiz emulation unit mask
    if (auto *lw = id->Get("ledWizUnitNum"); lw->IsNumber())
    {
        // A numeric value means "start at this unit, assign sequentially from there".
        // So set the mask with all bits set above the target unit number.
        int n = lw->Int8(1);
        n = n < 1 ? 1 : n > 16 ? 16 : n;
        unitID.ledWizUnitMask = (0xFFFF << (n-1)) & 0xFFFF;
    }
    else if (lw->IsArray())
    {
        // An array means "assign the units included in the array"
        uint16_t mask = 0;
        lw->ForEach([&mask](int index, const JSONParser::Value *ele) {
            if (int n = ele->Int(-1); n >= 1 && n <= 16)
                mask |= 1 << (n - 1);
        });
        unitID.ledWizUnitMask = mask;
    }
    else
    {
        // use the default (all IDs enabled), and warn if the property was defined
        // (meaning it's some invalid type - 'undefined' is okay since this property
        // is optional)
        unitID.ledWizUnitMask = 0xFFFF;
        if (!lw->IsUndefined())        
            Log(LOG_WARNING, "id.ledWizUnitNum must be a number or array of numbers; using default value 1\n");
    }

    // Configure the vendor interface logger
    vendorInterfaceLogger.Configure(json);

    // Configure UART logging
    uartLogger.Configure(json);

    // Configure the expansion board setup
    expansionBoard.Configure(json);

    // Cycle power to the peripherals, if possible
    expansionBoard.CyclePeripheralPower();

    // Configure the PWM manager
    pwmManager.Configure(json);

    // Configure and initialize I2C and SPI buses.  Note that bus
    // configuration must happen before any devices on the respective buses
    // are configured, so that the bus manager objectss have been created
    // before the device objects are added to their respective bus managers.
    I2C::Configure(json);
    SPI::Configure(json);

    //
    // Configure chips.  Chip configuration generally must come after
    // bus configuration (I2C, SPI), since many of the chips we support
    // are connected through one of the standard buses; and before the
    // higher-level "feature" subsystem configuration, since many of the
    // systems set up connections to peripheral devices.
    //

    // Configure RTC clock/calendar chips
    DS1307::Configure(json);
    DS3231M::Configure(json);
    RV3032C7::Configure(json);

    // Configure GPIO extenders.  These must go first, to allow them to
    // be used as control outputs or sense inputs connected to other
    // chips.
    PCA9555::Configure(json);

    // Configure PWM controllers and shift-register output chips
    TLC59116::Configure(json);
    PCA9685::Configure(json);
    TLC5940::Configure(json);
    C74HC595::Configure(json);
    PWMWorker::Configure(json);

    // Configure shift-register input chips
    C74HC165::Configure(json);

    // Configure accelerometer devices
    MXC6655XA::Configure(json);
    MC3416::Configure(json);
    LIS3DH::Configure(json);
    LIS3DSH::Configure(json);

    // Configure the ADC manager and outboard ADC chips
    adcManager.Configure(json);

    // Configure distance sensor/proximity sensor chips
    VL6180X::Configure(json);
    VCNL4010::Configure(json);

    // Configure quadrature encoder chips
    AEDR8300::Configure(json);

    // Configure image sensor chips
    TCD1103::Configure(json);
    TSL1410R::Configure(json);

    // Get the configured virtual HID input devices
    bool useKb = keyboard.Configure(json);
    bool useGamepad = gamepad.Configure(json);
    bool useXInput = xInput.Configure(json);
    bool usePinControl = openPinballDevice.Configure(json);

    // Set XInput enable status
    xInput.enabled = useXInput;

    // Configure the USB interface
    usbIfc.Configure(json);

    // Create the first HID interface
    std::list<USBIfc::HIDIfc*> hids;
    hids.emplace_back(usbIfc.AddHIDInterface());

    // Add the keyboard, if configured
    if (useKb)
    {
        // add the keyboard and media controller to the main HID interface
        hids.front()->AddDevice(&keyboard);
        hids.front()->AddDevice(&mediaControl);
    }

    // Add the gamepad, if configured
    if (useGamepad)
    {
        // We want to expose the gamepad and keyboard on separate interfaces,
        // if both are present.  The gamepad will generate continuous input
        // events if an accelerometer is attached, because the accelerometer
        // picks up small enough vibrations that it generally has changes to
        // report at every polling interval.  If the keyboard is on the same
        // interface, keyboard events will have average latency of 1.5x the
        // polling interval.  It's important to minimize keyboard latency,
        // so we expose the keyboard on a separate interface.  The host
        // polls each interface separately, so this will prevent key events
        // from waiting in line behind joystick events.  Note that neither
        // the keyboard nor joystick should be combined into an interface
        // that has any OTHER high-volume device, since both require low
        // latency for their button event inputs.
        if (hids.back()->GetNDevices() != 0)
            hids.emplace_back(usbIfc.AddHIDInterface());
        hids.back()->AddDevice(&gamepad);
    }

    // Add the pinball controller, if configured
    if (usePinControl)
    {
        // As with the gamepad, the pinball controller should be on its
        // own interface for low latency.
        if (hids.back()->GetNDevices() != 0)
            hids.emplace_back(usbIfc.AddHIDInterface());
        hids.back()->AddDevice(&openPinballDevice);
    }

    // Add the feedback controller device to HID0.  The feedback controller
    // primarily transacts Output (host-to-device) reports, and only sends
    // back low-priority replies to queries from the host, so it can
    // share an interface with other devices without creating latency for
    // them, and can tolerate any latency they create for it.
    hids.front()->AddDevice(&feedbackController);
    feedbackController.Init();

    // USB interfaces are configured - initialize the Tinyusb subsystem
    usbIfc.Init();

    // Configure USB CDC (virtual COM port) logging
    usbCdcLogger.Configure(json);

    //
    // Configure the higher-level "feature" subsystems.  Most of these
    // must be configured after the lower-level hardware interfaces are
    // configured, because many of the feature subsystems set up
    // connections to hardware peripherals, and thus need to know which
    // peripherals are available.
    //

    // configure the buttons inputs
    Button::Configure(json);

    // configure outputs
    OutputManager::Configure(json);

    // configure TV ON
    tvOn.Configure(json);

    // configure the IR remote control features
    irTransmitter.Configure(json);
    irReceiver.Configure(json);
    psVendorIfc.ConfigureIR(json);

    // configure the RGB status LED
    statusRGB.Configure(json);

    // configure the nudge device interface
    nudgeDevice.Configure(json);

    // configure the plunger sensor and ZB Launch
    plunger.Configure(json);
    zbLaunchBall.Configure(json);
}

// ---------------------------------------------------------------------------
//
// Main Loop
//
static void MainLoop()
{
    // Main loop.
    //
    // The main job of this outer loop is to invoke all of the asynchronous
    // tasks, and then go back and invoke them all again, repeating forever.
    // Everything in the main loop should use the "async task" idiom.  The
    // main loop is essentially just an async task dispatcher.
    // 
    // Some tasks are little state machines that carry out a small increment
    // of computing work, update their internal state, and return.  Some
    // tasks are essentially timers, monitoring the system clock and simply
    // returning each time they're invoked until the appointed hour arrives,
    // at which point they carry out some small increment of work and wait
    // again for an update scheduled time.  Some tasks manage external
    // hardware peripherals that can carry out time-consuming tasks in the
    // background; these tasks send work to their respective devices, then
    // return to the main loop each time they're invoked, until the device
    // indicates that it's ready to accept new work, at which point they
    // send any new work and repeat the async wait.
    //
    // For all task types, the goal is to carry out the next bit of work and
    // return as quickly as possible.  The loop is single-threaded, so the
    // time between invocations of a given task is the sum of the processing
    // times for all of the other tasks.  It's thus critical for each task
    // to return quickly, so that all of the other tasks have a chance to
    // run at regular, short intervals.  Some of the tasks poll devices for
    // input, and those tasks in particular need to be called frequently, to
    // keep input latency low.  Experience with the original Pinscape
    // software established that a virtual pinball controller will work well
    // with a maximum main loop time of around 2ms.  The limiting factor is
    // typically the plunger sensors, which have to be polled at around this
    // rate (or faster) to avoid aliasing of the position readings when the
    // plunger is moving near its peak speed.  Button inputs and accelerometer
    // inputs can also benefit from fast polling, for local processing on the
    // device between USB HID updates.  Most other devices and tasks can
    // tolerate longer update cycles without any performance impact.
    for (uint64_t tTopOfLoop = time_us_64(), mainLoopSafeTime = tTopOfLoop + 120000000 ; ; )
    {
        // check for a pending Pico reset
        picoReset.Task();

        // Run USB tasks on the main USB interface and our sub-interfaces
        usbIfc.Task();
        xInput.Task();
        psVendorIfc.Task();

        // Run logging tasks
        logger.Task();

        // run IR remote control tasks
        irReceiver.Task();

        // Blink the Pico's on-board LED, as a visual indication that the
        // software is still running; and update the RGB status LED, if
        // present.
        picoLED.Task();
        statusRGB.Task();

        // Run nudge device tasks
        nudgeDevice.Task();

        // Run plunger tasks
        plunger.Task();
        zbLaunchBall.Task();

        // Run button tasks
        Button::Task();

        // Run output manager tasks
        OutputManager::Task();

        // Run TV-ON tasks
        tvOn.Task();

        // Run I2C tasks (schedules bus access for all of the I2C devices)
        I2C::Task();

        // Run 74HC595 tasks
        C74HC595::Task();

        // Update the watchdog counter.  This is our keep-alive signal
        // to the watchdog, letting the watchdog know that the main loop
        // is still running properly and doesn't need to be rebooted out
        // of a crash, fault, or infinite loop.
        watchdog_update();

        // collect loop timing statistics for this loop iteration
        uint64_t tEndOfLoop = time_us_64();  
        mainLoopStats.AddSample(static_cast<uint32_t>(tEndOfLoop - tTopOfLoop));
        tTopOfLoop = tEndOfLoop;

        // If we made it past the main loop safe time, update the watchdog
        // scratch registers to indicate that we can do a normal boot on
        // the next reset.
        if (tEndOfLoop > mainLoopSafeTime)
        {
            picoReset.SetNextBootMode(PicoReset::BootMode::Normal);
            mainLoopSafeTime = ~0ULL;  // infinity
        }
    }
}

// ---------------------------------------------------------------------------
//
// Second core entrypoint.  This is the entrypoint for the second CPU
// core after a reboot.
//
// The secondary core's entire job currently is polling GPIO-source
// button inputs.  It's useful to poll GPIO buttons at extremely high
// frequency so that we can catch switch state transitions early in the
// "bounce" phase, during which the voltage rapidly oscillates between
// on and off states.  Bounces can be just a few microseconds long, so
// we have to poll at that time scale to catch the transition early in
// the process.  The earlier we catch the bounce oscilations, the
// quicker we detect the button state change, and the lower the latency
// in the corresponding HID report to the host.
//
// The Pico hardware is capable of interrupting the CPU on a GPIO input
// edge, so we *could* do the button monitoring via an IRQ handler.  But
// the high-frequency oscillations make it somewhat undesirable to use
// an interrupt handler, because of the heavy load it places on the CPU
// to service the flurry of interrupts that occur at switch transitions.
// Offloading this work to the second core eliminates the load on the
// primary core.
//
// Note that polling the GPIO inputs is somewhat better than using
// interrupts, because of the more constant secondary core CPU load with
// polling, but only if the main loop doesn't have other work to do.  As
// long as the loop is only polling the buttons, we can go through the
// loop in a few microseconds, achieving the high polling rate we need
// to achieve extremely low-latency button state change detection.  If
// in the future we need to add substantial other work here that extends
// the loop out to more than perhaps 25us, it will become better to do
// the monitoring via interrupts.
//
static void SecondCoreMain()
{
    // enter our main loop
    for (uint64_t t0 = time_us_64() ; ; )
    {
        // run second-core button polling
        Button::SecondCoreDebouncedSource::SecondCoreTask();

        // run 74HC165 second-core tasks
        C74HC165::SecondCoreTask();

        // update statistics
        uint64_t now = time_us_64();
        secondCoreLoopStats.AddSample(now - t0);
        t0 = now;

        // Reset statistics if desired.  The main core can set the flag to
        // request a reset, which we carry out on our core, since we always
        // have exclusive write access to the stats struct.
        //
        // Note that we don't need mutex protection for the test-and-set
        // sequence on the reset flag.  A test-and-set without protection
        // would in most cases constitute a race condition, in that the
        // other core could sneak in between the test and set operations
        // to change the value out from under us.  That will do no harm
        // in this special case, though, because the other core can only
        // ever set the flag to 'true', and we can only set it to 'false'.
        // If the other core does happen to set it to 'true' between the
        // test and set, it doesn't change the outcome of the test, and
        // the other core still gets the timely reset it wanted, so the
        // overall outcome is exactly the same as if the race had gone
        // the other way.  The race condition technically exists but is
        // moot by virtue of the identical outcomes.  This saves us the
        // slight performance cost of a formal synchronization step
        // (such as acquiring a mutex).
        if (secondCoreLoopStatsResetRequested)
        {
            secondCoreLoopStatsResetRequested = false;
            secondCoreLoopStats.Reset();
        }
    }
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

    c->Printf("Pinscape Pico firmware v%d.%d.%d (build %s)\n",
              VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, buildTimestamp);

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    c->Printf("Pico hardware ID: %02X%02X%02X%02X%02X%02X%02X%02X\n",
              id.id[0], id.id[1], id.id[2], id.id[3],
              id.id[4], id.id[5], id.id[6], id.id[7]);

    uint8_t cpuVsn = rp2040_chip_version();
    uint8_t romVsn = rp2040_rom_version();
    char romVsnStr[16];
    sprintf(romVsnStr, "B%d", romVsn - 1);
    c->Printf(
        "RP2040 CPU version: %d\n"
        "RP2040 ROM version: %d (%s)\n"
        "Pico SDK version:   %s\n"
        "Compiler:           %s\n",
        cpuVsn,
        romVsn, romVsn >= 1 ? romVsnStr : "Unknown",
        PICO_SDK_VERSION_STRING,
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
    else if (strcmp(c->argv[1], "-s") == 0 || strcmp(c->argv[1], "--safe-mode") == 0)
    {
        c->Printf("Entering Safe Mode\n");
        picoReset.Reboot(false, PicoReset::BootMode::SafeMode);
    }
    else if (strcmp(c->argv[1], "-b") == 0 || strcmp(c->argv[1], "--bootsel") == 0)
    {
        c->Print("Entering Pico ROM Boot Loader\n");
        picoReset.Reboot(true);
    }
    else
        c->Usage();
}

// ---------------------------------------------------------------------------
//
// Console command - development/debug tests
//
void Command_devtest(const ConsoleCommandContext *c)
{
    // make sure we have at least one option
    if (c->argc <= 1)
        return c->Usage();

    // parse options
    for (int argi = 1 ; argi < c->argc ; ++argi)
    {
        const char *arg = c->argv[argi];
        if (strcmp(arg, "--block-irq") == 0)
        {
            // get the time
            if (argi + 1 >= c->argc)
                return c->Printf("Missing time argument for --block-irq\n");
            int t = atoi(c->argv[++argi]);

            // wait with interrupts off
            c->Printf("Disabling interrupts and waiting %d ms...\n", t);
            uint64_t tEnd = time_us_64() + t*1000;
            {
                // extend the watchdog by the wait time (plus a bit)
                WatchdogTemporaryExtender wte(t + 10);

                // disable IRQs
                IRQDisabler irqd;

                // wait; run the logger task in the meantime
                while (time_us_64() < tEnd)
                    logger.Task();
            }
        }
        else if (strcmp(arg, "--config-put-timeout") == 0)
        {
            // get the delay time
            if (argi + 1 >= c->argc)
                return c->Printf("Missing time delay argument for --config-put-timeout\n");

            // set the time, converting milliseconds to microseconds
            auto delay_ms = atoi(c->argv[++argi]);
            G_debugAndTestVars.putConfigDelay = delay_ms * 1000ULL;
            if (delay_ms == 0)
                c->Printf("PutConfig delay simulation ended\n");
            else
                c->Printf("PutConfig delay simulation now in effect, delay %u ms; set to 0 to end simulation\n", delay_ms);
        }
        else
        {
            return c->Usage();
        }
    }
}
