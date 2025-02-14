// Pinscape Pico firmware - Output Port Manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Logical output port interface.  A logical output port represents a
// port exposed to the host through the feedback controller USB
// interface.  From the host's perspective, each port is an individual
// PWM switch that can be set to a duty cycle from 0% to 100%, with
// 8-bit resolution (in other words, in discrete step increments from 0
// to 255, where 0 corresponds to 0% and 255 corresponds to 100%).  The
// host sees the ports as a collection of numbered slots; other than the
// port number, all ports look the same to the host, and the "meaning"
// of a port number to the host is handled separately by application
// software, such as DOF.
//
// On the device side, each logical port maps to a physical hardware
// switching circuit, such as a TLC59116 PWM chip port, or to a virtual
// output, such as the Night Mode control or ZB Launch control.  The
// mapping of a logical port to a physical or virtual output is handled
// through the configuration file, and is transparent to host
// application software; all ports (whether physical or virtual) present
// the same uniform PWM switch interface to the host.
//
// In addition to the PWM switch capability, each output port can also
// be configured with "flipper logic": a time limiter that forces the
// port OFF, or to a reduced power level, after the port has been on for
// the configured maximum time limit.  This is essentially transparent
// to host application software, other than configuration tools; the
// host application just turns the port on and off (or to intermediate
// PWM levels), and the firmware monitors the timing and intervenes as
// necessary if the configured time limit for a port expires.  The host
// software doesn't know (and doesn't have to know) that this is
// happening.  The point of this feature is to protect high-power coils
// from "locked on" conditions that can occur due to software faults on
// the host, by ensuring that coils that need such protection are cut
// off even if the host fails to turn them off for long periods.  That
// makes its transparency to the host an essential part of the design,
// in that the time limiter overrides whatever the host thinks the port
// should be doing.

#pragma once

// standard library headers
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <list>
#include <memory>
#include <functional>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"

// forward and external declarations
class PWMWorker;
class TLC59116;
class TLC5940;
class TLC5947;
class PCA9685;
class PCA9555;
class C74HC595;
class Button;
class ConsoleCommandContext;
class DateTime;
namespace PinscapePico {
    struct OutputPortDesc;
    struct OutputDevDesc;
    struct OutputDevPortDesc;
    struct OutputDevLevel;
}

class OutputManager
{
public:
    // forward internal classes
    class Device;
    class Port;
    
    // Configure outputs based on the JSON settings
    static void Configure(JSONParser &json);

    // Perform a configuration task on an unrelated subsystem that depends
    // upon output ports.  If the output manager has already been
    // configured, this invokes the callback immediately; otherwise it saves
    // the callback and invokes it after the output manager configuration is
    // completed.
    static void AfterConfigure(JSONParser &json, std::function<void(JSONParser &json)> callback);

    // Parse an output device configuration object.  jsonLocus is the JOSN
    // config location to report in logged error messages; pinoutLabel is
    // the name to report in host queries for the pin out diagram label
    // for the item.
    //
    // pwmDefault sets the default PWM mode for devices where PWM is
    // optional, such as GPIO ports.  If the JSON doesn't specify the PWM
    // mode explicitly, this is used as the default.
    //
    // If allowBareGPIO is true, the entire definition can be a number
    // naming a GPIO port, instead of an object.
    //
    // Returns nullptr if the definition is invalid.
    //
    static Device *ParseDevice(
        const char *jsonLocus, const char *pinoutLabel,
        JSONParser &json, const JSONParser::Value *deviceVal,
        bool pwmDefault, bool allowBareGPIO);

    // create a GPIO output device
    class GPIODev;
    static GPIODev *CreateGPIODev(
        const char *jsonLocus, const char *pinoutLabel,
        int gpNum, bool pwm, int freq);

    // Get the number of ports
    static size_t GetNumPorts() { return portList.size(); }

    // Get a port by number or name.  The port numbering corresponds to
    // the array order in the 'outputs' array in the JSON configuration
    // file, with the first port in the array assigned port #1.
    //
    // NOTE that the port numbering starts at port #1 (rather than the
    // usual C/C++ idiom of array index 0).  Almost all of the user-
    // facing tools on the PC side that deal with output controllers
    // number their ports starting at port #1.  In particular, that's
    // how the DOF Config Tool numbers ports, and that's practically the
    // only place where users have to connect something external back to
    // the numbering here.  It would be extremely annoying to almost
    // everyone (myself included) if you had to remember to adjust all
    // of the numbers up or down by one every time.
    static Port *Get(int n) { return n >= 1 && n < static_cast<int>(portsByNumber.size()) ? portsByNumber[n] : nullptr; }
    static Port *Get(const char *name) { auto it = portsByName.find(name); return (it != portsByName.end()) ? it->second : nullptr; }

    // Get a port by JSON reference.  The JSON value can be either a
    // port number or a string giving a port name; we'll do the
    // appropriate lookup based on the type.
    //
    // The JSONParser object must be passed as an argument as a reminder
    // to the developer coding a call here that this can only be called
    // from a configuration context, when a JSONParser object is
    // available.  In particular, we want to discourage callers from
    // stashing Value* pointers during configuration and using them
    // later, because the Value* pointers are only valid as long as the
    // containing JSONParser object is valid.  Hopefully the requirement
    // in the interface to pass the JSONParser object will discourage
    // misuse.  (If not, the resulting hard crash will sort it out for
    // the miscreant coder; we're just trying to save them the debugging
    // time.)
    static Port *Get(JSONParser&, const JSONParser::Value *val);

    // Set a port level.  Does nothing if the port doesn't exist.  This
    // sets the DOF level on the port.
    static void Set(int n, int level) { if (n >= 1 && n <= static_cast<int>(portsByNumber.size())) portsByNumber[n]->SetDOFLevel(level); }

    // Set a device port PWM level.  This should only be used when
    // output management is suspended, since the output manager will
    // countermand any setting made here on the next task handler call
    // if the device port is assigned to any logical output port.
    //
    // devType is a PinscapePico::OutputPortDesc::DEV_xxx constant.
    // This combines with the config index to identify the target
    // device.
    //
    // The port number is in terms to the physical device's native port
    // numbering; for most chips, the port number corresponds to the
    // output port labeling in the data sheet, such as the LED0..LED15
    // port labels for a TLC59116.
    //
    // The PWM level uses the device's native scale (0-255 for 8-bit
    // chips like TLC59116; 0-4095 for 12-bit chips like PCA9685 and
    // TLC5940; 0-1 for digital on/off ports, such as in shift registers
    // and GPIO extenders).
    //
    // To avoid intefering with the normal operation of other subsystems,
    // this interface only allows writing to a GPIO port that's assigned
    // to a logical output port.  It ignores writes to non-assigned GPIO
    // ports.
    static void SetDevicePortLevel(int devType, int configIndex, int port, int pwmLevel);

    // Set the LedWiz state for the port.  Setting the LedWiz state
    // overrides the host level setting, and setting the host level
    // overrides the LedWiz state, so whichever one was set last is
    // the one currently in effect.  'period' is the LedWiz protocol
    // waveform time period code, 1-7 (each unit represents 250ms).
    static void SetLedWizSBA(int portNum, bool on, uint8_t period);
    static void SetLedWizPBA(int portNum, uint8_t profile);

    // Set all ports off (level 0)
    static void AllOff();

