#include "browser.h"
#include <math.h>

// Layout: turns the styled tree into a display list of positioned items.
//
// Block boxes stack vertically, with sibling margins collapsed. Runs of
// inline content are broken into lines made of "pieces" (a word, an image,
// an inline block); a finished line places its pieces by vertical-align and
// text-align. Inline blocks, floats, table cells and flex items are laid
// out at their own origin and moved into place afterwards.
//
// Intrinsic widths (for shrink-to-fit, tables and flex) come from laying a
// box out at width 0 (narrowest) and very wide (widest) and measuring what
// it used; the results are cached per node for one layout.

#define WIDE        (1 << 20)

enum { MODE_NORMAL, MODE_SHRINK, MODE_FILL, MODE_EXACT };

struct boxres {
    int w;                          // margin box width
    int h;                          // border box height
    int mt, mb;                     // vertical margins
    int extent;                     // right edge used by content (absolute)
    int bg, bd;                     // background and border items, or -1
};

static struct page_layout *P;
static int generation, measuring;
static struct node *cur_link;

// ---- The display list ----

static int add(struct item it)
{
    if (P->n == P->cap) {
        int cap = P->cap ? P->cap * 2 : 256;
        struct item *m = realloc(P->items, cap * sizeof(*m));

        if (!m)
            return -1;
        P->items = m;
        P->cap = cap;
    }
    it.link = it.link ? it.link : cur_link;
    P->items[P->n] = it;
    return P->n++;
}

static void translate(int first, int last, int dx, int dy)
{
    for (int i = first; i < last; i++) {
        P->items[i].r.x += dx;
        P->items[i].r.y += dy;
        if (P->items[i].clipped) {
            P->items[i].clip.x += dx;
            P->items[i].clip.y += dy;
        }
    }
}

static const char *keep_string(char *s)
{
    char **m;

    if (!s)
        return "";
    if (!(m = realloc(P->strings, (P->nstrings + 1) * sizeof(char *)))) {
        free(s);
        return "";
    }
    P->strings = m;
    P->strings[P->nstrings++] = s;
    return s;
}

static void anchor(struct node *n, int y)
{
    struct anchor *m;

    if (measuring || (!node_attr(n, "id") && !node_attr(n, "name")))
        return;
    if (!(m = realloc(P->anchors, (P->nanchors + 1) * sizeof(*m))))
        return;
    P->anchors = m;
    P->anchors[P->nanchors++] = (struct anchor){ n, y };
}

static int resolve(struct len l, int base)
{
    if (l.unit == LEN_PX)
        return (int)lroundf(l.v);
    if (l.unit == LEN_PCT)
        return (int)lroundf(l.v * base / 100);
    return 0;
}

static bool has_len(struct len l)
{
    return l.unit == LEN_PX || (l.unit == LEN_PCT && !measuring);
}

static int line_height_of(const struct style *s)
{
    return s->line_height > 0 ? (int)lroundf(s->line_height) : (int)lroundf(s->font_size * 1.2f);
}

static bool hidden_box(const struct style *s)
{
    return !s || s->display == D_NONE || s->position == POS_ABSOLUTE || s->position == POS_FIXED;
}

static bool is_whitespace_text(const struct node *n)
{
    for (const char *p = n->text; *p; p++)
        if (!isspace((unsigned char)*p))
            return false;
    return true;
}

static struct boxres layout_box(struct node *n, int x, int y, int avail, int mode, int exact);
static int layout_children(struct node *n, int x, int y, int w, int *ext);
static int measure(struct node *n, bool widest);

// ---- Inline formatting ----

struct piece {
    int first, last;                // items
    int x, w, h, asc;
    uint8_t valign;
    int shift;                      // super/sub
    bool right;                     // float: right
};

struct line {
    int x0, width, y;
    int cx;
    struct piece *pieces;
    int np, cap;
    bool space;                     // collapsible space pending
    bool can_break;                 // a line may end before the next piece
    struct font *space_font;
    const struct style *cs;         // the block container's style
    int ext;                        // widest line's right edge
    int right_w;                    // width taken by right floats on this line
    int lines;
    const struct style *bg;         // the innermost inline ancestor with a background
    int bg_item;
};

static void add_piece(struct line *L, struct piece p)
{
    if (L->np == L->cap) {
        int cap = L->cap ? L->cap * 2 : 16;
        struct piece *m = realloc(L->pieces, cap * sizeof(*m));

        if (!m)
            return;
        L->pieces = m;
        L->cap = cap;
    }
    L->pieces[L->np++] = p;
}

static void finish_line(struct line *L, bool force)
{
    struct font *f = style_font(L->cs);
    int lh = line_height_of(L->cs), fh = font_line_height(f);
    int asc = (lh - fh) / 2 + font_ascent(f), desc = lh - asc, height, off = 0, rx;
    int content = L->cx - L->right_w;

    if (!L->np && !force)
        return;
    for (int i = 0; i < L->np; i++) {
        struct piece *p = &L->pieces[i];

        if (p->valign == VA_BASELINE || p->valign == VA_SUPER || p->valign == VA_SUB) {
            asc = MAX(asc, p->asc + p->shift);
            desc = MAX(desc, p->h - p->asc - p->shift);
        }
    }
    height = asc + desc;
    for (int i = 0; i < L->np; i++)
        if (L->pieces[i].valign != VA_BASELINE && L->pieces[i].valign != VA_SUPER && L->pieces[i].valign != VA_SUB)
            height = MAX(height, L->pieces[i].h);
    if (L->cs->text_align == ALIGN_CENTER)
        off = MAX(0, (L->width - L->right_w - content) / 2);
    else if (L->cs->text_align == ALIGN_RIGHT)
        off = MAX(0, L->width - L->right_w - content);
    rx = L->x0 + L->width;
    for (int i = L->np - 1; i >= 0; i--) {
        struct piece *p = &L->pieces[i];
        int py, px;

        switch (p->valign) {
        case VA_MIDDLE:
            py = (height - p->h) / 2;
            break;
        case VA_TOP:
            py = 0;
            break;
        case VA_BOTTOM:
            py = height - p->h;
            break;
        default:
            py = asc - p->asc - p->shift;
            break;
        }
        if (p->right) {
            rx -= p->w;
            px = rx;
        } else {
            px = L->x0 + p->x + off;
        }
        translate(p->first, p->last, px, L->y + py);
    }
    L->ext = MAX(L->ext, L->x0 + L->cx);
    L->y += height;
    L->cx = 0;
    L->np = 0;
    L->space = false;
    L->can_break = false;
    L->right_w = 0;
    L->lines++;
}

static bool can_wrap(const struct line *L, const struct style *s)
{
    (void)L;
    return s->white_space == WS_NORMAL || s->white_space == WS_PRE_WRAP;
}

