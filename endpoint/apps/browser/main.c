#include "browser.h"
#include "ui.h"

// Web Browser: an address bar, a page view and history. Pages, style
// sheets and pictures load on worker threads; each finished load comes back
// to the window through a pipe.

static const char window_aui[] =
    "<window title='Web Browser' width='1040' height='740' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button id='back' flat='true' symbol='back' tooltip='Back (Alt+Left)' onclick='back' shortcut='Alt+Left'/>"
    "    <button id='forward' flat='true' symbol='forward' tooltip='Forward (Alt+Right)' onclick='forward'"
    "            shortcut='Alt+Right'/>"
    "    <button id='reload' flat='true' symbol='restart' tooltip='Reload (F5)' onclick='reload' shortcut='F5'/>"
    "    <button flat='true' symbol='home' tooltip='Start page (Alt+Home)' onclick='home' shortcut='Alt+Home'/>"
    "    <input id='address' expand='true' placeholder='Type a web address or a file path' onactivate='go'/>"
    "    <button flat='true' text='Source' tooltip='View the page source (Ctrl+U)' onclick='source'"
    "            shortcut='Ctrl+U'/>"
    "    <button flat='true' text='Downloads' onclick='downloads'/>"
    "  </toolbar>"
    "  <canvas id='view' focusable='true' expand='true'/>"
    "  <statusbar><label id='status' expand='true'/><label id='zoom' dim='true'/></statusbar>"
    "  <menu id='choices'/>"
    "</window>";

#define SCROLLBAR   12
#define MAX_ACTIVE  6

enum { JOB_PAGE, JOB_CSS, JOB_IMAGE, JOB_BACKGROUND };

struct job {
    int kind, gen, slot;
    char *url, *post;
    struct node *node;
    struct response *res;
    thread_t thread;
    struct job *next;
};

struct bg_image {
    char *url;
    struct surface *img;
    int state;                      // 1 loading, 2 loaded, 3 failed
};

struct history_entry {
    char *url;
    int scroll;
};

static struct ui_window *win;
static struct widget *view, *address, *status_label;
static int pipe_fds[2];
static int gen;                     // bumped on every navigation
static int active;
static struct job *queue;           // waiting to start

// The current page.
static char *page_url, *base_url, *page_source;
static struct node *doc;
static struct sheet **sheets;       // [0] is the user agent's
static int nsheets;
static struct page_layout layout;
static struct bg_image *bgs;
static int nbgs;
static int scroll_y, pending;
static bool loading, dirty, scroll_drag;
static int drag_offset;
static struct node *hover_link, *focus;
static int caret;

static struct history_entry *history;
static int nhistory, hindex = -1;

static void navigate(const char *url, const char *post, bool record);
static void relayout(void);

// ---- Small helpers ----

static void set_status(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_set_text(status_label, buf);
}

static void update_buttons(void)
{
    ui_set_enabled(ui_get(win, "back"), hindex > 0);
    ui_set_enabled(ui_get(win, "forward"), hindex + 1 < nhistory);
}

static int view_height(void)
{
    return ui_rect(view).h;
}

static int max_scroll(void)
{
    return MAX(layout.height - view_height(), 0);
}

static void set_scroll(int y)
{
    y = MIN(MAX(y, 0), max_scroll());
    if (y != scroll_y) {
        scroll_y = y;
        ui_redraw(view);
    }
}

