// Pinscape Pico Device - Boot Loader interface
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY
//
// Implements helper functions for working with a Pico in its native
// Boot Loader mode.  The Boot Loader is built into every Pico, in ROM
// embedded in the RP2040 CPU, so it's always available without any
// software installation required, and can't be erased or corrupted by
// errant firmware.  In Boot Loader mode, the Pico exposes a USB MSC
// "virtual thumb drive" interface, where it appears as a FAT-formatted
// removable drive.  It's only an emulated FAT drive; it can't actually
// store or retrieve arbitrary files.  It will accept a host-to-device
// file transfer for a file in UF2 format:
//
//   https://github.com/microsoft/uf2
// 
// It also provides a fake directory listing with a couple of files
// required by the UF2 spec, which the host can read to get information
// on the device.
// 
// This interface provides functions to enumerate the Pico Boot Loader
// devices currently attached, and to send UF2 files to the device.
// UF2 transfers can be used to install firmware or simply to write
// arbitrary data to specified locations in the Pico's flash.  This
// allows the host to store application data separately from the
// firmware program image.  Note, however, that the Pico's flash
// doesn't have any sort of file system or other structure; it's just
// an unformatted array of bytes.  The only structure is the simple
// program image layout implied by the way the boot loader launches
// firmware on Pico reset, which requires the program's startup code
// to be located at a certain point in flash space.  Beyond that, a
// linker can arrange the program layout however it wishes, and
// applications can use any space not allocated by the linker for
// data storage.
// 

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <tchar.h>
#include <ctype.h>
#include <memory>
#include <functional>
#include <list>
#include <string>
#include <regex>
#include <algorithm>
struct IUnknown;  // workaround for Microsoft SDK header bug when compiling with a Win 8.1 target
#include <Windows.h>
#include <shlwapi.h>
#include <usb.h>
#include <winusb.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <process.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <SetupAPI.h>
#include <shellapi.h>
#include <winerror.h>
#include "crc32.h"
#include "BytePackingUtils.h"
#include "Utilities.h"
#include "PinscapePicoAPI.h"
#include "RP2BootLoaderInterface.h"

using namespace PinscapePico;

#pragma comment(lib, "shlwapi")
#pragma comment(lib, "Shell32")

// --------------------------------------------------------------------------
//
// Boot Loader mode devices
//

