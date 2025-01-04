// Pinscape Pico firmware - Nudge Device
// Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines the abstract interface to the "Nudge" device, which sends
// data to the PC host representing the user's physical interaction with
// the pinball cabinet.
//
// From the user's perspective, nudging is all about influencing the
// trajectory of the ball, by imparting well-timed impulse forces to the
// cabinet.
//
// Realistic nudging is one of the big things that elevates full-scale
// virtual pinball cabinet play over ordinary desktop pin sims.  Most
// desktop pinball games have some kind of "nudge" feature, but it's
// usually nothing more than a button press that randomly perturbs the
// ball's trajectory.  That doesn't come close to replicating the playing
// experience on a real machine, where you get to interact at a deeply
// mechanical level with the physics of the game.  On a virtual pin cab,
// though, we can use an accelerometer to detect the cabinet's motion and
// pass the information to the game, which can carry out the appropriate
// effects on the simulation.  This lets the player interact with the
// cabinet just like with a real pinball machine, and see the effects play
// out in the game.
//
// This class acts as a broker between the accelerometer sensor device and
// the USB input data stream going to the pinball simulator.  Its purpose
// is to interpret raw accelerometer readings into nudge data that the
// PC-side simulator can use.
//
//
// DEVICE DRIVERS
//
// This class is designed to be able to work with any accelerometer
// device, so that the software isn't tied to any particular device type.
// This class uses the abstract Accelerometer class, which defines a
// generic interface that can be implemented concretely for each device
// to be supported.  Each device's implementation of the Accelerometer
// class is what we refer to as a "device driver".
//
// Most accelerometers currently on the market use one of the standard
// microcontroller bus interfaces, I2C or SPI, both of which are easy to
// connect to a Pico thanks to built-in hardware interfaces for both bus
// types.  So implementing a driver for a new accelerometer is usually
// just a matter of setting up the device's configuration registers and
// reading its output registers.  Each device's register set is utterly
// idiosyncratic in the bit-level details, but the concepts are all pretty
// much the same across devices, so it's not usually too daunting a task.
//
// Currently supported devices:
//
//   MXC6655XA
//   MC3416
//   LIS3DH
//
//
// OVERVIEW AND THEORY OF OPERATION
//
// On the PC side, the standard way to send accelerometer input to a
// pinball simulator is via a joystick/gamepad interface.  Almost all
// simulators that support analog nudging take their nudge input as a
// 2-dimensional vector input via a pair of joystick axes, and interpret
// the input as an instantaneous acceleration to be applied to the
// simulation in real-time.  Visual Pinball adopted this approach early
// on, and it remains the standard design, because it's extremely
// straightforward to implement on the device side.  The device simply
// sends the raw accelerometer readings from the horizontal (X/Y) axes
// via joystick HID reports.  The device doesn't have to do any
// computation on the accelerometer readings beyond normalizing them to
// the HID joystick axis range.
//
// The scale of the readings passed across the joystick interface is
// arbitrary.  All of the PC-side pinball software that accepts analog
// nudge data has its own way of rescaling the input to a suitable
// range.  The device is free to choose whatever scale is convenient.
//
// Most accelerometer chips report their readings with fixed-width
// signed integers, which is a natural datatype fit for HID reports.  A
// pinball device can define its HID format to use its accelerometer
// chip's native reporting format, or it can rescale to another integer
// type if it prefers.  It's just a matter of setting the desired size
// in the HID report descriptor that the device sends to the PC.
// Because this class is designed to work with any underlying
// accelerometer chip, we define a canonical type we use for our HID
// reports, and require the device class to rescale its samples to our
// format.  We use 16-bit signed integers as our canonical type.  We
// chose that type because it has enough precision for all of the
// accelerometer chips I've looked at, and also because it's the type
// Visual Pinball uses to ingest HID data, so VP won't do any further
// rescaling in its input layers.
//
// In addition to the basic joystick reporting function, most pinball
// nudge devices also provide automatic "centering", which means that
// they continually reset the logical zero point for each axis based on
// the average readings over a rolling time window (typically a few
// seconds).  The purpose of this auto-centering is to subtract out any
// component of the acceleration readings that come from a tilt in the
// accelerometer, so that we report only the portion due to actual
// cabinet motion.  Ideally, there just wouldn't be any gravity
// component in the horizontal (X/Y) axes, because ideally the device
// would be installed perfectly leve, so that the Z axis aligns exactly
// with the Earth's gravity veotor.  It's extremely difficult to get the
// installation perfectly level, though, so in practice there'll always
// be some constant bias due to tilting.  The tilt bias is a constant
// term in the acceleration readings, because it's equal to the
// projection of the Earth's gravity vector along the tilted
// accelerometer axis; the Earth's gravity is constant, so as long as
// the tilt is also constant (which should be true if the device is
// fixed in place in the cabinet), the overall bias will be constant.
// That makes it possible to subtract out by observing readings over
// time and filtering out the fixed "DC" component.