    // Enable/Disable all physical device outputs on peripherals that
    // provide such software control.  Many of the chips we use as
    // output controllers provide some form of "Output Enable" signal
    // that can be connected to a Pico GPIO port, allowing the Pico to
    // command the chip to enable or disable its ports at the hardware
    // level.  In most cases, a disable output port is placed into a
    // high-impedance state, which has the effect of switching off any
    // device connected to the port, even if it's connected through an
    // amplifier circuit.  This is usually the most reliable way of
    // ensuring glitch-free startup and shutdown.
    //
    // Initially, all of our drivers for peripherals that have Output
    // Enable controls will set their outputs to the disabled state
    // after a Pico reset.  The main loop will call this routine to
    // enable peripheral ports AFTER completing device and Output
    // Manager initialization, AND after letting the Output Manager set
    // the initial states for all logical output ports.  That will
    // properly initialize all physical device output states to account
    // for active high/low and computed values, so that all of the
    // physical ports are set to the physical state corresponding to
    // the initial logical state by the time we physically enable
    // the output connections.
    static void EnablePhysicalOutputs(bool enable);

    // Run periodic tasks
    static void Task();

    // Suspend the output manager.  This is primarily for testing
    // physical output ports at the device level, by preventing the
    // output manager from applying timers and computed data sources to
    // the ports, so that the underlying physical ports can be
    // controlled directly at the device level.
    //
    // This sets all of the logical ports to OFF, then sets all of the
    // direct device-level outputs to OFF (taking into account their
    // acive-low/active-high polarity).  Finally, it sets a flag in the
    // output manager that prevents the Task() routine from doing
    // further updates.
    //
    // timeout_ms gives a timeout in milliseconds for the suspension.
    // 0 suspends operations indefinitely (i.e., without a timeout).
    static void Suspend(uint32_t timeout_ms = 0);

    // Suspend the output manager if it's not already suspended, and log
    // a console message.  This is meant as a convenience for device
    // classes implementing console commands to exercise output ports at
    // the device level.  Does nothing if output management is already
    // suspended.
    static void SuspendIfActive(const ConsoleCommandContext *ctx, uint32_t timeout_ms = 0);

    // Resume output manager operation.  This clears the flag set
    // in Suspend() that blocks the Task() routine, allowing normal
    // operation of timers and computed data sources to resume.
    static void Resume();

    // is output management suspended?
    static bool IsSuspended() { return isSuspended; }

    // Query logical port descriptors, for the USB vendor interface.
    // Populates the buffer with the structures described in
    // VendorIfcProtocol.h, providing descriptions of the configured
    // logical output ports.  Returns the size in bytes of the result
    // data on success, zero on error.
    static size_t QueryLogicalPortDescs(uint8_t *buf, size_t bufSize);

    // Query a logical port's name.  This populates the buffer with a
    // null-terminated string (of single-byte characters) giving the
    // name assigned to the port in the JSON configuration.  Returns
    // the populated buffer size, including the terminal null byte.
    // If the port is unnamed, an empty null-terminated string is
    // returned.  Returns 0 on error.
    static size_t QueryLogicalPortName(uint8_t *buf, size_t bufSize, int portNum);

    // Query physical output device descriptors, for the USB vendor
    // interface.  Populates the buffer with the structures described in
    // VendorIfcProtocol.h, providing descriptions of the configured
    // physical output devices.  Returns the size in bytes of the result
    // data on success, zero on error.
    static size_t QueryDeviceDescs(uint8_t *buf, size_t bufSize);

    // Query physical output device port descriptors, for the USB vendor
    // interface.  Populates the buffer with the structures described in
    // VendorIfcProtocol.h, providing descriptions of the individual ports
    // of the physical output devices.  Returns the size in bytes of the
    // result data on success, zero on error.
    static size_t QueryDevicePortDescs(uint8_t *buf, size_t bufSize);

    // Query logical port levels, for the USB vendor interface.
    // Populates the buffer with the structures described in
    // VendorIfcProtocol.h, reporting the current PWM levels set on
    // the logical ports.  Retursn the size in bytes of the result
    // data on success, zero on error.
    static size_t QueryLogicalPortLevels(uint8_t *buf, size_t bufSize);

    // Query physical output device port levels, for the USB vendor
    // interface.  Populates the buffer with the structures described in
    // VendorIfcProtocol.h, reporting the current PWM levels set on the
    // physical device ports across all configured output devices.
    // Returns the size in bytes of the result data on success, zero on
    // error.
    static size_t QueryDevicePortLevels(uint8_t *buf, size_t bufSize);

    // Device interface.  This is an abstract class that represents a
    // physical or virtual output device.  Each logical port has an
    // associated Device subclass instance that connects the logical
    // port to the concrete device that the port controls.
    class Device
    {
    public:
        // Configure from the JSON device object.  This initializes the
        // common attributes: gamma, inverted logic.
        //
        // Subclasses can override this to propagate any common or port
        // properties to the device (for example, PWMWorker can handle
        // gamma and logic inversion on the device side).
        virtual void Configure(const JSONParser::Value *val);

        // Propagate port settings to the device, if applicable.  This
        // can propagate additional port-level properties to the device,
        // such as the flipper logic settings.
        virtual void ApplyPortSettings(Port *portObj) { }
        
        // get the device class name
        virtual const char *Name() const = 0;

        // format the full name for messages into the buffer, and returns
        // a pointer to the buffer
        virtual const char *FullName(char *buf, size_t buflen) const = 0;

        // Populate the device-specific portions of a vendor interface
        // port descriptor struct
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const = 0;
        
        // Set the PWM level, 0..255.  The interpretation of the level
        // varies by device type.  For PWM-capable physical outputs,
        // the abstract 0..255 level straightforwardly maps to the
        // underlying device's PWM duty cycle scale, scaling linearly
        // to the device's duty cycle resolution.  For simple digital
        // (ON/OFF) ports, any non-zero level generally maps to ON,
        // and 0 maps to OFF.  Virtual devices should interpret the
        // level in whatever way makes sense for the device, keeping
        // in mind that the user thinks of the port level as though
        // it represented the brightness of a connected lamp or the
        // power level applied to a connected solenoid or motor.
        virtual void Set(uint8_t level) = 0;

        // Get the current PWM level on the physical port
        virtual uint8_t Get() const = 0;

        // Calculate the physical output corresponding to a DOF level
        // for an N-bit output device, applying gamma correction and
        // logic inversion according to our configuration properties.
        inline bool To1BitPhys(uint8_t b)
        {
            // 0 -> OFF, other -> ON
            bool on = (b != 0);

            // apply inversion (gamma isn't meaningful with a 1-bit device)
            return on ^ inverted;
        }
        inline uint8_t To8BitPhys(uint8_t b)
        {
            // apply gamma and inversion
            if (gamma) b = gamma_8bit[b];
            if (inverted) b = 255 - b;
            return b;
        }
        inline uint16_t To12BitPhys(uint8_t b)
        {
            // apply gamma and inversion
            uint16_t l = gamma ? gamma_12bit[b] : Rescale12(b);
            if (inverted) l = 4095 - l;
            return l;
        }
        inline float ToFloatPhys(uint8_t b)
        {
            // select a mapping table based on the gamma and inversion settings
            return conv_tables_float[mappingIndex][b];
        }

