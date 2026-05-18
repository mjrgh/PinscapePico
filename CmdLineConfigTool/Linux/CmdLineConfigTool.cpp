// Pinscape Pico Config Tool - Linux Version
// Copyright 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Linux command-line interface to Pinscape Pico configuration functions
// Uses libusb for USB device communication

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <cstdarg>
#include <algorithm>
#include <regex>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <memory>
#include <list>

#include "PinscapeVendorInterface.h"
#include "FeedbackControllerInterface.h"
#include "../../Firmware/JSON.h"
#include "../../PinscapeVersion.h"

using namespace PinscapePico::Linux;

// Version information
#define PINSCAPE_VERSION PINSCAPE_PICO_VERSION_STRING

// Configuration file type constants (from VendorIfcProtocol.h)
namespace ConfigFile {
    constexpr uint8_t MAIN = 0x00;
    constexpr uint8_t SAFE_MODE = 0x01;
    constexpr uint8_t ALL = 0xFF;
}

// Get build timestamp
static const char *GetBuildTimestamp() {
    static char buf[32] = {0};
    if (buf[0] == 0) {
        // Parse __DATE__ and __TIME__ macros
        const char *date = __DATE__;
        const char *time = __TIME__;

        // Month lookup
        const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        int mon = 1;
        for (const char *p = months; *p; p += 3, ++mon) {
            if (strncmp(p, date, 3) == 0)
                break;
        }

        // Format: YYYYMMDDhhmmss
        snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d",
                atoi(date + 7), mon, atoi(date + 4),
                atoi(time), atoi(time + 3), atoi(time + 6));
    }
    return buf;
}

// ========================================================================
// Error handlers
// ========================================================================

[[noreturn]] static void ErrorExit(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

[[noreturn]] static void ErrorExitFmt(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    fprintf(stderr, "\n");
    exit(1);
}

[[noreturn]] static void ErrorExitWithDeviceList(const std::list<DeviceInfo> &devices, const char *msg) {
    printf("%s\n", msg);
    printf("Available devices:\n");
    printf("  Unit   Hardware ID        Name\n");

    if (devices.empty()) {
        printf("\nNo Pinscape units were detected.\n");
    } else {
        for (const auto &dev : devices) {
            printf("  %4d   %-18s %s\n", dev.unitNumber,
                   dev.serialNumber.c_str(), dev.unitName.c_str());
        }
        printf("\nYou can use the unit number, name, or hardware ID in the --id option.\n");
    }
    exit(1);
}

// ========================================================================
// Configuration file handling
// ========================================================================

static std::string ReadConfigFile(const char *filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        ErrorExitFmt("Error: Cannot open file '%s'", filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static void WriteConfigFile(const char *filename, const std::vector<uint8_t>& data) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        ErrorExitFmt("Error: Cannot create file '%s'", filename);
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!file.good()) {
        ErrorExitFmt("Error: Failed to write to file '%s'", filename);
    }
}

// ========================================================================
// Command implementations
// ========================================================================

static void ShowDeviceList() {
    std::vector<DeviceInfo> devices;

    if (!PinscapeVendorInterface::EnumerateDevices(devices)) {
        ErrorExit("Error enumerating device paths");
    }

    // list the devices
    int nErrors = 0;
    printf("Unit   Hardware ID        Name\n");
    for (const auto& dev : devices) {
        printf("%4d   %-18s %s\n", dev.unitNumber, dev.serialNumber.c_str(), dev.unitName.c_str());
    }

    if (devices.empty()) {
        printf("\nNo Pinscape Pico devices found.\n");
        printf("Please check that:\n");
        printf("  1. Device is connected via USB\n");
        printf("  2. Device firmware is running (not in bootloader mode)\n");
        printf("  3. USB permissions are set correctly (may need udev rules)\n");
    }
}



// Forward declaration of Statistics struct (from VendorIfcProtocol.h)
namespace PinscapePico {
    struct __attribute__((packed)) Statistics {
        uint16_t cb;
        uint16_t reserved0;
        uint32_t reserved1;
        uint64_t upTime;
        uint64_t nLoops;
        uint64_t nLoopsEver;
        uint32_t avgLoopTime;
        uint32_t maxLoopTime;
        uint64_t nLoops2;
        uint64_t nLoopsEver2;
        uint32_t avgLoopTime2;
        uint32_t maxLoopTime2;
        uint32_t heapSize;
        uint32_t heapUnused;
        uint32_t arenaSize;
        uint32_t arenaAlloc;
        uint32_t arenaFree;
    };
}

// Helper function to format numbers with comma separators
static std::string FormatNumber(uint64_t n) {
    std::string s = std::to_string(n);
    int insertPosition = s.length() - 3;
    while (insertPosition > 0) {
        s.insert(insertPosition, ",");
        insertPosition -= 3;
    }
    return s;
}

// ========================================================================
// Original working command implementations
// ========================================================================

static void GetConfig(const char *filename, uint8_t fileId = 0) {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    std::vector<uint8_t> config;
    if (!device.GetConfig(config, fileId)) {
        ErrorExit("Error: Failed to get configuration from device");
    }

    WriteConfigFile(filename, config);
    printf("Configuration data saved to %s\n", filename);
}

static void GetConfigToStdout(uint8_t fileId = 0) {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    std::vector<uint8_t> config;
    if (!device.GetConfig(config, fileId)) {
        ErrorExit("Error: Failed to get configuration from device");
    }

    fwrite(config.data(), 1, config.size(), stdout);
}

static void PutConfig(const char *filename, uint8_t fileId = 0) {
    std::string config_str = ReadConfigFile(filename);
    std::vector<uint8_t> config(config_str.begin(), config_str.end());

    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.PutConfig(config, fileId)) {
        ErrorExit("Error: Failed to send configuration to device");
    }

    printf("Configuration file update succeeded\n");
}

