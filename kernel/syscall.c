#include "syscall.h"
#include "accounts.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "mem.h"
#include "percpu.h"
#include "process.h"
#include "rtc.h"
#include "string.h"
#include "tty.h"

#define IO_CHUNK        65536
#define ARGV_MAX        256

extern void syscall_entry(void);

void syscall_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);
    wrmsr(MSR_STAR, ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)(GDT_USER_DATA - 8) << 48));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x700);
}

void syscall_set_kernel_stack(uint64_t rsp)
{
    this_cpu()->kernel_rsp = rsp;
}

static bool user_ok(uint64_t addr, uint64_t len)
{
    if (len == 0)
        return true;
    if (addr < USER_REGION_BASE || addr >= USER_REGION_END || len > USER_REGION_END - addr)
        return false;
    for (uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE); page < addr + len; page += PAGE_SIZE) {
        if (!paging_translate_in(read_cr3(), page))
            return false;
    }
    return true;
}

static int copy_in(void *dst, uint64_t src, size_t n)
{
    if (!user_ok(src, n))
        return -EFAULT;
    memcpy(dst, (const void *)src, n);
    return 0;
}

static int copy_out(uint64_t dst, const void *src, size_t n)
{
    if (!user_ok(dst, n))
        return -EFAULT;
    memcpy((void *)dst, src, n);
    return 0;
}

static int string_in(char *dst, uint64_t src, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || ((src + i) & (PAGE_SIZE - 1)) == 0) && !user_ok(src + i, 1))
            return -EFAULT;
        dst[i] = *(const char *)(src + i);
        if (!dst[i])
            return 0;
    }
    return -ENAMETOOLONG;
}

// Copies a user path into a fresh kernel buffer.
static int path_in(uint64_t src, char **out)
{
    char *p = kmalloc(AEGIS_PATH_MAX);
    int ret;

    if (!p)
        return -ENOMEM;
    if ((ret = string_in(p, src, AEGIS_PATH_MAX))) {
        kfree(p);
        return ret;
    }
    *out = p;
    return 0;
}

static void free_vector(char **v)
{
    for (size_t i = 0; v && v[i]; i++)
        kfree(v[i]);
    kfree(v);
}

static int vector_in(uint64_t src, char ***out)
{
    char **v = kzalloc((ARGV_MAX + 1) * sizeof(char *));
    size_t total = 0;

    if (!v)
        return -ENOMEM;
    for (size_t i = 0; src; i++) {
        uint64_t ptr;
        char *s;
        int ret;

        if (i == ARGV_MAX) {
            free_vector(v);
            return -E2BIG;
        }
        if ((ret = copy_in(&ptr, src + i * 8, 8))) {
            free_vector(v);
            return ret;
        }
        if (!ptr)
            break;
        if ((ret = path_in(ptr, &s))) {
            free_vector(v);
            return ret == -ENAMETOOLONG ? -E2BIG : ret;
        }
        total += strlen(s) + 1;
        v[i] = s;
        if (total > USER_ARG_MAX) {
            free_vector(v);
            return -E2BIG;
        }
    }
    *out = v;
    return 0;
}

static int64_t sys_read(struct process *p, int fd, uint64_t buf, size_t size)
{
    struct file *f = fd_get(p, fd);
    char *k;
    int64_t n;

    if (!f)
        return -EBADF;
    if (!user_ok(buf, size))
        return -EFAULT;
    size = MIN(size, IO_CHUNK);
    if (!(k = kmalloc(size ? size : 1)))
        return -ENOMEM;
    n = file_read(f, k, size);
    if (n > 0 && copy_out(buf, k, n))
        n = -EFAULT;
    kfree(k);
    return n;
}

