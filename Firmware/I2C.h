// Pinscape Pico - I2C bus manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class provides asynchronous I2C access for multiple devices
// sharing an I2C bus.  Devices are granted access to the bus on a
// round-robin basis.  Read and Write operations are performed
// asynchronously via DMA, so the main program can continue running
// while I2C transactions proceed in the background, managed by the
// hardware controllers without CPU intervention.
//
// The asynchronous design requires the devices to use a state-machine
// model.  A device can only initiate an I2C request when the bus
// manager invites it to, by invoking a callback the device provides.
// If the device has I2C work to do at that time, it can initiate a Read
// or Write operation from the callback.  The callback doesn't wait for
// the operation to complete; it must instead return after initiating
// the transaction, to allow the operation can proceed in the
// background.  This means that a Read operation won't receive its
// response inline in the same function call - the response will be
// delivered later via a separate callback, when the asynchronous
// operation completes.
//
// Note that during system startup, it might be more convenient to do
// the initial setup communications with I2C devices via the regular
// blocking API that the Pico SDK provides.  The asynchronous model is
// great for the typically simple and repetitive operations that most
// device code performs in the main loop, but it can be inconvenient
// for setup operation, which for many I2C devices involve sending a
// series of register-setting commands and reading replies.  It's fine
// to do the initial setup synchronously, since the initialization
// phase isn't as sensitive to latency as the main loop is.
//

#pragma once

// debugging
#define I2C_DEBUG
#ifdef I2C_DEBUG
#define IF_I2C_DEBUG(x) x
#else
#define IF_I2C_DEBUG(x)
#endif

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <vector>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/i2c.h>
#include <hardware/dma.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"

// forward/external declarations
class JSONParser;
class I2C;
class I2CX;
class ConsoleCommandContext;

// I2C device interface.  This is an abstract interface class that's
// meant to be implemented by a device-specific concrete class for each
// device type on the bus.
class I2CDevice
{
public:
    // Initialize with 7-bit I2C address.
    //
    // Note that the address is expressed in the 7-bit address notation
    // that the Pico SDK uses.  In 7-bit notation, the R/!W bit is NOT
    // considered part of the address, so an address with bit pattern
    // 0000001R (where R represents the R/!W bit) translates to 0x01 in
    // our notation, because we only use the first 7 bits.
    //
    // When reading the address from a data sheet, don't trust the hex
    // number, if they show one.  Always look for the bit pattern.  Some
    // data sheets use the same 7-bit convention we use, but others use
    // the 8-bit convention, where the R/!W bit is considered part of
    // the address (as the low-bit).  You can't tell which convention
    // they're using by looking at the hex number alone, so you really
    // have to find the binary representation.  Every data sheet I've
    // encountered shows the address in binary form somewhere, often in
    // the section where they present diagrams of the I2C signal timing.
    I2CDevice(uint16_t addr) : i2cAddr(addr) { }

    // The device's I2C bus address.  We use the 7-bit address notation
    // that the Pico SDK uses, where the R/!W bit is NOT considered part
    // of the address.
    uint16_t i2cAddr;

    // Device name (mostly for debugging)
    virtual const char *I2CDeviceName() const = 0;

    // Reinitialize the device.  We call this after a bus reset to send
    // any required startup commands to the device.
    virtual void I2CReinitDevice(I2C *i2c) = 0;

    // I2C bus ready.  The I2C manager calls this when the bus is idle
    // and it's the device's turn to access the bus.  If the device has
    // pending work, it can start a new Read or Write transaction here,
    // by calling the respective routine in the I2CX object.  The
    // routine returns true if it starts a new transaction.  If the
    // device doesn't have any pending transaction to perform, return
    // false.
    //
    // The routine should use the I2CX routines, NOT the blocking I2C
    // routines in the Pico SDK.  The Pico SDK routines block until the
    // bus operation completes, which stalls the maih loop for the
    // entire duration of the operation.  The async routines in I2CX
    // allow the main loop to continue running while the bus operation
    // proceeds in the background via DMA.
    virtual bool OnI2CReady(I2CX *i2c) = 0;

