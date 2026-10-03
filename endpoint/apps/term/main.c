#include "aegis.h"
#include "ui.h"
#include "vt.h"

// Terminal: runs the command shell in a pseudo-terminal.

#define PAD     6
#define BG      0x1B1F27
#define FG      0xD8DEE6

static const char page[] =
    "<window title='Terminal' width='780' height='480' padding='0'>"
    "  <canvas id='screen' focusable='true' wanttab='true' oncontext='menu'/>"
    "  <menu id='menu'>"
    "    <item text='Copy' shortcut='Ctrl+Shift+C' onclick='copy'/>"
    "    <item text='Paste' shortcut='Ctrl+Shift+V' onclick='paste'/>"
    "    <separator/>"
    "    <item text='Select all' onclick='selectall'/>"
    "    <item text='Clear scrollback' onclick='clear'/>"
    "  </menu>"
    "</window>";

static struct ui_window *win;
static struct widget *screen;
static struct vt vt;
static int master = -1, child = -1;
static int scroll;                  // lines scrolled back into history
static int cell_w, cell_h;
// Selection in absolute lines (0 = oldest history line).
static bool selecting, has_sel;
static int sel_x0, sel_y0, sel_x1, sel_y1;

static struct font *font(bool bold)
{
    return font_get(bold ? FONT_MONO_BOLD : FONT_MONO, 15);
}

static void to_shell(const char *s, size_t n)
{
    while (n > 0 && master >= 0) {
        ssize_t w = write(master, s, n);

        if (w <= 0)
            break;
        s += w;
        n -= w;
    }
}

static void fit(struct rect r)
{
    int cols = MAX(2, (r.w - 2 * PAD) / cell_w), rows = MAX(2, (r.h - 2 * PAD) / cell_h);

    if (cols != vt.cols || rows != vt.rows) {
        vt_resize(&vt, cols, rows);
        if (master >= 0)
            ioctl(master, IOCTL_PTY_SET_SIZE, ((unsigned long)rows << 16) | cols);
    }
}

static bool selected(int x, int line)
{
    int ax = sel_x0, ay = sel_y0, bx = sel_x1, by = sel_y1;

    if (!has_sel)
        return false;
    if (ay > by || (ay == by && ax > bx)) {
        int tx = ax, ty = ay;

        ax = bx, ay = by, bx = tx, by = ty;
    }
    if (line < ay || line > by)
        return false;
    if (line == ay && x < ax)
        return false;
    if (line == by && x >= bx)
        return false;
    return true;
}

static void paint(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    int first = vt.history_len - scroll;
    bool focused = true;

    (void)w;
    (void)u;
    fit(r);
    gfx_fill(g, r, RGB(BG));
    for (int y = 0; y < vt.rows; y++) {
        int line = first + y;
        const struct vt_cell *row = vt_history_line(&vt, line);
        int py = r.y + PAD + y * cell_h;

        for (int x = 0; x < vt.cols; x++) {
            const struct vt_cell *c = &row[x];
            uint32_t fg = c->fg == VT_DEFAULT ? FG : c->fg, bg = c->bg == VT_DEFAULT ? BG : c->bg;
            int px = r.x + PAD + x * cell_w;
            bool cursor = scroll == 0 && vt.cursor_visible && y == vt.cy && x == vt.cx && focused;

            if (c->flags & VT_INVERSE) {
                uint32_t t = fg;

                fg = bg;
                bg = t;
            }
            if (selected(x, line)) {
                bg = 0x3A5F9E;
                fg = 0xFFFFFF;
            }
            if (cursor && ui_is_focused_widget(screen)) {
                bg = 0xD8DEE6;
                fg = BG;
            }
            if (bg != BG)
                gfx_fill(g, (struct rect){ px, py, cell_w, cell_h }, RGB(bg));
            if (cursor && !ui_is_focused_widget(screen))
                gfx_outline(g, (struct rect){ px, py, cell_w, cell_h }, 1, RGB(0xD8DEE6));
            if (c->ch > ' ') {
                char buf[4];
                int n = utf8_encode(c->ch, buf);
                color_t col = (c->flags & VT_DIM) ? ALPHA(fg, 0xA0) : RGB(fg);

                text_draw(g, font(c->flags & VT_BOLD), px, py, buf, n, col);
            }
            if (c->flags & VT_UNDERLINE)
                gfx_fill(g, (struct rect){ px, py + cell_h - 2, cell_w, 1 }, RGB(fg));
        }
    }
    if (vt.history_len && scroll) {
        // Where we are in the scrollback.
        int total = vt.history_len + vt.rows, th = MAX(20, r.h * vt.rows / total);
        int ty = r.y + (r.h - th) * (vt.history_len - scroll) / MAX(1, vt.history_len);

        gfx_fill_rounded(g, (struct rect){ r.x + r.w - 6, ty, 4, th }, 2, ALPHA(0xFFFFFF, 0x60));
    }
}

