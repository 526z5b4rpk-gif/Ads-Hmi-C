/*
 * ADS HMI — service temps réel en C.
 *
 * Remplace le serveur Node de la version web à l'identique (même API REST, même protocole WebSocket) :
 *   GET  /api/config               configuration de la source
 *   PUT  /api/config               enregistre la configuration et redémarre la source
 *   GET  /api/dashboards           liste des écrans
 *   GET  /api/dashboards/<id>      un écran
 *   PUT  /api/dashboards/<id>      enregistre un écran
 *   DELETE /api/dashboards/<id>    supprime un écran
 *   GET  /api/calc                 variables calculées avec valeur courante et erreur éventuelle
 *   PUT  /api/calc                 remplace les variables calculées (data/calculs.json)
 *   POST /api/calc/test            évalue une expression sur les valeurs courantes
 *   GET  /ws                       WebSocket temps réel (schema, d, status, wr, dashboards)
 *   GET  /…                        fichiers du front web (dossier web/), index.html par défaut
 *
 * Boucle principale mono-thread : réseau (poll, 10 ms au plus) puis diffusion des changements du hub.
 * La source de données (simulateur, ADS) tourne sur son propre thread.
 */
#include "hub.h"
#include "http.h"
#include "plat.h"
#include "store.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "0.1.0"
/** au-delà, un client n'arrive plus à suivre : ses deltas sont abandonnés et il recevra un schéma complet */
#define SLOW_CLIENT_BYTES (4u * 1024 * 1024)

typedef struct {
    hub_t *hub;
    http_server *srv;
    char data_dir[1024];
    char dash_dir[1100];
    char web_dir[1024];
    cJSON *config;
    /** schéma complet construit au plus une fois par diffusion (clients à resynchroniser) */
    sb_t resync;
    int resync_built;
} app_t;

static plat_flag g_quit = 0;
static void on_quit(void) { pf_set(&g_quit, 1); }

/* ======================================================================= réponses */

static const char *JSON_TYPE = "application/json; charset=utf-8";
static const char *NO_STORE = "Cache-Control: no-store\r\n";

static void send_json(http_conn *c, int status, const char *json, size_t len)
{
    http_respond(c, status, JSON_TYPE, NO_STORE, json, len);
}

static void send_error(http_conn *c, int status, const char *msg)
{
    sb_t b = {0};
    sb_puts(&b, "{\"error\":");
    sb_json_str(&b, msg, (size_t)-1);
    sb_putc(&b, '}');
    send_json(c, status, b.p, b.len);
    sb_free(&b);
}

static void send_ok(http_conn *c) { send_json(c, 200, "{\"ok\":true}", 11); }

/* ======================================================================= diffusion */

typedef struct {
    app_t *app;
    const char *json;
    size_t len;
    int is_delta;
} bcast;

static void send_one(void *ctx, http_conn *c)
{
    bcast *b = ctx;
    int *resync = http_conn_flag(c);
    if (b->is_delta) {
        /* client trop lent : on ne bufferise pas indéfiniment les deltas */
        if (http_conn_pending(c) > SLOW_CLIENT_BYTES) {
            *resync = 1;
            return;
        }
        if (*resync) {
            *resync = 0;
            if (!b->app->resync_built) {
                sb_clear(&b->app->resync);
                hub_schema_msg(b->app->hub, &b->app->resync);
                b->app->resync_built = 1;
            }
            ws_send_text(c, b->app->resync.p, b->app->resync.len);
            return;
        }
    }
    ws_send_text(c, b->json, b->len);
}

static void broadcast(void *ctx, const char *json, size_t len, int is_delta)
{
    app_t *app = ctx;
    bcast b = {app, json, len, is_delta};
    app->resync_built = 0;
    http_ws_foreach(app->srv, send_one, &b);
}

/* ======================================================================= API */

static void api_config(app_t *app, http_conn *c, const http_req *r)
{
    if (!strcmp(r->method, "GET")) {
        char *text = cJSON_PrintUnformatted(app->config);
        send_json(c, 200, text, strlen(text));
        free(text);
        return;
    }
    if (strcmp(r->method, "PUT") != 0) { send_error(c, 405, "Méthode non autorisée"); return; }
    cJSON *raw = cJSON_ParseWithLength(r->body, r->body_len);
    if (!cJSON_IsObject(raw)) {
        cJSON_Delete(raw);
        send_error(c, 400, "Configuration invalide : objet JSON attendu");
        return;
    }
    cJSON *cfg = config_merge(raw);
    cJSON_Delete(raw);
    char err[256];
    if (config_save(app->data_dir, cfg, err, sizeof err) != 0) {
        cJSON_Delete(cfg);
        send_error(c, 500, err);
        return;
    }
    cJSON_Delete(app->config);
    app->config = cfg;
    log_info("configuration enregistrée, redémarrage de la source");
    hub_apply(app->hub, cfg);
    send_ok(c);
}

