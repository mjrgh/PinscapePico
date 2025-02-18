// Pinscape Pico - Button Tester Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <list>
#include <string>
#include <iterator>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <Windows.h>
#include <windowsx.h>
#include <hidclass.h>
#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>
#include <Xinput.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "../OpenPinballDevice/OpenPinballDeviceLib/OpenPinballDeviceLib.h"
#include "PinscapePicoAPI.h"
#include "WinUtil.h"
#include "ButtonTesterWin.h"
#include "resource.h"
#include "Utilities.h"

#pragma comment(lib, "XInput9_1_0.lib")
#pragma comment(lib, "OpenPinballDeviceLib")

// the window class is part of the PinscapePico namespace
using namespace PinscapePico;

ButtonTesterWin::ButtonTesterWin(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
    DeviceThreadWindow(hInstance, device, new UpdaterThread())
{
    // load bitmaps
    bmpKeyboard = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_KEYBOARD));
    BITMAP bmp;
    GetObject(bmpKeyboard, sizeof(bmp), &bmp);
    cxkb = bmp.bmWidth;
    cykb = bmp.bmHeight;

    bmpXBox = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_XBOX));
    bmpNoXBox = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_NO_XBOX));
    GetObject(bmpXBox, sizeof(bmp), &bmp);
    cxXBox = bmp.bmWidth;
    cyXBox = bmp.bmHeight;

    bmpGPBtnOff = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_GAMEPAD_BTN_OFF));
    bmpGPBtnOn = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_GAMEPAD_BTN_ON));
    bmpGPBtnNA = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_GAMEPAD_BTN_NA));
    GetObject(bmpGPBtnOff, sizeof(bmp), &bmp);
    cxGPBtn = bmp.bmWidth;
    cyGPBtn = bmp.bmWidth;

    // The ShiftIcon is a 1x2 array of cells (Shift, Unshift), so the
    // height of a cell is the height of the bitmap, and the width of a
    // cell is half the width of the bitmap.
    bmpShiftIcon = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_SHIFTICON));
    GetObject(bmpShiftIcon, sizeof(bmp), &bmp);
    cxShiftIcon = bmp.bmWidth / 2;  // 1x2 matrix of icons [Shift, Unshift]
    cyShiftIcon = bmp.bmHeight;

    // Open Pinball Device button images for the pre-defined buttons.
    // This bitmap has two rows of square cells, so the height and width
    // of a cell is half of the height of the bitmap.
    bmpOpenPinDevButtons = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_OPENPINDEV_BUTTONS));
    GetObject(bmpOpenPinDevButtons, sizeof(bmp), &bmp);
    cxOPDBtn = cyOPDBtn = bmp.bmHeight / 2;

    // query device information
    QueryDeviceInfo();
}

void ButtonTesterWin::ReQueryDeviceInfo()
{
    // forget the gamepad resources
    ReleaseGamepadResources();

    // forget the OpenPinDev resources
    ReleaseOpenPinDevResources();

    // forget old button descriptors
    buttonDescs.clear();
    shiftBitsUsed = 0;

    // forget the XInput info
    xInputId = -1;

    // re-query our device information
    QueryDeviceInfo();
}

void ButtonTesterWin::QueryDeviceInfo()
{
    // Scan the XBox controller list.  The XInput driver doesn't always
    // bother to initialize the controllers until someone scans devices,
    // so the Pico might not know its player number assignment yet.
    for (int i = 0 ; i < 4 ; ++i)
    {
        XINPUT_CAPABILITIES xCaps;
        XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &xCaps);
    }

    // query HID devices associated with the vendor interface, so that we
    // can find the device's gamepad interface, if one exists
    std::list<PinscapePico::TSTRING> hids;
    PinscapePico::DeviceID id;
    auto &device = updaterThread->device;
    if (VendorInterface::Shared::Locker l(device); l.locked)
    {
        // query the button configuration
        device->device->QueryButtonConfig(buttonDescs, buttonDevices);

        // query the device IDs, for the XInput player number
        device->device->QueryID(id);

        // enumerate associated logical HIDs, so that we can find the device's
        // gamepad interface if it has one
        device->device->EnumerateAssociatedHIDs(hids);
    }

    // Figure out which shift bits are used across all buttons.  This
    // is the bitwise OR of the shift bits of all of the shift buttons,
    // and the shift masks of all of the non-shift buttons.
    shiftBitsUsed = 0;
    for (const auto &desc : buttonDescs)
        shiftBitsUsed |= (desc.type == ButtonDesc::TYPE_SHIFT) ? desc.shiftBits : desc.shiftMask;

    // get the gamepad HID, if present
    for (auto &hid : hids)
    {
        // open this HID device
        HANDLE fh = CreateFile(hid.c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (fh != INVALID_HANDLE_VALUE)
        {
            // get the preparsed data
            PHIDP_PREPARSED_DATA ppd = NULL;
            if (HidD_GetPreparsedData(fh, &ppd))
            {
                // get capabilities
                HIDP_CAPS caps;
                if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
                {
                    // check the device type:
                    //
                    //  - Gamepad is Usage Page 1 (Generic Desktop), Usage 5 (Gamepad)
                    //  - Pinball Device is Usage Page 5 (Game Controls), Usage 2 (Pinball Device)
                    //
                    if (caps.UsagePage == HID_USAGE_PAGE_GENERIC && caps.Usage == HID_USAGE_GENERIC_GAMEPAD)
                    {
                        if (hGamepad == INVALID_HANDLE_VALUE)
                        {
                            // found it - save the device handle and preparsed data
                            hGamepad = fh;
                            ppdGamepad = ppd;

                            // allocate space for the button reports and usages (states)
                            gamepadReport.resize(caps.InputReportByteLength);

                            // get the button caps, for the report ID and button count
                            std::vector<HIDP_BUTTON_CAPS> btnCaps(caps.NumberInputButtonCaps);
                            USHORT btnCapsLen = caps.NumberInputButtonCaps;
                            if (btnCapsLen != 0 && HidP_GetButtonCaps(HidP_Input, btnCaps.data(), &btnCapsLen, ppd) == HIDP_STATUS_SUCCESS)
                            {
                                // save the report ID in the first byte of the report buffer;
                                // this is required to be set when calling HidD_GetInputReport()
                                gamepadReport[0] = btnCaps[0].ReportID;

                                // add up the number of buttons
                                USHORT nButtons = 0;
                                for (const auto &bc : btnCaps)
                                    nButtons += bc.IsRange ? (bc.Range.UsageMax - bc.Range.UsageMin + 1) : 1;

                                // allocate the button usage array to make space for all of the buttons
                                // changing in a single report
                                gamepadButtonUsages.resize(nButtons);
                            }

                            // we now own these objects, so clear the temporary references
                            fh = INVALID_HANDLE_VALUE;
                            ppd = NULL;
                        }
                    }
                }

                // free with the preparsed data if we didn't take ownership
                if (ppd != NULL)
                    HidD_FreePreparsedData(ppd);
            }

            // Close the file handle if we didn't take ownership
            if (fh != INVALID_HANDLE_VALUE)
                CloseHandle(fh);
        }
    }

    // If we have a valid XInput player index, check to see if
    // we can access the unit
    if (id.xinputPlayerIndex >= 0)
    {
        XINPUT_CAPABILITIES xCaps;
        if (XInputGetCapabilities(id.xinputPlayerIndex, XINPUT_FLAG_GAMEPAD, &xCaps) == ERROR_SUCCESS
            && xCaps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD)
        {
            // got it - remember it to use in display updates
            xInputId = id.xinputPlayerIndex;
        }
    }

    // Open the OpenPinballDevice interface, if present, matching by USB device serial number
    auto opdDescList = OpenPinballDevice::EnumerateDevices(); 
    if (auto it = std::find_if(opdDescList.begin(), opdDescList.end(),
        [serial = device->device->GetSerialNumber()](const OpenPinballDevice::DeviceDesc &desc) { return desc.serial == serial; });
        it != opdDescList.end())
    {
        // open an OpenPinballDevice reader
        openPinDevReader.reset(OpenPinballDevice::Reader::Open(*it));
    }

    // register for device removal notifications
    RegisterDeviceNotifiers();
}

ButtonTesterWin::~ButtonTesterWin()
{
    // clean up bitmaps
    DeleteBitmap(bmpKeyboard);
    DeleteBitmap(bmpXBox);
    DeleteBitmap(bmpNoXBox);
    DeleteBitmap(bmpGPBtnOff);
    DeleteBitmap(bmpGPBtnOn);
    DeleteBitmap(bmpGPBtnNA);
    DeleteBitmap(bmpShiftIcon);

    // release device interface resources
    ReleaseGamepadResources();
    ReleaseOpenPinDevResources();
}

void ButtonTesterWin::ReleaseGamepadResources()
{
    // free the gamepad preparsed data
    if (ppdGamepad != NULL)
    {
        HidD_FreePreparsedData(ppdGamepad);
        ppdGamepad = NULL;
    }

    // close the gamepad handle, if opened
    if (hGamepad != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hGamepad);
        hGamepad = INVALID_HANDLE_VALUE;
    }

    // unregister the notifier handle
    if (hGamepadNotifier != NULL)
    {
        UnregisterDeviceNotification(hGamepadNotifier);
        hGamepadNotifier = NULL;
    }
}

void ButtonTesterWin::ReleaseOpenPinDevResources()
{
    // delete the OpenPinDev reader handle
    openPinDevReader.reset();

    // unregister notifications
    if (hOpenPinDevNotifier != NULL)
    {
        UnregisterDeviceNotification(hOpenPinDevNotifier);
        hOpenPinDevNotifier = NULL;
    }
}