static void selection_text(char **out)
{
    int ax = sel_x0, ay = sel_y0, bx = sel_x1, by = sel_y1;
    size_t cap = 256, len = 0;
    char *s = malloc(cap);

    *out = NULL;
    if (!s || !has_sel)
        return free(s);
    if (ay > by || (ay == by && ax > bx)) {
        int tx = ax, ty = ay;

        ax = bx, ay = by, bx = tx, by = ty;
    }
    for (int line = ay; line <= by; line++) {
        const struct vt_cell *row = vt_history_line(&vt, line);
        int x0 = line == ay ? ax : 0, x1 = line == by ? bx : vt.cols, end = x1;

        // Trailing blanks are not part of the text.
        while (end > x0 && row[end - 1].ch == ' ')
            end--;
        for (int x = x0; x < end; x++) {
            if (len + 6 > cap && !(s = realloc(s, cap *= 2)))
                return;
            len += utf8_encode(row[x].ch ? row[x].ch : ' ', s + len);
        }
        if (line != by) {
            if (len + 2 > cap && !(s = realloc(s, cap *= 2)))
                return;
            s[len++] = '\n';
        }
    }
    s[len] = 0;
    *out = s;
}

static void copy(struct widget *w, void *u)
{
    char *t;

    (void)w;
    (void)u;
    selection_text(&t);
    if (t && *t)
        ui_clipboard_set(t);
    free(t);
}

static void paste(struct widget *w, void *u)
{
    const char *t = ui_clipboard_get();

    (void)w;
    (void)u;
    for (; *t; t++)
        to_shell(*t == '\n' ? "\r" : t, 1);
    scroll = 0;
    ui_redraw(screen);
}

static void select_all(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    has_sel = true;
    sel_x0 = 0;
    sel_y0 = 0;
    sel_x1 = vt.cols;
    sel_y1 = vt.history_len + vt.rows - 1;
    ui_redraw(screen);
}

static void clear_history(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    vt.history_len = 0;
    scroll = 0;
    has_sel = false;
    ui_redraw(screen);
}

static void context_menu(struct widget *w, void *u)
{
    (void)u;
    ui_menu_popup(ui_get(win, "menu"), w, -1, -1);
}

