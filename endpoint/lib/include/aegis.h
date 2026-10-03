#ifndef AEGIS_LIBC_H
#define AEGIS_LIBC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/errno.h"
#include "abi/fs.h"
#include "abi/block.h"
#include "abi/mman.h"
#include "abi/net.h"
#include "abi/poll.h"
#include "abi/proc.h"
#include "abi/signal.h"
#include "abi/socket.h"
#include "abi/syscall.h"

typedef int64_t ssize_t;

#ifndef AEGIS_NO_MINMAX
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))
#endif

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

// errno is per thread.
int *__errno_location(void);
#define errno (*__errno_location())
extern char **environ;

long syscall6(long n, long a, long b, long c, long d, long e, long f);
// Turns a raw syscall result into -1 and errno on failure.
long __check(long r);
#define syscall0(n)             syscall6(n, 0, 0, 0, 0, 0, 0)
#define syscall1(n, a)          syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define syscall2(n, a, b)       syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define syscall3(n, a, b, c)    syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define syscall4(n, a, b, c, d) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define syscall5(n, a, b, c, d, e) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

// System calls. On failure they return -1 and set errno.
__attribute__((noreturn)) void exit(int status);
ssize_t read(int fd, void *buf, size_t size);
ssize_t write(int fd, const void *buf, size_t size);
int open(const char *path, int flags, ...);
int close(int fd);
int64_t lseek(int fd, int64_t off, int whence);
int stat(const char *path, struct aegis_stat *st);
int fstat(int fd, struct aegis_stat *st);
int lstat(const char *path, struct aegis_stat *st);
ssize_t getdents(int fd, void *buf, size_t size);
int mkdir(const char *path, uint32_t mode);
int rmdir(const char *path);
int unlink(const char *path);
int rename(const char *from, const char *to);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int spawn(const char *path, char *const argv[], char *const envp[]);
int waitpid(int pid, int *status, int options);
int getpid(void);
int getppid(void);
int msleep(uint64_t ms);
int yield(void);
uint64_t uptime_ms(void);
int64_t time(int64_t *out);
void *sbrk(intptr_t increment);
int dup(int fd);
int dup2(int old, int new);
int chmod(const char *path, uint32_t mode);
int chown(const char *path, uint32_t uid, uint32_t gid);
int truncate(const char *path, uint64_t size);
int ftruncate(int fd, uint64_t size);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t size);
int link(const char *existing, const char *path);
int sync(void);
uint32_t getuid(void);
uint32_t geteuid(void);
uint32_t getgid(void);
uint32_t getegid(void);
int seteuid(uint32_t uid);
int login(const char *user, const char *password);
int sudo(const char *password);
// Root only: turn into another account (uid, gid and groups).
int become(uint32_t uid);
// A look inside a process (threads, memory map, files, memory): abi/proc.h.
int inspect(int pid, int what, void *buf, size_t size);
int reboot(int cmd);
int uname(struct aegis_utsname *u);
int kill(int pid, int sig);
long ioctl(int fd, unsigned long cmd, unsigned long arg);
int access(const char *path, int mode);
int utime(const char *path, int64_t atime, int64_t mtime);
int statfs(const char *path, struct aegis_statfs *st);
// Root only: mount a filesystem on a disk or partition ("dA2") at an absolute path.
int mount(const char *fstype, const char *dev, const char *target, uint32_t flags);
int umount(const char *target);

// Memory, pipes, polling and descriptors.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int shm_create(size_t size, int flags);     // shared memory object; mmap it with MAP_SHARED
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
// A pseudo-terminal: fds[0] is the master (the emulator's side), fds[1] the slave.
int openpty(int fds[2], int flags);
int poll(struct pollfd *fds, size_t n, int timeout_ms);
int fcntl(int fd, int cmd, long arg);
int procinfo(struct aegis_procinfo *buf, int max);
int sysinfo(struct aegis_sysinfo *info);

// Signals.
typedef void (*sighandler_t)(int);
#undef SIG_DFL
#undef SIG_IGN
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct aegis_sigaction *act, struct aegis_sigaction *old);
int sigprocmask(int how, const uint64_t *set, uint64_t *old);
int sigsuspend(const uint64_t *mask);
int raise(int sig);
__attribute__((noreturn)) void abort(void);

