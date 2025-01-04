// Pinscape Pico - Plunger Interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Defines the abstract interface to the plunger sensor.
//
// The Pinscape plunger system is designed to work with a wide variety
// of sensor types.  We try to make as much of the code as possible
// independent of the sensor type, with just a small device class for
// each sensor that implements an abstract interface that the generic
// code calls into.
//
// The job of each sensor is to provide the generic layer with a reading
// on the plunger's current position along its longitudinal axis.  Each
// sensor has its own way of measuring this, in its own native unit
// system, but ultimately it's always an integer value representing the
// distance of the plunger tip from some arbitrary zero point, which
// must map linearly to spatial distance on the plunger axis.  Every
// sensor can use whatever units it wants as long as the units map
// linearly to physical distance.  The sensor readings are passed as
// unsigned integers, so every reading has to be non-negative.
//
// By convention, higher numbers on the sensor native scale represent
// further retraction on the plunger.  But it's also okay for the sensor
// to get this backwards, because the generic software layer can be
// configured to reverse its interpretation of the direction.  This is
// important because some sensors get their sense of "front" and "back"
// from the installation orientation, and it's easy with some of those
// sensors to get the orientation backwards when installing the device.
// The software reversal option makes life easier for users - if you get
// the orientation backwards on the sensor, there's no need to open up
// the machine and redo the installation, since you can just flip the
// direction in the software setup.
//
// The generic code converts from the sensor's unit system to "Z Axis"
// logical units, which are the units we use for HID joystick reports.
// We call these "logical" units because they're scaled to fit the
// 16-bit integer type we use in the USB reports to the host rather than
// to any real-world physical unit.  And we use the term "Z Axis" out of
// long-standing convention; Visual Pinball and most of the other PC
// simulators that recognize analog plunger input accept that input via
// a HID joystick interface on the joystick's nominal Z axis.  Note that
// the Plunger class doesn't have any code relating to joysticks or USB
// in general - that's all handled in the USB classes.  The Plunger
// class's only job is to compute a value that's suitable for use in the
// USB classes for "Z Axis" type plunger position reports.  Also, note
// that our term "Z Axis" isn't literal.  We use it only for the sake of
// convention.  The actual mapping to USB input is configurable in the
// USB device classes, and it doesn't necessarily have to use a "Z" axis
// or even a joystick HID interface.
//
// Z Axis units are device-independent.  We always use a signed 16-bit
// scale for this, conforming to the convention that most of the
// simulators use, in which Z=0 represents the resting position of the
// plunger, and +32767 represents the point of maximum retraction.  The
// maping between this fixed Z Axis unit system and the sensor's native
// unit system is determined through a calibration process, where we
// monitor the raw readings while the user moves the plunger to its
// extremes so that we can find the maximum, minimum, and zero points
// that the sensor reports.


#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <unordered_map>
#include <string>
#include <memory>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>

// local project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "../USBProtocol/FeedbackControllerProtocol.h"

// forward/external declarations
class Plunger;
class ConsoleCommandContext;

// global singleton
extern Plunger plunger;

// Plunger Interface
class Plunger
{
protected:
    // shorthand for interface types
    using FiringState = PinscapePico::PlungerFiringState;

public:
    Plunger();

    // Configure from JSON data
    void Configure(JSONParser &json);

    // Perform periodic tasks
    void Task();

    // Get the type, for feedback controller reporting purposes.  This
    // returns a PinscapePico::FeedbackControllerReport::PLUNGER_xxx
    // constant corresponding to the configured sensor type.
    uint16_t GetSensorTypeForFeedbackReport() const { return sensor->GetTypeForFeedbackReport(); }

    // Enable/is enabled?
    void Enable(bool enable) { isEnabled = enable; }
    bool IsEnabled() const { return isEnabled; }

    // Is the plunger calibrated?
    bool IsCalibrated() const { return cal.calibrated; }

    // Start/stop calibration mode.  If autoSave is true when initiating
    // or ending the calibration, the changes will be applied when the
    // mode ends, whether it ends explicitly or due to the timed period
    // ending.
    void SetCalMode(bool start, bool autoSave);

    // Are we in calibration mode?
    bool IsCalMode() const { return calMode; }

    // push/release the calibration button
    void PushCalButton(bool on);
    bool IsCalButtonPushed() const { return calButtonState; }

    // Get the current Z Axis reading, for USB HID reporting on VP's
    // traditional joystick-based plunger interface.  This returns a
    // fully processed reading, with all filtering and calibration steps
    // applied, and normalized to our standard USB INT16 logical axis
    // range.  This value includes special corrections during firing
    // events that help the simulator infer a consistent impulse based
    // on the successive position readings.  This is meant to be used
    // directly as a gamepad/joystick Z axis value for passing to VP and
    // other simulators that can take plunger input via a joystick axis.
    int16_t GetZ() const { return zCur.z; }