// Keys to the bytes a terminal sends.
static void key(struct wm_event *ev)
{
    bool ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT, alt = ev->mods & MOD_ALT;
    const char *seq = NULL;
    char buf[8];

    if (!ev->value)
        return;
    if (ctrl && shift && ev->key == KEY_A + 2) {        // Ctrl+Shift+C
        copy(NULL, NULL);
        return;
    }
    if ((ctrl && shift && ev->key == KEY_A + 21) || (shift && ev->key == KEY_INSERT)) {    // Ctrl+Shift+V
        paste(NULL, NULL);
        return;
    }
    if (shift && (ev->key == KEY_PAGEUP || ev->key == KEY_PAGEDOWN)) {
        int page = vt.rows - 1;

        scroll = MIN(MAX(scroll + (ev->key == KEY_PAGEUP ? page : -page), 0), vt.history_len);
        ui_redraw(screen);
        return;
    }
    switch (ev->key) {
    case KEY_ENTER: case KEY_KPENTER: seq = "\r"; break;
    case KEY_BACKSPACE: seq = ctrl ? "\x08" : "\x7f"; break;
    case KEY_TAB: seq = shift ? "\x1b[Z" : "\t"; break;
    case KEY_ESC: seq = "\x1b"; break;
    case KEY_UP: seq = "\x1b[A"; break;
    case KEY_DOWN: seq = "\x1b[B"; break;
    case KEY_RIGHT: seq = ctrl ? "\x1b[1;5C" : "\x1b[C"; break;
    case KEY_LEFT: seq = ctrl ? "\x1b[1;5D" : "\x1b[D"; break;
    case KEY_HOME: seq = "\x1b[H"; break;
    case KEY_END: seq = "\x1b[F"; break;
    case KEY_INSERT: seq = "\x1b[2~"; break;
    case KEY_DELETE: seq = "\x1b[3~"; break;
    case KEY_PAGEUP: seq = "\x1b[5~"; break;
    case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
    case KEY_F1: seq = "\x1bOP"; break;
    case KEY_F1 + 1: seq = "\x1bOQ"; break;
    case KEY_F1 + 2: seq = "\x1bOR"; break;
    case KEY_F1 + 3: seq = "\x1bOS"; break;
    }
    if (!seq && ctrl && ev->key >= KEY_A && ev->key < KEY_A + 26) {
        buf[0] = (char)(ev->key - KEY_A + 1);
        buf[1] = 0;
        seq = buf;
    } else if (!seq && ctrl && ev->key == KEY_LEFTBRACE) {
        seq = "\x1b";
    } else if (!seq && ev->text[0]) {
        if (alt) {
            snprintf(buf, sizeof(buf), "\x1b%s", ev->text);
            seq = buf;
        } else {
            seq = ev->text;
        }
    }
    if (seq) {
        to_shell(seq, strlen(seq));
        if (scroll || has_sel) {
            scroll = 0;
            has_sel = false;
            ui_redraw(screen);
        }
    }
}

static void cell_at(struct rect r, int px, int py, int *x, int *line)
{
    int cx = (px - PAD + cell_w / 2) / cell_w, cy = (py - PAD) / cell_h;

    (void)r;
    *x = MIN(MAX(cx, 0), vt.cols);
    *line = vt.history_len - scroll + MIN(MAX(cy, 0), vt.rows - 1);
}

static void input(struct widget *w, struct wm_event *ev, void *u)
{
    struct rect r = ui_rect(w);

    (void)u;
    if (ev->type == WM_EV_KEY) {
        key(ev);
        return;
    }
    if (ev->type != WM_EV_POINTER)
        return;
    switch (ev->kind) {
    case WM_PTR_WHEEL:
        scroll = MIN(MAX(scroll + ev->detail * 3, 0), vt.history_len);
        ui_redraw(w);
        break;
    case WM_PTR_DOWN:
        ui_focus(w);
        if (ev->detail == BTN_RIGHT) {
            context_menu(w, NULL);
        } else if (ev->detail == BTN_MIDDLE) {
            paste(NULL, NULL);
        } else if (ev->detail == BTN_LEFT) {
            int x, line;

            cell_at(r, ev->x, ev->y, &x, &line);
            if (ui_click_count(w) == 2) {
                // A word: letters, digits and path characters.
                const struct vt_cell *row = vt_history_line(&vt, line);
                int s = MIN(x, vt.cols - 1), e = s;

                while (s > 0 && row[s - 1].ch > ' ' && !strchr("\"'()[]{}<>", row[s - 1].ch))
                    s--;
                while (e < vt.cols && row[e].ch > ' ' && !strchr("\"'()[]{}<>", row[e].ch))
                    e++;
                sel_x0 = s, sel_y0 = line, sel_x1 = e, sel_y1 = line;
                has_sel = e > s;
            } else {
                sel_x0 = sel_x1 = x;
                sel_y0 = sel_y1 = line;
                has_sel = false;
                selecting = true;
            }
            ui_redraw(w);
        }
        break;
    case WM_PTR_MOVE:
        if (selecting) {
            cell_at(r, ev->x, ev->y, &sel_x1, &sel_y1);
            has_sel = sel_x0 != sel_x1 || sel_y0 != sel_y1;
            ui_redraw(w);
        }
        break;
    case WM_PTR_UP:
        if (selecting) {
            selecting = false;
            if (has_sel)
                copy(NULL, NULL);     // selecting copies, as on other terminals
        }
        break;
    }
}

