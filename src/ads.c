/*
 * Source ADS : client AMS/TCP pour automates Beckhoff TwinCAT 2/3, sans bibliothèque externe.
 *
 * Déroulement (thread propre à la source) :
 *  1. connexion TCP au routeur (port 48898) : en mode « router », enregistrement d'un port AMS auprès
 *     du routeur TwinCAT local ; en mode « direct », adresse AMS locale fixée par la configuration
 *     (une route vers cette adresse doit exister sur l'automate) ;
 *  2. lecture de la table complète des types (une seule requête, 0xF00E) puis des symboles racines
 *     configurés (0xF009) : construction des arbres de types (offsets fournis par l'automate) ;
 *  3. une notification par symbole racine : l'automate envoie le bloc entier dès qu'un octet change
 *     (vérifié tous les cycleTime ms), transmis tel quel au hub qui compare octet par octet ;
 *  4. surveillance : état ADS lu chaque seconde (détection de coupure et de passage en STOP),
 *     notification sur la version des symboles (téléchargement d'un nouveau programme -> rechargement) ;
 *  5. en cas d'erreur : déconnexion propre, nouvelle tentative après quelques secondes.
 *
 * Les écritures de l'interface sont transmises au thread de la source (une à la fois) : écriture
 * directe à l'adresse du symbole racine + offset, ou par handle de symbole pour un BIT (sinon les
 * autres bits du même octet seraient écrasés).
 */
#include "hub.h"
#include "plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- constantes du protocole */

#define AMS_TCP_PORT_AMS_CMD 0x0000
#define AMS_TCP_PORT_CLOSE 0x0001
#define AMS_TCP_PORT_CONNECT 0x1000
#define AMS_TCP_PORT_ROUTER_NOTE 0x1001

#define ADSCMD_READ_DEVICE_INFO 1
#define ADSCMD_READ 2
#define ADSCMD_WRITE 3
#define ADSCMD_READ_STATE 4
#define ADSCMD_ADD_NOTIF 6
#define ADSCMD_DEL_NOTIF 7
#define ADSCMD_NOTIF 8
#define ADSCMD_READ_WRITE 9

#define STATEFLAG_REQUEST 0x0004
#define STATEFLAG_RESPONSE 0x0005

#define IG_SYM_HANDLE_BY_NAME 0xF003
#define IG_SYM_VALUE_BY_HANDLE 0xF005
#define IG_SYM_RELEASE_HANDLE 0xF006
#define IG_SYM_VERSION 0xF008
#define IG_SYM_INFO_BY_NAME_EX 0xF009
#define IG_SYM_UPLOAD_INFO 0xF00C
#define IG_SYM_DT_UPLOAD 0xF00E

#define ADSTRANS_SERVERCYCLE 3
#define ADSTRANS_SERVERONCHA 4

/* drapeaux des types (ADS_DATA_TYPE_FLAGS) */
#define DTF_REFERENCE_TO 0x00000004u
#define DTF_BIT_VALUES 0x00000020u
#define DTF_TYPE_GUID 0x00000080u
#define DTF_COPY_MASK 0x00000200u
#define DTF_METHOD_INFOS 0x00000800u
#define DTF_ATTRIBUTES 0x00001000u
#define DTF_ENUM_INFOS 0x00002000u
#define DTF_PLC_POINTER 0x00800000u

/* drapeaux des symboles (ADS_SYMBOL_FLAGS, 16 bits) */
#define SYF_TYPE_GUID 0x0008u
#define SYF_ATTRIBUTES 0x1000u

/* types ADS élémentaires */
#define ADST_INT16 2
#define ADST_INT32 3
#define ADST_REAL32 4
#define ADST_REAL64 5
#define ADST_INT8 16
#define ADST_UINT8 17
#define ADST_UINT16 18
#define ADST_UINT32 19
#define ADST_INT64 20
#define ADST_UINT64 21
#define ADST_STRING 30
#define ADST_WSTRING 31
#define ADST_BIT 33

#define ADS_STATE_RUN 5

#define MAX_FRAME (64u * 1024u * 1024u)
#define REQ_TIMEOUT_MS 5000
#define UPLOAD_TIMEOUT_MS 20000
#define CONNECT_TIMEOUT_MS 3000
#define RETRY_MS 3000
#define MAX_ROOTS 64
#define MAX_DEPTH 40

/* ------------------------------------------------------------- petits outils binaires */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/** lecteur borné : toute lecture hors des limites positionne `bad` et renvoie 0 */
typedef struct {
    const uint8_t *p, *end;
    int bad;
} rd_t;

static int rd_has(rd_t *r, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = 1; return 0; }
    return 1;
}
static unsigned r_u8(rd_t *r) { if (!rd_has(r, 1)) return 0; return *r->p++; }
static unsigned r_u16(rd_t *r) { if (!rd_has(r, 2)) return 0; unsigned v = rd16(r->p); r->p += 2; return v; }
static uint32_t r_u32(rd_t *r) { if (!rd_has(r, 4)) return 0; uint32_t v = rd32(r->p); r->p += 4; return v; }
static void r_skip(rd_t *r, size_t n) { if (rd_has(r, n)) r->p += n; }

/** chaîne de `len` caractères suivie d'un nul, convertie en UTF-8 dans l'arène */
static const char *r_str(rd_t *r, size_t len, int utf8, arena_t *a)
{
    if (!rd_has(r, len + 1)) return "";
    const uint8_t *s = r->p;
    r->p += len + 1;
    size_t n = 0;
    while (n < len && s[n]) n++;
    if (utf8) {
        char *o = arena_alloc(a, n + 1);
        memcpy(o, s, n);
        return o;
    }
    sb_t b = {0};
    utf8_from_latin1(&b, s, n);
    const char *o = arena_strdup(a, sb_cstr(&b));
    sb_free(&b);
    return o;
}

/** "192.168.1.10.1.1" -> 6 octets ; 0 si valide */
static int parse_netid(const char *s, uint8_t out[6])
{
    if (!s) return -1;
    int k = 0;
    const char *p = s;
    while (k < 6) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) return -1;
        out[k++] = (uint8_t)v;
        if (k < 6) {
            if (*end != '.') return -1;
            p = end + 1;
        } else if (*end) return -1;
    }
    return 0;
}

/** message d'erreur ADS en clair (codes les plus fréquents) */
static const char *ads_errstr(uint32_t code)
{
    switch (code) {
    case 0x1: return "erreur interne";
    case 0x6: return "port ADS cible introuvable (runtime PLC absent ou arrêté, mauvais port ADS)";
    case 0x7: return "machine cible introuvable (route ADS absente ou AMS Net ID incorrect)";
    case 0x12: return "port déjà utilisé";
    case 0x701: return "service non supporté par l'automate";
    case 0x702: return "groupe d'index invalide";
    case 0x703: return "offset d'index invalide";
    case 0x704: return "lecture/écriture non autorisée";
    case 0x705: return "taille de paramètre incorrecte";
    case 0x706: return "valeur de paramètre invalide";
    case 0x707: return "automate pas prêt";
    case 0x708: return "automate occupé";
    case 0x70A: return "mémoire insuffisante sur l'automate";
    case 0x70B: return "paramètre invalide";
    case 0x70C: return "introuvable";
    case 0x710: return "symbole introuvable";
    case 0x711: return "version des symboles invalide (programme modifié)";
    case 0x712: return "automate dans un état invalide";
    case 0x713: return "mode de transmission non supporté";
    case 0x714: return "handle de notification invalide";
    case 0x719: return "accès refusé";
    case 0x71A: return "licence manquante";
    case 0x745: return "délai de réponse dépassé";
    case 0x746: return "délai de réponse dépassé (automate)";
    case 0x74C: return "port AMS fermé";
    case 0x750: return "aucun routage vers l'automate";
    default: return NULL;
    }
}

