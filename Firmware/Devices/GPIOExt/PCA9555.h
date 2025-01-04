// Pinscape Pico - PCA9555 port extender chip device interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// The PCA9555 is a 16-port "GPIO extender" chip with an I2C interface.
// It's designed to be used in microcontroller systems (such as the
// Pico) to add supplemental I/O ports that can act as digital IN or
// OUT ports.
//
// At power-on, all ports are initially set to INPUT mode.  All ports
// are connected internally to VCC through a 100K resistor, in both
// INPUT and OUTPUT modes.  These features have important implications
// for hardware designs using the chip:
//
// 1. When using a port in INPUT mode to read a switch, the switch
//    (when closed) should be connected directly to GND.  No external
//    pull-up resistor is needed, since the port has the internal
//    100K pull-up to VCC.  Ports should be set to Active Low for
//    reading grounded switch inputs.
//
// 2. When using a port in OUTPUT mode, the port will read at VCC
//    (through the internal 100K resistor) at power-on, and remain
//    at VCC until the software configures the port as an output.
//    If glitch-free startup is desired, the port should be used
//    as an Active Low switch, since that makes its initial state
//    at power-on (pulling to VCC) equivalent to OFF, ensuring
//    that the connected device isn't triggered until the software
//    actually commands the port to read Low.
//
// The Pinscape Pico expansion boards use two of these chips to
// implement 32 button input ports.  The more conventional approach to
// adding button inputs to a microcontroller is to use parallel-to-
// serial shift register chips, but the PCA9555 chips have a couple
// of advantages.  One is that they provide internal pull-up resistors
// on all inputs ports, which reduces the part count.  With a shift
// register, an external resistor would be needed per port.  A second
// is that the PCA9555 provides an I2C interface, which means that the
// PCA9555 requires zero additional GPIOs on the Pico, since it shares
// the I2C bus that we need anyway for several other I2C chips.  In
// contrast, an input shift register typically requires at least three
// dedicated GPIOs; this is significant for this application because we
// don't have enough GPIOs to spare after covering all of the other
// functions that require them.  Note that the PCA9555 provides an
// interrupt signal, which does require one dedicated GPIO if used,
// but this software makes it optional, as we can use polling instead.
//
// Our public interface numbers the chip's output ports consecutively
// from 0 to 15.  the data sheet labels the ports in two banks, IO0
// and IO1, each with 8 ports numbered IOx_0 to IOx_7.  We map the
// first set (IO0_x) to our numbered ports 0-7, and the second set
// (IO1_x) to our ports 8-15.  Here's the full port list:
//
//   Our Port Num    Data Sheet Label
//      0            IO0_0
//      1            IO0_1
//      2            IO0_2
//      3            IO0_3
//      4            IO0_4
//      5            IO0_5
//      6            IO0_6
//      7            IO0_7
//      8            IO1_0
//      9            IO1_1
//     10            IO1_2
//     11            IO1_3
//     12            IO1_4
//     13            IO1_5
//     14            IO1_6
//     15            IO1_7


#pragma once
#include <stdint.h>
#include <list>
#include "I2C.h"

// external classes
class JSONParser;
namespace PinscapePico {
    struct ButtonDevice;
    struct OutputDevDesc;
    struct OutputDevPortDesc;
    struct OutputDevLevel;
}

class PCA9555 : public I2CDevice
{
public:
    // global list of active chips
    static std::list<PCA9555> chips;

    // Configure PCA9555 chips from the configuration data
    static void Configure(JSONParser &json);

    // Initialize the hardware
    void Init();

    // Find a PCA9555 instance by chip number.  The chip number is
    // an arbitrary ID assigned by the configuration, for referencing
    // the chip when defining input sources for buttons and any other
    // usages.  The chip number isn't related to the address; it's
    // just an opaque ID used within the config data.
    static PCA9555 *Get(int chipNumber);

    // get my configuration index
    int GetConfigIndex() const { return chipNumber; }

