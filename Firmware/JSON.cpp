// Pinscape Pico - simple JSON parser
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>

// local project headers
#include "JSON.h"

// Microsoft-specific code
#ifdef _MSC_VER
#define IF_MICROSOFT(x) x
#define IF_NOT_MICROSOFT(x)
#define IF_ELSE_MICROSOFT(x, y) x
#else
#define IF_MICROSOFT(x)
#define IF_NOT_MICROSOFT(x) x
#define IF_ELSE_MICROSOFT(x, y) y
#define _countof(arr) (sizeof(arr)/sizeof((arr)[0]))
#endif

// default undefined value holder
const JSONParser::Value JSONParser::undefValue;

// destruction
JSONParser::~JSONParser()
{
    // delete property pools
    for (auto *p = propMapPropPool, *nxt = p ; p != nullptr ; p = nxt)
    {
        nxt = p->nxt;
        delete p;
    }
}

// parse in-memory source text
void JSONParser::Parse(const char *src, size_t len)
{
    // get a pointer to the end of the source text
    const char *p = src;
    const char *endp = src + len;

    // For the sake of completeness, skip hash-bang comments at
    // the very start of the text.
    int lineNum = 1;
    if (p + 1 < endp && *p == '#' && *(p+1) == '!')
    {
        // hash-bang comment runs to the end of the first line
        for (p += 2 ; p < endp && *p != '\n' && *p != '\r' ; ++p) ;
        SkipNewline(p, endp);
        ++lineNum;
    }

    // parse the root value
    rootValue.Reset();
    TokenizerState ts(p, endp);
    ts.lineNum = lineNum;
    ParseValue(rootValue, ts);

    // it's an error if there's anything left
    Token tok;
    if (GetToken(tok, ts))
        errors.emplace_back(tok, "Extraneous text after top-level value definition\n");
}

void JSONParser::SkipNewline(const char* &p, const char *endp)
{
    if (*p == '\r' && p + 1 < endp && *(p+1) == '\n')
        p += 2;
    else
        p += 1;
}

bool JSONParser::ParseValue(Value &value, TokenizerState &ts)
{
    // check for an empty value
    Token tok;
    if (PeekToken(tok, ts))
    {
        switch (tok.type)
        {
        case Token::Type::Comma:
        case Token::Type::RBrace:
        case Token::Type::RParen:
        case Token::Type::RSquare:
            // unexpected comma or closing delimiter -> an empty value slot
            errors.emplace_back(tok, "Empty value\n");

            // set an empty value token
            value.Set(Value::Type::Undefined);
            IfValueKeepSourceRef(
                tok.len = tok.srcLen = tok.srcTxt - ts.src;
                tok.txt = tok.srcTxt = ts.src;
                value.startTok = tok;
                value.endTok = tok;
            )
            return true;

        case Token::Type::Identifier:
            // Identifiers can't be used as values, so assume that this was
            // meant to be a string, or is a keyword or number with a typo.
            // Treat it as an empty value with this token as its text.
            errors.emplace_back(tok, "Unexpected identifier; a value (string, number, boolean, etc) was expected\n");
            value.Set(Value::Type::Undefined);
            IfValueKeepSourceRef(
                tok.len = tok.srcLen = tok.srcTxt + tok.srcLen - ts.src;
                tok.txt = tok.srcTxt = ts.src;
                value.startTok = tok;
                value.endTok = tok;
            )

            // consume the token
            GetToken(tok, ts);
            return true;
        }
    }

    // fetch the first token
    bool eof = !GetToken(tok, ts);

    // If desired, store the source location of the token in the value.
    // Set the start and end token to the same point initially, since
    // most primitive value types will consist of a single token only.
    IfValueKeepSourceRef(
        value.startTok = tok;
        value.endTok = tok;
    )

    // stop if at EOF
    if (eof)
    {
        errors.emplace_back(tok, "Unexpected end of file; value required\n");
        return false;
    }

    // let's see what we have
    switch (tok.type)
    {
    case Token::Type::LBrace:
        // object definition
        ParseObject(value, ts);
        break;

    case Token::Type::LSquare:
        // array definition
        ParseArray(value, ts);
        break;

    case Token::Type::LParen:
        // a value in parentheses is simply the enclosed value
        ParseValue(value, ts);

        // ...but set the open token back to the open paren, and set
        IfValueKeepSourceRef(value.startTok = tok;)

        // parse the close paren
        if (!GetToken(tok, ts) || tok.type != Token::Type::RParen)
            errors.emplace_back(tok, "Expected ')'");

        // set the end token to the close paren
        IfValueKeepSourceRef(value.startTok = tok;)
        break;

    case Token::Type::Undefined:
        value.Set(Value::Type::Undefined);
        break;

    case Token::Type::Null:
        value.Set(Value::Type::Null);
        break;

    case Token::Type::True:
        value.Set(Value::Type::True);
        break;

    case Token::Type::False:
        value.Set(Value::Type::False);
        break;

    case Token::Type::Number:
        value.Set(tok.num);
        break;

    case Token::Type::String:
        value.Set(tok.txt, tok.len);
        break;

    default:
        // other types aren't valid values
        errors.emplace_back(tok, "Expected a value");
        break;
    }

    // success
    return true;
}

