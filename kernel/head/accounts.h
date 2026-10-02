#ifndef AEGIS_ACCOUNTS_H
#define AEGIS_ACCOUNTS_H

#include "process.h"

#define ACCOUNT_NAME_MAX    32

struct account {
    char name[ACCOUNT_NAME_MAX];
    uint32_t uid, gid;
    char home[128];
    char shell[128];
};

int account_by_name(const char *name, struct account *out);
int account_by_uid(uint32_t uid, struct account *out);
int account_login(struct process *p, const char *name, const char *password);
int account_sudo(struct process *p, const char *password);
int account_become(struct process *p, uint32_t uid);

#endif
