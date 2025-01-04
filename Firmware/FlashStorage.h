// Pinscape Pico - Flash Data Storage
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Implements a primitive file-like storage system on the Pico's built-in
// flash storage.  This is used for storing Pinscape's persistent data, such
// as configuration settings and plunger calibration data.
//
// Brief design overview:
//
//  - Central directory.  We allocate a fixed-size directory that stores
//    pointers to all of the files.  This is located at the very end of
//    the flash storage area.  The size is pre-determined at compile time
//    and is in units of whole sectors, so that we can erase it as a unit
//    during the initial "formatting" process.
//
//  - Each file has a fixed-size entry in the central directory.  The
//    entry contains the filename, fixed allocation size (the fixed upper
//    limit for the file size), and a pointer to the start of the file's
//    allocated space in flash.  When a new file is created, we find the
//    next available flash sector, working backwards from the last current
//    file, and add a directory entry with the pointer to the allocated
//    space and other metadata.
//
//  - Central directory entries are "write-once": they can be created
//    but not deleted or updated.  This prevents changing a file's upper
//    size limit after creation.
//
//  - When a file is written, we look up its location in the central
//    directory to find the allocated space (adding a new entry if one
//    doesn't already exist).  We then erase enough sectors to hold the
//    new file contents, and program the file into those sectors.
//
//  - To allow dynamic file sizing, each file's allocated space starts
//    with a small header containing the file's actual size.  This is
//    written as part of the file update process.
//
//  - As an integrity check, the header also contains a checksum of the
//    file's contents, also programmed when the file is updated.
//
//
// A note on byte-level flash writes
//
// The Pico SDK's write-to-flash function (flash_range_program()) only
// allows writing to flash in units of a flash "page" == 256 bytes.
// However, we can accomplish byte-granularity writes, even with the
// page-oriented API, by exploiting the "write-towards-zero" property of
// flash.  The Pico's flash works like most flash memory in that you can
// change a bit from '1' to '0' via a WRITE operation, but you can only
// change a bit from '0' to '1' via an ERASE operation.  If a bit is
// currently '0', writing '1' to the bit has no effect.  And, as would
// seem obvious, writing '1' to a bit that's already '1' leaves it as a
// '1'.  So the net effect is that writing '1' to a bit leaves that bit
// unchanged.  This allows us to change any single bit in flash, even
// though we can only write in whole pages, by writing a page full of
// '1' bits except for the single bit we want to change.  We exploit
// this to update the central directory structure incrementally, by
// leaving unused portions set to all '1' bits (0xFF bytes).  This lets
// us go back and fill in unused portions at any time.
// 

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <bitset>
#include <memory>
#include <vector>
#include <pico/stdlib.h>
#include <hardware/flash.h>
#include "Pinscape.h"

// forwards/externals
class FlashStorage;
class ConsoleCommandContext;
namespace PinscapePico { struct FlashFileSysInfo; }

// global singleton
extern FlashStorage flashStorage;

// Flash storage class
class FlashStorage
{
public:
    FlashStorage();

    // Initialize
    void Initialize();

    // Mount the file system.  This checks for a valid central directory
    // structure; if not found, this automatically creates a new one.
    // Returns true on success, false on failure.
    bool Mount(uint32_t centralDirectorySize);

    // Format a new file system.  This is called internally from Mount() if
    // no existing file system is found (or it appears to be corrupted).
    // This can also be used to explicitly erase all current data for a
    // factory reset.
    bool Format(uint32_t centralDirectorySize);

    // Is the file system mounted?
    bool IsMounted() const { return centralDirectory != nullptr; }

    // Open a file for reading.  Fills in the FileInfo struct and returns
    // OK if the file exists; returns a suitable status code if the doesn't
    // exist or if the contents don't match the checksum.  There's no need
    // to "close" the file (and no function to do so), because an open-for-
    // read dosn't affect the file system state.  It just retrieves the
    // location of the file's flash storage for the caller to access via
    // memory reads.
    struct FileInfo
    {
        // pointer to the first byte of the file's contents in flash
        const uint8_t *data = nullptr;

