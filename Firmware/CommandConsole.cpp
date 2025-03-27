// Pinscape Pico - Command Console
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <alloca.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <stdarg.h>
#include <vector>
#include <set>
#include <map>

// Pico SDK headers
#include <pico.h>
#include <pico/stdlib.h>
#include <pico/unique_id.h>

// local project headers
#include "Version.h"
#include "Utils.h"
#include "Pinscape.h"
#include "JSON.h"
#include "Logger.h"
#include "Reset.h"
#include "Outputs.h"
#include "Watchdog.h"
#include "CommandConsole.h"


// add a command table entry
void CommandConsole::AddCommand(const char *name, const char *desc, const char *usage, ExecFunc *exec, void *ownerContext)
{
    // add an entry to the main name map
    GetCommandTable().emplace(
        std::piecewise_construct,
        std::forward_as_tuple(name),
        std::forward_as_tuple(name, desc, usage, exec, ownerContext));
}

// add a generic PWM chip command handler
void CommandConsole::AddCommandPWMChip(const char *commandName, const char *commandDesc, const PWMChipClass *classDesc)
{
    // Add our genneric PWM chip command handler, with the class descriptor as the owner context.
    // Note that it's safe to remove the 'const' from the class desc pointer, because the general
    // command handler mechanism treats this as an opaque pointer that it just preserves to pass
    // to the 'exec' callback, and our exec callback treats the object as const.  The owner context
    // isn't const in general because we want to allow other callbacks to treat it as writable if
    // needed; we just don't need a writable context for this particular usage, so we declare it
    // const here, and only use it as const; it only needs to be converted to a generic void* for
    // the purposes of storing in the generic command list.
    AddCommand(commandName, commandDesc, nullptr, Command_pwmchip, const_cast<PWMChipClass*>(classDesc));
}

CommandConsole::CommandConsole()
{
    // on our first instantiation only, add our command handlers
    // to the static global table
    static bool inited = false;
    if (!inited)
    {
        AddCommand(
            "help", "list all commands, or show a command's usage details (\"help <command>\")",
            "help [<command>]\n"
            "  <command>  - name of a command; shows usage details for the given command\n"
            "\n"
            "With no arguments, 'help' shows a list of available commands.",
            &CommandConsole::Command_help);
            
        inited = true;
    }
}

// configure from the given parent key
bool CommandConsole::Configure(const char *descName, const JSONParser::Value *val)
{
    // disabled
    if (val->IsUndefined())
    {
        // enable by default
        return Configure(descName, true, 8192, 256);
    }
    else
    {
        // enable/disable by JSON settings
        return Configure(
            descName, 
            val->Get("enable")->Bool(true),
            val->Get("bufSize")->Int(-1),
            val->Get("historySize")->Int(-1));
    }
}

bool CommandConsole::Configure(const char *descName, bool enabled, int bufSize, int histSize)
{
    this->enabled = enabled;
    if (enabled)
    {
        // get the output buffer size
        bufSize = std::max(bufSize, 8192);
        bufSize = std::min(bufSize, 32767);
        outputBuf.resize(bufSize);

        // get the command history buffer size
        histSize = std::max(histSize, 256);
        histSize = std::min(histSize, 2048);
        history.resize(histSize);
        
        // start the first command prototype in the history
        AddHistory("");
    }

    // done; log it and return the enabled status
    Log(LOG_CONFIG, "%s console %sabled, output buffer size %d, history size %d\n",
        descName, enabled ? "en" : "dis", static_cast<int>(outputBuf.size()), static_cast<int>(history.size()));
    return enabled;
}

// perform period tasks
void CommandConsole::Task()
{
    // check for a continuation
    if (continuation != nullptr)
    {
        // invoke the continuation; remove it if it says it's done by returning false
        if (!continuation->Continue(this))
        {
            // clear the continuation object
            continuation.reset();

            // show a new prompt if we're in the foreground
            if (foregroundMode)
                PutOutputStr(prompt);

            // process pending input
            ProcessPendingInput();
        }
    }
}

// process input from the PC
void CommandConsole::ProcessInputChar(char c)
{
    // we can't process any input while an asynchronous command is still running
    if (continuation != nullptr)
    {
        // process Ctrl+C out of band
        if (c == 0x03)
        {
            // cancel the command
            continuation->OnUserCancel();
            continuation.reset();
            
            // clear any prior buffered data
            inBufCnt = 0;
            inBufWrite = 0;

            // if in the foreground, show ^C and a new prompt
            if (foregroundMode)
            {
                PutOutputStr("^C\n");
                PutOutputStr(prompt);
            }
        }
        else
        {
            // buffer the input, for processing after the current command finishes
            inBuf[inBufWrite++] = c;
            if (inBufWrite >= _countof(inBuf)) inBufWrite = 0;
            if (++inBufCnt >= _countof(inBuf)) inBufCnt = _countof(inBuf);
        }

        // do no further processing
        return;
    }

    // if there's any buffered text, process it first
    ProcessPendingInput();

    // now process this character
    ProcessInputCharInternal(c);
}

void CommandConsole::ProcessPendingInput()
{
    if (inBufCnt != 0)
    {
        // get the starting read pointer
        int r = inBufWrite - inBufCnt;
        if (r < 0) r += _countof(inBuf);

        // process one character at a time
        for ( ; inBufCnt != 0 ; --inBufCnt)
        {
            // process this character
            ProcessInputCharInternal(inBuf[r]);

            // advance the read pointer
            if (++r > _countof(inBuf))
                r = 0;
        }
    }
}

