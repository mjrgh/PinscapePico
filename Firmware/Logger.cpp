// Pinscape Pico - debug message logging
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <alloca.h>
#include <string.h>
#include <ctype.h>

// Pico SDK headers
#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <hardware/watchdog.h>
#include <tusb.h>

// project headers
#include "Pinscape.h"
#include "Utils.h"
#include "Logger.h"
#include "JSON.h"
#include "USBIfc.h"
#include "USBCDC.h"
#include "TimeOfDay.h"
#include "Watchdog.h"
#include "GPIOManager.h"
#include "CommandConsole.h"
#include "Version.h"

// global logger singletons
Logger logger;
Logger::VendorInterfaceLogger vendorInterfaceLogger;
Logger::UARTLogger uartLogger;
Logger::USBCDCLogger usbCdcLogger;

// Log message type code names for configuration input and display output
static struct {
    const char *configName = "";   // name for the logger.filter key in the config
    const char *dispName = "";     // display name
    const char *typeColor = "";    // ANSI color code string for the type string
    const char *textColor = "";    // ANSI type code for the main message text
} logTypeName[32];

// Prior session log buffer.  This is a static buffer in uninitialized storage
// space, so that it's preserved across CPU resets, allowing us to recover the
// tail end of the last session's log.  This can be helpful when diagnosing
// crashes and unexpected watchdog resets, by letting us see what the software
// was doing just before the crash.
Logger::LogTail __uninitialized_ram(Logger::logTail);

// Main logger
int Log(int type, const char *f, ...)
{
    // pass the varargs through to the va_list function
    va_list va;
    va_start(va, f);
    int ret = LogV(type, f, va);
    va_end(va);

    // return the length result
    return ret;
}

int LogV(int type, const char *f, va_list va)
{
    // there's nothing to do if the string is null or empty
    if (f == nullptr || f[0] == 0)
        return 0;

    // Check if the message type is enabled
    if (!logger.CheckFilter(type))
        return 0;

    // Measure the size of the formatted string
    va_list vaCount;
    va_copy(vaCount, va);
    int len = vsnprintf(nullptr, 0, f, vaCount);
    va_end(vaCount);

    // Format the string into space allocated for it on the stack
    int result = 0;
    if (len > 0)
    {
        // allocate space on the stack for the text plus a null terminator
        char *buf = static_cast<char*>(alloca(len + 1));
        if (buf != nullptr)
        {
            // format the text and add it to the ring buffer
            result = vsnprintf(buf, len + 1, f, va);
            logger.Puts(type, buf);
        }
    }
    else
    {
        // unable to format it - just write out the raw format string, so
        // that we have *something* to show in the log
        logger.Puts(type, f);
    }

    // return the length result from vsnprintf
    return result;
}

// Replacement for Pico SDK panic() - writes log, then executes a breakpoint
// instruction to trigger a hard fault, to capture the stack.
extern "C" void PinscapePanic(const char *msg, ...)
{
    // log the message
    Log(LOG_ERROR, "SDK Panic: ");
    va_list va;
    va_start(va, msg);
    LogV(LOG_ERROR, msg, va);
    va_end(va);

    // trigger a hard fault
    __asm volatile ("bkpt #0");
}

Logger::Logger()
{
    // set the initial buffer size
    buf.resize(bufSize);

    // Set up type info:        Config Var, Disp Name,  Type Prefix Color,    Text Color
    logTypeName[LOG_DEBUG]   = { "debug",   "Debug",    "\033[35;1m",         "\033[35;1m" }; /* bright magenta, ditto */
    logTypeName[LOG_DEBUGEX] = { "debugex", "Debug",    "\033[35;1m",         "\033[35;1m" }; /* bright magenta, ditto */
    logTypeName[LOG_ERROR]   = { "error",   "Error",    "\033[41m\033[37;1m", "\033[31;1m" }; /* white on red, bright red */ 
    logTypeName[LOG_WARNING] = { "warning", "Warning",  "\033[43;1m\033[30m", "\033[33;1m" }; /* black on bright yellow, bright yellow */ 
    logTypeName[LOG_INFO]    = { "info",    "Info",     "\033[32;1m",         "\033[32;1m" }; /* bright green, ditto */
    logTypeName[LOG_VENDOR]  = { "vendor",  "Vendor",   "",                   "" };
    logTypeName[LOG_XINPUT]  = { "xinput",  "XInput",   "",                   "" };
    logTypeName[LOG_CONFIG]  = { "config",  "Config",   "\033[36m",           "\033[36m"   }; /* cyan, ditto */
    logTypeName[LOG_TINYUSB] = { "tinyusb", "Tinyusb",  "",                   "" };

    // populate the device list
    devices.emplace_back(&vendorInterfaceLogger);
    devices.emplace_back(&uartLogger);
    devices.emplace_back(&usbCdcLogger);
}

void Logger::Init()
{
    // if the cross-reset log preservation buffer has valid data, make a snapshot
    if (logTail.IsValid())
    {
        // figure the copy length: the lesser of the actual data length available,
        // or the size of the snapshot buffer
        priorSessionLog.nBytes = std::min(logTail.nBytes, static_cast<int>(sizeof(priorSessionLog.buf)));

        // copy from the write position backwards
        for (int src = logTail.write, dst = priorSessionLog.nBytes, i = 0 ; i < priorSessionLog.nBytes ; ++i)
        {
            if (--src < 0) src = sizeof(logTail.buf) - 1;
            priorSessionLog.buf[--dst] = logTail.buf[src];
        }
    }

    // clear the log tail
    logTail.Clear();
}

