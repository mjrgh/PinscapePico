// Pinscape Pico firmware - Output Port Manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <ctype.h>
#include <vector>
#include <list>
#include <functional>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <pico/flash.h>
#include <pico/unique_id.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>
#include <hardware/i2c.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Logger.h"
#include "GPIOManager.h"
#include "PWMManager.h"
#include "Outputs.h"
#include "NightMode.h"
#include "TimeOfDay.h"
#include "Buttons.h"
#include "XInput.h"
#include "TVON.h"
#include "Nudge.h"
#include "StatusRGB.h"
#include "USBIfc.h"
#include "CommandConsole.h"
#include "Plunger/Plunger.h"
#include "Plunger/ZBLaunch.h"
#include "IRRemote/IRReceiver.h"
#include "IRRemote/IRTransmitter.h"
#include "Devices/GPIOExt/PCA9555.h"
#include "Devices/PWM/TLC59116.h"
#include "Devices/PWM/TLC5940.h"
#include "Devices/PWM/TLC5947.h"
#include "Devices/PWM/PCA9685.h"
#include "Devices/PWM/PWMWorker.h"
#include "Devices/ShiftReg/74HC595.h"

// port storage
std::list<OutputManager::Port> OutputManager::portList;

// Ports indxed by number.  Port #0 is unused, since we number the ports
// according to DOF conventions, which starts its nominal port numbering
// at port #1.  It's less error-prone (both here and for users creating
// configurations) to use a consistent numbering convention, and since
// we have to use DOF conventions in certain user-facing contexts, we
// have to use it everywhere if we want consistency.
std::vector<OutputManager::Port*> OutputManager::portsByNumber{ nullptr };

// ports indexed by name
std::unordered_map<std::string, OutputManager::Port*> OutputManager::portsByName;

// has the output manager been configured yet?
bool OutputManager::isConfigured = false;

// is the output manager Task() routine suspended?
bool OutputManager::isSuspended = false;

// suspend mode end time
uint64_t OutputManager::suspendModeEndTime = 0;

// post-configuration callback list
std::list<std::function<void(JSONParser&)>> OutputManager::postConfigCallbacks;

// Nudge device view.  If any outputs use the nudge device in a data source
// calculation, we'll create a nudge view.  This single instance can be shared
// among all outputs that access the nudge device, since the outputs are all
// updated at the same time.
static NudgeDevice::View *outputNudgeView = nullptr;

// Share group container map by name
std::unordered_map<std::string, OutputManager::ShareGroupContainer> OutputManager::shareGroups;

// look up a port by number or name from a JSON configuration cross-reference
OutputManager::Port *OutputManager::Get(JSONParser &, const JSONParser::Value *val)
{
    // if the JSON value is a number, look up the port by number;
    // otherwise coerce the value to a string and look up the port by
    // name
    return val->IsUndefined() ? nullptr :
        val->IsNumber() ? Get(val->Int()) :
        Get(val->String().c_str());
}

// set an LedWiz SBA state
void OutputManager::SetLedWizSBA(int portNum, bool on, uint8_t period)
{
    if (auto *port = Get(portNum) ; port != nullptr)
        port->SetLedWizSBA(on, period);
}

// set an LedWiz PBA state
void OutputManager::SetLedWizPBA(int portNum, uint8_t profile)
{
    if (auto *port = Get(portNum) ; port != nullptr)
        port->SetLedWizPBA(profile);
}


// Configure outputs from the JSON
//
// The output ports are configured via the 'outputs' property of
// the configuration root, which contains an array of descriptor
// objects.
//
// The first array entry is assigned as output port #1, the second
// is port #2, etc.  We start the numbering at #1 for consistency
// with the numbering used in almost all of the user-facing tools,
// most especially the DOF Config Tool.  The port number is used in
// all cross-references to the port.
//
// outputs: [
//   // output port #1
//   {
//     name: <string>,               // give the port a name, for references from data sources and other config entries
//     device: {  },                 // device specification - see DEVICE Below
//     gamma: <bool>,                // apply gamma correction to the physical output level
//     noisy: <bool>,                // disable the device when Night Mode is active
//     inverted: <bool>,             // invert the logical PWM level at the physical port
//     timeLimit: <milliseconds>,    // Flipper Logic limit time for high-level activation
//     powerLimit: <number>,         // Flipper Logic power limit (0..255) after time limit expires
//     coolingTime: <milliseconds>,  // Flipper Logic cooling-off period (minimum time between consecutive high-power activations)
//     source: <string>,             // data source; see below
//   },
//
//   // output port #2, ...
// ]
//
// DEVICE Each 'device' object must have a 'type' string that specifies
// the physical or virtual device controlled through the logical output
// port.  Additional parameters vary by device type.
//
// Basic object syntax:
//   device: { type: "type", /* other params... */ }
//
// Parameters by device type:
//
//   type: "gpio",                    // direct output through GPIO port
//     gp: <portNumber>,              // GP port number, 0-28
//     pwm: <bool>,                   // true -> port is PWM capable, false -> digital ON/OFF port
//     freq: <number>,                // PWM frequency in Hz, for PWM ports; uses pwm.defaultFreq otherwise
//  
//   type: "tlc59116",                // output through a port on a TLC59116 PWM controller chip
//     chip: <number>,                // chip number, as an index (0..) in tlc59116[] configuration array
//     port: <number>,                // port number on the chip, 0 to 15, corresponding to OUT0 to OUT15 pins on the chip
//  
//   type: "tlc5940",                 // output through a port on a TLC5940 PWM controller daisy chain
//     chain: <number>,               // chain number, as in an index (0..) in tlc5940[] configuration array; okay to omit if only one chain is defined
//     chip: <number>,                // chip index on the chain, 0=first chip (the one connected directly to the Pico)
//     port: <number>,                // port number on the chip, 0 to 15 for pins OUT0 to OUT15
//  
//   type: "tlc5947",                 // output through a port on a TLC5947 PWM controller daisy chain
//     chain: <number>,               // chain number, as in an index (0..) in tlc5947[] configuration array; okay to omit if only one chain is defined
//     chip: <number>,                // chip index on the chain, 0=first chip (the one connected directly to the Pico)
//     port: <number>,                // port number on the chip, 0 to 23 for pins OUT0 to OUT23
//  
//   type: "pca9685",                 // output through a port on a PCA9685 PWM controller chip
//     chip: <number>,                // chip number, as an index (0..) in pca9685[] configuration array
//     port: <number>,                // port number on the chip, 0 to 15, corresponding to LED0 to LED15 pins on the chip
//
//   type: "pca9555",                 // output through a port on a PCA9555 GPIO extender chip
//     chip: <number>,                // chip number, as an index (0..) in pca9555[] configuration array
//     port: <string>,                // port number on the chip, in either NXP or TI notation: "IO0_3", or just "03"
//
//   type: "74hc595",                 // output through a port on a 74HC595 daisy chain
//     chain: <number>,               // chain number, as an index (0..) in the "74hc595"[] configuration array; okay to omit if there's only one chain defined
//     chip: <number>,                // chip index on the chain, 0=first chip (the one connected directly to the Pico)
//     port: <number>,                // port number on the chip, 0 to 7 for output ports Q0 to Q7
//
//   type: "workerPico",              // output through an external Pico device, configured as a PWM Worker
//     unit: <number>,                // unit number, as an index (0..) in the workerPico[] configuration array
//     port: <number>,                // port number on the worker Pico, 0..23 (this is in terms of the abstract port labels
//                                    // assigned by the PWMWorker software on the other Pico, NOT the physical GPIO port numbers
//                                    // on the other Pico; each physical GPIO used as a PWM output is assigned an abstract Worker
//                                    // Pico port number 0-23)
//  
//   type: "zblaunch",                // ZB Launch control virtual port; receives ZB launch mode status; no physical device attached
//     
//   type: "virtual",                 // generic virtual output with no physical device; lets the host send data to port for use by other subsystems
//
// SOURCE

// Each port has an optional virtual data source.  The default, if no
// 'source' property is present, is that the host PC controls the the
// port through the USB connection, using DOF and other feedback-aware
// software.
//
// If a port has a 'source', the host PC won't be able to directly
// control the port, since the 'source' expression overrides the level
// set by the host.  However, the port is still visible to the host, and
// the host can still send level values to the port to store internally.
// We call these host-set values the "raw" levels, and you can access
// them via the rawport() expression.  An expression can thus simply
// apply a modification to the DOF value sent to the same port; for
// example, you can make a port blink when DOF turns it on by setting
// 'source' to "and(blink(250,250), self)".
//
//   "<number>",                      // a constant number value, e.g., "255"
//   "#n",                            // equivalent to port(n)
//   "port(<n>)",                     // the current computed value of the given port number; used in nested expressions
//   "port('name')",                  // same as above, but the port is identified by name rather than by number
//   "rawport(<n> or 'name')"         // raw port value: the value set directly by the host (e.g., DOF), bypassing the port's 'source' computation
//   "self",                          // raw port value of the current port (last level set directly by the host, such as from DOF)
//   "tvon",                          // TV-ON relay; port is controlled by the TV-ON countdown timer
//   "powersense(<off>,<countdown>,<relay>,<ir>,<on>)"  // TV-ON power-sensing circuit state; selects <off> when secondary PSU power is off,
//                                    // <countdown> when countdown is running after power on, <relay> when pulsing the relay, <ir> when
//                                    // sending IR TV ON commands, <on> during normal operation
//   "button(<number>,<on>,<off>)",   // port is controlled by the specified button state; returns <on> when button is pressed, <off> when not pressed
//   "button('name',<on>,<off>)",     // same as above, but the button is identified by name rather than by number
//   "zblaunch(<on>,<off>)",          // ZB Launch virtual button status; returns <on> when ZB Launch button pulse is firing, <off> otherwise
//   "zblaunchmode(<on>,<off>)",      // ZB Launch mode status; returns <on> when the ZB Launch Mode is engaged, <off> otherwise
//   "irtx(<onSource>,<offSource>)",  // IR remote transmission in progress; returns <onSource> when a transmission is running, <offSource> otherwise
//   "irrx(<onSource>,<offSource>)",  // IR remote reception in progress; returns <onSource> when a code is being received, <offSource> otherwise
//   "nudge",                         // nudge device reading as a vector value; if used as a scalar, the value is the normalized 0..255 magnitude
//   "plungerpos",                    // plunger position, normalized to 0..255, with the rest position at 42; values less than 42 are forward of the rest position
//   "plungerposf",                   // plunger position, as a floating point value -32768 to +32767, with the rest position at zero; negative values are forward of the rest position
//   "xboxled(<n>)",                  // XBox controller indicator LED, <n> in 1..4 for LED #1 to #4; 0 when LED is off, 255 when LED is on
//   "xboxrumble(<n>)",               // XBox controller rumble motor, <n> in 1..2 for left/right; yields current rumble intensity, 0..255
//   "x(<vector>)",                   // X component of a vector value
//   "y(<vector>",                    // Y component of a vector value
//   "arctan(<vector>)",              // arctangent of the given vector value, normalized to 0..255 (128 = 180 degrees, 255 = 358.6 degrees)
//   "arctan(<y>,<x>)",               // arctangent of the given y,x coordinate, normalized to 0..255 (128 = 180 degrees, 255 = 358.6 degrees)
//   "grayscale(<r>,<g>,>b>)",        // this port is the grayscale value derived from the three inputs
//   "grayscale(<rgb>)",              // grayscale derived from an RGB-valued source
//   "hue(<rgb>)",                    // translates the RGB value to HSB space, and returns the hue component
//   "saturation(<rgb>)",             // translates the RGB value to HSB space, and returns the saturation component
//   "brightness(<rgb>)",             // translates the RGB value to HSB space, and returns the brightness component
//   "hsb(<h>,<s>,<b>)",              // vector RGB value of Hue, Saturation, Brightness for the three inputs
//   "red(<rgb>)",                    // red component of RGB value
//   "green(<rgb>)",                  // green component of RGB value
//   "blue(<rgb>"),                   // blue component of RGB value
//   "ramp(<ms>)",                    // linear ramp value, varying from 0 to 255 over the millisecond period given, then repeating
//   "sine(<ms>)",                    // sine wave with the millisecond period given, varying between 0 and 255
//   "sawtooth(<ms>)",                // sawtooth wave with the millisecond period given, varying between 0 and 255
//   "blink(<on>,<off>)",             // blinking at 0/255, milliseconds on, milliseconds off; returns <on> wduring on period, <off> during off period
//   "scale(<source>,<number>)",      // scale the source input by the given factor; e.g., "scale(sine(1000),0.5)" for a half-height sine wave
//   "offset(<source>,<number>)",     // add an offset to the given source
//   "and(<source>,...)",             // returns last source value if all sources are non-zero, else zero
//   "or(<source>,...)",              // returns first non-zero source value, or zero if they're all zero
//   "max(<source>,...)",             // returns highest value for all of the sources
//   "min(<source>,...)",             // returns lowest value for all of the sources
//   "if(<condition>,<then>,<else>)", // if the <condition> source is ON (any non-zero value), returns <then>, otherwise returns <else>
//   "select(<control>,<val1>,<expr1>,...,<default>)",   // if <control> == <val1>, yield <expr1>, etc for any number of additional
//                                    // <val>,<expr> pairs; if none of the <valN> values are matched, yields <default>
//   "clip(<source>,<min>,<max>)"     // clips the source to at least <min> and at most <max> (min and max are constants)
//   "nightmode(<onSource>,<offSource>)",  // night mode status; returns <onSource> when night mode is in effect, <offSource> otherwise
//   "plungercal(<on>,<hold>,<off>)"  // plunger calibration mode; <on> when calibrating, <hold> when button is being held before calibration
//                                    // mode is engaged; <off> otherwise
//   "time('range',<in>,<out>)"       // time of day: if the current time of day is in range, returns <in>, else <out>; the range is a string
//                                    // with a time range, 'from-to'; each time can be hh:mm:ss on a 24-hour clock, or with an "am" or "pm"
//                                    // designator, with mm and ss optional; examples: '9pm-9am', '23:30-01:00'
//
// Sources can be nested.  For example, "red(hsb(ramp(2000),255,#17))"
// is the red component of the RGB value formed from the
// Hue/Saturation/Brightness formula, with the hue coming from a timed
// ramp, the saturation fixed at 255, and the brightness coming from
// output port #17.
//
// Simple comparison and math expressions are also allowed:
//
//   a + b    - sum of the two sources
//   a - b    - difference
//   a * b    - multiply
//   a / b    - divide
//   a % b    - modulo
//   a = b    - equality; yields 255 if the two values are equal after being converted to compatible types, 0 if not
//   a == b   - same as a = b, for those who prefer C/C++/javascript syntax
//   a === b  - identically equal: true if a and b have the same type and value
//   a != b   - inequality; the inverse of a == b
//   a <> b   - same as a != b, for those who prefer BASIC syntax
//   a !== b  - not identically equal; the inverse of a === b
//   a > b    - greater than numerically
//   a >= b   - greater or equal numerically
//   a < b    - less than numerically
//   a <= b   - less than or equal numerically
//
void OutputManager::Configure(JSONParser &json)
{
    // set up our console commands
    CommandConsole::AddCommand(
        "out", "set output port levels (simulate DOF commands)",
        "out [options] <port>=<level> ...\n"
        "options:"
        "  -O, --all-off  set all port levels to 0\n"
        "  -l, --list     list the output ports\n"
        "  --suspend      suspend output management, for direct device-level testing\n"
        "  --resume       resume normal output management\n"
        "\n"
        "  <port>         output port number or name (as specified in the configuration)\n"
        "  <level>        PWM level, 0-255",
        &Command_out);

    // Make the first pass over the outputs[] array.  This parses everything except
    // the 'source' definitions, which we defer to a second pass, because sources can
    // refer to other output ports.  We set up all of the port objects on the first
    // pass, so they'll all be populated by the time we parse the sources on the
    // second pass.
    json.Get("outputs")->ForEach([&json](int index, const JSONParser::Value *value)
    {
        // log extended debug info
        Log(LOG_DEBUGEX, "Output configuration: parsing output #%d\n", index);

        // get the common output parameters
        char jsonLocus[32];
        snprintf(jsonLocus, sizeof(jsonLocus), "outputs[%d]", index);
        auto *nameVal = value->Get("name");
        auto *devSpec = value->Get("device");
        auto *devType = devSpec->Get("type");
        const char *pinoutLabel = nameVal->IsUndefined() ?
            Format("Output #%d", index + 1) :
            Format("Output #%d (%s)", index + 1, nameVal->String().c_str());

        // Parse the device type.  Use PWM mode by default for devices that
        // can operate in digital or PWM mode.
        Device *device = ParseDevice(jsonLocus, pinoutLabel, json, devSpec, true, false);

        // If we didn't manage to create a device, create a null device
        // as a placeholder.  This lets us still create a working port
        // object, so that our internal port list numbering stays in
        // sync with the config file's list, and the port won't crash
        // the program when it's invoked.
        if (device == nullptr)
            device = new NullDev();

        // add the port to the storage list and the by-port-number index
        auto &port = portList.emplace_back(device);
        portsByNumber.emplace_back(&port);

        // configure additional options
        port.noisy = value->Get("noisy")->Bool();
        port.flipperLogic.dtHighPowerMax = value->Get("timeLimit")->UInt32(0) * 1000;
        port.flipperLogic.dtCooling = value->Get("coolingTime")->UInt32(0) * 1000;
        port.flipperLogic.reducedPowerLevel = value->Get("powerLimit")->UInt8(0);

        // check for a share group
        if (auto *shareGroupVal = value->Get("shareGroup"); !shareGroupVal->IsUndefined())
        {
            // it has a share group or list of groups - add the port to the group containers
            shareGroupVal->ForEach([&port](int index, const JSONParser::Value *val) {
                auto *container = ShareGroupContainer::FindOrCreate(val->String().c_str());
                port.shareGroups.emplace_back(container);
                container->AddPort(&port);
            }, true);

            // warn if there aren't any share groups
            if (port.shareGroups.size() == 0)
                Log(LOG_WARNING, "Output[%d]: no groups listed in 'shareGroup' property\n", index + 1);
        }

        // check for a port name
        if (!nameVal->IsUndefined())
        {
            // add to the by-name port map
            auto it = portsByName.emplace(nameVal->String(), &port);

            // remember the port name internally
            port.portName = it.first->first.c_str();
        }
        
        // mark flipper logic as enabled if there's a non-zero time limit
        port.flipperLogic.enabled = (port.flipperLogic.dtHighPowerMax != 0);

        // delegate settings to the device as appropriate to the device type
        device->ApplyPortSettings(&port);

        // log the settings
        char fullNameBuf[64];
        Log(LOG_CONFIG, "Output[%d]: device %s, %s%stime limit %u, reduced power %d, cooling %d\n",
            index + 1, device->FullName(fullNameBuf, sizeof(fullNameBuf)),
            port.device->gamma ? "gamma on, " : "", port.device->inverted ? "inverted, " : "",
            port.flipperLogic.dtHighPowerMax, port.flipperLogic.reducedPowerLevel, port.flipperLogic.dtCooling);
    });

    // Second pass: parse 'source' definitions.  We do this as a second
    // pass because one port's source string can reference other ports.
    // Completing the initial port setup scan first lets us check the
    // validity of cross-references during parsing
    json.Get("outputs")->ForEach([](int index, const JSONParser::Value *value)
    {
        // check for a data source
        if (auto *srcSpec = value->Get("source"); !srcSpec->IsUndefined())
        {
            // parse the source string into a DataSource object
            std::string srcStr = srcSpec->String();
            const char *start = srcStr.c_str(), *p = start, *end = start + srcStr.size();
            DataSource *source = ParseSource(index, p, start, end);

            // make sure there's no extraneous text at the end
            if (source != nullptr)
            {
                // skip trailing spaces
                for ( ; p < end && isspace(*p) ; ++p);
                if (p < end)
                {
                    int len = end - p;
                    int col = p - start;
                    Log(LOG_ERROR, "outputs[%d].source, col %d: extraneous text at end, starting at \"%.*s%s\"\n",
                        index, col, len > 16 ? 16 : len, len > 16 ? "..." : "");
                }

                // Log a warning if the data source doesn't yield a
                // UINT8 value.  Some data sources yield float, RGB, or
                // vector values, which are meant to be used as inputs
                // for other functions, not as final port values.  The
                // port level is always a UINT8, so the result of the
                // calculated source expression should be a UINT8 to
                // ensure that it's in the right range.  The other types
                // can all be implicitly converted to UINT8, so it's not
                // a hard error if the result is another type, but the
                // conversions might not be what the user expected, so
                // a warning might be helpful for troubleshooting.
                if (auto t = source->Calc().type; t != SourceVal::Type::UInt8)
                    Log(LOG_WARNING, "outputs[%d].source: %s result will be clipped to 0..255 range\n", index, SourceVal::TypeName(t));
            }

            // set the source in the port
            portsByNumber[index + 1]->source = source;
        }
    });

    // Go through all of the source expressions, and check for circular
    // references.  A circular reference is a port expression that
    // refers to a port whose source refers back to the original port,
    // directly or indirectly.  Break these cycles by changing the first
    // referencing port expression to a "raw" evaluation, which doesn't
    // invoke the other port's computed value.
    int index = 1;
    for (auto &port : portList)
    {
        if (port.source != nullptr)
        {
            // set up a path starting with the current port
            std::list<Port*> path;
            path.emplace_back(&port);

            // Traverse the tree, looking for cycles
            DataSource::TraverseFunc Check = [&Check, index, &path, &port](DataSource *s)
            {
                if (s != nullptr)
                {
                    // if this is a calculated port reference, check for cycles
                    if (auto *ps = s->AsPortSource() ; ps != nullptr && !ps->raw && ps->port != nullptr && ps->port->source != nullptr)
                    {
                        // If this refers back to anything in the path so far,
                        // far, we've found a circular reference.
                        if (auto it = std::find(path.begin(), path.end(), ps->port) ; it != path.end())
                        {
                            // It's a circular reference, so evaluating
                            // this expression would trigger an infinite
                            // recursion.  Remove the source expression
                            // in the originating node.
                            port.source = nullptr;
                            Log(LOG_ERROR, "outputs[%d].source: circular reference detected; source disabled for this session\n", index);

                            // no further recursive checks are needed on
                            // this port, since it's no longer a port
                            // cross-reference
                            return;
                        }

                        // traverse the port's sub-expression with this
                        // new port added to the path
                        path.emplace_back(ps->port);
                        ps->Traverse(Check);
                        path.pop_back();
                    }
                    else
                    {
                        // not a port node - traverse its sub-tree
                        s->Traverse(Check);
                    }
                }
            };
            Check(port.source);
        }

        // increment the slot index
        ++index;
    }

    // Check for empty share group pools.  An empty pool isn't necessarily
    // an error, but it's PROBABLY an error - it probably just means that
    // there's a naming mismatch between the devices and the pool.
    for (const auto &s : shareGroups)
    {
        if (s.second.GetPoolSize() == 0)
        {
            Log(LOG_WARNING, "Share group \"%s\" has no physical output ports assigned; "
                "check type:\"shareGroup\" devices in the outputs list for typos in their "
                "group: settings\n",
                s.first.c_str());
        }
    }

    // configuration completed
    isConfigured = true;

    // invoke any deferred post-configuration callbacks
    while (postConfigCallbacks.size() != 0)
    {
        // invoke and discard the first callback
        postConfigCallbacks.front()(json);
        postConfigCallbacks.pop_front();
    }
}