// parse a token
bool JSONParser::GetToken(Token &token, TokenizerState &ts)
{
    // skip spaces, newlines, and comments
    for (;;)
    {
        // stop at EOF
        if (ts.src >= ts.endp)
        {
            token.lineNum = ts.lineNum;
            token.txt = token.srcTxt = ts.src;
            token.len = token.srcLen = 0;
            token.type = Token::Type::Eof;
            return false;
        }

        // skip spaces
        if (isspace(*ts.src) || *ts.src == '\n' || *ts.src == '\r')
        {
            // count and skip newlines
            if (*ts.src == '\n' || *ts.src == '\r')
            {
                ++ts.lineNum;
                SkipNewline(ts.src, ts.endp);
            }
            else
            {
                // skip this space character
                ++ts.src;
            }

            // go back for another loop
            continue;
        }

        // skip comments
        if (*ts.src == '/' && ts.src + 1 < ts.endp)
        {
            // check for '//' til-end-of-line comments
            if (*(ts.src + 1) == '/')
            {
                // '//' comment - scan to the end of the line
                for (ts.src += 2 ; ts.src < ts.endp && *ts.src != '\n' && *ts.src != '\r' ; ++ts.src) ;
                continue;
            }

            // Check for '/*' block comments
            if (*(ts.src + 1) == '*')
            {
                // '/*' comment - scan to the matching '*/'.  Note that these
                // comments don't nest in Javascript (just like in C/C++), so
                // we don't have to look for new '/*' open comments or keep
                // count or anything like that - we merely have to scan for the
                // literal two characters '*' and '/'.  Note also that the
                // '*' from the open can't be part of the close, so we can
                // immediately skip ahead two characters to skip the open.
                for (ts.src += 2 ; ts.src + 1 < ts.endp && !(*ts.src == '*' && *(ts.src + 1) == '/') ; )
                {
                    // count newlines
                    if (*ts.src == '\n')
                    {
                        // count the line
                        ++ts.lineNum;

                        // check for and skip a CR-LF pair
                        SkipNewline(ts.src, ts.endp);
                    }
                    else
                    {
                        // ordinary character - skip it
                        ++ts.src;
                    }
                }

                // Skip the '*/'.  IF we found the whole thing, that is - we'll
                // have stopped one character short of EOF (because of the need
                // for a two-character lookahead) if the comment is unterminated.
                // So we can definitely skip one character, but we'll have to
                // check before skipping the second.
                ++ts.src;
                if (ts.src < ts.endp) ++ts.src;
                continue;
            }
        }

        // we're at the start of a token
        break;
    }

    // Set up at the start of the token, and presume the token text will
    // be one character long.  That's hardly a safe assumption, but it's
    // convenient for the 'switch' below because there are many
    // single-character punctuation tokens.  By pre-setting the length
    // to one, we won't have to set it again in any of the one-char
    // token switch cases.  We can easily override the assumption later
    // if it turns out to be wrong.
    token.txt = token.srcTxt = ts.src;
    token.len = token.srcLen = 1;
    token.lineNum = ts.lineNum;

    // set the number value to zero; the field only has meaning for
    // Number tokens, so this isn't really necessary, but it might
    // cause confusion when debugging to see non-zero values show up
    // for non-numeric tokens
    token.num = 0;

    // determine the token type by the first character
    switch (*ts.src++)
    {
    case '"':
    case '\'':
        // String in single or double quotes.  These strings run until
        // the matching (unescaped) quote mark, and can contain '\'
        // escape sequences.  Note that we don't accept "template
        // literal" strings (enclosed in `back quotes`).
        {
            // set the type
            token.type = Token::Type::String;

            // If the string doesn't contain any escape sequences, we can
            // represent its token with a direct pointer into the source
            // text, since the string text is identical to the source text
            // between the matching quote marks.  If the string contains
            // any escape sequences, we must make a private copy in the
            // string pool, where we can expand the escape characters.
            // Move with an assumption that we'll use the source text.
            std::string *poolEle = nullptr;

            // Set the initial token text pointer to point directly to
            // the source text, following the open quote.  Note that the
            // source text pointer (token.txtSrc) remains on the open
            // quote, since that represents the source representation of
            // the token rather than its string content.
            token.txt = ts.src;

            // this is also the start of the first string segment (a segment
            // is a run of literal text between escape sequences)
            const char *seg = ts.src;

            // scan the string for the matching quote
            char qu = *(ts.src - 1);
            for (; ts.src < ts.endp && *ts.src != qu && *ts.src != '\n' && *ts.src != '\r' ; ++ts.src)
            {
                // check for escape characters
                if (*ts.src == '\\')
                {
                    // Since we need to expand escapes, we need a pool copy
                    // of the string.  Create one if we haven't already done
                    // so for a previous escape sequence in the string.
                    if (poolEle == nullptr)
                    {
                        // Add a new element containing the text of the string
                        // up to this point (the text preceding the first '\'),
                        // and remember it as the new pool string.
                        poolEle = &stringPool.emplace_back(token.txt, ts.src - token.txt);
                    }
                    else
                    {
                        // We already have a pool element open.  Append the next
                        // segment of literal text, from the prior escape sequence
                        // to here.
                        poolEle->append(seg, ts.src - seg);
                    }

                    // skip the '\'
                    ++ts.src;

                    // check for EOF
                    if (ts.src == ts.endp)
                        break;

                    // parse the escape
                    char ch = *ts.src;
                    bool empty = false;
                    switch (ch)
                    {
                    case '0':
                        // null character
                        ch = 0;
                        break;

                    case 'n':
                        // newline
                        ch = 0x0A;
                        break;

                    case 'r':
                        // carriage return
                        ch = 0x0D;
                        break;

                    case 'v':
                        // vertical tab
                        ch = 0x0B;
                        break;

                    case 't':
                        // horizontal tab
                        ch = 0x09;
                        break;

                    case 'b':
                        // backspace
                        ch = 0x08;
                        break;

                    case 'f':
                        // form feed
                        ch = 0x0C;
                        break;

                    case '\n':
                    case '\r':
                        // Line terminator -> substitute an empty string
                        empty = true;
                        ++ts.lineNum;

                        // Recognize CR-LF or LF-CR as a single line terminator sequence,
                        // by skipping the second character of the sequence if present.
                        if (ts.src < ts.endp && ((ch == '\n' && *(ts.src + 1) == '\r') || (ch == '\r' && *(ts.src + 1) == '\n')))
                            ++ts.src;
                        break;

                    case 'x':
                    case 'u':
                        // \x = two-hex-digit numeric escape code, \u = four-hex-digit
                        // Unicode escape
                        {
                            // skip the 'x' or 'u'
                            ++ts.src;

                            // accumulate the 2-digit or 4-digit hex value
                            uint16_t acc = 0;
                            for (int nDig = (ch == 'x' ? 2 : 4) ; nDig != 0 && ts.src < ts.endp ; ++ts.src, --nDig)
                            {
                                acc <<= 4;
                                if (*ts.src >= '0' && *ts.src <= '9')
                                    acc += *ts.src - '0';
                                else if (*ts.src >= 'a' && *ts.src <= 'f')
                                    acc += *ts.src - 'a' + 10;
                                else if (*ts.src >= 'A' && *ts.src <= 'F')
                                    acc += *ts.src - 'A' + 10;
                                else
                                    break;
                            }

                            // We only allow 8-bit code points; set values above 0xFF to the null
                            // character.  There isn't any official Unicode character in the 0000
                            // to 00FF range (which we're limited to by our 8-bit encoding) that
                            // means "missing symbol", so we just use null.  This isn't a perfect
                            // solution, because the string can also encode a null explicitly via
                            // an escape (\0, 0x00, or 0u0000), but there's little reason for an
                            // application to do that.
                            if (acc > 0xFF)
                                acc = 0x00;

                            // apply the accumulated character code
                            ch = static_cast<char>(acc);

                            // since the main loop always consumes one more character, back
                            // up to the last character we included in the numeric sequence
                            --ts.src;
                        }
                        break;

                    case '"':
                    case '\'':
                    case '\\':
                    default:
                        // explicit identity escapes and all undefined escapes
                        // just substitute the character following the backslash
                        break;
                    }

                    // append the substitution, if any
                    if (!empty)
                        poolEle->append(1, ch);

                    // Remember the start of the next literal segment.  Note
                    // that 'p' is still on the last character of the escape
                    // sequence, so the next segment starts at the next character.
                    seg = ts.src;
                    if (seg < ts.endp)
                        ++seg;
                }
            }

            // close out the pool element, if any, and the token pointers
            if (poolEle != nullptr)
            {
                // pooled string - append the final literal segment
                poolEle->append(seg, ts.src - seg);

                // set the token to point to the completed pool element
                token.txt = poolEle->c_str();
                token.len = poolEle->size();

                // set the final length of the source text matched
                token.srcLen = ts.src - token.srcTxt;
            }
            else
            {
                // The token points directly into the source text, so we only
                // need to note its length.
                token.len = ts.src - token.txt;
                token.srcLen = ts.src - token.srcTxt;
            }

            // if we found the close quote, skip it
            if (ts.src < ts.endp && *ts.src == qu)
            {
                // skip the quote
                ++ts.src;

                // include the close quote in the original token source text
                token.srcLen += 1;
            }
        }
        break;

    case '(':
        token.type = Token::Type::LParen;
        break;

    case ')':
        token.type = Token::Type::RParen;
        break;

    case '[':
        token.type = Token::Type::LSquare;
        break;

    case ']':
        token.type = Token::Type::RSquare;
        break;

    case '{':
        token.type = Token::Type::LBrace;
        break;

    case '}':
        token.type = Token::Type::RBrace;
        break;

    case ',':
        token.type = Token::Type::Comma;
        break;

    case ':':
        token.type = Token::Type::Colon;
        break;

    case '.':
        // '.' can start a number
        if (ts.src < ts.endp && (isdigit(*ts.src)))
        {
            // back up to the '.' and parse the number
            --ts.src;
            ParseNumberToken(token, ts);
        }
        else
        {
            // no digit follows, so it's just a dot token
            token.type = Token::Type::Dot;
        }
        break;

    case '+':
        // check if this is the start of a number token
        if (ts.src < ts.endp && (isdigit(*ts.src) || *ts.src == '.'))
        {
            // back up to the '+' and parse the number
            --ts.src;
            ParseNumberToken(token, ts);
        }
        else
        {
            // no number follows, it's just a '+' token by itself
            token.type = Token::Type::Plus;
        }
        break;

    case '-':
        // check if this is the start of a number token
        if (ts.src < ts.endp && (isdigit(*ts.src) || *ts.src == '.'))
        {
            // back up to the '-' and parse the number
            --ts.src;
            ParseNumberToken(token, ts);
        }
        else
        {
            // nope, just a '-' token by itself
            token.type = Token::Type::Minus;
        }
        break;
        
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        // Number
        // The 'switch' consumes the first character to sense the
        // token type, so we need to back up to scan it again as
        // part of the number.
        --ts.src;
        ParseNumberToken(token, ts);
        break;

    default:
        // Anything else has to be an identifier or invalid.  Back up to
        // the start (since the switch consumed the first character), and
        // check for a leading symbol character.
        --ts.src;
        if (*ts.src == '_' || *ts.src == '$' || isalpha(*ts.src))
        {
            // valid leading character for an identifier - scan consecutive valid
            // ident characters
            for (++ts.src ; ts.src < ts.endp && 
                (isalpha(*ts.src) || isdigit(*ts.src) || *ts.src == '_' || *ts.src == '$') ; 
                ++ts.src) ;

            // store the token
            token.len = token.srcLen = ts.src - token.txt;
            token.type = Token::Type::Identifier;

            // check for reserved words
            if (token.len == 4 && memcmp(token.txt, "true", 4) == 0)
                token.type = Token::Type::True;
            else if (token.len == 5 && memcmp(token.txt, "false", 5) == 0)
                token.type = Token::Type::False;
            else if (token.len == 9 && memcmp(token.txt, "undefined", 9) == 0)
                token.type = Token::Type::Undefined;
            else if (token.len == 4 && memcmp(token.txt, "null", 4) == 0)
                token.type = Token::Type::Null;
        }
        else
        {
            // not a valid token character - mark the token as invalid and
            // skip one character
            ++ts.src;
            token.len = 1;
            token.type = Token::Type::Invalid;
        }
    }

    // We read a token, so we weren't at EOF on entry.  (Note that we could
    // be at EOF now, but in terms of parsing state, we're not at EOF until
    // the caller consumes the new token.)
    return true;
}