void Logger::SetDisplayModes(bool timestamps, bool typeCodes, bool colors)
{
    this->showTimestamps = timestamps;
    this->showTypeCodes = typeCodes;
    this->showColors = colors;
}

// check a message type against the filter
bool Logger::CheckFilter(int type)
{
    // The mask is a bit vector, indexed by type code
    return (mask & (1UL << type)) != 0;
}

// common configuration
void Logger::Configure(JSONParser &json)
{
    // set defaults - enable timestamps and message type prefixes, disable colors
    showTimestamps = true;
    showTypeCodes = true;
    showColors = false;

    // Set the buffer size first, so that we have a place to capture
    // error messages while we're parsing the configuration.  The size
    // limits are arbitrary; the minimum ensures that we don't shrink
    // the buffer from the initial default size, which would complicate
    // dealing with the resize, and the maximum is just a sanity check
    // to prevent the user from accidentally using up all available RAM
    // and crashing the Pico during initialization.
    if (auto *sizeVal = json.Get("logging.bufSize") ; !sizeVal->IsUndefined())
    {
        // set the size
        int newSize = sizeVal->Int();
        SetBufferSize(newSize);

        // log an error if the requested size was forced into range
        if (bufSize != newSize)
            Log(LOG_ERROR, "logging.bufSize is invalid; must be %d to %d\n", MinBufSize, MaxBufSize);
    }

    // get the logging key
    if (auto *val = json.Get("logging") ; !val->IsUndefined())
    {
        // get the message type filter
        if (auto *maskVal = val->Get("filter"); !maskVal->IsUndefined())
        {
            // get the string, and convert to lower-case for case insensitivity
            std::string s = maskVal->String();
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);

            // set the filters
            SetFilters(s.c_str());
        }

        // set the display format
        showTimestamps = val->Get("timestamps")->Bool(true);
        showTypeCodes = val->Get("typeCodes")->Bool(true);
        showColors = val->Get("colors")->Bool(false);
    }

    // install our console command
    InstallConsoleCommand();
}

// Install the console command
void Logger::InstallConsoleCommand()
{
    CommandConsole::AddCommand(
        "logger", "set and view logger options",
        "logger [options]\n"
        "options:\n"
        "  -s, --status        show settings and status\n"
        "  --log <text>        log <txt> as an Info message (use quotes if <text> contains spaces)\n"
        "  -l <text>           same as --log\n"
        "  -f <filters>        enable/disable filters; use +name to enable, -name to disable;\n"
        "                      use commas to set multiple filters; list filters with --status\n"
        "  --filter <filters>  same as -f\n"
        "  --colors            enable colors (ANSI color escape codes) in log messages\n"
        "  --timestamps        enable timestamps\n"
        "  --typecodes         enable type codes\n"
        "  --no-color          disable colors\n"
        "  --no-timestamps     disable timestamps\n"
        "  --no-typecodes      disable type codes\n",
        Command_loggerS);
}

// Set the buffer size
void Logger::SetBufferSize(int newSize)
{
    // limit the new size to the current size at minimum, and an arbitrary maxmimum
    if (newSize < bufSize)
        newSize = bufSize;
    else if (newSize > MaxBufSize)
        newSize = MaxBufSize;

    // resize the buffer if necessary (note that we only allow it
    // to expand from the initial default, enforced above)
    if (buf.size() != newSize)
    {
        // remember the old buffer size
        size_t oldSize = buf.size();

        // expand the buffer (we don't allow it to shrink)
        bufSize = newSize;
        buf.resize(bufSize);

        // Move everything after the write pointer to the end of the
        // expanded buffer, so that the new space is effectively
        // inserted at the gap between the current write pointer and
        // the current read pointer(s).
        size_t writeToEnd = oldSize - write;
        memmove(buf.data() + bufSize - writeToEnd, buf.data() + write, writeToEnd);

        // adjust any read pointers that are in the moved region
        int expandedBy = static_cast<int>(buf.size() - oldSize);
        for (auto &d : devices)
            d->OnExpandBuf(expandedBy);
    }
}

// Set filters.  The mask string must be in lower-case.
void Logger::SetFilters(const char *p)
{
    // start with an empty mask
    uint32_t newMask = 0;
    
    // skip spaces
    for ( ; isspace(*p) ; ++p) ;
    
    // If the first character is '~', it's an inverted mask.  If
    // it's '*', it's a simple "everything" mask.
    bool invert = false;
    if (*p == '~')
    {
        // note that it's an inverted mask
        invert = true;
        ++p;
    }
    else if (*p == '*')
    {
        // it's an "everything" mask - ignore the rest
        newMask = ~0;
        p += strlen(p);
    }
    
    // parse elements
    while (*p != 0)
    {
        // skip spaces
        for ( ; isspace(*p) ; ++p) ;
        
        // scan the token
        const char *tok = p;
        for ( ; *p != 0 && !isspace(*p) ; ++p) ;
        
        // find the name
        if (p != tok)
        {
            // search for a match
            int len = p - tok;
            auto *t = &logTypeName[0];
            bool found = false;
            for (int i = 0, bit = 1 ; i < _countof(logTypeName) ; ++i, ++t, bit <<= 1)
            {
                if (strlen(t->configName) == len && memcmp(t->configName, tok, len) == 0)
                {
                    // it's a match
                    newMask |= bit;
                    found = true;
                    break;
                }
            }
            
            // complain if not found
            if (!found)
                Log(LOG_ERROR, "logging.filter: unrecognized type code \"%.*s\"\n", len, tok);
        }
    }
    
    // save the new mask
    mask = (invert ? ~newMask : newMask);
}