    // get the number of configured chips
    static int CountConfigurations() { return static_cast<int>(chips.size()); }

    // count ports across all configured chips
    static int CountPorts() { return static_cast<int>(chips.size() * 16); }

    // Enable/disable output ports.  Disabling outputs sets all ports to
    // INPUT mode, which is the power-on condition.
    static void EnableOutputs(bool enable);

    // construction
    PCA9555(int chipNumber, uint8_t i2cBus, uint8_t i2cAddr, int gpInterrupt);

    // validate a port number
    static bool IsValidPort(int port) { return port >= 0 && port <= 15; }

    // Read an individual port.  This returns the port's input
    // value as of the last time we polled the chip.  (This doesn't
    // initiate new polling; it merely reports the last value read.)
    bool Read(uint8_t port);

    // Write a port.  This sets the output port level to LOW (0) or HIGH
    // (1, or any non-zero value).  This has no effect unless the port
    // has been configured as an output.  This routine only queues the
    // update; the actual hardware update happens on our next I2C access
    // cycle.
    void Write(uint8_t port, uint8_t level);

    // I2C callbacks
    virtual const char *I2CDeviceName() const override { return "PCA9555"; }
    virtual void I2CReinitDevice(I2C *i2c) override;
    virtual bool OnI2CReady(I2CX *i2c) override;
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) override;
    virtual bool OnI2CWriteComplete(I2CX *i2c) override { return false; }

    // Populate a Vendor Interface button query result buffer with
    // ButtonDevice structs representing the configured PCA9555 chips.
    // The caller is responsible for providing enough buffer space; we
    // require one PinscapePico::ButtonDevice per chip.  On return, the
    // buffer pointer is automatically incremented past the space
    // consumed.
    static void PopulateDescs(PinscapePico::ButtonDevice* &descs);

    // Populate a vendor interface output port query result buffer
    static void PopulateDescs(PinscapePico::OutputDevDesc* &descs);

    // Populate physical output port descriptions
    static void PopulateDescs(PinscapePico::OutputDevPortDesc* &descs);

    // Populate an output level query result buffer
    static void PopulateLevels(PinscapePico::OutputDevLevel* &levels);

    // Query the states of the PCA9555 input ports, for a Vendor
    // Interface button state query.  Populates the buffer with one byte
    // per input port, arranged in order of the chips in the config
    // list.  Returns the size in bytes of the populated buffer space,
    // or 0xFFFFFFFF on failure.  Note that 0 isn't an error: it simply
    // means that there are no 74HC165 ports configured.
    static size_t QueryInputStates(uint8_t *buf, size_t bufSize);

    // Claim a port as an input or output.  The owner name is a string
    // with static storage duration, which we record for use in error
    // log messages if (a) the port has already been claimed by another
    // subsystem, or (b) if this call succeeds, and a later call from
    // another subsystem tries to claim the port.
    bool ClaimPort(int portNum, const char *ownerName, bool asOutput);

