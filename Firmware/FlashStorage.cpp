// Pinscape Pico - Flash Data Storage
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements a primitive file-like storage system on the Pico's built-in
// flash storage.  This is used for storing Pinscape's persistent data, such
// as configuration settings and plunger calibration data.

#include <stdlib.h>
#include <stdint.h>

#include <pico/stdlib.h>
#include <pico/flash.h>
#include <pico/time.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>

#include "Pinscape.h"
#include "Utils.h"
#include "crc32.h"
#include "Logger.h"
#include "CommandConsole.h"
#include "Watchdog.h"
#include "MultiCore.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "FlashStorage.h"

// global singleton
FlashStorage flashStorage;

// ---------------------------------------------------------------------------
//
// Flash-safe execution context helper routines
//
// These functions take timeout arguments, expressed in milliseconds.  These
// set the maximum time for the other core to respond to the flash access
// lockout request, which it must do before we can start writing or erasing
// flash.  The timeouts DON'T set a limit on the actual flash operation once
// we have the necessary exclusive access - there's no need for the caller
// to estimate how long the operation will take once started.
//

// write to flash with dual-core lockout
static bool FlashSafeWrite(uint32_t flashOffset, const void *data, size_t size, uint32_t timeout, const char *op, const char *filename)
{
    int status;
    {
        // Extend the watchdog timeout - writing a page of flash takes about 5ms,
        // so allow that plus an arbitrary additional margin of safety.  We don't
        // want the watchdog to interrupt an operation that's slow but not stuck.
        WatchdogTemporaryExtender wte(timeout + 100 + 10*(size + FLASH_PAGE_SIZE - 1)/FLASH_PAGE_SIZE);
        
        // do the write
        status = FlashSafeExecute([flashOffset, data, size](){
            flash_range_program(flashOffset, reinterpret_cast<const uint8_t*>(data), size); }, timeout);
    }

    // check for errors
    if (status == PICO_ERROR_TIMEOUT)
    {
        Log(LOG_ERROR, "%s%s: timeout writing to flash (waiting for second core to halt)\n", op, filename);
        return false;
    }
    else if (status != PICO_OK)
    {
        Log(LOG_ERROR, "%s%s: error %d writing to flash\n", op, filename, status);
        return false;
    }

    // success
    return true;
}

static bool FlashSafeErase(uint32_t flashOffset, size_t size, uint32_t timeout, const char *op, const char *filename)
{
    int status;
    {
        // extend the watchdog timeout - erasing a sector of flash takes about 40ms,
        // so allow that plus an arbitrary additional margin of safety.  We don't want
        // the watchdog to interrupt an erase operation that's slow but not stuck.
        WatchdogTemporaryExtender wte(timeout + 100 + 100*(size + FLASH_SECTOR_SIZE - 1)/FLASH_SECTOR_SIZE);
        uint64_t t0 = time_us_64();

        // do the erase
        status = FlashSafeExecute([flashOffset, size](){ flash_range_erase(flashOffset, size); }, timeout);

        // log statistics
        Log(LOG_DEBUG, "Flash erase (%s%s): %lx, %u bytes, %llu us\n", op, filename, flashOffset, size, time_us_64() - t0);
    }

    // check status
    if (status == PICO_ERROR_TIMEOUT)
    {
        Log(LOG_ERROR, "%s%s: timeout erasing flash sector (waiting for second core to halt)\n", op, filename);
        return false;
    }
    else if (status != PICO_OK)
    {
        Log(LOG_ERROR, "%s%s: error %d erasing flash sector\n", op, filename, status);
        return false;
    }

    // success
    return true;
}

// ---------------------------------------------------------------------------
//
// Flash Storage implementation
//


// construction
FlashStorage::FlashStorage()
{
    // set up our console commands
    CommandConsole::AddCommand(
        "ls", "list files in the flash data storage area",
        "ls [options]\n"
        "options:\n"
        "  -a      list all, including deleted files and special files\n",
        &FlashStorage::Command_ls);
    CommandConsole::AddCommand("fsck", "show flash data storage area status", "fsck (no arguments)", &FlashStorage::Command_fsck);
    CommandConsole::AddCommand(
        "rm", "delete files from the flash storage area",
        "rm [options] <file>...\n"
        "options:\n"
        "  --enable       enable rm commands (required before first use)\n"
        "  -d, --disable  re-disable rm (disabled by default)\n"
        "\n"
        "The --enable option must be specified once, with no other arguments,\n"
        "to enable the command for the current session.  This is meant to help\n"
        "avoid accidental deletions.\n",
        &FlashStorage::Command_rm);
    CommandConsole::AddCommand(
        "flash", "display the byte contents of flash pages",
        "flash [options] <offset> ...\n"
        "options:\n"
        "   -n <number>   display <number> pages at each offset (256 bytes per page)\n"
        "   --sector-map  show a map of allocated/free sectors\n"
        "\n"
        "Each <offset> is a byte offset within the flash, expressed in hexadecimal,\n"
        "giving a starting location to display.\n",
        &FlashStorage::Command_flash);
    
}

// Initialize
void FlashStorage::Initialize()
{
    // only initialize once per session
    if (initialized) return;
    initialized = true;
    
    // Read the flash chip's JEDEC ID register.  This is a standard register
    // that most SPI flash chips implement, and in particular, the W25Q16JVUXIQ
    // used on the reference Pico from Raspberry Pi implements it with a
    // known byte sequence.  This lets us positively identify the standard
    // Pico flash chip, which in turn tells us the exact size of the chip.
    // However, it's not a reliable way to find the size in general, because
    // the only standard field is the first byte, which identifies the
    // manufacturer using a JEDEC-assigned ID number, which is 0xEF for
    // Winbond Serial Flash, manufacturer of the W25Q16JVUXIQ.  For this
    // particular chip, we can recognize the other two bytes as the device
    // identifier and capacity ID (device 0x40, capacity 0x15 = 2^21 = 2MB),
    // but those other bytes are explicitly vendor-defined and thus don't
    // have the same meanings with parts from other manufacturers.  The
    // best we can do is add recognition for specific parts used in other
    // Pico clones as cases come to our attention.  For now, the recognized
    // cases are:
    //
    //     Byte 0  Byte 1  Byte 2  Device                  Capacity
    //     ------  ------  ------  ----------------------  --------
    //     0xEF    0x40    0x15    Winbond W25Q16JVUXIQ    2MB
    //
    FlashSafeExecute([this]()
    {
        // send command 0x9F (Read JDEC ID)
        static const uint8_t READ_JEDEC_ID_CMD = 0x9F;
        static const size_t READ_JEDEC_ID_CMDLEN = 4;
        uint8_t txbuf[READ_JEDEC_ID_CMDLEN] { READ_JEDEC_ID_CMD };
        uint8_t rxbuf[READ_JEDEC_ID_CMDLEN] {0};
        flash_do_cmd(txbuf, rxbuf, READ_JEDEC_ID_CMDLEN);

        // decode the 24 bits from the result
        this->jedecId = { rxbuf[1], rxbuf[2], rxbuf[3] };
    }, 100);

    // check for known chips
    if (jedecId.DeviceIs(0xEF, 0x40))
    {
        // Winbond W25Q16JVUXIQ
        flashSizeBytes = 1 << jedecId.capacityId;
        isFlashSizeKnown = true;
        Log(LOG_INFO, "Flash JEDEC ID recognized as Winbond W25Q16JV-IQ/JQ, capacity %lu bytes\n", flashSizeBytes);
    }
    else
    {
        // Not a device we specifically recognize
        Log(LOG_INFO, "Flash JEDEC ID: Mfg ID %02X, Dev ID %02X, Capacity ID %02X\n",
            jedecId.mfgId, jedecId.devId, jedecId.capacityId);
    }

    // Read the SFDP information, if available.  SFDP is an industry-standard
    // mechanism that most flash chips manufactured after 2020 implement.  It
    // has the one bit of information we'd really like to know, which is the
    // total size of the installed flash.
    // 
    // The official JEDEC specification isn't published.  JEDEC is one of those
    // industry consortia (like USB-IF) that treats its standards as proprietary
    // to its dues-paying industry members.  I guess that's how these consortia
    // earn their dues.  There's a patchwork of information based on the spec
    // published in data sheets for various flash chips, but it's hard to piece
    // together the whole picture from those.  The best documentation I've found
    // on the Web by far is a bit of Chromium OS source code (open-source, BSD
    // license) that lays out the version 1.0 and 1.5 structures in good detail,
    // available here:
    //
    //    https://chromium.googlesource.com/chromiumos/platform/ec/+/master/include/sfdp.h
    //
    
    // read 16/32/64 bits from the SFDP table
    auto ReadSFDP_WORD = [this](uint32_t addr, uint8_t *buf)
    {
        static const uint8_t READ_SFDP_CMD = 0x5A;
        static const uint8_t READ_SFDP_CMDLEN =  7;
        uint8_t txbuf[READ_SFDP_CMDLEN] {
            READ_SFDP_CMD,
            static_cast<uint8_t>((addr >> 16) & 0xFF),
            static_cast<uint8_t>((addr >> 8) & 0xFF),
            static_cast<uint8_t>((addr >> 0) & 0xFF),
            0, 0, 0
        };
        uint8_t rxbuf[READ_SFDP_CMDLEN] { 0 };

        // read the high-order 16 bits
        FlashSafeExecute([&txbuf, &rxbuf](){ flash_do_cmd(txbuf, rxbuf, READ_SFDP_CMDLEN); }, 100);

        // copy back the two bytes
        buf[0] = rxbuf[5];
        buf[1] = rxbuf[6];
    };
    auto ReadSFDP_DWORD = [ReadSFDP_WORD](uint32_t addr, uint8_t *buf) {
        ReadSFDP_WORD(addr, buf);
        ReadSFDP_WORD(addr + 2, buf + 2);
    };
    auto ReadSFDP_QWORD = [ReadSFDP_DWORD](uint32_t addr, uint8_t *buf) {
        ReadSFDP_DWORD(addr, buf);
        ReadSFDP_DWORD(addr + 4, buf + 4);
    };

    // Read the SFDP header fields:
    //
    //   0:3  -> signature, "SFD"
    //   4:7  -> minor_rev, major_rev, num_param_headers, 0xFF
    //
    uint8_t sfdp[8];
    ReadSFDP_QWORD(0, sfdp);

    // check validity
    if (memcmp(sfdp, "SFDP", 4) == 0)
    {
        // get the number of headers; note that the stored value is really the 0-based index
        // of the last header, so "number of headers" is a misnomer, and the actual number
        // of headers is the stored index value plus 1
        int numHeaders = sfdp[6] + 1;

        // scan for the Basic Parameters header, ID=0
        for (int header = 0, ofs = 8 ; header < numHeaders ; ++header, ofs += 8)
        {
            // read the 8-byte header descriptor
            //   0:3  header_id, param_minor_rev, param_major_rev, param_len_dwords
            //   4:7  24_bit_offset_little_endian, 0xFF
            uint8_t hdr[8];
            ReadSFDP_QWORD(ofs, hdr);

            // decode the header
            int paramVsn = hdr[1] | (hdr[2] << 8);
            int paramLen = hdr[3] * 4;
            int paramOfs = hdr[4] + (hdr[5] << 8) + (hdr[6] << 16);

            // Get the param ID.  For version 1.5, this is a two-byte field,
            // with the high byte in position [7].
            int paramID = hdr[0];
            if (paramVsn >= 0x0105)
                paramID |= (hdr[7] << 8);

            // check the parameter ID
            if ((paramVsn == 0x0100 && paramID == 0)
                || (paramVsn >= 0x0105 && paramID == 0xFF00))
            {
                // Basic Parameters header (version 1.0 ID=0, version 1.5 ID=FF00)
                //
                // The only field we're interested in is the density, which tells us
                // the size of the flash.  This is in the second DWORD of the params
                // block.
                if (paramLen >= 8)
                {
                    // read the 2nd DWORD
                    uint8_t param[4];
                    ReadSFDP_DWORD(paramOfs + 4, param);

                    // bit 31 = high-density flag: 1=high, 0=standard
                    // bits 0:31 = N
                    //   high-density      -> capacity = 2^N bits
                    //   standard desnsity -> capacity = N+1 bits
                    uint32_t N = (param[0] | (param[1] << 8) | (param[2] << 16) | (param[3] << 24)) & 0x7FFFFFFF;
                    if ((param[3] & 0x80) != 0)
                        flashSizeBytes = (1ULL << N) / 8;
                    else
                        flashSizeBytes = (N + 1) /8;

                    // we now know the flash size for sure
                    isFlashSizeKnown = true;

                    Log(LOG_INFO, "SFDP basic parameters table found, capacity entry found [4:7]{ %02x %02x %02x %02x }, %llu bytes\n",
                        param[0], param[1], param[2], param[3], flashSizeBytes);
                }
            }
        }
    }
}

