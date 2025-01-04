// Pinscape Pico - Configuration Storage
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module handles the Pinscape Pico firmware's configuration data
// storage in flash memory.  The configuration data is created on the
// host side (typically via the Pinscape Config Tool on Windows, but
// it could also be created manually, or by a separate program), and
// is stored on the Pico in flash memory.
//
// The configuration data is stored in a JSON-like format ("-like" in
// that it relaxes some of the official rules, such as not requiring
// quotes around property names in object maps).  JSON isn't nearly as
// compact as most binary formats, but we can easily afford the extra
// space it requires, given that the standard Pico comes with 2MB of
// flash, and we only need a few hundred KB of that for the firmware
// program.  JSON also requires more CPU time to parse than most binary
// formats would, but we can afford that as well, since we only have to
// parse the config data once at program startup.  We get some nice
// benefits in exchange.  One is that JSON is easy for humans to read
// and write, which is helpful for debugging and testing, and makes it
// easier for end users to do their own troubleshooting.  Another is
// that JSON is self-describing and extensible, which makes it much
// easier to use for data interchange between programs that might be
// based on slightly different versions of the underlying data model
// (e.g., using a newer Config Tool version with an older firmware
// version).  Binary formats tend to require everyone to agree on the
// exact version of the data model, because that's the only way to tell
// which byte means what.  This same property makes JSON data easier to
// access from new programs and third-party programs.
//
// The persistent storage uses the Pico's built-in flash memory, with
// the low-level storage managed through our FlashStorage mini file
// system (see FlashStorage.h).


#pragma once

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <list>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/timer.h>

// project headers
#include "Reset.h"
#include "FlashStorage.h"

// external/forward declarations
class Config;
class ConsoleCommandContext;
class JSONParser;


// global singleton
extern Config config;

// Configuration manager.  This is instantiated as a global singleton.
class Config
{
protected:
    // forward declarations
    struct EEPROMStruct;
    
public:
    Config();
    ~Config();

    // Load the configuration.  If the flash memory space contains a
    // valid config file, parses the contents into the JSON object.
    // Returns true if a config was found and parsed successfully, false
    // if not.
    //
    // If bootMode is SafeMode or FactoryMode, we'll set the global Safe
    // Mode flag and load the user's safe mode config or the factory
    // default settings (according to the mode), on the theory that the
    // crash that led to the reduced functionality mode was caused by
    // something in the configuration we loaded last time.
    bool Load(PicoReset::BootMode bootMode, JSONParser &json);

    // Are we using factory settings?
    bool IsFactorySettings() const { return isFactorySettings; }

    // Are we in safe mode?
    bool IsSafeMode() const { return isSafeMode; }

    // Get the stored config file pointer and size.  Returns null if
    // the configuration hasn't been loaded, or is missing or invalid.
    // fileID is one of the VendorRequest::CONFIG_FILE_xxx constants,
    // selecting which configuration file to retrieve.
    const char *GetConfigFileText(size_t &size, uint8_t fileID);

    // test if a config file of the specified type exists
    bool ConfigFileExists(uint8_t fileID);

    // Get the filename for a config file by ID
    const char *GetConfigFilename(uint8_t fileID);

    // Test the stored flash-resident config data for validity.  Checks
    // the integrity of the stored "footer" fields at the end of flash,
    // and if they're valid, computes the checksum of the stored flash
    // data and tests it against the checksum stored in the footer.
    // Returns true if all tests pass, false if any test fails.  If
    // 'checksum' is not null, fills in *checksum with the computed
    // checksum (or zero if a checksum couldn't be computed).
    bool IsConfigFileValid(uint8_t fileID, uint32_t *checksum);

    // Get the stored checksum from the flash config data
    uint32_t GetStoredChecksum(uint8_t fileID);

    // Save one page of config data to flash.  nPages gives the total
    // number of pages of config data comprising the entire file, and
    // pageNo gives the current page, 0 to nPages-1.  The length must
    // always be the config page size of 4KB (4096 bytes).
    //
    // 'crc' is only used on the last page.  This must contain the
    // CRC-32 checksum of the entire config file; it's stored in the
    // flash space alongside the text contents, and the config loader
    // checks it against the CRC-32 computed from the RAM copy when
    // loading the config file in a future session, to validate the
    // integrity of the recovered file data.  If the config data came
    // from the host via the USB connection (as opposed to being
    // generated locally), it's ideal to have the *host* calculate the
    // CRC-32 of its original source data, and send that along with the
    // USB request.  That gives us an end-to-end integrity check,
    // ensuring that the RAM copy we recover on load matches the
    // original config file text from the host machine.  The vendor
    // interface API for sending the config file does exactly that.
    //
    // 'fileID' is one of the VendorRequest::CONFIG_FILE_xxx constants
    // defined in ../USBProtocols/VendorIfcProtocol.h, specifying which
    // config file we're storing (main config, Safe Mode config).
    //
    // Returns true on success, false if any error occurs.
    bool SavePage(const uint8_t *data, size_t len, int pageNo, int nPages, uint32_t crc, uint8_t fileID);

    // Erase the JSON configuration file.  This erases the config file
    // from the flash storage area.  It doesn't affect any of the
    // saved-structure files, so device-generated data outside of the
    // config file, such as calibration data, won't be affected.
    // This erases the selected file, or all config files (Main, Safe
    // Mode) if the file ID is CONFIG_FILE_ALL.
    bool EraseConfig(uint8_t fileID);

