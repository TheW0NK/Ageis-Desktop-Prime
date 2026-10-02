#ifndef MAIL_H
#define MAIL_H

#include "aegis.h"
#include "tls.h"

// Email: an IMAP client that keeps a local copy of each folder, an SMTP
// client for sending, and MIME for reading and writing messages.

enum { SEC_NONE, SEC_TLS, SEC_STARTTLS };

struct account {
    char name[64];                  // shown as the sender's name
    char email[128];
    char imap_host[128];
    int imap_port, imap_security;
    char smtp_host[128];
    int smtp_port, smtp_security;
    char user[128];                 // sign-in name (often the address)
    char ca_file[256];              // extra certificates to trust (private servers)
};

int account_load(struct account *a);            // 0 if one is set up
int account_save(const struct account *a);
// The name its password is stored under in the credential store.
void account_secret_name(const struct account *a, char *out, size_t size);

// ---- Connections (conn.c) ----

struct mconn {
    int fd;
    struct tls *tls;
    char buf[16384];
    size_t len, pos;
    char error[256];
};

int mconn_open(struct mconn *c, const char *host, int port, bool tls, const char *ca_file);
int mconn_starttls(struct mconn *c, const char *host, const char *ca_file);
// One line without its CRLF; -1 on errors or the end.
ssize_t mconn_line(struct mconn *c, char *line, size_t size);
int mconn_read(struct mconn *c, char *buf, size_t n);
int mconn_send(struct mconn *c, const void *data, size_t len);
int mconn_printf(struct mconn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void mconn_close(struct mconn *c);

// ---- Messages (mime.c) ----

struct attachment {
    char *name, *type;
    char *data;
    size_t len;
};

struct message {
    char *from, *to, *cc, *subject, *date, *message_id, *references;
    char *text;                     // the body to show, as plain text
    struct attachment *atts;
    int natts;
};

int mime_parse(const char *raw, size_t len, struct message *m);
void mime_free(struct message *m);
// One header, unfolded and decoded (malloc'd), or NULL.
char *mime_header(const char *raw, size_t len, const char *name);
// The address alone from "Name <addr>".
void mime_address(const char *from, char *out, size_t size);
// The display name, or the address if there is none.
void mime_display_name(const char *from, char *out, size_t size);
// A new message with optional attachments (file paths). Returns malloc'd text.
char *mime_compose(const struct account *a, const char *to, const char *cc, const char *subject,
                   const char *body, const char *in_reply_to, char **attachments, int natts, size_t *len_out);
// Seconds since 1970 from an RFC 5322 date (0 if unreadable).
int64_t mime_date(const char *date);

// ---- The local copy (store.c) ----

struct summary {
    uint32_t uid;
    bool seen, flagged, answered;
    int64_t when;
    char from[128], subject[200];
};

// Folder names are kept as the server spells them.
void store_dir(const char *folder, char *out, size_t size);
int store_folders(char names[][128], int max);
void store_set_folders(char names[][128], int n);
int store_list(const char *folder, struct summary **out);      // newest first
char *store_read(const char *folder, uint32_t uid, size_t *len);
int store_add(const char *folder, uint32_t uid, const char *flags, const char *raw, size_t len);
bool store_has(const char *folder, uint32_t uid);
void store_set_flags(const char *folder, uint32_t uid, const char *flags);
void store_remove(const char *folder, uint32_t uid);
// Drops messages the server no longer has.
void store_keep_only(const char *folder, const uint32_t *uids, int n);

// ---- IMAP (imap.c) ----

struct imap {
    struct mconn c;
    int tag;
    char error[256];
};

int imap_open(struct imap *im, const struct account *a, const char *password);
int imap_list(struct imap *im, char names[][128], int max);
// Brings the local copy of a folder up to date; returns how many are new.
int imap_sync(struct imap *im, const char *folder);
int imap_flag(struct imap *im, const char *folder, uint32_t uid, const char *flag, bool on);
// Moves to Trash (or deletes, inside Trash).
int imap_delete(struct imap *im, const char *folder, uint32_t uid, const char *trash);
int imap_append(struct imap *im, const char *folder, const char *msg, size_t len, const char *flags);
void imap_close(struct imap *im);

// ---- SMTP (smtp.c) ----

int smtp_send(const struct account *a, const char *password, const char *msg, size_t len, char *error,
              size_t size);

#endif