// parse an output port device definition
OutputManager::Device *OutputManager::ParseDevice(
    const char *jsonLocus, const char *pinoutLabel,
    JSONParser &json, const JSONParser::Value *devSpec,
    bool pwmDefault, bool allowBareGPIO)
{
    // check for a bare GPIO number, if allowed
    if (allowBareGPIO && devSpec->IsNumber())
        return CreateGPIODev(jsonLocus, pinoutLabel, devSpec->Int(-1), pwmDefault, -1);

    // no device yet
    Device *device = nullptr;
    
    // parse the type
    auto *devType = devSpec->Get("type");
    if (*devType == "tlc59116")
    {
        // TCL59116 port
        int port = devSpec->Get("port")->Int(-1);
        TLC59116 *chip = TLC59116::GetChip(devSpec->Get("chip")->Int(-1));
        if (chip != nullptr && chip->IsValidPort(port))
            device = new TLC59116Dev(chip, port);
        else
            Log(LOG_ERROR, "%s: Invalid TLC59116 chip/port spec\n", jsonLocus);
    }
    else if (*devType == "tlc5940")
    {
        // TLC5940 port
        int port = devSpec->Get("port")->Int(-1);
        TLC5940 *chain = TLC5940::GetChain(devSpec->Get("chain")->Int(0));

        // validate the chain
        if (chain == nullptr)
            Log(LOG_ERROR, "%s: No such TLC5940 chain\n", jsonLocus);

        // if a chip index is provided, adjust the port to be chip-relative
        if (const auto *chipVal = devSpec->Get("chip"); !chipVal->IsUndefined())
        {
            // validate the chip number
            int chip = chipVal->Int(-1);
            if (chip < 0 || (chain != nullptr && !chain->IsValidPort(chip*16 + 15)))
                Log(LOG_ERROR, "%s: TLC5940 'chip' index %d is out of range for this chain\n", jsonLocus, chip);

            // the port number has to be 0..15 when chip-relative
            if (port < 0 || port > 15)
                Log(LOG_ERROR, "%s: TLC5940 'port' %d is out of range; must be 0-15\n", jsonLocus, port);

            // figure the port index
            port += chip * 16;
        }
        else
        {
            // the port is chain-relative - validate it with the chain object
            if (chain != nullptr && !chain->IsValidPort(port))
                Log(LOG_ERROR, "%s: TLC5940 port %d is out of range\n", jsonLocus, port);
        }

        // if the chain and port are valid, create the device object
        if (chain != nullptr && chain->IsValidPort(port))
            device = new TLC5940Dev(chain, port);
    }
    else if (*devType == "tlc5947")
    {
        // TLC5947 port
        int port = devSpec->Get("port")->Int(-1);
        TLC5947 *chain = TLC5947::GetChain(devSpec->Get("chain")->Int(0));

        // validate the chain
        if (chain == nullptr)
            Log(LOG_ERROR, "%s: No such TLC5947 chain\n", jsonLocus);

        // if a chip index is provided, adjust the port to be chip-relative
        if (const auto *chipVal = devSpec->Get("chip"); !chipVal->IsUndefined())
        {
            // validate the chip number
            int chip = chipVal->Int(-1);
            if (chip < 0 || (chain != nullptr && !chain->IsValidPort(chip*24 + 23)))
                Log(LOG_ERROR, "%s: TLC5947 'chip' index %d is out of range for this chain\n", jsonLocus, chip);

            // the port number has to be 0..23 when chip-relative
            if (port < 0 || port > 23)
                Log(LOG_ERROR, "%s: TLC5947 'port' %d is out of range; must be 0-23\n", jsonLocus, port);

            // figure the port index
            port += chip * 24;
        }
        else
        {
            // the port is chain-relative - validate it with the chain object
            if (chain != nullptr && !chain->IsValidPort(port))
                Log(LOG_ERROR, "%s: TLC5947 port %d is out of range\n", jsonLocus, port);
        }

        // if the chain and port are valid, create the device object
        if (chain != nullptr && chain->IsValidPort(port))
            device = new TLC5947Dev(chain, port);
    }
    else if (*devType == "pca9685")
    {
        // PCA9685 port
        int port = devSpec->Get("port")->Int(-1);
        PCA9685 *chip = PCA9685::GetChip(devSpec->Get("chip")->Int(-1));
        if (chip != nullptr && chip->IsValidPort(port))
            device = new PCA9685Dev(chip, port);
        else
            Log(LOG_ERROR, "%s: Invalid PCA9685 chip/port spec\n", jsonLocus);
    }
    else if (*devType == "pca9555")
    {
        // PCA9555 port

        // get the chip
        int chipNum = devSpec->Get("chip")->Int(-1);
        PCA9555 *chip = PCA9555::Get(chipNum);
        if (chip == nullptr)
            Log(LOG_ERROR, "%s: Invalid PCA9555 chip number %d\n", jsonLocus, chipNum);

        // get the port name string
        int port;
        if (const auto *portVal = devSpec->Get("port"); portVal->IsString())
        {
            // parse the string, using either the NXP format ("IOn_m") or the
            // TI format (simply "nm"), or anything in between.
            std::string s = portVal->String();
            const char *p = s.c_str();
            int bank = -1;
            if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'o' || p[1] == 'O')) p += 2;
            if (*p == '0' || *p == '1') bank = (*p++ - '0') * 8;
            if (*p == '_') ++p;
            if (*p >= '0' && *p <= '7') port = bank + (*p++) - '0';

            if (*p != 0 || bank == -1 || port == -1)
            {
                Log(LOG_ERROR, "%s: invalid PCA9555 port name syntax \"%s\", "
                    "expected IO0_n or IO1_n, n=0-7\n", jsonLocus, s.c_str());
            }
        }
        else
        {
            Log(LOG_ERROR, "%s: invalid PCA9555 'port' property; must a port name string\n", jsonLocus);
        }

        // if the chip and port are valid, create the device object
        if (chip != nullptr && port >= 0 && port <= 15)
            device = new PCA9555Dev(chip, port, jsonLocus);
    }
    else if (*devType == "74hc595")
    {
        // 74HC595 shift register port

        // get the chain object
        C74HC595 *chain = C74HC595::GetChain(devSpec->Get("chain")->Int(0));
        if (chain == nullptr)
            Log(LOG_ERROR, "%s: Invalid 74HC595 chain number\n", jsonLocus);

        // get the chip number
        int chipNum = devSpec->Get("chip")->Int(-1);
        if (chipNum == -1 || (chain != nullptr && !chain->IsValidPort(chipNum*8 + 7)))
            Log(LOG_ERROR, "%s: Invalid or missing 74HC595 chip index %d\n", jsonLocus, chipNum);

        // get the port number
        int port = -1;
        if (auto *portVal = devSpec->Get("port") ; portVal->IsNumber())
        {
            // port number, 0-7
            port = portVal->Int(-1);
        }
        else if (portVal->IsString())
        {
            // port name, A-H or QA-QH
            std::string s = portVal->String();
            const char *p = s.c_str();

            // skip the optional 'Q' if present
            if (*p == 'q' || *p == 'Q')
                ++p;

            // get the port letter or number
            char c = toupper(*p++);
            if (c >= 'A' && c <= 'H' && *p == 0)
                port = c - 'A';
            else if (c >= '0' && c <= '7' && *p == 0)
                port = c - '0';

            // make sure we found a valid port number
            if (port < 0)
                Log(LOG_ERROR, "%s: Invalid 74HC595 port name \"%s\", expected A-H, QA-QH, 0-7, or Q0-Q7\n", jsonLocus, s.c_str());
        }

        // get the port number relative to the overall chain
        if (port >= 0)
            port += chipNum * 8;

        // if the chain and port are valid, set up the device
        if (chain != nullptr && chain->IsValidPort(port))   
            device = new C74HC595Dev(chain, port);
    }
    else if (*devType == "gpio")
    {
        // GPIO port
        int gp = devSpec->Get("gp")->Int(-1);
        bool pwm = devSpec->Get("pwm")->Bool(pwmDefault);
        int freq = devSpec->Get("freq")->Int(-1);
        device = CreateGPIODev(jsonLocus, pinoutLabel, gp, pwm, freq);
    }
    else if (*devType == "workerPico")
    {
        // external worker Pico, running the PWMWorker firmware
        int unit = devSpec->Get("unit")->Int(-1);
        int port = devSpec->Get("port")->Int(-1);

        // validate the unit
        PWMWorker *worker = PWMWorker::GetUnit(unit);
        if (worker == nullptr)
            Log(LOG_ERROR, "%s: Invalid workerPico unit number %d\n", jsonLocus, unit);

        // validate the port
        if (worker->IsValidPort(port))
            device = new PWMWorkerDev(worker, port);
        else
            Log(LOG_ERROR, "%s: Invalid workPico port number %d\n", jsonLocus, port);
    }
    else if (*devType == "zblaunch")
    {
        // ZB Launch control
        device = new ZBLaunchDev();
    }
    else if (*devType == "shareGroup")
    {
        // Share group device
        ShareGroupDev *shareGroupDev = new ShareGroupDev();
        device = shareGroupDev;
        if (auto *groupVal = devSpec->Get("group") ; !groupVal->IsUndefined())
        {
            groupVal->ForEach([shareGroupDev](int index, const JSONParser::Value *val) {
                shareGroupDev->AddGroup(ShareGroupContainer::FindOrCreate(val->String().c_str()));
            }, true);
        }

        // warn if the share group list is empty
        if (shareGroupDev->GetNGroups() == 0)
            Log(LOG_WARNING, "%s: share-group device 'group' property is missing or empty\n", jsonLocus);

        // check for pulse mode
        if (auto *pulseVal = devSpec->Get("pulseMode") ; !pulseVal->IsUndefined())
        {
            // Note the ON and OFF times; they're expressed in the property values
            // in milliseconds, but we store them internally in microseconds.
            shareGroupDev->SetPulseMode(
                pulseVal->Get("tOn")->Int(0) * 1000UL,
                pulseVal->Get("tOff")->Int(0) * 1000UL);
        }
    }
    else if (*devType == "virtual")
    {
        // virtual port with no physical device attached - use a null device
        device = new NullDev();
    }
    else
    {
        // unknown device type
        Log(LOG_ERROR, "%s: Invalid device type \"%s\"\n", jsonLocus, devType->String().c_str());
    }

    // if there's a device, complete its configuration
    if (device != nullptr)
        device->Configure(devSpec);

    // return the result (which may be null)
    return device;
}

// set up a GPIO output device
OutputManager::GPIODev *OutputManager::CreateGPIODev(const char *jsonLocus, const char *pinoutLabel, int gp, bool pwm, int freq)
{
    if (!IsValidGP(gp))
    {
        Log(LOG_ERROR, "%s: Invalid GPIO port number\n", jsonLocus);
    }
    else if (!gpioManager.Claim(stringPool.Add(jsonLocus), pinoutLabel, gp))
    {
        /* error already logged */
    }
    else if (pwm)
    {
        // create a PWM output device
        return new PWMGPIODev(gp, freq);
    }
    else
    {
        // not PWM, so create a plain digital GPIO output device
        return new DigitalGPIODev(gp);
    }

    // no device created
    return nullptr;
}


// schedule a callback for invocation after configuration is completed
void OutputManager::AfterConfigure(JSONParser &json, std::function<void(JSONParser &json)> func)
{
    // check if the output manager has been configured yet
    if (isConfigured)
    {
        // yes - we can invoke the callback immediately
        func(json);
    }
    else
    {
        // no - defer it until we complete configuration
        postConfigCallbacks.push_back(func);
    }
}

// Run periodic tasks
void OutputManager::Task()
{
    // Check suspend mode.  If we're in normal mode (not suspend mode),
    // run port tasks.  Otherwise, skip port tasks, so that the host can
    // directly address the underlying physical output devices without
    // any inteference from the output manager.  If we are in suspend
    // mode, check for mode timeout expiration.
    if (__builtin_expect(!isSuspended, true))
    {
        // normal mode - update all ports

        // take a nudge device snapshot, if applicable
        if (outputNudgeView != nullptr)
            outputNudgeView->TakeSnapshot();

        // run all port tasks
        for (auto &port : portList)
            port.Task();
    }
    else if (time_us_64() >= suspendModeEndTime)
    {
        // suspend mode, timeout expired - cancel suspend mode
        isSuspended = false;
    }
}

// Set all port levels to fully OFF (logical PWM level 0)
void OutputManager::AllOff()
{
    for (auto &port : portList)
        port.SetDOFLevel(0);
}

// Enable/disable physical outputs across all peripherals.  This asserts
// or removes the "Output Enable" or equivalent control for all
// peripherals that have such controls.  The exact meaning of "Output
// Enable" varies by chip, but in most cases, a disabled output is
// placed into a high-impedance state that effectively disconnects the
// device the port controls.
void OutputManager::EnablePhysicalOutputs(bool enable)
{
    // pass the request along to all of the peripheral types
    C74HC595::EnableOutputs(enable);
    PCA9555::EnableOutputs(enable);
    PCA9685::EnableOutputs(enable);
    PWMWorker::EnableOutputs(enable);
}

// Suspend output management if it's current active, logging a warning
// message to the console
void OutputManager::SuspendIfActive(const ConsoleCommandContext *ctx, uint32_t timeout_ms)
{
    // only proceed if we're not already suspended
    if (!isSuspended)
    {
        // suspend output management
        Suspend(timeout_ms);

        // note the change on the console
        ctx->Print(
            "Output management is now suspended, to allow device-level output testing.\n"
            "Use \033[37;1mout --resume\033[0m to resume.\n"
            "\n"
            "\033[33;1mWARNING: Flipper Logic is disabled! Use caution with affected devices.\033[0m\n"
            "\n");
    }
}

// Suspend output manager operation
void OutputManager::Suspend(uint32_t timeout_ms)
{
    // if not already suspended, turn off all ports
    if (!isSuspended)
    {
        // turn off all logical ports
        AllOff();
        
        // Set all port devices to logical and physical OFF.  We set the
        // logical level because that resets internal timers and flags based
        // on the logical level, such as the flipper-logic state.  We also
        // explicitly set the physical device level, to be sure that we've
        // bypassed all other dependency logic that might get introduced in
        // the future between logical and physical output levels.
        for (auto &port : portList)
        {
            port.SetLogicalLevel(0);
            port.SetDeviceOff();
        }
    }

    // set the suspend flag - this blocks the Task() routine
    isSuspended = true;

    // Set the timeout end time.
    //
    // 0 means "suspend indefinitely".  We can set the end time to
    // practical infinity by using the larger 64-bit timer value, which
    // represents about 585,000 years from the Pico system reset time.
    suspendModeEndTime = (timeout_ms == 0) ? UINT64_MAX :
                         time_us_64() + (static_cast<uint64_t>(timeout_ms) * 1000);
}

// Resume suspended operation
void OutputManager::Resume()
{
    // Clear the suspend flag.  Note that this is sufficient to restore
    // normal operation, since this allows the Task() routine to resume
    // applying updates, so all of the ports will sync with their host
    // and/or calculated levels on the next main loop cycle.
    isSuspended = false;
}

