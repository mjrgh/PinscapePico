// Pinscape Pico - I2C bus manager
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class provides asynchronous I2C access for multiple devices
// sharing an I2C bus, using round-robin scheduling to apportion bus
// time.
//
// The DMA implementation is based on code from pico-i2c-dma by Brian
// Cooke (https://github.com/fivdi/pico-i2c-dma, MIT license).
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <hardware/i2c.h>
#include <hardware/watchdog.h>

// local project headers
#include "Utils.h"
#include "Pinscape.h"
#include "Logger.h"
#include "Config.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "CommandConsole.h"
#include "ThunkManager.h"
#include "Watchdog.h"
#include "I2C.h"


// Controller instances
I2C *I2C::inst[2];

// get an instance, optionally initializing it if it's set to on-demand initialization
I2C *I2C::GetInstance(int bus, bool init)
{
    // validate the bus number
    if (bus < 0 || bus > _countof(inst))
        return nullptr;

    // get the instance
    I2C *i2c = inst[bus];

    // if desired, complete on-demand initialization if necessary
    if (init && i2c != nullptr && !i2c->initialized)
        i2c->Init();

    // return the instance object
    return i2c;
}

// Configure from JSON data
//
// i2c0: { sda: <gp number>, scl: <gp number>, speed: <bit rate in Hz> }
// i2c1: { sda: <gp number>, scl: <gp number>, speed: <bit rate in Hz> }
//
void I2C::Configure(JSONParser &json)
{
    // configure one bus
    auto ConfigBus = [&json](int n, const char *key)
    {
        // look up the config object for this bus
        if (auto *o = json.Get(key); o->IsObject())
        {
            uint8_t sda = o->Get("sda")->UInt8(255);
            uint8_t scl = o->Get("scl")->UInt8(255);
            int baud = o->Get("speed")->UInt32(400000);
            bool enablePulls = o->Get("pullup")->Bool(true);
            if (!IsValidGP(sda) || !IsValidGP(scl))
            {
                Log(LOG_ERROR, "I2C%d: SDA/SCL pins invalid or undefined\n", n);
                return;
            }
            else if (!gpioManager.Claim(Format("I2C%d SDA", n), sda)
                     || !gpioManager.Claim(Format("I2C%d SCL", n), scl))
            {
                // error already logged
                return;
            }

            // validate that SDA and SCL can actually be mapped to this I2C unit
            //                              0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20  21  22  23  24  25  26  27  28
            static const int sdaUnit[] = {  0, -1,  1, -1,  0, -1,  1, -1,  0, -1,  1, -1,  0, -1,  1, -1,  0, -1,  1, -1,  0, -1, -1, -1, -1, -1,  1, -1, -1 };
            static const int sclUnit[] = {  -1, 0, -1,  1, -1,  0, -1,  1, -1,  0, -1,  1, -1,  0, -1,  1, -1,  0, -1,  1, -1,  0, -1, -1, -1, -1, -1,  1, -1 };
            if (sda >= _countof(sdaUnit) || sdaUnit[sda] != n
                || scl >= _countof(sclUnit) || sclUnit[scl] != n)
            {
                Log(LOG_ERROR, "I2C%d: SDA and/or SCL pin cannot be mapped to this I2C unit; see Pico pinout chart for usable I2C%d pins\n", n, n);
                return;
            }

            // check the enable mode
            if (auto *ena = o->Get("enable"); ena->IsBool() && !ena->Bool())
            {
                // disabled - don't create an instance
                Log(LOG_CONFIG, "I2C%d is disabled\n");
            }
            else
            {
                // create the new object
                inst[n] = new I2C(n, sda, scl, enablePulls, baud);

                // check for enable-on-demand
                if (*ena == "on-demand")
                {
                    // enable on demand: don't initialize until we find a device configured on the bus
                    Log(LOG_CONFIG, "I2C%d set for on-demand initialization\n", n);
                }
                else
                {
                    // enabled unconditionally: initialize immediately
                    inst[n]->Init();
                    
                    // warn about invalid formats
                    if (!ena->IsBool() && !ena->IsUndefined())
                        Log(LOG_WARNING, "i2c%d.enable is invalid; must be true, false, or \"on-demand\", enabling by default\n", n);
                }
            }

            // add a console command
            CommandConsole::AddCommand(
                n == 0 ? "i2c0" : "i2c1", "I2C bus interface test functions",
                "i2cX [options]\n"
                "options:\n"
                "  --bus-clear               initiate a bus-clear operation (to clear a stuck-SDA condition)\n"
                "  --bus-scan                scan the bus for devices\n"
                "  -s, --stats               show statistics\n"
                "  --tx <addr> <bytes>       ad hoc send to <addr>, comma-separated byte list\n"
                "  --rx <addr> <bytes> <len> ad hoc read from <addr>, comma-separated bytes to send\n"
                IF_I2C_DEBUG("  --dump <n>                display recent captured transaction data (newest to oldest)\n")
                IF_I2C_DEBUG("  --capture-filter <addrs>  limit capture (for --dump) to comma-separated address list, '*' for all\n"),
                [](const ConsoleCommandContext *ctx){ reinterpret_cast<I2C*>(ctx->ownerContext)->Command_main(ctx); }, inst[n]);
        }
    };
    ConfigBus(0, "i2c0");
    ConfigBus(1, "i2c1");
}

