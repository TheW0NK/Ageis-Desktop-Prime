#include "aegis.h"
#include "ui.h"
#include "dom.h"

// Window Builder: design AUI windows. Pick widgets from the palette, see
// them live, arrange them in the outline, and set their attributes.

static const char page[] =
    "<window title='Window Builder' width='1180' height='700' padding='0' spacing='0' onclose='quit'>"
    "  <menubar>"
    "    <menu text='File'>"
    "      <item text='New' shortcut='Ctrl+N' onclick='new'/>"
    "      <item text='Open...' shortcut='Ctrl+O' onclick='open'/>"
    "      <item text='Save' shortcut='Ctrl+S' onclick='save'/>"
    "      <item text='Save as...' shortcut='Ctrl+Shift+S' onclick='saveas'/>"
    "      <separator/>"
    "      <item text='Try the window' shortcut='F5' onclick='test'/>"
    "      <separator/>"
    "      <item text='Quit' shortcut='Ctrl+Q' onclick='quit'/>"
    "    </menu>"
    "    <menu text='Edit'>"
    "      <item text='Undo' shortcut='Ctrl+Z' onclick='undo'/>"
    "      <item text='Duplicate' shortcut='Ctrl+D' onclick='duplicate'/>"
    "      <item text='Delete' shortcut='Delete' onclick='delete'/>"
    "      <separator/>"
    "      <item text='Move up' shortcut='Alt+Up' onclick='up'/>"
    "      <item text='Move down' shortcut='Alt+Down' onclick='down'/>"
    "      <item text='Move out of container' shortcut='Alt+Left' onclick='out'/>"
    "      <item text='Move into the one above' shortcut='Alt+Right' onclick='in'/>"
    "    </menu>"
    "  </menubar>"
    "  <hbox expand='1' padding='8' spacing='8'>"
    "    <vbox width='170' spacing='6'>"
    "      <label text='Add' bold='true'/>"
    "      <list id='palette' expand='1' onactivate='add'/>"
    "      <button text='Add selected' onclick='add'/>"
    "    </vbox>"
    "    <tabs id='center' expand='1' onchange='centertab'>"
    "      <tab title='Design' padding='0'>"
    "        <scroll id='stage' expand='1'>"
    "          <vbox padding='24'>"
    "            <card id='frame' padding='0' spacing='0' align='start'>"
    "              <label id='frametitle' bold='true' text='Window'/>"
    "              <separator/>"
    "              <vbox id='preview'/>"
    "            </card>"
    "          </vbox>"
    "        </scroll>"
    "      </tab>"
    "      <tab title='AUI source'>"
    "        <textarea id='source' expand='1' mono='true' tabfocus='true'/>"
    "        <hbox spacing='8'><label id='sourcemsg' dim='true' align='center'/><spacer/>"
    "          <button text='Apply source' onclick='applysource'/></hbox>"
    "      </tab>"
    "    </tabs>"
    "    <vbox width='300' spacing='8'>"
    "      <label text='Outline' bold='true'/>"
    "      <list id='outline' expand='1' onselect='outline'/>"
    "      <label id='seltitle' text='Attributes' bold='true'/>"
    "      <table id='attrs' height='170' expand='0' columns='Attribute:110|Value' onselect='attrsel'/>"
    "      <hbox spacing='6'>"
    "        <dropdown id='attrname' width='120' onchange='attrpick'/>"
    "        <input id='attrvalue' expand='1' placeholder='Value' onactivate='setattr'/>"
    "      </hbox>"
    "      <hbox spacing='6'>"
    "        <button text='Set' onclick='setattr'/>"
    "        <button text='Remove' onclick='unsetattr'/>"
    "      </hbox>"
    "    </vbox>"
    "  </hbox>"
    "  <statusbar><label id='status'/></statusbar>"
    "</window>";

