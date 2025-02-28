// Pinscape Pico - Command Console
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements a simple and primitive command line console
// that can be connected to a text-oriented terminal via a serial
// device, such as a UART port or USB CDC interface, to allow a user to
// connect to the Pico with an interactive terminal program such as
// PuTTY and enter commands manually.  This is a lower-level supplement
// to the host-side Config Tool, designed entirely for debugging and
// troubleshooting purposes.
//
// This isn't intended as a replacement for the Config Tool, and it's
// absolutely and positively not a programmatic interface.  We have two
// vastly superior programmatic interfaces (the Vendor Interface and the
// HID Feedback Controller interface), each designed for specific
// application domains; anyone who's tempted to write a client program
// that sends commands mechanically to a console should stop immediately
// and look at the two proper progamming interfaces - I hope I don't
// have to add a CAPTCHA to reinforce this point.
//
// To integrate this into a logger device:
//
//  - Define a CommandConsole object in the class that implements
//    the underlying physical device (UART, CDC, etc)
//
//  - In the device class, call ProcessInputChar() or ProcessInputStr()
//    upon receiving user input from the terminal
//
//  - In the Task() routine, call console.SetForegroundMode(false) any
//    time there's new output available from your logger, and call the
//    same routine with (true) when the logger output is empty.  This
//    lets the console manage the on-screen command editing display.
//
//  - In the logger Task(), merge output from the logger with output
//    from the console object, and feed the merged text stream to the
//    underlying physical serial device.  The console MUST take
//    precedence: send all console output to the host first; then, and
//    only then (i.e., after exhausing all buffered console output),
//    send the logger output.  Giving precedence to the console output
//    allows the console to manage its command-line appearance properly
//    when switching in and out of the foreground.  The console should
//    never be able to starve logger output, because the console never
//    generates output asynchronously; it only generates output when fed
//    input via ProcessInputXxx().  It's thus limited by how quickly the
//    user can manually enter commands.
//  
//

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <malloc.h>
#include <stdarg.h>
#include <vector>
#include <functional>
#include <map>
#include <memory>
#include <string>

// Pico SDK headers
#include <pico/stdlib.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "JSON.h"

// external/forward declarations
class CommandConsole;

// Command exeution context - this is passed to a command handler
// when the user invokes the command from a console.
struct ConsoleCommandContext
{
    // console object initiating the request
    CommandConsole *console;

    // arguments
    int argc;
    const char **argv;

    // Current command's usage string
    const char *usage;

    // Command handler
    void (*exec)(const ConsoleCommandContext*);

    // Owner context
    void *ownerContext;

    // print to the console
    void Print(const char *str) const;
    void Printf(const char *fmt, ...) const;
    void VPrintf(const char *fmt, va_list va) const;

    // show the usage string
    void Usage() const;
};

// Command console.  Each instance represents a single session attached
// to a particular host connection, typically a serial connection that
// can connect to a terminal on the host (particularly UART and USB
// CDC).
class CommandConsole
{
public:
    CommandConsole();

    // Show the console banner.  This is when we detect a newly detected
    // terminal on the console's serial port.  To allow this class to be
    // reused in different subprojects, we farm out the implementation
    // to the client code.  If you're seeing this as an unresolved symbol,
    // just add it to one of your subproject's files.  For the main
    // Pinscape Pico firmware, it's in main.cpp.
    void ShowBanner();
    
    // Configure from the given parent key.  Returns true if the
    // console is enabled, false if not.
    bool Configure(const char *descName, const JSONParser::Value *val);

    // configure from preloaded data
    bool Configure(const char *descName, bool enable, int bufSize, int histSize);

    // is the console enabled?
    bool IsEnabled() const { return enabled; }

