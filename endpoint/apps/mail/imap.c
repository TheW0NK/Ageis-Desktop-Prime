#include "mail.h"

// IMAP4rev1 (RFC 3501), the parts a mail reader needs: sign in, list
// folders, fetch new messages, set flags, delete and append.

#define MAX_LITERAL (64u << 20)

struct response {
    char *text;                     // the line(s), with a literal replaced by "{}"
    char *literal;
    size_t literal_len;
};

typedef void (*untagged_fn)(struct response *r, void *u);

// Reads one response (with up to one literal). Returns -1 on errors.
static int read_response(struct imap *im, struct response *r)
{
    char line[8192];
    ssize_t n;

    memset(r, 0, sizeof(*r));
    if ((n = mconn_line(&im->c, line, sizeof(line))) < 0)
        return -1;
    r->text = strdup(line);
    // "... {123}" means 123 bytes follow, then the rest of the response.
    while (n > 2 && line[n - 1] == '}' && strrchr(line, '{')) {
        char *brace = strrchr(line, '{');
        size_t len = strtoul(brace + 1, NULL, 10);
        char *rest;

        if (len > MAX_LITERAL)
            return -1;
        free(r->literal);
        if (!(r->literal = malloc(len + 1)) || mconn_read(&im->c, r->literal, len) < 0)
            return -1;
        r->literal[len] = 0;
        r->literal_len = len;
        if ((n = mconn_line(&im->c, line, sizeof(line))) < 0)
            return -1;
        rest = malloc(strlen(r->text) + n + 4);
        if (!rest)
            return -1;
        sprintf(rest, "%s%s", r->text, line);
        free(r->text);
        r->text = rest;
    }
    return 0;
}

static void response_clear(struct response *r)
{
    free(r->text);
    free(r->literal);
    memset(r, 0, sizeof(*r));
}

// Sends a tagged command and reads to its reply. 0 for OK.
static int command(struct imap *im, untagged_fn fn, void *u, const char *fmt, ...)
{
    char cmd[2048], tag[16];
    va_list ap;
    int n;

    snprintf(tag, sizeof(tag), "a%d", ++im->tag);
    n = snprintf(cmd, sizeof(cmd), "%s ", tag);
    va_start(ap, fmt);
    n += vsnprintf(cmd + n, sizeof(cmd) - n - 2, fmt, ap);
    va_end(ap);
    strlcat(cmd, "\r\n", sizeof(cmd));
    if (mconn_send(&im->c, cmd, strlen(cmd)) < 0) {
        strlcpy(im->error, im->c.error, sizeof(im->error));
        return -1;
    }
    for (;;) {
        struct response r;

        if (read_response(im, &r) < 0) {
            response_clear(&r);
            strlcpy(im->error, im->c.error[0] ? im->c.error : "The server's answer was not understood.",
                    sizeof(im->error));
            return -1;
        }
        if (!strncmp(r.text, tag, strlen(tag)) && r.text[strlen(tag)] == ' ') {
            const char *status = r.text + strlen(tag) + 1;
            int ok = strncasecmp(status, "OK", 2) ? -1 : 0;

            if (ok < 0)
                snprintf(im->error, sizeof(im->error), "The server said: %s", status);
            response_clear(&r);
            return ok;
        }
        if (r.text[0] == '*' && fn)
            fn(&r, u);
        response_clear(&r);
    }
}

