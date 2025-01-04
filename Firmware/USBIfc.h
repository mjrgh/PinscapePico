// Pinscape Pico - USB interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the Pinscape Pico USB interfaces, based on the Pico SDK's
// TinyUSB library.
//
// Pinscape Pico is a Composite USB device, which allows it to expose a
// collection of standard and custom USB interfaces to the host.  Some
// of the interfaces are optional, included or not according to the
// user-configurable settings.
//
// - Configuration & Control (Vendor interface); always enabled.  This
//   is a private vendor-defined interface that provides the device's
//   configuration and control functions.  It's designed to work with a
//   Windows-based Config Tool that's also part of the Pinscape Pico
//   project.  Third-party applications can also access this interface
//   either directly through its USB protocol (which is documented in
//   VendorIfcProtocol.h) or through a host-side C++ API, also supplied
//   as part of the project.
//
//   A USB Vendor Interface by design requires a corresponding host-side
//   device driver.  On Windows, we use the WinUsb driver, which is a
//   native Windows component, built into the operating system, that
//   provides generic byte-stream endpoint communications between USB
//   devices and user-mode applications.  We provide the necessary USB
//   descriptors to trigger automatic plug-and-play WinUsb driver
//   access, so the driver setup is completely transparent to the user.
//
//   Linux and other systems can access the vendor interface via libusb,
//   which is essentially an open-source equivalent of WinUsb.  libusb
//   also runs on Windows, but we opted for WinUsb instead because of
//   its perfectly seamless user experience as a built-in Windows
//   component.  libusb is seamier on Windows in that it requires some
//   manual installation and driver setup by the end user.  At the
//   programming level, though, the two are quite similar, so I expect
//   that anyone who really wants a libusb-based API will find it
//   straightforward to port the WinUsb-based C++ API layer to libusb.
//
// - Feedback Controller (HID); always enabled.  This is a custom HID
//   interface that provides access to the Feedback Controller
//   functions, for use by DOF and other applications that use feedback
//   effects.  This is a HID interface, so it's driverless on all hosts.
//   The HID report structure is custom (i.e., it doesn't use any of the
//   standard device type layouts), so it only works with applications
//   that have been specifically programmed to speak its protocol, which
//   is documented in FeedbackControllerProtocol.h.  We supply a Windows
//   C++ API to access the Feedback Controller as part of this project,
//   so it can be incorporated into applications at a higher level than
//   the USB byte protocol.  DOF has a built-in driver for it, so all
//   DOF-based applications have automatic access to the Pinscape Pico
//   feedback controller functions through DOF.
//
// - Pinball Controller (HID); optional.  This uses the little-known
//   Pinball Device usage (Game Controllers Usage Page 0x05, Usage 0x02)
//   to continuously convey the state of the pinball-specific inputs to
//   the host.  This is an alternative to the traditional joystick axis
//   method of sending this information to the host that let us
//   overcome some of the limitations of the joystick approach.  Like
//   the Feedback Controller HID, this uses a custom wire protocol, with
//   a C++ API to parse it on the host side.
//
// - Keyboard (standard HID device); optional.  This interface emulates
//   a standard HID keyboard, to send input to the PC in the form of
//   keystrokes.  Keyboard input can be sent for physical button presses
//   and other events on the device side, configurable via the user
//   settings.
//
// - Gamepad (standard HID device); optional.  This interface emulates
//   a standard HID gamepad, with two joysticks (linear and rotation)
//   with three axes each (X, Y, Z), and 32 buttons.  All of the axes
//   and buttons can be individually mapped to physical inputs on the
//   device, such as accelerometer readings, plunger sensor readings,
//   and button presses.
//
// - XInput (XBox controller device); optional.  This emulates the
//   proprietary Microsoft XBox Controller device, which has two
//   joysticks with two axes each (X, Y), two analog triggers, and 14
//   buttons.  All of the controls can be mapped individually to
//   physical inputs on the devices.  The XInput interface is similar to
//   the Gamepad interface; we provide both beause some applications
//   recognize one but not the other, or work better with one or the
//   other.  Having both options gives users more flexibility to find a
//   configuration that works best with the unique collection of
//   software they're using.
//
// - CDC (virtual COM port); always enabled.  This interface is only for
//   debugging and troubleshooting.  It lets you connect a terminal to
//   the device to see logging messages and send ad hoc commands.  The
//   logging output can be useful to diagnose configuration problems
//   and hardware setup problems.
//

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <memory>
#include <bitset>

// Pico SDK headers
#include <pico/stdlib.h>

// Tinyusb headers
#include <tusb.h>
#include <bsp/board.h>
#include <class/hid/hid_device.h>

// local project headers
#include "ButtonHelper.h"
#include "XInput.h"
#include "JSON.h"
#include "Nudge.h"
#include "IRRemote/IRReceiver.h"
#include "../USBProtocol/FeedbackControllerProtocol.h"


// forward/external declarations
class ConsoleCommandContext;


// Logical axis control source.  This is used for gamepads and
// XInput controls to map an analog axis to a physical control, such
// as an accelerometer or plunger sensor.
//
// All of our HID reports for axis usages use a signed 16-bit range
// (-32768..+32767).  Physical devices implementing this class should
// normalize their readings to that range.
class LogicalAxis
{
public:
    // Read the current sample from the source, using a signed 16-bit
    // range (-32768..32767).
    virtual int16_t Read() = 0;

    // Read the current sample, using an unsigned 8-bit range.  By
    // default, this takes the positive side of the underlying 16-bit
    // axis scaled down to 8-bit precision.
    virtual uint8_t ReadUI8()
    {
        int16_t v = Read();
        return v < 0 ? 0 : static_cast<uint8_t>(v >> 7);
    }

    // Constructor parameters.  We package the subclass constructor
    // arguments into a struct to make it convenient to pass
    // essentially the union of arguments that the various
    // subclasses might need.
    struct CtorParams
    {
        CtorParams(const char *deviceName, NudgeDeviceView **ppNudgeDeviceView) :
            deviceName(deviceName), ppNudgeDeviceView(ppNudgeDeviceView) { }

        // Get the nudge device view object.  If the containing device
        // already has a view object, we'll return the existing one.  If
        // not, we'll create a new one, set the pointer in the parent
        // object, and return the new object.  This lets the parent
        // object (e.g., a gamepad USB interface) defer creation of a
        // view object until we know for sure that it's actually needed.
        // Each view object has a slight run-time cost, because the
        // accelerometer will update its averaging counters on every new
        // physical sample taken from the accelerometer, so we can save
        // some unnecessary overhead by creating these on demand.  Each
        // device can share one view among all of its axes, because a
        // given device reports all of its axes at the same time, hence
        // it just needs one snapshot per report across all of its axes.
        NudgeDeviceView *GetNudgeDeviceView() const;
        
        // name of the device, for error reporting
        const char *deviceName;

        // pointer to pointer to nudge device view object
        NudgeDeviceView **ppNudgeDeviceView;