static char *escape_html(const char *s, size_t len)
{
    size_t cap = len * 6 + 1, o = 0;
    char *out = malloc(cap);

    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        const char *rep = s[i] == '<' ? "&lt;" : s[i] == '>' ? "&gt;" : s[i] == '&' ? "&amp;"
                          : s[i] == '"' ? "&quot;" : NULL;

        if (rep) {
            memcpy(out + o, rep, strlen(rep));
            o += strlen(rep);
        } else if (s[i]) {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    return out;
}

static char *downloads_dir(void)
{
    struct user_info me;
    char path[512];

    if (user_current(&me) < 0)
        return strdup("/osystem/temp");
    user_path(&me, "home/Downloads", path, sizeof(path));
    return strdup(path);
}

// ---- Loading ----

static void *worker(void *arg)
{
    struct job *j = arg;

    j->res = net_fetch(j->url, j->post);
    write(pipe_fds[1], &j, sizeof(j));
    return NULL;
}

static void job_free(struct job *j)
{
    response_free(j->res);
    free(j->url);
    free(j->post);
    free(j);
}

static void pump(void)
{
    while (queue && active < MAX_ACTIVE) {
        struct job *j = queue;

        queue = j->next;
        if (j->gen != gen) {
            job_free(j);
            continue;
        }
        if (thread_create(&j->thread, worker, j) < 0) {
            // No thread: do it now.
            j->thread = NULL;
            j->res = net_fetch(j->url, j->post);
            write(pipe_fds[1], &j, sizeof(j));
        }
        active++;
    }
}

static void start_job(int kind, const char *url, const char *post, struct node *node, int slot)
{
    struct job *j = calloc(1, sizeof(*j)), **tail;

    if (!j)
        return;
    j->kind = kind;
    j->gen = gen;
    j->url = strdup(url);
    j->post = post ? strdup(post) : NULL;
    j->node = node;
    j->slot = slot;
    for (tail = &queue; *tail; tail = &(*tail)->next)
        ;
    *tail = j;
    if (kind != JOB_PAGE)
        pending++;
    pump();
}

static void update_progress(void)
{
    if (loading)
        set_status("Loading %s...", page_url ? page_url : "");
    else if (pending)
        set_status("Loading %d more item%s...", pending, pending == 1 ? "" : "s");
    else if (!hover_link)
        set_status("Done");
}

// Background images: one shared picture per URL.
static void link_backgrounds(void)
{
    for (struct node *n = doc; n; n = node_walk(n, doc)) {
        struct style *s = n->style;
        char *url;
        int i;

        if (n->type != NODE_ELEMENT || !s || !s->background_url || s->display == D_NONE)
            continue;
        if (!(url = url_resolve(base_url, s->background_url)))
            continue;
        for (i = 0; i < nbgs; i++)
            if (!strcmp(bgs[i].url, url))
                break;
        if (i == nbgs) {
            struct bg_image *m = realloc(bgs, (nbgs + 1) * sizeof(*m));

            if (!m) {
                free(url);
                continue;
            }
            bgs = m;
            bgs[nbgs++] = (struct bg_image){ url, NULL, 1 };
            start_job(JOB_BACKGROUND, url, NULL, NULL, i);
        } else {
            free(url);
        }
        s->background_image = bgs[i].img;
    }
}

static void restyle(void)
{
    if (!doc)
        return;
    css_apply(doc, sheets, nsheets);
    link_backgrounds();
}

static void load_images(void)
{
    for (struct node *n = doc; n; n = node_walk(n, doc)) {
        const char *src;
        char *url;

        if (!node_is(n, "img") || n->image_state || !(src = node_attr(n, "src")) || !*src)
            continue;
        if (n->style && n->style->display == D_NONE)
            continue;
        if (!(url = url_resolve(base_url, src)))
            continue;
        n->image_state = 1;
        start_job(JOB_IMAGE, url, NULL, n, 0);
        free(url);
    }
}

static void relayout(void)
{
    struct rect r = ui_rect(view);

    if (!doc || r.w <= 0)
        return;
    layout_page(&layout, doc, MAX(r.w - SCROLLBAR, 100), r.h);
    scroll_y = MIN(scroll_y, max_scroll());
    dirty = false;
    ui_redraw(view);
}

static bool relayout_soon(void *u)
{
    (void)u;
    if (dirty)
        relayout();
    return false;
}

static void mark_dirty(void)
{
    if (!dirty) {
        dirty = true;
        ui_timer(120, relayout_soon, NULL);
    }
}

static void scroll_to_fragment(const char *frag)
{
    if (!frag || !*frag) {
        set_scroll(0);
        return;
    }
    for (int i = 0; i < layout.nanchors; i++) {
        const char *id = node_attr(layout.anchors[i].node, "id"), *name = node_attr(layout.anchors[i].node, "name");

        if ((id && !strcmp(id, frag)) || (name && !strcmp(name, frag))) {
            set_scroll(layout.anchors[i].y);
            return;
        }
    }
}

// ---- Pages ----

static void free_page(void)
{
    layout_free(&layout);
    node_free(doc);
    doc = NULL;
    for (int i = 1; i < nsheets; i++)
        css_free(sheets[i]);
    free(sheets);
    sheets = NULL;
    nsheets = 0;
    for (int i = 0; i < nbgs; i++) {
        free(bgs[i].url);
        if (bgs[i].img)
            surface_destroy(bgs[i].img);
    }
    free(bgs);
    bgs = NULL;
    nbgs = 0;
    hover_link = focus = NULL;
    pending = 0;
}

static void add_sheet(struct sheet *s)
{
    struct sheet **m = realloc(sheets, (nsheets + 1) * sizeof(*m));

    if (!m) {
        css_free(s);
        return;
    }
    sheets = m;
    sheets[nsheets++] = s;
}

// Sets the window title from <title>.
static void update_title(void)
{
    struct node *t = doc ? node_find(doc, "title") : NULL;
    char *text = t ? node_text(t) : NULL;
    char title[300];

    if (text && *text)
        snprintf(title, sizeof(title), "%s - Web Browser", text);
    else
        snprintf(title, sizeof(title), "%s - Web Browser", page_url ? page_url : "New page");
    ui_window_set_title(win, title);
    free(text);
}

// Gives form controls their starting state.
static void init_controls(void)
{
    for (struct node *n = doc; n; n = node_walk(n, doc)) {
        if (node_is(n, "input")) {
            const char *v = node_attr(n, "value");

            free(n->value);
            n->value = v ? strdup(v) : NULL;
            n->checked = node_attr(n, "checked") != NULL;
        } else if (node_is(n, "textarea")) {
            free(n->value);
            n->value = strdup(n->first && n->first->type == NODE_TEXT ? n->first->text : "");
        } else if (node_is(n, "select")) {
            n->selected = -1;
        }
    }
}

// Shows HTML that came from url.
static void show_html(const char *url, const char *html, size_t len)
{
    struct node *base;
    int slot;

    free_page();
    doc = html_parse(html, len);
    if (!doc)
        return;
    free(base_url);
    base_url = strdup(url);
    if ((base = node_find(doc, "base")) && node_attr(base, "href")) {
        char *b = url_resolve(url, node_attr(base, "href"));

        if (b) {
            free(base_url);
            base_url = b;
        }
    }
    add_sheet(css_default());
    // Style sheets in document order; linked ones fill their slot later.
    for (struct node *n = doc; n; n = node_walk(n, doc)) {
        if (node_is(n, "style")) {
            const char *media = node_attr(n, "media");
            char *css = node_text(n);

            if (media && strstr(media, "print") && !strstr(media, "screen")) {
                free(css);
                continue;
            }
            free(css);
            // node_text collapses spaces; take the raw text instead.
            css = NULL;
            {
                size_t total = 0;

                for (struct node *t = n->first; t; t = t->next)
                    if (t->type == NODE_TEXT)
                        total += strlen(t->text);
                if ((css = malloc(total + 1))) {
                    css[0] = 0;
                    for (struct node *t = n->first; t; t = t->next)
                        if (t->type == NODE_TEXT)
                            strcat(css, t->text);
                }
            }
            add_sheet(css_parse(css, 1));
            free(css);
        } else if (node_is(n, "link") && node_attr(n, "href") && node_attr(n, "rel")) {
            const char *rel = node_attr(n, "rel"), *media = node_attr(n, "media");
            char *href;

            if (!strstr(rel, "stylesheet") || strstr(rel, "alternate"))
                continue;
            if (media && strstr(media, "print") && !strstr(media, "screen") && !strstr(media, "all"))
                continue;
            if (!(href = url_resolve(base_url, node_attr(n, "href"))))
                continue;
            slot = nsheets;
            add_sheet(NULL);
            start_job(JOB_CSS, href, NULL, NULL, slot);
            free(href);
        }
    }
    init_controls();
    scroll_y = 0;
    restyle();
    load_images();
    update_title();
    relayout();
}

static void show_text_page(const char *url, const char *text, size_t len)
{
    char *esc = escape_html(text, len), *html;

    if (!esc)
        return;
    if ((html = malloc(strlen(esc) + 256))) {
        sprintf(html, "<!doctype html><style>body{margin:12px}pre{white-space:pre-wrap;font-size:14px}</style>"
                      "<pre>%s</pre>", esc);
        show_html(url, html, strlen(html));
        free(html);
    }
    free(esc);
}

static void show_message_page(const char *url, const char *title, const char *text)
{
    char *t = escape_html(title, strlen(title)), *m = escape_html(text, strlen(text)), *u = escape_html(url, strlen(url));
    char *html = malloc(4096 + (t ? strlen(t) : 0) + (m ? strlen(m) : 0) + (u ? strlen(u) : 0));

    if (html && t && m && u) {
        sprintf(html,
                "<!doctype html><title>%s</title><style>"
                "body{font-family:sans-serif;background:#f4f5f8;color:#2b2f36;margin:0}"
                ".box{max-width:620px;margin:80px auto;background:white;border:1px solid #dde0e6;"
                "border-radius:10px;padding:32px 40px}"
                "h1{font-size:24px;margin:0 0 12px;color:#1f2329}p{line-height:1.5}"
                ".url{color:#6b7280;font-family:monospace;font-size:13px;word-break:break-all}"
                "a.button{display:inline-block;background:#2f6fdf;color:white;padding:8px 18px;"
                "border-radius:6px;text-decoration:none;margin-top:12px}</style>"
                "<div class=box><h1>%s</h1><p>%s</p><p class=url>%s</p>"
                "<a class=button href='%s'>Try again</a></div>",
                t, t, m, u, u);
        show_html(url, html, strlen(html));
    }
    free(html);
    free(t);
    free(m);
    free(u);
}

static void save_download(struct response *r)
{
    char *dir = downloads_dir(), path[700], name[256];
    const char *slash = strrchr(r->url, '/'), *base = slash && slash[1] ? slash + 1 : "download";
    int fd;

    strlcpy(name, base, sizeof(name));
    name[strcspn(name, "?#")] = 0;
    if (!*name)
        strlcpy(name, "download", sizeof(name));
    unique_name(dir, name, path, sizeof(path));
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        size_t done = 0;

        while (done < r->len) {
            ssize_t n = write(fd, r->data + done, r->len - done);

            if (n <= 0)
                break;
            done += n;
        }
        close(fd);
        {
            char msg[900];

            snprintf(msg, sizeof(msg), "This file cannot be shown here, so it was saved as %s.", path);
            show_message_page(r->url, "Downloaded", msg);
        }
    } else {
        show_message_page(r->url, "Download failed", "The file could not be saved in your Downloads folder.");
    }
    free(dir);
}

