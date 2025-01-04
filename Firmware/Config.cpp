// Pinscape Pico - Configuration Storage
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This module handles storing configuration data persistently in the Pico's
// flash memory.
//

// standard library headers
#include <stdio.h>
#include <stdint.h>
#include <memory.h>

// Pico SDK headers
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/flash.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/timer.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>

// local project headers
#include "Pinscape.h"
#include "Main.h"
#include "FlashStorage.h"
#include "Config.h"
#include "JSON.h"
#include "crc32.h"
#include "Logger.h"
#include "Watchdog.h"
#include "Reset.h"
#include "../USBProtocol/VendorIfcProtocol.h"
#include "CommandConsole.h"

// singleton configuration instance
Config config;

// construction
Config::Config()
{
}

// destruction
Config::~Config()
{
}

// ---------------------------------------------------------------------------
//
// Config read/load functions
//

// Load the working copy in RAM from the persistent data in flash
bool Config::Load(PicoReset::BootMode bootMode, JSONParser &json)
{
    // factory default settings
    static const char factoryConfig[] = "";
    size_t factoryConfigSize = _countof(factoryConfig) - 1;

    // presume we won't find a valid config file to load
    isFactorySettings = true;
    const char *effectiveConfig = factoryConfig;
    size_t effectiveConfigSize = factoryConfigSize;
    const char *configFileName = nullptr;

    // check the boot mode
    switch (bootMode)
    {
    case PicoReset::BootMode::FactoryMode:
        // Factory Mode - bypass all stored configuration files, which will
        // use default ("factory") settings for all options
        isSafeMode = true;
        break;

    case PicoReset::BootMode::SafeMode:
        // Safe Mode - bypass the main configuration file, and use the
        // user's Safe Mode configuration file instead, if one is
        // provided.
        configFileName = SAFE_MODE_CONFIG_FILE_NAME;
        isSafeMode = true;
        break;

    default:
        // Normal mode or Uknown mode - the current boot wasn't caused by
        // an early watchdog crash, so load the full config file as normal.
        configFileName = CONFIG_FILE_NAME;
        break;
    }

    if (configFileName != nullptr)
    {
        // open the config file in the flash storage system
        do
        {
            // try opening the file
            FlashStorage::FileInfo fi;
            if (auto openStat = flashStorage.OpenRead(configFileName, fi); openStat != FlashStorage::OpenStatus::OK)
            {
                Log(LOG_INFO, "Config file (%s) not found in flash (status %d); using factory defaults\n",
                    configFileName, static_cast<int>(openStat));
                break;
            }
            
            // it must be at least sizeof(uint32_t), for the appended CRC-32 footer
            if (fi.size < sizeof(uint32_t))
            {
                Log(LOG_ERROR, "Invalid config file size (%s, %d bytes, must be at least 4 bytes)\n", configFileName, fi.size);
                break;
            }

            // get the config file pointer and size, minus the checksum footer
            const char *stream = reinterpret_cast<const char*>(fi.data);
            size_t streamSize = fi.size - sizeof(uint32_t);
            
            // retrieve the stored CRC
            uint32_t crcStored;
            memcpy(&crcStored, stream + streamSize, sizeof(crcStored));

            // compute the actual CRC of the stored text, and test for a match
            uint32_t crcComputed = CRC::Calculate(stream, streamSize, CRC::CRC_32());
            if (crcComputed != crcStored)
            {
                Log(LOG_ERROR, "Config file integrity check failed (%s, %u bytes, checksum stored %08lx, computed %08lx)\n",
                    configFileName, streamSize, crcStored, crcComputed);
                break;
            }

            // Successfully loaded - use the loaded file
            effectiveConfig = stream;
            effectiveConfigSize = streamSize;

            // mark the configuration as valid and using user settings (not factory settings)
            isConfigValid = true;
            isFactorySettings = false;
        }
        while (false);
    }

    // Parse the JSON text contents of the config data
    json.Parse(effectiveConfig, effectiveConfigSize);

    // success
    return !isFactorySettings;
}

