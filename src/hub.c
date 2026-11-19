#include "hub.h"
#include "calc.h"
#include "codec.h"
#include "plat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** variable calculée (définie dans data/calculs.json, exposée sous CALC.<nom>) */
typedef struct {
    char name[64];
    /** "auto" (BOOL si l'expression est logique, sinon LREAL), "BOOL", "LREAL", "REAL", "DINT", "INT" */
    char type[8];
    char *expr, *unit, *comment;
    calc_prog *prog;
    /** indice de variable de chaque référence de l'expression (-1 = inconnue) ; recalculé à chaque schéma */
    int *ref;
    /** référence introuvable dans le schéma courant (non bloquant : la source peut être déconnectée) */
    char err[256];
} calc_def;

#define CALC_ROOT "CALC"
#define CALC_MAX 500

struct hub {
    /** protège tout ce qui suit, sauf `source` et les tampons de sortie (boucle principale uniquement) */
    plat_mutex lock;

    /* schéma courant */
    roots_t *roots;
    leaf_list leaves;
    /** variables de la racine r : indices [root_start[r], root_end[r]) (contiguës par construction) */
    int *root_start, *root_end;
    /** dernier bloc reçu par racine (NULL tant qu'aucune donnée) : référence de comparaison et source du décodage */
    uint8_t **blk;
    size_t *blklen;
    /** table de hachage chemin -> indice + 1 (adressage ouvert, 0 = vide) */
    int *hidx;
    uint32_t hmask;
    int schema_version;
    int max_arr;
    /** symboles racines fournis par la source : [0, nsrc_roots) ; le symbole CALC éventuel vient ensuite */
    int nsrc_roots;
    /** indice du symbole racine virtuel CALC (bloc tenu par le hub), -1 si aucune variable calculée */
    int calc_root;

    /* variables calculées */
    calc_def *calcs;
    int ncalc;

    /* changements en attente d'envoi */
    uint8_t *dirty;
    int *dlist;
    int ndirty;
    int schema_pending;

    /* statut */
    conn_state state;
    char message[512];
    char source_kind[8];
    int status_pending;

    /* statistiques (fenêtre d'une seconde) */
    long notif_count;
    double proc_total_ms;
    double stat_t0;
    long notif_rate;
    double proc_ms;

    /* boucle principale uniquement */
    source_t *source;
    sb_t out_status, out_schema, out_delta;
};

/* ------------------------------------------------------------- schéma */

static void free_schema(hub_t *h)
{
    for (int r = 0; h->roots && r < h->roots->n; r++) free(h->blk[r]);
    free(h->blk);
    free(h->blklen);
    free(h->root_start);
    free(h->root_end);
    free(h->hidx);
    free(h->dirty);
    free(h->dlist);
    leaves_free(&h->leaves);
    roots_free(h->roots);
    h->roots = NULL;
    h->blk = NULL;
    h->blklen = NULL;
    h->root_start = h->root_end = NULL;
    h->hidx = NULL;
    h->dirty = NULL;
    h->dlist = NULL;
    h->ndirty = 0;
}

static int find_locked(hub_t *h, const char *path, size_t n);
static void resolve_calcs_locked(hub_t *h);
static void eval_calcs_locked(hub_t *h, int mark_dirty);
static void calc_defs_free(calc_def *v, int n);

/** ajoute le symbole racine virtuel CALC (une variable par définition) aux symboles de la source */
static void add_calc_root(hub_t *h, roots_t *roots)
{
    h->calc_root = -1;
    if (h->ncalc == 0) return;
    for (int r = 0; r < roots->n; r++) {
        if (str_ieq(roots->v[r].path, CALC_ROOT)) {
            log_error("un symbole de la source s'appelle déjà %s : variables calculées indisponibles", CALC_ROOT);
            return;
        }
    }
    member_spec *m = xcalloc((size_t)h->ncalc, sizeof *m);
    for (int i = 0; i < h->ncalc; i++) {
        const calc_def *c = &h->calcs[i];
        const char *t = strcmp(c->type, "auto") ? c->type : (calc_is_bool(c->prog) ? "BOOL" : "LREAL");
        m[i].name = c->name;
        m[i].node = tn_leaf(roots->arena, t, c->unit && *c->unit ? c->unit : NULL);
        m[i].comment = c->comment && *c->comment ? c->comment : NULL;
    }
    roots_add(roots, CALC_ROOT, tn_struct(roots->arena, "CALC", m, h->ncalc));
    free(m);
    h->calc_root = roots->n - 1;
}

