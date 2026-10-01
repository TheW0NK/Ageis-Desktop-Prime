#include "wm.h"

static int sock = -1;
static int screen_w, screen_h, work_h;
static uint32_t next_window = 1;
static struct wm_window *all;
static bool connected;

void wm_send(struct wm_msg *m)
{
    if (sock >= 0 && send(sock, m, sizeof(*m), MSG_NOSIGNAL) < 0 && errno != EAGAIN)
        connected = false;
}

static void send_simple(uint32_t type, struct wm_window *w, int a, int b, int c, int d)
{
    struct wm_msg m = { type, w ? w->id : 0, a, b, c, d, 0, 0, { 0 } };

    wm_send(&m);
}

bool wm_connect(void)
{
    struct wm_msg m = { WM_HELLO, 0, WM_VERSION, 0, 0, 0, 0, 0, { 0 } };

    if (connected)
        return true;
    if ((sock = unix_connect(WM_SOCKET, SOCK_SEQPACKET)) < 0)
        return false;
    connected = true;
    wm_send(&m);
    if (recv(sock, &m, sizeof(m), 0) != sizeof(m) || m.type != WM_WELCOME) {
        close(sock);
        sock = -1;
        connected = false;
        return false;
    }
    screen_w = m.a;
    screen_h = m.b;
    work_h = m.c;
    return true;
}

bool wm_connected(void)
{
    return connected;
}

int wm_fd(void)
{
    return sock;
}

void wm_screen_size(int *w, int *h, int *wh)
{
    if (w)
        *w = screen_w;
    if (h)
        *h = screen_h;
    if (wh)
        *wh = work_h;
}

// Gives the window a buffer of its current size.
static bool attach(struct wm_window *w)
{
    size_t len = (size_t)w->width * w->height * 4;
    int fd = shm_create(len, O_CLOEXEC);
    uint32_t *map;
    struct surface *s;
    struct wm_msg m = { WM_BUFFER, w->id, w->width, w->height, 0, 0, 0, 0, { 0 } };

    if (fd < 0)
        return false;
    map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED || !(s = surface_create(w->width, w->height))) {
        close(fd);
        return false;
    }
    if (w->shared)
        munmap(w->shared, w->shared_len);
    if (w->shm_fd >= 0)
        close(w->shm_fd);
    if (w->surface) {
        // Keep what was drawn so far where it still fits.
        struct gfx g;

        gfx_init(&g, s);
        gfx_blit(&g, w->surface, (struct rect){ 0, 0, w->surface->width, w->surface->height }, 0, 0);
        surface_destroy(w->surface);
    }
    w->surface = s;
    w->shared = map;
    w->shared_len = len;
    w->shm_fd = fd;
    send_fds(sock, &m, sizeof(m), &fd, 1);
    return true;
}

struct wm_window *wm_create_at(const char *title, int x, int y, int width, int height, uint32_t flags,
                               struct wm_window *parent)
{
    struct wm_window *w = calloc(1, sizeof(*w));
    struct wm_msg m = { WM_CREATE, 0, width, height, x, y, flags, parent ? parent->id : 0, { 0 } };

    if (!w || !connected)
        return NULL;
    w->id = next_window++;
    w->role = flags & WM_ROLE_MASK;
    w->width = width;
    w->height = height;
    w->shm_fd = -1;
    w->visible = !(flags & WM_FLAG_HIDDEN);
    m.window = w->id;
    strlcpy(m.text, title ? title : "", sizeof(m.text));
    wm_send(&m);
    if (!attach(w)) {
        send_simple(WM_DESTROY, w, 0, 0, 0, 0);
        free(w);
        return NULL;
    }
    w->next = all;
    all = w;
    return w;
}

struct wm_window *wm_create(const char *title, int width, int height, uint32_t flags)
{
    return wm_create_at(title, -1, -1, width, height, flags, NULL);
}

void wm_destroy(struct wm_window *w)
{
    if (!w)
        return;
    send_simple(WM_DESTROY, w, 0, 0, 0, 0);
    for (struct wm_window **pp = &all; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    }
    if (w->shared)
        munmap(w->shared, w->shared_len);
    if (w->shm_fd >= 0)
        close(w->shm_fd);
    surface_destroy(w->surface);
    free(w);
}

void wm_set_title(struct wm_window *w, const char *title)
{
    struct wm_msg m = { WM_SET_TITLE, w->id, 0, 0, 0, 0, 0, 0, { 0 } };

    strlcpy(m.text, title, sizeof(m.text));
    wm_send(&m);
}