// Write a UF2 file to a Pico in Boot Loader mode, constructing
// the file contents by repeatedly invoking the provided callback.
// Each callback invocation supplies one 256-byte block.  Returns
// true on success, false on failure.
bool RP2BootDevice::WriteUF2(
	uint32_t startingAddress, uint32_t numBlocks,
	std::function<void(uint8_t *buf, uint32_t blockNum)> fillBlockPayload,
	IProgressCallback *progress, const TCHAR *sourceDescription)
{
	// Synthesize the UF2 filename path.  The filename portion is
	// arbitrary, since the Pico only stores the contents of the
	// file; it doesn't have a notion of file metadata that would
	// let it store a filename.  We just need a filename for the
	// sake of the Boot Loader's virtual disk interface.
	TCHAR outName[MAX_PATH];
	PathCombine(outName, path.c_str(), _T("DATA.UF2"));

	// use a dummy progress callback if the caller didn't provide one
	IProgressCallback dummyProgress;
	if (progress == nullptr)
		progress = &dummyProgress;

	// start the progress reports
	progress->ProgressInit(sourceDescription, outName, numBlocks * 256);

	// from now on, notify the progress display when we exit
	auto Result = [progress](bool success) {
		progress->ProgressFinish(success);
		return success;
	};

	// Open a UF2 file on the Pico (the name is arbitrary, as the
	// Pico only stores the contents; it has no file system).  Use
	// overlapped I/O so that we can check asynchronously for user
	// cancellation requests.  Use cache write-through so that we
	// don't build up a big backlog of buffered writes waiting to
	// be transmitted to the device; that makes our progress bar
	// estimate more realistic.
	static const auto CancelAndClose = [](HANDLE hFile) { 
		CancelIo(hFile); 
		CloseHandle(hFile); 
	};
	HandleHolder<HANDLE> hOut(
		CreateFile(outName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH, NULL),
		CancelAndClose);
	if (hOut.handle == INVALID_HANDLE_VALUE)
		return false;

	// Event and OVERLAPPED object for write
	OverlappedObject ovOut;

	// Wait for an overlapped operation to finish, periodically checking for user cancellation
	auto WaitOverlapped = [progress](HANDLE hFile, OverlappedObject &ov, DWORD &numBytesTransferred)
	{
		// keep going until the I/O completes or fails, or the user cancels
		for (;;)
		{
			// get the overlapped result with a short timeout
			if (GetOverlappedResultEx(hFile, &ov.ov, &numBytesTransferred, 100, TRUE))
				return true;

			// If the error is "IO incomplete", it means the wait timed out.
			// Anything else is an error on the actual operation.
			switch (GetLastError())
			{
			case WAIT_TIMEOUT:
			case WAIT_IO_COMPLETION:
			case ERROR_IO_INCOMPLETE:
				// timed out or interrupted by alert - keep looping, after checking
				// for user cancellation
				if (progress->IsCancelRequested())
					return false;
				break;

			default:
				// anything else counts as a failed I/O
				return false;
			}
		}
	};
	
	// write the data
	for (uint32_t blockNo = 0, writeOfs = 0, targetAddr = startingAddress ; blockNo < numBlocks ;
		++blockNo, targetAddr += 256, writeOfs += 256)
	{
		// check for a cancellation request
		if (progress->IsCancelRequested())
			return false;

		// set up the block header
		UF2_Block blk(this, targetAddr, blockNo, numBlocks);

		// populate the block payload via the callback
		fillBlockPayload(blk.data, blockNo);

		// start an asynchronous write to the file
		DWORD bytesWritten = 0;
		if (!WriteFile(hOut.handle, &blk, sizeof(blk), &bytesWritten, ovOut.SetAppend()))
		{
			// make sure the error is 'I/O pending'
			if (GetLastError() != ERROR_IO_PENDING)
				return false;

			// wait for the write to complete
			if (!WaitOverlapped(hOut.handle, ovOut, bytesWritten))
				return false;
		}

		// make sure all bytes were written
		if (bytesWritten != sizeof(blk))
			return false;

		// update the progress UI
		progress->ProgressUpdate(writeOfs + 256);
	}

	// explicitly close the file so that we can check the final result for any buffered data
	if (!CloseHandle(hOut.release()))
		return false;

	// no errors -> success
	return true;
}

