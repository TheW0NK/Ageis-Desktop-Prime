#include "aegis.h"
#include "ui.h"

// Camera: a live picture from /osystem/devices/camera, photos saved to ~/Images, a
// self-timer, colour effects and a strip of the latest photos.

static const char window_aui[] =
    "<window title='Camera' width='860' height='660' padding='0' spacing='0'>"
    "  <canvas id='view' expand='true' focusable='true'/>"
    "  <hbox padding='10' spacing='10'>"
    "    <label text='Effect'/>"
    "    <dropdown id='effect' width='120' onchange='effect'><option>None</option><option>Mono</option>"
    "      <option>Sepia</option><option>Cool</option><option>Warm</option><option>Invert</option></dropdown>"
    "    <label text='Timer'/>"
    "    <dropdown id='timer' width='100'><option>Off</option><option>3 seconds</option>"
    "      <option>10 seconds</option></dropdown>"
    "    <checkbox id='mirror' text='Mirror' checked='true' onchange='mirror'/>"
    "    <spacer/>"
    "    <button id='shoot' text='Take photo' default='true' onclick='shoot' shortcut='Space'/>"
    "  </hbox>"
    "  <hbox id='strip' padding='10' spacing='8' height='92'/>"
    "  <statusbar><label id='status' expand='true'/><link text='Open Images' onclick='gallery'/></statusbar>"
    "</window>";

#define STRIP_MAX 6

static struct ui_window *win;
static struct widget *view;
static struct camera_info info;
static int cam = -1, pipe_fds[2];
static uint32_t *frames[2];         // the thread fills [1]; the window shows [0]
static mutex_t lock;
static bool fresh;
static struct surface *shown;
static int effect, countdown, countdown_timer = -1;
static bool mirror = true;
static uint64_t flash_at;
static char strip_paths[STRIP_MAX][512];
static int nstrip;

static void *capture(void *arg)
{
    (void)arg;
    for (;;) {
        char b = 1;

        if (camera_read(cam, frames[1], (size_t)info.width * info.height * 4) < 0) {
            msleep(200);
            continue;
        }
        mutex_lock(&lock);
        {
            uint32_t *t = frames[0];

            frames[0] = frames[1];
            frames[1] = t;
        }
        fresh = true;
        mutex_unlock(&lock);
        write(pipe_fds[1], &b, 1);
    }
    return NULL;
}

static uint32_t apply(uint32_t c)
{
    int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF, y = (r * 77 + g * 150 + b * 29) >> 8;

    switch (effect) {
    case 1:
        r = g = b = y;
        break;
    case 2:
        r = MIN(255, y + 40);
        g = MIN(255, y + 14);
        b = MAX(0, y - 24);
        break;
    case 3:
        r = r * 4 / 5;
        b = MIN(255, b + 30);
        break;
    case 4:
        r = MIN(255, r + 30);
        b = b * 4 / 5;
        break;
    case 5:
        r = 255 - r;
        g = 255 - g;
        b = 255 - b;
        break;
    }
    return 0xFF000000U | r << 16 | g << 8 | b;
}

// The current frame with the effect (and mirrored if asked), as a new surface.
static struct surface *processed_as(bool mirrored)
{
    struct surface *s = surface_create(info.width, info.height);

    if (!s)
        return NULL;
    mutex_lock(&lock);
    for (int y = 0; y < info.height; y++) {
        const uint32_t *src = frames[0] + (size_t)y * info.width;
        uint32_t *dst = s->pixels + (size_t)y * s->stride;

        for (int x = 0; x < info.width; x++)
            dst[mirrored ? info.width - 1 - x : x] = apply(src[x]);
    }
    mutex_unlock(&lock);
    return s;
}

static struct surface *processed(void)
{
    return processed_as(mirror);
}

static void frame_ready(int fd, void *u)
{
    char buf[16];
    struct surface *s;

    (void)u;
    read(fd, buf, sizeof(buf));
    if (!fresh || !(s = processed()))
        return;
    fresh = false;
    if (shown)
        surface_destroy(shown);
    shown = s;
    ui_redraw(view);
}