/**
 * Installe un schéma (verrou détenu, ancien schéma déjà libéré). `keep_blk` (facultatif) : derniers blocs
 * des symboles de la source à conserver (changement des seules variables calculées : une source qui
 * n'envoie que les changements, comme ADS, ne renverra pas les valeurs inchangées).
 */
static void install_locked(hub_t *h, roots_t *roots, uint8_t **keep_blk, size_t *keep_len)
{
    h->roots = roots;
    h->nsrc_roots = roots->n;
    add_calc_root(h, roots);
    int nr = h->roots->n;
    leaves_compile(h->roots, h->max_arr, &h->leaves);
    int n = h->leaves.n;
    h->blk = xcalloc((size_t)nr, sizeof *h->blk);
    h->blklen = xcalloc((size_t)nr, sizeof *h->blklen);
    h->root_start = xcalloc((size_t)nr, sizeof(int));
    h->root_end = xcalloc((size_t)nr, sizeof(int));
    for (int r = 0; r < nr; r++) h->root_start[r] = h->root_end[r] = 0;
    for (int i = n - 1; i >= 0; i--) h->root_start[h->leaves.v[i].root] = i;
    for (int i = 0; i < n; i++) h->root_end[h->leaves.v[i].root] = i + 1;
    h->dirty = xcalloc((size_t)n, 1);
    h->dlist = xcalloc((size_t)n, sizeof(int));
    /* index des chemins : taille puissance de 2 >= 2n */
    uint32_t size = 16;
    while (size < (uint32_t)n * 2) size <<= 1;
    h->hmask = size - 1;
    h->hidx = xcalloc(size, sizeof(int));
    for (int i = 0; i < n; i++) {
        const char *p = h->leaves.v[i].path;
        uint32_t k = hash_ci(p, strlen(p)) & h->hmask;
        while (h->hidx[k]) k = (k + 1) & h->hmask;
        h->hidx[k] = i + 1;
    }
    for (int r = 0; keep_blk && r < h->nsrc_roots; r++) {
        h->blk[r] = keep_blk[r];
        h->blklen[r] = keep_len[r];
    }
    if (h->calc_root >= 0) {
        size_t sz = (size_t)h->roots->v[h->calc_root].node->size;
        h->blk[h->calc_root] = xcalloc(sz ? sz : 1, 1);
        h->blklen[h->calc_root] = sz;
    }
    resolve_calcs_locked(h);
    eval_calcs_locked(h, 0);
    h->schema_version++;
    h->schema_pending = 1;
    log_info("schéma v%d : %d variables (%d symbole%s racine%s)", h->schema_version, n, nr, nr > 1 ? "s" : "",
             nr > 1 ? "s" : "");
}

/** remplace le schéma de la source (verrou détenu) ; `roots` NULL = schéma vide */
static void set_roots_locked(hub_t *h, roots_t *roots)
{
    free_schema(h);
    install_locked(h, roots ? roots : roots_new(), NULL, NULL);
}

/** reconstruit le schéma après changement des variables calculées, sans perdre les valeurs de la source */
static void rebuild_calc_locked(hub_t *h)
{
    roots_t *roots = h->roots;
    uint8_t **kb = h->blk;
    size_t *kl = h->blklen;
    if (h->calc_root >= 0) free(kb[h->calc_root]);
    h->roots = NULL;
    h->blk = NULL;
    h->blklen = NULL;
    free_schema(h);
    /* retire l'ancien symbole CALC (ses types restent dans l'arène jusqu'au prochain schéma de la source) */
    roots->n = h->nsrc_roots;
    install_locked(h, roots, kb, kl);
    free(kb);
    free(kl);
}