protected:
    // send initialization commands
    void SendInitCommands();
    
    // Register addresses
    static const uint8_t REG_INPUT0 = 0;    // input port 0
    static const uint8_t REG_INPUT1 = 1;    // input port 1
    static const uint8_t REG_OUTPUT0 = 2;   // output port 0
    static const uint8_t REG_OUTPUT1 = 3;   // output port 1
    static const uint8_t REG_INV0 = 4;      // polarity inversion port 0
    static const uint8_t REG_INV1 = 5;      // polarity inversion port 1
    static const uint8_t REG_CONFIG0 = 6;   // configuration port 0
    static const uint8_t REG_CONFIG1 = 7;   // configuration port 1
    
    // Pico SDK I2C instance
    i2c_inst_t *i2c;

    // config index
    int chipNumber;

    // Interrupt GPIO pin number, or -1 if this isn't connected (in
    // which case we'll use polling instead).
    int gpInterrupt = -1;

    // Next read time on the system closk.  We use this to determine
    // when we're ready to poll the device again.
    uint64_t tRead = 0;

    // Last input port values, one bit per port; bit 0x0001 is PORT0 #0,
    // 0x0002 is PORT0 #1, etc.  The high byte contains the PORT1 bits.
    uint16_t portBits = 0;

    // Outputs Enabled flag.  When outputs are disabled, we set all ports
    // to INPUT mode in the Configuration Register, regardless, of their
    // settings in portModes.
    bool outputsEnabled = false;

    // Port Modes, one bit per port.  A '1' bit is INPUT mode, which is
    // the power-on mode for all ports.  A '0' bit is OUTPUT mode.
    //
    // This is our internal "logical" mode setting per port.  When
    // outputs are disabled, the physical Configuration Register is set
    // to all '1' bits, to put all ports in input mode.  When outputs
    // are enabled, the Configuration Register is set to match this
    // logical state.
    //
    // The chip powers up in INPUT mode ('1' bit) for all ports.
    uint16_t portModes = 0xFFFF;

    // Build a port read I2C transaction if needed.  We call this during
    // each I2C access cycle to set up a read if needed.  If the chip
    // has signaled an input-change interrupt, or the polling interval
    // has rolled around, we'll add the I2C read to the transaction
    // builder.
    void BuildPortRead(I2C::TXRXBuilderBase &b);

    // New/old register pair.  On each I2C access cycle, we'll send the
    // new configuration if it differs from the old one.  The physical
    // registers on the PCA9555 are 8 bits each, but they're always
    // arranged in pairs, for the two banks of port pins, IO0 and I We
    // combine the two 8-bit registers into one 16-bit word, with the
    // low byte used for IO0 (our port numbers 0-7) and the high byte
    // for IO1 (our ports 8-15).
    struct RegPair
    {
        RegPair(uint8_t regNum, uint16_t initVal) :
            regNum(regNum), newVal(initVal), chipVal(initVal) { }

        // Register number - this is the PCA9555 I2C register number for
        // the first register in our IO0/IO1 pair.  The second register
        // in the pair is always the next register number sequentially.
        uint8_t regNum;

        // new register value - this is the new value that we'll send
        // to the chip on the next I2C access cycle
        uint16_t newVal;

        // chip register value - this is the last value we sent to the
        // physical chip via I2C
        uint16_t chipVal;

        // Set/clear the bit corresponding to the given port number.
        // This updates the 'new' entry, so that we'll send an update to
        // the chip on our next I2C access cycle.  'bit' is 0 or 1 (so
        // it's really a bool, but it's aesthetically nicer to write it
        // as an integer in calls to this routine, because the chip's
        // data sheet speaks in terms of '0' and '1' bits in the port
        // registers).
        void SetPortBit(int portNum, uint8_t bit)
        {
            uint16_t portBit = (1U << portNum);
            if (bit != 0)
                newVal |= portBit;
            else
                newVal &= ~portBit;
        }

        // get a port bit
        bool GetPortBit(int portNum)
        {
            uint16_t portBit = (1U << portNum);
            return (newVal & portBit) != 0;
        }

        // Build an I2C TX transaction for this register update, if needed
        void BuildI2C(I2C::TXRXBuilderBase &txrx);
    };

    // New/old configuration registers.  A '1' bit sets the port to input
    // mode; a '0' bit sets the port to output mode.  The power-on default
    // is '1' (input mode) for all ports.
    RegPair configReg{ REG_CONFIG0, 0xFFFF };

    // New/old output port registers.  These control the drive status of
    // ports configured as outputs.  On each I2C access cycle, we'll
    // send the new registers if they differ from the old ones.
    RegPair outputReg{ REG_OUTPUT0, 0x0000 };

    // Map of port claims
    struct PortClaim
    {
        // Name of the owner subsystem - a string with static storage
        // duration provided by the caller when staking the claim.
        // If null, the port is unclaimed.
        const char *owner = nullptr;

        // direction - true=output, false=input
        bool asOutput = false;
    };
    PortClaim portClaims[16];
};