    // Get the current uncorrected Z axis reading.  This is the Z axis
    // reading with calibration and jitter filtering applied, but
    // without any of the special firing-event processing applied.  This
    // is suitable for use with simulators that use the speed input
    // rather than using the position alone to calculate the impulse
    // during a release event.
    int16_t GetZ0() const { return z0Reported.z; }

    // Get the speed reading.  Returns the instantaneous speed as of
    // the latest position reading.
    int16_t GetSpeed() const { return speedCur; }

    // Get the latest raw/normalized sensor reading
    uint16_t GetRawSensorReading() const { return lastTaskRaw.rawPos; }

    // Is the plunger currently in a firing event?  This returns true if we've
    // detected a firing event and we're currently in the "settling" phase of
    // the event.  We've positively identified a firing event when we reach
    // this phase.  Earlier phases are tentative, since the user could still
    // interrupt the motion before the plunger crosses the finish line and
    // reaches the point where it would definitely strike the ball if this
    // were a physical machine.
    bool IsFiring() const { return firingState == FiringState::Settling; }

    // Set the jitter filter window size.  This is specified in native
    // sensor units.  This sets the value in memory only; it doesn't save
    // it persistently.
    void SetJitterWindow(uint16_t w);

    // Set the firing time limit, in microseconds.  This sets the value
    // in memory only; it doesn't save it persistently.
    void SetFiringTimeLimit(uint32_t time_us) { firingTimeLimit = time_us; }

    // Set the integration time.  This sets the value in memory only;
    // it doesn't save it persistently.
    void SetIntegrationTime(uint32_t time_us);

    // Set the manual calibration scaling factor.  This is percentage
    // value that's applied to the logical position reading after
    // converting it from the raw sensor reading.
    void SetManualScalingFactor(uint32_t percentage) { manualScalingFactor = percentage; }

    // Set calibration data from a vendor request
    bool SetCalibrationData(const PinscapePico::PlungerCal *data, size_t len);

    // Commit the current run-time-adjustable settings (jitter window, firing
    // time limit, integration time).  Returns true on success, false on
    // failure.  The commit requires writing to flash, which can potentially
    // fail.
    bool CommitSettings();

    // Initialize the in-memory settings file struct from the live settings
    void InitSettingsFileStructFromLive();

    // Restore saved settings.  Restores the jitter window and firing
    // time limit from flash.  Returns true on success, false on
    // failure.  If no settings file exists for the sensor, this applies
    // defaults and returns success (true).  If 'fileExists' is not
    // null, we fill that in with true if the file exists, false if we
    // used defaults. 
    bool RestoreSettings(bool *fileExists = nullptr);

    // Set/Get reverse orientation
    void SetReverseOrientation(bool f) { reverseOrientation = f; }
    bool IsReverseOrientation() const { return reverseOrientation; }

    // Set/get the scan mode.  Some imaging sensors have multiple
    // algorithms for analyzing the image, to optimize for different
    // installation conditions.  The mode codes are all specific to
    // the individual sensors.
    void SetScanMode(uint8_t mode);
    uint8_t GetScanMode() const;

    // Get the average release time calculated during the latest calibration,
    // in microseconds.  Returns zero if we haven't run a calibration during
    // this session, or if no release motions were detected.
    uint32_t GetAverageReleaseTime() const { return releaseTimeCount != 0 ? (releaseTimeSum / releaseTimeCount) : 0; }

    // Raw sample type
    struct RawSample
    {
        uint64_t t;         // sample timestamp
        uint32_t rawPos;    // raw sample from the sensor, using the sensor's native scaling
    };

    // Calibration data
    struct CalibrationData
    {
        // Is the calibration data valid?
        bool calibrated = false;

        // Range of raw sensor readings observed during calibration over the
        // plunger's physical travel range.  These are in raw sensor units.
        uint32_t min = 0;
        uint32_t zero = 0;
        uint32_t max = 0;

        // measured firing time, in microseconds
        uint32_t firingTimeMeasured = 0;

        // Extra space for sensor-specific data.  The Plunger::Sensor
        // subclasses can use these fields as desired in OnRestoreCalibration(),
        // BeginCalibration(), and EndCalibration() to store extra data that's
        // specific to the sensor.  The generic class treats these as opaque;
        // it just stores them in the calibration file.
        //
        // The sensor is also free to create its own private calibration file,
        // if it it needs more space than we provide here.
        uint32_t raw[8] { 0 };
    };

    // Plunger device.  This is the abstract base class for all of the
    // physical sensor types.
    class Sensor
    {
    public:
        // construction
        Sensor() { }

        // Configure.  Returns true on success.  On error, logs relevant
        // error messages and returns false.
        virtual bool Configure(const JSONParser::Value *val) { return true; }

