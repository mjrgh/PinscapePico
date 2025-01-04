// Pinscape Pico - Tab-Embeddable Window interface
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This is an abstract interface that can optionally be implemented
// by our windows to provide extra information to the tab container.
// We use dynamic casting to sense when this interface is provided.

#pragma once

class TabEmbeddableWindow
{
public:
	// Has the "document" represented by this window been modified
	// relative to the copy of the data stored on the device?
	virtual bool IsDocumentModified() const { return false; }

	// Check before erasing the device's configuration data, and
	// possibly other settings data (if factoryReset is true), to
	// see if this window contains any data modified locally that
	// will be affected by the reset.  
	// 
	// Returns true if any data changed locally might be lost, 
	// false if not.  If a window returns false, the command will
	// prompt the user for confirmation first.
	// 
	// This is similar to IsDocumentModified(), with the difference
	// that it asks if the specific kind of changes we're making, as
	// indicated by factoryReset, will affect this window's
	// changes.
	virtual bool PreEraseDeviceConfig(bool factoryReset) const { return false; }

	// Receive notification that the configuration has been
	// erased.  If 'factoryReset' is true, it means that all
	// settings have been erased, including internal settings
	// like plunger configuration data.  In all cases, the
	// main and safe-mode configuration files have been erased.
	virtual void OnEraseDeviceConfig(bool factoryReset) { }

	// Keep this window active when the device is disconnected?
	// If true, the window will continue to be displayed regardless
	// of device connection status.  If false, the placeholder
	// "Device Offline" window will be displayed instead when the
	// device is disconnected.
	virtual bool IsVisibleOffline() const { return false; }

	// Device reconnect notification.  The host calls this when
	// the window is brought to the foreground, and the host has
	// detected that the device was disconnected since the last
	// time the window was shown.  The device-side configuration
	// might have changed across the disconnect/reset, so the
	// window must reload any cached configuration data it
	// previously queried from the device.
	//
	// Returns true on a successful refresh, false on failure.  A
	// false return tells the caller to destroy the existing window
	// and create a new one, so the window can simply return false
	// in lieu of reconstructing the cached config data.  That's
	// fine as long as creating a whole new window is fast enough
	// not to be noticeable in the UI.  Windows that take more work
	// to set up should implement the update inline.  The default
	// implementation returns false to force the close/re-open.
	virtual bool OnDeviceReconnect() { return false; }
};