bool JSONParser::ParseNumberToken(Token &token, TokenizerState &ts)
{
    // check for a sign
    bool neg = false;
    if (*ts.src == '-')
    {
        neg = true;
        ++ts.src;
    }
    else if (*ts.src == '+')
    {
        ++ts.src;
    }

    // Check for a radix marker: 0x, 0o, 0b.
    //
    // Note that we ONLY accept the newer "0o..." (zero-oh) notation for
    // octal numbers.  We don't accept the older C-style notation in
    // which a leading zero all by itself (without the 'o') is enough to
    // indicate octal.  This is the same treatment that modern
    // Javascript interpreters use when in "strict mode", and we use it
    // as our only option because the old C "077" notation is much more
    // likely to create confusion for human readers/authors.  It would
    // never occur to most people that a leading zero even *might* have
    // a special meaning, because it never would in any ordinary,
    // non-programming context.  So treating "077" as decimal 77 is
    // exactly what almost everyone would expect.  The only people who
    // would even stop to wonder whether "077" is going to be treated as
    // anything other than decimal 77 are long-time C programmers, and
    // their intuition should NOT be that everything works like C, but
    // rather that this sort of thing varies, so you have to read the
    // manual.  (Or more likely, just avoid using leading zeroes at all
    // so you don't have to remember which way it is for this particular
    // thing.  No one has really had much reason to use octal since the
    // 1970s anyway, back when 9-bit bytes were a thing.)
    if (*ts.src == '0' && ts.src + 1 < ts.endp && strchr("bBoOxX", *(ts.src + 1)) != nullptr)
    {
        // skip the leading 0
        ++ts.src;

        // parse an integer in the specified radix
        uint64_t i = 0;
        switch (*ts.src++)
        {
        case 'b':
        case 'B':
            // binary
            for (ts.src ; ts.src < ts.endp && (*ts.src == '0' || *ts.src == '1') ; ++ts.src)
            {
                i <<= 1;
                i += *ts.src - '0';
            }
            break;

        case 'o':
        case 'O':
            // octal
            for (; ts.src < ts.endp && (*ts.src >= '0' && *ts.src <= '7') ; ++ts.src)
            {
                i <<= 3;
                i += *ts.src - '0';
            }
            break;

        case 'x':
        case 'X':
            // hex
            for (; ts.src < ts.endp ; ++ts.src)
            {
                if (*ts.src >= '0' && *ts.src <= '9')
                {
                    i <<= 4;
                    i += *ts.src - '0';
                }
                else if (*ts.src >= 'a' && *ts.src <= 'f')
                {
                    i <<= 4;
                    i += *ts.src - 'a' + 10;
                }
                else if (*ts.src >= 'A' && *ts.src <= 'F')
                {
                    i <<= 4;
                    i += *ts.src - 'A' + 10;
                }
                else
                    break;
            }
            break;
        }

        // apply the sign and store the value
        token.num = neg ? static_cast<double>(-static_cast<int64_t>(i)) : static_cast<double>(i);
    }
    else
    {
        // Decimal, possibly with fraction and/or exponent.  Move
        // with the whole part (before any decimal point or 'e').
        uint64_t i = 0;
        for (; ts.src < ts.endp && isdigit(*ts.src) ; ++ts.src)
        {
            i *= 10;
            i += *ts.src - '0';
        }

        // set the whole part in the token
        token.num = neg ? static_cast<double>(-static_cast<int64_t>(i)) : static_cast<double>(i);

        // if there's a decimal point, parse the fractional part
        if (ts.src < ts.endp && *ts.src == '.')
        {
            // Accumulate the fractional part as (numerator / denominator).
            // Calculate these as integers simply because that's faster.
            // A 64-bit int has more precision than a double, so we won't
            // lose any precision as long as the denominator doesn't
            // overflow.  
            int64_t numerator = 0;
            uint64_t denominator = 1;
            for (++ts.src ; ts.src < ts.endp && isdigit(*ts.src) ; ++ts.src)
            {
                // If the next digit won't overflow the denominator, add it
                // into the numerator.  Simply drop digits after the denominator
                // overflows, since we won't be able to store them anyway.  The
                // overflow point of interest here is the 52-bit mantissa of the
                // 'double' that ultimately stores the result value.  Since
                // that's below the limit for the uint64_t we're using for the
                // intermediate result, there's no possibility of overflowing
                // the intermediate registers.
                uint64_t dd = denominator * 10;
                if (dd < 9007199254740991ULL)
                {
                    // accumulate the numerator and denominator
                    denominator = dd;
                    numerator *= 10;
                    numerator += *ts.src - '0';
                }
            }

            // add the fractional part to the number under construction
            double frac = static_cast<double>(numerator) / static_cast<double>(denominator);
            token.num += (neg ? -frac : frac);
        }

        // check for an exponent
        if (ts.src < ts.endp && (*ts.src == 'e' || *ts.src == 'E'))
        {
            // check for the exponent sign
            bool eneg = false;
            ++ts.src;
            if (ts.src < ts.endp && *ts.src == '-')
            {
                ++ts.src;
                eneg = true;
            }
            else if (ts.src < ts.endp && *ts.src == '+')
            {
                ++ts.src;
            }

            // parse the exponent as a literal integer
            uint32_t e = 0;
            for (; ts.src < ts.endp && isdigit(*ts.src) ; ++ts.src)
            {
                e *= 10;
                e += *ts.src - '0';
            }

            // multiply the number under construction by the exponent
            token.num *= pow(10.0, eneg ? static_cast<double>(-static_cast<int32_t>(e)) : e);
        }
    }

    // set the token type and length
    token.type = Token::Type::Number;
    token.len = token.srcLen = ts.src - token.txt;
    token.lineNum = ts.lineNum;
   
    // success
    return true;
}

