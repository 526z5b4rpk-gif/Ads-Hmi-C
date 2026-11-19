#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- racines */

roots_t *roots_new(void)
{
    roots_t *r = xcalloc(1, sizeof *r);
    r->arena = arena_new();
    return r;
}

void roots_add(roots_t *r, const char *path, type_node *node)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4;
        r->v = xrealloc(r->v, (size_t)r->cap * sizeof *r->v);
    }
    r->v[r->n].path = arena_strdup(r->arena, path);
    r->v[r->n].node = node;
    r->n++;
}

void roots_free(roots_t *r)
{
    if (!r) return;
    arena_free(r->arena);
    free(r->v);
    free(r);
}

/* ---------------------------------------------------------- mise à plat */

typedef struct {
    leaf_list *out;
    int max_arr;
    int root;
    /* chemin en cours de construction (étendu puis tronqué à chaque niveau) */
    char *path;
    size_t cap;
} walk_ctx;

static void path_set(walk_ctx *c, size_t at, const char *fmt_part)
{
    size_t n = strlen(fmt_part);
    if (at + n + 1 > c->cap) {
        while (at + n + 1 > c->cap) c->cap *= 2;
        c->path = xrealloc(c->path, c->cap);
    }
    memcpy(c->path + at, fmt_part, n + 1);
}

static void add_leaf(walk_ctx *c, const type_node *n, int offset, int bitoff, const char *comment,
                     const attr_t *attrs, int nattrs)
{
    leaf_list *o = c->out;
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        o->v = xrealloc(o->v, (size_t)o->cap * sizeof *o->v);
    }
    leaf_t *l = &o->v[o->n];
    memset(l, 0, sizeof *l);
    l->index = o->n;
    l->path = arena_strdup(o->arena, c->path);
    l->type = n->type_name;
    l->kind = n->lkind;
    l->comment = (comment && *comment) ? comment : n->comment;
    l->enums = n->enums;
    l->nenums = n->nenums;
    /* fusion des attributs : ceux du type, puis ceux du membre qui les remplacent */
    int total = n->nattrs + nattrs;
    if (total > 0) {
        attr_t *m = arena_alloc(o->arena, (size_t)total * sizeof *m);
        int k = 0;
        for (int i = 0; i < n->nattrs; i++) m[k++] = n->attrs[i];
        for (int i = 0; i < nattrs; i++) {
            int j = 0;
            while (j < k && !str_ieq(m[j].key, attrs[i].key)) j++;
            m[j] = attrs[i];
            if (j == k) k++;
        }
        l->attrs = m;
        l->nattrs = k;
    }
    l->root = c->root;
    if (bitoff >= 0) {
        l->offset = bitoff / 8;
        l->bit = bitoff % 8;
        l->prim = n->prim == PR_BOOL ? PR_BIT : n->prim;
    } else {
        l->offset = offset;
        l->prim = n->prim;
    }
    l->size = n->size;
    o->n++;
}

/**
 * Parcours en profondeur. `plen` = longueur du chemin du nœud courant dans c->path ;
 * `bitoff` = offset en bits depuis le début du bloc pour un membre BIT, sinon -1 ;
 * `comment`/`attrs` = hérités du membre parent (prioritaires sur ceux du type).
 */