// Places one run of text on the line (breaking before it if needed).
static void place_text(struct line *L, const char *s, int len, const struct style *st, int w)
{
    struct font *f = style_font(st);
    int lh = line_height_of(st), fh = font_line_height(f), half = (lh - fh) / 2, first = P->n;
    int sw = 0;

    if (L->space && L->np)
        sw = text_width(L->space_font ? L->space_font : f, " ", 1);
    if (L->np && can_wrap(L, st) && (L->space || L->can_break) && L->cx + sw + w > L->width) {
        finish_line(L, false);
        sw = 0;
    }
    L->cx += sw;
    if (sw && L->np) {
        // Underlines and backgrounds continue across the space.
        struct piece *last = &L->pieces[L->np - 1];
        struct item *prev = last->last > last->first ? &P->items[last->last - 1] : NULL;

        if (prev && prev->kind == ITEM_TEXT && prev->link == cur_link
            && (prev->decoration & st->decoration & DEC_UNDERLINE))
            prev->r.w += sw;
        if (L->bg_item >= 0 && L->bg && L->bg_item == P->n - 2)
            P->items[L->bg_item].r.w += sw;
    }
    if (L->bg) {
        int pt = resolve(L->bg->padding[0], L->width), pb = resolve(L->bg->padding[2], L->width);

        L->bg_item = add((struct item){ .kind = ITEM_RECT, .r = { 0, half - pt, w, fh + pt + pb },
                                        .color = L->bg->background, .radius = L->bg->radius,
                                        .style = L->bg });
    }
    add((struct item){ .kind = ITEM_TEXT, .r = { 0, half, w, fh }, .color = st->color, .font = f, .text = s,
                       .len = len, .decoration = st->decoration });
    add_piece(L, (struct piece){ first, P->n, L->cx, w, lh, half + font_ascent(f), st->vertical_align,
                                 st->vertical_align == VA_SUPER ? st->font_size / 2
                                 : st->vertical_align == VA_SUB ? -st->font_size / 4 : 0, false });
    L->cx += w;
    L->space = false;
    L->can_break = false;
}

static bool cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF)
           || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F);
}

static const char *transformed(const char *s, int len, int how, bool *word_start)
{
    char *out = malloc(len * 2 + 1);
    int o = 0;

    if (!out)
        return s;
    for (int i = 0; i < len; i++) {
        unsigned char c = s[i];

        if (how == TT_UPPER || (how == TT_CAPITALIZE && *word_start))
            c = toupper(c);
        else if (how == TT_LOWER)
            c = tolower(c);
        *word_start = isspace(c) || c == '-';
        out[o++] = c;
    }
    out[o] = 0;
    return keep_string(out);
}

// Splits text into words and places them.
static void place_words(struct line *L, const char *s, int len, const struct style *st)
{
    struct font *f = style_font(st);
    const char *end = s + len;
    bool wrap = can_wrap(L, st);

    while (s < end) {
        const char *w = s;
        int ww;

        if (isspace((unsigned char)*s)) {
            while (s < end && isspace((unsigned char)*s))
                s++;
            if (L->np)
                L->space = true;
            L->space_font = f;
            continue;
        }
        // One word; CJK characters each stand alone.
        {
            const char *p = s;
            uint32_t cp = utf8_decode(&p);

            if (cjk(cp)) {
                s = p;
            } else {
                while (s < end && !isspace((unsigned char)*s)) {
                    const char *q = s;

                    cp = utf8_decode(&q);
                    if (cjk(cp))
                        break;
                    s = q;
                    // Allow a break after a hyphen or slash in long words.
                    if ((cp == '-' || cp == '/') && s - w > 6)
                        break;
                }
            }
        }
        ww = text_width(f, w, s - w);
        if (wrap && ww > L->width && L->width > 0) {
            // Too long for any line: break it where it fits.
            const char *p = w;

            while (p < s) {
                int avail = L->width - (L->np ? L->cx : 0), used = 0;
                const char *q = p, *fit = p;

                if (L->np && avail < ww / 4) {
                    finish_line(L, false);
                    avail = L->width;
                }
                while (q < s) {
                    const char *r = q;
                    int cw;

                    utf8_decode(&r);
                    cw = text_width(f, q, r - q);
                    if (used + cw > avail && fit > p)
                        break;
                    used += cw;
                    q = r;
                    fit = q;
                }
                place_text(L, p, fit - p, st, used);
                if (fit < s)
                    finish_line(L, false);
                p = fit;
            }
            continue;
        }
        place_text(L, w, s - w, st, ww);
        // After a hyphen, a slash or a CJK character the line may end.
        if (s > w) {
            const char *last = w + utf8_prev(w, s - w);
            uint32_t cp = utf8_decode(&last);

            L->can_break = cp == '-' || cp == '/' || cjk(cp);
        }
    }
}

static void place_text_node(struct line *L, struct node *t, const struct style *st)
{
    const char *s = t->text;
    int len = strlen(s);
    bool word_start = true;

    if (!len)
        return;
    if (st->transform != TT_NONE)
        s = transformed(s, len, st->transform, &word_start);
    if (st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP) {
        // Drop the newline just after <pre>.
        if (*s == '\n' && t->parent && !t->prev && node_is(t->parent, "pre")) {
            s++;
            len--;
        }
        while (len > 0) {
            const char *nl = memchr(s, '\n', len);
            int seg = nl ? nl - s : len;

            if (st->white_space == WS_PRE) {
                if (seg) {
                    struct font *f = style_font(st);

                    L->space = false;
                    place_text(L, s, seg, st, text_width(f, s, seg));
                }
            } else {
                place_words(L, s, seg, st);
            }
            if (nl) {
                if (!L->np) {
                    // An empty line still takes its height.
                    struct font *f = style_font(st);

                    place_text(L, "", 0, st, 0);
                    (void)f;
                }
                finish_line(L, true);
                s += seg + 1;
                len -= seg + 1;
            } else {
                break;
            }
        }
        return;
    }
    place_words(L, s, len, st);
}

// The size of an image from its attributes, style and picture.
static void image_size(struct node *n, int avail, int *w, int *h)
{
    const struct style *s = n->style;
    const char *aw = node_attr(n, "width"), *ah = node_attr(n, "height");
    int nw = n->image ? n->image->width : 0, nh = n->image ? n->image->height : 0;
    int W = -1, H = -1;

    if (has_len(s->width))
        W = resolve(s->width, avail);
    else if (aw && *aw && !strchr(aw, '%'))
        W = atoi(aw);
    else if (aw && strchr(aw, '%') && !measuring)
        W = atoi(aw) * avail / 100;
    if (s->height.unit == LEN_PX)
        H = resolve(s->height, 0);
    else if (ah && *ah && !strchr(ah, '%'))
        H = atoi(ah);
    if (W < 0 && H < 0) {
        W = nw;
        H = nh;
    } else if (W < 0) {
        W = nh ? nw * H / nh : H;
    } else if (H < 0) {
        H = nw ? nh * W / nw : W;
    }
    if (has_len(s->max_width) && W > resolve(s->max_width, avail)) {
        int mw = resolve(s->max_width, avail);

        if (W)
            H = H * mw / W;
        W = mw;
    }
    if (s->max_height.unit == LEN_PX && H > resolve(s->max_height, 0)) {
        int mh = resolve(s->max_height, 0);

        if (H)
            W = W * mh / H;
        H = mh;
    }
    *w = MAX(W, 0);
    *h = MAX(H, 0);
}

