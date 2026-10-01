#include "hid.h"
#include "input.h"
#include "mem.h"
#include "string.h"

#define PAGE_DESKTOP    0x01
#define PAGE_KEYBOARD   0x07
#define PAGE_LED        0x08
#define PAGE_BUTTON     0x09
#define PAGE_CONSUMER   0x0C

#define MAX_REPORT_IDS  16

// The boot protocol layouts, for devices whose own descriptor cannot be used.
const uint8_t hid_boot_keyboard_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01,
    0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};
const size_t hid_boot_keyboard_desc_len = sizeof(hid_boot_keyboard_desc);

const uint8_t hid_boot_mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};
const size_t hid_boot_mouse_desc_len = sizeof(hid_boot_mouse_desc);

struct globals {
    uint16_t page;
    int32_t lmin, lmax;
    uint32_t size, count;
    uint8_t report_id;
};

struct offsets {
    uint8_t id;
    uint32_t in, out;
};

static int32_t item_value(const uint8_t *p, int size, bool sign)
{
    switch (size) {
    case 1: return sign ? (int8_t)p[0] : p[0];
    case 2: return sign ? (int16_t)(p[0] | p[1] << 8) : (p[0] | p[1] << 8);
    case 4: return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    }
    return 0;
}

static struct offsets *offsets_for(struct offsets *o, int *n, uint8_t id)
{
    for (int i = 0; i < *n; i++) {
        if (o[i].id == id)
            return &o[i];
    }
    if (*n == MAX_REPORT_IDS)
        return NULL;
    o[*n] = (struct offsets){ id, id ? 8 : 0, id ? 8 : 0 };
    return &o[(*n)++];
}

static bool interesting(uint16_t page, uint16_t usage)
{
    switch (page) {
    case PAGE_KEYBOARD:
    case PAGE_BUTTON:
    case PAGE_CONSUMER:
        return true;
    case PAGE_DESKTOP:
        return usage == 0x30 || usage == 0x31 || usage == 0x38 || (usage >= 0x81 && usage <= 0x83);
    }
    return false;
}

