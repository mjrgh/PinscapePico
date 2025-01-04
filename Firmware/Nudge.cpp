// Pinscape Pico firmware - Nudge Device
// Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "Config.h"
#include "Logger.h"
#include "CommandConsole.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "Accel.h"
#include "Nudge.h"

// global singleton instance
NudgeDevice nudgeDevice;

// construction
NudgeDevice::NudgeDevice()
{
}

// Configure from JSON data
//
// nudge: {
//     source: <string>,       // the source device, by its configuration key, such as "mxc6655xa"; optional
//     x: "+X",                // the physical device axis to treat as the logical X axis; string, + or - followed by an axis, X, Y, or Z
//     y: "+Y",                // the physical device axis to treat as the logical Y axis
//     z: "+Z",                // the physical device axis to treat as the logical Z axis
// }
//
// The source device is optional, and isn't needed if only one physical
// accelerometer device is installed, since that device will automatically
// become the source by default.  The main purpose of 'source' is to
// select the accelerometer to use for nudge input if multiple devices are
// configured.  If 'source' isn't provided when multiple devices are
// present, the system will arbitrarily choose one to use as the source.
//
// The logical axis selections (x, y, z) lets the user remap the physical
// device axes to different logical axes, to account for the orientation
// and handedness of the device in the physical installation.  The Nudge
// Device models a pin cab where the X axis is lateral (side to side,
// positive to the right) and the Y axis is longitudinal (front to back,
// positive twoards the back), both in the plane of the cabinet floor.
// The optional Z axis is the vertical axis, aligned with the Earth's
// gravity vector and positive upwards.  It might be inconvenient or
// impossible to mount the physical sensor in this same orientation, and
// some sensors might use a different handedness.  The axis selection lets
// you map both rotation and handedness.  You still have to align the
// physical axes in parallel with the cabinet's major axes, but you can
// place it at any 90-degree rotation stop within that constraint.
//
void NudgeDevice::Configure(JSONParser &json)
{
    // configure the nudge device
    if (auto *val = json.Get("nudge"); !val->IsUndefined())
    {
        // Get the source device.  If there's an explicit "source"
        // property, look up the device in the accelerometer registration
        // table.  If there's no source, and there's exactly one device
        // configured, that device is the automatic default.  It's an
        // error if "source" doesn't refer to a configured device, or if
        // "source" is missing and there are multiple devices (or no
        // devices).
        if (auto *sourceProp = val->Get("source"); sourceProp->IsUndefined())
        {
            // source not specified - use the default device
            source = accelerometerRegistry.GetDefaultDevice();

            // confirm the selection, and warn if no devices or multiple devices are configured, 
            Log(LOG_CONFIG, "Nudge device: %s selected as accelerometer input\n", source->GetFriendlyName());
            if (source == accelerometerRegistry.GetNullDevice())
            {
                // null device selected - nudge will be inoperable
                Log(LOG_ERROR, "Nudge device: no accelerometer sensors are configured; nudging is disabled for this session\n");
            }
            else if (accelerometerRegistry.GetNumConfigured() > 1)
            {
                // multiple devices are configuerd, so the choice was arbitrary
                Log(LOG_WARNING, "Nudge device: additional accelerometers are configured; to select "
                    "a different device for nudge input, specify nudge.source in the JSON configuration\n");
            }
        }
        else if (auto sourceName = sourceProp->String(); (source = accelerometerRegistry.Get(sourceName)) == nullptr)
        {
            // source specified but no match found
            Log(LOG_ERROR, "nudge.source: no matching device for \"%s\"\n", sourceName.c_str());
        }

        // if there's no source, use the null device, so that we have a valid
        // object whether the selection process succeeded or failed
        if (source == nullptr)
            source = accelerometerRegistry.GetNullDevice();

        // Set the averaging window size to about 4 seconds worth of
        // samples.  With our set of 4 rolling averages, this will give
        // us a new sample snapshot once a second, each representing 4
        // seconds of trailing data.
        //
        // For the short average (used for manual centering), average
        // over 1/2 second, so that the average smooths out ambient
        // vibration and sensor noise but still reflects the
        // almost-instantaneous state that the user sees at the time
        // they apply manual centering.
        sampleRate = source->GetSamplingRate();
        autoCenterAverage.Init(sampleRate * 4);
        manualCenterAverage.Init(sampleRate / 2);

        // calculate the value of 1g in sensor units
        one_g = 32768 / source->GetGRange();

        // Figure the velocity conversion factor, to get the velocity
        // contribution in mm/s from an accelerometer reading.  This
        // combines the unit conversion from normalized accelerometer
        // units to mm/s^2, and the time step factor, or the time
        // interval of one sample from the device, equal to the inverse
        // of the sampling rate.  Normalized device units are in terms
        // of 'g' units, standard Earth gravity units, 9.80665 m/s^2.
        velocityConvFactor = static_cast<float>(source->GetGRange())/32768.0f * 9806.65f / static_cast<float>(sampleRate);

        // build the transform matrix from the logical axis selection
        auto SetTransform = [val, source = this->source](const char *prop, int *xform)
        {
            // get the property; just return and keep defaults if it's undefined
            auto *propVal = val->Get(prop);
            if (propVal->IsUndefined())
                return;

            // parse the string
            std::string txt = propVal->String();
            int idx = -1;
            const char *p = txt.c_str();

            // parse the sign, if present
            int sign = 0;
            if (*p == '+')
                sign = 1, ++p;
            else if (*p == '-')
                sign = -1, ++p;

            // use the main source by default
            auto *axisSource = source;

            // parse the axis source to get the transform matrix offset
            char axis = *p++;
            idx = (axis == 'x' || axis == 'X') ? 0 :
                  (axis == 'y' || axis == 'Y') ? 1 :
                  (axis == 'z' || axis == 'Z') ? 2 :
                  -1;

            // check for errors
            if (sign == 0 || idx == -1 || *p != 0)
            {
                Log(LOG_ERROR, "nudge.%s: invalid syntax, expected <sign><axis>, where the <sign> is '+' or '-', "
                    "and <axis> is X, Y, or Z\n", prop);
                return;
            }

            // success - fill in the matrix column, replacing the defaults
            xform[0] = xform[1] = xform[2] = 0;
            xform[idx] = sign;
        };
        SetTransform("x", &transform[0]);
        SetTransform("y", &transform[3]);
        SetTransform("z", &transform[6]);

        // restore saved settings, if available
        RestoreSettings();

        // success
        auto DescribeTransform = [](char *buf, int x, int y, int z) {
            auto Axis = [](char *p, char name, int val) {
                if (val == 1) *p++ = '+', *p++ = name;
                else if (val == -1) *p++ = '-', *p++ = name;
                return p;
            };
            *Axis(Axis(Axis(buf, 'X', x), 'Y', y), 'Z', z) = 0;
            return buf;
        };
        char xDesc[8], yDesc[8], zDesc[8];
        Log(LOG_CONFIG, "Nudge input configured on %s; nudge x = device %s, y = %s, z = %s\n",
            source->GetFriendlyName(),
            DescribeTransform(xDesc, transform[0], transform[1], transform[2]),
            DescribeTransform(yDesc, transform[3], transform[4], transform[5]),
            DescribeTransform(zDesc, transform[6], transform[7], transform[8]));

        // set the initial auto-centering quiet period, adding in a bit
        // extra to allow the initialization process to complete before the
        // first auto-centering is applied
        quietPeriodEndTime = time_us_64() + autoCenterInterval + 1000000;

        // Add our console command handler
        CommandConsole::AddCommand(
            "nudge", "Nudge device information",
            "nudge [options]\n"
            "options:\n"
            "  -s, --stats     show statistics\n"
            "  --calibrate     start noise calibration (runs for timed interval)\n"
            "  --dc-time <t>   set the DC filter time to <t> milliseconds; 0 disables the filter\n"
            "  --jitter-x <n>  set the X axis jitter (hysteresis) filter window to <n> units\n"
            "  --jitter-y <n>  set the Y axis jitter window to <n> units\n"
            "  --jitter-z <n>  set the Z axis jitter window to <n> units\n"
            "  --vscale <n>    set the velocity scaling factor to <n> units per mm/s\n"
            "  --vdecay <t>    set the velocity decay time to <t> milliseconds\n"
            "  --commit        commit in-memory settings to flash\n"
            "  --revert        revert settings to last values saved in flash\n",
            [](const ConsoleCommandContext *ctx){ nudgeDevice.Command_main(ctx); });
    }
}

