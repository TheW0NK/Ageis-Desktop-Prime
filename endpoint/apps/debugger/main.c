#include "aegis.h"
#include "ui.h"

// System Debugger: inspect running programs. Threads and their registers
// with symbol names, a scan of the stack for return addresses, the memory
// map, open files, raw memory, and recent crashes. Pause and resume
// programs with SIGSTOP and SIGCONT.

static const char page[] =
    "<window title='System Debugger' width='1080' height='660' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button flat='true' symbol='refresh' text='Refresh' onclick='refresh' shortcut='F5'/>"
    "    <separator/>"
    "    <button flat='true' symbol='pause' text='Pause' onclick='pause' shortcut='F6'/>"
    "    <button flat='true' symbol='play' text='Resume' onclick='resume' shortcut='F8'/>"
    "    <button flat='true' symbol='stop' text='End' onclick='end'/>"
    "    <spacer/>"
    "    <label id='lockstate' dim='true' align='center'/>"
    "    <button id='unlock' flat='true' symbol='lock' text='Unlock' onclick='unlock'/>"
    "  </toolbar>"
    "  <hbox expand='1' padding='8' spacing='8'>"
    "    <table id='procs' width='250' columns='Program|PID:60:right' onselect='pick'/>"
    "    <tabs id='tabs' expand='1' onchange='tab'>"
    "      <tab title='Threads'>"
    "        <table id='threads' expand='1' columns='Thread:70:right|State:80|Where|CPU:70:right' onselect='thread'/>"
    "        <label text='Registers' bold='true'/>"
    "        <textarea id='regs' height='110' expand='0' mono='true' readonly='true'/>"
    "      </tab>"
    "      <tab title='Stack'>"
    "        <p dim='true'>Words on the selected thread's stack that point into the program's code, newest first."
    "           Without frame pointers this is a scan, so some entries are stale.</p>"
    "        <table id='stack' expand='1' columns='Address:150|Value:150|Points into'/>"
    "      </tab>"
    "      <tab title='Memory map'>"
    "        <table id='maps' expand='1' columns='Start:150|End:150|Size:90:right|Access:70|Kind'/>"
    "      </tab>"
    "      <tab title='Files'>"
    "        <table id='files' expand='1' columns='Descriptor:90:right|Kind:110|Mode:90|Position:100:right|Size:100:right'/>"
    "      </tab>"
    "      <tab title='Memory'>"
    "        <hbox spacing='8'><label text='Address' align='center'/>"
    "          <input id='addr' width='220' placeholder='0x700000000000' onactivate='peek'/>"
    "          <button text='Show' onclick='peek'/><button text='Stack pointer' onclick='peeksp'/></hbox>"
    "        <textarea id='hex' expand='1' mono='true' readonly='true'/>"
    "      </tab>"
    "      <tab title='Crashes'>"
    "        <textarea id='crashes' expand='1' mono='true' readonly='true'/>"
    "      </tab>"
    "    </tabs>"
    "  </hbox>"
    "  <statusbar><label id='status'/></statusbar>"
    "</window>";

struct sym {
    uint64_t addr, size;
    char *name;
};

static struct ui_window *win;
static struct aegis_procinfo procs[256];
static int nprocs, pid = -1;
static struct aegis_threadinfo threads[64];
static int nthreads;
static struct sym *syms;
static int nsyms;
static uint64_t text_lo, text_hi;
static char loaded_for[64];

static void status(const char *s)
{
    ui_set_text(ui_get(win, "status"), s);
}

// ---- Symbols from the program's ELF file ----

struct elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct section {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t align, entsize;
};

struct elf_sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};