        // size of the file's contents
        uint32_t size = 0;

        // CRC-32 of the file contents
        uint32_t crc32 = 0;
    };
    enum class OpenStatus
    {
        OK = 0,             // open successful
        NotFound = -1,      // file not found - no directory for the file exists
        BadDirEntry = -2,   // bad directory entry
        BadChecksum = -3,   // bad checksum - file data corrupted
        NotMounted = -4,    // file system not mounted
    };
    OpenStatus OpenRead(const char *name, FileInfo &fi);

    // Test if a file exists.  This returns true if a directory entry
    // is found for the file, whether or not the stored data are valid.
    bool FileExists(const char *name);

    // test if a file exists based on an OpenRead result
    bool FileExists(OpenStatus status);

    // Open a file for writing.
    //
    // curSize is the size of the new file contents if known in advance,
    // otherwise zero.  maxSize is the maximum size that the file can EVER
    // be, now or in future versions; this is the amount of space allocated
    // for the file if a new entry is created.
    //
    // If the file already exists, this checks that the requested maximum
    // size is compatible with (no larger than) the existing maximum size,
    // and fails with an error if not.  If the file doesn't exist, this
    // attempts to add a new directory entry and allocate space, failing
    // with an error if there's not a free directory entry available, or if
    // there's insufficient space for the maxSize contents.  On success,
    // it returns a non-negative integer file handle.
    //
    // If the exact size of the file to be written on this operation is
    // known in advance, pass it in curSize.  This allows the file manager
    // to reserve exactly enough space for the new file data, which lets
    // greatly reduce the amount of sector erasing necessary when updating
    // small files.  If the exact size isn't known in advance, set curSize
    // to zero; this forces the file manager to reserve the entire maxSize
    // block for the new copy of the file.
    int OpenWrite(const char *name, uint32_t curSize, uint32_t maxSize);

    // Write to an open file
    bool WriteFile(int handle, const void *data, size_t len);
                                    
    // Close a file opened for writing
    bool CloseWrite(int handle);

    // Remove a file.  If 'silent' is true, the function succeeds
    // (returns true and doesn't log any error messages) if the file
    // doesn't exist.
    bool Remove(const char *name, bool silent);

    // Populate a PinscapePico::FlashFileSysInfo struct, for the Vendor
    // Interface file system information query command.
    size_t Populate(PinscapePico::FlashFileSysInfo *buf, size_t bufLen);

    // Virtual RAM file.  This creates a random-access file in RAM.  The
    // entire file is held in RAM, allowing a client to read and write into
    // the file dynamically without the cost of accessing flash or incurring
    // flash write wear.
    //
    // The primary use case that this was created for is construction of a
    // file from the host side across the USB connection, particularly the
    // configuration files.  The USB interface limits how much data can be
    // sent host-to-device in one request, so if the host wants to send a
    // file constructed on the host side for storage in Pico flash, it has
    // to send the file in chunks of no more than the USB transfer limit.
    // USB connections can be broken unpredictably at any time, so it's
    // entirely possible that the host could start a multi-chunk file
    // transfer that never completes, due a USB disconnect.  If we were to
    // write the file directly to flash one chunk at a time, we could leave
    // flash with a truncated copy of the file if the connection breaks in
    // the middle of the transfer.  Constructing the file in RAM, and then
    // committing it to flash at the end of the transfer, provides better
    // protection against that possibility: if the transfer is interrupted,
    // the memory file is simply never committed, so the old version of the
    // file is left intact in flash.
    class RAMFile
    {
    public:
        // Write at a given byte offset.  Writes beyond the current file
        // size automaticaly extend the file size to the new offset,
        // filling the space between the current end of file and the
        // write offset with null bytes (0x00).  Returns the length
        // actually written, or -1 on error.
        int Write(size_t ofs, const void *data, size_t len);
        
