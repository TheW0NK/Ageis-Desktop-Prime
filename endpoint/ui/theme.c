#include "ui_internal.h"

// Themes and the drawing helpers shared by the widgets.

static const struct ui_theme light = {
    .window = RGB(0xF3F5F8), .surface = RGB(0xFFFFFF), .surface_alt = RGB(0xE9EDF2),
    .border = RGB(0xC5CDD8), .text = RGB(0x1D2633), .text_dim = RGB(0x5E6A7A),
    .accent = RGB(0x2F6FE4), .accent_text = RGB(0xFFFFFF), .selection = RGB(0xCFE0FF),
    .selection_text = RGB(0x10203A), .hover = RGB(0xE6ECF4), .pressed = RGB(0xD3DCE8),
    .danger = RGB(0xD23C3C), .focus_ring = RGB(0x3D8BFF), .input = RGB(0xFFFFFF),
    .shadow = ARGB(0x40, 0, 0, 0),
    .font_size = 14, .radius = 6, .padding = 8, .spacing = 6, .row_height = 30,
    .font = FONT_SANS, .font_bold = FONT_SANS_BOLD, .font_mono = FONT_MONO,
};

static const struct ui_theme dark = {
    .window = RGB(0x1E2228), .surface = RGB(0x272C33), .surface_alt = RGB(0x30363E),
    .border = RGB(0x48505B), .text = RGB(0xE6EAF0), .text_dim = RGB(0x9AA4B2),
    .accent = RGB(0x4C8DFF), .accent_text = RGB(0xFFFFFF), .selection = RGB(0x2D4F86),
    .selection_text = RGB(0xFFFFFF), .hover = RGB(0x343B45), .pressed = RGB(0x404855),
    .danger = RGB(0xFF6464), .focus_ring = RGB(0x6FA8FF), .input = RGB(0x1A1E23),
    .shadow = ARGB(0x60, 0, 0, 0),
    .font_size = 14, .radius = 6, .padding = 8, .spacing = 6, .row_height = 30,
    .font = FONT_SANS, .font_bold = FONT_SANS_BOLD, .font_mono = FONT_MONO,
};

static const struct ui_theme high_contrast = {
    .window = RGB(0x000000), .surface = RGB(0x000000), .surface_alt = RGB(0x141414),
    .border = RGB(0xFFFFFF), .text = RGB(0xFFFFFF), .text_dim = RGB(0xE8E8E8),
    .accent = RGB(0xFFE600), .accent_text = RGB(0x000000), .selection = RGB(0x00E5FF),
    .selection_text = RGB(0x000000), .hover = RGB(0x2A2A2A), .pressed = RGB(0x4A4A4A),
    .danger = RGB(0xFF5050), .focus_ring = RGB(0xFFE600), .input = RGB(0x000000),
    .shadow = ARGB(0x80, 0, 0, 0),
    .font_size = 16, .radius = 2, .padding = 8, .spacing = 6, .row_height = 34,
    .font = FONT_SANS, .font_bold = FONT_SANS_BOLD, .font_mono = FONT_MONO,
};

struct ui_theme ui_theme = light;

void ui_set_theme(const char *name)
{
    if (!name || !strcmp(name, "light"))
        ui_theme = light;
    else if (!strcmp(name, "dark"))
        ui_theme = dark;
    else if (!strcmp(name, "high-contrast") || !strcmp(name, "contrast"))
        ui_theme = high_contrast;
    else
        return;
    for (struct ui_window *w = ui_windows; w; w = w->next)
        ui_relayout(w);
}

