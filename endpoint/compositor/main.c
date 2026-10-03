#include "compositor.h"

// The Aegis compositor: owns the display, keyboard and pointer; shows client
// windows from shared memory with frames, shadows and a cursor.
//
// compositor [PROGRAM [ARGS...]]   starts PROGRAM (default /osystem/core/greeter)

#define MAX_CLIENTS     64

struct screen screen;
struct window *windows, *focused;
static struct client *clients;
static uint32_t next_id = 1;
static uint32_t session_uid = (uint32_t)-1;

uint32_t compositor_session_uid(void)
{
    return session_uid;
}

static void drop_client(struct client *c);
static int cascade;

void begin_move(struct window *w);

void send_msg(struct client *c, struct wm_msg *m)
{
    if (c && c->fd >= 0)
        send(c->fd, m, sizeof(*m), MSG_DONTWAIT | MSG_NOSIGNAL);
}

// The session's clipboard: a shared memory object handed to each program.
static int clip_fd = -1, clip_len;
static uint32_t clip_uid;

static void send_clipboard(struct client *c)
{
    struct wm_msg m = { WM_CLIPBOARD, 0, clip_len, 0, 0, 0, 0, 0, { 0 } };
    int fd = clip_fd;

    if (c && c->fd >= 0 && clip_fd >= 0 && c->uid == clip_uid)
        send_fds(c->fd, &m, sizeof(m), &fd, 1);
}

void send_window(struct window *w, uint32_t type, int a, int b, int c, int d, uint32_t flags)
{
    struct wm_msg m = { type, w->cid, a, b, c, d, flags, 0, { 0 } };

    send_msg(w->owner, &m);
}

static int layer(struct window *w)
{
    switch (w->role) {
    case WM_ROLE_DESKTOP:   return 0;
    case WM_ROLE_PANEL:     return 2;
    case WM_ROLE_POPUP:     return 3;
    case WM_ROLE_OVERLAY:   return 4;
    }
    return 1;
}

static bool listed(struct window *w)
{
    return (w->role == WM_ROLE_NORMAL || w->role == WM_ROLE_DIALOG) && w->visible;
}

static uint32_t state_of(struct window *w)
{
    return (w->minimized ? WM_STATE_MINIMIZED : 0) | (w->maximized ? WM_STATE_MAXIMIZED : 0)
         | (w == focused ? WM_STATE_FOCUSED : 0);
}

// Tells panels about a window that appeared, changed or went away.
void announce(struct window *w, uint32_t type)
{
    struct wm_msg m = { type, w->id, (int)state_of(w), w->owner ? w->owner->pid : 0, 0, 0, 0, 0, { 0 } };

    if (type != WM_LIST_REMOVE && !listed(w))
        return;
    strlcpy(m.text, w->title, sizeof(m.text));
    for (struct client *c = clients; c; c = c->next) {
        if (c->subscribed)
            send_msg(c, &m);
    }
}

static void unlink_window(struct window *w)
{
    for (struct window **pp = &windows; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            return;
        }
    }
}

// Inserts at the top of its layer.
static void insert_window(struct window *w)
{
    struct window **pp = &windows;

    while (*pp && layer(*pp) <= layer(w))
        pp = &(*pp)->next;
    w->next = *pp;
    *pp = w;
}

void raise_window(struct window *w)
{
    unlink_window(w);
    insert_window(w);
    // Dialogs stay above the window that owns them.
    for (struct window *d = windows, *next; d; d = next) {
        next = d->next;
        if (d->parent == w && d->visible) {
            unlink_window(d);
            insert_window(d);
        }
    }
    damage_window(w);
}

struct window *window_at(int x, int y)
{
    struct window *hit = NULL;

    for (struct window *w = windows; w; w = w->next) {
        struct rect r = w->frame;

        if (!w->visible || w->minimized || !w->pixels)
            continue;
        if (window_framed(w) && !w->maximized && !(w->flags & WM_FLAG_NO_RESIZE)) {
            r.w += RESIZE_EDGE;
            r.h += RESIZE_EDGE;
        }
        if (rect_contains(r, x, y))
            hit = w;
    }
    return hit;
}

