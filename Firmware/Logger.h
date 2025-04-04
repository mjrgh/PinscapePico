// Pinscape Pico - Logging
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This defines the logging interface, which sends text information back
// to the PC, mostly for debugging and troubleshooting.  We can log to
// the USB CDC (virtual COM port), to a UART port, or both.
//

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>

// Pico SDK headers
#include <hardware/uart.h>
#include <hardware/dma.h>

// project headers
#include "JSON.h"
#include "CommandConsole.h"

// global logger singleton
class Logger;
extern Logger logger;


// Logging.  These routines format text with substitution parameters,
// printf-style, and add the formatted text to the log buffer.  'type'
// is a LOG_xxx constant indicating which type of logging message this
// is, which we can use for filtering.
int Log(int type, const char *f, ...);
int LogV(int type, const char *f, va_list va);

// Log type codes
//
// Note that these are message type codes, NOT priority levels.  Many
// logging systems use a hierarchy of priority levels (fatal, error,
// warning, informational, debugging) and let the user select the
// minimum priority to display.  We don't use quite that approach.
// Instead, we give each message a non-hierarchical classification,
// and we let the user filter by individual type code.  For production
// deployment, the ERROR, WARNING, INFO, and CONFIG message classes
// are probably the most helpful.
//
// The "Debug" class of messages are mostly for developers actively
// working on the firmware, to help trace code paths and internal
// state to verify that subsystems are working as expected, and to
// help isolate the cause when they're not.  The "Extra" debug class
// is meant for cases that tend to produce a lot of repetitive
// messages in the log, so much so that you might want to filter
// them out even when actively working on the firmware.
//
const int LOG_DEBUG   = 0;         // debugging messages
const int LOG_DEBUGEX = 1;         // extra debugging messages
const int LOG_ERROR   = 2;         // error messages
const int LOG_WARNING = 3;         // warning messages (less serious than errors, but more than just informational)
const int LOG_INFO    = 4;         // general information and status messages
const int LOG_VENDOR  = 5;         // informational: vendor interface requests
const int LOG_XINPUT  = 6;         // informational: XInput-related
const int LOG_CONFIG  = 7;         // informational: config manager-related
const int LOG_TINYUSB = 8;         // Tinyusb library debug logging


// Logger object.  This is a global singleton that collects log output
// from the program and distributes it to the log devices.  This object
// saves the log text in a ring buffer, which the devices read from to
// forward the text to the connected terminal.  Each log device has its
// own read-only view of the ring buffer, so each device can deliver
// text to its consumer at its own pace without affecting the other
// devices' views.
class Logger
{
public:
    // construct
    Logger();

    // Run-time initialization.  This should be called before any
    // log messages are generated.
    void Init();

    // set display modes
    void SetDisplayModes(bool timestamps, bool typeCodes, bool colors);

    // get/set the filter mask
    uint32_t GetFilterMask() const { return mask; }
    void SetFilterMask(uint32_t mask) { this->mask = mask; }

    // configure the logger subsystem from JSON data
    void Configure(JSONParser &json);

    // install the console command (done automatically in Configure;
    // this is broken out for use in subprojects that don't use a
    // JSON configuration)
    void InstallConsoleCommand();

    // Set the log buffer size.  This can only increase from the current
    // size, so that we don't lose any information currently in the
    // buffer.  The size is also limited to an arbitrary maximum, to
    // avoid running out of memory due to bad configuration settings.
    void SetBufferSize(int newSize);

    // Set filters from a space-delimited list of filter names.  The
    // string must be all lower-case.
    void SetFilters(const char *filters);

    // Run logging tasks
    void Task();

    // Check a LOG_xxx type code against the filter mask.  Returns true
    // if the type is included in the filter, false if not.  (true means
    // that the message type is displayed in the log.)
    bool CheckFilter(int type);

    // Put a string to the buffer, with newline translation ('\n' -> CR/LF)
    void Puts(int typeCode, const char *s);

    // Log the preserved log data from the prior session into the
    // current session log.  Does nothing if the prior session log is
    // empty.  'type' is the LOG_xxx code for logging the messages.
    void LogPriorSessionLog(int type);

    // Log one line of preserved log data from the prior session to a
    // console.  'ofs' is the offset in the buffer of the start of the
    // line to log; pass 0 on the first call, and then pass the return
    // value from the prior call on each subsequent call.  The return
    // value is the offset of the start of the next line, or -1 upon
    // reaching the end of the buffer.
    int PrintPriorSessionLogLine(CommandConsole *console, int ofs);

    // Logger device.  This is the base class for the concrete output
    // devices (UART, USB CDC, vendor interface).
    class Device
    {
        friend class Logger;

    public:
        // is this logger enabled?
        bool IsEnabled() const { return loggingEnabled; }
        
        // number of characters available in our view of the ring buffer
        int Available() const;

