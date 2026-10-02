#include "browser.h"

// Paints the display list: the part of the page inside the viewport.

static color_t shade(color_t c, int amount)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;

    r = MIN(MAX(r + amount, 0), 255);
    g = MIN(MAX(g + amount, 0), 255);
    b = MIN(MAX(b + amount, 0), 255);
    return (c & 0xFF000000U) | (r << 16) | (g << 8) | b;
}

static void side(struct gfx *g, struct rect r, int style, color_t c, bool horizontal)
{
    if (style == BS_DASHED || style == BS_DOTTED) {
        int seg = style == BS_DOTTED ? MAX(horizontal ? r.h : r.w, 1) : MAX(3 * (horizontal ? r.h : r.w), 3);

        if (horizontal)
            for (int x = r.x; x < r.x + r.w; x += seg * 2)
                gfx_fill(g, (struct rect){ x, r.y, MIN(seg, r.x + r.w - x), r.h }, c);
        else
            for (int y = r.y; y < r.y + r.h; y += seg * 2)
                gfx_fill(g, (struct rect){ r.x, y, r.w, MIN(seg, r.y + r.h - y) }, c);
        return;
    }
    if (style == BS_DOUBLE && (horizontal ? r.h : r.w) >= 3) {
        int t = (horizontal ? r.h : r.w) / 3;

        if (horizontal) {
            gfx_fill(g, (struct rect){ r.x, r.y, r.w, t }, c);
            gfx_fill(g, (struct rect){ r.x, r.y + r.h - t, r.w, t }, c);
        } else {
            gfx_fill(g, (struct rect){ r.x, r.y, t, r.h }, c);
            gfx_fill(g, (struct rect){ r.x + r.w - t, r.y, t, r.h }, c);
        }
        return;
    }
    gfx_fill(g, r, c);
}

static void paint_border(struct gfx *g, const struct item *it, struct rect r)
{
    const int *b = it->border;
    bool uniform = b[0] == b[1] && b[1] == b[2] && b[2] == b[3] && it->border_color[0] == it->border_color[1]
                   && it->border_color[1] == it->border_color[2] && it->border_color[2] == it->border_color[3]
                   && it->border_style[0] == it->border_style[1] && it->border_style[0] == it->border_style[2]
                   && it->border_style[0] == it->border_style[3];
    color_t c[4];

    for (int i = 0; i < 4; i++) {
        c[i] = it->border_color[i];
        if (it->border_style[i] == BS_INSET)
            c[i] = i == 0 || i == 3 ? shade(c[i], -60) : shade(c[i], 60);
        else if (it->border_style[i] == BS_OUTSET)
            c[i] = i == 0 || i == 3 ? shade(c[i], 60) : shade(c[i], -60);
    }
    if (uniform && it->radius > 0 && it->border_style[0] != BS_INSET && it->border_style[0] != BS_OUTSET) {
        gfx_outline_rounded(g, r, MIN(it->radius, MIN(r.w, r.h) / 2), b[0], c[0]);
        return;
    }
    if (b[0])
        side(g, (struct rect){ r.x, r.y, r.w, b[0] }, it->border_style[0], c[0], true);
    if (b[2])
        side(g, (struct rect){ r.x, r.y + r.h - b[2], r.w, b[2] }, it->border_style[2], c[2], true);
    if (b[3])
        side(g, (struct rect){ r.x, r.y + b[0], b[3], r.h - b[0] - b[2] }, it->border_style[3], c[3], false);
    if (b[1])
        side(g, (struct rect){ r.x + r.w - b[1], r.y + b[0], b[1], r.h - b[0] - b[2] }, it->border_style[1], c[1],
             false);
}

static int pos(struct len l, int space)
{
    return l.unit == LEN_PCT ? (int)(l.v * space / 100) : l.unit == LEN_PX ? (int)l.v : 0;
}

