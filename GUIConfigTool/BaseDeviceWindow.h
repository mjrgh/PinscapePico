// Pinscape Pico - Base Device Window class
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Common base class for windows with Vendor Interface device handles


#pragma once
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
#include "BaseWindow.h"

namespace PinscapePico
{
	class BaseDeviceWindow : public BaseWindow
	{
	public:
		BaseDeviceWindow(HINSTANCE hInstance, std::shared_ptr<VendorInterface::Shared> &device);

		// get a shared pointer the window's device
		std::shared_ptr<VendorInterface::Shared> GetDevice() const { return device; }

	protected:
		// Register this window for device notifications.  This can be called
		// during window creation, or at any other time when the device is
		// created or changed.  This registers for notifications specifically
		// for the device handle in the shared device object, so it must be
		// called again 
		void RegisterDeviceNotify();

		// remove our device notification registration
		void UnregisterDeviceNotify();

		// window messages
		virtual void OnDestroy() override;

		// device removal notification
		virtual bool OnDeviceRemoveComplete(DEV_BROADCAST_HDR *hdr) override;

		// shared device pointer
		std::shared_ptr<VendorInterface::Shared> device;

		// RegisterDeviceNotification() handle
		HANDLE deviceNotifyHandle = NULL;
	};
}