        // get the next character from the ring buffer
        char Get();

        // get a block of characters from our view of the ring buffer; returns the
        // number actually read, which might be less than the request
        int Get(char *dst, size_t n);

        // show status
        virtual void PrintStatus(const ConsoleCommandContext *c);
        virtual const char *Name() const = 0;

    protected:
        // periodic task - write buffered characters to the underlying
        // device whenever it's ready to accept more output
        virtual void Task();

        // check the underlying device for readiness to accept a character of output
        virtual bool IsReady() = 0;

        // write a character to the underlying device
        virtual void Write(char c) = 0;

        // Handle a Put() to the shared ring buffer.  This updates our
        // view's read pointer if the write pointer just collided.
        void OnPut() { if (read == logger.write) read = logger.Next(read); }

        // Handle expanding the underlying buffer.  The buffer expands
        // by inserting a gap (new blank space) starting at the current
        // write pointer.  This moves the existing text that was past
        // the write pointer to the end of the new buffer, moving it
        // right by the expansion amount.  If our read pointer is within
        // the moved section, we have to move it over to match.
        void OnExpandBuf(int expandedBy) { if (read > logger.write) read += expandedBy; }

        // is logging enabled on this device?
        bool loggingEnabled = false;

        // Ring buffer read pointer.  This points into the shared ring
        // buffer in the main Logger object.
        int read = 0;
    };

    // Vendor Interface logger.  This logger lets the USB vendor interface
    // retrieve log data to send to the PC, via the Config Tool.  This
    // device is pull-only; we can't push data to the underlying device
    // because it can only send data when explicitly polled by the PC.
    class VendorInterfaceLogger : public Device
    {
    public:
        VendorInterfaceLogger();
        const char *Name() const { return "VendorIfc"; }

        // configure
        void Configure(JSONParser &json);

    protected:
        // This device can only pull data from the buffer when the PC polls
        // for data, so it's never "ready".  It just leaves data in its view
        // of the ring buffer until the PC asks for it.
        virtual bool IsReady() override { return false; }
        virtual void Write(char) { }
    };

    // UART logger.  This logs to a Pico native UART port.
    class UARTLogger : public Device
    {
        friend class Logger;
        
    public:
        UARTLogger();
        const char *Name() const { return "UART"; }
        virtual void PrintStatus(const ConsoleCommandContext *c) override;
            
        void Configure(JSONParser &json);

        // command console
        CommandConsole console;

        // Flush bufers.  This waits for buffered data to go out on the wire.
        // This can be quite slow, so it's only for debugging.
        void FlushForDebugging();

    protected:
        void Init(uart_inst_t *uart, int gpTx, int gpRx, int baud);
        virtual bool IsReady() override;
        virtual void Write(char c) override;
        virtual void Task() override;;

        // low-level buffer output writer
        int Send(const uint8_t *buf, int len);

        // Pico SDK UART interface
        uart_inst_t *uart = nullptr;

        // DMA buffer.  The UART is quite slow, so we use DMA to move
        // data from the memory buffer to the UART, to reduce CPU load
        // managing the device.  At 115 kbps, we only move about 12
        // bytes per millisecond, so a 256-byte buffer gives us about
        // 20ms between refills.
        static const int DMA_BUF_SIZE = 256;
        uint8_t dmaBuf[DMA_BUF_SIZE];

        // DMA channel
        int dmaChannel = -1;

        // time limit for non-DMA transfers
        uint64_t directSendTimeLimit = 0;
    };
    
    // USB CDC logger.  This logs to the USB CDC (virtual COM port) interface.
    class USBCDCLogger : public Logger::Device
    {
        friend class Logger;

    public:
        USBCDCLogger();
        const char *Name() const { return "USB CDC"; }
        virtual void PrintStatus(const ConsoleCommandContext *c) override;
        
        void Configure(JSONParser &json);
        void Configure(bool loggingEnabled, bool consoleEnabled, int consoleBufSize, int consoleHistSize);

        // command console
        CommandConsole console;

    protected:
        void ConfigureFIFO();
        virtual bool IsReady() override;
        virtual void Write(char c) override;
        virtual void Task() override;

        // CDC TX completion timestamp at TX FIFO stall
        uint64_t tFifoStall = 0;

        // time the FIFO last changed from inactive/empty to active
        uint64_t tFifoActive = 0;

