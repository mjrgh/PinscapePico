// Pinscape Pico - Plunger Interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/time.h>
#include <pico/flash.h>

// local project headers
#include "Pinscape.h"
#include "Logger.h"
#include "JSON.h"
#include "Config.h"
#include "CommandConsole.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "Plunger.h"
#include "ZBLaunch.h"
#include "PotPlunger.h"
#include "ProxPlunger.h"
#include "ADCManager.h"
#include "QuadraturePlunger.h"
#include "LinearPhotoSensorPlunger.h"
#include "Devices/ADC/ADS1115.h"
#include "Devices/DistanceSensor/VL6180x.h"
#include "Devices/LinearPhotoSensor/TCD1103.h"
#include "Devices/LinearPhotoSensor/TSL1410R.h"
#include "Devices/ProxSensor/VCNL4010.h"
#include "Devices/Quadrature/AEDR8300.h"

// global singleton
Plunger plunger;

// construction
Plunger::Plunger()
{
}

// Configure from JSON data
//
// plunger: {
//   source: "ads1115",    // sensor used as data source; usually inferred automatically - see below
//
//   autoZero: false,      // auto-zero the plunger after a period of inactivity; default false
//   autoZeroTime: 5000,   // auto-zero inactivity time, in milliseconds; default 5000
// }
//
// "source" specifies the sensor type used as the data source for the
// plunger readings.  This doesn't normally have to be specified,
// because the plunger can usually figure it out automatically by
// looking at which sensor type is present in the configuration.  When
// more than one sensor is configured, the "source" property should be
// included to select the one to use, otherwise the plunger will choose
// one of the configured devices arbitrarily.
//
// Source options:
//
//   "pico_adc"     - Pico on-board ADC (GPIO 26-28) with potentiometer input
//   "pico_adc[0]"  - first logical channel on Pico ADC, if multiple channels are configured
//   "pico_adc[1]"  - second logical channel on Pico ADC, if multiple channels are configured, same for [2]...
//   "ads1115"      - ADS1115 ADC with potentiometer input, first chip, first channel
//   "ads1115_0[0]" - ADS1115 ADC, first chip, first logical channel
//   "ads1115[1]"   - ADS1115 ADC, second logical channel [1]
//   "ads1115_1"    - second ADS1115 chip, first logical channel
//   "ads1115_1[1]" - second ADS1115 chip, second logical channel
//   "aedr8300"     - AEDR-8300 optical quadrature encoder
//   "tcd1103"      - TCD1103 linear imaging sensor
//   "tsl1410r"     - TSL1410R linear imaging sensor
//   "tsl1412s"     - TSL1412S linear imaging sensor
//   "vcnl4010"     - VCNL4010/VCNL4020 IR proximity sensor
//   "vl6180x"      - VL6180X IR time-of-flight distance sensor
//
void Plunger::Configure(JSONParser &json)
{
    // check for a "plunger:" key
    if (const auto *val = json.Get("plunger"); !val->IsUndefined())
    {
        // get auto-zeroing mode and time
        autoZeroEnabled = val->Get("autoZero")->Bool();
        autoZeroInterval = val->Get("autoZeroTime")->UInt32(5000) * 1000;

        // get the default enabled/disabled mode
        isEnabled = val->Get("enabled")->Bool(true);

        // construct the map of available ADC devices and channels by config key
        adcManager.EnumerateChannelsByConfigKey([this](const char *key, ADC *adc, int channelNum) {
            sensors.emplace(key, new PotPlungerSensor(adc, channelNum));
        });

        // add special plunger sensors
        if (tsl1410r != nullptr)
        {
            // add a TSL1410R plunger
            const char *variant = tsl1410r->GetVariantName();
            if (strcmp(variant, "TSL1410R") == 0)
                sensors.emplace("tsl1410r", new TSL1410RPlunger());
            else if (strcmp(variant, "TSL1412S") == 0)
                sensors.emplace("tsl1412s", new TSL1412SPlunger());
        }
        if (tcd1103 != nullptr)
        {
            sensors.emplace("tcd1103", new TCD1103Plunger());
        }
        if (aedr8300 != nullptr)
        {
            sensors.emplace("aedr8300", new QuadraturePlungerSensor(
                aedr8300.get(), PinscapePico::FeedbackControllerReport::PLUNGER_AEDR8300, "AEDR-8300", "aedr8300.cal", "aedr8300"));
        }
        if (vl6180x != nullptr)
        {
            sensors.emplace("vl6180x", new VL6180XPlunger());
        }
        if (vcnl4010 != nullptr)
        {
            sensors.emplace("vcnl4010", new VCNL4010Plunger());
        }

        // If the configuration specifies the source, apply the explicit
        // selection.  Otherwise make an arbitrary selection from the
        // available sensor devices.
        Sensor *sensor = nullptr;
        if (auto *sourceVal = val->Get("source") ; !sourceVal->IsUndefined())
        {
            // a source was explicitly specified - look it up
            auto sourceStr = sourceVal->String();
            if (auto it = sensors.find(sourceStr); it != sensors.end())
            {
                // select the named sensor
                sensor = it->second.get();
                Log(LOG_CONFIG, "Plunger: %s sensor source selected\n", sensor->FriendlyName());
            }
            else
            {
                // error - sensor not found
                Log(LOG_ERROR, "Plunger: sensor source \"%s\" is unknown, or sensor not configured\n", sourceStr.c_str());
                return;
            }
        }

        // If no sensor is configured, choose a default
        if (sensor == nullptr && sensors.size() != 0)
        {
            // Pick a sensor, based on the priority ratings per sensor
            for (auto &s : sensors)
            {
                // if this sensor has a higher priority than the one we've picked
                // so far, pick it over the last one
                if (sensor == nullptr || s.second->PriorityForPickingDefault() > sensor->PriorityForPickingDefault())
                    sensor = s.second.get();
            }

            // warn if multiple sensors are configured
            if (sensors.size() > 1)
            {
                Log(LOG_WARNING, "Plunger: multiple sensors are available, %s selected; to select "
                    "a different sensor, specify plunger.source in the configuration\n", sensor->FriendlyName());
            }
        }

        // if no sensor is configured, it's an error
        if (sensor == nullptr)
        {
            Log(LOG_ERROR, "Plunger: no plunger sensor is available; at least one sensor "
                "type must be configured\n");
            return;
        }

        // successful configuration - set the sensor
        this->sensor = sensor;

        // configure the sensor
        if (!sensor->Configure(val))
        {
            Log(LOG_ERROR, "Plunger: disabled due to sensor configuration failure\n");
            return;
        }

        // successful configuration - initialize the plunger
        Init();
        Log(LOG_CONFIG, "Plunger configured, sensor data source is %s\n", sensor->FriendlyName());
    }
}