    // I2C completion IRQ.  The I2C manager calls this from IRQ context
    // when the DMA transfer for the current operation completes.  An
    // overrider can perform any actions on its own state that are safe
    // to perform from IRQ context.  Note that it's usually easier to
    // handle the completion notification in one of the thread-context
    // callbacks (OnI2CWriteComplete, OnI2CReceive), because of the need
    // to protect against race conditions when accessing shared
    // resources from IRQ context.  However, handling completion in IRQ
    // context is sometimes necessary for performance reasons, since it
    // allows an essentially instantaneous response to a hardware event.
    // The thread-context callbacks have slight latency relative to the
    // IRQ since they're not reached until the next main task loop after
    // the actual hardware event occurs.
    //
    // Overriders are specifically allowed to start a new I2C
    // transaction via the I2CX* object passed here.  If it does, the
    // thread-context callbacks for the operation represented by the IRQ
    // callback will NOT be invoked, since the new transaction initiated
    // here will replace the just-completed transaction as the "current"
    // operation on the bus.  If this routine doesn't start a new
    // transaction, the thread-context callbacks will be invoked as
    // usual.
    //
    // The receive buffer is always passed to the callback, but it's not
    // meaningful after a transaction that only did a 'write' operation.
    virtual void OnI2CCompletionIRQ(const uint8_t *rxBuf, size_t rxLen, I2CX *i2c) { }

    // Receive I22C data.  The I2C manager calls this when a read
    // transaction that the device initiated successfully completes, to
    // pass the received data back to the device.
    //
    // The callback can immediately start another transaction if needed,
    // in which case it should return true to indicate that it's still
    // using the bus.  Return false if not.  Some I2C transactions
    // require multiple consecutive reads and writes; for example, the
    // caller might have to read from the device to check an interrupt
    // flag, and then immediately write to the device to clear the flag.
    virtual bool OnI2CReceive(const uint8_t *data, size_t len, I2CX *i2c) = 0;

    // Write completion handler.  This is an optional handler that's
    // invoked when a transmit operation completes successfully.  This
    // can usually be left as a no-op, since most device code is written
    // with the simplifying assumption that all sends are successful.
    //
    // If desired, the callback can immediately start a new transaction
    // via the I2CX object.  Return true if another transaction was
    // started, false if not.
    virtual bool OnI2CWriteComplete(I2CX *i2c) { return false; }

    // I2C timeout event.  This is an optional handler that's called
    // if an I2C operation times out without completing.  The device
    // class can use this for things like error logging and automatic
    // recovery (e.g., sending a hardware reset signal to the device).
    virtual void OnI2CTimeout() { }

    // I2C abort event.  This is an optional handler that's called
    // if an I2C operation fails with an 'abort' condition on the bus.
    virtual void OnI2CAbort() { }

    // Statistics - automatically updated by the I2C manager
    struct I2CStats
    {
        uint64_t txStarted = 0;     // number of transmissions initiated
        uint64_t txCompleted = 0;   // transmissions completed successfully
        uint64_t txTimeout = 0;     // transmissions failed with timeout
        uint64_t txAborted = 0;     // transmissions aborted
        uint64_t rxCompleted = 0;   // number of receives completed successfully

        // display the statistics to a command console
        void Print(const ConsoleCommandContext *ctx);
    };
    I2CStats i2cStats;
};

// I2C manager.  Each instance manages devices sharing one bus.
class I2C
{
    // provide private access to the token class
    friend class I2CX;
    
public:
    // Configure the I2C buses based on the JSON data
    static void Configure(JSONParser &json);

    // Is the bus number valid?  All current Pico CPUs have two I2C units,
    // i2c0 and i2c1, so this could be hardcoded everywhere it's needed,
    // but we're abstracting it to make it easier to extend in the future.
    // We would need to extend it if future Pico models have more native
    // I2C units on board, or if we were to add an option to implement
    // additional I2C units via PIOs.
    static bool IsValidBus(int bus) { return bus == 0 || bus == 1; }

    // Validate an I2C bus from a configuration option.  If valid,
    // returns true; if invalid, logs an under the given feature name
    // and returns false.
    static bool ValidateBusConfig(const char *feature, int bus);

    // Get the instance for I2C<bus>, optionally initializing it if it's
    // set to on-demand initialization.
    static I2C *GetInstance(int bus, bool init);

    // destruction
    ~I2C();

    // Initialize.  Sets up the Pico hardware I2C unit.
    void Init();

    // Enable the interrupt handler.  Interrupts are disabled initially
    // (even after calling Init(), to allow device classes to use the
    // blocking I2C calls in the Pico SDK during initialization.
    // Interrupts should be enabled after initialization is completed
    // and before entering the main loop.
    void EnableIRQ(bool enable);

    // enable IRQs on all I2C bus manager instances
    static void EnableIRQs(bool enable);

    // Add a new device.  The device must have session lifetime.
    void Add(I2CDevice *device);

