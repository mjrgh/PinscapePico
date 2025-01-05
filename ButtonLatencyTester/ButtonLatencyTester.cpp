// Pinscape Pico - HID Button Latency Tester
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This program is designed to test and characterize the latency of a
// Pinscape Pico device reporting a button press to a pinball simulator 
// via HID input.  Latency is important to pinball simulators for the
// flipper buttons in particular, because a good playing experience
// requires that the simulated flippers respond almost immediately to
// the user physically pressing the flipper buttons.
// 
// How to use:
// 
// - Configure one or more buttons on the Pico as GPIO inputs, with
//   enableLogging set to true on the source:
// 
//   buttons: [
//     { type: "push", 
//       { source: { type: "gpio", gp: 10, enableLogging: true },
//       { action: { type: "openPinDev", button: "left flipper" },
//     },
//   ],
// 
// - Run the program
// 
// - Exercise the button(s) with logging enable.  The console will
//   display the measured latency of each press/release HID event.
//   The measured time represents the full end-to-end latency, from
//   the moment the button switch state changed to the time the
//   Windows application received the corresponding HID event.
//
// 
// Background
// 
// Latency is defined as the amount of elapsed time between the user
// physically pressing a button, and the HID report representing the
// button press event arriving at the simulator program.
// 
// The most accurate way to measure this latency is with external test
// equipment, since that allows an outside observer to detect the
// objective events (the button press and the simulator response) and
// mark them against an outside time clock to determine the elapsed
// time between the events.  However, we can still get a good reading
// on the latency by carefully devising some instrumentation in the
// device and host.  Embedding the measurement instrumentation runs
// the risk that the instrumentation itself affects the readings, but
// we can minimize that confounding effect with a careful design.
// 
// The first thing we need to perform the latency measurement is a
// common time reference point between the sender and receiver -
// essentially a way to synchronize the two clocks.  That lets us
// calculate the latency by subtracting the time that the device
// detects the button press from the time that the receiver reads
// the corresponding HID report.  Without clock synchronization, it
// would be meaningless to compute the difference of the two times.
// Pinscape Pico includes an API (in its USB Vendor Interface) that
// uses a simple NTP-like protocol for clock synchronization, which
// gets the clocks to within about 
// 
// 

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <memory>
#include <list>
#include <unordered_map>
#include <regex>
#include <string>
#include <mutex>
#include <conio.h>
#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "../WinAPI/PinscapeVendorInterface.h"
#include "../WinAPI/PicoClockSync.h"
#include "../OpenPinballDevice/OpenPinballDeviceReport.h"

#pragma comment(lib, "PinscapePicoAPI")


// shorthand some namespace-scoped types for convenience
using VendorIfc = PinscapePico::VendorInterface;
using VendorIfcDesc = PinscapePico::VendorInterfaceDesc;

// forwards
static void ButtonTestMode(std::shared_ptr<PinscapePico::VendorInterface> &dev, PinscapePico::PicoClockSync &clock, bool haveInitialSkew);
static void ClockTestMode(PinscapePico::PicoClockSync &cloc);

// synchronization clock options
const int NAveragingRounds = 100;
const int NFilterRounds = 10;

// convenience class wrapping a system handle (file, event)
struct SysHandleHolder
{
	SysHandleHolder() : h(NULL) { }
	SysHandleHolder(HANDLE h) : h(h) { }
	~SysHandleHolder() { Clear(); }
	
	void Clear()
	{
		if (h != NULL && h != INVALID_HANDLE_VALUE)
		{
			CloseHandle(h);
			h = NULL;
		}
	}

	HANDLE Release()
	{
		HANDLE ret = h;
		h = NULL;
		return ret;
	}

	operator HANDLE() { return h; }
	HANDLE* operator&() { return &h; }

	HANDLE h;
};

