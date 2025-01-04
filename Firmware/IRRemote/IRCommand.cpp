// Pinscape Pico firmware - IR Command Descriptor
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "IRCommand.h"

// Null singleton for three-state logic class
const Bool3 Bool3::null = Bool3();

// parse a command descriptor string
bool IRCommandDesc::Parse(const char *str)
{
    // parse hex digits into an accumulator
    static const auto GetHex = [](const char* &p, uint64_t& acc)
    {
        if (*p == 0)
            return false;
        
        char c = *p++;
        if (c >= '0' && c <= '9')
            acc = (acc << 4) + (c - '0');
        else if (c >= 'a' && c <= 'f')
            acc = (acc << 4) + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            acc = (acc << 4) + (c - 'A' + 10);
        else
            return false;

        return true;
    };

    // skip leading spaces
    for ( ; isspace(*str) ; ++str) ;

    // parse the protocol
    uint64_t proId = 0;
    if (!GetHex(str, proId) || !GetHex(str, proId) || *str++ != '.')
        return false;

    // parse the flags
    uint64_t flags = 0;
    if (!GetHex(str, flags) || !GetHex(str, flags) || *str++ != '.')
        return false;

    // make sure there's at least one hex digit remaining
    if (*str == 0)
        return false;

    // parse up to 16 hex digits for the code; stop if we reach whitespace
    uint64_t code = 0;
    for (int i = 0 ; i < 16 && *str != 0 && !isspace(*str) ; ++i)
    {
        if (!GetHex(str, code))
            return false;
    }

    // make sure there's nothing but whitespace after
    for ( ; isspace(*str) ; ++str) ;
    if (*str != 0)
        return false;

    // success - load the fields
    this->proId = static_cast<uint8_t>(proId);
    this->code = code;
    this->useDittos = (flags & 0x02) != 0;
    return true;
}

// generate a command descriptor string
char *IRCommandDesc::ToString(char *buf, size_t len) const
{
    // safely add a character
    static const auto Put = [](char* &p, size_t &len, char c) {
        if (len > 1) *p++ = c, --len;
    };

    // add the high four bits of a byte, rotating the byte
    static const auto PutHexDig = [](char* &p, size_t &len, uint8_t b) {
        b &= 0x0F;
        Put(p, len, static_cast<char>(b < 10 ? b + '0' : b - 10 + 'A'));
    };

    // add a 2-digit hex value
    static const auto PutHex = [](char* &p, size_t &len, uint8_t b)
    {
        PutHexDig(p, len, b >> 4);
        PutHexDig(p, len, b);
    };

    // add an n-bit hex value
    static const auto PutHexN = [](char* &p, size_t &len, uint64_t b, int nBits) {
        while (nBits >= 8)
        {
            nBits -= 8;
            PutHex(p, len, static_cast<uint8_t>(b >> nBits));
        }
    };

    // stop now if the buffer is zero-length
    if (len == 0)
        return buf;

    // set up at the start of the string
    char *p = buf;

    // add the protocol ID
    PutHex(p, len, proId);
    Put(p, len, '.');

    // add the flags
    uint8_t flags = (useDittos ? 0x02 : 0x00);
    PutHex(p, len, flags);
    Put(p, len, '.');

    // Add the code, working from MSB to LSB.  To make the format more
    // easily human-readable, try to infer the code size from the value,
    // and drop leading zeroes.  Only use the common code sizes of 64,
    // 48, 32, and 16 bits.  If the code actually use something in
    // between, we'll still get it right, just with some extra leading
    // zeroes.
    int nBits = (code >= (1ULL << 56)) ? 64 :
                (code >= (1ULL << 48)) ? 56 :
                (code >= (1ULL << 32)) ? 48 :
                (code >= (1ULL << 16)) ? 32 :
                16;
    PutHexN(p, len, code, nBits);

    // add the null terminator and return the buffer pointer
    *p = 0;
    return buf;
}

// foramt the received command
char *IRCommandReceived::Format(char *buf, size_t len) const
{
    // format the canonical command descriptor first
    ToString(buf, len);

    // add to the string buffer under construction
    static auto AddChar = [](char *p, char *endp, char c)
    {
        if (p + 1 < endp)
            *p++ = c, *p = 0;
        return p;
    };
    static auto AddStr = [](char *p, char *endp, const char *str)
    {
        for ( ; p + 1 < endp && *str != 0 ; *p++ = *str++) ;
        *p = 0;
        return p;
    };
    static auto AddBit = [](char *p, char *endp, const char *title, bool bit)
    {
        p = AddStr(p, endp, title);
        p = AddChar(p, endp, bit ? '1' : '0');
        return p;
    };

    // add the toggle flag, if the protocol uses toggles
    char *p = buf + strlen(buf);
    if (hasToggle)
        p = AddBit(p, buf + len, ", Toggle: ", toggle);

    // add the ditto flag, if the protocol uses dittos
    if (hasDittos)
        p = AddBit(p, buf + len, ", Ditto: ", ditto);

    // add the position flag
    switch (position)
    {
    case Position::Null:
        break;

    case Position::First:
        p = AddStr(p, buf + len, ", Pos: First");
        break;

    case Position::Middle:
        p = AddStr(p, buf + len, ", Pos: Mid");
        break;

    case Position::Last:
        p = AddStr(p, buf + len, ", Pos: Last");
        break;
    }

    // add the repeat flag
    p = AddBit(p, buf + len, ", Auto-Repeat: ", isAutoRepeat);

    // return the buffer pointer
    return buf;
}