bool JSONParser::ParseObject(Value &value, TokenizerState &ts)
{
    // initialize an empty object node
    value.Set(Value::Type::Object);
    auto *obj = value.object;

    // iterate until we reach '}' or EOF
    for (;;)
    {
        // peek to see if we're at the closing '}'
        Token tok;
        if (!PeekToken(tok, ts))
        {
            // EOF before closing '}'
            errors.emplace_back(tok, "Missing '}'");
            IfValueKeepSourceRef(value.endTok = tok;)
            return false;
        }
        if (tok.type == Token::Type::RBrace)
        {
            // '}' found - skip it, and return the finished object value
            GetToken(tok, ts);
            IfValueKeepSourceRef(value.endTok = tok;)
            return true;
        }

        // parse the next property name
        Token propTok;
        if (!GetToken(propTok, ts) || (propTok.type != Token::Type::Identifier && propTok.type != Token::Type::String))
            errors.emplace_back(propTok, "Expected property name");

        // if it's a comma, treat the whole current property entry as empty
        if (propTok.type == Token::Type::Comma)
        {
            // just continue like we had a property here
            continue;
        }

        // if a property with the same name already exists, it's an error
        Value::StringWithLen propName(propTok.txt, propTok.len);
        if (auto oldProp = obj->find(propName) ; oldProp != nullptr)
            errors.emplace_back(tok, "This property is already defined for this object (each property name must be unique within an object)");

        // Add the property to the object map.  Note that the object property
        // list is stored as a multimap, so we can store the new property even
        // if the name has already been defined in this object (which we just
        // flagged as an error if so).
        PropValue *propVal = &obj->emplace(this, propName, propTok, Value::Type::Undefined)->val;

        // parse the ':', if we didn't find it already
        Token colonTok;
        if (PeekToken(colonTok, ts) && colonTok.type == Token::Type::Colon)
        {
            // skip the colon and parse the value
            GetToken(colonTok, ts);
            ParseValue(*propVal, ts);

            // whatever follows the value is the delimiter token
            IfValueKeepSourceRef(PeekToken(propVal->delimTok, ts));
        }
        else
        {
            // flag the error
            errors.emplace_back(colonTok, "Expected ':'");

            // if *this* is our close brace or EOF, enroll the current property
            // with an empty value, and finish the object
            if (colonTok.type == Token::Type::RBrace)
            {
                // close brace - skip it and stop here
                GetToken(colonTok, ts);
                IfValueKeepSourceRef(value.endTok = colonTok;)
                return true;
            }
            else if (colonTok.type == Token::Type::Eof)
            {
                // eof - stop here
                IfValueKeepSourceRef(value.endTok = colonTok;)
                return true;
            }

            // If it's another identifier or string, assume that it's an
            // incomplete property definition followed by another property
            // definition.  Enroll it with an empty value, and proceed to
            // the next property.
            if (colonTok.type == Token::Type::Identifier || colonTok.type == Token::Type::String)
            {
                IfValueKeepSourceRef(propVal->delimTok = colonTok);
                continue;
            }
        }

        // we should be at either a comma or the closing brace
        Token sepTok;
        if (PeekToken(sepTok, ts))
        {
            if (sepTok.type == Token::Type::Comma)
            {
                // comma - skip it and continue
                GetToken(sepTok, ts);
            }
            else if (sepTok.type == Token::Type::RBrace)
            {
                // done with the object - skip it and return
                GetToken(sepTok, ts);
                IfValueKeepSourceRef(value.endTok = sepTok);
                return true;
            }
            else
            {
                // anything else is invalid - leave it in place and keep
                // going, since they might have dropped the comma and just
                // moved on to the next value
                errors.emplace_back(sepTok, "Expected ',' or '}'");
            }
        }
    }
}