// Threads. Mutexes and condition variables need no initialisation beyond
// being zeroed.
typedef struct { volatile uint32_t state; } mutex_t;
typedef struct { volatile uint32_t seq; } cond_t;
typedef struct thread *thread_t;
int thread_create(thread_t *out, void *(*fn)(void *), void *arg);
int thread_join(thread_t t, void **result);
__attribute__((noreturn)) void thread_exit(void *result);
thread_t thread_self(void);
int gettid(void);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);
void cond_wait(cond_t *c, mutex_t *m);
bool cond_timedwait(cond_t *c, mutex_t *m, uint64_t timeout_ms);     // false on timeout
void cond_signal(cond_t *c);
void cond_broadcast(cond_t *c);
int futex_wait(volatile uint32_t *addr, uint32_t val, int64_t timeout_ms);
int futex_wake(volatile uint32_t *addr, int count);

// Sockets.
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const void *addr, uint32_t len);
int listen(int fd, int backlog);
int accept(int fd, void *addr, uint32_t *len);
int accept4(int fd, void *addr, uint32_t *len, int flags);
int connect(int fd, const void *addr, uint32_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const void *addr, uint32_t alen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, void *addr, uint32_t *alen);
ssize_t sendmsg(int fd, const struct aegis_msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct aegis_msghdr *msg, int flags);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int opt, void *val, uint32_t *len);
int setsockopt(int fd, int level, int opt, const void *val, uint32_t len);
int getsockname(int fd, void *addr, uint32_t *len);
int getpeername(int fd, void *addr, uint32_t *len);
// AF_UNIX helpers: name is a path, or "@name" for an abstract socket.
int unix_listen(const char *name, int type);
int unix_connect(const char *name, int type);
// Sends or receives data together with file descriptors.
ssize_t send_fds(int fd, const void *buf, size_t len, const int *fds, int nfds);
ssize_t recv_fds(int fd, void *buf, size_t len, int *fds, int *nfds);

// Internet.
static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
// Parses dotted-quad text into a network-order address; returns false if invalid.
bool inet_parse(const char *text, uint32_t *addr);
// Formats a network-order address into buf (at least 16 bytes).
char *inet_format(uint32_t addr, char *buf);
int netconfig(int op, int index, struct aegis_netif *info);
// Looks a host name up (numeric, /msc/hosts, then DNS). Returns 0 or -1 with
// errno set (ENOENT: no such host, ETIMEDOUT: no answer).
int resolve_host(const char *name, uint32_t *addr);
// Connects a TCP socket to host:port; returns the descriptor or -1.
int tcp_connect(const char *host, uint16_t port, uint64_t timeout_ms);

// string.h
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strdup(const char *s);
char *strndup(const char *s, size_t max);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
const char *strerror(int err);

// stdlib.h
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int atoi(const char *s);
int abs(int v);
long labs(long v);
char *getenv(const char *name);
int setenv(const char *name, const char *value);
int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int ispunct(int c);
double strtod(const char *s, char **end);
double atof(const char *s);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

// stdio.h
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s);
int putchar(int c);
int fputs_fd(const char *s, int fd);
void perror(const char *msg);
ssize_t read_line(int fd, char *buf, size_t size);
// A line in the system log (root), or on standard error.
void syslog(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// time.h
struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;   // month 0-11, years since 1900
    int tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
struct tm *gmtime_r(const int64_t *t, struct tm *tm);
struct tm *localtime_r(const int64_t *t, struct tm *tm);
int64_t timegm(const struct tm *tm);
int64_t mktime(struct tm *tm);
size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm);
int timezone_offset(void);          // seconds east of UTC
const char *timezone_name(void);