/** indice d'une variable par chemin (insensible à la casse), -1 si inconnue ; verrou détenu */
static int find_locked(hub_t *h, const char *path, size_t n)
{
    if (!h->hidx) return -1;
    uint32_t k = hash_ci(path, n) & h->hmask;
    while (h->hidx[k]) {
        const char *p = h->leaves.v[h->hidx[k] - 1].path;
        if (strlen(p) == n) {
            size_t j = 0;
            while (j < n && tolower((unsigned char)p[j]) == tolower((unsigned char)path[j])) j++;
            if (j == n) return h->hidx[k] - 1;
        }
        k = (k + 1) & h->hmask;
    }
    return -1;
}

/* ------------------------------------------------------------- cycle de vie */

hub_t *hub_new(void)
{
    hub_t *h = xcalloc(1, sizeof *h);
    pm_init(&h->lock);
    h->max_arr = 1000;
    h->calc_root = -1;
    h->state = ST_STOPPED;
    str_copy(h->source_kind, sizeof h->source_kind, "sim");
    h->stat_t0 = mono_ms();
    set_roots_locked(h, NULL);
    return h;
}

static void stop_source(hub_t *h)
{
    if (!h->source) return;
    h->source->stop(h->source);
    h->source->destroy(h->source);
    h->source = NULL;
}

void hub_free(hub_t *h)
{
    if (!h) return;
    stop_source(h);
    free_schema(h);
    calc_defs_free(h->calcs, h->ncalc);
    h->calcs = NULL;
    sb_free(&h->out_status);
    sb_free(&h->out_schema);
    sb_free(&h->out_delta);
    pm_destroy(&h->lock);
    free(h);
}

static const cJSON *obj(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }

void hub_apply(hub_t *h, const cJSON *cfg)
{
    stop_source(h);
    const cJSON *src = obj(cfg, "source");
    int ads = cJSON_IsString(src) && strcmp(src->valuestring, "ads") == 0;
    const cJSON *maxa = obj(obj(cfg, "ads"), "maxArrayExpand");
    const cJSON *simc = obj(obj(cfg, "sim"), "cycleTime");
    int cycle = cJSON_IsNumber(simc) && simc->valuedouble >= 1 ? (int)simc->valuedouble : 20;

    pm_lock(&h->lock);
    str_copy(h->source_kind, sizeof h->source_kind, ads ? "ads" : "sim");
    h->max_arr = cJSON_IsNumber(maxa) && maxa->valuedouble >= 1 ? (int)maxa->valuedouble : 1000;
    /* jamais de valeurs d'une ancienne source affichées comme valides */
    set_roots_locked(h, NULL);
    h->state = ST_CONNECTING;
    str_copy(h->message, sizeof h->message, "Démarrage…");
    h->status_pending = 1;
    pm_unlock(&h->lock);

    char err[256] = "";
    if (ads) {
        h->source = ads_new(h, obj(cfg, "ads"), err, sizeof err);
        if (!h->source) {
            hub_source_status(h, ST_ERROR, err);
            return;
        }
    } else {
        h->source = sim_new(h, cycle);
    }
    if (h->source->start(h->source, err, sizeof err) != 0) {
        h->source->destroy(h->source);
        h->source = NULL;
        hub_source_status(h, ST_ERROR, err);
    }
}

void hub_report_error(hub_t *h, const char *message)
{
    hub_source_status(h, ST_ERROR, message);
}

/* ------------------------------------------------------------- côté source */

void hub_source_roots(hub_t *h, roots_t *roots)
{
    pm_lock(&h->lock);
    set_roots_locked(h, roots);
    pm_unlock(&h->lock);
}

