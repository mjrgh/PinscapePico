// Pinscape Pico - Expansion Boards
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdlib.h>
#include <stdint.h>
#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/watchdog.h>
#include "ExpansionBoard.h"
#include "JSON.h"
#include "GPIOManager.h"
#include "Logger.h"

// global singleton
ExpansionBoard expansionBoard;

// configuration
void ExpansionBoard::Configure(JSONParser &json)
{
    if (auto *val = json.Get("expansionBoard"); !val->IsUndefined())
    {
        // peripheral power control setup
        if (auto *pp = val->Get("peripheralPowerEnable"); !pp->IsUndefined())
        {
            int gp = peripheralPower.gpEnable = pp->Get("gp")->Int(-1);
            bool activeHigh = peripheralPower.activeHigh = pp->Get("activeHigh")->Bool(true);
            peripheralPower.offTime = pp->Get("powerOffTime")->UInt32(250);
            peripheralPower.waitTime = pp->Get("waitTime")->UInt32(100);

            // claim the GPIO, if configured
            if (IsValidGP(gp)
                && gpioManager.Claim("ExpansionBoard.PeripheralPowerEnable", "Peripheral Power Enable", gp))
            {
                // configure the output port, initially with the power off
                gpio_init(gp);
                gpio_put(gp, !activeHigh);
                gpio_set_dir(gp, GPIO_OUT);
                
                // log success
                Log(LOG_CONFIG, "Expansion board peripheral power enable configured on GP%d, active %s, power-off time %u ms, wait time %u ms\n",
                    gp, activeHigh ? "high" : "low", peripheralPower.offTime, peripheralPower.waitTime);
            }
            else
            {
                Log(LOG_ERROR, "Peripheral power control is not configured because expansionBoard.peripheralPowerEnable.gp is missing or invalid\n");
            }
        }
    }
}

void ExpansionBoard::CyclePeripheralPower()
{
    auto &pp = peripheralPower;
    if (IsValidGP(pp.gpEnable))
    {
        // turn power off
        gpio_put(pp.gpEnable, !pp.activeHigh);

        // sleep for the delay time, waking periodically to reset the watchdog
        Log(LOG_INFO, "Resetting peripherals by turning off power for %u ms\n", pp.offTime);
        uint64_t tEnd = time_us_64() + pp.offTime*1000ULL;
        while (time_us_64() < tEnd)
        {
            sleep_us(1000);
            watchdog_update();
        }

        // restore power
        gpio_put(pp.gpEnable, pp.activeHigh);

        // 
        Log(LOG_INFO, "Peripheral power restored; waiting %u ms for devices to cycle\n", pp.waitTime);
        tEnd = time_us_64() + pp.waitTime*1000ULL;
        while (time_us_64() < tEnd)
        {
            sleep_us(1000);
            watchdog_update();
        }
    }
}
