// Pinscape Pico - "Thunk", for associating contexts with context-less callbacks
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// How to use:
//
// 1. Install an IRQ handler, using a member function:
//
//   #include "ThunkManager.h"
//   class MyGPIO
//   {
//   public:
//     MyGPIO(int gpNum)
//     {
//       gpio_add_raw_irq_handler(gpNum, thunkManager.Create(&MyGPIO::IRQ, this));
//     }
//     void IRQ() { /* IRQ handler as a member function, with normal 'this' access */ }
//   };
//
// 2. Install an IRQ handler, using a static function taking a callback context:
//
//   #include "ThunkManager.h"
//   struct IRQContext { /* context struct elements... */ };
//   static void IRQ(IRQContext *context) { /* IRQ handler, has access to context */ }
//   
//   void main()
//   {
//     // when GP10 interrupts, call IRQ(context1)
//     IRQContext *context1 = new IRQContext(1);
//     gpio_add_raw_irq_handler(10, thunkManager.Create(IRQ, context1));
//
//     // when GP11 interrupts, call IRQ(context2)
//     IRQContext *context2 = new IRQContext(2);
//     gpio_add_raw_irq_handler(11, thunkManager.Create(IRQ, context2));
//   }
// 
//
// Overview:
//
// Provides a mechanism for creating "thunks": function adapters that let you
// provide a plain void(*)() function pointer to the Pico SDK where it calls
// for one, but use it to invoke a function you provide that takes a context
// object pointer as its argument.  This lets you use C++ objects to handle
// hardware callbacks that don't provide any context information.
//
// Why do we call this a "thunk"?  Microsoft originally coined the term in the
// 16-bit x86 days to refer to stubs that bridged calls between 16-bit and
// 32-bit code, but it has since come to be used more generally for any sort
// of indirect calling stub.  We re-speialize it here for our context-less
// to context-full adapter functions.
//
// In a Pico context, this is particularly useful for IRQ handlers, since IRQ
// vectors at the hardware level are just plain static void() function pointers.
// There's no way to pass a context to the function through the IRQ vector, so
// it's difficult to re-use the same function for multiple interrupt handlers.
// There are a few other SDK interfaces that likewise require a function
// callback but don't provide any way to store a context with it.
//
// Suppose that you have an IRQ handler that you wish to re-use to handle more
// than one interrupt.  For example, say that you're defining a class for a chip
// with an IRQ signal, and there might be more than one physical instance of the
// chip attached to the Pico, and each chip's interrupt signal output line is
// wired to a separate Pico GPIO input.  Since both interrupts are coming from
// the same type of chip, we'd like to write a class that represents the chip
// device driver, and then create two instances of the class, one for each
// physical chip in the system.  The problem arises when we try to install the
// IRQ handler.  Each instance of the chip needs a separate IRQ handler, but
// they're both represented by the same class.  Naively, we want to do this:
//
//   gpio_add_raw_irq_handler(gpio, &chip1->IRQ);
//   gpio_add_raw_irq_handler(gpio, &chip2->IRQ);
//
// But of course you can't do that, because &chip->IRQ is a high-level
// composite C++ type ("pointer to member function") that isn't convertible to
// the void(*)() type that the hardware requires here.  It might be tempting
// to try something like this next:
//
//   gpio_add_raw_irq_handler(gpio, [chip1](){ chip1->IRQ(); });
//   gpio_add_raw_irq_handler(gpio, [chip2](){ chip2->IRQ(); });
//
// But you can't do that, either, for basically the same reason: a lambda-
// with-captures is also a composite C++ type that isn't convertible to
// void(*)().
// 
// This is basically an insoluble problem at the C++ level.  The only entity in
// C++ that represents a combination "function pointer with context object" is a
// lambda with captures, and that's simply not convertible to a regular function
// pointer.  There's no magic cast that can accomplish it because the hardware
// data structure that represents a lambda can't be reduced to any hardware
// pointer type.
//
// If we step back and think about it at the machine-code level, we note that
// a void(*)() function actually does have SOME context, namely the address of
// the machine code that the pointer points to.  So if we could dynamically
// COMPILE a new function for each callback+context combination, where the
// new function contains the callback+context pointers as internal variables,
// we'd have our solution.  Essentially, we need to compile a little function
// that looks like this:
//
//    void DynamicallyGeneratedFunction() {
//      void *ctx = reinterpret_cast<void*>(0x12345678);  // replace 0x1234578 with the actual context pointer
//      void *func = reinterpret_cast<void(*)()>(0x89ABCDEF); // replace 0x89ABCDEF with the actual function pointer
//      func(ctx);
//    }
//
// Of course, C++ doesn't have a std::compile() function that would let us
// construct a text block like that and compile it.  But we can do the next
// best thing: we can hand-compile a function of the FORM above, to get the
// series of ARM Thumb opcode bytes that the function compiles to, and then
// use that as a template to do the equivalent of our notional std::compile()
// operation each time we need a new function, patching up the immediate data
// that gets loaded for the 'ctx' and 'func' pointers.  The function isn't
// anything magical at that point - it's just a malloc() block filled with
// the series of opcode bytes from our template function.
// 