void hub_source_data(hub_t *h, int root, const uint8_t *data, size_t len)
{
    double t0 = mono_ms();
    pm_lock(&h->lock);
    h->notif_count++;
    /* le bloc CALC appartient au hub : une source n'y écrit jamais */
    if (root >= 0 && root < h->nsrc_roots) {
        uint8_t *prev = h->blk[root];
        size_t plen = h->blklen[root];
        for (int i = h->root_start[root]; i < h->root_end[root]; i++) {
            const leaf_t *l = &h->leaves.v[i];
            if (prev && codec_same(l, prev, plen, data, len)) continue;
            if (!h->dirty[i]) {
                h->dirty[i] = 1;
                h->dlist[h->ndirty++] = i;
            }
        }
        /* copie dans le tampon existant (pas d'allocation) sauf au premier bloc ou si la taille change */
        if (!prev || plen != len) {
            free(prev);
            h->blk[root] = xmalloc(len);
            h->blklen[root] = len;
        }
        if (len) memcpy(h->blk[root], data, len);
    }
    h->proc_total_ms += mono_ms() - t0;
    pm_unlock(&h->lock);
}

void hub_source_status(hub_t *h, conn_state state, const char *message)
{
    pm_lock(&h->lock);
    h->state = state;
    str_copy(h->message, sizeof h->message, message ? message : "");
    h->status_pending = 1;
    pm_unlock(&h->lock);
    if (state == ST_ERROR) log_error("source : %s", message ? message : "");
    else log_info("source : %s", message ? message : "");
}

/* ------------------------------------------------------------- messages */

static const char *state_name(conn_state s)
{
    switch (s) {
    case ST_STOPPED: return "stopped";
    case ST_CONNECTING: return "connecting";
    case ST_CONNECTED: return "connected";
    case ST_ERROR: return "error";
    }
    return "stopped";
}

static void status_json_locked(hub_t *h, sb_t *o)
{
    sb_puts(o, "{\"t\":\"status\",\"status\":{\"state\":\"");
    sb_puts(o, state_name(h->state));
    sb_puts(o, "\",\"source\":\"");
    sb_puts(o, h->source_kind);
    sb_puts(o, "\",\"message\":");
    sb_json_str(o, h->message, (size_t)-1);
    sb_printf(o, ",\"notifRate\":%ld,\"procMs\":", h->notif_rate);
    sb_json_num(o, (double)(long)(h->proc_ms * 1000 + 0.5) / 1000, 0);
    sb_puts(o, "}}");
}

static void leaf_json(const leaf_t *l, sb_t *o)
{
    sb_printf(o, "{\"i\":%d,\"path\":", l->index);
    sb_json_str(o, l->path, (size_t)-1);
    sb_puts(o, ",\"type\":");
    sb_json_str(o, l->type, (size_t)-1);
    sb_printf(o, ",\"kind\":\"%s\"", leaf_kind_name(l->kind));
    if (l->comment && *l->comment) {
        sb_puts(o, ",\"comment\":");
        sb_json_str(o, l->comment, (size_t)-1);
    }
    if (l->kind == LK_ENUM && l->nenums > 0) {
        sb_puts(o, ",\"enums\":[");
        for (int i = 0; i < l->nenums; i++) {
            sb_puts(o, i ? ",{\"name\":" : "{\"name\":");
            sb_json_str(o, l->enums[i].name, (size_t)-1);
            sb_puts(o, ",\"value\":");
            sb_json_num(o, l->enums[i].value, 0);
            sb_putc(o, '}');
        }
        sb_putc(o, ']');
    }
    if (l->nattrs > 0) {
        sb_puts(o, ",\"attrs\":{");
        for (int i = 0; i < l->nattrs; i++) {
            if (i) sb_putc(o, ',');
            sb_json_str(o, l->attrs[i].key, (size_t)-1);
            sb_putc(o, ':');
            sb_json_str(o, l->attrs[i].value, (size_t)-1);
        }
        sb_putc(o, '}');
    }
    sb_putc(o, '}');
}

static void schema_json_locked(hub_t *h, sb_t *o)
{
    sb_printf(o, "{\"t\":\"schema\",\"schema\":{\"version\":%d,\"roots\":[", h->schema_version);
    for (int r = 0; r < h->roots->n; r++) {
        if (r) sb_putc(o, ',');
        sb_json_str(o, h->roots->v[r].path, (size_t)-1);
    }
    sb_puts(o, "],\"leaves\":[");
    for (int i = 0; i < h->leaves.n; i++) {
        if (i) sb_putc(o, ',');
        leaf_json(&h->leaves.v[i], o);
    }
    sb_puts(o, "]},\"values\":[");
    for (int i = 0; i < h->leaves.n; i++) {
        const leaf_t *l = &h->leaves.v[i];
        if (i) sb_putc(o, ',');
        codec_json(l, h->blk[l->root], h->blklen[l->root], o);
    }
    sb_puts(o, "]}");
}

