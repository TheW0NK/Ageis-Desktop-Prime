#include "aegis.h"
#include "ui.h"

// App Maker: make apps on Aegis. An app is a folder in ~/Documents/Apps
// with app.aui (designed in Window Builder), app.as (AegisScript) and
// app.info (name, icon, description). Installing it adds it to your
// launcher.

static const char page[] =
    "<window title='App Maker' width='1040' height='680' padding='0' spacing='0' onclose='quit'>"
    "  <toolbar>"
    "    <button flat='true' symbol='add' text='New app' onclick='new' shortcut='Ctrl+N'/>"
    "    <separator/>"
    "    <button flat='true' symbol='builder' text='Design window' onclick='design' shortcut='Ctrl+D'/>"
    "    <button flat='true' symbol='play' text='Run' onclick='run' shortcut='F5'/>"
    "    <button flat='true' text='Save code' onclick='save' shortcut='Ctrl+S'/>"
    "    <spacer/>"
    "    <button flat='true' symbol='package' text='Install' onclick='install'/>"
    "    <button flat='true' text='Uninstall' onclick='uninstall'/>"
    "  </toolbar>"
    "  <hbox expand='1' padding='10' spacing='10'>"
    "    <vbox width='230' spacing='8'>"
    "      <label text='Your apps' bold='true'/>"
    "      <list id='projects' expand='1' onselect='pick' placeholder='No apps yet'/>"
    "      <hbox spacing='6'><button text='Show folder' onclick='folder'/>"
    "        <button text='Delete' onclick='delete'/></hbox>"
    "    </vbox>"
    "    <vbox expand='1' spacing='8'>"
    "      <grid columns='4' spacing='8' stretch='1,3'>"
    "        <label text='Name'/><input id='name' onchange='meta'/>"
    "        <label text='Icon'/><dropdown id='icon' onchange='meta'/>"
    "        <label text='About'/><input id='about' onchange='meta'/>"
    "        <label text='Suite'/>"
    "        <dropdown id='suite' onchange='meta'><option>Default</option><option>System</option>"
    "          <option>Administrative</option><option>Development</option></dropdown>"
    "      </grid>"
    "      <hbox spacing='8'><label text='Code (app.as)' bold='true'/><spacer/>"
    "        <label id='state' dim='true'/></hbox>"
    "      <textarea id='code' expand='1' mono='true' autoindent='true'/>"
    "      <label id='help' dim='true' text='Handlers named in the window (onclick=\"name\") call fn name(widget) { ... } here. start() runs when the window opens. See docs/SCRIPT.md.'/>"
    "    </vbox>"
    "  </hbox>"
    "  <statusbar><label id='status'/></statusbar>"
    "</window>";

static const char *const icons[] = { "appmaker", "builder", "notepad", "calculator", "clock", "images", "music",
                                     "mail", "browser", "files", "terminal", "settings", "tasks", "users",
                                     "console", "features", "resources", "logs", "camera", "package" };

static const char starter_aui[] =
    "<window title=\"%s\" width=\"360\" padding=\"16\" spacing=\"10\">\n"
    "  <h1 text=\"%s\"/>\n"
    "  <input id=\"name\" placeholder=\"Your name\" onactivate=\"greet\"/>\n"
    "  <label id=\"answer\" dim=\"true\"/>\n"
    "  <hbox justify=\"end\">\n"
    "    <button text=\"Greet\" default=\"true\" onclick=\"greet\"/>\n"
    "  </hbox>\n"
    "</window>\n";

static const char starter_code[] =
    "// %s\n"
    "\n"
    "fn greet(widget) {\n"
    "    let name = trim(ui.get(\"name\").text);\n"
    "    if (name == \"\") { name = \"there\"; }\n"
    "    ui.get(\"answer\").text = \"Hello, \" + name + \"!\";\n"
    "}\n"
    "\n"
    "fn start() {\n"
    "    ui.get(\"name\").focus();\n"
    "}\n";

static struct ui_window *win;
static struct user_info me;
static char apps_dir[256];
static char current[512];           // the open project's folder
static char names[64][64];
static int nprojects;
static bool loading;

static void status(const char *s)
{
    ui_set_text(ui_get(win, "status"), s);
}

static void project_file(const char *file, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", current, file);
}

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

static bool spill(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t len = strlen(text);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok;
}

// app.info: key=value lines.
static void info_get(const char *key, char *out, size_t size)
{
    char path[600], *text, *p;
    size_t kl = strlen(key);

    out[0] = 0;
    project_file("app.info", path, sizeof(path));
    if (!(text = slurp(path)))
        return;
    for (p = text; *p;) {
        char *nl = strchr(p, '\n');

        if (nl)
            *nl = 0;
        if (!strncmp(p, key, kl) && p[kl] == '=')
            strlcpy(out, p + kl + 1, size);
        if (!nl)
            break;
        p = nl + 1;
    }
    free(text);
}