static void paint(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    gfx_fill(g, r, RGB(0x111111));
    if (!shown) {
        struct font *f = font_get(FONT_SANS, 18);
        const char *msg = cam < 0 ? "No camera was found." : "Starting the camera...";

        text_draw(g, f, r.x + (r.w - text_width(f, msg, -1)) / 2, r.y + r.h / 2 - 10, msg, -1, RGB(0xCCCCCC));
        return;
    }
    {
        // Fit, keeping the shape.
        int w2 = r.w, h2 = r.w * shown->height / shown->width;
        struct rect dst;

        if (h2 > r.h) {
            h2 = r.h;
            w2 = r.h * shown->width / shown->height;
        }
        dst = (struct rect){ r.x + (r.w - w2) / 2, r.y + (r.h - h2) / 2, w2, h2 };
        gfx_blit_scaled(g, shown, (struct rect){ 0, 0, shown->width, shown->height }, dst);
        if (countdown > 0) {
            struct font *f = font_get(FONT_SANS_BOLD, 140);
            char n[8];

            snprintf(n, sizeof(n), "%d", countdown);
            gfx_circle(g, dst.x + dst.w / 2.0f, dst.y + dst.h / 2.0f, 110, ALPHA(0x000000, 110));
            text_draw(g, f, dst.x + (dst.w - text_width(f, n, -1)) / 2,
                      dst.y + (dst.h - font_line_height(f)) / 2, n, -1, RGB(0xFFFFFF));
        }
        // The flash after a photo fades out.
        if (uptime_ms() - flash_at < 400) {
            int a = 220 - (int)(uptime_ms() - flash_at) * 220 / 400;

            gfx_fill(g, dst, ALPHA(0xFFFFFF, MAX(a, 0)));
        }
    }
}

// ---- Photos ----

static void shutter_sound(void)
{
    int fd = audio_open("Camera", 48000, 1);
    int16_t pcm[4800];
    uint32_t seed = 12345;

    if (fd < 0)
        return;
    // A short click: decaying noise.
    for (int i = 0; i < 4800; i++) {
        int env = i < 1200 ? 9000 - i * 7 : MAX(0, 600 - (i - 1200) / 6);

        seed = seed * 1103515245 + 12345;
        pcm[i] = (int16_t)(((int)(seed >> 16 & 0x7FFF) - 16384) * env / 16384);
    }
    audio_write(fd, pcm, sizeof(pcm));
    close(fd);
}

static void add_to_strip(const char *path)
{
    struct widget *strip = ui_get(win, "strip"), *img;
    struct surface *s = image_load(path), *thumb;

    if (!s)
        return;
    if ((thumb = surface_create(96, 72))) {
        struct gfx g;

        gfx_init(&g, thumb);
        gfx_blit_scaled(&g, s, (struct rect){ 0, 0, s->width, s->height }, (struct rect){ 0, 0, 96, 72 });
    }
    surface_destroy(s);
    if (!thumb)
        return;
    if (nstrip == STRIP_MAX) {
        ui_remove(ui_child(strip, 0));
        memmove(strip_paths[0], strip_paths[1], (STRIP_MAX - 1) * sizeof(strip_paths[0]));
        nstrip--;
    }
    strlcpy(strip_paths[nstrip++], path, sizeof(strip_paths[0]));
    img = ui_create(win, "image");
    ui_set_attr(img, "width", "96");
    ui_set_attr(img, "height", "72");
    ui_image_set(img, thumb, true);
    ui_add(strip, img);
    ui_relayout(win);
}

