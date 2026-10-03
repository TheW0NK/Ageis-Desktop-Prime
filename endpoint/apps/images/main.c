#include "aegis.h"
#include "ui.h"

// Image Viewer.

static const char page[] =
    "<window title='Image Viewer' width='900' height='640' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button flat='true' symbol='back' onclick='prev' shortcut='Left'/>"
    "    <button flat='true' symbol='forward' onclick='next' shortcut='Right'/>"
    "    <separator/>"
    "    <button flat='true' text='Open...' onclick='open' shortcut='Ctrl+O'/>"
    "    <separator/>"
    "    <button flat='true' text='&#x2212;' onclick='zoomout' shortcut='Minus'/>"
    "    <label id='zoom' text='Fit' width='60' textalign='center' align='center'/>"
    "    <button flat='true' text='+' onclick='zoomin' shortcut='Equal'/>"
    "    <button flat='true' text='Fit' onclick='fit' shortcut='F'/>"
    "    <button flat='true' text='1:1' onclick='actual' shortcut='1'/>"
    "    <separator/>"
    "    <button flat='true' symbol='restart' onclick='rotate' shortcut='R'/>"
    "    <spacer/>"
    "    <button flat='true' text='Set as background' onclick='background'/>"
    "  </toolbar>"
    "  <canvas id='view' focusable='true'/>"
    "  <statusbar><label id='info'/><spacer/><label id='pos' dim='true'/></statusbar>"
    "</window>";

static struct ui_window *win;
static struct widget *view;
static struct surface *img;
static char path[512];
static float zoom;                  // 0: fit to window
static float px, py;                // pan offset in screen pixels
static bool dragging;
static int drag_x, drag_y;
static char **files;
static int nfiles, index_in_dir = -1;

static bool is_image(const char *name)
{
    static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", NULL };
    const char *dot = strrchr(name, '.');

    for (int i = 0; dot && exts[i]; i++)
        if (!strcasecmp(dot, exts[i]))
            return true;
    return false;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(void)
{
    char dir[512], *slash;
    struct dir_stream *d;
    struct aegis_dirent *e;
    const char *base;

    for (int i = 0; i < nfiles; i++)
        free(files[i]);
    free(files);
    files = NULL;
    nfiles = 0;
    index_in_dir = -1;
    strlcpy(dir, path, sizeof(dir));
    if (!(slash = strrchr(dir, '/')))
        return;
    *slash = 0;
    base = strrchr(path, '/') + 1;
    if (!(d = opendir(*dir ? dir : "/")))
        return;
    while ((e = readdir(d))) {
        char **n;

        if (!is_image(e->name) || !(n = realloc(files, (nfiles + 1) * sizeof(char *))))
            continue;
        files = n;
        files[nfiles] = malloc(strlen(dir) + strlen(e->name) + 2);
        if (files[nfiles]) {
            snprintf(files[nfiles], strlen(dir) + strlen(e->name) + 2, "%s/%s", dir, e->name);
            nfiles++;
        }
    }
    closedir(d);
    qsort(files, nfiles, sizeof(char *), by_name);
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(strrchr(files[i], '/') + 1, base))
            index_in_dir = i;
}

static float scale(struct rect r)
{
    if (!img)
        return 1;
    if (zoom > 0)
        return zoom;
    return MIN(1.0f, MIN((float)(r.w - 20) / img->width, (float)(r.h - 20) / img->height));
}

static void update_info(void)
{
    char buf[700], size[32], z[16];
    struct aegis_stat st;

    if (!img) {
        ui_set_text(ui_get(win, "info"), "No picture open. Use Open to choose one.");
        ui_set_text(ui_get(win, "pos"), "");
        return;
    }
    if (stat(path, &st) == 0)
        ui_format_size(st.size, size, sizeof(size));
    else
        size[0] = 0;
    snprintf(buf, sizeof(buf), "%d \xC3\x97 %d pixels   %s", img->width, img->height, size);
    ui_set_text(ui_get(win, "info"), buf);
    if (nfiles)
        snprintf(buf, sizeof(buf), "%d of %d", index_in_dir + 1, nfiles);
    else
        buf[0] = 0;
    ui_set_text(ui_get(win, "pos"), buf);
    snprintf(z, sizeof(z), "%.0f%%", scale(ui_rect(view)) * 100);
    ui_set_text(ui_get(win, "zoom"), zoom > 0 ? z : "Fit");
    {
        const char *base = strrchr(path, '/');

        snprintf(buf, sizeof(buf), "%s - Image Viewer", base ? base + 1 : path);
        ui_window_set_title(win, buf);
    }
}

static bool load(const char *p)
{
    struct surface *s = image_load(p);
    char msg[700];

    if (!s) {
        snprintf(msg, sizeof(msg), "\"%s\" is not a picture this viewer can show.", p);
        ui_message(win, "Image Viewer", msg, "OK");
        return false;
    }
    surface_destroy(img);
    img = s;
    if (p != path)
        strlcpy(path, p, sizeof(path));
    zoom = 0;
    px = py = 0;
    update_info();
    ui_redraw(view);
    return true;
}

