// Pinscape Pico - PCA9555 port extender chip interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>
#include <malloc.h>
#include <ctype.h>
#include <math.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <pico/flash.h>
#include <pico/unique_id.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>

// project headers
#include "PCA9555.h"
#include "JSON.h"
#include "Logger.h"
#include "GPIOManager.h"
#include "../USBProtocol/VendorIfcProtocol.h"

// global chip list
std::list<PCA9555> PCA9555::chips;

// Configure PCA9555 chips from the config data
//
// pca9555: [                // array of PCA9555 chip objects
//   {
//     i2c: <number>,        // Pico I2C bus number (0 or 1)
//     addr: <number>,       // I2C address in 7-bit format
//     interrupt: <number>   // optional GPIO number where interrupt signal from chip is connected
//   },
//   { ... },
// ]
//
// The chip ID number is an arbitrary unique ID assigned by the
// configuration, for the purpose of referencing the chip from
// other parts of the configuration that define usages of the
// chip's ports, such as button definitions.
//
void PCA9555::Configure(JSONParser &json)
{
    int nChips = 0;
    auto *cfg = json.Get("pca9555");
    if (cfg->IsArray() || cfg->IsObject())
    {
        cfg->ForEach([&nChips](int index, const JSONParser::Value *value)
        {
            // get and validate the I2C bus number
            char facility[20];
            snprintf(facility, _countof(facility), "pca9555[%d]", index);
            int bus = value->Get("i2c")->Int(-1);
            if (!I2C::ValidateBusConfig(facility, bus))
                return;

            // get and validate the I2C address
            uint8_t addr = value->Get("addr")->Int(0);
            if (addr < 0x20 || addr > 0x27)
            {
                Log(LOG_ERROR, "pca9555[%d]: invalid or missing I2C address (must be 0x20..0x27)\n", index);
                return;
            }

            // get the interrupt port
            int intr = value->Get("interrupt")->Int(-1);
            if (intr != -1 && !IsValidGP(intr))
            {
                Log(LOG_ERROR, "pca9555[%d]: invalid GP (%d) specified for interrupt input\n", index, intr);
                return;
            }
            if (!gpioManager.Claim(Format("PCA9555[%d] (Intr)", index), intr))
                return;

            // create the chip instance
            auto &chip = chips.emplace_back(index, bus, addr, intr);

            // add it to its bus manager
            I2C::GetInstance(bus, false)->Add(&chip);

            // set the initial output port levels as desired
            if (auto *initialOut = value->Get("initialOut"); initialOut->IsNumber())
            {
                // a number is a mask of the port on/off values, as a UINT16
                chip.outputReg.chipVal = chip.outputReg.newVal = initialOut->UInt16(0);
            }
            else if (initialOut->IsArray())
            {
                // an array gives the initial values by element, as 0/1 or boolean values
                unsigned int acc = 0;
                initialOut->ForEach([&acc](int i, const JSONParser::Value *ele) { acc |= (ele->Bool() ? 0 : 1) << i; });
                chip.outputReg.chipVal = chip.outputReg.newVal = static_cast<uint16_t>(acc);

                // if it has over 16 elements, warn
                if (initialOut->Length() > 16)
                    Log(LOG_WARNING, "PCA9555[%d].initialOut array has more than 16 elements; extra element ignored\n");
            }
            else if (!initialOut->IsUndefined())
            {
                Log(LOG_ERROR, "PCA9555[%d].initialOut is invalid; must be a number or array of number/boolean\n");
            }

            // success
            Log(LOG_CONFIG, "PCA9555[%d] configured on I2C%d addr 0x%02x\n", index, bus, addr);
        }, true);
    }
    else if (!cfg->IsUndefined())
        Log(LOG_ERROR, "Config: 'pca9555' key must be an object or array\n");

    // initialize the chips
    for (auto &chip : chips)
        chip.Init();
}

PCA9555 *PCA9555::Get(int chipNum)
{
    // search the list for a matching chip ID
    for (auto &chip : chips)
    {
        if (chip.chipNumber == chipNum)
            return &chip;
    }

    // not found
    return nullptr;
}