// Periodic logging tasks
void Logger::Task()
{
    // run tasks on the devices that have periodic work
    uartLogger.Task();
    usbCdcLogger.Task();
}

// Put a string to the buffer; translates '\n' to CR-LF
void Logger::Puts(int typeCode, const char *s)
{
    // ignore empty strings
    if (*s == 0)
        return;
    
    // buffer the string one character at a time, with newline
    // translation and line header insertion
    while (*s != 0)
    {
        // if we're at column 0, add a line prefix
        if (col == 0)
        {
            // reset colors if we're using them
            if (showColors)
            {
                // reset and switch to text color
                for (const char *p = "\033[0m" ; *p != 0 ; Put(*p++)) ;
                for (const char *p = logTypeName[typeCode].textColor ; *p != 0 ; Put(*p++)) ;
            }

            // add the timestamp if desired
            if (showTimestamps)
            {
                // Format the current wall clock time.  Note that if the wall
                // clock time isn't known, the boot time is treated as 1/1/0000,
                // (which is actually year 1 BC, proleptic Gregorian calendar,
                // but has year value 0 in our internal numbering system). We'll
                // just format it like that, because it's easily recognizable as
                // a placeholder meaning that we don't know the real wall-clock
                // time.  So no special cases are required for known vs unknown
                // clock time.
                DateTime dt;
                timeOfDay.Get(dt);
                char buf[32];
                sprintf(buf, "%04d/%02d/%02d %02d:%02d:%02d ",
                        dt.yyyy, dt.mon, dt.dd, dt.hh, dt.mm, dt.ss);
                for (const char *p = buf ; *p != 0 ; Put(*p++), ++col) ;
            }

                // add the type code prefix if desired
            if (showTypeCodes)
            {
                // set type color
                if (showColors)
                {
                    for (const char *p = "\033[0m" ; *p != 0 ; Put(*p++)) ;
                    for (const char *p = logTypeName[typeCode].typeColor ; *p != 0 ; Put(*p++)) ;
                }

                // write the type code string
                int i = 0;
                for (const char *p = logTypeName[typeCode].dispName ; *p != 0 ; Put(*p++), ++i, ++col) ;
                Put(':'), ++i, ++col;

                // reset to text color
                if (showColors)
                {
                    for (const char *p = "\033[0m" ; *p != 0 ; Put(*p++)) ;
                    for (const char *p = logTypeName[typeCode].textColor ; *p != 0 ; Put(*p++)) ;
                }

                // pad the type code out to a fixed width, so that the message text
                // aligns on the same column in every message, for easier reading
                for ( ; i < 9 ; Put(' '), ++i, ++col) ;
            }
        }

        // translate newlines
        char c = *s++;
        if (c == '\n')
        {
            // write the CR-LF sequence
            Put('\r');
            Put('\n');

            // reset to column zero
            col = 0;
        }
        else
        {
            // write the character
            Put(c);

            // bump the column
            ++col;
        }
    }
}

// Put a character to the buffer
void Logger::Put(char c)
{
    // add the character and bump the write pointer
    buf[PostInc(write)] = c;

    // add it to the preserved-across-reset log tail buffer
    logTail.Add(c);

    // update the reader views - they might need to discard the oldest
    // character if the buffer is now full from their perspective
    for (auto d : devices)
        d->OnPut();
}

void Logger::LogPriorSessionLog(int type)
{
    auto &l = priorSessionLog;
    if (l.nBytes != 0)
    {
        // announce the start of the section
        Log(type, "----- Tail end of prior session log follows -----\n");

        // log the data
        for (int lineStart = 0 ; lineStart < l.nBytes ; )
        {
            // find the end of this line
            int lineEnd = lineStart;
            for ( ; lineEnd < l.nBytes && l.buf[lineEnd] != '\n' && l.buf[lineEnd] != '\r' ; ++lineEnd) ;

            // log this line
            Log(type, "%.*s\n", static_cast<int>(lineEnd - lineStart), &l.buf[lineStart]);

            // advance past the newline sequence
            if (lineEnd + 1 < l.nBytes && l.buf[lineEnd] == '\r' && l.buf[lineEnd+1] == '\n')
                lineEnd += 2;
            else if (lineEnd < l.nBytes)
                lineEnd += 1;

            // move the line start to the new line
            lineStart = lineEnd;
        }

        // announce the end of the section
        Log(type, "----- End of prior session log -----\n");
    }
}

int Logger::PrintPriorSessionLogLine(CommandConsole *console, int lineStart)
{
    // ignore if offset is out of range
    auto &l = priorSessionLog;
    if (lineStart < 0 || lineStart >= sizeof(l.buf) || lineStart > l.nBytes)
        return -1;
    
    // find the bounds of the next line
    int lineEnd = lineStart;
    for ( ; lineEnd < l.nBytes && l.buf[lineEnd] != '\n' ; ++lineEnd) ;

    // print the line
    console->Print("%.*s\n", static_cast<int>(lineEnd - lineStart), &l.buf[lineStart]);

    // advance past the newline sequence
    if (lineEnd + 1 < l.nBytes && l.buf[lineEnd] == '\r' && l.buf[lineEnd] == '\n')
        lineEnd += 2;
    else if (lineEnd < l.nBytes)
        lineEnd += 1;

    // return the offset of the next line, or -1 if at EOF
    return lineEnd < l.nBytes ? lineEnd : -1;
}


