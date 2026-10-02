#ifndef SCRIPT_INTERNAL_H
#define SCRIPT_INTERNAL_H

#include "script.h"

// ---- Heap values ----

struct s_str {
    int refs;
    size_t len;
    char data[];
};

struct s_list {
    int refs;
    int n, cap;
    struct script_value *items;
};

struct s_map {
    int refs;
    int n, cap;
    struct s_str **keys;
    struct script_value *vals;
};

struct env;
struct node;

struct s_func {
    int refs;
    struct node *def;
    struct env *closure;
};

struct s_native {
    int refs;
    char name[32];
    script_native fn;
};

void script_map_set(struct script_value map, const char *key, struct script_value v);

#endif