void ButtonTesterWin::RegisterDeviceNotifiers()
{
    // only proceed if the window is open
    if (hwnd != NULL)
    {
        // register for device change notifications on the gamepad USB handle, if valid and
        // not already registered
        if (hGamepad != INVALID_HANDLE_VALUE && hGamepadNotifier == NULL)
        {
            DEV_BROADCAST_HANDLE dbh{ sizeof(dbh), DBT_DEVTYP_HANDLE, 0, hGamepad };
            hGamepadNotifier = RegisterDeviceNotification(hwnd, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
        }

        // register the OPD handle, if valid and not already registered
        if (openPinDevReader != nullptr && hOpenPinDevNotifier == NULL)
        {
            if (HANDLE h = reinterpret_cast<HANDLE>(openPinDevReader->GetNativeHandle()); h != NULL)
            {
                DEV_BROADCAST_HANDLE dbh{ sizeof(dbh), DBT_DEVTYP_HANDLE, 0, h };
                hOpenPinDevNotifier = RegisterDeviceNotification(hwnd, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
            }
        }
    }
}

// Device removal notification handler.  This is called for handles that
// we explicitly register via RegisterDeviceNotification() when the
// underlying device is disconnected from the system.
bool ButtonTesterWin::OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr)
{
    // check for device handle notifications
    if (hdr->dbch_devicetype == DBT_DEVTYP_HANDLE)
    {
        // get the handle to the device being removed
        HANDLE h = reinterpret_cast<DEV_BROADCAST_HANDLE*>(hdr)->dbch_handle;

        // check if it's one of our device handles
        if (h == hGamepad)
        {
            // close the gamepad handles
            ReleaseGamepadResources();
        }
        else if (openPinDevReader != nullptr && h == reinterpret_cast<HANDLE>(openPinDevReader->GetNativeHandle()))
        {
            // close the OpenPinballDevice handles
            ReleaseOpenPinDevResources();
        }
    }

     // proceed with default system handling
    return false;
}

// USB key usage -> name map
static const std::unordered_map<uint8_t, const char *> usbKeyNames{
//  { 0x00, "" },                // 00 Reserved
//  { 0x01, "" },                // 01 Keyboard ErrorRollOver
//  { 0x02, "" },                // 02 Keyboard POSTFail 
//  { 0x03, "" },                // 03 Keyboard ErrorUndefined
	{ 0x04, "A" },               // 04 Keyboard a and A
	{ 0x05, "B" },               // 05 Keyboard b and B
	{ 0x06, "C" },               // 06 Keyboard c and C
	{ 0x07, "D" },               // 07 Keyboard d and D
	{ 0x08, "E" },               // 08 Keyboard e and E
	{ 0x09, "F" },               // 09 Keyboard f and F
	{ 0x0A, "G" },               // 0A Keyboard g and G
	{ 0x0B, "H" },               // 0B Keyboard h and H
	{ 0x0C, "I" },               // 0C Keyboard i and I
	{ 0x0D, "J" },               // 0D Keyboard j and J
	{ 0x0E, "K" },               // 0E Keyboard k and K
	{ 0x0F, "L" },               // 0F Keyboard l and L
	{ 0x10, "M" },               // 10 Keyboard m and M
	{ 0x11, "N" },               // 11 Keyboard n and N
	{ 0x12, "O" },               // 12 Keyboard o and O
	{ 0x13, "P" },               // 13 Keyboard p and P
	{ 0x14, "Q" },               // 14 Keyboard q and Q
	{ 0x15, "R" },               // 15 Keyboard r and R
	{ 0x16, "S" },               // 16 Keyboard s and S
	{ 0x17, "T" },               // 17 Keyboard t and T
	{ 0x18, "U" },               // 18 Keyboard u and U
	{ 0x19, "V" },               // 19 Keyboard v and V
	{ 0x1A, "W" },               // 1A Keyboard w and W
	{ 0x1B, "X" },               // 1B Keyboard x and X
	{ 0x1C, "Y" },               // 1C Keyboard y and Y
	{ 0x1D, "Z" },               // 1D Keyboard z and Z
	{ 0x1E, "1" },               // 1E Keyboard 1 and !
	{ 0x1F, "2" },               // 1F Keyboard 2 and @
	{ 0x20, "3" },               // 20 Keyboard 3 and #
	{ 0x21, "4" },               // 21 Keyboard 4 and $
	{ 0x22, "5" },               // 22 Keyboard 5 and %
	{ 0x23, "6" },               // 23 Keyboard 6 and ^
	{ 0x24, "7" },               // 24 Keyboard 7 and &
	{ 0x25, "8" },               // 25 Keyboard 8 and *
	{ 0x26, "9" },               // 26 Keyboard 9 and (
	{ 0x27, "0" },               // 27 Keyboard 0 and )
	{ 0x28, "Return" },          // 28 Keyboard Return (ENTER)
	{ 0x29, "Esc" },             // 29 Keyboard ESCAPE
	{ 0x2A, "Backspace" },       // 2A Keyboard DELETE (Backspace)
	{ 0x2B, "Tab" },             // 2B Keyboard Tab
	{ 0x2C, "Space" },           // 2C Keyboard Spacebar
	{ 0x2D, "-" },               // 2D Keyboard - and (underscore)
	{ 0x2E, "=" },               // 2E Keyboard = and +
	{ 0x2F, "[" },               // 2F Keyboard [ and {
	{ 0x30, "]" },               // 30 Keyboard ] and }
	{ 0x31, "\\" },              // 31 Keyboard \ and |
//  { 0x32, "" },                // 32 Keyboard Non-US # and 5
	{ 0x33, ";" },               // 33 Keyboard ; and :
	{ 0x34, "'" },               // 34 Keyboard ' and "
//  { 0x35, "" },                // 35 Keyboard Grave Accent and Tilde
	{ 0x36, "," },               // 36 Keyboard , and <
	{ 0x37, "." },               // 37 Keyboard . and >
	{ 0x38, "/" },               // 38 Keyboard / and ?
	{ 0x39, "CapsLock" },        // 39 Keyboard Caps Lock
	{ 0x3A, "F1" },              // 3A Keyboard F1
	{ 0x3B, "F2" },              // 3B Keyboard F2
	{ 0x3C, "F3" },              // 3C Keyboard F3
	{ 0x3D, "F4" },              // 3D Keyboard F4
	{ 0x3E, "F5" },              // 3E Keyboard F5
	{ 0x3F, "F6" },              // 3F Keyboard F6
	{ 0x40, "F7" },              // 40 Keyboard F7
	{ 0x41, "F8" },              // 41 Keyboard F8
	{ 0x42, "F9" },              // 42 Keyboard F9
	{ 0x43, "F10" },             // 43 Keyboard F10
	{ 0x44, "F11" },             // 44 Keyboard F11
	{ 0x45, "F12" },             // 45 Keyboard F12
	{ 0x46, "PrintScreen" },     // 46 Keyboard PrintScreen
	{ 0x47, "ScrollLock" },      // 47 Keyboard Scroll Lock
	{ 0x48, "Pause/Break" },     // 48 Keyboard Pause
	{ 0x49, "Insert" },          // 49 Keyboard Insert
	{ 0x4A, "Home" },            // 4A Keyboard Home
	{ 0x4B, "PageUp" },          // 4B Keyboard PageUp
	{ 0x4C, "Del" },             // 4C Keyboard Delete Forward
	{ 0x4D, "End" },             // 4D Keyboard Accelerate
	{ 0x4E, "PageDown" },        // 4E Keyboard PageDown
	{ 0x4F, "Right" },           // 4F Keyboard RightArrow
	{ 0x50, "Left" },            // 50 Keyboard LeftArrow
	{ 0x51, "Down" },            // 51 Keyboard DownArrow
	{ 0x52, "Up" },              // 52 Keyboard UpArrow
	{ 0x53, "NumLock" },         // 53 Keypad Num Lock and Clear
	{ 0x54, "Keypad /" },        // 54 Keypad /
	{ 0x55, "Keypad *" },        // 55 Keypad *
	{ 0x56, "Keypad -" },        // 56 Keypad -
	{ 0x57, "Keypad +" },        // 57 Keypad +
	{ 0x58, "Keypad enter" },    // 58 Keypad ENTER
	{ 0x59, "Keypad 1" },        // 59 Keypad 1 and Accelerate
	{ 0x5A, "Keypad 2" },        // 5A Keypad 2 and Down Arrow
	{ 0x5B, "Keypad 3" },        // 5B Keypad 3 and PageDn
	{ 0x5C, "Keypad 4" },        // 5C Keypad 4 and Left Arrow
	{ 0x5D, "Keypad 5" },        // 5D Keypad 5
	{ 0x5E, "Keypad 6" },        // 5E Keypad 6 and Right Arrow
	{ 0x5F, "Keypad 7" },        // 5F Keypad 7 and Home
	{ 0x60, "Keypad 8" },        // 60 Keypad 8 and Up Arrow
	{ 0x61, "Keypad 9" },        // 61 Keypad 9 and PageUp
	{ 0x62, "Keypad 0" },        // 62 Keypad 0 and Insert
	{ 0x63, "Keypad ." },        // 63 Keypad . and Delete
//  { 0x64, "" },                // 64 Keyboard Non-US \ and |
	{ 0x65, "Application" },     // 65 Keyboard Application
	{ 0x66, "Power" },           // 66 Keyboard Power
	{ 0x67, "Keypad =" },        // 67 Keypad =
	{ 0x68, "F13" },             // 68 Keyboard F13
	{ 0x69, "F14" },             // 69 Keyboard F14
	{ 0x6A, "F15" },             // 6A Keyboard F15
	{ 0x6B, "F16" },             // 6B Keyboard F16
	{ 0x6C, "F17" },             // 6C Keyboard F17
	{ 0x6D, "F18" },             // 6D Keyboard F18
	{ 0x6E, "F19" },             // 6E Keyboard F19
	{ 0x6F, "F20" },             // 6F Keyboard F20
	{ 0x70, "F21" },             // 70 Keyboard F21
	{ 0x71, "F22" },             // 71 Keyboard F22
	{ 0x72, "F23" },             // 72 Keyboard F23
	{ 0x73, "F24" },             // 73 Keyboard F24
	{ 0x74, "Execute" },         // 74 Keyboard Execute
	{ 0x75, "Help" },            // 75 Keyboard Help
	{ 0x76, "Menu" },            // 76 Keyboard Menu
	{ 0x77, "Select" },          // 77 Keyboard Select
	{ 0x78, "Stop" },            // 78 Keyboard Stop
	{ 0x79, "Again" },           // 79 Keyboard Again
	{ 0x7A, "Undo" },            // 7A Keyboard Undo
	{ 0x7B, "Cut" },             // 7B Keyboard Cut
	{ 0x7C, "Copy" },            // 7C Keyboard Copy
	{ 0x7D, "Paste" },           // 7D Keyboard Paste
	{ 0x7E, "Find" },            // 7E Keyboard Find
	{ 0x7F, "Mute" },            // 7F Keyboard Mute
	{ 0x80, "VolUp" },           // 80 Keyboard Volume Up
	{ 0x81, "VolDown" },         // 81 Keyboard Volume Down
//  { 0x82, "" },                // 82 Keyboard Locking Caps Lock
//  { 0x83, "" },                // 83 Keyboard Locking Num Lock
//  { 0x84, "" },                // 84 Keyboard Locking Scroll Lock
	{ 0x85, "Keypad ," },        // 85 Keypad Comma
//  { 0x86, "" },                // 86 Keypad Equal Sign (AS/400)
//  { 0x87, "" },                // 87 Keyboard International1
//  { 0x88, "" },                // 88 Keyboard International2
//  { 0x89, "" },                // 89 Keyboard International3
//  { 0x8A, "" },                // 8A Keyboard International4
//  { 0x8B, "" },                // 8B Keyboard International5
//  { 0x8C, "" },                // 8C Keyboard International6
//  { 0x8D, "" },                // 8D Keyboard International7
//  { 0x8E, "" },                // 8E Keyboard International8
//  { 0x8F, "" },                // 8F Keyboard International9
//  { 0x90, "" },                // 90 Keyboard LANG1
//  { 0x91, "" },                // 91 Keyboard LANG2
//  { 0x92, "" },                // 92 Keyboard LANG3
//  { 0x93, "" },                // 93 Keyboard LANG4
//  { 0x94, "" },                // 94 Keyboard LANG5
//  { 0x95, "" },                // 95 Keyboard LANG6
//  { 0x96, "" },                // 96 Keyboard LANG7
//  { 0x97, "" },                // 97 Keyboard LANG8
//  { 0x98, "" },                // 98 Keyboard LANG9
//  { 0x99, "" },                // 99 Keyboard Alternate Erase
//  { 0x9A, "" },                // 9A Keyboard SysReq/Attention
//  { 0x9B, "" },                // 9B Keyboard Cancel
//  { 0x9C, "" },                // 9C Keyboard Clear
//  { 0x9D, "" },                // 9D Keyboard Prior
//  { 0x9E, "" },                // 9E Keyboard Return
//  { 0x9F, "" },                // 9F Keyboard Separator
//  { 0xA0, "" },                // A0 Keyboard Out
//  { 0xA1, "" },                // A1 Keyboard Oper
//  { 0xA2, "" },                // A2 Keyboard Clear/Again
//  { 0xA3, "" },                // A3 Keyboard CrSel/Props
//  { 0xA4, "" },                // A4 Keyboard ExSel
//  { 0xA5, "" },                // A5 Reserved
//  { 0xA6, "" },                // A6 Reserved
//  { 0xA7, "" },                // A7 Reserved
//  { 0xA8, "" },                // A8 Reserved
//  { 0xA9, "" },                // A9 Reserved
//  { 0xAA, "" },                // AA Reserved
//  { 0xAB, "" },                // AB Reserved
//  { 0xAC, "" },                // AC Reserved
//  { 0xAD, "" },                // AD Reserved
//  { 0xAE, "" },                // AE Reserved
//  { 0xAF, "" },                // AF Reserved
	{ 0xB0, "Keypad 00" },       // B0 Keypad 00
	{ 0xB1, "Keypad 000" },      // B1 Keypad 000
//  { 0xB2, "" },                // B2 Thousands Separator
//  { 0xB3, "" },                // B3 Decimal Separator
//  { 0xB4, "" },                // B4 Currency Unit
//  { 0xB5, "" },                // B5 Currency Sub-unit
//  { 0xB6, "" },                // B6 Keypad ( Sel
//  { 0xB7, "" },                // B7 Keypad )
//  { 0xB8, "" },                // B8 Keypad {
//  { 0xB9, "" },                // B9 Keypad }
//  { 0xBA, "" },                // BA Keypad Tab
//  { 0xBB, "" },                // BB Keypad Backspace
//  { 0xBC, "" },                // BC Keypad A
//  { 0xBD, "" },                // BD Keypad B
//  { 0xBE, "" },                // BE Keypad C
//  { 0xBF, "" },                // BF Keypad D
//  { 0xC0, "" },                // C0 Keypad E
//  { 0xC1, "" },                // C1 Keypad F
//  { 0xC2, "" },                // C2 Keypad XOR
//  { 0xC3, "" },                // C3 Keypad ^
//  { 0xC4, "" },                // C4 Keypad %
//  { 0xC5, "" },                // C5 Keypad <
//  { 0xC6, "" },                // C6 Keypad >
//  { 0xC7, "" },                // C7 Keypad &
//  { 0xC8, "" },                // C8 Keypad &&
//  { 0xC9, "" },                // C9 Keypad |
//  { 0xCA, "" },                // CA Keypad ||
//  { 0xCB, "" },                // CB Keypad :
//  { 0xCC, "" },                // CC Keypad #
//  { 0xCD, "" },                // CD Keypad Space
//  { 0xCE, "" },                // CE Keypad @
//  { 0xCF, "" },                // CF Keypad !
//  { 0xD0, "" },                // D0 Keypad Memory Store
//  { 0xD1, "" },                // D1 Keypad Memory Recall
//  { 0xD2, "" },                // D2 Keypad Memory Clear
//  { 0xD3, "" },                // D3 Keypad Memory Add
//  { 0xD4, "" },                // D4 Keypad Memory Subtract
//  { 0xD5, "" },                // D5 Keypad Memory Multiply
//  { 0xD6, "" },                // D6 Keypad Memory Divide
//  { 0xD7, "" },                // D7 Keypad +/-
//  { 0xD8, "" },                // D8 Keypad Clear
//  { 0xD9, "" },                // D9 Keypad Clear Entry
//  { 0xDA, "" },                // DA Keypad Binary
//  { 0xDB, "" },                // DB Keypad Octal
//  { 0xDC, "" },                // DC Keypad Decimal
//  { 0xDD, "" },                // DD Keypad Hexadecimal
//  { 0xDE, "" },                // DE Reserved
//  { 0xDF, "" },                // DF Reserved
	{ 0xE0, "Left Ctrl" },       // E0 Keyboard LeftControl
	{ 0xE1, "Left Shift" },      // E1 Keyboard LeftShift
	{ 0xE2, "Left Alt" },        // E2 Keyboard LeftAlt
	{ 0xE3, "Left GUI" },        // E3 Keyboard Left GUI
	{ 0xE4, "Right Ctrl" },      // E4 Keyboard RightControl
	{ 0xE5, "Right Shift" },     // E5 Keyboard RightShift
	{ 0xE6, "Right Alt" },       // E6 Keyboard RightAlt
	{ 0xE7, "Right GUI" },       // E7 Keyboard Right GUI
};

// Media key usage -> name map
static const std::unordered_map<uint8_t, const char*> mediaKeyNames {
	{ 0xE2, "Mute" }, 
	{ 0xE9, "Vol Up" }, 
	{ 0xEA, "Vol Down" },
	{ 0xB5, "Next Track" },
	{ 0xB6, "Prev Track" }, 
	{ 0xB7, "Stop" },
	{ 0xCD, "Play/Pause" }, 
	{ 0xB8, "Eject" }
};

// Keyboard layout information, for the on-screen keyboard graphics.
// This is indexed by Windows 8-bit "scan code" combined with a 9th
// bit that's set to 1 if KF_EXTENDED is set in the keyboard event
// flags.
struct KeyboardLayout
{
    const char *keyName;
    int vkey;
    int x;
    int y;
    int cx;
    int cy;
};
KeyboardLayout keyboardLayout[] ={
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x00
    { "Escape", 0x1b, 3, 29, 21, 22 },                  // 0x01
    { "1 !", 0x31, 29, 66, 21, 22 },                    // 0x02
    { "2 @", 0x32, 55, 66, 21, 22 },                    // 0x03
    { "3 #", 0x33, 81, 66, 21, 22 },                    // 0x04
    { "4 $", 0x34, 107, 66, 21, 22 },                   // 0x05
    { "5 %", 0x35, 133, 66, 21, 22 },                   // 0x06
    { "6 ^", 0x36, 159, 66, 21, 22 },                   // 0x07
    { "7 &", 0x37, 185, 66, 21, 22 },                   // 0x08
    { "8 *", 0x38, 210, 66, 21, 22 },                   // 0x09
    { "9 (", 0x39, 236, 66, 21, 22 },                   // 0x0a
    { "0 )", 0x30, 262, 66, 21, 22 },                   // 0x0b
    { "-_", 0xbd, 288, 66, 21, 22 },                    // 0x0c
    { "=+", 0xbb, 314, 66, 21, 22 },                    // 0x0d
    { "Backspace", 0x8, 340, 66, 41, 22 },              // 0x0e
    { "Tab", 0x9, 4, 92, 31, 22 },                      // 0x0f
    { "Q", 0x51, 40, 92, 21, 22 },                      // 0x10
    { "W", 0x57, 66, 92, 21, 22 },                      // 0x11
    { "E", 0x45, 92, 92, 21, 22 },                      // 0x12
    { "R", 0x52, 118, 92, 21, 22 },                     // 0x13
    { "T", 0x54, 144, 92, 21, 22 },                     // 0x14
    { "Y", 0x59, 169, 92, 21, 22 },                     // 0x15
    { "U", 0x55, 195, 92, 21, 22 },                     // 0x16
    { "I", 0x49, 221, 92, 21, 22 },                     // 0x17
    { "O", 0x4f, 246, 92, 21, 22 },                     // 0x18
    { "P", 0x50, 272, 92, 21, 22 },                     // 0x19
    { "[{", 0xdb, 298, 92, 21, 22 },                    // 0x1a
    { "]}", 0xdd, 324, 92, 21, 22 },                    // 0x1b
    { "Enter", 0xd, 330, 118, 50, 22 },                 // 0x1c
    { "Left Control", 0xa2, 4, 169, 29, 22 },           // 0x1d
    { "A", 0x41, 48, 118, 21, 22 },                     // 0x1e
    { "S", 0x53, 73, 118, 21, 22 },                     // 0x1f
    { "D", 0x44, 99, 118, 21, 22 },                     // 0x20
    { "F", 0x46, 125, 118, 21, 22 },                    // 0x21
    { "G", 0x47, 150, 118, 21, 22 },                    // 0x22
    { "H", 0x48, 176, 118, 21, 22 },                    // 0x23
    { "J", 0x4a, 202, 118, 21, 22 },                    // 0x24
    { "K", 0x4b, 228, 118, 21, 22 },                    // 0x25
    { "L", 0x4c, 253, 118, 21, 22 },                    // 0x26
    { ";:", 0xba, 279, 118, 21, 22 },                   // 0x27
    { "'\"", 0xde, 305, 118, 21, 22 },                  // 0x28
    { "`~", 0xc0, 4, 66, 21, 22 },                      // 0x29
    { "Left Shift", 0xa0, 4, 144, 48, 22 },             // 0x2a
    { "|", 0xdc, 350, 92, 31, 22 },                     // 0x2b
    { "Z", 0x5a, 58, 144, 21, 22 },                     // 0x2c
    { "X", 0x58, 84, 144, 21, 22 },                     // 0x2d
    { "C", 0x43, 110, 144, 21, 22 },                    // 0x2e
    { "V", 0x56, 136, 144, 21, 22 },                    // 0x2f
    { "B", 0x42, 162, 144, 21, 22 },                    // 0x30
    { "N", 0x4e, 188, 144, 21, 22 },                    // 0x31
    { "M", 0x4d, 214, 144, 21, 22 },                    // 0x32
    { ",<", 0xbc, 239, 144, 21, 22 },                   // 0x33
    { ".>", 0xbe, 265, 144, 21, 22 },                   // 0x34
    { "/?", 0xbf, 291, 144, 21, 22 },                   // 0x35
    { "Right Shift", 0xa1, 317, 144, 64, 22 },          // 0x36
    { "Keypad *", 0x6a, 529, 66, 21, 22 },              // 0x37
    { "Left Alt", 0xa4, 67, 169, 29, 22 },              // 0x38
    { "Spacebar", 0x20, 99, 169, 153, 22 },             // 0x39
    { "Caps Lock", 0x14, 4, 118, 39, 22 },              // 0x3a
    { "F1", 0x70, 54, 29, 21, 22 },                     // 0x3b
    { "F2", 0x71, 80, 29, 21, 22 },                     // 0x3c
    { "F3", 0x72, 106, 29, 21, 22 },                    // 0x3d
    { "F4", 0x73, 132, 29, 21, 22 },                    // 0x3e
    { "F5", 0x74, 169, 29, 21, 22 },                    // 0x3f
    { "F6", 0x75, 196, 29, 21, 22 },                    // 0x40
    { "F7", 0x76, 222, 29, 21, 22 },                    // 0x41
    { "F8", 0x77, 248, 29, 21, 22 },                    // 0x42
    { "F9", 0x78, 286, 29, 21, 22 },                    // 0x43
    { "F10", 0x79, 312, 29, 21, 22 },                   // 0x44
    { "Pause/Break", 0x90, 446, 28, 21, 22 },           // 0x45
    { "Scroll Lock", 0x91, 421, 29, 21, 22 },           // 0x46
    { "Keypad 7/Home", 0x67, 477, 92, 21, 22 },         // 0x47
    { "Keypad 8/Up Arrow", 0x68, 504, 92, 21, 22 },     // 0x48
    { "Keypad 9/Page Up", 0x69, 530, 92, 21, 22 },      // 0x49
    { "Keypad -", 0x6d, 554, 66, 21, 22 },              // 0x4a
    { "Keypad 4/Left Arrow", 0x64, 477, 117, 21, 22 },  // 0x4b
    { "Keypad 5", 0x65, 504, 117, 21, 22 },             // 0x4c
    { "Keypad 6/Right Arrow", 0x66, 530, 117, 21, 22 }, // 0x4d
    { "Keypad +", 0x6b, 554, 92, 21, 46 },              // 0x4e
    { "Keypad 1/End", 0x61, 477, 143, 21, 22 },         // 0x4f
    { "Keypad 2/Down Arrow", 0x62, 504, 143, 21, 22 },  // 0x50
    { "Keypad 3/Page Down", 0x63, 530, 143, 21, 22 },   // 0x51
    { "Keypad 0/Insert", 0x60, 478, 169, 48, 22 },      // 0x52
    { "Keypad ./Delete", 0x6e, 530, 169, 21, 22 },      // 0x53
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x54
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x55
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x56
    { "F11", 0x7a, 337, 29, 21, 22 },                   // 0x57
    { "F12", 0x7b, 364, 29, 21, 22 },                   // 0x58
    { "Keypad =", 0x59, 489, 29, 21, 22 },              // 0x59
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x5a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x5b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x5c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x5d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x5e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x5f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x60
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x61
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x62
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x63
    { "F13", 0x7c, 54, 3, 21, 22 },                     // 0x64
    { "F14", 0x7d, 80, 3, 21, 22 },                     // 0x65
    { "F15", 0x7e, 106, 3, 21, 22 },                    // 0x66
    { "F16", 0x7f, 132, 3, 21, 22 },                    // 0x67
    { "F17", 0x80, 169, 3, 21, 22 },                    // 0x68
    { "F18", 0x81, 196, 3, 21, 22 },                    // 0x69
    { "F19", 0x82, 222, 3, 21, 22 },                    // 0x6a
    { "F20", 0x83, 248, 3, 21, 22 },                    // 0x6b
    { "F21", 0x84, 286, 3, 21, 22 },                    // 0x6c
    { "F22", 0x85, 312, 3, 21, 22 },                    // 0x6d
    { "F23", 0x86, 337, 3, 21, 22 },                    // 0x6e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x6f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x70
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x71
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x72
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x73
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x74
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x75
    { "F24", 0x87, 364, 3, 21, 22 },                    // 0x76
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x77
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x78
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x79
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x7a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x7b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x7c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x7d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x7e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x7f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x80
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x81
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x82
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x83
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x84
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x85
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x86
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x87
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x88
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x89
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x8a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x8b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x8c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x8d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x8e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x8f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x90
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x91
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x92
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x93
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x94
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x95
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x96
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x97
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x98
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x99
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x9a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x9b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x9c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x9d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x9e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x9f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xa9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xaa
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xab
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xac
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xad
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xae
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xaf
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xb9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xba
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xbb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xbc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xbd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xbe
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xbf
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xc9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xca
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xcb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xcc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xcd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xce
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xcf
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xd9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xda
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xdb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xdc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xdd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xde
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xdf
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xe9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xea
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xeb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xec
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xed
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xee
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xef
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xf9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xfa
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xfb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xfc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xfd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xfe
    { nullptr, -1, 0, 0, 0, 0 },                        // 0xff
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x100
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x101
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x102
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x103
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x104
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x105
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x106
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x107
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x108
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x109
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x10a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x10b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x10c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x10d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x10e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x10f
    { "Media Previous Track", 0xb1, 509, 3, 21, 22 },   // 0x110
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x111
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x112
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x113
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x114
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x115
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x116
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x117
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x118
    { "Media Next Track", 0xb0, 531, 3, 21, 22 },       // 0x119
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x11a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x11b
    { "Keypad Enter", -1, 554, 143, 21, 46 },           // 0x11c
    { "Right Control", 0xa3, 353, 169, 29, 22 },        // 0x11d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x11e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x11f
    { "Media Mute", 0xad, 554, 29, 21, 22 },            // 0x120
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x121
    { "Media Play/Pause", 0xb3, 554, 3, 21, 22 },       // 0x122
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x123
    { "Media Stop", 0xb2, 487, 3, 21, 22 },             // 0x124
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x125
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x126
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x127
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x128
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x129
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x12a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x12b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x12c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x12d
    { "Media Volume Down", 0xae, 531, 29, 21, 22 },     // 0x12e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x12f
    { "Media Volume Up", 0xaf, 509, 29, 21, 22 },       // 0x130
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x131
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x132
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x133
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x134
    { "Keypad /", 0x6f, 503, 66, 21, 22 },              // 0x135
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x136
    { "Print Screen", -1, 396, 29, 21, 22 },            // 0x137
    { "Right Alt", 0xa5, 258, 169, 29, 22 },            // 0x138
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x139
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x13a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x13b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x13c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x13d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x13e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x13f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x140
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x141
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x142
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x143
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x144
    { "Keypad Num Lock", 0x90, 477, 66, 21, 22 },       // 0x145
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x146
    { "Home", 0x24, 421, 66, 21, 22 },                  // 0x147
    { "Up Arrow", 0x26, 421, 144, 21, 22 },             // 0x148
    { "Page Up", 0x21, 447, 66, 21, 22 },               // 0x149
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x14a
    { "Left Arrow", 0x25, 395, 169, 21, 22 },           // 0x14b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x14c
    { "Right Arrow", 0x27, 447, 169, 21, 22 },          // 0x14d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x14e
    { "End", 0x23, 421, 92, 21, 22 },                   // 0x14f
    { "Down Arrow", 0x28, 421, 169, 21, 22 },           // 0x150
    { "Page Down", 0x22, 447, 92, 21, 22 },             // 0x151
    { "Insert", 0x2d, 396, 66, 21, 22 },                // 0x152
    { "Delete", 0x2e, 396, 92, 21, 22 },                // 0x153
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x154
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x155
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x156
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x157
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x158
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x159
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x15a
    { "Left GUI", 0x5b, 35, 169, 29, 22 },              // 0x15b
    { "Right GUI", 0x5c, 289, 169, 29, 22 },            // 0x15c
    { "Application Key", 0x5d, 321, 169, 29, 22 },      // 0x15d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x15e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x15f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x160
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x161
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x162
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x163
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x164
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x165
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x166
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x167
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x168
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x169
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x16a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x16b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x16c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x16d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x16e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x16f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x170
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x171
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x172
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x173
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x174
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x175
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x176
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x177
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x178
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x179
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x17a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x17b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x17c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x17d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x17e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x17f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x180
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x181
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x182
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x183
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x184
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x185
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x186
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x187
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x188
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x189
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x18a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x18b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x18c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x18d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x18e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x18f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x190
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x191
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x192
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x193
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x194
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x195
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x196
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x197
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x198
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x199
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x19a
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x19b
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x19c
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x19d
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x19e
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x19f
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1a9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1aa
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ab
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ac
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ad
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ae
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1af
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1b9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ba
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1bb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1bc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1bd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1be
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1bf
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1c9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ca
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1cb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1cc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1cd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ce
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1cf
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1d9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1da
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1db
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1dc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1dd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1de
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1df
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1e9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ea
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1eb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ec
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ed
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ee
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ef
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f0
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f1
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f2
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f3
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f4
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f5
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f6
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f7
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f8
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1f9
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1fa
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1fb
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1fc
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1fd
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1fe
    { nullptr, -1, 0, 0, 0, 0 },                        // 0x1ff
};

// VK_xxx to Scan Code translator.  Some keys, such as the "Consumer"
// usage page media keys, don't send valid scan codes through WM_KEYxxx
// events, but they do send valid VK_ codes.  This lets us look up the
// scan code for a given VK using our keyboard layout mapping.
static int vkToScanCode[256];
static void InitVKtoScanCode()
{
    static bool inited = false;
    if (!inited)
    {
        const auto *k = &keyboardLayout[0];
        for (UINT i = 0 ; i < _countof(keyboardLayout) ; ++i, ++k)
        {
            if (k->vkey != 0)
                vkToScanCode[k->vkey] = i;
        }
        inited = true;
    }
}

// XInput button names, indexed by the button IDs used in the USB protocol
static const char *xinputButtonNames[] ={
	"Up", "Down", "Left", "Right", "Start", "Back", "L3", "R3", "LB", "RB", "XBOX", "Unused(11)", "A", "B", "X", "Y"
};

struct XBoxLayout
{
    // button name
    const char *name;

    // circle center, rectangle upper left
    int x;
    int y;

    // rectangle width/height; 0,0 if not a rectangle
    int cx;
    int cy;

    // circle radius; 0 if not a circle
    int r;
};
static const XBoxLayout xboxLayout[] ={
    { "DPad Up", 54, 45, 8, 8, 0 },  // 0 = Up
    { "DPad Down", 54, 60, 8, 8, 0 },  // 1 = Down
    { "DPad Left", 47, 53, 8, 8, 0 },  // 2 = Left
    { "DPad Right", 63, 53, 8, 8, 0 },  // 3 = Right
    { "Start", 90, 30, 0, 0, 4  },  // 4 = Move
    { "Back", 67, 30, 0, 0, 4 },  // 5 = back
    { "L3", 39, 31, 0, 0, 10 },  // 6 = L3 (left joystick button click)
    { "R3", 99, 54, 0, 0, 10 },  // 7 = R3 (right joystick button click)
    { "LB", 24, 2, 30, 10, 0 },  // 8 = LB (left bumper)
    { "RB", 102, 2, 30, 10, 0 },  // 9 = RB (right bumper)
    { "XBox Button", 79, 15, 0, 0, 7 },  // 10 = XBox
    { "Button 11", 0, 0, 0, 0, 0 },  // 11 = unused
    { "A", 119, 41, 0, 0, 6 },  // 12 = A
    { "B", 129, 31, 0, 0, 6 },  // 13 = B
    { "X", 109, 31, 0, 0, 6 },  // 14 = X
    { "Y", 119, 20, 0, 0, 6 },  // 15 = Y
};

// OPD pinball button names, in order of the bits in the HID report struct
static const char *opdButtonNames[] = {
    "Start", "Exit", "Extra Ball", "Coin 1", "Coin 2", "Coin 3", "Coin 4", "Launch Ball",
    "Lockbar Fire", "Left Flipper", "Right Flipper", "Left Flipper 2", "Right Flipper 2", "Left MagnaSave", "Right MagnaSave", "Tilt Bob",
    "Slam Tilt", "Coin Door", "Service Cancel", "Service Down", "Service Up", "Service Enter", "Left Nudge", "Forward Nudge",
    "Right Nudge", "Vol Up", "Vol Down",
};

// Paint off-screen
void ButtonTesterWin::PaintOffScreen(HDC hdc0)
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// fill with white background
    HDCHelper hdc(hdc0);
	FillRect(hdc, &crc, GetStockBrush(WHITE_BRUSH));

    // select a gray pen for drawing separator lines
    HPen grayPen(RGB(0xC0, 0xC0, 0xC0));
    HPEN oldPen = SelectPen(hdc, grayPen);

    // if any shift buttons are defined, draw the global shift state
    int y = crc.top;
    POINT ptShiftState;
    if (shiftBitsUsed != 0)
    {
        // header
        int xss = cxPanel + cxScrollbar + marginKb;
        y += marginShiftState;
        SIZE sz = hdc.DrawText(xss, y, 1, boldFont, RGB(0, 0, 0), "Shift State: ");

        // remember this location - we'll fill it in after locking the current report
        ptShiftState ={ xss + sz.cx, y };

        // separator
        y += sz.cy + marginShiftState;
        MoveToEx(hdc, cxPanel + cxScrollbar, y, NULL);
        LineTo(hdc, crc.right, y);
        y += 1;
    }

	// draw the keyboard
    CompatibleDC bdc(hdc);
    bdc.Select(bmpKeyboard);
	int xkb = cxPanel + cxScrollbar + marginKb;
    int ykb = y + marginKb;
	BitBlt(hdc, xkb, ykb, cxkb, cykb, bdc, 0, 0, SRCCOPY);

    // highlight keys currently flagged as pressed
    for (size_t scanCode = 0 ; scanCode < _countof(keyboardLayout) ; ++scanCode)
    {
        const auto &kl = keyboardLayout[scanCode];
        if (keysDown[scanCode] && kl.cx != 0)
        {
            RECT rc{ 0, 0, kl.cx, kl.cy };
            OffsetRect(&rc, kl.x + xkb, kl.y + ykb);
            HBrush hilite(RGB(0xFF, 0x00, 0xFF));
            FrameRect(hdc, &rc, hilite);
            InflateRect(&rc, 1, 1);
            FrameRect(hdc, &rc, hilite);
        }
    }

    // read a gamepad report, if we have a gamepad handle open
    if (hGamepad != INVALID_HANDLE_VALUE)
    {
        // read all reports, to get the most recent
        int nReads = 0;
        for (;;)
        {
            // try an overlapped read with a zero timeout
            DWORD nBytes = static_cast<DWORD>(gamepadReport.size());
            if (ReadFile(hGamepad, gamepadReport.data(), nBytes, &nBytes, ovGamepad.Clear())
                || (GetLastError() == ERROR_IO_PENDING && GetOverlappedResultEx(hGamepad, &ovGamepad.ov, &nBytes, 0, TRUE)))
            {
                // successful read - count it
                ++nReads;
            }
            else
            {
                // no more packets available - stop here
                break;
            }
        }

        // if we read a packet, process it
        if (nReads != 0)
        {
            // update the buttons
            ULONG numButtons = static_cast<ULONG>(gamepadButtonUsages.size());
            auto ret = HidP_GetButtons(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, gamepadButtonUsages.data(), &numButtons,
                ppdGamepad, reinterpret_cast<PCHAR>(gamepadReport.data()), static_cast<ULONG>(gamepadReport.size()));
            if (ret == HIDP_STATUS_SUCCESS)
            {
                // Update the button states.  gamepadButtonUsages is a list of
                // the button numbers (1..32) that are currently ON.
                gamepadButtonStates = 0;
                for (ULONG i = 0 ; i < numButtons ; ++i)
                    gamepadButtonStates |= (1 << (gamepadButtonUsages[i] - 1));
            }
        }
    }

    // Figure the joystick layout size
    const char *captionGp = (hGamepad == INVALID_HANDLE_VALUE) ? " No Gamepad " : "  Gamepad Buttons  ";
    SIZE szCaption = hdc.MeasureText(boldFont, captionGp);
    int spacingGp = 4;
    int marginGp = 8;
    int cxGp = cxGPBtn*8 + spacingGp*7 + marginGp*2;
    int cyGp = cyGPBtn*4 + spacingGp*3 + marginGp*2 + szCaption.cy;

    // Figure the height needed for the joystick and XInput area, and the of the region
    int cyGpXb = max(cyXBox, cyGp);
    int yGpXb = ykb + cykb + 2*marginKb;

    // Draw the joystick buttons.  The Pinscape gamepad has a fixed 32
    // buttons.
    int xgp = xkb;
    int ygp = yGpXb + (cyGpXb - cyGp)/2;
    RECT frc{ xgp, ygp + szCaption.cy/2, xgp + cxGp, ygp + cyGp };
    FrameRect(hdc, &frc, HBrush(RGB(0xf0, 0xf0, 0xf0)));
    SetBkMode(hdc, OPAQUE);
    hdc.DrawText((frc.right + frc.left - szCaption.cx)/2, ygp, 1, boldFont, 
        HRGB(hGamepad == INVALID_HANDLE_VALUE ? 0xc84848 : 0x202020), captionGp);
    SetBkMode(hdc, TRANSPARENT);
    int y0 = ygp + szCaption.cy + marginGp;
    int x0 = xgp + marginGp;
    for (int i = 0, bit = 1, x = x0, y = y0 ; i < 32 ; ++i, bit <<= 1)
    {
        // draw the button circle
        bool on = ((gamepadButtonStates & bit) != 0);
        bdc.Select(hGamepad == INVALID_HANDLE_VALUE ? bmpGPBtnNA : on ? bmpGPBtnOn : bmpGPBtnOff);
        BitBlt(hdc, x, y, cxGPBtn, cyGPBtn, bdc, 0, 0, SRCCOPY);

        // label it
        char label[5];
        sprintf_s(label, "%d", i + 1);
        SIZE sz = hdc.MeasureText(boldFont, label);
        hdc.DrawText(x + (cxGPBtn - sz.cx)/2, y + (cyGPBtn - sz.cy)/2 - 1, 1, boldFont, RGB(0xFF, 0xFF, 0xFF), label);

        // advance to the next row every 8 buttons
        if ((i % 8) == 7)
        {
            x = x0;
            y += cyGPBtn + spacingGp;
        }
        else
        {
            x += cxGPBtn + spacingGp;
        }
    }

    // draw the XInput controller background graphics
    bdc.Select(xInputId == -1 ? bmpNoXBox : bmpXBox);
    int xxb = xgp + cxGp + marginKb;
    int yxb = yGpXb + (cyGpXb - cyXBox)/2;
    BitBlt(hdc, xxb, yxb, cxXBox, cyXBox, bdc, 0, 0, SRCCOPY);

    // highlight selected XInput controller buttons
    XINPUT_STATE xis;
    if (xInputId >= 0 && XInputGetState(xInputId, &xis) == ERROR_SUCCESS)
    {
        for (unsigned int i = 0, bit = 1 ; i < _countof(xboxLayout) ; ++i, bit <<= 1)
        {
            const auto &xl = xboxLayout[i];
            if (xl.name != nullptr && (xis.Gamepad.wButtons & bit) != 0)
            {
                if (xl.cx != 0)
                {
                    // rectangular layout
                    RECT rc{ xxb + xl.x, yxb + xl.y, xxb + xl.x + xl.cx, yxb + xl.y + xl.cy };
                    HBrush br(RGB(0xff, 0xff, 0x00));
                    FrameRect(hdc, &rc, br);
                    InflateRect(&rc, 1, 1);
                    FrameRect(hdc, &rc, br);
                }
                else if (xl.r != 0)
                {
                    // circular layout
                    HBRUSH oldBr = SelectBrush(hdc, GetStockBrush(HOLLOW_BRUSH));
                    HPen pen(RGB(0xff, 0xff, 0x00), 2);
                    HPEN oldPen = SelectPen(hdc, pen);
                    Ellipse(hdc, xxb + xl.x - xl.r, yxb + xl.y - xl.r, xxb + xl.x + xl.r, yxb + xl.y + xl.r);
                    SelectBrush(hdc, oldBr);
                    SelectPen(hdc, oldPen);
                }
            }
        }
    }

    // read an Open Pinball Device report
    if (openPinDevReader != nullptr)
    {
        OpenPinballDeviceReport r;
        if (openPinDevReader->Read(r))
        {
            // update the button states
            opdGenericButtonStates = r.genericButtons;
            opdPinballButtonStates = r.pinballButtons;
        }
    }

    // Draw the Open Pinball Device generic buttons
    const char *captionOpd = (openPinDevReader == nullptr) ? " No OpenPinDev " : "  OpenPinDev Generic Buttons  ";
    szCaption = hdc.MeasureText(boldFont, captionOpd);
    int xOpd = xxb + cxXBox + marginKb;
    int yOpd = yGpXb + (cyGpXb - cyGp)/2;
    frc = { xOpd, yOpd + szCaption.cy/2, xOpd + cxGp, yOpd + cyGp };
    FrameRect(hdc, &frc, HBrush(RGB(0xf0, 0xf0, 0xf0)));
    SetBkMode(hdc, OPAQUE);
    hdc.DrawText((frc.right + frc.left - szCaption.cx)/2, ygp, 1, boldFont,
        HRGB(openPinDevReader == nullptr ? 0xc84848 : 0x202020), captionOpd);
    SetBkMode(hdc, TRANSPARENT);
    y0 = yOpd + szCaption.cy + marginGp;
    x0 = xOpd + marginGp;
    for (int i = 0, bit = 1, x = x0, y = y0 ; i < 32 ; ++i, bit <<= 1)
    {
        // draw the button circle
        bool on = ((opdGenericButtonStates & bit) != 0);
        bdc.Select(openPinDevReader == nullptr ? bmpGPBtnNA : on ? bmpGPBtnOn : bmpGPBtnOff);
        BitBlt(hdc, x, y, cxGPBtn, cyGPBtn, bdc, 0, 0, SRCCOPY);

        // label it
        char label[5];
        sprintf_s(label, "%d", i + 1);
        SIZE sz = hdc.MeasureText(boldFont, label);
        hdc.DrawText(x + (cxGPBtn - sz.cx)/2, y + (cyGPBtn - sz.cy)/2 - 1, 1, boldFont, RGB(0xFF, 0xFF, 0xFF), label);

        // advance to the next row every 8 buttons
        if ((i % 8) == 7)
        {
            x = x0;
            y += cyGPBtn + spacingGp;
        }
        else
        {
            x += cxGPBtn + spacingGp;
        }
    }

    // Draw the Open Pinball Device predefined buttons, as a single row 
    // of icons below the gamepad/xInput section.
    const char *captionOpd2 = openPinDevReader == nullptr ? " No OpenPinDev " : " OpenPinDev Named Buttons ";
    szCaption = hdc.MeasureText(boldFont, captionOpd2);
    int xOpd2 = xgp;
    int yOpd2 = yGpXb + cyGpXb + 2;
    int cxOpd2 = cxOPDBtn*27 + 4;
    int cyOpd2 = cyOPDBtn + szCaption.cy + 4;
    frc = { xOpd2, yOpd2 + szCaption.cy/2, xOpd2 + cxOpd2, yOpd2 + cyOpd2 };
    FrameRect(hdc, &frc, HBrush(RGB(0xf0, 0xf0, 0xf0)));
    SetBkMode(hdc, OPAQUE);
    hdc.DrawText((frc.right + frc.left - szCaption.cx)/2, yOpd2, 1, boldFont,
        HRGB(openPinDevReader == nullptr ? 0xc84848 : 0x202020), captionOpd2);
    SetBkMode(hdc, TRANSPARENT);
    x0 = xOpd2 + 2;
    y0 = yOpd2 + szCaption.cy;
    bdc.Select(bmpOpenPinDevButtons);
    for (int i = 0, bit = 1, x = x0, y = y0 ; i < 27 ; ++i, bit <<= 1)
    {
        // Get the button state
        bool on = (opdPinballButtonStates & bit) != 0;

        // draw the button circle; the bitmap's top row is ON, bottom row is OFF
        BitBlt(hdc, x, y, cxOPDBtn, cyOPDBtn, bdc, i*cxOPDBtn, on ? 0 : cyOPDBtn, SRCCOPY);

        // set the tooltip on the first layout pass
        if (layoutPending)
        {
            RECT rcToolTip{ x, y, x + cxOPDBtn, y + cyOPDBtn };
            SetTooltip(rcToolTip, i + 100, opdButtonNames[i]);
        }

        // advance to the next cell (it's a single row, so just move horizontally)
        x += cxOPDBtn;
    }


    // separator bar under the joystick/XInput/OPD section
    y = cyKeyJoyPanel;
    MoveToEx(hdc, crc.left + cxPanel + cxScrollbar, y, NULL);
    LineTo(hdc, crc.right, y);
    y += marginKb;

    // lock the mutex so that we can access the current state data
	if (WaitForSingleObject(updaterThread->dataMutex, 100) == WAIT_OBJECT_0)
	{
		// cast to our subclass
		auto *ut = static_cast<UpdaterThread*>(updaterThread.get());

        // draw the button state header
        RECT hrc{ crc.left, crc.top, crc.left + cxPanel + cxScrollbar, crc.top + cyHeader };
        FillRect(hdc, &hrc, HBrush(RGB(0xF0,0xF0, 0xF0)));
        hdc.DrawText(crc.left + 16, (hrc.top + hrc.bottom - szBoldFont.cy)/2, 1, boldFont, RGB(0,0,0), "Logical Buttons");

        // figure some default (minimum) column widths for the button state list
        int onOffColWidth = hdc.MeasureText(boldFont, "ONf**").cx;
        int shiftColWidth = shiftBitsUsed != 0 ? hdc.MeasureText(boldFont, "X").cx * 10 : 0;
        int typeColWidth = hdc.MeasureText(mainFont, "Toggle Button XXX").cx;

		// draw the button states
        IntersectClipRect(hdc, crc.left, hrc.bottom, hrc.right, crc.bottom);
		RECT brc{ crc.left, crc.top + cyHeader - yScrollBtns, crc.left + cxPanel, crc.top + cyHeader - yScrollBtns + cyButton - 1 };
		const auto *pState = ut->buttonState.data();
		const auto *pDesc = buttonDescs.data();
		for (unsigned int i = 0 ; i < ut->buttonState.size() && i < buttonDescs.size() ; 
			++i, ++pState, ++pDesc, OffsetRect(&brc, 0, cyButton))
		{
			// start at left, with margins
			POINT pt{ brc.left + 16, brc.top + yMarginButton/2 };

			// fill background with yellow if button is on
			if (*pState != 0)
				FillRect(hdc, &brc, HBrush(RGB(0xff, 0xff, 0x00)));

			// button number
			hdc.DrawTextF(pt.x, pt.y, 1, boldFont, RGB(0, 0, 0), "#%u", i);
			pt.x += hdc.MeasureText(boldFont, "#000  ").cx;

			// on/off status
			if (*pState != 0)
				hdc.DrawText(pt.x, pt.y, 1, boldFont, RGB(0xFF, 0x00, 0xFF), "ON");
			else
				hdc.DrawText(pt.x, pt.y, 1, mainFont, RGB(0x80, 0x80, 0x80), "Off");
            pt.x += onOffColWidth;

            // Draw the shift bits for the current button
            auto DrawShiftBits = [this, &hdc, &bdc, &pt, pDesc](POINT pt)
            {
                // Get the shift mask.  For a regular button, this is the explicit
                // mask set in the descriptor.  A shift button doesn't really have
                // a mask, but since we use the mask here to determine which bits
                // to include in the display, and since we want to display all of
                // the bits in shiftBits, use shiftBits as the mask.
                uint32_t shiftMask = pDesc->type == ButtonDesc::TYPE_SHIFT ? pDesc->shiftBits : pDesc->shiftMask;

                // Only draw anything at all if the shift mask is non-zero.  If
                // the shift mask is zero, the button is completely unaffected
                // by the shift state, so don't display anything.
                if (shiftMask != 0)
                {
                    // if it's wholly unshifted, use the Unshift icon; otherwise
                    // use the Shift icon
                    bdc.Select(bmpShiftIcon);
                    BitBlt(hdc, pt.x, pt.y + (szMainFont.cy - cyShiftIcon)/2, cxShiftIcon, cyShiftIcon, bdc,
                        ((pDesc->shiftBits & shiftMask) == 0) ? cxShiftIcon : 0, 0, SRCCOPY);
                    pt.x += cxShiftIcon;

                    // draw only the shift bits that are included in the mask, since
                    // these are the only ones that affect the button's activation
                    for (int i = 1, nDisplayed = 0, bit = 1 ; i <= 32 ; ++i, bit <<= 1)
                    {
                        if ((shiftMask & bit) != 0)
                        {
                            bool on = (pDesc->shiftBits & bit) != 0;
                            pt.x += hdc.DrawTextF(pt.x, pt.y, 1, boldFont, on ? RGB(0x00, 0xB0, 0x00) : RGB(0xC0, 0xC0, 0xC0),
                                "%s%d", nDisplayed != 0 ? "+" : "", bit).cx;
                            ++nDisplayed;
                        }
                    }
                }
                return pt;
            };

            // draw the shift bits, EXCEPT for shift buttons, where this goes in
            // the type column instead of the shift column (since shift buttons
            // aren't themselves sensitive to the shift state)
            int startX = pt.x;
            if (pDesc->type != ButtonDesc::TYPE_SHIFT)
                pt = DrawShiftBits(pt);

            // advance to the next column
            pt.x = max(pt.x + 8, startX + shiftColWidth);

			// type description
			static const char *typeNames[] ={ "None", "Pushbutton", "Hold Button", "Pulse Button", "Toggle Button", "On/Off Toggle", "Shift " };
            startX = pt.x;
            pt.x += hdc.DrawText(pt.x, pt.y, 1, mainFont, RGB(0, 0, 0), (pDesc->type < _countof(typeNames)) ? typeNames[pDesc->type] : "Unknown").cx;

            // add the shift button ID if it's a shift button
            if (pDesc->type == ButtonDesc::TYPE_SHIFT)
                pt = DrawShiftBits(pt);

            // advance past the column
            pt.x = max(pt.x + 8, startX + typeColWidth);

			// source description
			static const char *srcNames[] ={ "None", "GPIO", "BOOTSEL", "PCA9555", "74HC165", "Accelerometer", "Plunger", "ZB Launch", "IR RX", "Time", "Out Port" };
			char srcDesc[32];
			if (pDesc->sourceType == PinscapePico::ButtonDesc::SRC_GPIO)
				sprintf_s(srcDesc, "GPIO (GP%d)", pDesc->sourcePort);
			else if (pDesc->sourceType == PinscapePico::ButtonDesc::SRC_PCA9555)
				sprintf_s(srcDesc, "PCA9555[%d] port IO%d_%d", pDesc->sourceUnit, pDesc->sourcePort / 8, pDesc->sourcePort % 8);
			else if (pDesc->sourceType == PinscapePico::ButtonDesc::SRC_74HC165)
				sprintf_s(srcDesc, "74HC165[%d] chip %d port %c", pDesc->sourceUnit, pDesc->sourcePort / 8, (pDesc->sourcePort % 8) + 'A');
			else if (pDesc->sourceType == PinscapePico::ButtonDesc::SRC_OUTPORT)
				sprintf_s(srcDesc, "Output Port #%d", pDesc->sourcePort);
			else
				sprintf_s(srcDesc, "%s", (pDesc->sourceType < _countof(srcNames)) ? srcNames[pDesc->sourceType] : "Unknown");

			// action description
			static const char *actionNames[] ={ 
                "Invalid", "None", "Keyboard", "Media", "Gamepad", "XInput", "Pico Reboot", 
                "Night Mode", "Plunger Cal", "IR TX", "Macro", "OpenPinDev" };
			char actionDesc[32];
            if (pDesc->actionType == PinscapePico::ButtonDesc::ACTION_KEY)
            {
                if (auto it = usbKeyNames.find(pDesc->actionDetail); it != usbKeyNames.end())
                {
                    if (strncmp(it->second, "Keypad", 6) == 0 || strncmp(it->second, "Keyboard", 8) == 0)
                        sprintf_s(actionDesc, "%s", it->second);
                    else
                        sprintf_s(actionDesc, "Keyboard %s", it->second);
                }
                else
                    sprintf_s(actionDesc, "Keyboard(keycode 0x%02x)", pDesc->actionDetail);
            }
            else if (pDesc->actionType == PinscapePico::ButtonDesc::ACTION_MEDIA)
            {
                if (auto it = mediaKeyNames.find(pDesc->actionDetail); it != mediaKeyNames.end())
                    sprintf_s(actionDesc, "Media Key %s", it->second);
                else
                    sprintf_s(actionDesc, "Media Key(code 0x%02x)", pDesc->actionDetail);
            }
            else if (pDesc->actionType == PinscapePico::ButtonDesc::ACTION_GAMEPAD)
                sprintf_s(actionDesc, "Gamepad Button %d", pDesc->actionDetail);
            else if (pDesc->actionType == PinscapePico::ButtonDesc::ACTION_OPENPINDEV)
            {
                if (pDesc->actionDetail >= 33 && pDesc->actionDetail <= 33 + _countof(opdButtonNames))
                    sprintf_s(actionDesc, "OpenPinDev '%s'", opdButtonNames[pDesc->actionDetail - 33]);
                else
                    sprintf_s(actionDesc, "OpenPinDev Button %d", pDesc->actionDetail);
            }
			else if (pDesc->actionType == PinscapePico::ButtonDesc::ACTION_XINPUT)
			{
				if (pDesc->actionDetail < _countof(xinputButtonNames))
					sprintf_s(actionDesc, "XInput %s", xinputButtonNames[pDesc->actionDetail]);
				else
					sprintf_s(actionDesc, "XInput Button 0x%02x", pDesc->actionDetail);
			}
			else
				sprintf_s(actionDesc, "%s", (pDesc->actionType < _countof(actionNames)) ? actionNames[pDesc->actionType] : "Unknown");

			// show the source => action portion
			pt.x += hdc.DrawTextF(pt.x, pt.y, 1, mainFont, RGB(0, 0, 0),
				"%s => %s", srcDesc, actionDesc).cx;

			// separator
			MoveToEx(hdc, brc.left, brc.bottom, NULL);
			LineTo(hdc, brc.right, brc.bottom);
		}
        SelectClipRgn(hdc, NULL);

        // fill in the shift states
        if (shiftBitsUsed != 0)
        {
            // scan all shift bits
            for (int i = 1, bit = 1, x = ptShiftState.x ; i <= 32 ; ++i, bit <<= 1)
            {
                // only draw bits actually used
                if ((shiftBitsUsed & bit) != 0)
                {
                    // note if the shift is currently activated
                    bool on = ((ut->shiftState & bit) != 0);

                    // draw the shift icon, in the appropriate on/off state
                    x += 8;
                    bdc.Select(bmpShiftIcon);
                    BitBlt(hdc, x, ptShiftState.y + (szMainFont.cy - cyShiftIcon)/2, cxShiftIcon, cyShiftIcon,
                        bdc, on ? 0 : cxShiftIcon, 0, SRCCOPY);
                    x += cxShiftIcon;

                    // draw the shift number, in the appropriate on/off color
                    x += hdc.DrawTextF(x, ptShiftState.y, 1, boldFont, on ? RGB(0x00, 0xB0, 0x00) : RGB(0xc0, 0xc0, 0xc0), "%d", i).cx;
                }
            }
        }

        // draw a chip port
        auto DrawPort = [this, &hdc](int x, int y, int portNum, bool state)
        {
            // draw the rectangle
            RECT rc{ x, y, x + portBoxSize, y + portBoxSize };
            FillRect(hdc, &rc, HBrush(state ? RGB(0x0, 0xC0, 0x0) : RGB(0xB0, 0xB0, 0xB0)));
            FrameRect(hdc, &rc, HBrush(RGB(0x40, 0x40, 0x40)));

            char lbl[10];
            sprintf_s(lbl, "%d", portNum);
            SIZE sz = hdc.MeasureText(boldFont, lbl);
            hdc.DrawText(x + (portBoxSize - sz.cx)/2, y + (portBoxSize - sz.cy)/2, 1, boldFont, RGB(0xff, 0xff, 0xff), lbl);

            return x + portBoxSize + portBoxSpacing;
        };

        // clip to the physical outputs panel
        IntersectClipRect(hdc, cxPanel, cyKeyJoyPanel + 1, crc.right - cxScrollbar, crc.bottom);

        // adjust y for scrolling, and remember where we started
        y -= yScrollPhys;
        y0 = y;

        // GPIO ports - GP0 to GP29
        if (ut->gpioState.size() >= 32)
        {
            y += hdc.DrawText(xkb, y, 1, boldFont, RGB(0, 0, 0), "Pico GPIO Ports").cy;
            for (int i = 0, x = xkb ; i <= 29 ; ++i)
                x = DrawPort(x, y, i, ut->gpioState[i] != 0);
            y += portBoxSize + 8;
        }

        // PCA9555 ports, if present
        if (ut->pca9555State.size() != 0)
        {
            y += hdc.DrawText(xkb, y, 1, boldFont, RGB(0, 0, 0), "PCA9555 Ports").cy;
            int idx = 0;
            for (auto &d : buttonDevices)
            {
                // check if this is a PCA9555 descriptor
                if (d.type == ButtonDesc::SRC_PCA9555)
                {
                    // Draw the ports.  Each PCA9555 descriptor corresponds to
                    // one physical chip, which has 16 ports.
                    int x = xkb;
                    for (int port = 0 ; port < 16 ; ++port)
                        x = DrawPort(x, y, port, ut->pca9555State[idx++] != 0);

                    // label it
                    hdc.DrawTextF(x, y + (portBoxSize - szMainFont.cy)/2, 1, mainFont, RGB(0, 0, 0),
                        "  Chip #%d (I2C%d 0x%02X)", d.configIndex, d.addr >> 8, d.addr & 0xFF);

                    y += portBoxSize + 4;
                }
            }
            y += 8;
        }

        // 74HC165 ports, if present
        if (ut->hc165State.size() != 0)
        {
            y += hdc.DrawText(xkb, y, 1, boldFont, RGB(0, 0, 0), "74HC165 Ports").cy;
            int idx = 0;
            for (auto &d : buttonDevices)
            {
                // check if this is a 74HC165 descriptor
                if (d.type == ButtonDesc::SRC_74HC165)
                {
                    // Draw the ports.  Each 74HC165 descriptor corresponds to
                    // a complete daisy chain of 74HC165 chips.  Each chip has
                    // 8 physical ports, so we can infer the number of chips
                    // from the total number of ports in the chain.  Draw each
                    // chip separately.
                    for (int chip = 0, dcPort = 0 ; dcPort < d.numPorts ; ++chip, dcPort += 8)
                    {
                        // draw the ports for one chip
                        int x = xkb;
                        for (int port = 0 ; port < 8 ; ++port)
                            x = DrawPort(x, y, port, ut->hc165State[idx++] != 0);

                        // label it
                        hdc.DrawTextF(x, y + (portBoxSize - szMainFont.cy)/2, 1, mainFont, RGB(0, 0, 0),
                            "  Chain %d, chip %d", d.configIndex, chip);

                        y += portBoxSize + 4;
                    }
                }
            }
            y += 8;
        }

        // Update the physical input panel "document" height.  If this is
        // different from before, adjust the scrollbar range accordingly.
        if (physPanelDocHeight != y - y0)
        {
            physPanelDocHeight = y - y0;
            AdjustScrollbarRanges();
        }

        // remove the clipping area
        SelectClipRgn(hdc, NULL);

        // done with the struct mutex
		ReleaseMutex(updaterThread->dataMutex);
	}

    // clean up drawing resources
    SelectPen(hdc, oldPen);
}