// Set a port PWM level on a physical device.  This is used for direct
// access to the physical devices from outside the output manager, such
// as from the USB Vendor Interface.  This is intended for testing,
// debugging, and troubleshooting purposes.   The Output Manager doesn't
// use this for its own access to device ports - it calls directly into
// the various device drivers.
void OutputManager::SetDevicePortLevel(int devType, int configIndex, int port, int pwmLevel)
{
    switch (devType)
    {
    case PinscapePico::OutputPortDesc::DEV_GPIO:
        GPIODev::SetDevicePortLevel(port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_PWMWORKER:
        PWMWorkerDev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_TLC59116:
        TLC59116Dev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_TLC5940:
        TLC5940Dev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_TLC5947:
        TLC5947Dev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_PCA9685:
        PCA9685Dev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_PCA9555:
        PCA9555Dev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;

    case PinscapePico::OutputPortDesc::DEV_74HC595:
        C74HC595Dev::SetDevicePortLevel(configIndex, port, pwmLevel);
        break;
    }
}

// Query logical output port descriptors, for the USB Vendor Interface
size_t OutputManager::QueryLogicalPortDescs(uint8_t *buf, size_t bufSize)
{
    // Figure the buffer size required.  The return data consists of a
    // PinscapePico::OutputPortList struct as a list header, followed by
    // one PinscapePico::OutputPortDesc struct per configured logical
    // output port.
    using PortList = PinscapePico::OutputPortList;
    using PortDesc = PinscapePico::OutputPortDesc;
    size_t replySize = sizeof(PortList) + (portList.size() * sizeof(PortDesc));

    // make sure there's room; return failure (0) if not
    if (replySize > bufSize)
        return 0;

    // Clear the whole reply buffer to zeroes, for any bytes we don't
    // explicitly fill in with field values.  This facilitates two-way
    // compatibility with future updates by ensuring that all bytes in
    // the buffer have well-defined values, even if reserved or unused
    // in the current version.
    memset(buf, 0, replySize);

    // populate the header struct
    PortList *hdr = reinterpret_cast<PortList*>(buf);
    hdr->cb = sizeof(PortList);
    hdr->cbDesc = sizeof(PortDesc);
    hdr->numDescs = static_cast<uint16_t>(portList.size());

    // populate the port descriptor list
    PortDesc *pp = reinterpret_cast<PortDesc*>(hdr + 1);
    for (const auto &port : portList)
    {
        // set flags
        pp->flags = 0;
        if (port.noisy) pp->flags |= PortDesc::F_NOISY;
        if (port.device->gamma) pp->flags |= PortDesc::F_GAMMA;
        if (port.device->inverted) pp->flags |= PortDesc::F_INVERTED;
        if (port.flipperLogic.enabled) pp->flags |= PortDesc::F_FLIPPERLOGIC;
        if (port.source != nullptr) pp->flags |= PortDesc::F_COMPUTED;
        if (port.portName != nullptr) pp->flags |= PortDesc::F_NAMED;

        // set the device information
        port.device->Populate(pp);

        // on to the next port
        ++pp;
    }

    // success - return the populated size
    return replySize;
}

size_t OutputManager::QueryLogicalPortName(uint8_t *buf, size_t bufSize, int portNum)
{
    // get the port; if the port number is invalid, return 0 to indicate an error
    Port *port = Get(portNum);
    if (port == nullptr)
        return 0;

    // get the required size, including the terminating null byte
    const char *name = port->portName != nullptr ? port->portName : "";
    size_t replySize = strlen(name) + 1;

    // make sure there's room; if not, return 0 to indicate an error
    if (replySize > bufSize)
        return 0;

    // copy the name, including the null byte
    memcpy(buf, name, replySize);

    // success - return the reply size
    return replySize;
}


// Visit all of the physical device types, calling a static function on each
#define DoForEachDeviceType(func, method, binop, ...) \
    func(GPIODev::method(__VA_ARGS__)) binop \
    func(TLC59116::method(__VA_ARGS__)) binop \
    func(TLC5940::method(__VA_ARGS__)) binop \
    func(TLC5947::method(__VA_ARGS__)) binop \
    func(PCA9685::method(__VA_ARGS__)) binop \
    func(PCA9555::method(__VA_ARGS__)) binop \
    func(C74HC595::method(__VA_ARGS__)) binop \
    func(PWMWorker::method(__VA_ARGS__))

#define SumOverDeviceTypes(func, method) DoForEachDeviceType(func, method, +)
#define binOpComma ,
#define ForEachDeviceType(method, ...) DoForEachDeviceType(, method, binOpComma, __VA_ARGS__)

// Query physical output device descriptors, for the USB Vendor Interface
size_t OutputManager::QueryDeviceDescs(uint8_t *buf, size_t bufSize)
{
    // Figure the size required: OutputDevList header + one OutputDevDesc
    // per device
    using DevList = PinscapePico::OutputDevList;
    using DevDesc = PinscapePico::OutputDevDesc;
    int numDevices = SumOverDeviceTypes(, CountConfigurations);
    size_t replySize = sizeof(DevList) + (numDevices * sizeof(DevDesc));

    // make sure there's room
    if (replySize > bufSize)
        return 0;

    // zero the buffer, to set predictable defaults for fields we don't
    // otherwise set
    memset(buf, 0, replySize);

    // build the header
    DevList *hdr = reinterpret_cast<DevList*>(buf);
    hdr->cb = sizeof(DevList);
    hdr->cbDesc = sizeof(DevDesc);
    hdr->numDescs = numDevices;

    // populate the device descriptors
    DevDesc *pdesc = reinterpret_cast<DevDesc*>(hdr + 1);
    ForEachDeviceType(PopulateDescs, pdesc);

    // success - return the populated size
    return replySize;
}

// Query physical output device port descriptors, for the USB Vendor Interface
size_t OutputManager::QueryDevicePortDescs(uint8_t *buf, size_t bufSize)
{
    // Figure the size required: OutputDevPortList header + one OutputDevPortDesc
    // per device port
    using List = PinscapePico::OutputDevPortList;
    using Desc = PinscapePico::OutputDevPortDesc;
    int numPorts = SumOverDeviceTypes(, CountPorts);
    size_t replySize = sizeof(List) + (numPorts * sizeof(Desc));

    // make sure there's room
    if (replySize > bufSize)
        return 0;

    // zero the buffer, to set predictable defaults for fields we don't
    // otherwise set
    memset(buf, 0, replySize);

    // build the header
    List *hdr = reinterpret_cast<List*>(buf);
    hdr->cb = sizeof(List);
    hdr->numDescs = numPorts;
    hdr->cbDesc = sizeof(Desc);

    // populate the device port descriptors
    Desc *pdesc = reinterpret_cast<Desc*>(hdr + 1);
    ForEachDeviceType(PopulateDescs, pdesc);

    // success - return the populated size
    return replySize;
}

size_t OutputManager::QueryLogicalPortLevels(uint8_t *buf, size_t bufSize)
{
    // figure the size required: the OutputLevelList header, then
    // one OutputLevel per configured logical port.
    using LevelList = PinscapePico::OutputLevelList;
    using Level = PinscapePico::OutputLevel;
    size_t replySize = sizeof(LevelList) + (portList.size() * sizeof(Level));

    // make sure there's room; return failure (0) if not
    if (replySize > bufSize)
        return 0;

    // zero the buffer, to set well-defined defaults for unset fields
    memset(buf, 0, replySize);

    // set up the list header
    LevelList *hdr = reinterpret_cast<LevelList*>(buf);
    hdr->cb = sizeof(LevelList);
    hdr->cbLevel = sizeof(Level);
    hdr->numLevels = static_cast<uint16_t>(portList.size());

    // set flags
    if (isSuspended) hdr->flags |= LevelList::F_TEST_MODE;

    // populate the port levels
    Level *pl = reinterpret_cast<Level*>(hdr + 1);
    for (auto &port : portList)
    {
        pl->dofLevel = port.GetDOFLevel();
        pl->calcLevel = port.Get();
        pl->outLevel = port.GetOutLevel();
        pl->lwState = (port.lw.period & pl->LWSTATE_PERIOD_MASK);
        if (port.lw.mode) pl->lwState |= pl->LWSTATE_MODE;
        if (port.lw.on) pl->lwState |= pl->LWSTATE_ON;
        pl->lwProfile = port.lw.profile;
        ++pl;
    }

    // success - return the populated size
    return replySize;
}

size_t OutputManager::QueryDevicePortLevels(uint8_t *buf, size_t bufSize)
{
    // figure the size required: the OutputLevelList header, then
    // one OutputLevel per device port across all devices.
    int numPorts = SumOverDeviceTypes(, CountPorts);
    using Header = PinscapePico::OutputDevLevelList;
    using Level = PinscapePico::OutputDevLevel;
    size_t replySize = sizeof(Header) + (numPorts * sizeof(Level));

    // make sure there's room; return failure (0) if not
    if (replySize > bufSize)
        return 0;

    // zero the buffer, to set well-defined defaults for unset fields
    memset(buf, 0, replySize);

    // set up the header
    Header *hdr = reinterpret_cast<Header*>(buf);
    hdr->cb = sizeof(Header);
    hdr->cbLevel = sizeof(Level);
    hdr->numLevels = numPorts;

    // populate the port levels
    Level *pl = reinterpret_cast<Level*>(hdr + 1);
    ForEachDeviceType(PopulateLevels, pl);

    // success - return the populated size
    return replySize;
}


// --------------------------------------------------------------------------
//
// Logical output port
//

// set the host DOF port level
void OutputManager::Port::SetDOFLevel(uint8_t newLevel)
{
    // shareGroup ports only accept commands from their share group
    if (shareGroups.size() != 0)
        return;

    // remember the new DOF port level
    dofLevel = newLevel;

    // this replaces any LedWiz state
    lw.mode = false;
    
    // If the port doesn't have a data source, and the output manager
    // isn't suspended, apply it as the logical level
    if (source == nullptr && !isSuspended)
        SetLogicalLevel(newLevel);
}

// set the share group level
void OutputManager::Port::SetShareGroupLevel(uint8_t newLevel)
{
    // assign this as the DOF level
    dofLevel = newLevel;
    lw.mode = false;

    // If the port doesn't have a data source, and the output manager
    // isn't suspended, apply it as the logical level
    if (source == nullptr && !isSuspended)
        SetLogicalLevel(newLevel);
}

// set the LedWiz SBA on/off state
void OutputManager::Port::SetLedWizSBA(bool on, uint8_t period)
{
    // shareGroup ports only accept commands from their share group
    if (shareGroups.size() != 0)
        return;
    
    // remember the new SBA state and period, and set LedWiz mode for the port
    lw.on = on;
    lw.period = period < 1 ? 1 : period > 7 ? 7 : period;  // force to 1..7 valid range
    lw.mode = true;

    // update the logical state
    if (source == nullptr && !isSuspended)
        SetLogicalLevel(lw.GetLiveLogLevel());
}

// set the LedWiz PBA brightness level/waveform state
void OutputManager::Port::SetLedWizPBA(uint8_t profile)
{
    // shareGroup ports only accept commands from their share group
    if (shareGroups.size() != 0)
        return;

    // remember the new profile state, and set LedWiz mode for the port
    lw.profile = profile;
    lw.mode = true;

    // update the logical state
    if (source == nullptr && !isSuspended)
        SetLogicalLevel(lw.GetLiveLogLevel());
}

// set the logical port level
void OutputManager::Port::SetLogicalLevel(uint8_t newLevel)
{
    // Update the flipper logic state, if enabled
    if (flipperLogic.enabled)
    {
        switch (flipperLogic.state)
        {
        case FlipperLogic::State::Ready:
            // switch to Armed state if the new level is above the low-power limit
            if (newLevel > flipperLogic.reducedPowerLevel)
            {
                flipperLogic.state = FlipperLogic::State::Armed;
                flipperLogic.tHighPowerCutoff = time_us_64() + flipperLogic.dtHighPowerMax;
            }
            break;
            
        case FlipperLogic::State::Armed:
        case FlipperLogic::State::Triggered:
            // Switch to Ready state if the new level is zero.  Note that switching
            // to low power doesn't clear a triggered condition; the port must be
            // switched completely off.  This also starts the cooling-off period.
            if (newLevel == 0)
            {
                flipperLogic.state = FlipperLogic::State::Ready;
                flipperLogic.tCoolingEnd = time_us_64() + flipperLogic.dtCooling;
            }
            break;
        }
    }
    
    // remember the new logical level
    logLevel = newLevel;

    // apply the change to the underlying physical/virtual output
    Apply();
}

uint8_t OutputManager::Port::LedWizState::GetLiveLogLevel() const
{
    // if the mode isn't active, just report 0 for fully off
    if (!mode)
        return 0;

    // if the SBA state is OFF, report level 0
    if (!on)
        return 0;

    // Translation table for LedWiz PWM levels, which are expressed
    // as increments of 1/48, for values 0..48.  Values above 48 are
    // invalid per the official LedWiz API documentation, but the
    // genuine LedWiz also happens to accept 49 as equivalent to 48,
    // so we'll do the same for the sake of compatibility with buggy
    // clients.  (Which are known to exist, which is how we learned
    // of this firmware bug in the first place.)
    static const uint8_t lw_to_dof[] = {
          0,   5,  11,  16,  21,  27,  32,  37,  43,  48,  53,  58,  64,  69,  74,  80,
         85,  90,  96, 101, 106, 112, 117, 122, 128, 133, 138, 143, 149, 154, 159, 165,
        170, 175, 181, 186, 191, 197, 202, 207, 213, 218, 223, 228, 234, 239, 244, 250, 
        255, 255
    };
    
    // sawtooth pattern (profile 129)
    static const uint8_t sawtooth[] = {
        0x01, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d, 0x0f, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f,
        0x21, 0x23, 0x25, 0x27, 0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35, 0x37, 0x39, 0x3b, 0x3d, 0x3f,
        0x41, 0x43, 0x45, 0x47, 0x49, 0x4b, 0x4d, 0x4f, 0x51, 0x53, 0x55, 0x57, 0x59, 0x5b, 0x5d, 0x5f,
        0x61, 0x63, 0x65, 0x67, 0x69, 0x6b, 0x6d, 0x6f, 0x71, 0x73, 0x75, 0x77, 0x79, 0x7b, 0x7d, 0x7f,
        0x81, 0x83, 0x85, 0x87, 0x89, 0x8b, 0x8d, 0x8f, 0x91, 0x93, 0x95, 0x97, 0x99, 0x9b, 0x9d, 0x9f,
        0xa1, 0xa3, 0xa5, 0xa7, 0xa9, 0xab, 0xad, 0xaf, 0xb1, 0xb3, 0xb5, 0xb7, 0xb9, 0xbb, 0xbd, 0xbf,
        0xc1, 0xc3, 0xc5, 0xc7, 0xc9, 0xcb, 0xcd, 0xcf, 0xd1, 0xd3, 0xd5, 0xd7, 0xd9, 0xdb, 0xdd, 0xdf,
        0xe1, 0xe3, 0xe5, 0xe7, 0xe9, 0xeb, 0xed, 0xef, 0xf1, 0xf3, 0xf5, 0xf7, 0xf9, 0xfb, 0xfd, 0xff,
        0xfe, 0xfc, 0xfa, 0xf8, 0xf6, 0xf4, 0xf2, 0xf0, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe2, 0xe0,
        0xde, 0xdc, 0xda, 0xd8, 0xd6, 0xd4, 0xd2, 0xd0, 0xce, 0xcc, 0xca, 0xc8, 0xc6, 0xc4, 0xc2, 0xc0,
        0xbe, 0xbc, 0xba, 0xb8, 0xb6, 0xb4, 0xb2, 0xb0, 0xae, 0xac, 0xaa, 0xa8, 0xa6, 0xa4, 0xa2, 0xa0,
        0x9e, 0x9c, 0x9a, 0x98, 0x96, 0x94, 0x92, 0x90, 0x8e, 0x8c, 0x8a, 0x88, 0x86, 0x84, 0x82, 0x80,
        0x7e, 0x7c, 0x7a, 0x78, 0x76, 0x74, 0x72, 0x70, 0x6e, 0x6c, 0x6a, 0x68, 0x66, 0x64, 0x62, 0x60,
        0x5e, 0x5c, 0x5a, 0x58, 0x56, 0x54, 0x52, 0x50, 0x4e, 0x4c, 0x4a, 0x48, 0x46, 0x44, 0x42, 0x40,
        0x3e, 0x3c, 0x3a, 0x38, 0x36, 0x34, 0x32, 0x30, 0x2e, 0x2c, 0x2a, 0x28, 0x26, 0x24, 0x22, 0x20,
        0x1e, 0x1c, 0x1a, 0x18, 0x16, 0x14, 0x12, 0x10, 0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x00,
    };

    // flash on/off pattern (profile 130)
    static const uint8_t flash[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    };

    // on/ramp down pattern (profile 131)
    static const uint8_t onRampDown[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xfe, 0xfc, 0xfa, 0xf8, 0xf6, 0xf4, 0xf2, 0xf0, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe2, 0xe0,
        0xde, 0xdc, 0xda, 0xd8, 0xd6, 0xd4, 0xd2, 0xd0, 0xce, 0xcc, 0xca, 0xc8, 0xc6, 0xc4, 0xc2, 0xc0,
        0xbe, 0xbc, 0xba, 0xb8, 0xb6, 0xb4, 0xb2, 0xb0, 0xae, 0xac, 0xaa, 0xa8, 0xa6, 0xa4, 0xa2, 0xa0,
        0x9e, 0x9c, 0x9a, 0x98, 0x96, 0x94, 0x92, 0x90, 0x8e, 0x8c, 0x8a, 0x88, 0x86, 0x84, 0x82, 0x80,
        0x7e, 0x7c, 0x7a, 0x78, 0x76, 0x74, 0x72, 0x70, 0x6e, 0x6c, 0x6a, 0x68, 0x66, 0x64, 0x62, 0x60,
        0x5e, 0x5c, 0x5a, 0x58, 0x56, 0x54, 0x52, 0x50, 0x4e, 0x4c, 0x4a, 0x48, 0x46, 0x44, 0x42, 0x40,
        0x3e, 0x3c, 0x3a, 0x38, 0x36, 0x34, 0x32, 0x30, 0x2e, 0x2c, 0x2a, 0x28, 0x26, 0x24, 0x22, 0x20,
        0x1e, 0x1c, 0x1a, 0x18, 0x16, 0x14, 0x12, 0x10, 0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x00,
    };

    // ramp up/on pattern (profile 132)
    static const uint8_t rampUpOn[] = {
        0x00, 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e,
        0x20, 0x22, 0x24, 0x26, 0x28, 0x2a, 0x2c, 0x2e, 0x30, 0x32, 0x34, 0x36, 0x38, 0x3a, 0x3c, 0x3e,
        0x40, 0x42, 0x44, 0x46, 0x48, 0x4a, 0x4c, 0x4e, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5a, 0x5c, 0x5e,
        0x60, 0x62, 0x64, 0x66, 0x68, 0x6a, 0x6c, 0x6e, 0x70, 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7c, 0x7e,
        0x80, 0x82, 0x84, 0x86, 0x88, 0x8a, 0x8c, 0x8e, 0x90, 0x92, 0x94, 0x96, 0x98, 0x9a, 0x9c, 0x9e,
        0xa0, 0xa2, 0xa4, 0xa6, 0xa8, 0xaa, 0xac, 0xae, 0xb0, 0xb2, 0xb4, 0xb6, 0xb8, 0xba, 0xbc, 0xbe,
        0xc0, 0xc2, 0xc4, 0xc6, 0xc8, 0xca, 0xcc, 0xce, 0xd0, 0xd2, 0xd4, 0xd6, 0xd8, 0xda, 0xdc, 0xde,
        0xe0, 0xe2, 0xe4, 0xe6, 0xe8, 0xea, 0xec, 0xee, 0xf0, 0xf2, 0xf4, 0xf6, 0xf8, 0xfa, 0xfc, 0xfe,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    // The LedWiz speed setting gives the flash period in 0.25s units
    // (speed 1 is a flash period of .25s, speed 7 is a period of 1.75s).
    //
    // What we're after here is the "phase", which is to say the point
    // in the current cycle.  If we assume that the cycle has been running
    // continuously since some arbitrary time zero in the past, we can
    // figure where we are in the current cycle by dividing the time since
    // that zero by the cycle period and taking the remainder.  E.g., if
    // the cycle time is 5 seconds, and the time since t-zero is 17 seconds,
    // we divide 17 by 5 to get a remainder of 2.  That says we're 2 seconds
    // into the current 5-second cycle, or 2/5 of the way through the
    // current cycle.
    //
    // We do this calculation on every iteration of the main loop, so we 
    // want it to be very fast.  To streamline it, we'll use some tricky
    // integer arithmetic.  The result will be the same as the straightforward
    // remainder and fraction calculation we just explained, but we'll get
    // there by less-than-obvious means.
    //
    // Rather than finding the phase as a continuous quantity or floating
    // point number, we'll quantize it.  We'll divide each cycle into 256 
    // time units, or quanta.  Each quantum is 1/256 of the cycle length,
    // so for a 1-second cycle (LedWiz speed 4), each quantum is 1/256 of 
    // a second, or about 3.9ms.  If we express the time since t-zero in
    // these units, the time period of one cycle is exactly 256 units, so
    // we can calculate our point in the cycle by taking the remainder of
    // the time (in our funny units) divided by 256.  The special thing
    // about making the cycle time equal to 256 units is that "x % 256" 
    // is exactly the same as "x & 255", which is a much faster operation
    // than division on ARM M0+: this CPU has no hardware DIVIDE operation,
    // so an integer division takes about 5us.  The bit mask operation, in 
    // contrast, takes only about 60ns - about 100x faster.  5us doesn't
    // sound like much, but we do this on every main loop, so every little
    // bit counts.  
    //
    // The snag is that our system timer gives us the elapsed time in
    // microseconds.  We still need to convert this to our special quanta
    // of 256 units per cycle.  The straightforward way to do that is by
    // dividing by (microseconds per quantum).  E.g., for LedWiz speed 4,
    // we decided that our quantum was 1/256 of a second, or 3906us, so
    // dividing the current system time in microseconds by 3906 will give
    // us the time in our quantum units.  But now we've just substituted
    // one division for another!
    //
    // This is where our really tricky integer math comes in.  Dividing
    // by X is the same as multiplying by 1/X.  In integer math, 1/3906
    // is zero, so that won't work.  But we can get around that by doing
    // the integer math as "fixed point" arithmetic instead.  It's still
    // actually carried out as integer operations, but we'll scale our
    // integers by a scaling factor, then take out the scaling factor
    // later to get the final result.  The scaling factor we'll use is
    // 2^24.  So we're going to calculate (time * 2^24/3906), then divide
    // the result by 2^24 to get the final answer.  I know it seems like 
    // we're substituting one division for another yet again, but this 
    // time's the charm, because dividing by 2^24 is a bit shift operation,
    // which is another single-cycle operation on M0+.  You might also
    // wonder how all these tricks don't cause overflows or underflows
    // or what not.  Well, the multiply by 2^24/3906 will cause an
    // overflow, but we don't care, because the overflow will all be in
    // the high-order bits that we're going to discard in the final 
    // remainder calculation anyway.
    //
    // Each entry in the array below represents 2^24/N for the corresponding
    // LedWiz speed, where N is the number of time quanta per cycle at that
    // speed.  The time quanta are chosen such that 256 quanta add up to 
    // approximately (LedWiz speed setting * 0.25s).
    // 
    // Note that the calculation has an implicit bit mask (result & 0xFF)
    // to get the final result mod 256.  But we don't have to actually
    // do that work because we're using 32-bit ints and a 2^24 fixed
    // point base (X in the narrative above).  The final shift right by
    // 24 bits to divide out the base will leave us with only 8 bits in
    // the result, since we started with 32.
    static const uint32_t inverseUsecPerQuantum[] = { // indexed by LedWiz speed value
        0, 17172, 8590, 5726, 4295, 3436, 2863, 2454
    };

    // check the waveform mode
    switch (profile)
    {
    case 129:
        // sawtooth
        return sawtooth[static_cast<uint8_t>((time_us_64() * inverseUsecPerQuantum[period]) >> 24)];
        break;

    case 130:
        // flash on/off
        return flash[static_cast<uint8_t>((time_us_64() * inverseUsecPerQuantum[period]) >> 24)];
        break;

    case 131:
        // on/ramp down
        return onRampDown[static_cast<uint8_t>((time_us_64() * inverseUsecPerQuantum[period]) >> 24)];
        break;

    case 132:
        // ramp up/on
        return rampUpOn[static_cast<uint8_t>((time_us_64() * inverseUsecPerQuantum[period]) >> 24)];
        break;

    default:
        // 0-48 = PWM duty cycle N/48; 49 = 100%; others are invalid
        return profile <= 49 ? lw_to_dof[profile] : 255;
    }
}

// Run periodic output port tasks
void OutputManager::Port::Task()
{
    // If we have a data source, update it, interpreting the computed
    // value as an 8-bit integer.  Bypass the calculation and turn the
    // port off if USB isn't active (connected and not suspended).
    //
    // Otherwise, if the port is in LedWiz mode, compute the current
    // level from the LedWiz port state.  This might change dynmically,
    // since some LW port states are waveforms.
    if (source != nullptr)
        SetLogicalLevel(usbIfc.IsConnectionActive() ? source->Calc().AsUInt8() : 0);
    else if (lw.mode)
        SetLogicalLevel(lw.GetLiveLogLevel());

    // if flipper logic is armed, check timer expiration
    if (flipperLogic.state == FlipperLogic::State::Armed && time_us_64() >= flipperLogic.tHighPowerCutoff)
        flipperLogic.state = FlipperLogic::State::Triggered;

    // Bring the physical output up to date with any changes to
    // the internal or external state.  Note that this might be
    // necessary even if the flipper logic triggering state isn't
    // changing, because the cooling off timer is handled
    // separately.
    Apply();
}

// Apply the current nominal level to the physical output
void OutputManager::Port::Apply()
{
    // start with the logical level
    uint8_t v = logLevel;
    
    // if this is a noisy output, and Night Mode is activated, disable it
    // entirely (set the effective output level to zero)
    if (noisy && nightModeControl.Get())
        v = 0;

    // Apply flipper logic if triggered.  Flipper logic sets a maximum level
    // for the output, so use the lesser of the current nominal level or the
    // flipper logic limit level.
    uint8_t reduced = flipperLogic.reducedPowerLevel;
    uint64_t now = time_us_64();
    if (v > reduced)
    {
        // attenuate if flipper logic is triggered
        if (flipperLogic.state == FlipperLogic::State::Triggered)
            v = reduced;

        // attenuate during the cooling-off period
        if (now < flipperLogic.tCoolingEnd)
            v = reduced;
    }

    // When the effective (physical) output level transitions from high
    // power to low power, start the cooling-off timer.
    if (v <= reduced && flipperLogic.prvEffectiveLevel > reduced)
        flipperLogic.tCoolingEnd = now + flipperLogic.dtCooling;

    // remember the new effective level for next time
    flipperLogic.prvEffectiveLevel = v;

    // Set the underlying physical or virtual device.  Note that we
    // leave it up to the underlying device handler to apply gamma
    // correction and inversion.  Applying gamma at the device level
    // allows using the full device resolution for the gamma adjustment;
    // for example, TLC5940 and PCA9685 can do the gamma calculation in
    // 12-bit space rather than 8-bit, which preserves more gradations
    // at the low end of the scale, where 8-bit simply maps to zero.
    // Gamma correction is non-linear, so logic inversion must be
    // applied after gamma correction, so delegating gamma to the device
    // handler also forces us to delegate inversion.
    outLevel = v;
    device->Set(v);
}

// set the underlying physical device port to fully OFF
void OutputManager::Port::SetDeviceOff()
{
    // set the underlying device to fully OFF
    device->Set(0);
}

// --------------------------------------------------------------------------
//
// Base device type
//

void OutputManager::Device::Configure(const JSONParser::Value *val)
{
    // get the gamma and inverted logic properties
    gamma = val->Get("gamma")->Bool();
    inverted = val->Get("inverted")->Bool();

    // set the mapping index for the gamma/inverted logic combination
    mappingIndex = 0;
    if (inverted) mappingIndex |= 0x01;
    if (gamma) mappingIndex |= 0x02;
}

//
// Gamma correction tables.  These tables map our internal 8-bit PWM
// levels, as used in the logical output port object, to PWM levels at
// various other resolutions.  Each concrete device class should use the
// table that matches its device's native resolution.  Note that the
// input is always 8-bit, because the input is always our internal
// logical level.  It's the output that can vary in bit size - the
// output is the value that will be sent to the physical device, which
// might be more or less than 8 bits depending on what kind of port it
// is.
//

// Gamma correction table for 8-bit device resolution
// Maps 8-bit input to 8-bit gamma-corrected output
const uint8_t OutputManager::Device::gamma_8bit[] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
    2,   3,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,
    5,   6,   6,   6,   6,   7,   7,   7,   7,   8,   8,   8,   9,   9,   9,   10,
    10,  10,  11,  11,  11,  12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,
    17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  23,  24,  24,  25,
    25,  26,  27,  27,  28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  35,  36,
    37,  38,  39,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  50,
    51,  52,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  66,  67,  68,
    69,  70,  72,  73,  74,  75,  77,  78,  79,  81,  82,  83,  85,  86,  87,  89,
    90,  92,  93,  95,  96,  98,  99,  101, 102, 104, 105, 107, 109, 110, 112, 114,
    115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
    144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175,
    177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
    215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};

// Gamma correction table for 12-bit device resolution
// Maps 8-bit input to 12-bit gamma-corrected output
const uint16_t OutputManager::Device::gamma_12bit[] = {
    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   1,   1,   1,  1,   1,
    2,   2,   2,   3,   3,   4,   4,    5,   5,   6,   7,   8,   8,   9,  10,  11,
    12,  13,  15,  16,  17,  18,  20,   21,  23,  25,  26,  28,  30,  32,  34,  36,
    38,  40,  43,  45,  48,  50,  53,   56,  59,  62,  65,  68,  71,  75,  78,  82,
    85,  89,  93,  97, 101,  105, 110,  114, 119, 123, 128, 133, 138, 143, 149, 154,
    159, 165, 171, 177, 183, 189, 195,  202, 208, 215, 222, 229, 236, 243, 250, 258,
    266, 273, 281, 290, 298, 306, 315,  324, 332, 341, 351, 360, 369, 379, 389, 399,
    409, 419, 430, 440, 451, 462, 473,  485, 496, 508, 520, 532, 544, 556, 569, 582,
    594, 608, 621, 634, 648, 662, 676,  690, 704, 719, 734, 749, 764, 779, 795, 811,
    827, 843, 859, 876, 893, 910, 927,  944, 962, 980, 998, 1016, 1034, 1053, 1072, 1091,
    1110, 1130, 1150, 1170, 1190, 1210, 1231, 1252, 1273, 1294, 1316, 1338, 1360, 1382, 1404, 1427,
    1450, 1473, 1497, 1520, 1544, 1568, 1593, 1617, 1642, 1667, 1693, 1718, 1744, 1770, 1797, 1823,
    1850, 1877, 1905, 1932, 1960, 1988, 2017, 2045, 2074, 2103, 2133, 2162, 2192, 2223, 2253, 2284,
    2315, 2346, 2378, 2410, 2442, 2474, 2507, 2540, 2573, 2606, 2640, 2674, 2708, 2743, 2778, 2813,
    2849, 2884, 2920, 2957, 2993, 3030, 3067, 3105, 3143, 3181, 3219, 3258, 3297, 3336, 3376, 3416,
    3456, 3496, 3537, 3578, 3619, 3661, 3703, 3745, 3788, 3831, 3874, 3918, 3962, 4006, 4050, 4095
};

// Gamma correction table for an output device using a 0..1 float scale
// Maps 8-bit input to gamma-corrected 'float' output
const float OutputManager::Device::gamma_float[] = {
    0.000000f, 0.000000f, 0.000001f, 0.000004f, 0.000009f, 0.000017f, 0.000028f, 0.000042f,
    0.000062f, 0.000086f, 0.000115f, 0.000151f, 0.000192f, 0.000240f, 0.000296f, 0.000359f,
    0.000430f, 0.000509f, 0.000598f, 0.000695f, 0.000803f, 0.000920f, 0.001048f, 0.001187f,
    0.001337f, 0.001499f, 0.001673f, 0.001860f, 0.002059f, 0.002272f, 0.002498f, 0.002738f,
    0.002993f, 0.003262f, 0.003547f, 0.003847f, 0.004162f, 0.004494f, 0.004843f, 0.005208f,
    0.005591f, 0.005991f, 0.006409f, 0.006845f, 0.007301f, 0.007775f, 0.008268f, 0.008781f,
    0.009315f, 0.009868f, 0.010442f, 0.011038f, 0.011655f, 0.012293f, 0.012954f, 0.013637f,
    0.014342f, 0.015071f, 0.015823f, 0.016599f, 0.017398f, 0.018223f, 0.019071f, 0.019945f,
    0.020844f, 0.021769f, 0.022720f, 0.023697f, 0.024701f, 0.025731f, 0.026789f, 0.027875f,
    0.028988f, 0.030129f, 0.031299f, 0.032498f, 0.033726f, 0.034983f, 0.036270f, 0.037587f,
    0.038935f, 0.040313f, 0.041722f, 0.043162f, 0.044634f, 0.046138f, 0.047674f, 0.049243f,
    0.050844f, 0.052478f, 0.054146f, 0.055847f, 0.057583f, 0.059353f, 0.061157f, 0.062996f,
    0.064870f, 0.066780f, 0.068726f, 0.070708f, 0.072726f, 0.074780f, 0.076872f, 0.079001f,
    0.081167f, 0.083371f, 0.085614f, 0.087895f, 0.090214f, 0.092572f, 0.094970f, 0.097407f,
    0.099884f, 0.102402f, 0.104959f, 0.107558f, 0.110197f, 0.112878f, 0.115600f, 0.118364f,
    0.121170f, 0.124019f, 0.126910f, 0.129844f, 0.132821f, 0.135842f, 0.138907f, 0.142016f,
    0.145170f, 0.148367f, 0.151610f, 0.154898f, 0.158232f, 0.161611f, 0.165037f, 0.168509f,
    0.172027f, 0.175592f, 0.179205f, 0.182864f, 0.186572f, 0.190327f, 0.194131f, 0.197983f,
    0.201884f, 0.205834f, 0.209834f, 0.213883f, 0.217982f, 0.222131f, 0.226330f, 0.230581f,
    0.234882f, 0.239234f, 0.243638f, 0.248094f, 0.252602f, 0.257162f, 0.261774f, 0.266440f,
    0.271159f, 0.275931f, 0.280756f, 0.285636f, 0.290570f, 0.295558f, 0.300601f, 0.305699f,
    0.310852f, 0.316061f, 0.321325f, 0.326645f, 0.332022f, 0.337456f, 0.342946f, 0.348493f,
    0.354098f, 0.359760f, 0.365480f, 0.371258f, 0.377095f, 0.382990f, 0.388944f, 0.394958f,
    0.401030f, 0.407163f, 0.413356f, 0.419608f, 0.425921f, 0.432295f, 0.438730f, 0.445226f,
    0.451784f, 0.458404f, 0.465085f, 0.471829f, 0.478635f, 0.485504f, 0.492436f, 0.499432f,
    0.506491f, 0.513614f, 0.520800f, 0.528052f, 0.535367f, 0.542748f, 0.550194f, 0.557705f,
    0.565282f, 0.572924f, 0.580633f, 0.588408f, 0.596249f, 0.604158f, 0.612133f, 0.620176f,
    0.628287f, 0.636465f, 0.644712f, 0.653027f, 0.661410f, 0.669863f, 0.678384f, 0.686975f,
    0.695636f, 0.704366f, 0.713167f, 0.722038f, 0.730979f, 0.739992f, 0.749075f, 0.758230f,
    0.767457f, 0.776755f, 0.786126f, 0.795568f, 0.805084f, 0.814672f, 0.824334f, 0.834068f,
    0.843877f, 0.853759f, 0.863715f, 0.873746f, 0.883851f, 0.894031f, 0.904286f, 0.914616f,
    0.925022f, 0.935504f, 0.946062f, 0.956696f, 0.967407f, 0.978194f, 0.989058f, 1.000000f
};

const float OutputManager::Device::gamma_float_invert[] = {
    1.000000f, 1.000000f, 0.999999f, 0.999996f, 0.999991f, 0.999983f, 0.999972f, 0.999958f,
    0.999938f, 0.999914f, 0.999885f, 0.999849f, 0.999808f, 0.999760f, 0.999704f, 0.999641f,
    0.999570f, 0.999491f, 0.999402f, 0.999305f, 0.999197f, 0.999080f, 0.998952f, 0.998813f,
    0.998663f, 0.998501f, 0.998327f, 0.998140f, 0.997941f, 0.997728f, 0.997502f, 0.997262f,
    0.997007f, 0.996738f, 0.996453f, 0.996153f, 0.995838f, 0.995506f, 0.995157f, 0.994792f,
    0.994409f, 0.994009f, 0.993591f, 0.993155f, 0.992699f, 0.992225f, 0.991732f, 0.991219f,
    0.990685f, 0.990132f, 0.989558f, 0.988962f, 0.988345f, 0.987707f, 0.987046f, 0.986363f,
    0.985658f, 0.984929f, 0.984177f, 0.983401f, 0.982602f, 0.981777f, 0.980929f, 0.980055f,
    0.979156f, 0.978231f, 0.977280f, 0.976303f, 0.975299f, 0.974269f, 0.973211f, 0.972125f,
    0.971012f, 0.969871f, 0.968701f, 0.967502f, 0.966274f, 0.965017f, 0.963730f, 0.962413f,
    0.961065f, 0.959687f, 0.958278f, 0.956838f, 0.955366f, 0.953862f, 0.952326f, 0.950757f,
    0.949156f, 0.947522f, 0.945854f, 0.944153f, 0.942417f, 0.940647f, 0.938843f, 0.937004f,
    0.935130f, 0.933220f, 0.931274f, 0.929292f, 0.927274f, 0.925220f, 0.923128f, 0.920999f,
    0.918833f, 0.916629f, 0.914386f, 0.912105f, 0.909786f, 0.907428f, 0.905030f, 0.902593f,
    0.900116f, 0.897598f, 0.895041f, 0.892442f, 0.889803f, 0.887122f, 0.884400f, 0.881636f,
    0.878830f, 0.875981f, 0.873090f, 0.870156f, 0.867179f, 0.864158f, 0.861093f, 0.857984f,
    0.854830f, 0.851633f, 0.848390f, 0.845102f, 0.841768f, 0.838389f, 0.834963f, 0.831491f,
    0.827973f, 0.824408f, 0.820795f, 0.817136f, 0.813428f, 0.809673f, 0.805869f, 0.802017f,
    0.798116f, 0.794166f, 0.790166f, 0.786117f, 0.782018f, 0.777869f, 0.773670f, 0.769419f,
    0.765118f, 0.760766f, 0.756362f, 0.751906f, 0.747398f, 0.742838f, 0.738226f, 0.733560f,
    0.728841f, 0.724069f, 0.719244f, 0.714364f, 0.709430f, 0.704442f, 0.699399f, 0.694301f,
    0.689148f, 0.683939f, 0.678675f, 0.673355f, 0.667978f, 0.662544f, 0.657054f, 0.651507f,
    0.645902f, 0.640240f, 0.634520f, 0.628742f, 0.622905f, 0.617010f, 0.611056f, 0.605042f,
    0.598970f, 0.592837f, 0.586644f, 0.580392f, 0.574079f, 0.567705f, 0.561270f, 0.554774f,
    0.548216f, 0.541596f, 0.534915f, 0.528171f, 0.521365f, 0.514496f, 0.507564f, 0.500568f,
    0.493509f, 0.486386f, 0.479200f, 0.471948f, 0.464633f, 0.457252f, 0.449806f, 0.442295f,
    0.434718f, 0.427076f, 0.419367f, 0.411592f, 0.403751f, 0.395842f, 0.387867f, 0.379824f,
    0.371713f, 0.363535f, 0.355288f, 0.346973f, 0.338590f, 0.330137f, 0.321616f, 0.313025f,
    0.304364f, 0.295634f, 0.286833f, 0.277962f, 0.269021f, 0.260008f, 0.250925f, 0.241770f,
    0.232543f, 0.223245f, 0.213874f, 0.204432f, 0.194916f, 0.185328f, 0.175666f, 0.165932f,
    0.156123f, 0.146241f, 0.136285f, 0.126254f, 0.116149f, 0.105969f, 0.095714f, 0.085384f,
    0.074978f, 0.064496f, 0.053938f, 0.043304f, 0.032593f, 0.021806f, 0.010942f, 0.000000f,
};

const float OutputManager::Device::linear_float[] = {
    0.000000f, 0.003922f, 0.007843f, 0.011765f, 0.015686f, 0.019608f, 0.023529f, 0.027451f,
    0.031373f, 0.035294f, 0.039216f, 0.043137f, 0.047059f, 0.050980f, 0.054902f, 0.058824f,
    0.062745f, 0.066667f, 0.070588f, 0.074510f, 0.078431f, 0.082353f, 0.086275f, 0.090196f,
    0.094118f, 0.098039f, 0.101961f, 0.105882f, 0.109804f, 0.113725f, 0.117647f, 0.121569f,
    0.125490f, 0.129412f, 0.133333f, 0.137255f, 0.141176f, 0.145098f, 0.149020f, 0.152941f,
    0.156863f, 0.160784f, 0.164706f, 0.168627f, 0.172549f, 0.176471f, 0.180392f, 0.184314f,
    0.188235f, 0.192157f, 0.196078f, 0.200000f, 0.203922f, 0.207843f, 0.211765f, 0.215686f,
    0.219608f, 0.223529f, 0.227451f, 0.231373f, 0.235294f, 0.239216f, 0.243137f, 0.247059f,
    0.250980f, 0.254902f, 0.258824f, 0.262745f, 0.266667f, 0.270588f, 0.274510f, 0.278431f,
    0.282353f, 0.286275f, 0.290196f, 0.294118f, 0.298039f, 0.301961f, 0.305882f, 0.309804f,
    0.313726f, 0.317647f, 0.321569f, 0.325490f, 0.329412f, 0.333333f, 0.337255f, 0.341176f,
    0.345098f, 0.349020f, 0.352941f, 0.356863f, 0.360784f, 0.364706f, 0.368627f, 0.372549f,
    0.376471f, 0.380392f, 0.384314f, 0.388235f, 0.392157f, 0.396078f, 0.400000f, 0.403922f,
    0.407843f, 0.411765f, 0.415686f, 0.419608f, 0.423529f, 0.427451f, 0.431373f, 0.435294f,
    0.439216f, 0.443137f, 0.447059f, 0.450980f, 0.454902f, 0.458824f, 0.462745f, 0.466667f,
    0.470588f, 0.474510f, 0.478431f, 0.482353f, 0.486275f, 0.490196f, 0.494118f, 0.498039f,
    0.501961f, 0.505882f, 0.509804f, 0.513726f, 0.517647f, 0.521569f, 0.525490f, 0.529412f,
    0.533333f, 0.537255f, 0.541176f, 0.545098f, 0.549020f, 0.552941f, 0.556863f, 0.560784f,
    0.564706f, 0.568627f, 0.572549f, 0.576471f, 0.580392f, 0.584314f, 0.588235f, 0.592157f,
    0.596078f, 0.600000f, 0.603922f, 0.607843f, 0.611765f, 0.615686f, 0.619608f, 0.623529f,
    0.627451f, 0.631373f, 0.635294f, 0.639216f, 0.643137f, 0.647059f, 0.650980f, 0.654902f,
    0.658824f, 0.662745f, 0.666667f, 0.670588f, 0.674510f, 0.678431f, 0.682353f, 0.686275f,
    0.690196f, 0.694118f, 0.698039f, 0.701961f, 0.705882f, 0.709804f, 0.713726f, 0.717647f,
    0.721569f, 0.725490f, 0.729412f, 0.733333f, 0.737255f, 0.741176f, 0.745098f, 0.749020f,
    0.752941f, 0.756863f, 0.760784f, 0.764706f, 0.768627f, 0.772549f, 0.776471f, 0.780392f,
    0.784314f, 0.788235f, 0.792157f, 0.796078f, 0.800000f, 0.803922f, 0.807843f, 0.811765f,
    0.815686f, 0.819608f, 0.823529f, 0.827451f, 0.831373f, 0.835294f, 0.839216f, 0.843137f,
    0.847059f, 0.850980f, 0.854902f, 0.858824f, 0.862745f, 0.866667f, 0.870588f, 0.874510f,
    0.878431f, 0.882353f, 0.886275f, 0.890196f, 0.894118f, 0.898039f, 0.901961f, 0.905882f,
    0.909804f, 0.913725f, 0.917647f, 0.921569f, 0.925490f, 0.929412f, 0.933333f, 0.937255f,
    0.941176f, 0.945098f, 0.949020f, 0.952941f, 0.956863f, 0.960784f, 0.964706f, 0.968627f,
    0.972549f, 0.976471f, 0.980392f, 0.984314f, 0.988235f, 0.992157f, 0.996078f, 1.000000f,
};

const float OutputManager::Device::linear_float_invert[] = {
    1.000000f, 0.996078f, 0.992157f, 0.988235f, 0.984314f, 0.980392f, 0.976471f, 0.972549f,
    0.968627f, 0.964706f, 0.960784f, 0.956863f, 0.952941f, 0.949020f, 0.945098f, 0.941176f,
    0.937255f, 0.933333f, 0.929412f, 0.925490f, 0.921569f, 0.917647f, 0.913725f, 0.909804f,
    0.905882f, 0.901961f, 0.898039f, 0.894118f, 0.890196f, 0.886275f, 0.882353f, 0.878431f,
    0.874510f, 0.870588f, 0.866667f, 0.862745f, 0.858824f, 0.854902f, 0.850980f, 0.847059f,
    0.843137f, 0.839216f, 0.835294f, 0.831373f, 0.827451f, 0.823529f, 0.819608f, 0.815686f,
    0.811765f, 0.807843f, 0.803922f, 0.800000f, 0.796078f, 0.792157f, 0.788235f, 0.784314f,
    0.780392f, 0.776471f, 0.772549f, 0.768627f, 0.764706f, 0.760784f, 0.756863f, 0.752941f,
    0.749020f, 0.745098f, 0.741176f, 0.737255f, 0.733333f, 0.729412f, 0.725490f, 0.721569f,
    0.717647f, 0.713725f, 0.709804f, 0.705882f, 0.701961f, 0.698039f, 0.694118f, 0.690196f,
    0.686275f, 0.682353f, 0.678431f, 0.674510f, 0.670588f, 0.666667f, 0.662745f, 0.658823f,
    0.654902f, 0.650980f, 0.647059f, 0.643137f, 0.639216f, 0.635294f, 0.631373f, 0.627451f,
    0.623529f, 0.619608f, 0.615686f, 0.611765f, 0.607843f, 0.603922f, 0.600000f, 0.596078f,
    0.592157f, 0.588235f, 0.584314f, 0.580392f, 0.576471f, 0.572549f, 0.568627f, 0.564706f,
    0.560784f, 0.556863f, 0.552941f, 0.549020f, 0.545098f, 0.541176f, 0.537255f, 0.533333f,
    0.529412f, 0.525490f, 0.521569f, 0.517647f, 0.513726f, 0.509804f, 0.505882f, 0.501961f,
    0.498039f, 0.494118f, 0.490196f, 0.486274f, 0.482353f, 0.478431f, 0.474510f, 0.470588f,
    0.466667f, 0.462745f, 0.458824f, 0.454902f, 0.450980f, 0.447059f, 0.443137f, 0.439216f,
    0.435294f, 0.431373f, 0.427451f, 0.423529f, 0.419608f, 0.415686f, 0.411765f, 0.407843f,
    0.403922f, 0.400000f, 0.396078f, 0.392157f, 0.388235f, 0.384314f, 0.380392f, 0.376471f,
    0.372549f, 0.368627f, 0.364706f, 0.360784f, 0.356863f, 0.352941f, 0.349020f, 0.345098f,
    0.341176f, 0.337255f, 0.333333f, 0.329412f, 0.325490f, 0.321569f, 0.317647f, 0.313725f,
    0.309804f, 0.305882f, 0.301961f, 0.298039f, 0.294118f, 0.290196f, 0.286274f, 0.282353f,
    0.278431f, 0.274510f, 0.270588f, 0.266667f, 0.262745f, 0.258824f, 0.254902f, 0.250980f,
    0.247059f, 0.243137f, 0.239216f, 0.235294f, 0.231373f, 0.227451f, 0.223529f, 0.219608f,
    0.215686f, 0.211765f, 0.207843f, 0.203922f, 0.200000f, 0.196078f, 0.192157f, 0.188235f,
    0.184314f, 0.180392f, 0.176471f, 0.172549f, 0.168627f, 0.164706f, 0.160784f, 0.156863f,
    0.152941f, 0.149020f, 0.145098f, 0.141176f, 0.137255f, 0.133333f, 0.129412f, 0.125490f,
    0.121569f, 0.117647f, 0.113725f, 0.109804f, 0.105882f, 0.101961f, 0.098039f, 0.094118f,
    0.090196f, 0.086275f, 0.082353f, 0.078431f, 0.074510f, 0.070588f, 0.066667f, 0.062745f,
    0.058824f, 0.054902f, 0.050980f, 0.047059f, 0.043137f, 0.039216f, 0.035294f, 0.031373f,
    0.027451f, 0.023529f, 0.019608f, 0.015686f, 0.011765f, 0.007843f, 0.003922f, 0.000000f,
};

// mapping table looking table for float conversions
const float* const OutputManager::Device::conv_tables_float[] = {
    linear_float, linear_float_invert, gamma_float, gamma_float_invert
};


// --------------------------------------------------------------------------
//
// Null device interface
//

void OutputManager::NullDev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_VIRTUAL;
}