    // perform periodic I2C tasks
    static void Task();

    // Is DMA working?  This checks that the bus controller successfully
    // claimed its DMA channels.
    bool IsDMAOk() { return dmaChannelTx >= 0 && dmaChannelRx >= 0; }

    // Get the Pico SDK hardware instance struct for this unit
    i2c_inst_t *GetHwInst() const { return i2c; }

    // Transaction descriptor for a multi read/write operation.  This
    // defines one read or write operation in a string of operations
    // to queue as a group.  If rxLen is zero, it's a write operation,
    // otherwise it's a read operation.
    struct TXRX
    {
        const uint8_t *txData;
        size_t txLen;
        size_t rxLen;
    };

    // Lightweight TXRX builder.  This helps a caller set up a TXRX list
    // incrementally.  It uses buffers provided by the caller, usually
    // allocated on the stack, to make it extremely lightweight on
    // memory usage and fast to run.  This is designed for use cases
    // where the caller knows in advance the maximum number of
    // transactions that it will need to queue and the maximum buffer
    // size.  This is almost always true for our use cases, because the
    // transactions will almost always be the same for a given chip; the
    // only variable is usually whether a particular register read/write
    // is needed on a particular invocation, which makes it easy to
    // determine the maximum sizes needed.
    struct TXRXBuilderBase
    {
        TXRXBuilderBase(TXRX *txrx, uint8_t *txBuf) : txrx(txrx), txBuf(txBuf), txBufWritePtr(txBuf) { }
        
        // Transaction list, provided by the caller.  This must be
        // pre-allocated with at least as many array slots as the caller
        // will consume.
        TXRX *txrx;

        // Overall TX buffer.  As transactions are added, the TX portion
        // is appended to this buffer.  The caller must allocate this to
        // be large enough for all of the data it will add.
        uint8_t *txBuf;

        // current txBuf write pointer
        uint8_t *txBufWritePtr;

        // number of transactions queued
        int n = 0;

        // Add a write transaction
        void AddWrite(const uint8_t *data, size_t len)
        {
            txrx[n++] = { txBufWritePtr, len, 0 };
            memcpy(txBufWritePtr, data, len);
            txBufWritePtr += len;
        }

        // Add a read transaction
        void AddRead(const uint8_t *data, size_t txLen, size_t rxLen)
        {
            txrx[n++] = { txBufWritePtr, txLen, rxLen };
            memcpy(txBufWritePtr, data, txLen);
            txBufWritePtr += txLen;
        }
    };
 
    // TXRX builder with internal buffer
    template<int nTransactions, int txBytes> struct TXRXBuilder : TXRXBuilderBase
    {
        TXRXBuilder() : TXRXBuilderBase(txrxMem, txBufMem) { }
        
        // transaction list
        TXRX txrxMem[nTransactions];

        // transmission buffer
        uint8_t txBufMem[txBytes];
    };

protected:
    // construction
    I2C(int busNum, int gpSDA, int gpSCL, bool enablePulls, int baudRate);

    // Singleton instances, one per Pico I2C controller unit.
    // inst[0] = controller for physical I2C0
    // inst[1] = controller for physical I2C1
    static I2C *inst[2];

    // Initialize DMA
    bool InitDMA();

    // periodic task handler for this unit
    void UnitTask();

    // Initiate a WRITE transaction on the current device in the
    // round-robin scheduling.
    void Write(const uint8_t *data, size_t len);

    // Initiate a READ transaction on the current device in the
    // round-robin scheduling.  The bytes received in the read
    // transaction will be provided to the device via the receive
    // callback in the device's I2CDevice interface.
    void Read(const uint8_t *txData, size_t txLen, size_t rxLen);

    // Internal common handler for read and write operations.  An
    // I2C READ is actually a WRITE + READ, since we have to send
    // the request to solicit input before reading the response.
    // So a WRITE is the same as a READ with no response expected.
    void ReadWrite(const uint8_t *txData, size_t txLen, size_t rxLen);

    // Multi read write.  This queues up one or more read/write
    // transactions.
    void MultiReadWrite(const TXRX *txrx, int n);

    // Execute the read/write queued in txBuf/rxBuf
    void ExecReadWrite();

    // Perform a bus-clear operation, to try to clear a stuck SDA line
    // being held low by a peripheral.  This bit-bangs a sequence of SCL
    // pulses to try to force the stuck peripheral's internal state
    // machine out of its wedged state.  A stuck-low SDA line is usually
    // caused by a peripheral that missed a clock signal in the middle
    // of an RX/TX operation and thus can't get its state machine to
    // move past that operation.  When this happens, it can often be
    // cleared simply by sending the peripheral enough clock pulses to
    // advance it back to the starting condition in its state machine.
    void BusClear(bool isStartup);