// A background picture inside r.
static void paint_background_image(struct gfx *g, const struct style *s, struct surface *img, struct rect r)
{
    struct gfx saved;
    int w = img->width, h = img->height, x0, y0;

    if (!w || !h || r.w <= 0 || r.h <= 0)
        return;
    if (s->bg_size == BG_SIZE_COVER || s->bg_size == BG_SIZE_CONTAIN) {
        float sx = (float)r.w / w, sy = (float)r.h / h, k = s->bg_size == BG_SIZE_COVER ? MAX(sx, sy) : MIN(sx, sy);

        w = MAX(1, (int)(w * k));
        h = MAX(1, (int)(h * k));
    }
    saved = gfx_push(g, r);
    x0 = pos(s->bg_x, r.w - w);
    y0 = pos(s->bg_y, r.h - h);
    {
        bool rx = s->bg_repeat == BG_REPEAT || s->bg_repeat == BG_REPEAT_X;
        bool ry = s->bg_repeat == BG_REPEAT || s->bg_repeat == BG_REPEAT_Y;
        int sx = rx ? x0 - (x0 > 0 ? ((x0 + w - 1) / w) * w : 0) : x0;
        int sy = ry ? y0 - (y0 > 0 ? ((y0 + h - 1) / h) * h : 0) : y0;

        for (int y = sy; y < r.h && (ry || y == sy); y += h)
            for (int x = sx; x < r.w && (rx || x == sx); x += w) {
                if (w == img->width && h == img->height)
                    gfx_blit(g, img, (struct rect){ 0, 0, img->width, img->height }, x, y);
                else
                    gfx_blit_scaled(g, img, (struct rect){ 0, 0, img->width, img->height },
                                    (struct rect){ x, y, w, h });
            }
    }
    gfx_restore(g, saved);
}

struct node *select_option(struct node *sel, int index)
{
    int i = 0;

    for (struct node *o = sel->first; o; o = node_walk(o, sel))
        if (node_is(o, "option") && i++ == index)
            return o;
    return NULL;
}

int select_current(struct node *sel)
{
    int i = 0;

    if (sel->selected >= 0)
        return sel->selected;
    for (struct node *o = sel->first; o; o = node_walk(o, sel))
        if (node_is(o, "option")) {
            if (node_attr(o, "selected"))
                return i;
            i++;
        }
    return 0;
}

const char *control_value(struct node *n)
{
    const char *v = n->value ? n->value : node_attr(n, "value");

    if (!v && node_is(n, "textarea"))
        v = n->first && n->first->type == NODE_TEXT ? n->first->text : "";
    return v ? v : "";
}

static void button_face(struct gfx *g, struct rect r, const char *label, struct font *f, color_t text, bool pressed)
{
    int tw = text_width(f, label, -1);

    gfx_fill_rounded(g, r, 3, pressed ? RGB(0xD0D0D7) : RGB(0xE9E9ED));
    gfx_outline_rounded(g, r, 3, 1, RGB(0x8F8F9D));
    text_draw(g, f, r.x + (r.w - tw) / 2, r.y + (r.h - font_line_height(f)) / 2, label, -1, text);
}