PCA9555::PCA9555(int chipNumber, uint8_t i2cBus, uint8_t i2cAddr, int gpInterrupt) :
    I2CDevice(i2cAddr),
    chipNumber(chipNumber), gpInterrupt(gpInterrupt)
{
    // remember the Pico I2C interface
    i2c = i2c_get_instance(i2cBus);

    // If a GPIO was specified for the interrupt input signal from the
    // chip, configure the GP as an input with a pull-up.
    //
    // Note that the signal is nominally an interrupt, per the data
    // sheet's terminology, but we don't literally wire it to an IRQ.
    // There's no point in doing that because we're not allowed to
    // access the I2C bus at just any time - we have to wait our turn
    // through the I2C bus manager object.  So we can simply check the
    // interrupt signal state when it's our turn on the I2C bus.  The
    // chip holds the signal low when it has a change to send, and keeps
    // holding it low until the host gets around to actually doing the
    // read, so the signal will patiently wait for us to get to it.
    //
    // The interrupt signal may be shared among multiple PCA9555 chips.
    // It's physically implemented as an open-drain output on the chip,
    // so multiple signals can be wired together to form a logical OR of
    // the interrupt states of all of the chips.  The reference hardware
    // design uses this approach to conserve GPIOs on the Pico - there's
    // only one GPIO that covers both of the PCA9555 chips in the
    // reference design.  This means that we can't identify which of the
    // chips is asserting the interrupt, so when we see the signal, we
    // have to keep reading the chips one by one until the signal
    // clears.  That's what the I2C manager's round-robin scheduler does
    // anyway, so it's a natural fit.  (Which is why I'm pointing it out
    // here: it's so implicit in the structure of the I2C manager's
    // design that it might not be apparent why we can get away with a
    // shared interrupt.)  It doesn't cost much extra to read all of the
    // chips when there are only two; in a design using six or eight of
    // the chips, it might make more sense to use finer granularity in
    // the interrupt sensing.
    if (gpInterrupt != -1)
    {
        gpio_init(gpInterrupt);
        gpio_set_dir(gpInterrupt, GPIO_IN);
        gpio_set_pulls(gpInterrupt, true, false);
    }
}

void PCA9555::Init()
{
    SendInitCommands();
}

void PCA9555::I2CReinitDevice(I2C *)
{
    SendInitCommands();
}

void PCA9555::SendInitCommands()
{
    // Note: during initialization, uniquely, we can use the SDK's
    // blocking I2C APIs.  During normal operation we have to use the
    // asynchronous interface through our class I2C.

    // Set all ports to Input mode.  That's the power-on default, so
    // this isn't completely necessary, but it has the useful side
    // effect of testing to see if the chip is receiving our commands,
    // which we can log if not.  It also ensures that the chip is
    // reinitialized to startup conditions if we went through a reset
    // cycle on the Pico without actually power cycling the whole system,
    // leaving the peripheral chips in whatever state they were in before
    // the Pico reset.
    //
    // The port direction is set through the/ port configuration registers.
    // A '1' bit sets the port as INPUT, and a '0' bit sets it as OUTPUT,
    // so set all bits to '1' (0xFF).
    bool ok = true;
    {
        uint8_t buf[] = { REG_CONFIG0, 0xFF, 0xFF };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        ok = (result == _countof(buf));
    }

    // Set the output level of all ports to their initial 'chip' values
    {
        uint8_t buf[] = { REG_OUTPUT0, static_cast<uint8_t>(outputReg.chipVal & 0xFF), static_cast<uint8_t>((outputReg.chipVal >> 8) & 0xFF) };
        int result = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), false, 1000);
        ok = ok && (result == _countof(buf));
    }

    // Read the initial status of all input ports
    {
        uint8_t buf[1] = { REG_INPUT0 };
        uint8_t inbuf[2];
        ok = i2c_write_timeout_us(i2c, i2cAddr, buf, _countof(buf), true, 1000) == _countof(buf)
             && i2c_read_timeout_us(i2c, i2cAddr, inbuf, _countof(inbuf), false, 1000) == _countof(inbuf)
             && ok;

        // the input bytes are INPUT0, INPUT1; encode these in our 16-bit
        // internal register, with INPUT1 in the high byte
        portBits = inbuf[0] | (static_cast<uint16_t>(inbuf[1]) << 8);
    }

    // log status
    Log(ok ? LOG_CONFIG : LOG_ERROR, "PCA9555(i2c addr 0x%02x) device initialization %s\n", i2cAddr, ok ? "OK" : "failed");
}

// enable/disable outputs across all chips
void PCA9555::EnableOutputs(bool enable)
{
    for (auto &chip : chips)
    {
        // set the new status
        chip.outputsEnabled = enable;

        // Update the Configuration Register.  If outputs are enabled,
        // set CR to match the logical port configuration for the chip.
        // If outputs are disabled, set CR to its initial setting of all
        // '1' bits, to set all ports as inputs.
        chip.configReg.newVal = enable ? chip.portModes : 0xFFFF;
    }
}

