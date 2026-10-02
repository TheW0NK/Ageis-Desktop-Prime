#include "browser.h"
#include "tls.h"

// Loading: http and https (HTTP/1.1 with redirects, chunked bodies and a
// cookie jar), file and data URLs, and URL arithmetic.

#define MAX_BODY        (48u << 20)
#define MAX_REDIRECTS   8
#define USER_AGENT      "Mozilla/5.0 (Aegis; x86_64) AegisBrowser/1.0"

// ---- URLs ----

struct url {
    char scheme[16];
    char host[256];
    int port;
    char path[2048];                // path?query (no fragment)
};

static bool parse_url(const char *s, struct url *u)
{
    const char *p = strstr(s, "://"), *host, *slash, *colon, *hash;
    size_t len;

    memset(u, 0, sizeof(*u));
    if (!p || p - s >= (int)sizeof(u->scheme))
        return false;
    for (size_t i = 0; i < (size_t)(p - s); i++)
        u->scheme[i] = tolower((unsigned char)s[i]);
    host = p + 3;
    slash = host + strcspn(host, "/?#");
    len = slash - host;
    // user:password@ is dropped.
    {
        const char *at = memchr(host, '@', len);

        if (at) {
            len -= at + 1 - host;
            host = at + 1;
        }
    }
    colon = memchr(host, ':', len);
    u->port = colon ? atoi(colon + 1) : !strcmp(u->scheme, "https") ? 443 : 80;
    if (colon)
        len = colon - host;
    if (len >= sizeof(u->host))
        return false;
    memcpy(u->host, host, len);
    for (char *c = u->host; *c; c++)
        *c = tolower((unsigned char)*c);
    hash = strchr(slash, '#');
    len = hash ? (size_t)(hash - slash) : strlen(slash);
    if (len + 2 >= sizeof(u->path))
        return false;
    if (*slash != '/')
        u->path[0] = '/';
    strncat(u->path, slash, len);
    return true;
}

// Removes "." and ".." segments from a path (in place).
static void normalize_path(char *path)
{
    char *query = strpbrk(path, "?#"), saved[2048] = "";
    char *out = path, *seg = path;

    if (query) {
        strlcpy(saved, query, sizeof(saved));
        *query = 0;
    }
    while (*seg) {
        char *next = strchr(seg + 1, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);

        if ((len == 2 && !strncmp(seg, "/.", 2))) {
            if (!next) {
                *out++ = '/';
            }
        } else if (len == 3 && !strncmp(seg, "/..", 3)) {
            while (out > path && *--out != '/')
                ;
            if (!next)
                *out++ = '/';
        } else {
            memmove(out, seg, len);
            out += len;
        }
        if (!next)
            break;
        seg = next;
    }
    if (out == path)
        *out++ = '/';
    *out = 0;
    strlcat(path, saved, 2048);
}

static bool has_scheme(const char *s)
{
    const char *p = s;

    if (!isalpha((unsigned char)*p))
        return false;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    return *p == ':' && p - s >= 2;
}

char *url_resolve(const char *base, const char *href)
{
    char *out, *b;
    const char *rest;
    size_t cap;

    while (isspace((unsigned char)*href))
        href++;
    if (has_scheme(href)) {
        out = strdup(href);
        // Trailing spaces and newlines in attributes.
        if (out)
            for (size_t n = strlen(out); n && isspace((unsigned char)out[n - 1]); n--)
                out[n - 1] = 0;
        return out;
    }
    if (!base)
        return strdup(href);
    cap = strlen(base) + strlen(href) + 8;
    if (!(out = malloc(cap)))
        return NULL;
    if (href[0] == '/' && href[1] == '/') {
        const char *colon = strchr(base, ':');

        snprintf(out, cap, "%.*s:%s", colon ? (int)(colon - base) : 4, colon ? base : "http", href);
        return out;
    }
    // Split base into origin and path.
    b = strdup(base);
    if (!b) {
        free(out);
        return NULL;
    }
    {
        char *hash = strchr(b, '#');

        if (hash)
            *hash = 0;
    }
    if (!*href) {
        strlcpy(out, b, cap);
        free(b);
        return out;
    }
    if (*href == '#') {
        snprintf(out, cap, "%s%s", b, href);
        free(b);
        return out;
    }
    {
        char *scheme_end = strstr(b, "://"), *path;

        if (scheme_end)
            path = strchr(scheme_end + 3, '/');
        else if (!strncmp(b, "file:", 5))
            path = b + 5;
        else
            path = strchr(b, ':') ? strchr(b, ':') + 1 : b;
        if (!path)
            path = b + strlen(b);
        if (*href == '?') {
            char *q = strchr(path, '?');

            if (q)
                *q = 0;
            snprintf(out, cap, "%s%s", b, href);
            free(b);
            return out;
        }
        rest = href;
        if (*href == '/') {
            char *full = malloc(cap + 2048);

            *path = 0;
            if (!full) {
                free(b);
                free(out);
                return NULL;
            }
            snprintf(full, cap + 2048, "%s", rest);
            normalize_path(full);
            snprintf(out, cap, "%s%s", b, full);
            free(full);
        } else {
            // Relative: replace the last segment.
            char *q = strchr(path, '?'), *last;
            char *full;
            size_t fl;

            if (q)
                *q = 0;
            last = strrchr(path, '/');
            fl = strlen(path) + strlen(rest) + 4;
            if (!(full = malloc(fl + 2048))) {
                free(b);
                free(out);
                return NULL;
            }
            snprintf(full, fl, "%.*s/%s", last ? (int)(last - path) : 0, path, rest);
            normalize_path(full);
            *path = 0;
            snprintf(out, cap, "%s%s", b, full);
            free(full);
        }
    }
    free(b);
    return out;
}