static void fmt_ads_err(char *err, size_t cap, const char *what, uint32_t code)
{
    const char *t = ads_errstr(code);
    if (t) snprintf(err, cap, "%s : %s (code ADS 0x%X)", what, t, (unsigned)code);
    else snprintf(err, cap, "%s : erreur ADS 0x%X", what, (unsigned)code);
}

/* ------------------------------------------------------------- table des types */

typedef struct dt_entry dt_entry;
struct dt_entry {
    const char *name, *type, *comment;
    uint32_t size, offset, adstype, flags;
    int ndims;
    array_dim *dims;
    int nsub;
    dt_entry *sub;
    int nattrs;
    attr_t *attrs;
    int nenums;
    enum_entry *enums;
    /* nœud déjà construit (types utilisés plusieurs fois), dans l'arène des racines */
    type_node *built;
    int building;
};

static int adst_signed(uint32_t t) { return t == ADST_INT8 || t == ADST_INT16 || t == ADST_INT32 || t == ADST_INT64; }

static double enum_value(const uint8_t *p, uint32_t size, uint32_t adstype)
{
    uint64_t u = 0;
    for (uint32_t i = 0; i < size && i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    if (adst_signed(adstype) && size > 0 && size < 8 && (u >> (8 * size - 1)) & 1) u |= ~(uint64_t)0 << (8 * size);
    return adst_signed(adstype) ? (double)(int64_t)u : (double)u;
}

static void parse_attrs(rd_t *r, int utf8, arena_t *a, attr_t **out, int *n)
{
    unsigned cnt = r_u16(r);
    if (r->bad || cnt == 0) return;
    attr_t *v = arena_alloc(a, cnt * sizeof *v);
    for (unsigned i = 0; i < cnt && !r->bad; i++) {
        unsigned nl = r_u8(r), vl = r_u8(r);
        v[i].key = r_str(r, nl, utf8, a);
        v[i].value = r_str(r, vl, utf8, a);
    }
    *out = v;
    *n = (int)cnt;
}

/** entrée de type (sans les 4 octets de longueur), voir parseAdsResponseDataType d'ads-client */
static int parse_dt(rd_t *r, int utf8, arena_t *a, dt_entry *e, int depth)
{
    memset(e, 0, sizeof *e);
    if (depth > MAX_DEPTH) return -1;
    r_u32(r); /* version */
    r_u32(r); /* hash */
    r_u32(r); /* hash du type de base */
    e->size = r_u32(r);
    e->offset = r_u32(r);
    e->adstype = r_u32(r);
    e->flags = r_u32(r);
    unsigned nl = r_u16(r), tl = r_u16(r), cl = r_u16(r);
    e->ndims = (int)r_u16(r);
    e->nsub = (int)r_u16(r);
    e->name = r_str(r, nl, utf8, a);
    e->type = r_str(r, tl, utf8, a);
    e->comment = r_str(r, cl, utf8, a);
    if (r->bad) return -1;
    if (e->ndims > 0) {
        e->dims = arena_alloc(a, (size_t)e->ndims * sizeof *e->dims);
        for (int i = 0; i < e->ndims; i++) {
            e->dims[i].start = (int)r_u32(r);
            e->dims[i].length = (int)r_u32(r);
        }
    }
    if (e->nsub > 0) {
        e->sub = arena_alloc(a, (size_t)e->nsub * sizeof *e->sub);
        for (int i = 0; i < e->nsub && !r->bad; i++) {
            uint32_t len = r_u32(r);
            if (len < 4 || !rd_has(r, len - 4)) { r->bad = 1; break; }
            rd_t sr = {r->p, r->p + len - 4, 0};
            if (parse_dt(&sr, utf8, a, &e->sub[i], depth + 1) != 0) return -1;
            r->p += len - 4;
        }
    }
    if (e->flags & DTF_TYPE_GUID) r_skip(r, 16);
    if (e->flags & DTF_COPY_MASK) r_skip(r, e->size);
    if (e->flags & DTF_METHOD_INFOS) {
        unsigned cnt = r_u16(r);
        for (unsigned i = 0; i < cnt && !r->bad; i++) {
            uint32_t len = r_u32(r); /* longueur de l'entrée, ces 4 octets compris */
            if (len < 4) { r->bad = 1; break; }
            r_skip(r, len - 4);
        }
    }
    if (e->flags & DTF_ATTRIBUTES) parse_attrs(r, utf8, a, &e->attrs, &e->nattrs);
    if (e->flags & DTF_ENUM_INFOS) {
        unsigned cnt = r_u16(r);
        if (cnt > 0 && !r->bad) {
            e->enums = arena_alloc(a, cnt * sizeof *e->enums);
            for (unsigned i = 0; i < cnt && !r->bad; i++) {
                unsigned l = r_u8(r);
                e->enums[i].name = r_str(r, l, utf8, a);
                if (rd_has(r, e->size)) {
                    e->enums[i].value = enum_value(r->p, e->size, e->adstype);
                    r->p += e->size;
                }
            }
            e->nenums = (int)cnt;
        }
    }
    /* suite éventuelle (drapeaux étendus, refactorisation…) ignorée : la longueur d'entrée fait foi */
    return r->bad ? -1 : 0;
}

/** table de hachage nom (insensible à la casse) -> entrée */
typedef struct {
    dt_entry *v;
    int n;
    int *slot; /* indice + 1, 0 = libre */
    int nslot;
} dt_table;

static dt_entry *dt_find(dt_table *t, const char *name)
{
    if (!t->nslot || !name || !*name) return NULL;
    uint32_t h = hash_ci(name, strlen(name));
    for (int k = 0; k < t->nslot; k++) {
        int s = t->slot[(h + (uint32_t)k) & (uint32_t)(t->nslot - 1)];
        if (!s) return NULL;
        if (str_ieq(t->v[s - 1].name, name)) return &t->v[s - 1];
    }
    return NULL;
}

static int dt_table_load(dt_table *t, const uint8_t *data, size_t len, int utf8, arena_t *a, char *err, size_t cap)
{
    /* comptage puis analyse */
    size_t pos = 0;
    int count = 0;
    while (pos + 4 <= len) {
        uint32_t l = rd32(data + pos);
        if (l < 4 || pos + l > len) break;
        pos += l;
        count++;
    }
    t->v = arena_alloc(a, (size_t)(count ? count : 1) * sizeof *t->v);
    t->nslot = 16;
    while (t->nslot < count * 2) t->nslot *= 2;
    t->slot = arena_alloc(a, (size_t)t->nslot * sizeof *t->slot);
    pos = 0;
    for (int i = 0; i < count; i++) {
        uint32_t l = rd32(data + pos);
        rd_t r = {data + pos + 4, data + pos + l, 0};
        pos += l;
        dt_entry *e = &t->v[t->n];
        if (parse_dt(&r, utf8, a, e, 0) != 0) {
            snprintf(err, cap, "table des types illisible (entrée %d)", i);
            return -1;
        }
        uint32_t h = hash_ci(e->name, strlen(e->name));
        for (int k = 0; k < t->nslot; k++) {
            int *s = &t->slot[(h + (uint32_t)k) & (uint32_t)(t->nslot - 1)];
            if (!*s) { *s = t->n + 1; break; }
            if (str_ieq(t->v[*s - 1].name, e->name)) break; /* doublon : premier conservé */
        }
        t->n++;
    }
    return 0;
}

/* ------------------------------------------------------------- construction des arbres de types */

typedef struct {
    dt_table *tab;
    arena_t *a; /* arène des racines (propriété du hub) */
    int skipped;
} builder;

static attr_t *copy_attrs(arena_t *a, const attr_t *src, int n)
{
    if (n <= 0) return NULL;
    attr_t *v = arena_alloc(a, (size_t)n * sizeof *v);
    for (int i = 0; i < n; i++) {
        v[i].key = arena_strdup(a, src[i].key);
        v[i].value = arena_strdup(a, src[i].value);
    }
    return v;
}

static int is_pointer_name(const char *n)
{
    return str_istarts(n, "POINTER TO ") || str_istarts(n, "REFERENCE TO ") || str_istarts(n, "PVOID") ||
           str_istarts(n, "XINT") || str_istarts(n, "UXINT") || str_istarts(n, "XWORD");
}

static type_node *new_leaf(builder *b, const char *type_name, int size, prim_t prim, leaf_kind kind)
{
    type_node *n = arena_alloc(b->a, sizeof *n);
    n->kind = TN_LEAF;
    n->type_name = arena_strdup(b->a, type_name);
    n->size = size;
    n->prim = prim;
    n->lkind = kind;
    return n;
}

/** type élémentaire d'après le nom, puis d'après le type ADS ; NULL si non représentable */
static type_node *prim_leaf(builder *b, const char *name, int size, uint32_t adstype)
{
    prim_t p;
    leaf_kind k;
    if (is_pointer_name(name)) return NULL;
    if (plc_prim(name, 0, &p, &k) == 0) return new_leaf(b, name, size, p, k);
    switch (adstype) {
    case ADST_BIT: p = PR_BOOL; k = LK_BOOL; break;
    case ADST_INT8: p = PR_I8; k = LK_NUMBER; break;
    case ADST_UINT8: p = size == 1 && str_istarts(name, "BOOL") ? PR_BOOL : PR_U8; k = p == PR_BOOL ? LK_BOOL : LK_NUMBER; break;
    case ADST_INT16: p = PR_I16; k = LK_NUMBER; break;
    case ADST_UINT16: p = PR_U16; k = LK_NUMBER; break;
    case ADST_INT32: p = PR_I32; k = LK_NUMBER; break;
    case ADST_UINT32: p = PR_U32; k = LK_NUMBER; break;
    case ADST_INT64: p = PR_I64; k = LK_NUMBER; break;
    case ADST_UINT64: p = PR_U64; k = LK_NUMBER; break;
    case ADST_REAL32: p = PR_F32; k = LK_NUMBER; break;
    case ADST_REAL64: p = PR_F64; k = LK_NUMBER; break;
    case ADST_STRING: p = PR_STRING; k = LK_STRING; break;
    case ADST_WSTRING: p = PR_WSTRING; k = LK_STRING; break;
    default: return NULL;
    }
    return new_leaf(b, name, size, p, k);
}

static type_node *resolve(builder *b, const char *name, int size, int depth);

static type_node *build_entry(builder *b, dt_entry *e, int depth)
{
    if (depth > MAX_DEPTH || e->building) return NULL;
    if (e->built) return e->built;
    if ((e->flags & (DTF_REFERENCE_TO | DTF_PLC_POINTER)) || is_pointer_name(e->name)) return NULL;
    e->building = 1;
    type_node *n = NULL;
    if (e->nsub > 0) {
        /* structure, bloc fonctionnel, union */
        n = arena_alloc(b->a, sizeof *n);
        n->kind = TN_STRUCT;
        n->type_name = arena_strdup(b->a, e->name);
        n->size = (int)e->size;
        n->members = arena_alloc(b->a, (size_t)e->nsub * sizeof *n->members);
        for (int i = 0; i < e->nsub; i++) {
            dt_entry *s = &e->sub[i];
            type_node *mn = NULL;
            if (s->nsub > 0 || s->ndims > 0) {
                /* membre décrit en ligne (rare) : construit tel quel */
                dt_entry tmp = *s;
                tmp.built = NULL;
                tmp.building = 0;
                if (!*tmp.name) tmp.name = s->type;
                mn = s->nsub > 0 ? build_entry(b, &tmp, depth + 1) : resolve(b, s->type, (int)s->size, depth + 1);
                if (!mn && s->ndims > 0) {
                    /* tableau anonyme : type de base d'après « ARRAY [..] OF X » */
                    const char *of = strstr(s->type, " OF ");
                    long total = 1;
                    for (int d = 0; d < s->ndims; d++) total *= s->dims[d].length > 0 ? s->dims[d].length : 1;
                    type_node *el = of ? resolve(b, of + 4, (int)(s->size / (uint32_t)total), depth + 1) : NULL;
                    if (el) {
                        mn = arena_alloc(b->a, sizeof *mn);
                        mn->kind = TN_ARRAY;
                        mn->type_name = arena_strdup(b->a, s->type);
                        mn->size = (int)s->size;
                        mn->ndims = s->ndims;
                        mn->dims = arena_alloc(b->a, (size_t)s->ndims * sizeof *mn->dims);
                        memcpy(mn->dims, s->dims, (size_t)s->ndims * sizeof *mn->dims);
                        mn->elem = el;
                    }
                }
            } else {
                mn = resolve(b, s->type, (int)s->size, depth + 1);
            }
            if (!mn) { b->skipped++; continue; }
            member_t *m = &n->members[n->nmembers++];
            m->name = arena_strdup(b->a, s->name);
            m->node = mn;
            m->comment = *s->comment ? arena_strdup(b->a, s->comment) : NULL;
            m->attrs = copy_attrs(b->a, s->attrs, s->nattrs);
            m->nattrs = s->nattrs;
            /* membre BIT : drapeau sur le membre ou sur la déclaration du type BIT, ou type ADS BIT */
            dt_entry *te = dt_find(b->tab, s->type);
            if ((s->flags & DTF_BIT_VALUES) || s->adstype == ADST_BIT || (te && (te->flags & DTF_BIT_VALUES))) {
                /* offset en bits depuis le début de la structure */
                m->bit_offset = (int)s->offset;
                m->offset = (int)(s->offset / 8);
            } else {
                m->bit_offset = -1;
                m->offset = (int)s->offset;
            }
        }
    } else if (e->ndims > 0) {
        long total = 1;
        for (int d = 0; d < e->ndims; d++) total *= e->dims[d].length > 0 ? e->dims[d].length : 1;
        type_node *el = resolve(b, e->type, (int)(e->size / (uint32_t)total), depth + 1);
        if (el) {
            n = arena_alloc(b->a, sizeof *n);
            n->kind = TN_ARRAY;
            n->type_name = arena_strdup(b->a, e->name);
            n->size = (int)e->size;
            n->ndims = e->ndims;
            n->dims = arena_alloc(b->a, (size_t)e->ndims * sizeof *n->dims);
            memcpy(n->dims, e->dims, (size_t)e->ndims * sizeof *n->dims);
            n->elem = el;
        }
    } else if (e->nenums > 0) {
        /* énumération : représentation du type de base, valeurs nommées */
        n = prim_leaf(b, *e->type ? e->type : e->name, (int)e->size, e->adstype);
        if (!n) n = prim_leaf(b, "", (int)e->size, e->adstype);
        if (n) {
            n->type_name = arena_strdup(b->a, e->name);
            n->lkind = LK_ENUM;
            enum_entry *ev = arena_alloc(b->a, (size_t)e->nenums * sizeof *ev);
            for (int i = 0; i < e->nenums; i++) {
                ev[i].name = arena_strdup(b->a, e->enums[i].name);
                ev[i].value = e->enums[i].value;
            }
            n->enums = ev;
            n->nenums = e->nenums;
        }
    } else {
        prim_t p;
        leaf_kind k;
        int base = plc_prim(e->name, 0, &p, &k) == 0;
        dt_entry *alias = (!base && *e->type) ? dt_find(b->tab, e->type) : NULL;
        if (alias && alias != e) {
            /* alias (TYPE T_Pression : REAL) : type de base, nom de l'alias conservé */
            type_node *bn = build_entry(b, alias, depth + 1);
            if (bn) {
                n = arena_alloc(b->a, sizeof *n);
                *n = *bn;
                n->type_name = arena_strdup(b->a, e->name);
            }
        } else if (!base && *e->type) {
            n = resolve(b, e->type, (int)e->size, depth + 1);
            if (n && n->kind == TN_LEAF) {
                type_node *c = arena_alloc(b->a, sizeof *c);
                *c = *n;
                c->type_name = arena_strdup(b->a, e->name);
                n = c;
            }
        } else {
            n = prim_leaf(b, e->name, (int)e->size, e->adstype);
        }
    }
    if (n && e->nattrs > 0 && n->nattrs == 0) {
        n->attrs = copy_attrs(b->a, e->attrs, e->nattrs);
        n->nattrs = e->nattrs;
    }
    e->building = 0;
    e->built = n;
    return n;
}

static type_node *resolve(builder *b, const char *name, int size, int depth)
{
    if (!name || !*name || depth > MAX_DEPTH) return NULL;
    dt_entry *e = dt_find(b->tab, name);
    if (e) return build_entry(b, e, depth);
    /* types de base absents de la table (TwinCAT 2, anciennes versions) */
    prim_t p;
    leaf_kind k;
    if (is_pointer_name(name)) return NULL;
    if (plc_prim(name, size, &p, &k) == 0) {
        int sz = size;
        if (sz <= 0) {
            switch (p) {
            case PR_BOOL: case PR_BIT: case PR_I8: case PR_U8: sz = 1; break;
            case PR_I16: case PR_U16: sz = 2; break;
            case PR_I32: case PR_U32: case PR_F32: sz = 4; break;
            case PR_I64: case PR_U64: case PR_F64: sz = 8; break;
            default: sz = 81; break;
            }
        }
        return new_leaf(b, name, sz, p, k);
    }
    return NULL;
}

/* ------------------------------------------------------------- état de la source */

typedef struct {
    char path[256];
    uint32_t ig, io, size;
} root_addr;

typedef struct {
    source_t base;
    hub_t *hub;
    /* configuration */
    uint8_t target[6], local[6];
    int target_port, local_port;
    int router_mode;
    char host[128];
    int tcp_port;
    char **symbols;
    int nsymbols;
    int cycle_ms;
    int on_change;

    plat_thread thread;
    plat_flag running;
    plat_flag connected;
    int started;

    /* connexion (thread de la source uniquement) */
    sock_t fd;
    uint8_t *rbuf;
    size_t rlen, rcap, rconsumed;
    uint32_t invoke;
    int registered;
    int utf8;
    root_addr roots[MAX_ROOTS];
    int nroots;
    uint32_t notif[MAX_ROOTS];
    uint32_t symver_notif;
    int symver_known;
    unsigned symver;
    int reload;
    int plc_run;

    /* écriture demandée par la boucle principale */
    plat_mutex wlock;
    int wstate; /* 0 libre, 1 demandée, 2 en cours, 3 terminée */
    int wabandoned;
    int wroot, woffset, wsize, wbit;
    prim_t wprim;
    char wpath[512];
    uint8_t wdata[4096];
    int wlen;
    int wresult;
    char werr[256];
} ads_t;

/* ------------------------------------------------------------- trames */

static int send_all(ads_t *s, const uint8_t *p, size_t n)
{
    double t0 = mono_ms();
    while (n > 0) {
        int k = (int)send(s->fd, (const char *)p, (int)(n > 65536 ? 65536 : n), 0);
        if (k > 0) { p += k; n -= (size_t)k; continue; }
        if (k < 0 && sock_would_block()) {
            if (mono_ms() - t0 > REQ_TIMEOUT_MS) return -1;
            struct pollfd pf;
            pf.fd = s->fd;
            pf.events = POLLOUT;
            pf.revents = 0;
            poll(&pf, 1, 50);
            continue;
        }
        return -1;
    }
    return 0;
}

/** trame AMS complète : en-tête TCP (6) + en-tête AMS (32) + données */
static int send_ams(ads_t *s, uint16_t cmd, const uint8_t *data, uint32_t len, uint32_t invoke)
{
    uint8_t *f = xmalloc(38 + (size_t)len);
    wr16(f, AMS_TCP_PORT_AMS_CMD);
    wr32(f + 2, 32 + len);
    memcpy(f + 6, s->target, 6);
    wr16(f + 12, (uint32_t)s->target_port);
    memcpy(f + 14, s->local, 6);
    wr16(f + 20, (uint32_t)s->local_port);
    wr16(f + 22, cmd);
    wr16(f + 24, STATEFLAG_REQUEST);
    wr32(f + 26, len);
    wr32(f + 30, 0);
    wr32(f + 34, invoke);
    if (len) memcpy(f + 38, data, len);
    int rc = send_all(s, f, 38 + (size_t)len);
    free(f);
    return rc;
}

typedef struct {
    unsigned tcp_cmd;
    unsigned cmd, flags;
    uint32_t error, invoke;
    const uint8_t *data;
    uint32_t len;
} frame_t;

/**
 * Attend une trame complète pendant au plus timeout_ms. 1 si trame (données valides jusqu'à l'appel
 * suivant), 0 si délai écoulé (ou arrêt demandé), -1 si connexion perdue ou trame invalide.
 */
static int recv_frame(ads_t *s, int timeout_ms, frame_t *fr, char *err, size_t cap)
{
    static const size_t HDR = 6;
    /* retire la trame rendue à l'appel précédent */
    if (s->rconsumed) {
        memmove(s->rbuf, s->rbuf + s->rconsumed, s->rlen - s->rconsumed);
        s->rlen -= s->rconsumed;
        s->rconsumed = 0;
    }
    double t0 = mono_ms();
    for (;;) {
        if (s->rlen >= HDR) {
            uint32_t plen = rd32(s->rbuf + 2);
            if (plen > MAX_FRAME) { snprintf(err, cap, "trame AMS trop grande (%u octets)", (unsigned)plen); return -1; }
            if (s->rlen >= HDR + plen) {
                memset(fr, 0, sizeof *fr);
                fr->tcp_cmd = rd16(s->rbuf);
                if (fr->tcp_cmd == AMS_TCP_PORT_AMS_CMD) {
                    if (plen < 32) { snprintf(err, cap, "trame AMS tronquée"); return -1; }
                    const uint8_t *h = s->rbuf + HDR;
                    fr->cmd = rd16(h + 16);
                    fr->flags = rd16(h + 18);
                    fr->len = rd32(h + 20);
                    fr->error = rd32(h + 24);
                    fr->invoke = rd32(h + 28);
                    if (fr->len > plen - 32) fr->len = plen - 32;
                    fr->data = h + 32;
                } else {
                    fr->data = s->rbuf + HDR;
                    fr->len = plen;
                }
                s->rconsumed = HDR + plen;
                return 1;
            }
        }
        int left = timeout_ms - (int)(mono_ms() - t0);
        if (left <= 0 || !pf_get(&s->running)) return 0;
        struct pollfd pf;
        pf.fd = s->fd;
        pf.events = POLLIN;
        pf.revents = 0;
        int pr = poll(&pf, 1, left < 50 ? left : 50);
        if (pr < 0) { snprintf(err, cap, "erreur réseau (%s)", plat_strerror()); return -1; }
        if (pr == 0) continue;
        if (s->rcap - s->rlen < 65536) {
            s->rcap = s->rcap ? s->rcap * 2 : 262144;
            while (s->rcap - s->rlen < 65536) s->rcap *= 2;
            s->rbuf = xrealloc(s->rbuf, s->rcap);
        }
        int k = (int)recv(s->fd, (char *)s->rbuf + s->rlen, (int)(s->rcap - s->rlen), 0);
        if (k == 0) { snprintf(err, cap, "connexion fermée par l'automate ou le routeur"); return -1; }
        if (k < 0) {
            if (sock_would_block()) continue;
            snprintf(err, cap, "connexion perdue (%s)", plat_strerror());
            return -1;
        }
        s->rlen += (size_t)k;
    }
}

/* ------------------------------------------------------------- requêtes */

#define NET_FATAL (-2)

static void handle_notif(ads_t *s, const uint8_t *d, uint32_t len)
{
    rd_t r = {d, d + len, 0};
    r_u32(&r); /* longueur */
    uint32_t stamps = r_u32(&r);
    for (uint32_t i = 0; i < stamps && !r.bad; i++) {
        r_skip(&r, 8); /* horodatage FILETIME */
        uint32_t samples = r_u32(&r);
        for (uint32_t k = 0; k < samples && !r.bad; k++) {
            uint32_t h = r_u32(&r), size = r_u32(&r);
            if (!rd_has(&r, size)) break;
            const uint8_t *p = r.p;
            r.p += size;
            if (s->symver_notif && h == s->symver_notif && size >= 1) {
                if (!s->symver_known) { s->symver_known = 1; s->symver = p[0]; }
                else if (p[0] != s->symver) s->reload = 1;
                continue;
            }
            for (int j = 0; j < s->nroots; j++)
                if (s->notif[j] == h) { hub_source_data(s->hub, j, p, size); break; }
        }
    }
}

/** traite une trame reçue hors requête ; 0, ou NET_FATAL */
static int dispatch(ads_t *s, const frame_t *fr)
{
    if (fr->tcp_cmd == AMS_TCP_PORT_AMS_CMD && fr->cmd == ADSCMD_NOTIF && !(fr->flags & 1)) handle_notif(s, fr->data, fr->len);
    return 0;
}

/** attend au plus timeout_ms une trame et la traite ; 0 ou NET_FATAL */
static int pump(ads_t *s, int timeout_ms, char *err, size_t cap)
{
    frame_t fr;
    int rc = recv_frame(s, timeout_ms, &fr, err, cap);
    if (rc < 0) return NET_FATAL;
    if (rc > 0) dispatch(s, &fr);
    return 0;
}

/**
 * Requête ADS : réponse (après le code de résultat ADS) copiée dans out.
 * 0 si succès, -1 si erreur ADS (message dans err), NET_FATAL si connexion perdue ou sans réponse.
 */
static int request(ads_t *s, uint16_t cmd, const uint8_t *data, uint32_t len, int timeout_ms, const char *what,
                   sb_t *out, char *err, size_t cap)
{
    uint32_t inv = ++s->invoke;
    if (send_ams(s, cmd, data, len, inv) != 0) { snprintf(err, cap, "%s : envoi impossible (connexion perdue)", what); return NET_FATAL; }
    double t0 = mono_ms();
    for (;;) {
        int left = timeout_ms - (int)(mono_ms() - t0);
        if (left <= 0) { snprintf(err, cap, "%s : pas de réponse en %d ms", what, timeout_ms); return NET_FATAL; }
        frame_t fr;
        int rc = recv_frame(s, left, &fr, err, cap);
        if (rc < 0) return NET_FATAL;
        if (rc == 0) {
            if (!pf_get(&s->running)) { snprintf(err, cap, "arrêt demandé"); return NET_FATAL; }
            continue;
        }
        if (fr.tcp_cmd != AMS_TCP_PORT_AMS_CMD || !(fr.flags & 1) || fr.invoke != inv || fr.cmd != cmd) {
            dispatch(s, &fr);
            continue;
        }
        if (fr.error) { fmt_ads_err(err, cap, what, fr.error); return fr.error == 0x6 || fr.error == 0x7 ? NET_FATAL : -1; }
        if (fr.len < 4) { snprintf(err, cap, "%s : réponse tronquée", what); return -1; }
        uint32_t res = rd32(fr.data);
        if (res) { fmt_ads_err(err, cap, what, res); return -1; }
        if (out) { sb_clear(out); sb_put(out, fr.data + 4, fr.len - 4); }
        return 0;
    }
}

static int ads_read(ads_t *s, uint32_t ig, uint32_t io, uint32_t n, int tmo, const char *what, sb_t *out, char *err, size_t cap)
{
    uint8_t q[12];
    wr32(q, ig); wr32(q + 4, io); wr32(q + 8, n);
    int rc = request(s, ADSCMD_READ, q, 12, tmo, what, out, err, cap);
    if (rc) return rc;
    if (out->len < 4) { snprintf(err, cap, "%s : réponse tronquée", what); return -1; }
    uint32_t l = rd32((uint8_t *)out->p);
    if (l > out->len - 4) l = (uint32_t)out->len - 4;
    memmove(out->p, out->p + 4, l);
    out->len = l;
    return 0;
}

static int ads_rw(ads_t *s, uint32_t ig, uint32_t io, uint32_t rlen, const void *w, uint32_t wlen, const char *what,
                  sb_t *out, char *err, size_t cap)
{
    uint8_t *q = xmalloc(16 + (size_t)wlen);
    wr32(q, ig); wr32(q + 4, io); wr32(q + 8, rlen); wr32(q + 12, wlen);
    memcpy(q + 16, w, wlen);
    int rc = request(s, ADSCMD_READ_WRITE, q, 16 + wlen, REQ_TIMEOUT_MS, what, out, err, cap);
    free(q);
    if (rc) return rc;
    if (out->len < 4) { snprintf(err, cap, "%s : réponse tronquée", what); return -1; }
    uint32_t l = rd32((uint8_t *)out->p);
    if (l > out->len - 4) l = (uint32_t)out->len - 4;
    memmove(out->p, out->p + 4, l);
    out->len = l;
    return 0;
}

static int ads_write(ads_t *s, uint32_t ig, uint32_t io, const void *d, uint32_t n, const char *what, char *err, size_t cap)
{
    uint8_t *q = xmalloc(12 + (size_t)n);
    wr32(q, ig); wr32(q + 4, io); wr32(q + 8, n);
    memcpy(q + 12, d, n);
    int rc = request(s, ADSCMD_WRITE, q, 12 + n, REQ_TIMEOUT_MS, what, NULL, err, cap);
    free(q);
    return rc;
}

static int ads_add_notif(ads_t *s, uint32_t ig, uint32_t io, uint32_t n, uint32_t mode, uint32_t cycle_100ns,
                         uint32_t *handle, const char *what, char *err, size_t cap)
{
    uint8_t q[40] = {0};
    wr32(q, ig); wr32(q + 4, io); wr32(q + 8, n); wr32(q + 12, mode);
    wr32(q + 16, 0); /* délai maximal */
    wr32(q + 20, cycle_100ns);
    sb_t out = {0};
    int rc = request(s, ADSCMD_ADD_NOTIF, q, 40, REQ_TIMEOUT_MS, what, &out, err, cap);
    if (rc == 0 && out.len >= 4) *handle = rd32((uint8_t *)out.p);
    else if (rc == 0) { snprintf(err, cap, "%s : réponse tronquée", what); rc = -1; }
    sb_free(&out);
    return rc;
}

static const char *ads_state_name(unsigned st)
{
    static const char *n[] = {"INVALID", "IDLE", "RESET", "INIT", "START", "RUN", "STOP", "SAVECFG", "LOADCFG",
                              "POWERFAILURE", "POWERGOOD", "ERROR", "SHUTDOWN", "SUSPEND", "RESUME", "CONFIG", "RECONFIG"};
    return st < sizeof n / sizeof n[0] ? n[st] : "?";
}

static int ads_read_state(ads_t *s, unsigned *state, char *err, size_t cap)
{
    sb_t out = {0};
    int rc = request(s, ADSCMD_READ_STATE, NULL, 0, REQ_TIMEOUT_MS, "lecture de l'état", &out, err, cap);
    if (rc == 0) *state = out.len >= 2 ? rd16((uint8_t *)out.p) : 0;
    sb_free(&out);
    return rc;
}

/* ------------------------------------------------------------- session */

/** enregistrement d'un port AMS auprès du routeur TwinCAT (mode « router ») */
static int register_port(ads_t *s, char *err, size_t cap)
{
    uint8_t q[8] = {0};
    wr16(q, AMS_TCP_PORT_CONNECT);
    wr32(q + 2, 2);
    if (send_all(s, q, 8) != 0) { snprintf(err, cap, "routeur : envoi impossible"); return NET_FATAL; }
    double t0 = mono_ms();
    while (mono_ms() - t0 < REQ_TIMEOUT_MS) {
        frame_t fr;
        int rc = recv_frame(s, 200, &fr, err, cap);
        if (rc < 0) return NET_FATAL;
        if (rc > 0 && fr.tcp_cmd == AMS_TCP_PORT_CONNECT && fr.len >= 8) {
            memcpy(s->local, fr.data, 6);
            s->local_port = rd16(fr.data + 6);
            s->registered = 1;
            return 0;
        }
        if (!pf_get(&s->running)) break;
    }
    snprintf(err, cap, "routeur TwinCAT %s:%d : pas de réponse à l'enregistrement du port", s->host, s->tcp_port);
    return NET_FATAL;
}

/** nœud du symbole racine d'après son type (tableau anonyme compris) */
static type_node *symbol_node(builder *b, const char *type, uint32_t size, int ndims, const array_dim *dims)
{
    type_node *n = resolve(b, type, (int)size, 0);
    if (n || ndims <= 0) return n;
    const char *of = strstr(type, " OF ");
    if (!of) return NULL;
    long total = 1;
    for (int d = 0; d < ndims; d++) total *= dims[d].length > 0 ? dims[d].length : 1;
    type_node *el = resolve(b, of + 4, (int)(size / (uint32_t)total), 0);
    if (!el) return NULL;
    n = arena_alloc(b->a, sizeof *n);
    n->kind = TN_ARRAY;
    n->type_name = arena_strdup(b->a, type);
    n->size = (int)size;
    n->ndims = ndims;
    n->dims = arena_alloc(b->a, (size_t)ndims * sizeof *n->dims);
    memcpy(n->dims, dims, (size_t)ndims * sizeof *n->dims);
    n->elem = el;
    return n;
}

/** charge types et symboles, transmet le schéma au hub ; 0, -1 (erreur ADS) ou NET_FATAL */
static int load_symbols(ads_t *s, char *err, size_t cap)
{
    sb_t out = {0};
    int rc = ads_read(s, IG_SYM_UPLOAD_INFO, 0, 64, REQ_TIMEOUT_MS, "informations de symboles", &out, err, cap);
    if (rc) { sb_free(&out); return rc; }
    uint32_t dtlen = out.len >= 16 ? rd32((uint8_t *)out.p + 12) : 0;
    s->utf8 = out.len >= 32 && rd32((uint8_t *)out.p + 28) == 65001;

    arena_t *tmp = arena_new();
    dt_table tab;
    memset(&tab, 0, sizeof tab);
    if (dtlen > 0) {
        rc = ads_read(s, IG_SYM_DT_UPLOAD, 0, dtlen, UPLOAD_TIMEOUT_MS, "table des types", &out, err, cap);
        if (rc == 0 && dt_table_load(&tab, (uint8_t *)out.p, out.len, s->utf8, tmp, err, cap) != 0) rc = -1;
    }
    roots_t *roots = roots_new();
    builder b = {&tab, roots->arena, 0};
    s->nroots = 0;
    for (int i = 0; rc == 0 && i < s->nsymbols; i++) {
        const char *name = s->symbols[i];
        char what[300];
        snprintf(what, sizeof what, "symbole %s", name);
        rc = ads_rw(s, IG_SYM_INFO_BY_NAME_EX, 0, 0xFFFF, name, (uint32_t)strlen(name), what, &out, err, cap);
        if (rc) break;
        rd_t r = {(uint8_t *)out.p, (uint8_t *)out.p + out.len, 0};
        r_u32(&r); /* longueur de l'entrée */
        uint32_t ig = r_u32(&r), io = r_u32(&r), size = r_u32(&r);
        r_u32(&r); /* type ADS */
        r_u16(&r); /* drapeaux */
        int ndims = (int)r_u16(&r);
        unsigned nl = r_u16(&r), tl = r_u16(&r), cl = r_u16(&r);
        const char *sname = r_str(&r, nl, s->utf8, tmp);
        const char *stype = r_str(&r, tl, s->utf8, tmp);
        r_str(&r, cl, s->utf8, tmp);
        array_dim dims[16];
        for (int d = 0; d < ndims; d++) {
            int st = (int)r_u32(&r), ln = (int)r_u32(&r);
            if (d < 16) { dims[d].start = st; dims[d].length = ln; }
        }
        if (r.bad) { snprintf(err, cap, "%s : description illisible", what); rc = -1; break; }
        type_node *n = symbol_node(&b, stype, size, ndims < 16 ? ndims : 16, dims);
        if (!n) { snprintf(err, cap, "%s : type %s non supporté", what, stype); rc = -1; break; }
        if (s->nroots >= MAX_ROOTS) { snprintf(err, cap, "trop de symboles (%d au plus)", MAX_ROOTS); rc = -1; break; }
        const char *path = *sname ? sname : name;
        roots_add(roots, path, n);
        root_addr *ra = &s->roots[s->nroots++];
        str_copy(ra->path, sizeof ra->path, path);
        ra->ig = ig;
        ra->io = io;
        ra->size = size;
    }
    sb_free(&out);
    arena_free(tmp);
    if (rc) { roots_free(roots); s->nroots = 0; return rc; }
    if (b.skipped) log_info("ADS : %d membre(s) ignoré(s) (pointeurs, références, interfaces)", b.skipped);
    hub_source_roots(s->hub, roots);
    return 0;
}

static void set_status(ads_t *s, conn_state st, const char *fmt, ...) HMI_PRINTF(3, 4);
static void set_status(ads_t *s, conn_state st, const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    hub_source_status(s->hub, st, m);
}

static void netid_str(const uint8_t *n, char out[32])
{
    snprintf(out, 32, "%u.%u.%u.%u.%u.%u", n[0], n[1], n[2], n[3], n[4], n[5]);
}

/** exécute l'écriture demandée par la boucle principale, s'il y en a une ; 0 ou NET_FATAL */
static int do_pending_write(ads_t *s)
{
    pm_lock(&s->wlock);
    if (s->wstate != 1) { pm_unlock(&s->wlock); return 0; }
    s->wstate = 2;
    pm_unlock(&s->wlock);

    char err[256] = "";
    int rc;
    if (s->wroot < 0 || s->wroot >= s->nroots) {
        snprintf(err, sizeof err, "Variable d'un ancien schéma : %s", s->wpath);
        rc = -1;
    } else if (s->wprim == PR_BIT) {
        /* BIT : écriture par handle (l'automate ne modifie que ce bit) */
        sb_t out = {0};
        rc = ads_rw(s, IG_SYM_HANDLE_BY_NAME, 0, 4, s->wpath, (uint32_t)strlen(s->wpath), "handle", &out, err, sizeof err);
        if (rc == 0 && out.len >= 4) {
            uint32_t h = rd32((uint8_t *)out.p);
            rc = ads_write(s, IG_SYM_VALUE_BY_HANDLE, h, s->wdata, (uint32_t)s->wlen, "écriture", err, sizeof err);
            uint8_t hb[4];
            wr32(hb, h);
            char e2[128];
            int r2 = ads_write(s, IG_SYM_RELEASE_HANDLE, 0, hb, 4, "libération du handle", e2, sizeof e2);
            if (rc == 0 && r2 == NET_FATAL) rc = r2;
        } else if (rc == 0) rc = -1;
        sb_free(&out);
    } else {
        const root_addr *ra = &s->roots[s->wroot];
        rc = ads_write(s, ra->ig, ra->io + (uint32_t)s->woffset, s->wdata, (uint32_t)s->wlen, "écriture", err, sizeof err);
    }

    pm_lock(&s->wlock);
    if (s->wabandoned) {
        s->wabandoned = 0;
        s->wstate = 0;
    } else {
        s->wresult = rc == 0 ? 0 : -1;
        str_copy(s->werr, sizeof s->werr, err);
        s->wstate = 3;
    }
    pm_unlock(&s->wlock);
    return rc == NET_FATAL ? NET_FATAL : 0;
}

/** une session complète, de la connexion à l'erreur (ou à l'arrêt, ou au rechargement) */
static void session(ads_t *s, char *err, size_t cap)
{
    char tid[32], lid[32];
    netid_str(s->target, tid);
    set_status(s, ST_CONNECTING, "Connexion à %s (%s:%d)…", s->host, tid, s->target_port);
    s->fd = tcp_connect(s->host, s->tcp_port, CONNECT_TIMEOUT_MS, err, cap);
    if (s->fd == SOCK_INVALID) return;
    if (s->router_mode && register_port(s, err, cap) != 0) return;
    netid_str(s->local, lid);

    unsigned state = 0;
    if (ads_read_state(s, &state, err, cap) != 0) return;
    s->plc_run = state == ADS_STATE_RUN;
    set_status(s, ST_CONNECTING, "Lecture des symboles de %s:%d…", tid, s->target_port);
    if (load_symbols(s, err, cap) != 0) return;

    /* version des symboles : change à chaque téléchargement de programme */
    if (ads_add_notif(s, IG_SYM_VERSION, 0, 1, ADSTRANS_SERVERONCHA, 1000000, &s->symver_notif,
                      "surveillance de la version des symboles", err, cap) != 0) {
        log_info("ADS : %s (rechargement automatique indisponible)", err);
        s->symver_notif = 0;
    }
    uint32_t cycle = (uint32_t)s->cycle_ms * 10000u;
    for (int i = 0; i < s->nroots; i++) {
        char what[300];
        snprintf(what, sizeof what, "abonnement à %s", s->roots[i].path);
        if (ads_add_notif(s, s->roots[i].ig, s->roots[i].io, s->roots[i].size,
                          s->on_change ? ADSTRANS_SERVERONCHA : ADSTRANS_SERVERCYCLE, cycle, &s->notif[i], what, err, cap) != 0)
            return;
    }
    log_info("ADS : connecté à %s:%d depuis %s:%d, %d symbole(s), %d variables, automate en %s", tid, s->target_port,
             lid, s->local_port, s->nroots, hub_leaf_count(s->hub), ads_state_name(state));
    if (s->plc_run) set_status(s, ST_CONNECTED, "Connecté à %s:%d (%d symbole(s))", tid, s->target_port, s->nroots);
    else set_status(s, ST_ERROR, "Connecté, automate en %s : valeurs figées", ads_state_name(state));
    pf_set(&s->connected, 1);

    double last_state = mono_ms();
    while (pf_get(&s->running) && !s->reload) {
        if (do_pending_write(s) == NET_FATAL) { snprintf(err, cap, "connexion perdue pendant une écriture"); return; }
        if (mono_ms() - last_state >= 1000) {
            last_state = mono_ms();
            if (ads_read_state(s, &state, err, cap) == NET_FATAL) return;
            int run = state == ADS_STATE_RUN;
            if (run != s->plc_run) {
                s->plc_run = run;
                log_info("ADS : automate en %s", ads_state_name(state));
                if (run) set_status(s, ST_CONNECTED, "Connecté à %s:%d (%d symbole(s))", tid, s->target_port, s->nroots);
                else set_status(s, ST_ERROR, "Connecté, automate en %s : valeurs figées", ads_state_name(state));
            }
        }
        if (pump(s, 5, err, cap) == NET_FATAL) return;
    }
}

/** fin de session : désabonnements (au mieux), libération du port, fermeture */
static void session_close(ads_t *s, int clean)
{
    if (s->fd != SOCK_INVALID && clean) {
        char e[256];
        for (int i = 0; i < s->nroots; i++)
            if (s->notif[i]) {
                uint8_t q[4];
                wr32(q, s->notif[i]);
                if (request(s, ADSCMD_DEL_NOTIF, q, 4, 500, "désabonnement", NULL, e, sizeof e) == NET_FATAL) break;
            }
        if (s->symver_notif) {
            uint8_t q[4];
            wr32(q, s->symver_notif);
            request(s, ADSCMD_DEL_NOTIF, q, 4, 500, "désabonnement", NULL, e, sizeof e);
        }
        if (s->registered) {
            uint8_t q[8];
            wr16(q, AMS_TCP_PORT_CLOSE);
            wr32(q + 2, 2);
            wr16(q + 6, (uint32_t)s->local_port);
            send_all(s, q, 8);
        }
    }
    if (s->fd != SOCK_INVALID) sock_close(s->fd);
    s->fd = SOCK_INVALID;
    s->rlen = s->rconsumed = 0;
    s->registered = 0;
    memset(s->notif, 0, sizeof s->notif);
    s->symver_notif = 0;
    s->symver_known = 0;
    s->nroots = 0;
    /* une écriture en attente échoue immédiatement */
    pm_lock(&s->wlock);
    if (s->wstate == 1) {
        s->wresult = -1;
        str_copy(s->werr, sizeof s->werr, "Automate non connecté");
        s->wstate = 3;
    }
    pm_unlock(&s->wlock);
}

static void ads_thread(void *arg)
{
    ads_t *s = arg;
    char last_err[400] = "";
    while (pf_get(&s->running)) {
        char err[400] = "";
        s->reload = 0;
        session(s, err, sizeof err);
        int reload = s->reload;
        pf_set(&s->connected, 0);
        session_close(s, reload || !pf_get(&s->running) || !err[0]);
        if (!pf_get(&s->running)) break;
        if (reload) {
            log_info("ADS : nouveau programme sur l'automate, rechargement des symboles");
            hub_source_status(s->hub, ST_CONNECTING, "Nouveau programme sur l'automate : rechargement des symboles…");
            sleep_ms(500);
            last_err[0] = 0;
            continue;
        }
        if (strcmp(err, last_err) != 0) {
            log_error("ADS : %s", err);
            str_copy(last_err, sizeof last_err, err);
        }
        set_status(s, ST_ERROR, "%s (nouvelle tentative dans %d s)", err, RETRY_MS / 1000);
        for (int t = 0; t < RETRY_MS && pf_get(&s->running); t += 50) sleep_ms(50);
    }
}

/* ------------------------------------------------------------- interface source */

static int ads_start(source_t *base, char *err, size_t cap)
{
    ads_t *s = (ads_t *)base;
    if (s->nsymbols == 0) { snprintf(err, cap, "ADS : aucun symbole configuré"); return -1; }
    pf_set(&s->running, 1);
    if (pt_start(&s->thread, ads_thread, s) != 0) {
        pf_set(&s->running, 0);
        snprintf(err, cap, "ADS : impossible de démarrer le thread");
        return -1;
    }
    s->started = 1;
    return 0;
}

static void ads_stop(source_t *base)
{
    ads_t *s = (ads_t *)base;
    if (!s->started) return;
    pf_set(&s->running, 0);
    pt_join(s->thread);
    s->started = 0;
}

static int ads_src_write(source_t *base, const leaf_t *leaf, const uint8_t *data, int len, char *err, size_t cap)
{
    ads_t *s = (ads_t *)base;
    if (!pf_get(&s->connected)) { snprintf(err, cap, "Automate non connecté"); return -1; }
    if (len <= 0 || len > (int)sizeof s->wdata) { snprintf(err, cap, "Valeur trop grande pour %s", leaf->path); return -1; }
    pm_lock(&s->wlock);
    if (s->wstate != 0) {
        pm_unlock(&s->wlock);
        snprintf(err, cap, "Écriture précédente encore en cours");
        return -1;
    }
    s->wroot = leaf->root;
    s->woffset = leaf->offset;
    s->wsize = leaf->size;
    s->wbit = leaf->bit;
    s->wprim = leaf->prim;
    str_copy(s->wpath, sizeof s->wpath, leaf->path);
    memcpy(s->wdata, data, (size_t)len);
    s->wlen = len;
    s->wstate = 1;
    pm_unlock(&s->wlock);

    double t0 = mono_ms();
    while (mono_ms() - t0 < REQ_TIMEOUT_MS + 500) {
        sleep_ms(1);
        pm_lock(&s->wlock);
        if (s->wstate == 3) {
            int rc = s->wresult;
            if (rc) snprintf(err, cap, "%s", s->werr);
            s->wstate = 0;
            pm_unlock(&s->wlock);
            return rc;
        }
        pm_unlock(&s->wlock);
    }
    pm_lock(&s->wlock);
    if (s->wstate == 1) s->wstate = 0; /* jamais prise en charge : annulée */
    else if (s->wstate == 2) s->wabandoned = 1;
    else s->wstate = 0;
    pm_unlock(&s->wlock);
    snprintf(err, cap, "Écriture de %s : pas de réponse de l'automate", leaf->path);
    return -1;
}

static void ads_destroy(source_t *base)
{
    ads_t *s = (ads_t *)base;
    ads_stop(base);
    for (int i = 0; i < s->nsymbols; i++) free(s->symbols[i]);
    free(s->symbols);
    free(s->rbuf);
    pm_destroy(&s->wlock);
    free(s);
}

static const cJSON *jget(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

source_t *ads_new(hub_t *h, const cJSON *cfg, char *err, size_t cap)
{
    ads_t *s = xcalloc(1, sizeof *s);
    s->base.start = ads_start;
    s->base.stop = ads_stop;
    s->base.write = ads_src_write;
    s->base.destroy = ads_destroy;
    s->hub = h;
    s->fd = SOCK_INVALID;
    pm_init(&s->wlock);
    pf_set(&s->running, 0);
    pf_set(&s->connected, 0);

    const cJSON *mode = jget(cfg, "mode");
    s->router_mode = cJSON_IsString(mode) && strcmp(mode->valuestring, "router") == 0;
    const cJSON *t = jget(cfg, "targetAmsNetId"), *l = jget(cfg, "localAmsNetId"), *ra = jget(cfg, "routerAddress");
    s->target_port = jint(cfg, "targetAdsPort", 851);
    s->local_port = jint(cfg, "localAdsPort", 32750);
    s->tcp_port = jint(cfg, "routerTcpPort", 48898);
    s->cycle_ms = jint(cfg, "cycleTime", 20);
    if (s->cycle_ms < 1) s->cycle_ms = 1;
    const cJSON *oc = jget(cfg, "sendOnChange");
    s->on_change = !cJSON_IsFalse(oc);
    str_copy(s->host, sizeof s->host, cJSON_IsString(ra) && *ra->valuestring ? ra->valuestring : "127.0.0.1");

    const char *bad = NULL;
    if (parse_netid(cJSON_IsString(t) ? t->valuestring : NULL, s->target) != 0) bad = "AMS Net ID de l'automate invalide (ex. 192.168.1.10.1.1)";
    else if (!s->router_mode && parse_netid(cJSON_IsString(l) ? l->valuestring : NULL, s->local) != 0)
        bad = "AMS Net ID local invalide (ex. 192.168.1.100.1.1)";
    else if (s->target_port <= 0 || s->target_port > 65535 || s->tcp_port <= 0 || s->tcp_port > 65535 ||
             s->local_port <= 0 || s->local_port > 65535)
        bad = "port invalide";
    const cJSON *syms = jget(cfg, "symbols"), *it;
    cJSON_ArrayForEach(it, syms)
    {
        if (!cJSON_IsString(it)) continue;
        const char *p = it->valuestring;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        s->symbols = xrealloc(s->symbols, (size_t)(s->nsymbols + 1) * sizeof *s->symbols);
        s->symbols[s->nsymbols] = xstrdup(p);
        size_t n = strlen(s->symbols[s->nsymbols]);
        while (n && (s->symbols[s->nsymbols][n - 1] == ' ' || s->symbols[s->nsymbols][n - 1] == '\t')) s->symbols[s->nsymbols][--n] = 0;
        s->nsymbols++;
    }
    if (!bad && s->nsymbols == 0) bad = "aucun symbole configuré (ex. GVL_HMI.stHmi)";
    if (!bad && s->nsymbols > MAX_ROOTS) bad = "trop de symboles";
    if (bad) {
        snprintf(err, cap, "Configuration ADS : %s", bad);
        ads_destroy(&s->base);
        return NULL;
    }
    return &s->base;
}