// initialize
void Plunger::Init()
{
    // get the sensor's native scale
    nativeScale = sensor->GetNativeScale();
    
    // restore saved settings, including the calibration settings
    RestoreSettings();

    // enable console commands
    CommandConsole::AddCommand(
        "plunger", "plunger sensor diagnostics",
        "plunger <options>\n"
        "options:\n"
        "  --calibrate, -c            run interactive calibration\n"
        "  -f <num>                   set the firing state time limit, in microseconds\n"
        "  --firing-time-limit <num>  same as -f\n"
        "  -j <num>                   set jitter filter window size\n"
        "  --jitter <num>             same as -j\n"
        "  --integration-time <num>   set the integration time, in microseconds (imaging sensors only)\n"
        "  --scan-mode <num>          set the scan mode (for imaging sensors)\n"
        "  --save-settings            commit the current settings (jitter, orientation, calibration) to flash\n"
        "  --restore-settings         restore saved settings from flash\n"
        "  --read, -r                 show the latest readings (sensor and Z axis)\n"
        "  --status, -s               show status\n",
        &Command_plunger_S);
}

// push the calibration button
void Plunger::PushCalButton(bool on)
{
    // if switching on, note the start time
    if (on && !calButtonState)
        tCalButtonPush = time_us_64();

    // save the new mode
    calButtonState = on;
}

// start/stop calibration mode
void Plunger::SetCalMode(bool start, bool autoSave)
{
    // if the mode isn't changing, there's nothing to do
    if (start == calMode)
        return;

    // apply the change
    if (start)
    {
        // remember the start time
        tCalModeStart = time_us_64();
        
        // initialize the calibration numbers all to the current position,
        // since this is the full extent of the range we've seen so far
        // during this calibration run
        cal.min = cal.zero = cal.max = lastTaskRaw.rawPos;

        // start the zero-position averaging accumulator with the last reading
        calZeroSum = cal.zero;
        calZeroCount = 1;

        // clear the release-time averaging accumulator
        releaseTimeSum = 0;
        releaseTimeCount = 0;

        // tell the plunger we're starting calibration
        sensor->BeginCalibration(cal);

        // Set the initial zero point based on the latest reading
        calZeroStart = lastTaskRaw;
        OnUpdateCal();
    }
    else
    {
        // Leaving calibration mode.  Figure the average zero point from the
        // running totals.
        cal.zero = static_cast<uint16_t>(calZeroSum / calZeroCount);

        // Make sure the max is past the zero point.  If it's not, something
        // must have gone wrong with the calibration process.
        if (cal.max <= cal.zero)
        {
            // bad settings - reset to defaults
            cal.min = 0;
            cal.max = nativeScale;
            cal.zero = nativeScale/6;
            cal.calibrated = false;
        }
        else
        {
            // looks good - mark it as calibrated
            cal.calibrated = true;
        }

        // calculate the average firing event time
        cal.firingTimeMeasured = (releaseTimeCount != 0) ?
            static_cast<uint32_t>(releaseTimeSum / releaseTimeCount) : 0;

        // finalize the configuration in the plunger object
        sensor->EndCalibration(cal);

        // update our internal cached information for the new calibration
        OnUpdateCal();

        // If auto-save was enabled at the start of the calibration, OR the
        // caller explicitly asked for saving now, save the changes.
        if (autoSave || calModeAutoSave)
            CommitSettings();
    }

    // remember the new mode and auto-save setting
    calMode = start;
    calModeAutoSave = autoSave;
}