        // property name
        const char *propName = nullptr;

        // add a property name to an existing set of parameters
        CtorParams(const CtorParams &params, const char *propName)
        {
            memcpy(this, &params, sizeof(params));
            this->propName = propName;
        }
    };
    
    // Configure a logical axis from JSON settings.  This returns a
    // logical axis source based on the given property of the given
    // configuration object.  Logs an error on failure.  If the
    // source isn't configured or an error occurs, returns the
    // NullAxisSource singleton.  (That means this never returns a
    // null pointer, so the caller can read from the returns pointer
    // unconditionally.)
    //
    // The property name doesn't have to be filled in in the
    // CtorParams struct, as it's passed as an extra parameter (for
    // syntactic convenience when initializing a set of properties).
    static LogicalAxis *Configure(
        const CtorParams &params, const JSONParser::Value *obj, const char *prop, const char *defaultValue = nullptr);

    // configure from a string (taken from a property value)
    static LogicalAxis *Configure(
        const CtorParams &params, const char *str, const char *prop);

    // Add a logical axis type.  Devices that expose analog sources that
    // can be mapped as logical axes call this during device
    // configuration to add themselves to the name table.
    using CreateFunc = std::function<LogicalAxis*(const CtorParams &params, std::vector<std::string>&)>;
    static void AddSource(const char *name, CreateFunc createFunc) { nameMap.emplace(name, createFunc); }

protected:
    // Logical axis source map.  This maps the configuration names
    // for the logical axis sources to functions that create the
    // corresponding axis reader objects.  Sources for the built-in
    // subsystems (plunger, nudge) and operators (negate, offset,
    // scale, abs) are always available.  ADCs and accelerometers
    // add device-specific sources when they're configured.
    static std::unordered_map<std::string, CreateFunc> nameMap;
};

// Null axis source.  We define a global singleton that we use
// during initialization, and for axes the aren't mapped to any
// underlying physical input source.  Reading this source just
// always returns zero.
class NullAxisSource : public LogicalAxis
{
public:
    virtual int16_t Read() override { return 0; }
};
extern NullAxisSource nullAxisSource;

// Nudge Device axis control sources
class NudgeXAxisSource : public LogicalAxis
{
public:
    NudgeXAxisSource(const CtorParams &params) { nudgeDeviceView = params.GetNudgeDeviceView(); }
    virtual int16_t Read() override { return nudgeDeviceView->GetX(); }
    NudgeDevice::View *nudgeDeviceView;
};
class NudgeYAxisSource : public LogicalAxis
{
public:
    NudgeYAxisSource(const CtorParams &params) { nudgeDeviceView = params.GetNudgeDeviceView(); }
    virtual int16_t Read() override { return nudgeDeviceView->GetY(); }
    NudgeDevice::View *nudgeDeviceView;
};
class NudgeZAxisSource : public LogicalAxis
{
public:
    NudgeZAxisSource(const CtorParams &params) { nudgeDeviceView = params.GetNudgeDeviceView(); }
    virtual int16_t Read() override { return nudgeDeviceView->GetZ(); }
    NudgeDevice::View *nudgeDeviceView;
};
class NudgeVXAxisSource : public LogicalAxis
{
public:
    NudgeVXAxisSource(const CtorParams &params) { }
    virtual int16_t Read() override { return nudgeDevice.GetVelocityX(); }
};
class NudgeVYAxisSource : public LogicalAxis
{
public:
    NudgeVYAxisSource(const CtorParams &params) { }
    virtual int16_t Read() override { return nudgeDevice.GetVelocityY(); }
};
class NudgeVZAxisSource : public LogicalAxis
{
public:
    NudgeVZAxisSource(const CtorParams &params) { }
    virtual int16_t Read() override { return nudgeDevice.GetVelocityZ(); }
};


// Plunger axis control source - raw sensor readings
class PlungerSensorRawAxisSource : public LogicalAxis
{
public:
    PlungerSensorRawAxisSource(const CtorParams &params) { }
    virtual int16_t Read() override;
};

// Plunger axis control source - Z axis readings, with Pinscape corrections for VP
class PlungerZAxisSource : public LogicalAxis
{
public:
    PlungerZAxisSource(const CtorParams &params) { }
    virtual int16_t Read() override;
};

// Plunger axis control source - Z axis readings, without Pinscape corrections for VP
class PlungerZ0AxisSource : public LogicalAxis
{
public:
    PlungerZ0AxisSource(const CtorParams &params) { }
    virtual int16_t Read() override;
};

// Plunger impulse reading
class PlungerSpeedAxisSource : public LogicalAxis
{
public:
    PlungerSpeedAxisSource(const CtorParams &params) { }
    virtual int16_t Read() override;
};

// Logical axis negating source.  This reverses the sign of
// an underlying axis source.
class NegativeAxisSource : public LogicalAxis
{
public:
    NegativeAxisSource(const CtorParams &params, std::vector<std::string> &args);
    virtual int16_t Read() override { return -source->Read(); }
    LogicalAxis *source = &nullAxisSource;
};

// Logical axis offset source.  This adds a constant offset to an
// underlying axis source.
class OffsetAxisSource : public LogicalAxis
{
public:
    OffsetAxisSource(const CtorParams &params, std::vector<std::string> &args);
    virtual int16_t Read() override {
        int i = source->Read() + offset;
        return static_cast<int16_t>((i < -32768 ? -32768 : i > 32767 ? 32767 : i));
    }
    LogicalAxis *source = &nullAxisSource;
    int offset = 0;
};

// Logical axis scaling source.  This scales the underlying axis source
// by a constant factor.
class ScaleAxisSource : public LogicalAxis
{
public:
    ScaleAxisSource(const CtorParams &params, std::vector<std::string> &args);
    virtual int16_t Read() override {
        float f = static_cast<float>(source->Read()) * scale;
        return static_cast<int16_t>((f < -32768.0f ? -32768.0f : f > 32767.0f ? 32767.0f : f));
    }
    LogicalAxis *source = &nullAxisSource;
    float scale = 1.0f;
};

// Logical axis absolute value source.  This scales the underlying axis source
// by a constant factor.
class AbsAxisSource : public LogicalAxis
{
public:
    AbsAxisSource(const CtorParams &params, std::vector<std::string> &args);
    virtual int16_t Read() override { return static_cast<int16_t>(abs(source->Read())); }
    LogicalAxis *source = &nullAxisSource;
};

// Sine wave control source.  This returns a generated sine wave,
// which is useful mostly for testing.
class SineAxisSource : public LogicalAxis
{
public:
    SineAxisSource(const CtorParams &params, std::vector<std::string> &args);
    virtual int16_t Read() override {
        return static_cast<int16_t>(roundf(32767.0f * sinf(
            static_cast<float>((time_us_64() + phase) % period)
            * 2.0f*3.14159265f / static_cast<float>(period))));
    }
    uint32_t period = 2000000;  // period in microseconds
    uint32_t phase = 0;         // phase offset in microseconds
};