// Claim a port
bool PCA9555::ClaimPort(int portNum, const char *owner, bool asOutput)
{
    // validate the port number
    if (portNum < 0 || portNum > 15)
        return Log(LOG_ERROR, "PCA9555: invalid port number %d in claim by %s\n", portNum, owner), false;
    
    // Check to see if it's already claimed.  If so, check to see if
    // either the previous or new owner want to use the port as an
    // output.  It's fine for multiple owners to claim a port as an
    // input, since reading from a port has no side effects that
    // would interfere with the other owner.  But if either owner
    // wants to write the port, it must own it exclusively.
    if (portClaims[portNum].owner != nullptr  && (asOutput || portClaims[portNum].asOutput))
    {
        // the port is already in use, and one or the other caller
        // is using it as an output, so this is a conflict
        Log(LOG_ERROR, "PCA9555: port %d is already in use by %s; can't assign to %s\n",
            portNum, portClaims[portNum].owner, owner);
        return false;
    }

    // claim the port
    portClaims[portNum].owner = owner;
    portClaims[portNum].asOutput = asOutput;

    // Set its portModes bit - '1' for input mode, '0' for output mode.
    // This sets the internal logical state of the port.
    uint16_t portBit = (1U << portNum);
    if (asOutput)
        portModes &= ~portBit;
    else
        portModes |= portBit;

    // If outputs are enabled, update the Configuration Register, so
    // that the physical chip configuration matches our logical port
    // configuration.
    if (outputsEnabled)
        configReg.newVal = portModes;

    // success
    return true;
}

// Read a port.  This returns the port status as of the last polling
// cycle.
bool PCA9555::Read(uint8_t port)
{
    // get the port's bit mask in the registers
    uint16_t mask = 1 << port;

    // if it's an input port ('1' in the config register), return the
    // input portBits bit; otherwise, we're controlling it, so return the
    // outputReg bit
    return ((configReg.chipVal & mask) != 0) ?  // check the config register
        (portBits & mask) != 0 :                // '1' in config reg -> input mode -> use last input port bit
        (outputReg.chipVal & mask) != 0;        // '0' in config reg -> output mode -> use output register bit
}

// Write a port.  This queues the update to send during our next I2C
// access cycle.
void PCA9555::Write(uint8_t portNum, uint8_t level)
{
    outputReg.SetPortBit(portNum, level);
}

// Populate a Vendor Interface button query result buffer with
// ButtonDevice structs representing the configured PCA9555 chips.
// The caller is responsible for providing enough buffer space; we
// require one PinscapePico::ButtonDevice per chip.  On return, the
// buffer pointer is automatically incremented past the space
// consumed.
void PCA9555::PopulateDescs(PinscapePico::ButtonDevice* &descs)
{
    for (auto &chip : chips)
    {
        descs->configIndex = chip.chipNumber;
        descs->type = PinscapePico::ButtonDesc::SRC_PCA9555;
        descs->numPorts = 16;
        descs->addr = (i2c_hw_index(chip.i2c) << 8) | chip.i2cAddr;
        ++descs;
    }
}

// Populate vendor interface output device descriptors
void PCA9555::PopulateDescs(PinscapePico::OutputDevDesc* &descs)
{
    for (auto &chip : chips)
    {
        descs->configIndex = chip.chipNumber;
        descs->devType = PinscapePico::OutputPortDesc::DEV_PCA9555;
        descs->numPorts = 16;
        descs->numPortsPerChip = 16;
        descs->pwmRes = 2;  // digital ports -> 2 duty cycle levels (0=off, 1=100% on)
        descs->addr = (i2c_hw_index(chip.i2c) << 8) | chip.i2cAddr;
        ++descs;
    }
}

// Populate vendor interface output device port descriptors
void PCA9555::PopulateDescs(PinscapePico::OutputDevPortDesc* &descs)
{
    for (auto &chip : chips)
    {
        for (int i = 0 ; i < 16 ; ++i, ++descs)
        {
            // PCA9555 ports can be configured as inputs ('1' bit in config
            // register) or outputs ('0' bit in config register)
            descs->type = (chip.configReg.GetPortBit(i) ? descs->TYPE_UNUSED : descs->TYPE_DIGITAL);
        }
    }
}

// Populate an output level query result buffer
void PCA9555::PopulateLevels(PinscapePico::OutputDevLevel* &levels)
{
    for (auto &chip : chips)
    {
        for (int i = 0 ; i < 16 ; ++i)
        {
            levels->level = (chip.outputReg.GetPortBit(i) ? 1 : 0);
            ++levels;
        }
    }
}