void ButtonTesterWin::AdjustLayout()
{
	// get the client size
	RECT crc;
	GetClientRect(hwnd, &crc);

	// Figure the panel width - give it the whole window width
    // minus the width of the right panel, or the minimum panel
    // width, whichever is larger.
    //
    // The right panel has to be wide enough to contain the
    // keyboard background graphic and the physical device input
    // status displays.  The widest physical device display is
    // the GPIO status, which shows 30 GPIO ports.  We also need
    // room for a scrollbar.
    const int rightPanelWidth = max(cxkb, portBoxSize*30 + 29*portBoxSpacing + cxScrollbar);
	cxPanel = max(cxPanelMin, crc.right - crc.left - rightPanelWidth - marginKb*2 - cxScrollbar);

	// position the button scrollbar
	MoveWindow(sbBtnPanel, cxPanel, crc.top + cyHeader, cxScrollbar, crc.bottom - cyHeader, TRUE);

    // Figure the height of the keyboard/joystick panel area
    cyKeyJoyPanel = marginKb*5 + cykb + cyXBox + (cyOPDBtn + mainFontMetrics.tmHeight + 4) + 1;
    if (shiftBitsUsed != 0)
        cyKeyJoyPanel += marginShiftState*2 + szBoldFont.cy + 1;

    // position the physical inputs panel scrollbar
    MoveWindow(sbPhysPanel, crc.right - cxScrollbar, cyKeyJoyPanel + 1, cxScrollbar, crc.bottom - cyKeyJoyPanel - 1, TRUE);

    // adjust layout on the next draw
    layoutPending = true;
}