// --------------------------------------------------------------------------
//
// GPIO output device interface
//

// port assignments
OutputManager::GPIODev::AssignmentType OutputManager::GPIODev::assignmentType[30];

// direct device port access, for testing/troubleshooting
void OutputManager::GPIODev::SetDevicePortLevel(int port, int pwmLevel)
{
    // validate the port number
    if (port >= 0 && port < numPorts)
    {
        // check the port type
        switch (assignmentType[port])
        {
        case AssignmentType::None:
            // Not assigned - suppress the write, since the port might be in use
            // by some other subsystem, which might not appreciate outside writes
            break;
            
        case AssignmentType::PWM:
            // PWM port - set the PWM level based on 8-bit input resolution
            pwmManager.SetLevel(port, std::min(pwmLevel, 255) / 255.0f);
            break;

        case AssignmentType::Digital:
            // digital port - set the port on or off; any non-zero level is ON
            gpio_put(port, pwmLevel != 0);
            break;
        }
    }
}

void OutputManager::GPIODev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_GPIO;
    desc->devPort = gp;
}

void OutputManager::GPIODev::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    // Populate the descriptor.  We report a nominal 8-bit PWM resolution,
    // even though the actual resolution can vary by port.  We don't have
    // a mechanism to report port-by-port PWM resolution, since we have no
    // particular need for it: clients (other than the Config Tool) always
    // see the output ports in terms of the DOF interface, which uses a
    // uniform 8-bit PWM level for all ports, regardless of the underlying
    // hardware.
    descs->configIndex = 0;
    descs->devType = PinscapePico::OutputPortDesc::DEV_GPIO;
    descs->numPorts = 30;
    descs->numPortsPerChip = 30;
    descs->pwmRes = 256;
    descs->addr = 0;

    // advance the pointer
    descs += 1;
}

void OutputManager::GPIODev::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    // visit all ports
    for (int i = 0 ; i < numPorts ; ++i, ++descs)
    {
        // set it according to the output port usage
        switch (GPIODev::GetAssignmentType(i))
        {
        case GPIODev::AssignmentType::PWM:
            // mapped as a PWM output
            descs->type = descs->TYPE_PWM;
            break;

        case GPIODev::AssignmentType::Digital:
            // mapped as a digital output
            descs->type = descs->TYPE_DIGITAL;
            break;

        default:
            // no mapped as an output - report the level as 0
            descs->type = descs->TYPE_UNUSED;
            break;
        }
    }
}

void OutputManager::GPIODev::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
{
    // Visit all ports.  Report levels even for ports not used as outputs,
    // to maintain consistency with the device descriptor and port descriptor
    // list, which include all GPIOs.
    for (int i = 0 ; i < numPorts ; ++i, ++levels)
    {
        // set it according to the output port usage
        switch (GPIODev::GetAssignmentType(i))
        {
        case GPIODev::AssignmentType::PWM:
            // mapped as a PWM output - report the level on a 0..255 scale
            levels->level = static_cast<uint8_t>(roundf(pwmManager.GetLevel(i) * 255.0f));
            break;
                
        case GPIODev::AssignmentType::Digital:
            // mapped as a digital output - report the level on a 0..1 scale
            levels->level = (gpio_get_out_level(i) ? 1 : 0);
            break;

        default:
            // no mapped as an output - report the level as 0
            levels->level = 0;
            break;
        }
    }
}

// --------------------------------------------------------------------------
//
// Digital GPIO output device interface
//

OutputManager::DigitalGPIODev::DigitalGPIODev(int gp) : GPIODev(gp)
{
    // initialize the port as a digital output
    gpio_init(gp);
    gpio_set_dir(gp, GPIO_OUT);

    // mark the port assignment type
    assignmentType[gp] = AssignmentType::Digital;
}

void OutputManager::DigitalGPIODev::Set(uint8_t level)
{
    gpio_put(gp, To1BitPhys(level));
}

uint8_t OutputManager::DigitalGPIODev::Get() const
{
    return gpio_get(gp) ? 255 : 0;
}

// --------------------------------------------------------------------------
//
// PWM GPIO output device interface
//

OutputManager::PWMGPIODev::PWMGPIODev(int gp, int freq) : GPIODev(gp)
{
    // initialize PWM on the port
    pwmManager.InitGPIO("Output[type=gpio]", gp);

    // if a valid frequency was specified, set it on the PWM unit
    if (freq > 0)
        pwmManager.SetFreq(gp, freq);

    // mark the port assignment type
    assignmentType[gp] = AssignmentType::PWM;
}

void OutputManager::PWMGPIODev::Set(uint8_t level)
{
    pwmManager.SetLevel(gp, ToFloatPhys(level));
}

uint8_t OutputManager::PWMGPIODev::Get() const
{
    return static_cast<uint8_t>(pwmManager.GetLevel(gp) * 255.0f);
}


// --------------------------------------------------------------------------
//
// Pico PWM Worker device interface
//

// configure common properties
void OutputManager::PWMWorkerDev::Configure(const JSONParser::Value *val)
{
    // do the base class work to configure the common properties
    Device::Configure(val);

    // delegate the gamma and logic inversion settings to the device
    worker->ConfigurePort(port, gamma, inverted);
}

// apply port-level properties
void OutputManager::PWMWorkerDev::ApplyPortSettings(Port *portObj)
{
    // delegate the basic flipper logic power and time limits to the device
    const auto &fl = portObj->flipperLogic;
    if (fl.enabled)
        worker->ConfigureFlipperLogic(port, fl.reducedPowerLevel, static_cast<uint16_t>(fl.dtHighPowerMax / 1000UL));
}


