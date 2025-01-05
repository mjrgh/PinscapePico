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
#include "PicoLED.h"
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
    // Set the buffer size first, so that we have a place to capture
    // error messages while we're parsing the configuration.  The size
    // limits are arbitrary; the minimum ensures that we don't shrink
    // the buffer from the initial default size, which would complicate
    // dealing with the resize, and the maximum is just a sanity check
    // to prevent the user from accidentally using up all available RAM
    // and crashing the Pico during initialization.
    int configuredBufSize = bufSize;
    const int MinBufSize = bufSize, MaxBufSize = 64*1024;
    if (auto *sizeVal = json.Get("logging.bufSize") ; !sizeVal->IsUndefined())
    {
        // validate the size
        configuredBufSize = sizeVal->Int();
        if (configuredBufSize >= MinBufSize && configuredBufSize <= MaxBufSize)
            bufSize = configuredBufSize;
    }

    // resize the buffer if necessary (note that we only allow it
    // to expand from the initial default, enforced above)
    if (buf.size() != bufSize)
    {
        // remember the old buffer size
        size_t oldSize = buf.size();
        
        // expand the buffer (we don't allow it to shrink)
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

    // set defaults - enable timestamps and message type prefixes, disable colors
    showTimestamps = true;
    showTypeCodes = true;
    showColors = false;

    // log an error on the buffer size if we rejected it
    if (bufSize != configuredBufSize)
        Log(LOG_ERROR, "logging.bufSize is invalid; must be %d to %d\n", MinBufSize, MaxBufSize);

    // get the logging key
    if (auto *val = json.Get("logging") ; !val->IsUndefined())
    {

        // get the message type filter
        if (auto *maskVal = val->Get("filter"); !maskVal->IsUndefined())
        {
            // start with an empty mask
            uint32_t newMask = 0;
            
            // get the string, and convert to lower-case for case insensitivity
            std::string s = maskVal->String();
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            const char *p = s.c_str();

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

        // set the display format
        showTimestamps = val->Get("timestamps")->Bool(true);
        showTypeCodes = val->Get("typeCodes")->Bool(true);
        showColors = val->Get("colors")->Bool(false);
    }
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
    // buffer the string one character at a time, with newline
    // translation and line header insertion
    while (*s != 0)
    {
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

    // update the reader views - they might need to discard the oldest
    // character if the buffer is now full from their perspective
    for (auto d : devices)
        d->OnPut();
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

// Periodic task.  The UART is quite slow, so we limit the amount we
// send on each pass, to avoid blocking the main loop for too long.
void Logger::UARTLogger::Task()
{
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
    if (int conAvail = console.OutputAvailable(); conAvail != 0)
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
}

void Logger::USBCDCLogger::Task()
{
    // if there isn't a terminal connected, do nothing; this will preserve
    // as much buffered text as we can until a terminal is connected
    bool connected = tud_cdc_n_connected(0);
    console.SetConnected(connected);
    if (!connected)
        return;

    // get the output space available
    int outAvail = tud_cdc_n_write_available(0);
    if (outAvail > 0)
    {
        // Switch the command console to foreground/background mode,
        // according to the log buffer.  Console editing goes into the
        // background when there are logging messages to send, since
        // log messages can be generated asynchronously and therefore
        // must be able to interrupt an editing session in progress.
        console.SetForegroundMode(!loggingEnabled || read == logger.write);

        // send output from the command console first
        if (int conAvail = console.OutputAvailable(); conAvail != 0)
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
// Tinyusb logging callback.  Tinyusb allows the application to define
// its own custom function to handle logging output.  We only need to
// define this if Tinyusb logging is enabled at compile-time.
//
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
