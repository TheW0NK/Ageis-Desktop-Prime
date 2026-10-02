#include "aegis.h"
#include "ui.h"

// A tour of the UI toolkit: every widget, menus, dialogs and themes.

static const char page[] =
    "<window title='AUI widget tour' width='760' height='520' onclose='quit'>"
    "  <menubar>"
    "    <menu text='File'>"
    "      <item text='Open...' shortcut='Ctrl+O' onclick='open'/>"
    "      <item text='Save as...' shortcut='Ctrl+S' onclick='save'/>"
    "      <separator/>"
    "      <menu text='Theme'>"
    "        <item text='Light' onclick='light'/>"
    "        <item text='Dark' onclick='dark'/>"
    "        <item text='High contrast' onclick='contrast'/>"
    "      </menu>"
    "      <separator/>"
    "      <item text='Quit' shortcut='Ctrl+Q' onclick='quit'/>"
    "    </menu>"
    "    <menu text='Help'>"
    "      <item text='About' shortcut='F1' onclick='about'/>"
    "    </menu>"
    "  </menubar>"
    "  <toolbar>"
    "    <button text='Open' flat='true' onclick='open'/>"
    "    <button text='Ask name' flat='true' onclick='ask'/>"
    "    <separator/>"
    "    <button text='Dark' flat='true' onclick='dark'/>"
    "    <button text='Light' flat='true' onclick='light'/>"
    "  </toolbar>"
    "  <tabs id='tabs' expand='1' onchange='tab'>"
    "    <tab title='Controls'>"
    "      <h2 text='Form controls'/>"
    "      <grid columns='2' spacing='10'>"
    "        <label text='Name'/>"
    "        <input id='name' placeholder='Type your name' onactivate='greet' onchange='typed'/>"
    "        <label text='Password'/>"
    "        <password id='pw'/>"
    "        <label text='Size'/>"
    "        <dropdown id='size' onchange='size'>"
    "          <option>Small</option><option selected='true'>Medium</option><option>Large</option>"
    "        </dropdown>"
    "        <label text='Copies'/>"
    "        <spin id='copies' value='2' min='1' max='99'/>"
    "        <label text='Volume'/>"
    "        <slider id='volume' value='40' onchange='volume'/>"
    "      </grid>"
    "      <hbox spacing='16'>"
    "        <checkbox id='check' text='Remember me' checked='true'/>"
    "        <radio text='Red' checked='true'/><radio text='Green'/><radio text='Blue'/>"
    "        <toggle id='wifi' text='Wi-Fi' onchange='wifi'/>"
    "      </hbox>"
    "      <progress id='progress' value='40'/>"
    "      <p id='status' dim='true'>Ready. This paragraph wraps when the window is too narrow"
    "         to hold it on one line, like text in a web page.</p>"
    "      <spacer/>"
    "      <hbox justify='end'>"
    "        <button text='Cancel' cancel='true' onclick='cancel'/>"
    "        <button text='Greet' default='true' onclick='greet'/>"
    "      </hbox>"
    "    </tab>"
    "    <tab title='Lists'>"
    "      <hbox expand='1' spacing='10'>"
    "        <list id='fruit' width='180' onselect='fruit' onactivate='fruitgo'>"
    "          <item>Apple</item><item>Banana</item><item>Cherry</item><item>Damson</item>"
    "          <item>Elderberry</item><item>Fig</item><item>Grape</item><item>Honeydew</item>"
    "        </list>"
    "        <table id='procs' expand='1' columns='Process|PID:70:right|Memory:100:right' onsort='sort'/>"
    "      </hbox>"
    "    </tab>"
    "    <tab title='Text'>"
    "      <textarea id='notes' expand='1' wrap='true'>Notes go here.\n\n"
    "Select text with the mouse or with Shift and the arrow keys; Ctrl+C, Ctrl+X and Ctrl+V"
    " use the clipboard, Ctrl+Z undoes.\n\tTabs line up.</textarea>"
    "    </tab>"
    "    <tab title='Canvas'>"
    "      <canvas id='canvas' expand='1'/>"
    "    </tab>"
    "  </tabs>"
    "  <statusbar>"
    "    <label id='where' text='Controls'/>"
    "    <spacer/>"
    "    <label id='clock' dim='true'/>"
    "  </statusbar>"
    "</window>";

static struct ui_window *win;
static int dots[64][2], ndots;

static void status(const char *s)
{
    ui_set_text(ui_get(win, "status"), s);
}

static void quit(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(win, "Quit", "Close the widget tour?", "Quit|Stay") == 0)
        ui_quit(0);
}

static void greet(struct widget *w, void *u)
{
    char buf[200];
    const char *name = ui_text(ui_get(win, "name"));

    (void)w;
    (void)u;
    snprintf(buf, sizeof(buf), "Hello, %s!", *name ? name : "whoever you are");
    status(buf);
}

static void typed(struct widget *w, void *u)
{
    char buf[200];

    (void)u;
    snprintf(buf, sizeof(buf), "Typed %d characters.", (int)strlen(ui_text(w)));
    status(buf);
}

static void cancel(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_set_text(ui_get(win, "name"), "");
    status("Cleared.");
}

