// Pinscape Pico - Config Tool Boot Loader Drive window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <Windows.h>
#include <windowsx.h>
#include "Application.h"
#include "FlashProgressDialog.h"
#include "FirmwareDropTarget.h"
#include "WrapperWin.h"
#include "Dialog.h"
#include "WinUtil.h"
#include "BootLoaderWin.h"
#include "resource.h"

using namespace PinscapePico;

BootLoaderWin::BootLoaderWin(HINSTANCE hInstance, RP2BootDevice *device) :
    device(*device),
    BaseWindow(hInstance)
{
    // Load my menu bar and accelerator
    hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_BOOTLOADERWIN));
    hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(ID_ACCEL_BOOTLOADERWIN));
}

BootLoaderWin::~BootLoaderWin()
{
    // detach from the Drop Target interface
    dropTarget->Detach();
    dropTarget->Release();
}

void BootLoaderWin::OnCreateWindow()
{
    // do the base class work
    __super::OnCreateWindow();

    // register as a drop target
    RegisterDragDrop(hwnd, dropTarget);
}

LRESULT BootLoaderWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case MSG_INSTALL_FIRMWARE:
        // deferred firmware install from drag/drop
        InstallFirmwareFile(pendingFirmwareInstall.c_str());
        pendingFirmwareInstall.clear();
        return 0;
    }

    // use the default handling
    return __super::WndProc(msg, wparam, lparam);
}

void BootLoaderWin::OnDestroy()
{
    // remove the drop target registration
    RevokeDragDrop(hwnd);

    // do the base class work
    __super::OnDestroy();
}

bool BootLoaderWin::InstallParentMenuBar(HWND hwndContainer)
{
    SetMenu(hwndContainer, hMenuBar);
    return true;
}

// translate keyboard accelerators
bool BootLoaderWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    return ::TranslateAccelerator(hwndMenu, hAccel, msg);
}

// Paint off-screen
void BootLoaderWin::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // get the mouse position in client coordinates
    POINT ptMouse;
    GetCursorPos(&ptMouse);
    ScreenToClient(hwnd, &ptMouse);

    // fill the background
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xFFFFFF)));

    // show the message
    int x = 16;
    int y = 16;
    y += hdc.DrawText(x, y, 1, boldFont, HRGB(0x800080), "Pico Firmware Installer").cy + 8;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "This Pico is in Boot Loader mode, which allows installing new firmware onto the").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "Pico.  You can use this to install Pinscape, or any other Pico firmware.").cy + 16;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "You can use this to install Pinscape onto a brand new Pico or to update an").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "existing system to a new version.  Your configuration settings will be preserved").cy;
    y += hdc.DrawText(x, y, 1, mainFont, HRGB(0x000000), "if you're upgrading from a previous version.").cy;

    // set up to draw the drop/select button
    const char *label1 = "Install Firmware";
    const char *label2 = "Drop a .UF2 file here or click to select a file";
    SIZE sz = hdc.MeasureText(mainFont, label2);
    y += 40;
    RECT brc{ x, y, x + sz.cx + 100, y + sz.cy*3/2 + 60 };
    rcFirmwareInstallButton = brc;
    bool hot = IsClientRectHot(brc) && (!dropTarget->IsDragActive() || dropTarget->IsTargetHot());

    // draw the rounded rect
    HBrush br(hot ? HRGB(0xd0ffff) : HRGB(0xf0f0f0));
    HPen pen(hot ? HRGB(0xff00ff) : HRGB(0x808080));
    HBRUSH oldbr = SelectBrush(hdc, br);
    HPEN oldpen = SelectPen(hdc, pen);
    RoundRect(hdc, brc.left, brc.top, brc.right, brc.bottom, 24, 24);
    SelectBrush(hdc, oldbr);
    SelectPen(hdc, oldpen);

    // draw the label
    COLORREF txtColor = (hot ? HRGB(0xa000a0) : HRGB(0x000000));
    sz = hdc.MeasureText(mainFont, label1);
    int yTxt = (brc.top + brc.bottom - sz.cy*2)/2;
    yTxt += hdc.DrawText((brc.left + brc.right - sz.cx)/2, yTxt, 1, boldFont, txtColor, label1).cy;

    sz = hdc.MeasureText(mainFont, label2);
    yTxt += hdc.DrawText((brc.left + brc.right - sz.cx)/2, yTxt, 1, mainFont, txtColor, label2).cy;
}

