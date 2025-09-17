// Pinscape Pico - JSON parser
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
// 
// This is a simple JSON parser that's optimized for use in the
// Pinscape Pico firmware.  We use it for parsing configuration data
// that's created on the host computer and stored on the Pico in its
// flash storage space.  The JSON data is typically created mechanically
// by the Pinscape Config Tool on the Windows host, and it's stored
// locally on the Pico in flash memory.  The Pico reads the JSON data
// at program startup to set up its run-time configuration.
// 
// The reason we use our own custom JSON instead of one of the
// millions of extant open-source versions is that it lets us make
// application-specific optimizations for our narrow use case.  Plus
// it's nice to minimize dependencies, especially for a small embedded
// application like this.
// 
// One major application-specific optimization goal is to minimize the
// memory footprint, since we're targeting a microcontroller platform
// with a relatively small RAM space and no virtual memory.  We do this
// in part by keeping direct pointers into the source text within the
// parse tree, which means that the caller must provide source text that
// will remain valid for the lifetime of the parse tree.
//

#pragma once

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <functional>

// For the firmware build, don't keep source locations in the JSON
// value tree, since we don't need them - they're meant for parsing
// and diagnostic tools on the host side.
#ifdef PICO_FIRMWARE_BUILD
#define IfValueKeepSourceRef(...)
#else
#define JSON_VALUE_KEEP_SOURCE_REF
#define IfValueKeepSourceRef(...) __VA_ARGS__
#endif

class JSONParser
{
public:
    // destruction
    ~JSONParser();

    // Parse JSON source.  The source must consist entirely of single-byte
    // characters (pure ASCII or a single-byte code page such as Latin-1).
    //
    // To minimize the memory footprint, the generated parse tree contains
    // direct pointers into the original source text.  This means that the
    // memory containing the source text must remain valid for the lifetime
    // of the parse tree.  If the caller is working with ephemeral source
    // text, the caller must explicitly make a copy that will remain valid
    // for the lifetime of the parse tree, such as in a std::vector<char>
    // that's kept together with the JSONParser object.
    void Parse(const char *src, size_t len);

    // token struct
    struct Token
    {
        // token types
        enum class Type
        {
            Invalid,       // invalid token
            Eof,           // end of file
            Undefined,     // 'undefined' keyword
            Null,          // 'null' keyword
            True,          // 'true' keyword
            False,         // 'false' keyword
            Identifier,    // identifier
            String,        // quoted string
            Number,        // number
            LParen,        // left parenthesis
            RParen,        // right parenthesis
            LSquare,       // left square bracket
            RSquare,       // right square bracket
            LBrace,        // left curly brace
            RBrace,        // right curly brace
            Dot,           // dot (period)
            Comma,         // comma
            Colon,         // colon
            Plus,          // plus sign
            Minus,         // minus sign (hyphen)
        };

        // initialize to defaults
        Token() { }

        // initialize to a source location
        Token(const char *txt) : txt(txt), srcTxt(txt) { }

        // type of this token
        Type type = Type::Invalid;

        // line number where the token appears
        int lineNum = 0;

        // Token text and length.  This points directly into the source
        // text whenever possible - that is, whenever the literal text
        // that appears in the source is the same as the token text.
        // For string tokens that contain escapes, this points into the
        // string pool instead, since the source representation of the
        // string is different from the actual string value.
        const char *txt = "";
        size_t len = 0;

        // Original source text of the token.  This unconditionally
        // points into the source, even for tokens that require a separate
        // copy for the 'txt' value, such as for strings with escapes.
        const char *srcTxt = "";
        size_t srcLen = 0;

        // pointer to end of source text
        const char *SrcEnd() const { return srcTxt + srcLen; }

        // is a pointer within this token?
        bool IsPtrIn(const char *p) const { return p >= srcTxt && p <= SrcEnd(); }

        // numeric value of the token; meaningful only for Type::Number tokens
        double num = 0.0;
    };

    // value node
    struct PropValue;
    struct ArrayEleValue;
    class PropMap;
    struct Value
    {
        // forward declaration
        struct StringWithLen;