// The palette: what can be added, with starting attributes.
static const struct {
    const char *tag, *label, *attrs;
} palette[] = {
    { "vbox", "Column (vbox)", "spacing=8" },
    { "hbox", "Row (hbox)", "spacing=8" },
    { "grid", "Grid", "columns=2|spacing=8" },
    { "group", "Group", "title=Group" },
    { "tabs", "Tabs", "" },
    { "scroll", "Scroll area", "" },
    { "h1", "Heading", "text=Heading" },
    { "label", "Label", "text=Label" },
    { "p", "Paragraph", "text=A paragraph of text that wraps when the window is narrow." },
    { "link", "Link", "text=Link" },
    { "button", "Button", "text=Button" },
    { "input", "Text field", "placeholder=Type here" },
    { "password", "Password field", "" },
    { "textarea", "Text area", "height=120" },
    { "checkbox", "Check box", "text=Option" },
    { "radio", "Radio button", "text=Choice" },
    { "toggle", "Switch", "text=Setting" },
    { "dropdown", "Dropdown", "" },
    { "list", "List", "height=120" },
    { "table", "Table", "columns=Name|Value|height=140" },
    { "slider", "Slider", "value=50" },
    { "progress", "Progress bar", "value=40" },
    { "spin", "Number field", "value=1" },
    { "image", "Image", "src=/usr/share/backgrounds/Sea.png|width=200|height=125|scale=fit" },
    { "separator", "Separator", "" },
    { "spacer", "Spacer", "" },
    { "canvas", "Canvas", "height=120" },
    { "menubar", "Menu bar", "" },
    { "toolbar", "Toolbar", "" },
    { "statusbar", "Status bar", "" },
};
#define NPALETTE ((int)(sizeof(palette) / sizeof(palette[0])))

static const char *const common_attrs[] = {
    "text", "id", "title", "expand", "width", "height", "padding", "spacing", "align", "textalign",
    "justify", "placeholder", "value", "min", "max", "step", "checked", "disabled", "hidden", "default",
    "cancel", "flat", "symbol", "appicon", "wrap", "mono", "dim", "bold", "size", "columns", "stretch",
    "src", "scale", "shortcut", "onclick", "onchange", "onactivate", "onselect", "resizable", NULL,
};

static struct ui_window *win;
static struct node *doc;
static int selected_id;
static char path[512];
static bool dirty;
static struct widget *selected_widget;
static char *undo_text[32];
static int nundo;

// ---- Helpers ----

static struct node *selected(void)
{
    struct node *n = dom_find(doc, selected_id);

    return n ? n : doc;
}

static void set_status(const char *s)
{
    ui_set_text(ui_get(win, "status"), s);
}

static void update_title(void)
{
    char t[600];
    const char *base = strrchr(path, '/');

    snprintf(t, sizeof(t), "%s%s - Window Builder", dirty ? "*" : "", *path ? (base ? base + 1 : path) : "Untitled");
    ui_window_set_title(win, t);
}

static void remember(void)
{
    // Undo keeps whole documents; they are small.
    if (nundo == 32) {
        free(undo_text[0]);
        memmove(undo_text, undo_text + 1, sizeof(char *) * 31);
        nundo--;
    }
    undo_text[nundo++] = dom_serialize(doc, false);
}

// ---- Showing the document ----

static struct widget *find_preview(struct widget *w, int id)
{
    const char *a = ui_attr(w, "__wb");

    if (a && atoi(a) == id)
        return w;
    for (int i = 0; i < ui_children(w); i++) {
        struct widget *f = find_preview(ui_child(w, i), id);

        if (f)
            return f;
    }
    return NULL;
}

static void outline_add(struct widget *list, struct node *n, int depth, int *rows, int *ids)
{
    char row[256], indent[64] = "";
    const char *id = dom_get(n, "id"), *text = dom_get(n, "text") ? dom_get(n, "text") : dom_get(n, "title");

    for (int i = 0; i < depth && i < 20; i++)
        strlcat(indent, "    ", sizeof(indent));
    snprintf(row, sizeof(row), "%s%s%s%s%s%s%s", indent, n->tag, id ? "  #" : "", id ? id : "", text ? "  \"" : "",
             text ? text : "", text ? "\"" : "");
    if (strlen(row) > 60)
        strcpy(row + 57, "...");
    ids[*rows] = n->id;
    ui_list_add(list, row);
    if (n->id == selected_id)
        ui_list_select(list, *rows);
    (*rows)++;
    for (struct node *c = n->first; c && *rows < 512; c = c->next)
        outline_add(list, c, depth + 1, rows, ids);
}

static int outline_ids[512];

static void show_attrs(void)
{
    struct node *n = selected();
    struct widget *t = ui_get(win, "attrs");
    char title[64];

    snprintf(title, sizeof(title), "Attributes of <%s>", n->tag);
    ui_set_text(ui_get(win, "seltitle"), title);
    ui_list_clear(t);
    for (int i = 0; i < n->nattrs; i++) {
        char row[600];

        snprintf(row, sizeof(row), "%s\t%s", n->names[i], n->values[i]);
        ui_list_add(t, row);
    }
}