// Users (lib/user.c). Each user has /userfiles/<name>/home and /userfiles/<name>/system.
// Aegis record files (lib/records.c): accounts (.aacc) and settings (.aset).
struct rec_field {
    char *key, *value;
};
struct rec_block {
    char kind[16];                  // "account", "group", ...; "" for block 0
    char name[64];
    int n, cap;
    struct rec_field *f;
};
struct records {
    char type[16];                  // the file's type: "accounts", "settings", ...
    int n, cap;
    struct rec_block *b;            // b[0] holds the settings outside any block
};
void records_init(struct records *r, const char *type);
// Fails (leaving r empty) if the file is missing or not of this type.
int records_load(const char *path, const char *type, struct records *r);
int records_save(const struct records *r, const char *path, uint32_t mode);
void records_free(struct records *r);
struct rec_block *records_top(struct records *r);
struct rec_block *records_find(struct records *r, const char *kind, const char *name);
struct rec_block *records_add(struct records *r, const char *kind, const char *name);
void records_remove(struct records *r, const char *kind, const char *name);
const char *rec_get(const struct rec_block *b, const char *key);
int rec_set(struct rec_block *b, const char *key, const char *value);
void rec_unset(struct rec_block *b, const char *key);
bool rec_list_has(const char *list, const char *item);
// One setting outside any block of a settings file: get returns its length
// or -1; set creates the file (with mode) if needed.
int aset_get(const char *path, const char *key, char *buf, size_t size);
int aset_set(const char *path, const char *key, const char *value, uint32_t mode);
void rec_list_edit(struct rec_block *b, const char *key, const char *item, bool present);

// Accounts and groups (readable by everyone) and password hashes (superuser).
#define ACCOUNTS_FILE "/msc/accounts.aacc"
#define SECRETS_FILE  "/msc/secrets.aacc"
// A new system's accounts: the superuser and the system groups.
#define ACCOUNTS_START "aegis accounts 1\n\naccount superuser\n    id: 0\n    group: 0\n    display: Superuser\n" \
    "    home: /userfiles/superuser\n    terminal: /sysapps/terminal\n\ngroup superuser\n    id: 0\n" \
    "    members: superuser\n\ngroup logs\n    id: 4\n    members:\n\ngroup admins\n    id: 27\n    members:\n\n" \
    "group video\n    id: 44\n    members:\n\ngroup audio\n    id: 63\n    members:\n\ngroup input\n    id: 50\n" \
    "    members:\n"
// The computer's own settings: name, timezone, timezone-offset (minutes east of UTC).
#define COMPUTER_FILE "/msc/computer.aset"

struct user_info {
    char name[32];
    uint32_t uid, gid;
    char display[64];               // from their settings, else the accounts file
    char home[128];                 // /userfiles/<name>/home
    char shell[128];
    char dir[128];                  // /userfiles/<name>
};
int user_by_name(const char *name, struct user_info *out);
int user_by_uid(uint32_t uid, struct user_info *out);
int user_current(struct user_info *out);
// People (id 1000 and up), in accounts file order.
int user_list(struct user_info *out, int max);
bool user_in_group(const char *name, const char *group);
// rel is relative to /userfiles/<name>, e.g. "system/appdata".
void user_path(const struct user_info *u, const char *rel, char *buf, size_t size);
// Settings: name, theme, language, background, picture. Returns the length or -1.
int user_setting_get(const struct user_info *u, const char *key, char *buf, size_t size);
int user_setting_set(const struct user_info *u, const char *key, const char *value);
int user_setup_dirs(const struct user_info *u);

// Applications (lib/apps.c): /sysapps/registry/<id>.app files.
struct app_info {
    char id[32];
    char name[64];
    char exec[128];
    char exec_arg[160];             // an argument given in the exec line
    char icon[256];
    char suite[32];                 // System, Administrative, Default, Development
    char description[128];
    char opens[128];                // file types: ".txt;.md"
    char tier[16];                  // basic, elevated, system, powersudo
    char feature[24];               // shown only while this feature is on
    bool hidden;
};
int app_list(struct app_info *out, int max);        // sorted by name
int app_find(const char *id, struct app_info *out);
int app_for_file(const char *name, struct app_info *out);
int app_launch(const struct app_info *a, const char *arg);
// Starts a program with its standard streams on /osystem/devices/nothing; returns the pid.
int launch(const char *path, const char *arg);