void focus_window(struct window *w)
{
    struct window *old = focused;

    if (w == focused)
        return;
    focused = w;
    if (old) {
        send_window(old, WM_FOCUS, 0, 0, 0, 0, 0);
        damage((struct rect){ old->frame.x, old->frame.y, old->frame.w, TITLE_H });
        damage_window(old);
        announce(old, WM_LIST_CHANGE);
    }
    if (w) {
        send_window(w, WM_FOCUS, 1, 0, 0, 0, 0);
        damage_window(w);
        announce(w, WM_LIST_CHANGE);
    }
}

static void focus_top(void)
{
    struct window *best = NULL;

    for (struct window *w = windows; w; w = w->next) {
        if (w->visible && !w->minimized && (w->role == WM_ROLE_NORMAL || w->role == WM_ROLE_DIALOG
                                            || w->role == WM_ROLE_OVERLAY))
            best = w;
    }
    // Nothing else: the desktop-role window (the installer, recovery).
    for (struct window *w = windows; !best && w; w = w->next)
        if (w->visible && w->role == WM_ROLE_DESKTOP && w->owner)
            best = w;
    focus_window(best);
}

void cycle_focus(void)
{
    struct window *first = NULL;

    // Bring the bottom-most normal window to the top.
    for (struct window *w = windows; w; w = w->next) {
        if (listed(w) && w != focused) {
            first = w;
            break;
        }
    }
    if (!first)
        return;
    if (first->minimized)
        set_minimized(first, false);
    raise_window(first);
    focus_window(first);
}

void close_popups(struct window *except)
{
    for (struct window *w = windows; w; w = w->next) {
        if (w->role != WM_ROLE_POPUP || !w->visible || w == except)
            continue;
        // Keep a popup if the click went into one of its sub-popups.
        bool ancestor = false;
        for (struct window *e = except; e && !ancestor; e = e->parent)
            ancestor = e == w;
        if (ancestor)
            continue;
        w->visible = false;
        damage_window(w);
        send_window(w, WM_POPUP_DONE, 0, 0, 0, 0, 0);
    }
}

static void fit_frame(struct window *w)
{
    if (window_framed(w)) {
        w->frame.w = w->cw + 2 * BORDER;
        w->frame.h = w->ch + TITLE_H + BORDER;
    } else {
        w->frame.w = w->cw;
        w->frame.h = w->ch;
    }
}

void set_maximized(struct window *w, bool on)
{
    if (!window_framed(w) || (w->flags & WM_FLAG_NO_RESIZE) || w->maximized == on)
        return;
    damage_window(w);
    if (on) {
        if (!w->tiled)
            w->saved = w->frame;
        w->tiled = TILE_NONE;
        w->frame = screen.work;
    } else {
        w->frame = w->saved;
    }
    w->maximized = on;
    w->cw = w->frame.w - 2 * BORDER;
    w->ch = w->frame.h - TITLE_H - BORDER;
    damage_window(w);
    send_window(w, WM_CONFIGURE, w->cw, w->ch, 0, 0, 0);
    announce(w, WM_LIST_CHANGE);
}

struct rect tile_rect(int side)
{
    struct rect r = screen.work;

    r.w /= 2;
    if (side == TILE_RIGHT) {
        r.x += r.w;
        r.w = screen.work.w - r.w;
    }
    return r;
}

// Snaps a window to the left or right half of the screen, or (TILE_NONE)
// puts it back where it was.
void set_tiled(struct window *w, int side)
{
    if (!window_framed(w) || (w->flags & WM_FLAG_NO_RESIZE) || w->tiled == side)
        return;
    damage_window(w);
    if (side) {
        if (!w->tiled && !w->maximized)
            w->saved = w->frame;
        w->maximized = false;
        w->frame = tile_rect(side);
    } else {
        w->frame = w->saved;
    }
    w->tiled = side;
    w->cw = w->frame.w - 2 * BORDER;
    w->ch = w->frame.h - TITLE_H - BORDER;
    damage_window(w);
    send_window(w, WM_CONFIGURE, w->cw, w->ch, 0, 0, 0);
    announce(w, WM_LIST_CHANGE);
}

void set_minimized(struct window *w, bool on)
{
    if (w->minimized == on)
        return;
    w->minimized = on;
    damage_window(w);
    if (on && focused == w)
        focus_top();
    if (!on) {
        raise_window(w);
        focus_window(w);
    }
    announce(w, WM_LIST_CHANGE);
}

