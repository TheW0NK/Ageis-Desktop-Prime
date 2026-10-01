#ifndef AEGIS_HID_H
#define AEGIS_HID_H

#include "kernel.h"

#define HID_MAX_FIELDS  48
#define HID_MAX_USAGES  16

struct hid_field {
    uint8_t report_id;
    bool variable, relative;
    uint16_t page;
    uint32_t bit_offset;
    uint8_t size;
    uint16_t count;
    int32_t lmin, lmax;
    uint16_t usages[HID_MAX_USAGES];
    uint8_t nusages;
    uint16_t usage_min, usage_max;
    uint16_t *prev;                 // key codes reported last time, one per slot
};

struct hid_device {
    struct hid_field fields[HID_MAX_FIELDS];
    int nfields;
    bool has_ids;
    uint32_t kind;                  // INPUT_KIND_*
    int input;                      // input device index
    // Output report carrying the keyboard LEDs, if any.
    uint8_t led_report_id;
    uint16_t led_report_bytes;
    int16_t led_bit[3];             // num, caps, scroll; -1 if absent
};

// Parses a report descriptor. Returns false if it describes nothing we use.
bool hid_parse(struct hid_device *h, const uint8_t *desc, size_t len);
// Handles one input report (including the report ID byte, if any).
void hid_report(struct hid_device *h, const uint8_t *data, size_t len);
// Fills an LED output report; returns its length, or 0 if there is none.
size_t hid_led_report(const struct hid_device *h, uint32_t mods, uint8_t *out, size_t max);
void hid_free(struct hid_device *h);

extern const uint8_t hid_boot_keyboard_desc[];
extern const size_t hid_boot_keyboard_desc_len;
extern const uint8_t hid_boot_mouse_desc[];
extern const size_t hid_boot_mouse_desc_len;

#endif