// --------------------------------------------------------------------------
//
// Logger control console command
//
void Logger::Command_logger(const ConsoleCommandContext *c)
{
    if (c->argc <= 1)
        return c->Usage();

    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-s") == 0 || strcmp(a, "--status") == 0)
        {
            c->Printf(
                "Logger status:\n"
                "  Buffer size: %u bytes\n"
                "  Colors:      %s\n"
                "  Type codes:  %s\n"
                "  Timestamps:  %s\n",
                bufSize,
                showColors ? "Enabled" : "Disabled",
                showTypeCodes ? "Enabled" : "Disabled",
                showTimestamps ? "Enabled" : "Disabled");

            c->Printf("  Filters:     ");
            const auto *t = &logTypeName[0];
            bool found = false;
            for (int ti = 0 ; ti < _countof(logTypeName) ; ++ti, ++t)
            {
                if (t->configName[0] != 0)
                    c->Printf("%c%s ", (mask & (1UL << ti)) != 0 ? '+' : '-', t->configName);
            }
            c->Printf("\n");

            uartLogger.PrintStatus(c);
            usbCdcLogger.PrintStatus(c);
            vendorInterfaceLogger.PrintStatus(c);
        }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--log") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing argument for \"%s\"\n", a);

            Log(LOG_INFO, "%s\n", c->argv[i]);
        }
        else if (strcmp(a, "-f") == 0 || strcmp(a, "--filter") == 0)
        {
            if (++i >= c->argc)
                return c->Printf("Missing argument for \"%s\"\n", a);

            for (const char *p = c->argv[i] ; *p != 0 ; )
            {
                bool enable = *p == '+';
                if (*p != '+' && *p != '-')
                    return c->Printf("logger %s: Expected \"+filter_name\" or \"-filter_name\", found \"%s\"\n", a, p);

                const char *start = ++p;
                for ( ; *p != ',' && *p != 0 ; ++p);
                size_t len = p - start;

                const auto *t = &logTypeName[0];
                bool found = false;
                for (int ti = 0 ; ti < _countof(logTypeName) ; ++ti, ++t)
                {
                    if (t->configName[0] != 0
                        && strlen(t->configName) == len
                        && strncmp(t->configName, start, len) == 0)
                    {
                        uint32_t maskBit = 1UL << ti;
                        if (enable)
                            mask |= maskBit;
                        else
                            mask &= ~maskBit;

                        c->Printf("Logger: %s filter %sabled\n", t->configName, enable ? "en" : "dis");
                        found = true;
                        break;
                    }
                }

                if (!found)
                    return c->Printf("logger %s: Invalid filter name \"%.*s\"\n", a, static_cast<int>(len), start);

                if (*p == ',')
                    ++p;
            }
        }
        else if (strcmp(a, "--colors") == 0)
        {
            showColors = true;
            c->Printf("Logger: colors enabled\n");
        }
        else if (strcmp(a, "--no-colors") == 0)
        {
            showColors = false;
            c->Printf("Logger: colors disabled\n");
        }
        else if (strcmp(a, "--timestamps") == 0)
        {
            showTimestamps = true;
            c->Printf("Logger: timestamps enabled\n");
        }
        else if (strcmp(a, "--no-timestamps") == 0)
        {
            showTimestamps = false;
            c->Printf("Logger: timestamps disabled\n");
        }
        else if (strcmp(a, "--typecodes") == 0)
        {
            showTypeCodes = true;
            c->Printf("Logger: message type code display enabled\n");
        }
        else if (strcmp(a, "--no-typecodes") == 0)
        {
            showTypeCodes = false;
            c->Printf("Logger: message type code display disabled\n");
        }
        else
        {
            return c->Printf("logger: invalid option \"%s\"\n", a);
        }
    }
}

// --------------------------------------------------------------------------
//
// Logger device base class
//

// Periodic task.  Writes buffered characters to the underlying physical
// output device whenever the device is ready.  Stop when the device is
// unable to accept more output without blocking.
void Logger::Device::Task()
{
    if (loggingEnabled && read != logger.write)
    {
        // Limit our time here, so that we don't block the main loop
        // for too long if we just had a big burst of log output.  If we
        // run out of time, we'll pick up where we left off next time.
        uint64_t timeout = time_us_64() + 250;
        while (time_us_64() < timeout && read != logger.write && IsReady())
            Write(Get());
    }
}

// figure available characters in our view of the base ring buffer 
int Logger::Device::Available() const
{
    return logger.write >= read ? logger.write - read : logger.write + static_cast<int>(logger.buf.size()) - read;
}

// get the next character from our view of the logger's ring buffer
char Logger::Device::Get()
{
    return (read != logger.write) ? logger.buf[logger.PostInc(read)] : 0;
}

// get a block of characters from the ring buffer
int Logger::Device::Get(char *dst, size_t n)
{
    // limit the read to the size available
    size_t avail = Available();
    if (n > avail)
        n = avail;

    // copy in segments
    if (logger.write >= read)
    {
        // the entire active text is contiguous from 'read' to 'write'
        memcpy(dst, logger.buf.data() + logger.PostInc(read, n), n);

        // return the size read
        return static_cast<int>(n);
    }
    else
    {
        // the active text is in two pieces, wrapped at the end of the
        // buffer; copy the first segment, from 'read' to the end of
        // the buffer
        size_t n1 = logger.buf.size() - read;
        if (n1 > n)
            n1 = n;

        // copy this section
        memcpy(dst, logger.buf.data() + logger.PostInc(read, n1), n1);
        dst += n1;

        // if there's more, we've now wrapped, so the rest is contiguous
        // from the start of the buffer
        size_t n2 = n - n1;
        if (n2 > 0)
            memcpy(dst, logger.buf.data() + logger.PostInc(read, n2), n2);

        // return the total size read
        return static_cast<int>(n1 + n2);
    }
}