static void page_loaded(struct response *r)
{
    char *frag;

    loading = false;
    wm_set_cursor(ui_wm_window(win), WM_CURSOR_ARROW);
    free(page_source);
    page_source = NULL;
    if (r->url && page_url && strcmp(r->url, page_url)) {
        // Redirected: keep the fragment we were going to.
        free(page_url);
        page_url = strdup(r->url);
        ui_set_text(address, page_url);
        if (hindex >= 0) {
            free(history[hindex].url);
            history[hindex].url = strdup(page_url);
        }
    }
    if (r->error) {
        show_message_page(page_url, "This page could not be opened", r->error);
    } else if (!strcmp(r->type, "text/html") || !strcmp(r->type, "application/xhtml+xml")) {
        page_source = malloc(r->len + 1);
        if (page_source) {
            memcpy(page_source, r->data, r->len);
            page_source[r->len] = 0;
        }
        show_html(page_url, r->data, r->len);
    } else if (!strncmp(r->type, "image/", 6) && strcmp(r->type, "image/svg+xml")) {
        char *u = escape_html(page_url, strlen(page_url)), *html = u ? malloc(strlen(u) * 2 + 400) : NULL;

        if (html) {
            const char *slash = strrchr(page_url, '/');

            sprintf(html, "<!doctype html><title>%s</title><style>body{margin:0;background:#2b2b2b;"
                          "text-align:center}img{margin:24px auto;max-width:96%%;background:white}</style>"
                          "<img src='%s'>", slash ? slash + 1 : u, u);
            show_html(page_url, html, strlen(html));
        }
        free(html);
        free(u);
    } else if (!strncmp(r->type, "text/", 5) || !strcmp(r->type, "application/json")
               || !strcmp(r->type, "application/javascript") || !strcmp(r->type, "image/svg+xml")) {
        show_text_page(page_url, r->data, r->len);
    } else {
        save_download(r);
    }
    frag = page_url ? strchr(page_url, '#') : NULL;
    scroll_y = 0;
    if (hindex >= 0 && history[hindex].scroll)
        set_scroll(history[hindex].scroll);
    else if (frag)
        scroll_to_fragment(frag + 1);
    update_buttons();
    update_progress();
}