        // If desired, keep the source text token range information for
        // each value
        IfValueKeepSourceRef(
            // Starting token.  For an object or array, this is the opening
            // '{' or '[' token.  For primitive values, it's the value token.
            Token startTok;
        
            // Ending token.  For an object or array, this is the closing
            // '}' or ']'.  For primitive values, it's the same as the start
            // token.
            Token endTok;
        )

        // datatype of value
        enum class Type
        {
            Undefined,
            Null,
            True,
            False,
            Number,
            String,
            Object,
            Array
        };
        Type type = Type::Undefined;

        // construction/destruction
        Value() : number(0) { }
        Value(Type type) { Set(type); }
        ~Value() { Reset(); }

        // get a sub-node via a property/array lookup
        const Value *Get(const char *expr) const;

        // Get the value of this node, coerced to a specified native
        // numeric type.  The provided default value is returned if the
        // value is undefined or can't be coerced to the target type.
        // The default value is also used if the underlying value is out
        // of range for the native signed int type.
        template<typename T> T NumX(T defaultValue, T minVal, T maxVal) const
        {
            // if the underlying value is undefined, return the default value
            if (type == Type::Undefined)
                return defaultValue;
            
            // coerce the value to the basic javascript numeric type, which is
            // equivalent to our native 'double'
            double d = Double();
            
            // if it's out of range for the native type, return the default value
            if (d < static_cast<double>(minVal) || d > static_cast<double>(maxVal))
                return defaultValue;
            
            // it's in range - coerce to the return type
            return static_cast<T>(d);
        }
        
        int Int(int defaultValue = 0) const { return NumX<int>(defaultValue, INT_MIN, INT_MAX); }
        unsigned int UInt(unsigned int defaultValue = 0) const { return NumX<unsigned int>(defaultValue, 0, UINT_MAX); }
        int8_t Int8(int8_t defaultValue = 0) const { return NumX<int8_t>(defaultValue, INT8_MIN, INT8_MAX); }
        uint8_t UInt8(uint8_t defaultValue = 0) const { return NumX<uint8_t>(defaultValue, 0, UINT8_MAX); }
        int16_t Int16(int16_t defaultValue = 0) const { return NumX<int16_t>(defaultValue, INT16_MIN, INT16_MAX); }
        uint16_t UInt16(uint16_t defaultValue = 0) const { return NumX<uint16_t>(defaultValue, 0, UINT16_MAX); }
        int32_t Int32(int32_t defaultValue = 0) const { return NumX<int32_t>(defaultValue, INT32_MIN, INT32_MAX); }
        uint32_t UInt32(uint32_t defaultValue = 0) const { return NumX<uint32_t>(defaultValue, 0, UINT32_MAX); }
        int64_t Int64(int64_t defaultValue = 0) const { return NumX<int64_t>(defaultValue, INT64_MIN, INT64_MAX); }
        uint64_t UInt64(int64_t defaultValue = 0) const { return NumX<uint64_t>(defaultValue, 0, UINT64_MAX); }
        float Float(float defaultValue = 0.0f) const { return NumX<float>(defaultValue, FLT_MIN, FLT_MAX); }

        // Get the value of this node, coerced to double.  Returns the
        // given default value if the node value is undefined.
        double Double(double defaultValue = 0.0) const;

        // Get the value of this node, coerced to string. Returns the
        // given default value if the node value is undefined.
        std::string String(const char *defaultValue = "") const;

        // exact comparisons - these act like javascript operator === in
        // that they require matching both type and value
        bool operator==(double d) const { return type == Type::Number && number == d; }
        bool operator==(const char *p) const { return type == Type::String && string == p; }
        bool operator==(Type type) const { return type == this->type; }

        // array length; returns the default for non-array values
        size_t Length(size_t defaultVal = 0) const { return type == Type::Array ? array->size() : defaultVal; }