void BootLoaderWin::OnInitMenuPopup(HMENU menu, WORD itemPos, bool isSysMenu)
{
    // enable all of the device-related commands
    EnableMenuItem(menu, ID_DEVICE_INSTALLFIRMWARE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_DEVICE_CLEARFLASHFILEAREA, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_DEVICE_CLEARENTIREFLASH, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_DEVICE_REBOOT, MF_BYCOMMAND | MF_ENABLED);
}

bool BootLoaderWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (notifyCode)
    {
    case 0:
    case 1:
        // menu/accelerator/button click
        switch (ctlCmdId)
        {
        case ID_DEVICE_OPENINEXPLORER:
            // open the drive path in the Windows shell
            ShellExecute(hwnd, _T("open"), device.path.c_str(), NULL, NULL, SW_SHOW);
            break;

        case ID_DEVICE_INSTALLFIRMWARE:
            // install firmware, using a file selected via a standard File Open dialog
            {
                // get the file to install
                GetFileNameDlg dlg(_T("Select a Pinscape Pico firmware file"),
                    OFN_ENABLESIZING, _T("UF2 Files\0*.uf2\0All Files\0*.*\0"), _T("uf2"));
                if (dlg.Open(GetDialogOwner()))
                {
                    // run the installation
                    InstallFirmwareFile(dlg.GetFilename());
                }
            }
            return true;

        case ID_DEVICE_CLEARFLASHFILEAREA:
            // erase the Pinscape flash file system control block
            if (MessageBoxA(GetDialogOwner(),
                "Warning!  This operation will erase all configuration files, settings, and "
                "calibration data saved on this Pico.  This can't be undone; the files will "
                "be unrecoverable once erased.  Pinscape will revert to factory defaults "
                "for all settings.\r\n"
                "\r\n"
                "Are you sure you want to erase all saved data on the Pico?",
                "Confirm Factory Reset",
                MB_ICONEXCLAMATION | MB_YESNOCANCEL | MB_DEFBUTTON3) == IDYES)
            {
                if (device.EraseConfigData())
                    gApp.AddTimedStatusMessage("Success - the Pico has been restored to factory default settings",
                        HRGB(0xffffff), HRGB(0x008000), 5000);
                else
                    MessageBoxFmt(hwnd, "Erase operation failed");
            }
            return true;

        case ID_DEVICE_CLEARENTIREFLASH:
            // erase the entire flash memory space
            if (MessageBoxA(GetDialogOwner(),
                "Warning!  This operation will erase EVERYTHING from the Pico's flash memory, "
                "including any installed firmware (Pinscape or otherwise) and any saved data.\r\n"
                "\r\n"
                "The Pico won't be bootable after this operation, since all firmware will be "
                "erased. You'll still be able to get the Pico into Boot Loader mode via the "
                "usual method of holding down the BOOTSEL button while plugging in the USB cable.\r\n"
                "\r\n"
                "The operation will take a few minutes if you proceed.\r\n"
                "\r\n"
                "Are you sure you want to erase the entire Pico flash?",
                "Confirm Total Erasure",
                MB_ICONEXCLAMATION | MB_YESNOCANCEL | MB_DEFBUTTON3) == IDYES)
            {
                // run the copy operation via the flash progress dialog
                bool ok = false;
                FlashProgressDialog dlg(hInstance, [this, &ok](FlashProgressDialog *dlg) { ok = device.EraseWholeFlash(dlg); });
                dlg.SetCaption(_T("Erasing Pico Flash"));
                dlg.SetBannerText(_T("Erasing Pico flash"));
                dlg.Show(IDD_FLASH_COPY_PROGRESS, GetDialogOwner());
    
                // announce the result
                if (ok)
                    gApp.AddTimedStatusMessage("Success - the Pico's flash has been completely erased",
                        HRGB(0xffffff), HRGB(0x008000), 5000);
                else
                    MessageBoxFmt(hwnd, "Erase operation failed");
            }
            return true;

        case ID_DEVICE_REBOOT:
            // Notify the parent window that we're about to reboot a boot
            // loader device, so that it knows that it should automatically
            // select the next new Pinscape devices that attaches.  We only
            // want to switch windows automatically when the user explicitly
            // triggers the reboot, because we can be sure in this situation
            // that we're not interrupting something else the user is doing.
            PostMessage(GetParent(hwnd), WrapperWin::MSG_DEV_REBOOTED, 
                static_cast<WPARAM>(WrapperWin::LastRebootCommand::Source::BootLoader), 0);

            // reboot the Pico
            if (device.RebootPico())
                gApp.AddTimedStatusMessage("Success - Pico rebooted into normal operating mode",
                    HRGB(0xffffff), HRGB(0x008000), 5000);
            else
                MessageBoxFmt(hwnd, "Reboot failed");
            return true;

        case ID_HELP_HELP:
            // show context-specific help
            ShowHelpFile("BootLoaderMode.htm");
            return true;
        }
        break;
    }

    // use the default handling
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