void Logger::Device::PrintStatus(const ConsoleCommandContext *c)
{
    c->Printf(
        "%s logger:\n"
        "  Enabled:     %s\n"
        "  Dev Ready:   %s\n"
        "  Pending txt: %u bytes (read=%u, write=%u)\n",
        Name(),
        loggingEnabled ? "Yes" : "No",
        IsReady() ? "Yes" : "No",
        read <= logger.write ? logger.write - read : logger.write + logger.bufSize - read,
        read, logger.write);
}

// --------------------------------------------------------------------------
//
// Vendor Interface logger
//

Logger::VendorInterfaceLogger::VendorInterfaceLogger()
{
    loggingEnabled = true;
}

void Logger::VendorInterfaceLogger::Configure(JSONParser &json)
{
}

// --------------------------------------------------------------------------
//
// UART logger
//

Logger::UARTLogger::UARTLogger()
{
}

void Logger::UARTLogger::Configure(JSONParser &json)
{
    if (auto *uc = json.Get("serialPorts.uart"); !uc->IsUndefined())
    {
        // The config file has to specify the GPIO pins to use for the TX and RX
        // connections.  We can identify the UART unit (UART0 or UART1) from the
        // GPIO assignments, since any given pin can connect to only one or the
        // other UART, and then only in a fixed capacity as TX or RX.  So make
        // a table of UARTn/TX/RX associations, and scan for the specified TX
        // and RX pins.  Note that only about half of the pins are available in
        // a UART TX/RX capacity (the other half are assigned as CTS/RTS, which
        // we don't use).
        //
        // The RX pin is optional, since the logger doesn't need to receive
        // input from the host.
        int tx = uc->Get("tx")->Int(-1);
        int rx = uc->Get("rx")->Int(-1);

        // enable logging/console?
        loggingEnabled = uc->Get("logging")->Bool(true);
        bool consoleEnabled = console.Configure("UART", uc->Get("console"));

        // validate the pins
        if (!IsValidGP(tx))
        {
            Log(LOG_ERROR, "UART serial port: tx GPIO invalid or undefined\n");
            return;
        }
        if (rx >= 0 && !IsValidGP(rx))
        {
            Log(LOG_ERROR, "UART serial port: rx GPIO invalid\n");
            return;
        }
        if (rx < 0 && consoleEnabled)
            Log(LOG_WARNING, "UART serial port: console enabled, but won't function, because no RX GPIO pin ('rx' property) is configured\n");

        // Infer the UART unit number from the pins
        static const struct {
            int id;
            int tx;
            int rx;
        } uartMap[] = {
            { 0, 0, 1 },   // UART0 on gp0, gp1
            { 1, 4, 5 },   // UART1 on gp4, gp5
            { 1, 8, 9 },   // UART1 on gp8, gp9
            { 0, 12, 13 }, // UART0 on gp12, gp13
            { 0, 16, 17 }, // UART0 on gp16, gp17
            { 1, 20, 21 }, // UART1 on gp20, gp21
            { 1, 24, 25 }, // UART1 on gp24, gp25
            { 0, 28, 29 }, // UART0 on gp28, gp29
        };
        int utx =-1, urx = -1;
        for (size_t i = 0 ; i < _countof(uartMap) ; ++i)
        {
            if (uartMap[i].tx == tx)
                utx = uartMap[i].id;
            if (uartMap[i].rx == rx)
                urx = uartMap[i].id;
        }
        if (utx < 0)
        {
            Log(LOG_ERROR, "UART serial port: tx pin isn't mappable to any UART TX; see Pico pinout diagram\n");
            return;
        }
        if (urx >= 0 && urx != utx)
        {
            Log(LOG_ERROR, "UART serial port: tx/rx pins must map to the same UARTn unit; see Pico pinout diagram\n");
            return;
        }

        // claim the pins
        if (!gpioManager.Claim(Format("UART%d TX", utx), tx)
            || !gpioManager.Claim(Format("UART%d RX", urx), rx))
            return;

        // The pin assignments are valid.  Initialize the UART.
        int baud = uc->Get("baud")->Int(115200);
        Init(utx == 0 ? uart0 : uart1, utx, urx, baud);
    }
}

void Logger::UARTLogger::Init(uart_inst_t *uart, int gpTx, int gpRx, int baud)
{
    // set the UART unit
    this->uart = uart;

    // initialize the UART device
    uart_init(uart, baud);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);

    // assign the TX GPIO port
    gpio_set_function(gpTx, GPIO_FUNC_UART);

    // assign the optional RX GPIO port
    if (gpRx >= 0)
        gpio_set_function(gpRx, GPIO_FUNC_UART);

    // allocate a DMA channel for transmitting, if possible
    dmaChannel = dma_claim_unused_channel(false);
    if (dmaChannel >= 0)
    {
        // configure the channel to read from our DMA buffer and write
        // to the UART FIFO, pacing via the UART DREQ
        dma_channel_config cfg = dma_channel_get_default_config(dmaChannel);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
        channel_config_set_dreq(&cfg, uart_get_dreq(uart, true));
        channel_config_set_enable(&cfg, true);
        dma_channel_configure(dmaChannel, &cfg, &uart_get_hw(uart)->dr, dmaBuf, DMA_BUF_SIZE, false);

        // log it
        Log(LOG_CONFIG, "UART serial port TX configured on DMA channel %d\n", dmaChannel);
    }
}