// Reboot a Pico in Boot Loader mode, by writing a no-op UF2 file.
// This writes a UF2 file that stores data in an unused portion of
// the flash-mapped space, to trigger a reset without changing
// anything in flash.
bool RP2BootDevice::RebootPico()
{
	// The reboot technique varies by hardware type
	if (boardID == "RP2350")
	{
		// RP2350:  The simple trick we use for RP2040 doesn't
		// work for RP2350, because the RP2350 boot loader validates
		// the target address.  It will only accept downloads to
		// RAM and valid flash addresses.  It actually validates
		// the address against the bounds of physical storage, too,
		// so we can't get away with picking an address that's
		// merely reserved for flash but isn't backed by physical
		// flash, as we can on the RP2040.
		// 
		// Since the bootloader will accept RAM addresses, the way
		// to download a UF2 to the device without overwriting any
		// flash data is to download to RAM.  But this is trickier
		// than it seems, because the bootloader always treats a
		// UF2 transfer to RAM as a RAM-resident program image to
		// launch.  If we download random data that isn't a valid
		// program image, the bootloader will try to launch it
		// anyway, which will crash the Pico, which will then
		// reboot it back into bootloader mode.  So we'd just be
		// back where we started, with the Pico in bootloader mode.
		// 
		// To accomplish a reboot into the flash-resident program,
		// we have to download a working RAM-resident program that
		// explicitly reboots the Pico into the flash program.
		// The binary data below represents a minimal working
		// image that accomplishes that.  The image represented
		// by the data below simply does the required post-boot
		// startup work, then programs the Watchdog register to
		// immediately reboot the Pico with the flash-resident
		// program as the launch target.  The program must be
		// linked with a base address of 0x20000000 (the start
		// of RAM).  Preparing a suitable program image isn't
		// trivial, since it must meet the requirements of the
		// RP2350 boot ROM, including metadata block wrappers,
		// ARM interrupt table header, and basic startup code.
		// The best way to create an image is to use a "bare
		// metal" link template with the Pico SDK GCC build
		// tools, omitting the Pico SDK libraries to minimize
		// the image size.  The smallest working image I've
		// been able to produce that way is about 500 bytes.
		//
		// Note that the array must be declared with a size that's
		// a multiple of 256, so that we don't have to bounds-check
		// the last packet in the transfer loop.  (It's okay to
		// populate the bytes short of the ending boundary, since
		// the C++ compiler will fill in the rest with 0x00 bytes.
		// The important thing is that the *declared* size is a
		// multiple of 256.)
		static const uint8_t rp2350Rebooter[512] = {
			0x00, 0x20, 0x08, 0x20, 0xD9, 0x00, 0x00, 0x20, 0xD1, 0x00, 0x00, 0x20, 0xD5, 0x00, 0x00, 0x20,
			0xD1, 0x00, 0x00, 0x20, 0xD1, 0x00, 0x00, 0x20, 0xD1, 0x00, 0x00, 0x20, 0xD1, 0x00, 0x00, 0x20,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD1, 0x00, 0x00, 0x20,
			0xD1, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0xD1, 0x00, 0x00, 0x20, 0xD1, 0x00, 0x00, 0x20,
			0xD3, 0xDE, 0xFF, 0xFF, 0x42, 0x01, 0x21, 0x10, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x79, 0x35, 0x12, 0xAB, 0xA3, 0xF5, 0x80, 0x3A, 0x70, 0x47, 0x00, 0xBF, 0x17, 0x4B, 0x00, 0x2B,
			0x08, 0xBF, 0x13, 0x4B, 0x9D, 0x46, 0xFF, 0xF7, 0xF5, 0xFF, 0x00, 0x21, 0x8B, 0x46, 0x0F, 0x46,
			0x13, 0x48, 0x14, 0x4A, 0x12, 0x1A, 0x00, 0xF0, 0x71, 0xF8, 0x0E, 0x4B, 0x00, 0x2B, 0x00, 0xD0,
			0x98, 0x47, 0x0D, 0x4B, 0x00, 0x2B, 0x00, 0xD0, 0x98, 0x47, 0x00, 0x20, 0x00, 0x21, 0x04, 0x00,
			0x0D, 0x00, 0x0D, 0x48, 0x00, 0x28, 0x02, 0xD0, 0x0C, 0x48, 0xAF, 0xF3, 0x00, 0x80, 0x00, 0xF0,
			0x65, 0xF8, 0x20, 0x00, 0x29, 0x00, 0x00, 0xF0, 0x39, 0xF8, 0x00, 0xF0, 0x45, 0xF8, 0x00, 0xBF,
			0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x08, 0x20,
			0xF4, 0x01, 0x00, 0x20, 0xF8, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xFE, 0xE7, 0x00, 0xBF, 0xFE, 0xE7, 0x00, 0xBF, 0x08, 0xB5, 0x0B, 0x4B, 0x83, 0xF3, 0x09, 0x88,
			0x0A, 0x4B, 0x83, 0xF3, 0x0A, 0x88, 0x83, 0xF3, 0x0B, 0x88, 0x09, 0x4A, 0x09, 0x4B, 0x93, 0x42,
			0x08, 0xD2, 0x01, 0x3A, 0xD2, 0x1A, 0x22, 0xF0, 0x03, 0x02, 0x18, 0x46, 0x06, 0x49, 0x04, 0x32,
			0x00, 0xF0, 0x58, 0xF8, 0xFF, 0xF7, 0xAA, 0xFF, 0x00, 0x20, 0x08, 0x20, 0x00, 0xA0, 0x07, 0x20,
			0xF4, 0x01, 0x00, 0x20, 0xF4, 0x01, 0x00, 0x20, 0xF4, 0x01, 0x00, 0x20, 0x00, 0x20, 0x01, 0x21,
			0x4F, 0xF0, 0x00, 0x42, 0x03, 0x4B, 0xD8, 0x61, 0x18, 0x62, 0x58, 0x62, 0x98, 0x62, 0x59, 0x60,
			0x1A, 0x60, 0x70, 0x47, 0x00, 0x80, 0x0D, 0x40, 0x08, 0xB5, 0x06, 0x4B, 0x04, 0x46, 0x13, 0xB1,
			0x00, 0x21, 0xAF, 0xF3, 0x00, 0x80, 0x04, 0x4B, 0x1B, 0x68, 0x03, 0xB1, 0x98, 0x47, 0x20, 0x46,
			0x00, 0xF0, 0x3E, 0xF8, 0x00, 0x00, 0x00, 0x00, 0xF4, 0x01, 0x00, 0x20, 0x02, 0x44, 0x03, 0x46,
			0x93, 0x42, 0x00, 0xD1, 0x70, 0x47, 0x03, 0xF8, 0x01, 0x1B, 0xF9, 0xE7, 0x70, 0xB5, 0x0D, 0x4B,
			0x00, 0x26, 0x0D, 0x4D, 0x5B, 0x1B, 0x9C, 0x10, 0xA6, 0x42, 0x09, 0xD1, 0x0B, 0x4D, 0x00, 0xF0,
			0x29, 0xF8, 0x0B, 0x4B, 0x00, 0x26, 0x5B, 0x1B, 0x9C, 0x10, 0xA6, 0x42, 0x05, 0xD1, 0x70, 0xBD,
			0x55, 0xF8, 0x04, 0x3B, 0x01, 0x36, 0x98, 0x47, 0xEE, 0xE7, 0x55, 0xF8, 0x04, 0x3B, 0x01, 0x36,
			0x98, 0x47, 0xF2, 0xE7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x0A, 0x44, 0x43, 0x1E, 0x91, 0x42, 0x00, 0xD1, 0x70, 0x47, 0x10, 0xB5,
			0x11, 0xF8, 0x01, 0x4B, 0x91, 0x42, 0x03, 0xF8, 0x01, 0x4F, 0xF9, 0xD1, 0x10, 0xBD, 0x00, 0x00,
			0xFE, 0xE7, 0x00, 0xBF, 0xF8, 0xB5, 0x00, 0xBF, 0xF8, 0xBC, 0x08, 0xBC, 0x9E, 0x46, 0x70, 0x47,
			0xF8, 0xB5, 0x00, 0xBF, 0xF8, 0xBC, 0x08, 0xBC, 0x9E, 0x46, 0x70, 0x47, 0x68, 0xFE, 0xFF, 0x7F,
			0x01, 0x00, 0x00, 0x00,
		};
		return WriteUF2(0x20000000, sizeof(rp2350Rebooter)/256, [](uint8_t *buf, uint32_t blockNum)
		{
			memcpy(buf, &rp2350Rebooter[blockNum*256], 256);
		});
	}
	else
	{
		// RP2040: Download one UF2 block, loaded into an unmapped area
		// in the flash region.  The contents of the download don't
		// matter, since the target address doesn't actually contain
		// any flash memory - the Pico will just discard the data
		// without updating flash.  But the RP2040 bootloader will
		// still consider the UF2 transfer to be successfully completed
		// after writing the block to nowhere, so it'll reboot into
		// the flash program as usual.
		// 
		// This isn't just an observational guess about how the
		// bootloader works.  The behavior is deterministically written
		// into the RP2040 ROM bootloader code, which Raspberry Pi
		// publishes here:
		// 
		// https://github.com/raspberrypi/pico-bootrom/blob/master/bootrom/virtual_disk.c
		//
		// The boot loader treats all addresses between 0x10000000 and
		// 0x20000000 as belonging to writable XIP flash space, but only
		// certain sub-ranges within that 256MB zone are actually mapped.
		// The architecture only allows for 16MB of flash maximum, so the
		// 256MB window allows for 16 views of the 16MB space.  It does
		// this by mapping five views, in the 24-bit ranges starting at
		// 0x10000000, 0x11000000, 0x12000000, 0x13000000, and 0x15000000.
		// Other ranges are documented as unused.  When the UF2 file sets
		// the target address to something within the 0x10000000..0x11FF0000
		// nominal flash region, but outside of the five mapped windows,
		// the boot loader will dutifully copy the bytes to the nominal
		// address - but since an address not in one of those windows
		// isn't physically mapped to any hardware, the Pico bus will
		// accept but ignore the writes.  The effect is that we can
		// write to these ranges without changing anything in the flash,
		// while still making the boot loader believe that something was
		// copied and thus requires a reboot.
		return WriteUF2(0x1F000000, 1,
			[](uint8_t *buf, uint32_t blockNum) { memset(buf, 0, 256); });
	}
}