static int64_t sys_write(struct process *p, int fd, uint64_t buf, size_t size)
{
    struct file *f = fd_get(p, fd);
    size_t done = 0;
    char *k;

    if (!f)
        return -EBADF;
    if (!user_ok(buf, size))
        return -EFAULT;
    if (!(k = kmalloc(MIN(size, (size_t)IO_CHUNK) + 1)))
        return -ENOMEM;
    while (done < size) {
        size_t n = MIN(size - done, (size_t)IO_CHUNK);
        int64_t r;

        memcpy(k, (const void *)(buf + done), n);
        r = file_write(f, k, n);
        if (r <= 0) {
            kfree(k);
            return done ? (int64_t)done : r;
        }
        done += r;
        if ((size_t)r < n)
            break;
    }
    kfree(k);
    return done;
}

static int64_t sys_open(struct process *p, uint64_t upath, uint32_t flags, uint32_t mode)
{
    struct file *f;
    char *path;
    int ret, fd;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_open(path, p->cwd, &p->cred, flags, mode, &f);
    kfree(path);
    if (ret)
        return ret;
    if ((fd = fd_alloc(p, f, 0)) < 0) {
        file_put(f);
        return fd;
    }
    p->cloexec[fd] = flags & O_CLOEXEC;
    return fd;
}

static int64_t sys_close(struct process *p, int fd)
{
    struct file *f = fd_get(p, fd);

    if (!f)
        return -EBADF;
    p->fds[fd] = NULL;
    file_put(f);
    return 0;
}

static int64_t stat_path(struct process *p, uint64_t upath, uint64_t ubuf, bool follow)
{
    struct aegis_stat st;
    struct vnode *v;
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_lookup(path, p->cwd, &p->cred, follow, &v);
    kfree(path);
    if (ret)
        return ret;
    vfs_stat(v, &st);
    vput(v);
    return copy_out(ubuf, &st, sizeof(st));
}

static int64_t sys_fstat(struct process *p, int fd, uint64_t ubuf)
{
    struct file *f = fd_get(p, fd);
    struct aegis_stat st;

    if (!f)
        return -EBADF;
    if (f->vnode) {
        vfs_stat(f->vnode, &st);
    } else {
        memset(&st, 0, sizeof(st));
        st.mode = S_IFCHR | 0620;
        st.nlink = 1;
    }
    return copy_out(ubuf, &st, sizeof(st));
}

static int64_t sys_getdents(struct process *p, int fd, uint64_t ubuf, size_t size)
{
    struct file *f = fd_get(p, fd);
    void *k;
    int64_t n;

    if (!f)
        return -EBADF;
    if (!user_ok(ubuf, size))
        return -EFAULT;
    size = MIN(size, (size_t)IO_CHUNK);
    if (!(k = kmalloc(size ? size : 1)))
        return -ENOMEM;
    n = file_getdents(f, k, size);
    if (n > 0 && copy_out(ubuf, k, n))
        n = -EFAULT;
    kfree(k);
    return n;
}

typedef int (*path_op1)(const char *path, struct vnode *cwd, const struct cred *c);

static int64_t with_path(struct process *p, uint64_t upath, path_op1 op)
{
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = op(path, p->cwd, &p->cred);
    kfree(path);
    return ret;
}

static int64_t with_two_paths(struct process *p, uint64_t a, uint64_t b,
                              int (*op)(const char *, const char *, struct vnode *, const struct cred *))
{
    char *pa, *pb;
    int ret;

    if ((ret = path_in(a, &pa)))
        return ret;
    if ((ret = path_in(b, &pb))) {
        kfree(pa);
        return ret;
    }
    ret = op(pa, pb, p->cwd, &p->cred);
    kfree(pa);
    kfree(pb);
    return ret;
}

static int64_t sys_mkdir(struct process *p, uint64_t upath, uint32_t mode)
{
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_mkdir(path, p->cwd, &p->cred, mode);
    kfree(path);
    return ret;
}