        // Read from a given byte offset.  Returns the length actually
        // read, or -1 on error.  Reading beyond the end of the file
        // isn't an error; it simply truncates the read to the remaining
        // number of bytes beyond the requested offset, which could be
        // zero.
        int Read(size_t ofs, void *dst, size_t len);
        
        // Commit to a flash file.  flashAllocSize is the maximum file size
        // to allocate in the flash storage space for the file, to use as
        // the maxSize argument in FlashStorage::OpenWrite().  Returns true
        // on success, false on failure.
        bool Commit(const char *filename, uint32_t flashAllocSize);

        // Load from an existing flash file.  Replaces any existing
        // contents.  Returns true on success, false on failure.
        bool Load(const char *name);
        
        // delete the current contents, resetting the file to an empty byte stream
        void Clear();

        // get the current in-memory size in bytes
        size_t GetSize() const { return size; }
        
    protected:
        // Pages.  The file is divided into a series of pages of a fixed
        // size.  Pages are only allocated as written, so the array can
        // be sparsely populated if the caller writes at different
        // offsets separated by more than the page size, without writing
        // to the region in between.  Unallocated pages are notionally
        // filled with null bytes (0x00).
        static const size_t PAGE_SIZE = 4096;
        std::vector<std::unique_ptr<uint8_t>> pages;

        // Current size in bytes.  This is the high water mark for
        // writes.  Writes past the end of the current size
        // automatically extend the size to the new location, filling
        // the space in between with null bytes (0x00).
        size_t size = 0;
    };

protected:
    // Is a flash offset/pointer valid?  Returns true if the location is
    // within the flash address range, false if not.
    static bool IsValidFlash(uint32_t ofs) { return ofs < PICO_FLASH_SIZE_BYTES; }
    static bool IsValidFlash(const void *ptr) { return IsValidFlash(reinterpret_cast<uintptr_t>(ptr) - XIP_BASE); }

    // Is a flash offset/pointer within the valid file stream area?  These
    // test only that the location is within range - it doesn't have to point
    // to the start of a stream or even an in-use part of a stream.  It just
    // checks that it's within the right overall memory range.
    bool IsInStreamSpace(uint32_t ofs) const { return ofs >= minAllocOffset && ofs < PICO_FLASH_SIZE_BYTES - centralDirectorySize; }
    bool IsInStreamSpace(const void *ptr) const { return IsInStreamSpace(reinterpret_cast<uintptr_t>(ptr) - XIP_BASE); }

    // Directory entry structure.  Each file in the central directory has
    // one of these entries.
    struct DirectoryEntry
    {
        // Check to see if this entry has been assigned.  An unassigned
        // entry has erased-and-unprogrammed 0xFF bytes throughout the
        // structure.  (We only check the sequence number part, because
        // we do a full integrity check when the file system is first
        // mounted, hence there's no reason to do all of those checks
        // again on every file lookup.)
        bool IsAssigned() const { return sequence != 0xFFFFFFFF; }