    // Perform a full factory reset, by deleting all configuration data
    // stored in flash.  This deletes everything by reformatting the
    // file storage area in flash, effectively deleting all of the
    // stored persistent data files.  This includes the JSON
    // configuration file and all of the saved data structures, so it
    // will make the device act like a new Pico where Pinscape has just
    // been installed for the first time.  Most of the configuration
    // settings are loaded during initialization only, so the factory
    // settings won't go into effect until the next Pico reset.  Returns
    // true on success, false if any error occurs.
    bool FactoryReset();

    // Save/load a data structure to/from flash, in the file storage
    // portion of the flash memory space.  These are for use by
    // subsystems to persistently store program-generated structs,
    // separately from the user-supplied configuration file.  Stored
    // structs aren't affected by configuration updates, and they don't
    // affect the configuration file.
    //
    // The name is simply a Flash Storage system filename to uniquely
    // identify the struct.  It should be unique among all Flash Storage
    // objects throughout the system.  (Given that the name is an
    // arbitrary string up to 255 characters, and that this facility is
    // lightly used, ad hoc naming should be more than adequate - I see
    // no need for any sort of name registry.)
    //
    // Save() replaces any existing persistent object of the same name,
    // and otherwise creates a new one.  sizeofStruct is the actual size
    // of the data to save, and reserveSize is the amount of space to
    // reserve for the struct in flash, taking into account the
    // possibility that fields will be added in the future.  The
    // reserved size will be rounded up automatically to a full flash
    // sector, which is 4K, since this is the minimum increment of flash
    // space that can be erased to make room for the file.  Most of our
    // structs are much smaller than this, so the minimum sector
    // increment naturally reserves considerable extra space to allow
    // for future growth.  Even so, we let the caller say explicitly how
    // much space to reserve.  Note that reserveSize is the full size to
    // be reserved, as opposed to extra space - this isn't added to the
    // struct size but is used all by itself as the initial file size.
    //
    // On retrieval, if the stored struct doesn't exist, the caller's
    // struct is filled with all zero bytes, and the function returns
    // false.  If the stored object exists but is smaller than the
    // caller's struct, the file data are loaded into the struct at the
    // bottom end, and the extra bytss are zeroed.  In both cases (no
    // file, short file), the idea is that bytes that weren't stored in
    // a previous program session are filled with zeroes, which the
    // caller should always use as the default/initial value for the
    // data represented by the struct elements.  This allows the struct
    // to be expanded in newer versions of the software with seamless
    // compatibility, provided that new fields are always added at the
    // end of the existing ones, and that existing fields are never
    // removed or resized.
    //
    // If the stored struct is larger than the caller's struct, the
    // excess bytes are simply discarded.  This could happen if the user
    // reverts to an older firmware version where the struct was
    // smaller.  This also tends to work seamlessly across versions;
    // whatever new information is stored in the extra fields is simply
    // ignored by the older software version where the fields (and
    // presumably the new feature or function they're associated with)
    // don't exist.
    //
    // A false return on load means that the file wasn't found OR an
    // error occurred loading it.  The caller can distinguish the two
    // possibilities by providing a pointer to a bool in 'fileExists'
    // that we'll fill in.  In either case, the struct is filled in
    // with all zeroes as defaults.
    //
    // A false return on save is always an error (there's no harmless
    // "not found" case for save), but there's probably not much that a
    // caller can do about it; whatever underlying condition caused the
    // failure is likely to cause retries to fail as well.  As with
    // loads, we'll at least log the error to help with troubleshooting.
    bool LoadStruct(const char *name, void *dst, size_t sizeofStruct, bool *fileExists = nullptr);
    bool SaveStruct(const char *name, const void *src, size_t sizeofStruct, size_t reserveSize);

protected:
    // Is the configuration existant and valid?
    bool isConfigValid = false;
    
    // Are we using factory settings?  This is set if the caller
    // explicitly loads factory settings (e.g., booting in safe mode),
    // OR we're forced to use factory settings because the flash config
    // file is missing or invalid.
    bool isFactorySettings = false;

    // Are we in safe mode?  The main loop sets this before loading the
    // configuration if it detects that the device was reset due to a
    // software failure in the prior session, in an attempt to prevent
    // the crash from recurring so that the user has a chance to
    // investigate and update the configuration if necessary.
    bool isSafeMode = false;

    // FlashStorage handle to open config file during save
    int flashHandle = -1;

    // Config file names - normal and safe mode
    constexpr static const char *CONFIG_FILE_NAME = "config.json";
    constexpr static const char *SAFE_MODE_CONFIG_FILE_NAME = "safemode.json";

    // Last config file requested through GetConfigFileText().  We use
    // this to cache an 'open'.  Since opens are stateless in the mini
    // file system, it doesn't cost anything to keep the last open file
    // indefinitely.
    struct ReadCache
    {
        // open a file, using the last file if already open
        FlashStorage::OpenStatus Open(const char *name, FlashStorage::FileInfo &fi);

        // test if a file exists
        bool FileExists(const char *name);

        // clear the cached copy if it matches the given name
        void Clear(const char *name);

        // clear unconditionally
        void Clear() { name = nullptr; }

        // name of cached file, or null if nothing cached
        const char *name = nullptr;

        // results from last open
        FlashStorage::OpenStatus openStat = FlashStorage::OpenStatus::NotFound;
        FlashStorage::FileInfo fi;
    };
    ReadCache readCache;
};