static void job_done(int fd, void *u)
{
    struct job *j;

    (void)u;
    if (read(fd, &j, sizeof(j)) != sizeof(j))
        return;
    if (j->thread)
        thread_join(j->thread, NULL);
    active--;
    if (j->kind != JOB_PAGE && j->gen == gen)
        pending--;
    if (j->gen != gen || !j->res) {
        if (j->gen == gen && j->kind == JOB_PAGE)
            page_loaded(&(struct response){ .error = "Out of memory.", .url = j->url });
        job_free(j);
        pump();
        return;
    }
    switch (j->kind) {
    case JOB_PAGE:
        page_loaded(j->res);
        break;
    case JOB_CSS:
        if (!j->res->error && j->res->status < 400 && j->slot < nsheets && !sheets[j->slot]) {
            sheets[j->slot] = css_parse(j->res->data, 1);
            restyle();
            mark_dirty();
        }
        break;
    case JOB_IMAGE: {
        struct surface *img = j->res->error ? NULL : image_decode(j->res->data, j->res->len);

        j->node->image = img;
        j->node->image_state = img ? 2 : 3;
        mark_dirty();
        break;
    }
    case JOB_BACKGROUND:
        if (j->slot < nbgs) {
            bgs[j->slot].img = j->res->error ? NULL : image_decode(j->res->data, j->res->len);
            bgs[j->slot].state = bgs[j->slot].img ? 2 : 3;
            if (bgs[j->slot].img) {
                link_backgrounds();
                mark_dirty();
            }
        }
        break;
    }
    job_free(j);
    pump();
    update_progress();
}

static const char home_html[] =
    "<!doctype html><html><head><title>Start</title><style>"
    "body{margin:0;font-family:sans-serif;background:#eef1f6;color:#1f2329}"
    ".hero{background:#2f6fdf;color:white;padding:48px 0 56px;text-align:center}"
    ".hero h1{margin:0;font-size:34px}.hero p{margin:8px 0 0;opacity:.9;font-size:16px}"
    ".grid{display:flex;flex-wrap:wrap;gap:16px;max-width:860px;margin:-28px auto 32px;padding:0 16px}"
    ".card{flex:1 1 240px;background:white;border-radius:10px;padding:18px 20px;border:1px solid #dde2ea}"
    ".card h2{font-size:17px;margin:0 0 8px}.card p{margin:0 0 10px;color:#5b6270;font-size:14px;line-height:1.45}"
    ".card a{color:#2f6fdf;text-decoration:none;font-weight:bold;font-size:14px}"
    "footer{text-align:center;color:#7a8190;font-size:12px;padding-bottom:24px}"
    "</style></head><body>"
    "<div class=hero><h1>Aegis Web Browser</h1><p>Type an address above, or start here.</p></div>"
    "<div class=grid>"
    "<div class=card><h2>Welcome tour</h2><p>A page that shows what this browser draws: text, lists, "
    "tables, flexible boxes, forms and pictures.</p><a href='file:///osystem/resources/browser/welcome.html'>Open the tour</a></div>"
    "<div class=card><h2>Your files</h2><p>Browse the folders in your home and open pages, pictures and text "
    "files.</p><a href='~'>Open your home folder</a></div>"
    "<div class=card><h2>The web</h2><p>Plain HTTP and secure HTTPS sites work when the computer is "
    "online. Scripts do not run.</p><a href='http://example.com/'>Visit example.com</a></div>"
    "</div><footer>Pages are drawn by Aegis itself: its own HTML parser, CSS engine and layout.</footer>"
    "</body></html>";

