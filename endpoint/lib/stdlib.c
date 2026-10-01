#include "aegis.h"

#define ALIGN       16
#define GROW_MIN    (64 * 1024)
#define USED        ((struct block *)0x5A5A5A5A5A5A5A5AULL)
#define MAPPED      ((struct block *)0x6B6B6B6B6B6B6B6BULL)
#define MMAP_MIN    (256 * 1024)        // larger blocks get their own mapping

struct block {
    size_t size;
    struct block *next;
};

static struct block *free_list;
static mutex_t heap_lock;

void malloc_init(void)
{
    free_list = NULL;
}

static void insert_free(struct block *b)
{
    struct block **pp = &free_list, *prev = NULL;

    while (*pp && *pp < b) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    b->next = *pp;
    *pp = b;
    if (b->next && (char *)b + b->size == (char *)b->next) {
        b->size += b->next->size;
        b->next = b->next->next;
    }
    if (prev && (char *)prev + prev->size == (char *)b) {
        prev->size += b->size;
        prev->next = b->next;
    }
}

static void *alloc_locked(size_t need)
{
    for (;;) {
        for (struct block **pp = &free_list, *b; (b = *pp); pp = &b->next) {
            if (b->size < need)
                continue;
            if (b->size - need >= sizeof(struct block) + ALIGN) {
                struct block *rest = (struct block *)((char *)b + need);
                rest->size = b->size - need;
                rest->next = b->next;
                *pp = rest;
                b->size = need;
            } else {
                *pp = b->next;
            }
            b->next = USED;
            return b + 1;
        }

        size_t grow = need > GROW_MIN ? need : GROW_MIN;
        void *cur = sbrk(0);
        uintptr_t aligned = ((uintptr_t)cur + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
        if (sbrk(grow + (aligned - (uintptr_t)cur)) == (void *)-1)
            return NULL;
        struct block *b = (struct block *)aligned;
        b->size = grow;
        insert_free(b);
    }
}

void *malloc(size_t size)
{
    size_t need;
    void *p;

    if (size == 0 || size > ((size_t)1 << 40))
        return NULL;
    need = ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1)) + sizeof(struct block);
    if (need >= MMAP_MIN) {
        size_t len = (need + 4095) & ~(size_t)4095;
        struct block *b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (b == MAP_FAILED)
            return NULL;
        b->size = len;
        b->next = MAPPED;
        return b + 1;
    }
    mutex_lock(&heap_lock);
    p = alloc_locked(need);
    mutex_unlock(&heap_lock);
    return p;
}

void free(void *ptr)
{
    struct block *b;

    if (!ptr)
        return;
    b = (struct block *)ptr - 1;
    if (b->next == MAPPED) {
        munmap(b, b->size);
        return;
    }
    if (b->next != USED)
        return;
    mutex_lock(&heap_lock);
    insert_free(b);
    mutex_unlock(&heap_lock);
}

void *calloc(size_t n, size_t size)
{
    void *p;

    if (size && n > ((size_t)-1) / size)
        return NULL;
    p = malloc(n * size);
    return p ? memset(p, 0, n * size) : NULL;
}

void *realloc(void *ptr, size_t size)
{
    struct block *b;
    void *n;

    if (!ptr)
        return malloc(size);
    if (!size) {
        free(ptr);
        return NULL;
    }
    b = (struct block *)ptr - 1;
    if (b->size - sizeof(struct block) >= size)
        return ptr;
    if (!(n = malloc(size)))
        return NULL;
    memcpy(n, ptr, b->size - sizeof(struct block));
    free(ptr);
    return n;
}

int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 0x20 && c < 0x7F; }
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;

    while (isspace(*s))
        s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    for (;; s++) {
        int d = isdigit(*s) ? *s - '0' : isalpha(*s) ? tolower(*s) - 'a' + 10 : 99;
        if (d >= base)
            break;
        v = v * base + d;
    }
    if (end)
        *end = (char *)s;
    return v;
}

long strtol(const char *s, char **end, int base)
{
    while (isspace(*s))
        s++;
    if (*s == '-')
        return -(long)strtoul(s + 1, end, base);
    if (*s == '+')
        s++;
    return strtoul(s, end, base);
}

int atoi(const char *s)
{
    return strtol(s, NULL, 10);
}

char *getenv(const char *name)
{
    size_t len = strlen(name);

    for (char **e = environ; e && *e; e++) {
        if (!strncmp(*e, name, len) && (*e)[len] == '=')
            return *e + len + 1;
    }
    return NULL;
}

int setenv(const char *name, const char *value)
{
    size_t len = strlen(name), n = 0;
    char *entry = malloc(len + strlen(value) + 2);
    char **list;

    if (!entry)
        return -1;
    strcpy(entry, name);
    entry[len] = '=';
    strcpy(entry + len + 1, value);

    for (char **e = environ; e && *e; e++, n++) {
        if (!strncmp(*e, name, len) && (*e)[len] == '=') {
            *e = entry;
            return 0;
        }
    }
    if (!(list = malloc((n + 2) * sizeof(char *))))
        return -1;
    for (size_t i = 0; i < n; i++)
        list[i] = environ[i];
    list[n] = entry;
    list[n + 1] = NULL;
    environ = list;
    return 0;
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