I2C::I2C(int busNum, int gpSDA, int gpSCL, bool enablePulls, int baudRate) :
    busNum(busNum), sda(gpSDA), scl(gpSCL), enablePulls(enablePulls), baudRate(baudRate)
{
    // get the SDK struct for the selected bus
    i2c = i2c_get_instance(busNum);

    // set our IRQ number
    i2cIrqNum = (busNum == 0 ? I2C0_IRQ : I2C1_IRQ);

    // create our IRQ thunk (we need a thunk because we use a member
    // function for the handler; a hardware vector on its own can't
    // call a member function because it has no 'this' context, but a
    // thunk does)
    i2cIrqThunk = thunkManager.Create(&I2C::I2CIRQ, this);
    if (i2cIrqThunk == nullptr)
        Log(LOG_ERROR, "I2C%d: out of memory creating thunk IRQ\n", busNum);
}

I2C::~I2C()
{
    // disable our IRQs
    irq_set_enabled(i2cIrqNum, false);
}

void I2C::Init()
{
    // initialize the SDK hardware struct
    i2c_init(i2c, baudRate);

    // Do an explicit bus reset, in case any peripherals on ths bus are
    // currently in a wedged state.  The Pico can be reset in software,
    // so the rest of the system might not have gone through a hard
    // reset.  If something on the bus was in a wedged state before the
    // reset, it could still be in the same state now.
    BusClear(true);

    // Set the interrupt handler if we have an IRQ thunk, and there
    // isn't already a handler installed for the IRQ.  (The only way
    // that a previous handler should be installed is if the Pico SDK
    // were to install one itself, which it shouldn't do.  But check
    // anyway, as the SDK function will assert and halt the machine if
    // it's set.  Better to discover it via a log message than a
    // mysterious freeze from an assert crash.)
    if (i2cIrqThunk != nullptr && irq_get_exclusive_handler(i2cIrqNum) == nullptr)
    {
        // set the interrupt mask for STOP and ABORT detected
        i2c_get_hw(i2c)->intr_mask = I2C_IC_INTR_MASK_M_STOP_DET_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;
        
        // install the IRQ router
        irq_set_exclusive_handler(i2cIrqNum, i2cIrqThunk);

        // Note that we intentionally leave the IRQ handler disabled
        // after initialization, so that device classes can send
        // initialization commands to their chips using the blocking
        // I2C functions in the Pico SDK.  We don't need the higher
        // concurrency of async mode during initialization, so we can
        // use the more convenient blocking functions instead.  Those
        // functions read the I2C status registers directly, so we
        // don't want our IRQ handler to get in the way by reading
        // and clearing status bits from the I2C controller before
        // the blocking routines have a chance to read the bits.
    }
    else
    {
        // can't install the IRQ router
        Log(LOG_ERROR, "I2C%d: No IRQ slot available or handler already installed\n", busNum);
    }

    // initialize DMA
    InitDMA();

    // success
    initialized = true;
    Log(LOG_CONFIG, "I2C%d configured on SDA=GP%d, SCL=GP%d; speed: %d; DMA channels: TX %d, RX %d\n",
        i2c_hw_index(i2c), sda, scl, baudRate, dmaChannelTx, dmaChannelRx);
}

void I2C::Add(I2CDevice *device)
{
    // if the bus is set to enable-on-demand, initialize it
    if (!initialized)
        Init();

    // add the device to our list
    devices.emplace_back(device);
}

void I2C::BusClear(bool isStartup)
{
    // allow for a delay here
    WatchdogTemporaryExtender wte(1000);

    // disable I2C interrupts while working
    if (!isStartup)
        EnableIRQ(false);
        
    // at startup, configure the GPIO pads for pull-up (optional), fast slew, Shmitt trigger enabled
    if (isStartup)
    {
        gpio_set_dir(sda, true);
        gpio_set_dir(scl, true);
        gpio_set_slew_rate(sda, GPIO_SLEW_RATE_FAST);
        gpio_set_slew_rate(scl, GPIO_SLEW_RATE_FAST);
        gpio_set_input_hysteresis_enabled(sda, true);
        gpio_set_input_hysteresis_enabled(scl, true);

        gpio_set_pulls(sda, enablePulls, false);
        gpio_set_pulls(scl, enablePulls, false);
    }

    // reconfigure the GPIOs as plain GPIOs
    gpio_put(sda, true);
    gpio_put(scl, true);
    gpio_set_function(sda, GPIO_FUNC_SIO);
    gpio_set_function(scl, GPIO_FUNC_SIO);

    // Bit-bang the clock until SDA goes high.  If this is going to work
    // at all, it will always work within 9 clocks, because the type of
    // problem this approach cures is a chip whose state machine is
    // stuck in the middle of an RX or TX operation.  It can never take
    // more than 9 clocks to reach the end of the RX/TX state in the
    // stuck peripheral, so if this is going to work at all, it'll work
    // within 9 clocks.  If it doesn't work within 9 clocks, there must
    // be some other problem that we can't fix with bus signals; a hard
    // reset of the stuck peripheral is probably required.
    int nClocks = 0;
    for (int i = 0 ; i < 10 ; ++i, ++nClocks)
    {
        // stop if SDA is high
        if (gpio_get(sda))
            break;
        
        // allow for clock stretching - wait for SCL to go high
        for (int j = 0 ; j < 10 && !gpio_get(scl) ; ++j) ;

        // make sure SCL is high; if not, one of the peripherals on the
        // bus is holding SCL low, which is impossible to clear via bus
        // signals (it requires a hard reset of the other chip)
        if (!gpio_get(scl))
            break;

        // clock the bus
        gpio_put(scl, false);
        sleep_us(5);
        gpio_put(scl, true);
        sleep_us(5);
    }

    // send a START / STOP sequence, to reset devcie state machines
    gpio_put(sda, false);
    sleep_us(5);
    gpio_put(scl, false);
    sleep_us(5);
    gpio_put(sda, true);
    sleep_us(5);
    gpio_put(scl, true);
    sleep_us(5);

    // restore the GPIOs to their I2C functions
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_set_function(sda, GPIO_FUNC_I2C);

    // note the result
    Log(LOG_INFO, "I2C%d bus reset %s, %d clocks sent\n",
        busNum,
        gpio_get(sda) && gpio_get(scl) ? "OK" :
        gpio_get(sda) && !gpio_get(scl) ? "FAILED (SCL stuck low)" :
        !gpio_get(sda) && gpio_get(scl) ? "FAILED (SDA stuck low)" :
        "FAILED (SDA and SCL stuck low)",
        nClocks);

    // extra work required if reinitializing the bus
    if (!isStartup)
    {
        // re-send device initialization commands
        for (auto &dev : devices)
        {
            // reinitialize the device
            dev->I2CReinitDevice(this);

            // update the watchdog timer (some device initialization
            // command sequences are relatively lengthy)
            watchdog_update();            
        }

        // re-enable the I2C IRQ
        EnableIRQ(true);
    }
}