static void volume(struct widget *w, void *u)
{
    char buf[64];

    (void)u;
    ui_set_value(ui_get(win, "progress"), ui_value(w));
    snprintf(buf, sizeof(buf), "Volume %.0f%%", ui_value(w));
    status(buf);
}

static void size_changed(struct widget *w, void *u)
{
    char buf[64];

    (void)u;
    snprintf(buf, sizeof(buf), "Size: %s", ui_list_item(w, ui_list_selected(w)));
    status(buf);
}

static void wifi(struct widget *w, void *u)
{
    (void)u;
    status(ui_value(w) ? "Wi-Fi on" : "Wi-Fi off");
}

static void fruit(struct widget *w, void *u)
{
    char buf[64];

    (void)u;
    snprintf(buf, sizeof(buf), "Picked %s", ui_list_item(w, ui_list_selected(w)));
    ui_set_text(ui_get(win, "where"), buf);
}

static void fruitgo(struct widget *w, void *u)
{
    char buf[64];

    (void)u;
    snprintf(buf, sizeof(buf), "You opened %s.", ui_list_item(w, ui_list_selected(w)));
    ui_message(win, "Fruit", buf, "OK");
}

static void fill_procs(void)
{
    struct aegis_procinfo info[64];
    struct widget *t = ui_get(win, "procs");
    int n = procinfo(info, 64);

    ui_list_clear(t);
    for (int i = 0; i < n; i++) {
        char row[160], mem[32];

        ui_format_size(info[i].memory, mem, sizeof(mem));
        snprintf(row, sizeof(row), "%s\t%d\t%s", info[i].name, info[i].pid, mem);
        ui_list_add(t, row);
    }
}

static void sort(struct widget *w, void *u)
{
    char buf[64];

    (void)u;
    snprintf(buf, sizeof(buf), "Sort by column %s", ui_attr(w, "sortcolumn"));
    ui_set_text(ui_get(win, "where"), buf);
}

static void tab(struct widget *w, void *u)
{
    static const char *const names[] = { "Controls", "Lists", "Text", "Canvas" };

    (void)u;
    ui_set_text(ui_get(win, "where"), names[(int)ui_value(w) & 3]);
    if (ui_value(w) == 1)
        fill_procs();
}

static void open_file(struct widget *w, void *u)
{
    char *path = ui_file_dialog(win, "Open a file", NULL, false, NULL);

    (void)w;
    (void)u;
    status(path ? path : "Nothing opened.");
    free(path);
}

static void save_file(struct widget *w, void *u)
{
    char *path = ui_file_dialog(win, "Save as", NULL, true, "untitled.txt");

    (void)w;
    (void)u;
    status(path ? path : "Not saved.");
    free(path);
}

static void ask(struct widget *w, void *u)
{
    char *name = ui_prompt(win, "Your name", "What should I call you?", "Ada");

    (void)w;
    (void)u;
    if (name) {
        ui_set_text(ui_get(win, "name"), name);
        free(name);
    }
}

static void about(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_message(win, "About", "The AUI toolkit tour. Every widget on these pages comes from AUI markup.", "OK");
}

static void light(struct widget *w, void *u) { (void)w; (void)u; ui_set_theme("light"); }
static void dark(struct widget *w, void *u) { (void)w; (void)u; ui_set_theme("dark"); }
static void contrast(struct widget *w, void *u) { (void)w; (void)u; ui_set_theme("high-contrast"); }

static void paint(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    gfx_gradient(g, r, RGB(0x1B3A5C), RGB(0x2F6FE4));
    text_draw(g, font_get(FONT_SANS_BOLD, 18), r.x + 16, r.y + 14, "Click to drop dots", -1, RGB(0xFFFFFF));
    for (int i = 0; i < ndots; i++)
        gfx_circle(g, r.x + dots[i][0], r.y + dots[i][1], 9, ALPHA(0xFFD54A, 0xE0));
}

static void input(struct widget *w, struct wm_event *ev, void *u)
{
    (void)u;
    if (ev->type != WM_EV_POINTER || ev->kind != WM_PTR_DOWN || ndots == 64)
        return;
    dots[ndots][0] = ev->x;
    dots[ndots][1] = ev->y;
    ndots++;
    ui_redraw(w);
}

static bool tick(void *u)
{
    char buf[32];
    int64_t now = time(NULL);
    struct tm tm;

    (void)u;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    ui_set_text(ui_get(win, "clock"), buf);
    return true;
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "quit", quit }, { "greet", greet }, { "typed", typed }, { "cancel", cancel }, { "volume", volume },
        { "size", size_changed }, { "wifi", wifi }, { "fruit", fruit }, { "fruitgo", fruitgo }, { "sort", sort },
        { "tab", tab }, { "open", open_file }, { "save", save_file }, { "ask", ask }, { "about", about },
        { "light", light }, { "dark", dark }, { "contrast", contrast }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "auidemo")))
        return 1;
    ui_canvas_set(ui_get(win, "canvas"), paint, input, NULL);
    tick(NULL);
    ui_timer(1000, tick, NULL);
    return ui_run();
}