static void navigate(const char *url, const char *post, bool record)
{
    char *u;

    if (!strncmp(url, "~", 1) || (url[0] == '/' && url[1] != '/'))
        u = url_from_input(url);
    else
        u = strdup(url);
    if (!u)
        return;
    // A link within the same page only scrolls.
    if (!post && page_url && doc && strchr(u, '#')) {
        size_t a = strcspn(u, "#"), b = strcspn(page_url, "#");

        if (a == b && !strncmp(u, page_url, a)) {
            if (record) {
                if (hindex >= 0)
                    history[hindex].scroll = scroll_y;
                for (int i = hindex + 1; i < nhistory; i++)
                    free(history[i].url);
                nhistory = hindex + 1;
                {
                    struct history_entry *m = realloc(history, (nhistory + 1) * sizeof(*m));

                    if (m) {
                        history = m;
                        history[nhistory++] = (struct history_entry){ strdup(u), 0 };
                        hindex = nhistory - 1;
                    }
                }
            }
            free(page_url);
            page_url = u;
            ui_set_text(address, page_url);
            scroll_to_fragment(strchr(u, '#') + 1);
            update_buttons();
            return;
        }
    }
    if (record) {
        if (hindex >= 0)
            history[hindex].scroll = scroll_y;
        for (int i = hindex + 1; i < nhistory; i++)
            free(history[i].url);
        nhistory = hindex + 1;
        {
            struct history_entry *m = realloc(history, (nhistory + 1) * sizeof(*m));

            if (m) {
                history = m;
                history[nhistory++] = (struct history_entry){ strdup(u), 0 };
                hindex = nhistory - 1;
            }
        }
    }
    gen++;
    free(page_url);
    page_url = u;
    ui_set_text(address, page_url);
    update_buttons();
    hover_link = NULL;
    if (!strcmp(u, "about:home") || !strcmp(u, "about:blank")) {
        struct response r = { .type = "text/html", .data = (char *)(strcmp(u, "about:blank") ? home_html : ""),
                              .url = u };

        r.len = strlen(r.data);
        loading = true;
        page_loaded(&r);
        return;
    }
    if (!strncmp(u, "view-source:", 12)) {
        loading = false;
        if (page_source)
            show_text_page(u, page_source, strlen(page_source));
        update_progress();
        return;
    }
    loading = true;
    wm_set_cursor(ui_wm_window(win), WM_CURSOR_WAIT);
    update_progress();
    start_job(JOB_PAGE, u, post, NULL, 0);
}

// ---- Forms ----

static struct node *form_of(struct node *n)
{
    while (n && !node_is(n, "form"))
        n = n->parent;
    return n;
}

static void append_field(char **body, size_t *len, size_t *cap, const char *name, const char *value)
{
    if (*len)
        url_encode_append(body, len, cap, ""), (*body)[(*len)++] = '&', (*body)[*len] = 0;
    url_encode_append(body, len, cap, name);
    if (*len + 2 > *cap) {
        char *m = realloc(*body, *cap = *len + 64);

        if (!m)
            return;
        *body = m;
    }
    (*body)[(*len)++] = '=';
    (*body)[*len] = 0;
    url_encode_append(body, len, cap, value);
}

static void submit(struct node *form, struct node *submitter)
{
    char *body = NULL, *action, *url;
    size_t len = 0, cap = 0;
    const char *method = node_attr(form, "method");
    bool post = method && !strcasecmp(method, "post");

    body = malloc(cap = 256);
    if (!body)
        return;
    body[0] = 0;
    for (struct node *n = form->first; n; n = node_walk(n, form)) {
        const char *name = node_attr(n, "name"), *type = node_attr(n, "type");

        if (!name || !*name || node_attr(n, "disabled"))
            continue;
        if (node_is(n, "input")) {
            if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
                if (n->checked)
                    append_field(&body, &len, &cap, name, n->value ? n->value : "on");
            } else if (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "button")
                                || !strcasecmp(type, "image") || !strcasecmp(type, "reset"))) {
                if (n == submitter)
                    append_field(&body, &len, &cap, name, control_value(n));
            } else if (!type || strcasecmp(type, "file")) {
                append_field(&body, &len, &cap, name, control_value(n));
            }
        } else if (node_is(n, "textarea")) {
            append_field(&body, &len, &cap, name, control_value(n));
        } else if (node_is(n, "select")) {
            struct node *o = select_option(n, select_current(n));

            if (o) {
                char *t = node_attr(o, "value") ? strdup(node_attr(o, "value")) : node_text(o);

                append_field(&body, &len, &cap, name, t ? t : "");
                free(t);
            }
        } else if (node_is(n, "button") && n == submitter) {
            append_field(&body, &len, &cap, name, node_attr(n, "value") ? node_attr(n, "value") : "");
        }
    }
    action = node_attr(form, "action") ? url_resolve(base_url, node_attr(form, "action")) : strdup(page_url);
    if (!action) {
        free(body);
        return;
    }
    if (post) {
        navigate(action, body, true);
    } else {
        action[strcspn(action, "?#")] = 0;
        if ((url = malloc(strlen(action) + len + 2))) {
            sprintf(url, "%s?%s", action, body);
            navigate(url, NULL, true);
            free(url);
        }
    }
    free(action);
    free(body);
}

static bool is_text_control(struct node *n)
{
    const char *type = node_attr(n, "type");

    if (node_is(n, "textarea"))
        return true;
    if (!node_is(n, "input"))
        return false;
    return !type || (strcasecmp(type, "checkbox") && strcasecmp(type, "radio") && strcasecmp(type, "submit")
                     && strcasecmp(type, "button") && strcasecmp(type, "reset") && strcasecmp(type, "image")
                     && strcasecmp(type, "hidden") && strcasecmp(type, "file"));
}

static void edit_insert(const char *text)
{
    const char *v = control_value(focus);
    size_t a = strlen(v), b = strlen(text);
    char *m = malloc(a + b + 1);

    if (!m)
        return;
    caret = MIN(caret, (int)a);
    memcpy(m, v, caret);
    memcpy(m + caret, text, b);
    memcpy(m + caret + b, v + caret, a - caret + 1);
    free(focus->value);
    focus->value = m;
    caret += b;
}