// Main USB interface.  This presents a collection of USB sub-devices to
// the host, bundled together into a USB composite device interface.
class USBIfc
{
public:
    USBIfc();
    virtual ~USBIfc();

    // get the VID/PID
    uint16_t GetVID() const { return vid; }
    uint16_t GetPID() const { return pid; }

    // console 'usbstats' command
    static void Command_usbstats(const ConsoleCommandContext *ctx);

    // configure the USB interface
    void Configure(JSONParser &json);

    // has the device EVER been mounted (since the last reset)?
    bool WasEverMounted() const { return everMounted; }

    // is the device mounted (connected to host)?
    bool IsMounted() const { return mounted; }

    // active connection status
    bool IsConnectionActive() const { return mounted && !suspended; }

    // suspended status
    bool IsSuspended() const { return suspended; }

    // Vendor transmission completion event callback
    void OnVendorTX(uint8_t ifc, uint32_t nBytesSent) { }

    // device mount state change events
    void OnMountDevice() { mounted = everMounted = true; suspended = false; OnDeviceStateChanged(); }
    void OnDismountDevice() { mounted = false; OnDeviceStateChanged(); }

    // bus suspend/resume (low-power state) events
    void OnBusSuspended(bool remoteWakeEnabled);
    void OnBusResumed();

    // mounting status and/or bus suspend/resume state changed
    void OnDeviceStateChanged();

    // get the HID device list
    class HIDIfc;
    const std::vector<std::unique_ptr<HIDIfc>> &GetHIDIfcs() const { return hidIfcs; }

    // Interface numbers
    static const uint8_t IFCNUM_CDC = 0;       // note - CDC counts as two interfaces (data + notification)
    static const uint8_t IFCNUM_VENDOR = 2;    // Pinscape vendor interface, for our control & configuration interface
    static const uint8_t IFCNUM_XINPUT = 3;    // XInput (xbox game controller) interace (Microsoft-defined custom class interface)
    static const uint8_t IFCNUM_HID0 = 4;      // first HID interface; additional HID interfaces use sequential values from here

    // Vendor interface instance numbers.  These are the indices of our respective
    // vencor interfaces in Tinyusb's internal list.  Note that these are NOT the
    // same as the USB interface IDs; these are just the internal array indices
    // that tusb assigns, starting at 0, in the order in which the interfaces are
    // listed in the configuration descriptor.  These must be kept in sync with
    // the configuration descriptor.
    static const int VENDORIFC_IDX_CONFIG = 0;   // our Config & Control interface
    static const int VENDORIFC_IDX_XINPUT = 1;   // xbox controller interface

    // String descriptor indices
    static const uint8_t STRDESC_LANG = 0;          // USB standard entry for language identifier code
    static const uint8_t STRDESC_MANUF = 1;         // USB standard entry for manufacturer name string
    static const uint8_t STRDESC_PRODUCT = 2;       // USB standard entry for product name string
    static const uint8_t STRDESC_SERIAL = 3;        // USB standard entry for device serial number string
    static const uint8_t STRDESC_VENIFC = 4;        // display name of our Configuration & Control vendor interface
    static const uint8_t STRDESC_XINPUTIFC = 5;     // display name for our XInput interface
    static const uint8_t STRDESC_CDCIFC = 6;        // display name for our CDC interface
    static const uint8_t STRDESC_FEEDBACK_LBL = 70; // HID usage label for feedback controller reports
    static const uint8_t STRDESC_OPENPINDEV_LBL = 71; // HID usage label for OpenPinballDeviceReport struct type

    // Endpoints
    static const uint8_t EndpointOutCDC = 0x01;     // CDC data out
    static const uint8_t EndpointInCDC = 0x81;      // CDC data in
    static const uint8_t EndpointNotifyCDC = 0x82;  // CDC notification in
    static const uint8_t EndpointOutVendor = 0x03;  // Vendor interface (WinUSB) data out
    static const uint8_t EndpointInVendor = 0x83;   // Vendor interface (WinUSB) data in
    static const uint8_t EndpointOutXInput = 0x04;  // XInput data out
    static const uint8_t EndpointInXInput = 0x84;   // XInput data in
    static const uint8_t EndpointOutHID0 = 0x05;    // First HID out
    static const uint8_t EndpointInHID0 = 0x85;     // First HID in
    // Note - additional HID interfaces will use subsequent endpoints, so
    // HID0 should be the highest numbered in the pre-assigned list.

    // Vendor device request codes, for BOS descriptor requests
    static const uint8_t VENDOR_REQUEST_WINUSB = 1;

    // Device type serial number uniquifier bits.  Assign a unique bit
    // to each OPTIONAL device type.  This is used to generate a
    // different USB Device Serial Number string to the host for each
    // possible combination of devices that we can be configured for.
    // Windows caches some of the USB descriptor information when a
    // device first connects, using the device's reported serial number
    // as the cache key, so we have to use a unique serial number for
    // each distinct configuration to prevent Windows from reusing
    // cached descriptors across our configurations.  The descriptors
    // tell Windows which device drivers to use and how to interpret
    // data packets sent and received on the wire, so it'll cause all
    // sorts of problems if Windows tries to use cached descriptors for
    // the wrong configuration.
    //
    // Note that we do still want to allow the host to cache descriptors
    // when appropriate, since it greatly speeds up system startup times
    // and device connection times, so we don't want to do anything as
    // heavy-handed as always generating a unique serial number for
    // every connection - that would defeat caching entirely.  Instead,
    // we must generate a serial number that's unique but also stable
    // *per configuration*, where a "configuration" is essentially the
    // overall set of USB descriptors that we generate.  All of the
    // variability in our USB descriptors comes from some of our virtual
    // devices being optional, so a configuration amounts to a list of
    // which devices are enabled.
    //
    // Note that Medie Control device type doesn't need its own bit,
    // because it's always paired with the Keyboard device.  We just
    // need one bit for the Keyboard/Media Control combination.
    static const uint8_t SerialBitKeyboard = 0x01;
    static const uint8_t SerialBitGamepad  = 0x02;
    static const uint8_t SerialBitXInput   = 0x04;
    static const uint8_t SerialBitPinballDevice = 0x08;

    // HID device interface.  This encapsulates a single HID device type
    // (keyboard, mouse, joystick, gamepad, etc).
    class HID
    {
    public:
        HID(const char *name, bool inOut, uint8_t reportID, uint8_t serialNoBit) :
            name(name), isInOut(inOut), reportID(reportID), serialNoBit(serialNoBit) { }
        virtual ~HID() { }

        // Desired maximum HID polling interval, in milliseconds
        virtual int GetMaxPollingInterval() const { return 8000; }

        // Display name, for logging purposes
        const char *name;

        // Is this an In/Out device?  true -> has both input and output
        // reports; false -> input (device-to-host) only
        bool isInOut;