bool JSONParser::ParseArray(Value &value, TokenizerState &ts)
{
    // initialize an empty array node
    value.Set(Value::Type::Array);

    // iterate until we reach '}' or EOF
    for (;;)
    {
        // peek to see if we're at the closing ']'
        Token tok;
        if (!PeekToken(tok, ts))
        {
            // EOF before closing '}'
            errors.emplace_back(tok, "Missing ']'");
            return false;
        }
        if (tok.type == Token::Type::RSquare)
        {
            // ']' found - skip it, and return the finished array value
            GetToken(tok, ts);
            IfValueKeepSourceRef(value.endTok = tok;)
            return true;
        }

        // add a new slot to the array
        ArrayEleValue *ele = value.array->emplace_back(new ArrayEleValue()).get();

        // parse the value into the new slot
        TokenizerState tsPreValue = ts;
        ParseValue(*ele, ts);
        
        // the next token is the ending delimiter
        PeekToken(ele->delimTok, ts);

        // we should be at either a comma or the closing bracket
        Token sepTok;
        if (PeekToken(sepTok, ts))
        {
            if (sepTok.type == Token::Type::Comma)
            {
                // comma - skip it and continue
                GetToken(sepTok, ts);
            }
            else if (sepTok.type == Token::Type::RSquare)
            {
                // done with the array - skip it and return
                GetToken(sepTok, ts);
                IfValueKeepSourceRef(value.endTok = sepTok;)
                return true;
            }
            else
            {
                // Anything else is invalid - leave it in place and keep
                // going, since they might have dropped the comma and just
                // moved on to the next value.
                errors.emplace_back(sepTok, "Expected ',' or ']'");

                // If the value also didn't consume any tokens, this must
                // be the end of the list, even though it's not the right
                // delimiter.  This can happen if we have a close delimiter
                // for an enclosing object, for example.  Stop here if so.
                if (ts.src == tsPreValue.src)
                    return true;
            }
        }
    }
}