        // index an array element; returns undefined for non-arrays or for a non-existent element
        const Value *Index(int i) const { 
            return type == Type::Array && i >= 0 && i < static_cast<int>(array->size()) ?
            (*array)[i].get() : &undefValue; 
        }

        // Array element iteration - invokes the callback for each array
        // element, in array order.  If 'this' isn't an array, the behavior
        // depends upon 'implicitWrap'.  By default, if false, we won't
        // invoke the callback at all, since there are no array elements to
        // iterate.  If 'implicitWrap' is true, we'll (notionally) wrap
        // 'this' into a one-element array, and invoke the callback once on
        // that single element.
        void ForEach(std::function<void(int index, const Value *value)> callback, bool implicitWrap = false) const;

        // property object iteration; doesn't invoke callback for non-objects
        void ForEach(std::function<void(const StringWithLen &propName, const Value *value)> callback) const;

        // exact type testing
        bool IsUndefined() const { return type == Type::Undefined; }
        bool IsNull() const { return type == Type::Null; }
        bool IsTrue() const { return type == Type::True; }
        bool IsFalse() const { return type == Type::False; }
        bool IsBool() const { return type == Type::True || type == Type::False; }
        bool IsNumber() const { return type == Type::Number; }
        bool IsString() const { return type == Type::String; }
        bool IsObject() const { return type == Type::Object; }
        bool IsArray() const { return type == Type::Array; }

        // Javascript-style 'falsy' and 'truthy'.
        //
        // IsFalsy() follows the normal Javascript 'falsy' rules, except for
        // 'undefined', which has special handling to make it easier to interpret
        // missing values as defaults.  IsFalsy() treats a value as equivalent to
        // boolean false if it's one of the following:
        //
        //  - null
        //  - false
        //  - a number with the value 0 or -0 (which have distinct representations in the IEEE double format)
        //  - a number with the special value NAN ("Not A Number", which is the well-defined result of certain calculations)
        //  - an empty string
        //
        // undefined has special handling: it returns the 'defaultIfUndefined'
        // argument.  Under normal Javascript rules, undefined is a falsy value,
        // so if defaultIfUndefined == true, you get the standard result.  But
        // you can override this for cases where a missing value should have a
        // different interpretation.
        //
        // (Note that IsFalsy() returns true if the value being tested matches
        // one of the falsy values.  Try not to think about it too hard.)
        //
        // IsTruthy() returns defaultIfUndefined if the value is undefined,
        // otherwise it returns !IsFalsy().
        //
        bool IsFalsy(bool defaultIfUndefined = true) const;
        bool IsTruthy(bool defaultIfUndefined = false) const { return IsUndefined() ? defaultIfUndefined : !IsFalsy(); }

        // Coerce to boolean; equivalent to IsTruthy().  The result is
        // true if the value is anything other than undefined, null,
        // false, the number 0, the number -0, the special number NAN, or
        // an empty string.  If the value is undefined, the result is
        // 'defaultIfUndefined', which makes it easier to apply a default
        // when the underlying value is missing.
        bool Bool(bool defaultIfUndefined = false) const { return IsTruthy(defaultIfUndefined); }

        // reset to undefined, deleting any owned objects
        void Reset()
        {
            // free any sub-object
            switch (type)
            {
            case Type::Object:
                // delete the object
                delete object;
                break;

            case Type::Array:
                // delete the vector holding the array
                delete array;
                break;
            }

            // set to undefined, and clear the numeric value so that there's
            // no confusing cruft in debug views
            type = Type::Undefined;
            number = 0;
        }

        // set from various source types
        void Set(double d) { type = Type::Number; number = d; }
        void Set(const char *p, size_t len) { type = Type::String; string.Set(p, len); }
        void Set(Type type);

        // Counted-length string type.  We use this so that can take the map
        // keys for an object property map directly from the source text (the
        // more conventional approach would be to make a separate copy in a
        // std::string for each key).  For use as unordered_map keys, we need
        // a custom hash function and equality test function.
        struct StringWithLen
        {
            StringWithLen() : txt(""), len(0) { }
            StringWithLen(const char *p, size_t len) : txt(p), len(len) { }
            StringWithLen(const char *p) : txt(p), len(strlen(p)) { }
            
