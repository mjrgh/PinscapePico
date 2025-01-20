// Pinscape Pico Button Latency Tester II - Main Program
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

// Root hub port - Pico built-in USB port = 0
#define TUD_OPT_RHPORT          0

// The Pico is a Full Speed (FS) USB device
#define BOARD_DEVICE_RHPORT_SPEED OPT_MODE_FULL_SPEED

// Enable Tinyusb
#define CFG_TUD_ENABLED         1
#define CFG_TUD_DEBUG           0

// Maximum number of HID interfaces
#define CFG_TUD_HID             0

// Maximum number of vendor interfaces
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
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUD_VENDOR_EP_BUFSIZE 64

// Tinyusb internal FIFO buffer sizes per interface
#define CFG_TUD_CDC_TX_BUFSIZE  1024
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_VENDOR_TX_BUFSIZE (256+4096)
#define CFG_TUD_VENDOR_RX_BUFSIZE (256+4096)

// Enable debugging if desired
//#define CFG_TUSB_DEBUG 2
//#define CFG_TUSB_DEBUG_PRINTF tusbLogPrintf

// Custom instrumentation entrypoint for additional debugging code
// we add to tinyusb
#ifdef __cplusplus
extern "C"
#endif
   void logToPinscape(const char *f, ...);