static void output(int fd, void *u)
{
    char buf[16384];
    ssize_t n;
    int total = 0;

    (void)u;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        vt_write(&vt, buf, n);
        total += n;
        if (vt.reply_len) {
            to_shell(vt.reply, vt.reply_len);
            vt.reply_len = 0;
        }
        if (total > 1 << 20)
            break;      // let the window repaint during floods
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        // The shell has exited.
        ui_unwatch_fd(fd);
        close(master);
        master = -1;
        ui_quit(0);
        return;
    }
    if (vt.title_changed) {
        vt.title_changed = false;
        ui_window_set_title(win, vt.title);
    }
    if (vt.dirty) {
        vt.dirty = false;
        if (scroll)
            scroll = MIN(scroll, vt.history_len);
        ui_redraw(screen);
    }
}

static int start_shell(const char *cmd)
{
    int fds[2], saved[3];
    char *argv[4];

    if (openpty(fds, O_CLOEXEC) < 0)
        return -1;
    // The slave end must survive into the child; the master must not.
    fcntl(fds[1], F_SETFD, 0);
    for (int i = 0; i < 3; i++) {
        saved[i] = dup(i);
        dup2(fds[1], i);
    }
    setenv("TERM", "xterm-256color");
    if (cmd) {
        argv[0] = "terminal";
        argv[1] = "-c";
        argv[2] = (char *)cmd;
        argv[3] = NULL;
    } else {
        argv[0] = "terminal";
        argv[1] = "--no-login";
        argv[2] = NULL;
    }
    child = spawn("/sysapps/terminal", argv, environ);
    for (int i = 0; i < 3; i++) {
        dup2(saved[i], i);
        close(saved[i]);
    }
    close(fds[1]);
    if (child < 0) {
        close(fds[0]);
        return -1;
    }
    master = fds[0];
    fcntl(master, F_SETFL, O_NONBLOCK);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "copy", copy }, { "paste", paste }, { "selectall", select_all }, { "clear", clear_history },
        { "menu", context_menu }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (argc > 1 && strcmp(argv[1], "-c"))
        chdir(argv[1]);         // started on a folder (from Files)
    if (!(win = ui_load_string_named(page, handlers, NULL, "term")))
        return 1;
    screen = ui_get(win, "screen");
    cell_w = text_width(font(false), "M", 1);
    cell_h = font_line_height(font(false));
    if (!vt_init(&vt, 80, 24))
        return 1;
    ui_canvas_set(screen, paint, input, NULL);
    if (start_shell(argc > 2 && !strcmp(argv[1], "-c") ? argv[2] : NULL) < 0) {
        ui_message(NULL, "Terminal", "The shell could not be started.", "OK");
        return 1;
    }
    ui_watch_fd(master, output, NULL);
    ui_window_show(win);
    ui_focus(screen);
    ui_run();
    if (child > 0) {
        kill(child, SIGHUP);
        waitpid(child, NULL, 0);
    }
    return 0;
}