bool Logger::UARTLogger::IsReady()
{
    // if using DMA, we're ready if the DMA channel isn't busy; otherwise we're ready
    // if the UART FIFO is writable without blocking
    return dmaChannel >= 0 ? !dma_channel_is_busy(dmaChannel) : uart_is_writable(uart);
}

void Logger::UARTLogger::Write(char c)
{
    uart_putc_raw(uart, c);
}

// flush the UART, for debugging purposes only
void Logger::UARTLogger::FlushForDebugging()
{
    while (loggingEnabled && read != logger.write)
    {
        watchdog_update();
        Task();
    }
}

void Logger::UARTLogger::PrintStatus(const ConsoleCommandContext *c)
{
    Logger::Device::PrintStatus(c);
    c->Printf(
        "  DMA channel: %d, %s\n"
        "  Console:     %s mode, %u bytes buffered\n",
        dmaChannel, dmaChannel >= 0 ? (dma_channel_is_busy(dmaChannel) ? "Busy" : "Idle") : "N/A",
        console.IsForegroundMode() ? "Foreground" : "Background",
        console.OutputBytesBuffered());
}

// Periodic task.  The UART is quite slow, so we limit the amount we
// send on each pass, to avoid blocking the main loop for too long.
void Logger::UARTLogger::Task()
{
    // process console async tasks
    console.Task();

    // check for input
    while (uart_is_readable(uart))
        console.ProcessInputChar(uart_getc(uart));

    // do nothing if we can't send output right now
    if (!IsReady())
        return;

    // set a time limit for non-DMA transfers
    directSendTimeLimit = time_us_64() + 250;
    
    // switch the command console to foreground/background mode
    console.SetForegroundMode(!loggingEnabled || read == logger.write);

    // send output from the command console first
    if (int conAvail = console.OutputBytesBuffered(); conAvail != 0)
    {
        // send as much as we can
        int actual = console.CopyOutput([](const uint8_t *p, int n) { return uartLogger.Send(p, n); }, conAvail);
        conAvail -= actual;

        // if that didn't empty the buffer, stop here
        if (actual < conAvail)
            return;
    }

    // if there's logger output, send it
    if (loggingEnabled && read != logger.write)
    {
        // if read > write, the first chunk is from read to end of buffer
        if (read > logger.write)
        {
            // copy the chunk from read to end of buffer, up to the output space
            int inAvail = logger.bufSize - read;
            int actual = Send(reinterpret_cast<const uint8_t*>(logger.buf.data() + read), inAvail);
            
            // advance pointers
            logger.PostInc(read, actual);
            if (actual < inAvail)
                return;
        }

        // if read < write, the next chunk is from read to write
        if (read < logger.write)
        {
            // copy the chunk from read to write, up to the output space
            int inAvail = logger.write - read;
            int actual = Send(reinterpret_cast<const uint8_t*>(logger.buf.data() + read), inAvail);

            // advance pointers
            logger.PostInc(read, actual);
        }
    }
}

// low-level output writer
int Logger::UARTLogger::Send(const uint8_t *src, int len)
{
    // use DMA if possible
    if (dmaChannel >= 0)
    {
        // if the last DMA operation is still running, there's nothing to do
        if (dma_channel_is_busy(dmaChannel))
            return 0;
        
        // copy as much as we can to the DMA transfer buffer
        int n = 0;
        for (uint8_t *dst = dmaBuf ; n < DMA_BUF_SIZE && len != 0 ; ++n, --len)
            *dst++ = *src++;
        
        // start the transfer
        dma_channel_transfer_from_buffer_now(dmaChannel, dmaBuf, n);

        // return the length written
        return n;
    }
    else
    {
        // send until we reach the timeout or the UART FIFO fills up
        int n = 0;
        for ( ; time_us_64() < directSendTimeLimit && len != 0 && IsReady() ; ++n, --len)
            uart_putc_raw(uart, *src++);

        // return the length written
        return n;
    }
}


// --------------------------------------------------------------------------
//
// USB CDC logger
//

Logger::USBCDCLogger::USBCDCLogger()
{
}

void Logger::USBCDCLogger::Configure(JSONParser &json)
{
    // default to logging and console enabled
    loggingEnabled = true;
    
    // check the configuration
    if (auto *uc = json.Get("serialPorts.usb"); !uc->IsUndefined())
    {
        // configure the USB port
        usbcdc.Configure(uc);
        
        // enable logging
        loggingEnabled = uc->Get("logging")->Bool(true);

        // configure the console
        console.Configure("USB CDC", uc->Get("console"));
    }

    ConfigureFIFO();
}

void Logger::USBCDCLogger::Configure(bool loggingEnabled, bool consoleEnabled, int consoleBufSize, int consoleHistSize)
{
    this->loggingEnabled = loggingEnabled;
    if (consoleEnabled)
        console.Configure("USB CDC", consoleEnabled, consoleBufSize, consoleHistSize);

    ConfigureFIFO();
}