// Erase configuration data.  This overwrites the small control
// block area in flash that the Pinscape firmware uses to keep track
// of its flash data files.  This effectively deletes all saved config
// data.
bool RP2BootDevice::EraseConfigData()
{
	// The file control block currently occupies the last 4K sector 
	// of flash, so we can erase everything by overwriting that one
	// sector.  To minimize flash wear, fill the space with all '1'
	// bits.  That wipes the previous data, but it also has the nice
	// feature that the next user of the space won't have to re-erase
	// the space, since the erase operation on this type of media
	// sets all bits to '1'.
	const uint32_t TOP_OF_FLASH = 0x10000000 + 2*1024*1024;
	const uint32_t sectorsToErase = 1;
	const uint32_t sectorSize = 4096;
	return WriteUF2(TOP_OF_FLASH - sectorsToErase*sectorSize, sectorSize/256,
		[](uint8_t *buf, uint32_t blockNum) { memset(buf, 0xFF, 256); });
}

// Erase the entire 2MB of Pico flash
bool RP2BootDevice::EraseWholeFlash(IProgressCallback *progress)
{
	// Write '1' bits to the entire 16MB flash space.  The flash
	// media on the Pico erases to all '1' bits, so writing '1' to
	// every bit after the erase will leave the memory in its
	// erased state, which might save the next writer the trouble
	// (and physical flash wear) of re-erasing the space.
	// 
	// Note that we write to the whole 16MB *potential* flash space,
	// even though the official Pico from Raspberry Pi only has 2MB
	// flash.  The thing is that the boot loader thumb drive doesn't
	// give us any indication of the actual size installed, so the
	// only way to be sure we erase the whole physical flash is to
	// erase the whole potential address space.  This ensures that
	// this command works properly even if the device is one of the
	// third-party Pico clones that comes with a larger flash chip
	// (there are several such clones available).  We can get away
	// with writing beyond the end of physical flash on a 2MB (or
	// other <16MB) Pico, because the RP2040 silently ignores writes
	// within the flash-mapped address ranges that aren't backed by
	// physical flash memory.  The ROM boot loader likewise doesn't
	// bounds-check writes against the physical flash size.
	//
	// The downside of wiping the whole 16MB space is that it takes
	// longer (obviously) than limiting it to just the populated 2MB
	// of a reference Pico.  It's not 8x longer, since writes to
	// unpopulated regions don't have to wait for the physical flash
	// erase/write to complete, but it's still a lot longer.  I think
	// this is acceptable for the gain in generality; plus, this
	// operation will certainly be rarely used, so it's not going to
	// inconvenience anyone routinely; plus, no one's going to be
	// surprised that a command called "erase entire flash" takes a
	// couple of minutes to complete.
	const uint32_t FLASH_START = 0x10000000;
	const uint32_t FLASH_SIZE = 16*1024*1024;
	return WriteUF2(FLASH_START, FLASH_SIZE/256,
		[](uint8_t *buf, uint32_t blockNum) { memset(buf, 0xFF, 256); },
		progress, _T("Erase All"));
}

