// USB device, configuration and string descriptors.

#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_descriptors.h"

// TinyUSB's test VID. Replace with your own VID/PID before shipping hardware.
#define USB_VID     0xCafe
#define USB_PID     0x4A10

#define EPNUM_AUDIO_IN    0x01
#define EPNUM_AUDIO_OUT   0x01

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_SPEAKER,
    STRID_MIC,
};

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // IAD is required for audio class 2
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_ROBOT_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_AUDIO_ROBOT_DESCRIPTOR(STRID_PRODUCT, EPNUM_AUDIO_OUT, 0x80 | EPNUM_AUDIO_IN),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static const char *const string_desc[] = {
    [STRID_MANUFACTURER] = "Robot Audio",
    [STRID_PRODUCT]      = "Robot Audio Subsystem",
    [STRID_SPEAKER]      = "Robot Speaker",
    [STRID_MIC]          = "Robot Mic Array",
};

static uint16_t desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t len;

    if (index == STRID_LANGID) {
        desc_str[1] = 0x0409;
        len = 1;
    } else if (index == STRID_SERIAL) {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        len = strlen(serial);
        for (size_t i = 0; i < len; i++) desc_str[1 + i] = (uint8_t)serial[i];
    } else {
        if (index >= count_of(string_desc) || !string_desc[index]) return NULL;
        const char *s = string_desc[index];
        len = strlen(s);
        if (len > 32) len = 32;
        for (size_t i = 0; i < len; i++) desc_str[1 + i] = (uint8_t)s[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
