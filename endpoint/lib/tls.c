#include "tls.h"
#include "bearssl.h"

struct tls {
    int fd;
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    char error[96];
};

// ---- Trust anchors ----

struct anchors {
    br_x509_trust_anchor *list;
    size_t count, cap;
};

static struct anchors cached;
static char cached_file[256];
static mutex_t anchors_lock;

struct buffer {
    unsigned char *data;
    size_t len, cap;
};

static void append(void *ctx, const void *data, size_t len)
{
    struct buffer *b = ctx;

    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        unsigned char *n;

        while (cap < b->len + len)
            cap *= 2;
        if (!(n = realloc(b->data, cap)))
            return;
        b->data = n;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void *dup_bytes(const void *src, size_t len)
{
    void *p = malloc(len ? len : 1);

    if (p)
        memcpy(p, src, len);
    return p;
}

// Turns one DER certificate into a trust anchor.
static bool add_anchor(struct anchors *a, const unsigned char *der, size_t len)
{
    br_x509_decoder_context dc;
    struct buffer dn = { 0 };
    br_x509_pkey *pk;
    br_x509_trust_anchor *ta;

    br_x509_decoder_init(&dc, append, &dn);
    br_x509_decoder_push(&dc, der, len);
    if (!(pk = br_x509_decoder_get_pkey(&dc)) || br_x509_decoder_last_error(&dc)) {
        free(dn.data);
        return false;
    }
    if (a->count == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 128;
        br_x509_trust_anchor *n = realloc(a->list, cap * sizeof(*n));

        if (!n) {
            free(dn.data);
            return false;
        }
        a->list = n;
        a->cap = cap;
    }
    ta = &a->list[a->count];
    ta->dn.data = dn.data;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = dup_bytes(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = dup_bytes(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = dup_bytes(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        free(dn.data);
        return false;
    }
    a->count++;
    return true;
}

static bool load_anchors(const char *file, struct anchors *a)
{
    br_pem_decoder_context pc;
    struct buffer der = { 0 };
    bool in_cert = false;
    unsigned char buf[4096];
    int fd = open(file, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return false;
    br_pem_decoder_init(&pc);
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        unsigned char *p = buf;
        size_t left = n;

        while (left) {
            size_t used = br_pem_decoder_push(&pc, p, left);

            p += used;
            left -= used;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                in_cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE");
                der.len = 0;
                br_pem_decoder_setdest(&pc, in_cert ? append : NULL, &der);
                break;
            case BR_PEM_END_OBJ:
                if (in_cert)
                    add_anchor(a, der.data, der.len);
                in_cert = false;
                break;
            case BR_PEM_ERROR:
                br_pem_decoder_init(&pc);
                break;
            }
        }
    }
    close(fd);
    free(der.data);
    return a->count > 0;
}

static bool anchors_for(const char *file, struct anchors *out)
{
    bool ok = true;

    mutex_lock(&anchors_lock);
    if (strcmp(cached_file, file)) {
        // A different bundle replaces the cached one (the old one leaks; it
        // is small and this only happens when a program changes bundles).
        struct anchors a = { 0 };

        if ((ok = load_anchors(file, &a))) {
            cached = a;
            strlcpy(cached_file, file, sizeof(cached_file));
        }
    }
    *out = cached;
    mutex_unlock(&anchors_lock);
    return ok;
}

// ---- Connections ----

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
    for (;;) {
        ssize_t n = recv(*(int *)ctx, buf, len, 0);

        if (n > 0)
            return n;
        if (n == 0 || errno != EINTR)
            return -1;
    }
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
    for (;;) {
        ssize_t n = send(*(int *)ctx, buf, len, MSG_NOSIGNAL);

        if (n > 0)
            return n;
        if (n == 0 || errno != EINTR)
            return -1;
    }
}

static const char *error_text(int err)
{
    switch (err) {
    case BR_ERR_X509_EXPIRED:           return "certificate expired or not yet valid";
    case BR_ERR_X509_BAD_SERVER_NAME:   return "certificate does not match the host name";
    case BR_ERR_X509_NOT_TRUSTED:       return "certificate is not trusted";
    case BR_ERR_X509_BAD_SIGNATURE:     return "certificate signature is invalid";
    case BR_ERR_BAD_VERSION:            return "unsupported TLS version";
    case BR_ERR_BAD_CIPHER_SUITE:       return "no common cipher suite";
    case BR_ERR_IO:                     return "connection closed during handshake";
    case 0:                             return "ok";
    }
    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256)
        return "server sent a fatal alert";
    if (err >= BR_ERR_X509_OK)
        return "certificate validation failed";
    return "TLS protocol error";
}