static void save_info(void)
{
    char path[600], text[1024];
    struct widget *icon = ui_get(win, "icon"), *suite = ui_get(win, "suite");

    if (!*current)
        return;
    project_file("app.info", path, sizeof(path));
    snprintf(text, sizeof(text), "name=%s\nicon=%s\ndescription=%s\nsuite=%s\n", ui_text(ui_get(win, "name")),
             ui_list_item(icon, MAX(ui_list_selected(icon), 0)), ui_text(ui_get(win, "about")),
             ui_list_item(suite, MAX(ui_list_selected(suite), 0)));
    spill(path, text);
}

static bool save_code(void)
{
    char path[600];
    struct widget *code = ui_get(win, "code");

    if (!*current)
        return false;
    project_file("app.as", path, sizeof(path));
    if (!spill(path, ui_text(code))) {
        ui_message(win, "App Maker", "The code could not be saved.", "OK");
        return false;
    }
    ui_textarea_set_modified(code, false);
    ui_set_text(ui_get(win, "state"), "Saved");
    return true;
}

static void list_projects(const char *select)
{
    struct widget *l = ui_get(win, "projects");
    struct dir_stream *d = opendir(apps_dir);
    struct aegis_dirent *e;

    ui_list_clear(l);
    nprojects = 0;
    if (!d)
        return;
    while ((e = readdir(d)) && nprojects < 64) {
        char p[600];
        struct aegis_stat st;

        if (e->name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s/app.aui", apps_dir, e->name);
        if (stat(p, &st) < 0)
            continue;
        strlcpy(names[nprojects], e->name, sizeof(names[0]));
        nprojects++;
    }
    closedir(d);
    qsort(names, nprojects, sizeof(names[0]), (int (*)(const void *, const void *))strcasecmp);
    for (int i = 0; i < nprojects; i++) {
        ui_list_add(l, names[i]);
        ui_list_set_icon_shared(l, i, icon_get("glyph:appmaker", 18));
        if (select && !strcmp(select, names[i]))
            ui_list_select(l, i);
    }
}

static void open_project(const char *name)
{
    char path[600], value[256], *code;
    struct widget *icon = ui_get(win, "icon"), *suite = ui_get(win, "suite");

    if (*current && ui_textarea_modified(ui_get(win, "code")))
        save_code();
    snprintf(current, sizeof(current), "%s/%s", apps_dir, name);
    loading = true;
    info_get("name", value, sizeof(value));
    ui_set_text(ui_get(win, "name"), *value ? value : name);
    info_get("description", value, sizeof(value));
    ui_set_text(ui_get(win, "about"), value);
    info_get("icon", value, sizeof(value));
    for (int i = 0; i < ui_list_count(icon); i++)
        if (!strcmp(ui_list_item(icon, i), value))
            ui_list_select(icon, i);
    info_get("suite", value, sizeof(value));
    for (int i = 0; i < ui_list_count(suite); i++)
        if (!strcmp(ui_list_item(suite, i), value))
            ui_list_select(suite, i);
    project_file("app.as", path, sizeof(path));
    code = slurp(path);
    ui_set_text(ui_get(win, "code"), code ? code : "");
    free(code);
    loading = false;
    {
        char msg[700];

        snprintf(msg, sizeof(msg), "%s", current);
        status(msg);
    }
    ui_set_text(ui_get(win, "state"), "");
}

// ---- Handlers ----

static void on_pick(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i >= 0 && i < nprojects)
        open_project(names[i]);
}

static void on_meta(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!loading)
        save_info();
}

static void on_new(struct widget *w, void *u)
{
    char *name = ui_prompt(win, "New app", "What is your app called?", "My App");
    char folder[600], path[700], text[2048];

    (void)w;
    (void)u;
    if (!name)
        return;
    if (!*name || strchr(name, '/')) {
        free(name);
        return;
    }
    mkdir(apps_dir, 0755);
    snprintf(folder, sizeof(folder), "%s/%s", apps_dir, name);
    if (mkdir(folder, 0755) < 0) {
        ui_message(win, "App Maker", "An app with that name already exists.", "OK");
        free(name);
        return;
    }
    snprintf(path, sizeof(path), "%s/app.aui", folder);
    snprintf(text, sizeof(text), starter_aui, name, name);
    spill(path, text);
    snprintf(path, sizeof(path), "%s/app.as", folder);
    snprintf(text, sizeof(text), starter_code, name);
    spill(path, text);
    snprintf(path, sizeof(path), "%s/app.info", folder);
    snprintf(text, sizeof(text), "name=%s\nicon=appmaker\ndescription=Made with App Maker\nsuite=Default\n", name);
    spill(path, text);
    list_projects(name);
    open_project(name);
    free(name);
}