RP2BootDevice::RP2BootDeviceList RP2BootDevice::EnumerateNewRP2BootDrives(const RP2BootDeviceList &before)
{
	// build the new list
	auto after = EnumerateRP2BootDrives();

	// Delete drives that also appear in the first list.
	// (Yes, this is a morally reprehensible O(N^2) algorithm that could
	// be made much more efficient in O() terms by using a sorted list or a
	// hash map.  But N is always small, typically *very* small, as in just
	// 0 or 1, so the overhead of sorting or hashing greatly exceeds any
	// gain that would accrue from the O() contribution.  This is a case
	// where the O() efficiency and the actual efficiency diverge
	// considerably due to small N, and the naive algorithm is actually
	// faster than the clever algorithm.)
	for (auto &b : before)
	{
		// Scan 'after' for 'b'
		for (auto a = after.begin() ; a != after.end() ; ++a)
		{
			// match on path
			if (a->path == b.path)
			{
				// Found a match - this item is in both lists, so
				// it's not new; delete it from the after list.  We
				// can stop the search here, since a given path will
				// only appear in the list once.  (This also happens
				// to save us the trouble of fixing up the iterator
				// to account for erasing the current element; we're
				// abandoning the whole iteration, so we don't have
				// to worry about 'a' being invalid after the
				// erase().  There's no such concern about the outer
				// loop over 'before', since we're not changing the
				// 'before' list here.)
				after.erase(a);
				break;
			}
		}
	}

	// return the pruned "after" list
	return after;
}