static void walk(walk_ctx *c, const type_node *n, size_t plen, int offset, int bitoff, const char *comment,
                 const attr_t *attrs, int nattrs)
{
    switch (n->kind) {
    case TN_LEAF:
        add_leaf(c, n, offset, bitoff, comment, attrs, nattrs);
        break;
    case TN_STRUCT:
        for (int i = 0; i < n->nmembers; i++) {
            const member_t *m = &n->members[i];
            path_set(c, plen, ".");
            path_set(c, plen + 1, m->name);
            int moff = offset + m->offset;
            int mbit = m->bit_offset >= 0 ? offset * 8 + m->bit_offset : -1;
            walk(c, m->node, plen + 1 + strlen(m->name), moff, mbit, m->comment, m->attrs, m->nattrs);
        }
        path_set(c, plen, "");
        break;
    case TN_ARRAY: {
        long total = 1;
        for (int d = 0; d < n->ndims; d++) {
            total *= n->dims[d].length > 0 ? n->dims[d].length : 0;
            if (total > c->max_arr) total = c->max_arr;
        }
        int esize = n->elem->size;
        int idx[16];
        int nd = n->ndims < 16 ? n->ndims : 16;
        for (long k = 0; k < total && k < c->max_arr; k++) {
            /* k -> indices, dernier indice le plus rapide */
            long rem = k;
            for (int d = nd - 1; d >= 0; d--) {
                idx[d] = n->dims[d].start + (int)(rem % n->dims[d].length);
                rem /= n->dims[d].length;
            }
            char part[16 * 14 + 4];
            size_t pl = 0;
            part[pl++] = '[';
            for (int d = 0; d < nd; d++)
                pl += (size_t)snprintf(part + pl, sizeof part - pl, d ? ",%d" : "%d", idx[d]);
            part[pl++] = ']';
            part[pl] = 0;
            path_set(c, plen, part);
            walk(c, n->elem, plen + pl, offset + (int)k * esize, -1, comment, attrs, nattrs);
        }
        path_set(c, plen, "");
        break;
    }
    }
}

void leaves_compile(const roots_t *roots, int max_array_expand, leaf_list *out)
{
    memset(out, 0, sizeof *out);
    out->arena = arena_new();
    walk_ctx c = {out, max_array_expand > 0 ? max_array_expand : 1000, 0, xmalloc(256), 256};
    for (int r = 0; roots && r < roots->n; r++) {
        c.root = r;
        path_set(&c, 0, roots->v[r].path);
        walk(&c, roots->v[r].node, strlen(roots->v[r].path), 0, -1, NULL, NULL, 0);
    }
    free(c.path);
}

void leaves_free(leaf_list *l)
{
    free(l->v);
    arena_free(l->arena);
    memset(l, 0, sizeof *l);
}

const char *leaf_kind_name(leaf_kind k)
{
    switch (k) {
    case LK_BOOL: return "bool";
    case LK_NUMBER: return "number";
    case LK_STRING: return "string";
    case LK_ENUM: return "enum";
    }
    return "number";
}

int plc_prim(const char *name, int size, prim_t *prim, leaf_kind *kind)
{
    static const struct { const char *n; prim_t p; } map[] = {
        {"BOOL", PR_BOOL}, {"BIT", PR_BOOL},
        {"SINT", PR_I8}, {"USINT", PR_U8}, {"BYTE", PR_U8},
        {"INT", PR_I16}, {"UINT", PR_U16}, {"WORD", PR_U16},
        {"DINT", PR_I32}, {"UDINT", PR_U32}, {"DWORD", PR_U32},
        {"TIME", PR_U32}, {"TOD", PR_U32}, {"TIME_OF_DAY", PR_U32}, {"DATE", PR_U32},
        {"DT", PR_U32}, {"DATE_AND_TIME", PR_U32},
        {"LINT", PR_I64}, {"ULINT", PR_U64}, {"LWORD", PR_U64}, {"LTIME", PR_U64},
        {"REAL", PR_F32}, {"LREAL", PR_F64},
    };
    if (str_istarts(name, "WSTRING")) { *prim = PR_WSTRING; *kind = LK_STRING; return 0; }
    if (str_istarts(name, "STRING")) { *prim = PR_STRING; *kind = LK_STRING; return 0; }
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (str_ieq(name, map[i].n)) {
            *prim = map[i].p;
            *kind = map[i].p == PR_BOOL ? LK_BOOL : LK_NUMBER;
            return 0;
        }
    }
    *kind = LK_NUMBER;
    switch (size) {
    case 1: *prim = PR_U8; return 0;
    case 2: *prim = PR_U16; return 0;
    case 4: *prim = PR_U32; return 0;
    case 8: *prim = PR_U64; return 0;
    }
    return -1;
}

/* ------------------------------------------------------ construction */