const char *OutputManager::PWMWorkerDev::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "WorkerPico[%d] OUT%d", worker != nullptr ? worker->GetConfigIndex() : -1, port);
    return buf;
}

void OutputManager::PWMWorkerDev::Set(uint8_t level)
{
    // Set our output port on the chip.  The worker Pico uses the same
    // linear 8-bit duty cycle scale that we use internally, so we can
    // simply pass our abstract 0..255 level straight through to the
    // chip without any rescaling.  What's more, the worker handles
    // gamma and inverted logic, so we always just pass through the DOF
    // port setting.
    worker->Set(port, level);
}

uint8_t OutputManager::PWMWorkerDev::Get() const
{
    return worker->Get(port);
}

// direct device port access
void OutputManager::PWMWorkerDev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *worker = PWMWorker::GetUnit(configIndex) ; worker != nullptr && worker->IsValidPort(port))
        worker->Set(port, static_cast<uint8_t>(std::min(pwmLevel, 255)));
}

// populate a vendor interface port descriptor's device information
void OutputManager::PWMWorkerDev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_PWMWORKER;
    desc->devId = worker->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// TLC59116 device interface
//

const char *OutputManager::TLC59116Dev::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "TLC59116[%d] OUT%d", chip != nullptr ? chip->GetConfigIndex() : -1, port);
    return buf;
}

void OutputManager::TLC59116Dev::Set(uint8_t level)
{
    // Set our output port on the chip.  The TLC59116 conveniently uses
    // the same linear 8-bit duty cycle scale that we use internally, so
    // we can simply pass our abstract 0..255 level straight through to
    // the chip without any rescaling.  We just have to apply gamma and
    // logic inversion as usual.
    chip->Set(port, To8BitPhys(level));
}

uint8_t OutputManager::TLC59116Dev::Get() const
{
    return chip->Get(port);
}

// direct device port access
void OutputManager::TLC59116Dev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *chip = TLC59116::GetChip(configIndex) ; chip != nullptr && chip->IsValidPort(port))
        chip->Set(port, static_cast<uint8_t>(std::min(pwmLevel, 255)));
}

// populate a vendor interface port descriptor's device information
void OutputManager::TLC59116Dev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_TLC59116;
    desc->devId = chip->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// TLC5940 device interface
//

const char *OutputManager::TLC5940Dev::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "TLC5940[%d] OUT%d", chain != nullptr ? chain->GetConfigIndex() : -1, port);
    return buf;
}

void OutputManager::TLC5940Dev::Set(uint8_t level)
{
    // Set our output port on the chip.  The TLC5940 uses a 12-bit
    // linear scale for the duty cycle, so we have to rescale our
    // 8-bit value to 12 bits.
    chain->Set(port, To12BitPhys(level));
}

uint8_t OutputManager::TLC5940Dev::Get() const
{
    // rescale from TLC5940 native 12-bit to DOF normalized 8-bit representation
    return static_cast<uint8_t>(chain->Get(port) >> 4);
}

// direct device port access
void OutputManager::TLC5940Dev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *chain = TLC5940::GetChain(configIndex) ; chain != nullptr && chain->IsValidPort(port))
        chain->Set(port, static_cast<uint16_t>(std::min(pwmLevel, 4095)));
}

// populate a vendor interface port descriptor's device information
void OutputManager::TLC5940Dev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_TLC5940;
    desc->devId = chain->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// TLC5947 device interface
//

const char *OutputManager::TLC5947Dev::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "TLC5947[%d] OUT%d", chain != nullptr ? chain->GetConfigIndex() : -1, port);
    return buf;
}

void OutputManager::TLC5947Dev::Set(uint8_t level)
{
    // Set our output port on the chip.  The TLC5947 uses a 12-bit
    // linear scale for the duty cycle, so we have to rescale our
    // 8-bit value to 12 bits.
    chain->Set(port, To12BitPhys(level));
}

uint8_t OutputManager::TLC5947Dev::Get() const
{
    // rescale from TLC5947 native 12-bit to DOF normalized 8-bit representation
    return static_cast<uint8_t>(chain->Get(port) >> 4);
}

// direct device port access
void OutputManager::TLC5947Dev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *chain = TLC5947::GetChain(configIndex) ; chain != nullptr && chain->IsValidPort(port))
        chain->Set(port, static_cast<uint16_t>(std::min(pwmLevel, 4095)));
}

// populate a vendor interface port descriptor's device information
void OutputManager::TLC5947Dev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_TLC5947;
    desc->devId = chain->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// 74HC595 device interface
//

const char *OutputManager::C74HC595Dev::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "74HC595[%d] port Q%c", chain != nullptr ? chain->GetConfigIndex() : -1, port + 'A');
    return buf;
}

void OutputManager::C74HC595Dev::Set(uint8_t level)
{
    // The chip can operate in digital mode or 8-bit PWM mode.  In either
    // case, we can set the DOF 8-bit level directly; if the chip is in
    // digital mode, it will interpret any non-zero value as ON.
    chain->Set(port, To8BitPhys(level));
}

uint8_t OutputManager::C74HC595Dev::Get() const
{
    // ask the chip for its DOF level
    return chain->GetDOFLevel(port);
}

// direct device port access
void OutputManager::C74HC595Dev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *chain = C74HC595::GetChain(configIndex) ; chain != nullptr && chain->IsValidPort(port))
        chain->Set(port, pwmLevel);
}

// populate a vendor interface port descriptor's device information
void OutputManager::C74HC595Dev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_74HC595;
    desc->devId = chain->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// PCA9685 device interface
//

const char *OutputManager::PCA9685Dev::FullName(char *buf, size_t buflen) const
{
    snprintf(buf, buflen, "PCA9685[%d] LED%d", chip != nullptr ? chip->GetConfigIndex() : -1, port);
    return buf;
}

void OutputManager::PCA9685Dev::Set(uint8_t level)
{
    // Set our output port on the chip.  The PCA9685 uses a 12-bit
    // linear scale for the duty cycle, so we have to rescale our
    // 8-bit value to 12 bits.
    chip->Set(port, To12BitPhys(level));
}

uint8_t OutputManager::PCA9685Dev::Get() const
{
    // rescale from the chip's native 12-bit PWM levels to DOF normalized 8-bit levels
    return static_cast<uint8_t>(chip->Get(port) >> 4);
}

// direct device port access
void OutputManager::PCA9685Dev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *chip = PCA9685::GetChip(configIndex) ; chip != nullptr && chip->IsValidPort(port))
        chip->Set(port, static_cast<uint16_t>(std::min(pwmLevel, 4095)));
}

// populate a vendor interface port descriptor's device information
void OutputManager::PCA9685Dev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_PCA9685;
    desc->devId = chip->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// PCA9555 device interface
//

const char *OutputManager::PCA9555Dev::FullName(char *buf, size_t buflen) const
{
    snprintf(
        buf, buflen, "PCA9555[%d] port IO%d_%d",
        chip != nullptr ? chip->GetConfigIndex() : -1,
        port / 8, port % 8);
    return buf;
}

OutputManager::PCA9555Dev::PCA9555Dev(PCA9555 *chip, int port, const char *jsonLocus) : chip(chip), port(port)
{
    // claim and configure the port as an output
    if (chip != nullptr)
        chip->ClaimPort(port, stringPool.Add(jsonLocus), true);
}

void OutputManager::PCA9555Dev::Set(uint8_t level)
{
    // Set our output port on the chip.  The PCA9555 is a GPIO extender
    // port that only supports digital on/off outputs, not PWM levels.
    // Any non-zero PWM level is ON.  Gamma isn't meaningful.
    chip->Write(port, To1BitPhys(level));
}

uint8_t OutputManager::PCA9555Dev::Get() const
{
    // this chip reports digital ON/OFF levels only; translate ON to 255 andg1665 OFF to 0
    return chip->Read(port) ? 255 : 0;
}

// direct device port access
void OutputManager::PCA9555Dev::SetDevicePortLevel(int configIndex, int port, int pwmLevel)
{
    if (auto *chip = PCA9555::Get(configIndex) ; chip != nullptr && chip->IsValidPort(port))
        chip->Write(port, pwmLevel != 0);
}

// populate a vendor interface port descriptor's device information
void OutputManager::PCA9555Dev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_PCA9555;
    desc->devId = chip->GetConfigIndex();
    desc->devPort = port;
}

// --------------------------------------------------------------------------
//
// ZB Launch virtual device interface
//

void OutputManager::ZBLaunchDev::Set(uint8_t level)
{
    // we only care about ON and OFF; treat any non-zero level as
    // fully ON
    if (level != 0)
        level = 255;

    // apply inversion; gamma isn't meaningful
    if (inverted)
        level = 255 - level;
    
    // if the level is changing from the previous value, update
    // the ZB launch status in the plunger controller
    if (level != prvLevel)
    {
        // remember the new level for next time
        prvLevel = level;

        // update the plunger controller
        zbLaunchBall.SetActive(level != 0);
    }
}

uint8_t OutputManager::ZBLaunchDev::Get() const
{
    return prvLevel;
}

void OutputManager::ZBLaunchDev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_ZBLAUNCH;
}

// ---------------------------------------------------------------------------
//
// Share group device
//

OutputManager::ShareGroupDev::ShareGroupDev()
{
}

void OutputManager::ShareGroupDev::SetPulseMode(uint32_t tOn, uint32_t tOff)
{
    // Set the times.  For tOn, set a minimum of 1us, since 0 has the
    // special meaning that pulse mode is disabled.
    pulse.tOn = std::max(tOn, 1UL);
    pulse.tOff = tOff;
}

const char *OutputManager::ShareGroupDev::FullName(char *buf, size_t buflen) const
{
    char *p = buf, *endp = buf + buflen;
    auto Add = [&p, endp](const char *s) { for ( ; *s != 0 && p + 1 < endp ; *p++ = *s++) ; };
    Add("Group(");
    const char *sep = "";
    for (auto *c : containers)
    {
        Add(sep);
        Add(c->GetName());
        sep = ",";
    }
    Add(")");
    if (p + 1 == endp)
    {
        const char *fill = "...)";
        const char *s = fill + strlen(fill);
        while (p > buf && s > fill)
            *--p = *--s;
    }
    if (p < endp)
        *p = 0;
    
    return buf;
}

void OutputManager::ShareGroupDev::Set(uint8_t newLevel)
{
    // Check for a transition between ON and OFF states
    if (level == 0 && newLevel != 0)
    {
        // OFF -> ON transition.  Claim a port and turn it on at the new
        // level.  If we're in Pulse mode, we'll only claim it for the
        // duration of the ON Pulse; in normal mode, we'll claim the port
        // as long as the calling port remains on.

        // reset pulse mode
        pulse.onPort = nullptr;
        pulse.offPulseLevel = 0;
        pulse.tPulseEnd = 0;

        // try claiming a port from a share pool
        if (containers.size() != 0)
        {
            // search from the next pool index
            for (int startIndex = poolIndex ; ; )
            {
                // advance to the next index, wrapping at end of list
                if (++poolIndex >= static_cast<int>(containers.size()))
                    poolIndex = 0;

                // try claiming from this pool
                if ((poolPort = containers[poolIndex]->ClaimPort(this)) != nullptr)
                {
                    // Success
                    // If we're in Pulse mode, set the pulse port and time
                    if (pulse.tOn != 0)
                    {
                        pulse.tPulseEnd = time_us_64() + pulse.tOn;
                        pulse.onPort = poolPort;
                    }
                    
                    // we can stop searching now - this->poolPort is set
                    break;
                }

                // stop when we wrap back to the starting point
                if (poolIndex == startIndex)
                    break;
            }
        }
    }
    else if (level != 0 && newLevel == 0)
    {
        // ON -> OFF transition
        if (poolPort != nullptr)
        {
            // we claimed a port for the duration of the event - turn it
            // off and release the claim
            poolPort->SetShareGroupLevel(0);
            poolPort->SetShareGroupClaimant(nullptr);
            poolPort = nullptr;
        }
        else if (pulse.tOn != 0 && pulse.tOff != 0 && pulse.onPort != nullptr)
        {
            // We don't have a claimed port now, but we did claim a port
            // for the ON pulse at the OFF->ON transition.  Claim the same
            // port again to generate an OFF pulse, if it's free.
            if (pulse.onPort->shareGroupClaimant == nullptr)
            {
                // claim the port and start an ON pulse at the last logical level
                poolPort = pulse.onPort;
                poolPort->SetShareGroupLevel(level);
                poolPort->SetShareGroupClaimant(this);
                pulse.tPulseEnd = time_us_64() + pulse.tOff;
                pulse.offPulseLevel = level;
            }

            // this consumes the ON port memory for this event (whether or not
            // we were able to re-claim the port)
            pulse.onPort = nullptr;
        }
    }

    // remember the new level
    level = newLevel;

    // if we have an underlying pool port assigned, pass through the new level
    if (poolPort != nullptr)
    {
        // check for pulse timeout
        if (pulse.tPulseEnd != 0 && time_us_64() > pulse.tPulseEnd)
        {
            // pulse timeout reached - turn off and release the port
            poolPort->SetShareGroupLevel(0);
            poolPort->SetShareGroupClaimant(nullptr);
            poolPort = nullptr;

            // clear the OFF pulse level
            pulse.offPulseLevel = 0;
        }
        else if (pulse.offPulseLevel != 0)
        {
            // an OFF pulse is running - maintain the last level before
            // the OFF transition
            poolPort->SetShareGroupLevel(pulse.offPulseLevel);            
        }
        else
        {
            // set the new level in the underlying port
            poolPort->SetShareGroupLevel(newLevel);
        }
    }
}

void OutputManager::ShareGroupDev::Populate(PinscapePico::OutputPortDesc *desc) const
{
    desc->devType = PinscapePico::OutputPortDesc::DEV_SHAREGROUP;
    desc->devId = containers.size() != 0 ? containers.front()->GetID() : 0;
}


// ---------------------------------------------------------------------------
//
// Share group container
//

OutputManager::ShareGroupContainer *OutputManager::ShareGroupContainer::FindOrCreate(const char *name)
{
    // check for an existing group
    if (auto it = shareGroups.find(name); it != shareGroups.end())
        return &it->second;
    
    // It doesn't exist yet - create a new group.  For the internal ID,
    // assign sequential values starting from 1 as we create groups.
    auto it = shareGroups.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(name),
        std::forward_as_tuple(name, static_cast<uint8_t>(shareGroups.size() + 1)));

    // return the new group
    return &it.first->second;
}

OutputManager::Port *OutputManager::ShareGroupContainer::ClaimPort(ShareGroupDev *dev)
{
    // we can't claim a port if the pool is empty
    if (pool.size() == 0)
        return nullptr;
    
    // find the next unclaimed port in our pool
    for (int startIdx = claimIdx ; ; )
    {
        // advance to the next index, wrapping at the end of the list
        if (++claimIdx >= static_cast<int>(pool.size()))
            claimIdx = 0;

        // if this element is free, assign it
        if (auto *port = pool[claimIdx]; port->shareGroupClaimant == nullptr)
        {
            // assign the port and return it back to the caller
            port->shareGroupClaimant = dev;
            return port;
        }

        // stop when we wrap back to the starting point, since that
        // means that we've visited every port in the pool without
        // finding a free entry
        if (claimIdx == startIdx)
            return nullptr;
    }
}

// ---------------------------------------------------------------------------
//
// Output port data source
//

OutputManager::PortSource::PortSource(DataSourceArgs &args, bool raw) : port(nullptr), raw(raw)
{
    // get the port name/number argument
    std::string name;
    SourceVal v;
    if (args.TryGetString(name))
    {
        // "self" has the special meaning of the port defining the 'source' string
        if (name == "self")
        {
            // use the "self" port from the arguments
            port = args.self;
        }
        else
        {
            // look up the port by name
            auto it = portsByName.find(name);
            if (it != portsByName.end())
                port = it->second;
        }
            
        // validate it
        if (port == nullptr)
            args.errors.emplace_back(args.argi - 1, Format("port name '%s' not found", name.c_str()));
    }
    else if (args.GetConstant(v))
    {
        // look up the port by number
        int n = (v.type == SourceVal::Type::Float ? static_cast<int>(v.f) : v.AsUInt8());
        port = OutputManager::Get(n);
        if (port == nullptr)
            args.errors.emplace_back(args.argi - 1, Format("invalid port number %d", n));
    }
}

// ---------------------------------------------------------------------------
//
// TV ON data source
//

OutputManager::SourceVal OutputManager::TVONSource::Calc()
{
    // fully on (255) when the relay is on, fully off (0) otherwise
    return SourceVal::MakeUInt8(tvOn.IsRelayOn() ? 255 : 0);
}

// ---------------------------------------------------------------------------
//
// TV ON power sense state data source
//

OutputManager::SourceVal OutputManager::PowerSenseSource::Calc()
{
    using S = TVON::State;
    switch (tvOn.GetState())
    {
    case S::PowerOff:
    case S::PulseLatch:
    case S::TestLatch:
        // these states all collapse to our POWER OFF state
        return sourceOff->Calc();

    case S::Countdown:
        return sourceCountdown->Calc();

    case S::RelayOn:
        return sourceRelay->Calc();

    case S::IRReady:
    case S::IRWaiting:
    case S::IRSending:
        // these state all collapse to our IR COMMAND state
        return sourceIR->Calc();

    case S::PowerOn:
    default:
        return sourceOn->Calc();
    }
}


// ---------------------------------------------------------------------------
//
// Night Mode data source
//

OutputManager::SourceVal OutputManager::NightModeSource::Calc()
{
    // return the ON source when night mode is in effect, OFF source otherwise
    return nightModeControl.Get() ? sourceOn->Calc() : sourceOff->Calc();
}


// ---------------------------------------------------------------------------
//
// Time range source - time("range")
//
// The "range" argument uses one of the following formats:
//
// "<time> - <time>"
//    A daily clock time range.  The source is active when the wall clock
//    time is between the start and end time.  The time can span midnight.
//
// "<weekday> <time> - <weekday> <time>"
//    A span of time during a week, such as "Mon 9:00 - Wed 17:00" for
//    Monday at 9 AM to Wednesday at 5 PM, including all day Tuesday.
//    The range can span a week boundary, such as "Fri 17:00 - Mon 8:00".
//    If the times are omitted, the range includes all day on the start
//    and end days.
//
// "<weekday>/<weekday>/... <time> - <time>"
//    A time range that applies to one or more days of the week, such as
//    "Mon/Wed/Fri 12:00-13:00" for the lunch hour on Mondays, Wednesdays,
//    and Fridays only.  The time range can be omitted to include all day
//    on the selected days.
//
// "<date> <time> - <date> <time>"
//    A portion of the calendar year, from the starting time on the starting
//    date to the ending time on the ending date.  The times can be omitted
//    to include all day on the endpoint dates.  The date can span the end
//    of the year, as in "Dec 23 - Jan 2".
//
// <time> values use the format "mm:mm:ss am/pm".  The minutes and seconds are
// optional.  If the am/pm marker is absent, the time is taken as a 24-hour
// clock time.  The am/pm marker can also be written as am, pm, a, or p.
//
// <weekday> values are the literals Mon, Tue, Wed, Thu, Fri, Sat, Sun.
//
// <date> values are given as "month day", where month is Jan, Feb, Mar, Apr,
// May, Jun, Jul, Aug, Sep, Oct, Nov, Dec, and day is 1 to 31.
//
// All literals are case-insensitive.
//


