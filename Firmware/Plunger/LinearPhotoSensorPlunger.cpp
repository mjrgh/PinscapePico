// Pinscape Pico - Plunger sensor interface for linear photo sensors (TCD1103, TSL1410R)
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>

#include "Pinscape.h"
#include "Utils.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "Logger.h"
#include "Plunger.h"
#include "Devices/LinearPhotoSensor/TCD1103.h"
#include "Devices/LinearPhotoSensor/TSL1410R.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "LinearPhotoSensorPlunger.h"

// ---------------------------------------------------------------------------
//
// Linear photo sensor plunger common base class
//

LinearPhotoSensorPlunger::LinearPhotoSensorPlunger(uint32_t nPixels) : nPixels(nPixels)
{
}

// JSON configuration common to all linear photo sensor plungers
bool LinearPhotoSensorPlunger::Configure(const JSONParser::Value *val)
{
    // (no generic configuration is currently neeeded, but subclasses
    // should call this anyway in case anything is needed in the future)
    
    // success
    return true;
}

bool LinearPhotoSensorPlunger::ReadRaw(Plunger::RawSample &r)
{
    // if a frame is available, process it
    const uint8_t *buf;
    uint64_t t;
    if (IsReady() && GetRawFrame(buf, t))
    {
        // set the sample time to the frame time
        r.t = t;

        // scan the image for the plunger position
        r.rawPos = ScanFrame(buf, plunger.IsReverseOrientation());

        // save the same to repeat until the next frame is available
        lastSample = r;
        
        // release the frame
        ReleaseFrame();

        // success
        return true;
    }

    // no frame available - return the last reading
    r = lastSample;
    return false;
}

size_t LinearPhotoSensorPlunger::ReportSensorData(uint8_t *buf, size_t maxSize)
{
    // Make sure there's room for the Image Sensor extra data report.  We
    // need space for the base structure plus the pixel data flex array.
    using PR = PinscapePico::PlungerReadingImageSensor;
    uint32_t structSize = sizeof(PR) + nPixels - 1;
    if (structSize > maxSize)
        return 0;

    // get the raw frame; if that fails, we can't return an image
    const uint8_t *pix;
    uint64_t timestamp;
    if (!GetRawFrame(pix, timestamp))
        return false;

    // populate the base report struct
    PR *pr = reinterpret_cast<PR*>(buf);
    pr->cb = structSize;
    pr->sensorType = GetTypeForFeedbackReport();
    pr->timestamp = timestamp;
    pr->nPix = nPixels;

    // populate the pixel array in the report
    memcpy(pr->pix, pix, nPixels);

    // release the lock on the raw frame
    ReleaseFrame();

    // return the populated struct size
    return structSize;
}


// ---------------------------------------------------------------------------
//
// Toshiba TCD1103 interface
//

TCD1103Plunger::TCD1103Plunger() : LinearPhotoSensorPlunger(1546)
{
}

bool TCD1103Plunger::Configure(const JSONParser::Value *val)
{
    // make sure the TCD1103 was configured
    if (tcd1103 == nullptr)
    {
        Log(LOG_ERROR, "Plunger: TCD1103 sensor hardware isn't configured; "
            "add a top-level 'tcd1103:' key to configure it, or check the startup log "
            "for initialization errors if the key is already present\n");
        return false;
    }

    // do the base class configuration
    if (!LinearPhotoSensorPlunger::Configure(val))
        return false;

    // success
    return true;
}

void TCD1103Plunger::SetIntegrationTime(uint32_t us)
{
    tcd1103->SetIntegrationTime(us);
}

uint32_t TCD1103Plunger::GetAvgScanTime()
{
    // get the frame scan time from the sensor
    return tcd1103->GetAvgScanTime();
}

bool TCD1103Plunger::IsReady()
{
    // check if the sensor has a newer image than our last snapshot
    return tcd1103->GetPixTimestamp() > lastSample.t;
}

// get the current frame
bool TCD1103Plunger::GetRawFrame(const uint8_t* &buf, uint64_t &timestamp)
{
    // get the current sensor frame buffer
    tcd1103->GetPix(buf, timestamp);
    return true;
}

// release the current frame
void TCD1103Plunger::ReleaseFrame()
{
    // nothing to do - the TCD1103 sensor interface doesn't require
    // a "release" operation
}

