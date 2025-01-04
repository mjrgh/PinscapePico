// Pinscape Pico - create configuration reset UF2
// Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This program creates a small UF2 file that can be used to clear the
// stored configuration file on a Pinscape Pico unit.  To clear the
// stored config:
//
// 1. Compile and run this program to create the UF2 file.
//
// 2. Reset the Pico into Boot Loader mode (by unplugging the Pico from
//    USB and plugging it back in while holding down the BOOTSEL button
//    on top of the Pico).  The Pico should appear as a virtual thumb
//    drive on the Windows desktop.
//
// 3. Drop the UF2 file created in step 1 onto the Pico's thumb drive.
//    The Pico will reboot into the Pinscape firmware, with factory
//    defaults restored.
//
// Note that you only need the generated UF2 file to clear the config,
// so after creating the file, this program is no longer needed.  The
// program thus doesn't have to be included in a binary distribution -
// it's sufficient to include only the generated UF2 file.  End-user
// instructions can be simplified to just steps 2 and 3 above.
//
// Why this is useful: The Pinscape Pico stores its configuration
// data in the Pico's flash space in such a way that the config data
// will persist across power cycles and even firmware updates.  In
// most cases, you can always send a new configuration using the Config
// Tool.  However, that depends upon the Pinscape firmware running
// properly, because the Config Tool has to send the request to the
// firmware across the USB connection.  In the event that a bad set
// of configuration data is crashing the Pico or putting it into an
// unusable state where it won't accept new config updates from the
// Config Tool, it would be necessary to remove the bad config so
// that the firmware returns to stable factory defaults.  The UF2
// file that this program creates accomplishes that WITHOUT the need
// for the Pinscape firmware to be running, since it uses the Pico's
// fixed ROM boot loader instead.  It's physically impossible for a
// software fault to contravene the ROM boot loader when the user
// invokes it using the BOOTSEL button, so no matter how badly wedged
// the Pinscape firmware or config is, you can always get the Pico
// back to the Boot Loader mode.  Once in that mode, the UF2 file
// lets you delete the bad config data.  You can also replace the
// Pinscape firmware itself in that mode if the firmware itself is
// at fault rather than the config data.
//
// How this works: The Pinscape Pico configuration data is stored at
// a fixed location in the Pico's on-board flash memory space (it's
// stored at the very end of the 2MB flash space).  The last few bytes
// of this section contain some metadata with details on the size of
// the config data and some checksum information to confirm that a
// valid file is stored there.  When the Pinscape software starts up,
// it reads the metadata and validates the checksum; if it's good, it
// loads the config data, otherwise it simply uses factory defaults.
// This program generates a UF2 file that overwrites the last 256
// bytes of the flash space with zeroes; that invalidates any
// configuration metadata section that was previously stored there,
// so the next time the Pinscape software boots, it won't find valid
// config data and so will revert to factory defaults.  When the Pico
// is in Boot Loader mode, its fixed ROM software presents a virtual
// thumb drive that will accept UF2 files to store arbitrary user
// data at arbitrary locations in flash space.  The UF2 file we
// generate is contrived to store 256 bytes of zeroes in the last
// 256 bytes of the flash space.  The Pico boot loader automatically
// reboots the Pico after processing a UF2 file, so dropping the file
// will both clear the config and restart the Pinscape software.
//
// The config clearer UF2 file doesn't remove or change any installed
// Pinscape firmware.  The firmware program is stored at the bottom
// of the flash memory space, which won't be affected by the config
// clearer UF2.  This UF2 only clears the top 256 bytes of flash.


#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Pico flash layout.  This is for an official Raspberry Pi Pico (RP2)
// with the standard 2MB flash chip.  The Pinscape software always uses
// the first 2MB of flash for its config data, no matter how big the
// actual installed flash is, so no changes to these parameters should
// be needed for compatibillity with any third-party Pico models with
// bigger flash chips, at least for the purposes of this program.
const uint32_t XIP_BASE = 0x10000000;
const uint32_t PICO_FLASH_SIZE_BYTES = 2*1024*1024;

// write a UF2 block to clear the flash page starting at the given
// flash offset
static void WriteBlockClearer(FILE *fp, uint32_t flashOffset)
{
    struct UF2_Block {
        uint32_t magicStart0 = 0x0A324655;
        uint32_t magicStart1 = 0x9E5D5157;
        uint32_t flags = 0;
        uint32_t targetAddr = 0;
        uint32_t payloadSize = 0;
        uint32_t blockNo = 0;
        uint32_t numBlocks = 0;
        uint32_t fileSize = 0; // or familyID;
        uint8_t data[476];
        uint32_t magicEnd = 0x0AB16F30;
    };
    UF2_Block blk;

    // set Pico family ID
    const uint32_t FLAGS_FAMILY_ID_PRESENT = 0x00002000;
    const uint32_t FAMILY_ID_RP2040 = 0xe48bff56;
    blk.flags = FLAGS_FAMILY_ID_PRESENT;
    blk.fileSize = FAMILY_ID_RP2040;

    // write one page (256 bytes) to the last page of flash
    blk.targetAddr = XIP_BASE + flashOffset;
    blk.payloadSize = 256;
    blk.blockNo = 0;
    blk.numBlocks = 1;

    // Set the page to all 0XFF bytes.  NOR flash such as on the Pico
    // erases to 0XFF bytes, and overwriting the erased bytes generally
    // has no effect, which might avoid the need for another erase pass
    // the next time someone wants to write this location.  (Set the
    // playload portion to 0xFF, and set the unused remainder to all
    // zeroes.)
    memset(blk.data, 0xFF, 256);
    memset(&blk.data[256], 0x00, sizeof(blk.data) - 256);

    // write the file
    if (fwrite(&blk, sizeof(blk), 1, fp) != 1)
        printf("Error writing file\n");
}

// main entrypoint
int main(int argc, const char **argv)
{
    // get the filename
    if (argc != 2)
    {
        printf("Usage: CreateConfigClearerUF2 <output-file-name>\n");
        return 1;
    }

    // open the file
    FILE *fp = fopen(argv[1], "wb");
    if (fp == nullptr)
    {
        printf("Unable to open output file \"%s\"\n", argv[1]);
        return 1;
    }

    // Write blocks to clear the 4k sector starting at 1MB, which is
    // where the littlefs file system area starts.  Each block is a
    // 256-byte flash page, so we need to write 8 pages to clear the
    // whole sector.
    for (int pgOfs = 0 ; pgOfs < 4096 ; pgOfs += 256)
        WriteBlockClearer(fp, 1024*1024 + pgOfs);

    // close the file and exit
    fclose(fp);
    return 0;
}
