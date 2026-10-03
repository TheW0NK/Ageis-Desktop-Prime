#include "desktop.h"

// The desktop shell, started by /osystem/core/session as the signed-in user.

struct user_info me;

int main(void)
{
    int code;

    if (user_current(&me) < 0) {
        dprintf(STDERR_FILENO, "desktop: who am I?\n");
        return 1;
    }
    // The shell is always dark; apps follow the user's theme.
    ui_set_theme("dark");
    // The display may not have learned yet that this user is signed in
    // (the greeter tells it just before starting the session): try again
    // for a moment.
    for (int tries = 0; !wm_connect(); tries++) {
        if (tries == 30) {
            dprintf(STDERR_FILENO, "desktop: no display\n");
            return 1;
        }
        msleep(100);
    }
    {
        char theme[32];

        // Window frames follow the user's theme.
        if (user_setting_get(&me, "theme", theme, sizeof(theme)) > 0)
            wm_setting_changed("theme", theme);
    }
    background_start();
    panel_start();
    code = ui_run();
    sync();
    return code;
}