// --------------------------------------------------------------------------
//
// Main program entrypoint
//
int main(int argc, char **argv)
{
	// parse arguments
	const char *unitId = nullptr;
	double initialSkew = 0.0;
	bool haveInitialSkew = false;
	bool clockTestMode = false;
	for (int i = 1 ; i < argc ; ++i)
	{
		const char *a = argv[i];
		if (strcmp(a, "--id") == 0)
		{
			// argument required
			if (++i >= argc)
				return printf("Missing unit ID for --id option\n"), 1;

			// set the unit ID
			unitId = argv[i];
		}
		else if (strcmp(a, "--help") == 0 || strcmp(a, "-?") == 0 || strcmp(a, "/?") == 0)
		{
			printf(
				"Usage: ButtonLatencyTester [options]\n"
				"Options:\n"
				"  --id <unit>      Select device by unit number, name, or hardware ID\n"
				"  --skew <skew>    Set the initial clock skew value\n"
				"  --sync-tests     Run clock synchronization statistical tests\n"
			);
			return 1;
		}
		else if (strcmp(a, "--skew") == 0)
		{
			// argument required
			if (++i >= argc)
				return printf("Missing value for --skew option\n"), 1;

			// set the initial skew
			initialSkew = atof(argv[i]);
			haveInitialSkew = true;
		}
		else if (strcmp(a, "--sync-tests") == 0)
		{
			// flag the test mode
			clockTestMode = true;
		}
		else
		{
			// invalid option
			printf("Invalid option \"%s\" (use --help to list options)\n", a);
			return 1;
		}
	}

	// get a list of units, filtering by ID
	std::list<VendorIfcDesc> descs;
	if (HRESULT hr = VendorIfc::EnumerateDevicesByID(descs, unitId); !SUCCEEDED(hr))
		return printf("Error finding Pinscape Pico devices (HRESULT %08lx)\n", hr), 1;

	// make sure we found at least one device
	if (descs.size() == 0)
	{
		if (unitId != nullptr)
			return printf("No Pinscape Pico devices found matching ID \"%s\"\n", unitId), 1;
		else
			return printf("No active Pinscape Pico devices found\n"), 1;
	}

	// if multiple devices were found, show the list
	if (descs.size() > 1)
	{

		printf(unitId == nullptr ?
			"Multiple Pinscape Pico devices were found; use --id to select a device\n"
			"by unit number, unit name, or hardware ID.  Available units are:\n" :
			"Multiple units matched this ID - please specify a unique ID.  Matching units:\n");

		printf("\n"
			"  Unit   Hardware ID        Name\n"
			"  ----   -----------        ----\n");
		for (const auto &desc : descs)
		{
			std::unique_ptr<VendorIfc> dev;
			PinscapePico::DeviceID id;
			if (SUCCEEDED(desc.Open(dev)) && dev->QueryID(id) == PinscapeResponse::OK)
				printf("  %3d    %.16s   %s\n", id.unitNum, id.hwid.ToString().c_str(), id.unitName.c_str());
			else
				printf("  (Error querying ID for device instance ID %ws, path %ws)\n", desc.DeviceInstanceId(), desc.Name());
		}
		return 1;
	}

	// open the device
	std::shared_ptr<VendorIfc> dev;
	if (HRESULT hr = descs.front().Open(dev); !SUCCEEDED(hr))
		return printf("Error opening device (HRESULT %08lx)\n", hr), 1;

	// set up a time synchronizer
	PinscapePico::PicoClockSync picoClock(dev, initialSkew);
	picoClock.Sync(NAveragingRounds, NFilterRounds);

	// run the appropriate mode
	if (clockTestMode)
	{
		ClockTestMode(picoClock);
	}
	else
	{
		// normal button latency testing mode
		ButtonTestMode(dev, picoClock, haveInitialSkew);
	}

	// normal termination
	return 0;
}