#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "Pinscape.h"
#include "Accel.h"

// external classes
class JSONParser;
class ConsoleCommandContext;
namespace PinscapePico {
    struct NudgeStatus;
    struct NudgeParams;
}
   
// optional quiet period debugging (for development/testing, not
// needed for releases)
#define DEBUG_QUIET_PERIOD 0

// Nudge interface
class NudgeDevice
{
public:
    NudgeDevice();
    
    // Configure
    void Configure(JSONParser &json);

    // Period task handler.  This invokes the physical accelerometer
    // sensor's task handler, if a device is configured.
    void Task();

    // Request manual centering.  This sets an internal flag
    // that we process in the Task handler.
    void RequestManualCentering() { manualCenterRequest = true; }

    // Start calibration
    void StartCalibration(bool autoSaveAtEnd);

    // Is calibration running?
    bool IsCalMode() const { return calMode; }

    // calibration interval
    static const uint32_t CAL_MODE_INTERVAL = 15000000;

    // Averaging "view" object.  This is designed to be used with
    // logical USB devices that report accelerometer readings back to
    // the PC, such as gamepads.  The PC controls the USB polling
    // interval, so there might be a significant mismatch between the
    // physical accelerometer input rate and the PC polling rate.  The
    // rate difference can result in sampling errors that can make the
    // input visible on the PC side seem erratic.  For example, the PC
    // might miss a large peak during one nudge because it sampled after
    // the peak had passed, while it might poll exactly at the right
    // time to pick up the exact peak of a second smaller nudge, making
    // the second smaller nudge *appear* bigger than the first, in the
    // PC's view of the data.  This can be frustrating for users trying
    // to tune the sensitivity in the simulator because it makes the
    // simulation response appear randomly disconnected from the
    // physical input.  It helps if we average the physical readings
    // taken between USB polling samples, because an average better
    // captures everything that happened in the interval.  Averaging
    // isn't a perfect solution, since it also has a low-pass filtering
    // effect that reduces peaks, but it tends to make the data viewed
    // on the PC side more proportionally consistent with the physical
    // inputs.
    //
    // We handle the averaging in this separate "view" object to allow
    // for multiple USB devices consuming the same undelrying sensor
    // data at different USB sampling rates.  Each logical USB device
    // (e.g., a gamepad interface or an XInput interface) should create
    // its own view object.
    class View
    {
        friend class NudgeDevice;
    public:
        // Take a snapshot of the current averages.  Call this once on
        // each UBS polling cycle to get the current snapshot.
        void TakeSnapshot();

        // Get the current snapshot readings
        int16_t GetX() const { return x; }
        int16_t GetY() const { return y; }
        int16_t GetZ() const { return z; }

    protected:
        // protected constructor for NudgeDevice class use
        View() { }

        // receive a device reading
        void OnDeviceReading(int x, int y, int z)
        {
            // accumulate the reading into the averages
            xSum += x;
            ySum += y;
            zSum += z;
            n += 1;
        }

        // current snapshot
        int16_t x = 0;
        int16_t y = 0;
        int16_t z = 0;

        // sums for averaging
        int xSum = 0;
        int ySum = 0;
        int zSum = 0;

        // number of samples in average
        int n = 0;
    };

    // Create a view object
    View *CreateView();

    // Get the current velocity
    uint16_t GetVelocityX() const { return Clip(vx * velocityScalingFactor); }
    uint16_t GetVelocityY() const { return Clip(vy * velocityScalingFactor); }
    uint16_t GetVelocityZ() const { return Clip(vz * velocityScalingFactor); }

    // Save/restore flash settings.  These return true on success, false
    // on failure.  Restore applies defaults and returns sucess (true)
    // if the settings file simply doesn't exist; fileExists can be used
    // to distinguish that case from error conditions.
    bool CommitSettings();
    bool RestoreSettings(bool *fileExists = nullptr);

    // Populate a vendor interface NudgeStatus query struct
    size_t Populate(PinscapePico::NudgeStatus *buf, size_t bufSize);

    // Get/set nudge device parameters
    size_t GetParams(PinscapePico::NudgeParams *buf, size_t bufSize);
    bool SetParams(const PinscapePico::NudgeParams *buf, size_t bufSize);

protected:
    // Source device
    Accelerometer *source = &nullDevice;

