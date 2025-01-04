// Pinscape Pico Device - Feedback Controller API
// Copyright 2024 Michael J Roberts / BSD-3-Clause license, 2025 / NO WARRANTY

#pragma once

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
#include <Windows.h>
#include <shlwapi.h>
#include <usb.h>
#include <winusb.h>
#include <hidsdi.h>
#include <process.h>
#include <cfgmgr32.h>
#include <SetupAPI.h>
#include <shellapi.h>
#include <winerror.h>
#include "crc32.h"
#include "BytePackingUtils.h"
#include "PinscapePicoAPI.h"
#include "Utilities.h"

namespace PinscapePico
{
	// Enumerate RP2 Boot drives.  This returns a list of root
	// paths representing the attached Pico devices that are
	// currently in ROM Boot Loader mode.  The root paths are
	// the virtual disk drives that the boot loaders on the
	// respective devices present to the operating system.
	// Copy a UF2 file to one of these drives will install the
	// UF2 file as new firmware on the target Pico and will
	// reboot the Pico into the newly installed firmware.
	struct RP2BootDevice
	{
        // forwards
		class IProgressCallback;

		RP2BootDevice(const TCHAR *path, std::string bootloaderVersion) :
			path(path), bootloaderVersion(bootloaderVersion)
		{ }

		RP2BootDevice(const RP2BootDevice &dev) :
			path(dev.path),
			bootloaderVersion(dev.bootloaderVersion),
			boardVersion(dev.boardVersion),
			tags(dev.tags)
		{ }

		// Reset the Pico,   This returns the Pico to normal operating
		// mode from Boot Loader Mode.  Returns true on success, false 
		// on failure.
		// 
		// To accomplish the reset, this routine writes a specially
		// contrived UF2 file to the Pico that doesn't change any data in
		// flash, purely to trigger the automatic reboot that occurs
		// after any successful boot loader UF2 transfer.
		bool RebootPico();

		// Delete Pinscape Pico configuration data from the Pico.
		// This overwrites the small control block area that Pinscape
		// uses to store its map of saved data.  (It's the equivalent
		// of the main allocation table on a FAT drive.)  After the
		// operation completes, the Pico will reset (since this is a
		// side effect of any UF2 transfer), which will launch the
		// current firmware in flash.  Assuming that Pinscape is
		// installed, this will launch Pinscape into its factory
		// default configuration, since the operation erases all
		// previously saved configuration data.
		bool EraseConfigData();

		// Erase the Pico's entire flash memory space.  Overwrites 
		// the entire 2MB flash space with '1' bits, leaving the 
		// flash completely empty.  The erased data are unrecoverable.
		// This should obviously be used with caution; it's intended
		// as a last resort to remove corrupted flash contents that 
		// are causing the installed firmware to crash even after a
		// reinstall.  This should never be necessary with Pinscape,
		// because Pinscape stores its files in a structured format
		// that can be reset without clearing the whole flash space,
		// via the much lighter-weight EraseConfigData().  But a
		// complete wipe might be needed for non-Pinscape software.
		// 
		// This operation can be quite time-consuming, so it accepts
		// an optional progress callback; pass nullptr if no progress
		// UI is desired.
		bool EraseWholeFlash(IProgressCallback *progress);

		// Write a mechanically synthesized UF2 file to the Pico.  The
		// file contents are constructed on the fly by repeatedly invoking
		// the provided callback.  Each callback invocation supplies one
		// 256-byte block.  Returns true on success, false on failure.
		bool WriteUF2(uint32_t startingAddress, uint32_t numBlocks,
			std::function<void(uint8_t *buf, uint32_t blockNum)> fillBlockPayload,
			IProgressCallback *progress = nullptr, const TCHAR *sourceDescription = nullptr);

		// File system root path of the Pico boot loader's virtual
		// disk drive (e.g., "D:\")
		std::basic_string<TCHAR> path;

		// UF2 Bootloader version string, as reported in INFO_UF2.TXT.
		std::string bootloaderVersion;

		// Board ID version suffix, if any.  This is the third
		// token in the board ID string above, parsed out into this
		// separate field for the client program's convenience.  This
		// is always empty for the current (original) version of the
		// Pico, but we break this out in anticipation that future
		// revisions might add a version suffix per the Microsoft
		// spec for the boot loader ID file.  This can also be
		// obtained from the tag string map's "board-id" element;
		// it's separated here for the caller's convenience.
		std::string boardVersion;

		// "Name: Value" tags found in the INFO_UF2.TXT file on
		// the boot loader's virtual drive.  The Name keys are
		// all converted to lower-case, since we assume that the
		// well-known name strings are meant to be case-insensitive.
		std::unordered_map<std::string, std::string> tags;

        // Enumerate currently attached boot devices
		using RP2BootDeviceList = std::list<RP2BootDevice>;
		static RP2BootDeviceList EnumerateRP2BootDrives();

