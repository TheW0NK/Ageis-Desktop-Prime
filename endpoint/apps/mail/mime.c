#include "mail.h"
#include "gfx.h"

// MIME (RFC 2045-2047): headers, encoded words, multipart bodies, base64 and
// quoted-printable, and composing new messages.

struct sbuf {
    char *s;
    size_t len, cap;
};

static void put(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->len + n + 256);
        char *m = realloc(b->s, cap);

        if (!m)
            return;
        b->s = m;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void puts_(struct sbuf *b, const char *s)
{
    put(b, s, strlen(s));
}

static char *finish(struct sbuf *b)
{
    if (!b->s)
        return strdup("");
    return b->s;
}

// ---- Character sets ----

static bool is_latin1(const char *charset)
{
    return charset && (!strncasecmp(charset, "iso-8859-1", 10) || !strncasecmp(charset, "latin1", 6)
                       || !strncasecmp(charset, "windows-1252", 12) || !strncasecmp(charset, "cp1252", 6)
                       || !strncasecmp(charset, "iso-8859-15", 11));
}

// Text in the given character set, as UTF-8.
static char *to_utf8(const char *s, size_t len, const char *charset)
{
    struct sbuf b = { 0 };

    if (!is_latin1(charset)) {
        put(&b, s, len);
        return finish(&b);
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];
        char u[4];

        if (c < 0x80)
            put(&b, (char *)&c, 1);
        else
            put(&b, u, utf8_encode(c == 0x80 ? 0x20AC : c, u));
    }
    return finish(&b);
}

// ---- Transfer encodings ----

static size_t qp_decode(const char *in, size_t len, char *out, bool header)
{
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=' && i + 1 < len && (in[i + 1] == '\n' || (in[i + 1] == '\r' && i + 2 < len))) {
            // A soft line break.
            i += in[i + 1] == '\r' ? 2 : 1;
            continue;
        }
        if (in[i] == '=' && i + 2 < len && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };

            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (header && in[i] == '_') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    return o;
}

// ---- Headers ----

static size_t header_end(const char *raw, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (raw[i] == '\n' && raw[i + 1] == '\n')
            return i + 2;
        if (raw[i] == '\n' && raw[i + 1] == '\r' && i + 2 < len && raw[i + 2] == '\n')
            return i + 3;
    }
    return len;
}

// "=?utf-8?B?...?=" words decoded; whitespace between two encoded words drops.
static char *decode_words(const char *s)
{
    struct sbuf b = { 0 };
    const char *p = s;
    bool last_encoded = false;

    while (*p) {
        const char *start = strstr(p, "=?");
        const char *q1, *q2, *end;

        if (!start) {
            puts_(&b, p);
            break;
        }
        q1 = strchr(start + 2, '?');
        q2 = q1 ? strchr(q1 + 1, '?') : NULL;
        end = q2 ? strstr(q2 + 1, "?=") : NULL;
        if (!q1 || !q2 || !end || q2 - q1 != 2) {
            put(&b, p, start + 2 - p);
            p = start + 2;
            last_encoded = false;
            continue;
        }
        // Text before it, unless it is only space between encoded words.
        {
            bool blank = true;

            for (const char *t = p; t < start; t++)
                if (!isspace((unsigned char)*t))
                    blank = false;
            if (!(blank && last_encoded))
                put(&b, p, start - p);
        }
        {
            char charset[40];
            size_t clen = MIN((size_t)(q1 - start - 2), sizeof(charset) - 1);
            char enc = toupper((unsigned char)q1[1]);
            const char *data = q2 + 1;
            size_t dlen = end - data;
            char *raw = malloc(dlen + 1), *utf;
            size_t n;

            memcpy(charset, start + 2, clen);
            charset[clen] = 0;
            {
                char *star = strchr(charset, '*');      // language: utf-8*en

                if (star)
                    *star = 0;
            }
            if (!raw)
                break;
            n = enc == 'B' ? (size_t)base64_decode(data, dlen, raw) : qp_decode(data, dlen, raw, true);
            utf = to_utf8(raw, n, charset);
            puts_(&b, utf);
            free(utf);
            free(raw);
        }
        p = end + 2;
        last_encoded = true;
    }
    return finish(&b);
}