    // source device sampling rate, samples per second
    int sampleRate = 100;

    // 1g (one standard Earth gravity), in sensor units
    int one_g = 16384;

    // default device
    NullAccelerometer nullDevice;

    // list of view objects
    std::list<View*> views;

    // Clip to int16_t range
    static int16_t Clip(int val) { return val < -32768 ? -32768 : val > 32767 ? 32767 : static_cast<int16_t>(val); }
    
    // is auto-centering enabled?
    bool autoCenterEnabled = true;

    // auto-centering time window in microseconds
    uint32_t autoCenterInterval = 4000000;

    // last auto-centering time
    uint64_t lastAutoCenterTime = 0;

    // three-axis vector
    struct XYZ
    {
        int x = 0;
        int y = 0;
        int z = 0;

        bool operator==(const XYZ &b) const { return x == b.x && y == b.y && z == b.z; }
        bool operator!=(const XYZ &b) const { return !(*this == b); }
    };

    // apply centering from the latest average snapshot
    void CenterNow(const XYZ &avg);

    // Physical axis to logical axis transform.  This is a rotation
    // matrix that we apply to the [X Y Z] vector of readings from the
    // physical accelerometer to obtain the [x y z] vector of logical
    // readings.  The logical orientation always uses 'x' as the
    // side-to-side axis (across the width of the cabinet) and 'y' as
    // front-to-back.  The transform matrix lets the user install the
    // physical device in any orientation and tell us how to obtain our
    // canonical logical axis readings from the raw readings.
    //
    //   physical   *   transform     ->  logical
    //                  [xX  yX  zX]  
    //   [X Y Z]        [xY  yY  zY]      [x y z]
    //                  [xZ  yZ  zZ]
    //
    // The default matrix is
    //                  [1  0  0]
    //   [X Y Z]        [0  1  0]         [X Y Z]
    //                  [0  0  1]
    //
    // For the C++ representation, we flatten the 3x3 matrix into a
    // linear array:
    //
    //                  [ X col ]   [ Y col ]  [ Z col ]
    //                  xX xY xZ    yX yY yZ   zX zY zZ
    //
    // For full generality, we could use floating-point values for the
    // matrix elements.  This isn't necessary in practice, though, because
    // I don't think there's any use case for installing the accelerometer
    // at non-right angles to the cabinet's major axes.  As long as it's
    // square with the cabinet, the matrix elements will all be zeroes and
    // ones (plus or minus).  This lets us use integer arithmetic to apply
    // the transfer, which is much faster than floating-point on a Pico.
    int transform[9] = { 1, 0, 0,   0, 1, 0,   0, 0, 1 };

    // a manual centering request has been submitted
    bool manualCenterRequest = false;

    // Latest instantaneous readings.  These reflect the raw data from
    // the accelerometer, not corrected for centering.
    int16_t ax = 0;
    int16_t ay = 0;
    int16_t az = 0;

    // Latest output report axis readings, after applying all filters:
    // auto-centering, DC removal, noise level attenuation.  These are
    // the readings reported on the axis outputs.
    int fx = 0;
    int fy = 0;
    int fz = 0;

    // timestamp of last instantaneous reading
    uint64_t timestamp = 0;

    // Center point.  This is the long-term average of recent readings
    // during periods where the accelerations have remained below the
    // noise threshold, indicating that these readings reflect the
    // component of the Earth's gravity along each axis.  Since the
    // Earth's gravity is a fixed constant background acceleration, we
    // wish to subtract it from the readings we pass to the PC, so that
    // the readings sent to the PC reflect only the accelerations due to
    // cabinet motion.
    int16_t cx = 0;
    int16_t cy = 0;
    int16_t cz = 0;

    // DC blocker/hysteresis filter
    struct Filter
    {
        // initialize with the adaptation time constant
        Filter(NudgeDevice *nudge) : nudge(nudge) { }

        // recalculate the alpha value from the sample rate and adaptation time
        void CalcAlpha();

        // set the hysteresis window size
        void SetWindow(int size);

        // containing nudge devie
        NudgeDevice *nudge;

        // Filter alpha, calculated from sample rate and DC adaptation time in parent device.
        // Zero disables the DC blocking part of the filter.
        float alpha = 0.0f;

        // hysteresis window size
        int windowSize = 0;

        // hysteresis window bounds
        int windowMin = 0;
        int windowMax = 0;

        // filter state
        int inPrv = 0;
        float outPrv = 0.0f;

