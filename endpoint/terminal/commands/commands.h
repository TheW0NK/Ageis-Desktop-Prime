#ifndef AEGIS_COMMANDS_H
#define AEGIS_COMMANDS_H

#include "../terminal.h"

int cmd_help(int argc, char **argv);
int cmd_echo(int argc, char **argv);
int cmd_exit(int argc, char **argv);
int cmd_clear(int argc, char **argv);
int cmd_history(int argc, char **argv);
int cmd_env(int argc, char **argv);
int cmd_export(int argc, char **argv);
int cmd_whoami(int argc, char **argv);
int cmd_id(int argc, char **argv);
int cmd_date(int argc, char **argv);
int cmd_uptime(int argc, char **argv);
int cmd_uname(int argc, char **argv);
int cmd_kill(int argc, char **argv);
int cmd_sleep(int argc, char **argv);
int cmd_sudo(int argc, char **argv);
int cmd_run(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_grep(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_rmdir(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_cp(int argc, char **argv);
int cmd_mv(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_chmod(int argc, char **argv);
int cmd_chown(int argc, char **argv);
int cmd_ln(int argc, char **argv);
int cmd_stat(int argc, char **argv);
int cmd_df(int argc, char **argv);
int cmd_sync(int argc, char **argv);
int cmd_reboot(int argc, char **argv);
int cmd_crash(int argc, char **argv);
int cmd_shutdown(int argc, char **argv);
int cmd_resolution(int argc, char **argv);

#endif