void ButtonTesterWin::OnCreateWindow()
{
	// do the base class work
	__super::OnCreateWindow();

    // Register for device removal notifications.  We might have already
    // connected to the USB devices before the window was open, but we
    // can't register until we have a window handle, so register now.
    RegisterDeviceNotifiers();

    // create a tooltips control, for identifying some of the on-screen
    // icons that might not be entirely self-explanatory on the strength
    // of their graphics
    CreateTooltipControl();

    // Load my menu bar and accelerator
    hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_DEVICEWIN));
    hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_DEVICEWIN));

    // add our system menu items, when running as a top-level window
    if ((GetWindowStyle(hwnd) & WS_CHILD) == 0)
    {
        HMENU sysMenu = GetSystemMenu(hwnd, FALSE);

        MENUITEMINFOA mii{ sizeof(MENUITEMINFOA), MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_CHECKMARKS };
        mii.fType = MFT_STRING;
        mii.dwTypeData = const_cast<LPSTR>("Help");
        mii.wID = ID_HELP;
        mii.hbmpChecked = mii.hbmpUnchecked = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_MENUICON_HELP));
        InsertMenuItemA(sysMenu, SC_CLOSE, FALSE, &mii);

        mii.fMask = MIIM_FTYPE;
        mii.fType = MFT_SEPARATOR;
        InsertMenuItemA(sysMenu, SC_CLOSE, FALSE, &mii);
    }

    // set up for layout computation - get the client area size and window DC
    RECT crc;
    GetClientRect(hwnd, &crc);
    HDC dc = GetWindowDC(hwnd);

    // figure the width of the button panel and height of a button
    HFONT oldfont = SelectFont(dc, mainFont);
    GetTextExtentPoint32A(dc, "M", 1, &szMainFont);
    cxPanelMin = szMainFont.cx * 42;
    cyHeader = szMainFont.cy + 10;
    cyButton = szMainFont.cy + yMarginButton + 1;

    // get the bold font size
    SelectFont(dc, boldFont);
    GetTextExtentPoint32A(dc, "M", 1, &szBoldFont);

    // create the button panel scrollbar
	cxScrollbar = GetSystemMetrics(SM_CXVSCROLL);
	sbBtnPanel = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
		0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_MAIN), hInstance, 0);

    // range calculation for the button scroll panel
    auto GetRangeBtns = [this](SCROLLINFO &si)
    {
        // figure the client area height
        RECT crc;
        GetClientRect(hwnd, &crc);
        int winHt = crc.bottom - crc.top;

        // figure the document height - number of buttons x button height
        int docHt = cyButton * static_cast<int>(buttonDescs.size()) + 8;

        // set the range
        si.nMin = 0;
        si.nMax = max(docHt, 0);
        si.nPage = max(winHt - cyButton, 20);
    };

    // get the button scroll region
    auto GetScrollRectBtns = [this](RECT *rc)
    {
        rc->top += cyHeader;
        rc->right = rc->left + cxPanel;
    };

    // change the scroll position for the button scroll panel
    auto SetScrollPosBtns = [this](int newPos, int deltaPos) { yScrollBtns = newPos; };

    // set up the scrollbar object
    scrollbars.emplace_back(sbBtnPanel, SB_CTL, cyButton, true, true, GetRangeBtns, GetScrollRectBtns, SetScrollPosBtns);

    // Physical inputs panel scrollbar
    sbPhysPanel = CreateWindowA(WC_SCROLLBARA, "", WS_VISIBLE | WS_CHILD | SBS_VERT,
        0, 0, cxScrollbar, 100, hwnd, reinterpret_cast<HMENU>(ID_SB_DEVS), hInstance, 0);

    // range calculation for the physical inputs panel
    auto GetRangePhys = [this](SCROLLINFO &si)
    {
        // figure the client area height
        RECT crc;
        GetClientRect(hwnd, &crc);
        int panelHt = crc.bottom - crc.top - cyKeyJoyPanel - 1;

        // set the range
        si.nMin = 0;
        si.nMax = max(physPanelDocHeight, 0);
        si.nPage = max(panelHt - cyLineScrollPhys, 20);
    };

    // get the physical inputs panel scrolling area
    auto GetScrollRectPhys = [this](RECT *rc)
    {
        rc->top += cyKeyJoyPanel + 1;
        rc->left += cxPanel;
        rc->right -= cxScrollbar;
    };

    // change the scroll position for the physical inputs panel
    auto SetScrollPosPhys = [this](int newPos, int deltaPos) { yScrollPhys = newPos; };

    // set up the scrollbar object
    scrollbars.emplace_back(sbPhysPanel, SB_CTL, cyLineScrollPhys, true, true, GetRangePhys, GetScrollRectPhys, SetScrollPosPhys);

	// adjust the layout
	AdjustLayout();
	
	// done with the window DC - clean it up and release it
	SelectFont(dc, oldfont);
	ReleaseDC(hwnd, dc);
}

