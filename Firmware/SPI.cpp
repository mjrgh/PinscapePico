// Pinscape Pico - SPI manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

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
#include "Logger.h"
#include "GPIOManager.h"
#include "SPI.h"

// global singleton instances
SPI SPI::inst[2]{
    { 0 },
    { 1 }
};

// reserve an SPI unit for exclusive use
bool SPI::ReserveExclusive(const char *newOwnerName)
{
    // if the unit has already been reserved, fail and log the conflict
    if (exclusiveOwner != nullptr)
    {
        Log(LOG_ERROR, "%s: can't assign SPI%d; the unit is already assigned to %s",
            newOwnerName, unit, exclusiveOwner);
        return false;
    }
    if (nSharedOwners != 0)
    {
        Log(LOG_ERROR, "%s: can't assign SPI%d for exclusive use; the unit is already assigned to %s%s\n",
            newOwnerName, unit, firstSharedOwner, nSharedOwners == 0 ? "" : " et al");
        return false;
    }

    // it's available - note the new owner and return success
    exclusiveOwner = newOwnerName;
    return true;
}

// reserve an SPI unit for shared use, on a bus with multiple devices
// that implement the standard SPI Chip Select mechanism
bool SPI::ReserveShared(const char *newOwnerName)
{
    // if the unit has already been reserved exclusive, fail and log the conflict
    if (exclusiveOwner != nullptr)
    {
        Log(LOG_ERROR, "%s: can't assign SPI%d; the unit is already assigned exclusively to %s",
            newOwnerName, unit, exclusiveOwner);
        return false;
    }

    // add the shared owner; if this is the first, remember its name
    if (nSharedOwners++ == 0)
        firstSharedOwner = newOwnerName;

    // success
    return true;
}

// Infer the SPI unit for a set of GPIO pin assignments
SPI *SPI::InferUnit(int sck, int tx, int rx, int csn)
{
    // The SCLK and SIN pins must be SPI SCK and TX capable pins
    // (respectively) connected to the same SPI unit.  Note that
    // this struct lists the pin assignments in SCK/TX pairs by
    // proximity, but there's no requirement that the assigned
    // pins have to be paired the same way - the only requirement
    // is that they're on the same unit (SPI0 or SPI1).  For
    // example, SCLK=GP2 and SIN=GP19 is perfectly fine, since
    // both pins are capable of their required roles and they're
    // both on SPI0.
    static const struct {
        int unit; int rx; int csn; int sck; int tx;
    } spi[] = 
    {
        { 0, 0, 1, 2, 3 },      // SPI0 on GP0, GP1, GP2, GP3
        { 0, 4, 5, 6, 7 },      // SPI0 on GP4, GP5, GP6, GP7
        { 1, 8, 9, 10, 11 },    // SPI1 on GP8, GP9, GP10, GP11
        { 1, 12, 13, 14, 15 },  // SPI1 on GP12, GP13, GP14, GP15
        { 0, 16, 17, 18, 19 },  // SPI0 on SP16, SP17, GP18, GP19
    };
    int spiSck = -1, spiTx = -1, spiRx = -1, spiCsn = -1;
    for (size_t i = 0 ; i < _countof(spi) ; ++i)
    {
        if (spi[i].sck == sck)
            spiSck = spi[i].unit;
        if (spi[i].tx == tx)
            spiTx = spi[i].unit;
        if (spi[i].rx == rx)
            spiRx = spi[i].unit;
        if (spi[i].csn == csn)
            spiCsn = spi[i].unit;
    }

    // Fail if any pin specified didn't match its specified SPI function.
    // At this stage, ignore pins that aren't specified.
    if ((sck != -1 && spiSck == -1)
        || (tx != -1 && spiTx == -1)
        || (rx != -1 && spiRx == -1)
        || (csn != -1 && spiCsn == -1))
        return nullptr;

    // Make sure all of the matched units agree.  If so, return the
    // matching unit number, otherwise return -1 to indicate a conflict.
    int unit = -1;
    auto Test = [&unit](int n) {
        if (n == -1)
            return true;
        else if (unit == -1)
            return (unit = n), true;
        else
            return unit == n;
    };
    if (Test(spiSck) && Test(spiTx) && Test(spiRx) && Test(spiCsn))
        return &inst[unit];
    else
        return nullptr;
}
