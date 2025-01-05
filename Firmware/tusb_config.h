// Pinscape Pico - Tinyusb configuration
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// Root hub port.  (This selects the physical USB port on MCUs that
// have multiple ports.  The Pico only has one port built-in, so this
// is just a formality on a stock Pico.  It is possible to add more
// ports by programming the PIOs to speak the USB wire protocol and
// wiring an outboard USB connector to GPIOs, but I'm not contemplating
// that for this project.)
#define TUD_OPT_RHPORT          0

// The Pico is a Full Speed (FS) USB device
#define BOARD_DEVICE_RHPORT_SPEED OPT_MODE_FULL_SPEED

// Enable Tinyusb
#define CFG_TUD_ENABLED         1
#define CFG_TUD_DEBUG           0

// Maximum number of HID interfaces.  This allocates space for HID
// structures internally in Tinyusb, so this is the *maximum* number
// of interfaces we can create.  The number we actually create depends
// on which devices are configured, so it can't be determined at compile
// time - we determine it at program startup after reading the config
// data.  Tinyusb requires a compile-time setting, though, so we have
// to set this to an upper bound for how many we might actually need
// to create.  Allocating more than we end up requiring at run-time
// is harmless other than tying up memory in the unused array slots.
//
// Note that we don't need a whole interface for each enabled HID
// type, because one interface can host multiple HID types.  In fact,
// we could put all of the HIDs on a single interface.  The reason we
// don't is that the host polls at the *interface* level rather than
// the virtual device level, so devices that share a single interface
// are only polled once *as a group* per host USB polling cycle.
// Devices that require low latency therefore shouldn't share an
// interface with devices that either also require low latency or
// that generate frequent inputs to the host.  For our purposes, the
// devices that require low latency are the ones with button inputs,
// in particular, the keyboard and gamepad emulations.  Button
// presses must be passed to the host as quickly as possible for
// responsiveness in gaming applications.  So the keyboard emulator
// and gamepad emulator should be on separate interfaces, since both
// require low latency and the gamepad generates frequent (essentially
// continuous) input when it's used for an accelerometer.  The gamepad
// should usually be on its own interface with no other devices apart
// from host-to-device-only (output-only) devices.  The keyboard can
// share an interface with low-input-traffic devices such as Media
// Control and Feedback Controller.
#define CFG_TUD_HID             5

// Maximum number of vendor interfaces.  We use one vendor interface
// for our custom Pinscape Configuration & Control interface.
//
// Note that Tinyusb's notion of a vendor interface is narrower than the
// USB specification's.  The USB spec defines a vendor interface as any
// interface as any interface that doesn't conform to one of the
// standard USB classes (HID, CDC, mass storage, etc).  But Tinyusb's
// vendor interface class driver can only handle a subset of those, with
// the big limitation that it only works with bulk-transfer endpoints.
// It would be more accurate if Tinyusb called this a "libusb"
// interface, because that's what it's really intended to work with,
// although it works equally well with WinUsb.  (libusb and WinUsb are
// generic vendor-interface drivers on the host side that pass access to
// the USB endpoints through to application programs, allowing the
// custom device-specific protocol code that would normally be
// implemented in a kernel device driver to be implemented instead as
// part of a user-mode application.  This saves the vendor the work of
// creating a custom kernel driver for their device, and saves users the
// hassle of installing and managing more device drivers.)
#define CFG_TUD_VENDOR          1

// One CDC interface, for our debug console port
#define CFG_TUD_CDC             1

// Unused class drivers
#define CFG_TUD_MSC             0  // no mass storage controller interfaces
#define CFG_TUD_MIDI            0  // no MIDI interfaces
   
// Endpoint sizes.  Pico is a Full-Speed USB device.  An FS device by
// specification has a maximum transfer size on all endpoints of
// 64 bytes.
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_HID_EP_BUFSIZE  64
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUD_VENDOR_EP_BUFSIZE 64

// FIFO buffer sizes per interface.
// These are the Tinyusb internal buffer sizes for the interfaces, NOT
// the packet sizes.  Tinyusb uses these to create internal buffers for
// sending and receiving data, allowing the application to send and
// receive in increments larger than the USB packet size without having
// to do its own buffering on large transfers.  Note that Tinuysb pre-
// allocates the FIFO buffers for each interface during intialization,
// so declaring large buffers here will tie up the memory for the whole
// session.  (The Pico has a fairly generous complement of RAM at 256KB,
// so buffers on the order of a a few KB are fine.)
#define CFG_TUD_CDC_TX_BUFSIZE  1024
#define CFG_TUD_CDC_RX_BUFSIZE  64

// For the vendor interface, make room in the FIFO to buffer a
// reply/request header plus the maximum "extra data" transfer size,
// currently 4K.  This allows the vendor interface handler to buffer an
// entire reply without blocking, and allows buffering one entire
// request on receive.  One receive request is adequate because the
// protocol only supports one client connection at a time (so we don't
// have to worry about multiple clients throwing requests at us
// concurrently), and it's synchronous, in that the client has to wait
// for a reply before sending the next request.
//
// Since this is included from tinyusb, we'd rather not complicate build
// dependencies by dragging in the USB protocol headers, so we'll just
// hard-code a number for the header size portion of the buffer size,
// leaving some extra room for future expansion.  The request/reply
// headers are currently 28 bytes; bump up to 256 for headroom.
#define CFG_TUD_VENDOR_TX_BUFSIZE (256+4096)
#define CFG_TUD_VENDOR_RX_BUFSIZE (256+4096)

// Report ID prefixes
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_GAMEPAD  2

// Enable debugging if desired
//#define CFG_TUSB_DEBUG 2
//#define CFG_TUSB_DEBUG_PRINTF tusbLogPrintf

// Custom instrumentation entrypoint for additional debugging code
// we add to tinyusb
#ifdef __cplusplus
extern "C"
#endif
   void logToPinscape(const char *f, ...);