        // Is this a replaced entry?  A replaced entry is one where the
        // maximum size was expanded on a later write, necessitating
        // creating a new entry for the same file.  The old entry has its
        // filename set to all zero bytes.
        bool IsReplaced() const { return memcmp(filename, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0; }

        // Has the file been deleted?  A deleted file has its first content
        // page erased, so its file size will read as all FF bytes.
        bool IsDeleted() const;

        // Is this a free entry?  An entry is free if all fields except the
        // sequence number are "erased" bytes, 0xFF.  This entry can be
        // reused for a new file, beacuse all of the allocation fields are
        // writable, as they're set to all '1' bits.
        bool IsFree() const {
            return memcmp(filename, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16) == 0
                && maxSize == 0xFFFFFFFF && flashOffset == 0xFFFFFFFF && crc32 == 0xFFFFFFFF;
        }
        
        // Directory sequence number.  Each entry is assigned a number
        // starting at SEQUENCE0 and incrementing by one on each new entry.
        // This is an integrity check; it's meant to be a bit pattern that's
        // clearly not a newly erased byte, and is unlikely to occur
        // randomly if the sector was previously written by a different
        // application with some unrelated data.
        uint32_t sequence;
        static const uint32_t SEQUENCE0 = 0x5AA55AA5;
        
        // Filename.  This is meant to be a human-readable name, but it
        // can contain any characters.  The name can use the full array
        // without null termination, but must be padded with null bytes
        // at the end if shorter.
        char filename[16];

        // Space allocated for the file, in bytes.  This is always a
        // multiple of the sector size (when creating a new file, we round
        // the caller's requested maximum size up to the next sector size
        // boundary if it's not already sector-aligned).  The allocated
        // space includes the file content header, so the actual usable
        // space for the file's contents is reduce by the header size.
        uint32_t maxSize;

        // Flash offset of the file.  This is the byte offset within the
        // flash of the file's assigned storage area.  Add this to XIP_BASE
        // to get a direct memory pointer to read from the file.  The file's
        // storage area is contiguous.  This points to the file's content
        // header; the actual file contents immediateley follow the header.
        uint32_t flashOffset;

        // CRC-32 checksum of this entry.  This is an additional integrity
        // check to ensure that the whole entry is actually a directory
        // entry (and not some random data from another application that
        // just happens to have the right sequence number in the first four
        // bytes, as unlikely as that might be), AND that it's intact.
        uint32_t crc32;

        // calculate the CRC for this struct
        uint32_t CalculateCRC() const;
    };

    // We've carefully devised this struct so that it evenly divides the
    // flash page size.  This is important because it allows us to write
    // each struct as part of a whole page write, which is the smallest
    // unit of flash programming that the Pico API offers.
    static_assert((FLASH_PAGE_SIZE % sizeof(DirectoryEntry)) == 0);

    // Central directory size.  This is a fixed size for the directory,
    // always located at the top of the flash space.  It must be in whole
    // flash sectors.  This is set when the file system is mounted.
    uint32_t centralDirectorySize = 0;

    // pointer to the start of the central directory
    const DirectoryEntry *centralDirectory = nullptr;

    // Sector usage map.  A '1' bit represents a sector that's in use.  We
    // populate this when mounting the file sysetm, and maintain it as we
    // allocate space.  This allows reclaiming fragmented free space created
    // by deleting or expanding files.
    //
    // We think of the flash as having sectors numbered from 0 to 511
    // (assuming the standard Pico flash size of 2MB with 4K sectors).  The
    // sector at flash offset 0 is sector 0, flash offset 4096 is sector 1,
    // etc.  The sector number is the bitset index.
    std::bitset<PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE> sectorsUsed;

    // "Low-water mark" of allocated flash storage.  This is the lowest
    // offset in flash that we've allocated to a file.
    uint32_t minAllocOffset = PICO_FLASH_SIZE_BYTES;

    // Next sequence number
    uint32_t nextSequence = 0;

    // Initialize a directory entry for a new file
    bool InitFileEntry(const DirectoryEntry *dir, const char *name, uint32_t maxSize);

    // Update a directory entry in the flash space.  This writes the
    // in-memory directory entry 'newData' into the DirectoryEntry in flash
    // space at 'flashPtr', preserving the rest of the flash page containing
    // the data.
    bool UpdateFileEntry(const DirectoryEntry *flashPtr, const DirectoryEntry *newData, const char *op, const char *filename);

    // Rebuild the central directory.  This searches for erased files
    // and replaced directory entries; if any are found, it rebuilds
    // the directory structure to make these slots available for use
    // as new files.
    bool RebuildCentralDirectory(bool &modified);