void I2C::EnableIRQ(bool enable)
{
    // enable/disable our I2C IRQ handler
    irq_set_enabled(i2cIrqNum, enable);
}

void I2C::EnableIRQs(bool enable)
{
    for (int i = 0 ; i < _countof(inst) ; ++i)
    {
        if (inst[i] != nullptr)
            inst[i]->EnableIRQ(enable);
    }
}

bool I2C::InitDMA()
{
    // configure the TX channel, if we haven't already
    if (dmaChannelTx < 0)
    {
        // Claim a free channel.  Don't panic on failure (required ==
        // false), but instead just abort the initialization for now.
        // The state machine will retry later, and won't actually
        // perform any DMA functions until initialization succeeds.
        // This lets the rest of the system keep going even when I2C
        // isn't working, so that the USB config interface can report
        // troubleshooting information back to the host.
        dmaChannelTx = dma_claim_unused_channel(false);
        if (dmaChannelTx < 0)
            return false;

        // Set up the configuration: read from memory with increment, write
        // to I2C port with no increment, 16-bit transfers, use I2C DREQ.
        // Writes use 16-bit transfers so that we set both the data bits
        // in the low byte of the control port AND the control bits in the
        // high byte of the control port on each transfer.
        configTx = dma_channel_get_default_config(dmaChannelTx);
        channel_config_set_read_increment(&configTx, true);
        channel_config_set_write_increment(&configTx, false);
        channel_config_set_transfer_data_size(&configTx, DMA_SIZE_16);
        channel_config_set_dreq(&configTx, i2c_get_dreq(i2c, true));
    }

    // configure the RX chanel. if we haven't already
    if (dmaChannelRx < 0)
    {
        // claim a free channel
        dmaChannelRx = dma_claim_unused_channel(false);
        if (dmaChannelRx < 0)
            return false;

        // Set up the configuration: read from I2C port with no increment,
        // write to memory with increment, 8-bit transfers, use I2C DREQ.
        // Reads only transfer the 8-bit data byte portion of the control
        // port (we don't need to read back the control bits in the high
        // byte).
        configRx = dma_channel_get_default_config(dmaChannelRx);
        channel_config_set_read_increment(&configRx, false);
        channel_config_set_write_increment(&configRx, true);
        channel_config_set_transfer_data_size(&configRx, DMA_SIZE_8);
        channel_config_set_dreq(&configRx, i2c_get_dreq(i2c, false));
    }

    // success
    return true;
}

// I2C interrupt handler
void I2C::I2CIRQ()
{
    // get the hardware control struct, read the interrupt status register
    auto *hw = i2c_get_hw(i2c);
    uint32_t status = hw->intr_stat;

    // flag that we're in the IRQ handler
    inIrq = true;

    // check for a TX abort
    if ((status & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) != 0)
    {
        // transfer aborted
        busAbort = true;

        // read the clr_tx_abrt register to clear the condition
        auto volatile dummy = hw->clr_tx_abrt;
    }

    // check for a stop condition
    if ((status & I2C_IC_INTR_STAT_R_STOP_DET_BITS) != 0)
    {
        // STOP -> current transfer completed
        busStop = true;

        // read the clr_stop_det register to clear the condition flag
        auto volatile dummy = hw->clr_stop_det;

        // if a device is active, call completion methods
        if (devices.size() != 0)
        {
            // count the completion
            if (state == State::Reading)
                devices[curDevice]->i2cStats.rxCompleted += 1;
            else
                devices[curDevice]->i2cStats.txCompleted += 1;
            
            // end the capture in progress
            IF_I2C_DEBUG(DebugCaptureEnd(devices[curDevice]->i2cAddr, rxBuf.data, rxBuf.len, "OK"));
            
            // invoke the device callback
            I2CX i2cx(this);
            devices[curDevice]->OnI2CCompletionIRQ(rxBuf.data, rxBuf.len, &i2cx);
        }
    }

    // done with IRQ
    inIrq = false;
}

void I2C::Task()
{
    if (inst[0] != nullptr)
        inst[0]->UnitTask();
    if (inst[1] != nullptr)
        inst[1]->UnitTask();
}