static void Reboot() {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.Reboot()) {
        ErrorExit("Reset request failed");
    }

    printf("Reset succeeded\n");
}

static void RebootSafeMode() {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.RebootSafeMode()) {
        ErrorExit("Safe Mode reset request failed");
    }

    printf("Safe Mode reset succeeded\n");
}

static void EraseConfig(uint8_t fileId = ConfigFile::ALL) {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.EraseConfig(fileId)) {
        ErrorExit("Error erasing the device's configuration file");
    }
    printf("Device configuration file erased; factory defaults restored\n");
}

static void FactoryReset() {
    // Confirm
    printf("Warning: this will delete the configuration file and all other\n"
        "saved settings on the device, including plunger calibration data.\n"
        "If you only wish to clear the settings file, use --put-config to\n"
        "send an empty text file.\n"
        "\n"
        "Do you really want to delete all configuration data? [y/N] ");

    char buf[128];
    if (fgets(buf, sizeof(buf), stdin) == nullptr || (buf[0] != 'y' && buf[0] != 'Y')) {
        printf("Canceled - no changes made\n");
        return;
    }

    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.FactoryReset()) {
        ErrorExit("Error executing factory reset");
    }
    printf("Success - all saved settings deleted, factory defaults restored\n");
}

static void ShowStats(PinscapeVendorInterface *device) {
    // retrieve the statistics
    std::vector<uint8_t> statsData;
    if (!device->GetStatistics(statsData)) {
        printf("Error reading stats\n");
        return;
    }

    // Check minimum size for basic stats
    if (statsData.size() < 40) {
        printf("Error: Statistics data too small (%zu bytes)\n", statsData.size());
        return;
    }

    // Parse statistics structure - handle both old and new formats
    PinscapePico::Statistics stats;
    memset(&stats, 0, sizeof(stats));
    memcpy(&stats, statsData.data(), std::min(statsData.size(), sizeof(stats)));

    // interpret the up time into days, hours, minutes, and seconds
    int days = static_cast<int>(stats.upTime / 1000000 / 86400);
    int sec = static_cast<int>((stats.upTime / 1000000) % 86400);
    char dayStr[32] = "";
    if (days != 0)
        snprintf(dayStr, sizeof(dayStr), "%d day%s, ", days, days == 1 ? "" : "s");

    // format the statistics
    printf("Statistics:\n"
        "  Time since reset:  %s us (%s%d:%02d:%02d hours)\n"
        "  Main loop iters:   %s since boot, %s since last snapshot\n"
        "  Average loop time: %s us\n"
        "  Maximum loop time: %s us\n",
        FormatNumber(stats.upTime).c_str(), dayStr, sec / 3600, (sec % 3600) / 60, sec % 60,
        FormatNumber(stats.nLoopsEver).c_str(), FormatNumber(stats.nLoops).c_str(),
        FormatNumber(stats.avgLoopTime).c_str(), FormatNumber(stats.maxLoopTime).c_str());

    // Only show memory stats if we have enough data
    if (statsData.size() >= sizeof(stats)) {
        printf("  Heap size:         %s bytes\n"
            "  Heap unused:       %s bytes\n"
            "  Arena size:        %s bytes\n"
            "  Arena allocated:   %s bytes\n"
            "  Arena not in use:  %s bytes\n",
            FormatNumber(stats.heapSize).c_str(), FormatNumber(stats.heapUnused).c_str(),
            FormatNumber(stats.arenaSize).c_str(), FormatNumber(stats.arenaAlloc).c_str(),
            FormatNumber(stats.arenaFree).c_str());
    }
}