char *url_from_input(const char *typed)
{
    char *out;

    while (isspace((unsigned char)*typed))
        typed++;
    if (!*typed)
        return strdup("about:home");
    if (has_scheme(typed))
        return strdup(typed);
    if (*typed == '/' || *typed == '~') {
        out = malloc(strlen(typed) + 512);
        if (!out)
            return NULL;
        if (*typed == '~') {
            const char *home = getenv("HOME");

            sprintf(out, "file://%s%s", home ? home : "", typed + 1);
        } else {
            sprintf(out, "file://%s", typed);
        }
        return out;
    }
    out = malloc(strlen(typed) + 16);
    if (out)
        sprintf(out, "http://%s", typed);
    return out;
}

void url_encode_append(char **buf, size_t *len, size_t *cap, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char enc[4];
        int n;

        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '*') {
            enc[0] = *p;
            n = 1;
        } else if (*p == ' ') {
            enc[0] = '+';
            n = 1;
        } else {
            enc[0] = '%';
            enc[1] = hex[*p >> 4];
            enc[2] = hex[*p & 15];
            n = 3;
        }
        if (*len + n + 1 > *cap) {
            size_t c = (*cap ? *cap * 2 : 128) + n;
            char *m = realloc(*buf, c);

            if (!m)
                return;
            *buf = m;
            *cap = c;
        }
        memcpy(*buf + *len, enc, n);
        *len += n;
        (*buf)[*len] = 0;
    }
}

// ---- Cookies ----

struct cookie {
    char host[256], name[128], *value;
    struct cookie *next;
};

static struct cookie *cookies;
static mutex_t cookie_lock;

static const char *find_nocase(const char *h, const char *n)
{
    size_t nl = strlen(n);

    for (; *h; h++)
        if (!strncasecmp(h, n, nl))
            return h;
    return NULL;
}

static bool domain_matches(const char *host, const char *domain)
{
    size_t hl = strlen(host), dl = strlen(domain);

    if (*domain == '.') {
        domain++;
        dl--;
    }
    return hl >= dl && !strcasecmp(host + hl - dl, domain) && (hl == dl || host[hl - dl - 1] == '.');
}

static void cookie_set(const char *host, const char *header)
{
    const char *eq = strchr(header, '='), *semi = strchr(header, ';'), *d;
    char domain[256];
    struct cookie *c;
    size_t nl;

    if (!eq || (semi && semi < eq))
        return;
    nl = eq - header;
    while (nl && isspace((unsigned char)header[nl - 1]))
        nl--;
    if (!nl || nl >= sizeof(c->name))
        return;
    strlcpy(domain, host, sizeof(domain));
    if ((d = find_nocase(header, "domain="))) {
        size_t len = strcspn(d + 7, ";");

        if (len < sizeof(domain)) {
            memcpy(domain, d + 7, len);
            domain[len] = 0;
            if (!domain_matches(host, domain))
                return;
        }
    }
    mutex_lock(&cookie_lock);
    for (c = cookies; c; c = c->next)
        if (!strcasecmp(c->host, domain) && strlen(c->name) == nl && !strncmp(c->name, header, nl))
            break;
    if (!c && (c = calloc(1, sizeof(*c)))) {
        strlcpy(c->host, domain, sizeof(c->host));
        memcpy(c->name, header, nl);
        c->next = cookies;
        cookies = c;
    }
    if (c) {
        free(c->value);
        c->value = strndup(eq + 1, semi ? (size_t)(semi - eq - 1) : strlen(eq + 1));
    }
    mutex_unlock(&cookie_lock);
}