JSONParser::PropMap::Prop *JSONParser::AllocPropMapProp()
{
    // allocate another pool block if needed
    if (propMapPropPool == nullptr || propMapPropPool->nextFree >= _countof(PropMapPropPool::props))
    {
        auto *newPool = new PropMapPropPool();
        newPool->nxt = propMapPropPool;
        propMapPropPool = newPool;
    }

    // allocate the next item
    return &propMapPropPool->props[propMapPropPool->nextFree++];
}

const JSONParser::Value *JSONParser::Get(const char *expr) const
{
    // evaluate the expression relative to the root node
    return rootValue.Get(expr);
}

void JSONParser::Value::Set(Type type)
{
    this->type = type;
    switch (type)
    {
    case Type::Array:
        // instantiate the vector that will hold the array
        array = new std::vector<std::unique_ptr<ArrayEleValue>>();
        break;

    case Type::Object:
        // instantiate the map that will hold the property table
        object = new PropMap();
        break;

    default:
        // no attached value; set the number field to zero so that it
        // doesn't look random when debugging, and so that the compiler
        // doesn't think we're leaving the whole union uninitialized
        number = 0;
        break;
    }
}


const JSONParser::Value *JSONParser::Value::Get(const char *expr) const
{
    // if the string is empty, this node is the result
    if (*expr == 0)
        return this;

    // scan ahead to the next delimiter
    const char *p = expr;
    for (; *p != 0 && *p != '.' ; ++p) ;

    // pull out the property name or array index
    StringWithLen ele(expr, p - expr);

    // skip to the next expression element (after the '.',
    // unless we're at the end of the string)
    expr = (*p == 0 ? p : p + 1);

    // check this node type
    switch (type)
    {
    case Type::Array:
        {
            // Interpret the element as an array index.  If it's
            // in range, parse the rest of the expression against the
            // indexed element; otherwise the result is 'undefined'.
            int idx = atoi(ele.txt);
            if (idx >= 0 && idx < static_cast<int>(array->size()))
                return array->at(idx)->Get(expr);
            else
                return &undefValue;
        }

    case Type::Object:
        // Interpret the element as a property name.  If it's in
        // the object's property map, parse the rest of the expression
        // against the property value; otherwise the result is
        // 'undefined'.
        if (auto *prop = object->find(ele); prop != nullptr)
            return prop->val.Get(expr);
        else
            return &undefValue;

    default:
        // All other types are scalars, so evaluating a property
        // or array index against this value yields 'undefined'.
        return &undefValue;
    }
}