static void edit_delete(int from, int to)
{
    const char *v = control_value(focus);
    char *m;

    if (from >= to)
        return;
    if (!(m = strdup(v)))
        return;
    memmove(m + from, m + to, strlen(m + to) + 1);
    free(focus->value);
    focus->value = m;
    caret = from;
}

static int choice_index;

static void on_choice(struct widget *w, void *u)
{
    (void)w;
    if (focus && node_is(focus, "select")) {
        focus->selected = (int)(intptr_t)u;
        mark_dirty();
        ui_redraw(view);
    }
}

static void open_select(struct node *sel, struct rect r)
{
    struct widget *menu = ui_get(win, "choices");
    int i = 0;

    while (ui_children(menu))
        ui_remove(ui_child(menu, 0));
    for (struct node *o = sel->first; o; o = node_walk(o, sel)) {
        struct widget *item;
        char *t;

        if (!node_is(o, "option"))
            continue;
        if (!(item = ui_create(win, "item")))
            break;
        t = node_text(o);
        ui_set_attr(item, "text", t && *t ? t : " ");
        free(t);
        ui_set_handler(item, "click", on_choice, (void *)(intptr_t)i);
        ui_add(menu, item);
        i++;
    }
    choice_index = i;
    if (i)
        ui_menu_popup(menu, view, ui_rect(view).x + r.x, ui_rect(view).y + r.y + r.h);
}

// ---- Input ----

static struct item *hit(int x, int y, bool links_only)
{
    int px = x, py = y + scroll_y;

    for (int i = layout.n - 1; i >= 0; i--) {
        struct item *it = &layout.items[i];

        if (it->clipped && !rect_contains(it->clip, px, py))
            continue;
        if (!rect_contains(it->r, px, py))
            continue;
        if (it->kind == ITEM_CONTROL && !links_only)
            return it;
        if (it->link && (it->kind == ITEM_TEXT || it->kind == ITEM_IMAGE || it->kind == ITEM_RECT))
            return it;
    }
    return NULL;
}

static void scrollbar_geometry(struct rect r, int *thumb_y, int *thumb_h)
{
    int total = MAX(layout.height, 1);

    *thumb_h = MAX(r.h * r.h / total, 24);
    *thumb_y = max_scroll() ? (r.h - *thumb_h) * scroll_y / max_scroll() : 0;
}

static void paint(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    if (!doc) {
        gfx_fill(g, r, RGB(0xFFFFFF));
        return;
    }
    paint_page(&layout, g, (struct rect){ r.x, r.y, r.w - SCROLLBAR, r.h }, 0, scroll_y, focus, hover_link, caret);
    // The scroll bar.
    gfx_fill(g, (struct rect){ r.x + r.w - SCROLLBAR, r.y, SCROLLBAR, r.h }, RGB(0xF0F0F2));
    if (max_scroll()) {
        int ty, th;

        scrollbar_geometry(r, &ty, &th);
        gfx_fill_rounded(g, (struct rect){ r.x + r.w - SCROLLBAR + 2, r.y + ty + 2, SCROLLBAR - 4, th - 4 }, 4,
                         scroll_drag ? RGB(0x8A8F99) : RGB(0xB8BCC4));
    }
}

static void set_hover(struct node *link)
{
    if (link == hover_link)
        return;
    hover_link = link;
    if (link && node_attr(link, "href")) {
        char *u = url_resolve(base_url, node_attr(link, "href"));

        set_status("%s", u ? u : "");
        free(u);
    } else {
        update_progress();
    }
}

static void activate_link(struct node *a)
{
    const char *href = node_attr(a, "href");
    char *u;

    if (node_is(a, "button")) {
        // A <button> submits its form unless it is a plain button.
        const char *type = node_attr(a, "type");

        if (form_of(a) && (!type || !strcasecmp(type, "submit")))
            submit(form_of(a), a);
        return;
    }
    if (!href)
        return;
    if (!strncasecmp(href, "javascript:", 11)) {
        set_status("Scripts do not run in this browser.");
        return;
    }
    if (!strncasecmp(href, "mailto:", 7)) {
        set_status("Email links open in the Email app.");
        return;
    }
    if ((u = url_resolve(base_url, href))) {
        navigate(u, NULL, true);
        free(u);
    }
}

static void click_control(struct node *n, struct rect r)
{
    const char *type = node_attr(n, "type");
    struct node *form = form_of(n);

    focus = n;
    if (is_text_control(n)) {
        caret = strlen(control_value(n));
    } else if (type && !strcasecmp(type, "checkbox")) {
        n->checked = !n->checked;
    } else if (type && !strcasecmp(type, "radio")) {
        const char *name = node_attr(n, "name");

        for (struct node *o = form ? form : doc; o; o = node_walk(o, form ? form : doc)) {
            const char *on = node_attr(o, "name");

            if (node_is(o, "input") && on && name && !strcmp(on, name))
                o->checked = false;
        }
        n->checked = true;
    } else if (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "image"))) {
        if (form)
            submit(form, n);
        return;
    } else if (type && !strcasecmp(type, "reset")) {
        if (form)
            for (struct node *o = form; o; o = node_walk(o, form))
                if (node_is(o, "input") || node_is(o, "textarea") || node_is(o, "select")) {
                    free(o->value);
                    o->value = node_attr(o, "value") ? strdup(node_attr(o, "value")) : NULL;
                    o->checked = node_attr(o, "checked") != NULL;
                    o->selected = -1;
                }
    } else if (node_is(n, "select")) {
        open_select(n, (struct rect){ r.x, r.y - scroll_y, r.w, r.h });
    }
    ui_redraw(view);
}