        // Friendly name, for log messages
        virtual const char *FriendlyName() const = 0;

        // Sensor priority when picking a default.  Many of the plunger
        // sensors can really only be used as plungers (as far as the
        // Pinscape software is concerned, anyway), while others have
        // other applications.  Potentiometers in particular can be used
        // for other purposes, such as for HID joystick axis inputs.
        // When we're selecting a sensor to use as the default plunger
        // sensor, we'd prefer to pick a sensor that's only usable as a
        // plunger over one that has multiple uses.  This allows the
        // user to configure one or more ADCs for other uses without the
        // need to tell us to ignore them as plungers, as long as some
        // other plunger-only sensor is also configured.  Note that this
        // only affects the *default* selection; the user can always
        // tell us explicitly which sensor to use if the default-picker
        // algorithm doesn't yield the desired result.  We just want to
        // minimize the need for manual configuration as much as
        // possible.
        //
        // High numbers are higher priority, so devices like TCD1103
        // should use high values and potentiometers should use low
        // values.  We return a high priority by default.
        virtual int PriorityForPickingDefault() const { return 32767; }

        // Is the sensor ready to take a reading?  Some sensors have a
        // lengthy cycle time; for example, the optical sensors take a
        // few milliseconds to transfer image snapshots.  Any such long-
        // running operations should be performed asynchronously, so
        // that the main loop isn't blocked waiting for a sensor
        // reading.  This lets the main loop poll the sensor for
        // completion of such asynchronous operations.
        virtual bool IsReady() { return true; }

        // Get a raw plunger reading.  This gets reading using the native
        // device units.  The sample value must be linear with respect to
        // the plunger position.  The timestamp should reflect when the
        // reading was collected from the physical device in real time.
        //
        // Note that the reading is "raw" in the sense that it's the value
        // before adjusting for calibration.  It might or might not be the
        // reading taken directly from the sensor.  For sensors that work
        // with non-scalar data, such as imaging sensors, it's obvious
        // that this represents a processed result, since you can't return
        // an image here.  But sensors that work with scalar data might
        // also return a processed result here.  For example, VCNL4010's
        // raw reading indicates the brightness of a reflected IR signal,
        // which is inversely related to the distance from sensor to
        // plunger.  Since the raw reading must be linear with respect
        // to plunger position, the VCNL4010 has to take the inverse of
        // the brightness reading (among other arithmetic) to get the raw
        // positional reading.
        virtual bool ReadRaw(RawSample &s) = 0;

        // Should jitter filtering be applied to the raw sample?  Most
        // sensors return true, to let the generic plunger layer apply
        // the jitter filter.  For some sensors, though, it makes more
        // sense to apply the jitter filter at an earlier processing
        // step, in which case this can be overridden to return false.
        virtual bool UseJitterFilter() const { return true; }

        // Auto-zero the plunger.  Relative sensor types, such as
        // quadrature sensors, can lose sync with the absolute position
        // over time if they ever miss any motion.  We can automatically
        // correct for this by resetting to the park position after
        // periods of inactivity.  It's usually safe to assume that the
        // plunger is at the park position if it hasn't moved in a long
        // time, since the spring always returns it to that position
        // when it isn't being manipulated.  The main loop monitors for
        // motion, and calls this after a long enough time goes by
        // without seeing any movement.  Sensor types that are
        // inherently absolute (TSL1410, potentiometers) shouldn't do
        // anything here.
        //
        // Returns true if the auto-zero was put into effect, false
        // if not.  Absolute sensors should always return zero here,
        // since auto-zeroing doesn't apply to sensors with absolute
        // reference points.
        virtual bool AutoZero(const CalibrationData &cal) { return false; }

        // Get this sensor's calibration file name.  Each sensor uses
        // its own filename so that we don't try to reuse data previous
        // saved for an old sensor type if the user reconfigures with a
        // new sensor type.
        virtual const char *GetCalFileName() const = 0;

        // Restore the saved calibration data from the configuration.  We
        // call this at startup to let us initialize internals from the
        // saved calibration data.  This is called even if the plunger isn't
        // calibrated, which is flagged in the config.
        virtual void OnRestoreCalibration(const CalibrationData &) { }

        // Begin calibration.  The main loop calls this when the user
        // activates calibration mode.  Sensors that work in terms of
        // relative positions, such as quadrature-based sensors, can use
        // this to set the reference point for the park position internally.
        virtual void BeginCalibration(CalibrationData &) { }

        // End calibration.  The main loop calls this when calibration
        // mode is completed.
        virtual void EndCalibration(CalibrationData &) { }

        // Set integration time, in microseconds.  This is only meaningful for
        // image-type sensors.  This allows the PC client to manually adjust
        // the exposure time for testing and debugging purposes.
        virtual void SetIntegrationTime(uint32_t us) { }