        // Interface instance.  This is the Tinyusb index of the HID
        // interface that the device is associated with.  (Note that this
        // isn't the same as the nominal USB interface number - this is the
        // internal index that Tinyusb uses for just the HID interfaces.)
        int ifcInstance = 0;

        // HID report ID for this device
        uint8_t reportID = 0;

        // Unique serial number bit for this device.  Each device type must
        // provide a bit to OR into the serial number, so that we generate a
        // different serial number for each combination of devices.  Windows
        // uses the serial number as a key for cached configuration data, so
        // it's important that we generate a different serial number for each
        // distinct configuration (i.e., each combination of devices exposed
        // in the composite interface) so that Windows doesn't try to reuse
        // the cached data from one congifuration with another.  For example,
        // if we're configured as a joystick on one occasion, and later
        // reconfigured as a keyboard, Windows would try to apply the joystick
        // configuration to the keyboard device if we gave it the same serial
        // number on both occasions.  That would make Windows try to parse
        // the keyboard reports in terms of the joystick report descriptor,
        // which would yield garbage input data.  Changing the serial number
        // forces Windows to query the new descriptors for each configuration.
        // This is provided at construction by the subclass's invocation
        // of the base class constructor.
        uint8_t serialNoBit;

        // device configured
        bool configured = false;

        // Reporting enabled; set to false to disable sending reports to the
        // host.  This can be used to temporarily disable reports from this
        // device without having to change the configuration to remove the
        // interface entirely.  This doesn't affect the visibility of the
        // USB interface on the host/PC side - it will still see the device
        // as present and functioning.
        bool enabled = true;

        // Report IDs.  These are arbitrary identifiers for our various
        // report types.  These distinguish report types within a single
        // HID interface.  (At the HID level, they only need to be
        // unique within a single USB interface, but we define them to
        // be unique across the whole device, since we assign HIDs to
        // USB interfaces dynamically during the configuration process
        // and thus can't predict statically which HIDs will share a
        // single USB interface.  There's no harm in making them unique
        // across the whole device other than the limited 8-bit
        // namespace, but we don't come close to exhausing that.)
        //
        // Note that software on the PC side can detect the report ID
        // dynamically, so there's no absolute requirement that these be
        // permanent.  They could be assigned dynamically instead of
        // pre-assigned.  But it's simpler for PC clients if we define
        // the IDs statically and permanently so that clients can just
        // hard-code the IDs and consider them part of the protocol.
        static const uint8_t ReportIDKeyboard = 1;
        static const uint8_t ReportIDMediaControl = 2;
        static const uint8_t ReportIDGamepad = 3;
        static const uint8_t ReportIDFeedbackController = 4;
        static const uint8_t ReportIDPinballDevice = 5;

        // cross-check the report ID for the feedback device against the external API header
        static_assert(ReportIDFeedbackController == PinscapePico::FEEDBACK_CONTROLLER_HID_REPORT_ID);

        // Report descriptor for this device.  The storage must be static,
        // since in most cases it's pre-iniitialized const data.  If the
        // device has to generate the data dynamically, it must be in a
        // static buffer or a malloc'd buffer that lasts as long as 'this'.
        virtual const uint8_t *GetReportDescriptor(uint16_t *byteLength) = 0;

        // Send the device's report.  Returns true if a report was sent,
        // false if the device has nothing to report for this cycle.  By
        // default, we call GetReport() to build the report in a local
        // buffer, then send the result on the wire.
        virtual bool SendReport();

        // Get a device-to-host report
        virtual uint16_t GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen) = 0;