static void rebuild(void)
{
    struct widget *preview = ui_get(win, "preview"), *list = ui_get(win, "outline");
    char *aui;
    int rows = 0;
    const char *w, *h, *title;

    // The preview: the window's contents, parsed by the real toolkit.
    while (ui_children(preview))
        ui_remove(ui_child(preview, 0));
    aui = dom_serialize(doc, true);
    if (*aui)
        ui_parse_into(preview, aui);
    free(aui);
    ui_set_attr(preview, "padding", dom_get(doc, "padding") ? dom_get(doc, "padding") : "0");
    ui_set_attr(preview, "spacing", dom_get(doc, "spacing") ? dom_get(doc, "spacing") : "0");
    w = dom_get(doc, "width");
    h = dom_get(doc, "height");
    ui_set_attr(ui_get(win, "frame"), "width", w ? w : "-1");
    ui_set_attr(preview, "height", h ? h : "-1");
    if (!w)
        ui_set_attr(ui_get(win, "frame"), "minwidth", "200");
    title = dom_get(doc, "title");
    ui_set_text(ui_get(win, "frametitle"), title ? title : "Window");
    selected_widget = find_preview(preview, selected_id);
    // The outline.
    ui_list_clear(list);
    outline_add(list, doc, 0, &rows, outline_ids);
    show_attrs();
    update_title();
    ui_window_redraw(win);
}

static void changed(void)
{
    dirty = true;
    rebuild();
}

static void select_node(int id)
{
    selected_id = id;
    rebuild();
}

// ---- The preview takes clicks to select ----

static bool in_preview(struct widget *w)
{
    struct widget *preview = ui_get(win, "preview");

    for (; w; w = ui_parent(w))
        if (w == preview)
            return true;
    return false;
}

static bool preview_pointer(struct ui_window *wnd, struct widget *hit, struct wm_event *ev, void *u)
{
    (void)wnd;
    (void)u;
    if (!in_preview(hit) || ev->kind == WM_PTR_WHEEL)
        return false;
    if (ev->kind == WM_PTR_DOWN) {
        for (struct widget *w = hit; w; w = ui_parent(w)) {
            const char *id = ui_attr(w, "__wb");

            if (id) {
                select_node(atoi(id));
                return true;
            }
        }
        select_node(doc->id);
    }
    return true;
}

static void overlay(struct ui_window *wnd, struct gfx *g, void *u)
{
    struct rect r;

    (void)wnd;
    (void)u;
    if (!selected_widget || !ui_visible(selected_widget))
        return;
    r = ui_rect(selected_widget);
    gfx_outline_rounded(g, (struct rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 }, 3, 2, ui_theme.accent);
    gfx_fill_rounded(g, (struct rect){ r.x - 2, r.y - 18, MIN(r.w + 4, 120), 16 }, 3, ui_theme.accent);
    text_draw(g, font_get(FONT_SANS, 11), r.x + 2, r.y - 18, ui_tag(selected_widget), -1, ui_theme.accent_text);
}

// ---- Editing ----

static void apply_defaults(struct node *n, const char *attrs)
{
    char copy[256], *p = copy;

    strlcpy(copy, attrs, sizeof(copy));
    while (p && *p) {
        char *bar = strchr(p, '|'), *eq;

        if (bar)
            *bar = 0;
        if ((eq = strchr(p, '='))) {
            *eq = 0;
            dom_set(n, p, eq + 1);
        }
        p = bar ? bar + 1 : NULL;
    }
}

static struct node *make(int i)
{
    struct node *n = dom_new(palette[i].tag), *c;