// period tasks
void Plunger::Task()
{
    // Check the calibration button state.  If we're not already in calibration
    // mode, and the button is pressed, check to see if we've exceeded the
    // holding period.  If so, start calibration mode.  (The button doesn't
    // have any effect once calibration mode starts.)
    //
    // Enable auto-save when starting a calibration via a button press.  The
    // dedicated button is meant to be an especially simple and quick user
    // interface, so we don't want to complicate things by requiring a
    // separate action to save the results of the calibration.
    if (!calMode && calButtonState && time_us_64() > tCalButtonPush + 2100000)
        SetCalMode(true, true);

    // check for calibration mode timeout
    if (calMode && time_us_64() > tCalModeStart + 15000000)
        SetCalMode(false, false);

    // Read a sample from the sensor; if we can't get a new sample, there's
    // nothing to do on this round
    RawSample s;
    if (!ReadSensor(s))
        return;

    // check for calibration mode
    ZWithTime zNew;
    uint32_t effectiveFiringTimeLimit = firingTimeLimit;
    if (calMode)
    {
        // Calibration mode active.  If the new reading is beyond the
        // calibration limits so far, note the new min/max point
        if (s.rawPos > cal.max)
            cal.max = s.rawPos;
        if (s.rawPos < cal.min)
            cal.min = s.rawPos;

        // If we've been sitting still for at least 200ms in the lower
        // portion of the sensor range, assume that we're at the rest
        // position.
        if (s.rawPos < nativeScale*4/10)
        {
            // check if it's close to the last position
            if (abs(static_cast<int32_t>(s.rawPos - calZeroStart.rawPos)) < nativeScale/100)
            {
                // it's close - check how long it's been here
                if (s.t - calZeroStart.t > 200000)
                {
                    // standing still here - add it to the average
                    calZeroSum += s.rawPos;
                    calZeroCount += 1;
                }
                else
                {
                    // it's stationary, but it hasn't been long enough yet;
                    // keep the current anchor point but don't count this
                    // sample
                }
            }
            else
            {
                // moving - set this as the new anchor point
                calZeroStart = s;
            }
        }
        
        // update our cached calibration data in case we moved a boundary
        OnUpdateCal();

        // Adjust for the current logical zero (park position) and rescale
        // to the logical axis range.  Since we're still calibrating, we
        // don't know the maximum sensor reading yet, so assume for now that
        // readings can span the entire sensor range.
        int64_t z = ((static_cast<int64_t>(s.rawPos) - static_cast<int64_t>(cal.zero)) * 32767) / (nativeScale - cal.zero);
        zNew.z = static_cast<int16_t>(z < -32768 ? -32768 : z > 32767 ? 32767 : z);
        zNew.t = s.t;

        // When in firing mode, set a high firing time limit, to be sure
        // that we capture firing events even if the user has manually
        // configured a low time limit.
        effectiveFiringTimeLimit = 100000;
    }
    else
    {
        // Not in calibration mode.  Apply the existing calibration and 
        // rescale to the joystick range.
        zNew.z = ApplyCalibration(s);
        zNew.t = s.t;
    }

    // If it hasn't been at least 1ms since the last reading, stop here.
    // Samples that are spaced too closely in time make the measurement
    // uncertainty in the 'dt' (delta time) too large relative to the
    // value, which propagates to the speed calculation.
    if (zNew.t - z0Nxt.t < 1000)
        return;

    // Shift the new reading into the three-point history
    z0Prv = z0Cur;
    z0Cur = z0Nxt;
    z0Nxt = zNew;

    // save the previous speed
    speedPrv = speedCur;

    // Figure the speed, in logical Z units per 10ms.  Use our three-point
    // history to calculate the speed at 'cur' from the slope taken at the
    // next and previous points.
    //
    // We use the peculiar unit system - "per 10ms" rather than "per ms" or
    // per some other common unit - because the real-world measurements for
    // a standard post-1980 pinball plunger assembly will range from about
    // -20000 to +20000 in these units.  That's an excellent fit for the
    // 16-bit signed integer container we'll use to move the readings across
    // the USB HID interface: it makes good use of the precision space (so
    // we can express distinct speeds with fine granularity) while leaving
    // plenty of headroom before overflow.
    int64_t v = (static_cast<int64_t>(z0Nxt.z - z0Prv.z) * 10000) / static_cast<int64_t>(z0Nxt.t - z0Prv.t);
    speedCur = (v < -32768) ? -32768 : (v > 32767) ? 32767 : static_cast<int16_t>(v);

    // If this is moved since last time, reset the auto-zero time
    if (autoZeroEnabled && z0Prv.z != z0Cur.z)
        tAutoZero = time_us_64() + autoZeroInterval;

    // Assume that the Z0 reported is simply the current Z0
    z0Reported = z0Cur;

    // If we're in a firing event, and the plunger is just now reversing
    // course at the bounce off the barrel spring, set the Z0 hold position.
    // This holds Z0 at the peak forward position for the time it takes to
    // send a few HID reports, to ensure that the host has a chance to see the
    // plunger reach the forward position.  The bounce is so fast that it
    // often occurs between HID reports, so without the hold, the host would
    // only see the pre- and post-bounce positions, which could both be behind
    // the resting position.  The host in this case wouldn't realize that the
    // plunger moved forard enough to launch the ball, so the launch would be
    // delayed until a HID report happened to catch a bounce position forward
    // enough to hit the ball.  That lag can be quite long, on the order of a
    // whole second, which to the user is a very obvious glitch.  Marking the
    // peak forward position and holding there for a few HID cycles corrects
    // this, and since we only need the time of a few HID cycles, it's fast
    // enough to be imperceptible to the user in the plunger animation.
    if (z0Cur.t < z0Hold.t)
    {
        // we're already in a hold - report the prior z0, but at the time
        // of the current reading
        z0Reported.z = z0Hold.z;
        speedCur = speedHold;
    }
    else if ((firingState == FiringState::Moving || firingState == FiringState::Fired) && z0Prv.z < 0 && z0Cur.z > z0Prv.z)
    {
        // bounce just started - set a hold at the peak position for a few
        // HID cycles
        z0Reported.z = z0Hold.z = z0Prv.z;
        z0Hold.t = z0Cur.t + 40000;
        speedHold = speedPrv;
    }

    // Set the tentative current "processed" Z reading
    zCur = z0Cur;

    // If the speed is negative, we're moving forward, so continue any
    // forward run in progress.  Otherwise, note this as the possible new
    // starting point for the next forward run.
    if (speedCur > 0)
        zForwardStart = z0Cur;

    // Update the firing state
    switch (firingState)
    {
    case FiringState::None:
        // Not previously in firing mode.  Check for forward motion starting
        // from at least 1/6 retraction.
        if (zCur.z >= 32767/6 && speedCur < 0)
        {
            // Enter Moving state, and freeze processed readings (zCur) at
            // the starting point of the acceleration.  We hold the plunger
            // at the retracted position in the processed reading to make
            // sure that the simulator actually sees the fully retracted
            // position - if we didn't hold it here for long enough, the
            // simulator is likely to miss the maximum retraction point due
            // to HID undersampling.  If the simulator misses the full
            // retraction point, it will calculate the impulse for the ball
            // launch based on whatever undersampled reading it did happen
            // to catch along the way, which effectively makes the impulse
            // calculation a random number generator.  The trickery of
            // holding the reading steady helps the simulator calculate a
            // meaningful impulse for the launch, based on how far back the
            // plunger was actually pulled just before the launch.
            firingState = FiringState::Moving;
            tFiringState = zCur.t;
            zCur = zForwardStart;
        }
        break;

    case FiringState::Moving:
        // Moving: we detected forward acceleration from a retracted
        // position.  Firing mode ends when we cross the resting
        // position, or when the acceleration ends.
        if (zCur.z <= 0)
        {
            // We crossed the zero point.  This is where the plunger normally
            // strikes a ball sitting in the shooter lane and launches the
            // ball, so we want to coordinate this event in real time with the
            // simulator.  Move the processed position (zCur) forward to the
            // new reading so that the simulator immediately fires its
            // plunger, which we've been artifically holding in the retracted
            // position until now.
            firingState = FiringState::Fired;
            tFiringState = zCur.t;

            // The real plunger will bounce off of the barrel spring at this
            // point.  The forward excursion is a function of the speed and
            // the barrel spring constant, but the distances are so small on
            // this side of the zero that I don't think it's worth a
            // detailed calculation here.  We'll just go forward to the
            // calibrated minimum point.
            zCur.z = ApplyCalibration(cal.min);

            // add statistics during calibration
            if (calMode)
            {
                releaseTimeSum += zCur.t - zForwardStart.t;
                releaseTimeCount += 1;
            }
        }
        else if (speedCur < 0 && zCur.t < zForwardStart.t + effectiveFiringTimeLimit)
        {
            // Still moving forward, and still within the firing time
            // limit.  Stay in firing mode, keep reporting the starting
            // point of the acceleration run.
            zCur = zForwardStart;
        }
        else
        {
            // No longer moving forward, or the time limit has expired,
            // indicating that it's moving forward too slowly to be a
            // pull-and-release operation (so the user must be manually
            // moving it).  Return to the default state.
            firingState = FiringState::None;
            tFiringState = zCur.t;
            zForwardStart = zCur;
        }
        break;
        
    case FiringState::Fired:
        // "Fired" state, holding at forward compression position after
        // crossing the zero point from a Moving state.  Hold the
        // processing readings here for 40ms to make sure that the
        // simulator sees a reading at this end of the range.  This
        // allows it to miss one or two USB reports and still see the
        // minimum point.
        if (zCur.t < tFiringState + 40000)
        {
            // stay at the minimum point
            zCur.z = cal.min;
        }
        else
        {
            // It's been long enough in the bouncing state.  Advance to the
            // Settling phase, where we report the park position until the
            // plunger comes to rest
            firingState = FiringState::Settling;
            tFiringState = zCur.t;

            // report the park position
            zCur.z = 0;
        }
        break;

    case FiringState::Settling:
        // Firing event, holding processed readings at the park position
        // while waiting for the mechanical plunger to actually settle.
        // Stay here for a few moments so that the PC client can simulate
        // the full release motion, then return to real readings.
        if (zCur.t < tFiringState + 100000)
        {
            // stay here a while longer
            zCur.z = 0;
        }
        else
        {
            // it's been long enough - return to normal operation
            firingState = FiringState::None;
            tFiringState = zCur.t;
        }
        break;
    }

    // Check for auto-zeroing, if enabled
    if (autoZeroEnabled && time_us_64() >= tAutoZero)
    {
        // The scheduled auto-zero time has arrived, meaning that
        // the plunger has been stationary for the the auto-zeroing
        // interval.  Auto-zero the sensor.
        if (sensor->AutoZero(cal))
        {
            // The sensor applied the auto-zeroing, so change the
            // current reading to zero.  Also zero out the whole
            // 3-point history, so that we don't interpret this as
            // an abrupt high-speed movement; the whole point is
            // that the plunger isn't moving at all, so we don't
            // want to interpret this as having non-zero speed.
            zCur.z = z0Cur.z = z0Prv.z = z0Nxt.z = 0;
        }
        
        // Set the timer to infinity, so that we don't keep repeating
        // the auto-zero call if the plunger sits still here forever.
        tAutoZero = ~0ULL;
    }
}