static void api_dashboard(app_t *app, http_conn *c, const http_req *r, const char *id)
{
    if (!store_valid_id(id)) { send_error(c, 400, "Identifiant invalide"); return; }
    if (!strcmp(r->method, "GET")) {
        size_t len;
        int notfound;
        char *text = store_get(app->dash_dir, id, &len, &notfound);
        if (!text) { send_error(c, notfound ? 404 : 500, notfound ? "Écran introuvable" : "Lecture impossible"); return; }
        send_json(c, 200, text, len);
        free(text);
        return;
    }
    if (!strcmp(r->method, "PUT")) {
        cJSON *doc = cJSON_ParseWithLength(r->body, r->body_len);
        if (!cJSON_IsObject(doc)) {
            cJSON_Delete(doc);
            send_error(c, 400, "Écran invalide : objet JSON attendu");
            return;
        }
        char err[256];
        int rc = store_put(app->dash_dir, id, doc, err, sizeof err);
        cJSON_Delete(doc);
        if (rc != 0) { send_error(c, 500, err); return; }
        send_ok(c);
        broadcast(app, "{\"t\":\"dashboards\"}", 18, 0);
        return;
    }
    if (!strcmp(r->method, "DELETE")) {
        int notfound;
        if (store_delete(app->dash_dir, id, &notfound) != 0) {
            send_error(c, notfound ? 404 : 500, notfound ? "Écran introuvable" : "Suppression impossible");
            return;
        }
        send_ok(c);
        broadcast(app, "{\"t\":\"dashboards\"}", 18, 0);
        return;
    }
    send_error(c, 405, "Méthode non autorisée");
}

/* ------------------------------------------------------------- variables calculées */

#define CALC_FILE "calculs.json"

/** charge data/calculs.json au démarrage ; absent = aucune variable calculée */
static void calc_load(app_t *app)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/" CALC_FILE, app->data_dir);
    size_t len;
    int notfound;
    char *text = file_read(path, &len, &notfound);
    if (!text) {
        if (!notfound) log_error("%s : lecture impossible (%s)", path, plat_strerror());
        return;
    }
    cJSON *doc = cJSON_ParseWithLength(text, len);
    free(text);
    char err[512];
    if (!cJSON_IsObject(doc)) {
        /* le fichier n'est pas réécrit tant que l'utilisateur n'enregistre pas : il peut le corriger */
        log_error("%s : JSON invalide, aucune variable calculée", path);
    } else if (hub_set_calcs(app->hub, cJSON_GetObjectItemCaseSensitive(doc, "variables"), err, sizeof err) != 0) {
        log_error("%s : %s ; aucune variable calculée", path, err);
    } else {
        int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(doc, "variables"));
        log_info("%d variable%s calculée%s (%s)", n, n > 1 ? "s" : "", n > 1 ? "s" : "", path);
    }
    cJSON_Delete(doc);
}

static void api_calc(app_t *app, http_conn *c, const http_req *r, int test)
{
    sb_t out = {0};
    if (test) {
        if (strcmp(r->method, "POST") != 0) { send_error(c, 405, "Méthode non autorisée"); return; }
        cJSON *doc = cJSON_ParseWithLength(r->body, r->body_len);
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(doc, "expr");
        if (!cJSON_IsString(e)) {
            cJSON_Delete(doc);
            send_error(c, 400, "« expr » attendu");
            return;
        }
        hub_calc_test(app->hub, e->valuestring, &out);
        cJSON_Delete(doc);
        send_json(c, 200, out.p, out.len);
        sb_free(&out);
        return;
    }
    if (!strcmp(r->method, "GET")) {
        hub_calcs_json(app->hub, 1, &out);
        send_json(c, 200, out.p, out.len);
        sb_free(&out);
        return;
    }
    if (strcmp(r->method, "PUT") != 0) { send_error(c, 405, "Méthode non autorisée"); return; }
    cJSON *doc = cJSON_ParseWithLength(r->body, r->body_len);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        send_error(c, 400, "Objet JSON {\"variables\":[…]} attendu");
        return;
    }
    char err[512];
    int rc = hub_set_calcs(app->hub, cJSON_GetObjectItemCaseSensitive(doc, "variables"), err, sizeof err);
    cJSON_Delete(doc);
    if (rc != 0) { send_error(c, 400, err); return; }
    /* fichier normalisé (champs connus uniquement), écrit de façon atomique */
    hub_calcs_json(app->hub, 0, &out);
    char path[1100];
    snprintf(path, sizeof path, "%s/" CALC_FILE, app->data_dir);
    if (file_write_atomic(path, out.p, out.len) != 0) {
        sb_free(&out);
        send_error(c, 500, "Variables appliquées mais non enregistrées : écriture de calculs.json impossible");
        return;
    }
    log_info("variables calculées enregistrées");
    sb_clear(&out);
    hub_calcs_json(app->hub, 1, &out);
    send_json(c, 200, out.p, out.len);
    sb_free(&out);
}