// scan a frame
uint32_t TCD1103Plunger::ScanFrame(const uint8_t *pix, bool reverse)
{
    // Pixels [16..28] are live pixels that are physically masked on the
    // sensor, to provide the host with a reference voltage for complete
    // darkness.  Take the average to get the dark level.
    int darkSum = 0;
    const uint8_t *p = pix + 16;
    for (int i = 16 ; i <= 28 ; ++i, darkSum += *p++);
    int darkRef = darkSum / (28-16+1);

    // Make a histogram of brightnesses in the active region
    int hist[256] = { 0 };
    for (int i = 0 ; i < 1500 ; ++i)
        hist[*p++] += 1;

    // find the brightest exposure level (lowest number), excluding
    // the top two as outliers that might be ADC noise
    int n = 0;
    int brightRef = darkRef;
    for (int i = 0 ; i < 256 ; ++i)
    {
        if (hist[i] != 0 && n++ > 1)
        {
            brightRef = i;
            break;
        }
    }

    // If the spread between brightest and darkest isn't at least 20 ADC
    // units, which corresponds to about 20% of the sensor's dynamic
    // range, the image is either too underexposed to be usable, or the
    // plunger isn't in sight and we can only see the dark background.
    // Trying to match anything in such an image would at best mistake a
    // few noisy pixels for an edge, so we'd get wildly random readings
    // from one frame to the next.  It's better to just fail up front
    // on these images.
    if (darkRef - brightRef < 20)
    {
        // unusable image - skip the scan and return the last reading
        return lastSample.rawPos;
    }

    // search for a bright object that's above 1/2 brightness on
    // this image
    int midRef = (darkRef + brightRef)/2;

    // Figure the scan bounds.  Start at the "tip" end of the plunger,
    // because we should always have some dark background space beyond
    // the end of the plunger.  The tip should be the first reflective
    // object we find working from this end.
    int iStart = 32, iEnd = 1532, di = 1;
    if (reverse)
        iStart = 1531, iEnd = 31, di = -1;

    // Scan until we find a block of bright pixels, which should be the
    // tip of the plunger.  The edge between the bright and dark regions
    // serves as the plunger location.
    p = pix + iStart;
    for (int i = iStart ; i != iEnd ; i += di, p += di)
    {
        // Check for a bright pixel, indicating a reflective object in
        // view of this pixel.  Remember that the voltage level on the
        // ADC is LOWER for brighter pixels with this sensor, so bright
        // pixels read as lower numbers than the dark level.  The
        // dynamic range of the sensor is about 43% of the voltage
        // range, or about 110 units on the 8-bit (256-level) ADC
        // reading.
        if (*p <= midRef)
        {
            // Bright pixel.  Check if at least 3/4 of the next
            // run of pixels are bright, to make sure we've found
            // a bright region rather than a noisy pixel.
            int nBright = 0;
            const int nWindow = 16;
            for (int n = 1, ii = di ; n < nWindow ; ++n, ii += di)
            {
                if (*(p + ii) <= midRef)
                    ++nBright;
            }

            if (nBright > nWindow*3/4)
            {
                // yes - take this as our plunger edge
                return i;
            }
        }
    }

    // no luck - return the last reading
    return lastSample.rawPos;
}


// ---------------------------------------------------------------------------
//
// TAOS TSL14XX plungers
//

TSL14XXPlunger::TSL14XXPlunger(int nPixels) :
    LinearPhotoSensorPlunger(nPixels)
{
}

bool TSL14XXPlunger::Configure(const JSONParser::Value *val)
{
    // Make sure the TSL14xx device was configured, and that it's the
    // matching device.  The difference between the devices is the pixel
    // count, so we can tell if it's the right type by checking for the
    // matching pixel count.
    if (tsl1410r == nullptr || tsl1410r->GetPixCount() != nPixels)
    {
        Log(LOG_ERROR, "Plunger: %s sensor hardware isn't configured; "
            "add a top-level '%s:' key to configure it, or check the startup log "
            "for initialization errors if the key is already present\n",
            VariantName(), SensorConfigKey());
        return false;
    }

    // do the base class configuration
    if (!LinearPhotoSensorPlunger::Configure(val))
        return false;

    // success
    return true;
}

void TSL14XXPlunger::SetIntegrationTime(uint32_t us)
{
    tsl1410r->SetIntegrationTime(us);
}

uint32_t TSL14XXPlunger::GetAvgScanTime()
{
    // get the frame scan time from the sensor
    return tsl1410r->GetAvgScanTime();
}

bool TSL14XXPlunger::IsReady()
{
    // check if the sensor has a newer image than our last snapshot
    return tsl1410r->GetPixTimestamp() > lastSample.t;
}