static void input(struct widget *w, struct wm_event *ev, void *u)
{
    struct rect r = ui_rect(w);

    (void)u;
    if (ev->type == WM_EV_POINTER) {
        struct item *it;

        switch (ev->kind) {
        case WM_PTR_WHEEL:
            set_scroll(scroll_y - ev->detail * 60);
            break;
        case WM_PTR_DOWN:
            ui_focus(w);
            if (ev->detail != BTN_LEFT)
                break;
            if (ev->x >= r.w - SCROLLBAR) {
                int ty, th;

                scrollbar_geometry(r, &ty, &th);
                if (ev->y >= ty && ev->y < ty + th) {
                    scroll_drag = true;
                    drag_offset = ev->y - ty;
                } else {
                    set_scroll(scroll_y + (ev->y < ty ? -1 : 1) * (r.h - 40));
                }
                ui_redraw(w);
                break;
            }
            it = hit(ev->x, ev->y, false);
            if (it && it->kind == ITEM_CONTROL) {
                click_control(it->node, it->r);
            } else if (it && it->link) {
                activate_link(it->link);
            } else {
                focus = NULL;
                ui_redraw(w);
            }
            break;
        case WM_PTR_MOVE:
            if (scroll_drag) {
                int ty, th;

                scrollbar_geometry(r, &ty, &th);
                if (r.h > th)
                    set_scroll((ev->y - drag_offset) * max_scroll() / (r.h - th));
                break;
            }
            it = hit(ev->x, ev->y, false);
            set_hover(it && it->link ? it->link : NULL);
            wm_set_cursor(ui_wm_window(win), loading ? WM_CURSOR_WAIT : it && it->link ? WM_CURSOR_HAND
                                                     : it && is_text_control(it->node) ? WM_CURSOR_TEXT
                                                     : WM_CURSOR_ARROW);
            break;
        case WM_PTR_UP:
            if (scroll_drag) {
                scroll_drag = false;
                ui_redraw(w);
            }
            break;
        case WM_PTR_LEAVE:
            set_hover(NULL);
            break;
        }
        return;
    }
    if (ev->type != WM_EV_KEY || !ev->value)
        return;
    if (focus && is_text_control(focus) && !(ev->mods & (MOD_ALT | MOD_META))) {
        const char *v = control_value(focus);
        int len = strlen(v);
        bool textarea = node_is(focus, "textarea");

        if ((ev->mods & MOD_CTRL) && ev->key == KEY_A + 'v' - 'a') {
            const char *clip = ui_clipboard_get();

            if (clip)
                edit_insert(clip);
        } else if (ev->mods & MOD_CTRL) {
            return;
        } else if (ev->key == KEY_BACKSPACE) {
            if (caret > 0)
                edit_delete(utf8_prev(v, caret), caret);
        } else if (ev->key == KEY_DELETE) {
            if (caret < len)
                edit_delete(caret, utf8_next(v, caret));
        } else if (ev->key == KEY_LEFT) {
            if (caret > 0)
                caret = utf8_prev(v, caret);
        } else if (ev->key == KEY_RIGHT) {
            if (caret < len)
                caret = utf8_next(v, caret);
        } else if (ev->key == KEY_HOME) {
            caret = 0;
        } else if (ev->key == KEY_END) {
            caret = len;
        } else if (ev->key == KEY_ENTER || ev->key == KEY_KPENTER) {
            if (textarea) {
                edit_insert("\n");
            } else if (form_of(focus)) {
                submit(form_of(focus), NULL);
                return;
            }
        } else if (ev->key == KEY_ESC) {
            focus = NULL;
        } else if (ev->text[0] && (unsigned char)ev->text[0] >= 0x20) {
            edit_insert(ev->text);
        } else {
            return;
        }
        if (textarea)
            mark_dirty();
        ui_redraw(view);
        return;
    }
    if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_META))
        return;
    switch (ev->key) {
    case KEY_DOWN:
        set_scroll(scroll_y + 48);
        break;
    case KEY_UP:
        set_scroll(scroll_y - 48);
        break;
    case KEY_PAGEDOWN:
    case KEY_SPACE:
        set_scroll(scroll_y + (ev->key == KEY_SPACE && (ev->mods & MOD_SHIFT) ? -1 : 1) * (r.h - 40));
        break;
    case KEY_PAGEUP:
        set_scroll(scroll_y - (r.h - 40));
        break;
    case KEY_HOME:
        set_scroll(0);
        break;
    case KEY_END:
        set_scroll(max_scroll());
        break;
    case KEY_BACKSPACE:
        if (hindex > 0) {
            history[hindex].scroll = scroll_y;
            hindex--;
            navigate(history[hindex].url, NULL, false);
        }
        break;
    }
}

// ---- Toolbar ----

static void on_go(struct widget *w, void *u)
{
    char *url = url_from_input(ui_text(address));

    (void)w;
    (void)u;
    if (url) {
        navigate(url, NULL, true);
        free(url);
    }
    ui_focus(view);
}

static void on_back(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (hindex > 0) {
        history[hindex].scroll = scroll_y;
        hindex--;
        navigate(history[hindex].url, NULL, false);
    }
}

static void on_forward(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (hindex + 1 < nhistory) {
        history[hindex].scroll = scroll_y;
        hindex++;
        navigate(history[hindex].url, NULL, false);
    }
}

