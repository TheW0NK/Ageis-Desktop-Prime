#include "aegis.h"
#include "ui.h"
#include "script.h"

// apprun: runs AegisScript.
//
//   apprun FILE.as [ARG...]     a script (print() goes to the terminal)
//   apprun APP_FOLDER           an app: app.aui (its window) and app.as
//
// In an app, on* attributes in app.aui name script functions; a function
// start() runs once the window exists.

extern struct ui_window *script_window;

static char *slurp(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct aegis_stat st;
    char *buf;
    ssize_t got = 0, n;

    if (fd < 0 || fstat(fd, &st) < 0 || !(buf = malloc(st.size + 1))) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    while (got < (ssize_t)st.size && (n = read(fd, buf + got, st.size - got)) > 0)
        got += n;
    close(fd);
    buf[got] = 0;
    return buf;
}

static int run_script(const char *path, int argc, char **argv)
{
    struct script *s = script_new();
    char *src = slurp(path);
    struct script_value args = script_list();

    if (!s || !src) {
        dprintf(STDERR_FILENO, "apprun: cannot read %s\n", path);
        return 1;
    }
    for (int i = 0; i < argc; i++) {
        struct script_value a = script_str(argv[i]);

        script_list_push(args, a);
        script_release(a);
    }
    script_set_global(s, "args", args);
    script_release(args);
    if (!script_run(s, src, path)) {
        dprintf(STDERR_FILENO, "%s\n", script_error(s));
        return 1;
    }
    free(src);
    script_free(s);
    return 0;
}

static int run_app(const char *dir)
{
    char aui[512], code[512], *src;
    struct script *s = script_new();
    struct aegis_stat st;

    snprintf(aui, sizeof(aui), "%s/app.aui", dir);
    snprintf(code, sizeof(code), "%s/app.as", dir);
    if (!s)
        return 1;
    ui_load_user_theme();
    script_add_ui(s);
    // The code runs first, so the window's handlers can find its functions.
    if (stat(code, &st) == 0) {
        if (!(src = slurp(code)) || !script_run(s, src, "app.as")) {
            ui_message(NULL, "App error", src ? script_error(s) : "app.as cannot be read.", "OK");
            return 1;
        }
        free(src);
    }
    if (!(script_window = ui_load(aui, NULL, NULL))) {
        ui_message(NULL, "App error", "The app's window (app.aui) cannot be loaded.", "OK");
        return 1;
    }
    if (script_has_function(s, "start")) {
        struct script_value r;

        if (!script_call(s, "start", NULL, 0, &r))
            ui_message(script_window, "Script error", script_error(s), "OK");
        else
            script_release(r);
    }
    ui_window_show(script_window);
    return ui_run();
}

int main(int argc, char **argv)
{
    struct aegis_stat st;
    size_t len;

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: apprun FILE.as [ARG...] | apprun APP_FOLDER\n");
        return 2;
    }
    len = strlen(argv[1]);
    if (stat(argv[1], &st) == 0 && S_ISDIR(st.mode))
        return run_app(argv[1]);
    if (len > 4 && !strcmp(argv[1] + len - 4, ".aui")) {
        // app.aui opened directly: run its folder.
        char dir[512], *slash;

        strlcpy(dir, argv[1], sizeof(dir));
        if ((slash = strrchr(dir, '/')))
            *slash = 0;
        else
            strcpy(dir, ".");
        return run_app(dir);
    }
    return run_script(argv[1], argc - 2, argv + 2);
}