// read a sample from the physical sensor
bool Plunger::ReadSensor(RawSample &s)
{
    // if the hardware isn't ready, or we fail to read a new one, return the last
    // sample and indicate Not Ready
    RawSample r;
    if (!sensor->IsReady() || !sensor->ReadRaw(r))
        return false;

    // apply reverse orientation if set
    if (reverseOrientation)
        r.rawPos = sensor->GetNativeScale() - r.rawPos;

    // apply the jitter filter if desired
    if (sensor->UseJitterFilter())
        r.rawPos = ApplyJitterFilter(r.rawPos);

    // save the last raw reading and pass it back to the caller
    lastTaskRaw = s = r;

    // success
    return true;
}

uint32_t Plunger::ApplyJitterFilter(uint32_t pos)
{
    // remember the incoming pre-filtered value
    jitterFilter.lastPre = pos;

    // Check to see where the new reading is relative to the
    // current window
    if (pos < jitterFilter.lo)
    {
        // the new position is below the current window, so move
        // the window down such that the new point is at the bottom 
        // of the window
        jitterFilter.lo = pos;
        jitterFilter.hi = pos + jitterFilter.window;

        // use the midpoint of the new window as the filtered position
        jitterFilter.lastPost = (jitterFilter.lo + jitterFilter.hi)/2;
    }
    else if (pos > jitterFilter.hi)
    {
        // the new position is above the current window, so move
        // the window up such that the new point is at the top of
        // the window
        jitterFilter.hi = pos;
        jitterFilter.lo = (pos <= jitterFilter.window) ? 0 : pos - jitterFilter.window;

        // use the midpoint of the new window as the filtered position
        jitterFilter.lastPost = (jitterFilter.lo + jitterFilter.hi)/2;
    }
    else
    {
        // the new position is inside the current window, so simply
        // repeat the last post-filtered reading
    }

    // return the new filtered position
    return jitterFilter.lastPost;
}