        // apply the filter to an incoming value
        int Apply(int in);
    };
    Filter xFilter{ this };
    Filter yFilter{ this };
    Filter zFilter{ this };

    // DC blocker filter adaptation time constant, in seconds; 0 disables the filter
    float dcTime;

    // set the DC time - recalculates the filter parameters
    void SetDCTime(float t)
    {
        dcTime = t;
        xFilter.CalcAlpha();
        yFilter.CalcAlpha();
        zFilter.CalcAlpha();
    }

    // Rolling average.  We collect retrospective averages over recent
    // samples to use in the auto-centering and manual-centering process.
    // The averages remove ambient vibration and sensor noise over a
    // desired time window to help us calculate the DC offset within the
    // window.
    template<int nWindows> struct RollingAverage
    {
        // Initialize based on the sensor sampling rate.  Set the window
        // to (averaging period in seconds) * (samples per second).
        void Init(int windowSize)
        {
            // set the window size
            this->windowSize = windowSize;

            // initialize each window with a staggered starting position,
            // so that we close out a window every 1/nWindows samples
            for (int i = 0 ; i < nWindows ; ++i)
                window[i].n = i / nWindows;
        }
        
        // number of samples to collect in each window
        int windowSize = 400;

        // rolling average window
        struct Window
        {
            // number of samples colleted so far in this window
            int n = 0;

            // add a sample
            void Add(int16_t ax, int16_t ay, int16_t az)
            {
                ++n;
                x.Add(ax);
                y.Add(ay);
                z.Add(az);
            }

            // take a snapshot
            void TakeSnapshot(XYZ &avg, int windowSize)
            {
                avg.x = x.sum / windowSize;
                avg.y = y.sum / windowSize;
                avg.z = z.sum / windowSize;

                // reset counters
                n = 0;
                x.Reset(avg.x);
                y.Reset(avg.y);
                z.Reset(avg.z);
            }

            // axis running sum
            struct Axis
            {
                int sum = 0;       // sum over this axis for the time window
                int16_t min = 0;   // minimum reading
                int16_t max = 0;   // maximum reading
                
                void Add(int16_t val)
                {
                    sum += val;
                    min = std::min(min, val);
                    max = std::max(max, val);
                }
                
                void Reset(int16_t val)
                {
                    sum = 0;
                    min = max = val;
                }
            };
            Axis x;
            Axis y;
            Axis z;
        };
        Window window[nWindows];

        // latest snapshot average
        XYZ snapshot;

        // add a new sample, and close out the window if full
        void Add(int16_t ax, int16_t ay, int16_t az)
        {
            // add the sample to eah window
            Window *w = &window[0];
            for (int i = 0 ; i < nWindows ; ++i, ++w)
            {
                // add the sample
                w->Add(ax, ay, az);

                // collect a snapshot if the window is full
                if (w->n >= windowSize)
                    w->TakeSnapshot(snapshot, windowSize);
            }
        }
    };

    // Long rolling average, for auto-centering.  Collect four rolling
    // windows of 4 seconds each.  This gives us a snapshot once per
    // second.
    RollingAverage<4> autoCenterAverage;

    // Short rolling average, for manual centering.  Keep two windows of
    // 1/2 second each.  We keep a short averaging window so that
    // auto-centering reflects the almost instantaneous state.
    // "Almost", because we still want a little low-pass filtering to
    // smooth out ambient vibrations and sensor noise.
    RollingAverage<2> manualCenterAverage;

    // Velocity data.  The nudge device integrates the acceleration
    // data over time to track the cabinet velocity.  This is a better
    // way to report accelerometer readings to the PC-side pinball
    // simulator than via the raw acceleration readings, because we
    // have isochronous access to the accelerometer samples, which
    // the PC does not.
    //
    // The velocities are stored in physical units of mm/s^2.
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;

    // Velocity conversion factor.  This is the scaling factor to apply
    // to an acceleration sample to get the incremental velocity
    // contribution.  This combines a unit conversion from normalized
    // accelerometer units to mm/s^2 and the time step size.
    float velocityConvFactor = 1.0f;

    // Velocity scaling factor.  This is the scaling factor to apply to
    // internal velocity calculations, which are in mm/s, to convert to
    // the arbitrary INT16 units for USB reports.
    float velocityScalingFactor = 100.0f;