// start calibration
void NudgeDevice::StartCalibration(bool autoSaveAtEnd)
{
    // set the mode flag, and figure the end time
    calMode = true;
    calModeAutoSave = autoSaveAtEnd;
    calEndTime = time_us_64() + CAL_MODE_INTERVAL;

    // clear the accumulators
    calModeData.Clear();
}

// save settings to flash
bool NudgeDevice::CommitSettings()
{
    // set up the settings file
    auto &s = settingsFile;
    s.quietThreshold = quietThreshold;
    s.dcTime = dcTime;
    s.xJitterWindow = xFilter.windowSize;
    s.yJitterWindow = yFilter.windowSize;
    s.zJitterWindow = zFilter.windowSize;
    s.autoCenterEnabled = autoCenterEnabled;
    s.autoCenterInterval = autoCenterInterval;
    s.velocityScalingFactor = static_cast<uint16_t>(velocityScalingFactor);
    s.velocityDecayTime = static_cast<uint16_t>(velocityDecayTime);

    // save it under the device name
    char fname[32];
    GetSettingsFilename(fname, sizeof(fname));
    return config.SaveStruct(fname, &s, sizeof(s), FLASH_SECTOR_SIZE);
}

// restore settings from flash
bool NudgeDevice::RestoreSettings(bool *pFileExists)
{
    // get the filename for the device
    char fname[32];
    GetSettingsFilename(fname, sizeof(fname));
    
    // load the settings file
    SettingsFile &s = settingsFile;
    bool fileExists = false;
    bool loaded = config.LoadStruct(fname, &s, sizeof(s), &fileExists);

    // pass back the file-exists status to the caller if desired
    if (pFileExists != nullptr)
        *pFileExists = fileExists;

    // set defaults if not loaded
    if (!loaded)
    {
        s.quietThreshold.x = DEFAULT_QUIET_THRESHOLD_XY;
        s.quietThreshold.y = DEFAULT_QUIET_THRESHOLD_XY;
        s.quietThreshold.z = DEFAULT_QUIET_THRESHOLD_Z;
        s.dcTime = 0.2f;
        s.autoCenterEnabled = true;
        s.autoCenterInterval = 4000000;
        s.velocityDecayTime = 2000;
        s.velocityScalingFactor = 100;
    }

    // count it as successful if we loaded a file, or if the file simply
    // doesn't exist; the latter case means that we should use defaults
    bool ok = loaded || !fileExists;

    // apply the quiet threshold
    quietThreshold = s.quietThreshold;

    // apply the auto-centering settings
    autoCenterEnabled = s.autoCenterEnabled;
    autoCenterInterval = s.autoCenterInterval;

    // set the filter parameters
    SetDCTime(s.dcTime);
    xFilter.SetWindow(s.xJitterWindow);
    yFilter.SetWindow(s.yJitterWindow);
    zFilter.SetWindow(s.zJitterWindow);

    // set the velocity scaling factor
    velocityScalingFactor = s.velocityScalingFactor;

    // set the velocity decay time
    SetVelocityDecayTime(s.velocityDecayTime);

    // return the load file status
    return ok;
}