// --------------------------------------------------------------------------
//
// Clock synchronization tester main
//
void ClockTestMode(PinscapePico::PicoClockSync &picoClock)
{
	// Introduce the mode
	printf("*** Synchronization Test Mode ***\n"
		"This mode gathers statistics on the Pico/Windows clock synchronization system.\n"
		"Precise clock synchronization is important for latency testing because it's\n"
		"needed to calculate the time between hardware events on the Pico and the USB\n"
		"reports representing those events reaching the host.\n"
		"\n"
		"The test compares live clock synchronization readings to projections based on\n"
		"the Windows clock time, to determine the range of deviation between the results.\n"
		"This provides an estimate of the uncertainty in the Pico clock readings, which\n"
		"carries over to uncertainty in latency measurements.\n\n"
		"Press Q to exit.\n\n");

	// adjust the skew time at increasing intervals
	double adjustSkewTime = 120.0f * 1.0e6;

	// main loop
	printf("\n"
		"*** Main loop ***\n"
		"The main loop gathers statistics indefinitely.  Press Q to quit.\n\n");

	int n = 0;
	int64_t errorSum = 0;
	int64_t errorSqSum = 0;
	for (UINT64 tPrint = GetTickCount64() ; ; )
	{
		// check for keyboard input
		if (_kbhit())
		{
			switch (_getch())
			{
			case 'q':
			case 'Q':
				// stop on Q
				return;
				continue;
			}
		}

		// read the Pico time from the Pico
		uint64_t picoTime;
		int64_t winTime;
		picoClock.ReadPicoTime(NAveragingRounds, NFilterRounds, picoTime, winTime);

		// project the Pico time from the current Windows time
		uint64_t picoTimeProjected = picoClock.ProjectPicoTime(winTime);
		int64_t error = picoTimeProjected - picoTime;

		// figure time since the reset
		double elapsed = picoClock.TicksToMicroseconds(picoClock.GetWindowsTime() - picoClock.GetLastWinSyncTime());

		// update average accumulators
		errorSum += error;
		errorSqSum += error*error;
		n += 1;

		// update statistics display at 5-second intervals
		if (UINT64 now = GetTickCount64() ; now > tPrint + 5000)
		{
			double avgErr = static_cast<double>(errorSum)/n;
			double sdErr = sqrt(static_cast<double>(errorSqSum)/n - avgErr*avgErr);

			printf("Error(n=%d, avg=%.2lf, sd=%.2lf), Pico time read=%llu, projected=%llu, error=%+lld; intervals: win=%d, pico=%d [t0 + %.2lf sec]\n",
				n, avgErr, sdErr,
				picoTime, picoTimeProjected, error,
				picoClock.GetWindowsInterval(), picoClock.GetPicoInterval(),
				elapsed/1000000.0);
			tPrint = now;

			// reset the accumulators every so often
			if (n > 250)
			{
				errorSum = 0;
				errorSqSum = 0;
				n = 0;
			}
		}

		// automatically adjust skew every so often
		if (elapsed > adjustSkewTime)
		{
			// update skew
			picoClock.AdjustSkew(NAveragingRounds, NFilterRounds);
			printf("Recalibrating skew = %.18e\n", picoClock.GetSkew());

			// double the skew interval on each update
			adjustSkewTime *= 2.0;
		}
	}
}

// --------------------------------------------------------------------------
//
// Button tester
//

// internal button descriptor
struct Button
{
	Button(int logicalButtonNum, int gpio, int actionDetail) :
		logicalButtonNum(logicalButtonNum), gpio(gpio), actionDetail(actionDetail) { }

	int logicalButtonNum;		// logical button number
	int gpio;					// GPIO port number
	int actionDetail;			// Button Descriptor action detail (OPD button number, joystick button number, etc)

	bool on = false;			// last on/off state

	// last displayed HID on/off times, for calculating time between events
	struct
	{
		int64_t tOn = 0;
		int64_t tOff = 0;
	} hidDisplayTimes;

	// state test function - set by subclass
	std::function<bool()> test = []() { return false; };

	// Button state tracking.  This keeps track of the point-in-time state 
	// of the button on the Pico side as we process queued events.
	struct PicoState
	{
		// current state
		bool on = false;

		// last ON and OFF transition times, on the Pico clock
		uint64_t tOn = 0;
		uint64_t tOff = 0;

