#include "syscall.h"
#include "accounts.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "futex.h"
#include "mem.h"
#include "percpu.h"
#include "pipe.h"
#include "poll.h"
#include "process.h"
#include "rtc.h"
#include "socket.h"
#include "string.h"
#include "tty.h"
#include "vm.h"
#include "net.h"

#define IO_CHUNK        65536
#define ARGV_MAX        256
#define POLL_MAX        1024
#define MSG_MAX         (256 * 1024)
#define MSR_FS_BASE     0xC0000100

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

// Copies a user path into a fresh kernel buffer.
static int path_in(uint64_t src, char **out)
{
    char *p = kmalloc(AEGIS_PATH_MAX);
    int ret;

    if (!p)
        return -ENOMEM;
    if ((ret = string_from_user(p, src, AEGIS_PATH_MAX))) {
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
        if ((ret = copy_from_user(&ptr, src + i * 8, 8))) {
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
    if (!user_range_ok(buf, size))
        return -EFAULT;
    size = MIN(size, IO_CHUNK);
    if (!(k = kmalloc(size ? size : 1)))
        return -ENOMEM;
    n = file_read(f, k, size);
    if (n > 0 && copy_to_user(buf, k, n))
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
    if (!user_range_ok(buf, size))
        return -EFAULT;
    if (!(k = kmalloc(MIN(size, (size_t)IO_CHUNK) + 1)))
        return -ENOMEM;
    while (done < size) {
        size_t n = MIN(size - done, (size_t)IO_CHUNK);
        int64_t r;

        if (copy_from_user(k, buf + done, n)) {
            kfree(k);
            return done ? (int64_t)done : -EFAULT;
        }
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

static int install(struct process *p, struct file *f, bool cloexec)
{
    int fd = fd_alloc(p, f, 0);

    if (fd < 0)
        file_put(f);
    else
        p->cloexec[fd] = cloexec;
    return fd;
}

static int64_t sys_open(struct process *p, uint64_t upath, uint32_t flags, uint32_t mode)
{
    struct file *f;
    char *path;
    int ret;

    if ((ret = path_in(upath, &path)))
        return ret;
    ret = vfs_open(path, p->cwd, &p->cred, flags, mode, &f);
    kfree(path);
    if (ret)
        return ret;
    return install(p, f, flags & O_CLOEXEC);
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
    return copy_to_user(ubuf, &st, sizeof(st));
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
        st.mode = socket_from_file(f) ? S_IFSOCK | 0777 : S_IFIFO | 0600;
        st.nlink = 1;
    }
    return copy_to_user(ubuf, &st, sizeof(st));
}

static int64_t sys_getdents(struct process *p, int fd, uint64_t ubuf, size_t size)
{
    struct file *f = fd_get(p, fd);
    void *k;
    int64_t n;

    if (!f)
        return -EBADF;
    if (!user_range_ok(ubuf, size))
        return -EFAULT;
    size = MIN(size, (size_t)IO_CHUNK);
    if (!(k = kmalloc(size ? size : 1)))
        return -ENOMEM;
    n = file_getdents(f, k, size);
    if (n > 0 && copy_to_user(ubuf, k, n))
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
        ret = len > size ? -ERANGE : copy_to_user(ubuf, k, len);
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

static int64_t sys_wait(int pid, uint64_t ustatus, uint64_t options)
{
    int status = 0;
    int ret = process_wait(pid, &status, options & WNOHANG);

    if (ret > 0 && ustatus && copy_to_user(ustatus, &status, sizeof(status)))
        return -EFAULT;
    return ret;
}

static int64_t sys_brk(struct process *p, uint64_t addr)
{
    uint64_t old_end = ALIGN_UP(p->brk, PAGE_SIZE), new_end, out;

    if (addr == 0)
        return p->brk;
    if (addr < p->brk_start || addr > MMAP_BASE)
        return p->brk;
    new_end = ALIGN_UP(addr, PAGE_SIZE);
    if (new_end > old_end) {
        if (vm_mmap(p->mm, old_end, new_end - old_end, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, NULL, 0, &out))
            return p->brk;
    } else if (new_end < old_end) {
        vm_munmap(p->mm, new_end, old_end - new_end);
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
    if (ret > 0 && copy_to_user(ubuf, k, ret))
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

    if ((ret = string_from_user(name, uname, sizeof(name)))
        || (ret = string_from_user(pass, upass, sizeof(pass))))
        return ret == -ENAMETOOLONG ? -EINVAL : ret;
    ret = account_login(p, name, pass);
    memset(pass, 0, sizeof(pass));
    return ret;
}

static int64_t sys_sudo(struct process *p, uint64_t upass)
{
    char pass[256];
    int ret;

    if ((ret = string_from_user(pass, upass, sizeof(pass))))
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
    return copy_to_user(ubuf, &u, sizeof(u));
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
    return ret ? ret : copy_to_user(ubuf, &st, sizeof(st));
}

// ---- Memory ----

static int64_t sys_mmap(struct process *p, uint64_t addr, uint64_t len, uint32_t prot,
                        uint32_t flags, int fd, uint64_t offset)
{
    struct file *f = NULL;
    uint64_t out;
    int ret;

    if (!(flags & MAP_ANONYMOUS) && !(f = fd_get(p, fd)))
        return -EBADF;
    ret = vm_mmap(p->mm, addr, len, prot, flags, f, offset, &out);
    return ret ? ret : (int64_t)out;
}

static int shm_mmap_op(struct file *f, struct vma *v)
{
    struct shm_object *o = f->priv;

    v->type = VMA_SHM;
    v->shm = o;
    shm_ref(o);
    return 0;
}

static void shm_close(struct file *f)
{
    shm_put(f->priv);
}

static int64_t shm_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    struct shm_object *o = f->priv;

    (void)arg;
    return cmd == IOCTL_SHM_SIZE ? (int64_t)(o->npages * PAGE_SIZE) : -ENOTTY;
}

static const struct file_ops shm_ops = {
    .ioctl = shm_ioctl, .close = shm_close, .mmap = shm_mmap_op,
};

static int64_t sys_shm_create(struct process *p, uint64_t size, uint32_t flags)
{
    struct shm_object *o = shm_create(size);
    struct file *f;

    if (!o)
        return size ? -ENOMEM : -EINVAL;
    if (!(f = file_alloc(&shm_ops, O_RDWR))) {
        shm_put(o);
        return -ENOMEM;
    }
    f->priv = o;
    return install(p, f, flags & O_CLOEXEC);
}

// ---- Pipes, poll, fcntl ----

static int64_t sys_pipe(struct process *p, uint64_t ufds, uint32_t flags)
{
    struct file *r, *w;
    int fds[2], ret;

    if ((ret = pipe_create(flags, &r, &w)))
        return ret;
    if ((fds[0] = install(p, r, flags & O_CLOEXEC)) < 0) {
        file_put(w);
        return fds[0];
    }
    if ((fds[1] = install(p, w, flags & O_CLOEXEC)) < 0) {
        sys_close(p, fds[0]);
        return fds[1];
    }
    if (copy_to_user(ufds, fds, sizeof(fds))) {
        sys_close(p, fds[0]);
        sys_close(p, fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_poll(struct process *p, uint64_t ufds, uint64_t n, int64_t timeout)
{
    struct pollfd *fds;
    int ret;

    if (n > POLL_MAX)
        return -EINVAL;
    if (!(fds = kmalloc(n * sizeof(*fds) + 1)))
        return -ENOMEM;
    if (copy_from_user(fds, ufds, n * sizeof(*fds))) {
        kfree(fds);
        return -EFAULT;
    }
    ret = do_poll(p, fds, n, timeout);
    if (ret >= 0 && copy_to_user(ufds, fds, n * sizeof(*fds)))
        ret = -EFAULT;
    kfree(fds);
    return ret;
}

static int64_t sys_fcntl(struct process *p, int fd, int cmd, uint64_t arg)
{
    struct file *f = fd_get(p, fd);
    int nfd;

    if (!f)
        return -EBADF;
    switch (cmd) {
    case F_DUPFD:
        if (arg >= MAX_FDS)
            return -EINVAL;
        file_ref(f);
        if ((nfd = fd_alloc(p, f, arg)) < 0)
            file_put(f);
        return nfd;
    case F_GETFD:
        return p->cloexec[fd] ? FD_CLOEXEC : 0;
    case F_SETFD:
        p->cloexec[fd] = arg & FD_CLOEXEC;
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:
        f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) | (arg & (O_NONBLOCK | O_APPEND));
        return 0;
    }
    return -EINVAL;
}

// ---- Threads and signals ----

static int64_t sys_sleep(uint64_t ms)
{
    uint64_t deadline = timer_uptime_ms() + ms, now;

    while ((now = timer_uptime_ms()) < deadline) {
        wait_prepare();
        if (signal_pending())
            return -EINTR;
        sched_block_timeout(deadline - now);
    }
    return 0;
}

static int64_t sys_sigaction(struct process *p, int sig, uint64_t uact, uint64_t uold)
{
    struct aegis_sigaction act;

    if (sig < 1 || sig > NSIG)
        return -EINVAL;
    if (uold && copy_to_user(uold, &p->sigactions[sig - 1], sizeof(act)))
        return -EFAULT;
    if (!uact)
        return 0;
    if (sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    if (copy_from_user(&act, uact, sizeof(act)))
        return -EFAULT;
    if (act.handler > SIG_IGN && (!user_range_ok(act.handler, 1) || !user_range_ok(act.restorer, 1)))
        return -EFAULT;
    p->sigactions[sig - 1] = act;
    return 0;
}

static int64_t sys_sigprocmask(int how, uint64_t uset, uint64_t uold)
{
    struct thread *t = sched_current();
    uint64_t set;

    if (uold && copy_to_user(uold, &t->sig_mask, sizeof(uint64_t)))
        return -EFAULT;
    if (!uset)
        return 0;
    if (copy_from_user(&set, uset, sizeof(set)))
        return -EFAULT;
    set &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    switch (how) {
    case SIG_BLOCK:     t->sig_mask |= set; break;
    case SIG_UNBLOCK:   t->sig_mask &= ~set; break;
    case SIG_SETMASK:   t->sig_mask = set; break;
    default:            return -EINVAL;
    }
    return 0;
}

static int64_t sys_sigsuspend(uint64_t umask)
{
    struct thread *t = sched_current();
    uint64_t mask, old = t->sig_mask;

    if (copy_from_user(&mask, umask, sizeof(mask)))
        return -EFAULT;
    t->sig_mask = mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    for (;;) {
        wait_prepare();
        if (signal_pending())
            break;
        sched_block_timeout(UINT64_MAX);
    }
    // The handler runs with the suspend mask; the old one returns after it.
    (void)old;
    return -EINTR;
}

static int64_t sys_kill(struct process *p, int pid, int sig)
{
    return signal_send(pid, sig, &p->cred);
}

// ---- Sockets ----

static struct socket *sock_fd(struct process *p, int fd, int *err)
{
    struct file *f = fd_get(p, fd);
    struct socket *s = socket_from_file(f);

    *err = !f ? -EBADF : !s ? -ENOTSOCK : 0;
    return s;
}

static int install_socket(struct process *p, struct socket *s, uint32_t flags)
{
    struct file *f = socket_file(s, (flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);

    if (!f) {
        socket_free(s);
        return -ENOMEM;
    }
    return install(p, f, flags & SOCK_CLOEXEC);
}

static int64_t sys_socket(struct process *p, int domain, uint32_t type, int protocol)
{
    struct socket *s;
    int ret = socket_create(domain, type & 0xFF, protocol, &s);

    return ret ? ret : install_socket(p, s, type);
}

static int64_t sys_socketpair(struct process *p, int domain, uint32_t type, int protocol, uint64_t ufds)
{
    struct socket *a, *b;
    int fds[2], ret;

    if (protocol)
        return -EPROTONOSUPPORT;
    if ((ret = socket_pair(domain, type & 0xFF, &a, &b)))
        return ret;
    if ((fds[0] = install_socket(p, a, type)) < 0) {
        socket_free(b);
        return fds[0];
    }
    if ((fds[1] = install_socket(p, b, type)) < 0) {
        sys_close(p, fds[0]);
        return fds[1];
    }
    if (copy_to_user(ufds, fds, sizeof(fds))) {
        sys_close(p, fds[0]);
        sys_close(p, fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_bind_connect(struct process *p, int fd, uint64_t uaddr, uint32_t len, bool connect)
{
    struct kmsg m;
    struct socket *s;
    int ret;

    if (!(s = sock_fd(p, fd, &ret)))
        return ret;
    if (len > sizeof(m.addr))
        return -EINVAL;
    if (copy_from_user(m.addr, uaddr, len))
        return -EFAULT;
    if (connect)
        return s->ops->connect ? s->ops->connect(s, m.addr, len, fd_get(p, fd)->flags & O_NONBLOCK)
                               : -EOPNOTSUPP;
    return s->ops->bind ? s->ops->bind(s, m.addr, len) : -EOPNOTSUPP;
}

static int64_t sys_listen(struct process *p, int fd, int backlog)
{
    struct socket *s;
    int ret;

    if (!(s = sock_fd(p, fd, &ret)))
        return ret;
    return s->ops->listen ? s->ops->listen(s, backlog) : -EOPNOTSUPP;
}

static int64_t sys_getname(struct process *p, int fd, uint64_t uaddr, uint64_t ulen, bool peer)
{
    uint8_t addr[sizeof(struct sockaddr_un)];
    struct socket *s;
    uint32_t len;
    int ret;

    if (!(s = sock_fd(p, fd, &ret)))
        return ret;
    if (copy_from_user(&len, ulen, sizeof(len)))
        return -EFAULT;
    len = MIN(len, (uint32_t)sizeof(addr));
    if (!s->ops->getname)
        return -EOPNOTSUPP;
    if ((ret = s->ops->getname(s, peer, addr, &len)))
        return ret;
    if (copy_to_user(uaddr, addr, len) || copy_to_user(ulen, &len, sizeof(len)))
        return -EFAULT;
    return 0;
}

static int64_t sys_accept(struct process *p, int fd, uint64_t uaddr, uint64_t ulen, uint32_t flags)
{
    struct socket *s, *c;
    int ret, nfd;

    if (!(s = sock_fd(p, fd, &ret)))
        return ret;
    if (!s->ops->accept)
        return -EOPNOTSUPP;
    if ((ret = s->ops->accept(s, fd_get(p, fd)->flags & O_NONBLOCK, &c)))
        return ret;
    if ((nfd = install_socket(p, c, flags)) < 0)
        return nfd;
    if (uaddr && ulen && sys_getname(p, nfd, uaddr, ulen, true) < 0) {
        uint32_t zero = 0;
        copy_to_user(ulen, &zero, sizeof(zero));
    }
    return nfd;
}

static int64_t sys_sendmsg(struct process *p, int fd, uint64_t umsg, uint32_t flags)
{
    struct aegis_msghdr h;
    struct kmsg *m;
    struct socket *s;
    size_t total = 0;
    int64_t ret;

    if (!(s = sock_fd(p, fd, (int *)&ret)))
        return ret;
    if (copy_from_user(&h, umsg, sizeof(h)))
        return -EFAULT;
    if (h.iovlen > 64 || h.nfds > SCM_MAX_FDS || h.namelen > sizeof(((struct kmsg *)0)->addr))
        return -EINVAL;
    if (!(m = kzalloc(sizeof(*m))))
        return -ENOMEM;
    for (uint32_t i = 0; i < h.iovlen; i++) {
        struct iovec iov;

        if (copy_from_user(&iov, (uint64_t)h.iov + i * sizeof(iov), sizeof(iov))) {
            kfree(m);
            return -EFAULT;
        }
        total += iov.len;
    }
    if (total > MSG_MAX) {
        kfree(m);
        return -EMSGSIZE;
    }
    if (!(m->data = kmalloc(total + 1))) {
        kfree(m);
        return -ENOMEM;
    }
    m->len = total;
    total = 0;
    ret = 0;
    for (uint32_t i = 0; i < h.iovlen && !ret; i++) {
        struct iovec iov;

        if (copy_from_user(&iov, (uint64_t)h.iov + i * sizeof(iov), sizeof(iov))
            || copy_from_user((uint8_t *)m->data + total, (uint64_t)iov.base, iov.len))
            ret = -EFAULT;
        total += iov.len;
    }
    if (!ret && h.namelen && copy_from_user(m->addr, (uint64_t)h.name, h.namelen))
        ret = -EFAULT;
    m->addrlen = h.namelen;
    for (uint32_t i = 0; i < h.nfds && !ret; i++) {
        int32_t sfd;
        struct file *f;

        if (copy_from_user(&sfd, (uint64_t)h.fds + i * 4, 4))
            ret = -EFAULT;
        else if (!(f = fd_get(p, sfd)))
            ret = -EBADF;
        else {
            file_ref(f);
            m->fds[m->nfds++] = f;
        }
    }
    m->flags = flags;
    if (!ret)
        ret = s->ops->send(s, m, (flags & MSG_DONTWAIT) || (fd_get(p, fd)->flags & O_NONBLOCK));
    for (int i = 0; i < m->nfds; i++)
        file_put(m->fds[i]);
    kfree(m->data);
    kfree(m);
    return ret;
}

static int64_t sys_recvmsg(struct process *p, int fd, uint64_t umsg, uint32_t flags)
{
    struct aegis_msghdr h;
    struct kmsg *m;
    struct socket *s;
    size_t total = 0, done = 0;
    int64_t ret;
    int installed = 0;

    if (!(s = sock_fd(p, fd, (int *)&ret)))
        return ret;
    if (copy_from_user(&h, umsg, sizeof(h)))
        return -EFAULT;
    if (h.iovlen > 64)
        return -EINVAL;
    if (!(m = kzalloc(sizeof(*m))))
        return -ENOMEM;
    for (uint32_t i = 0; i < h.iovlen; i++) {
        struct iovec iov;

        if (copy_from_user(&iov, (uint64_t)h.iov + i * sizeof(iov), sizeof(iov))) {
            kfree(m);
            return -EFAULT;
        }
        total += iov.len;
    }
    total = MIN(total, (size_t)MSG_MAX);
    if (!(m->data = kmalloc(total + 1))) {
        kfree(m);
        return -ENOMEM;
    }
    m->len = total;
    m->flags = flags;
    ret = s->ops->recv(s, m, (flags & MSG_DONTWAIT) || (fd_get(p, fd)->flags & O_NONBLOCK));
    for (uint32_t i = 0; ret > 0 && i < h.iovlen && done < (size_t)ret; i++) {
        struct iovec iov;
        size_t n;

        if (copy_from_user(&iov, (uint64_t)h.iov + i * sizeof(iov), sizeof(iov))) {
            ret = -EFAULT;
            break;
        }
        n = MIN(iov.len, (size_t)ret - done);
        if (copy_to_user((uint64_t)iov.base, (uint8_t *)m->data + done, n)) {
            ret = -EFAULT;
            break;
        }
        done += n;
    }
    // Received descriptors become new fds in this process.
    for (int i = 0; i < m->nfds; i++) {
        int nfd;

        if (ret < 0 || (uint32_t)installed >= h.nfds) {
            file_put(m->fds[i]);
            m->flags |= MSG_CTRUNC;
            continue;
        }
        if ((nfd = install(p, m->fds[i], false)) < 0) {
            m->flags |= MSG_CTRUNC;
            continue;
        }
        if (copy_to_user((uint64_t)h.fds + installed * 4, &nfd, 4))
            ret = -EFAULT;
        installed++;
    }
    if (ret >= 0) {
        h.nfds = installed;
        h.flags = m->flags & (MSG_TRUNC | MSG_CTRUNC);
        if (h.name && h.namelen) {
            uint32_t n = MIN(h.namelen, m->addrlen);

            if (copy_to_user((uint64_t)h.name, m->addr, n))
                ret = -EFAULT;
            h.namelen = n;
        } else {
            h.namelen = 0;
        }
        if (copy_to_user(umsg, &h, sizeof(h)))
            ret = -EFAULT;
    }
    kfree(m->data);
    kfree(m);
    return ret;
}

static int64_t sys_shutdown(struct process *p, int fd, int how)
{
    struct socket *s;
    int ret;

    if (!(s = sock_fd(p, fd, &ret)))
        return ret;
    if (how < SHUT_RD || how > SHUT_RDWR)
        return -EINVAL;
    return s->ops->shutdown ? s->ops->shutdown(s, how) : -EOPNOTSUPP;
}

static int64_t sys_sockopt(struct process *p, int fd, int level, int opt, uint64_t uval, uint64_t ulen,
                           bool set)
{
    uint8_t val[64];
    struct socket *s;
    uint32_t len;
    int ret;

    if (!(s = sock_fd(p, fd, &ret)))
        return ret;
    if (set) {
        len = ulen;
        if (len > sizeof(val) || copy_from_user(val, uval, len))
            return -EINVAL;
        return s->ops->setsockopt ? s->ops->setsockopt(s, level, opt, val, len) : -ENOPROTOOPT;
    }
    if (copy_from_user(&len, ulen, sizeof(len)))
        return -EFAULT;
    len = MIN(len, (uint32_t)sizeof(val));
    if (!s->ops->getsockopt)
        return -ENOPROTOOPT;
    if ((ret = s->ops->getsockopt(s, level, opt, val, &len)))
        return ret;
    if (copy_to_user(uval, val, len) || copy_to_user(ulen, &len, sizeof(len)))
        return -EFAULT;
    return 0;
}

// ---- System information ----

static int64_t sys_netconfig(struct process *p, int op, int index, uint64_t uinfo)
{
    struct aegis_netif info;
    int ret;

    if (op == NETCONFIG_SET && copy_from_user(&info, uinfo, sizeof(info)))
        return -EFAULT;
    ret = net_config(op, index, &info, p->cred.euid == 0);
    if (!ret && op == NETCONFIG_GET && copy_to_user(uinfo, &info, sizeof(info)))
        return -EFAULT;
    return ret;
}

static int64_t sys_procinfo(uint64_t ubuf, uint64_t max)
{
    struct aegis_procinfo *k;
    int n;

    if (max > 4096)
        max = 4096;
    if (!(k = kmalloc(max * sizeof(*k) + 1)))
        return -ENOMEM;
    n = process_info(k, max);
    if (copy_to_user(ubuf, k, n * sizeof(*k)))
        n = -EFAULT;
    kfree(k);
    return n;
}

static int64_t sys_sysinfo(uint64_t ubuf)
{
    struct aegis_sysinfo i;

    memset(&i, 0, sizeof(i));
    i.uptime_ms = timer_uptime_ms();
    i.memory_total = pmm_total_count() * PAGE_SIZE;
    i.memory_free = pmm_free_count() * PAGE_SIZE;
    i.cpus = cpu_count;
    i.processes = process_count();
    i.threads = process_thread_total();
    for (uint32_t c = 0; c < cpu_count && c < 64; c++) {
        i.cpu_busy_ms[c] = sched_busy_ticks(c) * 1000 / TIMER_HZ;
        i.cpu_idle_ms[c] = sched_idle_ticks(c) * 1000 / TIMER_HZ;
    }
    return copy_to_user(ubuf, &i, sizeof(i));
}

static int64_t dispatch(struct process *p, struct interrupt_frame *f)
{
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8, g = f->r9;
    struct file *file;

    switch (f->rax) {
    case SYS_EXIT:          process_exit((a & 0xFF) << 8);
    case SYS_READ:          return sys_read(p, a, b, c);
    case SYS_WRITE:         return sys_write(p, a, b, c);
    case SYS_OPEN:          return sys_open(p, a, b, c);
    case SYS_CLOSE:         return sys_close(p, a);
    case SYS_LSEEK:         return (file = fd_get(p, a)) ? file_seek(file, b, c) : -EBADF;
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
    case SYS_WAIT:          return sys_wait(a, b, c);
    case SYS_GETPID:        return p->pid;
    case SYS_GETPPID:       return p->parent ? p->parent->pid : 0;
    case SYS_SLEEP:         return sys_sleep(a);
    case SYS_YIELD:         sched_yield(); return 0;
    case SYS_UPTIME:        return timer_uptime_ms();
    case SYS_TIME:          return rtc_now();
    case SYS_BRK:           return sys_brk(p, a);
    case SYS_DUP:
        if (!(file = fd_get(p, a)))
            return -EBADF;
        file_ref(file);
        return install(p, file, false);
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
    case SYS_KILL:          return sys_kill(p, a, b);
    case SYS_IOCTL:         return sys_ioctl(p, a, b, c);
    case SYS_ACCESS:        return sys_access(p, a, b);
    case SYS_UTIME:         return sys_utime(p, a, b, c);
    case SYS_STATFS:        return sys_statfs(p, a, b);
    case SYS_MMAP:          return sys_mmap(p, a, b, c, d, e, g);
    case SYS_MUNMAP:        return vm_munmap(p->mm, a, b);
    case SYS_MPROTECT:      return vm_mprotect(p->mm, a, b, c);
    case SYS_SHM_CREATE:    return sys_shm_create(p, a, b);
    case SYS_PIPE:          return sys_pipe(p, a, b);
    case SYS_POLL:          return sys_poll(p, a, b, (int64_t)c);
    case SYS_THREAD_CREATE: return process_thread_create(p, a, b, c, d, e);
    case SYS_THREAD_EXIT:   process_thread_exit((a & 0xFF) << 8);
    case SYS_GETTID:        return sched_current()->id;
    case SYS_FUTEX_WAIT:    return futex_wait(p, a, b, (int64_t)c < 0 ? UINT64_MAX : c);
    case SYS_FUTEX_WAKE:    return futex_wake(p, a, b);
    case SYS_SET_TLS:
        sched_current()->fs_base = a;
        wrmsr(MSR_FS_BASE, a);
        return 0;
    case SYS_SIGACTION:     return sys_sigaction(p, a, b, c);
    case SYS_SIGPROCMASK:   return sys_sigprocmask(a, b, c);
    case SYS_SIGSUSPEND:    return sys_sigsuspend(a);
    case SYS_SOCKET:        return sys_socket(p, a, b, c);
    case SYS_SOCKETPAIR:    return sys_socketpair(p, a, b, c, d);
    case SYS_BIND:          return sys_bind_connect(p, a, b, c, false);
    case SYS_CONNECT:       return sys_bind_connect(p, a, b, c, true);
    case SYS_LISTEN:        return sys_listen(p, a, b);
    case SYS_ACCEPT:        return sys_accept(p, a, b, c, d);
    case SYS_SENDMSG:       return sys_sendmsg(p, a, b, c);
    case SYS_RECVMSG:       return sys_recvmsg(p, a, b, c);
    case SYS_SHUTDOWN:      return sys_shutdown(p, a, b);
    case SYS_GETSOCKOPT:    return sys_sockopt(p, a, b, c, d, e, false);
    case SYS_SETSOCKOPT:    return sys_sockopt(p, a, b, c, d, e, true);
    case SYS_GETSOCKNAME:   return sys_getname(p, a, b, c, false);
    case SYS_GETPEERNAME:   return sys_getname(p, a, b, c, true);
    case SYS_PROCINFO:      return sys_procinfo(a, b);
    case SYS_SYSINFO:       return sys_sysinfo(a);
    case SYS_FCNTL:         return sys_fcntl(p, a, b, c);
    case SYS_NETCONFIG:     return sys_netconfig(p, a, b, c);
    default:                return -ENOSYS;
    }
}

int syscall_dispatch(struct interrupt_frame *f)
{
    struct process *p = process_current();
    int iret = 0;

    if (!p)
        process_exit(SIGKILL);
    sti();
    if (f->rax == SYS_SIGRETURN) {
        signal_return(f);
        iret = 1;
    } else {
        f->rax = dispatch(p, f);
    }
    signal_deliver(f);
    cli();
    // sysret cannot return to an address outside the user region safely.
    if (f->rip >= USER_REGION_END || f->rip < USER_REGION_BASE)
        iret = 1;
    // Signal delivery or sigreturn may have changed rcx and r11, which sysret
    // would overwrite.
    if (f->rcx != f->rip || f->r11 != f->rflags)
        iret = 1;
    return iret;
}