static int64_t sys_chdir(struct process *p, uint64_t upath)
{
    struct vnode *v;
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_lookup(path, p->cwd, &p->cred, true, &v);
    kfree(path);
    if (ret)
        return ret;
    if (!S_ISDIR(v->mode))
        ret = -ENOTDIR;
    else
        ret = vfs_permission(v, &p->cred, X_OK);
    if (ret) {
        vput(v);
        return ret;
    }
    vput(p->cwd);
    p->cwd = v;
    return 0;
}

static int64_t sys_getcwd(struct process *p, uint64_t ubuf, size_t size)
{
    char *k = kmalloc(AEGIS_PATH_MAX);
    int ret;

    if (!k)
        return -ENOMEM;
    ret = vfs_getcwd(p->cwd, k, AEGIS_PATH_MAX);
    if (ret == 0) {
        size_t len = strlen(k) + 1;
        ret = len > size ? -ERANGE : copy_out(ubuf, k, len);
    }
    kfree(k);
    return ret;
}

static int64_t sys_spawn(struct process *p, uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    char *path, **argv = NULL, **envp = NULL;
    int ret, pid;

    if ((ret = path_in(upath, &path)))
        return ret;
    if ((ret = vector_in(uargv, &argv)) || (ret = vector_in(uenvp, &envp)))
        goto out;
    ret = process_spawn(path, argv, envp, p, &pid);
    if (ret == 0)
        ret = pid;
out:
    free_vector(argv);
    free_vector(envp);
    kfree(path);
    return ret;
}

static int64_t sys_wait(int pid, uint64_t ustatus)
{
    int status = 0;
    int ret = process_wait(pid, &status);

    if (ret > 0 && ustatus && copy_out(ustatus, &status, sizeof(status)))
        return -EFAULT;
    return ret;
}

static int64_t sys_brk(struct process *p, uint64_t addr)
{
    uint64_t limit = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;

    if (addr == 0)
        return p->brk;
    if (addr < p->brk_start || addr > limit)
        return p->brk;

    uint64_t old_end = ALIGN_UP(p->brk, PAGE_SIZE), new_end = ALIGN_UP(addr, PAGE_SIZE);

    for (uint64_t va = old_end; va < new_end; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();

        if (!phys || !paging_map_page_in(p->space, va, phys, PTE_USER | PTE_WRITABLE)) {
            if (phys)
                pmm_free_page(phys);
            return p->brk;
        }
        memset((void *)phys, 0, PAGE_SIZE);
    }
    p->brk = addr;
    return addr;
}

static int64_t sys_dup2(struct process *p, int old, int new)
{
    struct file *f = fd_get(p, old);

    if (!f || new < 0 || new >= MAX_FDS)
        return -EBADF;
    if (old == new)
        return new;
    file_ref(f);
    file_put(p->fds[new]);
    p->fds[new] = f;
    p->cloexec[new] = false;
    return new;
}

static int64_t sys_chmod(struct process *p, uint64_t upath, uint32_t mode)
{
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_chmod(path, p->cwd, &p->cred, mode);
    kfree(path);
    return ret;
}

static int64_t sys_chown(struct process *p, uint64_t upath, uint32_t uid, uint32_t gid)
{
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_chown(path, p->cwd, &p->cred, uid, gid);
    kfree(path);
    return ret;
}

static int64_t sys_truncate(struct process *p, uint64_t upath, uint64_t size)
{
    struct vnode *v;
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_lookup(path, p->cwd, &p->cred, true, &v);
    kfree(path);
    if (ret)
        return ret;
    ret = vfs_truncate(v, size, &p->cred);
    vput(v);
    return ret;
}

static int64_t sys_ftruncate(struct process *p, int fd, uint64_t size)
{
    struct file *f = fd_get(p, fd);

    if (!f || !f->vnode)
        return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    return vfs_truncate(f->vnode, size, NULL);
}

