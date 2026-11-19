#include "store.h"
#include "plat.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DEFAULT_CONFIG =
    "{\"source\":\"sim\","
    "\"ads\":{\"targetAmsNetId\":\"192.168.1.10.1.1\",\"targetAdsPort\":851,\"mode\":\"direct\","
    "\"routerAddress\":\"192.168.1.10\",\"routerTcpPort\":48898,\"localAmsNetId\":\"192.168.1.100.1.1\","
    "\"localAdsPort\":32750,\"symbols\":[\"GVL_HMI.stHmi\"],\"cycleTime\":20,\"sendOnChange\":true,"
    "\"maxArrayExpand\":1000},"
    "\"sim\":{\"cycleTime\":20}}";

cJSON *config_defaults(void) { return cJSON_Parse(DEFAULT_CONFIG); }

/** remplace (ou ajoute) dans `dst` chaque clé de `src` par une copie */
static void overlay(cJSON *dst, const cJSON *src)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, src)
    {
        if (!it->string) continue;
        cJSON *copy = cJSON_Duplicate(it, 1);
        if (cJSON_HasObjectItem(dst, it->string)) cJSON_ReplaceItemInObjectCaseSensitive(dst, it->string, copy);
        else cJSON_AddItemToObject(dst, it->string, copy);
    }
}

cJSON *config_merge(const cJSON *raw)
{
    cJSON *cfg = config_defaults();
    if (!cJSON_IsObject(raw)) return cfg;
    const cJSON *it;
    cJSON_ArrayForEach(it, raw)
    {
        if (!it->string) continue;
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(cfg, it->string);
        int section = !strcmp(it->string, "ads") || !strcmp(it->string, "sim");
        if (section && cJSON_IsObject(cur) && cJSON_IsObject(it)) overlay(cur, it);
        else if (!section || cJSON_IsObject(it)) {
            cJSON *copy = cJSON_Duplicate(it, 1);
            if (cur) cJSON_ReplaceItemInObjectCaseSensitive(cfg, it->string, copy);
            else cJSON_AddItemToObject(cfg, it->string, copy);
        }
    }
    return cfg;
}

cJSON *config_load(const char *data_dir, char *err, size_t cap)
{
    err[0] = 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/config.json", data_dir);
    int notfound;
    size_t len;
    char *text = file_read(path, &len, &notfound);
    if (!text) {
        if (!notfound) snprintf(err, cap, "lecture impossible : %s", plat_strerror());
        return config_defaults();
    }
    cJSON *raw = cJSON_ParseWithLength(text, len);
    free(text);
    if (cJSON_IsObject(raw)) {
        cJSON *cfg = config_merge(raw);
        cJSON_Delete(raw);
        return cfg;
    }
    snprintf(err, cap, raw ? "le contenu n'est pas un objet JSON" : "JSON invalide");
    cJSON_Delete(raw);
    /* le fichier d'origine est conservé à côté : « Appliquer » dans l'interface le remplacera */
    char backup[1100], stamp[32];
    plat_date d;
    plat_now(&d, 1);
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", d.year, d.mon, d.day, d.hour, d.min, d.sec);
    snprintf(backup, sizeof backup, "%s.illisible-%s", path, stamp);
    if (file_copy(path, backup) == 0) log_error("config.json illisible, copie conservée : %s", backup);
    return config_defaults();
}

int config_save(const char *data_dir, const cJSON *cfg, char *err, size_t cap)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/config.json", data_dir);
    char *text = cJSON_Print(cfg);
    int rc = file_write_atomic(path, text, strlen(text));
    if (rc != 0) snprintf(err, cap, "Écriture de config.json impossible : %s", plat_strerror());
    free(text);
    return rc;
}

/* ------------------------------------------------------------- écrans */

int store_valid_id(const char *id)
{
    size_t n = 0;
    for (const char *p = id; *p; p++, n++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= 80) return 0;
    }
    return n > 0;
}

typedef struct {
    char *id, *name, *updated;
} dash_entry;

typedef struct {
    const char *dir;
    dash_entry *v;
    int n, cap;
} list_ctx;