OutputManager::TimeRangeSource *OutputManager::TimeRangeSource::ParseArgs(DataSourceArgs &args)
{
    // parse the time range string argument
    std::string s;
    uint32_t tStart = 0, tEnd = 0;
    TimeRangeSource *source = nullptr;
    if (args.TryGetString(s))
    {
        // start at the beginning of the string
        const char *p = s.c_str();

        // Parse a time element
        auto ParseTime = [&args, &p](uint32_t &tResult)
        {
            // skip leading spaces
            for ( ; isspace(*p) ; ++p) ;
            
            // parse the hh, mm, and ss fields
            int ele[3]{ 0, 0, 0 };
            int nEle = 0;
            while (nEle < 3 && isdigit(*p))
            {
                // scan and skip the first digit, then scan the second digit if present
                int acc = *p++ - '0';
                if (isdigit(*p))
                    acc = (acc*10) + (*p++ - '0');

                // store the element
                ele[nEle++] = acc;

                // scan and skip ':'
                if (*p == ':')
                    ++p;
                else
                    break;
            }

            // it's an error if we didn't find at least one element
            if (nEle == 0)
            {
                args.errors.emplace_back(args.argi - 1, Format("invalid time format; expected 'hh:mm:ss [am/pm]', at %s", p));
                return false;
            }

            // check for an AM/PM marker
            for ( ; isspace(*p) ; ++p) ;
            char ampm = tolower(*p);
            if (ampm == 'a' || ampm == 'p')
            {
                // the hour now has to be 1-12
                if (ele[0] < 1 || ele[0] > 12)
                {
                    args.errors.emplace_back(args.argi - 1, "invalid time value (hour is out of range for 12-hour clock)");
                    return false;
                }

                // 12 AM    -> 00:00
                // 1-11 PM  -> add 12:00 hours
                // otherwise PM adds 12 hundred hours, AM doesn't change anything
                if (ele[0] == 12 && ampm == 'a')
                    ele[0] = 0;
                else if (ele[0] != 12 && ampm == 'p')
                    ele[0] += 12;

                // skip the 'a'/'p', and then skip the 'm' if present
                ++p;
                if (*p == 'm' || *p == 'M')
                    ++p;
            }

            // Range-check the elements.  Allow the special value 24:00:00, which
            // represents the start of the second just past the end of the day.
            // 24:00:00 is necessary to include because our intervals are treated
            // as exclusive of the ending time.
            if ((ele[0] > 23 || ele[1] > 59 || ele[2] > 59) && !(ele[0] == 24 && ele[1] == 0 && ele[2] == 0))
            {
                args.errors.emplace_back(args.argi - 1, "invalid time value; must be 00:00:00 to 24:00:00");
                return false;
            }

            // express the result as seconds since midnight
            tResult = ele[0]*60*60 + ele[1]*60 + ele[2];

            // skip any trailing spaces
            for ( ; isspace(*p) ; ++p) ;

            // successfully parsed
            return true;
        };

        // Parse a weekday.  0=Monday, following the convention of our DateTime class.
        // Returns true on a match, and advances the string pointer past the matched
        // day name.  Returns false if no match, with no error messages; p is still
        // moved, but only past any leading whitespace.
        auto ParseWeekday = [&args, &p](int &dayResult)
        {
            // skip spaces
            for ( ; isspace(*p) ; ++p) ;

            // Check for a weekday
            static const char weekdays[] = "montuewedthufrisatsun";
            const char *w = weekdays;
            for (int day = 0 ; *w != 0 ; w += 3, ++day)
            {
                // if the current day matches, and the token ends after the match
                // (that is, the next character isn't another alphabetic character),
                // it's a match
                if (tolower(p[0]) == w[0] && tolower(p[1]) == w[1] && tolower(p[2]) == w[2]
                    && !isalpha(p[3]))
                {
                    p += 3;
                    dayResult = day;
                    return true;
                }
            }

            // not matched
            return false;
        };

        // Parse a calendar date.  On success, fills in dateResult with the "year day"
        // (see YearDayNumber() in the header) of the date matched, advances p past the
        // matched text, and and returns true.  If no match, returns false, with no
        // error messages, and with p advanced only past any leading whitespace.
        auto ParseDate = [&args, &p](int &dateResult)
        {
            // skip spaces, and remember where we started
            for ( ; isspace(*p) ; ++p) ;
            const char *start = p;

            // Check for a weekday
            static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
            const char *m = months;
            for (int mon = 1 ; *m != 0 ; m += 3, ++mon)
            {
                // if the current month matches, and the token ends after the match
                // (that is, the next character isn't another alphabetic character),
                // it's a match
                if (tolower(p[0]) == m[0] && tolower(p[1]) == m[1] && tolower(p[2]) == m[2]
                    && !isalpha(p[3]))
                {
                    // skip the month and whitespace
                    for (p += 3 ; isspace(*p) ; ++p) ;

                    // there has to be a day number following
                    if (isdigit(*p))
                    {
                        // parse the day number
                        int day = *p++ - '0';
                        while (isdigit(*p))
                            day = day*10 + *p++ - '0';

                        // success - fill in the Year Day Number in the result and return true
                        dateResult = YearDayNumber(mon, day);
                        return true;
                    }
                }
            }

            // not matched - reset the parse point and return false
            p = start;
            return false;
        };

        // Check for a weekday.  This could be either a "Day Time - Day Time" range,
        // or a "Day/Day/Day Time-Time" range.
        if (int day; ParseWeekday(day))
        {
            // after the first day, we can have:
            //    -          - a full-day range, as in "Mon-Fri"
            //    /Day/...   - a day mask
            //    time       - either Day Time-Time or Day Time-Day Time
            //    end        - a single-day mask with no time
            for ( ; isspace(*p) ; ++p) ;
            if (*p == '-')
            {
                // full-day range - skip the '-' and parse the second day
                ++p;
                if (int endDay; ParseWeekday(endDay))
                {
                    // success - create the day range, from 00:00:00 on the
                    // starting day to 24:00:00 on the ending day
                    source = new WeekdayTimeRangeSource(day, endDay, 0, 24*60*60);
                }
                else
                    args.errors.emplace_back(args.argi - 1, Format("expected second weekday name in 'Day-Day' range at '%s'", p));
            }
            else if (*p == '/')
            {
                // definitely a day mask list - parse all remaining days
                int dayMask = (1 << day);
                bool ok = true;
                while (*p == '/')
                {
                    ++p;
                    if (!ParseWeekday(day))
                    {
                        args.errors.emplace_back(args.argi - 1, Format("expected another weekday name in 'Day/Day/...' range at '%s'", p));
                        ok = false;
                        break;
                    }
                    dayMask |= (1 << day);
                    for ( ; isspace(*p) ; ++p) ;
                }

                // if we didn't encounter an error in the day mask, continue to the optional time range
                if (ok)
                {
                    // we can now either be at end of string, or at a time range
                    if (*p == 0)
                    {
                        // it's a simple day mask, with no time range, so it's all day on each selected day
                        source = new WeekdayMaskTimeRangeSource(dayMask, 0, 24*60*60);
                    }
                    else if (ParseTime(tStart))
                    {
                        // we now need a '-' and the ending time
                        if (*p == '-')
                        {
                            ++p;
                            if (ParseTime(tEnd))
                            {
                                // success
                                source = new WeekdayMaskTimeRangeSource(dayMask, tStart, tEnd);
                            }
                        }
                        else
                            args.errors.emplace_back(args.argi - 1, Format("expected '-' in day-and-time range at '%s'", p));                        
                    }
                }
            }
            else if (isdigit(*p))
            {
                // It's a time, so this is either a "Day Time-Day Time" or "Day Time-Time"
                // range.  In either case, parse the starting time.
                if (ParseTime(tStart))
                {
                    // the next token must be the '-'
                    for ( ; isspace(*p) ; ++p) ;
                    if (*p == '-')
                    {
                        // The next token can be:
                        //    Day     - a "Day Time-Day Time" range
                        //    Time    - a single-day mask range, "Day Time-Time"
                        ++p;
                        if (int dayEnd; ParseWeekday(dayEnd))
                        {
                            // it's a "Day Time-Day Time" range - we now just need an ending time
                            if (ParseTime(tEnd))
                            {
                                // success - create the day-and-time range
                                source = new WeekdayTimeRangeSource(day, dayEnd, tStart, tEnd);
                            }
                        }
                        else
                        {
                            // it's not a day name, so it must be a time, for a single-day mask range
                            if (ParseTime(tEnd))
                            {
                                // success - create the single-day mask range
                                source = new WeekdayMaskTimeRangeSource(1 << day, tStart, tEnd);
                            }
                        }
                    }
                    else
                        args.errors.emplace_back(args.argi - 1, Format("expected '-' in day-and-time range at '%s'", p));
                }
            }
            else if (*p == 0)
            {
                // End of string.  A single day constitutes a day mask expression
                // that covers the whole period of the single day.
                source = new WeekdayMaskTimeRangeSource(1 << day, 0, 24*60*60);
            }
            else
            {
                // anything else is an error
                args.errors.emplace_back(args.argi - 1, Format("invalid day-and-time range format - expected /, -, time, or end at '%s'", p));
            }
        }

        // If that didn't work, check for a month, for a "Date Time - Date Time" range
        if (int startDate; source == nullptr && ParseDate(startDate))
        {
            // Found a date.  The next token can be a time (numeric), '-' for
            // an all-day date range, or end of string for a single day.
            for ( ; isspace(*p) ; ++p) ;
            if (*p == '-')
            {
                // it's an all-day range - parse the second date
                ++p;
                if (int endDate; ParseDate(endDate))
                {
                    // success
                    source = new DateAndTimeRangeSource(startDate, endDate, 0, 24*60*60);
                }
                else
                    args.errors.emplace_back(args.argi - 1, Format("expected date range ending date at '%s'", p));
            }
            else if (*p == 0)
            {
                // end of string - the range is a single day, all day
                source = new DateAndTimeRangeSource(startDate, startDate, 0, 24*60*60);
            }
            else if (ParseTime(tStart))
            {
                // this must be followed by '-', then another date and time
                for ( ; isspace(*p) ; ++p) ;
                if (*p == '-')
                {
                    // get the ending date
                    ++p;
                    if (int endDate; ParseDate(endDate))
                    {
                        // get the ending time
                        if (ParseTime(tEnd))
                        {
                            // success
                            source = new DateAndTimeRangeSource(startDate, endDate, tStart, tEnd);
                        }
                    }
                    else
                        args.errors.emplace_back(args.argi - 1, Format("expected date range ending date at '%s'", p));
                }
                else
                    args.errors.emplace_back(args.argi - 1, Format("expected '-' in date-and-time range at '%s'", p));
            }
        }

        // If we didn't match one of the other formats, it must be the simple time-of-day range.
        if (source == nullptr && ParseTime(tStart))
        {
            if (*p == '-')
            {
                // skip the '-' and parse the end time
                ++p;
                if (ParseTime(tEnd))
                {
                    // success - create the simple time range source
                    source = new TimeRangeSource(tStart, tEnd);
                }
            }
            else
                args.errors.emplace_back(args.argi - 1, Format("expected '-' in time range at '%s'", p));
        }

        // wherever we ended up, we have to be at the end of the string now
        for ( ; isspace(*p) ; ++p) ;
        if (*p != 0)
            args.errors.emplace_back(args.argi - 1, Format("unexpected extra text '%s' after time range ignored", p));
    }
    else
    {
        // flag it as an error
        args.errors.emplace_back(args.argi - 1, "time range must be a string");
    }

    // If we didn't already create a source, we must not have a valid format,
    // but we still need something, so create a basic time range source with
    // the default time span.
    if (source == nullptr)
        source = new TimeRangeSource(tStart, tEnd);

    // store the ON and OFF sub-sources
    source->sourceIn = args.GetSource();
    source->sourceOut = args.GetSource();

    // return the new source
    return source;
}

OutputManager::SourceVal OutputManager::TimeRangeSource::Calc()
{
    // check if we know the time of day
    DateTime t;
    if (timeOfDay.Get(t))
    {
        // If our range's starting time is less than the end time, as in
        // 10AM-2PM, treat the range as all contained within one 24-hour
        // day.  If the starting time is greater than the end time, as
        // in 11PM-1AM, treat it as spanning midnight, so it's really
        // two time ranges: start to midnight, and midnight to end.
        bool inRange;
        if (tStart < tEnd)
        {
            // the range is contained within one 24-hour period
            inRange = t.timeOfDay >= tStart && t.timeOfDay < tEnd;
        }
        else
        {
            // The range spans midnight, so treat it as two disjoint
            // ranges, start to midnight + midnight to end.  Note that
            // it's not necessary to compare t.timeOfDay to midnight in
            // either subrange, since it's always >= the start-of-day
            // mightnight and always < the end-of-day midnight.
            inRange = t.timeOfDay < tEnd || t.timeOfDay >= tStart;
        }
        
        // select the in-range or out-of-range sub-source
        return inRange ? sourceIn->Calc() : sourceOut->Calc();
    }
    else
    {
        // time of day is unknown - return the 'out of range' value
        return sourceOut->Calc();
    }
}

// Weekday time range, as in "Tue 9:00 - Fri 17:00" (from Tuesday at
// 9am to Friday at 5pm, which includes all day Wednesday and Thursday)
OutputManager::SourceVal OutputManager::WeekdayTimeRangeSource::Calc()
{
    // check if we know the time of day
    DateTime t;
    if (timeOfDay.Get(t))
    {
        // Determine if the time is in range.  First, the day of
        // the week has to be within the range.
        bool inRange;
        int weekday = t.GetWeekDay();
        if (weekdayStart < weekdayEnd)
        {
            // start day < end day -> a range within one week
            inRange = weekday >= weekdayStart && weekday <= weekdayEnd;
        }
        else
        {
            // end day < start day -> range spans a week boundary
            inRange = weekday <= weekdayEnd || weekday >= weekdayStart;
        }

        // Even if it's within the weekday range, it could be out of the
        // time range.  The time range is tStart to midnight on the
        // starting day, all day on days between start and end, and
        // midnight to tEnd on the ending day.
        if ((weekday == weekdayStart && t.timeOfDay < tStart)
            || weekday == weekdayEnd && t.timeOfDay >= tEnd)
            inRange = false;

        // select the in-range or out-of-range sub-source
        return inRange ? sourceIn->Calc() : sourceOut->Calc();
    }
    else
    {
        // time of day is unknown - return the 'out of range' value
        return sourceOut->Calc();
    }
}

// Weekday mask time range, as in "Mon/Wed/Fri 9:00-17:00" (9am to 5pm
// on Mondays, Wednesdays, and Fridays)
OutputManager::SourceVal OutputManager::WeekdayMaskTimeRangeSource::Calc()
{
    // check if we know the time of day
    DateTime t;
    if (timeOfDay.Get(t))
    {
        // Determine first if the weekday is in the mask
        int weekday = t.GetWeekDay();
        bool dayInMask = ((weekdayMask & (1 << weekday)) != 0);

        // Also check to see if the PRIOR weekday is in the mask
        int priorWeekday = (weekday == 0 ? 6 : weekday - 1);
        bool priorDayInMask = ((weekdayMask & (1 << priorWeekday)) != 0);

        // Now check the time range
        bool inRange;
        if (tStart < tEnd)
        {
            // the time range is all within a single day, so we only
            // have to check that the current time is between the start
            // and end points, and the current day is in the mask
            inRange = dayInMask && t.timeOfDay >= tStart && t.timeOfDay < tEnd;
        }
        else
        {
            // The time range spans midnight, so it's a match if the
            // current time is greater than the starting time AND
            // current day is in the mask, OR the current time is less
            // than the ending time AND the PRIOR day is in the mask.
            // We have to match the PRIOR day in the latter case because
            // the time period started on the prior day, before the
            // clock rolled over at midnight.
            inRange = (dayInMask && t.timeOfDay >= tStart)
                      || (priorDayInMask && t.timeOfDay < tEnd);
        }

        // select the in-range or out-of-range sub-source
        return inRange ? sourceIn->Calc() : sourceOut->Calc();
    }
    else
    {
        // time of day is unknown - return the 'out of range' value
        return sourceOut->Calc();
    }
}

// Calendar date and time range, as in "Mar 1 0:00:00 - Nov 30 23:59:59"
OutputManager::SourceVal OutputManager::DateAndTimeRangeSource::Calc()
{
    // check if we know the time of day
    DateTime t;
    if (timeOfDay.Get(t))
    {
        // First, determine if the date is in range.  We work in terms
        // of "year day numbers", where we can determine if one date is
        // before or after another by simple integer comparison.
        bool inRange = false;
        int day = YearDayNumber(t.mon, t.dd);
        if (dateStart < dateEnd)
        {
            // the date range is all within one year, so we're in range
            // if the date falls between the start and end dates
            inRange = day >= dateStart && day <= dateEnd;
        }
        else
        {
            // the date range spans the end of the year, so we're in
            // range if the date is after the starting date or before
            // the ending date
            inRange = day >= dateStart || day <= dateEnd;
        }

        // If the date is on the starting or ending date, the time
        // has to be after the starting time or before the ending
        // time (respectively).  Days between the endpoints include
        // the full 24-hour period, so the time of day isn't a factor
        // unless the date falls exactly on one of the endpoints.
        if ((day == dateStart && t.timeOfDay < tStart)
            || (day == dateEnd && t.timeOfDay >= tEnd))
            inRange = false;

        // select the in-range or out-of-range sub-source
        return inRange ? sourceIn->Calc() : sourceOut->Calc();
    }
    else
    {
        // time of day is unknown - return the 'out of range' value
        return sourceOut->Calc();
    }
}

// ---------------------------------------------------------------------------
//
// Plunger Position source - normalized 0..255 range
//

OutputManager::SourceVal OutputManager::PlungerPosSource::Calc()
{
    // Return the current plunger position, normalized to 0..255 range.
    // To make our INT16 fit this range, we assume that the practical
    // maximum forward travel distance is about 1/6 of the retraction
    // distance, which makes the practical minimum on the INT16 range
    // -5461.  So we first adjust the raw INT16 value by adding 5461,
    // which should give us a value from 0 to 38228 (32767+5461).  We
    // then multiply by 255/38228 to rescale the result to 0..255.
    // To keep the calculation in the integer domain while still
    // retaining maximum precision, do it in fixed-point 8.24.
    int32_t pos = static_cast<int32_t>(plunger.GetZ0()) + 5461;
    uint32_t upos = static_cast<uint32_t>(pos < 0 ? 0 : pos);
    uint32_t pos8 = ((pos * 111913UL) + 0x800000UL) >> 24;  // 111912 == 255/38228 in 8.24; 0x800000 == 0.5 in 8.24
    return SourceVal::MakeUInt8(static_cast<uint8_t>(pos8));
}

// ---------------------------------------------------------------------------
//
// Plunger Position source - native INT16 range, -32768..+32767
//

OutputManager::SourceVal OutputManager::PlungerPosFloatSource::Calc()
{
    return SourceVal::MakeFloat(static_cast<float>(plunger.GetZ0()));
}

// ---------------------------------------------------------------------------
//
// Plunger Calibration Mode source
//

OutputManager::SourceVal OutputManager::PlungerCalSource::Calc()
{
    // return the ON source when night mode is in effect, OFF source otherwise
    return plunger.IsCalMode() ? sourceOn->Calc() :
        plunger.IsCalButtonPushed() ? sourceHold->Calc() :
        sourceOff->Calc();
}

// ---------------------------------------------------------------------------
//
// RGB Status LED data source
//

OutputManager::SourceVal OutputManager::StatusLedSource::Calc()
{
    // calculate the current color
    auto color = statusRGB.GetColor();
    return SourceVal::MakeRGB(color.r, color.g, color.b);
}

// ---------------------------------------------------------------------------
//
// Button data source
//

OutputManager::ButtonSource::ButtonSource(DataSourceArgs &args) : button(nullptr)
{
    // get the button name/number argument
    std::string name;
    SourceVal v;
    if (args.TryGetString(name))
    {
        // look up the button by name
        button = Button::Get(name.c_str());

        // validate it
        if (button == nullptr)
            args.errors.emplace_back(args.argi - 1, Format("button name '%s' not found", name.c_str()));
    }
    else if (args.GetConstant(v))
    {
        // look up the button by number
        int n = (v.type == SourceVal::Type::Float ? static_cast<int>(v.f) : v.AsUInt8());
        button = Button::Get(n);
        if (button == nullptr)
            args.errors.emplace_back(args.argi - 1, Format("invalid button number %d", n));
    }

    sourceOn = args.GetSource();
    sourceOff = args.GetSource();
}

OutputManager::SourceVal OutputManager::ButtonSource::Calc()
{
    return button->GetLogicalState() ? sourceOn->Calc() : sourceOff->Calc();
}

// ---------------------------------------------------------------------------
//
// ZB Launch Button data source
//

OutputManager::SourceVal OutputManager::ZBLaunchButtonSource::Calc()
{
    return zbLaunchBall.IsFiring() ? sourceOn->Calc() : sourceOff->Calc();
}

// ---------------------------------------------------------------------------
//
// ZB Launch Mode data source
//

OutputManager::SourceVal OutputManager::ZBLaunchModeSource::Calc()
{
    return zbLaunchBall.IsActive() ? sourceOn->Calc() : sourceOff->Calc();
}

// ---------------------------------------------------------------------------
//
// IR TX data source
//

OutputManager::SourceVal OutputManager::IRTXSource::Calc()
{
    // return the ON source when a transmission is in progress
    return irTransmitter.IsSending() ? sourceOn->Calc() : sourceOff->Calc();
}

// ---------------------------------------------------------------------------
//
// IR RX data source
//

OutputManager::SourceVal OutputManager::IRRXSource::Calc()
{
    // return the ON source when a code is being received
    return irReceiver.IsReceiving() ? sourceOn->Calc() : sourceOff->Calc();
}

// ---------------------------------------------------------------------------
//
// Vector X Component source
//

OutputManager::SourceVal OutputManager::XSource::Calc()
{
    return SourceVal::MakeFloat(source->Calc().AsVector().vec.x);
}

// ---------------------------------------------------------------------------
//
// Vector Y Component source
//

OutputManager::SourceVal OutputManager::YSource::Calc()
{
    return SourceVal::MakeFloat(source->Calc().AsVector().vec.y);
}

// ---------------------------------------------------------------------------
//
// Vector Magnitude source
//

OutputManager::SourceVal OutputManager::MagnitudeSource::Calc()
{
    auto v = source->Calc().AsVector();
    return SourceVal::MakeFloat(sqrtf(v.vec.x*v.vec.x + v.vec.y*v.vec.y));
}

// ---------------------------------------------------------------------------
//
// Nudge Device data source
//

OutputManager::NudgeDeviceSource::NudgeDeviceSource(DataSourceArgs &args)
{
    // create the nudge device view singleton if we haven't already
    if (outputNudgeView == nullptr)
        outputNudgeView = nudgeDevice.CreateView();
}

OutputManager::SourceVal OutputManager::NudgeDeviceSource::Calc()
{
    // Make a 2D vector from the nudge device X/Y readings, using the
    // native signed 16-bit scale.
    return SourceVal::MakeVector(outputNudgeView->GetX(), outputNudgeView->GetY());
}

// ---------------------------------------------------------------------------
//
// XBox Controller LED source
//

OutputManager::SourceVal OutputManager::XBoxLedSource::Calc()
{
    // ignore if the LED is out of range 1-4
    if (led < 1 || led > 4)
        return SourceVal::MakeUInt8(0);

    // get the current LED value from the XInput object; return 255 if
    // it's on, 0 if off
    return SourceVal::MakeUInt8(xInput.led[led - 1] ? 255 : 0);
}

//
// XBox Controller rumble source
//

OutputManager::SourceVal OutputManager::XBoxRumbleSource::Calc()
{
    // ignore if the left/right is out of range
    if (dir < 1 || dir > 2)
        return SourceVal::MakeUInt8(0);

    // get the left/right rumble value - the native scale is 0..255,
    // perfectly matching our own, so no rescaling is necessary
    return SourceVal::MakeUInt8(dir == 1 ? xInput.leftRumble : xInput.rightRumble);
}

// ---------------------------------------------------------------------------
//
// Base class for sources that take an RGB input
//

OutputManager::SourceWithRGBInput::SourceWithRGBInput(DataSourceArgs &args)
{
    if (args.args.size() == 1)
    {
        // single argument -> RGB-valued source
        rgb = args.GetSource();
    }
    else
    {
        // other than one argument -> separate R, G, B sources
        r = args.GetSource();
        g = args.GetSource();
        b = args.GetSource();
    }
}

OutputManager::SourceWithRGBInput::RGB OutputManager::SourceWithRGBInput::CalcRGB() const
{
    if (rgb != nullptr)
    {
        // single RGB-valued source
        SourceVal v = rgb->Calc().AsRGB();
        return { v.rgb.r, v.rgb.g, v.rgb.b };
    }
    else
    {
        // separate components
        return { r->Calc().AsUInt8(), g->Calc().AsUInt8(), b->Calc().AsUInt8() };
    }
}


// ---------------------------------------------------------------------------
//
// Grayscale data source
//
// This source has two argument formats:
//
//    grayscale(<r>, <g>, <b>) - get the components from separate ports
//    grayscale(<rgb>)         - get the components from an RGB-valued source
//

OutputManager::SourceVal OutputManager::GrayscaleSource::Calc()
{
    auto v = CalcRGB();
    return SourceVal::MakeUInt8(static_cast<uint8_t>(
        0.299f * v.r
        + 0.587f * v.g
        + 0.114f * v.b));
}

// ---------------------------------------------------------------------------
//
// RGB -> HSB source, for extracting HSB components from an RGB value
//

// hue
OutputManager::SourceVal OutputManager::HueSource::Calc()
{
    auto v = CalcRGB();
    int cmax = std::max(std::max(v.r, v.g), v.b);
    int cmin = std::min(std::min(v.r, v.g), v.b);
    int delta = cmax - cmin;

    float h;
    if (delta == 0)
        h = 0.0f;
    else if (cmax == v.r)
        h = fmodf(static_cast<float>(v.g - v.g) / static_cast<float>(delta), 6.0f);
    else if (cmax == v.g)
        h = static_cast<float>(v.b - v.r) / static_cast<float>(delta) + 2.0f;
    else
        h = static_cast<float>(v.r - v.g) / static_cast<float>(delta) + 4.0f;
    
    return SourceVal::MakeUInt8(static_cast<uint8_t>(roundf(h*60.0f * 255.0f/360.0f)));
}

// saturation
OutputManager::SourceVal OutputManager::SaturationSource::Calc()
{
    auto v = CalcRGB();
    int cmax = std::max(std::max(v.r, v.g), v.b);
    int cmin = std::min(std::min(v.r, v.g), v.b);
    int delta = cmax - cmin;
    uint8_t s = (cmax == 0) ? 0 : static_cast<int>(roundf(
        static_cast<float>(delta) / static_cast<float>(cmax) * 255.0f));
    return SourceVal::MakeUInt8(s);
}

// brightness
OutputManager::SourceVal OutputManager::BrightnessSource::Calc()
{
    auto v = CalcRGB();
    uint8_t cmax = std::max(std::max(v.r, v.g), v.b);
    return SourceVal::MakeUInt8(cmax);
}


// ---------------------------------------------------------------------------
//
// Arctan data source
//
// This source has two argument formats:
//
//    arctan(<y>, <x>)     - get the components from separate sub-sources
//    arctan(<vector>)     - get the components from a single vector-valued source
//
OutputManager::ArctanSource::ArctanSource(DataSourceArgs &args)
{
    if (args.args.size() == 1)
    {
        // single argument -> vector-valued source
        vec = args.GetSource();
    }
    else
    {
        // other than one argument -> separate Y, X sources
        y = args.GetSource();
        x = args.GetSource();
    }
}

