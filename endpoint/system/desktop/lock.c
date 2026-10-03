#include "desktop.h"

// The lock screen: a full-screen window over everything until the user's
// password is typed again. Meta+L or "Lock" in the session menu.

static const char page[] =
    "<window role='overlay' padding='0'>"
    "  <vbox expand='true' justify='center' spacing='14'>"
    "    <label id='time' size='60' bold='true' textalign='center'/>"
    "    <label id='date' size='large' textalign='center'/>"
    "    <spacer size='30'/>"
    "    <canvas id='avatar' height='96'/>"
    "    <label id='name' size='large' bold='true' textalign='center'/>"
    "    <hbox justify='center'><password id='password' width='300' placeholder='Password'"
    "      onactivate='unlock'/></hbox>"
    "    <label id='error' textalign='center'/>"
    "  </vbox>"
    "</window>";

static struct ui_window *win;
static bool locked;

bool lock_active(void)
{
    return locked;
}

static void backdrop(struct ui_window *w, struct gfx *g, struct rect r, void *u)
{
    char bg[256] = "default";

    (void)w;
    (void)u;
    user_setting_get(&me, "background", bg, sizeof(bg));
    wallpaper_draw(g, r, bg);
    gfx_fill(g, r, ALPHA(0x000000, 0x90));
}

static void paint_avatar(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    char pic[256] = "";

    (void)w;
    (void)u;
    user_setting_get(&me, "picture", pic, sizeof(pic));
    {
        // A circle in the middle of whatever width the canvas got.
        int size = MIN(r.w, r.h);

        avatar_draw(g, (struct rect){ r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size, size }, me.display, pic);
    }
}

static bool tick(void *u)
{
    char buf[64];
    struct tm tm;
    int64_t now = time(NULL);

    (void)u;
    if (!locked)
        return true;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    ui_set_text(ui_get(win, "time"), buf);
    strftime(buf, sizeof(buf), "%A, %B %e", &tm);
    ui_set_text(ui_get(win, "date"), buf);
    return true;
}

static void on_unlock(struct widget *w, void *u)
{
    char pass[256];

    (void)u;
    strlcpy(pass, ui_text(w), sizeof(pass));
    ui_set_text(w, "");
    ui_set_text(ui_get(win, "error"), "Checking...");
    ui_redraw(ui_get(win, "error"));
    if (verify_own_password(pass) == 0) {
        locked = false;
        ui_set_text(ui_get(win, "error"), "");
        ui_window_hide(win);
        syslog("desktop", "%s unlocked the screen", me.name);
    } else {
        ui_set_text(ui_get(win, "error"), "That password is not right.");
        ui_focus(w);
    }
    memset(pass, 0, sizeof(pass));
}

static bool refuse_close(struct ui_window *w, void *u)
{
    (void)w;
    (void)u;
    return false;
}

void lock_screen(void)
{
    int sw, sh;

    if (!win || locked)
        return;
    locked = true;
    wm_screen_size(&sw, &sh, NULL);
    ui_window_set_size(win, sw, sh);
    ui_window_move(win, 0, 0);
    ui_set_text(ui_get(win, "error"), "");
    tick(NULL);
    ui_window_show(win);
    ui_focus(ui_get(win, "password"));
    syslog("desktop", "%s locked the screen", me.name);
}

void lock_init(void)
{
    static const struct ui_handler_entry handlers[] = { { "unlock", on_unlock }, { NULL, NULL } };

    if (!(win = ui_load_string_named(page, handlers, NULL, "lock")))
        return;
    ui_window_set_backdrop(win, backdrop, NULL);
    ui_canvas_set(ui_get(win, "avatar"), paint_avatar, NULL, NULL);
    ui_set_text(ui_get(win, "name"), me.display);
    ui_on_close(win, refuse_close, NULL);
    ui_window_hide(win);
    ui_timer(1000, tick, NULL);
}
