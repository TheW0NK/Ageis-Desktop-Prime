// BCD reader and parser. See bcd.h for the file format.

#include <efi.h>
#include <efilib.h>

#include "bcd.h"

static BOOLEAN is_space(CHAR8 c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Trims [*start, *end) in place.
static void trim(const CHAR8 **start, const CHAR8 **end)
{
    while (*start < *end && is_space(**start))
        (*start)++;
    while (*end > *start && is_space((*end)[-1]))
        (*end)--;
}

static BOOLEAN key_is(const CHAR8 *s, const CHAR8 *e, const char *key)
{
    while (s < e && *key && *s == (CHAR8)*key) {
        s++;
        key++;
    }
    return s == e && *key == '\0';
}

// Copies [s, e) into a CHAR16 buffer. Fails if it doesn't fit.
static BOOLEAN copy16(CHAR16 *dst, UINTN max, const CHAR8 *s, const CHAR8 *e)
{
    if ((UINTN)(e - s) >= max)
        return FALSE;
    while (s < e)
        *dst++ = (CHAR16)*s++;
    *dst = 0;
    return TRUE;
}

static BOOLEAN copy8(CHAR8 *dst, UINTN max, const CHAR8 *s, const CHAR8 *e)
{
    if ((UINTN)(e - s) >= max)
        return FALSE;
    while (s < e)
        *dst++ = *s++;
    *dst = 0;
    return TRUE;
}

static BOOLEAN parse_uint(const CHAR8 *s, const CHAR8 *e, UINTN *out)
{
    UINTN v = 0;

    if (s == e)
        return FALSE;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9' || v > 100000)
            return FALSE;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return TRUE;
}

static BOOLEAN parse_bool(const CHAR8 *s, const CHAR8 *e, BOOLEAN *out)
{
    if (key_is(s, e, "true") || key_is(s, e, "yes") || key_is(s, e, "1"))
        *out = TRUE;
    else if (key_is(s, e, "false") || key_is(s, e, "no") || key_is(s, e, "0"))
        *out = FALSE;
    else
        return FALSE;
    return TRUE;
}

static BOOLEAN parse_resolution(struct bcd *bcd, const CHAR8 *s, const CHAR8 *e)
{
    const CHAR8 *x = s;
    UINTN w, h;

    bcd->resolution_max = FALSE;
    bcd->resolution_width = bcd->resolution_height = 0;
    if (key_is(s, e, "auto"))
        return TRUE;
    if (key_is(s, e, "max")) {
        bcd->resolution_max = TRUE;
        return TRUE;
    }

    while (x < e && *x != 'x')
        x++;
    if (x == e || !parse_uint(s, x, &w) || !parse_uint(x + 1, e, &h) || !w || !h)
        return FALSE;
    bcd->resolution_width = w;
    bcd->resolution_height = h;
    return TRUE;
}

static BOOLEAN set_global(struct bcd *bcd, CHAR16 *default_id,
                          const CHAR8 *k, const CHAR8 *ke,
                          const CHAR8 *v, const CHAR8 *ve)
{
    if (key_is(k, ke, "timeout"))
        return parse_uint(v, ve, &bcd->timeout);
    if (key_is(k, ke, "default"))
        return copy16(default_id, BCD_ID_MAX, v, ve);
    if (key_is(k, ke, "loader"))
        return copy16(bcd->loader, BCD_PATH_MAX, v, ve);
    if (key_is(k, ke, "diagnostics"))
        return parse_bool(v, ve, &bcd->diagnostics);
    if (key_is(k, ke, "resolution"))
        return parse_resolution(bcd, v, ve);
    return FALSE;
}

static BOOLEAN set_entry(struct bcd_entry *entry,
                         const CHAR8 *k, const CHAR8 *ke,
                         const CHAR8 *v, const CHAR8 *ve)
{
    if (key_is(k, ke, "title"))
        return copy16(entry->title, BCD_TITLE_MAX, v, ve);
    if (key_is(k, ke, "path"))
        return copy16(entry->path, BCD_PATH_MAX, v, ve);
    if (key_is(k, ke, "cmdline"))
        return copy8(entry->cmdline, BCD_CMDLINE_MAX, v, ve);
    if (key_is(k, ke, "type")) {
        if (key_is(v, ve, "kernel"))
            entry->type = BCD_TYPE_KERNEL;
        else if (key_is(v, ve, "efi"))
            entry->type = BCD_TYPE_EFI;
        else
            return FALSE;
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN entry_complete(struct bcd_entry *entry)
{
    if (entry->type == BCD_TYPE_NONE || entry->path[0] == 0)
        return FALSE;
    if (entry->title[0] == 0)
        StrCpy(entry->title, entry->id);
    return TRUE;
}

EFI_STATUS bcd_parse(const CHAR8 *text, UINTN size, struct bcd *bcd, UINTN *error_line)
{
    const CHAR8 *p = text, *end = text + size;
    struct bcd_entry *entry = NULL;
    UINTN entry_line = 0, default_line = 0, line = 0;
    CHAR16 default_id[BCD_ID_MAX] = { 0 };

    ZeroMem(bcd, sizeof(*bcd));
    StrCpy(bcd->loader, BCD_DEFAULT_LOADER);
    *error_line = 0;

    while (p < end) {
        const CHAR8 *s = p, *e;

        while (p < end && *p != '\n')
            p++;
        e = p;
        if (p < end)
            p++;
        line++;

        trim(&s, &e);
        if (s == e || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            if (e[-1] != ']' || bcd->count == BCD_MAX_ENTRIES)
                goto syntax_error;
            if (entry && !entry_complete(entry)) {
                line = entry_line;
                goto syntax_error;
            }

            s++;
            e--;
            trim(&s, &e);
            entry = &bcd->entries[bcd->count++];
            entry_line = line;
            if (s == e || !copy16(entry->id, BCD_ID_MAX, s, e))
                goto syntax_error;
            continue;
        }

        const CHAR8 *k = s, *ke = s, *v, *ve = e;

        while (ke < e && *ke != '=')
            ke++;
        if (ke == e)
            goto syntax_error;
        v = ke + 1;
        trim(&k, &ke);
        trim(&v, &ve);

        if (entry ? !set_entry(entry, k, ke, v, ve)
                  : !set_global(bcd, default_id, k, ke, v, ve))
            goto syntax_error;
        if (!entry && key_is(k, ke, "default"))
            default_line = line;
    }

    if (entry && !entry_complete(entry)) {
        line = entry_line;
        goto syntax_error;
    }
    if (bcd->count == 0)
        return EFI_NOT_FOUND;

    if (default_id[0]) {
        entry = bcd_find(bcd, default_id);
        if (!entry) {
            line = default_line;
            goto syntax_error;
        }
        bcd->default_index = entry - bcd->entries;
    }
    return EFI_SUCCESS;

syntax_error:
    *error_line = line;
    return EFI_INVALID_PARAMETER;
}

static EFI_STATUS read_file(EFI_HANDLE device, CHAR16 *path, CHAR8 **data, UINTN *size)
{
    EFI_FILE_HANDLE root, file;
    EFI_FILE_INFO *info;
    EFI_STATUS status;

    root = LibOpenRoot(device);
    if (!root)
        return EFI_NOT_FOUND;

    status = uefi_call_wrapper(root->Open, 5, root, &file, path, EFI_FILE_MODE_READ, 0);
    uefi_call_wrapper(root->Close, 1, root);
    if (EFI_ERROR(status))
        return status;

    info = LibFileInfo(file);
    if (!info) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_DEVICE_ERROR;
    }
    *size = info->FileSize;
    FreePool(info);

    *data = AllocatePool(*size ? *size : 1);
    if (!*data) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_OUT_OF_RESOURCES;
    }

    status = uefi_call_wrapper(file->Read, 3, file, size, *data);
    uefi_call_wrapper(file->Close, 1, file);
    if (EFI_ERROR(status))
        FreePool(*data);
    return status;
}

EFI_STATUS bcd_load(EFI_HANDLE device, struct bcd *bcd, UINTN *error_line)
{
    CHAR8 *text;
    UINTN size;
    EFI_STATUS status;

    *error_line = 0;
    status = read_file(device, BCD_PATH, &text, &size);
    if (EFI_ERROR(status))
        return status;

    status = bcd_parse(text, size, bcd, error_line);
    FreePool(text);
    return status;
}

struct bcd_entry *bcd_find(struct bcd *bcd, const CHAR16 *id)
{
    for (UINTN i = 0; i < bcd->count; i++) {
        if (StrCmp(bcd->entries[i].id, id) == 0)
            return &bcd->entries[i];
    }
    return NULL;
}