static int control_width(struct node *n, const struct style *s, int avail)
{
    const char *type = node_attr(n, "type");
    struct font *f = style_font(s);

    if (has_len(s->width))
        return resolve(s->width, avail);
    if (node_is(n, "select")) {
        int w = 40;

        for (struct node *o = n->first; o; o = node_walk(o, n))
            if (node_is(o, "option")) {
                char *t = node_text(o);

                if (t) {
                    w = MAX(w, text_width(f, t, -1));
                    free(t);
                }
            }
        return w + 30;
    }
    if (node_is(n, "textarea")) {
        const char *c = node_attr(n, "cols");

        return (c ? atoi(c) : 30) * text_width(f, "n", 1) + 10;
    }
    if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")))
        return 13;
    if (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset"))) {
        const char *v = n->value ? n->value : !strcasecmp(type, "reset") ? "Reset"
                                              : !strcasecmp(type, "submit") ? "Submit" : "";

        return text_width(f, v, -1) + 20;
    }
    {
        const char *size = node_attr(n, "size");

        return size && atoi(size) > 0 ? atoi(size) * text_width(f, "n", 1) + 10 : 180;
    }
}

static int control_height(struct node *n, const struct style *s)
{
    const char *type = node_attr(n, "type");
    struct font *f = style_font(s);

    if (s->height.unit == LEN_PX)
        return resolve(s->height, 0);
    if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")))
        return 13;
    if (node_is(n, "textarea")) {
        const char *r = node_attr(n, "rows");

        return (r ? atoi(r) : 3) * font_line_height(f) + 8;
    }
    return font_line_height(f) + 10;
}

static void place_box(struct line *L, int first, int w, int h, int asc, const struct style *st, bool right)
{
    if (L->np && can_wrap(L, L->cs) && L->cx + w > L->width)
        finish_line(L, false);
    else if (L->space && L->np && !right)
        L->cx += text_width(L->space_font ? L->space_font : style_font(st), " ", 1);
    add_piece(L, (struct piece){ first, P->n, L->cx, w, h, asc, st->vertical_align, 0, right });
    L->cx += w;
    if (right)
        L->right_w += w;
    L->space = false;
    L->can_break = true;
}

static void walk_inline(struct line *L, struct node *n);

static void place_node(struct line *L, struct node *c)
{
    const struct style *st = c->style;
    struct node *saved_link = cur_link;

    if (c->type == NODE_TEXT) {
        place_text_node(L, c, c->parent && c->parent->style ? c->parent->style : L->cs);
        return;
    }
    if (hidden_box(st))
        return;
    if ((node_is(c, "a") && node_attr(c, "href")) || node_is(c, "button"))
        cur_link = c;
    anchor(c, L->y);
    if (node_is(c, "br")) {
        if (!L->np)
            place_text(L, "", 0, st, 0);
        finish_line(L, true);
    } else if (node_is(c, "img")) {
        int w, h, first = P->n, mt = resolve(st->margin[0], L->width), mb = resolve(st->margin[2], L->width);
        int ml = resolve(st->margin[3], L->width), mr = resolve(st->margin[1], L->width);
        const char *alt = node_attr(c, "alt");

        image_size(c, L->width, &w, &h);
        if (!w && !h && c->image_state == 3 && alt && *alt) {
            place_words(L, alt, strlen(alt), st);
        } else {
            int bw = st->border[1] + st->border[3], bh = st->border[0] + st->border[2];

            if (bw || bh)
                add((struct item){ .kind = ITEM_BORDER, .r = { ml, mt, w + bw, h + bh },
                                   .border = { st->border[0], st->border[1], st->border[2], st->border[3] },
                                   .border_color = { st->border_color[0], st->border_color[1],
                                                     st->border_color[2], st->border_color[3] },
                                   .border_style = { st->border_style[0], st->border_style[1],
                                                     st->border_style[2], st->border_style[3] } });
            add((struct item){ .kind = ITEM_IMAGE, .r = { ml + st->border[3], mt + st->border[0], w, h },
                               .node = c, .radius = st->radius });
            place_box(L, first, ml + w + bw + mr, mt + h + bh + mb, mt + h + bh, st, st->float_ == FLOAT_RIGHT);
        }
    } else if (node_is(c, "input") || node_is(c, "select") || node_is(c, "textarea")) {
        int w = control_width(c, st, L->width), h = control_height(c, st), first = P->n;
        int m = node_attr(c, "type") && (!strcasecmp(node_attr(c, "type"), "checkbox")
                                         || !strcasecmp(node_attr(c, "type"), "radio")) ? 3 : 1;

        add((struct item){ .kind = ITEM_CONTROL, .r = { m, m, w, h }, .node = c, .font = style_font(st),
                           .color = st->color, .style = st });
        place_box(L, first, w + 2 * m, h + 2 * m, h + 2 * m - (node_is(c, "textarea") ? h - 16 : 5), st, false);
    } else if (st->display == D_INLINE && !st->float_) {
        int ml = resolve(st->margin[3], L->width) + st->border[3] + resolve(st->padding[3], L->width);
        int mr = resolve(st->margin[1], L->width) + st->border[1] + resolve(st->padding[1], L->width);
        const struct style *saved_bg = L->bg;

        if (L->space && L->np && ml) {
            L->cx += text_width(L->space_font ? L->space_font : style_font(st), " ", 1);
            L->space = false;
        }
        L->cx += ml;
        if (st->background >> 24) {
            L->bg = st;
            L->bg_item = -1;
        }
        walk_inline(L, c);
        if (L->bg == st && L->bg_item >= 0 && L->bg_item < P->n) {
            // Padding around the ends.
            P->items[L->bg_item].r.w += mr;
        }
        L->bg = saved_bg;
        L->cx += mr;
    } else {
        // Inline blocks, floats, inline tables and flex.
        int first = P->n;
        struct boxres r = layout_box(c, 0, 0, L->width, MODE_SHRINK, 0);
        int h = r.mt + r.h + r.mb;
        struct font *f = style_font(st);
        int lh = line_height_of(st), asc;

        translate(first, P->n, 0, r.mt);
        // The baseline of its last line, roughly: near the bottom.
        asc = r.mt + r.h - st->border[2] - resolve(st->padding[2], L->width)
              - (lh - (lh - font_line_height(f)) / 2 - font_ascent(f));
        if (st->float_ || st->display == D_INLINE_FLEX || st->display == D_TABLE || asc < 0)
            asc = h;
        if (st->float_ && L->space)
            L->space = false;
        place_box(L, first, r.w, h, MIN(asc, h), st, st->float_ == FLOAT_RIGHT);
        if (st->float_)
            L->space = false;
    }
    cur_link = saved_link;
}

static void walk_inline(struct line *L, struct node *n)
{
    for (struct node *c = n->first; c; c = c->next)
        place_node(L, c);
}

// Lays out the inline nodes [first, stop) as lines; returns the new y.
static int inline_run(struct node *first, struct node *stop, int x, int y, int w, const struct style *cs,
                      int *ext, int indent)
{
    struct line L = { .x0 = x, .width = w, .y = y, .cs = cs, .ext = x, .bg_item = -1 };

    L.cx = indent;
    for (struct node *c = first; c != stop; c = c->next)
        place_node(&L, c);
    finish_line(&L, false);
    free(L.pieces);
    if (ext)
        *ext = MAX(*ext, L.ext);
    return L.y;
}

// ---- Blocks ----

static bool block_level(const struct style *s)
{
    return s->display == D_BLOCK || s->display == D_LIST_ITEM || s->display == D_TABLE || s->display == D_FLEX
           || s->display == D_TABLE_ROW_GROUP || s->display == D_TABLE_ROW || s->display == D_TABLE_CELL
           || s->display == D_TABLE_CAPTION;
}