void Plunger::SetJitterWindow(uint16_t w)
{
    // set the new window size
    jitterFilter.window = w;
    
    // reset the running window
    jitterFilter.hi = jitterFilter.lo = jitterFilter.lastPost;
}

void Plunger::SetIntegrationTime(uint32_t us)
{
    // set it in the sensor
    sensor->SetIntegrationTime(us);

    // save it internally
    integrationTime = us;
}

void Plunger::SetScanMode(uint8_t mode)
{
    // set it in the sensor
    sensor->SetScanMode(mode);

    // save it internally
    scanMode = mode;
}

void Plunger::InitSettingsFileStructFromLive()
{
    memcpy(&settingsFile.cal, &cal, sizeof(settingsFile.cal));
    settingsFile.jfWindow = jitterFilter.window;
    settingsFile.firingTimeLimit = firingTimeLimit;
    settingsFile.integrationTime = integrationTime;
    settingsFile.reverseOrientation = (reverseOrientation ? 1 : 0);
    settingsFile.scanMode = scanMode;
    settingsFile.manualScalingFactor = manualScalingFactor;
}

bool Plunger::CommitSettings()
{
    // save the in-memory settings
    InitSettingsFileStructFromLive();
    return config.SaveStruct(sensor->GetCalFileName(), &settingsFile, sizeof(settingsFile), FLASH_SECTOR_SIZE);
}

