// Pinscape Pico - Config Tool Version Number
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include "VersionNumber.h"
#include "../PinscapeVersion.h"

// program version string
const char *gVersionString = PINSCAPE_PICO_VERSION_STRING;

// Get the build timestamp in our canonical YYYYMMDDhhmmMODE format.
// Since the build timestamp is fixed at compile time, this builds
// the string into a static buffer, and returns a pointer to that.
const char *GetBuildTimestamp()
{
	// build the static buffer string if we haven't already
	static char buf[64]{ 0 };
	if (buf[0] == 0)
	{
		// get the build date/time from the compiler
		static const char *date = __DATE__;
		static const char *time = __TIME__;

		// figure the month from the name embedded in __DATE__
		int mon = 1;
		for (const char *p = "JanFebMarAprMayJunJulAugSepOctNovDec"; *p != 0 ; p += 3, ++mon)
		{
			if (memcmp(p, date, 3) == 0)
				break;
		}

		// now parse the __DATE__ and __TIME__ fields into the timestamp string
		sprintf_s(buf, "%04d%02d%02d%02d%02d%c/%s",
			atoi(&date[7]), mon, atoi(&date[4]), atoi(&time[0]), atoi(&time[3]),
			VSN_BUILD_MODE_CODE, VSN_BUILD_CPU);
	}

	// return a pointer to the static buffer
	return buf;
}
