// Pinscape Pico firmware - IR Protocol List
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This file provides a list of the protocol singletons.  It's designed
// to be included multiple times in the compilation.  On each inclusion,
// we insert a desired bit of code for each of the singletons.  We use
// this to declare all of the singleton instances, then to call various
// methods in all of them.
//
// By convention, the singletons are named s_ClassName.
//
// To use this file, #define ONE of the following macros:
//
// * IR_PROTOCOL_RXTX(className) - define this if you want to include 
//   expansions for all of the protocol classes, send or receive.  This 
//   includes the dual use classes, the send-only classes, and the
//   receive-only classes.
//
// * IR_PROTOCOL_RX(className) - define this if you only want to include 
//   expansions for RECEIVER protocol classes.  This includes dual-purpose 
//   classes with both sender and receiver, plus the receive-only classes.
//
// * IR_PROTOCOL_TX(className) - define this if you only want to include 
//    expansions for TRANSMITTER protocol classes.  This includes the
//    dual-purpose classes with both sender and receiver, plus the 
//    transmit-only classes.
//
// After #define'ing the desired macro, #include this file.  This file
// can be #include'd multiple times in one file for different expansions
// of the list.


// Internally, we use the same three macros:
//
//   IR_PROTOCOL_RXTX(className) - define a send/receive class
//   IR_PROTOCOL_RX(className)   - define a send-only class
//   IR_PROTOCOL_TX(className)   - define a receive-only class
//
// To set things up, see which one the caller defined, and implicitly
// create expansions for the other two.  If the caller wants all classes,
// define _RX and _TX to expand to the same thing as _RXTX.  If the caller
// only wants senders, define _RXTX to expand to _TX, and define _RX to
// empty.  Vice versa with receive-only.
//
#if defined(IR_PROTOCOL_RXTX)
# define IR_PROTOCOL_RX(cls) IR_PROTOCOL_RXTX(cls)
# define IR_PROTOCOL_TX(cls) IR_PROTOCOL_RXTX(cls)
#elif defined(IR_PROTOCOL_RX)
# define IR_PROTOCOL_RXTX(cls) IR_PROTOCOL_RX(cls)
# define IR_PROTOCOL_TX(cls)
#elif defined(IR_PROTOCOL_TX)
# define IR_PROTOCOL_RXTX(cls) IR_PROTOCOL_TX(cls)
# define IR_PROTOCOL_RX(cls)
#endif

// define the protocol handlers
IR_PROTOCOL_RXTX(IRPNEC_32_48)
IR_PROTOCOL_RXTX(IRPNEC_32x)
IR_PROTOCOL_RXTX(IRPRC5)
IR_PROTOCOL_RXTX(IRPRC6)
IR_PROTOCOL_RXTX(IRPSony)
IR_PROTOCOL_RXTX(IRPDenon)
IR_PROTOCOL_RXTX(IRPKaseikyo)
IR_PROTOCOL_RXTX(IRPSamsung20)
IR_PROTOCOL_RXTX(IRPSamsung36)
IR_PROTOCOL_RXTX(IRPLutron)
IR_PROTOCOL_RXTX(IRPOrtekMCE)
IR_PROTOCOL_RXTX(IRPJVC16)

// clear the macros to make way for future definitions
#undef IR_PROTOCOL_RXTX
#undef IR_PROTOCOL_RX
#undef IR_PROTOCOL_TX
