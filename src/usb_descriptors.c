// USB Audio 2.0 speaker + CDC diagnostics. Uses Espressif's UAC descriptors.
#include "tusb.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    // Espressif example VID/PID, for this local prototype only.
    .idVendor = 0x303A, .idProduct = 0x8000, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1,
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device_desc; }

enum { CDC_INTERFACE = 2, INTERFACE_COUNT = 4 };
#define TOTAL_LENGTH (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, INTERFACE_COUNT, 0, TOTAL_LENGTH, 0, 500),
    TUD_AUDIO_DESCRIPTOR(0, 4, 0x01, 0x82, 0x81),
    TUD_CDC_DESCRIPTOR(CDC_INTERFACE, 6, 0x83, 8, 0x02, 0x82, 64),
};
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return config_desc; }

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t out[64];
    static char serial[20];
    if (!serial[0]) {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    const char *strings[] = { "", "M5 Spectrum", "CoreS3 USB Spectrum", serial,
        "CoreS3 USB Spectrum", "Spectrum Speaker", "Spectrum Diagnostics" };
    unsigned n = 0;
    if (!index) { out[1] = 0x0409; n = 1; }
    else {
        if (index >= sizeof(strings) / sizeof(strings[0])) return NULL;
        n = strlen(strings[index]); if (n > 63) n = 63;
        for (unsigned i = 0; i < n; ++i) out[i + 1] = (uint8_t)strings[index][i];
    }
    out[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return out;
}