void I2C::UnitTask()
{
    // if there aren't any devices, there's nothing to do
    if (devices.size() == 0)
        return;

    // if DMA configuration failed, try again
    if ((dmaChannelTx < 0 || dmaChannelRx < 0))
    {
        // take another shot at DMA initialization
        if (!InitDMA())
            return;
    }

    // proceed from the last state
    switch (state)
    {
    case State::Ready:
        // Check for console bus-clear requests and bus timeout conditions
        if (busClearRequested)
        {
            // initiate the bus-clear operation
            BusClear(false);

            // request processed
            busClearRequested = false;
        }
        else if (consecutiveTimeouts > 20 && time_us_64() > tNextAutoReset)
        {
            // Too many consecutive timeouts - some device on the bus is probably
            // in a wedged state, holding SDA low waiting for an SCL tick it missed.
            // Try a bus-clear operation to try to reset the I2C state machine for
            // whichever device is holding onto SDA.
            Log(LOG_ERROR, "I2C%d: too many consecutive timeouts; attempting a bus reset\n");
            BusClear(false);

            // Reset the timeout monitor.  Don't try another automatic bus clear
            // for a while - if this one didn't work, the problem probably can't
            // be cleared this way, so there's no point in burning up a lot of
            // CPU time repeating the bus-clear attempt.  It might still be worth
            // trying again later in case the problem is temporary but of fairly
            // long duration, so the point here is to slow down the retries without
            // blocking them entirely.
            consecutiveTimeouts = 0;
            tNextAutoReset = time_us_64() + 10000000;
        }

        // Check for an ad hoc TX/RX request from the console
        if (adHocRequestAddr != 0)
        {
            // Disable IRQs - we're going to do direct read/write calls through
            // the SDK, which are incompatible with our interrupt handler.  (Our
            // handler messes with the interrupt status flags, which the SDK
            // direct read/write APIs read from user mode, so we can't have our
            // IRQ changing them out from under the SDK functions.)
            EnableIRQ(false);

            // try the write
            int result = i2c_write_timeout_us(i2c, adHocRequestAddr, adHocBuf, adHocTxLen, adHocRxLen != 0, 3000);
            if (result == adHocTxLen)
            {
                // try the read, if requested
                if (adHocRxLen != 0)
                {
                    adHocRxLen = std::min(adHocRxLen, static_cast<int>(_countof(adHocBuf)));
                    if (i2c_read_timeout_us(i2c, adHocRequestAddr, adHocBuf, adHocRxLen, false, 3000) == adHocRxLen)
                    {
                        // build a printable representation
                        char buf[_countof(adHocBuf)*3 + 1];
                        const uint8_t *src = adHocBuf;
                        char *dst = buf;
                        for (int i = 0 ; i < adHocRxLen ; ++i)
                        {
                            uint8_t hi = (*src >> 4) & 0x0F;
                            uint8_t lo = (*src++) & 0x0F;
                            *dst++ = hi < 10 ? hi + '0' : hi + 'A' - 10;
                            *dst++ = lo < 10 ? lo + '0' : lo + 'A' - 10;
                            *dst++ = ' ';
                        }
                        *(dst-1) = 0;

                        // log it
                        Log(LOG_INFO, "Ad hoc I2C RX from 0x%02x OK [%s]\n", adHocRequestAddr, buf);
                    }
                    else
                        Log(LOG_ERROR, "Ad hoc I2C RX from 0x%02x (%d bytes) failed\n", adHocRequestAddr, adHocRxLen);
                }
                else
                {
                    // just the transmit
                    Log(LOG_INFO, "Ad hoc I2C TX to 0x%02x (%d bytes) OK\n", adHocRequestAddr, adHocTxLen);
                }
            }
            else
                Log(LOG_ERROR, "Ad hoc I2C TX to 0x%02x (%d bytes) failed, error code %d%s\n",
                    adHocRequestAddr, adHocTxLen, result,
                    result == PICO_ERROR_TIMEOUT ? " (timeout)" : result == PICO_ERROR_GENERIC ? " (generic error)" : "");

            // clear the request
            adHocRequestAddr = 0;
            adHocTxLen = adHocRxLen = 0;

            // restore our IRQ
            EnableIRQ(true);
        }

        // Ready for a new transaction on the next device with pending
        // work.  If a bus scan is in progress, sneak in a test on the
        // next address.
        if (busScanAddr != -1 && time_us_64() > busScanTime)
        {
            // send out the next bus scan request
            uint8_t buf;
            if (i2c_read_blocking_until(i2c, static_cast<uint8_t>(busScanAddr), &buf, 1, false, time_us_64() + 100) >= 0)
                Log(LOG_INFO, "I2C%d bus scan: found device at 0x%02x\n", busNum, busScanAddr);

            // on to the next address; stop when we reach the reserved range 0x78-0x7F
            if (++busScanAddr >= 0x78)
            {
                busScanAddr = -1;
                Log(LOG_INFO, "I2C%d bus scan completed\n", busNum);
            }

            // set the next bus scan time
            busScanTime = time_us_64() + 5000;
        }
        
        // Scan for a device with pending work, starting where we left
        // off from last time.
        for (int prvDevice = curDevice ; ; )
        {
            // Tell the current device that the I2C bus is available for
            // its use.  This gives the device a chance to start a transaction
            // (a read or write) if it has work pending.  If the device does
            // start a transaction, stop the scan and return to the main loop,
            // to let other tasks run while the I2C operation proceeds in the
            // background via DMA.
            I2CX i2cx(this);
            if (devices[curDevice]->OnI2CReady(&i2cx))
                break;
            
            // advance to the next device, wrapping at the end of the list
            curDevice = (curDevice + 1) % devices.size();
            
            // stop if we've wrapped back to where we started - this means
            // that none of the devices have pending work at the moment, so
            // we should yield the CPU and let other non-I2C tasks proceed
            if (curDevice == prvDevice)
                break;
        }
        break;

    case State::Writing:
    case State::Reading:
        // Transmission in progress.  Check for an abort condition, indicating
        // an error; a stop condition, indicating successful completion; or a
        // timeout condition.
        if (busAbort)
        {
            // Abort condition -> bus error.  Abort any DMA operation still
            // in progress.
            IF_I2C_DEBUG(DebugCaptureEnd(devices[curDevice]->i2cAddr, nullptr, 0, "Abort"));
            dma_channel_abort(dmaChannelTx);
            if (state == State::Reading)
                dma_channel_abort(dmaChannelRx);

            // update statistics and notify the device
            devices[curDevice]->i2cStats.txAborted += 1;
            devices[curDevice]->OnI2CAbort();

            // return to Ready state
            state = State::Ready;

            // This doesn't count as a timeout, but it also doesn't
            // count as normal completion, so I think it's best to leave
            // the consecutive timeout counter unchanged in this case.
            // consecutiveTimeouts = 0;
        }
        else if (busStop)
        {
            // DMA completed.  If this was a read operation, pass the received
            // data to the device callback.  Otherwise call the TX completion
            // callback.
            I2CX i2cx(this);
            auto prevState = state;
            if (prevState == State::Reading)
            {
                // capture it for debugging
                IF_I2C_DEBUG(DebugCaptureEnd(devices[curDevice]->i2cAddr, rxBuf.data, rxBuf.len, "OK"));

                // read transaction - call the Receive callback with the received data
                state = State::Ready;
                devices[curDevice]->OnI2CReceive(rxBuf.data, rxBuf.len, &i2cx);
            }
            else
            {
                // capture it for debugging
                IF_I2C_DEBUG(DebugCaptureEnd(devices[curDevice]->i2cAddr, nullptr, 0, "OK"));

                // write transaction - call the completion callback
                state = State::Ready;
                devices[curDevice]->OnI2CWriteComplete(&i2cx);
            }

            // the operation ended properly, so clear the consecutive timeout counter
            consecutiveTimeouts = 0;
        }
        else if (time_us_64() > tTimeout)
        {
            // timeout - abort the DMA transfer(s)
            IF_I2C_DEBUG(DebugCaptureEnd(devices[curDevice]->i2cAddr, nullptr, 0, "Timeout"));
            dma_channel_abort(dmaChannelTx);
            if (state == State::Reading)
                dma_channel_abort(dmaChannelRx);

            // count statistics and notify the device
            devices[curDevice]->i2cStats.txTimeout += 1;
            devices[curDevice]->OnI2CTimeout();

            // count the consecutive timeout
            consecutiveTimeouts += 1;

            // return to ready state
            state = State::Ready;
        }

        // if we're now in Ready state, advance the round-robin counter
        // to the next device
        if (state == State::Ready)
            curDevice = (curDevice + 1) % devices.size();

        // done
        break;
    }
}

