#include "aegis.h"

// Optional parts of the system, switched in /msc/features.conf
// ("name=on" or "name=off"). Unlisted features use their default.

#define FEATURES "/msc/features.conf"
#define BCD_PATH "/osystem/boot/EFI/Aegis/bcd"

static const struct feature features[] = {
    { "console", "Text console", "A shell on Ctrl+Alt+F2 for when the desktop cannot help. Takes effect after "
      "a restart.", true, true },
    { "routines", "Routines", "Runs the routines set up in the Routines app. Takes effect after a restart.", true, true },
    { "audio", "Sound", "The sound server that plays every app's audio. Takes effect after a restart.", true, true },
    { "development", "Development apps", "Window Builder, App Maker, the System Debugger and the Log Viewer in "
      "the launcher.", true, false },
    { "bootlog", "Show startup messages", "Starts the computer with the verbose boot entry, so messages scroll by "
      "instead of the logo (F13 or the boot menu still work).", false, false },
    { "autologin", "Sign in automatically", "Starts the desktop of the first administrator without asking for a "
      "password. Anyone at the computer gets their files.", false, false },
};

int feature_list(const struct feature **out)
{
    *out = features;
    return sizeof(features) / sizeof(features[0]);
}

bool feature_enabled(const char *name)
{
    int fd = open(FEATURES, O_RDONLY);
    char line[128];
    size_t n = strlen(name);
    bool on = false, found = false;

    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); i++)
        if (!strcmp(features[i].id, name))
            on = features[i].default_on;
    if (fd < 0)
        return on;
    while (!found && read_line(fd, line, sizeof(line)) >= 0) {
        if (!strncmp(line, name, n) && line[n] == '=') {
            on = !strcmp(line + n + 1, "on");
            found = true;
        }
    }
    close(fd);
    return on;
}

// The boot menu's default entry follows the bootlog feature.
static int set_boot_default(bool verbose)
{
    char buf[8192], tmp[] = BCD_PATH ".new";
    int fd = open(BCD_PATH, O_RDONLY), out;
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    if ((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        return -1;
    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);

        if (!strncmp(line, "default=", 8))
            dprintf(out, "default=%s\n", verbose ? "verbose" : "aegis");
        else
            dprintf(out, "%.*s\n", (int)len, line);
        line += len + (nl ? 1 : 0);
    }
    close(out);
    // FAT cannot replace a file by renaming over it.
    unlink(BCD_PATH);
    if (rename(tmp, BCD_PATH) < 0)
        return -1;
    sync();
    return 0;
}

int feature_set(const char *name, bool on)
{
    const struct feature *list;
    int n = feature_list(&list), fd;
    char tmp[] = FEATURES ".new";

    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        return -1;
    dprintf(fd, "# Optional features. Change them with the Feature Manager.\n");
    for (int i = 0; i < n; i++) {
        bool v = !strcmp(list[i].id, name) ? on : feature_enabled(list[i].id);

        dprintf(fd, "%s=%s\n", list[i].id, v ? "on" : "off");
    }
    close(fd);
    if (rename(tmp, FEATURES) < 0)
        return -1;
    if (!strcmp(name, "bootlog"))
        return set_boot_default(on);
    return 0;
}