// Internal processing for one input character.  This is called on
// live and buffered input to process one character at a time.
void CommandConsole::ProcessInputCharInternal(char c)
{
    // if we're processing an escape sequence, and we timed out, stop here
    if (escState != EscState::None && time_us_64() >= escTimeout)
        ProcessEscape(0);

    // process escape input sequences
    switch (escState)
    {
    case EscState::None:
    NoEscape:
        //
        // Not processing an escape - it's a normal key
        //
        switch (c)
        {
        case '\r':
        case '\n':
            // Enter/return - enter the command
            ProcessCommand();
            
            // done
            break;

        case 8:
        case 127:
            // backspace/delete - delete left
            DelLeft();
            break;

        case 9:
            // Tab - command completion
            AutoComplete();
            break;

        case 1:
            // Ctrl+A - home
            MoveHome();
            break;

        case 2:
            // Ctrl+B - left
            MoveLeft();
            break;

        case 3:
            // Ctrl+C - cancel continuation command in progress
            // empty the command buffer
            cmdLen = 0;
            cmdBuf[0] = 0;
            col = 0;

            // if in the foreground, echo a literal "^C" sequence, end the line,
            // and display a new prompt
            if (foregroundMode)
            {
                PutOutputStr("^C\n");
                PutOutputStr(prompt);
            }
            break;

        case 4:
            // Ctrl+D - delete in place
            DelChar();
            break;

        case 5:
            // Ctrl+E - end of line
            MoveEOL();
            break;

        case 6:
            // Ctrl+F - right
            MoveRight();
            break;

        case 11:
            // Ctrl+K - delete to end of line
            DelEOL();
            break;

        case 14:
            // Ctrl+N - next history item
            HistNext();
            break;

        case 16:
            // Ctrl+P - previous history item
            HistPrev();
            break;

        case 21:
            // Ctrl+U - delete line
            DelLine();
            break;

        case 27:
            // Escape - start an escape input sequence
            escState = EscState::Esc;

            // set a timeout in case we don't get a well-formed escape sequence promptly
            escTimeout = time_us_64() + 100000;
            break;

        default:
            // add ordinary printable characters to the buffer
            if (c >= 32 && c <= 126)
            {
                // make sure it fits, leaving space for a null terminator
                if (cmdLen + 2 < _countof(cmdBuf))
                {
                    // append or insert the new character
                    if (col == cmdLen)
                    {
                        // at end - just append it
                        cmdBuf[cmdLen++] = c;
                        cmdBuf[cmdLen] = 0;
                        ++col;

                        // echo it
                        if (foregroundMode)
                            PutOutputChar(c);
                    }
                    else
                    {
                        // in the middle - insert it
                        memmove(&cmdBuf[col + 1], &cmdBuf[col], cmdLen + 1 - col);
                        ++cmdLen;
                        cmdBuf[col++] = c;

                        // redraw the rest of the command
                        if (foregroundMode)
                        {
                            // erase to end of screen + redisplay rest of line
                            PutOutputFmt("\033[0J%s", &cmdBuf[col-1]);

                            // back up, if not at end of line
                            if (int b = DisplayCol(cmdLen) - DisplayCol(col); b > 0)
                                PutOutputFmt("\033[%dD", b);
                        }
                    }
                }
            }
            break;
        }
        break;

    case EscState::Esc:
        // the next character must be '[' to stay in escape mode; discard anything else
        if (c == '[')
        {
            // advance to the code section
            escState = EscState::Code;

            // clear the code accumulators
            escAcc[0] = escAcc[1] = escAcc[2] = 0;
            escAccIndex = 0;
        }
        else
        {
            // invalid escape - ignore it, and treat the new character as an ordinary key
            escState = EscState::None;
            goto NoEscape;
        }
        break;

    case EscState::Code:
        // processing a code - all input in this phase must be numeric; ';' ends the code
        // and advances to the next code; anything else ends the sequence
        if (c == ';')
        {
            // advance to the next code section
            escAccIndex = std::max(escAccIndex + 1, 2);
        }
        else if (isdigit(c))
        {
            // accumulate it into the code
            auto &acc = escAcc[escAccIndex];
            acc *= 10;
            acc += c - '0';
        }
        else
        {
            // anything else ends the escape
            ProcessEscape(c);
        }
        break;
    }
}