// Is this child laid out in a line (true) or as a block (false)?
static bool inline_level(struct node *c)
{
    const struct style *s;

    if (c->type == NODE_TEXT)
        return true;
    s = c->style;
    if (!s)
        return true;
    if (s->float_)
        return true;
    if (block_level(s))
        return false;
    if (s->display == D_INLINE) {
        // An inline element holding blocks is treated as a block.
        for (struct node *k = c->first; k; k = k->next)
            if (k->type == NODE_ELEMENT && k->style && !hidden_box(k->style) && !inline_level(k))
                return false;
    }
    return true;
}

static bool run_is_blank(struct node *first, struct node *stop)
{
    for (struct node *c = first; c != stop; c = c->next) {
        if (c->type == NODE_TEXT) {
            const struct style *ps = c->parent ? c->parent->style : NULL;

            if (!is_whitespace_text(c) || (ps && ps->white_space == WS_PRE && strchr(c->text, '\n')
                                           && strlen(c->text) > 1))
                return false;
        } else if (!hidden_box(c->style)) {
            return false;
        }
    }
    return true;
}

static int collapse(int a, int b)
{
    if (a >= 0 && b >= 0)
        return MAX(a, b);
    if (a < 0 && b < 0)
        return MIN(a, b);
    return a + b;
}

static int layout_children(struct node *n, int x, int y, int w, int *ext)
{
    int cy = y, prev_mb = 0, indent = n->style ? resolve(n->style->text_indent, w) : 0;
    struct node *run = NULL;
    const struct style *cs = n->style;
    static struct style root_style;
    // Floats beside the flow: the width they take on each side, and where
    // they end. Content next to them is narrowed.
    int fl_l = 0, fl_r = 0, fl_lb = y, fl_rb = y;

    if (!cs) {
        if (!root_style.font_size) {
            root_style.font_size = 16;
            root_style.lh_factor = 1.2f;
            root_style.line_height = 19.2f;
            root_style.color = RGB(0);
        }
        cs = &root_style;
    }
    for (struct node *c = n->first;; c = c->next) {
        bool floating = c && c->type == NODE_ELEMENT && c->style && c->style->float_ && !hidden_box(c->style);

        if (c && !floating
            && (c->type == NODE_TEXT || (c->type == NODE_ELEMENT && (hidden_box(c->style) || inline_level(c))))) {
            if (!run && !(c->type == NODE_ELEMENT && hidden_box(c->style)))
                run = c;
            continue;
        }
        if (run && !run_is_blank(run, c)) {
            cy += prev_mb;
            prev_mb = 0;
            if (cy >= fl_lb)
                fl_l = 0;
            if (cy >= fl_rb)
                fl_r = 0;
            cy = inline_run(run, c, x + fl_l, cy, w - fl_l - fl_r, cs, ext, indent);
            indent = 0;
        }
        run = NULL;
        if (!c)
            break;
        if (c->type != NODE_ELEMENT)
            continue;
        if (floating) {
            const struct style *s = c->style;
            int start = P->n, avail, dx, top = cy;
            struct boxres r;

            if (top >= fl_lb)
                fl_l = 0;
            if (top >= fl_rb)
                fl_r = 0;
            if (s->clear) {
                top = MAX(top, MAX((s->clear & 1) ? fl_lb : top, (s->clear & 2) ? fl_rb : top));
                if (s->clear & 1)
                    fl_l = 0;
                if (s->clear & 2)
                    fl_r = 0;
            }
            avail = w - fl_l - fl_r;
            r = layout_box(c, 0, 0, avail, MODE_SHRINK, 0);
            if (r.w > avail && (fl_l || fl_r)) {
                // No room beside the others: below them, at full width.
                P->n = start;
                top = MAX(top, MAX(fl_l ? fl_lb : top, fl_r ? fl_rb : top));
                fl_l = fl_r = 0;
                r = layout_box(c, 0, 0, w, MODE_SHRINK, 0);
            }
            if (s->float_ == FLOAT_LEFT) {
                dx = x + fl_l;
                fl_l += r.w;
                fl_lb = MAX(fl_lb, top + r.mt + r.h + r.mb);
            } else {
                dx = x + w - fl_r - r.w;
                fl_r += r.w;
                fl_rb = MAX(fl_rb, top + r.mt + r.h + r.mb);
            }
            translate(start, P->n, dx, top + r.mt);
            if (ext)
                *ext = MAX(*ext, x + fl_l + fl_r);
            continue;
        }
        {
            const struct style *s = c->style;
            int mt = resolve(s->margin[0], w), top = cy + collapse(prev_mb, mt);
            int bx = x, bw = w;
            struct boxres r;

            if (s->clear) {
                int below = MAX((s->clear & 1) ? fl_lb : top, (s->clear & 2) ? fl_rb : top);

                top = MAX(top, below);
            }
            if (top >= fl_lb)
                fl_l = 0;
            if (top >= fl_rb)
                fl_r = 0;
            if (fl_l || fl_r) {
                bx += fl_l;
                bw -= fl_l + fl_r;
            }
            r = layout_box(c, bx, top, bw, MODE_NORMAL, 0);
            if (ext)
                *ext = MAX(*ext, r.extent);
            cy = top + r.h;
            prev_mb = r.mb;
        }
    }
    cy += MAX(prev_mb, 0);
    // Floats are kept inside their container.
    cy = MAX(cy, MAX(fl_lb, fl_rb));
    return cy - y;
}

// ---- Lists ----

static void roman(int n, bool upper, char *out, size_t size)
{
    static const struct {
        int v;
        const char *s;
    } t[] = { { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" }, { 100, "c" }, { 90, "xc" },
              { 50, "l" }, { 40, "xl" }, { 10, "x" }, { 9, "ix" }, { 5, "v" }, { 4, "iv" }, { 1, "i" } };

    out[0] = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]) && n > 0; i++)
        while (n >= t[i].v) {
            strlcat(out, t[i].s, size);
            n -= t[i].v;
        }
    if (upper)
        for (char *p = out; *p; p++)
            *p = toupper((unsigned char)*p);
}

static int list_number(struct node *li)
{
    struct node *list = li->parent;
    const char *start = list ? node_attr(list, "start") : NULL;
    int n = start ? atoi(start) : 1;
    bool reversed = list && node_attr(list, "reversed");

    if (reversed) {
        int count = 0;

        for (struct node *c = list->first; c; c = c->next)
            if (c->type == NODE_ELEMENT && c->style && c->style->display == D_LIST_ITEM)
                count++;
        n = start ? atoi(start) : count;
    }
    for (struct node *c = list ? list->first : li; c && c != li; c = c->next) {
        if (c->type != NODE_ELEMENT || !c->style || c->style->display != D_LIST_ITEM)
            continue;
        if (node_attr(c, "value"))
            n = atoi(node_attr(c, "value"));
        n += reversed ? -1 : 1;
    }
    if (node_attr(li, "value"))
        n = atoi(node_attr(li, "value"));
    return n;
}

