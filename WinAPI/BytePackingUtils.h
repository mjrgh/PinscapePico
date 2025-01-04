// Pinscape Pico - Byte Packing/Unpacking Utilities
// Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Helper functions for protocol encoders/decoders.  These transfer
// values between native integer formats and the little-endian wire
// format.

#pragma once
#include <stdint.h>

static inline void PutUInt16(uint8_t* &p, uint16_t i) {
    *p++ = static_cast<uint8_t>(i & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 8) & 0xFF);
}
static inline void PutInt16(uint8_t* &p, int16_t i) { PutUInt16(p, static_cast<uint16_t>(i)); }

static inline void PutUInt32(uint8_t* &p, uint32_t i) {
    *p++ = static_cast<uint8_t>(i & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 8) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 16) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 24) & 0xFF);
}
static inline void PutInt32(uint8_t* &p, int32_t i) { PutUInt32(p, static_cast<uint32_t>(i)); }

static inline void PutUInt64(uint8_t* &p, uint64_t i) {
    *p++ = static_cast<uint8_t>(i & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 8) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 16) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 24) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 32) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 40) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 48) & 0xFF);
    *p++ = static_cast<uint8_t>((i >> 56) & 0xFF);
}
static inline void PutInt64(uint8_t* &p, int64_t i) { PutUInt64(p, static_cast<uint64_t>(i)); }

static inline void PutBytes(uint8_t* &p, void *src, size_t len) { memcpy(p, src, len); p += len; }

static inline uint16_t GetUInt16(const uint8_t* &p) {
    uint16_t i = static_cast<uint16_t>(*p++);
    i |= static_cast<uint16_t>(*p++) << 8;
    return i;
}
static inline int16_t GetInt16(const uint8_t* &p) { return static_cast<int16_t>(GetUInt16(p)); }

static inline uint32_t GetUInt32(const uint8_t* &p) {
    uint32_t i = static_cast<uint32_t>(*p++);
    i |= static_cast<uint32_t>(*p++) << 8;
    i |= static_cast<uint32_t>(*p++) << 16;
    i |= static_cast<uint32_t>(*p++) << 24;
    return i;
}
static inline int32_t GetInt32(const uint8_t* &p) { return static_cast<int32_t>(GetUInt32(p)); }

static inline uint64_t GetUInt64(const uint8_t* &p) {
    uint64_t i = static_cast<uint64_t>(*p++);
    i |= static_cast<uint64_t>(*p++) << 8;
    i |= static_cast<uint64_t>(*p++) << 16;
    i |= static_cast<uint64_t>(*p++) << 24;
    i |= static_cast<uint64_t>(*p++) << 32;
    i |= static_cast<uint64_t>(*p++) << 40;
    i |= static_cast<uint64_t>(*p++) << 48;
    i |= static_cast<uint64_t>(*p++) << 56;
    return i;
}
static inline int64_t GetInt64(const uint8_t* &p) { return static_cast<int64_t>(GetUInt64(p)); }

static inline void GetBytes(const uint8_t* &p, void *dst, size_t len) { memcpy(dst, p, len); p += len; }