static void cookie_header(const char *host, char *out, size_t size)
{
    out[0] = 0;
    mutex_lock(&cookie_lock);
    for (struct cookie *c = cookies; c; c = c->next) {
        if (!domain_matches(host, c->host) || !c->value)
            continue;
        if (!out[0])
            strlcat(out, "Cookie: ", size);
        else
            strlcat(out, "; ", size);
        strlcat(out, c->name, size);
        strlcat(out, "=", size);
        strlcat(out, c->value, size);
    }
    mutex_unlock(&cookie_lock);
    if (out[0])
        strlcat(out, "\r\n", size);
}

// ---- Responses ----

void response_free(struct response *r)
{
    if (!r)
        return;
    free(r->type);
    free(r->data);
    free(r->url);
    free(r->error);
    free(r);
}

static struct response *failed(const char *url, const char *fmt, const char *detail)
{
    struct response *r = calloc(1, sizeof(*r));
    char msg[512];

    if (!r)
        return NULL;
    snprintf(msg, sizeof(msg), fmt, detail);
    r->error = strdup(msg);
    r->url = strdup(url);
    return r;
}

static const char *type_for_name(const char *name)
{
    static const struct {
        const char *ext, *type;
    } types[] = {
        { ".html", "text/html" }, { ".htm", "text/html" }, { ".xhtml", "text/html" }, { ".css", "text/css" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".bmp", "image/bmp" }, { ".tga", "image/tga" }, { ".txt", "text/plain" }, { ".md", "text/plain" },
        { ".c", "text/plain" }, { ".h", "text/plain" }, { ".as", "text/plain" }, { ".aui", "text/plain" },
        { ".conf", "text/plain" }, { ".log", "text/plain" }, { ".json", "text/plain" }, { ".svg", "image/svg+xml" },
    };
    const char *dot = strrchr(name, '.');

    for (size_t i = 0; dot && i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(dot, types[i].ext))
            return types[i].type;
    return "application/octet-stream";
}

static void percent_decode(char *s)
{
    char *o = s;

    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };

            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

// A folder shown as a page of links.
static struct response *listing(const char *url, const char *path)
{
    struct response *r = calloc(1, sizeof(*r));
    struct dir_stream *d = opendir(path);
    struct aegis_dirent *e;
    size_t len = 0, cap = 4096;
    char *html = malloc(cap);

    if (!r || !d || !html) {
        free(r);
        free(html);
        if (d)
            closedir(d);
        return failed(url, "Cannot open the folder %s.", path);
    }
    len = snprintf(html, cap, "<!doctype html><title>%s</title><style>body{font-family:sans-serif;margin:24px}"
                   "a{text-decoration:none}li{margin:3px 0}</style><h1>%s</h1><ul>"
                   "<li><a href='../'>Parent folder</a>", path, path);
    while ((e = readdir(d))) {
        char line[600];
        int n;

        if (e->name[0] == '.')
            continue;
        n = snprintf(line, sizeof(line), "<li><a href='%s%s'>%s%s</a>", e->name,
                     e->type == DT_DIR ? "/" : "", e->name, e->type == DT_DIR ? "/" : "");
        if (len + n + 1 > cap) {
            char *m = realloc(html, cap *= 2);

            if (!m)
                break;
            html = m;
        }
        memcpy(html + len, line, n + 1);
        len += n;
    }
    closedir(d);
    r->data = html;
    r->len = len;
    r->type = strdup("text/html");
    r->url = strdup(url);
    return r;
}

static struct response *fetch_file(const char *url)
{
    char path[2048];
    struct aegis_stat st;
    struct response *r;
    int fd;
    size_t got = 0;