static void ShowStatsCmd() {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }
    ShowStats(&device);
}

static void SetNightMode(bool on) {
    FeedbackControllerInterface feedbackIfc;
    if (!feedbackIfc.Open()) {
        ErrorExit("Error: Failed to connect to Feedback Controller HID interface");
    }

    if (!feedbackIfc.SetNightMode(on)) {
        ErrorExit("Error sending Night Mode command to device");
    }

    printf("Night Mode set to %s\n", on ? "ON" : "OFF");
}

static void QueryNightMode() {
    FeedbackControllerInterface feedbackIfc;
    if (!feedbackIfc.Open()) {
        ErrorExit("Error: Failed to connect to Feedback Controller HID interface");
    }

    FeedbackControllerInterface::StatusReport status;
    if (!feedbackIfc.QueryStatus(status)) {
        ErrorExit("Error querying device status");
    }

    printf("Night Mode is currently set to %s\n", status.nightMode ? "ON" : "OFF");
}

static void SendIR(const char *irCmdStr) {
    // Parse IR code in format: protocol.flags.code (hex)
    std::regex pattern("([0-9a-f]{2})\\.([0-9a-f]{2})\\.([0-9a-f]{4,16})",
                      std::regex_constants::icase);
    std::cmatch match;

    if (!std::regex_match(irCmdStr, match, pattern)) {
        printf("Invalid IR command format \"%s\"; expected <protocol>.<flags>.<code>, where <protocol> and <flags> are "
            "2-digit hex numbers, and <code> is a 4-digit to 16-digit hex number\n", irCmdStr);
        exit(1);
    }

    uint8_t protocol = static_cast<uint8_t>(strtol(match[1].str().c_str(), nullptr, 16));
    uint8_t flags = static_cast<uint8_t>(strtol(match[2].str().c_str(), nullptr, 16));
    std::string code_str = match[3].str();
    uint64_t code = strtoull(code_str.c_str(), nullptr, 16);

    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.SendIRCode(protocol, flags, code)) {
        ErrorExit("Error sending IR code");
    }

    printf("IR code sent\n");
}

static void PulseTVRelay() {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.PulseTVRelay()) {
        ErrorExit("Error pulsing TV relay");
    }
    printf("TV relay pulse OK\n");
}

static void SetTVRelay(bool on) {
    PinscapeVendorInterface device;
    if (!device.Open()) {
        ErrorExit("Error: Failed to connect to device");
    }

    if (!device.SetTVRelay(on)) {
        ErrorExit("Error setting TV relay manual state");
    }
    printf("TV relay manual set to manual %s\n", on ? "ON" : "OFF");
}

// ========================================================================
// Help and usage
// ========================================================================