// A string as an IMAP quoted string.
static void quote(const char *s, char *out, size_t size)
{
    size_t o = 0;

    out[o++] = '"';
    for (; *s && o + 3 < size; s++) {
        if (*s == '"' || *s == '\\')
            out[o++] = '\\';
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
}

int imap_open(struct imap *im, const struct account *a, const char *password)
{
    char line[1024], user[300], pass[600];

    memset(im, 0, sizeof(*im));
    if (mconn_open(&im->c, a->imap_host, a->imap_port, a->imap_security == SEC_TLS, a->ca_file) < 0) {
        strlcpy(im->error, im->c.error, sizeof(im->error));
        return -1;
    }
    if (mconn_line(&im->c, line, sizeof(line)) < 0 || strncmp(line, "* ", 2)) {
        strlcpy(im->error, "The mail server did not greet us.", sizeof(im->error));
        mconn_close(&im->c);
        return -1;
    }
    if (a->imap_security == SEC_STARTTLS) {
        if (command(im, NULL, NULL, "STARTTLS") < 0
            || mconn_starttls(&im->c, a->imap_host, a->ca_file) < 0) {
            if (!im->error[0])
                strlcpy(im->error, im->c.error, sizeof(im->error));
            mconn_close(&im->c);
            return -1;
        }
    }
    quote(a->user[0] ? a->user : a->email, user, sizeof(user));
    quote(password, pass, sizeof(pass));
    if (command(im, NULL, NULL, "LOGIN %s %s", user, pass) < 0) {
        memset(pass, 0, sizeof(pass));
        snprintf(im->error, sizeof(im->error), "Signing in to %s failed. Check the name and password.",
                 a->imap_host);
        mconn_close(&im->c);
        return -1;
    }
    memset(pass, 0, sizeof(pass));
    return 0;
}

struct list_state {
    char (*names)[128];
    int n, max;
};

static void on_list(struct response *r, void *u)
{
    struct list_state *s = u;
    const char *p = r->text + 2, *name;
    size_t len;

    if (strncasecmp(p, "LIST ", 5) || strstr(p, "\\Noselect") || strstr(p, "\\NonExistent") || s->n >= s->max)
        return;
    // The name is last: quoted, an atom, or a literal.
    if (r->literal) {
        strlcpy(s->names[s->n++], r->literal, 128);
        return;
    }
    len = strlen(p);
    if (len && p[len - 1] == '"') {
        const char *e = p + len - 1, *q = e - 1;
        char *o;

        while (q > p && !(*q == '"' && q[-1] != '\\'))
            q--;
        o = s->names[s->n];
        for (q++; q < e && o - s->names[s->n] < 127; q++) {
            if (*q == '\\' && q + 1 < e)
                q++;
            *o++ = *q;
        }
        *o = 0;
        s->n++;
        return;
    }
    name = strrchr(p, ' ');
    if (name)
        strlcpy(s->names[s->n++], name + 1, 128);
}

int imap_list(struct imap *im, char names[][128], int max)
{
    struct list_state s = { names, 0, max };

    if (command(im, on_list, &s, "LIST \"\" \"*\"") < 0)
        return -1;
    // INBOX first.
    for (int i = 1; i < s.n; i++)
        if (!strcasecmp(names[i], "INBOX")) {
            char tmp[128];

            memcpy(tmp, names[i], 128);
            memmove(names[1], names[0], (size_t)i * 128);
            memcpy(names[0], tmp, 128);
        }
    return s.n;
}

static int select_folder(struct imap *im, const char *folder)
{
    char q[300];

    quote(folder, q, sizeof(q));
    return command(im, NULL, NULL, "SELECT %s", q);
}

struct uid_list {
    uint32_t *uids;
    int n;
};

static void on_search(struct response *r, void *u)
{
    struct uid_list *l = u;
    const char *p = r->text + 2;

    if (strncasecmp(p, "SEARCH", 6))
        return;
    for (p += 6; *p;) {
        char *end;
        unsigned long v;

        while (*p == ' ')
            p++;
        v = strtoul(p, &end, 10);
        if (end == p)
            break;
        {
            uint32_t *m = realloc(l->uids, (l->n + 1) * sizeof(uint32_t));

            if (!m)
                return;
            l->uids = m;
            l->uids[l->n++] = v;
        }
        p = end;
    }
}

// FLAGS (...) out of a FETCH response.
static void fetch_flags(const char *text, char *out, size_t size)
{
    const char *f = strstr(text, "FLAGS (");

    out[0] = 0;
    if (f) {
        const char *e = strchr(f + 7, ')');

        snprintf(out, size, "%.*s", e ? (int)(e - f - 7) : 0, f + 7);
    }
}

static uint32_t fetch_uid(const char *text)
{
    const char *u = strstr(text, "UID ");

    return u ? strtoul(u + 4, NULL, 10) : 0;
}

struct fetch_state {
    const char *folder;
    int added;
};

static void on_fetch_body(struct response *r, void *u)
{
    struct fetch_state *s = u;
    char flags[200];
    uint32_t uid = fetch_uid(r->text);

    if (!strstr(r->text, "FETCH") || !r->literal || !uid)
        return;
    fetch_flags(r->text, flags, sizeof(flags));
    store_add(s->folder, uid, flags, r->literal, r->literal_len);
    s->added++;
}

static void on_fetch_flags(struct response *r, void *u)
{
    struct fetch_state *s = u;
    char flags[200];
    uint32_t uid = fetch_uid(r->text);

    if (!strstr(r->text, "FETCH") || !uid || !store_has(s->folder, uid))
        return;
    fetch_flags(r->text, flags, sizeof(flags));
    store_set_flags(s->folder, uid, flags);
}

int imap_sync(struct imap *im, const char *folder)
{
    struct uid_list l = { NULL, 0 };
    struct fetch_state s = { folder, 0 };

    if (select_folder(im, folder) < 0 || command(im, on_search, &l, "UID SEARCH ALL") < 0) {
        free(l.uids);
        return -1;
    }
    store_keep_only(folder, l.uids, l.n);
    for (int i = 0; i < l.n; i++) {
        if (store_has(folder, l.uids[i]))
            continue;
        if (command(im, on_fetch_body, &s, "UID FETCH %u (UID FLAGS BODY.PEEK[])", l.uids[i]) < 0) {
            free(l.uids);
            return -1;
        }
    }
    if (l.n)
        command(im, on_fetch_flags, &s, "UID FETCH 1:* (UID FLAGS)");
    free(l.uids);
    return s.added;
}

int imap_flag(struct imap *im, const char *folder, uint32_t uid, const char *flag, bool on)
{
    if (select_folder(im, folder) < 0)
        return -1;
    return command(im, NULL, NULL, "UID STORE %u %cFLAGS (%s)", uid, on ? '+' : '-', flag);
}

int imap_delete(struct imap *im, const char *folder, uint32_t uid, const char *trash)
{
    if (select_folder(im, folder) < 0)
        return -1;
    if (trash && *trash && strcmp(folder, trash)) {
        char q[300];

        quote(trash, q, sizeof(q));
        if (command(im, NULL, NULL, "UID COPY %u %s", uid, q) < 0)
            return -1;
    }
    if (command(im, NULL, NULL, "UID STORE %u +FLAGS (\\Deleted)", uid) < 0)
        return -1;
    return command(im, NULL, NULL, "EXPUNGE");
}

int imap_append(struct imap *im, const char *folder, const char *msg, size_t len, const char *flags)
{
    char q[300], line[1024];
    char tag[16];

    quote(folder, q, sizeof(q));
    snprintf(tag, sizeof(tag), "a%d", ++im->tag);
    if (mconn_printf(&im->c, "%s APPEND %s (%s) {%lu}\r\n", tag, q, flags ? flags : "", (unsigned long)len) < 0)
        return -1;
    // Wait for "+ go ahead".
    for (;;) {
        if (mconn_line(&im->c, line, sizeof(line)) < 0)
            return -1;
        if (line[0] == '+')
            break;
        if (!strncmp(line, tag, strlen(tag))) {
            snprintf(im->error, sizeof(im->error), "The server said: %s", line + strlen(tag) + 1);
            return -1;
        }
    }
    if (mconn_send(&im->c, msg, len) < 0 || mconn_send(&im->c, "\r\n", 2) < 0)
        return -1;
    for (;;) {
        if (mconn_line(&im->c, line, sizeof(line)) < 0)
            return -1;
        if (!strncmp(line, tag, strlen(tag)))
            return strncasecmp(line + strlen(tag) + 1, "OK", 2) ? -1 : 0;
    }
}

void imap_close(struct imap *im)
{
    command(im, NULL, NULL, "LOGOUT");
    mconn_close(&im->c);
}