    // I2C bus number - this is the unit number of the Pico I2C
    // controller, 0 for I2C0 or 1 for I2C1
    int busNum;

    // GPIO configuration
    int sda = -1;
    int scl = -1;
    bool enablePulls = true;

    // I2C bit rate setting
    int baudRate = 0;

    // I2C IRQ number
    int i2cIrqNum;

    // IRQ thunk - this is a dynamically generated 'static void()'
    // function that bridges the hardware I2C IRQ vector to our
    // IRQ() member function.  We need a thunk because there are
    // two I2C hardware vectors, and we need to route each one to
    // its corresponding object.  (The alternative would be to use
    // separate hard-coded static handlers for each vector, say
    // 'static void I2C1_IRQ()' and 'IC20_IRQ()', but this way is
    // cleaner, and it adds negligible overhead.)
    void (*i2cIrqThunk)();

    // Member function I2C handler.  This are invoked on interrupt by
    // way of the IRQ thunk.
    void I2CIRQ();

    // flag: we're in the IRQ handler
    bool inIrq = false;

    // Device list.  Each physical device on the bus is represented by an
    // instance in this list.
    std::vector<I2CDevice*> devices;

    // Current device list scanning position
    int curDevice = 0;

    // Current transaction state
    enum class State
    {
        Ready,      // no work pending, ready to start a new transaction
        Writing,    // write in progress
        Reading,    // read in progress
    };
    State state = State::Ready;

    // Bus scan address.  -1 means that no scan is in progress; other
    // values mean that we're scanning the specified address.
    int busScanAddr = -1;

    // bus scan time - we wait a little bit between tests to allow time
    // for log buffers to clear
    uint64_t busScanTime = 0;

    // Ad hoc I2C TX/RX requests.  If the address is non-zero, we'll
    // attempt an inline ad hoc send/receive of the specified lengths.
    uint8_t adHocRequestAddr = 0;
    uint8_t adHocBuf[128];
    int adHocTxLen = 0;
    int adHocRxLen = 0;

    // Bus clear requested
    bool busClearRequested = false;

    // Automatic bus clear monitoring.  If we encounter too many
    // conseuctive timeouts, we'll try initiating a bus reset operation,
    // in case a peripheral's I2C state machine is stuck.  This can
    // happen due to missed clock signals on the peripheral side; the
    // other device's state machine can just sit there holding SDA low,
    // waiting for a new clock that never comes because the bus master
    // (the RP2040) is waiting for the peripheral to release SDA.  Some
    // devices have their own built-in watchdog timers that clear these
    // conditions automatically, but many don't, so it's up to the host
    // to resolve.  Note that there's a known error in the RP2040 data
    // sheet that claims that the RP2040 I2C controller monitors for and
    // clears these conditions automatically; the RP2040 I2c unit has no
    // such capability, so we have to implement it in software.
    int consecutiveTimeouts = 0;

    // Minimum time of next automatic bus reset attempt.  If the bus
    // gets wedged in such a way that clearing it doesn't fix it, this
    // limits the rate of retries.
    uint64_t tNextAutoReset = 0;

    // For a READ transaction, the number of bytes requested.  We buffer
    // this many bytes from the read before invoking the device's
    // receive callback.  This lets the device handle the receive as a
    // single block operation without having to implement its own
    // internal buffering.
    size_t readRequestLength = 0;

    // Current transaction timeout.  This is the system clock (time_us_64())
    // time when the current transaction in progress times out.
    uint64_t tTimeout = 0;

    // Transmit/Receive buffers.  The Pico I2C controllers have their
    // own hardware FIFOs, but they're small, so we provide some extra
    // buffering.  This lets the device-specific classes treat reads and
    // writes as simple block operations without having to do their own
    // buffering.  Each buffer is a simple linear buffer (no wrapping).
    // The caller gets and puts bytes all at once, while the I2C device
    // accesses the buffers a byte at a time.
    //
    // The Transmit buffer uses 16-bit entries so that we can write the
    // data byte AND the control byte on each transfer.  The buffer also
    // needs space for the transmission PLUS the received data, since
    // we have to enlist a command word in the transmit buffer for each
    // byte we want to receive - the controller has to be told explicitly
    // via a port command to go into RX mode for each byte.
    //
    // The Receive buffer only needs space for the received bytes, and
    // only needs to store the 8-bit data bytes.  There's no "control"
    // portion for the received data.
    static const unsigned int BUF_SIZE = 128;
    struct
    {
        uint16_t data[BUF_SIZE*2];
        unsigned int len = 0;
    } txBuf;
    struct
    {
        uint8_t data[BUF_SIZE];
        unsigned int len = 0;
    } rxBuf;