static void marker(struct node *li, int x, int y)
{
    const struct style *s = li->style;
    struct font *f = style_font(s);
    int lh = line_height_of(s), fh = font_line_height(f), half = (lh - fh) / 2;
    char buf[32];
    int n;

    switch (s->list_style) {
    case LS_NONE:
        return;
    case LS_DISC:
    case LS_CIRCLE:
    case LS_SQUARE: {
        int size = MAX(5, s->font_size / 3);

        add((struct item){ .kind = ITEM_BULLET, .r = { x - size - 8, y + half + font_ascent(f) - size - s->font_size / 6,
                                                        size, size },
                           .color = s->color, .radius = s->list_style });
        return;
    }
    default:
        n = list_number(li);
        if (s->list_style == LS_LOWER_ALPHA || s->list_style == LS_UPPER_ALPHA)
            snprintf(buf, sizeof(buf), "%c.", (s->list_style == LS_LOWER_ALPHA ? 'a' : 'A') + (n - 1) % 26);
        else if (s->list_style == LS_LOWER_ROMAN || s->list_style == LS_UPPER_ROMAN) {
            roman(n, s->list_style == LS_UPPER_ROMAN, buf, sizeof(buf) - 1);
            strlcat(buf, ".", sizeof(buf));
        } else
            snprintf(buf, sizeof(buf), "%d.", n);
        {
            const char *t = keep_string(strdup(buf));
            int w = text_width(f, t, -1);

            add((struct item){ .kind = ITEM_TEXT, .r = { x - w - 6, y + half, w, fh }, .color = s->color,
                               .font = f, .text = t, .len = strlen(t) });
        }
    }
}

// ---- Tables ----

struct cell {
    struct node *n;
    int row, col, colspan, rowspan;
    int first, last, h;
    struct boxres r;
};

static int layout_table(struct node *t, int x, int y, int avail, bool fixed_width, int *used_w)
{
    const struct style *ts = t->style;
    struct node **rows = NULL;
    struct cell *cells = NULL;
    int nrows = 0, ncells = 0, ncols = 0, sp = ts->border_collapse ? 0 : ts->border_spacing;
    int *occupied = NULL, nocc = 0, *mins, *maxs, *widths, *rowh, *rowy, W, sum_min = 0, sum_max = 0;
    int cy = y, caption_h = 0;
    const char *cs = node_attr(t, "cellspacing");

    if (cs)
        sp = atoi(cs);
    // Captions go first.
    for (struct node *c = t->first; c; c = c->next)
        if (c->type == NODE_ELEMENT && c->style && c->style->display == D_TABLE_CAPTION) {
            struct boxres r = layout_box(c, x, cy, avail, MODE_NORMAL, 0);

            cy += r.h + r.mt + r.mb;
        }
    caption_h = cy - y;
    // Rows, in order.
    for (struct node *c = t->first; c; c = c->next) {
        if (c->type != NODE_ELEMENT || hidden_box(c->style))
            continue;
        if (c->style->display == D_TABLE_ROW) {
            struct node **m = realloc(rows, (nrows + 1) * sizeof(*m));

            if (m) {
                rows = m;
                rows[nrows++] = c;
            }
        } else if (c->style->display == D_TABLE_ROW_GROUP) {
            for (struct node *r = c->first; r; r = r->next)
                if (r->type == NODE_ELEMENT && !hidden_box(r->style) && r->style->display == D_TABLE_ROW) {
                    struct node **m = realloc(rows, (nrows + 1) * sizeof(*m));

                    if (m) {
                        rows = m;
                        rows[nrows++] = r;
                    }
                }
        }
    }
    // Cells on the grid (rowspan keeps columns busy below).
    for (int r = 0; r < nrows; r++) {
        int col = 0;

        for (struct node *c = rows[r]->first; c; c = c->next) {
            struct cell *m;
            const char *span;

            if (c->type != NODE_ELEMENT || hidden_box(c->style))
                continue;
            while (col < nocc && occupied[col] > r)
                col++;
            if (!(m = realloc(cells, (ncells + 1) * sizeof(*m))))
                break;
            cells = m;
            m = &cells[ncells++];
            memset(m, 0, sizeof(*m));
            m->n = c;
            m->row = r;
            m->col = col;
            span = node_attr(c, "colspan");
            m->colspan = MIN(MAX(span ? atoi(span) : 1, 1), 100);
            span = node_attr(c, "rowspan");
            m->rowspan = MIN(MAX(span ? atoi(span) : 1, 1), nrows - r);
            if (m->rowspan < 1)
                m->rowspan = 1;
            if (col + m->colspan > nocc) {
                int *o = realloc(occupied, (col + m->colspan) * sizeof(int));

                if (o) {
                    for (int k = nocc; k < col + m->colspan; k++)
                        o[k] = 0;
                    occupied = o;
                    nocc = col + m->colspan;
                }
            }
            for (int k = col; k < col + m->colspan && k < nocc; k++)
                occupied[k] = r + m->rowspan;
            col += m->colspan;
            ncols = MAX(ncols, col);
        }
    }
    free(occupied);
    mins = calloc(ncols + 1, sizeof(int));
    maxs = calloc(ncols + 1, sizeof(int));
    widths = calloc(ncols + 1, sizeof(int));
    rowh = calloc(nrows + 1, sizeof(int));
    rowy = calloc(nrows + 1, sizeof(int));
    if (!mins || !maxs || !widths || !rowh || !rowy) {
        free(mins);
        free(maxs);
        free(widths);
        free(rowh);
        free(rowy);
        free(rows);
        free(cells);
        *used_w = 0;
        return caption_h;
    }
    // Column widths from the cells' narrowest and widest layouts.
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ncells; i++) {
            struct cell *c = &cells[i];
            const struct style *s = c->n->style;
            int mn = measure(c->n, false), mx = measure(c->n, true);

            if (s->width.unit == LEN_PX) {
                int fixed = resolve(s->width, 0) + (s->box_sizing_border ? 0 : resolve(s->padding[1], 0)
                            + resolve(s->padding[3], 0) + s->border[1] + s->border[3]);

                mn = MAX(mn, fixed);
                mx = MAX(mn, fixed);
            } else if (s->width.unit == LEN_PCT && !measuring && avail < WIDE / 2) {
                mx = MAX(mn, (int)(s->width.v * avail / 100));
            }
            if (pass == 0 && c->colspan == 1) {
                mins[c->col] = MAX(mins[c->col], mn);
                maxs[c->col] = MAX(maxs[c->col], mx);
            } else if (pass == 1 && c->colspan > 1) {
                int smin = (c->colspan - 1) * sp, smax = smin;

                for (int k = c->col; k < c->col + c->colspan; k++) {
                    smin += mins[k];
                    smax += maxs[k];
                }
                for (int k = c->col; k < c->col + c->colspan; k++) {
                    if (mn > smin)
                        mins[k] += (mn - smin + c->colspan - 1) / c->colspan;
                    if (mx > smax)
                        maxs[k] += (mx - smax + c->colspan - 1) / c->colspan;
                }
            }
        }
    for (int k = 0; k < ncols; k++) {
        maxs[k] = MAX(maxs[k], mins[k]);
        sum_min += mins[k];
        sum_max += maxs[k];
    }
    {
        int spacing = sp * (ncols + 1);

        if (fixed_width)
            W = MAX(avail, sum_min + spacing);
        else
            W = MAX(MIN(sum_max + spacing, avail), sum_min + spacing);
        W -= spacing;
        if (W >= sum_max) {
            int extra = W - sum_max, given = 0;

            for (int k = 0; k < ncols; k++) {
                int e = fixed_width ? (sum_max ? (int)((long)extra * maxs[k] / sum_max) : extra / MAX(ncols, 1)) : 0;

                widths[k] = maxs[k] + e;
                given += e;
            }
            if (fixed_width && ncols)
                widths[ncols - 1] += extra - given;
        } else if (W >= sum_min && sum_max > sum_min) {
            for (int k = 0; k < ncols; k++)
                widths[k] = mins[k] + (int)((long)(maxs[k] - mins[k]) * (W - sum_min) / (sum_max - sum_min));
        } else {
            for (int k = 0; k < ncols; k++)
                widths[k] = mins[k];
        }
        W = spacing;
        for (int k = 0; k < ncols; k++)
            W += widths[k];
    }
    // Rows.
    cy += sp;
    for (int r = 0; r < nrows; r++) {
        const struct style *rs = rows[r]->style;
        int rbg = -1;

        rowy[r] = cy;
        if (rs->background >> 24)
            rbg = add((struct item){ .kind = ITEM_RECT, .color = rs->background, .style = rs });
        for (int i = 0; i < ncells; i++) {
            struct cell *c = &cells[i];
            int cx = x + sp, w = (c->colspan - 1) * sp;

            if (c->row != r)
                continue;
            for (int k = 0; k < c->col; k++)
                cx += widths[k] + sp;
            for (int k = c->col; k < c->col + c->colspan && k < ncols; k++)
                w += widths[k];
            c->first = P->n;
            c->r = layout_box(c->n, cx, cy, w, MODE_EXACT, w);
            c->last = P->n;
            c->h = c->r.h;
            if (c->rowspan == 1)
                rowh[r] = MAX(rowh[r], c->h);
        }
        if (rs->height.unit == LEN_PX)
            rowh[r] = MAX(rowh[r], resolve(rs->height, 0));
        if (rbg >= 0)
            P->items[rbg].r = (struct rect){ x + sp, cy, W - 2 * sp, rowh[r] };
        cy += rowh[r] + sp;
    }
    // Spanning cells may need taller rows.
    for (int i = 0; i < ncells; i++) {
        struct cell *c = &cells[i];
        int last = c->row + c->rowspan - 1, h = (c->rowspan - 1) * sp;

        if (c->rowspan == 1)
            continue;
        for (int r = c->row; r <= last; r++)
            h += rowh[r];
        if (c->h > h) {
            int grow = c->h - h;

            rowh[last] += grow;
            for (int r = last + 1; r < nrows; r++)
                rowy[r] += grow;
            for (int k = 0; k < ncells; k++)
                if (cells[k].row > last)
                    translate(cells[k].first, cells[k].last, 0, grow);
            cy += grow;
        }
    }
    // Cells fill their rows; their content is aligned in them.
    for (int i = 0; i < ncells; i++) {
        struct cell *c = &cells[i];
        const struct style *s = c->n->style;
        int h = (c->rowspan - 1) * sp, dy = 0;

        for (int r = c->row; r < c->row + c->rowspan; r++)
            h += rowh[r];
        if (s->vertical_align == VA_MIDDLE || s->vertical_align == VA_BASELINE)
            dy = (h - c->h) / 2;
        else if (s->vertical_align == VA_BOTTOM)
            dy = h - c->h;
        if (s->vertical_align == VA_BASELINE && !node_attr(c->n, "valign"))
            dy = s->display == D_TABLE_CELL && s->vertical_align == VA_BASELINE ? 0 : dy;
        if (dy > 0)
            translate(c->first, c->last, 0, dy);
        if (c->r.bg >= 0) {
            P->items[c->r.bg].r.y = rowy[c->row];
            P->items[c->r.bg].r.h = h;
        }
        if (c->r.bd >= 0) {
            P->items[c->r.bd].r.y = rowy[c->row];
            P->items[c->r.bd].r.h = h;
        }
    }
    free(mins);
    free(maxs);
    free(widths);
    free(rowh);
    free(rowy);
    free(rows);
    free(cells);
    *used_w = W;
    return cy - y;
}