		// Update the state with an event
		void Update(PinscapePico::ButtonEventLogItem &e)
		{
			switch (e.eventType)
			{
			case e.EVENTTYPE_PRESS:
				// update the state
				on = true;

				// Update the time.  If this is less than 500us from the
				// OFF transition, treat the ON-OFF-ON sequence as switch
				// bounce, by leaving the ON time unchanged.
				if (e.t > tOff + 500)
					tOn = e.t;
				break;

			case e.EVENTTYPE_RELEASE:
				// update the state
				on = false;

				// update the time, accounting for switch bounce
				if (e.t > tOn + 500)
					tOff = e.t;
				break;
			}
		}
	};
	PicoState state;
};

// main button tester context
struct ButtonTesterCtx
{
	ButtonTesterCtx(std::shared_ptr<VendorIfc> &dev, PinscapePico::PicoClockSync &clockSync) : dev(dev), clockSync(clockSync)
	{
		// get the device ID
		if (int stat = dev->QueryID(devId); stat != PinscapeResponse::OK)
			printf("Warning: unable to query device ID (error code %d, %s)\n", stat, VendorIfc::ErrorText(stat));

		// enumerate associated HIDs
		if (int stat = dev->EnumerateAssociatedHIDs(hids); stat != PinscapeResponse::OK)
			printf("Warning: unable to query associated HIDs for device (error code %d, %s)\n", stat, VendorIfc::ErrorText(stat));
	}

	// device
	std::shared_ptr<VendorIfc> dev;

	// Pico clock synchronizer
	PinscapePico::PicoClockSync &clockSync;

	// Flag: always resync clocks with the Pico each time we process
	// events.  If this is true, we'll resync the Pico clock during
	// each event processing cycle.  If false, we'll rely on the
	// projected time from the initial sync.  Resyncing on each event
	// is slower, since it requires a series of USB transactions, but
	// it might be somewhat more accurate given that the clocks tend
	// to drift out of sync over time, since they're independent
	// hardware.  The clock skew factor is meant to help with that,
	// but it assumes a constant skew rate, whereas the actual skew
	// is probably pretty variable.
	bool alwaysResyncClocks = true;

	// device ID information
	PinscapePico::DeviceID devId;

	// associated HIDs
	std::list<std::wstring> hids;

	// GPIO pins we're monitoring
	struct GPIOWatch
	{
		// event log
		std::list<PinscapePico::ButtonEventLogItem> picoEventLog;
	};
	std::unordered_map<int, GPIOWatch> gpioMap;

	// add a GPIO to watch
	void AddGPIOWatch(int gpio)
	{
		// only add it if it's not already enrolled
		if (gpioMap.find(gpio) == gpioMap.end())
			gpioMap.emplace(std::piecewise_construct, std::forward_as_tuple(gpio), std::forward_as_tuple());
	}

	// clear all of the Pico event logs
	void ClearPicoEvents()
	{
		// clear the local logs
		for (auto &it : gpioMap)
			it.second.picoEventLog.clear();

		// clear all Pico-side logs (using the special wildcard GPIO 255)
		dev->ClearButtonEventLog(255);
	}

	// stop requested
	bool stopRequested = false;

	// queue a button change event
	void QueueEvent(Button &b, int64_t windowsTimestamp, uint64_t picoTimestamp, int eventType)
	{
		// acquire the mutex
		std::lock_guard lock(mutex);

		// queue the event
		eventQueue.emplace_back(b, windowsTimestamp, picoTimestamp, eventType);
	}