void CommandConsole::AutoComplete()
{
    char qu = 0;
    auto NextToken = [this, &qu](char *tokBuf, size_t tokBufLen, int startCol, int endCol, int &tokCol)
    {
        // skip spaces
        int i = startCol;
        char *p = &cmdBuf[i];
        for ( ; i < cmdLen && isspace(*p) ; ++p, ++i) ;
        
        // this is the start of the next token
        tokCol = i;
        
        // check if we're at the end of the line
        if (i == cmdLen)
        {
            // no more tokens
            tokBuf[0] = 0;
            return i;
        }
        
        // scan this token
        char *tokp = tokBuf;
        char *tokEnd = tokBuf + tokBufLen - 1;
        for (qu = 0 ; i < endCol ; ++p, ++i)
        {
            char ch = *p;
            if (ch == '\\')
            {
                // copy the next character literally if not at end of line
                if (i+1 < cmdLen)
                {
                    ++p;
                    ++i;
                    if (tokp < tokEnd)
                        *tokp++ = *p;
                }
            }
            else if (qu != 0)
            {
                // in a quoted section
                if (ch == qu)
                {
                    // end the quoted section, omit the quote from the token
                    qu = 0;
                }
                else
                {
                    // copy the character to the token under construction
                    if (tokp < tokEnd)
                        *tokp++ = ch;
                }
            }
            else
            {
                // check for a quote
                if (ch == '"' || ch == '\'')
                {
                    // start a quoted section; don't include the quote in the token
                    qu = ch;
                }
                else if (isspace(*p))
                {
                    // end of token
                    break;
                }
                else
                {
                    // copy the character
                    if (tokp < tokEnd)
                        *tokp++ = ch;
                }
            }
        }
        
        // end the token
        *tokp = 0;
        
        // return the new position
        return i;
    };

    auto CompleteWith = [this, &NextToken](const std::list<const char*> &matches, int tokStart)
    {
        if (matches.size() == 0)
        {
            // no match - just beep
            PutOutputStr("\007");
        }
        else if (matches.size() == 1)
        {
            // single match - apply the replacement

            // erase the current line from the screen
            PutOutputFmt("\033[%dD\033[0J", DisplayCol(col));

            // re-scan the current token to get its whole extent, including quotes
            char tok[64];
            int tokEnd = NextToken(tok, sizeof(tok), tokStart, cmdLen, tokStart);
            
            // delete the original token from the buffer
            if (tokEnd > tokStart)
            {
                memmove(&cmdBuf[tokStart], &cmdBuf[tokEnd], cmdLen - tokEnd + 1);
                cmdLen -= tokEnd - tokStart;
                col = tokStart;
            }

            // get the new token
            const char *newTok = matches.front();
            size_t newTokLen = strlen(newTok);

            // if there's not already a space after the token, add one
            size_t insLen = newTokLen;
            bool addSpace = false;
            if (tokStart == cmdLen)
            {
                addSpace = true;
                insLen += 1;
            }

            // insert the new text
            if (cmdLen + insLen + 1 < sizeof(cmdBuf))
            {
                // make space
                memmove(&cmdBuf[tokStart + insLen], &cmdBuf[tokStart], cmdLen - tokStart + 1);
                cmdLen += insLen;
                
                // insert the text
                char *dst = &cmdBuf[tokStart];
                memcpy(dst, newTok, newTokLen);
                dst += newTokLen;

                // add a space if needed
                if (addSpace)
                    *dst++ = ' ';
            }

            // move to the end of the inserted text; if we didn't add a space,
            // also advance past the existing space
            col += insLen;
            if (!addSpace)
                col += 1;

            // clear and re-print the line
            RePrintCurCommand();
        }
        else
        {
            // multiple matches - display the possible matches, without doing any replacement
            PutOutputStr("\n");
            int n = 0;
            for (auto &m : matches)
            {
                PutOutputFmt("%s", m);
                if (n++ >= 4)
                {
                    PutOutputStr("\n");
                    n = 0;
                }
                else
                {
                    PutOutputStr(strlen(m) < 8 ? "\t\t" : "\t");
                }
            }
            if (n != 0)
                PutOutputStr("\n");

            // recall the command
            RePrintCurCommand();
        }
    };

    // get the command token
    char cmdTok[32];
    int cmdTokCol;
    int i = NextToken(cmdTok, sizeof(cmdTok), 0, col, cmdTokCol);

    // if the cursor is within or just after the command token, complete the command
    auto &cmdTab = GetCommandTable();
    if (col <= i)
    {
        // look up the command
        size_t l = strlen(cmdTok);
        std::list<const char*> matches;
        for (const auto &c : cmdTab)
        {
            if (c.first.size() >= l && memcmp(c.first.c_str(), cmdTok, l) == 0)
                matches.emplace_back(c.first.c_str());
        }
        CompleteWith(matches, cmdTokCol);
        return;
    }

    // look up the command
    auto it = cmdTab.find(cmdTok);
    if (it == cmdTab.end())
    {
        // we can't do any completion if we don't have a valid command;
        // just beep and return
        PutOutputStr("\007");
        return;
    }

    // scan ahead until we find the token the cursor is in
    for (;;)
    {
        // scan the next token
        char tok[64];
        int tokCol;
        i = NextToken(tok, sizeof(tok), i, col, tokCol);

        // stop if we've reached the cursor column
        if (col <= i)
        {
            // parse the usage message for the command
            std::set<std::string> opts;
            size_t tokLen = strlen(tok);
            for (const char *p = it->second.usage ; *p != 0 ; )
            {
                // skip spaces
                for ( ; isspace(*p) ; ++p) ;

                // check for an option starter
                while (*p == '-')
                {
                    // scan for a space, comma, '=', ':', or end of line
                    const char *start = p;
                    for ( ; *p != 0 && strchr(" \t\n,=:", *p) == nullptr ; ++p) ;

                    // filter for leading substrings
                    size_t optLen = p - start;
                    if (optLen >= tokLen && memcmp(tok, start, tokLen) == 0)
                    {
                        // it's a match - add it to the map of possible completions
                        opts.emplace(start, optLen);
                    }

                    // check for another option following a comma
                    for ( ; *p != 0 && *p != ',' && *p != '\n' ; ++p) ;
                    if (*p == ',' && *(p+1) == ' ')
                        p += 2;
                }

                // skip to end of line
                for ( ; *p != 0 && *p != '\n' ; ++p) ;
                if (*p == '\n')
                    ++p;
            }

            // perform the replacement
            std::list<const char*> optsPtrs;
            for (auto &o : opts)
                optsPtrs.emplace_back(o.c_str());
            CompleteWith(optsPtrs, tokCol);

            // done
            return;
        }
    }
}

void CommandConsole::DelChar()
{
    if (col < cmdLen)
    {
        // update it visually
        if (foregroundMode)
        {
            // del end of screen, redisplay rest of line
            PutOutputFmt("\033[0J%s", &cmdBuf[col + 1]);
        }

        // close the gap
        memmove(&cmdBuf[col], &cmdBuf[col + 1], strlen(&cmdBuf[col]));
        cmdLen -= 1;

        // finish the visual update
        if (foregroundMode)
        {
            // if not at end of line, back up to the cursor location
            if (int b = DisplayCol(cmdLen) - DisplayCol(col); b > 0)
                PutOutputFmt("\033[%dD", b);
        }
    }
}