// ---- Flex ----

struct flex_item {
    struct node *n;
    int basis, min, size, ml, mr, first, last, h, mt, mb;
    float grow, shrink;
    struct boxres r;
};

static int by_order(const void *a, const void *b)
{
    const struct flex_item *x = a, *y = b;
    int ox = x->n->type == NODE_ELEMENT ? x->n->style->order : 0, oy = y->n->type == NODE_ELEMENT ? y->n->style->order : 0;

    return ox != oy ? ox - oy : x->first - y->first;
}

// Lays out one flex item (an element or a bit of text) at x, y.
static struct boxres flex_layout_item(struct node *n, const struct style *cs, int x, int y, int w, int mode)
{
    if (n->type == NODE_TEXT) {
        struct boxres r = { .bg = -1, .bd = -1 };

        r.extent = x;
        r.h = inline_run(n, n->next, x, y, w, cs, &r.extent, 0) - y;
        r.w = w;
        return r;
    }
    return layout_box(n, x, y, w, mode, w);
}

static int layout_flex(struct node *fx, int x, int y, int w, int *ext)
{
    const struct style *cs = fx->style;
    struct flex_item *items = NULL;
    int n = 0, gap = resolve(cs->gap, w), cy = y;

    for (struct node *c = fx->first; c; c = c->next) {
        struct flex_item *m;

        if (c->type == NODE_TEXT ? is_whitespace_text(c) : hidden_box(c->style))
            continue;
        if (!(m = realloc(items, (n + 1) * sizeof(*m))))
            break;
        items = m;
        memset(&items[n], 0, sizeof(*items));
        items[n].n = c;
        items[n].first = n;     // for a stable sort
        n++;
    }
    qsort(items, n, sizeof(*items), by_order);
    if (cs->flex_column) {
        for (int i = 0; i < n; i++) {
            struct node *c = items[i].n;
            const struct style *s = c->type == NODE_ELEMENT ? c->style : cs;
            bool stretch = cs->align_items == AI_STRETCH && !has_len(s->width);
            int first = P->n, mt = c->type == NODE_ELEMENT ? resolve(s->margin[0], w) : 0;
            struct boxres r = flex_layout_item(c, cs, x, cy + mt, w, stretch ? MODE_NORMAL : MODE_SHRINK);

            if (!stretch && (cs->align_items == AI_CENTER || cs->align_items == AI_END)) {
                int dx = cs->align_items == AI_CENTER ? (w - r.w) / 2 : w - r.w;

                if (dx > 0)
                    translate(first, P->n, dx, 0);
                r.extent += MAX(dx, 0);
            }
            if (ext)
                *ext = MAX(*ext, r.extent);
            cy += mt + r.h + r.mb + (i + 1 < n ? gap : 0);
        }
        free(items);
        return cy - y;
    }
    // Sizes before growing and shrinking.
    for (int i = 0; i < n; i++) {
        struct flex_item *it = &items[i];
        const struct style *s = it->n->type == NODE_ELEMENT ? it->n->style : NULL;

        it->grow = s ? s->flex_grow : 0;
        it->shrink = s ? s->flex_shrink : 1;
        it->ml = s ? resolve(s->margin[3], w) : 0;
        it->mr = s ? resolve(s->margin[1], w) : 0;
        it->min = measure(it->n, false) - it->ml - it->mr;
        if (s && has_len(s->flex_basis) && !(s->flex_basis.unit == LEN_PX && s->flex_basis.v == 0 && measuring))
            it->basis = resolve(s->flex_basis, w);
        else if (s && has_len(s->width))
            it->basis = resolve(s->width, w) + (s->box_sizing_border ? 0 : resolve(s->padding[1], w)
                        + resolve(s->padding[3], w) + s->border[1] + s->border[3]);
        else
            it->basis = measure(it->n, true) - it->ml - it->mr;
        if (s && (has_len(s->width) || s->overflow_hidden))
            it->min = MIN(it->min, it->basis);
        if (s && has_len(s->min_width))
            it->min = MAX(it->min, resolve(s->min_width, w));
        it->basis = MAX(it->basis, s && has_len(s->min_width) ? resolve(s->min_width, w) : 0);
        if (s && has_len(s->max_width))
            it->basis = MIN(it->basis, resolve(s->max_width, w));
        it->size = it->basis;
    }
    for (int start = 0; start < n;) {
        int end = start, used = 0, free_space, line_h = 0, off = 0, between = gap;
        float grow = 0, shrink = 0;

        // Items on this line.
        while (end < n) {
            int add_w = items[end].basis + items[end].ml + items[end].mr + (end > start ? gap : 0);

            if (cs->flex_wrap && end > start && used + add_w > w)
                break;
            used += add_w;
            end++;
        }
        free_space = w - used;
        for (int i = start; i < end; i++) {
            grow += items[i].grow;
            shrink += items[i].shrink * items[i].basis;
        }
        if (free_space > 0 && grow > 0) {
            for (int i = start; i < end; i++)
                items[i].size = items[i].basis + (int)(free_space * items[i].grow / grow);
            free_space = 0;
        } else if (free_space < 0 && shrink > 0) {
            for (int i = start; i < end; i++) {
                int cut = (int)(free_space * items[i].shrink * items[i].basis / shrink);

                items[i].size = MAX(items[i].basis + cut, items[i].min);
            }
            free_space = 0;
        }
        if (free_space > 0) {
            int k = end - start;

            switch (cs->justify) {
            case JC_CENTER:
                off = free_space / 2;
                break;
            case JC_END:
                off = free_space;
                break;
            case JC_BETWEEN:
                between += k > 1 ? free_space / (k - 1) : 0;
                break;
            case JC_AROUND:
                off = free_space / (2 * k);
                between += free_space / k;
                break;
            case JC_EVENLY:
                off = free_space / (k + 1);
                between += free_space / (k + 1);
                break;
            }
        }
        // Lay them out side by side.
        {
            int cx = x + off;

            for (int i = start; i < end; i++) {
                struct flex_item *it = &items[i];
                const struct style *s = it->n->type == NODE_ELEMENT ? it->n->style : NULL;

                it->mt = s ? resolve(s->margin[0], w) : 0;
                it->first = P->n;
                it->r = flex_layout_item(it->n, cs, cx, cy + it->mt, MAX(it->size, 0), MODE_EXACT);
                if (s)
                    translate(it->first, P->n, it->ml, 0);
                it->last = P->n;
                it->h = it->r.h;
                it->mb = it->r.mb;
                line_h = MAX(line_h, it->mt + it->h + it->mb);
                if (ext)
                    *ext = MAX(*ext, cx + it->ml + it->size + it->mr);
                cx += it->ml + it->size + it->mr + between;
            }
        }
        for (int i = start; i < end; i++) {
            struct flex_item *it = &items[i];
            const struct style *s = it->n->type == NODE_ELEMENT ? it->n->style : NULL;
            int outer = it->mt + it->h + it->mb, dy = 0;
            int align = s && s->align_self ? s->align_self - 1 : cs->align_items;

            if (align == AI_CENTER)
                dy = (line_h - outer) / 2;
            else if (align == AI_END)
                dy = line_h - outer;
            if (dy > 0)
                translate(it->first, it->last, 0, dy);
            if (align == AI_STRETCH && s && s->height.unit == LEN_AUTO) {
                int h = line_h - it->mt - it->mb;

                if (it->r.bg >= 0)
                    P->items[it->r.bg].r.h = h;
                if (it->r.bd >= 0)
                    P->items[it->r.bd].r.h = h;
            }
        }
        cy += line_h + (end < n ? resolve(cs->gap, w) : 0);
        start = end;
    }
    free(items);
    return cy - y;
}