// get the device-specific settings filename
void NudgeDevice::GetSettingsFilename(char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s.acc", source->GetConfigKey());
}

// task handler
void NudgeDevice::Task()
{
    // take a reading from the device
    int16_t xRaw, yRaw, zRaw;
    uint64_t t;
    source->Read(xRaw, yRaw, zRaw, t);

    // if this is a new sample, update the rolling averages
    if (t != timestamp)
    {
        // apply the transform to get logical coordinates
        int16_t x = transform[0]*xRaw + transform[1]*yRaw + transform[2]*zRaw;
        int16_t y = transform[3]*xRaw + transform[4]*yRaw + transform[5]*zRaw;
        int16_t z = transform[6]*xRaw + transform[7]*yRaw + transform[8]*zRaw;

        // update calibration data if in calibration mode
        if (calMode)
        {
            // accumulate the sample
            calModeData.sum.Add(x, y, z);
            calModeData.sum2.Add(x*x, y*y, z*z);
            calModeData.n += 1;

            // end calibration mode if we've reached the end time
            if (time_us_64() > calEndTime)
            {
                // no longer in calibration mode
                calMode = false;

                // Update the quiet threshold value
                static const auto CalcThreshold = [](uint64_t sum, uint64_t sum2, int n) {
                    int32_t sd2 = static_cast<int32_t>((sum2/n) - (sum*sum)/(n*n));
                    float sd = sqrtf(static_cast<float>(sd2));
                    return static_cast<int>(roundf(sd * 4.0f));
                };
                quietThreshold.x = CalcThreshold(calModeData.sum.x, calModeData.sum2.x, calModeData.n);
                quietThreshold.y = CalcThreshold(calModeData.sum.y, calModeData.sum2.y, calModeData.n);
                quietThreshold.z = CalcThreshold(calModeData.sum.z, calModeData.sum2.z, calModeData.n);

                // save the new calibration data persistently if requested
                if (calModeAutoSave)
                    CommitSettings();
            }
        }

        // Apply auto-centering and filtering.
        //
        // The Z axis should register 1g (one Earth standard gravitY) at
        // its resting point, so subtract out 1g before applying the
        // filter, and add it back in to the result.  This is necessary
        // because the DC blocker filter is designed to pull the
        // constant level to zero, so we have to make this baseline
        // adjustment for an axis with a non-zero constant level.
        fx = xFilter.Apply(x - cx);
        fy = yFilter.Apply(y - cy);
        fz = zFilter.Apply(z - cz - one_g) + one_g;

        // apply velocity attenuation
        vx *= velocityDecayFactor;
        vy *= velocityDecayFactor;
        vz *= velocityDecayFactor;

        // Apply the new acceleration reading to the velocity.  As usual
        // for Z, consider 1g the resting state, so the cabinet isn't
        // actually moving when steady 1g acceleration is applied; only
        // count it as moving when we detect a Z acceleration above or
        // below 1g.
        vx += static_cast<float>(fx) * velocityConvFactor;
        vy += static_cast<float>(fy) * velocityConvFactor;
        vz += static_cast<float>(fz - one_g) * velocityConvFactor;

        // report the filtered outputs to the views
        for (auto &view : views)
            view->OnDeviceReading(fx, fy, fz);

        // update the rolling averages
        autoCenterAverage.Add(x, y, z);
        manualCenterAverage.Add(x, y, z);

        // Update the auto-centering filter.  If the new readings differ
        // from the current rolling average on each axis by less than the
        // "quiet" threshold, consider the cabinet to be at rest, meaning
        // that it's not actively being nudged or otherwise disturbed,
        // making it a good time to collect averages to determine the
        // equilibrium point of each axis.  When the readings show no
        // motion continuously for the programmed auto-centering time
        // interval, use the trailing average as the new center point.
        int x2 = abs(x - autoCenterAverage.snapshot.x);
        int y2 = abs(y - autoCenterAverage.snapshot.y);
        int z2 = abs(z - autoCenterAverage.snapshot.z);
        uint64_t now = time_us_64();
        if (x2 < quietThreshold.x && y2 < quietThreshold.y && z2 < quietThreshold.z)
        {
            // This reading is within the quiet threshold.  Extend the
            // current quiet period, simply by not advancing the end time.
            // If we've reached the end time, and auto-centering is enabled,
            // apply the new average as the center point.
            if (autoCenterEnabled && now > quietPeriodEndTime)
            {
                // update the center point to the current rolling average
                CenterNow(autoCenterAverage.snapshot);
                
                // start a new quiet period
                quietPeriodEndTime = now + autoCenterInterval;
            }
        }
        else
        {
            // This reading exceeds the quiet threshold.  Interpret this as
            // the cabinet being actively disturbed, which makes this a bad
            // time to try to figure the centering position.  Reset the
            // quiet period end point to the full interval.  We'll keep
            // advancing it by the full interval until we stop getting these
            // large readings.
            quietPeriodEndTime = now + autoCenterInterval;

#if DEBUG_QUIET_PERIOD
            // capture the event in debug mode
            unquietEvents[unquietIndex++] = { { x, y, z }, { x2, y2, z2 }, now };
            if (unquietIndex >= _countof(unquietEvents))
                unquietIndex = 0;
#endif // DEBUG_QUIET_PERIOD
        }

        // Apply manual centering, if requested
        if (manualCenterRequest)
        {
            CenterNow(manualCenterAverage.snapshot);
            manualCenterRequest = false;
        }

        // Store the new instantaneous readings
        ax = x;
        ay = y;
        az = z;
        timestamp = t;
    }
}