    // Set foreground/background mode.  A console typically shares a
    // host connection with a logger.  The logger can generate output
    // asynchronously, which can interrupt the command line editing in
    // progress.  We therefore provide a call that lets the containing
    // connection manager alert us when new logging output arrives, so
    // that we can remove the command line from the screen and wait for
    // the logging output to be sent.  Foreground mode means that an
    // editing session is open, and we're displaying a partial command
    // line; background mode means that log output is in progress, and
    // the command line in progress is hidden.
    //
    // A new console starts in background mode.  The parent must
    // explicitly enable foreground mode when appropriate (as defined by
    // the parent's design).
    void SetForegroundMode(bool fg);
    bool IsForegroundMode() const { return foregroundMode; }

    // Set connected mode.  This lets us know if the underlying
    // connection layer (CDC, UART) senses the presence of a remote
    // terminal.
    void SetConnected(bool connected);

    // Add a command execution callback.  This adds an entry to our
    // global collection of commands that the user can invoke via a
    // console.  Each subsystem that provides commands must invoke this
    // during initialization to add its commands to the global list.
    // (This should ONLY be called during initialization, because it
    // involves dynamic memory allocation, which we try to avoid during
    // normal main loop operation as a general rule, to ensure that
    // resource usage is fixed throughout a session and thus not subject
    // to unpredictable failures.)
    //
    // 'name' is the token that invokes the command, which is the first
    // token of the command in the usual shell command process idiom.
    // (This is also passed as argv[0] in the command arguments.)
    //
    // 'desc' is a short one-liner command description that the
    // console's "help command displays in the listing of available
    // commands.
    //
    // 'usage' shows a more detailed usage message.  During the command
    // execution callback, this can be displayed by calling Usage() in
    // the context object.  If this is null, Usage() will call back into
    // the 'exec' function with argv set to { name, "--help" }, to let
    // the command provide a generated usage message instead of a fixed
    // string.
    //
    // 'exec' is the function invoked to carry out the command.
    //
    // 'ownerContext' is an optional opaque context object for the
    // caller's use in the 'exec' callback.  This is passed in the
    // ConsoleCommandContext object when the function is invoked, to
    // pass any needed context back to the function implementation.
    // This allows sharing a common 'exec' function for multiple uses,
    // with the concrete variations selected in the context object.
    using ExecFunc = void(const ConsoleCommandContext*);
    static void AddCommand(
        const char *name, const char *desc, const char *usage,
        ExecFunc *exec, void *ownerContext = nullptr);


    // Add a generic PWM output controller chip command handler.  This
    // uses the Command_pwmchip() common handler to implement a basic
    // command handler for a PWM chip, with options to list the port
    // levels, show statistics, and set port levels for testing.
    struct PWMChipClass
    {
        // chip name, for display in messages
        const char *name;
        
        // number of chip instances
        int nInstances;

        // maximum port level
        int maxLevel;

        // Instance select option string.  This is a single string, with
        // fields separated by tab (\t) characters and entries separated
        // by newlines (\n), giving one or more switch option names for
        // selecting a chip/chain/instance.  For example:
        //   "-c <num>\tselect chip <num>\n--chip <num>\tsame as -c"
        const char *selectOpt;

        // validate an instance
        virtual bool IsValidInstance(int instance) const = 0;

        // list instance statistics
        virtual void ShowStats(const ConsoleCommandContext *ctx, int instance) const = 0;

        // get the number of ports on a given instance
        virtual int GetNumPorts(int instance) const = 0;

        // validate a port number
        virtual bool IsValidPort(int instance, int port) const = 0;

        // set the specified port level in the specied value
        virtual void SetPort(int instance, int port, int level) const = 0;

        // get a port setting
        virtual int GetPort(int instance, int port) const = 0;

        // print the pin name of a port (e.g., "LED0", "Chip 3, LED15")
        virtual void PrintPortName(const ConsoleCommandContext *ctx, int instance, int port) const = 0;
    };
    static void AddCommandPWMChip(
        const char *commandName, const char *commandDesc, const PWMChipClass *classDesc);

    // process asynchronous tasks
    void Task();
    