// get the current frame
bool TSL14XXPlunger::GetRawFrame(const uint8_t* &buf, uint64_t &timestamp)
{
    // get the current sensor frame buffer
    tsl1410r->GetPix(buf, timestamp);
    return true;
}

// release the current frame
void TSL14XXPlunger::ReleaseFrame()
{
    // nothing to do - the hardware sensor interface doesn't require
    // a "release" operation
}

// set the scan mode
void TSL14XXPlunger::SetScanMode(uint8_t mode)
{
    switch (mode)
    {
    case 0:
        scanMethodFunc = &TSL14XXPlunger::ScanFrameSteadySlope;
        break;

    case 1:
        scanMethodFunc = &TSL14XXPlunger::ScanFrameSteepestSlope;
        break;
        
    case 2:
        scanMethodFunc = &TSL14XXPlunger::ScanFrameSpeedGap;
        break;

    default:
        Log(LOG_ERROR, "Plunger: %s: scan mode %d is invalid; using default method\n",
            VariantName(), mode);
        break;
    }
}

// Scan a frame - method 0, monotonic slope detection.  This method
// searches the frame for a sustained bright-to-dark slope, with a
// flat bright region on one side and a flat dark region on the other
// side.
//
// This method is the most tolerant of motion blur, because it doesn't
// depend on the steepness of the slope, only that there's a slope at
// all.
//
// Method 0 is the default.
//
uint32_t TSL14XXPlunger::ScanFrameSteadySlope(const uint8_t *pix, bool reverse)
{
    // Get the levels at each end
    int a = (int(pix[0]) + pix[1] + pix[2] + pix[3] + pix[4])/5;
    int b = (int(pix[nPixels-1]) + pix[nPixels-2] + pix[nPixels-3] + pix[nPixels-4] + pix[nPixels-5])/5;

    // figure the midpoint brightness
    int midpt = ((a + b)/2);

    // Figure the bright and dark thresholds at the quarter points
    int brightThreshold = ((a > b ? a : b) + midpt)/2;
    int darkThreshold = ((a < b ? a : b) + midpt)/2;

    // rolling-average window size
    const int windowShift = 3;
    const int windowSize = (1 << windowShift);  // must be power of two
    const int windowMask = windowSize - 1;

    // Search for the starting point.  The core algorithm searches for
    // the shadow from the bright side, so if the plunger is all the way
    // back, we'd have to scan the entire sensor length if we started at
    // the bright end.  We can save a lot of time by skipping most of
    // the bright section, by doing a binary search for a point where
    // the brightness dips below the bright threshold.
    int leftIdx = 0;
    int rightIdx = nPixels - 1;
    int leftAvg = (pix[leftIdx] + pix[leftIdx+1] + pix[leftIdx+2] + pix[leftIdx+3])/4;
    int rightAvg = (pix[rightIdx] + pix[rightIdx-1] + pix[rightIdx-2] + pix[rightIdx-3])/4;
    for (int i = 0 ; i < 8 ; ++i)
    {
        // find the halfway point in this division
        int centerIdx = (leftIdx + rightIdx)/2;
        int centerAvg = (pix[centerIdx-1] + pix[centerIdx] + pix[centerIdx+1] + pix[centerIdx+2])/4;

        // move the bounds towards the dark region
        if (reverse ? centerAvg < brightThreshold : centerAvg > brightThreshold)
        {
            // center is in same region as left side, so move right
            leftIdx = centerIdx - windowSize;
            leftAvg = (pix[leftIdx] + pix[leftIdx+1] + pix[leftIdx+2] + pix[leftIdx+3])/4;
        }
        else
        {
            // center is in same region as right side, so move left
            rightIdx = centerIdx + windowSize;
            rightAvg = (pix[rightIdx] + pix[rightIdx-1] + pix[rightIdx-2] + pix[rightIdx-3])/4;
        }
    }

    // We sometimes land with the range exactly starting or ending at
    // the transition point, so make sure we have enough runway on either
    // side to detect the steady state and slope we look for in the loop.
    leftIdx = (leftIdx > windowSize) ? leftIdx - windowSize : 0;
    rightIdx = (rightIdx < nPixels - windowSize) ? rightIdx + windowSize : nPixels - 1;

    // Adjust the points for the window sum.  The window is an average
    // over windowSize pixels, but to save work in the loop, we don't
    // divide by the number of samples, so the value we actually work
    // with is (average * N) == (average * windowSize).  So all of our
    // reference points have to be likewise adjusted.
    midpt <<= windowShift;
    darkThreshold <<= windowShift;

    // initialize the rolling-average window, starting at the bright end
    // of the region we narrowed down to with the binary search
    int iPix = reverse ? rightIdx : leftIdx;
    int nScan = (reverse ? iPix - windowSize : nPixels - iPix - windowSize);
    uint8_t window[windowSize];
    unsigned int sum = 0;
    int dIndex = reverse ? -1 : 1;
    for (int i = 0 ; i < windowSize ; ++i, iPix += dIndex)
        sum += (window[i] = pix[iPix]);

    // search for a monotonic falling edge
    int prv = sum;
    int edgeStart = -1;
    int edgeMid = -1;
    int nShadow = 0;
    int edgeFound = -1;
    for (int i = windowSize, wi = 0 ; i < nScan ; ++i, iPix += dIndex)
    {
        // advance the rolling window
        sum -= window[wi];
        sum += (window[wi] = pix[iPix]);

        // advance and wrap the window index
        wi += 1;
        wi &= windowMask;

        // check for a falling edge
        if (sum < prv)
        {
            // dropping - start or continue the falling edge
            if (edgeStart < 0)
                edgeStart = iPix;
        }
        else if (sum > prv)
        {
            // rising - cancel the falling edge
            edgeStart = -1;
        }

        // are we in an edge?
        if (edgeStart >= 0)
        {
            // check for a midpoint crossover, which we'll take as the edge position
            if (prv > midpt && sum <= midpt)
                edgeMid = iPix;

            // if we've reached the dark threshold, count it as a potential match
            if (sum < darkThreshold)
                edgeFound = edgeMid;
        }

        // If we're above the midpoint, cancel any match position.  We must
        // have encountered a dark patch where the brightness dipped briefly
        // but didn't actually cross into the shadow zone.
        if (sum > midpt)
        {
            edgeFound = -1;
            nShadow = 0;
        }

        // if we have a potential match, check if we're still in shadow
        if (edgeFound && sum < darkThreshold)
        {
            // count the dark region
            ++nShadow;

            // if we've seen enough contiguous shadow, declare success
            if (nShadow > 10)
                return edgeFound;
        }

        // remember the previous item
        prv = sum;
    }

    // no edge found
    return lastSample.rawPos;
}