void Logger::USBCDCLogger::ConfigureFIFO()
{
    // retain the FIFO contents across bus resets
    tud_cdc_configure_fifo_t cfg;
    cfg.tx_persistent = true;
    cfg.rx_persistent = true;
    tud_cdc_configure_fifo(&cfg);
}

void Logger::USBCDCLogger::PrintStatus(const ConsoleCommandContext *c)
{
    Logger::Device::PrintStatus(c);
    c->Printf(
        "  Connected:   %s\n"
        "  Out avail:   %u\n"
        "  Console:     %s mode, %u bytes buffered\n",
        tud_cdc_n_connected(0) ? "Yes" : "No",
        tud_cdc_n_write_available(0),
        console.IsForegroundMode() ? "Foreground" : "Background",
        console.OutputBytesBuffered());
}

void Logger::USBCDCLogger::Task()
{
    // if there isn't a terminal connected, do nothing; this will preserve
    // as much buffered text as we can until a terminal is connected
    bool connected = tud_cdc_n_ready(0) && usbcdc.IsTerminalConnected();
    console.SetConnected(connected);
    if (!connected)
    {
        // mark the FIFO as inactive and stop here
        fifoState = FifoState::Inactive;
        return;
    }

    // process console tasks
    console.Task();

    // flush any pending output
    tud_cdc_n_write_flush(0);

    // get the output space available
    int outAvail = tud_cdc_n_write_available(0);

    // Check for TX FIFO stalls.  If the FIFO has pending data, and the
    // last TX completion is more than a few seconds ago, assume that
    // the host has stopped polling for input.
    //
    // This check is an attempt to detect an error condition that occurs
    // with the Windows CDC driver, by observing when the data we're
    // putting in the FIFO isn't getting cleared out (by being
    // transmitted to the host) for an extended period.
    //
    // The Windows CDC driver has a known, long-standing bug where it
    // will stop polling the data IN endpoint (our TX endpoint) if a bus
    // reset occurs while an application session is open.  The symptom
    // you'll see on the Windows side when this occurs is that an open
    // terminal program will stop receiving any data from the device,
    // but it also won't see any error on the line - it thinks it has a
    // good connection that just happens not to be sending any new data.
    //
    // The bug manifest swith a Pinscape device every time the device
    // comes out of sleep mode, IF the XInput interface is enabled on
    // the Pinscape side AND a terminal session (e.g., PuTTY) is open
    // across the sleep/resume.  The reason that XInput is involved is
    // that the Windows XInput driver apparently always performs a USB
    // bus reset after coming out of sleep mode, presumably to allow it
    // to re-enumerate attached controllers.  I haven't observed the bus
    // reset from any other drivers on the Windows side, so to a first
    // approximation, it only happens when XInput is enabled on the
    // device side.  But XInput is only a sufficient condition, not a
    // necessary one - the bug is really triggered by the bus reset, not
    // by XInput per se, so it could just as well be triggered by any
    // OTHER driver that initiates a bus reset.  I just haven't observed
    // any other conditions (so far) where this happens.
    //
    // The bug reports on the Web claim that the only way to clear the
    // error is to disconnect the device, either by physically
    // unplugging it, or by forcing a USB disconnect in software on the
    // device side.  Microsoft might have partially fixed in a more
    // recent version, because on Windows 11 at least, restarting the
    // application session (closing and relaunching Putty, for example)
    // clears it.  But they didn't fix the underlying problem, so the
    // terminal freeze-up will still occur until you close and relaunch
    // the terminal program.
    //
    // The point of this FIFO tracker is to try to detect when the bug
    // has occurred, by noticing when bytes are sitting in the TX FIFO
    // without being transmitted to the host AND it looks like the host
    // still has a terminal session open.  When a terminal session is
    // open, the host SHOULD be regularly polling for data on its IN
    // endpoint, which rapidly remove any data in our TX FIFO.  So if
    // bytes just sit in the FIFO for an extended period, the host
    // probably isn't polling.
    //
    // Unfortunately, there currently isn't anything that we can do to
    // fix the bug when it occurs, so detection is actually kind of
    // pointless.  I've tried a bunch of approaches without success, and
    // all of the discussion of the bug on the Web agrees that the
    // device is powerless to fix it.  But I'm hoping that someone will
    // eventually come up with a clever solution that we can execute on
    // the Pico side, which would make it worthwhile to know when the
    // bug has occurred.  And detection is simple and low-impact.  We
    // can at least log a report when we detect the bug.
    if (outAvail < CFG_TUD_CDC_TX_BUFSIZE)
    {
        // FIFO contains data.  If we're transitioning from inactive
        // or empty, note the time, since this is the starting point
        // for clearing data.
        uint64_t now = time_us_64();
        if (fifoState != FifoState::Active)
        {
            fifoState = FifoState::Active;
            tFifoActive = now;
        }

        // If we haven't already reported a stall as of the last
        // successful transmission, and nothing has moved out of the
        // queue for a few seconds, consider it a stall.  Don't check
        // until we've been active for a couple of seconds, since new
        // data can't leave the FIFO until the next IN endpoint polling
        // cycle, and we could make multiple checks here between those
        // polling cycles.
        uint64_t tLastTx = usbcdc.GetTimeTxCompleted();
        const int tMaxFifoWaitInSeconds = 5;
        if (tLastTx != tFifoStall
            && now > tLastTx + tMaxFifoWaitInSeconds*1000000
            && now > tFifoActive + 2000000)
        {
            // the FIFO seems to be stuck
            Log(LOG_DEBUG, "CDC output stream data has been pending for %d seconds; "
                "the IN endpoint might be stalled.  Try closing and re-launching "
                "the terminal emulator program.\n",
                tMaxFifoWaitInSeconds);

            // NOTE:
            // I haven't found anything we can do here to restore the
            // connection.  I've tried all sorts of things without any
            // luck: blocking output on the IN endpoint for a few seconds
            // after a RESUME event so that nothing is in flight when
            // the bus reset occurs; sending a BREAK signal via the
            // notify endpoint; aborting the stuck transmission in the
            // Pico USB controller and starting a new one; closing and
            // re-opening the endpoint.  Nothing makes any difference.
            //
            // Reports on the Web say that the only thing a device can
            // do to the clear the error is disconnect entirely, or just
            // do a hardware reset, but I see no good reason to disrupt
            // the other endpoints, since this isn't a USB problem or
            // a device problem or even a Windows problem; it's only a
            // problem in the Windows CDC driver specifically, and it
            // can be cleared on the Windows side by closing PuTTY (or
            // whatever terminal program you're using) and launching
            // a new session.  The big annoyance is that the CDC driver
            // doesn't even let the application know that there's a
            // problem.  But it should be obvious enough to a user that
            // the terminal has stopped working.

            // Note the last transmit time as the stall time.  This
            // ensures that we report this stall only once - we won't
            // check again until a new transmission completes, which
            // would indicate that the last jam was cleared.
            tFifoStall = tLastTx;
        }
    }
    else
    {
        // mark the FIFO empty
        fifoState = FifoState::Empty;
    }

    // if there's no space available in the FIFO, don't try to send
    // anything on this task iteration, since the FIFO can't accept
    // any new data
    if (outAvail == 0)
        return;

    // Switch the command console to foreground/background mode,
    // according to the log buffer.  Console editing goes into the
    // background when there are logging messages to send, since
    // log messages can be generated asynchronously and therefore
    // must be able to interrupt an editing session in progress.
    console.SetForegroundMode(!loggingEnabled || read == logger.write);
    
    // send output from the command console first
    if (int conAvail = console.OutputBytesBuffered(); conAvail != 0)
    {
        // copy as much as we have space for
        int copy = std::min(conAvail, outAvail);
        int actual = console.CopyOutput([](const uint8_t *p, int n){ tud_cdc_n_write(0, p, n); return n; }, copy);
        outAvail -= actual;
        
        // flush output
        tud_cdc_n_write_flush(0);
        
        // stop here if we didn't exhaust the console output
        if (actual < copy)
            return;
    }
    
    // if logging is enabled, add the logger output
    if (loggingEnabled)
    {
        // if read > write, the first chunk is from read to end of buffer
        if (read > logger.write && outAvail > 0)
        {
            // copy the chunk from read to end of buffer, up to the output space
            int inAvail = logger.bufSize - read;
            int copy = std::min(inAvail, outAvail);
            tud_cdc_n_write(0, &logger.buf.data()[logger.PostInc(read, copy)], copy);
            outAvail -= copy;
        }
        
        // if read < write, the next chunk is from read to write
        if (read < logger.write && outAvail > 0)
        {
            // copy the chunk from read to write, up to the output space
            int inAvail = logger.write - read;
            int copy = std::min(inAvail, outAvail);
            tud_cdc_n_write(0, &logger.buf.data()[logger.PostInc(read, copy)], copy);
            outAvail -= copy;
        }
    }
}