static int64_t sys_readlink(struct process *p, uint64_t upath, uint64_t ubuf, size_t size)
{
    char *path, *k;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    if (!(k = kmalloc(AEGIS_PATH_MAX))) {
        kfree(path);
        return -ENOMEM;
    }
    ret = vfs_readlink(path, p->cwd, &p->cred, k, MIN(size, (size_t)AEGIS_PATH_MAX));
    if (ret > 0 && copy_out(ubuf, k, ret))
        ret = -EFAULT;
    kfree(k);
    kfree(path);
    return ret;
}

static int64_t sys_seteuid(struct process *p, uint32_t uid)
{
    if (uid != p->cred.uid && p->cred.euid != 0 && p->cred.uid != 0)
        return -EPERM;
    p->cred.euid = uid;
    if (uid == p->cred.uid)
        p->cred.egid = p->cred.gid;
    return 0;
}

static int64_t sys_login(struct process *p, uint64_t uname, uint64_t upass)
{
    char name[ACCOUNT_NAME_MAX], pass[256];
    int ret;

    if ((ret = string_in(name, uname, sizeof(name))) || (ret = string_in(pass, upass, sizeof(pass))))
        return ret == -ENAMETOOLONG ? -EINVAL : ret;
    ret = account_login(p, name, pass);
    memset(pass, 0, sizeof(pass));
    return ret;
}

static int64_t sys_sudo(struct process *p, uint64_t upass)
{
    char pass[256];
    int ret;

    if ((ret = string_in(pass, upass, sizeof(pass))))
        return ret == -ENAMETOOLONG ? -EINVAL : ret;
    ret = account_sudo(p, pass);
    memset(pass, 0, sizeof(pass));
    return ret;
}

static int64_t sys_reboot(struct process *p, int cmd)
{
    if (p->cred.euid != 0)
        return -EPERM;
    if (cmd != REBOOT_RESTART && cmd != REBOOT_POWEROFF)
        return -EINVAL;
    vfs_unmount_all();
    if (cmd == REBOOT_RESTART)
        acpi_reboot();
    acpi_shutdown();
}

static int64_t sys_uname(uint64_t ubuf)
{
    struct aegis_utsname u;

    memset(&u, 0, sizeof(u));
    memcpy(u.sysname, "Aegis", 6);
    memcpy(u.release, AEGIS_VERSION, sizeof(AEGIS_VERSION));
    memcpy(u.version, __DATE__ " " __TIME__, sizeof(__DATE__ " " __TIME__));
    memcpy(u.machine, "x86_64", 7);
    return copy_out(ubuf, &u, sizeof(u));
}

static int64_t sys_ioctl(struct process *p, int fd, uint64_t cmd, uint64_t arg)
{
    struct file *f = fd_get(p, fd);

    if (!f)
        return -EBADF;
    if (!f->ops || !f->ops->ioctl)
        return -ENOTTY;
    if (cmd == IOCTL_DISPLAY_MODE && p->cred.euid != 0)
        return -EPERM;
    return f->ops->ioctl(f, cmd, arg);
}

static int64_t sys_access(struct process *p, uint64_t upath, int mode)
{
    struct cred real = p->cred;
    struct vnode *v;
    char *path;
    int ret;

    real.euid = real.uid;
    real.egid = real.gid;
    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_lookup(path, p->cwd, &real, true, &v);
    kfree(path);
    if (ret)
        return ret;
    ret = mode ? vfs_permission(v, &real, mode & 7) : 0;
    vput(v);
    return ret;
}

static int64_t sys_utime(struct process *p, uint64_t upath, int64_t atime, int64_t mtime)
{
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_utime(path, p->cwd, &p->cred, atime, mtime);
    kfree(path);
    return ret;
}

static int64_t sys_statfs(struct process *p, uint64_t upath, uint64_t ubuf)
{
    struct aegis_statfs st;
    struct vnode *v;
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_lookup(path, p->cwd, &p->cred, true, &v);
    kfree(path);
    if (ret)
        return ret;
    ret = vfs_statfs(v, &st);
    vput(v);
    return ret ? ret : copy_out(ubuf, &st, sizeof(st));
}