bool ButtonTesterWin::InstallParentMenuBar(HWND hwndContainer)
{
    SetMenu(hwndContainer, hMenuBar);
    return true;
}

// translate keyboard accelerators
bool ButtonTesterWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

bool ButtonTesterWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (ctlCmdId)
    {
    case ID_HELP_HELP:
        // note - this applies when we're running as a child window under a
        // container app like the Config Tool, where the parent frame window
        // provides a standard Help menu with this item
        ShowHelpFile("ButtonTester.htm");
        return true;
    }

    // not handled
    return false;
}

bool ButtonTesterWin::OnSysCommand(WPARAM id, WORD x, WORD y, LRESULT &lresult)
{
    switch (id)
    {
    case ID_HELP:
        // note - this applies when we're running as a top-level window, where we
        // install our Help command in the system window menu
        ShowHelpFile("ButtonTester.htm");
        return true;
    }

    // not handled
    return false;
}

void ButtonTesterWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the layout
	AdjustLayout();

    // do the base class work
    __super::OnSizeWindow(type, width, height);
}

bool ButtonTesterWin::OnKeyDown(WPARAM vkey, LPARAM flags)
{
    // mark the key as down
    return OnKeyEvent(vkey, flags, true);
}

bool ButtonTesterWin::OnKeyUp(WPARAM vkey, LPARAM flags)
{
    // mark the key as up
    return OnKeyEvent(vkey, flags, false);
}