bool hid_parse(struct hid_device *h, const uint8_t *desc, size_t len)
{
    struct globals g = { 0 }, stack[4];
    struct offsets offs[MAX_REPORT_IDS];
    int nstack = 0, noffs = 0;
    uint16_t usages[HID_MAX_USAGES];
    uint32_t usage_min = 0, usage_max = 0;
    int nusages = 0;
    bool have_range = false;

    memset(h->fields, 0, sizeof(h->fields));
    h->nfields = 0;
    h->has_ids = false;
    h->kind = 0;
    h->led_report_bytes = 0;
    h->led_bit[0] = h->led_bit[1] = h->led_bit[2] = -1;

    for (size_t i = 0; i < len;) {
        uint8_t prefix = desc[i];
        int size = prefix & 3, type = (prefix >> 2) & 3, tag = prefix >> 4;
        const uint8_t *data = desc + i + 1;
        uint32_t uval;
        int32_t sval;

        if (prefix == 0xFE) {           // long item
            if (i + 2 >= len)
                break;
            i += 3 + desc[i + 1];
            continue;
        }
        if (size == 3)
            size = 4;
        if (i + 1 + size > len)
            break;
        i += 1 + size;
        uval = (uint32_t)item_value(data, size, false);
        sval = item_value(data, size, true);

        if (type == 1) {                // global
            switch (tag) {
            case 0x0: g.page = uval; break;
            case 0x1: g.lmin = sval; break;
            case 0x2: g.lmax = (g.lmin >= 0 && size < 4) ? (int32_t)uval : sval; break;
            case 0x7: g.size = uval; break;
            case 0x8: g.report_id = uval; h->has_ids = true; break;
            case 0x9: g.count = uval; break;
            case 0xA: if (nstack < 4) stack[nstack++] = g; break;
            case 0xB: if (nstack) g = stack[--nstack]; break;
            }
            continue;
        }
        if (type == 2) {                // local
            uint16_t u = uval & 0xFFFF;

            if (size == 4 && (uval >> 16))
                g.page = uval >> 16;    // extended usage carries its page
            switch (tag) {
            case 0x0: if (nusages < HID_MAX_USAGES) usages[nusages++] = u; break;
            case 0x1: usage_min = u; have_range = true; break;
            case 0x2: usage_max = u; have_range = true; break;
            }
            continue;
        }
        if (type != 0)
            continue;

        if (tag == 0x8 || tag == 0x9) { // Input or Output
            struct offsets *o = offsets_for(offs, &noffs, g.report_id);
            uint32_t bits = g.size * g.count;

            if (!o)
                return false;
            if (tag == 0x9) {
                if (g.page == PAGE_LED && (uval & 2)) {
                    for (uint32_t k = 0; k < g.count && g.size == 1; k++) {
                        uint32_t usage = (k < (uint32_t)nusages) ? usages[k] : usage_min + k;
                        if (usage >= 1 && usage <= 3) {
                            h->led_bit[usage - 1] = o->out + k;
                            h->led_report_id = g.report_id;
                        }
                    }
                }
                o->out += bits;
                if (h->led_bit[0] >= 0 || h->led_bit[1] >= 0 || h->led_bit[2] >= 0)
                    h->led_report_bytes = MAX(h->led_report_bytes, (o->out + 7) / 8);
            } else {
                bool constant = uval & 1;
                uint16_t first = nusages ? usages[0] : usage_min;

                if (!constant && g.size && g.size <= 32 && g.count && h->nfields < HID_MAX_FIELDS
                    && interesting(g.page, first)) {
                    struct hid_field *f = &h->fields[h->nfields++];

                    f->report_id = g.report_id;
                    f->variable = uval & 2;
                    f->relative = uval & 4;
                    f->page = g.page;
                    f->bit_offset = o->in;
                    f->size = g.size;
                    f->count = MIN(g.count, 256U);
                    f->lmin = g.lmin;
                    f->lmax = g.lmax;
                    memcpy(f->usages, usages, nusages * sizeof(uint16_t));
                    f->nusages = nusages;
                    f->usage_min = have_range ? usage_min : first;
                    f->usage_max = have_range ? usage_max : first;
                    if (g.page == PAGE_KEYBOARD)
                        h->kind |= INPUT_KIND_KEYBOARD;
                    if (g.page == PAGE_BUTTON)
                        h->kind |= INPUT_KIND_POINTER;
                    if (g.page == PAGE_DESKTOP && (first == 0x30 || first == 0x31))
                        h->kind |= f->relative ? INPUT_KIND_POINTER : INPUT_KIND_TABLET;
                }
                o->in += bits;
            }
        }
        if (tag == 0x8 || tag == 0x9 || tag == 0xB || tag == 0xA) {
            // Local items apply to one main item only.
            nusages = 0;
            usage_min = usage_max = 0;
            have_range = false;
        }
    }
    if (h->kind & INPUT_KIND_TABLET)
        h->kind &= ~INPUT_KIND_POINTER;

    for (int i = 0; i < h->nfields; i++) {
        struct hid_field *f = &h->fields[i];

        if (!(f->prev = kzalloc(f->count * sizeof(uint16_t))))
            return false;
    }
    return h->nfields > 0;
}

void hid_free(struct hid_device *h)
{
    for (int i = 0; i < h->nfields; i++) {
        kfree(h->fields[i].prev);
        h->fields[i].prev = NULL;
    }
    h->nfields = 0;
}

static uint32_t extract(const uint8_t *data, size_t len, uint32_t bit, uint32_t size)
{
    uint32_t v = 0;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t b = bit + i;

        if (b / 8 >= len)
            break;
        if (data[b / 8] & (1 << (b % 8)))
            v |= 1U << i;
    }
    return v;
}

static uint16_t consumer_key(uint16_t usage)
{
    switch (usage) {
    case 0x6F:  return KEY_BRIGHTNESSUP;
    case 0x70:  return KEY_BRIGHTNESSDOWN;
    case 0xB5:  return KEY_NEXTSONG;
    case 0xB6:  return KEY_PREVIOUSSONG;
    case 0xB7:  return KEY_STOPCD;
    case 0xB8:  return KEY_EJECTCD;
    case 0xCD:  return KEY_PLAYPAUSE;
    case 0xE2:  return KEY_MUTE;
    case 0xE9:  return KEY_VOLUMEUP;
    case 0xEA:  return KEY_VOLUMEDOWN;
    case 0x183: return KEY_MEDIA;
    case 0x18A: return KEY_MAIL;
    case 0x192: return KEY_CALC;
    case 0x194: return KEY_COMPUTER;
    case 0x221: return KEY_SEARCH;
    case 0x223: return KEY_WWW;
    case 0x224: return KEY_BACK;
    case 0x225: return KEY_FORWARD;
    case 0x227: return KEY_REFRESH;
    case 0x22A: return KEY_BOOKMARKS;
    }
    return 0;
}