        // Receive a host-to-device report
        virtual void SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen) = 0;

        // Report timing stats
        struct Stats
        {
            // number of reports started/finished
            uint64_t nReportsStarted = 0;
            uint64_t nReportsCompleted = 0;

            // number of reports completed with events started during the report,
            // for calculating event delivery latency
            uint64_t nReportsCompletedWithEvents = 0;

            // cumulative time between start of send and completion notification, in microseconds
            uint64_t totalCompletionTime = 0;

            // cumulative time between consecutive report starts, in microseconds
            uint64_t totalTimeBetweenReports = 0;

            // last report start time and completion time
            uint64_t tSendStart = 0;
            uint64_t tSendComplete = 0;

            // Timestamp of FIRST physical event of the current reporting cycle.
            // This is zero if no event has occurred during this cycle yet.
            uint64_t tFirstPhysicalEventOfCycle = 0;

            // Timestamp of first event for current report being sent
            uint64_t tFirstPhysicalEventOfSend = 0;

            // Total event latency.  This is the cumulative time between a physical
            // event that generates input to the logical device (e.g., the user presses
            // a button that's assigned to this logical device) and the completion of
            // the next HID report transmission to the host.  That interval represents
            // the total latency between the physical event's occurrence and the
            // transmission of the event data to the host.
            uint64_t totalEventLatency = 0;

            // reset stats
            void Reset()
            {
                nReportsStarted = 0;
                nReportsCompleted = 0;
                nReportsCompletedWithEvents = 0;
                totalCompletionTime = 0;
                totalTimeBetweenReports = 0;
                totalEventLatency = 0;
            }

            // Flag a physical event occurrence.  Call this when an event that
            // generates data to send to the host via this logical device occurs
            // on a physical input assigned to the logical device.  For example,
            // if a button is assigned to this device as a keyboard key, call
            // this when the user presses or releases the button, since the
            // state change of the physical button will generate a HID keyboard
            // event to send to the host.
            void MarkEventTime(uint64_t t)
            {
                // if there's not an event on this cycle already, this is the
                // first event - record its timestamp
                if (tFirstPhysicalEventOfCycle == 0)
                    tFirstPhysicalEventOfCycle = t;
            }

            // Start a report
            void StartReport(uint64_t t)
            {
                // count the report started and note the start time
                nReportsStarted += 1;
                tSendStart = t;

                // snapshot the physical event start time for this cycle
                tFirstPhysicalEventOfSend = tFirstPhysicalEventOfCycle;
                tFirstPhysicalEventOfCycle = 0;
            }

            // Complete a report
            void CompleteReport(uint64_t t)
            {
                // count the report completed
                nReportsCompleted += 1;

                // count the completion time and time between consecutive reports
                totalCompletionTime += t - tSendStart;
                totalTimeBetweenReports += t - tSendComplete;

                // if a physical event was recorded for this cycle, count the
                // time between the event and the completion of the send as
                // the latency for this event
                if (tFirstPhysicalEventOfSend != 0)
                {
                    totalEventLatency += t - tFirstPhysicalEventOfSend;
                    nReportsCompletedWithEvents += 1;
                    tFirstPhysicalEventOfSend = 0;
                }

                // mark the new completion time
                tSendComplete = t;
            }


            // log my stats to a command console session
            void Log(const ConsoleCommandContext *ctx, HID *hid);
        } stats;
    };

    // Keyboard HID type
    class Keyboard : public HID
    {
    public:
        Keyboard();

        // configure; returns true if enabled
        bool Configure(JSONParser &json);

        // console 'kb' command - temporary keyboard configuration changes
        static void Command_kb(const ConsoleCommandContext *ctx);

        // HID reporting
        virtual const uint8_t *GetReportDescriptor(uint16_t *byteLength) override;
        virtual bool SendReport() override;
        virtual uint16_t GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen) override;
        virtual void SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen) override;

        // use low-latency HID polling
        virtual int GetMaxPollingInterval() const override { return 1000; }

        // Process a key event.  This queues the key press or release for
        // the next USB report.  The key code is a usage code from USB Usage
        // Page 0x07 (Keyboard/Keypad), which defines the standard set of
        // keyboard keys  'down' is true if the key was pressed, false if it
        // was released.
        void KeyEvent(uint8_t keyCode, bool down);

        // Map the USB Usage Page 0x07 (Keyboard/Keypad) code for a modifier
        // key (shift, ctrl, alt, GUI) to the corresponding bit in the mod
        // key bit vector in our HID reporting format.  Returns 0 if the key
        // isn't a modifier key.
        uint8_t ModKeyToBitMask(uint8_t keyCode);

        // LED state
        uint8_t ledState = 0;

        // Current "live" key state, as a 256-bit vector.  This represents
        // the latest state of the keys as reported through KeyEvent()
        // calls.
        std::bitset<256> liveKeys;

        // New key state.  This is the key state that we'll send in the
        // next USB report.  When a live key changes state, we'll check
        // to see if this represents a change from the last report, and
        // if so, we'll record it here.  We don't update this from the
        // live keys if the live key reverts back to the same state as
        // in the last report, so that we can ensure that we send at
        // least one report in the new state when a key is tapped so
        // quickly that it switches ON and then back OFF within the
        // space of one USB polling interval.
        std::bitset<256> nextKeys;

        // Last reported key state
        std::bitset<256> reportedKeys;
    };

    // Media control HID type.  This is essentially part of the keyboard
    // interface, to handle the special media control buttons found on some
    // fancier keyboards.  The USB spec defines the media buttons on a
    // separate interface from the basic keyboard interface, with its own
    // reports, which is why we need to put this in a separate class from
    // the keyboard.  We assume that we'll always create a media control
    // interface when a keyboard is present, and only then, so this class
    // doesn't need a separate serial number bit (it just piggybacks on the
    // keyboard's).
    class MediaControl : public HID
    {
    public:
        MediaControl() : HID("media", false, ReportIDMediaControl, 0) { }
        virtual const uint8_t *GetReportDescriptor(uint16_t *byteLength) override;
        virtual bool SendReport() override;
        virtual uint16_t GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen) override;
        virtual void SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen) override { }

        // use low-latency HID polling
        virtual int GetMaxPollingInterval() const override { return 1000; }

        // Internal IDs for the media keys.  These are arbitrary internal
        // codes used only within this class - they're not the Windows
        // keyboard codes or USB usage codes for the respective keys.
        // Note that these are arranged in the same order as the bits
        // for the respective keys in the HID report format, so we can
        // get a bit mask for the HID format with (1 << enum int value).
        enum class Key
        {
            INVALID = -1,
            MUTE = 0,
            VOLUME_UP = 1,
            VOLUME_DOWN = 2,
            NEXT_TRACK = 3,
            PREV_TRACK = 4,
            STOP = 5,
            PLAY_PAUSE = 6,
            EJECT = 7
        };

        // Get the usage code associated with the given key code
        static uint8_t GetUsageFor(Key key);

        // Media key make/break event.  Call this when a physical input
        // control (e.g., a pushbutton or remote control code input) or a
        // virtual input (e.g., a timer) mapped to a media key changes
        // state.  isDown is true when the key is pressed, false when
        // released.
        void KeyEvent(Key key, bool isDown);

    protected:
        // Current "live" key state, as a bit mask in our HID reporting
        // format.  This represents the latest state of the keys as
        // reported through KeyEvent() calls.
        uint8_t liveKeys = 0;

        // New key state, as a bit mask in our HID report format.  This
        // is the key state that we'll send in the NEXT report.  Whenever
        // a live key changes state, we'll check to see if we've recorded
        // a pending state change here, and if not, we'll propagate it
        // here.  Once a key changes state here, the change sticks until
        // the next USB update.  This ensures that a rapid press/release
        // action that happens within a USB polling cycle gets reported
        // to the host with the key down for one USB cycle.  (This can't
        // capture multiple quick hits between cycles - multiple hits
        // will only get reported as a single hit.  But that's really
        // not a problem, since the reporting cycle is fast enough that
        // multiple hits are essentially impossible after taking the
        // necessity of debouncing into account.  Debouncing amounts to
        // a low-pass filter on the key state change frequency, with a
        // minimum time between keys that's roughly on par with the USB
        // polling interval.)
        uint8_t nextKeys = 0;

        // Last report key state, as a bit mask in our HID report format.
        // This is the key state snapshot as of our last report.
        uint8_t reportedKeys = 0;

        // Table of usages, in order of the Key enum, which is the same
        // order as the bits in our report byte.
        static const uint8_t usages[];
    };

    // Gamepad HID type.  This is a generic gamepad, not based on any
    // particular brand or model of real-world device.  It features six
    // joystick axes (linear X/Y/Z, rotational RX/RY/RZ) with 16-bit
    // resolution, two sliders with 16-bit resolution, and 32 generic
    // buttons.
    //
    // There are no conventions on the PC for what the joystick buttons
    // mean.  They're just a collection of numbered buttons that can be
    // assigned concrete meanings per application.
    //
    // Most of the major pinball simulators agree on a basic convention for
    // the joystick axis mappings: accelerometer on X/Y, plunger on Z.
    // However, the mappings are freely configurable in most of the
    // simulators, so you can usually rearrange things as needed.  One
    // common reason for changing the conventional mapping is that some
    // non-pinball games inflexibly interpret X/Y as an actual joystick, so
    // maping the accelerometer to X/Y can cause problems for such games due
    // to the constant low-level jiggling it receives from sensor noise.
    // The standard solution is to remap the accelerometer to RX/RY, since
    // fewer non-pinball games attach any meaning to those.
    //
    // The sliders don't have any standard mappings in the pinball games.
    // VP can read up to two sliders, allowing them to be mapped to any of
    // the input functions that joystick axes can take on, so they give us a
    // couple more options for resolving conflicts with other games.
    class Gamepad : public HID
    {
    public:
        Gamepad();

        // console 'gamepad' command - temporary gamepad configuration changes
        static void Command_gamepad(const ConsoleCommandContext *ctx);

        // configure; returns true if enabled
        bool Configure(JSONParser &json);

        // validate a button ID
        bool IsValidButton(int n) { return n >= 1 && n <= 32; }

        // HID reporting
        virtual const uint8_t *GetReportDescriptor(uint16_t *byteLength) override;
        virtual uint16_t GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen) override;
        virtual void SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen) override { }

        // use low-latency HID polling
        virtual int GetMaxPollingInterval() const override { return 1000; }

        // Handle a button state change.  buttonNum is in 1..32.
        void ButtonEvent(int buttonNum, bool isDown);

        // Nudge device averaging viewer
        NudgeDevice::View *nudgeDeviceView = nullptr;

        // Logical axis sources
        LogicalAxis *xSource = &nullAxisSource;
        LogicalAxis *ySource = &nullAxisSource;
        LogicalAxis *zSource = &nullAxisSource;
        LogicalAxis *rxSource = &nullAxisSource;
        LogicalAxis *rySource = &nullAxisSource;
        LogicalAxis *rzSource = &nullAxisSource;
        LogicalAxis *slider1Source = &nullAxisSource;
        LogicalAxis *slider2Source = &nullAxisSource;

        // button states
        ButtonHelper buttons;

        // Report length, in bytes.  THIS MUST BE UPDATED TO KEEP IN
        // SYNC WITH THE REPORT LAYOUT.
        static const int ReportLength = 20;

        // Capture buffer for last report.  We use this to suppress
        // reports when nothing has changed since the last one.
        uint8_t lastReportCapture[ReportLength];
    };

    // Feedback controller HID interface (for DOF access)
    //
    // This is a custom HID interface that lets the host control feedback
    // devices (lights, solenoids, motors, etc) connected to on the Pinscape
    // board.  Most applications will access this through DOF, for which
    // we'll provide a custom driver that speaks our custom protocol.
    // 
    // Although we use a HID foundation, we don't try to emulate any
    // standard HID device (keyboard, joystick, mouse, etc).  Generic HID
    // clients on the PC won't recognize this device and won't know what to
    // do with it; it's only usable by clients that are specifically aware
    // of it, which for the most part means DOF clients.  The reason we use
    // HID for this otherwise vendor-specific interface, rather than
    // explicitly using a USB vendor interface, is simply that Windows has
    // better native driver support for HID.  Devices with vendor interfaces
    // are generally expected to come with their own Windows kernel-mode
    // device drivers, or lacking that, to use WinUsb, which has some
    // significant limits.  HID is frictionless for the user.
    //
    // This interface is conceptually similar to the LedWiz's HID interface,
    // but it's NOT LedWiz-compatible.  The original Pinscape firmware did
    // provide a full LedWiz emulation at the USB HID level, but we chose
    // not to include that in this newer system.  For one thing, hardly
    // anyone in the virtual pinball world uses software that depends on the
    // LedWiz at all any more; practically all of the software still in use
    // is DOF-aware, allowing it to work with any device that DOF supports.
    // For another, even for the remaining legacy LedWiz-only software, USB
    // HID-level compatibility isn't important, because virtually all legacy
    // LedWiz-aware software access the device through the DLL that the
    // vendor supplies rather than directly through the USB protocol.  We
    // can thus make almost any device emulate an LedWiz by replacing that
    // DLL with one that translates to the other device's USB protocol.
    // That actually allows for a superior emulation because it lets us
    // expose a single Pinscape device through the DLL as a collection of
    // virtual LedWiz devices, so that legacy software is able to access
    // the larger set of output ports we provide.
    //
    // The feedback controller doesn't require a serial number bit because
    // it's a fixed feature of all configurations.
    class FeedbackController : public HID, public IRReceiver::Subscriber
    {
    public:
        FeedbackController() : HID("feedback", true, ReportIDFeedbackController, 0) { }

        // HID reporting
        virtual const uint8_t *GetReportDescriptor(uint16_t *byteLength) override;
        virtual uint16_t GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen) override;
        virtual void SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen) override;

        // initialize - call during main loop setup
        void Init();

        // IR Receiver event subscriber
        virtual void OnIRCommandReceived(const IRCommandReceived &command, uint64_t dt) override;

    protected:
        // Pending INPUT (device-to-host) report types, as requested by
        // the host.  Since each report type is essentially an
        // instantaneous state report, we don't have to queue individual
        // requests, but rather just keep track of each type that has
        // been requested so that we can generate one report of that
        // type.  We use a simple bit vector for the reports by type,
        // where (1 << (Message_Type_Byte-1)) is the bit for a message
        // type (so bit 0 == 0x00000001 is message type 1).  Currently,
        // there are only a few report types in the single digits, so an
        // int32 is more than adequate to cover them all.
        //
        // On each input polling cycle, we'll select one bit that's
        // set, send the corresponding report, and clear the bit.
        uint32_t pendingInputReports = 0;

        // Previous request type and count, for logging.  This lets us
        // consolidate logging for repeated requests to avoid excessive
        // reporting.
        uint8_t prvRequestCmd = 0;
        uint8_t prvRequestCnt = 0;
        uint64_t prvRequestBatchStartTime = 0;

        // Continuous status reporting mode.  If this is set, we'll
        // send status reports (message type 0x02) on every polling
        // cycle where we don't have any other pending input reports
        // to send.  (Other reports take priority, because they're
        // one-offs that clear as soon as we send them.)
        bool continuousStatusMode = false;

        // Queue of IR codes received.  We report all IR codes received
        // to the host, for host-side processing of arbitrary commands.
        // This is a fixed-size circular buffer; we'll discard old items
        // if the buffer overflows, since delivery doesn't have to be
        // perfectly reliable.
        static const int IRCmdBufSize = 16;
        struct
        {
            IRCommandReceived cmd;
            uint64_t dt;
        } irCmdBuf[IRCmdBufSize];
        int irCmdRead = 0;
        int irCmdWrite = 0;
    };

    // Pinball Device HID interface.  Implements usage page 0x05 (Game
    // Controls), usage 0x02 (Pinball Device) from the published USB
    // usage tables.
    class OpenPinballDevice : public HID
    {
    public:
        OpenPinballDevice();

        // HID reporting
        virtual const uint8_t *GetReportDescriptor(uint16_t *byteLength) override;
        virtual uint16_t GetReport(hid_report_type_t type, uint8_t *buf, uint16_t reqLen) override;
        virtual void SetReport(hid_report_type_t type, const uint8_t *buf, uint16_t reqLen) override { }

        // use low-latency HID polling
        virtual int GetMaxPollingInterval() const override { return 1000; }

        // console 'pindev' command - temporary pinball device configuration changes
        static void Command_pindev(const ConsoleCommandContext *ctx);

        // configure; returns true if enabled
        bool Configure(JSONParser &json);

        // initialize - call during main loop setup
        void Init();

        // Handle a generic (numbered) button state change; buttonNum is in 1..32
        void GenericButtonEvent(int buttonNum, bool isDown);

        // Handle a pinball button state change; buttonNum is the index for the
        // button defined in the spec, 0..31
        void PinballButtonEvent(int buttonNum, bool isDown);

    protected:
        // Nudge device averaging viewer
        NudgeDevice::View *nudgeDeviceView = nullptr;

        // Nudge accelerations
        LogicalAxis *axNudgeSource = &nullAxisSource;
        LogicalAxis *ayNudgeSource = &nullAxisSource;

        // Nudge integrated velocities
        LogicalAxis *vxNudgeSource = &nullAxisSource;
        LogicalAxis *vyNudgeSource = &nullAxisSource;

        // Plunger
        LogicalAxis *plungerPosSource = &nullAxisSource;
        LogicalAxis *plungerSpeedSource = &nullAxisSource;

        // button helpers for the simple on/off buttons
        ButtonHelper genericButtons;
        ButtonHelper pinballButtons;
    };

    // HID interface instance.  This is a collection of logical HID devices
    // grouped into a single USB interface within the configuration.  The
    // HID spec allows mutiple logical devices (e.g., a keyboard and a media
    // controller) to share a single HID interface by assigning a unique
    // non-zero report ID to each logical device sharing the interface.
    // In addition, since our top-level USB device is defined as a composite
    // device with multiple interfaces (our private vendor interface, a CDC
    // interface for a virtual COM port, and one or more HID interfaces), we
    // can present multiple HID interfaces at the top level.  We thus can
    // choose how to distribute our logical HID device across interfaces:
    // one HID interface containing multiple logical HID devices, multiple
    // HID interfaces containing one logical device each, or multiple HID
    // interfaces containing one or more logical devices each.
    //
    // Each HID interface consumes two USB endpoints, and endpoints are
    // a limited resource, so there's a resource conservation advantage to
    // minimizing the number of HID interfaces by combining devices into
    // interfaces, ideally combining all of the logical devices into a
    // single interface.  But there's a competing factor, which is that
    // the host only polls each interface once per polling interval
    // (which is determined by the host, and is typically about 10ms
    // with a Windows host), no matter how many logical devices the
    // interface contains.  So if we were to combine ten logical devices
    // under a single interface, for example, and all ten devices
    // produced input reports continuously, each device's reports
    // would have a potential latency of up to 10x the host's polling
    // interval, which could be an unacceptable 100ms of latency.
    // Since the host polls each interface separately, we can minimize
    // latency by placing each logical device under a separate interface.
    // In practice, though, we can achieve low latency without giving
    // each device its own interface, because most devices don't
    // generate input reports continuously.  We can minimize latency
    // without exhausting our endpoints by ensuring that each interface
    // only contains one device that generates high-priority input.
    // For our purposes, the high-priority input devices are keyboards
    // and joysticks/gamepads.  Other devices either don't generate
    // input reports (device-to-host) or don't require low latency.
    // Here's how we can arrange our devices to achieve this:
    //
    // HID interface 0
    //     keyboard           - high-priority button input (game controls)
    //     media controller   - lower-priority button input (mute,
    //                          volume controls)
    //     feedback controls  - low-priority status reports
    //
    // HID interface 1
    //     joystick/gamepad   - high-priority button, accelerometer,
    //                          and plunger input; generates input
    //                          continuously from accelerometer and
    //                          plunger analog inputs, so this should be
    //                          on its own interface with no other devices
    //
    // Note that the keyboard and media controller can be combined on
    // one interface, even though they're both button/key inputs, for
    // two reasons.  One is that the mappings of the media control keys
    // are usually lower-priority by their nature, assuming that the
    // user is mapping them based on their nominal semantics.  These
    // are functions like mute and volume up/down that aren't usually
    // used as game controls.  More importantly, though, button-press
    // events are manual user inputs, so they're inherently low-
    // frequency and low-volume.  It should be rare that input events
    // occur on the keyboard and media controller simultaneously, so
    // so they should rarely be in contention for the reporting slot.
    // 
    // 
    class HIDIfc
    {
    public:
        HIDIfc(int instance, uint8_t epOut, uint8_t epIn) :
            instance(instance), epOut(epOut), epIn(epIn) { }

        // Tinyusb HID instance number.  Note that this is our index
        // in Tinyusb's list of HID interface, which ONLY includes the
        // other HID interfaces.  The index isn't the same as our USB
        // inteface index, since the overall USB list also includes
        // the other interface types in our composite device (CDC and
        // vendor).
        int instance;

        // Endpoint numbers
        uint8_t epOut;
        uint8_t epIn;

        // Is this an IN/OUT interface?  true -> at least one of our
        // devices has both input and output reports; false -> all of
        // our devices aer input-only (i.e., only generate input reports,
        // device-to-host)
        bool isInOut = false;
        
        // Add a HID device to the interface.  Call this during
        // initialization to set up the configured sub-device list.  The
        // caller retains ownership of the referenced object; the HIDIfc
        // retains the reference, so the underlying HID device object must
        // remain valid as long as the HIDIfc is still valid.  (The device
        // objects generally have session lifetime, so a suitable and
        // simple way to instantiate them is as statics or globals.)
        void AddDevice(HID *dev);

        // number of devices in the list
        int GetNDevices() const { return static_cast<int>(devices.size()); }

        // Get the HID report descriptor for this interface.  This combines
        // the report descriptors from the sub-devices into a concatenated
        // buffer for return to the host with the configuration descriptor.
        // Each HID interface has its own combined report descriptor.
        //
        // We automatically build the combined descriptor on the first call,
        // and cache the result for subsequent calls.  All sub-devices must
        // be added before the first call to ensure that they're included
        // in the cached descriptor.
        const uint8_t *GetCombinedReportDescriptor();

        // Perform period tasks.  This checks for pending reports and
        // sends them when the device is available.
        void Task();

        // Report completed notification (callback from tinyusb)
        void OnSendReportComplete(const uint8_t *report, size_t len);
        
        // HID polling interval in microseconds.  This is set to the shortest
        // desired polling interval for all devices attached to the interface.
        int pollingInterval = 8000;

        // Polling refractory interval, in microseconds.  This is the time to
        // wait until after a send completes before we set up the next send.
        // During initialization, we set this to the polling interval minus a
        // few milliseconds.
        //
        // The refractory interval is designed to minimize the latency between
        // an event actually occurring physically, and the event being reported
        // to the host.  A HID device can only send an event to the host when
        // the host polls for it, which happens at fixed intervals specified by
        // pollingInterval.  "Sending" a report actually means that we load the
        // report into a buffer on the Pico's USB hardware controller, so that
        // the report is ready to transmit the next time the host polls for an
        // input report on our IN endpoint.  So the transmission doesn't actually
        // happen when we call "send"; our "send" only stages the data for later
        // transmission on the host's schedule.  If we stage the send early in
        // one of these polling cycles, and a physical quantity in the report
        // happens to change in the time between the "send" and the host polling
        // request, the host won't see the update until the NEXT polling cycle.
        // So it's best to do each "send" as close as possible to next host
        // polling request.  Since we know the host polls at fixed intervals,
        // we can estimate the time of the next host request, as the time of
        // the LAST host request plus the polling interval.  However, we also
        // need to leave time to complete our "send" setup before the next
        // host event actually occurs, so we shouldn't wait until the last
        // possible microsecond; instead, we leave a little padding (a few
        // milliseconds) before the expected next polling time.
        static const int RefractoryIntervalPadding = 2500;
        int pollingRefractoryInterval{ pollingInterval - RefractoryIntervalPadding };

        // device list
        std::vector<HID*> devices;

        // buffer for concatenated HID report descriptors
        std::vector<uint8_t> combinedReportDescriptor;

        // index in the devices vector of the device that initiated the
        // current send report
        int deviceSending = -1;

        // index in the devices vector of the last device that sent a report
        int lastSender = -1;

        // system timestamp (us) of start of send for last report
        uint64_t tSendStart = 0;

        // system timestamp (us) of completion of last report sent
        uint64_t tSendComplete = 0;
    };

    // Add a HID interface.  Returns the new interface object.
    HIDIfc *AddHIDInterface();

    // Initialize - call once at startup, after calling AddDevice() as many
    // times as necessary to add the configured devices to the active list.
    void Init();

    // run periodic USB tasks
    void Task();

    // Get the USB device descriptor
    const uint8_t *GetDeviceDescriptor();
    
    // Get the USB configuration descriptor
    const uint8_t *GetConfigurationDescriptor(int index);

    // Get the MS OS 2.0 WinUSB descriptor
    const uint8_t *GetMSOSDescriptor(size_t *byteSize = nullptr);

    // Get the USB BOS descriptor
    const uint8_t *GetBOSDescriptor();

    // get the combined HID report descriptor for one of our HID interfaces
    const uint8_t *GetHIDReportDescriptor(int instance);

    // Get a string descriptor
    const uint16_t *GetStringDescriptor(uint8_t index, uint16_t langId);

    // Handle a vendor-interface control transfer
    bool VendorControlXfer(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request);

    // Get Report event handler
    uint16_t OnGetHIDReport(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t reqLen);

    // Set Report event handler
    void OnSetHIDReport(uint8_t instance, uint8_t id, hid_report_type_t type, const uint8_t *buf, uint16_t bufSize);

    // Send Report Completed event handler
    void OnSendHIDReportComplete(uint8_t instance, const uint8_t *report, uint16_t len);

    // Set Wake Pending.  This sets an internal flag that tells the task
    // handler to send a wakeup packet to the host at the next polling
    // cycle, to attempt to wake the host from a low-power sleep mode.
    // This has no effect if the host isn't already suspended, or the
    // host told us to disable remote wakeup on the last suspend.  The
    // host might also ignore the wakeup packet, depending on the
    // Windows and PC BIOS configuration.
    void SetWakePending() { wakePending = true; }