void NudgeDevice::CenterNow(const XYZ &avg)
{
    // Center in the X/Y plane.  X and Y should read zero when the device
    // is perfectly level, so any deviation from 0,0 in the averages
    // represents a fixed tilt in the plane of the device, from the
    // projection of the Earth's gravity vector along the actual device
    // X/Y axis.  So simply set the the center point (cx, cy) to the
    // recent average, so that we subtract out this bias in the adjusted
    // readings we pass to clients.
    cx = avg.x;
    cy = avg.y;

    // Z should read exactly +1g when X/Y is perfectly level, since it
    // represents the vertical vector relative to the Earth's gravity.
    //
    // Z[adjusted] = Z[raw] - cz
    // -> cz = Z[raw] - Z[adjusted] = Z[raw] - 1g
    //
    // For true generality, the local gravity should be a parameter, in
    // case someone wants to set up a pin cab on the moon, Mars, or on a
    // spaceship.  For the time being, though, I think those use cases
    // will be uncommon; most users will have a better experience if we
    // *don't* add another parameter.
    cz = avg.z - one_g;

    // record the time
    lastAutoCenterTime = time_us_64();
}

// create an averaging device view
NudgeDevice::View *NudgeDevice::CreateView()
{
    // add a new list entry and return a pointer to it
    return views.emplace_back(new View());
}