static void paint(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    float s;
    int dw, dh;

    (void)w;
    (void)u;
    // A checkerboard shows transparency.
    gfx_fill(g, r, RGB(0x2A2E35));
    if (!img) {
        struct font *f = font_get(FONT_SANS, 16);
        const char *t = "Open a picture to view it here";

        icon_draw_glyph(g, "image", (struct rect){ r.x + r.w / 2 - 48, r.y + r.h / 2 - 80, 96, 96 },
                        RGB(0x5A6270));
        text_draw(g, f, r.x + (r.w - text_width(f, t, -1)) / 2, r.y + r.h / 2 + 24, t, -1, RGB(0x9AA4B2));
        return;
    }
    s = scale(r);
    dw = (int)(img->width * s + 0.5f);
    dh = (int)(img->height * s + 0.5f);
    {
        struct rect dst = { r.x + (r.w - dw) / 2 + (int)px, r.y + (r.h - dh) / 2 + (int)py, dw, dh };
        struct rect clip;

        if (rect_intersect(dst, r, &clip))
            for (int y = clip.y; y < clip.y + clip.h; y += 12)
                for (int x = clip.x; x < clip.x + clip.w; x += 12)
                    gfx_fill(g, (struct rect){ x, y, MIN(12, clip.x + clip.w - x), MIN(12, clip.y + clip.h - y) },
                             ((x - dst.x) / 12 + (y - dst.y) / 12) % 2 ? RGB(0x9A9A9A) : RGB(0xC8C8C8));
        if (dw == img->width && dh == img->height)
            gfx_blit(g, img, (struct rect){ 0, 0, img->width, img->height }, dst.x, dst.y);
        else
            gfx_blit_scaled(g, img, (struct rect){ 0, 0, img->width, img->height }, dst);
    }
}

static void set_zoom(float z)
{
    zoom = MIN(MAX(z, 0.05f), 16.0f);
    update_info();
    ui_redraw(view);
}

static void input(struct widget *w, struct wm_event *ev, void *u)
{
    (void)u;
    if (ev->type != WM_EV_POINTER)
        return;
    switch (ev->kind) {
    case WM_PTR_WHEEL:
        if (img)
            set_zoom(scale(ui_rect(w)) * (ev->detail > 0 ? 1.15f : 1 / 1.15f));
        break;
    case WM_PTR_DOWN:
        ui_focus(w);
        if (ev->detail == BTN_LEFT && ui_click_count(w) == 2) {
            zoom = zoom > 0 ? 0 : 1;
            px = py = 0;
            update_info();
            ui_redraw(w);
        } else if (ev->detail == BTN_LEFT) {
            dragging = true;
            drag_x = ev->x;
            drag_y = ev->y;
        }
        break;
    case WM_PTR_MOVE:
        if (dragging) {
            px += ev->x - drag_x;
            py += ev->y - drag_y;
            drag_x = ev->x;
            drag_y = ev->y;
            ui_redraw(w);
        }
        break;
    case WM_PTR_UP:
        dragging = false;
        break;
    }
}

static void step(int d)
{
    if (!nfiles)
        return;
    index_in_dir = (index_in_dir + d + nfiles) % nfiles;
    load(files[index_in_dir]);
}

static void on_prev(struct widget *w, void *u) { (void)w; (void)u; step(-1); }
static void on_next(struct widget *w, void *u) { (void)w; (void)u; step(1); }
static void on_zoomin(struct widget *w, void *u) { (void)w; (void)u; set_zoom(scale(ui_rect(view)) * 1.25f); }
static void on_zoomout(struct widget *w, void *u) { (void)w; (void)u; set_zoom(scale(ui_rect(view)) / 1.25f); }

static void on_fit(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    zoom = 0;
    px = py = 0;
    update_info();
    ui_redraw(view);
}

static void on_actual(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    px = py = 0;
    set_zoom(1);
}

static void on_rotate(struct widget *w, void *u)
{
    struct surface *r;

    (void)w;
    (void)u;
    if (!img || !(r = surface_create(img->height, img->width)))
        return;
    // A quarter turn clockwise.
    for (int y = 0; y < img->height; y++)
        for (int x = 0; x < img->width; x++)
            r->pixels[x * r->stride + (img->height - 1 - y)] = img->pixels[y * img->stride + x];
    surface_destroy(img);
    img = r;
    update_info();
    ui_redraw(view);
}

static void on_open(struct widget *w, void *u)
{
    char *p = ui_file_dialog_filtered(win, "Open a picture", NULL, false, NULL,
                                      "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga");

    (void)w;
    (void)u;
    if (!p)
        return;
    if (load(p))
        scan_dir();
    update_info();
    free(p);
}

static void on_background(struct widget *w, void *u)
{
    struct user_info me;

    (void)w;
    (void)u;
    if (!img || user_current(&me) < 0)
        return;
    user_setting_set(&me, "background", path);
    wm_setting_changed("background", path);
}

// A picture dropped on the window opens in it.
static bool open_dropped(const char *p, void *u)
{
    (void)u;
    if (load(p))
        scan_dir();
    return false;
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "prev", on_prev }, { "next", on_next }, { "open", on_open }, { "zoomin", on_zoomin },
        { "zoomout", on_zoomout }, { "fit", on_fit }, { "actual", on_actual }, { "rotate", on_rotate },
        { "background", on_background }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "images")))
        return 1;
    view = ui_get(win, "view");
    ui_canvas_set(view, paint, input, NULL);
    ui_accept_files(ui_root(win), open_dropped, NULL);
    if (argc > 1 && load(argv[1]))
        scan_dir();
    update_info();
    ui_window_show(win);
    ui_focus(view);
    return ui_run();
}