		// Enumerate new RP2 Boot drives added since a prior check.
		// This enumerates the current drives, skipping the drives that
		// also appear in the "before" list.  This is intended to help
		// with two workflows:
		//
		// - Setting up a brand new Pico device.  The UI should instruct
		// the user to make sure the new device is unplugged, then scan
		// for a list of current RP2 Boot devices and store it as the
		// "before" list.  Now ask the user to plug in the new device
		// (holding down the BOOTSEL button to enter boot loader mode).
		// Scan periodically (say, every 500ms) for new devices; when a
		// single new device appears, it's fairly safe to assume that
		// it's the Pico that the user just plugged in.
		//
		// - Updating the Pinscape firmware on an existing Pico.  When
		// the user initiates the update, do a scan to build a "before"
		// list of any already-attached devices, then send a BOOTSEL
		// command to the Pinscape firmware via its USB control
		// interface.  Now scan periodically (e.g., at 500ms intervals)
		// for new boot devices.  When a single new device appears, we
		// can assume that it's the Pico that we commanded to reboot
		// into the boot loader.
		// 
		// In both cases, the caller should make sure that exactly one
		// new device appeared in the "after" scan.  If more than one
		// device appears, it's impossible to know which is the correct
		// target device.  This should be considered an error condition
		// requiring user intervention, either by the user manually
		// unplugging the "other" devices, or by the user identifying
		// the desired target device via a list presented in the UI.
		// 
		// We leave it up to the client program to provide a periodic
		// scanning mechanism, because that will require a timed wait
		// that has to be coordinated with the program's UI layer.  A
		// simple blocking wait routine here would be unusable for most
		// GUI callers, since any decent GUI would keep the UI
		// responsive while waiting for the new drive to show up.
		static RP2BootDeviceList EnumerateNewRP2BootDrives(const RP2BootDeviceList &before);

		// UF2 file block - see https://github.com/microsoft/uf2?tab=readme-ov-file
		//
		// A UF2 file consists of a collection of one or more of these blocks;
		// each block contains one page (256 bytes on Pico) of flash data to 
		// store at a specified location in flash.  The UF2 format doesn't have
		// a separate file header; it's just an array of these blocks.
		struct UF2_Block
		{
			UF2_Block() { }
			UF2_Block(uint32_t targetAddr, uint32_t blockNo, uint32_t numBlocks, const char *src = nullptr) :
				targetAddr(targetAddr), blockNo(blockNo), numBlocks(numBlocks)
			{
				if (src != nullptr)
					memcpy(this->data, src, 256);
			}

			uint32_t magicStart0 = 0x0A324655;
			uint32_t magicStart1 = 0x9E5D5157;
			uint32_t flags = F_FAMILYID_PRESENT;
			uint32_t targetAddr = 0;
			uint32_t payloadSize = 256;
			uint32_t blockNo = 0;
			uint32_t numBlocks;
			uint32_t fileSizeOrFamilyID = FAMILYID_RP2040;
			uint8_t data[476]{ 0 };
			uint32_t magicEnd = 0x0AB16F30;

			// flags
			static const uint32_t F_NOT_MAIN_FLASH       = 0x00000001;  // do not copy to flash
			static const uint32_t F_FILE_CONTAINER       = 0x00001000;  // member of a zip/tar style archive
			static const uint32_t F_FAMILYID_PRESENT     = 0x00002000;  // fileSize contains familyID, not a size
			static const uint32_t F_MD5_CHECKSUM_PRESENT = 0x00004000;  // MD5 checksum is present
			static const uint32_t F_EXT_TAGS_PRESENT     = 0x00008000;  // extension tags present

			// familyID constants
			static const uint32_t FAMILYID_RP2040 = 0xe48bff56;
		};

		// Progress callback interface for InstallFirmware
		class IProgressCallback
		{
		public:
			// Initialize.  The task processor calls this just before
			// starting the task.  Passes in the copy size and file 
			// names for display in the UI.
			virtual void ProgressInit(const TCHAR *from, const TCHAR *to, uint32_t fileSizeBytes) { }

			// Update progress for number of bytes copied so far.
			// The task calls this periodically to update the UI
			// with the progress so far.
			virtual void ProgressUpdate(uint32_t bytesCopied) { }

			// Accelerate the task.  The task processor calls this after
			// completing the task or when exiting due to error.
			virtual void ProgressFinish(bool success) { }

			// Check for cancellation request.  The task calls this
			// periodically to check for a user request to cancel
			// the task.  The task isn't obligated to respect the
			// request at all, and even if does, the effect might
			// not be immediate.  The UI should always  
			virtual bool IsCancelRequested() { return false; }
		};

		// Install firmware onto a Pico via its RP2 Boot Loader virtual
		// disk drive.  This takes a path to a UF2 file (containing a
		// compiled Pico firmware program - UF2 is a binary executable
		// file format that serves as the Pico's equivalent of a Windows
		// .EXE file) and the root path of a Pico RP2 Boot Loader virtual
		// disk.  The Pico automatically reboots into the newly installed
		// firmware immediately after the installation successfully
		// completes, so the virtual disk drive should disappear from the
		// system after this returns.
		// 
		// The UF2 file should be provided as a fully-qualified absolute
		// file system path.
		// 
		// The destination drive path should be a path obtained from an
		// RP2 Boot device enumeration via EnumerateRP2BootDrives().  We
		// don't make any attempt to validate the drive here; we just
		// copy the file.  If the destination is just an ordinary file
		// system path, the effect will be to simply copy the UF2 file
		// to the destination folder.  (The caller could conceivably
		// check to see if the drive root path is still valid after a
		// short delay following a successful copy, and delete the newly
		// created destination file if so.  If the target really is a
		// valid RP2 Boot drive, the Pico should reboot immediately
		// after the copy completes, at which point the RP2 drive should
		// dismount, so you shouldn't see the destination drive at all,
		// let alone a copy of the destination file, a short time after
		// the copy completes.  So seeing the destination file still
		// mounted is a sign that something went wrong, probably that
		// the wrong destination drive was selected.  On the other hand,
		// it's probably better to just leave the detritus in place and
		// let the user manually delete it, since there's more potential
		// for harm in deleting a file than in creating a stray file.)
		//  
		// The progress callback interface is optional.
		static HRESULT InstallFirmware(const TCHAR *uf2FilePath,
			const TCHAR *rp2BootPath, IProgressCallback *progress);
	};
}

