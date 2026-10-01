#ifndef AEGIS_TLS_H
#define AEGIS_TLS_H

#include "aegis.h"

// TLS client connections (BearSSL). Certificates are checked against the
// roots in /etc/ssl/certs/ca-bundle.pem (or ca_file) and the host name.

#define TLS_CA_BUNDLE   "/etc/ssl/certs/ca-bundle.pem"

struct tls;

// Connects to host:port and completes the handshake. Returns NULL on
// failure with a message in *error (static storage).
struct tls *tls_connect(const char *host, uint16_t port, const char *ca_file, const char **error);
// Like tls_connect over an already connected socket, which it then owns.
struct tls *tls_wrap(int fd, const char *host, const char *ca_file, const char **error);
ssize_t tls_read(struct tls *t, void *buf, size_t len);
ssize_t tls_write(struct tls *t, const void *buf, size_t len);
int tls_fd(struct tls *t);
const char *tls_error(struct tls *t);
void tls_close(struct tls *t);

#endif