void hub_poll(hub_t *h, hub_emit_fn emit, void *ctx)
{
    sb_clear(&h->out_status);
    sb_clear(&h->out_schema);
    sb_clear(&h->out_delta);
    double now = mono_ms();
    pm_lock(&h->lock);
    if (now - h->stat_t0 >= 1000) {
        h->notif_rate = h->notif_count;
        h->proc_ms = h->notif_count ? h->proc_total_ms / (double)h->notif_count : 0;
        h->notif_count = 0;
        h->proc_total_ms = 0;
        h->stat_t0 = now;
        h->status_pending = 1;
    }
    if (h->status_pending) {
        status_json_locked(h, &h->out_status);
        h->status_pending = 0;
    }
    /* variables calculées : réévaluées dès qu'une variable a changé */
    if (h->calc_root >= 0 && (h->ndirty || h->schema_pending)) eval_calcs_locked(h, 1);
    if (h->schema_pending) {
        /* le schéma contient toutes les valeurs : les changements en attente y sont inclus */
        schema_json_locked(h, &h->out_schema);
        h->schema_pending = 0;
        for (int k = 0; k < h->ndirty; k++) h->dirty[h->dlist[k]] = 0;
        h->ndirty = 0;
    } else if (h->ndirty) {
        sb_t *o = &h->out_delta;
        sb_puts(o, "{\"t\":\"d\",\"v\":[");
        for (int k = 0; k < h->ndirty; k++) {
            int i = h->dlist[k];
            const leaf_t *l = &h->leaves.v[i];
            h->dirty[i] = 0;
            sb_printf(o, k ? ",%d," : "%d,", i);
            codec_json(l, h->blk[l->root], h->blklen[l->root], o);
        }
        h->ndirty = 0;
        /* double exact jusqu'à 2^53 ms ; évite %lld, inégalement pris en charge selon les bibliothèques C */
        sb_printf(o, "],\"ts\":%.0f}", (double)wall_ms());
    }
    pm_unlock(&h->lock);
    /* diffusion hors verrou : la source n'est jamais bloquée par un client lent */
    if (h->out_status.len) emit(ctx, h->out_status.p, h->out_status.len, 0);
    if (h->out_schema.len) emit(ctx, h->out_schema.p, h->out_schema.len, 0);
    if (h->out_delta.len) emit(ctx, h->out_delta.p, h->out_delta.len, 1);
}

void hub_schema_msg(hub_t *h, sb_t *out)
{
    pm_lock(&h->lock);
    schema_json_locked(h, out);
    pm_unlock(&h->lock);
}

void hub_status_msg(hub_t *h, sb_t *out)
{
    pm_lock(&h->lock);
    status_json_locked(h, out);
    pm_unlock(&h->lock);
}

int hub_leaf_count(hub_t *h)
{
    pm_lock(&h->lock);
    int n = h->leaves.n;
    pm_unlock(&h->lock);
    return n;
}

/* ------------------------------------------------------------- écriture */