char *mime_header(const char *raw, size_t len, const char *name)
{
    size_t hend = header_end(raw, len), nl = strlen(name);
    const char *p = raw, *end = raw + hend;

    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);

        if (!eol)
            eol = end;
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            struct sbuf b = { 0 };
            const char *v = p + nl + 1;
            char *decoded;

            // This line and its continuations.
            for (;;) {
                const char *e = eol;

                while (v < e && (*v == ' ' || *v == '\t') && !b.len)
                    v++;
                if (e > v && e[-1] == '\r')
                    e--;
                put(&b, v, e - v);
                if (eol + 1 < end && (eol[1] == ' ' || eol[1] == '\t')) {
                    put(&b, " ", 1);
                    v = eol + 2;
                    while (v < end && (*v == ' ' || *v == '\t'))
                        v++;
                    eol = memchr(v, '\n', end - v);
                    if (!eol)
                        eol = end;
                } else {
                    break;
                }
            }
            decoded = decode_words(finish(&b));
            free(b.s);
            return decoded;
        }
        p = eol + 1;
    }
    return NULL;
}

// A parameter of a header value: boundary, charset, name, filename.
static char *param(const char *value, const char *name)
{
    size_t nl = strlen(name);

    if (!value)
        return NULL;
    for (const char *p = strchr(value, ';'); p; p = strchr(p + 1, ';')) {
        const char *q = p + 1;

        while (*q == ' ' || *q == '\t')
            q++;
        if (!strncasecmp(q, name, nl) && (q[nl] == '=' || (q[nl] == '*' && strchr(q, '=')))) {
            const char *v = strchr(q, '=') + 1;
            bool extended = q[nl] == '*';

            while (*v == ' ')
                v++;
            if (*v == '"') {
                const char *e = strchr(v + 1, '"');

                return e ? strndup(v + 1, e - v - 1) : strdup(v + 1);
            }
            {
                size_t n = strcspn(v, "; \t");
                char *out = strndup(v, n);

                if (extended && out) {
                    // filename*=utf-8''name%20here
                    char *q2 = strstr(out, "''"), *o, *s;

                    if (q2) {
                        memmove(out, q2 + 2, strlen(q2 + 2) + 1);
                        for (s = o = out; *s; s++) {
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
                }
                return out;
            }
        }
    }
    return NULL;
}

void mime_address(const char *from, char *out, size_t size)
{
    const char *lt = from ? strchr(from, '<') : NULL;

    if (!from) {
        out[0] = 0;
        return;
    }
    if (lt) {
        const char *gt = strchr(lt, '>');

        snprintf(out, size, "%.*s", gt ? (int)(gt - lt - 1) : (int)strlen(lt + 1), lt + 1);
    } else {
        while (isspace((unsigned char)*from))
            from++;
        strlcpy(out, from, size);
    }
}

void mime_display_name(const char *from, char *out, size_t size)
{
    const char *lt = from ? strchr(from, '<') : NULL;

    if (!from) {
        out[0] = 0;
        return;
    }
    if (lt && lt > from) {
        const char *s = from, *e = lt;

        while (s < e && (isspace((unsigned char)*s) || *s == '"'))
            s++;
        while (e > s && (isspace((unsigned char)e[-1]) || e[-1] == '"'))
            e--;
        if (e > s) {
            snprintf(out, size, "%.*s", (int)(e - s), s);
            return;
        }
    }
    mime_address(from, out, size);
}

// ---- HTML as text ----

static char *html_to_text(const char *s, size_t len)
{
    struct sbuf b = { 0 };
    bool space = false;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '<') {
            const char *t = s + i + 1;
            const char *close = memchr(s + i, '>', len - i);
            bool end = *t == '/';

            if (end)
                t++;
            if (!strncasecmp(t, "style", 5) || !strncasecmp(t, "script", 6) || !strncasecmp(t, "head", 4)) {
                // Skip to the end tag.
                const char *name = !strncasecmp(t, "style", 5) ? "</style" : !strncasecmp(t, "script", 6)
                                   ? "</script" : "</head";
                const char *e = NULL;

                if (!end)
                    for (size_t k = i; k + strlen(name) < len; k++)
                        if (!strncasecmp(s + k, name, strlen(name))) {
                            e = s + k;
                            break;
                        }
                if (e) {
                    close = memchr(e, '>', s + len - e);
                }
            } else if (!strncasecmp(t, "br", 2) || !strncasecmp(t, "p", 1) || !strncasecmp(t, "div", 3)
                       || !strncasecmp(t, "tr", 2) || !strncasecmp(t, "li", 2) || !strncasecmp(t, "h", 1)
                       || !strncasecmp(t, "table", 5)) {
                if (b.len && b.s[b.len - 1] != '\n')
                    put(&b, "\n", 1);
                if (!strncasecmp(t, "li", 2) && !end)
                    puts_(&b, "  \xE2\x80\xA2 ");
                if ((!strncasecmp(t, "p", 1) && (t[1] == '>' || isspace((unsigned char)t[1]))) && end)
                    put(&b, "\n", 1);
                space = false;
            }
            if (!close)
                break;
            i = close - s;
            continue;
        }
        if (s[i] == '&') {
            static const struct {
                const char *name;
                uint32_t cp;
            } ents[] = { { "nbsp", ' ' }, { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
                         { "apos", '\'' }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "ldquo", 0x201C },
                         { "rdquo", 0x201D }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
                         { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "euro", 0x20AC },
                         { "pound", 0xA3 }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "eacute", 0xE9 },
                         { "egrave", 0xE8 }, { "ecirc", 0xEA }, { "aacute", 0xE1 }, { "agrave", 0xE0 },
                         { "acirc", 0xE2 }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC },
                         { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "szlig", 0xDF },
                         { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "oacute", 0xF3 }, { "iacute", 0xED },
                         { "uacute", 0xFA }, { "Eacute", 0xC9 }, { "laquo", 0xAB }, { "raquo", 0xBB } };
            const char *semi = memchr(s + i, ';', MIN(len - i, (size_t)12));
            uint32_t cp = 0;

            if (semi && s[i + 1] == '#') {
                cp = s[i + 2] == 'x' || s[i + 2] == 'X' ? strtoul(s + i + 3, NULL, 16) : strtoul(s + i + 2, NULL, 10);
            } else if (semi) {
                for (size_t k = 0; k < sizeof(ents) / sizeof(ents[0]); k++)
                    if ((size_t)(semi - s - i - 1) == strlen(ents[k].name)
                        && !strncmp(s + i + 1, ents[k].name, strlen(ents[k].name)))
                        cp = ents[k].cp;
            }
            if (cp && cp < 0x110000) {
                char u[4];

                put(&b, u, utf8_encode(cp, u));
                i = semi - s;
                space = false;
                continue;
            }
        }
        if (isspace((unsigned char)s[i])) {
            if (!space && b.len && b.s[b.len - 1] != '\n')
                put(&b, " ", 1);
            space = true;
            continue;
        }
        put(&b, s + i, 1);
        space = false;
    }
    return finish(&b);
}

