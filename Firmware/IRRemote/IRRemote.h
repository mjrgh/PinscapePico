// Pinscape Pico firmware - IR Remote Control send/receive library - common definitions
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

#pragma once

namespace IRRemote 
{
    // Tolerance range for timing inaccuracies, as a percentage +/- of the 
    // reference time from the protocol specification.  Our measured signal
    // times will never exactly match the specification times, because
    // neither the receiver nor the transmitter have perfect clocks.  And
    // even if we had a perfect clock, we'd still be a little off due to
    // latencies in the detector hardware, the detector's signal processing
    // (demodulation, filtering), and our own latencies in detecting and
    // responding to signals on the input pin.  So we need to add a little
    // wiggle room in interpreting times: if the spec says we should see an 
    // X-microsecond IR-ON state (a "mark"), we can expect it to actually
    // range from X-error to X+error.  This tolerance value sets the error
    // range we'll accept.
    //
    // Typical Arduino IR receiver implementations use 30% tolerances.  I 
    // think we could be quite a bit tighter than that if we wanted to be,
    // since the biggest source of timing inaccuracies on the Arduino systems
    // is probably the Arduino itself - the CPU is fairly slow to start with,
    // and most of the Arduino code I've seen uses polling to detect signal
    // edges, which is inherently high-latency.  We're on a relatively fast
    // CPU, and we use interrupts to detect edges.  In my own measurements
    // of miscellaneous remotes, our readings seem to be within about 3% of 
    // spec, so I think 30% is far looser than we really need to be.
    //
    // The main reason to prefer tighter tolerances is that we want to
    // work with many different transmitters from different vendors, using 
    // different protocols.  Higher tolerances mean more ambiguity in
    // identifying timing signatures.  The tradeoff, of course, is that a
    // lower tolerance means more chance of rejecting borderline signals
    // that we could have interpreted properly.  This will probably have
    // to be fine-tuned over time based on practical experience.  For now,
    // it appears that the super loose Arduino levels work just fine, in
    // that we can still easily identify protocols.
    //const float tolerance = 0.3f;
    const int toleranceShl8 = 77;  // tolerance*256
    
    // Check a reading against a reference value using a specified
    // base value for figuring the tolerance.  This is useful when
    // looking for multiples of a base value, because it pegs the
    // tolerance window to the base value rather than allowing it
    // to grow at each multiple.
    inline bool InRange(int reading, int referenceVal, int baseVal)
    {
        int delta = (baseVal * toleranceShl8) >> 8;
        return reading > referenceVal - delta && reading < referenceVal + delta;
    }

    // Check a reading against a reference value, applying the tolerance
    // range to the reference value.
    inline bool InRange(int reading, int referenceVal) 
    {
        return InRange(reading, referenceVal, referenceVal);
    }
    
    // Test a reading against a reference value to see if it's out of
    // range of the reference value above.  This is equivalent to asking
    // if the value is NOT InRange() AND is higher than the reference
    // value.
    inline bool AboveRange(uint32_t val, uint32_t ref, uint32_t baseVal)
    {
        return val > ref + ((baseVal*(256-toleranceShl8)) >> 8);
    }
    
    // Test a reading against a reference value to see if it's in range
    // or above the range
    inline bool InRangeOrAbove(uint32_t val, uint32_t ref, uint32_t baseVal)
    {
        int delta = (baseVal * toleranceShl8) >> 8;
        return val > ref - delta;
    }
}