int hub_write(hub_t *h, const char *path, const cJSON *value, char *err, size_t cap)
{
    if (!path) path = "";
    while (isspace((unsigned char)*path)) path++;
    size_t n = strlen(path);
    while (n && isspace((unsigned char)path[n - 1])) n--;

    pm_lock(&h->lock);
    int i = find_locked(h, path, n);
    if (i < 0) {
        pm_unlock(&h->lock);
        snprintf(err, cap, "Variable inconnue : %.*s", (int)(n > 200 ? 200 : n), path);
        return -1;
    }
    if (h->leaves.v[i].root == h->calc_root) {
        snprintf(err, cap, "Variable calculée, lecture seule : %s", h->leaves.v[i].path);
        pm_unlock(&h->lock);
        return -1;
    }
    /* copie de la variable : le schéma peut être remplacé dès le verrou relâché */
    leaf_t copy = h->leaves.v[i];
    char pbuf[512], tbuf[128];
    str_copy(pbuf, sizeof pbuf, copy.path);
    str_copy(tbuf, sizeof tbuf, copy.type);
    size_t bufcap = copy.size > 8 ? (size_t)copy.size : 8;
    uint8_t *buf = xmalloc(bufcap);
    int len = codec_encode(&copy, value, buf, bufcap, err, cap);
    pm_unlock(&h->lock);

    copy.path = pbuf;
    copy.type = tbuf;
    copy.comment = NULL;
    copy.enums = NULL;
    copy.nenums = 0;
    copy.attrs = NULL;
    copy.nattrs = 0;
    int rc = -1;
    if (len < 0) {
        /* message déjà dans err */
    } else if (!h->source) {
        snprintf(err, cap, "Aucune source active");
    } else {
        rc = h->source->write(h->source, &copy, buf, len, err, cap);
    }
    free(buf);
    return rc;
}

/* ------------------------------------------------------------- variables calculées */

/** associe chaque référence des expressions à une variable du schéma courant (verrou détenu) */
static void resolve_calcs_locked(hub_t *h)
{
    int first_calc = h->calc_root >= 0 ? h->root_start[h->calc_root] : h->leaves.n;
    for (int i = 0; i < h->ncalc; i++) {
        calc_def *c = &h->calcs[i];
        int n = calc_nrefs(c->prog);
        free(c->ref);
        c->ref = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(int));
        c->err[0] = 0;
        if (h->calc_root < 0) str_copy(c->err, sizeof c->err, "symbole CALC indisponible (nom déjà utilisé par la source)");
        for (int k = 0; k < n; k++) {
            const char *name = calc_ref(c->prog, k);
            int li = find_locked(h, name, strlen(name));
            /* une variable calculée ne lit que celles définies au-dessus d'elle (contrôlé à l'enregistrement) */
            if (li >= first_calc && li - first_calc >= i) li = -1;
            c->ref[k] = li;
            if (li < 0 && !c->err[0]) snprintf(c->err, sizeof c->err, "variable inconnue : %s", name);
        }
    }
}

typedef struct {
    hub_t *h;
    const int *ref;
} eval_ctx;

static double ref_value(void *p, int k)
{
    eval_ctx *e = p;
    int li = e->ref[k];
    if (li < 0) return NAN;
    const leaf_t *l = &e->h->leaves.v[li];
    return codec_num(l, e->h->blk[l->root], e->h->blklen[l->root]);
}

/** encode un résultat dans le type de la variable calculée ; invalide : NaN (réels), 0 / FALSE sinon */
static int encode_calc(const leaf_t *l, double v, uint8_t *out)
{
    char e[8];
    memset(out, 0, 8);
    if (l->prim == PR_BOOL) { out[0] = !isnan(v) && v != 0; return 1; }
    if (l->prim == PR_I16 || l->prim == PR_I32) {
        double lo = l->prim == PR_I16 ? -32768.0 : -2147483648.0, hi = l->prim == PR_I16 ? 32767.0 : 2147483647.0;
        if (isnan(v)) v = 0;
        v = v < lo ? lo : v > hi ? hi : v;
    }
    int n = isnan(v) ? -1 : codec_encode_num(l, v, out, 8, e, sizeof e);
    if (n >= 0) return n;
    if (l->prim == PR_F64) { double d = NAN; memcpy(out, &d, 8); return 8; }
    if (l->prim == PR_F32) { float f = (float)NAN; memcpy(out, &f, 4); return 4; }
    return l->size > 8 ? 8 : l->size;
}

/** évalue toutes les variables calculées dans l'ordre (verrou détenu) ; marque celles qui changent */
static void eval_calcs_locked(hub_t *h, int mark_dirty)
{
    if (h->calc_root < 0) return;
    int r = h->calc_root;
    uint8_t *blk = h->blk[r];
    for (int i = 0; i < h->ncalc; i++) {
        int li = h->root_start[r] + i;
        const leaf_t *l = &h->leaves.v[li];
        eval_ctx ctx = {h, h->calcs[i].ref};
        uint8_t enc[8];
        int n = encode_calc(l, calc_eval(h->calcs[i].prog, ref_value, &ctx), enc);
        if (l->offset + n > (int)h->blklen[r] || memcmp(blk + l->offset, enc, (size_t)n) == 0) continue;
        memcpy(blk + l->offset, enc, (size_t)n);
        if (mark_dirty && !h->dirty[li]) {
            h->dirty[li] = 1;
            h->dlist[h->ndirty++] = li;
        }
    }
}