        // FIFO tracking state
        enum class FifoState
        {
            Inactive,        // disconnected, bus suspend mode
            Empty,           // bus active, FIFO empty
            Active,          // FIFO contains data
        };
        FifoState fifoState = FifoState::Inactive;
    };

protected:
    // "Tail" - this is a small static buffer that we locate in
    // not-initialized-at-reset RAM, where we keep a copy of the most
    // recent section of the log data.  Since the buffer is in storage
    // that's not initialized at reset, it remains intact across a CPU
    // reset, allowing us to capture the PRIOR session's log in the new
    // session after a reset.  This can be useful for diagnostics after
    // a crash or unexpected watchdog reset, by providing some
    // information on what the program was doing just before the crash.
    //
    // The complete "prior session log" mechanism consists of two
    // buffers: this struct, which is in not-initialized-at-reset RAM,
    // and the PriorSessionLog struct, which is in ordinary RAM.  The
    // prior session struct is initialized immediately after reset with
    // a copy of the log tail struct.  After that, the log tail struct
    // is cleared, and then maintains a live copy of the log as it's
    // written during the new session.  So the "log tail" always
    // reflects output from the CURRENT session, EXCEPT immediately
    // after a CPU reset, when it still contains the data left over from
    // the PREVIOUS session.
    //
    // Because this is in not-initialized-at-reset storage, the struct
    // should be expected to contain randomized data after a power-on
    // reset.  We use a lightweight checksum to distinguish this from
    // valid data carried over across a reset.
    struct LogTail
    {
        // test for validity
        uint32_t IsValid() const
        {
            // it's valid if the integrity checksum is correct and
            // the counters are all in range
            return write >= 0 && write < sizeof(buf)
                && nBytes >= 0 && nBytes <= sizeof(buf)
                && check == CalcCheck();
        }
        
        // clear the buffer
        void Clear()
        {
            write = 0;
            nBytes = 0;
            check = CalcCheck();
        }
        
        // add a byte
        void Add(char c)
        {
            buf[write++] = c;
            if (write >= sizeof(buf)) write = 0;
            if (nBytes < sizeof(buf)) nBytes += 1;
            check = CalcCheck();
        }
        
        // number of bytes stored
        int nBytes;
        
        // write offset
        int write;
        
        // Integrity check.  This is a lightweight checksum, to determine if
        // the buffer contains random power-on garbage or contains valid
        // data.  It's updated after each buffer write, and can be tested at
        // startup to determine if the buffer contains valid data carried
        // over from before the CPU reset.
        uint32_t check;
        
        // calculate the integrity check
        uint32_t CalcCheck() const {
            int prevWrite = write == 0 ? sizeof(buf) - 1 : write < sizeof(buf) ? write - 1 : 0;
            return ~((nBytes << 18) | (write << 8) | buf[prevWrite]);
        }
        
        // storage area
        static const int BUF_SIZE = 1024;
        char buf[BUF_SIZE];
    };
    static LogTail logTail;

    // Prior session log snapshot.  At startup, we copy a snapshot of
    // the not-initialized-at-reset log tail buffer into this buffer, to
    // preserve the tail end of the log from just before the last reset
    // for inspection during this session.
    struct PriorSessionLog
    {
        int nBytes = 0;                // number of bytes actually stored
        char buf[LogTail::BUF_SIZE];   // buffer snapshot
    };
    PriorSessionLog priorSessionLog;

    // Put a character to the buffer
    void Put(char c);

    // console command
    void Command_logger(const ConsoleCommandContext *ctx);
    static void Command_loggerS(const ConsoleCommandContext *ctx) { logger.Command_logger(ctx); }

    // list of log devices
    std::list<Device*> devices;

    // Message mask.  This selects which message types are passed
    // to this output.  Each bit corresponds to (1 << LOG_xxx) for
    // one of the LOG_xxx constants.  Enable all message types by
    // default.
    uint32_t mask = ~0UL;

    // Include timestamps in log messages?
    bool showTimestamps = false;

    // Include type codes in log messages?
    bool showTypeCodes = false;

    // Include ANSI color code escape sequences?
    bool showColors = false;

    // Current output column; resets to zero at each newline through
    // Puts().  We use this to determine when to insert line headers
    // (timestamps and type codes).
    int col = 0;

    // default ring buffer size; this can be configured in the JSON settings
    static const int MinBufSize = 8192;
    static const int MaxBufSize = 65536;
    int bufSize = MinBufSize;

    // ring buffer
    std::vector<char> buf;

    // Write index.  This is a circular buffer, so the write index wraps
    // when it reaches the end of the buffer.  The Logger object doesn't
    // have a read pointer; instead, each device has its own private
    // read pointer.  That allows each device to separately consume data
    // from the buffer at its own pace without affecting the other
    // devices' views of the text.
    int write = 0;

    // get the next value for a ring buffer index after wrapping
    int Next(int i) const { return (i + 1) % buf.size(); }

    // post-increment an index into the ring buffer
    int PostInc(int &i) { int orig = i; i = Next(i); return orig; }
    int PostInc(int &i, int by) { int orig = i; i = (i + by) % buf.size(); return orig; }
};

// Logger device global singletons
extern Logger::VendorInterfaceLogger vendorInterfaceLogger;
extern Logger::UARTLogger uartLogger;
extern Logger::USBCDCLogger usbCdcLogger;