    // Velocity decay time, in milliseconds.  This is the velocity
    // half-life: the time in seconds that it takes for the velocity to
    // attenuate by 1/2 in the absence of new accelerations.
    //
    // The point of the decay time is to remove any non-random bias
    // to the noise component of the signal.  A velocity is essentially
    // the sum over all time of instantaneous accelerations, so if
    // there's any fixed bias in the acceleration data, it will cause
    // the velocity to grow without bound.  In our special case of a
    // pin cab, we can make the assumption that the overall system is
    // stationary, so the velocity should average to zero over long
    // time periods.  Attenuation is a simple way to apply this
    // assumption to correct for any bias in the accelerometer data.
    unsigned int velocityDecayTime = 2000;

    // Velocity decay factor, calculated from the decay time.  This
    // is the factor applied to the current velocity on each time
    // step (at the device sampling rate).
    float velocityDecayFactor = 1.0f;

    // Set the decay time - figures the new decay factor based on
    // the desired decay time and the sample rate.
    void SetVelocityDecayTime(unsigned int t);
    
    // Quiet threshold levels.  This gives the maximum deviation for
    // each axis from its rolling average that we'll consider to be
    // meaningless noise.  When we see deviations below these levels for
    // the duration of the quiet period, we'll auto-center it.  Note
    // that the noise level on most devices is proportional to the axis
    // reading when expressed in absolute units, so the noise level on Z
    // will be much higher than on X/Y for a device at rest, because Z
    // will always read close to 1g, while X/Y will read near zero.  We
    // use a default of 1% full scale for X/Y and 3% for Z.
    static const int DEFAULT_QUIET_THRESHOLD_XY = 328;
    static const int DEFAULT_QUIET_THRESHOLD_Z = 984;
    XYZ quietThreshold{ DEFAULT_QUIET_THRESHOLD_XY, DEFAULT_QUIET_THRESHOLD_XY, DEFAULT_QUIET_THRESHOLD_Z };

    // Quiet period end time.  We set this forward by the auto-centering
    // interval whenever there's too much current motion to count as a
    // quiet period.  When the system clock comes up on this time, then,
    // it means that there hasn't been any significant motion detected
    // for at least the centering interval, which makes it safe to take
    // the recent rolling average as the actual center point.
    uint64_t quietPeriodEndTime = ~0ULL;

    // Calibration data.  Calibration collects readings over a timed
    // interval where the user is keeping the cabinet still, to
    // measuer the typical the noise level on each axis.  We collect
    // an average and standard deviation for each axis.
    struct XYZ64
    {
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;

        void Add(int32_t x, int32_t y, int32_t z) { this->x += x; this->y += y; this->z += z; }
        void Clear() { x = y = z = 0; }
    };
    struct CalibrationData
    {
        // running totals for the average and standard deviation
        XYZ64 sum;     // sum of samples
        XYZ64 sum2;    // sum of squares of samples

        // number of samples
        int n = 0;

        void Clear() { n = 0; sum.Clear(); sum2.Clear(); }
    };
    CalibrationData calModeData;

    // flag: calibration is in progress
    bool calMode = false;

    // flag: auto-save at end of calibration
    bool calModeAutoSave = false;

    // calibration end time; meaningful when calibration is running
    uint64_t calEndTime = 0;

    // Settings file data
    struct SettingsFile
    {
        // auto-centering enabled
        bool autoCenterEnabled;

        // auto-centering interval in microseconds
        uint32_t autoCenterInterval;

        // calibrated quiet threshold
        XYZ quietThreshold;

        // filter parameters
        float dcTime;
        int xJitterWindow;
        int yJitterWindow;
        int zJitterWindow;

        // velocity decay time, in milliseconds
        uint16_t velocityDecayTime;

        // velocity scaling factor - INT16 units per mm/s
        uint16_t velocityScalingFactor;
    };

    // last loaded settings file
    SettingsFile settingsFile{ 0 };

    // get the settings filename
    void GetSettingsFilename(char *buf, size_t buflen);

#ifdef DEBUG_QUIET_PERIOD
    // log of recent events that exceeded the auto-centering bounds
    struct Unquiet
    {
        XYZ reading;
        XYZ delta;
        uint64_t t;
    };
    Unquiet unquietEvents[32];
    int unquietIndex = 0;
#endif // DEBUG_QUIET_PERIOD

    // console command handler
    void Command_main(const ConsoleCommandContext *ctx);
    void ShowStats(const ConsoleCommandContext *ctx);
};

// top-level alias for NudgeDevice::View nested class
typedef NudgeDevice::View NudgeDeviceView;

// Global singleton.  We always instantiate the generic nudge device
// interface, whether or not the user has configured a physical device,
// so that other subsystems can take readings without having to check
// if a device is configured.  In the absence of a configured physical
// device, the readings will simply always be zeroes.
extern NudgeDevice nudgeDevice;
