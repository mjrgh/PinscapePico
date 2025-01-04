// Pinscape Pico - Base Device Window class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Common base class for windows with Vendor Interface device handles

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <string>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <Dbt.h>
#include "PinscapePicoAPI.h"
#include "BaseWindow.h"
#include "BaseDeviceWindow.h"

using namespace PinscapePico;

BaseDeviceWindow::BaseDeviceWindow(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device) :
	BaseWindow(hInstance), device(device)
{
}

void BaseDeviceWindow::OnDestroy()
{
	// unregister our device change notification
	UnregisterDeviceNotify();
}

void BaseDeviceWindow::RegisterDeviceNotify()
{
	// register for device change notifications
	DEV_BROADCAST_HANDLE dbh{ sizeof(dbh), DBT_DEVTYP_HANDLE, 0, device->device->GetDeviceHandle() };
	deviceNotifyHandle = RegisterDeviceNotification(hwnd, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void BaseDeviceWindow::UnregisterDeviceNotify()
{
	if (deviceNotifyHandle != NULL)
	{
		UnregisterDeviceNotification(deviceNotifyHandle);
		deviceNotifyHandle = NULL;
	}
}

bool BaseDeviceWindow::OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr)
{
	// if our device was just removed, close its handle
	if (hdr->dbch_devicetype == DBT_DEVTYP_HANDLE
		&& reinterpret_cast<DEV_BROADCAST_HANDLE*>(hdr)->dbch_handle == device->device->GetDeviceHandle())
	{
		// unregister notifications, since the handle will no longer be valid after this
		UnregisterDeviceNotify();

		// close the device handle
		if (VendorInterface::Shared::Locker l(device); l.locked)
			device->device->CloseDeviceHandle();
	}

	// proceed with default system handling
	return false;
}