    strlcpy(path, url + 5, sizeof(path));
    if (!strncmp(path, "//", 2))
        memmove(path, path + 2, strlen(path + 2) + 1);
    {
        char *q = strpbrk(path, "?#");

        if (q)
            *q = 0;
    }
    percent_decode(path);
    if (stat(path, &st) < 0)
        return failed(url, "The file %s does not exist.", path);
    if (S_ISDIR(st.mode)) {
        size_t n = strlen(url);

        if (n && url[n - 1] != '/') {
            // Folders end in a slash so relative links work.
            char *slashed = malloc(n + 2);
            struct response *res;

            if (!slashed)
                return NULL;
            sprintf(slashed, "%s/", url);
            res = listing(slashed, path);
            free(slashed);
            return res;
        }
        return listing(url, path);
    }
    if (st.size > MAX_BODY)
        return failed(url, "%s is too large to show.", path);
    if ((fd = open(path, O_RDONLY)) < 0)
        return failed(url, "Cannot read %s.", path);
    if (!(r = calloc(1, sizeof(*r))) || !(r->data = malloc(st.size + 1))) {
        free(r);
        close(fd);
        return NULL;
    }
    while (got < st.size) {
        ssize_t n = read(fd, r->data + got, st.size - got);

        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    r->data[got] = 0;
    r->len = got;
    r->type = strdup(type_for_name(path));
    r->url = strdup(url);
    return r;
}

static int b64(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
           : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

static struct response *fetch_data(const char *url)
{
    const char *comma = strchr(url, ',');
    struct response *r = calloc(1, sizeof(*r));
    bool base64;
    size_t n = 0;

    if (!r || !comma) {
        free(r);
        return failed(url, "%s", "A data URL without data.");
    }
    base64 = comma - url >= 7 && !strncmp(comma - 7, ";base64", 7);
    r->type = strndup(url + 5, strcspn(url + 5, ";,"));
    if (!*r->type) {
        free(r->type);
        r->type = strdup("text/plain");
    }
    r->data = malloc(strlen(comma) + 1);
    if (!r->data) {
        response_free(r);
        return NULL;
    }
    if (base64) {
        unsigned acc = 0;
        int bits = 0;

        for (const char *p = comma + 1; *p; p++) {
            int v = b64(*p);

            if (v < 0)
                continue;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                r->data[n++] = (acc >> bits) & 0xFF;
            }
        }
    } else {
        strcpy(r->data, comma + 1);
        percent_decode(r->data);
        n = strlen(r->data);
    }
    r->data[n] = 0;
    r->len = n;
    r->url = strdup(url);
    return r;
}

// ---- HTTP ----

struct conn {
    struct tls *tls;
    int fd;
};

static ssize_t conn_read(struct conn *c, void *buf, size_t len)
{
    return c->tls ? tls_read(c->tls, buf, len) : recv(c->fd, buf, len, 0);
}

static bool conn_write(struct conn *c, const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {
        ssize_t n = c->tls ? tls_write(c->tls, p, len) : send(c->fd, p, len, 0);

        if (n <= 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}

static void conn_close(struct conn *c)
{
    if (c->tls)
        tls_close(c->tls);
    else if (c->fd >= 0)
        close(c->fd);
}

static const char *header_value(const char *headers, const char *name)
{
    size_t nl = strlen(name);

    for (const char *line = headers; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL) {
        if (!strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;

            while (*v == ' ' || *v == '\t')
                v++;
            return v;
        }
        if (!strncmp(line, "\r\n", 2))
            break;
    }
    return NULL;
}

static char *header_dup(const char *headers, const char *name)
{
    const char *v = header_value(headers, name);

    return v ? strndup(v, strcspn(v, "\r\n")) : NULL;
}

// Decodes a chunked body in place; returns the new length.
static size_t unchunk(char *data, size_t len)
{
    size_t in = 0, out = 0;

    while (in < len) {
        char *end;
        unsigned long size = strtoul(data + in, &end, 16);
        char *crlf = memchr(data + in, '\n', len - in);

        if (!crlf || end == data + in)
            break;
        in = crlf - data + 1;
        if (!size || in + size > len) {
            if (size && in < len) {
                memmove(data + out, data + in, len - in);
                out += len - in;
            }
            break;
        }
        memmove(data + out, data + in, size);
        out += size;
        in += size;
        if (in < len && data[in] == '\r')
            in++;
        if (in < len && data[in] == '\n')
            in++;
    }
    return out;
}

static struct response *fetch_http(const char *url, const char *post, char **redirect, int *status_out)
{
    struct url u;
    struct conn c = { NULL, -1 };
    const char *error;
    char *req, cookie[2048], *buf = NULL, *body, *headers;
    size_t len = 0, cap = 0;
    struct response *r;
    int status = 0;

    *redirect = NULL;
    if (!parse_url(url, &u) || !*u.host)
        return failed(url, "%s is not a web address this browser understands.", url);
    if (!strcmp(u.scheme, "https")) {
        if (!(c.tls = tls_connect(u.host, u.port, NULL, &error))) {
            char msg[400];

            snprintf(msg, sizeof(msg), "%s (%s)", u.host, error);
            return failed(url, "Could not make a secure connection to %s.", msg);
        }
    } else if ((c.fd = tcp_connect(u.host, u.port, 15000)) < 0) {
        return failed(url, "Could not connect to %s.", u.host);
    }
    cookie_header(u.host, cookie, sizeof(cookie));
    if (!(req = malloc(4096 + strlen(u.path) + (post ? strlen(post) : 0)))) {
        conn_close(&c);
        return NULL;
    }
    if (post)
        sprintf(req, "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
                "Accept: text/html,image/*,*/*;q=0.8\r\nAccept-Encoding: identity\r\nAccept-Language: en\r\n"
                "Connection: close\r\n%sContent-Type: application/x-www-form-urlencoded\r\n"
                "Content-Length: %lu\r\n\r\n%s", u.path, u.host, cookie, (unsigned long)strlen(post), post);
    else
        sprintf(req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
                "Accept: text/html,image/*,*/*;q=0.8\r\nAccept-Encoding: identity\r\nAccept-Language: en\r\n"
                "Connection: close\r\n%s\r\n", u.path, u.host, cookie);
    if (!conn_write(&c, req, strlen(req))) {
        free(req);
        conn_close(&c);
        return failed(url, "Sending the request to %s failed.", u.host);
    }
    free(req);
    for (;;) {
        ssize_t n;

        if (len + 16384 + 1 > cap) {
            char *m;

            if (cap >= MAX_BODY)
                break;
            cap = cap ? cap * 2 : 65536;
            if (!(m = realloc(buf, cap))) {
                free(buf);
                conn_close(&c);
                return NULL;
            }
            buf = m;
        }
        n = conn_read(&c, buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += n;
    }
    conn_close(&c);
    if (!buf || len < 12 || strncmp(buf, "HTTP/", 5)) {
        free(buf);
        return failed(url, "%s sent no answer.", u.host);
    }
    buf[len] = 0;
    status = atoi(buf + 9);
    *status_out = status;
    headers = strstr(buf, "\r\n");
    body = strstr(buf, "\r\n\r\n");
    if (!headers || !body) {
        free(buf);
        return failed(url, "%s sent a broken answer.", u.host);
    }
    headers += 2;
    body += 4;
    // Cookies.
    for (const char *line = headers; line < body; line = strstr(line, "\r\n") + 2) {
        if (!strncasecmp(line, "Set-Cookie:", 11)) {
            char *v = strndup(line + 11, strcspn(line + 11, "\r\n"));

            if (v) {
                cookie_set(u.host, v + strspn(v, " "));
                free(v);
            }
        }
        if (!strstr(line, "\r\n"))
            break;
    }
    if (status >= 300 && status < 400 && header_value(headers, "Location")) {
        char *loc = header_dup(headers, "Location");

        *redirect = loc ? url_resolve(url, loc) : NULL;
        free(loc);
        free(buf);
        return NULL;
    }
    if (!(r = calloc(1, sizeof(*r)))) {
        free(buf);
        return NULL;
    }
    r->status = status;
    r->url = strdup(url);
    {
        char *type = header_dup(headers, "Content-Type");
        const char *te = header_value(headers, "Transfer-Encoding");
        size_t blen = len - (body - buf);

        if (type) {
            char *semi = strchr(type, ';');

            if (semi)
                *semi = 0;
            for (char *t = type; *t; t++)
                *t = tolower((unsigned char)*t);
        }
        r->type = type ? type : strdup(type_for_name(u.path));
        memmove(buf, body, blen);
        if (te && !strncasecmp(te, "chunked", 7))
            blen = unchunk(buf, blen);
        buf[blen] = 0;
        r->data = buf;
        r->len = blen;
    }
    return r;
}

struct response *net_fetch(const char *url, const char *post)
{
    char *current = strdup(url);
    struct response *r = NULL;

    if (!current)
        return NULL;
    if (!strncasecmp(url, "file:", 5)) {
        r = fetch_file(url);
        free(current);
        return r;
    }
    if (!strncasecmp(url, "data:", 5)) {
        r = fetch_data(url);
        free(current);
        return r;
    }
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) {
        r = failed(url, "This browser cannot open %s addresses.", url);
        free(current);
        return r;
    }
    for (int i = 0; i <= MAX_REDIRECTS; i++) {
        char *next = NULL;
        int status = 0;

        r = fetch_http(current, post, &next, &status);
        if (r || !next)
            break;
        // After a redirect, a form post becomes a plain request (except 307/308).
        if (status != 307 && status != 308)
            post = NULL;
        free(current);
        current = next;
    }
    if (!r)
        r = failed(current, "%s redirected too many times.", current);
    free(current);
    return r;
}