void wm_show(struct wm_window *w, bool show)
{
    w->visible = show;
    send_simple(show ? WM_SHOW : WM_HIDE, w, 0, 0, 0, 0);
}

void wm_move(struct wm_window *w, int x, int y)
{
    send_simple(WM_MOVE, w, x, y, 0, 0);
}

void wm_resize(struct wm_window *w, int width, int height)
{
    send_simple(WM_RESIZE, w, width, height, 0, 0);
}

void wm_set_state(struct wm_window *w, uint32_t state)
{
    send_simple(WM_SET_STATE, w, state, 0, 0, 0);
}

void wm_set_cursor(struct wm_window *w, int cursor)
{
    send_simple(WM_SET_CURSOR, w, cursor, 0, 0, 0);
}

void wm_begin_move(struct wm_window *w)
{
    send_simple(WM_BEGIN_MOVE, w, 0, 0, 0, 0);
}

void wm_subscribe(void)
{
    send_simple(WM_SUBSCRIBE, NULL, 0, 0, 0, 0);
}

void wm_activate(uint32_t id, bool toggle)
{
    struct wm_msg m = { WM_ACTIVATE, id, toggle, 0, 0, 0, 0, 0, { 0 } };

    wm_send(&m);
}

void wm_present(struct wm_window *w, struct rect r)
{
    struct rect all_r = { 0, 0, w->width, w->height };

    if (!rect_intersect(r, all_r, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(w->shared + (size_t)y * w->width + r.x, w->surface->pixels + (size_t)y * w->surface->stride + r.x,
               r.w * 4);
    send_simple(WM_DAMAGE, w, r.x, r.y, r.w, r.h);
}

void wm_present_all(struct wm_window *w)
{
    wm_present(w, (struct rect){ 0, 0, w->width, w->height });
}

static struct wm_window *lookup(uint32_t id)
{
    for (struct wm_window *w = all; w; w = w->next) {
        if (w->id == id)
            return w;
    }
    return NULL;
}

bool wm_next_event(struct wm_event *ev, int timeout_ms)
{
    struct pollfd p = { sock, POLLIN, 0 };
    struct wm_msg m;
    ssize_t n;

    memset(ev, 0, sizeof(*ev));
    if (!connected)
        return false;
    if (timeout_ms >= 0 && poll(&p, 1, timeout_ms) != 1)
        return false;
    n = recv(sock, &m, sizeof(m), 0);
    if (n <= 0) {
        if (n == 0 || errno != EINTR)
            connected = false;
        return false;
    }
    if (n != sizeof(m))
        return true;
    ev->msg = m;
    ev->window = lookup(m.window);
    switch (m.type) {
    case WM_KEY:
        ev->type = WM_EV_KEY;
        ev->key = m.a;
        ev->value = m.b;
        ev->mods = m.c;
        strlcpy(ev->text, m.text, sizeof(ev->text));
        break;
    case WM_POINTER:
        ev->type = WM_EV_POINTER;
        ev->x = m.a;
        ev->y = m.b;
        ev->buttons = m.c;
        ev->kind = m.d;
        ev->detail = (int32_t)m.flags;
        break;
    case WM_CONFIGURE:
        ev->type = WM_EV_RESIZE;
        if (ev->window && (m.a != ev->window->width || m.b != ev->window->height) && m.a > 0 && m.b > 0) {
            ev->window->width = m.a;
            ev->window->height = m.b;
            attach(ev->window);
        }
        ev->width = m.a;
        ev->height = m.b;
        break;
    case WM_CLOSE_REQUEST:
        ev->type = WM_EV_CLOSE;
        break;
    case WM_FOCUS:
        ev->type = WM_EV_FOCUS;
        ev->focused = m.a;
        if (ev->window)
            ev->window->focused = m.a;
        break;
    case WM_FRAME:
        ev->type = WM_EV_FRAME;
        break;
    case WM_POPUP_DONE:
        ev->type = WM_EV_POPUP_DONE;
        if (ev->window)
            ev->window->visible = false;
        break;
    case WM_LIST_ADD:
    case WM_LIST_REMOVE:
    case WM_LIST_CHANGE:
        ev->type = WM_EV_LIST;
        ev->window = NULL;
        break;
    case WM_SCREEN:
        ev->type = WM_EV_SCREEN;
        screen_w = m.a;
        screen_h = m.b;
        break;
    default:
        ev->type = WM_EV_NONE;
    }
    return true;
}