static void set_clock(br_x509_minimal_context *xc)
{
    int64_t now = time(NULL);

    // Days since 0000-01-01 (proleptic Gregorian); the Unix epoch is day 719528.
    br_x509_minimal_set_time(xc, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
}

struct tls *tls_wrap(int fd, const char *host, const char *ca_file, const char **error)
{
    static char message[96];
    struct anchors a;
    struct tls *t;
    unsigned char seed[32];
    int rfd, err;

    if (!ca_file)
        ca_file = TLS_CA_BUNDLE;
    if (!anchors_for(ca_file, &a)) {
        snprintf(message, sizeof(message), "cannot load trusted certificates from %s", ca_file);
        *error = message;
        close(fd);
        return NULL;
    }
    if (!(t = calloc(1, sizeof(*t)))) {
        *error = "out of memory";
        close(fd);
        return NULL;
    }
    t->fd = fd;
    br_ssl_client_init_full(&t->cc, &t->xc, a.list, a.count);
    set_clock(&t->xc);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof(t->iobuf), 1);
    if ((rfd = open("/osystem/devices/urandom", O_RDONLY)) < 0 || read(rfd, seed, sizeof(seed)) != sizeof(seed)) {
        *error = "no random source";
        if (rfd >= 0)
            close(rfd);
        tls_close(t);
        return NULL;
    }
    close(rfd);
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
    if (!br_ssl_client_reset(&t->cc, host, 0)) {
        *error = "cannot start the handshake";
        tls_close(t);
        return NULL;
    }
    br_sslio_init(&t->io, &t->cc.eng, low_read, &t->fd, low_write, &t->fd);
    // Flushing with nothing queued drives the handshake to completion.
    br_sslio_flush(&t->io);
    if ((err = br_ssl_engine_last_error(&t->cc.eng)) || (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_CLOSED)) {
        snprintf(message, sizeof(message), "%s (BearSSL error %d)", error_text(err), err);
        *error = message;
        tls_close(t);
        return NULL;
    }
    return t;
}

struct tls *tls_connect(const char *host, uint16_t port, const char *ca_file, const char **error)
{
    static char message[96];
    int fd = tcp_connect(host, port, 15000);

    if (fd < 0) {
        snprintf(message, sizeof(message), "%s", strerror(errno));
        *error = message;
        return NULL;
    }
    return tls_wrap(fd, host, ca_file, error);
}

ssize_t tls_read(struct tls *t, void *buf, size_t len)
{
    int n = br_sslio_read(&t->io, buf, len);

    if (n < 0) {
        // A clean close_notify, or the peer closing after it, is end of file.
        int err = br_ssl_engine_last_error(&t->cc.eng);
        return err == 0 || err == BR_ERR_IO ? 0 : -1;
    }
    return n;
}

ssize_t tls_write(struct tls *t, const void *buf, size_t len)
{
    if (br_sslio_write_all(&t->io, buf, len) < 0 || br_sslio_flush(&t->io) < 0)
        return -1;
    return len;
}

int tls_fd(struct tls *t)
{
    return t->fd;
}

const char *tls_error(struct tls *t)
{
    int err = br_ssl_engine_last_error(&t->cc.eng);

    snprintf(t->error, sizeof(t->error), "%s (BearSSL error %d)", error_text(err), err);
    return t->error;
}

void tls_close(struct tls *t)
{
    if (!t)
        return;
    if (br_ssl_engine_current_state(&t->cc.eng) != BR_SSL_CLOSED)
        br_sslio_close(&t->io);
    close(t->fd);
    memset(t, 0, sizeof(*t));
    free(t);
}
