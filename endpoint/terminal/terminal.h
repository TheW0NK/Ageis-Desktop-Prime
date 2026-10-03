#ifndef AEGIS_TERMINAL_H
#define AEGIS_TERMINAL_H

#include "aegis.h"

#define LINE_MAX    1024
#define ARGS_MAX    64

struct command {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
};

struct renamed_command {
    const char *old, *now;
};

extern const struct command commands[];
extern const struct renamed_command renamed_commands[];
extern const size_t command_count;

extern char user_name[32];
extern char home_dir[256];

const struct command *find_command(const char *name);
int run_args(int argc, char **argv);
int run_external(int argc, char **argv);
int report_status(int status);
ssize_t read_secret(const char *prompt, char *buf, size_t size);
void set_raw(bool raw);
void history_print(void);

int name_to_uid(const char *name, uint32_t *uid);
int name_to_gid(const char *name, uint32_t *gid);
const char *uid_to_name(uint32_t uid, char *buf, size_t size);
const char *gid_to_name(uint32_t gid, char *buf, size_t size);

#define fail(cmd, what) dprintf(STDERR_FILENO, "%s: %s: %s\n", cmd, what, strerror(errno))

#endif