        // Linearly rescale a uint8_t to a notional "uint12_t" (unsigned
        // 12 bit integer, 0..4095, actually stored in a native
        // uint16_t).  This is a utility routine that we use for devices
        // with native 12-bit PWM resolution, such as TLC5940 and
        // PCA9685.  This uses the "shift-and-fill" algorithm, which
        // yields excellent linearity over the whole range - the result
        // is within +/- 1 of roundf(b * 4095.0f/255.0f), using only
        // integer arithmetic.
        static inline uint16_t Rescale12(uint8_t b)
        {
            // Shift left 4 bits to get to 12 bits, and fill in the vacated
            // low 4 bits with the original high 4 bits.  This fills the
            // entire 12-bit output range from 0..4095 with good linearity
            // to within rounding error, and it uses integer operations
            // only.
            uint16_t w = b;
            return (w << 4) | (w >> 4);
        }

        // Is gamma correction enabled?  If this is true, the DOF PWM
        // level will be translated to the physical output level through
        // a gamma curve.  If false, the DOF level is mapped to the
        // physical level linearly.
        bool gamma = false;

        // Is the output logic state inverted?  If this is true, the DOF
        // level is translated to the physical output level by inverting
        // the sign: Physical = PhysicalMax - Scaled(DOF).  So DOF 0
        // becomes fully ON at 100% duty cycle on the physical port, and
        // DOF 255 is fully OFF at 0% duty cycle.
        //
        // Inverted logic can be used when the physical output port is
        // wired such that the nominal ON state of the port corresponds
        // to the OFF state of the attached device, and vice versa.
        //
        // For example, this applies to a GPIO port that's wired to a
        // device in a low-side switch configuration, with the device
        // getting its positive supply voltage from a fixed connection
        // to the power supply, and its GND connection through the GPIO
        // port.  The device turns ON when the GPIO port is LOW, which
        // for a GPIO is nominally the OFF state.
        //
        // Another example: a TLC59116F port that's wired to a booster
        // circuit with an active-high trigger.  TLC59116F outputs are
        // open-drain, so a port conducts to GND when nominally ON.
        // For an active-high trigger, you'd have to wire this with
        // a pull-up resistor to the trigger voltage, so that the port
        // is at trigger voltage when the port is nominally OFF, NOT
        // conducting to GND, and at zero voltage when the port is
        // nominally ON, conducting to GND.  In this arrangement, the
        // output manager must set the TLC59116F port status to OFF
        // when the device is meant to be ON, so inverted logic
        // applies.
        bool inverted = false;

        // Mapping table selector index.  This is computed based on the
        // gamma and inverted-logic settings so that mapping routines
        // can select a table for correct combination using an array
        // lookup, rather than conditionals.  The index mapping is:
        //
        //   0  -> linear, positive logic
        //   1  -> linear, inverted logic
        //   2  -> gamma, positive logic
        //   3  -> gamma, inverted logic
        //
        // This is equivalent to a bit map:
        //
        //   0x01   inverted
        //   0x02   gamma
        //
        int mappingIndex = 0;

        // gamma and linear conversion tables, for conversions from DOF
        // levels to physical output levels for different device types
        static const uint8_t gamma_8bit[];
        static const uint16_t gamma_12bit[];
        static const float gamma_float[];
        static const float gamma_float_invert[];
        static const float linear_float[];
        static const float linear_float_invert[];

        // table selectors for different conversions, using mappingIndex
        static const uint8_t* const conv_tables_8bit[];
        static const uint16_t* const conv_tables_12bit[];
        static const float* const conv_tables_float[];
    };