bool Logger::USBCDCLogger::IsReady()
{
    return tud_cdc_n_write_available(0) > 0;
}

void Logger::USBCDCLogger::Write(char c)
{
    tud_cdc_n_write_char(0, c);
}


// --------------------------------------------------------------------------
//
// TinyUSB logging callback.  TinyUSB allows the application to define
// its own custom function to handle logging output.  We only need to
// define this if TinyUSB logging is enabled at compile-time, which must
// be done explicitly via the CFG_TUSB_DEBUG macro in ./tusb_config.h.
// (That header is supplied by the application, not the library, despite
// its name.)
//
// Note that TinyUSB's built-in logging is extremely voluminous, since
// it's designed for debugging the library and all of its low-level code
// that interfaces to the USB hardware.  It's rarely useful to enable it
// unless you suspect a bug within TinyUSB, and even then it can be
// difficult to interpret the deubg output simply because there's so
// much of it.  In most cases, I've found it more helpful to selectively
// add my own instrumentation via the logToPinscape() function below.
#if defined(CFG_TUSB_DEBUG) && (CFG_TUSB_DEBUG > 0)
extern "C" int tusbLogPrintf(const char *f, ...)
{
    // log through the main logger
    va_list va;
    va_start(va, f);
    int ret = LogV(LOG_TINYUSB, f, va);
    va_end(va);

    // return the length
    return ret;
}
#endif // CFG_TUD_DEBUG

// Additional instrumentation for tinyusb.  This is the same idea as
// tusbLogPrintf() above, but this one's for adding OUR instrumentation
// to tinyusb code - tusbLogPrintf is for tinyusb's own instrumentation
// that's part of the official distribution.  Our version lets us add
// selective instrumentation, without activating the voluminous built-in
// debug logging, to track down specific issues we want to investigate.
//
// Since this is our own instrumentation, we log it under the DEBUG
// category (rather than the TINYUSB category that we use for tinyusb's
// own native instrumentation).
extern "C" void logToPinscape(const char *f, ...)
{
    va_list va;
    va_start(va, f);
    LogV(LOG_DEBUG, f, va);
    va_end(va);
}