static void paint_control(struct gfx *g, const struct item *it, struct rect r, bool focused, int caret)
{
    struct node *n = it->node;
    const char *type = node_attr(n, "type");
    struct font *f = it->font;
    const char *v = control_value(n);
    color_t text = it->color ? it->color : RGB(0);
    color_t ring = focused ? RGB(0x2F6FDF) : RGB(0x8F8F9D);

    if (node_is(n, "select")) {
        struct node *o = select_option(n, select_current(n));
        char *label = o ? node_text(o) : NULL;
        float tri[6] = { r.x + r.w - 16, r.y + r.h / 2 - 2, r.x + r.w - 8, r.y + r.h / 2 - 2, r.x + r.w - 12,
                         r.y + r.h / 2 + 3 };

        gfx_fill_rounded(g, r, 3, RGB(0xE9E9ED));
        gfx_outline_rounded(g, r, 3, focused ? 2 : 1, ring);
        if (label) {
            struct gfx saved = gfx_push(g, (struct rect){ r.x + 6, r.y, MAX(r.w - 26, 0), r.h });

            text_draw(g, f, 0, (r.h - font_line_height(f)) / 2, label, -1, text);
            gfx_restore(g, saved);
            free(label);
        }
        gfx_polygon(g, tri, 3, RGB(0x333333));
        return;
    }
    if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
        bool radio = !strcasecmp(type, "radio");

        if (radio) {
            gfx_circle(g, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 2.0f, n->checked ? RGB(0x2F6FDF) : RGB(0xFFFFFF));
            gfx_ring(g, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 2.0f - 0.5f, 1, n->checked ? RGB(0x2F6FDF) : ring);
            if (n->checked)
                gfx_circle(g, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 5.0f, RGB(0xFFFFFF));
        } else {
            gfx_fill_rounded(g, r, 2, n->checked ? RGB(0x2F6FDF) : RGB(0xFFFFFF));
            gfx_outline_rounded(g, r, 2, 1, n->checked ? RGB(0x2F6FDF) : ring);
            if (n->checked) {
                gfx_line(g, r.x + 3, r.y + r.h / 2.0f, r.x + r.w / 2.5f, r.y + r.h - 3.5f, 2, RGB(0xFFFFFF));
                gfx_line(g, r.x + r.w / 2.5f, r.y + r.h - 3.5f, r.x + r.w - 3, r.y + 3, 2, RGB(0xFFFFFF));
            }
        }
        return;
    }
    if (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset")
                 || !strcasecmp(type, "image"))) {
        const char *label = n->value ? n->value : node_attr(n, "value");

        if (!label)
            label = !strcasecmp(type, "reset") ? "Reset" : !strcasecmp(type, "submit") ? "Submit" : "";
        button_face(g, r, label, f, text, false);
        if (focused)
            gfx_outline_rounded(g, (struct rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 }, 4, 1, ring);
        return;
    }
    // Text fields.
    gfx_fill_rounded(g, r, 2, RGB(0xFFFFFF));
    gfx_outline_rounded(g, r, 2, focused ? 2 : 1, ring);
    {
        struct gfx saved = gfx_push(g, (struct rect){ r.x + 4, r.y + 2, MAX(r.w - 8, 0), MAX(r.h - 4, 0) });
        bool password = type && !strcasecmp(type, "password");
        int lh = font_line_height(f), ty = node_is(n, "textarea") ? 2 : (r.h - 4 - lh) / 2;

        if (!*v && !focused && node_attr(n, "placeholder")) {
            text_draw(g, f, 0, ty, node_attr(n, "placeholder"), -1, RGB(0x8A8A8A));
        } else if (node_is(n, "textarea")) {
            const char *line = v;
            int y = ty, at = 0;

            for (;;) {
                const char *nl = strchr(line, '\n');
                int len = nl ? nl - line : (int)strlen(line);

                text_draw(g, f, 0, y, line, len, text);
                if (focused && caret >= at && caret <= at + len)
                    gfx_fill(g, (struct rect){ text_width(f, line, caret - at), y, 1, lh }, text);
                if (!nl)
                    break;
                at += len + 1;
                line = nl + 1;
                y += lh;
            }
        } else {
            char masked[512];
            const char *shown = v;
            int cx, scroll = 0;
            int cpos = MIN(caret, (int)strlen(v));

            if (password) {
                size_t k = 0;

                for (const char *p = v; *p && k + 4 < sizeof(masked); utf8_decode(&p))
                    k += utf8_encode(0x2022, masked + k);
                masked[k] = 0;
                shown = masked;
                // The caret counts characters in the original.
                {
                    int chars = 0;

                    for (const char *p = v; *p && p < v + cpos; utf8_decode(&p))
                        chars++;
                    cpos = chars * 3;
                }
            }
            cx = text_width(f, shown, cpos);
            if (focused && cx > r.w - 12)
                scroll = cx - (r.w - 12);
            text_draw(g, f, -scroll, ty, shown, -1, text);
            if (focused)
                gfx_fill(g, (struct rect){ cx - scroll, ty, 1, lh }, text);
        }
        gfx_restore(g, saved);
    }
}