    if (!n)
        return NULL;
    apply_defaults(n, palette[i].attrs);
    // Some widgets come with content so they show up.
    if (!strcmp(n->tag, "tabs")) {
        for (int k = 0; k < 2; k++) {
            struct node *t = dom_new("tab"), *l = dom_new("label");

            dom_set(t, "title", k ? "Second" : "First");
            dom_set(l, "text", k ? "Second page" : "First page");
            dom_append(t, l);
            dom_append(n, t);
        }
    } else if (!strcmp(n->tag, "dropdown") || !strcmp(n->tag, "list") || !strcmp(n->tag, "table")) {
        for (int k = 0; k < 3; k++) {
            char text[32];

            c = dom_new(!strcmp(n->tag, "dropdown") ? "option" : "item");
            snprintf(text, sizeof(text), !strcmp(n->tag, "table") ? "Row %d\tValue" : "Item %d", k + 1);
            dom_set(c, "text", text);
            dom_append(n, c);
        }
    } else if (!strcmp(n->tag, "menubar")) {
        struct node *m = dom_new("menu"), *it = dom_new("item");

        dom_set(m, "text", "File");
        dom_set(it, "text", "Quit");
        dom_append(m, it);
        dom_append(n, m);
    } else if (!strcmp(n->tag, "toolbar")) {
        c = dom_new("button");
        dom_set(c, "text", "Action");
        dom_set(c, "flat", "true");
        dom_append(n, c);
    } else if (!strcmp(n->tag, "statusbar")) {
        c = dom_new("label");
        dom_set(c, "text", "Ready");
        dom_append(n, c);
    } else if (!strcmp(n->tag, "group") || !strcmp(n->tag, "scroll")) {
        c = dom_new("label");
        dom_set(c, "text", "Contents");
        dom_append(n, c);
    }
    return n;
}

static bool holds_widgets(struct node *n)
{
    // Lists and menus hold items, not widgets.
    return dom_is_container(n->tag) && strcmp(n->tag, "list") && strcmp(n->tag, "table")
           && strcmp(n->tag, "dropdown") && strcmp(n->tag, "menubar") && strcmp(n->tag, "menu");
}

static void on_add(struct widget *w, void *u)
{
    int i = ui_list_selected(ui_get(win, "palette"));
    struct node *sel = selected(), *n;

    (void)w;
    (void)u;
    if (i < 0 || !(n = make(i)))
        return;
    remember();
    // Into the selected container, or after the selected widget.
    if (holds_widgets(sel) && strcmp(sel->tag, "tabs"))
        dom_append(sel, n);
    else if (sel->parent)
        dom_insert_after(sel, n);
    else
        dom_append(doc, n);
    selected_id = n->id;
    changed();
}

static void on_delete(struct widget *w, void *u)
{
    struct node *n = selected();

    (void)w;
    (void)u;
    if (n == doc)
        return;
    remember();
    selected_id = n->parent ? n->parent->id : doc->id;
    dom_detach(n);
    dom_free(n);
    changed();
}

static void on_duplicate(struct widget *w, void *u)
{
    struct node *n = selected(), *c;

    (void)w;
    (void)u;
    if (n == doc || !(c = dom_clone(n)))
        return;
    remember();
    dom_set(c, "id", "");
    dom_unset(c, "id");
    dom_insert_after(n, c);
    selected_id = c->id;
    changed();
}

static void move(int how)
{
    struct node *n = selected(), *prev, *parent;

    if (n == doc || !(parent = n->parent))
        return;
    prev = dom_prev(n);
    remember();
    switch (how) {
    case 0:                         // up
        if (!prev)
            return;
        dom_detach(n);
        if (dom_prev(prev))
            dom_insert_after(dom_prev(prev), n);
        else {
            n->parent = parent;
            n->next = parent->first;
            parent->first = n;
        }
        break;
    case 1:                         // down
        if (!n->next)
            return;
        {
            struct node *after = n->next;

            dom_detach(n);
            dom_insert_after(after, n);
        }
        break;
    case 2:                         // out of the container
        if (parent == doc)
            return;
        dom_detach(n);
        dom_insert_after(parent, n);
        break;
    case 3:                         // into the container above
        if (!prev || !holds_widgets(prev))
            return;
        dom_detach(n);
        dom_append(prev, n);
        break;
    }
    changed();
}

static void on_up(struct widget *w, void *u) { (void)w; (void)u; move(0); }
static void on_down(struct widget *w, void *u) { (void)w; (void)u; move(1); }
static void on_out(struct widget *w, void *u) { (void)w; (void)u; move(2); }
static void on_in(struct widget *w, void *u) { (void)w; (void)u; move(3); }

static void on_undo(struct widget *w, void *u)
{
    char err[160];
    struct node *n;

    (void)w;
    (void)u;
    if (!nundo)
        return;
    if ((n = dom_parse(undo_text[nundo - 1], err, sizeof(err)))) {
        dom_free(doc);
        doc = n;
        selected_id = doc->id;
    }
    free(undo_text[--nundo]);
    changed();
}

static void on_outline(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i >= 0 && i < 512 && outline_ids[i] != selected_id)
        select_node(outline_ids[i]);
}