void CommandConsole::DelLeft()
{
    if (col > 0)
    {
        // update it visually
        if (foregroundMode)
        {
            // back up one character, del to end of screen, redisplay rest of line
            PutOutputFmt(
                "\033[%dD\033[0J%s",
                DisplayCol(col) - DisplayCol(col - 1),
                &cmdBuf[col]);
        }
        
        // back up one character in the command buffer
        --col;
        --cmdLen;
        
        // close the gap
        memmove(&cmdBuf[col], &cmdBuf[col + 1], strlen(&cmdBuf[col]));

        // finish the visual update
        if (foregroundMode)
        {
            // if not at end of line, back up to the cursor location
            if (int b = DisplayCol(cmdLen) - DisplayCol(col); b > 0)
                PutOutputFmt("\033[%dD", b);
        }
    }
}

void CommandConsole::DelLine()
{
    if (cmdLen != 0)
    {
        if (foregroundMode)
        {
            if (col != 0)
                PutOutputFmt("\033[%dD\033[0J", DisplayCol(col) - DisplayCol(0));
            else
                PutOutputStr("\033[0J");
        }

        cmdLen = 0;
        cmdBuf[0] = 0;
        col = 0;
    }
}

void CommandConsole::DelEOL()
{
    if (col < cmdLen)
    {
        if (foregroundMode)
            PutOutputStr("\033[0J");

        cmdLen = col;
        cmdBuf[cmdLen] = 0;
    }
}

void CommandConsole::MoveLeft()
{
    if (col > 0)
    {
        if (foregroundMode)
            PutOutputFmt("\033[%dD", DisplayCol(col) - DisplayCol(col - 1));
        --col;
    }
}

void CommandConsole::MoveRight()
{
    if (col < cmdLen)
    {
        if (foregroundMode)
            PutOutputFmt("\033[%dC", DisplayCol(col + 1) - DisplayCol(col));
        ++col;
    }
}

void CommandConsole::MoveHome()
{
    if (col != 0)
    {
        if (foregroundMode)
            PutOutputFmt("\033[%dD", DisplayCol(col) - DisplayCol(0));
        col = 0;
    }
}

void CommandConsole::MoveEOL()
{
    if (col != cmdLen)
    {
        if (foregroundMode)
            PutOutputFmt("\033[%dC", DisplayCol(cmdLen) - DisplayCol(col));
        col = cmdLen;
    }
}

void CommandConsole::ProcessEscape(char lastChar)
{
    // exit escape parsing mode
    escState = EscState::None;

    // If we have two numeric codes, the first is the key code, and the
    // second is the modifier.  If we have one numeric code, it's the
    // key code if it's a VT sequence (ending in '~'), or the modifier
    // for xterm sequences.
    int modifier = 1;
    int keycode = 0;
    if (escAccIndex == 0)
        (lastChar == '~' ? keycode : modifier) = escAcc[0];
    else
        modifier = escAcc[1], keycode = escAcc[0];

    // get the modifier flags (excess 1)
    if (modifier > 0) modifier -= 1;
    bool shift = (modifier & 0x01) != 0;
    bool alt = (modifier & 0x02) != 0;
    bool ctrl = (modifier & 0x04) != 0;
    bool meta = (modifier & 0x08) != 0;

    // let's see what kind of sequence we have
    switch (lastChar)
    {
    case '~':
        // It's a VT-100 sequence, so the numeric keycode gives us the key
        switch (keycode)
        {
        case 1:
            // Home
            MoveHome();
            break;

        case 3:
            // Delete
            DelChar();
            break;

        case 4:
            // End
            MoveEOL();
            break;

        case 7:
            // Home
            MoveHome();
            break;
        }
        break;

    case 'A':
        // Up arrow
        HistPrev();
        break;

    case 'B':
        // Down arrow
        HistNext();
        break;

    case 'C':
        // Right arrow
        MoveRight();
        break;

    case 'D':
        // Left arrow
        MoveLeft();
        break;

    case 'F':
        // End
        MoveEOL();
        break;

    case 'H':
        // Home
        MoveHome();
        break;
    }
}


void CommandConsole::ProcessInputStr(const char *str)
{
    while (*str != 0)
        ProcessInputChar(*str++);
}

void CommandConsole::ProcessInputStr(const char *str, size_t len)
{
    while (len-- != 0)
        ProcessInputChar(*str++);
}

void CommandConsole::PutOutputStr(const char *str)
{
    while (*str != 0)
        PutOutputChar(*str++);
}

void CommandConsole::PutOutputStr(const char *str, size_t len)
{
    while (len-- != 0)
        PutOutputChar(*str++);
}

void CommandConsole::PutOutputChar(char c)
{
    // translate \n -> \r\n
    if (c == '\n')
        PutOutputCharRaw('\r');
    PutOutputCharRaw(c);
}

void CommandConsole::PutOutputCharRaw(char c)
{
    // add this character
    if (enabled)
    {
        outputBuf[PostInc(outputWrite)] = c;
        if (outputWrite == outputRead)
            PostInc(outputRead);
    }
}


void CommandConsole::Print(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    PutOutputFmtV(fmt, va);
    va_end(va);
}

void CommandConsole::PutOutputFmt(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    PutOutputFmtV(fmt, va);
    va_end(va);
}

void CommandConsole::PutOutputFmtV(const char *fmt, va_list va)
{
    // Measure the size of the formatted string
    va_list vaCount;
    va_copy(vaCount, va);
    int len = vsnprintf(nullptr, 0, fmt, vaCount);
    va_end(vaCount);

    // if successful, format the string into a buffer
    if (len > 0)
    {
        // allocate space on the stack for the text plus a null terminator
        char *buf = static_cast<char*>(alloca(len + 1));
        if (buf != nullptr)
        {
            // format the text and add it to the ring buffer
            vsnprintf(buf, len + 1, fmt, va);
            PutOutputStr(buf);
        }
    }
    else
    {
        // unable to format it - just write the raw string
        PutOutputStr(fmt);
    }
}