bool Plunger::RestoreSettings(bool *pFileExists)
{
    // load the settings file
    bool fileExists = false;
    bool loaded = config.LoadStruct(sensor->GetCalFileName(), &settingsFile, sizeof(settingsFile), &fileExists);

    // return success if we loaded the struct OR the file doesn't exist;
    // the latter case still counts as success, since the absence of the
    // file isn't an error, but simply means that we should use defaults
    bool ok = loaded || !fileExists;

    // pass back the file-exists status to the caller, if desired
    if (pFileExists != nullptr)
        *pFileExists = fileExists;

    // apply the loaded calibration data, or use defaults if nothing was loaded
    if (loaded)
    {
        // loaded successfully - use the loaded data
        memcpy(&cal, &settingsFile.cal, sizeof(cal));
    }
    else
    {
        // nothing loaded - use defaults
        memset(&cal, 0, sizeof(cal));
        
        // set the range to the full native sensor range (0 to sensor scale), with the zero at the 1/6 point
        cal.min = 0;
        cal.max = sensor->GetNativeScale();
        cal.zero = cal.max / 6;
        Log(LOG_CONFIG, "No saved plunger calibration data found; using defaults\n");
    }
    
    // update derived calibration data
    OnUpdateCal();
    
    // pass the calibration data to the sensor for initialization
    sensor->OnRestoreCalibration(cal);

    // apply the saved setting
    SetJitterWindow(settingsFile.jfWindow);
    SetFiringTimeLimit(settingsFile.firingTimeLimit != 0 ? settingsFile.firingTimeLimit : 50000);
    SetIntegrationTime(settingsFile.integrationTime);
    SetReverseOrientation(settingsFile.reverseOrientation != 0);
    SetScanMode(settingsFile.scanMode);
    manualScalingFactor = (settingsFile.manualScalingFactor == 0 ? 100 : settingsFile.manualScalingFactor);

    // if we're using defaults, initialize the settings file from the defaults,
    // so that we'll be able to tell if the live values have been modified from
    // the defaults by user action
    InitSettingsFileStructFromLive();

    // return the status
    return ok;
}

bool Plunger::SetCalibrationData(const PinscapePico::PlungerCal *data, size_t len)
{
    // make sure we have the required fields
    if (len < sizeof(PinscapePico::PlungerCal))
    {
        Log(LOG_ERROR, "Plunger SetCalibrationData vendor request: bad transfer size (%u bytes, smaller than v1 struct size of %u bytes)\n",
            static_cast<int>(len), static_cast<int>(sizeof(PinscapePico::PlungerCal)));
        return false;
    }

    // load the data
    cal.min = data->calMin;
    cal.zero = data->calZero;
    cal.max = data->calMax;
    cal.calibrated = ((data->flags & data->F_CALIBRATED) != 0);
    cal.firingTimeMeasured = data->firingTimeMeasured;
    memcpy(cal.raw, data->sensorData, std::min(sizeof(cal.raw), sizeof(data->sensorData)));

    // update derived calibration data
    OnUpdateCal();

    // update the sensor with the new data
    sensor->OnRestoreCalibration(cal);

    // success
    return true;
}