static void on_attrsel(struct widget *w, void *u)
{
    struct node *n = selected();
    int i = ui_list_selected(w);
    struct widget *names = ui_get(win, "attrname");

    (void)u;
    if (i < 0 || i >= n->nattrs)
        return;
    for (int k = 0; k < ui_list_count(names); k++)
        if (!strcmp(ui_list_item(names, k), n->names[i]))
            ui_list_select(names, k);
    ui_set_text(ui_get(win, "attrvalue"), n->values[i]);
    ui_focus(ui_get(win, "attrvalue"));
}

static void on_attrpick(struct widget *w, void *u)
{
    const char *v = dom_get(selected(), ui_list_item(w, ui_list_selected(w)));

    (void)u;
    ui_set_text(ui_get(win, "attrvalue"), v ? v : "");
    ui_focus(ui_get(win, "attrvalue"));
}

static void on_setattr(struct widget *w, void *u)
{
    struct widget *names = ui_get(win, "attrname");
    const char *name = ui_list_item(names, ui_list_selected(names));

    (void)w;
    (void)u;
    if (!name)
        return;
    remember();
    dom_set(selected(), name, ui_text(ui_get(win, "attrvalue")));
    changed();
}

static void on_unsetattr(struct widget *w, void *u)
{
    struct widget *names = ui_get(win, "attrname");
    const char *name = ui_list_item(names, ui_list_selected(names));

    (void)w;
    (void)u;
    if (!name || !dom_get(selected(), name))
        return;
    remember();
    dom_unset(selected(), name);
    changed();
}

// ---- Source view ----

static void on_centertab(struct widget *w, void *u)
{
    (void)u;
    if (ui_value(w) == 1) {
        char *aui = dom_serialize(doc, false);

        ui_set_text(ui_get(win, "source"), aui);
        free(aui);
        ui_set_text(ui_get(win, "sourcemsg"), "Edit the AUI, then apply it.");
    }
}

static void on_applysource(struct widget *w, void *u)
{
    char err[200];
    struct node *n;

    (void)w;
    (void)u;
    if (!(n = dom_parse(ui_text(ui_get(win, "source")), err, sizeof(err)))) {
        ui_set_text(ui_get(win, "sourcemsg"), err);
        return;
    }
    remember();
    dom_free(doc);
    doc = n;
    selected_id = doc->id;
    ui_set_text(ui_get(win, "sourcemsg"), "Applied.");
    changed();
}

// ---- Files ----

static const char template_aui[] =
    "<window title=\"My window\" width=\"420\" padding=\"16\" spacing=\"10\">\n"
    "  <h1 text=\"Hello\"/>\n"
    "  <p text=\"Pick widgets on the left to add them here.\"/>\n"
    "  <hbox justify=\"end\" spacing=\"8\">\n"
    "    <button text=\"OK\" default=\"true\"/>\n"
    "  </hbox>\n"
    "</window>\n";

static bool load(const char *p)
{
    int fd = open(p, O_RDONLY);
    char *text, err[200], msg[400];
    struct aegis_stat st;
    struct node *n;
    ssize_t got = 0, r;

    if (fd < 0 || fstat(fd, &st) < 0 || !(text = malloc(st.size + 1))) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    while (got < (ssize_t)st.size && (r = read(fd, text + got, st.size - got)) > 0)
        got += r;
    close(fd);
    text[got] = 0;
    n = dom_parse(text, err, sizeof(err));
    free(text);
    if (!n) {
        snprintf(msg, sizeof(msg), "\"%s\" could not be read: %s", p, err);
        ui_message(win, "Window Builder", msg, "OK");
        return false;
    }
    dom_free(doc);
    doc = n;
    selected_id = doc->id;
    strlcpy(path, p, sizeof(path));
    dirty = false;
    while (nundo)
        free(undo_text[--nundo]);
    rebuild();
    return true;
}

static bool write_to(const char *p)
{
    char *aui = dom_serialize(doc, false), tmp[600], msg[700];
    int fd;
    size_t len = strlen(aui);

    snprintf(tmp, sizeof(tmp), "%s.saving", p);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 || write(fd, aui, len) != (ssize_t)len) {
        if (fd >= 0)
            close(fd);
        free(aui);
        snprintf(msg, sizeof(msg), "\"%s\" could not be saved: %s.", p, strerror(errno));
        ui_message(win, "Window Builder", msg, "OK");
        return false;
    }
    close(fd);
    free(aui);
    if (rename(tmp, p) < 0)
        return false;
    strlcpy(path, p, sizeof(path));
    dirty = false;
    update_title();
    set_status("Saved.");
    return true;
}

