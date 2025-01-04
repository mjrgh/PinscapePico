// Pinscape Pico - SPI manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module provides some basic utility resource management functions
// layered on the Pico SPI hardware.
//
// Currently, the Pinscape software doesn't support any SPI devices, but
// it does use the SPI hardware as an implementation detail to access to
// one non-SPI device, specifically the TLC5940 PWM LED controller.
// TLC5940 isn't an SPI device, but it has an ad hoc synchronous serial
// data interface with physical signal voltage and timing that happens
// to be compatible with the SPI clock and data.  That makes it
// convenient to use the SPI hardware for the chip's serial interface;
// the alternative would be to bit-bang the clock and serial data bit
// signals, which would be CPU-intensive and not amenable to DMA.  The
// thing that makes the TLC5940 not a true SPI device is that it doesn't
// implement a Chip Select mechanism, which makes it impossible for
// other devices to share the same bus.
//
// The main functions provided here are pin identification and resource
// management, specifically, a mechanism for reserving an SPI unit for a
// caller's exclusive use.  The purpose is to detect conflicting SPI
// usage in the user-supplied configuration data so that we can provide
// useful troubleshooting diagnostics, without having to make the
// TLC5940 subsystem directly aware of any other SPI-related subsystems
// that might come along in the future.  As long as any future subsystem
// that wants to access SPI also uses the reservation system, we'll be
// able to generate conflict logging messages without having to also add
// special awareness in the TLC5940 module of the new subsystem.
//
// FUTURE DIRECTIONS: If we ever add support for any native SPI devices,
// we'll need to expand the SPI class to use the same shared bus design
// that we current implement in the I2C class, with bus scheduling and
// asynchronous DMA transfers.  We don't need this currently simply
// because we don't have any SPI devices to include in the mix.  The
// TLC5940 doesn't need bus scheduling because it can't share a bus.

#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/spi.h>
#include <hardware/dma.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"

// external/forward declartions
class JSONParser;

// SPI manager.  Each instance manages one SPI unit.
class SPI
{
public:
    // construction
    SPI(int unit) : unit(unit), hw(unit == 0 ? spi0 : spi1) { }

    // Global singleton instances, one per Pico SPI controler unit
    static SPI inst[2];

    // Configure SPI bus units from the JSON data.  This is currently
    // just a no-op placeholder, since we don't support any SPI devices
    // at the moment.  If we add support for any SPI devices in the
    // future, this will need to read and apply the JSON configuration
    // for one or both Pico SPI units; the corresponding code in the I2C
    // class should serve as a model.
    static void Configure(JSONParser &) { }

    // Infer which Pico SPI unit to use for a given function, based on
    // the GPIO pins assigned.  Any combination of pins can be supplied;
    // any pins that aren't used should be set to -1.  Returns the SPI
    // unit number (0 or 1) that matches all of the supplied GPIO pins
    // in their respective SPI capacities, or -1 if the pins don't map
    // to the specified SPI features OR if they don't all refer to the
    // same unit.
    //
    // This doesn't log any error messages, and it doesn't require that
    // the full set of pin assignments is specified - any of the pins
    // can be set to -1, as long as there's at least one pin provided.
    // (We can't infer the unit if no pins are specified, obviously.)
    // The caller must enforce any requirements as to which pins must
    // be specified, since this can vary by use case.  A general SPI
    // bus master would generally need SCK, TX, and RX; a slave device
    // might also need the Chip Select signal.
    static SPI *InferUnit(int sck, int tx, int rx, int csn);

    // get the Pico SDK hardware instance for a given SPIn unit number
    spi_inst_t *GetHW() const { return hw; }

    // Reserve an SPI unit for exclusive use.  If the unit is available,
    // this marks the unit as reserved and returns true; if another
    // caller has already reserved the unit, this logs an error and
    // returns false.  The 'owner name' gives a string (which must have
    // static storage duration) to identify the new owner; this will be
    // included in any error messages we log, to identify the conflicting
    // subsystems.
    //
    // This doesn't have any effect on the underlying Pico hardware; the
    // reservation is only effective if all code that accesses the SPI
    // hardware uses the reservation system.  The main point of the
    // reservation system is to provide diagnostics to the user in case
    // the user-supplied configuration uses the same SPI unit for more
    // than one subsystem.  The subsystems might otherwise be unaware of
    // each other (by design, for modularity), so the reservation system
    // provides a structured way for self-contained subsystems to
    // negotiate their global resource usage.
    bool ReserveExclusive(const char *ownerName);

    // Reserve an SPI unit for shared access.  This marks the unit as
    // being used for an SPI bus that might have multiple devices
    // attached.  A bus that's been reserved for exclusive use can't
    // be used for shared access, and vice versa.  Returns true on
    // success, false if the bus has been reserved in exclusive mode.
    // On failure, this logs an error describing the conflict, to help
    // the user troubleshoot the configuration.
    bool ReserveShared(const char *ownerName);

protected:
    // unit number
    int unit;

    // Pico SDK hardware instance
    spi_inst_t *hw;
    
    // Exclusive owner.  See ReserveExclusive().
    const char *exclusiveOwner = nullptr;

    // Shared owners.  See ReserveShared().  We only record one
    // shared owner, but we keep track of the number of owners.
    const char *firstSharedOwner = nullptr;
    int nSharedOwners = 0;
};
