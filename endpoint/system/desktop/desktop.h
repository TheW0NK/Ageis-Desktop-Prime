#ifndef DESKTOP_H
#define DESKTOP_H

#include "aegis.h"
#include "ui.h"

// The desktop shell: background with shortcuts, taskbar, launcher.

extern struct user_info me;

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

// launcher.c
void launcher_init(struct ui_window *panel);
void launcher_toggle(void);
bool launcher_shown(void);
void launcher_refresh_user(void);

#endif