static int plc_size(const char *t)
{
    static const struct { const char *n; int s; } sizes[] = {
        {"BOOL", 1}, {"SINT", 1}, {"USINT", 1}, {"BYTE", 1},
        {"INT", 2}, {"UINT", 2}, {"WORD", 2},
        {"DINT", 4}, {"UDINT", 4}, {"DWORD", 4}, {"REAL", 4}, {"TIME", 4},
        {"LINT", 8}, {"ULINT", 8}, {"LWORD", 8}, {"LREAL", 8},
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (str_ieq(t, sizes[i].n)) return sizes[i].s;
    if (str_istarts(t, "STRING(")) return atoi(t + 7) + 1;
    if (str_ieq(t, "STRING")) return 81;
    if (str_istarts(t, "WSTRING(")) return (atoi(t + 8) + 1) * 2;
    if (str_ieq(t, "WSTRING")) return 162;
    return -1;
}

type_node *tn_leaf(arena_t *a, const char *plc_type, const char *unit)
{
    int size = plc_size(plc_type);
    prim_t p;
    leaf_kind k;
    if (size <= 0 || plc_prim(plc_type, size, &p, &k) != 0) return NULL;
    type_node *n = arena_alloc(a, sizeof *n);
    n->kind = TN_LEAF;
    n->type_name = arena_strdup(a, plc_type);
    n->size = size;
    n->prim = p;
    n->lkind = k;
    if (unit) {
        attr_t *at = arena_alloc(a, sizeof *at);
        at->key = "unit";
        at->value = arena_strdup(a, unit);
        n->attrs = at;
        n->nattrs = 1;
    }
    return n;
}

type_node *tn_enum(arena_t *a, const char *base, const char *enum_type, const enum_entry *e, int count)
{
    type_node *n = tn_leaf(a, base, NULL);
    if (!n) return NULL;
    n->type_name = arena_strdup(a, enum_type);
    n->lkind = LK_ENUM;
    n->enums = e;
    n->nenums = count;
    return n;
}

/** alignement naturel : taille du plus grand élément, plafonnée à 8 ; chaînes : 1 */
static int tn_align(const type_node *n)
{
    switch (n->kind) {
    case TN_LEAF:
        if (n->prim == PR_STRING || n->prim == PR_WSTRING) return 1;
        return n->size > 8 ? 8 : (n->size < 1 ? 1 : n->size);
    case TN_ARRAY:
        return tn_align(n->elem);
    case TN_STRUCT: {
        int m = 1;
        for (int i = 0; i < n->nmembers; i++) {
            int al = tn_align(n->members[i].node);
            if (al > m) m = al;
        }
        return m;
    }
    }
    return 1;
}

type_node *tn_struct(arena_t *a, const char *type_name, const member_spec *m, int count)
{
    type_node *n = arena_alloc(a, sizeof *n);
    n->kind = TN_STRUCT;
    n->type_name = arena_strdup(a, type_name);
    n->members = arena_alloc(a, (size_t)(count > 0 ? count : 1) * sizeof *n->members);
    n->nmembers = count;
    int off = 0, max_align = 1;
    for (int i = 0; i < count; i++) {
        int al = tn_align(m[i].node);
        if (al > max_align) max_align = al;
        off = (off + al - 1) / al * al;
        member_t *mm = &n->members[i];
        mm->name = arena_strdup(a, m[i].name);
        mm->offset = off;
        mm->bit_offset = -1;
        mm->node = m[i].node;
        mm->comment = arena_strdup(a, m[i].comment);
        off += m[i].node->size;
    }
    n->size = (off + max_align - 1) / max_align * max_align;
    return n;
}

type_node *tn_array(arena_t *a, type_node *elem, int start, int length)
{
    type_node *n = arena_alloc(a, sizeof *n);
    char name[256];
    snprintf(name, sizeof name, "ARRAY [%d..%d] OF %s", start, start + length - 1, elem->type_name);
    n->kind = TN_ARRAY;
    n->type_name = arena_strdup(a, name);
    n->size = elem->size * length;
    n->dims = arena_alloc(a, sizeof *n->dims);
    n->dims[0].start = start;
    n->dims[0].length = length;
    n->ndims = 1;
    n->elem = elem;
    return n;
}
