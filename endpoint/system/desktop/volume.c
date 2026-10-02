#include "desktop.h"

// The volume control: a panel button that opens a popup with the master
// volume, mute and a slider for each app playing sound. The volume keys
// change it and show the popup for a moment.

#define WIDTH   330

static const char page[] =
    "<window padding='14' spacing='10'>"
    "  <hbox spacing='8'>"
    "    <button id='mute' flat='true' symbol='volume' tooltip='Mute' onclick='mute'/>"
    "    <slider id='master' expand='true' min='0' max='100' onchange='master'/>"
    "    <label id='pct' width='44' textalign='right'/>"
    "  </hbox>"
    "  <separator/>"
    "  <label text='Apps' bold='true' dim='true'/>"
    "  <vbox id='apps' spacing='6'/>"
    "  <label id='none' text='No app is playing sound.' dim='true'/>"
    "</window>";

static struct ui_window *win, *panel_win;
static struct widget *button;
static uint64_t hidden_at;
static int hide_timer = -1;
static bool muted;
static int volume = -1;

static void show_level(void)
{
    char pct[16];

    snprintf(pct, sizeof(pct), muted ? "Muted" : "%d%%", volume);
    if (win) {
        ui_set_text(ui_get(win, "pct"), pct);
        ui_set_value(ui_get(win, "master"), volume);
    }
    if (button) {
        char tip[48];

        snprintf(tip, sizeof(tip), "Volume: %s", pct);
        ui_set_attr(button, "tooltip", tip);
        ui_set_attr(button, "dim", muted || volume == 0 ? "true" : "false");
    }
}

static void read_level(void)
{
    volume = audio_get_volume(&muted);
    if (button)
        ui_set_visible(button, volume >= 0);
    if (volume < 0)
        volume = 0;
}

static void stream_changed(struct widget *w, void *u)
{
    audio_set_stream_volume((int)(intptr_t)u, (int)ui_value(w));
}

static void rebuild_streams(void)
{
    struct widget *box = ui_get(win, "apps");
    struct audio_stream streams[16];
    int n = audio_streams(streams, 16);

    while (ui_children(box))
        ui_remove(ui_child(box, 0));
    for (int i = 0; i < n; i++) {
        struct widget *row = ui_create(win, "hbox"), *name = ui_create(win, "label"),
                      *slider = ui_create(win, "slider");
        char value[8];

        ui_set_attr(row, "spacing", "8");
        ui_set_text(name, streams[i].name);
        ui_set_attr(name, "width", "120");
        ui_set_attr(slider, "min", "0");
        ui_set_attr(slider, "max", "100");
        ui_set_attr(slider, "expand", "true");
        snprintf(value, sizeof(value), "%d", streams[i].volume);
        ui_set_attr(slider, "value", value);
        ui_set_handler(slider, "change", stream_changed, (void *)(intptr_t)streams[i].id);
        ui_add(row, name);
        ui_add(row, slider);
        ui_add(box, row);
    }
    ui_set_visible(ui_get(win, "none"), n <= 0);
}

static void place(void)
{
    struct rect r = ui_rect(ui_root(panel_win)), b = ui_rect(button);

    ui_window_fit(win);
    ui_window_set_size(win, WIDTH, ui_wm_window(win) ? ui_wm_window(win)->height : 160);
    ui_window_move(win, MIN(b.x + b.w / 2 - WIDTH / 2, r.w - WIDTH - 4), -(ui_wm_window(win)
                   ? ui_wm_window(win)->height : 160) - 6);
}

static bool auto_hide(void *u)
{
    (void)u;
    hide_timer = -1;
    if (win)
        ui_window_hide(win);
    return false;
}

static void open_popup(bool briefly)
{
    if (hide_timer >= 0) {
        ui_timer_cancel(hide_timer);
        hide_timer = -1;
    }
    read_level();
    show_level();
    rebuild_streams();
    place();
    ui_window_show(win);
    if (briefly)
        hide_timer = ui_timer(1500, auto_hide, NULL);
}

static void on_button(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_wm_window(win) && ui_wm_window(win)->visible) {
        ui_window_hide(win);
        return;
    }
    if (uptime_ms() - hidden_at < 300)
        return;
    open_popup(false);
}

static void on_master(struct widget *w, void *u)
{
    (void)u;
    volume = (int)ui_value(w);
    muted = false;
    audio_set_volume(volume);
    show_level();
}

static void on_mute(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    muted = !muted;
    audio_set_mute(muted);
    show_level();
}

static bool dismissed(struct ui_window *w, void *u)
{
    (void)u;
    hidden_at = uptime_ms();
    ui_window_hide(w);
    return false;
}

bool volume_key(struct wm_event *ev)
{
    if (!ev->value || (ev->key != KEY_VOLUMEUP && ev->key != KEY_VOLUMEDOWN && ev->key != KEY_MUTE))
        return false;
    read_level();
    if (ev->key == KEY_MUTE) {
        muted = !muted;
        audio_set_mute(muted);
    } else {
        volume = MIN(MAX(volume + (ev->key == KEY_VOLUMEUP ? 5 : -5), 0), 100);
        muted = false;
        audio_set_volume(volume);
    }
    open_popup(true);
    return true;
}

void volume_init(struct ui_window *panel, struct widget *panel_button)
{
    static const struct ui_handler_entry handlers[] = {
        { "master", on_master }, { "mute", on_mute }, { NULL, NULL },
    };

    panel_win = panel;
    button = panel_button;
    ui_set_handler(button, "click", on_button, NULL);
    if (!(win = ui_load_string_named(page, handlers, NULL, "volume")))
        return;
    ui_window_set_parent(win, panel);
    ui_window_set_flags(win, WM_ROLE_POPUP);
    ui_window_hide(win);
    ui_on_close(win, dismissed, NULL);
    read_level();
    show_level();
}