// Scan a frame - "method 1", steepest slope across a fixed-width gap.
// This method uses two rolling average windows, separated by a gap of a
// fixed pixel size.  It scans across the whole pixel file, recording the
// slope of the brightness change across the gap at each point.  The result
// is the point with the steepest slope, which corresponds to the sharpest
// bright/dark edge in the image.
//
// The gap size takes into account the unfocused optics of the TSL14XX
// reference setup, where the sensor is positioned very near to the
// plunger, with a light source shining at the plunger from the other
// side, such that the plunger casts a shadow on the sensor.  The image
// on the sensor captures this unfocused shadow, so the edge appears
// as a slope of brightness across the width of the shadow.  The blur
// width depends on the distance of the light source from the sensor,
// and the size of the light source aperture.  The width of the blurred
// region also increases when the plunger is moving rapidly, since a
// typical plunger assembly can move across a significant number of
// pixels in the course of the 2.5 ms exposure time - it can cover
// about 200 pixels in that time at a normal top speed for the spring
// drive, which will expand the blur by about the same pixel area.
// This makes a fixed-size gap method inaccurate at high speeds,
// thus "method 2", which takes the prior speed into account.
uint32_t TSL14XXPlunger::ScanFrameSteepestSlope(const uint8_t *pix, bool reverse)
{
    // Initialize a pair of rolling-average windows.  This sensor tends
    // to have a bit of per-pixel noise, so if we looked at the slope
    // from one pixel to the next, we'd see a lot of steep edges from
    // the noise alone.  Averaging a few pixels smooths out that
    // high-frequency noise.  We use two windows because we're looking
    // for the edge of the shadow, so we want to know where the average
    // suddenly changes across a small gap.  The standard physical setup
    // with this sensor doesn't use focusing optics, so the shadow is a
    // little fuzzy, crossing a few pixels; the gap is meant to
    // approximate the fuzzy extent of the shadow.
    const int windowSize = 5;
    const int gapSize = 2;
    uint8_t window1[windowSize], window2[windowSize];
    unsigned int sum1 = 0, sum2 = 0;
    int iPix1 = reverse ? nPixels - 1 : 0;
    int dir = reverse ? -1 : 1;
    for (int i = 0 ; i < windowSize ; ++i, iPix1 += dir)
        sum1 += (window1[i] = pix[iPix1]);

    int iGap = iPix1 + dir*gapSize/2;
    int iPix2 = iPix1 + dir*gapSize;
    for (int i = 0 ; i < windowSize ; ++i, iPix2 += dir)
        sum2 += (window2[i] = pix[iPix2]);

    // search for the steepest bright-to-dark gradient
    int steepestSlope = 0;
    int steepestIdx = 0;
    for (int i = windowSize*2 + gapSize, wi = 0 ; i < nPixels ; ++i, iPix1 += dir, iPix2 += dir, iGap += dir)
    {
        // compute the slope at the current gap
        int slope = sum1 - sum2;

        // record the steepest slope
        if (slope > steepestSlope)
        {
            steepestSlope = slope;
            steepestIdx = iGap;
        }

        // move to the next pixel in each window
        sum1 -= window1[wi];
        sum1 += (window1[wi] = pix[iPix1]);
        sum2 -= window2[wi];
        sum2 += (window2[wi] = pix[iPix2]);
        wi = (wi + 1) % windowSize;
    }

    // Reject the reading if the steepest slope is too shallow, which
    // indicates that the contrast is too low to take a reading.  It's
    // better to repeat the last reading in these cases.
    if (steepestSlope < 10*windowSize)
        return lastSample.rawPos;    

    // return the best slope point
    return steepestIdx;
}

