#ifndef AEGIS_ABI_FS_H
#define AEGIS_ABI_FS_H

#include <stdint.h>

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_ACCMODE       0x0003
#define O_CREAT         0x0040
#define O_EXCL          0x0080
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_CLOEXEC       0x80000

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#define S_IFMT          0170000
#define S_IFSOCK        0140000
#define S_IFLNK         0120000
#define S_IFREG         0100000
#define S_IFBLK         0060000
#define S_IFDIR         0040000
#define S_IFCHR         0020000
#define S_IFIFO         0010000
#define S_ISUID         04000
#define S_ISGID         02000
#define S_ISVTX         01000

#define S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)      (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)      (((m) & S_IFMT) == S_IFCHR)
#define S_ISSOCK(m)     (((m) & S_IFMT) == S_IFSOCK)
#define S_ISFIFO(m)     (((m) & S_IFMT) == S_IFIFO)

#define R_OK            4
#define W_OK            2
#define X_OK            1
#define F_OK            0

#define DT_UNKNOWN      0
#define DT_FIFO         1
#define DT_CHR          2
#define DT_DIR          4
#define DT_BLK          6
#define DT_REG          8
#define DT_LNK          10
#define DT_SOCK         12

#define AEGIS_NAME_MAX  255
#define AEGIS_PATH_MAX  1024

struct aegis_stat {
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blocks;            // 512-byte units
    uint32_t blksize;
    uint32_t reserved;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
};

// getdents fills the buffer with these records, each padded to 8 bytes.
struct aegis_dirent {
    uint64_t ino;
    uint16_t reclen;
    uint8_t type;
    uint8_t namelen;
    char name[];
};

struct aegis_statfs {
    uint64_t block_size;
    uint64_t blocks;
    uint64_t blocks_free;
    uint64_t files;
    uint64_t files_free;
    char fstype[16];
};

struct aegis_utsname {
    char sysname[32];
    char release[32];
    char version[64];
    char machine[32];
};

#endif
