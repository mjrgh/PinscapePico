// Pinscape Pico - Set PWM Worker Address
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is a small Windows program that sets the I2C slave address for a
// Pico running the PWMWorker firmware.  To use this, you must place the
// Pico in its native Boot Loader mode:
//
//   - Unplug the Pico from power and USB
//   - Press AND HOLD the BOOTSEL button on top of the Pico
//   - While still pressing the BOOTSEL button, plug the Pico into USB
//   - Release BOOTSEL
//
// The Pico should now appear as a virtual USB thumb drive on your
// Windows desktop, with a drive letter assigned by the system (e.g.,
// K:).  You can now run this program to assign an address:
//
//  SetPWMWorkerAddr 31 [K:]
//
// - Replace 31 with the new I2C slave address, in hexadecimal, that
//   you'd like to assign to the Pico.  The default address is 30 hex.
//
// - Replace K: with the actual drive letter that Windows assigns.  You
//   can omit this if only one Pico in Boot Loader mode is currently
//   attached.
//
// Every I2C device connected to a bus must have a unique address.
// This means that you must change a PWMWorker's address from the default
// if any of the following apply:
//
//   - You have some OTHER device on the same bus that uses address 0x30
//
//   - You're using more than one PWMWorker on the same bus
//
// In either case, simply change the address of each PWMWorker that
// conflicts with an existing device.  You can use any address in the
// range 08 to F7.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "../WinAPI/RP2BootLoaderInterface.h"

static void UsageExit()
{
    printf("Usage: SetPWMWorkerAddr <hex-address> [<drive>]\n"
           "\n"
           "Example: SetPWMWorkerAddr 3F K:\n"
           "\n"
           "<hex-address> is the new I2C slave address to assign to the Pico, as\n"
           "a hexadecimal number from 08 to F7.  Each address sharing an I2C bus\n"
           "must have a unique address, so you must select a different address\n"
           "for each PWMWorker you attach, and they all must be different from\n"
           "any other types of devices connected to the same bus.\n"
           "\n"
           "<drive> is the drive letter (with a colon) that Windows assigns to\n"
           "the Pico Boot Loader drive.  This lets you select a specific device\n"
           "if more than one is currently active.  You can omit this if there's\n"
           "only one Pico currently in Boot Loader mode.\n"
           "\n"
           "To place a Pico in Boot Loader mode, disconnect it from all power and\n"
           "USB, then plug it into a USB port on the PC while HOLDING DOWN the\n"
           "BOOTSEL button on top of the Pico.\n");
    exit(1);
}

int main(int argc, char **argv)
{
    // check arguments
    if (argc < 2)
        UsageExit();

    // Get the address.  Allow plain hex notation, or a C-style "0x" prefix,
    // or an assembler-style "$" prefix.  Treat the number as hex regardless;
    // this is just for the sake of people who want to type one of the prefixes
    // by force of habit.
    int argi = 1;
    const char *p = argv[argi++];
    if (*p == '$')
        ++p;
    else if (p[0] == '0' && tolower(p[1]) == 'x')
        p += 2;
    int addr = static_cast<int>(p, nullptr, 16);
    if (addr < 0x08 || addr > 0xF7)
    {
        printf("Invalid address - must be a hex number from 08 to F7\n");
        return 1;
    }

    // get the drive letter, if present
    std::string drive;
    if (argi < argc)
    {
        drive = argv[argi++];
        if (drive.back() != ':')
        {
            printf("Invalid drive letter - use the format X:\n");
            return 1;
        }

        // add '\\' to make it a path
        drive.append("\\");
    }

    // make sure that's all the arguments
    if (argi < argc)
        UsageExit();

    // get a list of boot devices, and search for the specified drive letter
    PinscapePico::RP2BootDevice *device = nullptr;
    auto drives = PinscapePico::RP2BootDevice::EnumerateRP2BootDrives();
    if (drives.size() == 0)
    {
        printf("No Picos in Boot Loader mode detected.\n");
        return 1;
    }
    else if (drives.size() == 1)
    {
        // there's exactly one drive - select it
        device = &drives.front();

        // if a drive letter was specified, make sure it matches
        if (drive.size() != 0 && _stricmp(device->path.c_str(), drive.c_str()) != 0)
        {
            printf("Drive %s not found or does not appear to be a Pico in Boot Loader mode\n", drive.c_str());
            return 1;
        }
    }
    else if (drive.size() == 0)
    {
        printf("Multiple Boot Loader devices found - please specify the drive letter for\n"
               "the one you'd like to program (see the program usage instructions).\n");
        return 1;
    }
    else
    {
        // scan for the drive letter
        for (auto &d : drives)
        {
            if (_stricmp(d.path.c_str(), drive.c_str()) == 0)
            {
                device = &d;
                break;
            }
        }

        if (device == nullptr)
        {
            printf("Drive %s not found or does not appear to be a Pico in Boot Loader mode\n", drive.c_str());
            return 1;
        }
    }

    // make sure there's a device
    if (device == nullptr)
    {
        printf("No Boot Loader device found\n");
        return 1;
    }

    // Copy the data
    static const uint32_t TOP_OF_FLASH = 0x10000000 + 2*1024*1024;
    static const uint32_t SECTOR_SIZE = 4096;
    static const uint32_t BLOCK_SIZE = 256;
    if (device->WriteUF2(TOP_OF_FLASH - SECTOR_SIZE, 1, [addr](uint8_t *buf, uint32_t blockNum)
    {
        // Generate our data block
        memset(buf, 0, BLOCK_SIZE);
        sprintf_s(reinterpret_cast<char*>(buf), BLOCK_SIZE, "PinscapePicoPWMWorker I2C Addr [%02X] \032", addr);
    }))
    {
        printf("Successfully updated device I2C address on %s to 0x%02X\n", device->path.c_str(), addr);
    }
    else
    {
        printf("Error updating I2C address\n");
        return 1;
    }

    // success
    return 0;
}