// Query the states of the PCA9555 input ports, for a Vendor Interface
// button state query.  Populates the buffer with one byte per input
// port, arranged in order of the chips in the config list.  Returns the
// size in bytes of the populated buffer space, or 0xFFFFFFFF on
// failure.  Note that 0 isn't an error: it simply means that there are
// no 74HC165 ports configured.
size_t PCA9555::QueryInputStates(uint8_t *buf, size_t bufSize)
{
    // make sure we have space
    size_t resultSize = CountConfigurations() * 16;
    if (resultSize > bufSize)
        return 0xFFFFFFFF;

    // visit each chip
    for (auto &chip : chips)
    {
        // populate the buffer for each port
        uint16_t portBits = chip.portBits;
        for (int i = 0, bit = 1 ; i < 16 ; ++i, bit <<= 1)
            *buf++ = ((portBits & bit) != 0) ? 1 : 0;
    }

    // success - return the populated space
    return resultSize;
}


// Start an I2C transaction.  The I2C bus manager calls this when it's
// our turn to use the bus.
//
// We always start off our cycle with a read, if one is pending.  After
// reading (either after determining that no read is necessary, or when
// the read completes), we'll check for pending writes.
bool PCA9555::OnI2CReady(I2CX *i2cx)
{
    // Transaction list.  We might need to update the config register
    // and output register, and read the input register.  Consolidate
    // these into a transaction batch that we can send off to DMA in
    // one shot.  We might not need all of these on any given cycle,
    // but we'll make room for all of them, and then fill in the ones
    // that we actually need.
    I2C::TXRXBuilder<3, 128> b;

    // check for a change of config and/or output registers
    configReg.BuildI2C(b);
    outputReg.BuildI2C(b);

    // check if it's time to do a port read
    BuildPortRead(b);

    // If we have any work to do, fire off the transasction batch
    if (b.n != 0)
    {
        i2cx->MultiReadWrite(b);
        return true;
    }

    // no work to do
    return false;
}

// Receive I2C data.  The I2C bus manager calls this when it receives
// the response to a READ operation that we initiated.
bool PCA9555::OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2cx)
{
    // we expect two bytes, which contain the INPUT0 and INPUT1
    // register values, with the input port bits
    if (len == 2)
        portBits = data[0] | (static_cast<uint16_t>(data[1]) << 8);

    // no more transactions needed
    return false;
}

// Build a write transaction if needed for a register pair
void PCA9555::RegPair::BuildI2C(I2C::TXRXBuilderBase &b)
{
    // check for a value change since the last chip update
    if (newVal != chipVal)
    {
        // value has been updated - write the new register pair
        uint8_t buf[3] = { regNum, static_cast<uint8_t>(newVal & 0xFF), static_cast<uint8_t>(newVal >> 8) };
        b.AddWrite(buf, _countof(buf));

        // we've now sent the latched value, so it becomes the "old" value for next time
        chipVal = newVal;
    }
}

// Check if we should set up an I2C port read for our current turn at bus access
void PCA9555::BuildPortRead(I2C::TXRXBuilderBase &b)
{
    // If we have an interrupt input, the chip holds the interrupt line
    // low when it has changes to report since the last read, so we can
    // use that to determine if there's any reason to read from the
    // port.  (The interrupt signal clears automatically when the host
    // reads the port, so we don't have to do anything extra to reset
    // the signal.  The interrupt output from the PCA9555 is open-drain,
    // so the signals from multiple chips can be simply wired together
    // to form the logical OR of the signals from all connected chips.
    // The hardware reference design does that to conserve GPIOs, so we
    // can't necessarily identify which chip has pending data to read,
    // and reading from the current chip won't clear the signal if one
    // of the other chips also [or only] has changes to read.  But
    // that's okay, because the I2C manager schedules bus time
    // round-robin, so the next time we get back here, all of the other
    // chip drivers will have had a chance to read their chips.)
    //
    // If there's no interrupt input, simply poll
    bool read = gpInterrupt != -1 ?
                !gpio_get(gpInterrupt) :
                time_us_64() >= tRead;

    // if we decided to proceed with the read, queue the I2C transaction
    if (read)
    {
        // kick off a read for two bytes from INPUT0
        uint8_t buf[1] = { REG_INPUT0 };
        b.AddRead(buf, _countof(buf), 2);

        // set the next read time
        tRead = time_us_64() + 1000;
    }
}