void update_work_area(void)
{
    struct rect work = { 0, 0, screen.width, screen.height };

    for (struct window *w = windows; w; w = w->next) {
        if (w->role != WM_ROLE_PANEL || !w->visible)
            continue;
        if (w->flags & WM_FLAG_PANEL_TOP) {
            work.y = MAX(work.y, w->frame.y + w->frame.h);
            work.h = screen.height - work.y;
        } else {
            work.h = MIN(work.h, w->frame.y - work.y);
        }
    }
    screen.work = work;
    for (struct window *w = windows; w; w = w->next) {
        if (w->maximized || w->tiled) {
            damage_window(w);
            w->frame = w->tiled ? tile_rect(w->tiled) : work;
            w->cw = work.w - 2 * BORDER;
            w->ch = work.h - TITLE_H - BORDER;
            damage_window(w);
            send_window(w, WM_CONFIGURE, w->cw, w->ch, 0, 0, 0);
        }
    }
}

static struct window *find(struct client *c, uint32_t cid)
{
    for (struct window *w = windows; w; w = w->next) {
        if (w->owner == c && w->cid == cid)
            return w;
    }
    return NULL;
}

static void destroy_window(struct window *w)
{
    damage_window(w);
    announce(w, WM_LIST_REMOVE);
    unlink_window(w);
    input_forget(w);
    for (struct window *o = windows; o; o = o->next) {
        if (o->parent == w)
            o->parent = NULL;
    }
    if (w->pixels)
        munmap(w->pixels, w->map_len);
    if (w->role == WM_ROLE_PANEL)
        update_work_area();
    if (focused == w) {
        focused = NULL;
        focus_top();
    }
    free(w);
}

static void place(struct window *w, int x, int y)
{
    struct rect work = screen.work;

    if (w->role == WM_ROLE_PANEL) {
        w->frame = (struct rect){ 0, (w->flags & WM_FLAG_PANEL_TOP) ? 0 : screen.height - w->ch, screen.width, w->ch };
        w->cw = screen.width;
        return;
    }
    if (w->role == WM_ROLE_DESKTOP || w->role == WM_ROLE_OVERLAY) {
        w->frame = (struct rect){ 0, 0, screen.width, screen.height };
        w->cw = screen.width;
        w->ch = screen.height;
        return;
    }
    fit_frame(w);
    if (w->role == WM_ROLE_POPUP && w->parent) {
        struct rect pc = window_content(w->parent);

        x += pc.x;
        y += pc.y;
        // Keep popups on screen.
        if (x + w->frame.w > screen.width)
            x = screen.width - w->frame.w;
        if (y + w->frame.h > screen.height)
            y = MAX(0, y - w->frame.h);
        w->frame.x = MAX(0, x);
        w->frame.y = MAX(0, y);
        return;
    }
    if (x >= 0 && y >= 0) {
        w->frame.x = x;
        w->frame.y = y;
        return;
    }
    if (w->role == WM_ROLE_DIALOG && w->parent) {
        w->frame.x = w->parent->frame.x + (w->parent->frame.w - w->frame.w) / 2;
        w->frame.y = w->parent->frame.y + MIN(80, (w->parent->frame.h - w->frame.h) / 2);
    } else {
        // Centred, stepping each new window down and right.
        w->frame.x = work.x + (work.w - w->frame.w) / 2 + cascade * 28 - 56;
        w->frame.y = work.y + MAX(16, (work.h - w->frame.h) / 3) + cascade * 28 - 56;
        cascade = (cascade + 1) % 5;
    }
    w->frame.x = MAX(work.x, MIN(w->frame.x, work.x + work.w - w->frame.w));
    w->frame.y = MAX(work.y, MIN(w->frame.y, work.y + work.h - w->frame.h));
}