        // Get the average sensor scan time in microseconds
        virtual uint32_t GetAvgScanTime() { return 0; }

        // Get/set the scan mode
        virtual void SetScanMode(uint8_t scanMode) { }

        // Get the sensor type ID for use in a Feedback Controller report.
        // Returns the PinscapePico::FeedbackControllerReport::PLUNGER_xxx
        // constnat corresponding to this sensor type.
        virtual uint16_t GetTypeForFeedbackReport() const = 0;

        // Populate a Vendor Interface plunger snapshot report with sensor-
        // specific data.  This fills in the buffer with the device-level
        // details of the sensor's current state, for use in debugging and
        // plunger setup tools on the PC side.  An imaging sensor typically
        // returns a full snapshot of the current frame buffer, for example;
        // a quadrature encoder would return its current A/B channel states.
        // 
        // Returns the number of bytes written to the buffer.
        virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) = 0;

        // Native scale of the device.  This is the scale used for the
        // position reading in status reports.  This lets us report the
        // position in the same units the sensor itself uses, to avoid any
        // rounding error converting to an abstract scale.
        //
        // The nativeScale value is the number of units in the range of raw
        // sensor readings returned from ReadRaw().  Raw readings thus have
        // a valid range of 0 to nativeScale-1.
        //
        // Image edge detection sensors typically use the pixel span of the
        // image as the scale, since the position can usually be fixed at
        // pixel resolution.  Quadrature sensors and other sensors that
        // report the distance in terms of true physical distance units
        // should normalize their reported scale according to the number of
        // quanta in the approximate total plunger travel distance of 3",
        // usually adding a little bit for headroom at either end of the
        // scale. For example, the VL6180X uses millimeter quanta, so can
        // report about 77 quanta over 3" and thus might report on a 0..100
        // scale; similarly, a quadrature sensor that reports at 1/300"
        // intervals has about 900 quanta over 3", so it might set the
        // native scale to 1000.  Absolute encoders (e.g., bar code sensors)
        // have a natural native range of whatever range the encoding scheme
        // is capable of representing.
        //
        // Sensors that are inherently analog (e.g., potentiometers, analog
        // distance sensors) can quantize on any arbitrary scale.  In most
        // cases, it's best to use whatever scale the underlying hardware
        // device uses for its reporting, since there won't be any loss of
        // precision from rescaling through the initial processing in raw
        // units.
        //
        // IMPORTANT: this is assumed to be constant once the sensor is
        // configured.  The Plunger class stores the value read during
        // initialization and doesn't check back here later.  The scale is
        // static for all currently supported sensors, but if the assumption
        // proves untrue for some future sensor, we'll have to remove the
        // Plunger::nativeScale member variable and call this instead
        // wherever the cached value was used.  I don't see any reason to
        // think this will ever be necessary, since the scale is an inherent
        // part of the hardware design for every sensor I can imagine using
        // in a plunger context.
        virtual uint32_t GetNativeScale() const = 0;
    };

    // Null plunger sensor.  This is used as a placeholder when no physical
    // sensor is configured.  The null plunger always reports "not ready",
    // which makes the task loop ignore it.  Raw samples always read as 15% of
    // full scale, because that approximates the resting position for a
    // standard mechanical plunger with the sensor readings idealized to
    // exactly cover its full travel range.
    class NullSensor : public Sensor
    {
    public:
        NullSensor() : Sensor() { }
        virtual const char *FriendlyName() const override { return "Null Sensor"; }
        virtual uint16_t GetTypeForFeedbackReport() const override { return PinscapePico::FeedbackControllerReport::PLUNGER_NONE; }
        virtual size_t ReportSensorData(uint8_t *buf, size_t maxSize) override { return 0; }
        virtual bool IsReady() override { return false; }
        virtual bool ReadRaw(RawSample &r) override { r = { time_us_64(), 9830 }; return true; }
        virtual const char *GetCalFileName() const override { return "NullPlunger.cal"; }
        virtual uint32_t GetNativeScale() const override { return 65535; }
    };

    // get the sensor
    Sensor *GetSensor() const { return sensor; }

    // Populate the vendor interface query structs, along with any
    // special per-sensor data, such as a sensor image snapshot for an
    // imaging sensor.  maxSize is the maximum size of the buffer.
    // Returns the size actually populated, or zero if there wasn't
    // enough space in the buffer for the returned data.
    size_t Populate(PinscapePico::PlungerReading *pd, size_t maxSize);
    size_t Populate(PinscapePico::PlungerConfig *pd, size_t maxSize);

    // Apply the jitter filter.  The position is in unscaled native 
    // sensor units.
    uint32_t ApplyJitterFilter(uint32_t pos);