static void on_design(struct widget *w, void *u)
{
    char path[600];

    (void)w;
    (void)u;
    if (!*current)
        return;
    save_code();
    project_file("app.aui", path, sizeof(path));
    launch("/sysapps/builder", path);
    status("Window Builder is open. Save there, then run the app here.");
}

static void on_run(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!*current || !save_code())
        return;
    launch("/sysapps/apprun", current);
}

static void on_save(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    save_code();
}

static void installed_path(const char *project, char *out, size_t size)
{
    char id[80];
    int k = 0;

    // A file name for the registry entry from the folder name.
    for (const char *p = project; *p && k < 60; p++)
        id[k++] = isalnum((unsigned char)*p) ? tolower((unsigned char)*p) : '-';
    id[k] = 0;
    user_path(&me, "system/appdata/applications", out, size);
    strlcat(out, "/user-", size);
    strlcat(out, id, size);
    strlcat(out, ".app", size);
}

static void on_install(struct widget *w, void *u)
{
    char path[600], dir[300], text[1024], icon[32], about[160], suite[32];
    const char *name = ui_text(ui_get(win, "name"));

    (void)w;
    (void)u;
    if (!*current)
        return;
    save_code();
    save_info();
    info_get("icon", icon, sizeof(icon));
    info_get("description", about, sizeof(about));
    info_get("suite", suite, sizeof(suite));
    user_path(&me, "system/appdata/applications", dir, sizeof(dir));
    mkdir(dir, 0755);
    installed_path(strrchr(current, '/') + 1, path, sizeof(path));
    snprintf(text, sizeof(text), "name=%s\nexec=/sysapps/apprun %s\nicon=%s\nsuite=%s\ndescription=%s\n", name,
             current, *icon ? icon : "appmaker", *suite ? suite : "Default", about);
    if (!spill(path, text)) {
        ui_message(win, "App Maker", "The app could not be installed.", "OK");
        return;
    }
    status("Installed for your account.");
    ui_message(win, "App Maker", "Your app is installed for your account. Find it in the launcher.", "OK");
}

static void on_uninstall(struct widget *w, void *u)
{
    char path[600];

    (void)w;
    (void)u;
    if (!*current)
        return;
    installed_path(strrchr(current, '/') + 1, path, sizeof(path));
    if (unlink(path) == 0)
        status("Uninstalled.");
    else
        status("This app was not installed.");
}

static void on_folder(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launch("/sysapps/files", *current ? current : apps_dir);
}

static void on_delete(struct widget *w, void *u)
{
    char msg[700];

    (void)w;
    (void)u;
    if (!*current)
        return;
    snprintf(msg, sizeof(msg), "Delete the app \"%s\" and its files?", strrchr(current, '/') + 1);
    if (ui_message(win, "App Maker", msg, "Delete|Cancel") != 0)
        return;
    on_uninstall(NULL, NULL);
    remove_path(current);
    current[0] = 0;
    ui_set_text(ui_get(win, "code"), "");
    list_projects(NULL);
}

static void on_quit(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (*current && ui_textarea_modified(ui_get(win, "code")))
        save_code();
    ui_quit(0);
}

static bool tick(void *u)
{
    (void)u;
    if (*current && ui_textarea_modified(ui_get(win, "code")))
        ui_set_text(ui_get(win, "state"), "Not saved");
    return true;
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "new", on_new }, { "design", on_design }, { "run", on_run }, { "save", on_save },
        { "install", on_install }, { "uninstall", on_uninstall }, { "pick", on_pick }, { "meta", on_meta },
        { "folder", on_folder }, { "delete", on_delete }, { "quit", on_quit }, { NULL, NULL },
    };
    struct widget *icon;

    ui_load_user_theme();
    if (user_current(&me) < 0)
        return 1;
    snprintf(apps_dir, sizeof(apps_dir), "%s/Documents/Apps", me.home);
    if (!(win = ui_load_string_named(page, handlers, NULL, "appmaker")))
        return 1;
    icon = ui_get(win, "icon");
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        ui_list_add(icon, icons[i]);
        ui_list_set_icon_shared(icon, i, icon_get(icons[i], 18));
    }
    list_projects(NULL);
    if (nprojects) {
        ui_list_select(ui_get(win, "projects"), 0);
        open_project(names[0]);
    } else {
        status("Start with New app.");
    }
    ui_timer(1000, tick, NULL);
    ui_window_show(win);
    return ui_run();
}