    // Bus status.  These are written from the IRQ handler.
    volatile bool busStop = false;
    volatile bool busAbort = false;

    // Pico SDK I2C instance struct
    i2c_inst_t *i2c;

    // Has initialization been completed?  We normally initialize during
    // the JSON configuration step, but we defer this until the first
    // device is configured on the bus if the enable mode is set to
    // "on-demand" in the JSON.
    bool initialized = false;

    // DMA channels for TX and RX operations.  Many of the I2C devices
    // in a typical Pinscape setup run essentially continuously, so
    // we reserve and hold a pair of DMA channels permanently.
    int dmaChannelTx = -1;
    int dmaChannelRx = -1;

    // DMA channel configurations
    dma_channel_config configTx;
    dma_channel_config configRx;

    // console commands
    void Command_main(const ConsoleCommandContext *ctx);

#ifdef I2C_DEBUG
    // Debugging - capture buffer for recent transactions.  We keep
    // a rolling window of send and receive data for inspection in the
    // command console, to help debug device interactions.
    static const size_t DEBUG_CAPTURE_BUFLEN = 32;
    struct DebugCapture
    {
        uint64_t t;                             // timestamp
        const char *status = "Unknown";         // status
        uint8_t addr = 0;                       // device address
        uint8_t txLen = 0;                      // transmit data length
        uint8_t tx[DEBUG_CAPTURE_BUFLEN];       // transmit buffer
        uint8_t rxExpected = 0;                 // receive data length expected
        uint8_t rxLen = 0;                      // receive data length
        uint8_t rx[DEBUG_CAPTURE_BUFLEN];       // receive buffer
    };
    static const int DEBUG_CAPTURE_COUNT = 128;
    DebugCapture debugCapture[DEBUG_CAPTURE_COUNT];

    // Write pointer in the ring buffer.  This points to the next
    // spot to be written.
    int debugCaptureWrite = 0;

    // is a debug capture open?
    bool debugCaptureOpen = false;

    // Debug capture filter, by address.  Empty means no filtering
    // is in effect, so transactions to all addresses are captured.
    // Filter size -1 disables capture entirely.
    static const int DEBUG_CAPTURE_ADDR_FILTER_MAX = 8;
    uint8_t debugCaptureAddrFilter[DEBUG_CAPTURE_ADDR_FILTER_MAX];
    int debugCaptureAddrFilterCnt = 0;

    // if the address in the filter?
    bool DebugCaptureMatchAddrFilter(uint8_t addr) const;

    // capture the current send/receive buffer
    void DebugCaptureStart(uint8_t addr, const uint8_t *txData, size_t txLen, size_t rxLen);
    void DebugCaptureEnd(uint8_t addr, const uint8_t *rxData, size_t rxLen, const char *status);

#endif // I2C_DEBUG
};

// I2C bus access object.  This class acts as a token in the device
// callback to provide the callback with access to the protected Read()
// and Write() routines in the I2C object.  Devices are only allowed to
// call those routines from the callback, since the device can only
// initiate an operation when it's device's "turn" in the round-robin
// scheduling order.  We try to emphasize this restriction by limiting
// access to the Read/Write routines to the token object, which can
// only be created by the main I2C class.
class I2CX
{
    // allow I2C to access private elements, since our whole job is to
    // coordinate with I2C to grant conditional access
    friend class I2C;
    
public:
    void Write(const uint8_t *data, size_t len) { i2c->Write(data, len); }
    void Read(const uint8_t *txData, size_t txLen, size_t rxLen) { i2c->Read(txData, txLen, rxLen); }

    // Queue multiple read/write operations.  This allows several
    // transactions to be performed consecutively without CPU
    // intervention.
    void MultiReadWrite(const I2C::TXRX *txrx, int n) { i2c->MultiReadWrite(txrx, n); }
    void MultiReadWrite(const I2C::TXRXBuilderBase &txrx) { i2c->MultiReadWrite(txrx.txrx, txrx.n); }

private:
    // private constructor - only I2C can construct these
    I2CX(I2C *i2c) : i2c(i2c) { };

    // our I2C object
    I2C *i2c;
};