static void calc_defs_free(calc_def *v, int n)
{
    for (int i = 0; i < n; i++) {
        calc_free(v[i].prog);
        free(v[i].ref);
        free(v[i].expr);
        free(v[i].unit);
        free(v[i].comment);
    }
    free(v);
}

static int valid_calc_name(const char *s)
{
    static const char *reserved[] = {"true", "false", "pi", "and", "or", "xor", "not", "mod"};
    size_t n = strlen(s);
    if (n == 0 || n > 63 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (str_ieq(s, reserved[i])) return 0;
    return 1;
}

static const char *opt_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

int hub_set_calcs(hub_t *h, const cJSON *vars, char *err, size_t cap)
{
    if (vars && !cJSON_IsArray(vars)) { snprintf(err, cap, "« variables » doit être une liste"); return -1; }
    int n = vars ? cJSON_GetArraySize(vars) : 0;
    if (n > CALC_MAX) { snprintf(err, cap, "trop de variables calculées (%d, maximum %d)", n, CALC_MAX); return -1; }
    calc_def *v = xcalloc((size_t)(n > 0 ? n : 1), sizeof *v);
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, vars) {
        calc_def *c = &v[i];
        const char *name = opt_str(it, "name"), *expr = opt_str(it, "expr"), *type = opt_str(it, "type");
        const char *unit = opt_str(it, "unit"), *comment = opt_str(it, "comment");
        if (!cJSON_IsObject(it) || !name || !valid_calc_name(name)) {
            snprintf(err, cap, "variable n° %d : nom invalide « %.64s » (lettres, chiffres et _, sans espace ni point, "
                     "ne commence pas par un chiffre)", i + 1, name ? name : "");
            goto fail;
        }
        for (int j = 0; j < i; j++)
            if (str_ieq(v[j].name, name)) { snprintf(err, cap, "%s : nom déjà utilisé", name); goto fail; }
        str_copy(c->name, sizeof c->name, name);
        if (!type || !*type || str_ieq(type, "auto")) str_copy(c->type, sizeof c->type, "auto");
        else if (str_ieq(type, "BOOL") || str_ieq(type, "LREAL") || str_ieq(type, "REAL") || str_ieq(type, "DINT") ||
                 str_ieq(type, "INT")) {
            for (int k = 0; type[k] && k < 7; k++) c->type[k] = (char)toupper((unsigned char)type[k]);
        } else {
            snprintf(err, cap, "%s : type « %.20s » non pris en charge (auto, BOOL, INT, DINT, REAL, LREAL)", name, type);
            goto fail;
        }
        if (!expr || strlen(expr) > 4000) { snprintf(err, cap, "%s : expression absente ou trop longue", name); goto fail; }
        if ((unit && strlen(unit) > 40) || (comment && strlen(comment) > 200)) {
            snprintf(err, cap, "%s : unité (40 caractères) ou commentaire (200) trop long", name);
            goto fail;
        }
        char perr[200];
        c->prog = calc_compile(expr, perr, sizeof perr);
        if (!c->prog) { snprintf(err, cap, "%s : %s", name, perr); goto fail; }
        c->expr = xstrdup(expr);
        c->unit = unit && *unit ? xstrdup(unit) : NULL;
        c->comment = comment && *comment ? xstrdup(comment) : NULL;
        i++;
        /* une variable calculée ne peut lire que celles définies au-dessus : pas de boucle possible */
        for (int k = 0; k < calc_nrefs(c->prog); k++) {
            const char *ref = calc_ref(c->prog, k);
            if (!str_istarts(ref, CALC_ROOT ".")) continue;
            const char *short_name = ref + strlen(CALC_ROOT) + 1;
            int found = -1;
            for (int j = 0; j < i; j++)
                if (str_ieq(v[j].name, short_name)) found = j;
            int later = 0;
            const cJSON *o;
            cJSON_ArrayForEach(o, vars) {
                const char *nm = opt_str(o, "name");
                if (nm && str_ieq(nm, short_name)) later = 1;
            }
            if (found == i - 1) { snprintf(err, cap, "%s : la variable ne peut pas utiliser sa propre valeur", name); goto fail; }
            if (found < 0 && later) {
                snprintf(err, cap, "%s : utilise %.64s, définie plus bas ; la placer au-dessus", name, short_name);
                goto fail;
            }
            if (found < 0) { snprintf(err, cap, "%s : variable calculée inconnue : %.64s", name, short_name); goto fail; }
        }
    }

    pm_lock(&h->lock);
    calc_defs_free(h->calcs, h->ncalc);
    h->calcs = v;
    h->ncalc = n;
    rebuild_calc_locked(h);
    pm_unlock(&h->lock);
    return 0;

fail:
    calc_defs_free(v, n);
    return -1;
}