// Accounts (lib/accounts.c). Changes need root.
int password_hash(const char *password, char *out, size_t size);
bool password_matches(const char *hash, const char *password);
int account_check_password(const char *name, const char *password);
int account_set_password(const char *name, const char *password);
int account_set_display_name(const char *name, const char *display);
int account_add(const char *name, const char *display, const char *password, bool admin);
int account_add_hashed(const char *name, const char *display, const char *hash, bool admin);
int account_remove(const char *name, bool remove_files);
bool account_is_admin(const char *name);
int group_set_member(const char *group, const char *user, bool member);
// Any user: through the account sentry (/osystem/core/account-sentry).
#define PRIVD_SOCKET "@aegis/accounts"
int change_own_password(const char *old_password, const char *new_password, char *error, size_t size);
// Checks the signed-in user's password (the lock screen). 0 if right.
int verify_own_password(const char *password);

// Sound: the audio server (lib/audio.c). audio_open returns a descriptor
// to write 16-bit little-endian PCM to; closing it ends the stream.
struct audio_stream {
    int id, volume;
    char name[64];
};
int audio_open(const char *name, int rate, int channels);
ssize_t audio_write(int fd, const void *pcm, size_t bytes);
int audio_get_volume(bool *muted);              // 0..100, or -1 without a server
int audio_set_volume(int volume);
int audio_set_mute(bool mute);
int audio_streams(struct audio_stream *out, int max);
int audio_set_stream_volume(int id, int volume);

// Cameras (lib/camera.c): /osystem/devices/video<index>.
struct camera_info {
    int width, height, fps;
    char name[48];
};
int camera_open(int index, struct camera_info *info);          // a descriptor, or -1
int camera_read(int fd, uint32_t *pixels, size_t bytes);       // one whole frame

// Notifications (lib/notify.c): shown by the desktop and kept in its list.
int notify(const char *app, const char *title, const char *body);
// The same to another user's desktop (for system services).
int notify_user(uint32_t uid, const char *app, const char *title, const char *body);

// Base64.
size_t base64_encode(const void *in, size_t len, char *out);    // out: 4 * ((len + 2) / 3) + 1 bytes
ssize_t base64_decode(const char *in, size_t len, void *out);   // out: at least len * 3 / 4 bytes

// Encrypted credentials (creds.c). Signing in unlocks the store: cred_unlock
// gives the session key as 64 hex digits for AEGIS_CRED_KEY.
int cred_unlock(const struct user_info *u, const char *password, char *hex_out, size_t size);
int cred_rewrap(const struct user_info *u, const char *old_password, const char *new_password);
bool cred_unlocked(void);
int cred_set(const char *name, const char *secret);
char *cred_get(const char *name);                   // free it; NULL if missing or locked
int cred_delete(const char *name);

// Routine schedules (lib/cron.c).
struct cron_job {
    char schedule[64];
    char user[32];                  // /msc/routines only
    char command[256];
    char name[64];
    bool enabled;
};
bool cron_matches(const char *schedule, const struct tm *tm);
bool cron_parse(char *line, bool with_user, struct cron_job *job);
void cron_format(const struct cron_job *job, bool with_user, char *out, size_t size);
int64_t cron_next(const char *schedule, int64_t after);
// /userfiles/<name>/system/appdata/routines/<file> ("routines", "log").
void cron_user_path(const struct user_info *u, const char *file, char *out, size_t size);

// Optional features (lib/features.c), in /msc/features.aset.
struct feature {
    const char *id, *name, *description;
    bool default_on;
    bool needs_restart;
};
int feature_list(const struct feature **out);
bool feature_enabled(const char *id);
int feature_set(const char *id, bool on);          // root

// Whole trees (lib/fileops.c).
int copy_path(const char *src, const char *dst);
int remove_path(const char *path);
int move_path(const char *src, const char *dst);
void unique_name(const char *dir, const char *name, char *out, size_t size);