// populate a vendor interface NudgeStatus query struct
size_t NudgeDevice::Populate(PinscapePico::NudgeStatus *s, size_t bufSize)
{
    // ensure there's room
    if (bufSize < sizeof(PinscapePico::NudgeStatus))
        return 0;

    // clear it to zero any fields we don't set
    memset(s, 0, sizeof(PinscapePico::NudgeStatus));

    // populate it - set the size
    s->cb = sizeof(PinscapePico::NudgeStatus);

    // flags
    if (calMode) s->flags |= s->F_CALIBRATING;

    // G range
    s->gRange = static_cast<uint8_t>(source->GetGRange());

    auto &f = settingsFile;
    if (quietThreshold != f.quietThreshold
        || dcTime != f.dcTime
        || xFilter.windowSize != f.xJitterWindow
        || yFilter.windowSize != f.yJitterWindow
        || zFilter.windowSize != f.zJitterWindow
        || autoCenterEnabled != f.autoCenterEnabled
        || autoCenterInterval != f.autoCenterInterval
        || quietThreshold.x != f.quietThreshold.x
        || quietThreshold.y != f.quietThreshold.y
        || quietThreshold.z != f.quietThreshold.z
        || velocityDecayTime != f.velocityDecayTime
        || velocityScalingFactor != f.velocityScalingFactor)
        s->flags |= s->F_MODIFIED;

    // raw accelerometer reading
    s->xRaw = ax;
    s->yRaw = ay;
    s->zRaw = az;

    // filtered accelerometer reading
    s->xFiltered = Clip(fx);
    s->yFiltered = Clip(fy);
    s->zFiltered = Clip(fz);
    s->timestamp = timestamp;

    // auto-centering information
    s->lastCenteringTime = time_us_64() - lastAutoCenterTime;
    s->xCenter = cx;
    s->yCenter = cy;
    s->zCenter = cz;

    // noise threshold
    s->xThreshold = quietThreshold.x;
    s->yThreshold = quietThreshold.y;
    s->zThreshold = quietThreshold.z;

    // recent averages
    s->xAvg = autoCenterAverage.snapshot.x;
    s->yAvg = autoCenterAverage.snapshot.y;
    s->zAvg = autoCenterAverage.snapshot.z;

    // velocities
    s->vx = Clip(vx * velocityScalingFactor);
    s->vy = Clip(vy * velocityScalingFactor);
    s->vz = Clip(vz * velocityScalingFactor);

    // return the struct size
    return sizeof(PinscapePico::NudgeStatus);
}