double JSONParser::Value::Double(double defaultValue) const
{
    switch (type)
    {
    case Type::Number:
        // number - use the number value
        return number;
        break;

    case Type::String:
        // string
        {
            // parse the string using the same rules as a numeric token
            Token tok;
            TokenizerState ts(string.txt, string.txt + string.len);
            ParseNumberToken(tok, ts);

            // use the numeric value of the token
            return tok.num;
        }

    case Type::True:
        // true has numeric value 1
        return 1.0;

    case Type::False:
    case Type::Null:
        // false and null have numeric value 0
        return 0.0;

    case Type::Undefined:
        // undefined -> return the default value
        return defaultValue;

    default:
        // any unhandled types return zero
        return 0.0;
    }
}

std::string JSONParser::Value::String(const char *defaultValue) const
{
    switch (type)
    {
    case Type::String:
        // it's already a string, so just convert to a std::string
        return std::string(string.txt, string.len);

    case Type::Number:
        // convert the number to a string representation in decimal
        {
            // check the range
            double d = fabs(number);
            char buf[128];
            if (d == 0)
            {
                // exactly zero is simple
                buf[0] = '0';
                buf[1] = 0;
            }
            else if (d < 1e-6 || d > 1e21)
            {
                // very large or very small - force to scientific notation
                snprintf(buf, sizeof(buf), "%.15le", number);

                // trim trailing zeroes between the '.' and 'e'
                if (char *p = strchr(buf, '.'); p != nullptr)
                {
                    if (char *e = strchr(p, 'e'); e != nullptr)
                    {
                        // scan for trailing zeroes leading up to the 'e', stopping
                        // at the decimal point
                        char *q = e;
                        for (; q != p && *(q-1) == '0' ; --q);

                        // if we stopped at the decimal point, remove that as well,
                        // since there are no digits following it now
                        if (q == p)
                            --q;

                        // Copy the 'e...' portion to the new position.  Note that
                        // this strcpy is inherently safe (it can't write past the
                        // end of the buffer), since we're moving the existing 'e'
                        // substring to a lower position in the existing buffer.
                        // The overall string can only stay the same length or get
                        // shorter.  (MSVC warning 4996 is "never use strcpy", but
                        // in this case it's actually safe.)
                        IF_MICROSOFT(__pragma(warning(suppress: 4996))) strcpy(q, e);
                    }
                }
            }
            else
            {
                // check if it has a fractional part
                double intPart = 0;
                if (modf(number, &intPart) == 0)
                {
                    // fraction is zero - suppress the decimal point entirely
                    // by using the .0 format modifier
                    snprintf(buf, sizeof(buf), "%.0lf", number);
                }
                else
                {
                    // it has a fractional part - use the default float
                    // format, and clean up the result by trimming
                    // trailing zeroes after the decimal point
                    snprintf(buf, sizeof(buf), "%lf", number);
                    char *p = strchr(buf, '.');
                    if (p != nullptr)
                    {
                        // remove trailing zeroes, stopping at the decimal point
                        for (char *q = p + strlen(p) ; q != p && *(q-1) == '0' ; *--q = 0);

                        // if there's nothing left after the point, remove that, too
                        if (*(p+1) == 0)
                            *p = 0;
                    }
                }
            }
            return buf;
        }

    case Type::Undefined:
        // undefined - return the default value
        return defaultValue;
        
    case Type::Null:
        // null -> "null"
        return "null";

    case Type::True:
        // true -> "true"
        return "true";

    case Type::False:
        // false -> "false"
        return "false";

    case Type::Object:
        return "[Object]";

    case Type::Array:
        return "[Array]";

    default:
        return "[NoStringConversion]";
    }
}