static void api(app_t *app, http_conn *c, const http_req *r)
{
    const char *p = r->path;
    if (!strcmp(p, "/api/config")) { api_config(app, c, r); return; }
    if (!strcmp(p, "/api/dashboards")) {
        if (strcmp(r->method, "GET") != 0) { send_error(c, 405, "Méthode non autorisée"); return; }
        char *list = store_list(app->dash_dir);
        send_json(c, 200, list, strlen(list));
        free(list);
        return;
    }
    if (!strncmp(p, "/api/dashboards/", 16)) { api_dashboard(app, c, r, p + 16); return; }
    if (!strcmp(p, "/api/calc")) { api_calc(app, c, r, 0); return; }
    if (!strcmp(p, "/api/calc/test")) { api_calc(app, c, r, 1); return; }
    send_error(c, 404, "Not found");
}

/* ======================================================================= fichiers statiques */

static const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } M[] = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},   {".svg", "image/svg+xml"},
        {".png", "image/png"},                 {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},               {".json", "application/json; charset=utf-8"},
        {".ico", "image/x-icon"},              {".woff2", "font/woff2"},
        {".webp", "image/webp"},               {".map", "application/json"},
    };
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
            if (str_ieq(dot, M[i].ext)) return M[i].type;
    return "application/octet-stream";
}

/** chemin sûr : pas de segment « .. », pas de « \ » ni de « : » (lecteurs Windows) */
static int safe_path(const char *p)
{
    if (strchr(p, '\\') || strchr(p, ':')) return 0;
    for (const char *s = p; *s; s++) {
        if (s[0] == '.' && s[1] == '.' && (s == p || s[-1] == '/') && (s[2] == 0 || s[2] == '/')) return 0;
    }
    return 1;
}

static void serve_static(app_t *app, http_conn *c, const http_req *r)
{
    if (strcmp(r->method, "GET") != 0 && strcmp(r->method, "HEAD") != 0) {
        send_error(c, 405, "Méthode non autorisée");
        return;
    }
    if (!safe_path(r->path)) { send_error(c, 403, "Forbidden"); return; }
    char file[3200];
    snprintf(file, sizeof file, "%s%s", app->web_dir, r->path);
    if (path_is_dir(file)) {
        size_t n = strlen(file);
        snprintf(file + n, sizeof file - n, "%sindex.html", file[n - 1] == '/' ? "" : "/");
    }
    /* application monopage : toute route inconnue sert index.html (le routage est fait par le navigateur) */
    if (!path_is_file(file)) snprintf(file, sizeof file, "%s/index.html", app->web_dir);
    size_t len;
    char *data = file_read(file, &len, NULL);
    if (!data) { send_error(c, 404, "Not found (front web absent : dossier web/)"); return; }
    const char *cache = strstr(file, "/assets/") ? "Cache-Control: public, max-age=31536000, immutable\r\n"
                                                 : "Cache-Control: no-cache\r\n";
    http_respond(c, 200, mime_type(file), cache, data, strcmp(r->method, "HEAD") ? len : 0);
    free(data);
}

/* ======================================================================= gestionnaires */

static void on_request(void *ctx, http_conn *c, const http_req *r)
{
    app_t *app = ctx;
    if (!strncmp(r->path, "/api/", 5)) api(app, c, r);
    else serve_static(app, c, r);
}

static void on_ws_open(void *ctx, http_conn *c)
{
    app_t *app = ctx;
    sb_t b = {0};
    hub_status_msg(app->hub, &b);
    ws_send_text(c, b.p, b.len);
    sb_clear(&b);
    hub_schema_msg(app->hub, &b);
    ws_send_text(c, b.p, b.len);
    sb_free(&b);
}