    // get the file content stream for a directory entry
    bool GetFileStream(const DirectoryEntry *dir, FileInfo &fi, bool &isDeleted);

    // Find a directory entry by filename.  Returns a pointer to the
    // directory entry if found.
    //
    // If no existing entry is found, the result depends upon forWriting.
    // In read mode (forWriting is false), this simply returns null if no
    // entry is found.  In writing mode (forWriting is true), this returns a
    // pointer to the first free entry, if one is available, otherwise null.
    // Write mode returns a free entry when no existing entry is found
    // because the caller presumably needs a free slot to create a new
    // entry.
    const DirectoryEntry *FindFileEntry(const char *name, bool forWriting);

    // File content header.  This is placed at the start of each file's
    // allocated storage area.  The file's actual byte stream contents
    // immediately follow the header.
    struct FileHeader
    {
        // size of the file in bytes
        uint32_t fileSize;

        // CRC-32 of the content stream (excluding the header)
        uint32_t crc;
    };

    // Write handless
    struct WriteHandle
    {
        // Current write offset in the overall file.  This represents the
        // stream byte offset of the next byte to be written from our
        // buffer.  The write offset isn't updated when a byte is buffered;
        // it's only updated when the buffer is flushed.
        //
        // Note that this is an offset in the file's content area, not an
        // offset in the whole flash space.  Add this to the file's flash
        // offset to get the location in flash of the next write.
        uint32_t writeOffset = 0;

        // File header offset, from the start of the file's content area.
        // This might be somewhere in the middle of the file's allocation
        // region, because we can append a new copy of the file to existing
        // allocation region if there's room, rather than erasing the whole
        // flash area again.
        uint32_t headerOffset = 0;

        // Write buffer.  This gathers up one flash page before writing.
        uint8_t buf[FLASH_PAGE_SIZE];
        uint16_t bufOffset = 0;

        // Flush the buffer
        bool Flush();

        // directory entry pointer
        const DirectoryEntry *dirEntry = nullptr;

        // time the handle was opened
        uint64_t tOpen = 0;
    };
    WriteHandle writeHandles[1];

    // initialization completed?
    bool initialized = false;

    // Flash device ID information, from standard command 0x9F
    struct JEDEC_ID
    {
        uint8_t mfgId;        // JEDEC manufacturer ID code
        uint8_t devId;        // device ID (manufacturer-specific)
        uint8_t capacityId;   // capacity ID (manufacturer-specific)

        // test for a known manufacturer and device ID
        bool DeviceIs(uint8_t mfgId, uint8_t devId) const {
            return this->mfgId == mfgId && this->devId == devId;
        }
    };
    JEDEC_ID jedecId;

    // Pico built-in flash memory size.  If we recognize the device by
    // JEDEC ID, or we find a valid SFDP record, we can reliably determine
    // the size of the flash.  Otherwise, we'll use a generic assumption
    // that the flash is 2MB, based on the reference design from Raspberry
    // Pi.  isFlashSizeKnown is set to true if we can determine the size,
    // false if it's just the default assumption.
    uint64_t flashSizeBytes = 2*1024*1024;
    bool isFlashSizeKnown = false;

    // find a free handle
    int FindFreeHandle();

    // Check if a sector has been erased
    static bool IsSectorErased(const void *sector);
    static bool IsSectorErased(uint32_t sectorOffset);

    // Check if an arbitrary region of flash has been erased
    static bool IsFlashErased(const void *ptr, uint32_t nBytes);
    static bool IsFlashErased(uint32_t startOfs, uint32_t nBytes);

    // Console commands
    static void Command_fsck(const ConsoleCommandContext *c);
    static void Command_ls(const ConsoleCommandContext *c);
    static void Command_rm(const ConsoleCommandContext *c);
    static void Command_flash(const ConsoleCommandContext *c);
};