OutputManager::SourceVal OutputManager::ArctanSource::Calc()
{
    uint8_t x, y;
    if (vec != nullptr)
    {
        // single vector-valued source
        SourceVal v = vec->Calc().AsVector();
        x = static_cast<uint8_t>(abs(v.vec.x));
        y = static_cast<uint8_t>(abs(v.vec.y));
    }
    else
    {
        // separate components
        x = this->x->Calc().AsUInt8();
        y = this->y->Calc().AsUInt8();
    }

    // check for the special case 0,0
    if (x == 0 && y == 0)
    {
        // atan2f(0,0) can return a range error on some platforms, so treat
        // it as a special case, with a result of 0 degrees.  0,0 is
        // formally a pole for atan2(), but we'll interpret it as a
        // zero-length vector where the angle is arbitrary (all angles are
        // equally valid) and thus we can just pick zero by convention.
        return SourceVal::MakeUInt8(0);
    }
    else
    {
        // atan2f() returns an angle from -PI to +PI; rebase this to
        // 0..2*PI, then renormalize to 0..255
        return SourceVal::MakeUInt8(static_cast<float>((atan2f(y, x) + 3.14159265f) * 255.0f / 6.28318531f));
    }
}

// ---------------------------------------------------------------------------
//
// Blink data source
//

OutputManager::SourceVal OutputManager::BlinkSource::Calc()
{
    // figure the time within our cycle - one cycle consists of the ON
    // interval plus the OFF interval
    uint32_t period = on_us + off_us;
    uint32_t t = static_cast<uint32_t>(time_us_64() % period);

    // fully on when in the ON period, else fully off
    return SourceVal::MakeUInt8(t <= on_us ? 255 : 0);
}

// ---------------------------------------------------------------------------
//
// Ramp data source
//

OutputManager::SourceVal OutputManager::RampSource::Calc()
{
    // figure the time within our cycle
    float t = static_cast<float>(time_us_64() % period_us);

    // figure a linear ramp within the cycle
    return SourceVal::MakeUInt8(static_cast<uint8_t>(t / static_cast<float>(period_us) * 255.0f));
}

// ---------------------------------------------------------------------------
//
// Sine data source
//

OutputManager::SourceVal OutputManager::SineSource::Calc()
{
    // figure the time within our cycle
    float t = static_cast<float>(time_us_64() % period_us);

    // figure the position in the sine wave over this period
    return SourceVal::MakeUInt8(static_cast<uint8_t>((sinf(6.28318531f * (t / static_cast<float>(period_us))) * 127.0f) + 127.0f));
}

// ---------------------------------------------------------------------------
//
// Sawtooth data source
//

OutputManager::SourceVal OutputManager::SawtoothSource::Calc()
{
    // figure the time within our cycle
    float t = static_cast<float>(time_us_64() % period_us);

    // Figure a sawtooth ramp within the cycle.  The rising part is the
    // first half of the cycle, the falling part is the second half.
    float half = static_cast<float>(period_us) / 2.0f;
    return SourceVal::MakeUInt8(static_cast<uint8_t>(t <= half ? (t/half * 255.0f) : (510.0f - (t/half * 255.0f))));
}

// ---------------------------------------------------------------------------
//
// Rescaling data source
//

OutputManager::SourceVal OutputManager::ScaleSource::Calc()
{
    SourceVal v = source->Calc();
    switch (v.type)
    {
    case SourceVal::Type::UInt8:
        // convert to float and multiply
        v.f = v.i * scale;
        v.type = SourceVal::Type::Float;
        break;

    case SourceVal::Type::Float:
        v.f *= scale;
        break;

    case SourceVal::Type::RGB:
        v.rgb.r = SourceVal::ClipU8(v.rgb.r * scale);
        v.rgb.g = SourceVal::ClipU8(v.rgb.g * scale);
        v.rgb.b = SourceVal::ClipU8(v.rgb.b * scale);
        break;
    }

    return v;
}

// ---------------------------------------------------------------------------
//
// Offset data source
//

OutputManager::SourceVal OutputManager::OffsetSource::Calc()
{
    SourceVal v = source->Calc();
    switch (v.type)
    {
    case SourceVal::Type::UInt8:
        // convert to float and add
        v.f = v.i + offset;
        v.type = SourceVal::Type::Float;
        break;

    case SourceVal::Type::Float:
        v.f += offset;
        break;

    case SourceVal::Type::RGB:
        v.rgb.r = SourceVal::ClipU8(v.rgb.r + offset);
        v.rgb.g = SourceVal::ClipU8(v.rgb.g + offset);
        v.rgb.b = SourceVal::ClipU8(v.rgb.b + offset);
        break;

    case SourceVal::Type::Vector:
        v.vec.x = SourceVal::ClipI16(v.vec.x + offset);
        v.vec.y = SourceVal::ClipI16(v.vec.y + offset);
        break;
    }

    return v;
}

// ---------------------------------------------------------------------------
//
// Absolute value data source
//

OutputManager::SourceVal OutputManager::AbsSource::Calc()
{
    SourceVal v = source->Calc();
    switch (v.type)
    {
    case SourceVal::Type::UInt8:
    case SourceVal::Type::RGB:
        // UINT8 and RGB are inherently unsigned, so abs() has no effect
        break;
        
    case SourceVal::Type::Float:
        v.f = fabsf(v.f);
        break;

    case SourceVal::Type::Vector:
        // the absolute value of a vector is its vector magnitude, as a float
        v = SourceVal::MakeFloat(sqrtf(v.vec.x*v.vec.x + v.vec.y*v.vec.y));
        break;
    }

    return v;
}

// ---------------------------------------------------------------------------
//
// HSB data source
//

OutputManager::SourceVal OutputManager::HSBSource::Calc()
{
    // Get the component values.  All inputs are UINT8, range 0..255.
    // Normalize S and B to 0..1 range, and normalize H to an angle,
    // 0..360.
    float h = this->h->Calc().AsUInt8() / 255.0f * 360.0f;
    float s = this->s->Calc().AsUInt8() / 255.0f;
    float b = this->b->Calc().AsUInt8() / 255.0f;

    // Apply the standard HSV -> RGB formula
    static const auto f = [](float n, float h, float s, float b) {
        float k = fmod(n + h/60.0f, 6.0f);
        return static_cast<uint8_t>(255.0f * b * (1.0f - s*fmax(0.0f, fmin(1.0f, fmin(k, 4.0f - k)))));
    };
    return SourceVal::MakeRGB(f(5, h, s, b), f(3, h, s, b), f(1, h, s, b));
}

// ---------------------------------------------------------------------------
//
// AND data source
//

OutputManager::SourceVal OutputManager::AndSource::Calc()
{
    // if any source is zero, return zero, otherwise return the last value
    SourceVal val = SourceVal::MakeUInt8(0);
    for (auto &source : sources)
    {
        // calculate this value; if non-zero, keep it as the last one
        // so far and keep scanning, otherwise stop right now with a
        // final value of zero
        val = source->Calc();
        if (val.AsUInt8() == 0)
            return SourceVal::MakeUInt8(0);
    }

    // we didn't find any zeroes, so return the last value
    return val;
}

// ---------------------------------------------------------------------------
//
// OR data source
//

OutputManager::SourceVal OutputManager::OrSource::Calc()
{
    // return the last non-zero source value
    for (auto &source : sources)
    {
        // calculate this value; if non-zero, stop now with this value
        // as the result; otherwise just ignore it and keep looking
        SourceVal val = source->Calc();
        if (val.AsUInt8() != 0)
            return val;
    }

    // no non-zero value was found, so return zero
    return SourceVal::MakeUInt8(0);
}

// ---------------------------------------------------------------------------
//
// Max data source
//

OutputManager::SourceVal OutputManager::MaxSource::Calc()
{
    // return the max value, interpreting as UINT8
    uint8_t maxVal = 0;
    for (auto &source : sources)
    {
        uint8_t cur = source->Calc().AsUInt8();
        if (cur > maxVal)
            maxVal = cur;
    }
    return SourceVal::MakeUInt8(maxVal);
}

// ---------------------------------------------------------------------------
//
// Min data source
//

OutputManager::SourceVal OutputManager::MinSource::Calc()
{
    // return the minimum value, interpreting as UINT8
    uint8_t minVal = 0;
    int i = 0;
    for (auto &source : sources)
    {
        uint8_t cur = source->Calc().AsUInt8();
        if (i++ == 0 || cur < minVal)
            minVal = cur;
    }
    return SourceVal::MakeUInt8(minVal);
}

// ---------------------------------------------------------------------------
//
// IF (conditional) data source
//

OutputManager::IfSource::IfSource(DataSourceArgs &args)
{
    // get the if-then pairs
    for (int i = args.args.size() ; i >= 2 ; i -= 2)
        ifThens.emplace_back(args);

    // get the final 'else' source
    elseSource = args.GetSource();
}

void OutputManager::IfSource::Traverse(TraverseFunc func)
{
    for (auto &i : ifThens)
    {
        func(i.ifSource);
        func(i.thenSource);
    }
    func(elseSource);
}

OutputManager::SourceVal OutputManager::IfSource::Calc()
{
    for (auto &i : ifThens)
    {
        if (i.ifSource->Calc().AsUInt8() != 0)
            return i.thenSource->Calc();
    }
    return elseSource->Calc();
}

// ---------------------------------------------------------------------------
//
// SELECT (C-like switch/case) data source
//

OutputManager::SourceVal OutputManager::SelectSource::Calc()
{
    // evaluate the control condition
    int ctl = controlSource->Calc().AsUInt8();

    // process value/result pairs until we find a match
    for (auto it = sources.begin() ; it != sources.end() ; ++it)
    {
        // evaluate this value
        int cur = (*it)->Calc().AsUInt8();

        // advance to the corresponding result value; stop if it's missing
        ++it;
        if (it == sources.end())
            break;

        // if the current matches the control, yield the result value
        if (cur == ctl)
            return (*it)->Calc();
    }

    // no match to any of the case values; yield the default value
    return defaultSource->Calc();
}

// ---------------------------------------------------------------------------
//
// Clipping data source
//

OutputManager::SourceVal OutputManager::ClipSource::Calc()
{
    // calculate the source value as a UINT8
    uint8_t val = source->Calc().AsUInt8();

    // return the clipped value
    return SourceVal::MakeUInt8(val < lo ? lo : val > hi ? hi : val);
}

// ---------------------------------------------------------------------------
//
// Identical/Not Identical conditional sources
//

OutputManager::SourceVal OutputManager::IdentSource::Calc()
{
    auto a = lhs->Calc();
    auto b = rhs->Calc();
    if (a.type != b.type)
        return SourceVal::MakeUInt8(0);

    switch (a.type)
    {
    case SourceVal::Type::UInt8:
        return SourceVal::MakeUInt8(a.i == b.i ? 255 : 0);

    case SourceVal::Type::Float:
        return SourceVal::MakeUInt8(a.f == b.f ? 255 : 0);

    case SourceVal::Type::RGB:
        return SourceVal::MakeUInt8(a.rgb.r == b.rgb.r && a.rgb.g == b.rgb.g && a.rgb.b == b.rgb.b ? 255 : 0);

    case SourceVal::Type::Vector:
        return SourceVal::MakeUInt8(a.vec.x == b.vec.x && a.vec.y == b.vec.y ? 255 : 0);

    default:
        return SourceVal::MakeUInt8(0);
    }
}

OutputManager::SourceVal OutputManager::NIdentSource::Calc()
{
    // calculate the === result, then logically invert it (0 -> 255, 255 -> 0)
    SourceVal val = IdentSource::Calc();
    val.i = 255 - val.i;
    return val;
}

// ---------------------------------------------------------------------------
//
// Math operator sources
//

OutputManager::SourceVal OutputManager::AddSource::Calc()
{
    SourceVal a = lhs->Calc(), b = rhs->Calc();
    switch (a.type)
    {
    case SourceVal::Type::UInt8:
        return SourceVal::MakeUInt8(SourceVal::ClipU8(a.i + b.AsUInt8()));

    case SourceVal::Type::Float:
        return SourceVal::MakeFloat(a.f + b.AsFloat());

    case SourceVal::Type::Vector:
        if (b.type == a.type)
        {
            // vector + vector - add the components
            return SourceVal::MakeVector(SourceVal::ClipI16(a.vec.x + b.vec.x), SourceVal::ClipI16(a.vec.y + b.vec.y));
        }
        else
        {
            // vector + anything else - convert to scalar and add as an offset to each vector component
            float f = b.AsFloat();
            return SourceVal::MakeVector(SourceVal::ClipI16(a.vec.x + f), SourceVal::ClipI16(a.vec.y + f));
        }

    case SourceVal::Type::RGB:
        if (b.type == a.type)
        {
            // RGB + RGB - add the components
            return SourceVal::MakeRGB(
                SourceVal::ClipU8(a.rgb.r + b.rgb.r),
                SourceVal::ClipU8(a.rgb.g + b.rgb.g),
                SourceVal::ClipU8(a.rgb.b + b.rgb.b));
        }
        else
        {
            // RGB + anything else - convert to scalar and add as an offset to each component
            uint8_t i = b.AsUInt8();
            return SourceVal::MakeRGB(
                SourceVal::ClipU8(a.rgb.r + i),
                SourceVal::ClipU8(a.rgb.g + i),
                SourceVal::ClipU8(a.rgb.b + i));
        }

    default:
        return a;
    }
}

OutputManager::SourceVal OutputManager::SubtractSource::Calc()
{
    SourceVal a = lhs->Calc(), b = rhs->Calc();
    switch (a.type)
    {
    case SourceVal::Type::UInt8:
        return SourceVal::MakeUInt8(SourceVal::ClipU8(static_cast<int>(a.i) - b.AsUInt8()));

    case SourceVal::Type::Float:
        return SourceVal::MakeFloat(a.f - b.AsFloat());

    case SourceVal::Type::Vector:
        if (b.type == a.type)
        {
            // vector - vector - subtract the components
            return SourceVal::MakeVector(SourceVal::ClipI16(a.vec.x - b.vec.x), SourceVal::ClipI16(a.vec.y - b.vec.y));
        }
        else
        {
            // vector - anything else - convert to scalar and subtract as an offset to each vector component
            float f = b.AsFloat();
            return SourceVal::MakeVector(SourceVal::ClipI16(a.vec.x - f), SourceVal::ClipI16(a.vec.y - f));
        }

    case SourceVal::Type::RGB:
        if (b.type == a.type)
        {
            // RGB - RGB - subtract the components
            return SourceVal::MakeRGB(
                SourceVal::ClipU8(static_cast<int>(a.rgb.r) - b.rgb.r),
                SourceVal::ClipU8(static_cast<int>(a.rgb.g) - b.rgb.g),
                SourceVal::ClipU8(static_cast<int>(a.rgb.b) - b.rgb.b));
        }
        else
        {
            // RGB - anything else - convert to scalar and subtract as an offset to each component
            uint8_t i = b.AsUInt8();
            return SourceVal::MakeRGB(
                SourceVal::ClipU8(static_cast<int>(a.rgb.r) - i),
                SourceVal::ClipU8(static_cast<int>(a.rgb.g) - i),
                SourceVal::ClipU8(static_cast<int>(a.rgb.b) - i));
        }

    default:
        return a;
    }
}

OutputManager::SourceVal OutputManager::MulSource::Calc()
{
    SourceVal a = lhs->Calc(), b = rhs->Calc();
    switch (a.type)
    {
    case SourceVal::Type::UInt8:
        return SourceVal::MakeUInt8(SourceVal::ClipU8(static_cast<int>(a.i) * b.AsUInt8()));

    case SourceVal::Type::Float:
        return SourceVal::MakeFloat(a.f * b.AsFloat());

    case SourceVal::Type::Vector:
        if (b.type == a.type)
        {
            // vector * vector - take dot product
            return SourceVal::MakeFloat(a.vec.x*b.vec.x + a.vec.y*b.vec.y);
        }
        else
        {
            // vector * anything else - convert to scalar and multiply the components
            float f = b.AsFloat();
            return SourceVal::MakeVector(SourceVal::ClipI16(a.vec.x * f), SourceVal::ClipI16(a.vec.y * f));
        }

    case SourceVal::Type::RGB:
    default:
        // RGB/other multiplication makes no sense; just return the lhs
        return a;
    }
}

OutputManager::SourceVal OutputManager::DivSource::Calc()
{
    SourceVal a = lhs->Calc(), b = rhs->Calc();
    switch (a.type)
    {
    case SourceVal::Type::UInt8:
        return SourceVal::MakeUInt8(SourceVal::ClipU8(static_cast<int>(a.i) / b.AsUInt8()));

    case SourceVal::Type::Float:
        return SourceVal::MakeFloat(a.f / b.AsFloat());

    case SourceVal::Type::Vector:
        // vector / value -> convert rhs to scalar and divide the components
        {
            float f = b.AsFloat();
            return SourceVal::MakeVector(SourceVal::ClipI16(a.vec.x / f), SourceVal::ClipI16(a.vec.y / f));
        }

    case SourceVal::Type::RGB:
    default:
        // RGB/other division makes no sense; just return the lhs
        return a;
    }
}

OutputManager::SourceVal OutputManager::ModuloSource::Calc()
{
    SourceVal a = lhs->Calc(), b = rhs->Calc();
    switch (a.type)
    {
    case SourceVal::Type::UInt8:
        return SourceVal::MakeUInt8(SourceVal::ClipU8(static_cast<int>(a.i) % b.AsUInt8()));

    case SourceVal::Type::Float:
        return SourceVal::MakeFloat(fmodf(a.f, b.AsFloat()));

    case SourceVal::Type::Vector:
        // vector % value -> convert rhs to scalar and modulo the components
        {
            float f = b.AsFloat();
            return SourceVal::MakeVector(SourceVal::ClipI16(fmodf(a.vec.x, f)), SourceVal::ClipI16(fmodf(a.vec.y, f)));
        }

    case SourceVal::Type::RGB:
    default:
        // RGB/other modulo makes no sense; just return the lhs
        return a;
    }
}

// ---------------------------------------------------------------------------
//
// Data Source variant value type
//

uint8_t OutputManager::SourceVal::AsUInt8() const
{
    switch (type)
    {
    case Type::UInt8:
        // no conversion needed
        return i;

    case Type::Float:
        // coerce float to uint8_t, clipping to the uint8_t range
        return f < 0 ? 0 : f > 255 ? 255 : static_cast<uint8_t>(f);

    case Type::RGB:
        // convert the RGB to an equivalent grayscale value
        return Grayscale(rgb.r, rgb.g, rgb.b);

    case Type::Vector:
        // return the vector magnitude
        {
            float m = sqrtf(vec.x*vec.x + vec.y*vec.y);
            return m > 255.0 ? 255 : static_cast<uint8_t>(m);
        }

    default:
        // unknown type; return 0
        return 0;
    }
}

float OutputManager::SourceVal::AsFloat() const
{
    switch (type)
    {
    case Type::UInt8:
        // coerce to float; uint8_t always fits in the float range, so no range checking is necessary
        return static_cast<float>(i);

    case Type::Float:
        // no conversion necessary
        return f;

    case Type::RGB:
        // convert the RGB to an equivalent grayscale value
        return static_cast<float>(Grayscale(rgb.r, rgb.g, rgb.b));

    case Type::Vector:
        // return the vector magnitude
        return sqrtf(vec.x*vec.x + vec.y*vec.y);

    default:
        // unknown type; return 0
        return 0;
    }
}

OutputManager::SourceVal OutputManager::SourceVal::AsRGB() const
{
    switch (type)
    {
    case Type::UInt8:
        // treat the integer value as a grayscale level
        return MakeRGB(i);

    case Type::Float:
        // treat the float as a grayscale level, clipping to 0..255
        {
            uint8_t i = static_cast<uint8_t>(f < 0 ? 0 : f > 255 ? 255 : f);
            return MakeRGB(i);
        }

    case Type::Vector:
        // treat the vector magnitude as the grayscale level, clipping
        {
            float m = sqrtf(vec.x*vec.x + vec.y*vec.y);
            return MakeRGB(m > 255.0 ? 255 : static_cast<uint8_t>(m));
        }

    case Type::RGB:
        // no conversion necessary
        return *this;

    default:
        // unknown type; return RGB with all components set to zero
        return MakeRGB(0, 0, 0);
    }
}

OutputManager::SourceVal OutputManager::SourceVal::AsVector() const
{
    switch (type)
    {
    case Type::UInt8:
        // treat the scalar as the vector x component
        return MakeVector(i, 0);

    case Type::Float:
        // treat the scalar as the vector x component
        return MakeVector(f < 0.0 ? 0 : f > 255.0 ? 255 : static_cast<uint8_t>(f), 0);

    case Type::RGB:
        // make a vector from the grayscale value
        return MakeVector(Grayscale(rgb.r, rgb.g, rgb.b), 0);

    case Type::Vector:
        // no conversion necessary
        return *this;

    default:
        // unknown type; use a zero-length vector
        return MakeVector(0, 0);
    }
}

// ---------------------------------------------------------------------------
//
// Data source argument list
//

uint8_t OutputManager::DataSourceArgs::GetU8()
{
    // get the next argument as a constant node
    SourceVal v;
    if (!GetConstant(v))
        return 0;

    // range-check it if it's a float
    if (v.type == SourceVal::Type::Float && (v.f < 0 || v.f > 255))
        errors.emplace_back(argi - 1, "integer value out of range, must be 0..255");

    // coerce to int (which in this context means uint8_t)
    return v.AsUInt8();
}

int OutputManager::DataSourceArgs::GetInt()
{
    // get the next argument as a constant node
    SourceVal v;
    if (!GetConstant(v))
        return 0;

    // coerce to int, checking range if it's expressed as a float
    if (v.type == SourceVal::Type::Float)
    {
        // log an error if it's out of the native int range
        if (v.f < INT_MIN || v.f > INT_MAX)
            errors.emplace_back(argi - 1, "integer value out of range, must fit 32-bit int (-2147483648..2147483647)");

        // coerce the float value to int
        return static_cast<int>(v.f);
    }
    else
    {
        // anything else can be coerced implicitly
        return v.AsUInt8();
    }
}

float OutputManager::DataSourceArgs::GetFloat()
{
    // get the next argument as a constant node
    SourceVal v;
    if (!GetConstant(v))
        return 0;

    // coerce to float (this is the widest type a SourceVal can represent,
    // so no range checking is required)
    return v.f;
}

bool OutputManager::DataSourceArgs::GetConstant(SourceVal &v)
{
    // get the next argument as a data source
    std::unique_ptr<DataSource> source(GetSource());
    if (source == nullptr)
        return false;
    
    // downcast it to ConstantSource
    ConstantSource *c = source->AsConstantSource();
    if (c == nullptr)
    {
        errors.emplace_back(argi - 1, "constant value required");
        return false;
    }

    // success - retrieve the constant value
    v = c->val;
    return true;
}

bool OutputManager::DataSourceArgs::TryGetString(std::string &str)
{
    // if there's a next argument, and it's a constant node, and
    // the constant node contains a string, it's a string
    if (args.size() != 0)
    {
        // check if it's a constant node with a string
        if (auto *c = args.front()->AsConstantSource() ; c != nullptr && c->str.size() != 0)
        {
            // it's a string - fill in the string value
            str = c->str;

            // consume the argument
            args.pop_front();
            ++argi;

            // return success
            return true;
        }
    }

    // it's not a string - return false without consuming an argument
    return false;
}


