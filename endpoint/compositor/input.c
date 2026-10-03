#include "compositor.h"

extern int hover_button;
extern struct window *hover_window;
int title_button_at(struct window *w, int x, int y);

enum drag { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

static void save_screenshot(void);

static enum drag drag;
static uint32_t mods;
static struct window *drag_window, *grab, *pointer_window, *press_window;
static int drag_x, drag_y, press_button;
static struct rect drag_start;
static uint32_t buttons;
static uint64_t last_resize_ms;
static bool meta_combo;             // another key went with Meta: its release is not "Meta alone"
int pointer_x, pointer_y;
struct dnd dnd = { .fd = -1 };
struct window *snap_window;
struct rect snap_preview;

// The edge snap a window dragged to the pointer's position would take:
// 1 maximize (top), TILE_LEFT + 1, TILE_RIGHT + 1, or 0.
static int snap_target(struct rect *where)
{
    int t = 0;

    if (pointer_x <= 1)
        t = 1 + TILE_LEFT;
    else if (pointer_x >= screen.width - 2)
        t = 1 + TILE_RIGHT;
    else if (pointer_y <= screen.work.y + 1)
        t = 1;
    if (where)
        *where = t == 1 ? screen.work : t ? tile_rect(t - 1) : (struct rect){ 0, 0, 0, 0 };
    return t;
}

static void set_snap_preview(struct window *w, struct rect r)
{
    if (snap_preview.w)
        damage(snap_preview);
    snap_window = w;
    snap_preview = r;
    if (r.w)
        damage(r);
}

// Restores a maximized or tiled window being dragged, keeping the same
// point of the title bar under the pointer.
static void unsnap_for_drag(struct window *w)
{
    int rel = (pointer_x - w->frame.x) * w->saved.w / MAX(1, w->frame.w);

    if (w->maximized)
        set_maximized(w, false);
    else
        set_tiled(w, TILE_NONE);
    damage_window(w);
    w->frame.x = pointer_x - rel;
    w->frame.y = pointer_y - TITLE_H / 2;
    damage_window(w);
}

// ---- Keyboard layout (US) ----

static const char plain[0x39] = {
    [0x04] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\r', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
};

static const char shifted[0x39] = {
    [0x04] = 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\r', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?',
};

// Printable text for a key press (control keys produce nothing).
void key_text(uint16_t code, uint32_t m, char *out)
{
    char c = 0;

    out[0] = 0;
    if (m & (MOD_CTRL | MOD_ALT | MOD_META))
        return;
    if (code < sizeof(plain) && code >= KEY_1 - 26) {
        c = (m & MOD_SHIFT) ? shifted[code] : plain[code];
        if ((m & MOD_CAPSLOCK) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c ^= 0x20;
        if ((unsigned char)c < 0x20)
            c = 0;
    } else if (code >= KEY_KP1 && code <= KEY_KPDOT && (m & MOD_NUMLOCK)) {
        c = code == KEY_KPDOT ? '.' : code == KEY_KP0 ? '0' : '1' + (code - KEY_KP1);
    } else {
        switch (code) {
        case KEY_KPSLASH:    c = '/'; break;
        case KEY_KPASTERISK: c = '*'; break;
        case KEY_KPMINUS:    c = '-'; break;
        case KEY_KPPLUS:     c = '+'; break;
        case KEY_102ND:      c = (m & MOD_SHIFT) ? '|' : '\\'; break;
        }
    }
    if (c) {
        out[0] = c;
        out[1] = 0;
    }
}

int input_open_device(void)
{
    int fd = open("/osystem/devices/input", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd >= 0)
        ioctl(fd, IOCTL_INPUT_GRAB, 1);
    return fd;
}

static void pointer_event(struct window *w, int type, uint32_t flags)
{
    struct rect c;

    if (!w || !w->owner)
        return;
    struct wm_msg m = { 0 };

    c = window_content(w);
    m.type = WM_POINTER;
    m.window = w->cid;
    m.a = pointer_x - c.x;
    m.b = pointer_y - c.y;
    m.c = buttons;
    m.d = type;
    m.flags = flags;
    m.parent = mods;
    send_msg(w->owner, &m);
}

static void update_hover(struct window *w)
{
    int b = w ? title_button_at(w, pointer_x, pointer_y) : 0;

    if (b != hover_button || (b && w != hover_window)) {
        if (hover_window)
            damage((struct rect){ hover_window->frame.x, hover_window->frame.y, hover_window->frame.w, TITLE_H });
        hover_button = b;
        hover_window = b ? w : NULL;
        if (w)
            damage((struct rect){ w->frame.x, w->frame.y, w->frame.w, TITLE_H });
    }
}

// ---- Drag and drop ----

static void dnd_send(struct window *w, uint32_t type, int a, int b, int c, int d)
{
    struct rect cr;

    if (!w || !w->owner)
        return;
    struct wm_msg m = { 0 };

    cr = window_content(w);
    m.type = type;
    m.window = w->cid;
    m.a = a == INT32_MIN ? pointer_x - cr.x : a;
    m.b = b == INT32_MIN ? pointer_y - cr.y : b;
    m.c = c;
    m.d = d;
    m.parent = mods;
    strlcpy(m.text, dnd.type, sizeof(m.text));
    // Drops, and windows a drag enters, get the data itself.
    if (type == WM_DROP || d == WM_DRAG_ENTER) {
        int fd = dnd.fd;

        m.flags = dnd.len;
        send_fds(w->owner->fd, &m, sizeof(m), &fd, 1);
        return;
    }
    send_msg(w->owner, &m);
}

static void dnd_cursor(void)
{
    static const int shapes[] = { CURSOR_DND_NONE, CURSOR_DND_COPY, CURSOR_DND_MOVE, 0, CURSOR_DND_LINK };

    set_cursor_shape(dnd.action > 0 && dnd.action <= WM_DND_LINK ? shapes[dnd.action] : CURSOR_DND_NONE);
}

// The window under the pointer hears about the drag; its answer
// (WM_DRAG_STATUS) decides the cursor.
static void dnd_motion(void)
{
    struct window *w = window_at(pointer_x, pointer_y);

    if (w && (!w->owner || !rect_contains(window_content(w), pointer_x, pointer_y)))
        w = NULL;
    if (dnd.released_ms)
        return;
    if (w != dnd.target) {
        dnd_send(dnd.target, WM_DRAG_OVER, INT32_MIN, INT32_MIN, dnd.actions, WM_DRAG_LEAVE);
        dnd.target = w;
        dnd.action = 0;
        dnd.unanswered = 0;
        dnd_send(w, WM_DRAG_OVER, INT32_MIN, INT32_MIN, dnd.actions, WM_DRAG_ENTER);
    } else {
        dnd_send(w, WM_DRAG_OVER, INT32_MIN, INT32_MIN, dnd.actions, WM_DRAG_MOVE);
    }
    if (w && w->owner)
        dnd.unanswered++;
    dnd_cursor();
}

// Starts a drag for the program holding the pointer. text is "type\nlabel".
bool dnd_start(struct window *w, int fd, int len, uint32_t actions, const char *text)
{
    const char *nl = strchr(text, '\n');

    if (dnd.active || !buttons || !grab || grab->owner != w->owner || drag != DRAG_NONE
        || !(actions & (WM_DND_COPY | WM_DND_MOVE | WM_DND_LINK)))
        return false;
    dnd = (struct dnd){ .active = true, .source = w, .fd = fd, .len = len, .actions = actions };
    snprintf(dnd.type, sizeof(dnd.type), "%.*s", nl ? (int)(nl - text) : (int)strlen(text), text);
    strlcpy(dnd.label, nl ? nl + 1 : "", sizeof(dnd.label));
    // The source no longer holds the pointer; windows under it get drag
    // messages instead of pointer events until the button is released.
    if (pointer_window)
        pointer_event(pointer_window, WM_PTR_LEAVE, 0);
    pointer_window = NULL;
    grab = NULL;
    update_hover(NULL);
    dnd_motion();
    return true;
}

static void dnd_finish(bool drop);

void dnd_status(struct window *w, int action)
{
    if (!dnd.active || w != dnd.target)
        return;
    // One action, and only one the source allows.
    dnd.action = (action == WM_DND_COPY || action == WM_DND_MOVE || action == WM_DND_LINK)
                 && (action & dnd.actions) ? action : 0;
    if (dnd.unanswered > 0)
        dnd.unanswered--;
    // Released before this answer came: it decides the drop.
    if (dnd.released_ms && !dnd.unanswered)
        dnd_finish(true);
    else
        dnd_cursor();
}

int dnd_tick(void)
{
    uint64_t waited;

    if (!dnd.active || !dnd.released_ms)
        return -1;
    // A target that does not answer within half a second gets nothing.
    if ((waited = uptime_ms() - dnd.released_ms) >= 500) {
        dnd.action = 0;
        dnd_finish(true);
        return -1;
    }
    return 500 - (int)waited;
}

// A window that goes away (already out of the window list) leaves no
// references behind: the pointer grab, a move or resize, a title button
// being pressed, or a drag.
void input_forget(struct window *w)
{
    if (grab == w)
        grab = NULL;
    if (pointer_window == w)
        pointer_window = NULL;
    if (press_window == w)
        press_window = NULL;
    if (drag_window == w) {
        drag_window = NULL;
        drag = DRAG_NONE;
        if (snap_window == w) {
            damage(snap_preview);
            snap_window = NULL;
            snap_preview = (struct rect){ 0, 0, 0, 0 };
        }
    }
    if (hover_window == w) {
        hover_window = NULL;
        hover_button = 0;
    }
    if (dnd.source == w)
        dnd.source = NULL;
    if (dnd.target == w) {
        dnd.target = NULL;
        dnd.action = dnd.unanswered = 0;
        if (dnd.active && dnd.released_ms)
            dnd_finish(true);
        else if (dnd.active)
            dnd_cursor();
    }
}

static void pointer_moved(void);

static void dnd_finish(bool drop)
{
    int action = drop && dnd.target ? dnd.action : 0;

    if (action)
        dnd_send(dnd.target, WM_DROP, INT32_MIN, INT32_MIN, action, dnd.len);
    else
        dnd_send(dnd.target, WM_DRAG_OVER, INT32_MIN, INT32_MIN, dnd.actions, WM_DRAG_LEAVE);
    if (dnd.source && dnd.source->owner) {
        struct wm_msg m = { WM_DRAG_END, dnd.source->cid, action, 0, 0, 0, 0, 0, { 0 } };

        send_msg(dnd.source->owner, &m);
    }
    if (dnd.fd >= 0)
        close(dnd.fd);
    set_cursor_shape(WM_CURSOR_ARROW);
    dnd = (struct dnd){ .fd = -1 };
    pointer_moved();
}

static void pointer_moved(void)
{
    struct window *w;

    if (dnd.active) {
        dnd_motion();
        return;
    }

    if (drag == DRAG_MOVE) {
        struct window *d = drag_window;

        struct rect where;

        damage_window(d);
        d->frame.x = drag_start.x + pointer_x - drag_x;
        d->frame.y = MAX(screen.work.y, drag_start.y + pointer_y - drag_y);
        damage_window(d);
        if (!(d->flags & WM_FLAG_NO_RESIZE)) {
            snap_target(&where);
            if (where.x != snap_preview.x || where.y != snap_preview.y || where.w != snap_preview.w)
                set_snap_preview(d, where);
        }
        return;
    }
    if (drag == DRAG_RESIZE) {
        struct window *d = drag_window;
        int nw = MAX(160, drag_start.w + pointer_x - drag_x), nh = MAX(TITLE_H + 60, drag_start.h + pointer_y - drag_y);
        uint64_t now = uptime_ms();

        damage_window(d);
        d->frame.w = nw;
        d->frame.h = nh;
        d->cw = nw - 2 * BORDER;
        d->ch = nh - TITLE_H - BORDER;
        damage_window(d);
        // Ask the client to redraw at the new size, at most every 30 ms.
        if (now - last_resize_ms >= 30) {
            last_resize_ms = now;
            send_window(d, WM_CONFIGURE, d->cw, d->ch, 0, 0, 0);
        }
        return;
    }
    w = grab ? grab : window_at(pointer_x, pointer_y);
    if (w != pointer_window) {
        if (pointer_window)
            pointer_event(pointer_window, WM_PTR_LEAVE, 0);
        pointer_window = w;
        if (w)
            pointer_event(w, WM_PTR_ENTER, 0);
    }
    update_hover(w);
    if (w && (grab || rect_contains(window_content(w), pointer_x, pointer_y))) {
        pointer_event(w, WM_PTR_MOVE, 0);
        set_cursor_shape(w->cursor);
    } else {
        int shape = cursor_for_point(w, pointer_x, pointer_y);
        set_cursor_shape(shape >= 0 ? shape : WM_CURSOR_ARROW);
    }
}

static void button(uint16_t code, bool down)
{
    uint32_t bit = 1U << (code - BTN_LEFT);
    struct window *w;

    if (down)
        buttons |= bit;
    else
        buttons &= ~bit;

    if (dnd.active) {
        // Releasing the buttons drops (once the target has answered for
        // where the pointer is); another button cancels.
        if (!down && !buttons && dnd.unanswered && dnd.target)
            dnd.released_ms = MAX(uptime_ms(), 1);
        else if (!down && !buttons)
            dnd_finish(true);
        else if (down)
            dnd_finish(false);
        return;
    }
    if (!down) {
        if (drag != DRAG_NONE && code == BTN_LEFT) {
            if (drag == DRAG_RESIZE)
                send_window(drag_window, WM_CONFIGURE, drag_window->cw, drag_window->ch, 0, 0, 0);
            if (drag == DRAG_MOVE && snap_preview.w) {
                int t = snap_target(NULL);

                // Restoring later puts it back where it was picked up.
                damage_window(drag_window);
                drag_window->frame = drag_start;
                if (t == 1)
                    set_maximized(drag_window, true);
                else if (t)
                    set_tiled(drag_window, t - 1);
                set_snap_preview(NULL, (struct rect){ 0, 0, 0, 0 });
            }
            drag = DRAG_NONE;
            drag_window = NULL;
            return;
        }
        if (press_window && code == press_button) {
            // A click on a title button acts on release, if still over it.
            int b = title_button_at(press_window, pointer_x, pointer_y);

            if (b == 3)
                send_window(press_window, WM_CLOSE_REQUEST, 0, 0, 0, 0, 0);
            else if (b == 2)
                set_maximized(press_window, !press_window->maximized);
            else if (b == 1)
                set_minimized(press_window, true);
            press_window = NULL;
            return;
        }
        if (grab) {
            pointer_event(grab, WM_PTR_UP, code);
            if (!buttons)
                grab = NULL;
        }
        return;
    }

    w = grab ? grab : window_at(pointer_x, pointer_y);
    if (!grab)
        close_popups(w);
    if (!w)
        return;
    if (w->role == WM_ROLE_NORMAL || w->role == WM_ROLE_DIALOG || w->role == WM_ROLE_OVERLAY) {
        raise_window(w);
        focus_window(w);
    } else if (w->role == WM_ROLE_DESKTOP) {
        // Full-screen programs below other windows (installer, recovery)
        // take the keyboard when clicked; they stay at the bottom.
        focus_window(w);
    }
    if (window_framed(w) && !rect_contains(window_content(w), pointer_x, pointer_y) && code == BTN_LEFT) {
        if (title_button_at(w, pointer_x, pointer_y)) {
            press_window = w;
            press_button = code;
            return;
        }
        if (cursor_for_point(w, pointer_x, pointer_y) >= 0) {
            drag = DRAG_RESIZE;
        } else if (pointer_y < w->frame.y + TITLE_H) {
            drag = DRAG_MOVE;
            // Dragging a maximized or tiled window restores it under the pointer.
            if (w->maximized || w->tiled)
                unsnap_for_drag(w);
        } else {
            return;
        }
        drag_window = w;
        drag_x = pointer_x;
        drag_y = pointer_y;
        drag_start = w->frame;
        return;
    }
    grab = w;
    pointer_event(w, WM_PTR_DOWN, code);
}

void begin_move(struct window *w)
{
    if (!buttons || drag != DRAG_NONE)
        return;
    grab = NULL;
    drag = DRAG_MOVE;
    drag_window = w;
    drag_x = pointer_x;
    drag_y = pointer_y;
    drag_start = w->frame;
}

static bool shortcut(uint16_t code, int value)
{
    if (value == 0)
        return false;
    if ((mods & MOD_ALT) && code == KEY_TAB) {
        cycle_focus();
        return true;
    }
    if ((mods & MOD_ALT) && code == 0x3D && focused && value == 1) {     // F4
        send_window(focused, WM_CLOSE_REQUEST, 0, 0, 0, 0, 0);
        return true;
    }
    if (code == KEY_SYSRQ && value == 1) {
        save_screenshot();
        return true;
    }
    // Meta+arrows: snap left or right, maximize, restore or minimize.
    if ((mods & MOD_META) && focused && window_framed(focused) && focused->role == WM_ROLE_NORMAL
        && (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN)) {
        struct window *w = focused;

        if (value != 1)
            return true;
        if (code == KEY_UP) {
            set_maximized(w, true);
        } else if (code == KEY_DOWN) {
            if (w->maximized)
                set_maximized(w, false);
            else if (w->tiled)
                set_tiled(w, TILE_NONE);
            else
                set_minimized(w, true);
        } else {
            int side = code == KEY_LEFT ? TILE_LEFT : TILE_RIGHT;

            // Towards the other side from a tile puts it back first.
            if (w->tiled && w->tiled != side)
                set_tiled(w, TILE_NONE);
            else if (w->maximized) {
                set_maximized(w, false);
                set_tiled(w, side);
            } else {
                set_tiled(w, side);
            }
        }
        return true;
    }
    return false;
}

// Print Screen: a PNG in the signed-in user's Images/Screenshots, and a
// notification on their desktop.
static void save_screenshot(void)
{
    uint32_t uid = compositor_session_uid();
    struct user_info u;
    char dir[300], name[96], unique[128], path[512];
    int64_t now = time(NULL);
    struct tm tm;

    if (uid == (uint32_t)-1 || user_by_uid(uid, &u) < 0) {
        snprintf(path, sizeof(path), "/osystem/temp/screenshot-%ld.png", (long)now);
        screenshot(path);
        return;
    }
    user_path(&u, "home/Images/Screenshots", dir, sizeof(dir));
    if (mkdir(dir, 0755) == 0)
        chown(dir, u.uid, u.gid);
    localtime_r(&now, &tm);
    strftime(name, sizeof(name), "Screenshot %Y-%m-%d %H.%M.%S.png", &tm);
    // unique_name gives the whole path.
    unique_name(dir, name, path, sizeof(path));
    strlcpy(unique, strrchr(path, '/') + 1, sizeof(unique));
    if (screenshot(path) < 0) {
        char why[400];

        snprintf(why, sizeof(why), "%s: %s", path, strerror(errno));
        notify_user(uid, "Screenshots", "The screenshot could not be saved", why);
        return;
    }
    chown(path, u.uid, u.gid);
    notify_user(uid, "Screenshots", "Screenshot saved", unique);
}

static void key(uint16_t code, int value)
{
    struct window *target = focused;
    struct wm_msg m = { 0 };

    if (mods & MOD_META && value && code != KEY_LEFTMETA && code != KEY_RIGHTMETA)
        meta_combo = true;
    if (dnd.active) {
        // Esc cancels a drag; Ctrl and Shift change what it does, so the
        // window under the pointer is asked again.
        if (code == KEY_ESC && value == 1)
            dnd_finish(false);
        else if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
            dnd_motion();
        return;
    }
    if (shortcut(code, value))
        return;
    // Meta released after a combination is not "Meta on its own" (the launcher).
    if ((code == KEY_LEFTMETA || code == KEY_RIGHTMETA) && !value && meta_combo) {
        meta_combo = false;
        return;
    }
    // A popup that asked for the keyboard (a launcher) has it while shown.
    for (struct window *w = windows; w; w = w->next)
        if (w->role == WM_ROLE_POPUP && (w->flags & WM_FLAG_KEYBOARD) && w->visible && w->owner)
            target = w;
    // The Meta key alone, and media and volume keys, go to panels (the launcher
    // and the volume control).
    if (!target || (target->role != WM_ROLE_POPUP
                    && (code == KEY_LEFTMETA || code == KEY_RIGHTMETA || code >= 0x100 || code == KEY_MUTE
                        || code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN
                        || ((mods & MOD_META) && code == KEY_A + 'l' - 'a')))) {
        for (struct window *w = windows; w; w = w->next) {
            if (w->role == WM_ROLE_PANEL && w->owner) {
                target = w;
                break;
            }
        }
    }
    if (!target || !target->owner)
        return;
    m.type = WM_KEY;
    m.window = target->cid;
    m.a = code;
    m.b = value;
    m.c = mods;
    if (value)
        key_text(code, mods, m.text);
    send_msg(target->owner, &m);
}

void input_handle(int fd)
{
    struct input_event ev[64];
    ssize_t n;
    bool moved = false;

    while ((n = read(fd, ev, sizeof(ev))) > 0) {
        for (int i = 0; i < (int)(n / sizeof(ev[0])); i++) {
            struct input_event *e = &ev[i];

            mods = e->modifiers;
            switch (e->type) {
            case EV_REL:
                if (e->code == REL_X || e->code == REL_Y) {
                    int d = e->value;

                    // A little acceleration for fast movements.
                    if (d > 6 || d < -6)
                        d = d * 3 / 2;
                    if (e->code == REL_X)
                        pointer_x = MAX(0, MIN(screen.width - 1, pointer_x + d));
                    else
                        pointer_y = MAX(0, MIN(screen.height - 1, pointer_y + d));
                    moved = true;
                } else if (e->code == REL_WHEEL || e->code == REL_HWHEEL) {
                    struct window *w = grab ? grab : window_at(pointer_x, pointer_y);

                    if (w)
                        pointer_event(w, e->code == REL_WHEEL ? WM_PTR_WHEEL : WM_PTR_HWHEEL, e->value);
                }
                break;
            case EV_ABS:
                if (e->code == ABS_X)
                    pointer_x = (int64_t)e->value * (screen.width - 1) / INPUT_ABS_MAX;
                else if (e->code == ABS_Y)
                    pointer_y = (int64_t)e->value * (screen.height - 1) / INPUT_ABS_MAX;
                moved = true;
                break;
            case EV_SYN:
                if (e->code == SYN_VT_LEAVE)
                    vt_leave();
                else if (e->code == SYN_VT_ENTER)
                    vt_enter();
                break;
            case EV_KEY:
                if (moved) {
                    pointer_moved();
                    moved = false;
                }
                if (e->code >= BTN_LEFT && e->code <= BTN_EXTRA)
                    button(e->code, e->value != 0);
                else
                    key(e->code, e->value);
                break;
            }
        }
    }
    if (moved)
        pointer_moved();
}