// ---------------------------------------------------------------------------
//
// Get the stored config file checksum.  The checksum is stored as a 32-bit
// (4-byte) footer at the end of the text stream.
//
uint32_t Config::GetStoredChecksum(uint8_t fileID)
{
    // try opening the file (note that there's no "close" required)
    FlashStorage::FileInfo fi;
    if (readCache.Open(GetConfigFilename(fileID), fi) != FlashStorage::OpenStatus::OK)
        return 0;

    // it must be at least sizeof(uint32_t), for the appended CRC-32 footer
    if (fi.size < sizeof(uint32_t))
        return 0;

    // retrieve the stored CRC
    uint32_t crcStored;
    memcpy(&crcStored, fi.data + fi.size - sizeof(uint32_t), sizeof(crcStored));
    return crcStored;
}

// ---------------------------------------------------------------------------
//
// Test if the specified config file exists
//
bool Config::ConfigFileExists(uint8_t fileID)
{
    // ask our read cache
    return readCache.FileExists(GetConfigFilename(fileID));
}

// ---------------------------------------------------------------------------
//
// Test the specific config file for a valid internal checksum
//
bool Config::IsConfigFileValid(uint8_t fileID, uint32_t *pChecksum)
{
    // zero the returned checksum, in case we fail before computing the real thing
    if (pChecksum != nullptr)
        *pChecksum = 0;

    // try opening the file (note that there's no "close" required)
    FlashStorage::FileInfo fi;
    if (readCache.Open(GetConfigFilename(fileID), fi) != FlashStorage::OpenStatus::OK)
        return false;

    // it must be at least sizeof(uint32_t), for the appended CRC-32 footer
    if (fi.size < sizeof(uint32_t))
        return 0;

    // retrieve the stored CRC
    uint32_t textSize = fi.size - sizeof(uint32_t);
    uint32_t crcStored;
    memcpy(&crcStored, fi.data + textSize, sizeof(crcStored));

    // compute the checksum over the stored data
    uint32_t crcComputed = CRC::Calculate(fi.data, textSize, CRC::CRC_32());

    // if desired, pass back the computed checksum
    if (pChecksum != nullptr)
        *pChecksum = crcComputed;

    // it's valid if the stored checksum matches the computed checksum
    return crcComputed == crcStored;
}

// ---------------------------------------------------------------------------
//
// Cached config file reader
//
FlashStorage::OpenStatus Config::ReadCache::Open(const char *name, FlashStorage::FileInfo &fi)
{
    // if the filename is null, fail
    if (name == nullptr)
        return FlashStorage::OpenStatus::NotFound;

    // if it matches the open file, return the current contents
    if (this->name != nullptr && strcmp(this->name, name) == 0)
    {
        fi = this->fi;
        return openStat;
    }

    // remember the new filename, and try opening it
    this->name = name;
    openStat = flashStorage.OpenRead(name, this->fi);

    // return the result
    fi = this->fi;
    return openStat;
}

bool Config::ReadCache::FileExists(const char *name)
{
    // if the name is null, return not found
    if (name == nullptr)
        return false;

    // if it's in cache, return the status from cache
    if (this->name != nullptr && strcmp(this->name, name) == 0)
        return flashStorage.FileExists(openStat);

    // cache it and return the result
    this->name = name;
    openStat = flashStorage.OpenRead(name, fi);
    return flashStorage.FileExists(openStat);
}

void Config::ReadCache::Clear(const char *name)
{
    // if the name matches, clear our cached information
    if (name != nullptr && this->name != nullptr && strcmp(name, this->name) == 0)
        this->name = nullptr;
}


// ---------------------------------------------------------------------------
//
// Get the stored config file pointer and size.  Returns null if
// the configuration hasn't been loaded, or is missing or invalid.
//
const char *Config::GetConfigFileText(size_t &size, uint8_t fileID)
{
    // try opening the file through our cache (note that there's no "close"
    // required; reads through the mini file system are stateless, as they
    // simply locate the file's data in flash memory)
    FlashStorage::FileInfo fi;
    if (readCache.Open(GetConfigFilename(fileID), fi) != FlashStorage::OpenStatus::OK)
        return nullptr;

    // it must be at least sizeof(uint32_t), for the appended CRC-32 footer
    if (fi.size < sizeof(uint32_t))
        return nullptr;

    // set the size, exclusive of the 32-bit CRC footer, and return the stream pointer
    size = fi.size - sizeof(uint32_t);
    return reinterpret_cast<const char*>(fi.data);
}