OutputManager::DataSource *OutputManager::DataSourceArgs::GetSource()
{
    // make sure another argument is available
    if (args.size() == 0)
    {
        errors.emplace_back(argi, "missing argument");
        return nullptr;
    }

    // count the argument consumed
    ++argi;

    // release the DataSource pointer to the caller's ownership,
    // and pop it off the list
    DataSource *source = args.front().release();
    args.pop_front();
    return source;
}

std::list<OutputManager::DataSource*> OutputManager::DataSourceArgs::GetAll(int exceptN)
{
    // make a list of the remaining arguments except for the last
    // 'exceptN'
    std::list<OutputManager::DataSource*> ret;
    while (args.size() > exceptN)
    {
        // release the pointer to the caller's ownership
        ret.emplace_back(args.front().release());
        args.pop_front();
        ++argi;
    }

    // return the list
    return ret;
}

// ---------------------------------------------------------------------------
//
// Data Source Parser
//

// top-level parser - comparison expressions
OutputManager::DataSource *OutputManager::ParseSource(int index, const char* &p, const char *start, const char *end)
{
    // parse a comparison expression
    static const BinOp compOp{
        "comparison operator (= == === != !== < <= > >=)",
        &OutputManager::ParseSourceComparable,
        {
            { "==", [](){ return new EqSource(); } },
            { "===", [](){ return new IdentSource(); } },
            { "!=", [](){ return new NeSource(); } },
            { "<>", [](){ return new EqSource(); } },
            { "!==", [](){ return new NIdentSource(); } },
            { "!<>", [](){ return new NIdentSource(); } },
            { "<", [](){ return new LtSource(); } },
            { "<=", [](){ return new LeSource(); } },
            { ">", [](){ return new GtSource(); } },
            { ">=", [](){ return new GeSource(); } },
        }
    };
    return ParseSourceBinaryExpr(index, p, start, end, compOp);
}

// term expression
OutputManager::DataSource *OutputManager::ParseSourceComparable(int index, const char* &p, const char *start, const char *end)
{
    // parse an additive expression
    static const BinOp addOp{
        "addition operator (+ -)",
        &OutputManager::ParseSourceTerm,
        {
            { "+", [](){ return new AddSource(); } },
            { "-", [](){ return new SubtractSource(); } },
        }
    };
    return ParseSourceBinaryExpr(index, p, start, end, addOp);
}

// factor expression
OutputManager::DataSource *OutputManager::ParseSourceTerm(int index, const char* &p, const char *start, const char *end)
{
    // parse a multiplicative expression
    static const BinOp mulOp{
        "multiplicative operator (* / %)",
        &OutputManager::ParseSourcePostfix,
        {
            { "*", [](){ return new MulSource(); } },
            { "/", [](){ return new DivSource(); } },
            { "%", [](){ return new ModuloSource(); } },
        }
    };
    return ParseSourceBinaryExpr(index, p, start, end, mulOp);
}

// Binary operator table: scan for a match to operator text
bool OutputManager::BinOp::Match(const char *op, int opLen) const
{
    // scan the operator list for a match
    for (auto &ele : ops)
    {
        // if it matches, return success
        if (strlen(ele.op) == opLen && memcmp(ele.op, op, opLen) == 0)
            return true;
    }

    // not found
    return false;
}

// Binary operator table: apply the selected operator
OutputManager::DataSource *OutputManager::BinOp::Apply(const char *op, int opLen, DataSource *lhs, DataSource *rhs) const
{
    // scan the operator list for a match
    for (auto &ele : ops)
    {
        // if it matches, apply the operator
        if (strlen(ele.op) == opLen && memcmp(ele.op, op, opLen) == 0)
        {
            // construct the new binary operator source object, populate it, and return it
            BinOpSource *source = ele.create();
            source->lhs = lhs;
            source->rhs = rhs;
            return source;
        }
    }

    // no match
    return nullptr;
}

OutputManager::DataSource *OutputManager::ParseSourceBinaryExpr(
    int index, const char* &p, const char *start, const char *end, const BinOp &binOp)
{
    // parse the left-hand side
    std::unique_ptr<DataSource> lhs(binOp.Parse(index, p, start, end));
    if (lhs == nullptr)
        return nullptr;

    // now keep going until we run out of comparison operator expressions
    for (;;)
    {
        // skip spaces; if there's nothing left after that, we're done; the current lhs
        // is the whole expression
        for ( ; p < end && isspace(*p) ; ++p) ;
        if (p == end)
            return lhs.release();

        // Parse the operator.  Keep scanning for the longest match, so that
        // we find operators like "===", while still distinguishing two
        // distinct operators with no spaces between, like "3+-1".  Scan
        // for the longest match we can find.
        int longestOpMatch = 0;
        for (int ofs = 0 ; p + ofs < end && strchr("=!<>+-*/~%^&|[]", p[ofs]) != nullptr ; ++ofs)
        {
            // test for a match to an operator, and note the longest match so far
            int curLen = ofs + 1;
            if (binOp.Match(p, curLen))
                longestOpMatch = curLen;
        }

        // if we didn't find any matches, this isn't one of our operators,
        // so hand it back to the enclosing layer to handle
        if (longestOpMatch == 0)
            return lhs.release();

        // skip the operator
        const char *op = p;
        p += longestOpMatch;

        // parse the right-hand side
        std::unique_ptr<DataSource> rhs(binOp.Parse(index, p, start, end));
        if (rhs == nullptr)
            return  nullptr;

        // build the source node for the expression
        lhs.reset(binOp.Apply(op, longestOpMatch, lhs.release(), rhs.release()));
    }
}

// Data source parser - postfix expression
//
//   <postfix> . <property>
//   <primary>
//
OutputManager::DataSource *OutputManager::ParseSourcePostfix(int index, const char* &p, const char *start, const char *end)
{
    // parse a primary
    std::unique_ptr<DataSource> expr(ParseSourcePrimary(index, p, start, end));
    if (expr == nullptr)
        return nullptr;

    // now apply postfixes iteratively
    for (;;)
    {
        // skip spaces
        for ( ; p < end && isspace(*p) ; ++p) ;

        // check for a postfix operator
        switch (*p)
        {
        case '.':
            {
                // property postfix - skip the '.' and any spaces that follow
                for (++p ; p < end && isspace(*p) ; ++p) ;

                // a property name token must follow
                int col = p - start;
                if (!(isalpha(*p) || *p == '_' || *p == '$'))
                {
                    Log(LOG_ERROR, "outputs[%d].source, col %d: expected property name after '.'\n", index, col);
                    return nullptr;
                }

                // parse the property name token
                const char *namep = p;
                for ( ; p < end && (isalpha(*p) || isdigit(*p) || *p == '_' || *p == '$') ; ++p);
                std::string propName(namep, p - namep);

                // look it up
                typedef DataSource *Initer(DataSource*);
                static const std::unordered_map<std::string, Initer*> propMap = {
                    { "x",         [](DataSource *expr) -> DataSource* { return new XSource(expr); } },
                    { "y",         [](DataSource *expr) -> DataSource* { return new YSource(expr); } },
                    { "magnitude", [](DataSource *expr) -> DataSource* { return new MagnitudeSource(expr); } },
                    { "red",       [](DataSource *expr) -> DataSource* { return new RedSource(expr); } },
                    { "r",         [](DataSource *expr) -> DataSource* { return new RedSource(expr); } },
                    { "green",     [](DataSource *expr) -> DataSource* { return new GreenSource(expr); } },
                    { "g",         [](DataSource *expr) -> DataSource* { return new GreenSource(expr); } },
                    { "blue",      [](DataSource *expr) -> DataSource* { return new BlueSource(expr); } },
                    { "b",         [](DataSource *expr) -> DataSource* { return new BlueSource(expr); } },
                };
                auto it = propMap.find(propName);
                if (it == propMap.end())
                {
                    Log(LOG_ERROR, "outputs[%d].source, col %d: unknown property name '.%s'\n", index, col, propName.c_str());
                    return nullptr;
                }

                // compose the property expression
                expr.reset(it->second(expr.release()));
            }

            // done - continue on to check for another postfix
            break;

        default:
            // anything else is not a postfix operator, so return what we have
            return expr.release();
        }
    }
}

// Data source parser - primary expression
//
//   <number>           - numeric constant, integer or float
//   #<number>          - equivalent to port(<number>)
//   'name'             - named port
//   func(<params>)     - function with optional parameters, comma-delimited
//   ( <expr> )         - a parenthesized expression
//
OutputManager::DataSource *OutputManager::ParseSourcePrimary(int index, const char* &p, const char *start, const char *end)
{
    // skip leading spaces
    for ( ; p < end && isspace(*p) ; ++p) ;

    // if the string was empty, silently return a null source
    if (p == end)
        return nullptr;

    // check the token type
    if (isdigit(*p) || *p == '-' || *p == '+')
    {
        // numeric constant - use the JSON parser's handy number token lexer
        JSONParser::Token tok;
        int col = p - start;
        JSONParser::TokenizerState ts(p, end);
        if (!JSONParser::ParseNumberToken(tok, ts) || tok.type != JSONParser::Token::Type::Number)
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: syntax error in numeric constant\n", index, col);
            return nullptr;
        }

        // advance past the number token
        p = ts.src;

        // Return a constant node.  If the number parsed out to a whole
        // number in the uint8_t range, interpret it as an integer value,
        // otherwise as a float.
        double n = tok.num, intpart;
        if (n < -FLT_MAX || n > FLT_MAX)
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: numeric value is out of range (%.f to %.f)\n",
                index, col, -FLT_MAX, FLT_MAX);
            return nullptr;
        }
        else if (modf(n, &intpart) == 0 && n >= 0 && n <= 255)
            return new ConstantSource(SourceVal::MakeUInt8(static_cast<uint8_t>(n)));
        else
            return new ConstantSource(SourceVal::MakeFloat(static_cast<float>(n)));
    }
    else if (*p == '\'' || *p == '"')
    {
        // string
        int col = p - start;
        const char quote = *p++;
        const char *namep = p;
        for ( ; p < end && *p != quote ; ++p);
        if (p == end)
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: unterminated string\n", index, col);
            return nullptr;
        }

        // skip the close quote
        ++p;

        // add a string node
        return new ConstantSource(namep, p - namep - 1);
    }
    else if (*p == '#')
    {
        // port by number - parse the number
        const char *nump = ++p;
        int col = nump - start;
        for ( ; p < end && isdigit(*p) ; ++p);
        if (p == nump)
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: port number expected after '#'\n", index, col);
            return nullptr;
        }

        // validate the port number
        int portNum = atoi(nump);
        Port *port = Get(portNum);
        if (port == nullptr)
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: invalid port number %d\n", index, col, portNum);
            return nullptr;
        }

        // Return a port source.  Use raw mode if it's a self-reference,
        // otherwise use the computed value.
        return new PortSource(port, port == Get(index+1));
    }
    else if (isalpha(*p))
    {
        // Anything else starting with a symbol character uses the
        // func(<params>) notation.  Start by pulling out the function
        // name token.
        const char *namep = p;
        int col = namep - start;
        for ( ; p < end && (isalpha(*p) || isdigit(*p) || *p == '_' || *p == '$') ; ++p);
        std::string funcName(namep, p - namep);

        // skip spaces and check for a '(' token
        DataSourceArgs args(Get(index + 1));
        for ( ; p < end && isspace(*p) ; ++p);
        if (p < end && *p == '(')
        {
            // Open paren found - parse the argument list
            for (++p ; ; )
            {
                // skip spaces; stop on end or ')'
                for ( ; p < end && isspace(*p) ; ++p) ;
                if (p == end || *p == ')')
                    break;

                // Parse an argument.  If that fails, it will log an error
                // and return null.  We can just return null ourselves, since
                // the error in the recursive parse was already logged.
                DataSource *arg = ParseSource(index, p, start, end);
                if (arg == nullptr)
                    return nullptr;

                // add it to our list
                args.args.emplace_back(arg);

                // skip spaces, check for ',' or ')'
                for ( ; p < end && isspace(*p) ; ++p) ;
                if (p == end || *p == ')')
                {
                    // end of arguments
                    break;
                }
                else if (*p == ',')
                {
                    // another argument follows - skip the ',' and continue
                    ++p;
                }
                else
                {
                    col = p - start;
                    Log(LOG_ERROR, "outputs[%d].source, col %d: expected ',' or ')' in argument list\n", index, col);
                    return nullptr;
                }
            }

            // make sure we ended on ')'
            if (p == end || *p != ')')
            {
                col = p - start;
                Log(LOG_ERROR, "outputs[%d].source, col %d: missing ')' in argument list\n", index, col);
                return nullptr;
            }

            // skip the ')'
            ++p;
        }
        else
        {
            // No '(', so there's no argument list.  Treat it as a
            // function with zero arguments.
        }

        // find the function name
        typedef DataSource *Initer(DataSourceArgs&);
        static const std::unordered_map<std::string, Initer*> funcMap = {
            { "port",        [](DataSourceArgs &args) -> DataSource* { return new PortSource(args, false); } },
            { "rawport",     [](DataSourceArgs &args) -> DataSource* { return new PortSource(args, true); } },

            // note that 'self's constructor dosn't use the argument vector
            { "self",        [](DataSourceArgs &args) -> DataSource* { return new PortSource(args.self, true); } },
            
            { "tvon",        [](DataSourceArgs &args) -> DataSource* { return new TVONSource(args); } },
            { "powersense",  [](DataSourceArgs &args) -> DataSource* { return new PowerSenseSource(args); } },
            { "irtx",        [](DataSourceArgs &args) -> DataSource* { return new IRTXSource(args); } },
            { "irrx",        [](DataSourceArgs &args) -> DataSource* { return new IRRXSource(args); } },
            { "nudge",       [](DataSourceArgs &args) -> DataSource* { return new NudgeDeviceSource(args); } },
            { "plungerpos",  [](DataSourceArgs &args) -> DataSource* { return new PlungerPosSource(args); } },
            { "plungerposf", [](DataSourceArgs &args) -> DataSource* { return new PlungerPosFloatSource(args); } },
            { "xboxled",     [](DataSourceArgs &args) -> DataSource* { return new XBoxLedSource(args); } },
            { "xboxrumble",  [](DataSourceArgs &args) -> DataSource* { return new XBoxRumbleSource(args); } },
            { "arctan",      [](DataSourceArgs &args) -> DataSource* { return new ArctanSource(args); } },
            { "x",           [](DataSourceArgs &args) -> DataSource* { return new XSource(args); } },
            { "y",           [](DataSourceArgs &args) -> DataSource* { return new YSource(args); } },
            { "magnitude",   [](DataSourceArgs &args) -> DataSource* { return new MagnitudeSource(args); } },
            { "button",      [](DataSourceArgs &args) -> DataSource* { return new ButtonSource(args); } },
            { "zblaunch",    [](DataSourceArgs &args) -> DataSource* { return new ZBLaunchButtonSource(args); } },
            { "zblaunchmode",[](DataSourceArgs &args) -> DataSource* { return new ZBLaunchModeSource(args); } },
            { "blink",       [](DataSourceArgs &args) -> DataSource* { return new BlinkSource(args); } },
            { "ramp",        [](DataSourceArgs &args) -> DataSource* { return new RampSource(args); } },
            { "sine",        [](DataSourceArgs &args) -> DataSource* { return new SineSource(args); } },
            { "sawtooth",    [](DataSourceArgs &args) -> DataSource* { return new SawtoothSource(args); } },
            { "scale",       [](DataSourceArgs &args) -> DataSource* { return new ScaleSource(args); } },
            { "offset",      [](DataSourceArgs &args) -> DataSource* { return new OffsetSource(args); } },
            { "abs",         [](DataSourceArgs &args) -> DataSource* { return new AbsSource(args); } },
            { "grayscale",   [](DataSourceArgs &args) -> DataSource* { return new GrayscaleSource(args); } },
            { "hue",         [](DataSourceArgs &args) -> DataSource* { return new HueSource(args); } },
            { "saturation",  [](DataSourceArgs &args) -> DataSource* { return new SaturationSource(args); } },
            { "brightness",  [](DataSourceArgs &args) -> DataSource* { return new BrightnessSource(args); } },
            { "hsb",         [](DataSourceArgs &args) -> DataSource* { return new HSBSource(args); } },
            { "red",         [](DataSourceArgs &args) -> DataSource* { return new RedSource(args); } },
            { "green",       [](DataSourceArgs &args) -> DataSource* { return new GreenSource(args); } },
            { "blue",        [](DataSourceArgs &args) -> DataSource* { return new BlueSource(args); } },
            { "and",         [](DataSourceArgs &args) -> DataSource* { return new AndSource(args); } },
            { "or",          [](DataSourceArgs &args) -> DataSource* { return new OrSource(args); } },
            { "max",         [](DataSourceArgs &args) -> DataSource* { return new MaxSource(args); } },
            { "min",         [](DataSourceArgs &args) -> DataSource* { return new MinSource(args); } },
            { "if",          [](DataSourceArgs &args) -> DataSource* { return new IfSource(args); } },
            { "select",      [](DataSourceArgs &args) -> DataSource* { return new SelectSource(args); } },
            { "clip",        [](DataSourceArgs &args) -> DataSource* { return new ClipSource(args); } },
            { "nightmode",   [](DataSourceArgs &args) -> DataSource* { return new NightModeSource(args); } },
            { "plungercal",  [](DataSourceArgs &args) -> DataSource* { return new PlungerCalSource(args); } },
            { "statusled",   [](DataSourceArgs &args) -> DataSource* { return new StatusLedSource(args); } },
            { "time",        [](DataSourceArgs &args) -> DataSource* { return TimeRangeSource::ParseArgs(args); } },
        };
        auto it = funcMap.find(funcName);
        if (it == funcMap.end())
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: unknown function name '%s'\n", index, col, funcName.c_str());
            return nullptr;
        }

        // Matched - call the initer to create the source object
        std::unique_ptr<DataSource> newSource(it->second(args));

        // check for argument type match errors
        if (args.errors.size() != 0)
        {
            // log all errors
            for (auto &err : args.errors)
                Log(LOG_ERROR, "outputs[%d].source, col %d: %s(), argument %d: %s\n", index, col, funcName.c_str(), err.argi, err.msg);
            return nullptr;
        }

        // make sure that all arguments were consumed
        if (args.args.size() != 0)
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: %s(): too many arguments\n", index, col, funcName.c_str());
            return nullptr;
        }

        // success - return the new data source
        return newSource.release();
    }
    else if (*p == '(')
    {
        // note the paren column and skip the open paren
        int parenCol = p - start;
        ++p;

        // parse the enclosed expression; if that fails, it already logged an error, so just bubble up the failure
        std::unique_ptr<DataSource> source(ParseSource(index, p, start, end));
        if (source == nullptr)
            return nullptr;

        // skip spaces and parse the close paren
        for ( ; p < end && isspace(*p) ; ++p);
        if (p == end || *p != ')')
        {
            Log(LOG_ERROR, "outputs[%d].source, col %d: unmatched '('\n", index, parenCol);
            return nullptr;
        }

        // success - return the subexpression
        return source.release();
    }
    else
    {
        // invalid token
        int col = p - start;
        Log(LOG_ERROR, "outputs[%d].source, col %d: invalid token character '%c'\n", index, col, *p);
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
//
// Console commands
//

void OutputManager::Command_out(const ConsoleCommandContext *c)
{
    // make sure we have some arguments
    if (c->argc == 1)
        return c->Usage();

    // process arguments
    bool changesWhileSuspended = false;
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0)
        {
            // list ports
            c->Print("Port  Name              Device      Source     Setting  Output\n");
            int portNum = 1;
            for (auto &port : portList)
            {
                // figure the "setting" column, depending on mode
                char setting[12];
                if (port.GetDataSource() == nullptr && port.IsLedWizMode())
                    sprintf(setting, "%3d/%s", port.GetLedWizProfileState(), port.GetLedWizOnState() ? "On" : "Off");
                else
                    sprintf(setting, "%3d", port.GetDOFLevel());

                // display the line
                c->Printf(
                    " %3d  %-16.16s  %-10.10s  %-8s   %-8s    %3d\n",
                    portNum++, port.GetName(), port.GetDevice()->Name(),
                    port.GetDataSource() != nullptr ? "Computed" : port.IsLedWizMode() ? "LedWiz" : "Host",
                    setting, port.Get());
            }

            // make a note of suspended status
            if (isSuspended)
                c->Print("(Output management is suspended; use --resume to re-enable)\n");
        }
        else if (strcmp(a, "-O") == 0 || strcmp(a, "--all-off") == 0)
        {
            // set all ports to level 0
            AllOff();
            c->Print("All ports off (level 0)\n");
        }
        else if (strcmp(a, "--suspend") == 0)
        {
            if (!isSuspended)
                SuspendIfActive(c);
            else
                c->Print("Output management is suspended\n");
        }
        else if (strcmp(a, "--resume") == 0)
        {
            if (isSuspended)
            {
                Resume();
                c->Print("Output management resumed\n");
            }
            else
                c->Print("Output management is active\n");
        }
        else if (a[0] == '-')
        {
            return c->Printf("Invalid option \"%s\"\n", a);
        }
        else
        {
            // Anything else is a <port>=<level> command

            // scan for the '='
            const char *eq = strchr(a, '=');
            if (eq == nullptr)
                return c->Printf("out: expected <port>=<level>, found \"%s\"\n", a);

            // try parsing the port number portion
            int portNum = 0;
            const char *portName = a;
            for ( ; isdigit(*a) ; ++a)
                portNum = (portNum * 10) + (*a - '0');

            // if we ended at '=', it's all digits, so try interpreting it as
            // a port number first
            Port *port = nullptr;
            if (*a == '=')
                port = Get(portNum);

            // if that didn't work, try interpreting the part up to '=' as a port name
            if (port == nullptr)
            {
                // Look up the port by name - we have to make a local copy to null-terminate it.
                // This imposes a length limit on the name that we could avoid simply by making
                // this a std::string construction, but that would allocate memory (always to be
                // avoided during main loop operation), and I don't think anyone's ever going
                // to notice the length limit.
                char namebuf[64];
                memcpy(namebuf, portName, std::min(static_cast<int>(_countof(namebuf)), static_cast<int>(eq - portName)));
                namebuf[_countof(namebuf)-1] = 0;
                port = Get(namebuf);
                if (port == nullptr)
                    return c->Printf("out: no such port name or number \"%s\"\n", namebuf);

                // find the port number
                portNum = 1;
                for (auto &pp : portList)
                {
                    if (&pp == port)
                        break;
                    ++portNum;
                }
            }

            // parse the level
            int level = 0;
            const char *levelp = eq + 1;
            for ( ; isdigit(*levelp) ; ++levelp)
                level = (level * 10) + (*levelp - '0');

            // make sure we used the whole string
            if (*levelp != 0 || level < 0 || level > 255)
                return c->Printf("out: invalid level \"%s\" (in \"%s\"), expected a number 0-255\n", eq + 1, c->argv[i]);

            // success - set the level
            port->SetDOFLevel(level);
            c->Printf("Port %d -> %d%s\n", portNum, level, isSuspended ? "   [Suspended]" : "");

            // note if we made changes while suspended
            if (isSuspended)
                changesWhileSuspended = true;
        }
    }

    // caution about changes while suspended
    if (changesWhileSuspended)
        c->Print("Note: logical output level changes were made while output management\n"
                 "was suspended; these won't affect the physical device outputs until\n"
                 "management resumes.\n");
}