	// process queued events
	void ProcessEvents()
	{
		// update the GPIO button event logs
		auto QueryEventLog = [this]()
		{
			for (auto &it : gpioMap)
			{
				std::vector<PinscapePico::ButtonEventLogItem> l;
				if (dev->QueryButtonEventLog(l, it.first) == PinscapeResponse::OK)
				{
					for (auto &e : l)
						it.second.picoEventLog.push_back(e);
				}
			}
		};
		QueryEventLog();

		// If desired, sync the clock
		if (alwaysResyncClocks)
			clockSync.Sync(1, 5);

		// process the event queue until empty
		mutex.lock();
		while (eventQueue.size() != 0)
		{
			// pull the oldest event
			Event hidEvent = eventQueue.front();
			eventQueue.pop_front();

			// release the mutex while we work on the event
			mutex.unlock();

			// log the HID event
			auto &button = hidEvent.button;
			auto hidTimeOnPico = clockSync.ProjectPicoTime(hidEvent.windowsTime);
			printf("Button %d [GPIO %d] [%s]  HID time %.3lf win -> %.3lf Pico, Pico timestamp %.3lf, last %s + %.3lf ms\n",
				hidEvent.button.logicalButtonNum, hidEvent.button.gpio,
				hidEvent.eventType == 0 ? "Release" : "Press",
				clockSync.TicksToMicroseconds(hidEvent.windowsTime) / 1000.0f, hidTimeOnPico / 1000.0,
				hidEvent.picoTime / 1000.0,
				hidEvent.eventType == 0 ? "press" : "release",
				clockSync.TicksToMicroseconds(hidEvent.windowsTime - (hidEvent.eventType == 0 ? button.hidDisplayTimes.tOn : button.hidDisplayTimes.tOff))/1000.0);

			// update the last displayed time
			(hidEvent.eventType == 0 ? button.hidDisplayTimes.tOff : button.hidDisplayTimes.tOn) = hidEvent.windowsTime;

			// find the oldest matching event in the Pico-side event log
			bool found = false;
			std::list<PinscapePico::ButtonEventLogItem> postItems;
			if (auto it = gpioMap.find(button.gpio); it != gpioMap.end())
			{
				// get the log
				auto &picoLog = it->second.picoEventLog;

				// if it's empty, try re-querying
				if (picoLog.size() == 0)
				{
					Sleep(100);
					QueryEventLog();
				}

				// scan the log for the most recent matching event
				PinscapePico::ButtonEventLogItem matchedEvent{ 0 };
				while (picoLog.size() != 0)
				{
					// get the front element
					auto &picoEvent = picoLog.front();

					// update the button state
					button.state.Update(picoEvent);

					printf(". Pico %s %.3f\n", picoEvent.eventType == picoEvent.EVENTTYPE_PRESS ? "Press" : "Release", picoEvent.t/1000.0);

					// if this is newer than the event we're seeking, stop here
					if (picoEvent.t > hidTimeOnPico)
						break;

					// check for a type match
					bool foundIsCur = false;
					if (picoEvent.eventType == hidEvent.eventType && hidTimeOnPico > picoEvent.t)
					{
						matchedEvent = picoEvent;
						postItems.clear();
						found = foundIsCur = true;
					}

					// keep items after the last found event
					if (found && !foundIsCur)
						postItems.push_front(picoEvent);

					// discard the event 
					picoLog.pop_front();

					// if the log is empty and we haven't found a match, try reloading
					if (picoLog.size() == 0 && !found)
					{
						Sleep(100);
						QueryEventLog();
					}
				}

				// log it if found
				if (found)
				{
					printf("-> IRQ time %.3lf, dt %.3f ms\n",
						matchedEvent.t/1000.0, (hidTimeOnPico - matchedEvent.t)/1000.0);
				}

				// restore the items after the last match back to the list, since
				// they must apply to events still waiting in the HID queue
				for (auto &e : postItems)
					picoLog.push_front(e);
			}

			// report if we didn't find anything
			if (!found)
				printf("-> NO IRQ EVENT MATCH\n");

			// grab the mutex again for the next queue check
			mutex.lock();
		}
		mutex.unlock();
	}

	// event queue
	struct Event
	{
		Event(Button &b, int64_t windowsTime, uint64_t picoTime, int eventType) : 
			button(b), windowsTime(windowsTime), picoTime(picoTime), eventType(eventType) { }
		Button &button;				// button changing state in HID report
		int64_t windowsTime;		// Windows tick time when HID report was received
		uint64_t picoTime;			// Pico timestamp of event, if available
		int eventType;				// event type - 1=press, 0=release
	};
	std::list<Event> eventQueue;

	// mutex protecting the event queue
	std::mutex mutex;
};

class Thread
{
public:
	Thread(const char *name, ButtonTesterCtx *ctx) : name(name), ctx(ctx) { }
	~Thread()
	{
		if (hThread != NULL)
			CloseHandle(hThread);
	}