// ---- Bodies ----

static void add_attachment(struct message *m, char *name, const char *type, const char *data, size_t len)
{
    struct attachment *a = realloc(m->atts, (m->natts + 1) * sizeof(*a));

    if (!a) {
        free(name);
        return;
    }
    m->atts = a;
    a = &m->atts[m->natts++];
    a->name = name ? name : strdup("attachment");
    a->type = strdup(type);
    a->data = malloc(len + 1);
    if (a->data) {
        memcpy(a->data, data, len);
        a->data[len] = 0;
    }
    a->len = a->data ? len : 0;
}

static void parse_part(const char *raw, size_t len, struct message *m, int depth, char **html_text);

static void parse_multipart(const char *body, size_t len, const char *boundary, bool alternative,
                            struct message *m, int depth, char **html_text)
{
    size_t bl = strlen(boundary);
    const char *p = body, *end = body + len, *part = NULL;

    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);
        size_t ll;

        if (!eol)
            eol = end;
        ll = eol - p;
        if (ll >= bl + 2 && p[0] == '-' && p[1] == '-' && !strncmp(p + 2, boundary, bl)) {
            if (part) {
                size_t plen = p - part;

                // The line break before the boundary belongs to it.
                if (plen && part[plen - 1] == '\n')
                    plen--;
                if (plen && part[plen - 1] == '\r')
                    plen--;
                parse_part(part, plen, m, depth + 1, html_text);
            }
            if (ll >= bl + 4 && p[bl + 2] == '-' && p[bl + 3] == '-')
                return;     // the closing boundary
            part = eol + 1;
        }
        p = eol + 1;
    }
    (void)alternative;
}