static void on_reload(struct widget *w, void *u)
{
    char *url;

    (void)w;
    (void)u;
    if (!page_url || !(url = strdup(page_url)))
        return;
    if (hindex >= 0)
        history[hindex].scroll = scroll_y;
    navigate(url, NULL, false);
    free(url);
}

static void on_home(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    navigate("about:home", NULL, true);
}

static void on_source(struct widget *w, void *u)
{
    char *url;

    (void)w;
    (void)u;
    if (!page_url || !page_source || !strncmp(page_url, "view-source:", 12))
        return;
    if ((url = malloc(strlen(page_url) + 16))) {
        sprintf(url, "view-source:%s", page_url);
        navigate(url, NULL, true);
        free(url);
    }
}

static void on_downloads(struct widget *w, void *u)
{
    char *dir = downloads_dir(), *url;

    (void)w;
    (void)u;
    if (dir && (url = malloc(strlen(dir) + 16))) {
        sprintf(url, "file://%s/", dir);
        navigate(url, NULL, true);
        free(url);
    }
    free(dir);
}

static void on_key(struct ui_window *w, struct wm_event *ev, void *u)
{
    (void)w;
    (void)u;
    if (!ev->value || !(ev->mods & MOD_CTRL))
        return;
    if (ev->key == KEY_A + 'l' - 'a') {
        ui_focus(address);
        ui_edit_command(address, "selectall");
    }
}

static int last_w, last_h;

static bool watch_size(void *u)
{
    struct rect r = ui_rect(view);

    (void)u;
    if (r.w != last_w || r.h != last_h) {
        last_w = r.w;
        last_h = r.h;
        relayout();
    }
    return true;
}

// browser --check URL: loads and lays out a page without a window, and
// reports what it found (for tests).
static int check(const char *where)
{
    char *url = url_from_input(where);
    struct response *r = url ? net_fetch(url, NULL) : NULL;
    int links = 0, images = 0, texts = 0;
    struct node *title;
    char *t;

    if (!r || r->error) {
        printf("browser: cannot load %s: %s\n", where, r && r->error ? r->error : "out of memory");
        return 1;
    }
    doc = html_parse(r->data, r->len);
    base_url = strdup(r->url);
    add_sheet(css_default());
    for (struct node *n = doc; n; n = node_walk(n, doc))
        if (node_is(n, "style")) {
            char *css = n->first && n->first->type == NODE_TEXT ? n->first->text : "";

            add_sheet(css_parse(css, 1));
        }
    init_controls();
    css_apply(doc, sheets, nsheets);
    layout_page(&layout, doc, 1000, 700);
    for (int i = 0; i < layout.n; i++) {
        texts += layout.items[i].kind == ITEM_TEXT;
        images += layout.items[i].kind == ITEM_IMAGE;
        links += layout.items[i].link != NULL;
    }
    title = node_find(doc, "title");
    t = title ? node_text(title) : NULL;
    printf("browser: \"%s\": %d items, %d words, %d pictures, %d link parts, %dx%d\n", t ? t : "", layout.n,
           texts, images, links, layout.width, layout.height);
    free(t);
    if (texts > 50 && layout.height > 1000)
        printf("browser: ok\n");
    response_free(r);
    return 0;
}

// A file or a link dropped on the page opens it.
static int drop_over(struct widget *w, struct ui_drop *d, void *u)
{
    (void)w;
    (void)u;
    if (d->kind == WM_DRAG_LEAVE || (strcmp(d->type, "files") && strcmp(d->type, "text"))
        || (!strcmp(d->type, "text") && (memchr(d->data, '\n', d->len) || !d->len || d->len > 2000)))
        return 0;
    return d->actions & WM_DND_LINK ? WM_DND_LINK : d->actions & WM_DND_COPY ? WM_DND_COPY : 0;
}

static void drop(struct widget *w, struct ui_drop *d, int action, void *u)
{
    const char *nl = strchr(d->data, '\n');
    char first[2048], *url;

    (void)w;
    (void)action;
    (void)u;
    snprintf(first, sizeof(first), "%.*s", nl ? (int)(nl - d->data) : (int)strlen(d->data), d->data);
    if ((url = url_from_input(first))) {
        navigate(url, NULL, true);
        free(url);
    }
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "go", on_go }, { "back", on_back }, { "forward", on_forward }, { "reload", on_reload },
        { "home", on_home }, { "source", on_source }, { "downloads", on_downloads }, { NULL, NULL },
    };

    if (argc > 2 && !strcmp(argv[1], "--check"))
        return check(argv[2]);
    ui_load_user_theme();
    if (pipe(pipe_fds) < 0 || !(win = ui_load_string_named(window_aui, handlers, NULL, "browser")))
        return 1;
    view = ui_get(win, "view");
    address = ui_get(win, "address");
    status_label = ui_get(win, "status");
    ui_canvas_set(view, paint, input, NULL);
    ui_set_drop_target(view, drop_over, drop, NULL);
    ui_on_key(win, on_key, NULL);
    ui_watch_fd(pipe_fds[0], job_done, NULL);
    ui_window_show(win);
    ui_timer(200, watch_size, NULL);
    if (argc > 1) {
        char *url = url_from_input(argv[1]);

        navigate(url ? url : "about:home", NULL, true);
        free(url);
    } else {
        navigate("about:home", NULL, true);
    }
    ui_focus(view);
    return ui_run();
}
