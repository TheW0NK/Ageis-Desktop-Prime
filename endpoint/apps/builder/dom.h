#ifndef BUILDER_DOM_H
#define BUILDER_DOM_H

#include "aegis.h"

// The document Window Builder edits: AUI elements with attributes. Text
// content is kept as a "text" attribute.

#define DOM_MAX_ATTRS 32

struct node {
    char tag[24];
    int nattrs;
    char *names[DOM_MAX_ATTRS], *values[DOM_MAX_ATTRS];
    struct node *parent, *first, *next;
    int id;                         // stable while the program runs
};

struct node *dom_new(const char *tag);
void dom_free(struct node *n);
struct node *dom_clone(const struct node *n);
const char *dom_get(const struct node *n, const char *name);
void dom_set(struct node *n, const char *name, const char *value);
void dom_unset(struct node *n, const char *name);
void dom_append(struct node *parent, struct node *child);
void dom_insert_after(struct node *ref, struct node *child);
void dom_detach(struct node *n);
struct node *dom_prev(struct node *n);
struct node *dom_find(struct node *root, int id);
// Parses AUI text. Returns the root (a <window>), or NULL with err set.
struct node *dom_parse(const char *text, char *err, size_t size);
// AUI text for the tree. preview: skip the <window> element itself, tag
// elements with __wb ids and drop handlers.
char *dom_serialize(const struct node *root, bool preview);
bool dom_is_container(const char *tag);

#endif
