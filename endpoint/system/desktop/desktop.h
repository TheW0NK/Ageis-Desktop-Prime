#ifndef DESKTOP_H
#define DESKTOP_H

#include "aegis.h"
#include "ui.h"

// The desktop shell: background with shortcuts, taskbar, launcher.

extern struct user_info me;
// Signed in as Guest: no password, and everything goes at sign-out.
extern bool me_guest;

// Exit codes the session passes to the greeter.
#define EXIT_SIGN_OUT   0
#define EXIT_POWEROFF   10
#define EXIT_RESTART    11

// background.c
void background_start(void);
void background_reload(void);
// Opens a file, folder or .shortcut the way a double click would.
void open_path(const char *path);

// panel.c
void panel_start(void);
void end_session(int code);
struct panel_window {
    uint32_t id;                    // global window id
    char title[WM_TEXT_MAX];
    int workspace;
    bool focused;
};
// The open windows, as the taskbar knows them.
int panel_windows(struct panel_window *out, int max);
int panel_workspace(void);
// The panel's top edge on the screen (popups are placed relative to it).
int panel_top(void);
// Runs a session menu action: "lock", "signout", "restart", "poweroff", "settings".
void panel_action(const char *name);

// palette.c: the command palette.
void palette_init(struct ui_window *panel);
void palette_toggle(void);
bool palette_shown(void);
void palette_command_item(const struct wm_msg *m);

// launcher.c
void launcher_init(struct ui_window *panel);
void launcher_toggle(void);
bool launcher_shown(void);
void launcher_refresh_user(void);

// volume.c
void volume_init(struct ui_window *panel, struct widget *button);
// Handles the volume keys; false for other keys.
bool volume_key(struct wm_event *ev);

// notify.c: notifications from notify() and the list behind the bell.
void notify_init(struct ui_window *panel, struct widget *button);

// lock.c: the lock screen.
void lock_init(void);
void lock_screen(void);
bool lock_active(void);

#endif
