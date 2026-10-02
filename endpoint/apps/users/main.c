#include "aegis.h"
#include "ui.h"

// User Manager: accounts on this computer.

static const char page[] =
    "<window title='User Manager' width='760' height='500' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button flat='true' symbol='add' text='Add account...' onclick='add' shortcut='Ctrl+N'/>"
    "    <spacer/>"
    "    <label id='lockstate' dim='true' align='center'/>"
    "    <button id='unlock' flat='true' symbol='lock' text='Unlock' onclick='unlock'/>"
    "  </toolbar>"
    "  <hbox expand='1' padding='12' spacing='12'>"
    "    <list id='users' width='240' onselect='select'/>"
    "    <vbox expand='1' spacing='12'>"
    "      <hbox spacing='16'>"
    "        <canvas id='avatar' expand='0' width='88' height='88'/>"
    "        <vbox spacing='4' align='center'>"
    "          <h2 id='display'/>"
    "          <label id='login' dim='true'/>"
    "          <label id='home' dim='true'/>"
    "        </vbox>"
    "      </hbox>"
    "      <grid columns='2' spacing='10'>"
    "        <label text='Account type'/>"
    "        <dropdown id='type' width='220' onchange='type'>"
    "          <option>Standard</option><option>Administrator</option>"
    "        </dropdown>"
    "        <label text='Display name'/>"
    "        <hbox spacing='8'><input id='newname' expand='1'/><button text='Rename' onclick='rename'/></hbox>"
    "      </grid>"
    "      <hbox spacing='8'>"
    "        <button text='Reset password...' onclick='password'/>"
    "        <spacer/>"
    "        <button text='Remove account...' onclick='remove'/>"
    "      </hbox>"
    "      <label id='note' dim='true'/>"
    "    </vbox>"
    "  </hbox>"
    "</window>";

static const char add_page[] =
    "<window title='Add account' width='440' padding='18' spacing='12' resizable='false'>"
    "  <grid columns='2' spacing='10'>"
    "    <label text='Full name'/><input id='full' onchange='full'/>"
    "    <label text='User name'/><input id='name' placeholder='lowercase, no spaces'/>"
    "    <label text='Password'/><password id='pw1'/>"
    "    <label text='Repeat password'/><password id='pw2'/>"
    "  </grid>"
    "  <checkbox id='admin' text='Administrator (can change the whole computer)'/>"
    "  <label id='error' dim='true'/>"
    "  <hbox justify='end' spacing='8'>"
    "    <button text='Cancel' cancel='true' onclick='cancel'/>"
    "    <button text='Add' default='true' onclick='ok'/>"
    "  </hbox>"
    "</window>";

static struct ui_window *win, *dlg;
static struct user_info users[64];
static int nusers;
static struct user_info me;

static struct user_info *current(void)
{
    int i = ui_list_selected(ui_get(win, "users"));

    return i >= 0 && i < nusers ? &users[i] : NULL;
}

static void show_lock(void)
{
    bool root = geteuid() == 0;

    ui_set_text(ui_get(win, "lockstate"), root ? "Changes allowed" : "Unlock to make changes");
    ui_set_visible(ui_get(win, "unlock"), !root);
}

static bool unlocked(void)
{
    bool ok = ui_elevate(win, "Changing accounts affects the whole computer.");

    show_lock();
    return ok;
}

static void paint_avatar(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    struct user_info *cu = current();
    char pic[256] = "";

    (void)w;
    (void)u;
    if (!cu)
        return;
    user_setting_get(cu, "picture", pic, sizeof(pic));
    avatar_draw(g, r, cu->display, pic);
}

static void show_user(void)
{
    struct user_info *u = current();
    char buf[200];

    if (!u)
        return;
    ui_set_text(ui_get(win, "display"), u->display);
    snprintf(buf, sizeof(buf), "Signs in as \"%s\" (number %u)", u->name, u->uid);
    ui_set_text(ui_get(win, "login"), buf);
    snprintf(buf, sizeof(buf), "Files in %s", u->home);
    ui_set_text(ui_get(win, "home"), buf);
    ui_list_select(ui_get(win, "type"), account_is_admin(u->name) ? 1 : 0);
    ui_set_text(ui_get(win, "newname"), u->display);
    ui_set_text(ui_get(win, "note"), u->uid == me.uid ? "This is your account." : "");
    ui_redraw(ui_get(win, "avatar"));
}

static void load(void)
{
    struct widget *l = ui_get(win, "users");
    int sel = ui_list_selected(l);

    nusers = user_list(users, 64);
    ui_list_clear(l);
    for (int i = 0; i < nusers; i++) {
        char row[160];

        snprintf(row, sizeof(row), "%s\t%s", users[i].display,
                 account_is_admin(users[i].name) ? "Administrator" : "Standard");
        ui_list_add(l, row);
        ui_list_set_icon_shared(l, i, icon_get("glyph:user", 20));
    }
    ui_list_select(l, sel >= 0 && sel < nusers ? sel : 0);
    show_user();
}

static void on_select(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show_user();
}

static void on_unlock(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    unlocked();
}

static void on_type(struct widget *w, void *u)
{
    struct user_info *cu = current();
    bool admin = ui_list_selected(w) == 1;
    int admins = 0;

    (void)u;
    if (!cu || admin == account_is_admin(cu->name))
        return;
    for (int i = 0; i < nusers; i++)
        admins += account_is_admin(users[i].name);
    if (!admin && admins <= 1) {
        ui_message(win, "User Manager", "The computer needs at least one administrator.", "OK");
        ui_list_select(w, 1);
        return;
    }
    if (!unlocked()) {
        ui_list_select(w, admin ? 0 : 1);
        return;
    }
    group_set_member("sudo", cu->name, admin);
    group_set_member("adm", cu->name, admin);
    syslog("users", "%s is now %s", cu->name, admin ? "an administrator" : "a standard account");
    load();
}