/** message client : { "t":"w", "id":N, "path":"…", "value":… } -> { "t":"wr", "id":N, "ok":…, "error"? } */
static void on_ws_text(void *ctx, http_conn *c, const char *data, size_t len)
{
    app_t *app = ctx;
    cJSON *m = cJSON_ParseWithLength(data, len);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(m, "t");
    if (cJSON_IsString(t) && !strcmp(t->valuestring, "w")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(m, "path");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(m, "value");
        char err[512] = "";
        int rc = cJSON_IsString(path) ? hub_write(app->hub, path->valuestring, value, err, sizeof err)
                                      : (snprintf(err, sizeof err, "Chemin de variable manquant"), -1);
        sb_t b = {0};
        sb_puts(&b, "{\"t\":\"wr\",\"id\":");
        sb_json_num(&b, cJSON_IsNumber(id) ? id->valuedouble : 0, 0);
        if (rc == 0) {
            sb_puts(&b, ",\"ok\":true}");
        } else {
            sb_puts(&b, ",\"ok\":false,\"error\":");
            sb_json_str(&b, err, (size_t)-1);
            sb_putc(&b, '}');
            log_error("écriture %s refusée : %s", cJSON_IsString(path) ? path->valuestring : "?", err);
        }
        ws_send_text(c, b.p, b.len);
        sb_free(&b);
    }
    cJSON_Delete(m);
}

/* ======================================================================= démarrage */

static void usage(void)
{
    printf("ADS HMI service %s\n\n"
           "Usage : hmi-service [options]\n"
           "  --data <dossier>   écrans, configuration (défaut : data)\n"
           "  --web <dossier>    front web compilé (défaut : web)\n"
           "  --host <adresse>   adresse d'écoute (défaut : 127.0.0.1 ; 0.0.0.0 = tout le réseau)\n"
           "  --port <port>      port HTTP (défaut : 8080)\n"
           "  --version          affiche la version\n",
           VERSION);
}

int main(int argc, char **argv)
{
    app_t app;
    memset(&app, 0, sizeof app);
    const char *data = "data", *web = "web", *host = "127.0.0.1";
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has = i + 1 < argc;
        if (!strcmp(a, "--data") && has) data = argv[++i];
        else if (!strcmp(a, "--web") && has) web = argv[++i];
        else if (!strcmp(a, "--host") && has) host = argv[++i];
        else if (!strcmp(a, "--port") && has) port = atoi(argv[++i]);
        else if (!strcmp(a, "--version")) { printf("%s\n", VERSION); return 0; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "option inconnue : %s (voir --help)\n", a); return 2; }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "port invalide\n"); return 2; }
    str_copy(app.data_dir, sizeof app.data_dir, data);
    str_copy(app.web_dir, sizeof app.web_dir, web);
    snprintf(app.dash_dir, sizeof app.dash_dir, "%s/dashboards", app.data_dir);
    if (dir_make(app.dash_dir) != 0) {
        log_error("dossier de données inutilisable : %s (%s)", app.dash_dir, plat_strerror());
        return 1;
    }
    if (!path_is_file(app.web_dir) && !path_is_dir(app.web_dir))
        log_error("dossier du front web introuvable : %s (seule l'API répondra)", app.web_dir);

    net_init();
    plat_on_quit(on_quit);

    http_handlers h = {&app, on_request, on_ws_open, on_ws_text};
    char err[512];
    app.srv = http_listen(host, port, &h, err, sizeof err);
    if (!app.srv) {
        log_error("%s", err);
        return 1;
    }
    app.hub = hub_new();
    calc_load(&app);

    char cerr[256];
    app.config = config_load(app.data_dir, cerr, sizeof cerr);
    if (cerr[0]) {
        /* configuration illisible : aucune source démarrée (surtout pas le simulateur par défaut) */
        char msg[512];
        snprintf(msg, sizeof msg,
                 "config.json illisible (%s) — aucune connexion. Corriger le fichier ou reconfigurer la connexion.", cerr);
        hub_report_error(app.hub, msg);
    } else {
        hub_apply(app.hub, app.config);
    }
    log_info("ADS HMI service %s : http://%s:%d  (données : %s)", VERSION,
             strcmp(host, "0.0.0.0") ? host : "localhost", port, app.data_dir);

    while (!pf_get(&g_quit)) {
        http_poll(app.srv, 10);
        hub_poll(app.hub, broadcast, &app);
    }

    log_info("arrêt demandé");
    hub_free(app.hub);
    http_free(app.srv);
    cJSON_Delete(app.config);
    sb_free(&app.resync);
    return 0;
}