bool ButtonTesterWin::OnKeyEvent(WPARAM vkey, LPARAM flags, bool down)
{
    // get the scan code and extended key bit
    int scanCode = static_cast<int>((flags >> 16) & 0x1ff);

    // Empirically, the media keys on the Consumer usage page all
    // send can codes of 0 with the extended bit set.  This is
    // contrary to the documentation, so it must be either a
    // documentation erratum or a bug in the keyboard driver.  In
    // any case, we *do* get the correct virtual key in these cases,
    // so look it up by virtual key.  It's still preferable to look 
    // it up by scan code when possible, since the scan code can
    // distinguish some of the alternate keys (such as left/right
    // modifiers and numpad keys) that the VK_code can't.
    if (scanCode == 0 || scanCode == 0x100)
    {
        // bad scan code - look it up by VK_ instead
        InitVKtoScanCode();
        scanCode = vkToScanCode[vkey];
    }

    // update the key state
    keysDown[scanCode] = down;

    // handled
    return true;
}

bool ButtonTesterWin::OnSysKeyDown(WPARAM vkey, LPARAM flags)
{
    return OnKeyDown(vkey, flags);
}

bool ButtonTesterWin::OnSysKeyUp(WPARAM vkey, LPARAM flags)
{
    return OnKeyUp(vkey, flags);
}