// Get parameters
size_t NudgeDevice::GetParams(PinscapePico::NudgeParams *s, size_t bufSize)
{
    // ensure there's room
    if (bufSize < sizeof(PinscapePico::NudgeParams))
        return 0;

    // clear it to zero any fields we don't set
    memset(s, 0, sizeof(PinscapePico::NudgeParams));

    // Populate it
    s->cb = sizeof(PinscapePico::NudgeParams);
    if (autoCenterEnabled) s->flags |= s->F_AUTOCENTER;
    s->autoCenterInterval = autoCenterInterval / 1000;  // we use us, network struct uses ms
    s->xThresholdAutoCenter = quietThreshold.x;
    s->yThresholdAutoCenter = quietThreshold.y;
    s->zThresholdAutoCenter = quietThreshold.z;
    s->dcTime = static_cast<uint16_t>(dcTime * 1000.0f);
    s->xJitterWindow = xFilter.windowSize;
    s->yJitterWindow = yFilter.windowSize;
    s->zJitterWindow = zFilter.windowSize;
    s->velocityScalingFactor = static_cast<uint16_t>(velocityScalingFactor);
    s->velocityDecayTime_ms = static_cast<uint16_t>(velocityDecayTime);

    // return the populated struct size
    return sizeof(PinscapePico::NudgeParams);
}

// Set model parameters
bool NudgeDevice::SetParams(const PinscapePico::NudgeParams *s, size_t bufSize)
{
    // make sure the struct is the right size - it has to be at least the original struct size
    if (bufSize < sizeof(PinscapePico::NudgeParams) || s->cb < sizeof(PinscapePico::NudgeParams))
        return false;

    // Update the auto-centering settings.  The network struct uses
    // milliseconds for the time interval, whereas we use microseconds
    // internally, so apply the unit conversion, and set a minimum of
    // 1ms (1000us).
    autoCenterEnabled = ((s->flags & s->F_AUTOCENTER) != 0);
    autoCenterInterval = std::max(1000, s->autoCenterInterval * 1000);
    quietThreshold.x = s->xThresholdAutoCenter;
    quietThreshold.y = s->yThresholdAutoCenter;
    quietThreshold.z = s->zThresholdAutoCenter;

    // if auto-centering is disabled, zero the centering point
    if (!autoCenterEnabled)
    {
        cx = 0;
        cy = 0;
        cz = 0;
    }

    // update the axis filter parameters
    SetDCTime(static_cast<float>(s->dcTime) / 1000.0f);
    xFilter.SetWindow(s->xJitterWindow);
    yFilter.SetWindow(s->yJitterWindow);
    zFilter.SetWindow(s->zJitterWindow);

    // set the velocity scaling factor and decay time
    velocityScalingFactor = static_cast<float>(s->velocityScalingFactor);
    SetVelocityDecayTime(s->velocityDecayTime_ms);
    
    // success
    return true;
}

void NudgeDevice::SetVelocityDecayTime(unsigned int ms)
{
    // set the new time
    velocityDecayTime = ms;

    // figure the new decay factor
    velocityDecayFactor = powf(0.5f, static_cast<float>(ms) / 1000.0f / static_cast<float>(sampleRate));
}