void I2C::ReadWrite(const uint8_t *txData, size_t txLen, size_t rxLen)
{
    // only proceed if we're in READY state
    if (state != State::Ready && !inIrq)
    {
        Log(LOG_ERROR, "I2C%d Read/Write not in Ready state (state %d)\n", busNum, static_cast<int>(state));
        return;
    }

    // limit the transmission length
    if (txLen + rxLen > _countof(txBuf.data))
    {
        Log(LOG_ERROR, "I2C%d Read/Write TX overflow (%d bytes TX + %d bytes RX, %d buffer max)\n",
            busNum, static_cast<int>(txLen), static_cast<int>(rxLen), BUF_SIZE);
        txLen = BUF_SIZE;
    }
    txBuf.len = static_cast<unsigned int>(txLen);

    // limit the reception length
    if (rxLen > _countof(rxBuf.data))
    {
        Log(LOG_ERROR, "I2C%d Read/Write RX overflow (%d bytes, %d buffer max)\n", busNum, static_cast<int>(rxLen), BUF_SIZE);
        rxLen = BUF_SIZE;
    }
    rxBuf.len = static_cast<unsigned int>(rxLen);

    // capture for debugging, if applicable
    IF_I2C_DEBUG(DebugCaptureStart(devices[curDevice]->i2cAddr, txData, txLen, rxLen));

    // Store the transmit data, translating from the 8-bit source data to the
    // 16-bit combined data/control words that we DMA-transfer to the I2C
    // control port.
    uint16_t *dst = txBuf.data;
    const uint8_t *src = txData;
    for (size_t i = 0 ; i < txLen ; ++i)
        *dst++ = *src++;

    // the first byte of the transmission must be preceded by a START condition
    txBuf.data[0] |= I2C_IC_DATA_CMD_RESTART_BITS;

    // If there's a READ portion, add a command port entry for each byte
    // we intend to read.  The I2C controller only receives bytes when
    // commanded to do so.
    if (rxLen != 0)
    {
        // add a CMD port command to the TX buffer for each receive byte
        for (size_t i = 0 ; i < rxLen ; ++i)
            *dst++ = I2C_IC_DATA_CMD_CMD_BITS;

        // the first byte read must be preceded by a START condition
        txBuf.data[txLen] |= I2C_IC_DATA_CMD_RESTART_BITS;
    }

    // the last byte must be followed by a STOP
    txBuf.data[txLen + rxLen - 1] |= I2C_IC_DATA_CMD_STOP_BITS;

    // execute the read/write
    ExecReadWrite();
}

