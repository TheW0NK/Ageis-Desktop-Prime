#ifndef AEGIS_SCRIPT_H
#define AEGIS_SCRIPT_H

#include "aegis.h"

// AegisScript: a small JavaScript-like language for apps made on Aegis
// (App Maker). See docs/SCRIPT.md.
//
//   let names = ["Ada", "Grace"];
//   fn greet(name) { return "Hello, " + name; }
//   for (n in names) { print(greet(n)); }

struct script;
struct widget;

enum script_type {
    S_NIL, S_BOOL, S_NUM, S_STR, S_LIST, S_MAP, S_FN, S_NATIVE, S_WIDGET,
};

struct script_value {
    enum script_type type;
    union {
        bool b;
        double n;
        struct s_str *s;
        struct s_list *l;
        struct s_map *m;
        struct s_func *f;
        struct s_native *nf;
        struct widget *w;
    };
};

typedef struct script_value (*script_native)(struct script *s, struct script_value *args, int nargs);

struct script *script_new(void);
void script_free(struct script *s);
// Loads and runs a program's top level. Returns false on an error
// (script_error() says what and where).
bool script_run(struct script *s, const char *source, const char *name);
// Calls a global function by name. Returns false if it fails or is missing.
bool script_call(struct script *s, const char *function, struct script_value *args, int nargs,
                 struct script_value *result);
bool script_has_function(struct script *s, const char *function);
const char *script_error(struct script *s);
void script_define(struct script *s, const char *name, script_native fn);
void script_set_global(struct script *s, const char *name, struct script_value v);

// Values.
struct script_value script_nil(void);
struct script_value script_num(double n);
struct script_value script_bool(bool b);
struct script_value script_str(const char *text);
struct script_value script_str_len(const char *text, size_t len);
struct script_value script_list(void);
void script_list_push(struct script_value list, struct script_value v);
struct script_value script_widget(struct widget *w);
void script_retain(struct script_value v);
void script_release(struct script_value v);
// Text form of any value (a malloc'd string).
char *script_to_string(struct script_value v);
bool script_truthy(struct script_value v);
// For natives: raise an error from inside a native function.
struct script_value script_fail(struct script *s, const char *fmt, ...);
// The string inside a value (or "" if not a string).
const char *script_text(struct script_value v);

// The UI library (ui.*, dialogs, files, timers) for apps.
void script_add_ui(struct script *s);

#endif
