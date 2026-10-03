#include "aegis.h"

// Kernel self-test: threads, FPU state, futexes, memory mapping, shared
// memory, pipes, poll, signals, Unix sockets with descriptor passing, and
// process information. Prints PASS/FAIL per test and "ktest: all passed".

static int failures;

#define CHECK(name, cond) do {                                          \
        if (cond)                                                       \
            printf("PASS %s\n", name);                                  \
        else {                                                          \
            printf("FAIL %s (line %d, errno %d)\n", name, __LINE__, errno); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static char *self_path;

static int run_child(char *mode, int *status)
{
    char *argv[] = { "ktest", mode, NULL };
    int pid = spawn(self_path, argv, environ);

    if (pid < 0)
        return -1;
    waitpid(pid, status, 0);
    return pid;
}

// ---- Threads and FPU ----

static mutex_t counter_lock;
static long counter;

static void *count_up(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        mutex_lock(&counter_lock);
        counter++;
        mutex_unlock(&counter_lock);
    }
    return (void *)7;
}

static void *float_work(void *arg)
{
    double x = (double)(long)arg, sum = 0;

    for (int i = 1; i <= 200000; i++) {
        sum += x / i;
        if (i % 1000 == 0)
            yield();
    }
    return (void *)(long)(sum * 1000);
}

static void test_threads(void)
{
    thread_t t[4];
    void *r = NULL;
    bool ok = true;

    for (int i = 0; i < 4; i++)
        ok &= thread_create(&t[i], count_up, NULL) == 0;
    for (int i = 0; i < 4; i++)
        ok &= thread_join(t[i], &r) == 0 && r == (void *)7;
    CHECK("threads: create, mutex, join", ok && counter == 80000);

    // Two threads doing different floating-point work concurrently must not
    // corrupt each other's registers.
    thread_t a, b;
    void *ra, *rb;
    long expect_a = (long)float_work((void *)1), expect_b = (long)float_work((void *)3);

    thread_create(&a, float_work, (void *)1);
    thread_create(&b, float_work, (void *)3);
    thread_join(a, &ra);
    thread_join(b, &rb);
    CHECK("threads: FPU state is per thread", (long)ra == expect_a && (long)rb == expect_b);
}

static mutex_t q_lock;
static cond_t q_cond;
static int q_items;

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        mutex_lock(&q_lock);
        q_items++;
        cond_signal(&q_cond);
        mutex_unlock(&q_lock);
    }
    return NULL;
}

static void test_condvar(void)
{
    thread_t t;
    int got = 0;
    volatile uint32_t word = 5;

    thread_create(&t, producer, NULL);
    mutex_lock(&q_lock);
    while (got < 100) {
        while (!q_items)
            cond_wait(&q_cond, &q_lock);
        got += q_items;
        q_items = 0;
    }
    mutex_unlock(&q_lock);
    thread_join(t, NULL);
    CHECK("condition variables", got == 100);

    uint64_t start = uptime_ms();
    int r = futex_wait(&word, 5, 50);
    CHECK("futex timeout", r < 0 && errno == ETIMEDOUT && uptime_ms() - start >= 40);
    CHECK("futex value mismatch", futex_wait(&word, 6, 50) < 0 && errno == EAGAIN);
}

// ---- Memory ----