void CommandConsole::HistNext()
{
    // do nothing if we're at the zeroeth command
    if (histIndex == 0)
        return;

    // Find the next newer command, and do nothing if it doesn't exist.
    // It should be impossible for it NOT to exist, because (a) it had
    // to exist for us to get where we are now in the history via
    // HistPrev() and (b) we don't modify the history buffer when off
    // of the 0th command.  But it's cheap to sanity-check it.
    int i = FindHistory(histIndex - 1);
    if (i < 0)
        return;

    // update the history index
    histIndex -= 1;

    // load the new command
    LoadFromHistory(i);

    // if we're back at the zeroeth command, restore the original
    // editing column
    if (histIndex == 0)
    {
        // if in the foreground, move the cursor from end of line
        // to the target column
        if (foregroundMode && histOldCol < col)
            PutOutputFmt("\033[%dD", DisplayCol(col) - DisplayCol(histOldCol));

        // restore the column counter
        col = histOldCol;
    }
}

void CommandConsole::HistPrev()
{
    // If we're on the zeroeth command, save the current command in the
    // history buffer (in navigation mode, meaning that we save it even
    // if it's empty or redundant, since we want to be able to come back
    // here even in those cases).  The save replaces the prior prototype
    // command that's already in the buffer.  Also save the editing
    // column, so that we can restore things just like they were if we
    // end up coming back here.
    if (histIndex == 0)
    {
        SaveHistory(true);
        histOldCol = col;
    }

    // Find the next older command, and do nothing if it doesn't
    // exist, meaning that we've reached the oldest command still
    // present in the ring buffer.
    int i = FindHistory(histIndex + 1);
    if (i < 0)
        return;

    // advance to the next newer command
    histIndex += 1;

    // load the new command
    LoadFromHistory(i);
}

void CommandConsole::LoadFromHistory(int bufferIndex)
{
    // if we're in foreground mode, erase the current command from the screen
    if (foregroundMode)
    {
        if (col != 0)
            PutOutputFmt("\033[%dD\033[0J", DisplayCol(col) - DisplayCol(0));
        else
            PutOutputStr("\033[0J");
    }

    // reset the command buffer
    cmdLen = 0;

    // copy the command from the history to the command buffer
    for (int i = bufferIndex ; cmdLen < _countof(cmdBuf) - 1 && history[i] != 0 ; i = (i + 1) % history.size())
        cmdBuf[cmdLen++] = history[i];

    // null-terminate the buffer
    cmdBuf[cmdLen] = 0;

    // put the cursor at the last column of the new command
    col = cmdLen;

    // if we're in the foreground, display the new command
    if (foregroundMode)
        PutOutputStr(cmdBuf);
}

void CommandConsole::SaveHistory(bool forNavigation)
{
    // A prototype entry for the current command is always in the
    // history as soon as we start a new command, so start by seeking
    // the start of the zeroeth command and resetting the write pointer
    // there.  This will effectively delete the prototype command from
    // the buffer, so that the final command replaces it.
    if (int i = FindHistory(0) ; i >= 0)
        histWrite = i;

    // if this isn't for navigation purposes, prune empty and redundant
    // commands, to keep the history easier to navigate
    if (!forNavigation)
    {
        // if the new command is empty, don't bother saving it
        if (cmdLen == 0)
            return;
        
        // if the new command exactly matches the previous command, don't
        // bother saving another copy
        if (int i = FindHistory(0) ; i >= 0)
        {
            // compare the command
            bool matches = true;
            for (const char *p = cmdBuf ; ; ++p, i = (i + 1) % history.size())
            {
                // stop on a mismatch
                if (*p != history[i])
                {
                    matches = false;
                    break;
                }

                // Stop when we reach the end of the string.  Note that
                // both strings necessarily end here if either one does,
                // since we've already bailed out if the characters
                // don't match.
                if (*p == 0)
                    break;
            }
            
            if (matches)
                return;
        }
    }

    // Now add the new command to the history
    AddHistory(cmdBuf);
}

void CommandConsole::AddHistory(const char *buf)
{
    for (const char *p = buf ; ; ++p)
    {
        // add the character, wrap the history pointer
        history[histWrite++] = *p;
        histWrite %= history.size();

        // check for collision with the read pointer
        if (histWrite == histRead)
        {
            // delete the entire command here, stopping when we reach a null byte
            while (history[histRead] != 0)
            {
                // discard a character
                histRead = (histRead + 1) % history.size();

                // stop if this leaves the buffer completely empty
                if (histRead == histWrite)
                    break;
            }

            // if we didn't exhaust the buffer, skip the null byte, so that
            // the final read pointer is at the start of the next command
            // after the one we just discarded
            if (histRead != histWrite)
                histRead = (histRead + 1) % history.size();
        }

        // stop after writing the null (AFTER writing the null, not
        // upon encountering the null, since the null goes into the
        // history along with the rest of the command string)
        if (*p == 0)
            break;
    }
}

int CommandConsole::FindHistory(int n)
{
    // if the history is completely empty, there's nothing to find
    if (histWrite == histRead)
        return -1;

    // The write index always points to the byte after the null
    // at the end of the new command in the buffer.  Back up
    // onto the null as the starting point.
    int i = histWrite;
    if (--i < 0)
        i = history.size() - 1;

    // Now work backwards, seeking the start of each comamnd by finding
    // the null byte at the end of prior command.  
    for (;;)
    {
        // if we've reached the read pointer, this is the start of
        // the first command in the buffer
        if (i == histRead)
        {
            // if this is the command we're looking for, return it,
            // otherwise the command we're seeking doesn't exist
            return n == 0 ? i : -1;
        }
        
        // back up
        if (--i < 0)
            i = history.size() - 1;

        // if this is a null, or we're at the read pointer (which is the
        // start of the first command in the ring), we've found the
        // start of the next command
        if (history[i] == 0)
        {
            // if n == 0, this is the command we're looking for
            if (n == 0)
            {
                // this null is the end of the prior command, so the command
                // we're seeking starts at the next character
                return (i + 1) % history.size();
            }

            // decrement n and keep looking
            --n;
        }
    }
}