// Reads the theme from the user's settings (or AEGIS_THEME) once.
void ui_load_user_theme(void)
{
    const char *env = getenv("AEGIS_THEME");
    char path[256], buf[64];
    const char *home = getenv("HOME");
    int fd;
    ssize_t n;

    if (env) {
        ui_set_theme(env);
        return;
    }
    if (!home)
        return;
    // /users/<name>/home -> /users/<name>/system/settings/theme
    snprintf(path, sizeof(path), "%s/../system/settings/theme", home);
    if ((fd = open(path, O_RDONLY)) < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    for (char *p = buf; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == ' ')
            *p = 0;
    ui_set_theme(buf);
}

struct font *ui_font_sized(const char *name, int px)
{
    struct font *f = font_get(name, px);

    return f ? f : font_get(FONT_SANS, px);
}

struct font *ui_font(void)
{
    return ui_font_sized(ui_theme.font, ui_theme.font_size);
}

struct font *ui_font_bold(void)
{
    return ui_font_sized(ui_theme.font_bold, ui_theme.font_size);
}

struct font *ui_font_mono(void)
{
    return ui_font_sized(ui_theme.font_mono, ui_theme.font_size);
}

color_t ui_mix(color_t a, color_t b, int t)
{
    uint32_t r = 0;

    for (int s = 0; s < 32; s += 8) {
        int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;

        r |= (uint32_t)((ca * (255 - t) + cb * t) / 255) << s;
    }
    return r;
}

void ui_clip(struct gfx *g, struct rect r)
{
    r.x += g->ox;
    r.y += g->oy;
    if (!rect_intersect(g->clip, r, &g->clip))
        g->clip = (struct rect){ 0, 0, 0, 0 };
}

void ui_draw_text(struct gfx *g, struct font *f, struct rect r, const char *s, color_t c, enum align h)
{
    int len = strlen(s), w = text_width(f, s, len), x = r.x;
    int y = r.y + (r.h - font_line_height(f)) / 2;
    static const char ellipsis[] = "\xE2\x80\xA6";

    if (w > r.w) {
        // Cut at a character boundary and end with an ellipsis.
        int ew = text_width(f, ellipsis, 3), cut = len;

        while (cut > 0 && text_width(f, s, cut) + ew > r.w)
            cut = utf8_prev(s, cut);
        x = text_draw(g, f, r.x, y, s, cut, c);
        text_draw(g, f, x, y, ellipsis, 3, c);
        return;
    }
    if (h == ALIGN_CENTER)
        x = r.x + (r.w - w) / 2;
    else if (h == ALIGN_END)
        x = r.x + r.w - w;
    text_draw(g, f, x, y, s, len, c);
}

void ui_draw_focus(struct gfx *g, struct rect r, int radius)
{
    gfx_outline_rounded(g, (struct rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 }, radius + 2, 2,
                        ui_theme.focus_ring);
}

void ui_draw_button_bg(struct gfx *g, struct widget *w, struct rect r, bool primary)
{
    color_t bg, border;
    int rad = ui_theme.radius;

    if (primary) {
        bg = ui_theme.accent;
        if (w->pressed && w->hover)
            bg = ui_mix(bg, RGB(0x000000), 40);
        else if (w->hover)
            bg = ui_mix(bg, RGB(0xFFFFFF), 30);
        border = bg;
    } else {
        bg = w->pressed && w->hover ? ui_theme.pressed : w->hover ? ui_theme.hover : ui_theme.surface;
        border = ui_theme.border;
    }
    if (!w->enabled) {
        bg = ui_mix(bg, ui_theme.window, 140);
        border = ui_mix(border, ui_theme.window, 140);
    }
    gfx_fill_rounded(g, r, rad, bg);
    if (border != bg)
        gfx_outline_rounded(g, r, rad, 1, border);
}

void ui_draw_check(struct gfx *g, float x, float y, float s, color_t c)
{
    gfx_line(g, x + s * 0.20f, y + s * 0.52f, x + s * 0.42f, y + s * 0.74f, 2.2f, c);
    gfx_line(g, x + s * 0.42f, y + s * 0.74f, x + s * 0.80f, y + s * 0.28f, 2.2f, c);
}

void ui_draw_arrow(struct gfx *g, int cx, int cy, int size, int dir, color_t c)
{
    float s = size / 2.0f;

    switch (dir) {
    case 0:
        gfx_line(g, cx - s, cy - s / 2, cx, cy + s / 2, 1.6f, c);
        gfx_line(g, cx, cy + s / 2, cx + s, cy - s / 2, 1.6f, c);
        break;
    case 1:
        gfx_line(g, cx - s, cy + s / 2, cx, cy - s / 2, 1.6f, c);
        gfx_line(g, cx, cy - s / 2, cx + s, cy + s / 2, 1.6f, c);
        break;
    case 2:
        gfx_line(g, cx - s / 2, cy - s, cx + s / 2, cy, 1.6f, c);
        gfx_line(g, cx + s / 2, cy, cx - s / 2, cy + s, 1.6f, c);
        break;
    default:
        gfx_line(g, cx + s / 2, cy - s, cx - s / 2, cy, 1.6f, c);
        gfx_line(g, cx - s / 2, cy, cx + s / 2, cy + s, 1.6f, c);
        break;
    }
}

int ui_scrollbar_thumb(struct rect track, int total, int visible, int offset, int *thumb_h)
{
    int range = total - visible, th;

    if (total <= 0 || visible >= total) {
        *thumb_h = track.h;
        return track.y;
    }
    th = MAX(24, (int)((int64_t)track.h * visible / total));
    th = MIN(th, track.h);
    *thumb_h = th;
    return track.y + (int)((int64_t)(track.h - th) * MIN(MAX(offset, 0), range) / MAX(range, 1));
}

void ui_draw_scrollbar(struct gfx *g, struct rect track, int total, int visible, int offset, bool hot)
{
    int th, ty;

    if (visible >= total)
        return;
    ty = ui_scrollbar_thumb(track, total, visible, offset, &th);
    gfx_fill_rounded(g, (struct rect){ track.x + 2, track.y, track.w - 4, track.h }, (track.w - 4) / 2,
                     ALPHA(ui_theme.text, hot ? 0x14 : 0x0A));
    gfx_fill_rounded(g, (struct rect){ track.x + 2, ty, track.w - 4, th }, (track.w - 4) / 2,
                     ALPHA(ui_theme.text, hot ? 0x80 : 0x50));
}

// The offset that puts the thumb's grab point (pixels from its top) at y.
int ui_scrollbar_offset(struct rect track, int total, int visible, int y, int grab)
{
    int th, range = total - visible;

    if (range <= 0)
        return 0;
    ui_scrollbar_thumb(track, total, visible, 0, &th);
    if (track.h - th <= 0)
        return 0;
    return MIN(MAX((int)((int64_t)(y - grab - track.y) * range / (track.h - th)), 0), range);
}