// ---- Boxes ----

static int measure_text(struct node *t, bool widest)
{
    const struct style *s = t->parent && t->parent->style ? t->parent->style : NULL;
    struct font *f;
    int best = 0, run = 0, sw;
    const char *p = t->text;

    if (!s)
        return 0;
    f = style_font(s);
    sw = text_width(f, " ", 1);
    while (*p) {
        const char *w;

        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p)) {
                if (*p == '\n' && s->white_space != WS_NORMAL && s->white_space != WS_NOWRAP)
                    run = 0;
                p++;
            }
            if (run)
                run += sw;
            continue;
        }
        w = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (widest || s->white_space == WS_NOWRAP || s->white_space == WS_PRE) {
            run += text_width(f, w, p - w);
            best = MAX(best, run);
        } else {
            best = MAX(best, text_width(f, w, p - w));
        }
    }
    return best;
}

static int measure(struct node *n, bool widest)
{
    int saved = P->n, v;
    struct node *link = cur_link;
    struct boxres r;

    if (n->type == NODE_TEXT)
        return measure_text(n, widest);
    if (!n->style || hidden_box(n->style))
        return 0;
    if (n->mgen == generation && (widest ? n->mmax : n->mmin) >= 0)
        return widest ? n->mmax : n->mmin;
    if (n->mgen != generation) {
        n->mgen = generation;
        n->mmin = n->mmax = -1;
    }
    measuring++;
    r = layout_box(n, 0, 0, widest ? WIDE : 0, MODE_FILL, 0);
    measuring--;
    P->n = saved;
    cur_link = link;
    v = MAX(r.extent, 0);
    if (widest)
        n->mmax = v;
    else
        n->mmin = v;
    return v;
}

static struct boxres layout_box(struct node *n, int x, int y, int avail, int mode, int exact)
{
    const struct style *s = n->style;
    struct boxres res = { .bg = -1, .bd = -1 };
    int ml = resolve(s->margin[3], avail), mr = resolve(s->margin[1], avail);
    int pt = resolve(s->padding[0], avail), pr = resolve(s->padding[1], avail);
    int pb = resolve(s->padding[2], avail), pl = resolve(s->padding[3], avail);
    int bt = s->border[0], br = s->border[1], bb = s->border[2], bl = s->border[3];
    int frame = pl + pr + bl + br, cw, bw, h, start = P->n, ext, cx, cy;
    bool auto_ml = s->margin[3].unit == LEN_AUTO, auto_mr = s->margin[1].unit == LEN_AUTO;
    bool shrink = mode == MODE_SHRINK || (mode == MODE_NORMAL && (s->float_ || s->display == D_INLINE_BLOCK));
    bool sized = false, is_table = s->display == D_TABLE;
    struct node *saved_link = cur_link;