	bool CreateThread()
	{
		hThread = ::CreateThread(NULL, 0, &Entrypoint, this, 0, &tid);
		if (hThread == NULL)
		{
			printf("Unable to create %s thread\n", name);
			return false;
		}
		return true;
	}

	// main thread routine
	virtual void Main() = 0;

	// static entrypoint
	static DWORD WINAPI Entrypoint(void *pv)
	{
		reinterpret_cast<Thread*>(pv)->Main();
		return 0;
	}

	// thread name, for messages
	const char *name;

	// main button tester context
	ButtonTesterCtx *ctx;

	// thread handle and ID
	HANDLE hThread = NULL;
	DWORD tid = 0;

	// Update button states.  
	// 
	// picoTimestamp is the Pico microsecond clock timestamp when the Pico
	// sent the HID event, if available.  Open Pinball Device reports contain
	// this information; other interfaces generally don't.  Use zero if
	// it's not available.
	void UpdateButtonStates(uint64_t picoTimestamp)
	{
		// note the Windows time of the update
		LARGE_INTEGER ii;
		if (!QueryPerformanceCounter(&ii))
			return;

		// update button states
		for (auto &b : buttons)
		{
			// test for a state change since the last report
			bool newOn = b.test();
			if (newOn != b.on)
			{
				// record the new state and queue the change event
				b.on = newOn;
				ctx->QueueEvent(b, ii.QuadPart, picoTimestamp, newOn ? 1 : 0);
			}
		}
	}

	// add a button
	void AddButton(int logicalButtonNum, int gpio, int actionDetail) {
		buttons.emplace_back(logicalButtonNum, gpio, actionDetail); 
	}

	// list of buttons associated with this input type
	std::list<Button> buttons;
};

// Open Pinball Device HID monitor thread
class OPDThread : public Thread
{
public:
	OPDThread(ButtonTesterCtx *ctx) : Thread("Open Pinball Device monitor", ctx) { }

	// monitor thread
	virtual void Main()
	{
		// find the Open Pinball Device HID for the selected unit
		SysHandleHolder hOPD;
		USHORT inReportLen = 0;
		for (auto &hid : ctx->hids)
		{
			// open the HID
			SysHandleHolder h(CreateFileW(hid.c_str(),
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL));
			if (h != INVALID_HANDLE_VALUE)
			{
				// presume this isn't the one we're looking for
				bool matched = false;

				// get the preparsed data
				PHIDP_PREPARSED_DATA ppd;
				if (HidD_GetPreparsedData(h, &ppd))
				{
					// get the device capabilities
					HIDP_CAPS caps;
					if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
					{
						// Open Pinball Device has usage page 0x05/usage 0x02
						// (Game Controls/Pinball Device).
						if (caps.UsagePage == HID_USAGE_PAGE_GAME && caps.Usage == HID_USAGE_GAME_PINBALL_DEVICE)
						{
							// check for the custom usage string on the first button usage
							HIDP_BUTTON_CAPS btnCaps;
							USHORT btnCapsLen = 1;
							WCHAR usageString[128]{ 0 };
							if (caps.NumberInputButtonCaps == 1
								&& HidP_GetButtonCaps(HidP_Input, &btnCaps, &btnCapsLen, ppd) == HIDP_STATUS_SUCCESS
								&& btnCapsLen == 1
								&& !btnCaps.IsStringRange
								&& btnCaps.NotRange.StringIndex != 0
								&& HidD_GetIndexedString(h, btnCaps.NotRange.StringIndex, usageString, sizeof(usageString))
								&& wcsncmp(usageString, L"OpenPinballDeviceStruct/", 24) == 0)
							{
								// this is the one - capture it and stop searching
								hOPD.h = h.Release();
								inReportLen = caps.InputReportByteLength;
								break;
							}
						}
					}

					// done with the preparsed data
					HidD_FreePreparsedData(ppd);
				}
			}
		}

		// if we didn't match a device, abort
		if (hOPD == NULL)
		{
			printf("Unable to connect to Open Pinball Device HID\n");
			return;
		}

		// set up the button test functions
		OpenPinballDeviceReport report{ 0 };
		bool newState = false;
		for (auto &b : buttons)
		{
			if (b.actionDetail >= 1 && b.actionDetail <= 32)
			{
				// generic button 1-32
				b.test = [&report](uint32_t mask) {
					return [&report, mask] { return report.genericButtons & mask; };
				}(1 << (b.actionDetail - 1));
			}
			else if (b.actionDetail >= 33 && b.actionDetail <= 65)
			{
				// pinball button 1-32
				b.test = [&report](uint32_t mask) {
					return [&report, mask] { return report.pinballButtons & mask; };
				}(1 << (b.actionDetail - 33));
			}
		}

		// set up an event for overlapped read
		SysHandleHolder hEvent(CreateEvent(NULL, TRUE, FALSE, NULL));

		// monitor input
		std::vector<BYTE> buf(inReportLen);
		int64_t tLastEvent = 0;
		while (!ctx->stopRequested)
		{
			// read input
			OVERLAPPED ov{ 0 };
			ov.hEvent = hEvent;
			DWORD nBytesRead = 0;
			if (ReadFile(hOPD, buf.data(), inReportLen, &nBytesRead, &ov)
				|| (GetLastError() == ERROR_IO_PENDING && GetOverlappedResultEx(hOPD, &ov, &nBytesRead, 500, FALSE)))
			{
				// record time statistics
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				if (tLastEvent != 0)
					hidInterval.Collect(ctx->clockSync.TicksToMicroseconds(now.QuadPart - tLastEvent)/1000.0);
				tLastEvent = now.QuadPart;

				// success - decode the report
				memcpy(&report, buf.data() + 1, min(sizeof(report), inReportLen - 1));

				// update buttons
				UpdateButtonStates(report.timestamp);
			}
			else
			{
				// error reading endpoint; pause and retry
				Sleep(100);
			}
		}

		// done (return code not used)
		return;
	}