void Plunger::OnUpdateCal()
{
    // Figure the scaling factor to convert from raw sensor readings to
    // logical Z axis values.  The Z axis is normalized so that 0
    // represents the rest position and +32767 represents the maximum
    // retraction point, so there are 32767 logical Z axis units over
    // the native range between cal.max and cal.zero.
    //
    // Ensure the range size is a non-zero positive number.  Zero would
    // give us a divide-by-zero error, and negative would invert the
    // range, so both are nonsensical.  Use a default scaling if the
    // values look invalid.
    if (cal.max > cal.zero)
        logicalAxisScalingFactor = static_cast<int32_t>(32767*65536 / (cal.max - cal.zero));
    else
        logicalAxisScalingFactor = 32767;
}

int16_t Plunger::ApplyCalibration(uint32_t rawPos)
{
    // adjust for the zero point
    int64_t pos = static_cast<int64_t>(rawPos) - cal.zero;

    // scale to the logical axis range
    pos = (pos * logicalAxisScalingFactor) / 65536L;

    // apply the manual scaling factor
    pos = (pos * manualScalingFactor) / 100;

    // clip to the INT16 range
    return pos > 32767 ? 32767 : pos < -32768 ? -32768 : pos;
}

int16_t Plunger::ApplyCalibration(const RawSample &s)
{
    return ApplyCalibration(s.rawPos);
}

void Plunger::Command_plunger(const ConsoleCommandContext *c)
{
    // make sure we have some arguments
    if (c->argc == 1)
        return c->Usage();

    // process arguments
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "--read") == 0 || strcmp(a, "-r") == 0)
        {
            c->Printf("Last reading:\n"
                      "  Sensor units:           %u\n"
                      "  Z axis (logical units): %d\n"
                      "  Timestamp:              %llu (%llu us ago)\n",
                      lastTaskRaw.rawPos, zCur.z, lastTaskRaw.t, time_us_64() - lastTaskRaw.t);
        }
        else if (strcmp(a, "--status") == 0 || strcmp(a, "-s") == 0)
        {
            c->Printf(
                "Calibration button state: %s\n"
                "Calibration mode active:  %s\n"
                "Orientation:              %s\n"
                "Firing state:             %s\n"
                "Firing time average:      %.2fms\n"
                "Firing time limit:        %luus%s\n"
                "Integration time:         %luus\n"
                "Scan mode:                %d\n"
                "Calibration data:\n"
                "   Min     %lu\n"
                "   Zero    %lu\n"
                "   Max     %lu\n"
                "Jitter filter:\n"
                "   Window  %lu\n"
                "   Low     %lu\n"
                "   High    %lu\n",
                calButtonState ? "Down" : "Up",
                calMode ? "Yes" : "No",
                reverseOrientation ? "Reverse" : "Standard",
                firingState == FiringState::None ? "None" :
                firingState == FiringState::Moving ? "Moving" :
                firingState == FiringState::Fired ? "Fired" :
                firingState == FiringState::Settling ? "Settling" :
                "Unknown",
                static_cast<float>(releaseTimeSum / releaseTimeCount) / 1000.0f,
                firingTimeLimit == 0 ? DEFAULT_FIRING_TIME_LIMIT : firingTimeLimit,
                firingTimeLimit == 0 ? " (Default)" : "",
                integrationTime,
                scanMode,
                cal.min, cal.zero, cal.max,
                jitterFilter.window, jitterFilter.lo, jitterFilter.hi);
        }
        else if (strcmp(a, "--calibrate") == 0)
        {
            c->Printf("Entering calibration mode - pull and release the plunger several times\n");
            SetCalMode(true, false);
        }
        else if (strcmp(a, "-j") == 0 || strcmp(a, "--jitter") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing jitter window size argument for %s\n", a);

            int n = atoi(c->argv[i]);
            SetJitterWindow(n);
            c->Printf("Jitter window set to %d\n", n);
        }
        else if (strcmp(a, "--integration-time") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing integration time argument for %s\n", a);

            int n = atoi(c->argv[i]);
            SetIntegrationTime(n);
            c->Printf("Integration time set to %d us\n", n);
        }
        else if (strcmp(a, "--scan-mode") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing scan mode argument for %s\n", a);

            int n = atoi(c->argv[i]);
            if (n < 0 || n > 255)
                return c->Printf("Invalid scan mode - must be 0..255\n");
            
            SetScanMode(static_cast<uint8_t>(n));
            c->Printf("Scan mode set to %d\n", n);
        }
        else if (strcmp(a, "-f") == 0 || strcmp(a, "--firing-time-limit") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing firing time argument for %s\n", a);

            int n = atoi(c->argv[i]);
            SetFiringTimeLimit(n);
            c->Printf("Firing time set to %d us\n", n);
        }
        else if (strcmp(a, "--save-settings") == 0)
        {
            if (CommitSettings())
                c->Printf("Settings saved\n");
            else
                c->Printf("Error saving settings\n");
        }
        else if (strcmp(a, "--restore-settings") == 0)
        {
            if (RestoreSettings())
                c->Printf("Settings restored\n");
            else
                c->Printf("Error restoring settings\n");
        }
        else
        {
            return c->Printf("Unrecognized option \"%s\"\n", a);
        }
    }
}