static void on_rename(struct widget *w, void *u)
{
    struct user_info *cu = current();
    const char *name = ui_text(ui_get(win, "newname"));

    (void)w;
    (void)u;
    if (!cu || !*name || strchr(name, ':'))
        return;
    if (cu->uid != getuid() && !unlocked())
        return;
    // The settings file wins; the passwd comment is kept in step when possible.
    user_setting_set(cu, "name", name);
    if (geteuid() == 0)
        account_set_display_name(cu->name, name);
    load();
}

static void on_password(struct widget *w, void *u)
{
    struct user_info *cu = current();
    char *pw, *again, msg[200];

    (void)w;
    (void)u;
    if (!cu || !unlocked())
        return;
    snprintf(msg, sizeof(msg), "New password for %s:", cu->display);
    if (!(pw = ui_prompt_password(win, "Reset password", msg)))
        return;
    if (!(again = ui_prompt_password(win, "Reset password", "Type it again:"))) {
        free(pw);
        return;
    }
    if (strcmp(pw, again))
        ui_message(win, "User Manager", "The passwords do not match.", "OK");
    else if (strlen(pw) < 4)
        ui_message(win, "User Manager", "Use at least four characters.", "OK");
    else if (account_set_password(cu->name, pw) < 0)
        ui_message(win, "User Manager", "The password could not be changed.", "OK");
    else {
        syslog("users", "password reset for %s", cu->name);
        ui_message(win, "User Manager", "The password has been changed.", "OK");
    }
    memset(pw, 0, strlen(pw));
    memset(again, 0, strlen(again));
    free(pw);
    free(again);
}

static void on_remove(struct widget *w, void *u)
{
    struct user_info *cu = current();
    char msg[300];
    int r;

    (void)w;
    (void)u;
    if (!cu)
        return;
    if (cu->uid == me.uid) {
        ui_message(win, "User Manager", "You cannot remove the account you are signed in with.", "OK");
        return;
    }
    snprintf(msg, sizeof(msg), "Remove the account of %s? Choose whether their files are kept in %s.",
             cu->display, cu->dir);
    r = ui_message(win, "Remove account", msg, "Remove and delete files|Remove, keep files|Cancel");
    if (r < 0 || r == 2 || !unlocked())
        return;
    if (account_remove(cu->name, r == 0) < 0) {
        snprintf(msg, sizeof(msg), "The account could not be removed: %s.", strerror(errno));
        ui_message(win, "User Manager", msg, "OK");
    } else {
        syslog("users", "removed %s%s", cu->name, r == 0 ? " and their files" : "");
    }
    load();
}

// ---- Adding accounts ----

static void on_full(struct widget *w, void *u)
{
    char name[32];
    int n = 0;

    (void)u;
    // Suggest a user name from the full name.
    for (const char *p = ui_text(w); *p && n < 31; p++) {
        if (isalnum((unsigned char)*p))
            name[n++] = tolower((unsigned char)*p);
        else if (*p == ' ')
            break;
    }
    name[n] = 0;
    if (n && isdigit((unsigned char)name[0]))
        memmove(name + 1, name, n + 1), name[0] = 'u';
    ui_set_text(ui_get(dlg, "name"), name);
}

static void on_ok(struct widget *w, void *u)
{
    const char *name = ui_text(ui_get(dlg, "name")), *p1 = ui_text(ui_get(dlg, "pw1"));

    (void)w;
    (void)u;
    if (!*name) {
        ui_set_text(ui_get(dlg, "error"), "Choose a user name.");
        return;
    }
    if (strcmp(p1, ui_text(ui_get(dlg, "pw2")))) {
        ui_set_text(ui_get(dlg, "error"), "The passwords do not match.");
        return;
    }
    if (strlen(p1) < 4) {
        ui_set_text(ui_get(dlg, "error"), "Use a password of at least four characters.");
        return;
    }
    if (account_add(name, ui_text(ui_get(dlg, "full")), p1, ui_value(ui_get(dlg, "admin")) != 0) < 0) {
        char msg[160];

        snprintf(msg, sizeof(msg), errno == EEXIST ? "That user name is taken."
                 : errno == EINVAL ? "User names use lowercase letters, digits, - and _, starting with a letter."
                 : "The account could not be added: %s.", strerror(errno));
        ui_set_text(ui_get(dlg, "error"), msg);
        return;
    }
    syslog("users", "added %s", name);
    ui_dialog_end(dlg, 1);
}

static void on_cancel(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_dialog_end(dlg, 0);
}

static void on_add(struct widget *w, void *u)
{
    static const struct ui_handler_entry handlers[] = {
        { "full", on_full }, { "ok", on_ok }, { "cancel", on_cancel }, { NULL, NULL },
    };

    (void)w;
    (void)u;
    if (!unlocked() || !(dlg = ui_load_string_named(add_page, handlers, NULL, "users-add")))
        return;
    if (ui_dialog_run(dlg, win) == 1) {
        load();
        ui_list_select(ui_get(win, "users"), nusers - 1);
        show_user();
    }
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "add", on_add }, { "unlock", on_unlock }, { "select", on_select }, { "type", on_type },
        { "rename", on_rename }, { "password", on_password }, { "remove", on_remove }, { NULL, NULL },
    };

    ui_load_user_theme();
    user_current(&me);
    if (!(win = ui_load_string_named(page, handlers, NULL, "users")))
        return 1;
    ui_canvas_set(ui_get(win, "avatar"), paint_avatar, NULL, NULL);
    load();
    show_lock();
    ui_window_show(win);
    return ui_run();
}
