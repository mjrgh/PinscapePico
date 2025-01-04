// Pinscape Pico - "Thunk", for associating contexts with context-less callbacks
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module implements "thunks": dynamically allocated stub functions that
// expose context-less void(*)() interfaces, suitable for use in IRQ vectors
// and other similar cases, and invoke a caller's function with a caller-
// supplied context object as argument.  The thunk thus serves as an adapter
// between hardware callers that don't allow any context to be stored, and
// higher-level software that uses context objects.
//
//   static void MyCallback(void *context) { ... }
//   void *myContext = new MyContextStruct(...);
//   irq_set_exclusive_handler(irqNum, thunkManager.Create(MyCallback, myContext);
//
// Higher-level operations are also provided for calls with typed (not just
// opaque void*) context objects, and for calls to member functions with a
// specified 'this' object.
//
//    irq_set_exclusive_handler(irqNum, thunkManager.Create(&ThisClass::Func, this));
//
// The way this works is to observe that a hardware callback doesn't allow
// storing anything beyond a function pointer, but it DOES allow storing a
// SPECIFIC function pointer.  That means that the function pointer itself can
// serve as its own context.  To make that work, though, we need a unique
// function pointer - a unique block of machine opcodes at a unique address -
// for each context.  We accomplish that by malloc()'ing a block of memory,
// and then dynamically "compiling" a function into the memory that invokes
// the specific user function with the specific context object.  C++ doesn't
// have a native way of compiling new code dynamically, but we can fake it by
// constructing the appropriate series of ARM opcodes by hand.  This is easier
// than it sounds, because the dynamic function we need to generate is
// extremely simple: all it has to do is load the context pointer into
// register R0 and invoke the user callback function.  The whole thing only
// amounts to six Thumb opcodes, including the function prolog and epilog,
// so this adds very little overhead vs calling a static function directly.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <new>

#include <pico/stdlib.h>

#include "Pinscape.h"
#include "Utils.h"
#include "Logger.h"
#include "ThunkManager.h"

// global singleton
ThunkManager thunkManager;

// allocate a thunk
ThunkManager::Thunk ThunkManager::Create(void (*callback)(void*), void *context)
{
    // Thunk code template.  This is the byte sequence of the series of ARM
    // Thumb-mode opcodes shown in the comments alongside the byte values.
    // This implements the following C++ code (using the code-generation
    // conventions of the ARM gcc supplied with the Pico SDK):
    //
    // void func() {
    //   reinterpret_cast<void(*)()>(0x00000000)(reinterpret_cast<void*>(0x00000000));
    // }
    //
    // The constant 0x00000000 values are just placeholders that we patch with
    // the actual 'callback' and 'context' pointers passed in by our caller.
    const static uint8_t codeTemplate[] = {
        0x10, 0xB5,             // 0000  push {r4, lr}     ; Stack frame setup
        0x02, 0x48,             // 0002  ldr r0, [pc, #8]  ; R0 <- WORD [000C] - context pointer (argument 0)
        0x00, 0xBF,             // 0004  nop               ; Padding (to get the next ldr onto a 4-byte boundary)
        0x02, 0x4B,             // 0006  ldr r3, [pc, #8]  ; R3 <- WORD [0010] - callback function pointer
        0x98, 0x47,             // 0008  blx r3            ; Call [R3]
        0x10, 0xBD,             // 000A  pop {r4, pc}      ; Return from subroutine
        0x00, 0x00, 0x00, 0x00, // 000C  .word 0x00000000  ; placeholder; patched with 'context'
        0x00, 0x00, 0x00, 0x00, // 0010  .word 0x00000000  ; placehodler; patched with 'callback'
    };

    // allocate memory for the thunk
    uint8_t *thunk = new (std::nothrow) uint8_t[sizeof(codeTemplate)];
    if (thunk == nullptr)
        return nullptr;

    // copy the template
    memcpy(thunk, codeTemplate, sizeof(codeTemplate));

    // populate the context pointer and function pointer in the thunk
    *reinterpret_cast<uint32_t*>(&thunk[0x000C]) = reinterpret_cast<uint32_t>(context);
    *reinterpret_cast<uint32_t*>(&thunk[0x0010]) = reinterpret_cast<uint32_t>(callback);
    static_assert(sizeof(intptr_t) == sizeof(uint32_t));

    // Return the object, cast to a function pointer.  This requires some bit
    // manipulation that's extremely peculiar to ARM.  ARM takes the code mode
    // when invoking a function pointer from the low-order bit of the pointer.
    // For Thumb mode, the bit must be set to '1'.  So we have to take the
    // actual memory address of our new thunk object and OR a '1' bit into
    // the low-order bit to form the actual function pointer.
    return reinterpret_cast<Thunk>(reinterpret_cast<intptr_t>(thunk) | 0x00000001);
}

// check availability
bool ThunkManager::IsThunkAvailable() const
{
    // With our dynamic code generation scheme, we can always create a new
    // thunk as long as there's malloc memory available, which we'll take for
    // granted at this point, so always return true.  This routine is included
    // in the abstract interface to allow for a less dynamic implementation,
    // where the thunks come from a statically pre-allocated pool, and thus
    // are a much more limited resource than overall RAM.
    return true;
}

// delete a thunk
void ThunkManager::Delete(Thunk thunk)
{
    // To recover the raw memory pointer, cast the thunk pointer to an
    // intptr_t, and remove the "Thumb bit" (the '1' in the least
    // significant bit that marks a pointer to Thumb code on ARM)
    intptr_t addr = reinterpret_cast<intptr_t>(thunk) & ~static_cast<intptr_t>(0x00000001);

    // Now we have the malloc address in integer format.  We originally
    // allocated this via 'new uint8_t[]', so deallocate it with the
    // corresponding delete.
    delete[] reinterpret_cast<uint8_t*>(addr);
}