void CommandConsole::ProcessCommand()
{
    // If we're in editing mode, end the editing session visually, by
    // moving the cursor to end of line and writing a newline.  If we're
    // in background mode, the command isn't currently showing, so
    // there's nothing to display.
    if (foregroundMode)
    {
        if (col < cmdLen)
            PutOutputFmt("\033[%dC\n", DisplayCol(cmdLen) - DisplayCol(col));
        else
            PutOutputChar('\n');
    }
    
    // save the command to the history buffer
    SaveHistory(false);

    // Parse the command.  This parses the buffer in-place into tokens,
    // with a limit on the number of tokens, to keep memory usage small
    // and deterministic.
    const char *argv[32];
    int argc = 0;
    char *src = cmdBuf;
    for ( ; *src != 0 && argc < _countof(argv) ; )
    {
        // skip spaces
        for ( ; isspace(*src) ; ++src) ;

        // stop at the null
        if (*src == 0)
            break;

        // if the first token starts with '#', the whole line is a comment
        if (*src == '#' && argc == 0)
        {
            // skip to the end of the line and stop parsing
            for ( ; *src != 0 ; ++src) ;
            break;
        }

        // start a new token
        argv[argc++] = src;

        // Parse the token for quotes and escapes.  We rewrite the token
        // in-place in the source buffer - this is possible because we only
        // delete characters, so each character will either stay where it
        // is or move left, which won't destroy anything later in the buffer.
        char *dst = src;
        for (char qu = 0 ; *src != 0 ; )
        {
            // check for special characters
            char ch = *src++;
            if (ch == '\\')
            {
                // escape - copy the next character literally if it's not null
                if (*src != 0)
                    *dst++ = *src++;
            }
            else if (qu != 0)
            {
                // inside a quoted section - copy everything, including spaces,
                // until we find the matching quote
                if (ch == qu)
                {
                    // end of quoted section
                    qu = 0;
                }
                else
                {
                    // copy the character
                    *dst++ = ch;
                }
            }
            else
            {
                // not in a quote section - any space ends the token; a quote
                // starts a new quoted section
                if (isspace(ch))
                {
                    // space outside of a quoted section - end of token
                    break;
                }
                else if (ch == '"' || ch == '\'')
                {
                    // new quoted section starts here - note the quote and omit it
                    // from the destination string
                    qu = ch;
                }
                else
                {
                    // ordinary character - copy it to the destination
                    *dst++ = ch;
                }
            }
        }

        // null-terminate the new token
        *dst = 0;
    }

    // warn if we have extra text past the last token (which means there
    // are more tokens than we can accommodate in our fixed-size argv[])
    if (*src != 0)
    {
        PutOutputFmt("\033[31;1mWarning: this command is too complex; text discarded after \"%.10s\"\033[0m\n",
            src, strlen(src) > 10 ? "..." : "");
    }

    // process the command
    ExecCommand(argc, argv);

    // reset to an empty command buffer
    cmdLen = 0;
    col = 0;
    cmdBuf[0] = 0;
    histIndex = 0;
    
    // start a new empty command in the history, for the next command entry
    AddHistory("");

    // display the next prompt if we're in the foreground and there's no
    // continuation handler in effect
    if (foregroundMode && continuation == nullptr)
        PutOutputStr(prompt);
}

void CommandConsole::ExecCommand(int argc, const char **argv)
{
    // ignore empty commands
    if (argc == 0)
        return;

    // translate commands of the form "<name> -?" or "<name> --help" to "help <name>"
    const char *helpArgv[2] = { "help", argv[0] };
    if (argc == 2 && (strcmp(argv[1], "-?") == 0 || strcmp(argv[1], "--help") == 0))
        argv = helpArgv;

    // look up the command
    auto &t = GetCommandTable();
    if (auto it = t.find(argv[0]) ; it != t.end())
    {
        // construct the execution context
        ConsoleCommandContext ctx{ this, argc, argv, it->second.usage, it->second.exec, it->second.ownerContext };

        // execute the command
        it->second.exec(&ctx);
    }
    else
    {
        // not found
        PutOutputFmt("\033[31;1m%s: no such command (type 'help' for a list of available commands)\033[0m\n", argv[0]);
    }
}

int CommandConsole::CopyOutput(std::function<int(const uint8_t *dst, int len)> func, int len)
{
    // get the part from read to end of buffer
    int total = 0;
    if (outputRead > outputWrite)
    {
        // copy this section, up to the length requested
        int avail = outputBuf.size() - outputRead;
        int copy = std::min(avail, len);
        int actual = func(outputBuf.data() + outputRead, copy);

        // advance pointers
        PostInc(outputRead, actual);
        len -= actual;
        total += actual;

        // if the write was short, stop here - we presumably won't
        // be able to write any more on this round
        if (actual < avail)
            return total;
    }

    // get the part from the beginning of the buffer to write
    if (outputRead < outputWrite)
    {
        // copy this section, up to the remaining length requested
        int avail = outputWrite - outputRead;
        int copy = std::min(avail, len);
        int actual = func(outputBuf.data() + outputRead, copy);

        // advance pointers
        PostInc(outputRead, actual);
        len -= actual;
        total += actual;
    }

    // return the total length copied
    return total;
}