// array element iteration
void JSONParser::Value::ForEach(std::function<void(int, const Value*)> callback, bool implicitWrap) const
{
    // check the type
    if (type == Type::Array)
    {
        // array - invoke the callback for each array element, starting at index 0
        int index = 0;
        for (auto &ele : *array)
            callback(index++, ele.get());
    }
    else if (implicitWrap)
    {
        // this isn't an array, but implicitWrap is set, so pretend that
        // we're a one-element array with our actual value as the one
        // element, so invoke the callback once on 'this'
        callback(0, this);
    }
}

// object property iteration
void JSONParser::Value::ForEach(std::function<void(const StringWithLen &, const Value*)> callback) const
{
    // only iterate objects
    if (type == Type::Object)
    {
        // invoke the callback for each property
        for (auto *ele = object->props ; ele != nullptr ; ele = ele->nxt)
            callback(ele->name, &ele->val);
    }
}

bool JSONParser::Value::IsFalsy(bool defaultIfUndefined) const
{
    // use the default value for an undefined
    if (type == Type::Undefined)
        return defaultIfUndefined;

    // otherwise, use the normal Javascript falsy rules
    return type == Type::Null
        || type == Type::False
        || (type == Type::Number && (number == 0.0 || number == -0.0 || isnan(number)))
        || (type == Type::String && string.len == 0);
}