    // process input from the PC
    void ProcessInputChar(char c);
    void ProcessInputStr(const char *str);
    void ProcessInputStr(const char *str, size_t len);

    // write a string to the output buffer
    void PutOutputStr(const char *str);
    void PutOutputStr(const char *str, size_t len);

    // write a formatted string to the buffer
    void PutOutputFmt(const char *fmt, ...);
    void PutOutputFmtV(const char *fmt, va_list va);

    // alias for PutOutputFmt
    void Print(const char *fmt, ...);
    void PrintV(const char *fmt, va_list va) { PutOutputFmtV(fmt, va); }

    // write to the output buffer, with newline translation
    void PutOutputChar(char c);

    // write to the output buffer, no translation
    void PutOutputCharRaw(char c);

    // is another buffered output character available?
    bool OutputReady() const { return outputRead != outputWrite; }

    // how much output text is stored in the buffer?
    int OutputBytesBuffered() const {
        return outputRead <= outputWrite ?
            outputWrite - outputRead :
            static_cast<int>(outputBuf.size() - (outputRead - outputWrite));
    }

    // how much free space is in the buffer?
    int OutputBufferBytesFree() const { return outputBuf.size() - OutputBytesBuffered(); }

    // get the next buffered output character
    char GetOutputChar() { return (outputRead != outputWrite) ? outputBuf[PostInc(outputRead)] : 0; }

    // Copy output through a callback.  The callback takes a buffer and
    // length, and writes as much as it can, returning the number of
    // characters actually written.  This function in turn returns the
    // total number of characters written across all of its callback
    // invocations (it might call the callback more than once, to send
    // separate buffer segments).
    int CopyOutput(std::function<int(const uint8_t *dst, int len)> func, int len);

    // Asynchronous command contination handler.
    //
    // A command handler can use a completion handler to carry out tasks
    // that can't be conveniently executed in a single main loop
    // iteration: tasks with a lot of output that need to wait for the
    // buffer to clear; tasks that require more than one main loop
    // iteration's worth of time to complete; or tasks that block on
    // hardware polling.
    //
    // To set up asynchronous completion, define a concrete subclass of
    // this class, instantiate it, and pass it to ContinueWith().  The
    // console will hold onto a unique pointer to this object, deleting
    // it when done.  On each main loop iteration, the console task
    // handler will call the object's task handler to give it another
    // time slice.  This continues until the handler returns false to
    // indicate that it's done, or the user cancels the command with
    // a Ctrl+C key press.
    class Continuation
    {
    public:
        // destruction
        virtual ~Continuation() { }
        
        // Main task handler.  On each main loop, the console will call
        // this to let the command handler continue its work.  Returns
        // true if the command should continue executing, false if its
        // work is done.
        virtual bool Continue(CommandConsole *console) = 0;

        // Cancel the command by user interrupt.  This is called when
        // the user presses Ctrl+C while the command is active.  This is
        // a notification only; it can't prevent the cancellation.  In
        // most cases, it's not necessary to override this, since the
        // destructor can usually take care of all necessary cleanup.
        // This is meant for cases where the command might want to do
        // something extra in case of explicit cancellation in lieu of
        // its own determination that its work is complete.
        virtual void OnUserCancel() { }
    };

    // Set a continuation handler.  The console takes ownership of
    // the object, and deletes it when done.
    void ContinueWith(Continuation *c) {
        continuation.reset(c);
    }

protected:
    // command continuation object
    std::unique_ptr<Continuation> continuation;

    // internal input character processing
    void ProcessInputCharInternal(char c);

    // process pending input
    void ProcessPendingInput();

    // command line editing
    void DelChar();
    void DelLeft();
    void DelLine();
    void DelEOL();
    void MoveLeft();
    void MoveRight();
    void MoveHome();
    void MoveEOL();

    // command history operations
    void HistPrev();
    void HistNext();

    // re-display the current command
    void RePrintCurCommand();