static void test_memory(void)
{
    size_t len = 32 << 20;
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct aegis_procinfo info[64];
    int status = 0, n;
    uint64_t resident = 0;

    CHECK("mmap 32 MiB", p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    p[0] = 1;
    p[len - 1] = 2;
    p[len / 2] = 3;
    CHECK("mmap pages are zeroed and writable", p[1] == 0 && p[0] == 1 && p[len - 1] == 2);
    n = procinfo(info, 64);
    for (int i = 0; i < n; i++) {
        if (info[i].pid == getpid())
            resident = info[i].memory;
    }
    // Only touched pages are resident: far less than the 32 MiB mapped.
    CHECK("demand paging", resident > 0 && resident < (8 << 20));
    CHECK("munmap", munmap(p, len) == 0);

    char *big = malloc(1 << 20);
    memset(big, 0x5A, 1 << 20);
    CHECK("large malloc", big && big[(1 << 20) - 1] == 0x5A);
    free(big);

    run_child("segv", &status);
    CHECK("NULL dereference gives SIGSEGV", WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    run_child("readonly", &status);
    CHECK("write to PROT_READ gives SIGSEGV", WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    run_child("exit42", &status);
    CHECK("exit status", WIFEXITED(status) && WEXITSTATUS(status) == 42);

    int fd = open("/osystem/temp/ktest.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "mapped file contents", 20);
    char *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK("mmap a file", m != MAP_FAILED && !memcmp(m, "mapped file contents", 20) && m[20] == 0);
    close(fd);
    unlink("/osystem/temp/ktest.dat");
}

static void test_shm(void)
{
    int fd = shm_create(65536, 0);
    char *a, *b;

    CHECK("shm_create", fd >= 0);
    if (fd < 0)
        return;
    a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    b = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    strcpy(a + 40000, "shared");
    CHECK("shared memory between mappings", a != MAP_FAILED && b != MAP_FAILED && a != b
          && !strcmp(b + 40000, "shared"));
    CHECK("shm size", ioctl(fd, IOCTL_SHM_SIZE, 0) == 65536);

    // Pass the object to a child over a socket; it answers through the memory.
    int sv[2], status = 0, child;
    char *argv[] = { "ktest", "shmchild", NULL };

    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    int saved = dup(3);
    dup2(sv[1], 3);
    child = spawn(self_path, argv, environ);
    dup2(saved, 3);
    close(saved);
    close(sv[1]);
    send_fds(sv[0], "map", 3, &fd, 1);
    char reply[8] = { 0 };
    recv(sv[0], reply, sizeof(reply), 0);
    waitpid(child, &status, 0);
    CHECK("descriptor passing and cross-process shm", !strcmp(reply, "done") && !strcmp(a, "from child"));
    close(sv[0]);
    munmap(a, 65536);
    munmap(b, 65536);
    close(fd);
}

static int shm_child(void)
{
    char buf[8];
    int fds[1], nfds = 1;
    char *m;

    if (recv_fds(3, buf, sizeof(buf), fds, &nfds) < 0 || nfds != 1)
        return 1;
    m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    if (m == MAP_FAILED)
        return 2;
    strcpy(m, "from child");
    send(3, "done", 5, 0);
    return 0;
}

// ---- Pipes and poll ----

static void test_pipes(void)
{
    int fds[2];
    struct pollfd p;
    char buf[16] = { 0 };

    CHECK("pipe", pipe(fds) == 0);
    p = (struct pollfd){ fds[0], POLLIN, 0 };
    uint64_t start = uptime_ms();
    CHECK("poll times out", poll(&p, 1, 50) == 0 && uptime_ms() - start >= 40);
    write(fds[1], "data", 4);
    CHECK("poll sees data", poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
    CHECK("pipe read", read(fds[0], buf, sizeof(buf)) == 4 && !strcmp(buf, "data"));
    close(fds[1]);
    CHECK("pipe EOF", poll(&p, 1, 1000) == 1 && read(fds[0], buf, sizeof(buf)) == 0);
    close(fds[0]);

    pipe(fds);
    close(fds[0]);
    signal(SIGPIPE, SIG_IGN);
    CHECK("write to closed pipe gives EPIPE", write(fds[1], "x", 1) < 0 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(fds[1]);
}

// ---- Signals ----

static volatile int got_signal;
static volatile double handler_float;

static void on_usr1(int sig)
{
    got_signal = sig;
    handler_float = 1.5 * 2.0;      // uses SSE registers inside the handler
}

static void test_signals(void)
{
    uint64_t set = SIGBIT(SIGUSR1);
    double keep = 3.25;

    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    CHECK("signal handler runs", got_signal == SIGUSR1 && handler_float == 3.0);
    CHECK("FPU state survives a handler", keep * 2 == 6.5);

    got_signal = 0;
    sigprocmask(SIG_BLOCK, &set, NULL);
    raise(SIGUSR1);
    bool blocked = got_signal == 0;
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    CHECK("blocked signals wait until unblocked", blocked && got_signal == SIGUSR1);

    char *argv[] = { "ktest", "loop", NULL };
    int pid = spawn(self_path, argv, environ), status = 0;
    struct aegis_procinfo info[64];
    uint32_t state = 99;

    msleep(100);
    kill(pid, SIGSTOP);
    msleep(100);
    for (int i = 0, n = procinfo(info, 64); i < n; i++) {
        if (info[i].pid == pid)
            state = info[i].state;
    }
    CHECK("SIGSTOP stops a process", state == PROC_STOPPED);
    kill(pid, SIGCONT);
    msleep(50);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    CHECK("SIGKILL", WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

// ---- Sockets ----

static void *stream_server(void *arg)
{
    int lfd = (int)(long)arg, c = accept(lfd, NULL, NULL);
    char buf[64];
    ssize_t n;

    while ((n = recv(c, buf, sizeof(buf), 0)) > 0)
        send(c, buf, n, 0);         // echo
    close(c);
    return NULL;
}

static void test_sockets(void)
{
    int lfd = unix_listen("@ktest", SOCK_STREAM), c;
    thread_t t;
    char buf[64] = { 0 };
    struct ucred cred;
    uint32_t len = sizeof(cred);

    CHECK("listen on an abstract socket", lfd >= 0);
    thread_create(&t, stream_server, (void *)(long)lfd);
    c = unix_connect("@ktest", SOCK_STREAM);
    CHECK("connect", c >= 0);
    send(c, "ping", 4, 0);
    CHECK("stream echo", recv(c, buf, sizeof(buf), 0) == 4 && !strcmp(buf, "ping"));
    CHECK("SO_PEERCRED", getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0
          && cred.pid == getpid() && cred.uid == geteuid());
    shutdown(c, SHUT_WR);
    CHECK("EOF after shutdown", recv(c, buf, sizeof(buf), 0) == 0);
    close(c);
    thread_join(t, NULL);
    CHECK("connect to a closed name fails", unix_connect("@nobody", SOCK_STREAM) < 0 && errno == ECONNREFUSED);
    close(lfd);

    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    send(sv[0], "one", 3, 0);
    send(sv[0], "second", 6, 0);
    memset(buf, 0, sizeof(buf));
    ssize_t a = recv(sv[1], buf, sizeof(buf), 0);
    ssize_t b = recv(sv[1], buf + 10, sizeof(buf) - 10, 0);
    CHECK("message boundaries (seqpacket)", a == 3 && b == 6 && !strcmp(buf + 10, "second"));
    close(sv[0]);
    CHECK("peer close gives EOF", recv(sv[1], buf, sizeof(buf), 0) == 0);
    close(sv[1]);

    // A named socket in the filesystem.
    unlink("/osystem/temp/ktest.sock");
    lfd = unix_listen("/osystem/temp/ktest.sock", SOCK_STREAM);
    c = unix_connect("/osystem/temp/ktest.sock", SOCK_STREAM);
    int s = accept(lfd, NULL, NULL);
    struct pollfd p = { s, POLLIN, 0 };
    send(c, "x", 1, 0);
    CHECK("filesystem socket and poll", lfd >= 0 && c >= 0 && s >= 0 && poll(&p, 1, 1000) == 1);
    close(c);
    close(s);
    close(lfd);
    unlink("/osystem/temp/ktest.sock");
}

static void test_pty(void)
{
    int fds[2], status = 0;
    char buf[512] = { 0 }, *argv[] = { "hello", "pty", NULL };
    size_t got = 0;
    ssize_t n;

    CHECK("openpty", openpty(fds, O_CLOEXEC) == 0);
    int saved = dup(1);
    dup2(fds[1], 1);
    int pid = spawn("/sysapps/hello", argv, environ);
    dup2(saved, 1);
    close(saved);
    waitpid(pid, &status, 0);
    struct pollfd p = { fds[0], POLLIN, 0 };
    while (got < sizeof(buf) - 1 && poll(&p, 1, 200) == 1 && (n = read(fds[0], buf + got, sizeof(buf) - 1 - got)) > 0)
        got += n;
    CHECK("program output through a pty", strstr(buf, "Hello from an Aegis program") && strstr(buf, "\r\n"));

    // Cooked mode: the line is echoed to the master and delivered on Enter.
    write(fds[0], "abc\n", 4);
    memset(buf, 0, sizeof(buf));
    n = read(fds[1], buf, sizeof(buf));
    CHECK("pty line discipline", n == 4 && !strcmp(buf, "abc\n"));
    memset(buf, 0, sizeof(buf));
    n = read(fds[0], buf, sizeof(buf));
    CHECK("pty echo", n > 0 && !strncmp(buf, "abc", 3));
    CHECK("pty window size", ioctl(fds[0], IOCTL_PTY_SET_SIZE, (30 << 16) | 100) == 0
          && ioctl(fds[1], IOCTL_CONSOLE_SIZE, 0) == ((30 << 16) | 100));
    close(fds[0]);
    CHECK("pty hangup", read(fds[1], buf, sizeof(buf)) == 0);
    close(fds[1]);
}

static void test_info(void)
{
    struct aegis_sysinfo si;
    struct aegis_procinfo info[64];
    int n = procinfo(info, 64);
    bool found = false;

    for (int i = 0; i < n; i++)
        found |= info[i].pid == getpid() && !strcmp(info[i].name, "ktest");
    CHECK("procinfo lists this process", found);
    CHECK("sysinfo", sysinfo(&si) == 0 && si.cpus >= 1 && si.memory_total > si.memory_free
          && si.processes >= 2);
}

int main(int argc, char **argv)
{
    self_path = argc > 0 && strchr(argv[0], '/') ? argv[0] : "/sysapps/ktest";
    if (argc > 1) {
        if (!strcmp(argv[1], "segv"))
            return *(volatile int *)0;
        if (!strcmp(argv[1], "readonly")) {
            char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            p[0] = 1;
            return 0;
        }
        if (!strcmp(argv[1], "exit42"))
            exit(42);
        if (!strcmp(argv[1], "loop"))
            for (;;)
                ;
        if (!strcmp(argv[1], "shmchild"))
            return shm_child();
    }
    test_threads();
    test_condvar();
    test_memory();
    test_shm();
    test_pipes();
    test_signals();
    test_sockets();
    test_pty();
    test_info();
    if (failures)
        printf("ktest: %d failed\n", failures);
    else
        printf("ktest: all passed\n");
    return failures != 0;
}