void ButtonTesterWin::OnActivate(WPARAM code, HWND other)
{
    // do the base class work
    __super::OnActivate(code, other);

    // If activating, refresh activate keys.  We keep track via
    // WM_KEYUP/WM_KEYDOWN when we're in the foreground, but that
    // only works when we have focus, so we have to re-sync with
    // the current keyboard state when we've been in the background.
    RefreshKeysDown();
}

void ButtonTesterWin::RefreshKeysDown()
{
    // clear the key list
    memset(keysDown, 0, sizeof(keysDown));

    // get the current keyboard state
    BYTE k[256];
    if (GetKeyboardState(k))
    {
        // Set each entry in the keysDown map.  keysDown is indexed by virtual
        // scan code, whereas the GetKeyboardState() is indexed by VK_xxx code.
        // Our layout array can serve as the Rosetta stone.
        for (size_t scanCode = 0 ; scanCode < _countof(keyboardLayout) ; ++scanCode)
        {
            // if this scan code entry has a valid VK_xxx value, and the associated
            // VK_xxx entry in k[] has the high bit set, then the key is down, so
            // mark it as highlighted in our key state array.
            int vkey = keyboardLayout[scanCode].vkey;
            if (vkey >= 0 && (k[vkey] & 0x80) != 0)
                keysDown[scanCode] = true;
        }
    }
}