static int by_addr(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;

    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void free_symbols(void)
{
    for (int i = 0; i < nsyms; i++)
        free(syms[i].name);
    free(syms);
    syms = NULL;
    nsyms = 0;
    text_lo = text_hi = 0;
}

static void load_symbols(const char *name)
{
    char path[128];
    int fd = -1;
    struct elf_header eh;
    struct section *sh = NULL;
    char *strtab = NULL;
    struct elf_sym *st = NULL;

    if (!strcmp(name, loaded_for))
        return;
    strlcpy(loaded_for, name, sizeof(loaded_for));
    free_symbols();
    snprintf(path, sizeof(path), "/sysapps/%s", name);
    if ((fd = open(path, O_RDONLY)) < 0) {
        snprintf(path, sizeof(path), "/osystem/core/%s", name);
        if ((fd = open(path, O_RDONLY)) < 0)
            return;
    }
    if (read(fd, &eh, sizeof(eh)) != sizeof(eh) || memcmp(eh.ident, "\x7f" "ELF", 4) || eh.shentsize != sizeof(*sh)
        || !(sh = calloc(eh.shnum, sizeof(*sh))))
        goto done;
    lseek(fd, eh.shoff, SEEK_SET);
    if (read(fd, sh, eh.shnum * sizeof(*sh)) != (ssize_t)(eh.shnum * sizeof(*sh)))
        goto done;
    for (int i = 0; i < eh.shnum; i++) {
        // Executable sections bound the code addresses.
        if ((sh[i].flags & 0x4) && sh[i].addr) {
            if (!text_lo || sh[i].addr < text_lo)
                text_lo = sh[i].addr;
            if (sh[i].addr + sh[i].size > text_hi)
                text_hi = sh[i].addr + sh[i].size;
        }
        if (sh[i].type == 2 && sh[i].link < eh.shnum) {        // SHT_SYMTAB
            struct section *strs = &sh[sh[i].link];
            int count = sh[i].size / sizeof(struct elf_sym);

            if (!(st = malloc(sh[i].size)) || !(strtab = malloc(strs->size + 1)))
                goto done;
            lseek(fd, sh[i].offset, SEEK_SET);
            read(fd, st, sh[i].size);
            lseek(fd, strs->offset, SEEK_SET);
            read(fd, strtab, strs->size);
            strtab[strs->size] = 0;
            if (!(syms = calloc(count, sizeof(*syms))))
                goto done;
            for (int k = 0; k < count; k++) {
                // Functions (STT_FUNC) with a name.
                if ((st[k].info & 0xF) != 2 || !st[k].value || st[k].name >= strs->size)
                    continue;
                syms[nsyms].addr = st[k].value;
                syms[nsyms].size = st[k].size;
                syms[nsyms].name = strdup(strtab + st[k].name);
                nsyms++;
            }
        }
    }
    qsort(syms, nsyms, sizeof(*syms), by_addr);
done:
    free(sh);
    free(st);
    free(strtab);
    if (fd >= 0)
        close(fd);
}

static void symbolize(uint64_t addr, char *out, size_t size)
{
    int lo = 0, hi = nsyms - 1, best = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (syms[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best >= 0 && (addr < syms[best].addr + MAX(syms[best].size, 1) || addr < text_hi))
        snprintf(out, size, "%s+0x%lx", syms[best].name, (unsigned long)(addr - syms[best].addr));
    else
        snprintf(out, size, "0x%lx", (unsigned long)addr);
}

// ---- Showing a process ----

static void show_threads(void)
{
    struct widget *t = ui_get(win, "threads");
    static const char *const states[] = { "Running", "Ready", "Waiting", "Paused", "Ended" };

    ui_list_clear(t);
    nthreads = pid > 0 ? inspect(pid, INSPECT_THREADS, threads, sizeof(threads)) : 0;
    if (nthreads < 0) {
        status(errno == EPERM ? "This program belongs to another account: unlock to look inside."
                              : "The program has ended.");
        nthreads = 0;
        return;
    }
    for (int i = 0; i < nthreads; i++) {
        char row[256], where[160];

        symbolize(threads[i].rip, where, sizeof(where));
        snprintf(row, sizeof(row), "%lu\t%s\t%s\t%lu ms", (unsigned long)threads[i].tid,
                 states[MIN(threads[i].state, 4)], where, (unsigned long)threads[i].cpu_ms);
        ui_list_add(t, row);
    }
    if (nthreads)
        ui_list_select(t, 0);
}

static struct aegis_threadinfo *current_thread(void)
{
    int i = ui_list_selected(ui_get(win, "threads"));

    return i >= 0 && i < nthreads ? &threads[i] : NULL;
}

static void show_regs(void)
{
    struct aegis_threadinfo *t = current_thread();
    char text[1024], where[160];

    if (!t) {
        ui_set_text(ui_get(win, "regs"), "");
        return;
    }
    symbolize(t->rip, where, sizeof(where));
    snprintf(text, sizeof(text),
             "rip %016lx  %s\nrsp %016lx  rbp %016lx  rflags %08lx  fs %016lx\n"
             "rax %016lx  rbx %016lx  rcx %016lx  rdx %016lx\n"
             "rsi %016lx  rdi %016lx  r8  %016lx  r9  %016lx\n"
             "r10 %016lx  r11 %016lx  r12 %016lx  r13 %016lx\nr14 %016lx  r15 %016lx",
             t->rip, where, t->rsp, t->rbp, t->rflags, t->fs_base, t->rax, t->rbx, t->rcx, t->rdx, t->rsi, t->rdi,
             t->r8, t->r9, t->r10, t->r11, t->r12, t->r13, t->r14, t->r15);
    ui_set_text(ui_get(win, "regs"), text);
}

static void show_stack(void)
{
    struct aegis_threadinfo *t = current_thread();
    struct widget *tb = ui_get(win, "stack");
    uint64_t words[512];
    int n;

    ui_list_clear(tb);
    if (!t || !t->rsp)
        return;
    words[0] = t->rsp;
    n = inspect(pid, INSPECT_MEMORY, words, sizeof(words));
    for (int i = 0; i < n / 8; i++) {
        char row[256], where[160];

        // Return addresses point into the code.
        if (words[i] < text_lo || words[i] >= text_hi)
            continue;
        symbolize(words[i], where, sizeof(where));
        snprintf(row, sizeof(row), "%016lx\t%016lx\t%s", (unsigned long)(t->rsp + i * 8),
                 (unsigned long)words[i], where);
        ui_list_add(tb, row);
    }
}

static void show_maps(void)
{
    struct aegis_mapinfo maps[128];
    struct widget *t = ui_get(win, "maps");
    int n = pid > 0 ? inspect(pid, INSPECT_MAPS, maps, sizeof(maps)) : 0;

    ui_list_clear(t);
    if (n < 0 && errno == EAGAIN)
        n = inspect(pid, INSPECT_MAPS, maps, sizeof(maps));
    for (int i = 0; i < n; i++) {
        static const char *const kinds[] = { "Private memory", "File", "Shared memory", "Device" };
        char row[256], size[32], kind[96];

        ui_format_size(maps[i].end - maps[i].start, size, sizeof(size));
        if (maps[i].kind == MAP_KIND_FILE)
            snprintf(kind, sizeof(kind), "File (inode %lu, offset 0x%lx)", (unsigned long)maps[i].ino,
                     (unsigned long)maps[i].offset);
        else if (maps[i].start <= threads[0].rsp && threads[0].rsp < maps[i].end && nthreads)
            snprintf(kind, sizeof(kind), "%s (stack)", kinds[maps[i].kind & 3]);
        else
            snprintf(kind, sizeof(kind), "%s", kinds[maps[i].kind & 3]);
        snprintf(row, sizeof(row), "%016lx\t%016lx\t%s\t%c%c%c\t%s", (unsigned long)maps[i].start,
                 (unsigned long)maps[i].end, size, (maps[i].prot & 1) ? 'r' : '-', (maps[i].prot & 2) ? 'w' : '-',
                 (maps[i].prot & 4) ? 'x' : '-', kind);
        ui_list_add(t, row);
    }
}

static void show_files(void)
{
    struct aegis_fileinfo files[64];
    struct widget *t = ui_get(win, "files");
    int n = pid > 0 ? inspect(pid, INSPECT_FILES, files, sizeof(files)) : 0;

    ui_list_clear(t);
    for (int i = 0; i < n; i++) {
        static const char *const kinds[] = { "File", "Folder", "Device", "Pipe, socket or terminal" };
        char row[256], size[32];

        ui_format_size(files[i].size, size, sizeof(size));
        snprintf(row, sizeof(row), "%d\t%s\t%o\t%lu\t%s", files[i].fd, kinds[files[i].kind & 3],
                 files[i].mode & 07777, (unsigned long)files[i].offset, files[i].kind == FILE_KIND_FILE ? size : "");
        ui_list_add(t, row);
    }
}

// One hex dump line: address, 16 bytes in hex, then as text.
static int dump_line(char *o, uint64_t addr, const uint8_t *b, int n)
{
    int len = snprintf(o, 24, "%016lx  ", (unsigned long)addr);

    for (int i = 0; i < 16; i++) {
        if (i < n)
            len += snprintf(o + len, 8, "%02x ", b[i]);
        else
            len += snprintf(o + len, 8, "   ");
        if (i == 7)
            o[len++] = ' ';
    }
    o[len++] = ' ';
    for (int i = 0; i < n; i++)
        o[len++] = b[i] >= 0x20 && b[i] < 0x7F ? b[i] : '.';
    o[len++] = '\n';
    o[len] = 0;
    return len;
}

static void hexdump(uint64_t addr)
{
    uint8_t buf[512];
    char *out = malloc(512 / 16 * 96 + 64), *o = out;
    int n;

    if (!out)
        return;
    memcpy(buf, &addr, 8);
    n = inspect(pid, INSPECT_MEMORY, buf, sizeof(buf));
    if (n <= 0) {
        ui_set_text(ui_get(win, "hex"), "Nothing readable at this address (not mapped, or not loaded yet).");
        free(out);
        return;
    }
    *o = 0;
    for (int line = 0; line < n; line += 16) {
        o += dump_line(o, addr + line, buf + line, MIN(16, n - line));
    }
    ui_set_text(ui_get(win, "hex"), out);
    free(out);
}

static void show_crashes(void)
{
    int fd = open("/osystem/devices/kmsg", O_RDONLY | O_NONBLOCK);
    char *buf = malloc(256 * 1024), *out = malloc(64 * 1024);
    ssize_t len = 0, n;
    size_t ol = 0;

    if (!buf || !out || fd < 0) {
        ui_set_text(ui_get(win, "crashes"), "The system log is for administrators: unlock to see crashes.");
        if (fd >= 0)
            close(fd);
        free(buf);
        free(out);
        return;
    }
    while (len < 256 * 1024 - 1 && (n = read(fd, buf + len, 256 * 1024 - 1 - len)) > 0)
        len += n;
    close(fd);
    buf[len] = 0;
    out[0] = 0;
    // Crash reports and the lines that follow them (registers, backtrace).
    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');
        static int follow;

        if (nl)
            *nl = 0;
        if (strstr(line, "crashed") || strstr(line, "panic") || strstr(line, "PANIC"))
            follow = 6;
        if (follow > 0 && ol + strlen(line) + 2 < 64 * 1024) {
            ol += snprintf(out + ol, 64 * 1024 - ol, "%s\n", line);
            follow--;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    ui_set_text(ui_get(win, "crashes"), ol ? out : "No crashes since the computer started.");
    free(buf);
    free(out);
}

static void show_all(void)
{
    if (pid <= 0)
        return;
    show_threads();
    show_regs();
    show_stack();
    show_maps();
    show_files();
}

static void list_procs(void)
{
    struct widget *t = ui_get(win, "procs");
    int keep = pid;

    nprocs = procinfo(procs, 256);
    ui_list_clear(t);
    for (int i = 0; i < nprocs; i++) {
        char row[96];

        snprintf(row, sizeof(row), "%s%s\t%d", procs[i].name, procs[i].state == PROC_STOPPED ? " (paused)" : "",
                 procs[i].pid);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, i, icon_get(procs[i].uid == getuid() || geteuid() == 0 ? "glyph:cpu"
                                               : "glyph:lock", 18));
        if (procs[i].pid == keep)
            ui_list_select(t, i);
    }
}

// ---- Handlers ----

static void on_pick(struct widget *w, void *u)
{
    int i = ui_list_selected(w);
    char msg[160];

    (void)u;
    if (i < 0 || i >= nprocs)
        return;
    pid = procs[i].pid;
    load_symbols(procs[i].name);
    snprintf(msg, sizeof(msg), "%s (process %d): %d symbols", procs[i].name, pid, nsyms);
    status(msg);
    show_all();
}

static void on_thread(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show_regs();
    show_stack();
}

static void on_refresh(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    list_procs();
    show_all();
    show_crashes();
}

static void signal_current(int sig, const char *what)
{
    char msg[160];

    if (pid <= 1) {
        status("Choose a program first (init cannot be paused).");
        return;
    }
    if (kill(pid, sig) < 0) {
        snprintf(msg, sizeof(msg), "Could not %s process %d: %s.", what, pid, strerror(errno));
        status(msg);
        return;
    }
    snprintf(msg, sizeof(msg), "Process %d: %s.", pid, what);
    status(msg);
    msleep(50);
    on_refresh(NULL, NULL);
}

static void on_pause(struct widget *w, void *u) { (void)w; (void)u; signal_current(SIGSTOP, "paused"); }
static void on_resume(struct widget *w, void *u) { (void)w; (void)u; signal_current(SIGCONT, "resumed"); }
static void on_end(struct widget *w, void *u) { (void)w; (void)u; signal_current(SIGTERM, "asked to end"); }

static void on_peek(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    hexdump(strtoul(ui_text(ui_get(win, "addr")), NULL, 0));
}

static void on_peeksp(struct widget *w, void *u)
{
    struct aegis_threadinfo *t = current_thread();
    char buf[32];

    (void)w;
    (void)u;
    if (!t)
        return;
    snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)t->rsp);
    ui_set_text(ui_get(win, "addr"), buf);
    hexdump(t->rsp);
}

static void on_tab(struct widget *w, void *u)
{
    (void)u;
    if (ui_value(w) == 5)
        show_crashes();
}

static void show_lock(void)
{
    ui_set_text(ui_get(win, "lockstate"), geteuid() == 0 ? "Every program" : "Your programs");
    ui_set_visible(ui_get(win, "unlock"), geteuid() != 0);
}

static void on_unlock(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_elevate(win, "Looking inside other accounts' programs needs an administrator."))
        on_refresh(NULL, NULL);
    show_lock();
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "refresh", on_refresh }, { "pause", on_pause }, { "resume", on_resume }, { "end", on_end },
        { "pick", on_pick }, { "thread", on_thread }, { "peek", on_peek }, { "peeksp", on_peeksp },
        { "tab", on_tab }, { "unlock", on_unlock }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "debugger")))
        return 1;
    list_procs();
    if (argc > 1) {
        // debugger PID
        int want = atoi(argv[1]);

        for (int i = 0; i < nprocs; i++)
            if (procs[i].pid == want)
                ui_list_select(ui_get(win, "procs"), i);
        on_pick(ui_get(win, "procs"), NULL);
    }
    show_lock();
    status("Choose a program on the left.");
    ui_window_show(win);
    return ui_run();
}