// populate the PlungerReading vendor interface struct
size_t Plunger::Populate(PinscapePico::PlungerReading *pd, size_t maxSize)
{
    // make sure the buffer is big enough for the struct
    using PlungerReading = PinscapePico::PlungerReading;
    if (sizeof(PlungerReading) > maxSize)
        return 0;
    
    // clear the struct and set the size
    size_t actualSize = sizeof(PlungerReading);
    memset(pd, 0, sizeof(PlungerReading));
    pd->cb = sizeof(PlungerReading);

    // set the position data
    pd->timestamp = zCur.t;
    pd->z = zCur.z;
    pd->z0 = z0Cur.z;
    pd->speed = speedCur;
    pd->z0Prv = z0Prv.z;
    pd->z0Nxt = z0Nxt.z;
    pd->dt = z0Nxt.t - z0Prv.t;
    pd->rawPos = lastTaskRaw.rawPos;

    // set flags
    pd->flags = 0;
    if (reverseOrientation) pd->flags |= PlungerReading::F_REVERSE;
    if (cal.calibrated) pd->flags |= PlungerReading::F_CALIBRATED;
    if (calMode) pd->flags |= PlungerReading::F_CALIBRATING;
    if (zbLaunchBall.IsFiring())
        pd->flags |= PlungerReading::F_ZBLAUNCH;

    // test for modifications between live settings and saved settings
    if (jitterFilter.window != settingsFile.jfWindow
        || firingTimeLimit != settingsFile.firingTimeLimit
        || integrationTime != settingsFile.integrationTime
        || reverseOrientation != (settingsFile.reverseOrientation != 0)
        || scanMode != settingsFile.scanMode
        || manualScalingFactor != settingsFile.manualScalingFactor
        || memcmp(&cal, &settingsFile.cal, sizeof(cal)) != 0)
        pd->flags |= PlungerReading::F_MODIFIED;

    // set the firing state
    pd->firingState = static_cast<uint16_t>(firingState);

    // set the calibration data
    pd->calMin = cal.min;
    pd->calZero = cal.zero;
    pd->calMax = cal.max;
    pd->firingTimeMeasured = cal.firingTimeMeasured;
    memcpy(pd->calSensorData, cal.raw, std::min(sizeof(pd->calSensorData), sizeof(cal.raw)));

    // set the firing time limit
    pd->firingTimeLimit = firingTimeLimit;

    // set the integration time
    pd->integrationTime = integrationTime;

    // set the scan mode
    pd->scanMode = scanMode;

    // set the manual scaling factor
    pd->manualScalingFactor = manualScalingFactor;

    // set the jitter filter data
    pd->jfWindow = jitterFilter.window;
    pd->jfLo = jitterFilter.lo;
    pd->jfHi = jitterFilter.hi;
    pd->jfLastPre = jitterFilter.lastPre;
    pd->jfLastPost = jitterFilter.lastPost;

    // add sensor-specific data
    actualSize += sensor->ReportSensorData(reinterpret_cast<uint8_t*>(pd + 1), maxSize - sizeof(PlungerReading));

    // return the size of the populated data
    return actualSize;
}

// populate the PlungerConfig vendor interface struct
size_t Plunger::Populate(PinscapePico::PlungerConfig *pc, size_t maxSize)
{
    // make sure the buffer is big enough for the struct
    using PlungerConfig = PinscapePico::PlungerConfig;
    if (sizeof(PlungerConfig) > maxSize)
        return 0;

    // clear the struct and set the size
    size_t actualSize = sizeof(PlungerConfig);
    memset(pc, 0, sizeof(PlungerConfig));
    pc->cb = sizeof(PlungerConfig);

    // set the sensor type code
    pc->sensorType = GetSensorTypeForFeedbackReport();

    // set the scale
    pc->nativeScale = nativeScale;

    // set flags
    pc->flags = 0;

    // return the size of the populated data
    return actualSize;
}