RP2BootDevice::RP2BootDeviceList RP2BootDevice::EnumerateRP2BootDrives()
{
	// create a list to hold the results
	RP2BootDeviceList drives;

	// enumerate volumes
	TCHAR vol[MAX_PATH];
	if (HANDLE hVol = FindFirstVolume(vol, MAX_PATH); hVol != INVALID_HANDLE_VALUE)
	{
		// iterate volumes
		do
		{
			// get the paths
			std::vector<TCHAR> pathNames(MAX_PATH + 1);
			for (;;)
			{
				// get the path names
				DWORD sz = static_cast<DWORD>(pathNames.size());
				if (GetVolumePathNamesForVolumeName(vol, pathNames.data(), sz, &sz))
					break;

				// if there's no room, resize and try again, otherwise give up here
				if (GetLastError() == ERROR_MORE_DATA)
					pathNames.resize(sz);
				else
					break;
			}

			// add the paths
			for (TCHAR *p = pathNames.data() ; *p != 0 ; p += _tcslen(p))
			{
				// Check for the RP2 Boot volume contents.  Pico RP2 Boot conforms
				// to the Microsoft UF2 standard, which requires that the virtual
				// drive's root folder contains a file called INFO_UF2.TXT with
				// some identifying information.  Try opening the file.
				FILE *fp;
				TCHAR fname[MAX_PATH];
				PathCombine(fname, p, _T("INFO_UF2.TXT"));
				if (_tfopen_s(&fp, fname, _T("r")) == 0 && fp != nullptr)
				{
					// Got it - read the first line, verify that it's of the
					// form "UF2 Bootloader <version>"
					char buf[256];
					if (fgets(buf, sizeof(buf), fp) != nullptr
						&& _strnicmp(buf, "UF2 Bootloader v", 16) == 0)
					{
						// pull out the bootloader version string, and remove any
						// trailing newline character
						if (size_t len = strlen(buf + 16) ; len != 0 && buf[16 + len - 1] == '\n')
							buf[16 + len - 1] = 0;

						// add a list entry for the new drive
						auto &drive = drives.emplace_back(p, buf+16);

						// scan the Name: Value tags in the file
						for (;;)
						{
							// read the next line, stop at EOF
							if (fgets(buf, sizeof(buf), fp) == nullptr)
								break;

							// make absolutely sure the buffer is null-terminated
                            buf[_countof(buf)-1] = 0;

                            // remove the newline at the end of the line, if present
                            if (size_t len = strlen(buf) ; len != 0 && buf[len-1] == '\n')
                                buf[len-1] = 0;

							//
							// Parse the "Name: Value" tag format that the INFO_UF2.TXT file
							// should contain.
							// 

							// skip leading spaces
							char *v = buf;
							for (; isspace(*v) ; ++v) ;

							// this is the start of the Name portion; find the end of this part,
							// which should be contiguous non-space characters up to a ':'
							char *start = v;
							for (; *v != 0 && *v != ':' && !isspace(*v) ; ++v) ;
							std::string tagName(start, v - start);

							// skip spaces, and make sure there's a ':' - if there's not, just 
							// ignore the line and move on to the next
							for (; isspace(*v) ; ++v) ;
							if (*v != ':')
								continue;

							// skip the ':' and any spaces that follow
							for (++v ; isspace(*v); ++v) ;

							// the value portion is everything up to the end of the line,
							// with trailing spaces trimmed off
                            for (char *endp = v + strlen(v) ; endp > v && isspace(*(endp-1)) ; *--endp = 0) ;
							std::string tagValue(v);

							// convert the Name to lower-case for case-insensitive keying
							// in the map
							std::transform(tagName.begin(), tagName.end(), tagName.begin(), ::tolower);

							// add the Name/Value pair to the map
							drive.tags.emplace(tagName, tagValue);
						}

						// done with the file
						fclose(fp);

						// Check to see if this is actually a Pico.  If it is, it will have
                        // a Board-ID tag with a value of "RPI-RP2" (for a Pico RP2040), or
                        // "RP2350" (for Pico2).  The Microsoft spec calls for a version
						// suffix, delimited by a '-', but none of the current Pico or Pico
						// 2 boards use this.  Check for it anyway in case a future Pico
						// adds it.
						bool isPico = false;
						if (auto it = drive.tags.find("board-id") ; it != drive.tags.end())
						{
							// test this Board-ID against the known Raspberry Pi Pico IDs
							const char *id = it->second.c_str();
                            static const char *testIDs[] = { "RPI-RP2", "RP2350" };
                            for (const char *testID : testIDs)
                            {
                                // test this known ID, either as the whole token or with a '-' suffix
                                size_t len = strlen(testID);
                                if (_strnicmp(id, testID, len) == 0 && (id[len] == 0 || id[len] == '-'))
                                {
                                    // matched - it's a Pico
                                    isPico = true;

									// save the base board ID
									drive.boardID = testID;

                                    // if there's a version suffix, pull it out
									if (id[len] == '-')
										drive.boardVersion.assign(id + len + 1);

                                    // stop searching
                                    break;
                                }
                            }
						}

						// if it's not a Pico after all, remove it from the list
						if (!isPico)
							drives.pop_back();
					}
				}
			}

		} while (FindNextVolume(hVol, vol, MAX_PATH));

		// done with the volume search
		FindVolumeClose(hVol);
	}

	// return the drive list
	return drives;
}