static void UsageExit() {
    printf(
        "Usage: ConfigTool [-q|--quiet] [--id <unit>] [--list] [options]\n"
        "\n"
        "-q or --quiet runs the command in quiet mode, suppressing the normal\n"
        "program banner and listing of device information.  This must be the\n"
        "first option, if used.\n"
        "\n"
        "--id is only required if multiple Pinscape Pico devices are currently\n"
        "running, to select which one to address for this command.  The <unit>\n"
        "can be specified as the configured unit number, unit name, or Pico\n"
        "hardware ID.  --id must be the first option after -q/--quiet.\n"
        "\n"
        "--list shows a list of all attached Pinscape Pico devices.\n"
        "\n"
        "Options:\n"
        "  --help, -?, /?                show this command-line help message\n"
        "  --reset                       reboot the Pico\n"
        "  --safe-mode                   reboot the Pico into Safe Mode\n"
        "  --stats                       display device memory usage and time statistics\n"
        "  --get-config                  display device configuration file on console\n"
        "  --get-config=<file>           write device configuration file to <file>\n"
        "  --put-config <file>           install configuration file <file> on device\n"
        "  --get-safemode-config         display device's safe-mode configuration file on console\n"
        "  --get-safemode-config=<file>  write device's safe-mode configuration to <file>\n"
        "  --put-safemode-config <file>  install <file> as the safe-mode config file\n"
        "  --erase-config                delete all configuration files from device\n"
        "  --factory-reset               delete all configuration data from device\n"
        "  --nightmode ON|OFF|SHOW       set Night Mode to ON or OFF, or display the status\n"
        "  --ir-send <code>              send <code> through the IR transmitter\n"
        "  --pulse-tv-relay              pulse the TV ON relay for the configured interval\n"
        "  --tv-relay on|off             set the TV relay manual state to ON or OFF\n");

    exit(1);
}

// ========================================================================
// Main entry point
// ========================================================================