static void take_photo(void)
{
    // The preview is a mirror; the photo is the scene the right way round.
    struct surface *s = processed_as(false);
    struct user_info me;
    char dir[300], name[96], unique[128], path[512], status[600];
    struct tm tm;
    int64_t now = time(NULL);

    if (!s)
        return;
    if (user_current(&me) < 0) {
        surface_destroy(s);
        return;
    }
    user_path(&me, "home/Images", dir, sizeof(dir));
    localtime_r(&now, &tm);
    strftime(name, sizeof(name), "Photo %Y-%m-%d %H.%M.%S.png", &tm);
    // unique_name gives the whole path.
    unique_name(dir, name, path, sizeof(path));
    strlcpy(unique, strrchr(path, '/') + 1, sizeof(unique));
    if (image_save_png(s, path) < 0) {
        snprintf(status, sizeof(status), "The photo could not be saved in %s.", dir);
    } else {
        snprintf(status, sizeof(status), "Saved %s", unique);
        add_to_strip(path);
    }
    surface_destroy(s);
    ui_set_text(ui_get(win, "status"), status);
    flash_at = uptime_ms();
    shutter_sound();
}

static bool tick_countdown(void *u)
{
    (void)u;
    if (--countdown > 0) {
        ui_redraw(view);
        return true;
    }
    countdown_timer = -1;
    ui_set_enabled(ui_get(win, "shoot"), true);
    take_photo();
    return false;
}

static void on_shoot(struct widget *w, void *u)
{
    int t = ui_list_selected(ui_get(win, "timer"));

    (void)w;
    (void)u;
    if (cam < 0 || !shown || countdown_timer >= 0)
        return;
    if (t <= 0) {
        take_photo();
        return;
    }
    countdown = t == 1 ? 3 : 10;
    ui_set_enabled(ui_get(win, "shoot"), false);
    countdown_timer = ui_timer(1000, tick_countdown, NULL);
    ui_redraw(view);
}

static void on_effect(struct widget *w, void *u)
{
    (void)u;
    effect = ui_list_selected(w);
}

static void on_mirror(struct widget *w, void *u)
{
    (void)u;
    mirror = ui_value(w) != 0;
}

static void on_gallery(struct widget *w, void *u)
{
    struct user_info me;
    struct app_info a;
    char path[300];

    (void)w;
    (void)u;
    if (user_current(&me) < 0)
        return;
    if (nstrip && app_find("images", &a) == 0) {
        app_launch(&a, strip_paths[nstrip - 1]);
        return;
    }
    user_path(&me, "home/Images", path, sizeof(path));
    if (app_find("files", &a) == 0)
        app_launch(&a, path);
}

static bool repaint_effects(void *u)
{
    (void)u;
    if (uptime_ms() - flash_at < 450)
        ui_redraw(view);
    return true;
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "shoot", on_shoot }, { "effect", on_effect }, { "mirror", on_mirror }, { "gallery", on_gallery },
        { NULL, NULL },
    };
    thread_t t;

    ui_load_user_theme();
    if (pipe(pipe_fds) < 0 || !(win = ui_load_string_named(window_aui, handlers, NULL, "camera")))
        return 1;
    view = ui_get(win, "view");
    ui_canvas_set(view, paint, NULL, NULL);
    ui_list_select(ui_get(win, "effect"), 0);
    ui_list_select(ui_get(win, "timer"), 0);
    if ((cam = camera_open(0, &info)) >= 0
        && (frames[0] = calloc((size_t)info.width * info.height, 4))
        && (frames[1] = calloc((size_t)info.width * info.height, 4))
        && thread_create(&t, capture, NULL) == 0) {
        char status[128];

        snprintf(status, sizeof(status), "%s, %dx%d at %d frames a second", info.name, info.width, info.height,
                 info.fps);
        ui_set_text(ui_get(win, "status"), status);
        ui_watch_fd(pipe_fds[0], frame_ready, NULL);
    } else {
        ui_set_text(ui_get(win, "status"), errno == EACCES ? "You are not allowed to use the camera."
                                                            : "No camera was found.");
        ui_set_enabled(ui_get(win, "shoot"), false);
        cam = -1;
    }
    ui_window_show(win);
    ui_timer(40, repaint_effects, NULL);
    return ui_run();
}