// Get a config filename by file ID
const char *Config::GetConfigFilename(uint8_t fileID)
{
    switch (fileID)
    {
    case PinscapePico::VendorRequest::CONFIG_FILE_MAIN:
        return CONFIG_FILE_NAME;

    case PinscapePico::VendorRequest::CONFIG_FILE_SAFE_MODE:
        return SAFE_MODE_CONFIG_FILE_NAME;

    default:
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
//
// Config save
//

bool Config::SavePage(const uint8_t *data, size_t len, int pageNo, int nPages, uint32_t crc, uint8_t fileID)
{
    // Extend the watchdog timeout to allow for relatively slow file system
    // operations.  We certainly still want to reboot if we crash or freeze,
    // but we don't want to misinterpret a lengthy flash operation (a sector
    // erase, for example) as a crash.
    WatchdogTemporaryExtender wte(250);

    // select the filename according to the file ID
    const char *filename = GetConfigFilename(fileID);

    // make sure the file ID is valid
    if (filename == nullptr)
    {
        Log(LOG_ERROR, "Config: invalid config file ID 0x%02x in SavePage request\n", fileID);
        return false;
    }
    
    // If this is the first page, open the file
    if (pageNo == 0)
    {
        // close any existing handle
        if (flashHandle >= 0)
            flashStorage.CloseWrite(flashHandle);

        // Open a new handle.  Reserve 128K for the file if it doesn't
        // already exist.  (128K is an arbitrary upper limit, based on
        // the expected range of sizes for real-world usage.  We expect
        // that real file sizes will be on the order of 16KB to 32KB,
        // so 128K should leave us with lots of headroom for expansion.)
        flashHandle = flashStorage.OpenWrite(filename, nPages*PinscapePico::VendorRequest::CONFIG_PAGE_SIZE, 128*1024);
        if (flashHandle < 0)
        {
            Log(LOG_ERROR, "Config: unable to open flash config file (%s) for writing\n", filename);
            return false;
        }

        // if we cached the file for reading, forget that old copy, as
        // it's now invalid
        readCache.Clear(filename);
    }
    else if (flashHandle < 0)
    {
        Log(LOG_ERROR, "Config: config file (ID %02x) isn't open for writing page %d\n", fileID, pageNo);
        return false;
    }

    // write the data
    bool ok = false;
    if (!flashStorage.WriteFile(flashHandle, data, len))
        Log(LOG_ERROR, "Config: error writing to config file (%s, %d bytes)\n", filename, len);
    else
        ok = true;

    // if this is the last page, close the file
    if (pageNo + 1 == nPages)
    {
        // append the CRC32 value to the file
        if (!flashStorage.WriteFile(flashHandle, &crc, sizeof(crc)))
        {
            Log(LOG_ERROR, "Config: error appending CRC to config file (%s)\n", filename);
            ok = false;
        }

        // close the file
        if (!flashStorage.CloseWrite(flashHandle))
        {
            Log(LOG_ERROR, "Config: error closing config (%s)\n", filename);
            ok = false;
        }

        // forget the handle
        flashHandle = -1;
    }

    // return the result
    return ok;
}

// ---------------------------------------------------------------------------
//
// Config erase
//
bool Config::EraseConfig(uint8_t fileID)
{
    // check for *.*
    if (fileID == PinscapePico::VendorRequest::CONFIG_FILE_ALL)
    {
        // clear the read cache
        readCache.Clear();
        
        // Delete all files.  Use "silent" mode, which succeeds if a file
        // doesn't exist, since it satisfies the goal here if the file
        // wasn't there to start with.
        bool ok = flashStorage.Remove(CONFIG_FILE_NAME, true);
        ok = flashStorage.Remove(SAFE_MODE_CONFIG_FILE_NAME, true) && ok;
        return ok;
    }

    // check for a valid file
    const char *filename = GetConfigFilename(fileID);
    if (filename == nullptr)
        return false;

    // discard the file from cache
    readCache.Clear(filename);

    // delete the file, using "silent" mode (no error if it doesn't already exist)
    return flashStorage.Remove(filename, true);
}

// ---------------------------------------------------------------------------
//
// Factory reset.  This clears the configuration data and the EEPROM area.
//
bool Config::FactoryReset()
{
    // the flash operations might take a while, so extend the crash-detect timeout
    WatchdogTemporaryExtender wte(1000);

    // Reformat and remount the FlashStorage file system
    bool ok = flashStorage.Format(FLASHSTORAGE_CENTRAL_DIRECTORY_SIZE);
    if (!flashStorage.Mount(FLASHSTORAGE_CENTRAL_DIRECTORY_SIZE))
        ok = false;

    // still alive
    watchdog_update();

    // log status on success
    if (ok)
        Log(LOG_INFO, "Factory reset successful; all flash config data cleared via littlefs reformat\n");

    // return the result
    return ok;
}


// ---------------------------------------------------------------------------
//
// Persistent struct load/save functions.  These are for use by subsystems
// for storing program-generated data (separate from the user-supplied
// configuration file) persistently in flash.
//

bool Config::SaveStruct(const char *name, const void *src, size_t sizeofStruct, size_t reserveSize)
{
    // Open or create the file.  Use the larger of the actual struct
    // size or the reserve size, just in case the reserve size was
    // specified as smaller (which would be a parameter error, but one
    // we'll let slide with this correction - the caller might have
    // gotten confused into thinking that the reserve size is an extra
    // amount to add to the struct size rather than simply the initial
    // file size).
    auto h = flashStorage.OpenWrite(name, sizeofStruct, std::max(sizeofStruct, reserveSize));
    if (h < 0)
    {
        Log(LOG_ERROR, "Error saving struct '%s': file open failed\n", name);
        return false;
    }
    
    // write the data
    bool ok = true;
    if (!flashStorage.WriteFile(h, src, sizeofStruct))
        ok = false, Log(LOG_ERROR, "Error saving struct '%s': write failed\n", name);
    
    // close the file
    if (!flashStorage.CloseWrite(h))
        ok = false, Log(LOG_ERROR, "Error saving struct '%s': file close failed\n", name);

    // return the status indication
    return ok;
}

bool Config::LoadStruct(const char *name, void *dst, size_t sizeofStruct, bool *fileExists)
{
    // Zero the caller's struct.  This guarantees that fields we don't
    // fill from the file are zeroed, which the caller should treat as
    // factory default settings for whatever the struct represents.
    memset(dst, 0, sizeofStruct);

    // open the file in littlefs
    FlashStorage::FileInfo fi;
    auto stat = flashStorage.OpenRead(name, fi);

    // tell the caller the file exists (if it cares)
    if (fileExists != nullptr)
        *fileExists = flashStorage.FileExists(stat);

    // if the file open failed, return "not loaded"
    if (stat != FlashStorage::OpenStatus::OK)
    {
        // log the error
        Log(LOG_INFO, "Flash struct '%s' not found (flash storage status code %d); using defaults\n",
            name, static_cast<int>(stat));
        
        // return false for "not loaded"
        return false;
    }

    // Load the smaller of the file size or the caller's struct size.
    //
    // If the caller's struct is smaller, we'll simply truncate the
    // file data to the caller's size; this presumably means that the
    // struct was saved by a newer version of the firmware than what's
    // currently running, and the newer version added new fields to
    // this struct that aren't supported in this version.  Those new
    // fields would be meaningless to this version of the caller, so
    // there's no loss of functionality in truncating them.
    //
    // If the file struct is smaller than the caller's, it probably
    // means that we're running a newer version of the firmware than the
    // version that saved the struct in flash (i.e., the firmware has
    // bee updated since the struct was last saved).  The old version
    // that saved the struct was missing some fields that are in the
    // new version, so it doesn't have anything meaningful to put in
    // the new space in the caller's struct.  We'll simply leave those
    // fields zeroed, which the caller should interpret to mean that
    // the factory defaults should be applied - which is perfectly
    // appropriate for new features added since the user last saved
    // settings.
    size_t copySize = std::min(static_cast<size_t>(fi.size), sizeofStruct);
    memcpy(dst, fi.data, copySize);

    // success
    return true;
}