int main(int argc, char *argv[]) {
    // check for quiet mode
    int argi = 1;
    bool quietMode = false;
    if (argi < argc && (strcmp(argv[argi], "-q") == 0 || strcmp(argv[argi], "--quiet") == 0)) {
        quietMode = true;
        ++argi;
    }

    // version banner
    if (!quietMode) {
        printf("Pinscape Pico Config Tool  Version %s, build %s\n"
            "Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause License / NO WARRANTY\n\n",
            PINSCAPE_VERSION, GetBuildTimestamp());
    }

    // Scan for a device ID argument (it's required to be the first argument, if present)
    std::string deviceID;
    if (argi + 1 < argc && strcmp(argv[argi], "--id") == 0) {
        // get the device ID, and trim the argument pair from the list
        // of remaining arguments to process
        deviceID = argv[argi + 1];
        argi += 2;
    }

    // with no arguments, just show usage
    if (argi == argc && deviceID.size() == 0) {
        printf("\n");
        UsageExit();
    }

    // check for help/usage request before opening device
    for (int i = argi; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "/?") == 0 || strcmp(argv[i], "-?") == 0) {
            UsageExit();
        }
    }

    // check for a lone --list argument
    if (argi < argc && strcmp(argv[argi], "--list") == 0) {
        // show the list
        ShowDeviceList();

        // if that's the only remaining argument, we're done
        if (++argi >= argc)
            return 0;
    }

    // Don't show banner if we're in quiet mode
    // But show a minimal connection message if not quiet
    bool showedBanner = !quietMode;

    // process the command line
    for ( ; argi < argc ; ++argi) {
        if (strcmp(argv[argi], "--list") == 0) {
            ShowDeviceList();
        }
        else if (strcmp(argv[argi], "--reset") == 0) {
            Reboot();
        }
        else if (strcmp(argv[argi], "--safe-mode") == 0) {
            RebootSafeMode();
        }
        else if (bool isPutConfig = (strcmp(argv[argi], "--put-config") == 0);
                 isPutConfig || strcmp(argv[argi], "--put-safemode-config") == 0) {
            // set the type
            auto configType = isPutConfig ? ConfigFile::MAIN : ConfigFile::SAFE_MODE;

            // get the config file
            if (++argi >= argc)
                ErrorExitFmt("Missing config file name; usage is %s <filename>", argv[argi-1]);

            // read and validate the config file
            std::string txt = ReadConfigFile(argv[argi]);

            // parse the JSON to ensure that it's well-formed
            JSONParser json;
            json.Parse(txt.c_str(), txt.size());
            if (json.errors.size() != 0) {
                // report the JSON errors
                printf("Error: configuration text contains JSON parsing errors\n\n");
                for (auto &e : json.errors) {
                    // find the line number of the error
                    int lineNum = 1, colNum = 0;
                    const char *lineStart = txt.c_str();
                    const char *lineEnd = nullptr;
                    const char *tokPtr = nullptr;
                    const char *endp = txt.c_str() + txt.size();
                    for (const char *p = txt.c_str() ; p < endp ; ++p) {
                        // count line starts
                        if (*p == '\n') {
                            ++lineNum;
                            lineStart = p + 1;
                        }

                        // flag the token location
                        if (p == e.src) {
                            // note the token location
                            tokPtr = p;
                            colNum = static_cast<int>(p - lineStart);

                            // scan to the end of the line
                            for (++p ; p < endp && *p != '\n' ; ++p);
                            lineEnd = p;

                            // we've identified the token context
                            break;
                        }
                    }

                    // show the error
                    printf("Line %d, col %d: %s\n", lineNum, colNum, e.message.c_str());

                    // show context if available
                    if (tokPtr != nullptr) {
                        printf("|%.*s\n|", static_cast<int>(lineEnd - lineStart), lineStart);
                        for (const char *p = lineStart ; p < endp && p != tokPtr ; ++p)
                            printf("=");
                        printf("^\n\n");
                    }
                }

                // abort
                exit(1);
            }
            else {
                // valid - send to device
                PutConfig(argv[argi], configType);
            }
        }
        else if (strcmp(argv[argi], "--erase-config") == 0) {
            EraseConfig(ConfigFile::ALL);
        }
        else if (bool isGetConfig = (strcmp(argv[argi], "--get-config") == 0);
                 isGetConfig || strcmp(argv[argi], "--get-safemode-config") == 0) {
            // set the type
            auto configType = isGetConfig ? ConfigFile::MAIN : ConfigFile::SAFE_MODE;
            GetConfigToStdout(configType);
        }
        else if (bool isGetConfig = (strncmp(argv[argi], "--get-config=", 13) == 0);
                 isGetConfig || strncmp(argv[argi], "--get-safemode-config=", 22) == 0) {
            // set the type
            auto configType = isGetConfig ? ConfigFile::MAIN : ConfigFile::SAFE_MODE;

            // retrieve the config file and save to the named file
            const char *filename = isGetConfig ? &argv[argi][13] : &argv[argi][22];
            GetConfig(filename, configType);
        }
        else if (strcmp(argv[argi], "--factory-reset") == 0) {
            FactoryReset();
        }
        else if (strcmp(argv[argi], "--stats") == 0) {
            ShowStatsCmd();
        }
        else if (strcmp(argv[argi], "--ir-send") == 0) {
            // get the command
            if (++argi >= argc)
                ErrorExit("Missing IR command: usage is --ir-send <protocol>.<flags>.<command>, hex notation");

            // send the command
            SendIR(argv[argi]);
        }
        else if (strcmp(argv[argi], "--pulse-tv-relay") == 0) {
            PulseTVRelay();
        }
        else if (strcmp(argv[argi], "--tv-relay") == 0) {
            // get the mode option
            if (++argi >= argc)
                ErrorExit("Missing ON/OFF mode argument for --tv-relay\n");

            // send the command
            SetTVRelay(strcasecmp(argv[argi], "on") == 0);
        }
        else if (strcmp(argv[argi], "--nightmode") == 0) {
            // get the ON/OFF option
            if (++argi >= argc)
                ErrorExit("Missing ON/OFF argument for --nightmode\n");

            // send the command
            const char *modeArg = argv[argi];
            bool on = strcasecmp(modeArg, "on") == 0;
            if (on || strcasecmp(modeArg, "off") == 0)
                SetNightMode(strcasecmp(argv[argi], "on") == 0);
            else if (strcasecmp(modeArg, "show") == 0)
                QueryNightMode();
            else
                ErrorExit("Expected ON, OFF, or SHOW argument for --nightmode\n");
        }
        else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "/?") == 0 || strcmp(argv[argi], "-?") == 0) {
            UsageExit();
        }
        else {
            printf("Unrecognized option \"%s\"\n", argv[argi]);
            break;
        }
    }

    // normal completion
    return 0;
}