void CommandConsole::SetForegroundMode(bool fg)
{
    // do nothing if the mode isn't changing
    if (fg == foregroundMode)
        return;

    // make the appropriate mode switch
    foregroundMode = fg;
    if (fg)
    {
        // if this is the first time in the foreground, show our little
        // banner to let the user know the console is ready
        if (!firstPromptDisplayed)
        {
            // first prompt -> show a little banner to let them know this is a console
            ShowBanner();
            firstPromptDisplayed = true;
        }

        // restore the editing session in progress
        RePrintCurCommand();
    }
    else
    {
        // end the editing session - back up to the start of the line
        // and erase to end of screen
        PutOutputFmt("\033[%dD\033[0J", DisplayCol(col));
    }
}

void CommandConsole::SetConnected(bool f)
{
    // nothing to do if the status isn't changing
    if (f == connected)
        return;

    // set the new status
    connected = f;

    // if reconnecting after being disconnected, reset our console session
    if (connected)
    {
        foregroundMode = false;
        firstPromptDisplayed = false;
    }
}

void CommandConsole::RePrintCurCommand()
{
    // restore the editing session in progress: write the prompt,
    // the command line, and cursor-left to get us back to the
    // saved cursor column
    PutOutputStr(prompt);
    PutOutputStr(cmdBuf);
    if (col < cmdLen)
        PutOutputFmt("\033[%dD", DisplayCol(cmdLen) - DisplayCol(col));
}

// Calculate the display column for the given portion of the command
// buffer, taking into account that some characters (such as tabs and
// escape sequences) occupy more or less than one column.
int CommandConsole::DisplayCol(int len)
{
    static const auto CalcBasic = [](const char *s, int len, int startCol)
    {
        // start at the starting column
        int col = startCol;

        // scan the text
        for ( ; len != 0 ; ++s, --len)
        {
            // check for escape sequences
            if (*s == '\033')
            {
                // Escape sequence.  The only escapes are in the prompt, and
                // they just set colors without changing the column.  Simply
                // scan ahead to the end of the escape, which is the next
                // alpha character.
                for ( ; len > 1 && !isalpha(*s) ; ++s, --len) ;
            }
            else if (*s == '\t')
            {
                // tab - advance one character, then to the next multiple of 8
                ++col;
                while ((col & 0x07) != 0)
                    ++col;
            }
            else
            {
                // everything else is one column
                ++col;
            }
        }

        // return the final column
        return col;
    };
    
    // count columns in the prompt, then count from there to the desired point
    // in the command buffer
    int col = CalcBasic(prompt, static_cast<int>(strlen(prompt)), 0);
    return CalcBasic(cmdBuf, len, col);
}

// ---------------------------------------------------------------------------
//
// Command handlers
//

void CommandConsole::Command_help(const ConsoleCommandContext *c)
{
    // check arguments
    auto &t = GetCommandTable();
    if (c->argc == 1)
    {
        // Show the command summary.  Note that the commands are stored in
        // a sorted map with the command names as keys, so we get a nice
        // alphabetically sorted list just by iterating over the keys.
        c->Printf(
            "Command summary:\n\n"
            "   # <comment>       ignored; a line starting with '#' is a comment\n");
        for (auto &ele : t)
            c->Printf("   %-16s  %s\n", ele.second.name, ele.second.desc);
        c->Print("\n");
    }
    else
    {
        for (int i = 1 ; i < c->argc ; ++i)
        {
            if (auto it = t.find(c->argv[i]) ; it != t.end())
            {
                // show the command name and description
                c->Printf("%s: %s\n", it->second.name, it->second.desc);

                // show the usage
                const char *subArgv[2] = { c->argv[1], "--help" };
                ConsoleCommandContext subctx{ c->console, 2, subArgv, it->second.usage, it->second.exec, it->second.ownerContext };
                subctx.Usage();
            }
            else
            {
                c->Printf("%s: no such command\n", c->argv[i]);
            }
            c->Print("\n");
        }
    }
}

// ---------------------------------------------------------------------------
//
// Generic command handler for PWM output controller chips
//

