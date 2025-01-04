// Pinscape Pico firmware - IR Protocol Identifiers
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Each protocol has an arbitrarily assigned integer identifier.  We use
// these in the universal command code representation to tell us which
// protocol the code was received with, and which to use when
// transmitting it.  This provides a standard way of storing codes on
// the device and host side and transferring them across the USB
// connection and between PCs.

#pragma once
#include <stdint.h>

const uint8_t IRPRO_NONE = 0;         // no protocol, unknown, or invalid
const uint8_t IRPRO_NEC32 = 1;        // NEC 32-bit with 9000us/4500us headers
const uint8_t IRPRO_NEC32X = 2;       // NEC 32-bit with 4500us/4500us headers
const uint8_t IRPRO_NEC48 = 3;        // NEC 48-bit with 9000us/4500us headers
const uint8_t IRPRO_RC5 = 4;          // Philips RC5
const uint8_t IRPRO_RC6 = 5;          // Philips RC6
const uint8_t IRPRO_KASEIKYO48 = 6;   // Kaseikyo 48-bit
const uint8_t IRPRO_KASEIKYO56 = 7;   // Kaseikyo 56-bit
const uint8_t IRPRO_DENONK = 8;       // Denon-K (Kaseikyo-48 with OEM 54:32)
const uint8_t IRPRO_FUJITSU48 = 9;    // Fujutsu 48-bit (Kaseikyo-48 with OEM 14:63)
const uint8_t IRPRO_FUJITSU56 = 10;   // Fujitsu 56-bit (Kaseikyo-56 with OEM 14:63)
const uint8_t IRPRO_JVC48 = 11;       // JVC 48-bit (Kaseikyo-48 with OEM 03:01)
const uint8_t IRPRO_JVC56 = 12;       // JVC 56-bit (Kaseikyo-56 with OEM 03:01)
const uint8_t IRPRO_MITSUBISHIK = 13; // Mitsubishi-K (Kaseikyo-48 with OEM 23:CB)
const uint8_t IRPRO_PANASONIC48 = 14; // Panasonic 48-bit (Kaseikyo-48 with OEM 02:20)
const uint8_t IRPRO_PANASONIC56 = 15; // Panasonic 56-bit (Kaseikyo-56 with OEM 02:20)
const uint8_t IRPRO_SHARPK = 16;      // Sharp 48-bit (Kaseikyo-48 with OEM AA:5A)
const uint8_t IRPRO_TEACK = 17;       // Teac-K (Kaseikyo-48 with OEM 43:53)
const uint8_t IRPRO_DENON = 18;       // Denon 15-bit
const uint8_t IRPRO_PIONEER = 19;     // Pioneer (NEC 32-bit with "shift" extensions)
const uint8_t IRPRO_SAMSUNG20 = 20;   // Samsung 20-bit
const uint8_t IRPRO_SAMSUNG36 = 21;   // Samsung 36-bit
const uint8_t IRPRO_SONY8 = 22;       // Sony 8-bit
const uint8_t IRPRO_SONY12 = 23;      // Sony 12-bit
const uint8_t IRPRO_SONY15 = 25;      // Sony 15-bit
const uint8_t IRPRO_SONY20 = 26;      // Sony 20-bit
const uint8_t IRPRO_ORTEKMCE = 27;    // OrtekMCE
const uint8_t IRPRO_LUTRON = 28;      // Lutron lights, fans, and home automation
const uint8_t IRPRO_TCLROKU = 29;     // TCL Roku models (NEC 32 with doubled code words)
const uint8_t IRPRO_JVC16 = 30;       // JVC custom 16-bit protocol