HRESULT RP2BootDevice::InstallFirmware(const TCHAR *uf2FilePath, const TCHAR *rp2BootPath, IProgressCallback *progress)
{
	// Form the output file name.  Use the same root name as the source 
	// file, replacing the source path with the Pico boot drive path.  
	// The name of the file on the Pico side isn't important, as it's not
	// stored anywhere; the Pico doesn't have a file system, so it
	// doesn't have a concept of filenames or any other metadata.  We
	// only need a filename to play along with the charade that the
	// Pico is a thumb drive when in this mode.
	TCHAR outName[MAX_PATH];
	PathCombine(outName, rp2BootPath, PathFindFileName(uf2FilePath));

	// file closer - cancel pending I/O and close the handle
	static const auto CancelAndClose = [](HANDLE hFile) { CancelIo(hFile); CloseHandle(hFile); };

	// open the source file
	HandleHolder<HANDLE> hIn(
		CreateFile(uf2FilePath, GENERIC_READ, FILE_SHARE_READ,
			NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL),
		CancelAndClose);
	if (hIn.handle == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

	// get the file size
	DWORD inSizeHigh, inSize = GetFileSize(hIn.handle, &inSizeHigh);

	// initialize the progress display, if provided
	if (progress != nullptr)
		progress->ProgressInit(uf2FilePath, outName, inSize);

	// from now on, notify the progress display when we exit
	auto Result = [progress](HRESULT hr) {
		progress->ProgressFinish(SUCCEEDED(hr));
		return hr;
	};

	// Open the output file.  Use overlapped writes so that we can
	// poll for user cancellation requests.  Use cache write-through
	// so that we don't build up a backlog of buffered writes awaiting
	// transmission to the device; this makes our progress estimates
	// more accurate, since we don't have to account for a big batch
	// of buffered writes to finish at the end when we close the file.
	HandleHolder<HANDLE> hOut(
		CreateFile(outName, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH, NULL),
		CancelAndClose);
	if (hOut.handle == INVALID_HANDLE_VALUE)
		return Result(HRESULT_FROM_WIN32(GetLastError()));

	// OVERLAPPED objects for read and write
	OverlappedObject ovIn, ovOut;

	// Wait for an overlapped operation to finish, periodically checking for user cancellation
	auto WaitOverlapped = [progress](HANDLE hFile, OverlappedObject *ov, DWORD &numBytesTransferred)
	{
		// keep going until the I/O completes or fails, or the user cancels
		for (;;)
		{
			// get the overlapped result with a short timeout
			if (GetOverlappedResultEx(hFile, &ov->ov, &numBytesTransferred, 100, TRUE))
				return S_OK;

			// If the error is "IO incomplete", it means the wait timed out.
			// Anything else is an error on the actual operation.
			DWORD err = GetLastError();
			switch (err)
			{
			case WAIT_TIMEOUT:
			case WAIT_IO_COMPLETION:
			case ERROR_IO_INCOMPLETE:
				// timed out or interrupted by alert - keep looping, after checking
				// for user cancellation
				if (progress->IsCancelRequested())
					return HRESULT_FROM_WIN32(ERROR_CANCELLED);
				break;

			default:
				// anything else counts as a failed I/O
				return HRESULT_FROM_WIN32(err);
			}
		}
	};

	// copy data
	DWORD totalBytes = 0;
	while (totalBytes < inSize)
	{
		// check for a cancellation request
		if (progress->IsCancelRequested())
			return Result(HRESULT_FROM_WIN32(ERROR_CANCELLED));

		// start an asynchronous read for the next buffer-full
		BYTE buf[4096];
		DWORD bytesRead;
		if (!ReadFile(hIn.handle, buf, sizeof(buf), &bytesRead, ovIn.SetOffset(totalBytes)))
		{
			// make sure the error is "I/O pending"
			if (DWORD winErr = GetLastError(); winErr != ERROR_IO_PENDING)
				return Result(HRESULT_FROM_WIN32(winErr));

			// wait for the read to complete
			if (HRESULT hr = WaitOverlapped(hIn.handle, &ovIn, bytesRead); !SUCCEEDED(hr))
				return hr;
		}

		// check for end of file
		if (bytesRead == 0)
			break;

		// start an asynchronous write for the next buffer-full
		DWORD bytesWritten;
		if (!WriteFile(hOut.handle, buf, bytesRead, &bytesWritten, ovOut.SetAppend()))
		{
			// make sure the error is "I/O pending"
			if (DWORD winErr = GetLastError(); winErr != ERROR_IO_PENDING)
				return Result(HRESULT_FROM_WIN32(winErr));

			// wait for the write to complete
			if (HRESULT hr = WaitOverlapped(hOut.handle, &ovOut, bytesWritten); !SUCCEEDED(hr))
				return hr;
		}

		// make sure all the bytes were written as requested
		if (bytesWritten != bytesRead)
			return Result(E_FAIL);

		// update progress
		totalBytes += bytesRead;
		if (progress != nullptr)
			progress->ProgressUpdate(totalBytes);
	}

	// make sure that we copied all bytes expected
	if (totalBytes != inSize)
		return E_FAIL;

	// success
	return Result(S_OK);
}