static bool save_as(void)
{
    char *p = ui_file_dialog_filtered(win, "Save window", NULL, true, "window.aui", "*.aui");
    bool ok;

    if (!p)
        return false;
    ok = write_to(p);
    free(p);
    return ok;
}

static bool save(void)
{
    return *path ? write_to(path) : save_as();
}

static bool settle(void)
{
    int r;

    if (!dirty)
        return true;
    r = ui_message(win, "Window Builder", "Save the changes to this window?", "Save|Don't save|Cancel");
    return r == 0 ? save() : r == 1;
}

static void on_new(struct widget *w, void *u)
{
    char err[100];

    (void)w;
    (void)u;
    if (!settle())
        return;
    dom_free(doc);
    doc = dom_parse(template_aui, err, sizeof(err));
    selected_id = doc->id;
    path[0] = 0;
    dirty = false;
    rebuild();
}

static void on_open(struct widget *w, void *u)
{
    char *p;

    (void)w;
    (void)u;
    if (!settle() || !(p = ui_file_dialog_filtered(win, "Open a window", NULL, false, NULL, "*.aui")))
        return;
    load(p);
    free(p);
}

static void on_save(struct widget *w, void *u) { (void)w; (void)u; save(); }
static void on_saveas(struct widget *w, void *u) { (void)w; (void)u; save_as(); }

static void on_quit(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (settle())
        ui_quit(0);
}

static void on_test(struct widget *w, void *u)
{
    char dir[64], file[96], script[600];
    struct aegis_stat st;

    (void)w;
    (void)u;
    // An App Maker project (app.aui next to app.as) runs with its code.
    if (*path && strlen(path) > 8 && !strcmp(path + strlen(path) - 8, "/app.aui")) {
        snprintf(script, sizeof(script), "%.*s/app.as", (int)(strlen(path) - 8), path);
        if (stat(script, &st) == 0 && save()) {
            char folder[512];

            snprintf(folder, sizeof(folder), "%.*s", (int)(strlen(path) - 8), path);
            launch("/bin/apprun", folder);
            return;
        }
    }
    snprintf(dir, sizeof(dir), "/tmp/builder-%d", getpid());
    mkdir(dir, 0700);
    snprintf(file, sizeof(file), "%s/app.aui", dir);
    {
        char *aui = dom_serialize(doc, false);
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (fd >= 0) {
            write(fd, aui, strlen(aui));
            close(fd);
        }
        free(aui);
    }
    launch("/bin/apprun", dir);
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "new", on_new }, { "open", on_open }, { "save", on_save }, { "saveas", on_saveas }, { "quit", on_quit },
        { "test", on_test }, { "undo", on_undo }, { "duplicate", on_duplicate }, { "delete", on_delete },
        { "up", on_up }, { "down", on_down }, { "out", on_out }, { "in", on_in }, { "add", on_add },
        { "outline", on_outline }, { "attrsel", on_attrsel }, { "attrpick", on_attrpick },
        { "setattr", on_setattr }, { "unsetattr", on_unsetattr }, { "centertab", on_centertab },
        { "applysource", on_applysource }, { NULL, NULL },
    };
    char err[100];
    struct widget *pal, *names;

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "builder")))
        return 1;
    pal = ui_get(win, "palette");
    for (int i = 0; i < NPALETTE; i++)
        ui_list_add(pal, palette[i].label);
    ui_list_select(pal, 0);
    names = ui_get(win, "attrname");
    for (int i = 0; common_attrs[i]; i++)
        ui_list_add(names, common_attrs[i]);
    ui_list_select(names, 0);
    ui_on_pointer(win, preview_pointer, NULL);
    ui_window_set_overlay(win, overlay, NULL);
    doc = dom_parse(template_aui, err, sizeof(err));
    selected_id = doc->id;
    if (argc > 1) {
        struct aegis_stat st;

        if (stat(argv[1], &st) == 0)
            load(argv[1]);
        else
            strlcpy(path, argv[1], sizeof(path));      // a new file there
    }
    rebuild();
    set_status("Click a widget to select it. Double-click a palette entry to add it.");
    ui_window_show(win);
    return ui_run();
}