            void Set(const char *p, size_t len) { this->txt = p; this->len = len; }

            bool operator==(const char *txt2) const {
                return strlen(txt2) == len && memcmp(txt, txt2, len) == 0;
            }
            bool operator==(const StringWithLen &s) const {
                return s.len == len && memcmp(txt, s.txt, len) == 0;
            }
            bool Equals(const char *txt2, size_t len2) const {
                return len == len2 && memcmp(txt, txt2, len2) == 0;
            }

            // string pointer and length
            const char *txt;
            size_t len;

            // hash function (for maps)
            struct HashFunc {
                int operator()(const StringWithLen &s) const {
                    int seed = 131, hash = 0;
                    const char *p = &s.txt[0];
                    for (size_t i = 0 ; i < s.len ; ++i)
                        hash = (hash * seed) + *p++;
                    return hash & 0x7FFFFFFF;
                }
            };

            // equality test function (for maps)
            struct EqualsFunc {
                bool operator()(const StringWithLen &a, const StringWithLen &b) const {
                    return a.len == b.len && memcmp(a.txt, b.txt, a.len) == 0;
                }
            };
        };

        // value; interpret based on 'type'
        union
        {
            // as a number
            double number;

            // as a string - note that this is NOT null-terminated
            StringWithLen string;

            // object, as a property name/value hash table
            PropMap *object;

            // array, as a vector of Value structs
            std::vector<std::unique_ptr<ArrayEleValue>> *array;
        };
    };

    // Property Value.  This is a specialization of Value that adds
    // the property token defining the property and the delimiter at
    // the end, for figuring the full span of the property definition
    // in the source file.
    struct PropValue : Value
    {
        PropValue() { }
        PropValue(const Token &propTok) IfValueKeepSourceRef(: propTok(propTok)) { }
        PropValue(const Token &propTok, Type type) : Value(type) IfValueKeepSourceRef(, propTok(propTok)) { }

        // property name token
        IfValueKeepSourceRef(Token propTok);

        // ending delimiter token
        IfValueKeepSourceRef(Token delimTok);
    };

    // Object property map.  This represents the set of properties defined
    // within an object, with each property being a name/value pair.
    // 
    // In well-formed JSON, the property names within an object are always
    // unique.  But we want to be able to accept ill-formed JSON as well,
    // for the sake of generating good diagnostics, so we accept redundant
    // property names within an object.
    // 
    // The property list is implemented as a custom linked list type, to
    // conserve memory when deployed on a Pico.  For the same reason, the
    // property name key is expressed with our counted-length string type,
    // so that we can keep a pointer directly into the source text wherever
    // possible.  This also means that we don't have to explicitly free the
    // individual properties, since they'll all be deleted when the parser
    // object is deleted.
    class PropMap
    {
    public:
        // property entry
        struct Prop
        {
            Prop() { }

            Prop(const Value::StringWithLen &name, const Token &tok, Value::Type type) :
                name(name), val(tok, type)
            {
                Value::StringWithLen::HashFunc hf;
                hash = hf(name);
            }

            Value::StringWithLen name;
            int hash = 0;
            PropValue val;
            Prop *nxt = nullptr;
        };

        Prop *emplace(JSONParser *js, const Value::StringWithLen &name, const Token &tok, Value::Type type)
        {
            Prop *prop = new (js->AllocPropMapProp()) Prop(name, tok, type);
            prop->nxt = this->props;
            this->props = prop;
            return prop;
        }

        Prop *find(const Value::StringWithLen &name)
        {
            Value::StringWithLen::HashFunc hf;
            int hash = hf(name);
            for (Prop *cur = props ; cur != nullptr ; cur = cur->nxt)
            {
                if (cur->hash == hash && cur->name == name)
                    return cur;
            }
            return nullptr;
        }

        // property list
        Prop *props = nullptr;
    };