// Updater thread
bool ButtonTesterWin::UpdaterThread::Update(bool &releasedMutex)
{
	// update button states
	std::vector<BYTE> buttons;
    uint32_t shiftState;
	bool ok = (device->device->QueryLogicalButtonStates(buttons, shiftState) == PinscapeResponse::OK);

	// update GPIO states
	std::vector<BYTE> gpios;
	ok = ok && (device->device->QueryButtonGPIOStates(gpios) == PinscapeResponse::OK);

	// update PCA9555 states
	std::vector<BYTE> pca9555;
	ok = ok && (device->device->QueryButtonPCA9555States(pca9555) == PinscapeResponse::OK);

	// update 74HC165 states
	std::vector<BYTE> hc165;
	ok = ok && (device->device->QueryButton74HC165States(hc165) == PinscapeResponse::OK);

    // done with the device
    ReleaseMutex(device->mutex);
    releasedMutex = true;

	// if the queries succeeded, update our internal records
	if (ok)
	{
		// success - update the data in the context, holding the mutex while updating
		if (WaitForSingleObject(dataMutex, 100) == WAIT_OBJECT_0)
		{
            this->shiftState = shiftState;
			this->buttonState = buttons;
			this->gpioState = gpios;
			this->pca9555State = pca9555;
			this->hc165State = hc165;

			// done accessing the shared data
			ReleaseMutex(dataMutex);

			// let the main thread know about the update
			PostMessage(hwnd, DeviceThreadWindow::MSG_NEW_DATA, 0, 0);
		}
	}

    // return the result
	return ok;
}