    // Null device.  This is a virtual device that does nothing.  We
    // use this as a placeholder for ports that have invalid config
    // data, and the user can explicitly configure a port with a
    // null output if desired.
    class NullDev : public Device
    {
    public:
        NullDev() { }
        virtual const char *Name() const override { return "None"; }
        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "Null"); return buf; }
        virtual void Set(uint8_t level) override { }
        virtual uint8_t Get() const override { return 0; }
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;
    };

    // common base class for GPIO port devices
    class GPIODev : public Device
    {
    public:
        GPIODev(int gp) : gp(gp) { }
        
        // direct port access, for testing/debugging/troubleshooting clients
        static void SetDevicePortLevel(int port, int pwmLevel);

        // get the port assignment type
        enum class AssignmentType
        {
            None = 0, // port is not assigned to a logical output
            PWM,      // port is assigned as a PWM output port
            Digital,  // port is assigned as a digital on/off output port
        };
        static AssignmentType GetAssignmentType(int port) {
            return port >= 0 && port < numPorts ? assignmentType[port] : AssignmentType::None;
        }

        // there's always exactly one Pico in the system
        static size_t CountConfigurations() { return 1; }

        // get the GPIO port count
        static size_t CountPorts() { return numPorts; }

        // populate a logical output port description
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // populate a vendor interface output port query result buffer
        static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

        // populate physical output port descriptions
        static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

        // populate an output level query result buffer
        static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

    protected:
        // the Pico has a fixed complement of GPIO ports
        static const int numPorts = 30;

        // List of all GPIO ports assigned to logical ports.  This
        // is used in the direct device port writer interface to
        // suppress writing to GPIOs that might be in use by
        // other subsystems.
        static AssignmentType assignmentType[numPorts];

        // GPIO port number
        int gp;
    };

    // GPIO output port device with PWM support
    class PWMGPIODev : public GPIODev
    {
    public:
        PWMGPIODev(int gp, int freq);
        virtual const char *Name() const { return "GPIO(PWM)"; }
        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "GP%d [PWM]", gp); return buf; }
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
    };

    // GPIO output port device with digital on/off control only
    class DigitalGPIODev : public GPIODev
    {
    public:
        DigitalGPIODev(int gp);
        virtual const char *Name() const { return "GPIO(Digital)"; }
        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "GP%d [Digital]", gp); return buf; }
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
    };

    // External Worker Pico unit, running our PWMWorker firmware program,
    // which turns the Pico into a virtual PWM controller chip.
    class PWMWorkerDev : public Device
    {
    public:
        PWMWorkerDev(PWMWorker *worker, int port) : worker(worker), port(port) { }
        virtual const char *Name() const { return "WorkerPico"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // configure the port
        virtual void Configure(const JSONParser::Value *val) override;

        // propagate port settings
        virtual void ApplyPortSettings(Port *portObj) override;

        // the underlying PWMWorker interface
        PWMWorker *worker;

        // the GPIO port number on the external Pico
        int port;
    };

    // TLC59116 output port device.  This represents a single port
    // on a TLC59116 chip.
    class TLC59116Dev : public Device
    {
    public:
        TLC59116Dev(TLC59116 *chip, int port) : chip(chip), port(port) { }
        virtual const char *Name() const { return "TLC59116"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // the chip instance and port number
        TLC59116 *chip;
        int port;
    };

    // TLC5940 output port device.  This represents a single port
    // on a TLC5940 chip.
    class TLC5940Dev : public Device
    {
    public:
        TLC5940Dev(TLC5940 *chain, int port) : chain(chain), port(port) { }
        virtual const char *Name() const { return "TLC5940"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // daisy chain instance and port number
        TLC5940 *chain;
        int port;
    };

    // TLC5947 output port device.  This represents a single port
    // on a TLC5947 chip.
    class TLC5947Dev : public Device
    {
    public:
        TLC5947Dev(TLC5947 *chain, int port) : chain(chain), port(port) { }
        virtual const char *Name() const { return "TLC5947"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // daisy chain instance and port number
        TLC5947 *chain;
        int port;
    };

    // PCA9685 output port device.  This represents a single port
    // on a PCA9685 chip.
    class PCA9685Dev : public Device
    {
    public:
        PCA9685Dev(PCA9685 *chip, int port) : chip(chip), port(port) { }
        virtual const char *Name() const { return "PCA9685"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // chip instance and port number
        PCA9685 *chip;
        int port;
    };

    // PCA9555 output port device.  This represents a single port on
    // a PCA9555 chip.
    class PCA9555Dev : public Device
    {
    public:
        PCA9555Dev(PCA9555 *chip, int port, const char *jsonLocus);
        virtual const char *Name() const { return "PCA9555"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // chip instance and port number
        PCA9555 *chip;
        int port;
    };

    // 74HC595 output port device.  This represents a single port
    // on a 74HC595 chip.
    class C74HC595Dev : public Device
    {
    public:
        C74HC595Dev(C74HC595 *chain, int port) : chain(chain), port(port) { }
        virtual const char *Name() const { return "74HC595"; }
        virtual const char *FullName(char *buf, size_t buflen) const override;
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        static void SetDevicePortLevel(int configIndex, int port, int pwmLevel);
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // daisy chain instance and port number
        C74HC595 *chain;
        int port;
    };

    // ZB Launch Control port device.  This is a virtual port that
    // sets the ZB Launch Control mode flag.  Any non-zero value is
    // treated as ON.
    class ZBLaunchDev : public Device
    {
    public:
        ZBLaunchDev() { }
        virtual const char *Name() const { return "ZBLaunch"; }
        virtual const char *FullName(char *buf, size_t buflen) const override { snprintf(buf, buflen, "ZBLaunch"); return buf; }
        virtual void Set(uint8_t level) override;
        virtual uint8_t Get() const override;
        virtual void Populate(PinscapePico::OutputPortDesc *desc) const override;

        // previous level
        uint8_t prvLevel = 0;
    };

    // Logical output port.  This is the port that the host sees through
    // the Feedback Controller USB interface.  Each logical part is
    // connected to an underlying physical or virtual output device via
    // a Device instance.
    class DataSource;
    class Port
    {
        friend class OutputManager;
        
    public:
        // create a port given the physical device
        Port(Device *device) : device(device) { }

        // get the port name
        const char *GetName() const { return portName; }

        // get the device
        Device *GetDevice() const { return device; }

        // Get the logical port level (the calculated level)
        uint8_t Get() const { return logLevel; }

        // Get the current host level as set through the Feedback
        // Controller interface OR the LedWiz emulation interface,
        // whichever sent the more recent update.
        uint8_t GetHostLevel() const { return lw.mode ? lw.GetLiveLogLevel() : dofLevel; }

        // Get/Set the port's DOF level setting.  This is the level set
        // the PC host through the Feedback Controller interface.  In
        // most cases, the host software is DOF or a similar game
        // controller program, so we also loosely refer to this as the
        // current DOF level.
        uint8_t GetDOFLevel() const { return dofLevel; }
        void SetDOFLevel(uint8_t level);

        // Set the LedWiz state for the port.  Setting the LedWiz state
        // overrides the host level setting, and setting the host level
        // overrides the LedWiz state, so whichever one was set last is
        // the one currently in effect.  'period' is the LedWiz protocol
        // waveform time period code, 1-7 (each unit represents 250ms).
        void SetLedWizSBA(bool on, uint8_t period);
        void SetLedWizPBA(uint8_t profile);

        // Get the latest device output level
        uint8_t GetOutLevel() const { return outLevel; }

        // Turn off the port's underlying physical port.  This bypasses
        // the host level, flipper logic timers, and computed data
        // sources, to explicitly set the physical port to OFF.  This
        // takes into account the active high/low polarity.
        void SetDeviceOff();

        // get the port's data source
        DataSource *GetDataSource() const { return source; }

        // Is the port in LedWiz mode?
        bool IsLedWizMode() const { return lw.mode; }

        // Get the LedWiz mode on/off state and "profile" (PBA) setting
        bool GetLedWizOnState() const { return lw.on; }
        int GetLedWizProfileState() const { return lw.profile; }

        // Set the port's data source.  This can be used by other subsystems
        // to give the port a derived source.
        // 
        // We allow this outside meddling with the port layout so that the
        // JSON configuration layout can be made more human-friendly, by
        // grouping cross-references between output ports and outside
        // subsystems with the outside subsystem's other config data.  One
        // example is the TV ON subsystem, which lets the user designate an
        // output port as the TV relay.  The association can be programmed
        // in the JSON outputs[] array via a 'source' formula on the desired
        // port, but that might be less intuitive to some people than the
        // other way around, which is to just list the desired port number
        // with the 'tvon' config setcion.
        void SetDataSource(DataSource *source) { this->source = source; }

    protected:
        // Set the logical port level.  This is called from SetDOFevel()
        // and from the derived data source calculator.
        void SetLogicalLevel(uint8_t level);

        // Perform periodic device tasks.  This updates calculated values
        // for ports with non-host data sources, and applies the flipper
        // logic timer.
        void Task();

        // Apply the current nominal level to the physical output port.
        // This should be called whenever any of the inputs to the physical
        // output level change: nominal level, flipper logic activation,
        // night mode activation.
        void Apply();

        // Port name assigned in the configuration, if any.  This points
        // to the port name in the portsByName map (to save space by
        // just referencing the copy of the string we have to save there
        // anyway), or null if the port doesn't have an assigned name.
        const char *portName = nullptr;

        // The concrete device that this port is connected to
        Device *device;

        // The port's data source, if any.  If this is null, the port gets
        // its value directly from the host PC, via DOF commands or the
        // like.  If the port has a data source, the port value is
        // calculated dynamically from the data source.  This overrides the
        // host setting; any updates the host writes to the port are
        // ignored.  The port is still visible to the host for the sake of
        // keeping the port numbering consistent between the host view and
        // the config file layout, but it's read-only on the host.
        DataSource *source = nullptr;

        // Is this a "noisy" output, subject to disabling when Night Mode is
        // activated?
        bool noisy = false;

        // Flipper logic settings
        struct FlipperLogic
        {
            // is flipper logic enabled for this port?
            bool enabled = false;
            
            // Maximum high-power activation interval, in microseconds.  The
            // device is considered activated at high power when its nominal
            // PWM level is above the reduced power setting.
            uint32_t dtHighPowerMax = 0;

            // Cooling-off period, in microseconds.  This is the minimum
            // amount of time between high-power activations.
            uint32_t dtCooling = 0;

            // Reduced power level setting.  When the device is activated
            // at high power for tMax microseconds or longer, the level is
            // reduced to this value, and can't be increased above this level
            // until the port is turned off.
            uint8_t reducedPowerLevel = 0;

            // current flipper logic state
            enum class State
            {
                // Ready.  The port is in a low power state (fully OFF, or at
                // or below the reduced power limit).  No timer is running.
                Ready,

                // Armed.  The port is in a high-power state and the time
                // limit timer is running.
                Armed,

                // Triggered.  The port was in Armed state for longer than
                // the time limit, so the reduced power level is now being
                // applied.
                Triggered,
            };
            State state = State::Ready;

            // System clock time for high-power cutoff.  This is set
            // when the logical port level transitions from low power to
            // high power, and monitored in the task routine.
            uint64_t tHighPowerCutoff = 0;

            // End of the cooling-off period.  When we transition from high
            // power to reduced power, this is set to the current system
            // clock time plus the cooling off interval.
            uint64_t tCoolingEnd = 0;

            // Previous effective (physical) output level, as of the last
            // Apply check, for monitoring the cooling-off period.
            uint8_t prvEffectiveLevel = 0;
        } flipperLogic;

        // Current DOF host level setting, 0..255.  This is the last port level
        // setting commanded by the host (e.g., by DOF).
        uint8_t dofLevel = 0;

        // LedWiz emulation state.
        //
        // To support a host-side LedWiz emulation via a custom replacement
        // for LEDWIZ.DLL, we store an LedWiz state alongside the DOF host
        // state in 'dofLevel'.  It's not enough to translate an LedWiz state
        // into a DOF setting at the moment the LedWiz setting is transmitted,
        // because the LedWiz exposes a complex model to applications that the
        // DOF state can't represent.  The LedWiz application model separates
        // the On/Off state from the brightness setting, and doesn't actually
        // have brightness settings at all, but rather has *waveform*
        // settings.  Many of the waveforms that can be selected are just
        // constant PWM levels, so they're equivalent to brightness settings,
        // but a few of the settings represent actual time-varying waveforms,
        // such as square waves (that is, blinking patterns) and sawtooth
        // waves.
        //
        // To handle all of these details, we have to store the two orthogonal
        // elements of the LedWiz setting sent from the host (the On/Off state
        // and the waveform selection), and then calculate the resulting live
        // PWM level on the fly from moment to moment.  That's what this state
        // structure is for.
        //
        // The LedWiz state and DOF host level are mutually exclusive.  Both
        // interfaces luckily expose application models where ports are
        // writable registers with non-exclusive access, so applications (and
        // users) expect that a port will hold whatever value was written most
        // recently from any application.  So we can arbitrate LedWiz vs DOF
        // control over the port simply by whoever wrote last.  We do that
        // with a flag here saying whether or not the LedWiz state is in
        // effect.  This is set to true whenever the host sends an LedWiz
        // protocol "SBA" or "PBA" command, and set to false whenever the host
        // sends a DOF port setting command.
        struct LedWizState
        {
            // Is the LedWiz mode in effect?  Set to true whenever an LedWiz
            // comand is used to set the port state; set to false whenever a
            // DOF command is used to set the port state.
            bool mode = false;

            // on/off state (SBA commands, per the LedWiz protocol)
            bool on = false;

            // "profile" state (PBA commands, per the LedWiz protocol)
            //
            //   0-48   = proportional PWM level, N/48 for 0-100%
            //   49     = 100% (48 and 49 have the same meaning, which is
            //            contrary to the LedWiz's official API documentation
            //            but is observed for genuine LedWiz's)
            //   129    = sawtooth (linear ramp up 0%-100%, linear ramp down 100%-0%)
            //   130    = flash on/off (50% duty cycle)
            //   131    = on/ramp up (50% duty cycle with fade out)
            //   132    = ramp up/on (50% duty cycle with fade in)
            uint8_t profile = 0;

            // LedWiz flash period.  We express this in units of 250ms, which
            // is the same unit system the LedWiz protocol uses.  Valid values
            // are 1 through 7, corresponding to 250 ms through 1.75 seconds.
            uint8_t period = 1;

            // Compute the current PWM level for this state
            uint8_t GetLiveLogLevel() const;
        } lw;

        // "Logical" level, 0..255.  This is the nominal level for the port,
        // after taking into account the host level, LedWiz port state, and the
        // data source calculation, but before gamma correction and flipper
        // logic attenuation.
        uint8_t logLevel = 0;

        // Device output level.  This is the final value, 0..255, sent to
        // the underlying physical device port.  This is the calculated
        // value, further adjusted by flipper logic time limitation and
        // logic inversion.
        uint8_t outLevel = 0;
    };

    // Data source value.  This is a run-time-tagged variant type, to allow
    // for a mix of argument and return types.
    struct SourceVal
    {
        enum class Type
        {
            UInt8,      // 8-bit integer value
            Float,      // float
            RGB,        // RGB, 8-bit components
            Vector,     // XY vector, signed 9-bit components, -256..255
        } type;

        static const char *TypeName(Type t)
        {
            return t == Type::UInt8 ? "UInt8" :
                t == Type::Float ? "float" :
                t == Type::RGB ? "RGB" :
                t == Type::Vector ? "vector" :
                "<invalid>";
        }

        union
        {
            uint8_t i;  // type == UInt8
            float f;    // type == Float
            struct {
                uint8_t r;
                uint8_t g;
                uint8_t b;
            } rgb;      // type == RGB
            struct {
                int16_t x;
                int16_t y;
            } vec;      // type == Vector
        };

        SourceVal() : type(Type::UInt8), i(0) { }
        SourceVal(Type type) : type(type) { }
        SourceVal(const SourceVal &v) { memcpy(this, &v, sizeof(v)); }

        static SourceVal MakeUInt8(uint8_t i) { SourceVal v(Type::UInt8); v.i = i; return v; }
        static SourceVal MakeFloat(float f) { SourceVal v(Type::Float); v.f = f; return v; }
        static SourceVal MakeRGB(uint8_t r, uint8_t g, uint8_t b) { SourceVal v(Type::RGB); v.rgb = { r, g, b }; return v; }
        static SourceVal MakeRGB(uint8_t gray) { SourceVal v(Type::RGB); v.rgb = { gray, gray, gray }; return v; }
        static SourceVal MakeVector(int16_t x, int16_t y) { SourceVal v(Type::Vector); v.vec = { x, y }; return v; }

        uint8_t AsUInt8() const;
        float AsFloat() const;
        SourceVal AsRGB() const;
        SourceVal AsVector() const;

        static uint8_t ClipU8(float f) { return f < 0.0f ? 0 : f > 255.0f ? 255 : static_cast<uint8_t>(f); }
        static int16_t ClipI16(float f) { return f < -32768.0f ? -32768 : f > 32767.0f ? 32767 : static_cast<int16_t>(f); }

        static uint8_t Grayscale(uint8_t r, uint8_t g, uint8_t b) {
            return static_cast<uint8_t>(r*0.299f + g*0.587f + b*0.114f);
        }
    };

    // Parse a data source declaration.  Returns a DataSource pointer on
    // success.  On error, logs the error and returns null.  Advances the
    // text pointer past the consumed text.
    static DataSource *ParseSource(int index, const char* &p, const char *start, const char *end);

    // Recursive-descent source parser expression level parsers
    //
    // expr = comparable comparison comparable ...      comparison: = != < <= > >=
    //        term
    //
    // comparable = term additive term ...              additive: + -
    //        factor
    //
    // term = factor mult factor ...                    mult: * / %
    //
    // factor = postfix
    //
    // postfix = postfix . property
    //        primary
    //
    // primary = ( expr )
    //        source
    // 
    static DataSource *ParseSourceComparable(int index, const char* &p, const char *start, const char *end);
    static DataSource *ParseSourceTerm(int index, const char* &p, const char *start, const char *end);
    static DataSource *ParseSourceFactor(int index, const char* &p, const char *start, const char *end);
    static DataSource *ParseSourcePostfix(int index, const char* &p, const char *start, const char *end);
    static DataSource *ParseSourcePrimary(int index, const char* &p, const char *start, const char *end);

    // data source binary-operator parser
    class BinOpSource;
    struct BinOp
    {
        // error message text list which operators are expected
        const char *expected;

        // parse a subexpression
        std::function<DataSource*(int, const char*&, const char *, const char *)> Parse;

        // operator -> data source creator lookup table
        struct Op
        {
            const char *op;
            std::function<BinOpSource*()> create;
        };
        std::list<Op> ops;

        // match an operator
        bool Match(const char *op, int opLen) const;

        // apply an operator
        DataSource *Apply(const char *op, int opLen, DataSource *lhs, DataSource *rhs) const;
    };
    static DataSource *ParseSourceBinaryExpr(int index, const char* &p, const char *start, const char *end, const BinOp &binOp);

    // data source argument pack
    class ConstantSource;
    class DataSourceArgs
    {
    public:
        DataSourceArgs(Port *self) : self(self) { }

        // port whose 'source' expression we're parsing
        Port *self;

        // argument list
        std::list<std::unique_ptr<DataSource>> args;

        // Get and consume the next argument as a constant value of the
        // specified type.  On error, add a message to the error list.
        uint8_t GetU8();
        int GetInt();
        float GetFloat();

        // get and consume the next argument as a generic constant value
        bool GetConstant(SourceVal &v);

        // Try retrieving the naxt argument as a string; if it's string,
        // fills in 'str' with the string contents and returns true,
        // otherwise returns false.  It's not an error if the argument isn't
        // a string, since this routine is only a check for the possibility.
        bool TryGetString(std::string &str);
        
        // Get and consume the next argument as a data source.  Passes
        // ownership of the object to the caller.
        DataSource *GetSource();

        // Get and consume all remaining arguments as data sources,
        // reserving the last exceptN sources
        std::list<DataSource*> GetAll(int exceptN = 0);

        // current argument processing number, for error logging
        int argi = 1;

        // error list - if any errors occur during argument conversions,
        // we'll place loggable error messages here
        struct Error
        {
            Error(int argi, const char *msg) : argi(argi), msg(msg) { }
            int argi;              // argument number
            const char *msg;       // message, as a static string
        };
        std::list<Error> errors;
    };

    // Data Source
    class ConstantSource;
    class PortSource;
    class DataSource
    {
    public:
        // calculate the current value
        virtual SourceVal Calc() = 0;

        // invoke a callback on this data source's sub-sources
        using TraverseFunc = std::function<void(DataSource*)>;
        virtual void Traverse(TraverseFunc func) = 0;

        // explicit downcasts (so that we don't have to enable compiler RTTI for dynamic_cast<>)
        virtual ConstantSource *AsConstantSource() { return nullptr; }
        virtual PortSource *AsPortSource() { return nullptr; }
    };

    // Constant value.  Uniquely, a constant node can be string-valued,
    // for references to named ports, buttons, etc.
    class ConstantSource : public DataSource
    {
    public:
        ConstantSource(SourceVal val) : val(val) { }
        ConstantSource(const char *str, size_t len) : str(str, len) { }
        virtual ConstantSource *AsConstantSource() override { return this; }
        virtual SourceVal Calc() override { return val; }
        virtual void Traverse(TraverseFunc func) override { }
        SourceVal val;
        std::string str;
    };

    // Output port source.  This uses the calculated logical level of the port.
    class PortSource : public DataSource
    {
    public:
        PortSource(Port *port, bool raw) : port(port), raw(raw) { }
        PortSource(DataSourceArgs &args, bool raw);
        virtual PortSource *AsPortSource() override { return this; }
        virtual SourceVal Calc() override { return SourceVal::MakeUInt8(raw ? port->GetHostLevel() : port->Get()); }
        virtual void Traverse(TraverseFunc func) override { if (!raw) func(port->source); }
        Port *port;
        bool raw = false;
    };

    // TV ON relay source
    class TVONSource : public DataSource
    {
    public:
        TVONSource() { }
        TVONSource(DataSourceArgs &args) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // TV ON power sense source
    class PowerSenseSource : public DataSource
    {
    public:
        PowerSenseSource(DataSourceArgs &args) :
            sourceOff(args.GetSource()),
            sourceCountdown(args.GetSource()),
            sourceRelay(args.GetSource()),
            sourceIR(args.GetSource()),
            sourceOn(args.GetSource())
        { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override {
            func(sourceOff);
            func(sourceCountdown);
            func(sourceRelay);
            func(sourceIR);
            func(sourceOn);
        }
        DataSource *sourceOff;
        DataSource *sourceCountdown;
        DataSource *sourceRelay;
        DataSource *sourceIR;
        DataSource *sourceOn;
    };

    // Night Mode source
    class NightModeSource : public DataSource
    {
    public:
        NightModeSource(DataSourceArgs &args) : sourceOn(args.GetSource()), sourceOff(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceOff); }
        DataSource *sourceOn, *sourceOff;
    };

    // Time range source - selects inputs based on the time of day
    class TimeRangeSource : public DataSource
    {
    public:
        static TimeRangeSource *ParseArgs(DataSourceArgs &args);
        
        TimeRangeSource(uint32_t tStart, uint32_t tEnd) : tStart(tStart), tEnd(tEnd) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override{ func(sourceIn); func(sourceOut); }
        DataSource *sourceIn = nullptr;
        DataSource *sourceOut = nullptr;

        // Endpoints of the time time-of-day range, as seconds since midnight.
        // If tEnd < tStart, the range spans midnight, so it's from tStart on
        // day N to tEnd on day N+1.
        uint32_t tStart = 0, tEnd = 0;

        // Calculate a "year day number".  This converts a date from mm/dd format
        // to a high-radix integer representing the day of the year in non-contiguous
        // ascending order.  (Non-continguous means that there are gaps between valid
        // day numbers.)  This format places all dates over the year in a sortable
        // order, where we can determine if one date is before or after another
        // by simple integer comparison of the day number values.  It's also easy
        // to recover the mm/dd date from a day number, although we don't need to
        // do that for our purposes here.
        static int YearDayNumber(int mm, int dd) { return (mm << 8) + dd; }
    };

    // Weekday time range source.  Selects inputs depending on
    // whether the current time is within a specified range
    // within the week.
    class WeekdayTimeRangeSource : public TimeRangeSource
    {
    public:
        WeekdayTimeRangeSource(int weekdayStart, int weekdayEnd, uint32_t tStart, uint32_t tEnd) :
            TimeRangeSource(tStart, tEnd), weekdayStart(weekdayStart), weekdayEnd(weekdayEnd) { }

        virtual SourceVal Calc() override;

        // Endpoints of the weekday range, 0=Monday, 1=Tuesday, etc.  If
        // weekdayStart < weekdayEnd, the range spans the start of the
        // new week.
        int weekdayStart;
        int weekdayEnd;
    };

    // Weekday mask time range source.  Selects inputs according to
    // the time of day on selected days of the week.
    class WeekdayMaskTimeRangeSource : public TimeRangeSource
    {
    public:
        WeekdayMaskTimeRangeSource(int weekdayMask, uint32_t tStart, uint32_t tEnd) :
            TimeRangeSource(tStart, tEnd), weekdayMask(weekdayMask) { }

        virtual SourceVal Calc() override;

        // Weekday mask.  This encodes a set of days of the week as bits,
        // with (1<<0)=Monday, (1<<1)=Tuesday, etc.  The time range is only
        // valid for tStart within a day in the mask.
        //
        // For example, 0x15 selects Monday, Wednesday, and Friday, so if
        // tStart is 13:00 and tEnd is 14:00, valid times are from 13:00
        // to 14:00 on Mondays, Wednesdays, and Fridays.
        //
        // If tEnd < tStart, the time range spans midnight, and must START
        // on a day in the mask.  Continuing our 0x15 M-W-F example, if the
        // range is 23:00 to 01:00, the times span 23:00 Monday to 01:00 Tuesday,
        // 23:00 Wednesday to 01:00 Thursday, and 23:00 Friday to 01:00 Saturday.
        int weekdayMask = 0;
    };

    // Date-and-time range source.  Selects inputs according to the
    // time of day within a span of calendar dates within a year.
    class DateAndTimeRangeSource : public TimeRangeSource
    {
    public:
        DateAndTimeRangeSource(int yearDayStart, int yearDayEnd, uint32_t tStart, uint32_t tEnd) :
            TimeRangeSource(tStart, tEnd), dateStart(yearDayStart), dateEnd(yearDayEnd) { }

        virtual SourceVal Calc() override;

        // Date range, as a "year day number"
        int dateStart = 0, dateEnd = 0;
    };

    // Plunger position source, normalized to 0..255 integer range with park position at 42
    class PlungerPosSource : public DataSource
    {
    public:
        PlungerPosSource(DataSourceArgs &args) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Plunger position source, as a floating point value -32768 to +32767, with the park position at zero
    class PlungerPosFloatSource : public DataSource
    {
    public:
        PlungerPosFloatSource(DataSourceArgs &args) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Plunger Calibration Mode source
    class PlungerCalSource : public DataSource
    {
    public:
        PlungerCalSource(DataSourceArgs &args) : sourceOn(args.GetSource()), sourceHold(args.GetSource()), sourceOff(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceHold); func(sourceOff); }
        DataSource *sourceOn, *sourceHold, *sourceOff;
    };

    // RGB Status LED source
    class StatusLedSource : public DataSource
    {
    public:
        StatusLedSource(DataSourceArgs &args) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Button source
    class ButtonSource : public DataSource
    {
    public:
        ButtonSource(DataSourceArgs &args);
        Button *button;
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceOff); }
        DataSource *sourceOn, *sourceOff;
    };

    // ZB Launch Button source
    class ZBLaunchButtonSource : public DataSource
    {
    public:
        ZBLaunchButtonSource(DataSourceArgs &args) : sourceOn(args.GetSource()), sourceOff(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceOff); }
        DataSource *sourceOn, *sourceOff;
    };

    // ZB Launch Mode source
    class ZBLaunchModeSource : public DataSource
    {
    public:
        ZBLaunchModeSource(DataSourceArgs &args) : sourceOn(args.GetSource()), sourceOff(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceOff); }
        DataSource *sourceOn, *sourceOff;
    };

    // Blink source
    class BlinkSource : public DataSource
    {
    public:
        BlinkSource(DataSourceArgs &args) : on_us(args.GetInt() * 1000), off_us(args.GetInt() * 1000) { }
        uint32_t on_us;
        uint32_t off_us;
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Ramp source
    class RampSource : public DataSource
    {
    public:
        RampSource(DataSourceArgs &args) : period_us(args.GetInt() * 1000) { }
        uint32_t period_us;
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Sinewave source
    class SineSource : public DataSource
    {
    public:
        SineSource(DataSourceArgs &args) : period_us(args.GetInt() * 1000) { }
        uint32_t period_us;
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Sawtooth source
    class SawtoothSource : public DataSource
    {
    public:
        SawtoothSource(DataSourceArgs &args) : period_us(args.GetInt() * 1000) { }
        uint32_t period_us;
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // Scaling source
    class ScaleSource : public DataSource
    {
    public:
        ScaleSource(DataSourceArgs &args) : source(args.GetSource()), scale(args.GetFloat()) { }
        DataSource *source;
        float scale;
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
    };

    // Offset source
    class OffsetSource : public DataSource
    {
    public:
        OffsetSource(DataSourceArgs &args) : source(args.GetSource()), offset(args.GetFloat()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
        DataSource *source;
        float offset;
    };

    // Absolute value source
    class AbsSource : public DataSource
    {
    public:
        AbsSource(DataSourceArgs &args) : source(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
        DataSource *source;
    };

    // Base class for sources with RGB input values
    class SourceWithRGBInput : public DataSource
    {
    public:
        SourceWithRGBInput(DataSourceArgs &args);
        virtual void Traverse(TraverseFunc func) override { func(rgb); func(r); func(g); func(b); }
        DataSource *rgb = nullptr;
        DataSource *r = nullptr;
        DataSource *g = nullptr;
        DataSource *b = nullptr;

        struct RGB { uint8_t r; uint8_t g; uint8_t b; };
        RGB CalcRGB() const;
    };

    // Grayscale source
    class GrayscaleSource : public SourceWithRGBInput
    {
    public:
        GrayscaleSource(DataSourceArgs &args) : SourceWithRGBInput(args) { }
        virtual SourceVal Calc() override;
    };

    // RGB -> HSV hue source
    class HueSource : public SourceWithRGBInput
    {
    public:
        HueSource(DataSourceArgs &args) : SourceWithRGBInput(args) { }
        virtual SourceVal Calc() override;
    };

    // RGB -> HSB saturation source
    class SaturationSource : public SourceWithRGBInput
    {
    public:
        SaturationSource(DataSourceArgs &args) : SourceWithRGBInput(args) { }
        virtual SourceVal Calc() override;
    };

    // RGB -> HSB brightness source
    class BrightnessSource : public SourceWithRGBInput
    {
    public:
        BrightnessSource(DataSourceArgs &args) : SourceWithRGBInput(args) { }
        virtual SourceVal Calc() override;
    };

    // HSB (Hue, Saturation, Brightness) source
    class HSBSource : public DataSource
    {
    public:
        HSBSource(DataSourceArgs &args) : h(args.GetSource()), s(args.GetSource()), b(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(h); func(s); func(b); }
        DataSource *h;
        DataSource *s;
        DataSource *b;
    };

    // Red-component-of-RGB source
    class RedSource : public DataSource
    {
    public:
        RedSource(DataSourceArgs &args) : rgb(args.GetSource()) { }
        RedSource(DataSource *rgb) : rgb(rgb) { }
        virtual SourceVal Calc() override { return SourceVal::MakeUInt8(rgb->Calc().AsRGB().rgb.r); }
        virtual void Traverse(TraverseFunc func) override { func(rgb); }
        DataSource *rgb;
    };

    // Green-component-of-RGB source
    class GreenSource : public DataSource
    {
    public:
        GreenSource(DataSourceArgs &args) : rgb(args.GetSource()) { }
        GreenSource(DataSource *rgb) : rgb(rgb) { }
        virtual SourceVal Calc() override { return SourceVal::MakeUInt8(rgb->Calc().AsRGB().rgb.g); }
        virtual void Traverse(TraverseFunc func) override { func(rgb); }
        DataSource *rgb;
    };

    // Blue-component-of-RGB source
    class BlueSource : public DataSource
    {
    public:
        BlueSource(DataSourceArgs &args) : rgb(args.GetSource()) { }
        BlueSource(DataSource *rgb) : rgb(rgb) { }
        virtual SourceVal Calc() override { return SourceVal::MakeUInt8(rgb->Calc().AsRGB().rgb.b); }
        virtual void Traverse(TraverseFunc func) override { func(rgb); }
        DataSource *rgb;
    };

    // AND source - returns last source level if all sources are non-zero, else zero
    class AndSource : public DataSource
    {
    public:
        AndSource(DataSourceArgs &args) : sources(args.GetAll()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { for (auto &source : sources) func(source); }
        std::list<DataSource*> sources;
    };

    // OR source - returns last source level if any sources are non-zero, else zero
    class OrSource : public DataSource
    {
    public:
        OrSource(DataSourceArgs &args) : sources(args.GetAll()) { }
        virtual SourceVal Calc() override;        
        virtual void Traverse(TraverseFunc func) override { for (auto &source : sources) func(source); }
        std::list<DataSource*> sources;
    };

    // MAX source - returns highest level of all of the sources
    class MaxSource : public DataSource
    {
    public:
        MaxSource(DataSourceArgs &args) : sources(args.GetAll()) { }
        virtual SourceVal Calc() override;        
        virtual void Traverse(TraverseFunc func) override { for (auto &source : sources) func(source); }
        std::list<DataSource*> sources;
    };

    // MIN source - returns lowest level of all sources
    class MinSource : public DataSource
    {
    public:
        MinSource(DataSourceArgs &args) : sources(args.GetAll()) { }
        virtual SourceVal Calc() override;        
        virtual void Traverse(TraverseFunc func) override { for (auto &source : sources) func(source); }
        std::list<DataSource*> sources;
    };

    // IF source - conditional source
    class IfSource : public DataSource
    {
    public:
        IfSource(DataSourceArgs &args);
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override;
        struct IfThen
        {
            IfThen(DataSourceArgs &args) : ifSource(args.GetSource()), thenSource(args.GetSource()) { }
            DataSource *ifSource;
            DataSource *thenSource;
        };
        std::vector<IfThen> ifThens;
        DataSource *elseSource;
    };

    // SELECT source - selects one of several outcomes based on a control value,
    // similar to a C 'switch' statement
    class SelectSource : public DataSource
    {
    public:
        SelectSource(DataSourceArgs &args) : controlSource(args.GetSource()), sources(args.GetAll(1)), defaultSource(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override  { func(controlSource); func(defaultSource); for (auto &source : sources) func(source); }
        DataSource *controlSource, *defaultSource;
        std::list<DataSource*> sources;
    };

    // Clip source - clips value between a min and max level
    class ClipSource : public DataSource
    {
    public:
        ClipSource(DataSourceArgs &args) : source(args.GetSource()), lo(args.GetU8()), hi(args.GetU8()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
        DataSource *source;
        uint8_t lo, hi;
    };

    // IR TX source - IR transmission in progress
    class IRTXSource : public DataSource
    {
    public:
        IRTXSource(DataSourceArgs &args) : sourceOn(args.GetSource()), sourceOff(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceOff); }
        DataSource *sourceOn, *sourceOff;
    };

    // IR RX source - IR code reception in progress
    class IRRXSource : public DataSource
    {
    public:
        IRRXSource(DataSourceArgs &args) : sourceOn(args.GetSource()), sourceOff(args.GetSource()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(sourceOn); func(sourceOff); }
        DataSource *sourceOn, *sourceOff;
    };

    // Nudge Device source
    class NudgeDeviceSource : public DataSource
    {
    public:
        NudgeDeviceSource(DataSourceArgs &args);
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
    };

    // XBox Controller LED source
    class XBoxLedSource : public DataSource
    {
    public:
        XBoxLedSource(DataSourceArgs &args) : led(args.GetInt()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
        int led;
    };

    // XBox Controller rumble source
    class XBoxRumbleSource : public DataSource
    {
    public:
        XBoxRumbleSource(DataSourceArgs &args) : dir(args.GetInt()) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { }
        int dir;  // 1 = left rumble, 2 = right rumble
    };

    // Vector X component
    class XSource : public DataSource
    {
    public:
        XSource(DataSourceArgs &args) : source(args.GetSource()) { }
        XSource(DataSource *source) : source(source) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
        DataSource *source;
    };

    // Vector Y component
    class YSource : public DataSource
    {
    public:
        YSource(DataSourceArgs &args) : source(args.GetSource()) { }
        YSource(DataSource *source) : source(source) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
        DataSource *source;
    };

    // Vector magnitude
    class MagnitudeSource : public DataSource
    {
    public:
        MagnitudeSource(DataSourceArgs &args) : source(args.GetSource()) { }
        MagnitudeSource(DataSource *source) : source(source) { }
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(source); }
        DataSource *source;
    };

    // Arctan source
    class ArctanSource : public DataSource
    {
    public:
        ArctanSource(DataSourceArgs &args);
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(vec); func(x); func(y); }
        DataSource *vec = nullptr;
        DataSource *x = nullptr;
        DataSource *y = nullptr;
    };

    // Binary operand sources
    class BinOpSource : public DataSource
    {
    public:
        virtual SourceVal Calc() override;
        virtual void Traverse(TraverseFunc func) override { func(lhs); func(rhs); }
        DataSource *lhs, *rhs;
    };

    class ComparisonOpSource : public BinOpSource
    {
    public:
        virtual SourceVal Calc() override { return SourceVal::MakeUInt8(Compare(lhs->Calc().AsFloat(), rhs->Calc().AsFloat()) ? 255 : 0); }
        virtual bool Compare(float a, float b) const = 0;
    };
    class EqSource : public ComparisonOpSource { public: virtual bool Compare(float a, float b) const { return a == b; } };
    class NeSource : public ComparisonOpSource { public: virtual bool Compare(float a, float b) const { return a != b; } };
    class GtSource : public ComparisonOpSource { public: virtual bool Compare(float a, float b) const { return a > b; } };
    class GeSource : public ComparisonOpSource { public: virtual bool Compare(float a, float b) const { return a >= b; } };
    class LtSource : public ComparisonOpSource { public: virtual bool Compare(float a, float b) const { return a < b; } };
    class LeSource : public ComparisonOpSource { public: virtual bool Compare(float a, float b) const { return a <= b; } };
    class IdentSource : public BinOpSource { public: virtual SourceVal Calc() override; };
    class NIdentSource : public IdentSource { public: virtual SourceVal Calc() override; };

    class AddSource : public BinOpSource { public: virtual SourceVal Calc() override; };
    class SubtractSource : public BinOpSource { public: virtual SourceVal Calc() override; };
    class MulSource : public BinOpSource { public: virtual SourceVal Calc() override; };
    class DivSource : public BinOpSource { public: virtual SourceVal Calc() override; };
    class ModuloSource : public BinOpSource { public: virtual SourceVal Calc() override; };

    // Internal storage for the ports.  They're allocated here, and
    // indexed by number in portsByNumber, and by name in portsByName.
    static std::list<Port> portList;

    // Global output list, indexed by port number
    static std::vector<Port*> portsByNumber;

    // Named port map.  The config file can optionally assign an arbitrary,
    // user-specified name to each port, and this name can be used to refer
    // to the port elsewhere in the config file, rather than referring to
    // them by port number.  This makes port references in the config file
    // clearer to human readers, and it also makes the config file more
    // robust, since a port's name won't change if you insert or delete
    // 'outputs' array entries in the config file.
    static std::unordered_map<std::string, Port*> portsByName;

    // has the output manager been configured yet?
    static bool isConfigured;

    // is the output manager Task() handler suspended?
    static bool isSuspended;

    // suspend mode timeout, as the system clock time to restore normal operations
    static uint64_t suspendModeEndTime;

    // post-configuration callback list
    static std::list<std::function<void(JSONParser&)>> postConfigCallbacks;

    // console commands
    static void Command_out(const ConsoleCommandContext *ctx);
};