static void parse_part(const char *raw, size_t len, struct message *m, int depth, char **html_text)
{
    size_t hend = header_end(raw, len);
    char *ctype = mime_header(raw, hend, "Content-Type");
    char *cte = mime_header(raw, hend, "Content-Transfer-Encoding");
    char *disp = mime_header(raw, hend, "Content-Disposition");
    const char *body = raw + hend;
    size_t blen = len - hend;
    char type[80] = "text/plain", *charset, *name;
    bool attachment;

    if (depth > 8)
        goto out;
    if (ctype) {
        size_t n = strcspn(ctype, "; \t");

        snprintf(type, sizeof(type), "%.*s", (int)MIN(n, sizeof(type) - 1), ctype);
        for (char *t = type; *t; t++)
            *t = tolower((unsigned char)*t);
    }
    if (!strncmp(type, "multipart/", 10)) {
        char *boundary = param(ctype, "boundary");

        if (boundary)
            parse_multipart(body, blen, boundary, !strcmp(type, "multipart/alternative"), m, depth, html_text);
        free(boundary);
        goto out;
    }
    if (!strcmp(type, "message/rfc822")) {
        parse_part(body, blen, m, depth + 1, html_text);
        goto out;
    }
    name = param(disp, "filename");
    if (!name)
        name = param(ctype, "name");
    attachment = (disp && !strncasecmp(disp, "attachment", 10)) || (name && strncmp(type, "text/", 5));
    {
        // Undo the transfer encoding.
        char *data = malloc(blen + 1);
        size_t n = blen;

        if (!data) {
            free(name);
            goto out;
        }
        if (cte && !strncasecmp(cte, "base64", 6))
            n = base64_decode(body, blen, data);
        else if (cte && !strncasecmp(cte, "quoted-printable", 16))
            n = qp_decode(body, blen, data, false);
        else
            memcpy(data, body, blen);
        data[n] = 0;
        charset = param(ctype, "charset");
        if (!attachment && !strcmp(type, "text/plain") && !m->text) {
            m->text = to_utf8(data, n, charset);
            free(name);
        } else if (!attachment && !strcmp(type, "text/html") && !*html_text) {
            char *utf = to_utf8(data, n, charset);

            *html_text = html_to_text(utf, strlen(utf));
            free(utf);
            free(name);
        } else if (attachment || name || strncmp(type, "text/", 5)) {
            add_attachment(m, name ? decode_words(name) : NULL, type, data, n);
            free(name);
        } else {
            free(name);
        }
        free(charset);
        free(data);
    }
out:
    free(ctype);
    free(cte);
    free(disp);
}

int mime_parse(const char *raw, size_t len, struct message *m)
{
    char *html_text = NULL;

    memset(m, 0, sizeof(*m));
    m->from = mime_header(raw, len, "From");
    m->to = mime_header(raw, len, "To");
    m->cc = mime_header(raw, len, "Cc");
    m->subject = mime_header(raw, len, "Subject");
    m->date = mime_header(raw, len, "Date");
    m->message_id = mime_header(raw, len, "Message-ID");
    m->references = mime_header(raw, len, "References");
    parse_part(raw, len, m, 0, &html_text);
    if (!m->text) {
        m->text = html_text ? html_text : strdup("");
        html_text = NULL;
    }
    free(html_text);
    // Line ends as plain newlines.
    {
        char *o = m->text;

        for (char *s = m->text; *s; s++)
            if (*s != '\r')
                *o++ = *s;
        *o = 0;
    }
    return 0;
}