static int list_one(const char *fname, void *p)
{
    list_ctx *c = p;
    size_t n = strlen(fname);
    if (n <= 5 || strcmp(fname + n - 5, ".json") != 0) return 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", c->dir, fname);
    size_t len;
    char *text = file_read(path, &len, NULL);
    if (!text) return 0;
    cJSON *d = cJSON_ParseWithLength(text, len);
    free(text);
    if (!cJSON_IsObject(d)) { cJSON_Delete(d); return 0; } /* écran illisible : ignoré dans la liste */
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->v = xrealloc(c->v, (size_t)c->cap * sizeof *c->v);
    }
    dash_entry *e = &c->v[c->n++];
    e->id = xstrdup(fname);
    e->id[n - 5] = 0;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
    const cJSON *upd = cJSON_GetObjectItemCaseSensitive(d, "updatedAt");
    e->name = xstrdup(cJSON_IsString(name) ? name->valuestring : fname);
    e->updated = cJSON_IsString(upd) ? xstrdup(upd->valuestring) : NULL;
    cJSON_Delete(d);
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const dash_entry *x = a, *y = b;
    /* tri insensible à la casse (ASCII) ; à égalité, ordre des octets */
    for (const char *p = x->name, *q = y->name;; p++, q++) {
        int cp = (*p >= 'A' && *p <= 'Z') ? *p + 32 : (unsigned char)*p;
        int cq = (*q >= 'A' && *q <= 'Z') ? *q + 32 : (unsigned char)*q;
        if (cp != cq) return cp - cq;
        if (!*p) break;
    }
    return strcmp(x->name, y->name);
}

char *store_list(const char *dash_dir)
{
    list_ctx c = {dash_dir, NULL, 0, 0};
    dir_list(dash_dir, list_one, &c);
    if (c.n > 1) qsort(c.v, (size_t)c.n, sizeof *c.v, cmp_entry);
    sb_t o = {0};
    sb_putc(&o, '[');
    for (int i = 0; i < c.n; i++) {
        sb_puts(&o, i ? ",{\"id\":" : "{\"id\":");
        sb_json_str(&o, c.v[i].id, (size_t)-1);
        sb_puts(&o, ",\"name\":");
        sb_json_str(&o, c.v[i].name, (size_t)-1);
        if (c.v[i].updated) {
            sb_puts(&o, ",\"updatedAt\":");
            sb_json_str(&o, c.v[i].updated, (size_t)-1);
        }
        sb_putc(&o, '}');
        free(c.v[i].id);
        free(c.v[i].name);
        free(c.v[i].updated);
    }
    sb_putc(&o, ']');
    free(c.v);
    return o.p;
}

static void dash_path(char *out, size_t cap, const char *dir, const char *id)
{
    snprintf(out, cap, "%s/%s.json", dir, id);
}

char *store_get(const char *dash_dir, const char *id, size_t *len, int *notfound)
{
    char path[1024];
    dash_path(path, sizeof path, dash_dir, id);
    return file_read(path, len, notfound);
}

int store_put(const char *dash_dir, const char *id, cJSON *doc, char *err, size_t cap)
{
    char stamp[32], path[1024];
    iso_utc_now(stamp);
    cJSON *idv = cJSON_CreateString(id), *upd = cJSON_CreateString(stamp);
    if (cJSON_HasObjectItem(doc, "id")) cJSON_ReplaceItemInObjectCaseSensitive(doc, "id", idv);
    else cJSON_AddItemToObject(doc, "id", idv);
    if (cJSON_HasObjectItem(doc, "updatedAt")) cJSON_ReplaceItemInObjectCaseSensitive(doc, "updatedAt", upd);
    else cJSON_AddItemToObject(doc, "updatedAt", upd);
    char *text = cJSON_Print(doc);
    dash_path(path, sizeof path, dash_dir, id);
    int rc = file_write_atomic(path, text, strlen(text));
    if (rc != 0) snprintf(err, cap, "Écriture de l'écran impossible : %s", plat_strerror());
    free(text);
    return rc;
}

int store_delete(const char *dash_dir, const char *id, int *notfound)
{
    char path[1024];
    dash_path(path, sizeof path, dash_dir, id);
    *notfound = !path_is_file(path);
    if (*notfound) return -1;
    return file_remove(path);
}