// Disks (lib/disk.c), as root: listing, partitioning and formatting.
struct disk_info {
    char name[16];                  // under /osystem/devices
    char description[48];
    uint64_t size;                  // bytes
    uint32_t sector_size;
    bool mounted;
};
struct disk_part {
    const char *type;               // GPT type GUID, e.g. DISK_TYPE_ESP
    const char *name;
    uint64_t size_mb;               // 0: the rest of the disk
};
#define DISK_TYPE_ESP   "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define DISK_TYPE_DATA  "0fc63daf-8483-4772-8e79-3d69d8477de4"
int disk_list(struct disk_info *out, int max);
// Replaces the partition table; guids receives each partition's GUID.
int disk_write_gpt(const char *disk, const struct disk_part *parts, int nparts, char guids[][37]);
int mkfs_fat32(const char *part, const char *label);
int mkfs_ext4(const char *part, const char *label, void (*progress)(int percent, void *u), void *u);
void guid_to_string(const uint8_t g[16], char out[37]);

// Installing Aegis on a whole disk, erasing it (lib/install.c), as root.
struct install_options {
    const char *disk;               // e.g. "dB"
    const char *user, *display, *password;
    bool admin;
    const char *hostname;
    const char *timezone;           // name, e.g. "UTC"
    int tz_offset_min;
    const char *language;           // "en", "es", "fr", "de", "zh"
};
typedef void (*install_progress_fn)(int percent, const char *step, void *u);
int install_system(const struct install_options *o, install_progress_fn progress, void *u, char *error,
                   size_t error_size);
// Recovery: copy fresh system files onto an installed system, keeping its
// accounts, settings, apps and files.
int install_refresh(const char *root, const char *esp, install_progress_fn progress, void *u, char *error,
                    size_t error_size);

// .aip packages (lib/aip.c).
enum { AIP_TIER_BASIC, AIP_TIER_ELEVATED, AIP_TIER_SYSTEM, AIP_TIER_POWERSUDO };
struct aip_permission {
    const char *id, *description;
    int tier;
};
struct aip {
    uint8_t *data;
    size_t size, files_off;
    uint64_t payload;               // bytes of files
    uint32_t nfiles;
    char *manifest;
    char id[32], name[64], version[32], publisher[64], description[160], exec[256], icon[128];
    char suite[32], opens[128], permissions[256], command[32], scope[16];
};
struct aip_installed {
    char id[32], name[64], version[32], publisher[64], granted[256], package[400];
    bool everyone;
};
int aip_open(const char *path, struct aip *p, char *error, size_t esize);
void aip_close(struct aip *p);
int aip_requested(const struct aip *p, const struct aip_permission **out, int max);
const struct aip_permission *aip_permission_find(const char *id);
const char *aip_tier_name(int tier);
// granted: comma-separated permission ids. everyone needs root.
int aip_install(const struct aip *p, bool everyone, const char *granted, char *error, size_t esize);
int aip_list_installed(struct aip_installed *out, int max);
int aip_uninstall(const char *id, bool everyone, char *error, size_t esize);
int aip_repair(const struct aip_installed *a, char *error, size_t esize);

// System images (lib/sysimage.c): a whole installed system in one file.
typedef void (*sysimage_progress_fn)(int percent, const char *what, void *u);
int sysimage_create(const char *root, const char *out, sysimage_progress_fn progress, void *u, char *error,
                    size_t esize);
int sysimage_restore(const char *image, const char *root, sysimage_progress_fn progress, void *u, char *error,
                     size_t esize);
int sysimage_info(const char *path, int64_t *created, uint64_t *size);

// The recycle bin (lib/trash.c), in /userfiles/<name>/system/trash.
struct trash_item {
    char name[256];         // its name in the bin
    char path[512];         // where it is now
    char origin[512];       // where it was deleted from
    int64_t deleted;
    uint64_t size;
    bool dir;
};
int trash_put(const char *path);
int trash_list(struct trash_item **out);     // free() the array
int trash_restore(const struct trash_item *t);
int trash_delete(const struct trash_item *t);
int trash_empty(void);
int trash_count(void);
void trash_dir(char *out, size_t size);

// Directory reading.
struct dir_stream {
    int fd;
    size_t pos, len;
    char buf[4096];
};
struct dir_stream *opendir(const char *path);
struct aegis_dirent *readdir(struct dir_stream *d);
void closedir(struct dir_stream *d);

#endif