void mime_free(struct message *m)
{
    free(m->from);
    free(m->to);
    free(m->cc);
    free(m->subject);
    free(m->date);
    free(m->message_id);
    free(m->references);
    free(m->text);
    for (int i = 0; i < m->natts; i++) {
        free(m->atts[i].name);
        free(m->atts[i].type);
        free(m->atts[i].data);
    }
    free(m->atts);
    memset(m, 0, sizeof(*m));
}

// ---- Dates ----

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    {
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        int64_t yoe = y - era * 400;
        int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

        return era * 146097 + doe - 719468;
    }
}

int64_t mime_date(const char *date)
{
    static const char *const months[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct",
                                          "nov", "dec" };
    int day = 0, month = -1, year = 0, h = 0, mi = 0, s = 0, zone = 0;
    const char *p = date;

    if (!date)
        return 0;
    if (strchr(p, ','))
        p = strchr(p, ',') + 1;
    while (*p == ' ')
        p++;
    day = atoi(p);
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(p, months[i], 3))
            month = i;
    while (*p && *p != ' ')
        p++;
    year = atoi(p);
    if (year < 100)
        year += year < 50 ? 2000 : 1900;
    while (*p == ' ')
        p++;
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    h = atoi(p);
    if ((p = strchr(p, ':'))) {
        mi = atoi(p + 1);
        if ((p = strchr(p + 1, ':')) && isdigit((unsigned char)p[1]))
            s = atoi(p + 1);
        p = strpbrk(p ? p : date, "+-");
        if (p && strlen(p) >= 5) {
            int z = atoi(p + 1);

            zone = (z / 100 * 60 + z % 100) * 60 * (*p == '-' ? -1 : 1);
        }
    }
    if (month < 0 || !day)
        return 0;
    return days_from_civil(year, month + 1, day) * 86400 + h * 3600 + mi * 60 + s - zone;
}

// ---- Composing ----

static void random_hex(char *out, int n)
{
    uint8_t b[16];
    int fd = open("/osystem/devices/urandom", O_RDONLY);

    if (fd < 0 || read(fd, b, sizeof(b)) != sizeof(b)) {
        int64_t t = time(NULL);

        memcpy(b, &t, sizeof(t));
        memcpy(b + 8, &t, sizeof(t));
    }
    if (fd >= 0)
        close(fd);
    for (int i = 0; i < n / 2 && i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", b[i]);
}

static bool ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80)
            return false;
    return true;
}

// A header value, encoded if it is not plain ASCII.
static void put_header(struct sbuf *b, const char *name, const char *value)
{
    puts_(b, name);
    puts_(b, ": ");
    if (ascii(value)) {
        puts_(b, value);
    } else {
        size_t n = strlen(value);
        char *enc = malloc(n * 4 / 3 + 8);

        if (enc) {
            base64_encode(value, n, enc);
            puts_(b, "=?UTF-8?B?");
            puts_(b, enc);
            puts_(b, "?=");
            free(enc);
        }
    }
    puts_(b, "\r\n");
}

static void qp_encode(struct sbuf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    int col = 0;

    for (; *s; s++) {
        unsigned char c = *s;

        if (c == '\r')
            continue;
        if (c == '\n') {
            puts_(b, "\r\n");
            col = 0;
            continue;
        }
        if (col >= 73) {
            puts_(b, "=\r\n");
            col = 0;
        }
        if (c == '=' || c >= 0x80 || (c < 0x20 && c != '\t') || (c == ' ' && (s[1] == '\n' || !s[1]))) {
            char e[3] = { '=', hex[c >> 4], hex[c & 15] };

            put(b, e, 3);
            col += 3;
        } else if (c == '.' && col == 0) {
            puts_(b, "=2E");        // keeps lines from starting with a dot
            col += 3;
        } else {
            put(b, (char *)&c, 1);
            col++;
        }
    }
}