// ---------------------------------------------------------------------------
//
// Averaging device view
//

void NudgeDevice::View::TakeSnapshot()
{
    // take an average if we have any new samples since the last snapshot
    if (n != 0)
    {
        // compute the averages
        x = xSum / n;
        y = ySum / n;
        z = zSum / n;

        // reset the sums
        xSum = ySum = zSum = 0;
        n = 0;
    }
}

// ---------------------------------------------------------------------------
//
// Console commands
//

// main console command handler
void NudgeDevice::Command_main(const ConsoleCommandContext *c)
{
    // with no arguments, just show stats
    if (c->argc == 1)
        return ShowStats(c);

    // parse arguments
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            ShowStats(c);
        }
        else if (strcmp(a, "--calibrate") == 0)
        {
            StartCalibration(false);
            c->Printf("Calibration started; keep accelerometer still for %d seconds\n",
                CAL_MODE_INTERVAL/1000000);
        }
        else if (strcmp(a, "--dc-time") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing argument for --dc-time\n");

            int t = atoi(c->argv[i]);
            SetDCTime(static_cast<float>(t)/1000.0f);
            if (t == 0)
                c->Printf("DC filter disabled\n");
            else
                c->Printf("DC filter time set to %d ms\n", t);
        }
        else if (strncmp(a, "--jitter-", 9) == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing argument for %s\n", a);

            int t = atoi(c->argv[i]);
            if (t < 0 || t > 32767)
                return c->Printf("%s argument out of range; must be 0 to 32767\n", a);

            for (const char *p = a + 9; *p != 0 ; ++p)
            {
                char axis = *p;
                if (strchr("xyz", axis) == nullptr)
                    return c->Printf("Invalid --jitter axis '%c'\n", axis);
                
                (axis == 'x' ? xFilter : axis == 'y' ? yFilter : zFilter).SetWindow(t);
                c->Printf("%c axis jitter (hysteresis) filter window set to %d\n", toupper(axis), t);
            }
        }
        else if (strcmp(a, "--vscale") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing argument for --vscale\n");

            velocityScalingFactor = static_cast<float>(atoi(c->argv[i]));
            c->Printf("Velocity scaling factor set to %.0f units per mm/s\n", velocityScalingFactor);
        }
        else if (strcmp(a, "--vdecay") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing argument for --vdecay\n");

            SetVelocityDecayTime(atoi(c->argv[i]));
            c->Printf("Velocity decay time set to %u ms\n", velocityDecayTime);
        }
        else if (strcmp(a, "--commit") == 0)
        {
            bool ok = CommitSettings();
            c->Printf("Committing settings %s\n", ok ? "OK" : "failed");
        }
        else if (strcmp(a, "--revert") == 0)
        {
            bool ok = RestoreSettings();
            c->Printf("Reverting settings %s\n", ok ? "OK" : "failed");
        }
        else
        {
            return c->Printf("nudge: unknown option \"%s\"\n", a);
        }
    }
}