void paint_page(struct page_layout *pl, struct gfx *g, struct rect r, int sx, int sy, struct node *focus,
                struct node *hover, int caret)
{
    struct gfx saved = gfx_push(g, r);

    (void)hover;
    gfx_fill(g, (struct rect){ 0, 0, r.w, r.h }, pl->background);
    if (pl->background_style && pl->background_style->background_image)
        paint_background_image(g, pl->background_style, pl->background_style->background_image,
                               (struct rect){ -sx, -sy, MAX(pl->width, r.w), MAX(pl->height, r.h) });
    for (int i = 0; i < pl->n; i++) {
        const struct item *it = &pl->items[i];
        struct rect ir = { it->r.x - sx, it->r.y - sy, it->r.w, it->r.h };
        struct gfx clipped;
        bool clip = it->clipped;

        if (ir.y > r.h || ir.y + ir.h < -4 || ir.x > r.w || ir.x + ir.w < -4)
            if (it->kind != ITEM_TEXT || ir.y > r.h || ir.y + ir.h < 0)
                continue;
        if (clip) {
            struct rect c = { it->clip.x - sx, it->clip.y - sy, it->clip.w, it->clip.h };

            if (c.w <= 0 || c.h <= 0)
                continue;
            clipped = gfx_push(g, c);
            ir.x -= c.x;
            ir.y -= c.y;
        }
        switch (it->kind) {
        case ITEM_RECT:
            if (it->color >> 24) {
                if (it->radius > 0)
                    gfx_fill_rounded(g, ir, MIN(it->radius, MIN(ir.w, ir.h) / 2), it->color);
                else
                    gfx_fill(g, ir, it->color);
            }
            if (it->image && it->style)
                paint_background_image(g, it->style, it->image, ir);
            break;
        case ITEM_BORDER:
            paint_border(g, it, ir);
            break;
        case ITEM_TEXT: {
            int asc = font_ascent(it->font), size = font_line_height(it->font);
            int thick = MAX(1, size / 16);

            text_draw(g, it->font, ir.x, ir.y, it->text, it->len, it->color);
            if (it->decoration & DEC_UNDERLINE)
                gfx_fill(g, (struct rect){ ir.x, ir.y + asc + MAX(1, size / 10), ir.w, thick }, it->color);
            if (it->decoration & DEC_LINE_THROUGH)
                gfx_fill(g, (struct rect){ ir.x, ir.y + asc * 2 / 3, ir.w, thick }, it->color);
            if (it->decoration & DEC_OVERLINE)
                gfx_fill(g, (struct rect){ ir.x, ir.y, ir.w, thick }, it->color);
            break;
        }
        case ITEM_IMAGE: {
            struct surface *img = it->node ? it->node->image : NULL;

            if (img && ir.w > 0 && ir.h > 0) {
                if (ir.w == img->width && ir.h == img->height)
                    gfx_blit(g, img, (struct rect){ 0, 0, img->width, img->height }, ir.x, ir.y);
                else
                    gfx_blit_scaled(g, img, (struct rect){ 0, 0, img->width, img->height }, ir);
            } else if (ir.w > 0 && ir.h > 0) {
                gfx_fill(g, ir, RGB(0xF0F0F0));
                gfx_outline(g, ir, 1, RGB(0xC8C8C8));
            }
            break;
        }
        case ITEM_BULLET:
            if (it->radius == LS_SQUARE)
                gfx_fill(g, ir, it->color);
            else if (it->radius == LS_CIRCLE)
                gfx_ring(g, ir.x + ir.w / 2.0f, ir.y + ir.h / 2.0f, ir.w / 2.0f - 0.5f, 1, it->color);
            else
                gfx_circle(g, ir.x + ir.w / 2.0f, ir.y + ir.h / 2.0f, ir.w / 2.0f, it->color);
            break;
        case ITEM_CONTROL:
            paint_control(g, it, ir, it->node == focus, caret);
            break;
        }
        if (clip)
            gfx_restore(g, clipped);
    }
    gfx_restore(g, saved);
}
