#pragma once
#include <cstdint>

#define OPT_OS_PICO 1
#define OPT_MODE_DEVICE 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUSB_DESC_DEVICE 1
#define TUSB_DESC_STRING 3
#define TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP 0x20
#define TUD_CONFIG_DESC_LEN 9
#define TUD_MSC_DESC_LEN 23
#define TUD_CONFIG_DESCRIPTOR(...) 9,2,32,0,1,1,0,0xA0,100
#define TUD_MSC_DESCRIPTOR(...) 8,11,0,1,8,6,80,4,9,4,0,0,2,8,6,80,7,5,1,2,64,0,0

#define SCSI_SENSE_NOT_READY 2
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E

struct tusb_desc_device_t {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

inline bool tusb_init() { return true; }
inline void tud_task() {}
inline void tud_connect() {}
inline void tud_disconnect() {}
inline void tud_msc_set_sense(uint8_t, uint8_t, uint8_t, uint8_t) {}
