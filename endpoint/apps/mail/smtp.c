#include "mail.h"

// SMTP submission (RFC 5321, 3207 STARTTLS, 4954 AUTH PLAIN).

// Reads a reply ("250-..." lines up to "250 ..."); returns its code. The
// lines are collected in text (for EHLO capabilities and errors).
static int reply(struct mconn *c, char *text, size_t size)
{
    char line[1024];

    if (text)
        text[0] = 0;
    for (;;) {
        if (mconn_line(c, line, sizeof(line)) < 4)
            return -1;
        if (text) {
            strlcat(text, line + 4, size);
            strlcat(text, "\n", size);
        }
        if (line[3] != '-')
            return atoi(line);
    }
}

static int step(struct mconn *c, int want, char *text, size_t size, const char *fmt, ...)
{
    char cmd[1200];
    va_list ap;
    int code;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd) - 2, fmt, ap);
    va_end(ap);
    strlcat(cmd, "\r\n", sizeof(cmd));
    if (mconn_send(c, cmd, strlen(cmd)) < 0)
        return -1;
    code = reply(c, text, size);
    return code / 100 == want / 100 ? 0 : (code < 0 ? -1 : -code);
}

static bool has_cap(const char *caps, const char *cap)
{
    size_t n = strlen(cap);

    for (const char *p = caps; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncasecmp(p, cap, n) && (p[n] == '\n' || p[n] == ' ' || p[n] == '='))
            return true;
    return false;
}

// Adds the addresses in a To/Cc header to the recipients.
static int recipients(struct mconn *c, const char *header, char *error, size_t size)
{
    char *list = header ? strdup(header) : NULL, *p, *next;
    int count = 0;

    for (p = list; p && *p; p = next) {
        char addr[256], text[512];
        int quoted = 0;
        char *q = p;

        // Split on commas outside quotes.
        for (; *q; q++) {
            if (*q == '"')
                quoted = !quoted;
            else if (*q == ',' && !quoted)
                break;
        }
        next = *q ? q + 1 : q;
        *q = 0;
        mime_address(p, addr, sizeof(addr));
        while (*addr && isspace((unsigned char)addr[strlen(addr) - 1]))
            addr[strlen(addr) - 1] = 0;
        if (!*addr)
            continue;
        if (step(c, 250, text, sizeof(text), "RCPT TO:<%s>", addr) < 0) {
            snprintf(error, size, "The server refused %s: %s", addr, text);
            free(list);
            return -1;
        }
        count++;
    }
    free(list);
    return count;
}

int smtp_send(const struct account *a, const char *password, const char *msg, size_t len, char *error,
              size_t size)
{
    struct mconn c;
    char caps[2048], text[512];
    char *to = mime_header(msg, len, "To"), *cc = mime_header(msg, len, "Cc");
    int n = 0, m;

    error[0] = 0;
    if (mconn_open(&c, a->smtp_host, a->smtp_port, a->smtp_security == SEC_TLS, a->ca_file) < 0) {
        strlcpy(error, c.error, size);
        goto fail_free;
    }
    if (reply(&c, NULL, 0) != 220) {
        snprintf(error, size, "The mail server %s is not ready.", a->smtp_host);
        goto fail;
    }
    if (step(&c, 250, caps, sizeof(caps), "EHLO aegis") < 0) {
        snprintf(error, size, "The mail server did not accept EHLO.");
        goto fail;
    }
    if (a->smtp_security == SEC_STARTTLS) {
        if (!has_cap(caps, "STARTTLS") || step(&c, 220, NULL, 0, "STARTTLS") < 0
            || mconn_starttls(&c, a->smtp_host, a->ca_file) < 0) {
            snprintf(error, size, "The mail server cannot make the connection secure. %s", c.error);
            goto fail;
        }
        if (step(&c, 250, caps, sizeof(caps), "EHLO aegis") < 0)
            goto fail;
    }
    if (has_cap(caps, "AUTH") && password && *password) {
        const char *user = a->user[0] ? a->user : a->email;
        size_t ul = strlen(user), pl = strlen(password);
        char *plain = malloc(ul + pl + 2), *enc = malloc((ul + pl + 2) * 4 / 3 + 8);

        if (!plain || !enc) {
            free(plain);
            free(enc);
            goto fail;
        }
        plain[0] = 0;
        memcpy(plain + 1, user, ul);
        plain[ul + 1] = 0;
        memcpy(plain + ul + 2, password, pl);
        base64_encode(plain, ul + pl + 2, enc);
        memset(plain, 0, ul + pl + 2);
        free(plain);
        m = step(&c, 235, text, sizeof(text), "AUTH PLAIN %s", enc);
        memset(enc, 0, strlen(enc));
        free(enc);
        if (m < 0) {
            snprintf(error, size, "Signing in to %s failed. Check the name and password.", a->smtp_host);
            goto fail;
        }
    }
    if (step(&c, 250, text, sizeof(text), "MAIL FROM:<%s>", a->email) < 0) {
        snprintf(error, size, "The server refused the sender: %s", text);
        goto fail;
    }
    if ((m = recipients(&c, to, error, size)) < 0)
        goto fail;
    n += m;
    if ((m = recipients(&c, cc, error, size)) < 0)
        goto fail;
    n += m;
    if (!n) {
        snprintf(error, size, "There is nobody to send it to.");
        goto fail;
    }
    if (step(&c, 354, text, sizeof(text), "DATA") < 0) {
        snprintf(error, size, "The server refused the message: %s", text);
        goto fail;
    }
    // The message, with lines that start with a dot doubled.
    {
        const char *p = msg, *end = msg + len;

        while (p < end) {
            const char *nl = memchr(p, '\n', end - p);
            size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);

            if (ll && p[ll - 1] == '\r')
                ll--;
            if (*p == '.' && mconn_send(&c, ".", 1) < 0)
                goto fail;
            if (mconn_send(&c, p, ll) < 0 || mconn_send(&c, "\r\n", 2) < 0)
                goto fail;
            p = nl ? nl + 1 : end;
        }
    }
    if (mconn_send(&c, ".\r\n", 3) < 0 || reply(&c, text, sizeof(text)) != 250) {
        snprintf(error, size, "The server did not take the message: %s", text);
        goto fail;
    }
    step(&c, 221, NULL, 0, "QUIT");
    mconn_close(&c);
    free(to);
    free(cc);
    return 0;
fail:
    if (!error[0])
        strlcpy(error, c.error[0] ? c.error : "Sending failed.", size);
    mconn_close(&c);
fail_free:
    free(to);
    free(cc);
    return -1;
}