#pragma once
#include <stdio.h>
#include <stdint.h>
#include <pico/stdlib.h>

// global singleton
class ThunkManager;
extern ThunkManager thunkManager;

// Thunk manager
class ThunkManager
{
public:
    // Thunk function type.  This is a simple void(*)()
    using Thunk = void(*)();

    // Check if a new thunk is available.  Callers can check ahead of time to
    // make sure that a new thunk can be allocated, so that they can fail
    // early and thereby avoid committing other resources that will go unused
    // when the thunk allocation fails.  (This always returns true in the
    // dynamic ARM implementation that we use in Pinscape Pico, because the
    // only limit on thunk generation is overall malloc memory, and we assume
    // that the caller has bigger problems if we're that low on malloc memory.
    // This is included for the sake of abstraction in the interface, to allow
    // for a less dynamic implementation that allocates thunks from a pre-
    // allocated pool of static objects.  It's possible to use such a scheme
    // to define thunks in portable C++ only, without resorting to any
    // machine-specific coding, but it results in the total number of thunks
    // being limited by the pre-allocated pool size.  The dynamic version that
    // we use has no pre-set limits but does require some non-portable code.)
    bool IsThunkAvailable() const;

    // Allocate a new thunk.  This hands back a thunk that can be used as an
    // IRQ vector function or any other kind of context-less callback required
    // by the Pico SDK.  When invoked, the thunk calls the specified function
    // with the specified context object.
    //
    // Important: the context object must have session lifetime, because the
    // reference is stored permanently in the thunk table, and passed to the
    // callback on each invocation.
    Thunk Create(void (*callback)(void*), void *context);

    // Create a thunk that takes a specified context object pointer type:
    //
    // class C { ... };
    // static void F(C *c) { ... };
    // C *c = new C();
    // auto thunk = thunkManager.Create(&F, c);
    //
    // Important: the context must have session lifetime.
    //
    // The type conversion is handled with an inner context object
    // (struct Context) that encapsulates the user's callback function
    // and context.  We then provide a generic basic void(*)() function
    // that takes the inner context object as its argument and extracts
    // the user's function and context, appropriately typed.  The inner
    // context object is never deleted, which isn't a leak - it has to
    // remain valid for the lifetime of the session because it's stored
    // in the thunk table as the actual thunk callback context.
    template<class T> Thunk Create(void (*userFunc)(T*), T *userCtx) {
        struct Context { void (*userFunc)(T*); T *userCtx; };
        Context *ctx = new Context();
        ctx->userFunc = userFunc;
        ctx->userCtx = userCtx;
        return Create([](void *pv) {
            Context *ctx = reinterpret_cast<Context*>(pv);
            return ctx->userFunc(ctx->userCtx);
        }, ctx);
    }

    // Create a thunk that invokes a member function on a given object:
    //
    //   class C {
    //     void A() { ... }
    //   };
    //   C *c = new C();
    //   auto thunk = thunkManager.Create(&C::A, c);
    //
    // Important: the object instance must have session lifetime.
    //
    // This works just like the typed-context adapter above, but using
    // a 'this' pointer as the context, and doing that type conversion
    // in the adpater callback.  The inner context object isn't deleted
    // before return beecause it's stored in the thunk table, and must
    // therefore have session lifetime.
    template<class T> Thunk Create(void (T::*func)(), T *obj) {
        struct Context { void (T::*func)(); T *obj; };
        Context *ctx = new Context;
        ctx->func = func;
        ctx->obj = obj;
        return Create([](void *pv) {
            Context *ctx = reinterpret_cast<Context*>(pv);
            return (ctx->obj->*(ctx->func))();
        }, ctx);
    }

    // Delete a thunk.  This frees the memory associated with the
    // dynamic function object.  It's up to the caller to ensure that
    // the thunk isn't being referenced from an IRQ vector or any other
    // context where the thunk could be invoked as a function.
    void Delete(Thunk thunk);
};