void I2C::MultiReadWrite(const TXRX *txrx, int n)
{
    // Only proceed if we're in READY state or in the IRQ handler.  A
    // new transaction is allowed from the completion callback, called
    // in interrupt context.
    if (state != State::Ready && !inIrq)
    {
        Log(LOG_ERROR, "I2C%d Read/Write not in Ready state (state %d)\n", busNum, static_cast<int>(state));
        return;
    }

    // build the transmission control block
    uint16_t *dst = txBuf.data;
    size_t txLenTotal = 0;
    size_t rxLenTotal = 0;
    IF_I2C_DEBUG(uint8_t txDbgBuf[BUF_SIZE]);
    IF_I2C_DEBUG(uint8_t *pTxDbgBuf = &txDbgBuf[0]);
    for (int i = 0 ; i < n ; ++i, ++txrx)
    {
        // add the totals
        txLenTotal += txrx->txLen;
        rxLenTotal += txrx->rxLen;

        // limit the transmission length
        if (txLenTotal + rxLenTotal > _countof(txBuf.data))
        {
            if (!inIrq)
            {
                Log(LOG_ERROR, "I2C%d Multi Read/Write TX overflow (%d bytes TX + %d bytes RX, %d buffer max)\n",
                    busNum, static_cast<int>(txLenTotal), static_cast<int>(rxLenTotal), BUF_SIZE);
            }
            return;
        }

        // limit the reception length
        if (rxLenTotal > _countof(rxBuf.data))
        {
            if (!inIrq)
                Log(LOG_ERROR, "I2C%d Multi Read/Write RX overflow (%d bytes, %d buffer max)\n", busNum, static_cast<int>(rxLenTotal), BUF_SIZE);
            return;
        }

        // Store the transmit data, translating from the 8-bit source data to the
        // 16-bit combined data/control words that we DMA-transfer to the I2C
        // control port.
        const uint8_t *src = txrx->txData;
        uint16_t *dst0 = dst;
        for (size_t j = 0 ; j < txrx->txLen ; ++j)
        {
            IF_I2C_DEBUG(if (pTxDbgBuf < &txDbgBuf[BUF_SIZE]) *pTxDbgBuf++ = *src);
            *dst++ = *src++;
        }

        // the first byte of the transmission must be preceded by a START condition
        *dst0 |= I2C_IC_DATA_CMD_RESTART_BITS;

        // If there's a READ portion, add a command port entry for each byte
        // we intend to read.  The I2C controller only receives bytes when
        // commanded to do so, which is done by (paradoxically) writing a
        // UINT16 to the TX FIFO with the CMD (bit 0x0100) set.  This doesn't
        // actually transmit anything, but rather has the special side effect
        // of clocking in a byte from the slave and delivering it to the RX
        // FIFO.
        if (txrx->rxLen != 0)
        {
            // add a CMD port command to the TX buffer for each receive byte
            dst0 = dst;
            for (size_t j = 0 ; j < txrx->rxLen ; ++j)
                *dst++ = I2C_IC_DATA_CMD_CMD_BITS;

            // the first byte read must be preceded by a START condition
            *dst0 |= I2C_IC_DATA_CMD_RESTART_BITS;
        }
    }

    // capture for debugging, if applicable
    IF_I2C_DEBUG(DebugCaptureStart(devices[curDevice]->i2cAddr, txDbgBuf, txLenTotal, rxLenTotal));

    // the last byte must be followed by a STOP
    *(dst - 1) |= I2C_IC_DATA_CMD_STOP_BITS;

    // set the total buffer lengths
    txBuf.len = static_cast<unsigned int>(txLenTotal);
    rxBuf.len = static_cast<unsigned int>(rxLenTotal);

    // Execute the read/write
    ExecReadWrite();
}

void I2C::ExecReadWrite()
{
    // set up the target device address
    auto *hw = i2c_get_hw(i2c);
    hw->enable = 0;
    hw->tar = devices[curDevice]->i2cAddr;
    hw->enable = 1;

    // reset the bus condition flags
    busStop = false;
    busAbort = false;

    // Enter TX state.  If this is a write only, we're in WRITING state,
    // otherwise we're in the TX portion of the READING state.
    state = (rxBuf.len != 0 ? State::Reading : State::Writing);

    // count the transmission initiated
    devices[curDevice]->i2cStats.txStarted += 1;

    // Start the RX DMA operation, if this tranaction includes an RX
    // portion.  This won't actually trigger until the peripheral we're
    // talking to starts sending.  We have to set it up ahead of time,
    // so that it's ready to go when the I2C controller starts clocking
    // in the RX portion of the transaction.
    if (rxBuf.len != 0)
        dma_channel_configure(dmaChannelRx, &configRx, rxBuf.data, &i2c_get_hw(i2c)->data_cmd, rxBuf.len, true);

    // Now start the DMA TX operation to initiate the request.  The device
    // will start transmitting as soon as it receives the request, and our
    // DMA RX operation that we set up above will be ready for the incoming
    // data as soon as it starts arriving.  The transmission is the sum of
    // the TX and RX lengths, since we have to write a word to the TX
    // command register to clock in each RX byte.
    dma_channel_configure(dmaChannelTx, &configTx, &i2c_get_hw(i2c)->data_cmd, txBuf.data, txBuf.len + rxBuf.len, true);

    // set a timeout
    tTimeout = time_us_64() + 2500;
}

void I2C::Write(const uint8_t *data, size_t len)
{
    // a Write is just a Read/Write without the read
    ReadWrite(data, len, 0);
}
    
void I2C::Read(const uint8_t *txData, size_t txLen, size_t rxLen)
{
    // do the combination Read/Write
    ReadWrite(txData, txLen, rxLen);
}

// extended atoi() with 0x and $ prefix for hex input
static int atoix(const char *p)
{
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        // 0x prefix - hex
        return strtol(p + 2, nullptr, 16);
    }
    else if (p[0] == '$' || p[0] == 'x' || p[0] == 'X')
    {
        // $ or x prefix - hex
        return strtol(p + 1, nullptr, 16);
    }
    else if (p[0] == '0' && p[1] == 'o')
    {
        // 0o prefix - octal
        return strtol(p + 2, nullptr, 8);
    }
    else if (p[0] == '0' && p[1] == 'b')
    {
        // 0b prefix - binary
        return strtol(p + 2, nullptr, 2);
    }
    else
    {
        // anything else is ordinary decimal
        return atoi(p);
    }
}