static const char *type_of(const char *name)
{
    static const struct {
        const char *ext, *type;
    } types[] = { { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
                  { ".gif", "image/gif" }, { ".txt", "text/plain" }, { ".html", "text/html" },
                  { ".pdf", "application/pdf" }, { ".zip", "application/zip" }, { ".mp3", "audio/mpeg" },
                  { ".wav", "audio/wav" } };
    const char *dot = strrchr(name, '.');

    for (size_t i = 0; dot && i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(dot, types[i].ext))
            return types[i].type;
    return "application/octet-stream";
}

static char *read_whole(const char *path, size_t *len)
{
    struct aegis_stat st;
    int fd = open(path, O_RDONLY);
    char *data;
    size_t got = 0;

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) < 0 || st.size > (32u << 20) || !(data = malloc(st.size + 1))) {
        close(fd);
        return NULL;
    }
    while (got < st.size) {
        ssize_t n = read(fd, data + got, st.size - got);

        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    *len = got;
    return data;
}

char *mime_compose(const struct account *a, const char *to, const char *cc, const char *subject,
                   const char *body, const char *in_reply_to, char **attachments, int natts, size_t *len_out)
{
    struct sbuf b = { 0 };
    char line[512], id[40], boundary[40];
    struct tm tm;
    int64_t now = time(NULL);

    localtime_r(&now, &tm);
    strftime(line, sizeof(line), "%a, %d %b %Y %H:%M:%S %z", &tm);
    put_header(&b, "Date", line);
    if (a->name[0] && ascii(a->name))
        snprintf(line, sizeof(line), "\"%s\" <%s>", a->name, a->email);
    else if (a->name[0]) {
        char enc[200];

        base64_encode(a->name, strlen(a->name), enc);
        snprintf(line, sizeof(line), "=?UTF-8?B?%s?= <%s>", enc, a->email);
    } else
        snprintf(line, sizeof(line), "<%s>", a->email);
    puts_(&b, "From: ");
    puts_(&b, line);
    puts_(&b, "\r\n");
    put_header(&b, "To", to);
    if (cc && *cc)
        put_header(&b, "Cc", cc);
    put_header(&b, "Subject", subject);
    random_hex(id, 24);
    snprintf(line, sizeof(line), "<%s@%s>", id, strchr(a->email, '@') ? strchr(a->email, '@') + 1 : "aegis");
    put_header(&b, "Message-ID", line);
    if (in_reply_to && *in_reply_to) {
        put_header(&b, "In-Reply-To", in_reply_to);
        put_header(&b, "References", in_reply_to);
    }
    puts_(&b, "MIME-Version: 1.0\r\nUser-Agent: Aegis Email\r\n");
    if (natts) {
        random_hex(boundary, 24);
        snprintf(line, sizeof(line), "Content-Type: multipart/mixed; boundary=\"aegis-%s\"\r\n\r\n", boundary);
        puts_(&b, line);
        puts_(&b, "This is a message in several parts.\r\n\r\n");
        snprintf(line, sizeof(line), "--aegis-%s\r\n", boundary);
        puts_(&b, line);
    }
    puts_(&b, "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n");
    qp_encode(&b, body);
    puts_(&b, "\r\n");
    for (int i = 0; i < natts; i++) {
        size_t len;
        char *data = read_whole(attachments[i], &len), *enc;
        const char *base = strrchr(attachments[i], '/') ? strrchr(attachments[i], '/') + 1 : attachments[i];

        if (!data)
            continue;
        snprintf(line, sizeof(line),
                 "--aegis-%s\r\nContent-Type: %s; name=\"%s\"\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
                 "Content-Transfer-Encoding: base64\r\n\r\n", boundary, type_of(base), base, base);
        puts_(&b, line);
        if ((enc = malloc(len * 4 / 3 + 8))) {
            size_t n = base64_encode(data, len, enc);

            for (size_t k = 0; k < n; k += 76) {
                put(&b, enc + k, MIN(76, n - k));
                puts_(&b, "\r\n");
            }
            free(enc);
        }
        free(data);
    }
    if (natts) {
        snprintf(line, sizeof(line), "--aegis-%s--\r\n", boundary);
        puts_(&b, line);
    }
    *len_out = b.len;
    return finish(&b);
}