static void handle(struct client *c, struct wm_msg *m, int fd)
{
    struct window *w = m->type == WM_CREATE || m->type == WM_ACTIVATE ? NULL : find(c, m->window);
    struct wm_msg r = { 0 };

    switch (m->type) {
    case WM_HELLO:
        r.type = WM_WELCOME;
        r.a = screen.width;
        r.b = screen.height;
        r.c = screen.work.h;
        send_msg(c, &r);
        send_clipboard(c);
        break;
    case WM_CLIPBOARD_SET:
        if (fd < 0 || m->a < 0 || m->a > (16 << 20))
            break;
        if (clip_fd >= 0)
            close(clip_fd);
        clip_fd = fd;
        fd = -1;
        clip_len = m->a;
        clip_uid = c->uid;
        for (struct client *k = clients; k; k = k->next)
            if (k != c)
                send_clipboard(k);
        break;
    case WM_DRAG_START:
        if (w && fd >= 0 && m->a >= 0 && m->a <= (16 << 20) && ioctl(fd, IOCTL_SHM_SIZE, 0) >= m->a
            && dnd_start(w, fd, m->a, m->b, m->text))
            fd = -1;
        else if (w)
            send_window(w, WM_DRAG_END, 0, 0, 0, 0, 0);
        break;
    case WM_DRAG_STATUS:
        if (w)
            dnd_status(w, m->a);
        break;
    case WM_CREATE:
        if (find(c, m->window) || m->a <= 0 || m->b <= 0 || m->a > 8192 || m->b > 8192
            || !(w = calloc(1, sizeof(*w))))
            break;
        w->id = next_id++;
        w->cid = m->window;
        w->owner = c;
        w->role = m->flags & WM_ROLE_MASK;
        w->flags = m->flags;
        w->cw = m->a;
        w->ch = m->b;
        w->parent = m->parent ? find(c, m->parent) : NULL;
        strlcpy(w->title, m->text, sizeof(w->title));
        w->visible = !(m->flags & WM_FLAG_HIDDEN);
        place(w, m->c, m->d);
        insert_window(w);
        if (w->role == WM_ROLE_PANEL)
            update_work_area();
        if (w->cw != m->a || w->ch != m->b)
            send_window(w, WM_CONFIGURE, w->cw, w->ch, 0, 0, 0);
        if (w->visible && w->role != WM_ROLE_PANEL && w->role != WM_ROLE_DESKTOP && w->role != WM_ROLE_POPUP)
            focus_window(w);
        announce(w, WM_LIST_ADD);
        break;
    case WM_BUFFER: {
        size_t need = (size_t)m->a * m->b * 4;
        void *map;

        if (fd < 0 || !w || m->a <= 0 || m->b <= 0 || m->a > 8192 || m->b > 8192
            || ioctl(fd, IOCTL_SHM_SIZE, 0) < (long)need
            || (map = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
            break;
        if (w->pixels)
            munmap(w->pixels, w->map_len);
        w->pixels = map;
        w->map_len = need;
        w->bw = m->a;
        w->bh = m->b;
        if (!window_framed(w) && w->role != WM_ROLE_PANEL && w->role != WM_ROLE_DESKTOP
            && w->role != WM_ROLE_OVERLAY) {
            damage_window(w);
            w->cw = w->bw;
            w->ch = w->bh;
            fit_frame(w);
        }
        damage_window(w);
        break;
    }
    case WM_DAMAGE:
        if (w) {
            struct rect cr = window_content(w), d = { cr.x + m->a, cr.y + m->b, m->c, m->d };

            if (rect_intersect(d, cr, &d))
                damage(d);
            w->frame_pending = true;
        }
        break;
    case WM_SET_TITLE:
        if (w) {
            strlcpy(w->title, m->text, sizeof(w->title));
            damage((struct rect){ w->frame.x, w->frame.y, w->frame.w, TITLE_H });
            announce(w, WM_LIST_CHANGE);
        }
        break;
    case WM_SHOW:
    case WM_HIDE:
        if (w && w->visible != (m->type == WM_SHOW)) {
            if (m->type == WM_HIDE)
                announce(w, WM_LIST_REMOVE);
            w->visible = m->type == WM_SHOW;
            damage_window(w);
            if (w->visible) {
                raise_window(w);
                if ((w->role != WM_ROLE_POPUP && w->role != WM_ROLE_PANEL && w->role != WM_ROLE_DESKTOP)
                    || (w->role == WM_ROLE_DESKTOP && !focused))
                    focus_window(w);
                announce(w, WM_LIST_ADD);
            } else if (focused == w) {
                focus_top();
            }
            if (w->role == WM_ROLE_PANEL)
                update_work_area();
        }
        break;
    case WM_DESTROY:
        if (w)
            destroy_window(w);
        break;
    case WM_MOVE:
        if (w) {
            damage_window(w);
            if (w->role == WM_ROLE_POPUP && w->parent)
                place(w, m->a, m->b);
            else {
                w->frame.x = m->a;
                w->frame.y = m->b;
            }
            damage_window(w);
        }
        break;
    case WM_RESIZE:
        if (w && m->a > 0 && m->b > 0 && m->a <= 8192 && m->b <= 8192) {
            damage_window(w);
            w->cw = m->a;
            w->ch = m->b;
            fit_frame(w);
            damage_window(w);
            send_window(w, WM_CONFIGURE, w->cw, w->ch, 0, 0, 0);
        }
        break;
    case WM_SET_STATE:
        if (w) {
            if (m->a & WM_STATE_MINIMIZED)
                set_minimized(w, true);
            else if (m->a & WM_STATE_MAXIMIZED)
                set_maximized(w, true);
            else {
                set_minimized(w, false);
                set_maximized(w, false);
            }
        }
        break;
    case WM_ACTIVATE:
        // Panels switch to any window by its global id.
        for (w = windows; w && w->id != m->window; w = w->next)
            ;
        if (w && listed(w)) {
            if (m->a && focused == w && !w->minimized)
                set_minimized(w, true);
            else if (w->minimized)
                set_minimized(w, false);
            else {
                raise_window(w);
                focus_window(w);
            }
        }
        break;
    case WM_SET_CURSOR:
        if (w) {
            w->cursor = m->a;
            if (window_at(pointer_x, pointer_y) == w)
                set_cursor_shape(m->a);
        }
        break;
    case WM_SUBSCRIBE:
        c->subscribed = true;
        for (struct window *o = windows; o; o = o->next) {
            struct wm_msg a = { WM_LIST_ADD, o->id, (int)state_of(o), o->owner ? o->owner->pid : 0, 0, 0, 0, 0, { 0 } };

            if (!listed(o))
                continue;
            strlcpy(a.text, o->title, sizeof(a.text));
            send_msg(c, &a);
        }
        break;
    case WM_SET_SESSION:
        if (c->uid == 0 && session_uid != (uint32_t)m->a) {
            uint32_t old = session_uid;

            session_uid = (uint32_t)m->a;
            // The previous session's programs lose the display.
            for (struct client *k = clients, *next; k; k = next) {
                next = k->next;
                if (k->uid != 0 && k->uid == old)
                    drop_client(k);
            }
        }
        break;
    case WM_SETTING_CHANGED:
        m->text[WM_TEXT_MAX - 1] = 0;
        if (!strncmp(m->text, "theme=", 6))
            set_frame_theme(m->text + 6);
        // Pass it on to the user's other programs.
        r.type = WM_SETTING;
        strlcpy(r.text, m->text, sizeof(r.text));
        for (struct client *k = clients; k; k = k->next)
            if (k != c && (k->uid == c->uid || c->uid == 0))
                send_msg(k, &r);
        break;
    case WM_BEGIN_MOVE:
        if (w && window_framed(w))
            begin_move(w);
        break;
    case WM_SCREENSHOT:
        m->text[WM_TEXT_MAX - 1] = 0;
        if (screenshot(m->text) < 0) {
            r.type = WM_ERROR;
            r.a = errno;
            strlcpy(r.text, "cannot save the screenshot", sizeof(r.text));
            send_msg(c, &r);
        }
        break;
    }
    if (fd >= 0)
        close(fd);
}

static void drop_client(struct client *c)
{
    for (struct window *w = windows, *next; w; w = next) {
        next = w->next;
        if (w->owner == c)
            destroy_window(w);
    }
    for (struct client **pp = &clients; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    close(c->fd);
    free(c);
}

static void accept_client(int lfd)
{
    struct ucred cred;
    uint32_t len = sizeof(cred);
    struct client *c;
    int fd, n = 0;

    while ((fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
        for (c = clients; c; c = c->next)
            n++;
        if (n >= MAX_CLIENTS || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0
            || (cred.uid != 0 && cred.uid != session_uid) || !(c = calloc(1, sizeof(*c)))) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->pid = cred.pid;
        c->uid = cred.uid;
        c->next = clients;
        clients = c;
    }
}

bool screen_paused;

void vt_leave(void)
{
    screen_paused = true;
    ioctl(screen.fd, IOCTL_FB_VT_RELEASED, 0);
}

void vt_enter(void)
{
    screen_paused = false;
    damage((struct rect){ 0, 0, screen.width, screen.height });
}

static bool open_screen(void)
{
    if ((screen.fd = open("/osystem/devices/display", O_RDWR | O_CLOEXEC)) < 0)
        return false;
    if (ioctl(screen.fd, IOCTL_FB_INFO, (unsigned long)&screen.info) < 0)
        return false;
    screen.fb = mmap(NULL, screen.info.size, PROT_READ | PROT_WRITE, MAP_SHARED, screen.fd, 0);
    if (screen.fb == MAP_FAILED)
        return false;
    screen.width = screen.info.width;
    screen.height = screen.info.height;
    screen.work = (struct rect){ 0, 0, screen.width, screen.height };
    screen.back = surface_create(screen.width, screen.height);
    return screen.back != NULL;
}

int main(int argc, char **argv)
{
    char *greeter[] = { "greeter", NULL };
    char **session = argc > 1 ? argv + 1 : greeter;
    char path[256];
    int input, lfd, session_pid = -1;
    struct pollfd *fds = calloc(MAX_CLIENTS + 2, sizeof(*fds));

    signal(SIGPIPE, SIG_IGN);
    if (!open_screen()) {
        perror("compositor: /osystem/devices/display");
        return 1;
    }
    if ((input = input_open_device()) < 0) {
        perror("compositor: /osystem/devices/input");
        return 1;
    }
    if ((lfd = unix_listen(WM_SOCKET, SOCK_SEQPACKET)) < 0) {
        perror("compositor: " WM_SOCKET);
        return 1;
    }
    fcntl(lfd, F_SETFL, O_NONBLOCK);
    // Started while the text console is in front (Ctrl+Alt+F2).
    screen_paused = ioctl(input, IOCTL_INPUT_VT, 0) == 1;
    pointer_x = screen.width / 2;
    pointer_y = screen.height / 2;
    damage((struct rect){ 0, 0, screen.width, screen.height });
    render();

    snprintf(path, sizeof(path), strchr(session[0], '/') ? "%s" : "/osystem/core/%s", session[0]);
    if ((session_pid = spawn(path, session, environ)) < 0) {
        snprintf(path, sizeof(path), "/sysapps/%s", session[0]);
        if ((session_pid = spawn(path, session, environ)) < 0)
            dprintf(STDERR_FILENO, "compositor: cannot start %s\n", session[0]);
    }

    for (;;) {
        int n = 0, status;
        struct client *order[MAX_CLIENTS];

        fds[n++] = (struct pollfd){ input, POLLIN, 0 };
        fds[n++] = (struct pollfd){ lfd, POLLIN, 0 };
        for (struct client *c = clients; c && n < MAX_CLIENTS + 2; c = c->next) {
            order[n - 2] = c;
            fds[n++] = (struct pollfd){ c->fd, POLLIN, 0 };
        }
        {
            int wait = dnd_tick();

            if (poll(fds, n, wait >= 0 ? wait : 1000) < 0)
                continue;
        }
        {
            int pid;

            // The display lives as long as the program it was started for
            // (the greeter); init starts both again.
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
                if (pid == session_pid)
                    return 0;
        }
        dnd_tick();
        if (fds[0].revents & POLLIN)
            input_handle(input);
        if (fds[1].revents & POLLIN)
            accept_client(lfd);
        for (int i = 2; i < n; i++) {
            struct client *c = order[i - 2];
            bool gone = fds[i].revents & (POLLHUP | POLLERR);

            if (fds[i].revents & POLLIN) {
                for (int k = 0; k < 64; k++) {
                    struct wm_msg m;
                    int got_fd = -1, nfds = 1;
                    ssize_t len = recv_fds(c->fd, &m, sizeof(m), &got_fd, &nfds);

                    if (len < 0) {
                        if (errno != EAGAIN)
                            gone = true;
                        break;
                    }
                    if (len == 0) {
                        gone = true;
                        break;
                    }
                    if (len == sizeof(m)) {
                        m.text[WM_TEXT_MAX - 1] = 0;
                        handle(c, &m, nfds ? got_fd : -1);
                    } else if (nfds) {
                        close(got_fd);
                    }
                }
            }
            if (gone)
                drop_client(c);
        }
        render();
    }
}