// Console commands
void I2C::Command_main(const ConsoleCommandContext *c)
{
    if (c->argc <= 1)
        return c->Usage();

    const char *cmd = c->argv[0];
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "--bus-scan") == 0)
        {
            // start the bus scan at the first non-reserved address
            busScanAddr = 0x08;
            c->Printf("Starting bus scan on I2C%d\n", busNum);
        }
        else if (strcmp(a, "--bus-clear") == 0)
        {
            busClearRequested = true;
            c->Printf("Initiating bus clear on I2C%d\n", busNum);
        }
        else if (strcmp(a, "--stats") == 0 || strcmp(a, "-s") == 0)
        {
            c->Printf(
                "Current device: %d\n"
                "State:          %s\n",
                curDevice,
                state == State::Ready ? "Ready" : state == State::Writing ? "TX" : state == State::Reading ? "TX" : "Unknown");
            int devIdx = 0;
            for (auto &dev : devices)
            {
                c->Printf("\nDevice %d: %s, addr 0x%02X\n", devIdx++, dev->I2CDeviceName(), dev->i2cAddr);
                dev->i2cStats.Print(c);
            }
        }
        else if (strcmp(a, "--tx") == 0 || strcmp(a, "--rx") == 0)
        {
            // parse the address
            int addr;
            if (++i >= c->argc || (addr = atoix(c->argv[i])) < 0x08 || addr > 0x77)
                return c->Printf("%s: %s requires address argument in range 0x08..0x77\n", cmd, a);

            // parse the TX byte list
            if (++i >= c->argc)
                return c->Printf("%s: %s requires transmit argument list (or - for an RX with no TX)\n", cmd, a);

            const char *s = c->argv[i];
            uint8_t *dst = adHocBuf;
            int nTx = 0;
            for ( ; nTx < _countof(adHocBuf) && *s != 0 ; ++nTx)
            {
                // stop at '-'
                if (s[0] == '-' && s[1] == 0)
                    break;

                // parse the next number
                *dst++ = static_cast<uint8_t>(atoix(s));

                // scan for a comma
                for ( ; *s != 0 && *s != ',' ; ++s);
                if (*s == ',')
                    ++s;
            }

            // make sure we didn't overflow the buffer
            if (*s != 0)
                return c->Printf("%s: too many TX bytes for %s (limit %d)\n", cmd, s, static_cast<int>(_countof(adHocBuf)));

            // set the transmission length
            adHocTxLen = nTx;

            // if this is an RX, set the read length
            adHocRxLen = 0;
            if (strcmp(a, "--rx") == 0)
            {
                if (++i >= c->argc || (adHocRxLen = atoix(c->argv[i])) == 0 || adHocRxLen > _countof(adHocBuf))
                    return c->Printf("%s: %s requires receive length 0..%d\n", cmd, a, static_cast<int>(_countof(adHocBuf)));
            }

            // successfully configured - set the request address
            adHocRequestAddr = addr;
        }
#ifdef I2C_DEBUG
        else if (strcmp(a, "--dump") == 0)
        {
            int n;
            if (++i >= c->argc || (n = atoi(c->argv[i])) == 0 || n > DEBUG_CAPTURE_COUNT)
                return c->Printf("%s: --dump requires number of transactions to display, 1..%d\n", cmd, DEBUG_CAPTURE_COUNT);

            // check for an --addr or -a following
            int addr = 0;
            if (i + 1 < c->argc && (strcmp(c->argv[i+1], "-a") == 0 || strcmp(c->argv[i+1], "--addr") == 0))
            {
                i += 2;
                addr = i < c->argc ? atoix(c->argv[i]) : 0;
                if (addr < 0x08 || addr > 0x77)
                    return c->Printf("%s: --dump <n> %s requires address argument, 0x08..0x77\n", cmd, c->argv[i-1]);
            }

            for (int i = 0, idx = debugCaptureWrite - 1 ; i < n ; ++i, --idx)
            {
                // wrap at 0
                if (idx < 0)
                    idx = DEBUG_CAPTURE_COUNT - 1;

                // if the address is zero, we're out of entries
                auto &dc = debugCapture[idx];
                if (dc.addr == 0)
                {
                    c->Printf("(No more entries)\n");
                    break;
                }

                // filter for address
                if (addr != 0 && dc.addr != addr)
                    continue;

                // display this item
                c->Printf("Addr 0x%02X, time %llu: tx={ ", dc.addr, dc.t);
                const char *sep = "";
                for (int i = 0 ; i < dc.txLen && i < DEBUG_CAPTURE_BUFLEN ; ++i, sep = ", ")
                    c->Printf("%s%02x", sep, dc.tx[i]);
                c->Printf(" }");
                if (dc.rxExpected != 0)
                {
                    c->Printf(", rx={ ");
                    sep = "";
                    for (int i = 0 ; i < dc.rxLen && i < DEBUG_CAPTURE_BUFLEN ; ++i, sep = ", ")
                        c->Printf("%s%02x", sep, dc.rx[i]);
                    c->Printf(" }");
                }
                c->Printf(" %s", dc.status);
                c->Printf("\n");
            }
        }
        else if (strcmp(a, "--capture-filter") == 0)
        {
            // make sure there's an argument
            int n;
            if (++i >= c->argc)
                return c->Printf("%s: %s requires comma-separated address list or '*'\n", cmd, a);

            // parse the list, '*', or '-'
            if (strcmp(c->argv[i], "*") == 0)
            {
                // '*' clears the filter
                debugCaptureAddrFilterCnt = 0;
                c->Printf("%s: debug capture filter reset; transactions on all addresses will be captured\n", cmd);
            }
            else if (strcmp(c->argv[i], "-") == 0)
            {
                // '-' disables capture
                debugCaptureAddrFilterCnt = -1;
                c->Printf("%s: debug capture disabled; use \"--capture-filter *\" to re-enable\n", cmd);
            }
            else
            {
                // parse the list of addresses
                const char *p = c->argv[i];
                for (debugCaptureAddrFilterCnt = 0 ; *p != 0 ; )
                {
                    // abort if too many addresses
                    if (debugCaptureAddrFilterCnt >= DEBUG_CAPTURE_ADDR_FILTER_MAX)
                        return c->Printf("%s: too many addresses in filter list (maximum of %d allowed)\n", cmd, DEBUG_CAPTURE_ADDR_FILTER_MAX);

                    // add this address
                    debugCaptureAddrFilter[debugCaptureAddrFilterCnt++] = atoix(p);

                    // skip to and then past the next comma
                    for ( ; *p != ',' && *p != 0 ; ++p) ;
                    if (*p == ',')
                        ++p;
                }
                c->Printf("%s: capture address filter set; use \"--capture-filter *\" to remove filter and capture all\n", cmd);
            }
        }