	// HID event time statistics
	struct HIDInterval
	{
		double iMin = 1000000.0;
		double iMax = 0.0;
		double iAvg = 0.0;
		double iSum = 0.0;
		int64_t cnt = 0;
		int64_t nBuffered = 0;

		void Collect(double dt)
		{
			if (dt < 0.25)
			{
				// this is too short to be a true HID interval, so count it as
				// a "buffered" event that was sitting in the Windows internal
				// event buffer prior to our call
				++nBuffered;
			}
			else if (dt > 25.0)
			{
				// Too long - the program must have been suspended by the C++
				// debugger, or a system sleep, or something else that blocked
				// it for an extended time.  Ignore this event as an outlier
				// that shouldn't count in the statistics.
			}
			else
			{
				// count it as a normal HID event
				if (dt < iMin) iMin = dt;
				if (dt > iMax) iMax = dt;

				// update the running total, and update the average periodically
				iSum += dt;
				if ((++cnt & 255) == 0)
					iAvg = iSum / cnt;
			}
		}
	} hidInterval;
};


// 
// Button tester main
//
void ButtonTestMode(std::shared_ptr<PinscapePico::VendorInterface> &dev, PinscapePico::PicoClockSync &picoClock, bool haveInitialSkew)
{
	auto CheckStat = [](int stat, const char *desc)
	{
		if (stat != PinscapeResponse::OK)
		{
			printf("Error %s: code %d, %s\n", desc, stat, VendorIfc::ErrorText(stat));
			exit(1);
		}
	};

	// get the button configuration
	std::vector<PinscapePico::ButtonDesc> buttonDesc;
	std::vector<PinscapePico::ButtonDevice> buttonDev;
	CheckStat(dev->QueryButtonConfig(buttonDesc, buttonDev), "querying button configuration");

	// thread context
	ButtonTesterCtx ctx(dev, picoClock);

	// threads
	std::list<Thread*> threads;
	std::unique_ptr<OPDThread> opdThread;

	// look for buttons with event logging enabled
	int buttonNum = 0;
	for (auto &bd : buttonDesc)
	{
		// check for a GPIO source with event logging enabled
		if (bd.sourceType == bd.SRC_GPIO && (bd.flags & bd.FLAG_EVENTLOG) != 0)
		{
			printf("Found button %d with event logging enabled on GPIO %d", buttonNum, bd.sourcePort);

			// check the action 
			bool keep = true;
			if (bd.actionType == bd.ACTION_OPENPINDEV)
			{
				// enable OPD monitoring
				printf(", OpenPinDev input %d\n", bd.actionDetail);
				if (opdThread == nullptr)
				{
					opdThread.reset(new OPDThread(&ctx));
					threads.emplace_back(opdThread.get());
				}

				// add this button to the monitor list
				opdThread->AddButton(buttonNum, bd.sourcePort, bd.actionDetail);
			}
			else if (bd.actionType == bd.ACTION_XINPUT)
			{
				// enable XInput monitoring
				printf(", XInput button %d\n", bd.actionDetail);
				// TO DO
			}
			else if (bd.actionType == bd.ACTION_GAMEPAD)
			{
				// enable gamepad monitoring
				printf(", gamepad button %d\n", bd.actionDetail);
				// TO DO
			}
			else if (bd.actionType == bd.ACTION_KEY)
			{
				// enable keyboard monitoring
				printf(", keyboard code %d\n", bd.actionDetail);
				// TO DO
			}
			else
			{
				printf("; ignoring, can't monitor this action type (%d)\n", bd.actionType);
				keep = false;
			}

			// add it to the monitoring list if it's a keeper
			if (keep)
				ctx.AddGPIOWatch(bd.sourcePort);
		}

		++buttonNum;
	}

	// start threads
	for (auto &thread : threads)
	{
		if (!thread->CreateThread())
			return;
	}

	// Clear all prior events on the Pico side, so that we don't
	// try to correlate old Pico events to new HID events.
	ctx.ClearPicoEvents();

	// main loop
	printf("*** Monitoring button latency ***\n"
		"\n"
		"   C   = Clear Pico event log\n"
		"   S   = Synchronize clocks\n"
		"   Q   = Quit\n");
	int64_t nextSkewAdjustTime = picoClock.GetWindowsTime() + picoClock.MicrosecondsToTicks(60.0e6);
	for (bool done = false ; !done ; )
	{
		// pause, so that we don't burn up a ton of CPU time monitoring the keyboard;
		// but make it brief so that we remain responsive
		Sleep(20);

		// adjust skew periodically
		if (!ctx.alwaysResyncClocks && picoClock.GetWindowsTime() > nextSkewAdjustTime)
		{
			nextSkewAdjustTime += picoClock.MicrosecondsToTicks(60.0e6);
			picoClock.AdjustSkew(NAveragingRounds, NFilterRounds);
			printf("Adjusted Pico clock skew to %.18e\n", picoClock.GetSkew());
		}

		// process queued events
		ctx.ProcessEvents();

		// check for user input
		if (_kbhit())
		{
			switch (_getch())
			{
			case 'q':
			case 'Q':
				// exit
				printf("Exiting...\n");
				done = true;
				break;

			case 'c':
			case 'C':
				// clear Pico events
				printf("Clearing Pico GPIO button event history\n");
				ctx.ClearPicoEvents();
				break;

			case 's':
			case 'S':
				// sync clocks
				printf("Synchronizing clocks\n");
				picoClock.Sync(NAveragingRounds, NFilterRounds);
				
				// don't recalculate skew for a while, so that we have a
				// reasonably large denominator (time between samples) for
				// the skew calculation
				nextSkewAdjustTime = picoClock.MicrosecondsToTicks(picoClock.GetWindowsTime() + 60.0e6);
				break;
			}
		}
	}

	// exit threads
	ctx.stopRequested = true;

	// Wait briefly for threads to exit
	for (auto &thread : threads)
	{
		if (WaitForSingleObject(thread->hThread, 1000) != WAIT_OBJECT_0)
			printf("Warning: %s thread isn't responding to exit request\n", thread->name);
	}
}