// Populate a PinscapePico::FlashFileSysInfo struct, for the Vendor
// Interface file system information query command.
size_t FlashStorage::Populate(PinscapePico::FlashFileSysInfo *info, size_t bufLen)
{
    // make sure there's room
    size_t populatedSize = sizeof(PinscapePico::FlashFileSysInfo);
    if (bufLen < populatedSize)
        return 0;

    // populate the struct
    info->cb = sizeof(PinscapePico::FlashFileSysInfo);
    info->numDirSectors = centralDirectorySize / FLASH_SECTOR_SIZE;
    info->fileSysStartOffset = minAllocOffset;
    info->fileSysByteLength = PICO_FLASH_SIZE_BYTES - minAllocOffset;
    info->flashSizeBytes = flashSizeBytes;
    info->flags = 0;

    // set flags
    if (isFlashSizeKnown) info->flags |= info->F_FLASH_SIZE_KNOWN;

    // return the populated size
    return populatedSize;
}


// mount the file system
bool FlashStorage::Mount(uint32_t centralDirectorySize)
{
    // initialize if we haven't already
    Initialize();
    
    // The central directory size must always be a multiple of the flash
    // sector size.  Round up to the next sector if it's not already a
    // whole multiple.
    centralDirectorySize = ((centralDirectorySize + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    // Get the pointer to the central directory structure.  The directory is
    // aligned at the end of the flash space.
    const uintptr_t XIP_TOP = XIP_BASE + PICO_FLASH_SIZE_BYTES;
    const uintptr_t DIRECTORY_BASE = XIP_TOP - centralDirectorySize;
    const auto *endOfFlash = reinterpret_cast<DirectoryEntry*>(XIP_TOP);
    const auto *dirBase = reinterpret_cast<DirectoryEntry*>(DIRECTORY_BASE);

    // The first file entry is always "/", which is obviously meant to
    // represent the "root folder" conceptually.  But we don't have a
    // concept of subdirectories, so this isn't a true hierarchy control
    // entity the way it might be in a real file system.  It's just a
    // pseudo-entry that we create in the initial empty file system for
    // the sake of integrity checking.
    if (strncmp("/", dirBase->filename, sizeof(dirBase->filename)) != 0)
    {
        Log(LOG_INFO, "Flash file system central directory not found (no '/' entry); formatting\n");
        return Format(centralDirectorySize);
    }

    // Clear the sector-in-use map, and mark the sectors used by the
    // central directory as used.
    sectorsUsed.reset();
    for (int i = 0, idx = PICO_FLASH_SIZE_BYTES/FLASH_SECTOR_SIZE ; i < centralDirectorySize/FLASH_SECTOR_SIZE ; ++i)
        sectorsUsed[--idx] = 1;

    // Extend the watchdog timeout during the scan, since we might have to
    // scan a large amount of flash data (which can be fairly slow to read)
    WatchdogTemporaryExtender wte(100);

    // Check each entry, until we run off the end of flash or encounter
    // an erased entry (with all 0xFF bytes)
    uint32_t seqno = DirectoryEntry::SEQUENCE0;
    int index = 0;
    uint32_t minAllocOffset = DIRECTORY_BASE - XIP_BASE;
    char allFF[sizeof(DirectoryEntry)];
    memset(allFF, 0xFF, sizeof(allFF));
    for (const auto *dir = dirBase ; dir < endOfFlash ; ++dir, ++seqno, index++)
    {
        // if this entry is all 0xFF bytes, it's unused erased space
        // past the last entry, so our scan is complete
        if (memcmp(dir, allFF, sizeof(DirectoryEntry)) == 0)
            break;

        // Check that the entry has the expected sequence number
        if (dir->sequence != seqno)
        {
            Log(LOG_INFO, "Flash file system central directory not found (mismatched stored sequence %08lx for entry %d, expected %08lx); formatting\n",
                dir->sequence, index, seqno);
            return Format(centralDirectorySize);
        }

        // If the entry wasn't replaced with an all-null filename, check the
        // CRC-32.  Replaced entries don't have valid CRC-32's since they're
        // overwritten in place.
        if (!dir->IsReplaced())
        {
            uint32_t crcComputed = dir->CalculateCRC();
            if (crcComputed != dir->crc32)
            {
                Log(LOG_INFO, "Flash file system central directory not found (mismatched stored CRC32 %08x for entry %d, computed %08lx); formatting\n",
                    dir->crc32, index, crcComputed);
                return Format(centralDirectorySize);
            }
        }

        // note the "low-water mark" so far
        if (dir->flashOffset < minAllocOffset)
            minAllocOffset = dir->flashOffset;

        // mark the sectors used
        for (int i = 0, idx = dir->flashOffset/FLASH_SECTOR_SIZE ; i < dir->maxSize/FLASH_SECTOR_SIZE ; ++i, ++idx)
            sectorsUsed[idx] = true;

        // tell the watchdog we're still going
        watchdog_update();
    }

    // All integrity checks passed.  Mark the file system as mounted and
    // return success.
    Log(LOG_INFO, "Flash file system mounted; %u bytes allocated to %d files\n", DIRECTORY_BASE - XIP_BASE - minAllocOffset, index);
    this->centralDirectorySize = centralDirectorySize;
    this->centralDirectory = dirBase;
    this->minAllocOffset = minAllocOffset;
    this->nextSequence = DirectoryEntry::SEQUENCE0 + index;
    return true;
}

// Format a new file system
bool FlashStorage::Format(uint32_t centralDirectorySize)
{
    // The central directory size must always be a multiple of the flash
    // sector size.  Round up to the next sector if it's not already a
    // whole multiple.
    centralDirectorySize = ((centralDirectorySize + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    // Clear the sector-in-use map, and mark the sectors used by the
    // central directory as used.
    sectorsUsed.reset();
    for (int i = 0, idx = PICO_FLASH_SIZE_BYTES/FLASH_SECTOR_SIZE ; i < centralDirectorySize/FLASH_SECTOR_SIZE ; ++i)
        sectorsUsed[--idx] = true;

    // Get the pointer to the central directory structure.  The directory is
    // aligned at the end of the flash space.
    const uintptr_t XIP_TOP = XIP_BASE + PICO_FLASH_SIZE_BYTES;
    const uintptr_t DIRECTORY_BASE = XIP_TOP - centralDirectorySize;
    const auto *endOfFlash = reinterpret_cast<DirectoryEntry*>(XIP_TOP);
    const auto *dirBase = reinterpret_cast<DirectoryEntry*>(DIRECTORY_BASE);

    // Erase the directory structure in flash.  We've already rounded the
    // directory size to a sector size boundary, so we can erase the exact
    // size of the structure.
    FlashSafeErase(DIRECTORY_BASE - XIP_BASE, centralDirectorySize, 100, "Formatting flash file system", "");

    // Fill in the first directory entry with a "/" pseudo-file.  This looks
    // superficially like a "root folder" entry (intentionally), but it's
    // not really, because we don't have any concept of a hierarchy of
    // folders.  This is really just here as a marker that we can recognize
    // on future Mount() operations to indicate that the file system has
    // been formatted previously.
    DirectoryEntry e{
        DirectoryEntry::SEQUENCE0,     // first sequence number
        { '/', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },  // filename '/'
        0,                             // max size is zero
        DIRECTORY_BASE - XIP_BASE,     // allocated file location (none -> at directory base)
        0                              // placeholder for CRC, filled in below
    };
    e.crc32 = e.CalculateCRC();

    // Set up the buffer initially with all 0xFF bytes, so that we leave
    // bytes after the end of this entry as "erased".  Writing a '1' bit to
    // an erased bit leaves that bit erased and available for future
    // writing.
    uint8_t buf[FLASH_PAGE_SIZE];
    memset(buf, 0xFF, FLASH_PAGE_SIZE);

    // copy our struct into the buffer
    memcpy(buf, &e, sizeof(e));

    // program the flash
    FlashSafeWrite(DIRECTORY_BASE - XIP_BASE, buf, sizeof(buf), 100, "Formatting flash file system", "");

    // Make sure we programmed the page successfully
    if (memcmp(dirBase, buf, sizeof(buf)) != 0)
    {
        Log(LOG_ERROR, "Flash file system format failed; directory structure couldn't be written to flash\n");
        return false;
    }

    // Mark the file system as mounted and return success
    this->centralDirectorySize = centralDirectorySize;
    this->centralDirectory = dirBase;
    this->minAllocOffset = DIRECTORY_BASE - XIP_BASE;
    this->nextSequence = DirectoryEntry::SEQUENCE0 + 1;
    return true;
}


// test if a file exists
bool FlashStorage::FileExists(const char *name)
{
    FileInfo fi;
    return FileExists(OpenRead(name, fi));
}

bool FlashStorage::FileExists(OpenStatus stat)
{
    switch (stat)
    {
    case OpenStatus::OK:
        // success - the file definitely exists
        return true;

    case OpenStatus::NotFound:
        // no such file
        return false;
        
    case OpenStatus::BadDirEntry:
    case OpenStatus::BadChecksum:
        // directory entry exists, so report that the file exists, even
        // though it won't be readable due to the error
        return true;

    case OpenStatus::NotMounted:
        // we have no information one way or the other if we can't access
        // the file system at all; we'd really like to return a third state
        // or error code, but all we can do with a bool return is say 'no'
        return false;

    default:
        // unknown case; treat this as not found
        return false;
    }
}

// open for reading
FlashStorage::OpenStatus FlashStorage::OpenRead(const char *name, FileInfo &fi)
{
    // collect timing statistics
    uint64_t t0 = time_us_64();
    
    // fail if not mounted
    if (!IsMounted())
        return OpenStatus::NotMounted;

    // find the file entry
    const auto *dir = FindFileEntry(name, false);
    if (dir == nullptr || !dir->IsAssigned())
    {
        Log(LOG_DEBUG, "File \"%s\" not found in open for read\n", name);
        return OpenStatus::NotFound;
    }

    // get the content info
    bool isDeleted;
    if (!GetFileStream(dir, fi, isDeleted))
    {
        Log(LOG_ERROR, "Opening file \"%s\" for reading: %s\n", name,
            isDeleted ? "file was deleted" : "directory entry is invalid");
        return isDeleted ? OpenStatus::NotFound : OpenStatus::BadDirEntry;
    }

    // Extend the watchdog timeout during the CRC scan, since this might
    // have to scan a large amount of flash memory, which can be fairly
    // slow to read.  Allow an extra 1ms per 2500 bytes, plus some
    // arbitrarly padding.
    WatchdogTemporaryExtender wte(50 + fi.size/2500);

    // check the CRC
    uint32_t crcComputed = CRC::Calculate(fi.data, fi.size, CRC::CRC_32());
    if (crcComputed != fi.crc32)
    {
        Log(LOG_ERROR, "Error opening file \"%s\" for reading: CRC failure (stored %08lx, computed %08lx)\n", name, fi.crc32, crcComputed);
        return OpenStatus::BadChecksum;
    }

    // success
    Log(LOG_DEBUG, "File \"%s\" opened for read, %llu us\n", name, time_us_64() - t0);
    return OpenStatus::OK;
}

// get the content stream for a directory entry
bool FlashStorage::GetFileStream(const DirectoryEntry *dir, FileInfo &fi, bool &isDeleted)
{
    // if the directory entry is invalid, there's no content stream
    isDeleted = false;
    if (!dir->IsAssigned() || dir->IsReplaced() || dir->IsFree())
        return false;

    // make sure the flash offset is within the file stream area of flash
    if (!IsInStreamSpace(dir->flashOffset))
        return false;

    // get the file's content header pointer
    auto const *header = reinterpret_cast<const FileHeader*>(XIP_BASE + dir->flashOffset);

    // if the first sector is all 0xFF bytes, the file has been deleted
    if (IsSectorErased(dir->flashOffset))
    {
        isDeleted = true;
        return false;
    }

    // the file content stream starts immediately after the header
    fi.data = reinterpret_cast<const uint8_t*>(header + 1);
    fi.size = header->fileSize;
    fi.crc32 = header->crc;

    // if the stream size extends past the end of the allocation block, the header
    // is invalid, and so the whole steram must be considered invalid
    if (fi.size > dir->maxSize - sizeof(FileHeader))
        return false;
    
    // Scan forward to find the latest copy of the file.  Each time the
    // file is updated, a new copy can be appended to the storage space
    // after the end of the previous file.  So we have to look for newer
    // copies.
    for (auto const *endp = reinterpret_cast<const uint8_t*>(XIP_BASE + dir->flashOffset + dir->maxSize) ; ; )
    {
        // advance to the next fi.data struct, rounding up for alignment
        auto const *p = fi.data + ((fi.size + 3) & ~3);
        
        // stop if this takes us past the end of the allocated space
        if (p >= endp)
            break;
        
        // If the header is all erased bytes, there's nothing here.  It's
        // enough to check the size field, since this can never by all 0xFF
        // bytes - that would be an impossible file size.
        auto const *hdr = reinterpret_cast<const FileHeader*>(p);
        if (hdr->fileSize == 0xFFFFFFFF)
            break;

        // ensure that the stream ends within the file's allocation block
        const uint8_t *streamp = reinterpret_cast<const uint8_t*>(hdr + 1);
        if (streamp + hdr->fileSize > endp)
        {
            // The stream doesn't fit within the allocation block, so
            // this header is invalid and must be considered corrupted.
            // We could fall back on the previous version of the file at
            // the prior header, but since that file was presumably
            // intended to be overwritten by the newer version, I think
            // it's better to treat the whole file as corrupted.  So
            // return failure.
            return false;
        }

        // This looks like a valid header, so it must be a newer copy of
        // the file appended after the old one.  Advance the returned data
        // to point here.
        fi.data = streamp;
        fi.size = hdr->fileSize;
        fi.crc32 = hdr->crc;

        // still alive
        watchdog_update();
    }

    // success
    return true;
}

bool FlashStorage::Remove(const char *filename, bool silent)
{
    // fail if not mounted
    if (!IsMounted())
        return false;

    // we can't delete the special "/" root entry
    if (strcmp(filename, "/") == 0)
    {
        Log(LOG_ERROR, "Can't remove \"/\": not a file\n", filename);
        return false;
    }

    // find the file entry
    const auto *dir = FindFileEntry(filename, false);
    if (dir == nullptr || !dir->IsAssigned())
    {
        // not found - if in silent mode, this counts as success (the caller got
        // what they wanted, after all: there's no file of this name now!, never
        // mind that there wasn't one before, either)
        if (silent)
            return true;
        
        // log the error and return failure
        Log(LOG_ERROR, "Error removing file \"%s\": file not found\n", filename);
        return false;
    }

    // erase the file's first page
    if (!IsSectorErased(dir->flashOffset)
        && !FlashSafeErase(dir->flashOffset, FLASH_SECTOR_SIZE, 100, "Removing file ", dir->filename))
        return false;

    // success
    return true;
}

// open for reading
int FlashStorage::OpenWrite(const char *name, uint32_t curSize, uint32_t maxSize)
{
    // collect timing statistics
    uint64_t t0 = time_us_64();

    // fail if not mounted
    if (!IsMounted())
        return false;

    // don't allow writing to the special marker file "/"
    if (strcmp(name, "/") == 0)
    {
        Log(LOG_ERROR, "Error opening file \"%s\" for write: this is a reserved filename\n", name);
        return -1;
    }

    // round the requested maximum size up to a sector boundary
    maxSize = ((maxSize + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    // make sure we have a file handle available (but don't actually
    // consume it yet)
    int handle = FindFreeHandle();
    if (handle < 0)
    {
        Log(LOG_ERROR, "Error opening file \"%s\" for write: no file handles available\n", name);
        return -1;
    }

    // find an existing or free file entry
    const auto *dir = FindFileEntry(name, true);

    // If we found an entry, but the maximum size is less than the new
    // requested maximum size, we have to reallocate the file at the new
    // size, by creating a new directory entry for it.
    if (dir != nullptr && dir->IsAssigned() && maxSize > dir->maxSize)
    {
        // Delete the old entry in-place, by overwriting the filename
        // portion with zero bytes.
        DirectoryEntry e;
        memcpy(&e, dir, sizeof(e));
        memset(e.filename, 0, sizeof(e.filename));

        // update the in-flash data
        if (!UpdateFileEntry(dir, &e, "Expanding[1] file ", name))
            return false;

        // Now allocate a new free entry for the file.  We can do this
        // simply by looking up the filename in write mode, since the old
        // entry no longer exists thanks to the erasure we just did.  This
        // will find a free entry for us instead.
        dir = FindFileEntry(name, true);
    }

    // check what we found
    bool isNew = false;
    if (dir == nullptr)
    {
        // not found, no more entries available -> can't create a new file
        Log(LOG_ERROR, "Error creating file \"%s\": no more directory entries available\n", name);
        return -1;
    }
    else if (!dir->IsAssigned())
    {
        // File not found, and we have a free directory entry available.
        // Create a new entry.
        if (!InitFileEntry(dir, name, maxSize))
            return false;

        // this is a new file
        isNew = true;
    }
    else
    {
        // Found it, so overwrite the existing file.  Make sure the
        // requested maximum size is compatible with the existing allocated
        // size.
        if (maxSize > dir->maxSize)
        {
            Log(LOG_ERROR, "Error updating file \"%s\": requested size %u exceeds allocated size %u\n", name, maxSize, dir->maxSize);
            return -1;
        }
    }
    
    // claim the file handle
    auto &wh = writeHandles[handle];
    wh.writeOffset = 0;
    wh.headerOffset = 0;
    wh.bufOffset = 0;
    wh.dirEntry = dir;
    wh.tOpen = t0;

    // If the file previously existed, see if there's room to append a new
    // copy of the file into erased space after the end of the current file.
    // We can only do this if curSize is non-zero - zero means that the size
    // of the file isn't yet known, so we have to assume that the entire
    // allocated space will be used.
    if (!isNew && curSize != 0)
    {
        // get the file stream
        FileInfo fi;
        bool isDeleted = false;
        if (GetFileStream(dir, fi, isDeleted) && !isDeleted)
        {
            // determine how much room is left after this point
            uintptr_t streamOfs = reinterpret_cast<uintptr_t>(fi.data) - XIP_BASE;
            uintptr_t streamEndOfs = streamOfs + fi.size;
            uintptr_t nextHeaderOfs = (streamEndOfs + 3) & ~3;
            uintptr_t allocEndOfs = dir->flashOffset + dir->maxSize;
            uintptr_t bytesFree = nextHeaderOfs < allocEndOfs ? allocEndOfs - nextHeaderOfs : 0;

            // Debugging
            Log(LOG_DEBUG, "OpenWrite: found existing: streamOfs=%lx, streamEndOfs=%lx, nextHeaderOfs=%lx, allocEndOfs=%lx, bytesFree=%lx\n",
                streamOfs, streamEndOfs, nextHeaderOfs, allocEndOfs, bytesFree);

            // if there's enough left, use append mode
            if (bytesFree >= curSize + sizeof(FileHeader))
            {
                // Debugging
                Log(LOG_DEBUG, "OpenWrite: trying append\n");

                // Find sector containing the new file header
                uint32_t nextHeaderSector = (nextHeaderOfs / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

                // We can proceed with the append if (a) the new header is positioned
                // exactly at the start of a sector, or (b) the rest of the sector,
                // starting at the location of the new header, has already been erased
                // (all set to 0xFF bytes, which we can safely write into).  It's always
                // okay to start on a sector boundary, because the buffer flush routine
                // can erase the whole new sector as it enters it.  It's also okay to
                // continue into a sector where the remainder is all 0xFF bytes, since
                // that region is writable by virtue of the 0xFF bytes and the
                // write-towards-zero property of the physical media.
                //
                // But it's not possible to start in the middle of a sector that hasn't
                // already been erased.  We can't overwrite non-erased bytes without
                // first erasing them, and the media controller doesn't allow us to
                // erase just part of a sector - we'd have to erase the entire sector
                // containing the new header, including existing data earlier in the
                // sector, which would wipe out that prior data.
                //
                // If we find ourselves in that situation (middle of a non-erased
                // sector), it must mean that a previous attempt to append to the file
                // failed without closing the file and updating the file header.  The
                // easiest thing to do is just erase the whole existing stream list and
                // start over at the start of the allocation block.  There is one thing
                // we can try first, though: if we have enough memory available, we can
                // save the current sector contents up to the new header position, erase
                // the whole sector, and write back the part up to the new header.
                if (!(nextHeaderOfs == nextHeaderSector
                      || IsFlashErased(nextHeaderOfs, nextHeaderSector + FLASH_SECTOR_SIZE - nextHeaderOfs)))
                {
                    // Debugging
                    Log(LOG_DEBUG, "OpenWrite: dirty sector for append, trying erase-and-restore\n");

                    // Try to allocate space to save the old contents.  Since the writer can only
                    // work in whole page units, round the allocation up to a page boundary.
                    size_t oldContentsSize = nextHeaderOfs - nextHeaderSector;
                    size_t oldContentsBufSize = (oldContentsSize + FLASH_PAGE_SIZE - 1)/FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
                    std::unique_ptr<uint8_t> oldContents(new (std::nothrow) uint8_t[oldContentsBufSize]);
                    if (oldContents != nullptr)
                    {
                        // save the old contents
                        memcpy(oldContents.get(), reinterpret_cast<const void*>(nextHeaderSector + XIP_BASE), oldContentsSize);

                        // clear any remainder to "erased" 0xFF bytes
                        if (oldContentsBufSize > oldContentsSize)
                            memset(oldContents.get() + oldContentsSize, 0xFF, oldContentsBufSize - oldContentsSize);

                        // try erasing the sector
                        if (FlashSafeErase(nextHeaderSector, FLASH_SECTOR_SIZE, 100, "Opening (for write) file ", name))
                        {
                            // success - copy back the original contents
                            FlashSafeWrite(nextHeaderSector, oldContents.get(), oldContentsBufSize, 100, "Opening (for write) file ", name);

                            // Debugging
                            Log(LOG_DEBUG, "OpenWrite: dirty sector for append, successful erase-and-restore\n");
                        }
                    }
                }

                // Verify that we have the writable-rest-of-sector conditions described
                // above.  If so, proceed with the append operation.
                if (nextHeaderOfs == nextHeaderSector
                    || IsFlashErased(nextHeaderOfs, nextHeaderSector + FLASH_SECTOR_SIZE - nextHeaderOfs))
                {
                    // We have room, and the rest of the sector has been erased, so
                    // we can append the new file into the existing space without
                    // erasing the existing storage.
                    
                    // find the flash page containing the start of the header
                    uint32_t nextHeaderPage = (nextHeaderOfs / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
                    
                    // fill the buffer with 0xFF bytes, so that the part preceding
                    // the header won't overwrite existing data when flushed
                    memset(wh.buf, 0xFF, sizeof(wh.buf));
                    
                    // Figure the starting write offset.  The write offset is where the
                    // buffer will go when flushed, and this must always be
                    // page-aligned, so start at the beginning of the page containing
                    // the first byte of the header.  The value stored is relative to
                    // the allocation unit for the file.
                    wh.writeOffset = nextHeaderPage - dir->flashOffset;

                    // figure the starting buffer offset as the offset in the page
                    wh.bufOffset = nextHeaderOfs - nextHeaderPage;

                    // remember the header offset (from the start of the allocation block)
                    wh.headerOffset = nextHeaderOfs - dir->flashOffset;

                    // Debugging
                    Log(LOG_DEBUG, "OpenWrite: append mode OK, writeOffset=%lx, headerOffset=%lx, bufOffset=%lx\n",
                        wh.writeOffset, wh.headerOffset, wh.bufOffset);
                }
            }
        }
    }

    // Populate the file header struct with 0xFF bytes.  This allows us to
    // go back and patch the header when we finish writing the file, since
    // we can always overwrite 0xFF bytes due to the write-towards-zero
    // property of the physical media.
    FileHeader fhTemp;
    memset(&fhTemp, 0xFF, sizeof(fhTemp));
    if (!WriteFile(handle, &fhTemp, sizeof(fhTemp)))
    {
        wh.dirEntry = nullptr;
        return -1;
    }

    // success - return the new handle
    Log(LOG_DEBUG, "File \"%s\" opened for write, %llu us\n", name, time_us_64() - t0);
    return handle;
}

// Create a directory entry for a new file
bool FlashStorage::InitFileEntry(const DirectoryEntry *dir, const char *name, uint32_t maxSize)
{
    // Search for free space above the minimum allocation point
    uint32_t allocOffset = 0;
    uint32_t sectorsNeeded = (maxSize + FLASH_SECTOR_SIZE - 1)/FLASH_SECTOR_SIZE;
    for (uint32_t sector = minAllocOffset/FLASH_SECTOR_SIZE, endSector = (PICO_FLASH_SIZE_BYTES - centralDirectorySize)/FLASH_SECTOR_SIZE ;
         sector < endSector; ++sector)
    {
        // if this sector is free, check for enough free sectors
        if (!sectorsUsed[sector])
        {
            // scan for 'sectorsNeeded' consecutive free sectors
            bool ok = true;
            for (uint32_t i = 0 ; i < sectorsNeeded ; ++i)
            {
                // stop if this sector is in use
                if (sectorsUsed[sector + i])
                {
                    // no good
                    ok = false;

                    // since we now know this sector is in use, we can skip
                    // ahead to this sector in the outer loop - there's no
                    // need to check again at each point along the way
                    sector += i;

                    // stop searching
                    break;
                }
            }

            // if we found space, use this location
            if (ok)
            {
                allocOffset = sector * FLASH_SECTOR_SIZE;
                break;
            }
        }
    }

    // If we couldn't find fragmented free space, allocate free space at
    // the bottom of the current allocation area.
    if (allocOffset == 0)
    {
        // Make sure there's space for the requested file size, given the
        // current program image size.  This doesn't guarantee that this
        // space will still be available in future firmware updates, since
        // the program could grow, but it's at least a sanity check that
        // things aren't growing out of control.
        extern char __flash_binary_end;
        const uintptr_t PROGRAM_IMAGE_END_OFFSET = reinterpret_cast<uintptr_t>(&__flash_binary_end) - XIP_BASE;
        if (minAllocOffset < PROGRAM_IMAGE_END_OFFSET || minAllocOffset - PROGRAM_IMAGE_END_OFFSET < maxSize)
        {
            Log(LOG_ERROR, "Error creating file \"%s\": insufficient flash space available above program image "
                "(%lu bytes requested, %lu bytes available)\n", maxSize, minAllocOffset - PROGRAM_IMAGE_END_OFFSET);
            return -1;
        }
        
        // Allocate the requested space at the bottom of our claimed area
        minAllocOffset -= maxSize;
        allocOffset = minAllocOffset;
    }
        
    // mark the sectors used
    for (int i = 0, idx = allocOffset/FLASH_SECTOR_SIZE ; i < maxSize/FLASH_SECTOR_SIZE ; ++i, ++idx)
        sectorsUsed[idx] = true;

    // Set up the new directory entry
    DirectoryEntry e;
    strncpy(e.filename, name, sizeof(e.filename));
    e.maxSize = maxSize;
    e.flashOffset = allocOffset;
    e.crc32 = 0xFFFFFFFF;

    // Set the sequence number ONLY IF it's not already set in the entry.
    // If we're using a reclaimed entry, it will already have a sequence
    // number, so don't overwrite it.
    if (dir->sequence == 0xFFFFFFFF)
    {
        // set the sequence
        e.sequence = nextSequence++;

        // set the CRC32
        e.crc32 = e.CalculateCRC();
    }

    // update it
    return UpdateFileEntry(dir, &e, "Creating file ", name);
}

bool FlashStorage::UpdateFileEntry(const DirectoryEntry *flashPtr, const DirectoryEntry *newData, const char *op, const char *filename)
{
    // Find the base of the flash page containing the struct
    uintptr_t dirFlashOffset = reinterpret_cast<uint32_t>(flashPtr) - XIP_BASE;
    uintptr_t dirFlashPageBase = (dirFlashOffset / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;

    // copy the existing page into a local buffer
    uint8_t buf[FLASH_PAGE_SIZE];
    memcpy(buf, reinterpret_cast<uint8_t*>(dirFlashPageBase + XIP_BASE), sizeof(buf));

    // copy the new file struct into its place within the flash page
    memcpy(buf + (dirFlashOffset - dirFlashPageBase), newData, sizeof(DirectoryEntry));

    // write the page
    if (!FlashSafeWrite(dirFlashPageBase, buf, sizeof(buf), 100, op, filename))
        return false;

    // success
    return true;
}

// Write to an open file
bool FlashStorage::WriteFile(int handle, const void *data, size_t len)
{
    // collect timing statistics
    uint64_t t0 = time_us_64();

    // validate the handle
    if (handle < 0 || handle >= _countof(writeHandles) || writeHandles[handle].dirEntry == nullptr)
    {
        Log(LOG_ERROR, "WriteFile: invalid handle %d\n", handle);
        return false;
    }
    
    // buffer data until we've satisfied the request
    auto &wh = writeHandles[handle];
    const uint8_t *src = reinterpret_cast<const uint8_t*>(data);
    for (size_t rem = len ; rem != 0 ; )
    {
        // flush the buffer if applicable
        if (wh.bufOffset == sizeof(wh.buf))
        {
            if (!wh.Flush())
                return false;
        }

        // buffer as much as possible
        size_t copySize = std::min(rem, sizeof(wh.buf) - wh.bufOffset);
        memcpy(&wh.buf[wh.bufOffset], src, copySize);

        // bump counters by the copied size
        src += copySize;
        rem -= copySize;
        wh.bufOffset += copySize;

        // make sure we don't time out the watchdog while we're still working
        watchdog_update();
    }

    // success
    Log(LOG_DEBUG, "WriteFile \"%s\", %u bytes written, %llu us\n", wh.dirEntry->filename, len, time_us_64() - t0);
    return true;
}

// Close a file opened for writing
bool FlashStorage::CloseWrite(int handle)
{
    // collect timing statistics
    uint64_t t0 = time_us_64();

    // validate the handle
    if (handle < 0 || handle >= _countof(writeHandles) || writeHandles[handle].dirEntry == nullptr)
    {
        Log(LOG_ERROR, "CloseFile: invalid handle %d\n", handle);
        return false;
    }

    // Calculate the final stream size.  This is the size of the committed
    // data (writeOffset) plus the remaining buffered data, minus the size
    // of the file header.  The header is stored in the raw stream, so the
    // write offset is relative to the start of the header, but the stored
    // file size only includes the size of the caller's payload.
    auto &wh = writeHandles[handle];
    const char *filename = wh.dirEntry->filename;
    uint32_t streamSize = wh.writeOffset + wh.bufOffset - wh.headerOffset - sizeof(FileHeader);

    // flush the write buffer
    bool ok = wh.Flush();

    // If this leaves the write offset at a sector boundary, and the next
    // sector is within the file's allocated region, erase the next sector.
    // This is required when the stream ends on the sector boundary, since
    // the next file-open operation will attempt to follow the header list
    // into the new sector.  The sector must be erased to detect the end
    // of the header list there.
    if ((wh.writeOffset % FLASH_SECTOR_SIZE) == 0 && wh.writeOffset < wh.dirEntry->maxSize)
    {
        if (!FlashSafeErase(wh.dirEntry->flashOffset + wh.writeOffset, FLASH_SECTOR_SIZE, 100, "Writing file ", filename))
            ok = false;
    }

    // get a pointer to the file header and content stream in flash
    uint32_t flashHeaderOfs = wh.dirEntry->flashOffset + wh.headerOffset;
    const auto *fileHeaderPtr = reinterpret_cast<const FileHeader*>(flashHeaderOfs + XIP_BASE);
    const auto *streamPtr = reinterpret_cast<const uint8_t*>(fileHeaderPtr + 1);

    // figure the offset of the start of the flash page containing the header
    uint32_t flashHeaderPage = (flashHeaderOfs/FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;

    // calculate the checksum of the stream contents
    uint32_t crc = CRC::Calculate(streamPtr, streamSize, CRC::CRC_32());    

    // set up the final file header
    FileHeader newHeader{ streamSize, crc };
    
    // Fill a two-page buffer with '1' bits, so that we don't change parts
    // of the page we don't need to (writing a '1' bit has no effect due to
    // the write-towards-zero property of the flash media).  We set up a
    // buffer big enough for two pages because the file header can span a
    // page boundary.
    uint8_t buf[FLASH_PAGE_SIZE*2];
    memset(buf, 0xFF, sizeof(buf));
    
    // patch the new header into its spot in the buffer
    memcpy(&buf[flashHeaderOfs - flashHeaderPage], &newHeader, sizeof(newHeader));

    // if the header fits into the first page, we only need to write one
    // page from the buffer to flash; otherwise we need to write both
    uint32_t writeSize = (flashHeaderOfs - flashHeaderPage + sizeof(FileHeader) > FLASH_PAGE_SIZE) ?
        FLASH_PAGE_SIZE*2 : FLASH_PAGE_SIZE;

    // Debugging
    Log(LOG_DEBUG, "CloseWrite, flashHeaderOfs=%lx, flashHeaderPage=%lx, writeSize=%lu\n",
        flashHeaderOfs, flashHeaderPage, writeSize);

    // Rewrite the page to set the final header.  Note that this is only
    // possible because we were careful to leave the header with the
    // "erased" value of all '1' bits (0xFF bytes) when we previously wrote
    // this page.  Writing 0xFF bytes to erased flash space doesn't actually
    // change anything, so we can keep rewriting a section as long as it's
    // still set to 0xFF bytes.
    if (!FlashSafeWrite(flashHeaderPage, buf, writeSize, 100, "Writing file ", filename))
        ok = false;

    // release the handle by marking the directory entry as null
    wh.dirEntry = nullptr;

    // Debugging - log the timing
    uint64_t now = time_us_64();
    Log(LOG_DEBUG, "CloseWrite \"%s\", %llu us in close, %llu us since open\n",
        filename, now - t0, now - wh.tOpen);

    // return the result
    return ok;
}

// flush a file handle buffer
bool FlashStorage::WriteHandle::Flush()
{
    // there's nothing to do if the buffer is empty
    if (bufOffset == 0)
        return true;
    
    // make sure this doesn't take us over the file limit
    if (writeOffset + bufOffset > dirEntry->maxSize)
    {
        Log(LOG_ERROR, "Writing file \"%s\": maximum size %d exceeded\n", dirEntry->filename, dirEntry->maxSize);
        return false;
    }

    // fill in any unused portion of the buffer with 0xFF bytes,
    // so that we don't un-erase any data that hasn't already been
    // written
    if (bufOffset < sizeof(buf))
        memset(&buf[bufOffset], 0xFF, sizeof(buf) - bufOffset);

    // If we're entering a sector, and the sector hasn't already been erased, erase it.
    // We're entering a sector if the write offset is exactly on a sector boundary, AND
    // the file itself didn't start at a non-zero offset within this sector.  It's
    // possible for the file to start AFTER the first buffer's write pointer, because we
    // could be appending the file to existing data in the same sector.
    uint32_t writeOffsetInFlash = dirEntry->flashOffset + writeOffset;
    uint32_t headerOffsetInFlash = dirEntry->flashOffset + headerOffset;
    if ((writeOffset % FLASH_SECTOR_SIZE) == 0 && headerOffsetInFlash <= writeOffsetInFlash && !IsSectorErased(writeOffsetInFlash))
    {
        if (!FlashSafeErase(writeOffsetInFlash, FLASH_SECTOR_SIZE, 100, "Writing file ", dirEntry->filename))
            return false;
    }

    // write out the page
    if (!FlashSafeWrite(dirEntry->flashOffset + writeOffset, buf, sizeof(buf), 100, "Writing file ", dirEntry->filename))
        return false;

    // bump the write pointer by the buffer size, and reset the buffer pointer
    writeOffset += sizeof(buf);
    bufOffset = 0;

    // success
    return true;
}


// look up a file by name
const FlashStorage::DirectoryEntry *FlashStorage::FindFileEntry(const char *name, bool forWriting)
{
    // fail if not mounted
    if (!IsMounted())
        return nullptr;

    // The central directory scan might have to read a lot of flash data,
    // which can be failry slow.  Allow 1ms per 2500 bytes of scanning, plus
    // some arbitrary extra padding for safety.
    WatchdogTemporaryExtender wte(50 + centralDirectorySize/2500);

    // We might have to do the scan twice: if the first pass doesn't find
    // any free entries, we'll try rebuilding the directory, after which
    // we can do another pass to see if there are now any free entries.
    for (int pass = 0 ; pass < 2 ; ++pass)
    {
        // scan the central directory for a name match
        const uintptr_t XIP_TOP = XIP_BASE + PICO_FLASH_SIZE_BYTES;
        const auto *endOfFlash = reinterpret_cast<DirectoryEntry*>(XIP_TOP);
        const auto *dir = centralDirectory;
        const DirectoryEntry *firstFree = nullptr;
        for ( ; dir < endOfFlash ; ++dir)
        {
            // if the name matches, return this entry
            if (strncmp(name, dir->filename, sizeof(dir->filename)) == 0)
                return dir;

            // remember the first free entry, in case we're in write mode
            if (firstFree == nullptr && dir->IsFree())
                firstFree = dir;
        }

        // tell the watchdog we're still alive
        watchdog_update();

        // We didn't find an existing entry with this filename.  If we're in
        // write mode, the caller wants a free entry if we can't find an
        // existing one.
        if (forWriting)
        {
            // if we found a free entry, return the first free
            if (firstFree != nullptr)
                return firstFree;

            // Try rebuilding the central directory, to see if we can free
            // up any unused entries (removed entries or deleted files).  If
            // that fails, or if it doesn't make any changes, there's no
            // hope of finding a new free entry on a second pass, so just
            // return null now.
            bool modified = false;
            if (!RebuildCentralDirectory(modified) || !modified)
                return nullptr;

            // We made changes in the rebuild, so we presumably freed up some
            // entries - go back for another scan to find one.
            continue;
        }

        // Not found - return null
        return nullptr;
    }

    // not found - return null
    return nullptr;
}

// Rebuild the directory, by removing unused entries
bool FlashStorage::RebuildCentralDirectory(bool &modified)
{
    // persume we won't make any changes
    modified = false;
    
    // we need memory to hold an in-memory copy of the sector
    std::unique_ptr<uint8_t> buf(new (std::nothrow) uint8_t[FLASH_SECTOR_SIZE]);
    if (buf == nullptr)
    {
        Log(LOG_ERROR, "Insufficient memory for flash central directory rebuild\n");
        return false;
    }

    // scan each directory sector
    for (uint32_t flashOffset = PICO_FLASH_SIZE_BYTES - centralDirectorySize ;
         flashOffset < PICO_FLASH_SIZE_BYTES ; flashOffset += FLASH_SECTOR_SIZE)
    {
        // extend the watchdog timer
        WatchdogTemporaryExtender wte(100);

        // make an in-memory copy of the sector
        memcpy(buf.get(), reinterpret_cast<const uint8_t*>(XIP_BASE + flashOffset), FLASH_SECTOR_SIZE);
        watchdog_update();

        // scan for entries we can reclaim
        auto *p = reinterpret_cast<DirectoryEntry*>(buf.get());
        int nReclaimed = 0;
        for (int i = 0 ; i < FLASH_SECTOR_SIZE / sizeof(DirectoryEntry) ; ++i, ++p)
        {
            // we can reclaim a replaced entry or a deleted file
            if (p->IsReplaced() || p->IsDeleted())
            {
                // Replaced or deleted - we can reclaim this entry.
                // Mark the sectors as free, since they're no longer
                // referenced from the directory structure.
                for (uint32_t i = p->flashOffset, s = i/FLASH_SECTOR_SIZE ; i < p->flashOffset + p->maxSize ; i += FLASH_SECTOR_SIZE, ++s)
                    sectorsUsed[s] = false;
                
                // Leave the sequence number intact and mark the other
                // fields as erased by setting them to FF bytes.  The FF
                // bytes allow this to be overwritten in place later by
                // an incremental page write.
                memset(p->filename, 0xFF, sizeof(p->filename));
                p->maxSize = 0xFFFFFFFF;
                p->flashOffset = 0xFFFFFFFF;
                p->crc32 = 0xFFFFFFFF;

                // count the reclaimed entry
                nReclaimed += 1;
            }

            // If we found any entries to reclaim, save the updated
            // directory back to flash.  There's no need to save it if we
            // didn't reclaim any entries, since we won't have made any
            // changes in that case.
            if (nReclaimed != 0)
            {
                // erase the sector
                if (!FlashSafeErase(flashOffset, FLASH_SECTOR_SIZE, 100, "Rebuilding central directory", "")
                    || !FlashSafeWrite(flashOffset, buf.get(), FLASH_SECTOR_SIZE, 100, "Rebuilding central directory", ""))
                    return false;

                // we made changes to the directory structure
                modified = true;
            }
        }
    }

    // success
    return true;
}

bool FlashStorage::IsFlashErased(const void *pv, uint32_t nBytes)
{
    // allow some extra time for the flash scan
    WatchdogTemporaryExtender wte(50 + nBytes/2500);

    // check to see if all bytes are erased (all '1' bits, byte value 0xFF)
    const auto *p = reinterpret_cast<const uint8_t*>(pv);
    for (uint32_t i = 0 ; i < nBytes ; ++i, ++p)
    {
        if (*p != 0xFF)
            return false;
    }

    // all bytes are erased/writable
    return true;
}

bool FlashStorage::IsFlashErased(uint32_t startOfs, uint32_t nBytes)
{
    return IsFlashErased(reinterpret_cast<const void*>(startOfs + XIP_BASE), nBytes);
}

// check if a sector has been erased
bool FlashStorage::IsSectorErased(const void *sectorPtr)
{
    return IsFlashErased(sectorPtr, FLASH_SECTOR_SIZE);
}

bool FlashStorage::IsSectorErased(uint32_t sectorOfs)
{
    return IsFlashErased(reinterpret_cast<const void*>(sectorOfs + XIP_BASE), FLASH_SECTOR_SIZE);
}

// get a free write handle
int FlashStorage::FindFreeHandle()
{
    // search for a write handle with no associated directory entry
    for (int i = 0 ; i < _countof(writeHandles) ; ++i)
    {
        if (writeHandles[i].dirEntry == nullptr)
            return i;
    }

    // no handles available
    return -1;
}

uint32_t FlashStorage::DirectoryEntry::CalculateCRC() const
{
    return CRC::Calculate(this, reinterpret_cast<size_t>(&this->crc32) - reinterpret_cast<size_t>(this), CRC::CRC_32());
}

bool FlashStorage::DirectoryEntry::IsDeleted() const
{
    return flashOffset != 0 && reinterpret_cast<FileHeader*>(flashOffset + XIP_BASE)->fileSize == 0xFFFFFFFF;
}


// ---------------------------------------------------------------------------
//
// Console command handlers
//

void FlashStorage::Command_fsck(const ConsoleCommandContext *c)
{
    // no arguments
    if (c->argc != 1)
        return c->Usage();
    
    if (flashStorage.IsMounted())
    {
        // get the end of the directory area
        const uintptr_t XIP_TOP = XIP_BASE + PICO_FLASH_SIZE_BYTES;
        const uintptr_t DIRECTORY_BASE = XIP_TOP - flashStorage.centralDirectorySize;
        const auto *dirEnd = reinterpret_cast<const DirectoryEntry*>(XIP_TOP);

        // get the offset in flash of the program image end
        extern char __flash_binary_end;
        const uintptr_t PROGRAM_IMAGE_END_OFFSET = reinterpret_cast<uintptr_t>(&__flash_binary_end) - XIP_BASE;

        // scan for the first free entry to determine how many files are allocated
        int nFiles = 0;
        for (const auto *dir = flashStorage.centralDirectory ; dir < dirEnd && dir->IsAssigned() ; ++dir, ++nFiles) ;

        // show the statistics
        NumberFormatter<80> nf;
        c->Printf(
            "Flash file system status:\n"
            "  Mounted:                      Yes\n"
            "  Number of files:              %u\n"
            "  Total space allocated:        %s bytes\n"
            "  Boot image size:              %s bytes\n"
            "  Free space above boot image:  %s bytes\n",
            nFiles,
            nf.Format("%lu", DIRECTORY_BASE - XIP_BASE - flashStorage.minAllocOffset),
            nf.Format("%lu", PROGRAM_IMAGE_END_OFFSET),
            nf.Format("%lu", flashStorage.minAllocOffset - PROGRAM_IMAGE_END_OFFSET));
    }
    else
    {
        c->Printf(
            "Flash file system status:\n"
            "  Mounted:                      NO - Check startup log for errors\n");
    }
}

void FlashStorage::Command_ls(const ConsoleCommandContext *c)
{
    // do nothing if not mounted
    if (!flashStorage.IsMounted())
        return c->Printf("ls: file system not mounted\n");

    // the listing might take some time
    WatchdogTemporaryExtender wte(1000);

    // check options
    bool listAll = false;
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (strcmp(a, "-a") == 0)
            listAll = true;
        else if (a[0] == '-')
            return c->Printf("ls: unknown option \"%s\"\n", a);
        else
            return c->Usage();
    }

    // get the end of the directory area
    const uintptr_t XIP_TOP = XIP_BASE + PICO_FLASH_SIZE_BYTES;
    const auto *dirEnd = reinterpret_cast<const DirectoryEntry*>(XIP_TOP);

    // scan files
    int nFiles = 0, nListed = 0;
    uint32_t totalSize = 0;
    uint32_t allocSize = 0;
    c->Printf(
        "    Name          StrmAddr  StrmSize  Blk Addr  Blk Size   CRC-32   Status\n"
        "----------------  --------  --------  --------  --------  --------  ------\n");
    for (const auto *dir = flashStorage.centralDirectory ; dir < dirEnd && dir->IsAssigned() ; ++dir, ++nFiles)
    {
        // check for special files
        const char *special = nullptr;
        if (nFiles == 0)
            special = "Special";

        // skip special files unless in List All mode
        if (!listAll && special != nullptr)
            continue;

        // check for special entries - deleted, replaced, free
        bool isDeleted = dir->IsDeleted();
        bool isFree = dir->IsFree();
        bool isReplaced = dir->IsReplaced();

        // if it's an active file, open it to get the size and CRC
        size_t streamSize = 0;
        uint32_t streamOfs = 0;
        bool isValid = false;
        uint32_t crc = 0;
        if (!isDeleted && !isFree && !isReplaced)
        {
            FileInfo fi;
            bool isDeleted;
            if (flashStorage.GetFileStream(dir, fi, isDeleted) && !isDeleted)
            {
                // check the CRC if it's an active file and the size is within range
                uint32_t crcCalc = CRC::Calculate(fi.data, fi.size, CRC::CRC_32());
                isValid = (crcCalc == fi.crc32);
                crc = fi.crc32;
                streamOfs = reinterpret_cast<uintptr_t>(fi.data) - XIP_BASE;
                streamSize = fi.size;
            }
        }

        // skip deleted/free/replaced files if not in List All mode
        if (!listAll && (isDeleted || isFree || isReplaced))
            continue;

        // count the file and size
        nListed += 1;
        allocSize += dir->maxSize;
        if (isValid)
            totalSize += streamSize;
        
        // show the status
        c->Printf(
            "%-16.16s  %08lx  %8d  %08lx  %8d  %08lx  %s\n",
            (isFree || isReplaced) ? "" : dir->filename,
            streamOfs,
            streamSize,
            dir->flashOffset,
            dir->maxSize,
            crc,
            
            special != nullptr ? special :
            isDeleted ? "Deleted" :
            isReplaced ? "Replaced" :
            isFree ? "Free" :
            isValid ? "OK" : "Bad CRC");
    }

    // show a summary
    c->Printf("\n%lu bytes used (%lu bytes allocated) in %d files\n", totalSize, allocSize, nListed);
    if (nFiles > nListed)
        c->Printf("%d non-file entries not listed (use ls -a to show all)\n", nFiles - nListed);
}

void FlashStorage::Command_rm(const ConsoleCommandContext *c)
{
    // do nothing if not mounted
    if (!flashStorage.IsMounted())
        return c->Printf("rm: file system not mounted\n");

    // show usage if no arguments
    if (c->argc == 1)
        return c->Usage();

    // command must be enabled before use
    static bool enabled = false;

    // check for --enable, which must be used with no other options
    if (c->argc == 2 && strcmp(c->argv[1], "--enable") == 0)
    {
        enabled = true;
        c->Print("rm is now enabled for this session.  Deleting a file will\n"
                 "remove the associated configuration settings and reset them\n"
                 "to factory defaults.  In most cases, settings are loaded only\n"
                 "during initialization, so the factory reset won't take effect\n"
                 "until the Pico is rebooted.  Use with caution.\n");
        return;
    }

    // abort if not enabled
    if (!enabled)
    {
        c->Print("rm is currently disabled; use 'rm --enable' to enable it\n");
        return;
    }

    // process arguments
    for (int i = 1 ; i < c->argc ; ++i)
    {
        // check for -d/--disable
        const char *f = c->argv[i];
        if (strcmp(f, "-d") == 0 || strcmp(f, "--disable") == 0)
        {
            enabled = false;
            c->Print("rm is now disabled; use 'rm --enable' to re-enable it\n");
            return;
        }

        // other '-' options are invalid
        if (f[0] == '-')
            return c->Usage();

        // try deleting the file
        WatchdogTemporaryExtender wte(1000);
        if (const auto *dir = flashStorage.FindFileEntry(f, false) ; dir == nullptr || !dir->IsAssigned())
            c->Printf("%s: not found\n", f);
        else if (flashStorage.Remove(f, false))
            c->Printf("%s: deleted OK\n", f);
        else
            c->Printf("%s: delete failed\n", f);
    }
}

void FlashStorage::Command_flash(const ConsoleCommandContext *c)
{
    // the listing could take a while, just because of the large amount of text displayed
    WatchdogTemporaryExtender wde(3000);

    // make sure we have some arguments
    if (c->argc <= 1)
        return c->Usage();

    // parse the options and address list
    int n = 1;
    for (int i = 1 ; i < c->argc ; ++i)
    {
        const char *a = c->argv[i];
        if (a[0] == '-')
        {
            if (strcmp(a, "-n") == 0)
            {
                if (i + 1 >= c->argc)
                    return c->Printf("flash: missing argument for -n\n");

                n = atoi(c->argv[++i]);
                if (n < 1 || n > 16)
                {
                    c->Printf("flash: -n out range (must be 1..16); defaulting to 1\n");
                    n = 1;
                }
            }
            else if (strcmp(a, "--sector-map") == 0)
            {
                c->Printf("In-use sector map [C=control, D=data, P=program, '.'=free):\n");
                for (int i = 0 ; i < 512 ; )
                {
                    const int nSectors = PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE;
                    const int nCentralDirSectors = flashStorage.centralDirectorySize / FLASH_SECTOR_SIZE;
                    extern char __flash_binary_end;
                    const uintptr_t PROGRAM_IMAGE_END_OFFSET = reinterpret_cast<uintptr_t>(&__flash_binary_end) - XIP_BASE;
                    const int nProgramSectors = (PROGRAM_IMAGE_END_OFFSET + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;

                    char buf[65];
                    for (int j = 0 ; j < 64 ; ++j, ++i)
                    {
                        if (i >= nSectors - nCentralDirSectors)
                            buf[j] = 'C';
                        else if (flashStorage.sectorsUsed[i])
                            buf[j] = 'D';
                        else if (i < nProgramSectors)
                            buf[j] = 'P';
                        else
                            buf[j] = '.';
                    }
                    buf[64] = 0;
                    c->Printf("   %03x  %s\n", i - 64, buf);
                }
                c->Printf("\n");
                watchdog_update();
            }
            else
                return c->Usage();
        }
        else
        {
            // interpret it as a hex number
            uint32_t addr = strtol(a, nullptr, 16);
            if (addr > PICO_FLASH_SIZE_BYTES)
                return c->Printf("flash: starting address %08lx out of range (must be 0 to %08lx)\n", addr, PICO_FLASH_SIZE_BYTES);
                    
            const auto *p = reinterpret_cast<const uint8_t*>(XIP_BASE + addr);
            const auto *endp = reinterpret_cast<const uint8_t*>(XIP_BASE + PICO_FLASH_SIZE_BYTES);
            const int nBytes = n * FLASH_PAGE_SIZE;
            for (int i = 0 ; i < nBytes && p < endp ; i += 32, p += 32)
            {
                c->Printf("%08x ", addr + i);
                for (int j = 0 ; j < 32 ; ++j)
                    c->Printf(" %02x", p[j]);

                c->Printf("  ");
                for (int j = 0 ; j < 32 ; ++j)
                {
                    uint8_t b = p[j];
                    c->Printf("%c", b >= 32 && b < 127 ? b : '.');
                }

                c->Printf("\n");
            }
            c->Printf("\n");
            watchdog_update();
        }
    }
}

// ----------------------------------------------------------------------------
//
// Virtual RAM File
//

int FlashStorage::RAMFile::Write(size_t ofs, const void *vsrc, size_t len)
{
    // there's nothing to do if the write size is zero
    if (len == 0)
        return 0;

    // get a byte pointer
    auto *src = reinterpret_cast<const uint8_t*>(vsrc);

    // figure the page number containing the last byte we're writing
    size_t lastByteOfs = ofs + len - 1;
    size_t lastPageNo = lastByteOfs / PAGE_SIZE;

    // if the page array isn't big enough, expand it
    if (pages.size() < lastPageNo + 1)
        pages.resize(lastPageNo + 1);

    // the storage space is all in place, so now we just need to copy the data in
    size_t lenRemaining = len;
    size_t bytesCopied = 0;
    for (size_t outOfs = ofs, pageNo = ofs / PAGE_SIZE, pageOfs = ofs % PAGE_SIZE ; lenRemaining != 0 ; ++pageNo, pageOfs = 0)
    {
        // figure the bytes on this page past the page offset
        size_t bytesOnPage = PAGE_SIZE - pageOfs;

        // limit the write to the bytes remaining on the page, or the remaining source data length
        size_t bytesToCopy = std::min(bytesOnPage, lenRemaining);

        // make sure the page is populated
        if (pages[pageNo] == nullptr)
        {
            // allocate the page; abort on failure
            pages[pageNo].reset(new (std::nothrow) uint8_t[PAGE_SIZE]);
            if (pages[pageNo] == nullptr)
            {
                Log(LOG_ERROR, "RAMFile: insufficient memory for write request (%u bytes at offset %u)\n",
                    static_cast<unsigned int>(ofs), static_cast<unsigned int>(len));
                return bytesCopied;
            }

            // clear the new page to 0x00 bytes
            memset(pages[pageNo].get(), 0x00, PAGE_SIZE);
        }

        // copy the data
        memcpy(pages[pageNo].get() + pageOfs, src, bytesToCopy);

        // advance the source pointer, deduct the copy from the remaining length, count the bytes copied
        src += bytesToCopy;
        lenRemaining -= bytesToCopy;
        bytesCopied += bytesToCopy;
        outOfs += bytesToCopy;

        // adjust the total file size if past the current high water mark
        size = std::max(size, outOfs);
    }

    // return the number of bytes written
    return bytesCopied;
}

int FlashStorage::RAMFile::Read(size_t ofs, void *vdst, size_t len)
{
    // if the offset is past EOF, read zero bytes
    if (ofs >= size)
        return 0;

    // get a byte pointer
    uint8_t *dst = reinterpret_cast<uint8_t*>(vdst);

    // figure the bytes remaining at the offset
    size_t bytesPastOfs = size - ofs;

    // limit the read to the remaining size
    len = std::min(len, bytesPastOfs);

    // read in pages
    size_t lenRemaining = len;
    size_t bytesCopied = 0;
    for (size_t pageNo = ofs / PAGE_SIZE, pageOfs = ofs % PAGE_SIZE ; lenRemaining != 0 ; ++pageNo, pageOfs = 0)
    {
        // figure the bytes on this page past the page offset
        size_t bytesOnPage = PAGE_SIZE - pageOfs;

        // limit the read to the bytes remaining on the page, or the remaining read length
        size_t bytesToCopy = std::min(bytesOnPage, lenRemaining);

        // If the page has been allocated, copy the data.  Otherwise,
        // fill the output buffer with the same number of null bytes
        // (0x00), since unallocated pages are notionally filled with
        // null bytes.
        if (pages[pageNo] != nullptr)
            memcpy(dst, pages[pageNo].get() + pageOfs, bytesToCopy);
        else
            memset(dst, 0x00, bytesToCopy);

        // advance the write pointer, deduct the copy from the remaining length, count the bytes copied
        dst += bytesToCopy;
        lenRemaining -= bytesToCopy;
        bytesCopied += bytesToCopy;
    }

    // return the bytes copied
    return bytesCopied;
}

void FlashStorage::RAMFile::Clear()
{
    size = 0;
    pages.clear();
}


bool FlashStorage::RAMFile::Commit(const char *filename, uint32_t flashAllocSize)
{
    // open the file for write
    int fh = flashStorage.OpenWrite(filename, static_cast<uint32_t>(size), std::max(static_cast<uint32_t>(size), flashAllocSize));
    if (fh < 0)
        return false;

    // write the data
    bool ok = true;
    for (size_t pageNo = 0, bytesRemaining = size ; bytesRemaining != 0 && pageNo < pages.size() ; ++pageNo)
    {
        // figure the number of bytes remaining on this page
        size_t bytesOnPage = std::min(bytesRemaining, PAGE_SIZE);

        // write it
        if (!flashStorage.WriteFile(fh, pages[pageNo].get(), bytesOnPage))
        {
            ok = false;
            break;
        }

        // deduct this write from the remaining write
        bytesRemaining -= bytesOnPage;
    }

    // close the file
    if (!flashStorage.CloseWrite(fh))
        ok = false;

    // return the result
    return ok;
}

bool FlashStorage::RAMFile::Load(const char *filename)
{
    // clear any existing contents
    Clear();

    // try opening the file
    FileInfo fi;
    if (flashStorage.OpenRead(filename, fi) != OpenStatus::OK)
        return false;

    // write data from the flash memory to our RAM file
    return Write(0, fi.data, fi.size);
}