#endif
        else
        {
            // bad option
            return c->Printf("%s: unknown option \"%s\"\n", cmd, a);
        }
    }
}


// ---------------------------------------------------------------------------
//
// Debugging
//
#ifdef I2C_DEBUG

void I2C::DebugCaptureStart(uint8_t addr, const uint8_t *txData, size_t txLen, size_t rxLen)
{
    // this can be called from regular or interrupt context
    IRQDisabler irqd;

    // flag an error if a capture is already open
    if (debugCaptureOpen)
    {
        Log(LOG_DEBUG, "I2C%d debug capture started while already open (cur addr 0x%02X, prv 0x%02X; reg 0x%02x, txLen %d, rxLen %d)\n",
            busNum, addr, debugCapture[(debugCaptureWrite > 0 ? debugCaptureWrite : DEBUG_CAPTURE_COUNT) - 1].addr,
            txData != nullptr ? txData[0] : 0x00, txLen, rxLen);
    }

    // if there's a filter, only capture addresses matching the filter
    if (!DebugCaptureMatchAddrFilter(addr))
        return;

    // populate the next write element
    auto &dc = debugCapture[debugCaptureWrite];
    dc.t = time_us_64();
    dc.status = (rxLen == 0 ? "TX" : "RX");
    dc.addr = addr;
    dc.txLen = static_cast<uint8_t>(std::min(txLen, 255U));
    dc.rxExpected = static_cast<uint8_t>(std::min(rxLen, 255U));  // set the expected receive len
    dc.rxLen = 0;                                                 // ..but we haven't received anything yet
    if (txLen != 0)
        memcpy(dc.tx, txData, std::min(txLen, DEBUG_CAPTURE_BUFLEN));

    // advance the write pointer, wrapping at the end of the ring
    if (++debugCaptureWrite >= DEBUG_CAPTURE_COUNT)
        debugCaptureWrite = 0;

    // capture is now open
    debugCaptureOpen = true;
}

void I2C::DebugCaptureEnd(uint8_t addr, const uint8_t *rxData, size_t rxLen, const char *status)
{
    // this can be called from regular or interrupt context
    IRQDisabler irqd;

    // If no capture is open, ignore it.  This isn't an error, since the
    // current transaction could have been skipped for capture due to the
    // filter or other settings.
    if (!debugCaptureOpen)
        return;

    // make sure that this lines up with the last record
    int idx = (debugCaptureWrite > 0 ? debugCaptureWrite : DEBUG_CAPTURE_COUNT) - 1;
    auto &dc = debugCapture[idx];
    if (dc.addr == addr)
    {
        dc.status = status;
        dc.rxLen = static_cast<uint8_t>(std::min(rxLen, 255U));
        if (rxLen != 0)
            memcpy(dc.rx, rxData, std::min(rxLen, DEBUG_CAPTURE_BUFLEN));
    }
    else
    {
        Log(LOG_DEBUG, "I2C%d end-capture addr mismatch, idx=%d, new addr=0x%02x, old=0x%02x, rxLen=%d\n",
            busNum, idx, addr, dc.addr, rxLen);
    }

    // capture closed
    debugCaptureOpen = false;
}

bool I2C::DebugCaptureMatchAddrFilter(uint8_t addr) const
{
    if (debugCaptureAddrFilterCnt < 0)
    {
        // negative -> disable capture entirely
        return false;
    }
    else if (debugCaptureAddrFilterCnt == 0)
    {
        // 0 -> no filter, capture all addresses
        return true;
    }
    else
    {
        // only match addresses in the filter list
        for (int i = 0 ; i < debugCaptureAddrFilterCnt ; ++i)
        {
            if (debugCaptureAddrFilter[i] == addr)
                return true;
        }
        return false;
    }
}

#endif // I2C_DEBUG


// ---------------------------------------------------------------------------
//
// I2C device statistics
//

// display the statistics to a command console
void I2CDevice::I2CStats::Print(const ConsoleCommandContext *ctx)
{
    ctx->Printf(
        "  TX/RX started: %llu\n"
        "  TX completed:  %llu\n"
        "  RX completed:  %llu\n"
        "  TX timed out:  %llu\n"
        "  TX aborted:    %llu\n",
        txStarted, txCompleted, rxCompleted, txTimeout, txAborted);
}