void CommandConsole::Command_pwmchip(const ConsoleCommandContext *c)
{
    // show usage
    static const auto Usage = [](const ConsoleCommandContext *c)
    {
        // get the chip class description
        auto *cls = reinterpret_cast<const PWMChipClass*>(c->ownerContext);
        
        // headline
        c->Printf(
            "usage: %s [options] <port>=<level>\n"
            "options:\n"
            "\n", c->argv[0]);

        // show chip/chain/instance select options
        for (const char *p = cls->selectOpt ; ; )
        {
            const char *opt = p;
            p = strchr(p, '\t');
            if (p == nullptr)
                break;
            int optLen = p - opt;

            const char *expl = ++p;
            p = strchr(p, '\n');
            if (p == nullptr)
                p = expl + strlen(expl);
            int explLen = p - expl;

            c->Print("  ");
            c->Printf("%.*s%*s%.*s\n", optLen, opt, 16 - optLen, "", explLen, expl);

            if (*p == '\n')
                ++p;
            else
                break;
        }

        // show the rest
        c->Printf(
            "  --l, --list     list port levels\n"
            "  --s, --stats    show statistics\n"
            "\n"
            "  <port>          port number, port range (e.g., 3-7), or * to set all ports\n"
            "  <level>         level to set, 0..%d\n"
            "\n"
            "Setting a port level suspends output management to allow direct chip testing.\n",
            cls->maxLevel);
    };
    
    // show diagnostics or list ports
    static auto Show = [](const ConsoleCommandContext *c, int chipNum, bool ports, bool stats)
    {
        // get the chip class description
        auto *cls = reinterpret_cast<const PWMChipClass*>(c->ownerContext);
        
        // show info on the --chip selection, or all if chip == -1
        int nListed = 0;
        for (int i = 0 ; i < cls->nInstances ; ++i)
        {
            // show the chip if it exists, and it either matches the
            // selected instance number, or there is no selected
            // instance (in which case just show everything)
            if (cls->IsValidInstance(i) && (i == chipNum || chipNum == -1))
            {
                // show stats if desired
                if (stats)
                    cls->ShowStats(c, i);

                // show port states if desired
                if (ports)
                {
                    c->Printf(
                        "%s[%d] port levels:\n"
                        "  Port  Level  Pin Name\n",
                        cls->name, i);
                    for (int port = 0, nPorts = cls->GetNumPorts(i) ; port < nPorts ; ++port)
                    {
                        c->Printf("    %2d   %4d  ", port, cls->GetPort(i, port));
                        cls->PrintPortName(c, i, port);
                        c->Print("\n");
                    }
                }

                // count the listing
                ++nListed;
            }
        }

        // if we didn't generate any diagnostics, say so, so that we don't
        // exit without any messages at all
        if (nListed == 0)
            c->Printf("No %s chips configured; check startup log for errors\n", cls->name);
    };

    // get the chip class description
    auto *cls = reinterpret_cast<const PWMChipClass*>(c->ownerContext);

    // make sure there are arguments
    if (c->argc == 1)
        return Usage(c);

    // process arguments
    int chip = -1;
    for (int i = 1 ; i < c->argc ; ++i)
    {
        // get the argument
        const char *a = c->argv[i];

        // check for chip select options
        size_t aLen = strlen(a);
        bool foundcs = false;
        for (const char *cs = cls->selectOpt ; ; )
        {
            const char *t = cs;
            for ( ; *t != 0 && *t != ' ' && *t != '\t' && *t != '\n' ; ++t) ;
            if (*t == 0)
                break;

            size_t len = t - cs;
            if (len == aLen && memcmp(a, cs, len) == 0)
            {
                // got it - it's a chip select option
                
                // make sure there's an argument
                ++i;
                if (i >= c->argc)
                    return c->Printf("Missing number after %s\n", a);

                // get the chip and validate that it's active
                chip = atoi(c->argv[i]);
                if (!cls->IsValidInstance(chip))
                    return c->Print("Invalid chip number or chip not configured; check startup log for errors\n");

                // done
                foundcs = true;
                break;
            }

            cs = strchr(t, '\n');
            if (cs == nullptr)
                break;
            ++cs;
        }
        if (foundcs)
            continue;

        // check for fixed options
        else if (strcmp(a, "-s") == 0 || strcmp(a, "--stats") == 0)
        {
            Show(c, chip, false, true);
        }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0)
        {
            Show(c, chip, true, false);
        }
        else if (strcmp(a, "-?") == 0 || strcmp(a, "--help") == 0)
        {
            Usage(c);
        }
        else if (a[0] == '-')
        {
            return c->Printf("Invalid option \"%s\"\n", a);
        }
        else
        {
            // anything else is a <port>=<level> command
            const char *eq = strchr(a, '=');
            if (eq == nullptr)
                return c->Printf("Expected <port>=<number> syntax, found \"%s\"\n", a);
            int level = atoi(eq+1);
            if (level < 0 || level > cls->maxLevel)
                return c->Printf("Level must be 0..%d\n", cls->maxLevel);

            // use chip 0 if no chip is selected (-> chip == -1)
            int effChip = chip;
            if (effChip == -1)
            {
                effChip = 0;
                if (!cls->IsValidInstance(effChip))
                    return c->Printf("%s[%d] is not configured; check startup log for errors\n", cls->name, effChip);
            }

            // suspend output management if it's not already suspended
            OutputManager::SuspendIfActive(c);

            // set all ports if desired, otherwise set the specific port
            if (a[0] == '*')
            {
                // set all ports
                for (int n = 0, nPorts = cls->GetNumPorts(effChip) ; n < nPorts ; ++n)
                    cls->SetPort(effChip, n, level);

                c->Printf("%s[%d] All -> level %d\n", cls->name, effChip, level);
            }
            else if (const char *dash = strchr(a, '-'); dash != nullptr && dash < eq)
            {
                // port range
                int first = atoi(a);
                int last = atoi(dash+1);
                if (!cls->IsValidPort(effChip, first) || !cls->IsValidPort(effChip, last) || last < first)
                    return c->Printf("Invalid port range; ports are numbered 0..%d, range must be ascending\n", cls->GetNumPorts(effChip) - 1);

                // set each port
                for (int n = first ; n <= last ; ++n)
                    cls->SetPort(effChip, n, level);

                c->Printf("%s[%d] Ports %d to %d -> level %d\n", cls->name, effChip, first, last, level);
            }
            else
            {
                // validate the port
                int port = atoi(a);
                if (!cls->IsValidPort(effChip, port))
                    return c->Printf("Invalid port number %d; must be 0..%d\n", port, cls->GetNumPorts(effChip) - 1);

                // set the level
                cls->SetPort(effChip, port, level);
                c->Printf("%s[%d] Port %d -> level %d\n", cls->name, effChip, port, level);
            }
        }
    }
}


// ---------------------------------------------------------------------------
//
// Command execution context
//

void ConsoleCommandContext::Print(const char *str) const
{
    console->PutOutputStr(str);
}

void ConsoleCommandContext::Printf(const char *fmt, ...) const
{
    va_list va;
    va_start(va, fmt);
    console->PutOutputFmtV(fmt, va);
    va_end(va);
}

void ConsoleCommandContext::VPrintf(const char *fmt, va_list va) const
{
    console->PutOutputFmtV(fmt, va);
}

void ConsoleCommandContext::Usage() const
{
    // if there's a fixed usage string, display it; otherwise call the
    // comand handler with the --help option specified
    if (usage != nullptr)
    {
        Print("usage: ");
        Print(usage);
        Print("\n");
    }
    else
    {
        const char *subArgv[2] = { argv[0], "--help" };
        ConsoleCommandContext subctx{ console, 2, subArgv, "No usage available", exec, ownerContext };
        exec(&subctx);
    }
}