void hub_calcs_json(hub_t *h, int with_values, sb_t *o)
{
    pm_lock(&h->lock);
    sb_puts(o, "{\"variables\":[");
    for (int i = 0; i < h->ncalc; i++) {
        const calc_def *c = &h->calcs[i];
        if (i) sb_putc(o, ',');
        sb_puts(o, "{\"name\":");
        sb_json_str(o, c->name, (size_t)-1);
        sb_puts(o, ",\"expr\":");
        sb_json_str(o, c->expr, (size_t)-1);
        sb_puts(o, ",\"type\":");
        sb_json_str(o, c->type, (size_t)-1);
        if (c->unit) { sb_puts(o, ",\"unit\":"); sb_json_str(o, c->unit, (size_t)-1); }
        if (c->comment) { sb_puts(o, ",\"comment\":"); sb_json_str(o, c->comment, (size_t)-1); }
        if (with_values) {
            sb_printf(o, ",\"path\":\"%s.%s\"", CALC_ROOT, c->name);
            if (h->calc_root >= 0) {
                const leaf_t *l = &h->leaves.v[h->root_start[h->calc_root] + i];
                sb_puts(o, ",\"plcType\":");
                sb_json_str(o, l->type, (size_t)-1);
                sb_puts(o, ",\"value\":");
                codec_json(l, h->blk[h->calc_root], h->blklen[h->calc_root], o);
            }
            if (c->err[0]) { sb_puts(o, ",\"error\":"); sb_json_str(o, c->err, (size_t)-1); }
        }
        sb_putc(o, '}');
    }
    sb_puts(o, "]}");
    pm_unlock(&h->lock);
}

void hub_calc_test(hub_t *h, const char *expr, sb_t *o)
{
    char err[256];
    calc_prog *p = calc_compile(expr, err, sizeof err);
    if (!p) {
        sb_puts(o, "{\"error\":");
        sb_json_str(o, err, (size_t)-1);
        sb_putc(o, '}');
        return;
    }
    int n = calc_nrefs(p);
    int *ref = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    err[0] = 0;
    pm_lock(&h->lock);
    for (int k = 0; k < n; k++) {
        const char *name = calc_ref(p, k);
        ref[k] = find_locked(h, name, strlen(name));
        if (ref[k] < 0 && !err[0]) snprintf(err, sizeof err, "variable inconnue : %s", name);
    }
    eval_ctx ctx = {h, ref};
    double v = calc_eval(p, ref_value, &ctx);
    pm_unlock(&h->lock);
    sb_puts(o, "{\"value\":");
    if (calc_is_bool(p) && !isnan(v)) sb_puts(o, v != 0 ? "true" : "false");
    else sb_json_num(o, v, 0);
    sb_printf(o, ",\"type\":\"%s\"", calc_is_bool(p) ? "BOOL" : "LREAL");
    if (err[0]) { sb_puts(o, ",\"error\":"); sb_json_str(o, err, (size_t)-1); }
    sb_putc(o, '}');
    free(ref);
    calc_free(p);
}