static int64_t dispatch(struct process *p, struct syscall_frame *f)
{
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10;

    switch (f->rax) {
    case SYS_EXIT:          process_exit((int)a);
    case SYS_READ:          return sys_read(p, a, b, c);
    case SYS_WRITE:         return sys_write(p, a, b, c);
    case SYS_OPEN:          return sys_open(p, a, b, c);
    case SYS_CLOSE:         return sys_close(p, a);
    case SYS_LSEEK:         return fd_get(p, a) ? file_seek(fd_get(p, a), b, c) : -EBADF;
    case SYS_STAT:          return stat_path(p, a, b, true);
    case SYS_FSTAT:         return sys_fstat(p, a, b);
    case SYS_LSTAT:         return stat_path(p, a, b, false);
    case SYS_GETDENTS:      return sys_getdents(p, a, b, c);
    case SYS_MKDIR:         return sys_mkdir(p, a, b);
    case SYS_RMDIR:         return with_path(p, a, vfs_rmdir);
    case SYS_UNLINK:        return with_path(p, a, vfs_unlink);
    case SYS_RENAME:        return with_two_paths(p, a, b, vfs_rename);
    case SYS_CHDIR:         return sys_chdir(p, a);
    case SYS_GETCWD:        return sys_getcwd(p, a, b);
    case SYS_SPAWN:         return sys_spawn(p, a, b, c);
    case SYS_WAIT:          return sys_wait(a, b);
    case SYS_GETPID:        return p->pid;
    case SYS_GETPPID:       return p->parent ? p->parent->pid : 0;
    case SYS_SLEEP:         sched_sleep(a); return 0;
    case SYS_YIELD:         sched_yield(); return 0;
    case SYS_UPTIME:        return timer_uptime_ms();
    case SYS_TIME:          return rtc_now();
    case SYS_BRK:           return sys_brk(p, a);
    case SYS_DUP:           return fd_get(p, a) ? (file_ref(fd_get(p, a)), fd_alloc(p, fd_get(p, a), 0)) : -EBADF;
    case SYS_DUP2:          return sys_dup2(p, a, b);
    case SYS_CHMOD:         return sys_chmod(p, a, b);
    case SYS_CHOWN:         return sys_chown(p, a, b, c);
    case SYS_TRUNCATE:      return sys_truncate(p, a, b);
    case SYS_FTRUNCATE:     return sys_ftruncate(p, a, b);
    case SYS_SYMLINK:       return with_two_paths(p, a, b, vfs_symlink);
    case SYS_READLINK:      return sys_readlink(p, a, b, c);
    case SYS_LINK:          return with_two_paths(p, a, b, vfs_link);
    case SYS_SYNC:
    case SYS_FSYNC:         return vfs_sync_all();
    case SYS_GETUID:        return p->cred.uid;
    case SYS_GETEUID:       return p->cred.euid;
    case SYS_GETGID:        return p->cred.gid;
    case SYS_GETEGID:       return p->cred.egid;
    case SYS_SETEUID:       return sys_seteuid(p, a);
    case SYS_LOGIN:         return sys_login(p, a, b);
    case SYS_SUDO:          return sys_sudo(p, a);
    case SYS_REBOOT:        return sys_reboot(p, a);
    case SYS_UNAME:         return sys_uname(a);
    case SYS_KILL:          return process_kill(a, &p->cred);
    case SYS_IOCTL:         return sys_ioctl(p, a, b, c);
    case SYS_ACCESS:        return sys_access(p, a, b);
    case SYS_UTIME:         return sys_utime(p, a, b, c);
    case SYS_STATFS:        return sys_statfs(p, a, b);
    default:                (void)d; return -ENOSYS;
    }
}

void syscall_dispatch(struct syscall_frame *f)
{
    struct process *p = process_current();

    if (f->rcx >= USER_REGION_END || !p)
        process_exit(-1);
    sti();
    f->rax = dispatch(p, f);
    cli();
    process_check_killed();
}