static uint16_t usage_key(uint16_t page, uint16_t usage)
{
    switch (page) {
    case PAGE_KEYBOARD:
        return (usage >= 4 && usage <= 0xE7) ? usage : 0;
    case PAGE_BUTTON:
        return (usage >= 1 && usage <= 5) ? BTN_LEFT + usage - 1 : 0;
    case PAGE_CONSUMER:
        return consumer_key(usage);
    case PAGE_DESKTOP:
        return usage == 0x81 ? KEY_POWER : usage == 0x82 ? KEY_SLEEP : usage == 0x83 ? KEY_WAKEUP : 0;
    }
    return 0;
}

static uint16_t field_usage(const struct hid_field *f, uint32_t slot)
{
    if (slot < f->nusages)
        return f->usages[slot];
    if (f->nusages && f->usage_min == f->usage_max)
        return f->usages[f->nusages - 1];
    return f->usage_min + slot;
}

static void axis(struct hid_device *h, const struct hid_field *f, uint16_t usage, int32_t v)
{
    if (f->relative) {
        if (f->page == PAGE_DESKTOP)
            input_rel(h->input, usage == 0x30 ? REL_X : usage == 0x31 ? REL_Y : REL_WHEEL, v);
        else
            input_rel(h->input, REL_HWHEEL, v);
    } else if (f->page == PAGE_DESKTOP && (usage == 0x30 || usage == 0x31) && f->lmax > f->lmin) {
        int64_t scaled = (int64_t)(v - f->lmin) * INPUT_ABS_MAX / (f->lmax - f->lmin);
        input_abs(h->input, usage == 0x30 ? ABS_X : ABS_Y, (int32_t)scaled);
    }
}

static bool is_axis(const struct hid_field *f, uint16_t usage)
{
    return (f->page == PAGE_DESKTOP && (usage == 0x30 || usage == 0x31 || usage == 0x38))
        || (f->page == PAGE_CONSUMER && usage == 0x238);
}

void hid_report(struct hid_device *h, const uint8_t *data, size_t len)
{
    uint8_t id = 0;
    uint16_t now[256];

    if (h->has_ids) {
        if (!len)
            return;
        id = data[0];
    }
    for (int i = 0; i < h->nfields; i++) {
        struct hid_field *f = &h->fields[i];
        bool sign = f->lmin < 0;

        if (f->report_id != id)
            continue;
        for (uint32_t s = 0; s < f->count; s++) {
            uint32_t raw = extract(data, len, f->bit_offset + s * f->size, f->size);
            int32_t v = raw;

            if (sign && f->size < 32 && (raw & (1U << (f->size - 1))))
                v = (int32_t)(raw | ~((1U << f->size) - 1));

            now[s] = 0;
            if (f->variable) {
                uint16_t usage = field_usage(f, s);

                if (is_axis(f, usage)) {
                    axis(h, f, usage, v);
                    continue;
                }
                if (v)
                    now[s] = usage_key(f->page, usage);
            } else if (v >= f->lmin && v <= f->lmax) {
                uint16_t usage = f->usage_min + (v - f->lmin);

                if (usage)
                    now[s] = usage_key(f->page, usage);
            }
        }
        // Release what is no longer down, then press what is new.
        for (uint32_t s = 0; s < f->count; s++) {
            bool still = false;

            if (!f->prev[s])
                continue;
            for (uint32_t t = 0; t < f->count && !still; t++)
                still = now[t] == f->prev[s];
            if (!still)
                input_key(h->input, f->prev[s], false);
        }
        for (uint32_t s = 0; s < f->count; s++) {
            bool was = false;

            if (!now[s])
                continue;
            for (uint32_t t = 0; t < f->count && !was; t++)
                was = f->prev[t] == now[s];
            if (!was)
                input_key(h->input, now[s], true);
        }
        memcpy(f->prev, now, f->count * sizeof(uint16_t));
    }
    input_sync(h->input);
}

size_t hid_led_report(const struct hid_device *h, uint32_t mods, uint8_t *out, size_t max)
{
    static const uint32_t bits[3] = { MOD_NUMLOCK, MOD_CAPSLOCK, MOD_SCROLLLOCK };
    size_t n = h->led_report_bytes;

    if (!n || n > max)
        return 0;
    memset(out, 0, n);
    if (h->led_report_id)
        out[0] = h->led_report_id;
    for (int i = 0; i < 3; i++) {
        if (h->led_bit[i] >= 0 && (mods & bits[i]))
            out[h->led_bit[i] / 8] |= 1 << (h->led_bit[i] % 8);
    }
    return n;
}