    // calculate the display column for a given portion of the command buffer
    int DisplayCol(int len);

    // auto-complete at the current editing position
    void AutoComplete();

    // command prompt
    const char *prompt = "\033[0m> ";

    // first prompt displayed?
    bool firstPromptDisplayed = false;

    // is the console enabled?
    bool enabled = false;
    
    // Current foreground/background mode
    bool foregroundMode = false;

    // terminal is connected
    bool connected = false;

    // process the current command buffer as a new command
    void ProcessCommand();

    // execute a tokenized command
    void ExecCommand(int argc, const char **argv);

    // post-increment a ring buffer pointer
    int PostInc(int &i) { int ret = i++; if (i >= outputBuf.size()) i = 0; return ret; }
    int PostInc(int &i, int by) { int ret = i; i = (i + by) % outputBuf.size(); return ret; }

    // output buffer - this is a ring buffer
    std::vector<uint8_t> outputBuf;

    // read and write indices in the output ring buffer
    int outputRead = 0;
    int outputWrite = 0;

    // Input buffer.  This accumulates input characters when a command
    // is executing asynchronously.  This is a circular buffer.
    char inBuf[256];
    int inBufWrite = 0;
    int inBufCnt = 0;

    // command-line buffer
    char cmdBuf[256] = "";
    int cmdLen = 0;

    // cursor column
    int col = 0;

    // Command history buffer.  This is another ring buffer; each command
    // is represented as a null-terminated string.
    std::vector<char> history;

    // history read/write pointers
    int histRead = 0;
    int histWrite = 0;

    // Current history recall index.  0 is the newest line.  When we
    // start a new line, we enter it as an empty string in the buffer,
    // and if we scroll back into an older item, we stash the line in
    // progress into the history.
    int histIndex = 0;

    // selected column in the last line, so that we can restore our
    // editing position if we end up navigating back to the new entry
    int histOldCol = 0;

    // save the current command to the history buffer
    void SaveHistory(bool forNavigation);

    // add a string to the history buffer as a new command
    void AddHistory(const char *buf);

    // load the history command starting at the given ring buffer index
    void LoadFromHistory(int bufferIndex);

    // find the nth history command; returns the buffer index, or -1
    // if no such command exists
    int FindHistory(int n);

    // escape input state
    //
    // ESC [ <number> ; <number> ~|letter
    //
    // code = numeric code
    // key = numeric key code OR double-quoted string key name
    // more = any code sections after the key
    enum class EscState
    {
        None,   // not processing an escape
        Esc,    // escape received
        Code,   // '[' received, processing code section
    };
    EscState escState;

    // escape accumulators
    int escAcc[3]{ 0, 0, 0 };
    int escAccIndex = 0;

    // escape timeout
    uint64_t escTimeout;

    // process the current escape code contained in the escape state variables
    void ProcessEscape(char lastChar);

    // command table - this is a global list of available commands
    struct CommandTableEle
    {
        CommandTableEle(const char *name, const char *desc, const char *usage, ExecFunc *exec, void *ownerContext) :
            name(name), desc(desc), usage(usage), exec(exec), ownerContext(ownerContext) { }
        const char *name;
        const char *desc;
        const char *usage;
        ExecFunc *exec;
        void *ownerContext;
    };

    // Get the command table.  This uses the local static initialization
    // idiom to guarantee that the table is constructed before its first
    // use, even if it's used from a static constructor that runs before
    // our own static constructor.
    using CommandTable = std::map<std::string, CommandTableEle>;
    static CommandTable &GetCommandTable() { static CommandTable t; return t; }

    // command handlers
    static void Command_help(const ConsoleCommandContext *ctx);

    // Generic PWM output controller chip command handler.  Provides
    // options to view the chip's statistics, list output port status,
    // and directly set port levels for testing.  The command can be
    // used by any PWM output chip by supplying an owner context that
    // provides details on the chip class.
    static void Command_pwmchip(const ConsoleCommandContext *ctx);
};