    res.mt = resolve(s->margin[0], avail);
    res.mb = resolve(s->margin[2], avail);
    // Width.
    if (mode == MODE_EXACT) {
        bw = exact;
        cw = bw - frame;
        ml = mr = 0;
        sized = true;
    } else if (has_len(s->width)) {
        cw = resolve(s->width, avail) - (s->box_sizing_border ? frame : 0);
        sized = true;
    } else if (shrink && !is_table && mode != MODE_FILL) {
        int mn = measure(n, false), mx = measure(n, true);

        cw = MIN(MAX(mn, avail), mx) - ml - mr - frame;
        sized = true;
    } else {
        cw = avail - ml - mr - frame;
    }
    if (has_len(s->max_width))
        cw = MIN(cw, resolve(s->max_width, avail) - (s->box_sizing_border ? frame : 0));
    if (has_len(s->min_width))
        cw = MAX(cw, resolve(s->min_width, avail) - (s->box_sizing_border ? frame : 0));
    cw = MAX(cw, 0);
    bw = cw + frame;
    if ((sized || has_len(s->max_width)) && mode != MODE_EXACT && !shrink) {
        int left = avail - bw;

        if (auto_ml && auto_mr) {
            ml = mr = MAX(left / 2, 0);
        } else if (auto_ml) {
            ml = MAX(left - mr, 0);
        }
    }
    if (s->background >> 24 || s->background_image)
        res.bg = add((struct item){ .kind = ITEM_RECT, .color = s->background, .radius = s->radius, .style = s,
                                    .image = s->background_image });
    if (bt || br || bb || bl)
        res.bd = add((struct item){ .kind = ITEM_BORDER, .radius = s->radius,
                                    .border = { bt, br, bb, bl },
                                    .border_color = { s->border_color[0], s->border_color[1], s->border_color[2],
                                                      s->border_color[3] },
                                    .border_style = { s->border_style[0], s->border_style[1], s->border_style[2],
                                                      s->border_style[3] } });
    if ((node_is(n, "a") && node_attr(n, "href")) || node_is(n, "button"))
        cur_link = n;
    anchor(n, y);
    cx = x + ml + bl + pl;
    cy = y + bt + pt;
    ext = cx;
    if (is_table) {
        int used;

        h = layout_table(n, cx, cy, cw, has_len(s->width) || mode == MODE_EXACT, &used);
        if (!has_len(s->width) && mode != MODE_EXACT) {
            cw = used;
            bw = cw + frame;
            if (auto_ml && auto_mr && mode != MODE_FILL) {
                int dx = MAX((avail - bw) / 2, 0);

                translate(start, P->n, dx, 0);
                ml += dx;
                cx += dx;
            }
        }
        ext = cx + cw;
    } else if (s->display == D_FLEX || s->display == D_INLINE_FLEX) {
        h = layout_flex(n, cx, cy, cw, &ext);
    } else if (node_is(n, "img") || node_is(n, "input") || node_is(n, "select") || node_is(n, "textarea")) {
        // Replaced elements made blocks.
        struct line L = { .x0 = cx, .width = cw, .y = cy, .cs = s, .ext = cx, .bg_item = -1 };
        struct style copy = *s;

        copy.display = D_INLINE_BLOCK;
        if (sized || mode == MODE_EXACT)
            copy.width = (struct len){ cw, LEN_PX };
        copy.margin[0] = copy.margin[1] = copy.margin[2] = copy.margin[3] = (struct len){ 0, LEN_PX };
        copy.border[0] = copy.border[1] = copy.border[2] = copy.border[3] = 0;
        copy.float_ = FLOAT_NONE;
        n->style = &copy;
        if (res.bd >= 0)
            P->items[res.bd].kind = ITEM_RECT, P->items[res.bd].color = 0;
        place_node(&L, n);
        finish_line(&L, false);
        free(L.pieces);
        n->style = (struct style *)s;
        h = L.y - cy;
        ext = L.ext;
        if (!sized && !has_len(s->width)) {
            // Shrink to the picture.
            cw = MIN(cw, L.ext - cx);
            bw = cw + frame;
        }
    } else {
        if (s->display == D_LIST_ITEM)
            marker(n, cx, cy);
        h = layout_children(n, cx, cy, cw, &ext);
    }
    if (s->height.unit == LEN_PX)
        h = resolve(s->height, 0) - (s->box_sizing_border ? pt + pb + bt + bb : 0);
    if (s->min_height.unit == LEN_PX)
        h = MAX(h, resolve(s->min_height, 0) - (s->box_sizing_border ? pt + pb + bt + bb : 0));
    if (s->max_height.unit == LEN_PX)
        h = MIN(h, resolve(s->max_height, 0) - (s->box_sizing_border ? pt + pb + bt + bb : 0));
    h = MAX(h, 0);
    res.h = bt + pt + h + pb + bb;
    if (res.bg >= 0)
        P->items[res.bg].r = (struct rect){ x + ml, y, bw, res.h };
    if (res.bd >= 0)
        P->items[res.bd].r = (struct rect){ x + ml, y, bw, res.h };
    if (s->overflow_hidden && (s->height.unit == LEN_PX || s->max_height.unit == LEN_PX || has_len(s->width))) {
        struct rect clip = { x + ml, y, bw, res.h };

        for (int i = start; i < P->n; i++) {
            struct item *it = &P->items[i];

            if (it->clipped)
                rect_intersect(it->clip, clip, &it->clip);
            else
                it->clip = clip;
            it->clipped = true;
        }
        ext = MIN(ext, x + ml + bw);
    }
    if (s->position == POS_RELATIVE && !measuring) {
        int dx = has_len(s->left) ? resolve(s->left, avail) : 0, dy = s->top.unit == LEN_PX ? resolve(s->top, 0) : 0;

        translate(start, P->n, dx, dy);
    }
    if (s->visibility_hidden && !measuring)
        P->n = start;
    cur_link = saved_link;
    res.w = ml + bw + mr;
    if (sized || mode == MODE_EXACT)
        res.extent = x + ml + bw + mr;
    else
        res.extent = MAX(ext, cx) + pr + br + mr;
    if (s->float_ || mode == MODE_SHRINK)
        res.extent = MAX(res.extent, x + ml + bw + mr);
    if (res.bg >= 0 && P->n <= res.bg)
        res.bg = -1;
    if (res.bd >= 0 && P->n <= res.bd)
        res.bd = -1;
    return res;
}

// ---- The page ----

void layout_free(struct page_layout *pl)
{
    for (int i = 0; i < pl->nstrings; i++)
        free(pl->strings[i]);
    free(pl->strings);
    free(pl->anchors);
    free(pl->items);
    memset(pl, 0, sizeof(*pl));
}

void layout_page(struct page_layout *pl, struct node *root, int width, int viewport_height)
{
    struct node *html = node_find(root, "html"), *body = node_find(root, "body");
    int ext = 0;

    layout_free(pl);
    P = pl;
    generation++;
    measuring = 0;
    cur_link = NULL;
    css_viewport_width = width;
    css_viewport_height = viewport_height;
    pl->background = RGB(0xFFFFFF);
    // The canvas takes the root's background, or the body's.
    if (html && html->style && (html->style->background >> 24 || html->style->background_image)) {
        pl->background = html->style->background;
        pl->background_style = html->style;
    } else if (body && body->style && (body->style->background >> 24 || body->style->background_image)) {
        pl->background = body->style->background;
        pl->background_style = body->style;
    }
    if (!(pl->background >> 24))
        pl->background = RGB(0xFFFFFF);
    pl->height = layout_children(root, 0, 0, width, &ext);
    pl->width = MAX(ext, width);
    pl->height = MAX(pl->height, 1);
    P = NULL;
}