protected:
    // Device USB version.  Incremeent this whenever one of the HID
    // report descriptors changes.
    //
    // The version number is included in the serial number string that
    // we provide to the host via the HID string descriptors.  The host
    // uses the serial number string as a key for caching the device's
    // HID descriptors, so changing the serial number - which we can
    // do by changing the version number, since the version is embedded
    // in the serial - should force the host to re-query the descriptors.
    //
    // This number does NOT need to change with the overall firmware
    // version number.  Most types of changes in the rest of the
    // firmware program shouldn't require a USB version change.  The USB
    // version only needs to change when we alter the USB descriptors in
    // such a way that host machines must be forced to discard cached
    // copies of the descriptors and re-query them from the device.
    const uint8_t DEVICE_USB_VERSION = 0x01;

    // Add a string descriptor from ASCII text or from pre-formatted
    // binary data.  We call these internally during initialization
    // to populate the string descriptor table.  Each call adds
    // a string at the next available index.  For the text version,
    // we convert from ASCII to the Unicode wire format; for the byte
    // version, we copy the bytes exactly as submitted, so the buffer
    // must already be in the correct format to send on the wire.
    void AddStringDescriptorText(uint8_t index, const char *str);
    void AddStringDescriptorBytes(uint8_t index, const uint8_t *data, size_t nBytes);
    
    // device VID and PID
    uint16_t vid = 0;
    uint16_t pid = 0;

    // is the device mounted?
    bool mounted = false;

    // has the device EVER been mounted (since the last reset)?
    bool everMounted = false;

    // is the bus suspended?
    bool suspended = false;

    // USB string descriptor indices
    static const int STRID_LANGID  = 0;
    static const int STRID_MANUF = 1;
    static const int STRID_PROD = 2;
    static const int STRID_SERIAL = 3;

    // Remote wakeup pending.  We set this when the host PC is in "suspend"
    // mode (a low-power sleep state) and an event occurs on the client side
    // that's designated to wake the host.  Input devices like keykboards and
    // mice typically wake the host on an explicit user input action, such as
    // a key press or jiggling the mouse.
    bool wakePending = false;

    // Is remote wakeup enabled?
    bool remoteWakeEnabled = true;

    // Configuration descriptor.  This is built on the first call to
    // GetConfigurationDescriptor() and then cached.  All HID interfaces must
    // be added and populated with their logical devices before the first call
    // to GetConfigurationDescriptor(), to ensure that everything is included
    // in the cached descriptor.
    std::unique_ptr<uint8_t> cfgDesc;

    // HID interface list
    std::vector<std::unique_ptr<HIDIfc>> hidIfcs;

    // Next available HID input/output endpoint numbers
    uint8_t nextHidEpOut = EndpointOutHID0;
    uint8_t nextHidEpIn = EndpointInHID0;

    // String descriptors, keyed by string index.  The descriptors are stored
    // in the USB wire format: a two-byte descriptor prefix, followed by the text
    // of the string as little-Endian UCS-2 (16-bit Unicode) characters.  The
    // prefix looks like this:
    //
    //   byte [0] = length of the entire object, in bytes, including the two-byte prefix
    //   byte [1] = 0x03 (TUSB_DESC_STRING, HID desriptor type code for String)
    //
    std::unordered_map<uint8_t, std::vector<uint8_t>> stringDescriptors;

    // device descriptor
    tusb_desc_device_t deviceDescriptor;
};

// global singleton instance for the main USB interface
extern USBIfc usbIfc;

// global singletons for the USB HID devices
extern USBIfc::Keyboard keyboard;
extern USBIfc::MediaControl mediaControl;
extern USBIfc::Gamepad gamepad;
extern USBIfc::OpenPinballDevice openPinballDevice;
extern USBIfc::FeedbackController feedbackController;

// Cover macro for driver name in usbd_class_driver_t definitions.
// This is only present for DEBUG >= 2 for versions before 0.17, and
// always present for 0.17 and later.
#if (CFG_TUSB_DEBUG >= 2) || (TUSB_VERSION_NUMBER >= 1700)
#define TUSB_DRIVER_NAME(_name)  _name,
#else
#define TUSB_DRIVER_NAME(_name)
#endif

// Changes in 0.17.0
#if TUSB_VERSION_NUMBER >= 1700
#define IF_TUSB_001700(...) __VA_ARGS__
#else
#define IF_TUSB_001700(...)
#endif