// Scan a frame - "method 2", steepest slope across a varying-width gap.
// This is a variation on method 1 that varies the gap according to the
// prior two readings.  We use the difference between the prior readings
// as an estimate of the current speed, to set a gap size that increases
// with increasing speed.  The motion blur spreads out the edge across
// the distance the plunger travels in the 2.5ms exposure, so the
// compensates by increasing the gap accordingly.
uint32_t TSL14XXPlunger::ScanFrameSpeedGap(const uint8_t *pix, bool reverse)
{
    // Initialize a pair of rolling-average windows.  This sensor tends
    // to have a bit of per-pixel noise, so if we looked at the slope
    // from one pixel to the next, we'd see a lot of steep edges from
    // the noise alone.  Averaging a few pixels smooths out that
    // high-frequency noise.  We use two windows because we're looking
    // for the edge of the shadow, so we want to know where the average
    // suddenly changes across a small gap.  The standard physical setup
    // with this sensor doesn't use focusing optics, so the shadow is a
    // little fuzzy, crossing a few pixels; the gap is meant to
    // approximate the fuzzy extent of the shadow.
    const int windowSize = 5;
    const int prvDelta = abs(prvRawResult0 - prvRawResult1);
    const int gapSize = prvDelta < 2 ? 2 : prvDelta > 175 ? 175 : prvDelta;
    uint8_t window1[windowSize], window2[windowSize];
    unsigned int sum1 = 0, sum2 = 0;
    int iPix1 = reverse ? nPixels - 1 : 0;
    int dir = reverse ? -1 : 1;
    for (int i = 0 ; i < windowSize ; ++i, iPix1 += dir)
        sum1 += (window1[i] = pix[iPix1]);

    int iGap = iPix1 + dir*gapSize/2;
    int iPix2 = iPix1 + dir*gapSize;
    for (int i = 0 ; i < windowSize ; ++i, iPix2 += dir)
        sum2 += (window2[i] = pix[iPix2]);

    // search for the steepest bright-to-dark gradient
    int steepestSlope = 0;
    int steepestIdx = 0;
    for (int i = windowSize*2 + gapSize, wi = 0 ; i < nPixels ; ++i, iPix1 += dir, iPix2 += dir, iGap += dir)
    {
        // compute the slope at the current gap
        int slope = sum1 - sum2;

        // record the steepest slope
        if (slope > steepestSlope)
        {
            steepestSlope = slope;
            steepestIdx = iGap;
        }

        // move to the next pixel in each window
        sum1 -= window1[wi];
        sum1 += (window1[wi] = pix[iPix1]);
        sum2 -= window2[wi];
        sum2 += (window2[wi] = pix[iPix2]);
        wi = (wi + 1) % windowSize;
    }

    // Reject the reading if the steepest slope is too shallow, which
    // indicates that the contrast is too low to take a reading.  It's
    // better to repeat the last reading in these cases.
    if (steepestSlope < 10*windowSize)
        return lastSample.rawPos;    

    // return the best slope point, rotating it into the speed history
    prvRawResult1 = prvRawResult0;
    prvRawResult0 = steepestIdx;
    return steepestIdx;
}