bool BootLoaderWin::OnLButtonDown(WPARAM keys, int x, int y)
{
    // check for a click in the firmware install button
    POINT pt{ x, y };
    if (PtInRect(&rcFirmwareInstallButton, pt))
    {
        PostMessage(hwnd, WM_COMMAND, ID_DEVICE_INSTALLFIRMWARE, 0);
        return true;
    }

    // inherit the base handling
    return __super::OnLButtonDown(keys, x, y);
}

void BootLoaderWin::InstallFirmwareFile(const TCHAR *filename)
{
    // run the copy operation via the flash progress dialog
    HRESULT hr = E_FAIL;
    FlashProgressDialog dlg(hInstance, [this, &filename, &hr](FlashProgressDialog *dlg) {
        hr = RP2BootDevice::InstallFirmware(filename, device.path.c_str(), dlg);
    });
    dlg.SetCaption(_T("Installing firmware"));
    dlg.SetBannerText(_T("Copying firmware to Pico"));
    dlg.Show(IDD_FLASH_COPY_PROGRESS, GetDialogOwner());

    // Notify the parent that we just rebooted a Boot Loader device into 
    // Pinscape mode by user command.  The user didn't actually issue a
    // reboot command as such, but they did issue a firmware install
    // command, which on success has the side effect of rebooting the
    // Pico.  Do this regardless of the status from the dialog, since
    // (a) the Pico might reboot even if something went wrong on the
    // Window side, and (b) the effect of the notification only lasts
    // a few seconds, so if the device *doesn't* reboot, the notice
    // won't do anything anyway.
    PostMessage(GetParent(hwnd), WrapperWin::MSG_DEV_REBOOTED,
        static_cast<WPARAM>(WrapperWin::LastRebootCommand::Source::BootLoader), 0);

    // announce the result
    if (SUCCEEDED(hr))
        gApp.AddTimedStatusMessage("Success - the new firmware has been installed",
            HRGB(0xffffff), HRGB(0x008000), 5000);
    else
        MessageBoxFmt(hwnd, "Error installing new firmware (error code %08lx)", static_cast<unsigned long>(hr));
}

bool BootLoaderWin::IsFirmwareDropLocation(POINTL ptl)
{
    // adjust to client coordinates
    POINT pt{ ptl.x, ptl.y };
    ScreenToClient(hwnd, &pt);

    // get if it's in the firmware install button
    return PtInRect(&rcFirmwareInstallButton, pt);
}

void BootLoaderWin::ExecFirmwareDrop(POINTL, const TCHAR *filename)
{
    // execute the install via a posted message, so that we can
    // immediately return from the drag/drop nested event loop
    // (without going another level deeper with the nested event
    // loop for the modal progress dialog)
    pendingFirmwareInstall = filename;
    PostMessage(hwnd, MSG_INSTALL_FIRMWARE, 0, 0);
}