// show statistics on a command console
void NudgeDevice::ShowStats(const ConsoleCommandContext *c)
{
    uint64_t now = time_us_64();
    c->Printf(
        "Nudge device status:\n"
        "  Last raw reading: %d, %d, %d\n"
        "  Filtered:         %d, %d, %d\n"
        "  Velocity (mm/s):  %.1lf, %.1lf, %.1lf\n"
        "  Velocity (INT16): %d,%d,%d\n"
        "  Velocity scaling: %.0lf\n"
        "  DC blocker time:  %d ms\n"
        "  Jitter window:    %d, %d, %d\n"
        "  Average snapshot: %d, %d, %d\n"
        "  Auto-centering:   %s\n"
        "  Center coords:    %d, %d, %d\n"
        "  Last centered at: %llu us (%.2lf s ago)\n"
        "  Quiet thresholds: %d, %d, %d\n"
        "  Quiet period end: %llu us (now + %llu ms)\n"
        "  Calibrating:      %s\n",
        ax, ay, az,
        fx, fy, fz,
        vx, vy, vz,
        Clip(vx * velocityScalingFactor), Clip(vy * velocityScalingFactor), Clip(vz * velocityScalingFactor),
        velocityScalingFactor,
        static_cast<int>(roundf(dcTime * 1000.0f)),
        xFilter.windowSize, yFilter.windowSize, zFilter.windowSize,
        autoCenterAverage.snapshot.x, autoCenterAverage.snapshot.y, autoCenterAverage.snapshot.z,
        autoCenterEnabled ? "Enabled" : "Disabled",
        cx, cy, cz,
        lastAutoCenterTime, static_cast<double>((now - lastAutoCenterTime) / 10000)/100.0,
        quietThreshold.x, quietThreshold.y, quietThreshold.z,
        quietPeriodEndTime, static_cast<uint64_t>((quietPeriodEndTime - now)/1000),
        calMode ? "Yes" : "No");

#if DEBUG_QUIET_PERIOD
    //
    // For debugging, show rolling averages and recent readings that
    // interrupted auto-centering.  These shouldn't be needed for
    // general use but might be helpful from time to time for ongoing
    // development work on the firmware, so I'm leaving them here as
    // conditional code that's disabled by default.
    //
    c->Printf("  Auto-center averages:\n");
    for (int i = 0 ; i < _countof(autoCenterAverage.window) ; ++i)
    {
        auto &r = autoCenterAverage.window[i];
        int n = std::max(r.n, 1);
        c->Printf(
            "   [%d]: %d (%d..%d), %d (%d..%d), %d (%d..%d) / %u samples\n",
            i,
            r.x.sum / n, r.x.min, r.x.max,
            r.y.sum / n, r.y.min, r.y.max,
            r.z.sum / n, r.z.min, r.z.max,
            r.n);
    }

    c->Printf("  Recent readings exceeding quiet thresholds:\n");
    for (int i = 0, idx = unquietIndex - 1 ; i < 16 ; ++i, --idx)
    {
        if (idx < 0) idx = _countof(unquietEvents) - 1;
        auto &u = unquietEvents[idx];
        c->Printf(
            "   [%d]: (%d, %d, %d) delta(%d, %d, %d), %llu us ago\n",
            i,
            u.reading.x, u.reading.y, u.reading.z,
            u.delta.x, u.delta.y, u.delta.z,
            now - u.t);
    }
#endif // DEBUG_QUIET_PERIOD
}

// ---------------------------------------------------------------------------
//
// Filter
//

void NudgeDevice::Filter::CalcAlpha()
{
    // A time constant of zero means no DC blocking.  Very small
    // values make the filter unstable, so set a lower limit.
    if (nudge->dcTime == 0.0f)
        alpha = 0.0f;
    else
        alpha = 1.0f - 1.0f/(static_cast<float>(nudge->sampleRate) * fmaxf(nudge->dcTime, 0.05f));
}

void NudgeDevice::Filter::SetWindow(int size)
{
    windowSize = size;
    float midpt = (windowMin + windowMax) / 2;
    windowMin = midpt - windowSize;
    windowMax = midpt + windowSize;
}

int NudgeDevice::Filter::Apply(int in)
{
    // Apply the hysteresis filter.  Do this before the DC blocker, so that
    // wandering noise that settles near the zero point by not quite exactly
    // at zero is pulled to exactly zero in the DC blocker stage.
    if (in < windowMin)
    {
        windowMin = in;
        windowMax = in + windowSize;
    }
    else if (in > windowMax)
    {
        windowMax = in;
        windowMin = in - windowSize;
    }
    int out = in = (windowMin + windowMax) / 2;

    // apply the DC removal filter
    if (alpha != 0.0f)
    {
        // to retain fractional precision across readings, calculate ain
        // floating point, and store the previous output as a float
        float outf = alpha*outPrv + static_cast<float>(in - inPrv);
        inPrv = in;
        outPrv = outf;

        // convert to integer for the final output
        out = static_cast<int>(roundf(outf));
    }

    // return the result
    return out;
}
