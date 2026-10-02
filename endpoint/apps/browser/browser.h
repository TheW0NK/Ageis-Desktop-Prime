#ifndef BROWSER_H
#define BROWSER_H

#include "aegis.h"
#include "gfx.h"

// The web browser: an HTML parser (html.c), CSS (css.c), layout into a
// display list (layout.c), HTTP and files (net.c), and the window (main.c).

// ---- Documents ----

enum { NODE_DOCUMENT, NODE_ELEMENT, NODE_TEXT };

struct attr {
    char *name, *value;
};

struct style;

struct node {
    int type;
    char *tag;                      // lower case (elements)
    struct attr *attrs;
    int nattrs;
    char *text;                     // text nodes
    struct node *parent, *first, *last, *next, *prev;
    struct style *style;            // computed (elements)
    // Form controls.
    char *value;
    bool checked;
    int selected;                   // <select>: the chosen <option>
    // Images.
    struct surface *image;
    int image_state;                // 0 not asked, 1 loading, 2 loaded, 3 failed
    // Intrinsic widths cached during one layout.
    int mgen, mmin, mmax;
};

struct node *html_parse(const char *src, size_t len);
void node_free(struct node *n);
const char *node_attr(const struct node *n, const char *name);
bool node_is(const struct node *n, const char *tag);
// Depth-first: the next node after n inside root (NULL at the end).
struct node *node_walk(struct node *n, const struct node *root);
// The first element with this tag.
struct node *node_find(struct node *root, const char *tag);
// All the text inside n, with spaces collapsed (malloc'd).
char *node_text(const struct node *n);

// ---- Styles ----

enum { LEN_AUTO, LEN_PX, LEN_PCT };

struct len {
    float v;
    uint8_t unit;
};

enum {
    D_NONE, D_INLINE, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE, D_TABLE_ROW_GROUP,
    D_TABLE_ROW, D_TABLE_CELL, D_FLEX, D_INLINE_FLEX, D_TABLE_CAPTION,
};
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_JUSTIFY };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PRE_WRAP };
enum { FAM_SANS, FAM_SERIF, FAM_MONO };
enum { DEC_UNDERLINE = 1, DEC_LINE_THROUGH = 2, DEC_OVERLINE = 4 };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { LS_NONE, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA, LS_LOWER_ROMAN,
       LS_UPPER_ROMAN };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FLOAT_NONE, FLOAT_LEFT, FLOAT_RIGHT };
enum { VA_BASELINE, VA_MIDDLE, VA_TOP, VA_BOTTOM, VA_SUPER, VA_SUB };
enum { JC_START, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH, AI_START, AI_CENTER, AI_END, AI_BASELINE };
enum { BS_NONE, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_INSET, BS_OUTSET };

struct style {
    uint8_t display, text_align, white_space, family, decoration, transform, list_style, position,
            float_, vertical_align, justify, align_items, flex_column, flex_wrap, visibility_hidden,
            border_style[4], box_sizing_border, overflow_hidden,
            align_self,                     // 0 auto, else AI_* + 1
            clear;                          // 1 left, 2 right, 3 both
    bool bold, italic;
    color_t color, background;
    color_t border_color[4];
    int font_size;                  // px
    float line_height;              // px; 0: normal
    struct len margin[4], padding[4], width, height, min_width, max_width, min_height, max_height,
               text_indent, gap, top, left;
    int border[4];                  // px (0 when the style is none)
    int radius;
    float flex_grow, flex_shrink;
    struct len flex_basis;
    int order;
    float lh_factor;                // line-height given as a number (or normal: 1.2); 0 for lengths
    color_t outline_color;
    struct surface *background_image;   // not owned
    char *background_url;
    uint8_t bg_repeat, bg_size;     // BG_*
    int border_spacing;
    bool border_collapse;
    struct len bg_x, bg_y;
    struct cssvar *vars;            // custom properties (own ones first, then the parent's)
    int nvars;                      // how many of vars are this element's
};

enum { BG_REPEAT, BG_NO_REPEAT, BG_REPEAT_X, BG_REPEAT_Y };
enum { BG_SIZE_AUTO, BG_SIZE_COVER, BG_SIZE_CONTAIN };

struct cssvar {
    char *name, *value;
    struct cssvar *next;
};

// The viewport, for vw/vh units and media queries.
extern int css_viewport_width, css_viewport_height;
void style_free(struct style *s);

struct sheet;

struct sheet *css_parse(const char *src, int origin);
void css_free(struct sheet *s);
// The user agent's style sheet.
struct sheet *css_default(void);
// Computes n->style for every element under root from the sheets.
void css_apply(struct node *root, struct sheet **sheets, int nsheets);
// Parses one colour; false if it is not one.
bool css_color(const char *v, color_t *out);
struct font *style_font(const struct style *s);

// ---- Layout ----

enum { ITEM_RECT, ITEM_BORDER, ITEM_TEXT, ITEM_IMAGE, ITEM_BULLET, ITEM_CONTROL, ITEM_RULE };

struct item {
    uint8_t kind;
    struct rect r;
    color_t color;
    struct font *font;
    const char *text;               // ITEM_TEXT: into a text node, or a static/owned string
    int len;
    uint8_t decoration;
    int radius;
    int border[4];
    color_t border_color[4];
    uint8_t border_style[4];
    struct node *node;              // what this belongs to (controls, images)
    struct node *link;              // the <a> it is inside
    struct surface *image;
    const struct style *style;      // backgrounds: the box's style
    struct rect clip;               // when clipped (overflow: hidden)
    bool clipped;
};

struct page_layout {
    struct item *items;
    int n, cap;
    int width, height;              // the whole page
    color_t background;
    const struct style *background_style;   // the root's, for a background image
    char **strings;                 // text made during layout (transformed, numbers)
    int nstrings;
    struct anchor {
        struct node *node;
        int y;
    } *anchors;                     // elements with an id or name, for #fragments
    int nanchors;
};

void layout_page(struct page_layout *pl, struct node *root, int width, int viewport_height);
void layout_free(struct page_layout *pl);
// Draws the part of the page at scroll offset y into r.
void paint_page(struct page_layout *pl, struct gfx *g, struct rect r, int scroll_x, int scroll_y,
                struct node *focus, struct node *hover_link, int caret);

// Form controls (paint.c).
struct node *select_option(struct node *sel, int index);
int select_current(struct node *sel);
const char *control_value(struct node *n);

// ---- Network ----

struct response {
    int status;                     // HTTP status; 0 for files
    char *type;                     // "text/html", "image/png", ...
    char *data;
    size_t len;
    char *url;                      // where it finally came from (after redirects)
    char *error;                    // non-NULL if it failed
};

// Fetches a URL: http, https, file and data. post (may be NULL) is sent as
// an application/x-www-form-urlencoded body. Blocks.
struct response *net_fetch(const char *url, const char *post);
void response_free(struct response *r);
// Resolves href against base (malloc'd).
char *url_resolve(const char *base, const char *href);
// Turns what the user typed into a URL (malloc'd).
char *url_from_input(const char *typed);
// Appends text to a form body, percent-encoded.
void url_encode_append(char **buf, size_t *len, size_t *cap, const char *text);

#endif