    // Array Element Value.  This is a specialization of Value that
    // adds the ending delimeter token, for figuring the full span
    // of the element text in the source file.
    struct ArrayEleValue : Value
    {
        ArrayEleValue() : Value() { }
        ArrayEleValue(Type type) : Value(type) { }

        // ending delimiter token
        Token delimTok;
    };

    // Get a Value node by traversing an object/array dereference path
    // from the root node.  Use '.' for both property and array lookups;
    // an array index is given by number after the '.'.
    const Value *Get(const char *expr) const;

    // The root value node for the parsed JSON data.  This is normally
    // an object (Value::Type::Object, containing a Value::PropMap) for
    // well-formed JSON source.
    Value rootValue;

    // Default undefined value holder.  We use this as the result of
    // a Get that evaluates into an undefined property or value node.
    static const Value undefValue;

    // Tokenizer state
    struct TokenizerState
    {
        TokenizerState(const char *src, const char *endp) : src(src), endp(endp) { }

        // current source pointer
        const char *src;

        // end-of-text pointer
        const char *endp;

        // line number
        int lineNum = 1;
    };

    // parse a value node
    bool ParseValue(Value &value, TokenizerState &ts);

    // parse an object, within { } delimiters parsed by the caller
    bool ParseObject(Value &value, TokenizerState &ts);

    // parse an array, within [ ] delimiters parsed by the caller
    bool ParseArray(Value &value, TokenizerState &ts);

    // parse a token - returns false at eof
    bool GetToken(Token &token, TokenizerState &ts);

    // peek at the next token without consuming it
    bool PeekToken(Token &token, TokenizerState &ts) { 
        TokenizerState tmp(ts);
        return GetToken(token, tmp); 
    }

    // Parse a number value.  Returns a combination of NUMLEX_xxx flags
    // with details on the lexical format of the number.
    static uint32_t ParseNumberToken(Token &token, TokenizerState &ts);

    // If this bit is set in the ParseNumberToken() return flags, it
    // means that the number is explicitly an integer lexically, because
    // it contains a radix prefix.
    static const uint32_t NUMLEX_INT = 0x0001;

    // If this bit is set in the ParseNumberToken() return flags, it
    // means that the number if explicitly a floating-point value lexcially,
    // because it contains a '.' or an exponent marker.  Some callers might
    // wish to distinguish floats from integer values even when the numeric
    // value itself is an integer, since this can affect how the value is
    // used in containing expressions.  For example, a caller implementing
    // a C-like arithmetic expression language might treat "3" as an
    // integer but "3.0" (or even just "3.") as a float.
    static const uint32_t NUMLEX_FLOAT = 0x0002;

    // Skip a newline.  This skips a CR-LF pair if present.
    static void SkipNewline(const char* &p, const char *endp);

    // String pool.  For strings that contain escaped text, we have to
    // expand the escape sequences to obtain the string value, so we can't
    // store direct pointers into the source text for these in the parse
    // tree.  Instead, we make a copy of each such string and add it to
    // this list, ensuring that each expanded string remains valid for
    // the life of the parse tree, and ensuring that it's deleted when
    // the parse tree is deleted.
    std::list<std::string> stringPool;

    // Allocate a property object.  Since these objects are small, and we
    // might allocate a lot of them for a large JSON file, allocating them
    // as individual heap objects is inefficient.  Instead, we use a pooled
    // allocator that allocates the objects out of fixed-size arrays.
    PropMap::Prop *AllocPropMapProp();

    // PropMap::Prop pool
    struct PropMapPropPool
    {
        PropMap::Prop props[128];
        int nextFree = 0;
        PropMapPropPool *nxt = nullptr;
    };
    PropMapPropPool *propMapPropPool = nullptr;

    // Error list.  This captures a list of error messages generated
    // during parsing.
    struct Error
    {
        Error(const Token &tok, const char *message) : 
            src(tok.txt), lineNum(tok.lineNum), message(message) { }

        // source location, as a pointer into the Parse() argument buffer
        const char *src;

        // line number
        int lineNum;

        // error message
        std::string message;
    };
    std::list<Error> errors;
};