protected:
    // Calibration button pushed state.  This is the state of the logical
    // button assigned to start the calibration process.
    bool calButtonState = false;

    // Calibration button push start time
    uint64_t tCalButtonPush = 0;

    // Live calibration data
    CalibrationData cal;

    // Initialize - called internally on successful configuration
    void Init();

    // Is the plunger enabled?
    bool isEnabled = false;

    // Is auto-zeroing enabled?
    bool autoZeroEnabled = true;

    // Auto-zero interval - the plunger must remain stationary for this
    // long to activate auto-zering
    uint32_t autoZeroInterval = 0;

    // Next scheduled auto-zero time.  Set this initially to infinity,
    // to indicate that no auto-zeroing is scheduled yet.
    uint64_t tAutoZero = ~0ULL;

    // Read a raw sample from the sensor, applying the jitter filter and
    // orientation correction.  Returns true on success, false if it wasn't
    // possible to take a reading.  On success, fills in 's' with the
    // current sample, and returns true.  Returns false if a reading
    // couldn't be taken.
    bool ReadSensor(RawSample &s);

    // active sensor - initialize to the null sensor by default
    NullSensor nullSensor;
    Sensor *sensor = &nullSensor;

    // Configured sensors.  We set up a sensor object for each
    // plunger-capable hardware sensor that's configured in the system,
    // whether or not that device is designated as the main plunger.
    std::unordered_map<std::string, std::unique_ptr<Sensor>> sensors;

    // Reverse the raw reading orientation.  If set, raw readings will be
    // switched to the opposite orientation.  This allows flipping the sensor
    // orientation virtually to correct for installing the physical device
    // backwards.
    bool reverseOrientation = false;

    // integration time in microseconds; applicable only to imaging sensors
    // that implement adjustable exposure time
    uint32_t integrationTime = 0;

    // scan mode
    uint8_t scanMode = 0;

    // Jitter filtering
    struct JitterFilter
    {
        uint32_t window = 0;       // window size, in native sensor units
        uint32_t lo = 0;           // low end of current window
        uint32_t hi = 0;           // high end of current window
        
        uint32_t lastPre = 0;      // last pre-filtered reading
        uint32_t lastPost = 0;     // last filtered reading
    };
    JitterFilter jitterFilter;

    // Manual scaling factor.  This is a scale adjustment that the user
    // can manually apply to improve the automatic calibration if desired.
    // This is expressed as a percentage, and is applied to the logical
    // position reading after converting from raw units.
    uint32_t manualScalingFactor = 100;

    // Persistent settings file data.  This is stored under the
    // per-sensor calibration file name provided by the sensor type
    // subclass.  We store separate settings for each sensor type
    // because all of the settings are inherently specific to the
    // sensor.  Making the file sensor-specific ensures that we start
    // with a clean slate (without any special user action to reset
    // settings) if the user changes sensor type.  It also restores the
    // settings for a past sensor type if the user happens to switch
    // between two or more types, although that doesn't seem like a
    // likely use case.
    struct SettingsFile
    {
        // jitter filter window size
        uint32_t jfWindow = 0;

        // firing time limit (microseconds)
        uint32_t firingTimeLimit = 0;

        // integration time (microseconds), for imaging sensors
        uint32_t integrationTime = 0;

        // manual scaling factor
        uint32_t manualScalingFactor = 0;

        // reverse orientation (interpreted as a boolean)
        uint8_t reverseOrientation = 0;

        // Scan mode.  Some imaging sensors have options for different
        // image analysis algorithms, which might have tradeoffs that make
        // the optimal selection vary by installation.
        uint8_t scanMode = 0;

        // calibration data
        CalibrationData cal;
    };
    SettingsFile settingsFile;

    // last raw sensor reading taken in the Task() routine
    RawSample lastTaskRaw{ 0, 0 };

    // Z-axis reading with timestamp
    struct ZWithTime
    {
        uint64_t t = 0;
        int16_t z = 0;
    };

    // Current "Z Axis" reporting value.  By convention, VP and other pinball
    // simulators take analog plunger input from the Z Axis on a HID joystick/
    // gamepad interface.  This is our latest calculated value for that
    // reporting system, using a normalized INT16 (-32768..+32767) scale.
    // This value is updated in the Task() routine based on raw sensor
    // readings and our internal time-based processing to present VP with an
    // idealized model plunger signal that VP can reconstruct accurately from
    // the USB HID input.
    ZWithTime zCur;

    // Three-point history of logical Z axis readings, without launch-event
    // processing.  These represent the previous reading reported, current
    // reading being reported, and next reading pending.  We keep one
    // reading of look-ahead so that we can calculate the instantaneous
    // speed of the current reported event from the two adjacent points.
    ZWithTime z0Prv;
    ZWithTime z0Cur;
    ZWithTime z0Nxt;

    // Report Z0.  This is z0Cur, except when a Z0 Hold is in effect.
    ZWithTime z0Reported;

    // Z0 hold.  When a firing event occurs, we hold Z0 at the peak
    // forward point for a few HID reporting cycles, to ensure that the
    // host actually receives some reports at the forward position.  The
    // plunger bounces back at the end of a firing event so quickly that
    // the HID report cycle would otherwise miss the forward position
    // entirely, by sampling at positions before and after the peak is
    // reached, while the plunger is still behind the resting point.
    // That manifests in the simulation as a delayed launch, because the
    // simulator can't tell that the plunger was in contact with the
    // ball in between the pre- and post-bounce readings.  It's the
    // classic aliasing problem with that faster-than-HID bounce.  So
    // after a firing event, hold the peak briefly - just long enough to
    // ensure it makes it into a few HID reports, about 40ms.  That's
    // not long enough to cause any visible effect on the animation, but
    // it's very effective and reliable at triggering a timely launch in
    // the simulator.
    ZWithTime z0Hold;

    // Speed hold.  During the Z0 hold, we also hold the speed, so that
    // the simulator calculates the launch impulse based on the speed at
    // the time we reached the hold position.
    int16_t speedHold = 0;

    // Start of current forward run.  This is the starting point for the
    // current run of consecutive forward motion readings.
    ZWithTime zForwardStart;

    // Current reported speed.  This is either the current instantaneous
    // speed, or the peak recent forward speed (see below).
    //
    // The speed is measured in logical Z axis units per centiseconds
    // (1cs = 10ms).  Centiseconds are not a commonly used unit of time,
    // but it's a good scale for our purposes because speed values
    // expressed in this system fit well into the 16-bit field we use in
    // the HID reports.  Empirically, peak mechanical plunger speeds are
    // around 5mm/ms, which translates to 2153 logical Z axis units/ms,
    // assuming a standard post-1980s plunger mechanism and calibration
    // such that 32767 logical units equals the 70mm travel distance
    // from the resting position to fully retracted.  LZU/ms would thus
    // only use about a 12-bit range, leaving 4 bits of precision
    // unused.  Changing the time base to 10ms yields a peak of 21530
    // LZU/10ms, which uses most of the 16-bit space without running
    // much risk of overflow for mecahnisms that are faster than the
    // ones I've measured.
    int16_t speedCur = 0;

    // prior speed, to repeat during Z0 holds
    int16_t speedPrv = 0;

    // update the calibration data
    void OnUpdateCal();

    // Apply calibration to a raw sample to yield a logical sample
    int16_t ApplyCalibration(uint32_t rawPos);
    int16_t ApplyCalibration(const RawSample &s);

    // Are we in calibration mode?
    bool calMode = false;

    // calibration mode start time
    uint64_t tCalModeStart = 0;

    // Auto-save the calibration on completion.  When calibration is
    // initiated by a button press, we automatically save it (along with all
    // of the other ephemeral settings) when the mode ends after the timed
    // calibration period.  When it's initiated by the USB protocol, we
    // don't auto-save, but rather leave it up to the host to decide if and
    // when to save.  This lets the PC-side software bundle a set of changes
    // into one file save, and lets the user temporarily try new settings
    // settings before deciding whether to commit them or r oll them back.
    bool calModeAutoSave = false;

    // Calibration zero point statistics.  During calibration mode, we track
    // the plunger through several pull-and-release cycles, so that we can get
    // an average of the rest position readings over several trials.  The
    // average is useful because the plunger doesn't always stop at exactly
    // the same spot in physical space.  This is mostly due to friction, and
    // maybe a little variability in the spring tensions.  If the springs were
    // perfect, there'd a single point along the plunger's travel range where
    // the tensions on the main spring and barrel spring are perfectly
    // balanced, so if there were no friction, the plunger would always come
    // to rest at exactly this spot, every single time.  (Although thank
    // goodness for friction, because the plunger would oscillate for a very
    // long time without it, rendering the notion of a "rest position" almost
    // moot.)  But with friction in the mix, the plunger won't always stop on
    // this exact mark.  If the plunger happens to slow down enough within a
    // short distance of the true balance point, friction can make up the
    // difference between the spring tensions and let the plunger stay at rest
    // at that slight offset.  This range, where friction overcomes the the
    // imbalance between the spring tensions, is wide enough that we can
    // measure on most of our sensors, so we'll get a range of readings for
    // the rest position if we repeat the experiment.  There's also noise in
    // the sensor that would add a little variability in the raw readings even
    // if the physical plunger rest position were always precisely the same,
    // but that's less than the real mechanical variation, at least with the
    // better sensors.
    RawSample calZeroStart{ 0, 0 };
    uint64_t calZeroSum = 0;
    uint32_t calZeroCount = 0;

    // Release time accumulator, for figuring the average release time
    // during calibration.
    uint64_t releaseTimeSum = 0;
    uint32_t releaseTimeCount = 0;

    // Sensor's raw scale.  We read this from the sensor and cache it after
    // sensor configuration is complete.  We assume that the scale doesn't
    // change during the session.
    uint32_t nativeScale = 0;

    // Logical axis scaling factor.  This is the value F, expressed as a
    // 16.16 fixed-point fraction, such that:
    //
    //   <LogicalAxisValue> = F * (<RawReading> - cal.zero)
    // 
    // On the logical axis, 0 represents the calibrated resting position,
    // positive values represent retracted positions (plunger pulled back),
    // and negative values are forward of the resting point (plunger pushed
    // forward against the barrel spring).  When calibrated, the plunger's
    // full retraction travel range should exactly fit the full arithmetic
    // range of the logical axis value, 0..+32767, with +32767 mapping to the
    // fully retracted position.  Therefore, the concrete value of F can be
    // obtained from the calibration data as:
    //
    //   32767 = F * (cal.max - cal.zero)
    //   -> F = 32767 / (cal.max - cal.zero)
    //
    // To calculate this as a 16.16 fixed-point fraction, just shift
    // everything left 16 bits:
    //
    //   F = 32767*65536 / (cal.max - cal.zero)
    //
    // The definition of the logical axis, by the way, comes from Visual
    // Pinball (VP).  This is the definition it requires for a HID joystick
    // axis mapped to an analog plunger device.  Other pinball simulators with
    // similar plunger support, such as Future Pinball, generally seem to
    // agree on this definition.
    //
    // Note that the negative portion of the logical axis range doesn't figure
    // into the calibration calculation.  We fix the scaling factor based on
    // the positive portion only, and just carry over the same scaling to the
    // negative region, so that logical units map to physical distances
    // uniformly across the whole axis.  This is the right way to figure
    // calibration, for two reasons.  The first is that zero is the most
    // important point on the logical axis: it's VP's single fixed reference
    // point, representing the plunger's rest position, and VP will only work
    // properly if the logical zero point matches the physical rest point.  So
    // the physical rest position (or, rather, the raw sensor reading with the
    // plunger at the rest position), and zero on the logical axis, are
    // necessarily our primary anchor points for the whole calibration.
    // Second, this is all based on the geometry of the standard mechanical
    // pinball plunger.  A standard plunger has about 85% of its total travel
    // range in the "retracted" portion of its travel, which maps to the
    // positive portion of the logical axis.  The forward travel portion,
    // negative on the logical axis, is the remaining 15% of the total range.
    // We want the position-to-logical-axis mapping to be uniform across the
    // entire positive and negative range - i.e., one millimeter of physical
    // travel maps to N logical axis units, uniformly across the entire axis,
    // positive and negative. (This wasn't always so clear; see historical
    // note below.)  Given the gross asymmetry between the positive and
    // negative excursion ranges, there's simply no point in taking the
    // forward part into account in the calibration calculation: the forward
    // portion is so tiny that it will always fit within the negative half of
    // the logical axis - it simply can't overflow because it'll only use
    // about 1/6 of the available arithmetic range.  This wastes 5/6 of the
    // available numerical space, which means that we don't have gradations of
    // distance that are quite as fine as we could if we were to use the
    // entire space, but this hardly matters given that we're absolutely
    // constrained to this same step size on the positive half of the axis.
    // And even at 1/6 of the range, that still leaves us with 5461 steps to
    // represent 12mm of physical travel, which works out to 1 logical unit =
    // 0.0022mm or 0.000087".  I'd be very happy indeed with a sensor with
    // that kind of precision; the best sensors we have available can resolve
    // about 0.06mm / .0025", and the worst ones operate at about the
    // centimeter level.  That means that we only need about 200 steps on the
    // logical axis to represent every single distinct point that the best
    // sensors can resolve forward of the rest point - and we have over 5000.
    // Sensors will have to get a whole lot better before we run out of steps.
    //
    // Historical note: Older VP versions treated the positive and negative
    // half-axes as having distinct physical-to-logical unit conversion
    // factors, specifically because of the asymmetry between the physical
    // plunger's forward and retracted travel distances.  The thinking at the
    // time was that it was somehow important for the plunger HID reports to
    // use the entire arithmetic range of the logical axis, from -32768 to
    // +32767, and the only way to do that, given the condition that you have
    // to peg zero to the physical rest position, is to have each logical unit
    // on the positive half-axis represent 6X the physical distance of one
    // unit on the negative half-axis.  The plunger device therefore built the
    // HID reports using this wacky "bi-linear" unit system, and VP used a
    // correspondingly asymmetric conversion turning the HID reports into
    // model plunger positions.  For reasons that are probably obvious by now
    // to the astute reader, this was a disastrously bad idea: it created a
    // gigantic discontinuity in the resolving power of the sensor across the
    // zero point.  It would have been workable with a sensor with infinite
    // resolution, perfect accuracy, and a plunger that always settled at
    // exactly the same physical resting spot, but of course none of these
    // conditions are close to true for our real systems.  The sensors aren't
    // perfectly linear and tend to have some noise, and the plunger has a
    // range of resting positions that's wide enough to be significant even
    // with the limitations of the sensors.  With the physical plunger and VP
    // not agreeing on exactly where the zero was, the 6X discontinuity in
    // scaling across the zero point made the simulated plunger behavior quite
    // erratic near the rest position.  This wasn't as noticeable as it might
    // have been with the early plunger devices, because they used Sharp IR
    // distance sensors that had awfully poor spatial resolution, on the order
    // of 1cm, which is only one part in eight over the plunger travel range;
    // so it was easy at the time to chalk up the erratic behavior to the
    // crappy sensors and call it unfixable.  As T'Pau says, the air is the
    // air; what can be done?  But better sensors that came along later made
    // it obvious that the "bi-linear" scheme was just wrong.  VP eventually
    // fixed it, and now uses a nice uniform linear scaling for any sensor it
    // recognizes as modern (it's conditional on such recognition in case
    // anyone is still using an ancient sensor, I guess).
    int32_t logicalAxisScalingFactor = 1;

    // Firing event state.  This tracks high-speed forward travel when the
    // user pulls back and releases the plunger.
    //
    // The reason we track firing events on the device side is that it's the
    // only place we can do it accurately.  It's impossible for the host to do
    // this work, because it doesn't have a fast enough data channel for
    // reading the sensor.  The host gets its readings via USB HID reports,
    // which are far too slow to track the high-speed motion that happens
    // during a pull-and-release gesture.  The speed mismatch results in the
    // classic "aliasing" problem in discrete-time sampling, where you can't
    // reconstruct a signal whose frequency is greater than one half of the
    // sampling rate.  In discrete-time sampling in general, attempting to
    // sample a signal that's above that critical frequency yields random
    // numbers rather than the original signal.  Visual Pinball and other
    // simulators use the "naive" approach of reading the plunger position at
    // each HID polling interval, and then trying to reconstruct the plunger
    // speed from successive position readings.  That's fine for low-speed
    // plunger motion, but the release gesture is so fast that it triggers the
    // aliasing problem, making it impossible for the simulator to reconstruct
    // the plunger speed correctly.  The simulator ends up with a random
    // number of the plunger speed, and applies a random amount of force to
    // the ball being launched.  Random is bad: it defeats the whole purpose
    // of the plunger simulation, which is to replicate the kind of skill-shot
    // action that's possible on a physical pinball table.  If the mechanical
    // plunger is just a random number generator, there's little point in
    // having it; we might as well just have a button labeled Launch Ball
    // Randomly.  That way there's no pretense, at least, but it's not very
    // fun, either.  This is supposed to be pinball, not a slot machine.
    //
    // The solution is to process the high-speed release motion locally on the
    // device side, where we have a high-speed channel to the sensor that's
    // fast enough to track the release motion accurately.  That lets us get
    // an accurate speed reading.  But how do we pass this to the simulator on
    // the PC?  This requires some manipulation of the Z Axis data passed
    // through the HID reports.  We don't just report the raw plunger position
    // duging the high-speed firing event.  Instead, we construct a series of
    // synthetic position reports that VP and other simulators will
    // reconstruct into an accurate simulation of the original physical
    // plunger motion.  This is a bit of a hack in that VP isn't a cooperating
    // partner in this scheme; instead, we exploit what VP was going to do
    // anyway, with its naive HID input model, constructing a series of
    // reports that VP's model will reconstruct into the correct simulated
    // motion.  A better approach (which we might explore in the future) would
    // be to make VP a cooperating partner by adding a new HID report type
    // that explicitly passes the launch force calculated on the device side,
    // so that we don't have to trick VP into doing the right thing.  That
    // would be better because it would be insensitive to changes in VP's
    // direct plunger-position input interpretation.
    // 
    // The firing state here tracks when we detect a firing event, and which
    // phase of the event we're in.
    FiringState firingState = FiringState::None;

    // Firing state time limit, in microseconds
    static const uint32_t DEFAULT_FIRING_TIME_LIMIT = 50000;
    uint32_t firingTimeLimit = DEFAULT_FIRING_TIME_LIMIT;

    // start time of current firing state
    uint64_t tFiringState = 0;

    // Console commands
    static void Command_plunger_S(const ConsoleCommandContext *ctx) { plunger.Command_plunger(ctx); }
    void Command_plunger(const ConsoleCommandContext *ctx);
};
