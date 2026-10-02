#include "aegis.h"
#include "ui.h"

// Feature Manager: optional parts of the system.

static const char page[] =
    "<window title='Feature Manager' width='640' padding='20' spacing='14'>"
    "  <h1 text='Features'/>"
    "  <p dim='true'>Turn optional parts of Aegis on or off. Changes affect everyone on this computer.</p>"
    "  <vbox id='list' spacing='10'/>"
    "  <hbox spacing='8'>"
    "    <label id='note' dim='true' align='center'/>"
    "    <spacer/>"
    "    <button id='restart' text='Restart now' hidden='true' onclick='restart'/>"
    "    <button text='Close' default='true' onclick='close'/>"
    "  </hbox>"
    "</window>";

static struct ui_window *win;
static const struct feature *features;
static int nfeatures;
static struct widget *toggles[16];

static void changed(struct widget *w, void *u)
{
    const struct feature *f = u;
    bool on = ui_value(w) != 0;
    char msg[160];

    if (!ui_elevate(win, "Features affect everyone on this computer.")) {
        ui_set_value(w, !on);
        return;
    }
    if (feature_set(f->id, on) < 0) {
        snprintf(msg, sizeof(msg), "\"%s\" could not be changed: %s.", f->name, strerror(errno));
        ui_message(win, "Feature Manager", msg, "OK");
        ui_set_value(w, feature_enabled(f->id));
        return;
    }
    syslog("features", "%s turned %s", f->id, on ? "on" : "off");
    if (f->needs_restart) {
        ui_set_text(ui_get(win, "note"), "Some changes take effect after a restart.");
        ui_set_visible(ui_get(win, "restart"), true);
    }
}

static void on_restart(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(win, "Restart", "Restart the computer now? Unsaved work in open apps will be lost.",
                   "Restart|Cancel") == 0) {
        sync();
        if (geteuid() == 0)
            reboot(REBOOT_RESTART);
    }
}

static void on_close(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_quit(0);
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "restart", on_restart }, { "close", on_close }, { NULL, NULL },
    };
    struct widget *list;

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "features")))
        return 1;
    list = ui_get(win, "list");
    nfeatures = MIN(feature_list(&features), 16);
    for (int i = 0; i < nfeatures; i++) {
        struct widget *box = ui_create(win, "group"), *row = ui_create(win, "hbox"), *desc = ui_create(win, "p");

        ui_set_attr(box, "padding", "12");
        ui_add(list, box);
        toggles[i] = ui_create(win, "toggle");
        ui_set_text(toggles[i], features[i].name);
        ui_set_value(toggles[i], feature_enabled(features[i].id));
        ui_set_handler(toggles[i], "change", changed, (void *)&features[i]);
        ui_add(row, toggles[i]);
        ui_add(box, row);
        ui_set_text(desc, features[i].description);
        ui_set_attr(desc, "dim", "true");
        ui_add(box, desc);
    }
    ui_window_show(win);
    return ui_run();
}
