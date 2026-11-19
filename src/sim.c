/*
 * Simulateur de procédé. Produit un bloc binaire identique à celui d'un automate (offsets, types,
 * alignement TwinCAT) : le décodage testé avec le simulateur est exactement celui utilisé en ADS.
 *
 * Procédé simulé : machine avec convoyeur, four (8 zones), cuve, 3 axes, presse d'emmanchement,
 * alarme aléatoire, commandes opérateur (marche, acquittement, consignes, mode).
 */
#include "codec.h"
#include "hub.h"
#include "plat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT_PATH "SIM.stHmi"

typedef struct {
    source_t base;
    hub_t *hub;
    int cycle_ms;
    /** protège buf (calcul du cycle et écritures de l'interface) */
    plat_mutex lock;
    plat_thread thread;
    plat_flag running;
    int started;
    /** description propre au simulateur (accès aux variables par chemin) */
    roots_t *roots;
    leaf_list leaves;
    /** « mémoire automate » simulée */
    uint8_t *buf;
    /** copie publiée au hub (thread du simulateur uniquement) */
    uint8_t *pub;
    size_t len;
    /** fraction de pièce accumulée (compteur de production) */
    double acc;
    /** cycle de presse en cours au pas précédent (détection de front montant) */
    int press_cycle;
    uint64_t rng;
} sim_t;

static const enum_entry STATES[] = {{"Arret", 0}, {"Init", 1}, {"Production", 2}, {"Defaut", 3}};
static const enum_entry MODES[] = {{"Manu", 0}, {"Auto", 1}, {"Maintenance", 2}};

/** Structure simulée ST_Hmi (mêmes noms que les écrans de démonstration). */
static roots_t *build_roots(void)
{
    roots_t *r = roots_new();
    arena_t *a = r->arena;
    member_spec axe[] = {
        {"rPos", tn_leaf(a, "LREAL", "mm"), "Position"},
        {"rVel", tn_leaf(a, "LREAL", "mm/s"), "Vitesse"},
        {"bReady", tn_leaf(a, "BOOL", NULL), "Axe prêt"},
        {"bError", tn_leaf(a, "BOOL", NULL), "Axe en défaut"},
    };
    member_spec presse[] = {
        {"bCycle", tn_leaf(a, "BOOL", NULL), "Cycle d'emmanchement en cours"},
        {"rPosition", tn_leaf(a, "LREAL", "mm"), "Position vérin"},
        {"rForce", tn_leaf(a, "LREAL", "kN"), "Force d'emmanchement"},
        {"nCycles", tn_leaf(a, "UDINT", NULL), "Nombre de cycles"},
    };
    member_spec cmd[] = {
        {"bMarche", tn_leaf(a, "BOOL", NULL), "Commande marche"},
        {"bAcquit", tn_leaf(a, "BOOL", NULL), "Acquittement défaut"},
        {"rConsigneVitesse", tn_leaf(a, "REAL", "m/min"), "Consigne vitesse"},
        {"rConsigneTemp", tn_leaf(a, "REAL", "°C"), "Consigne température"},
        {"nMode", tn_leaf(a, "INT", NULL), "Mode (0=manu, 1=auto)"},
    };
    member_spec root[] = {
        {"bRun", tn_leaf(a, "BOOL", NULL), "Machine en marche"},
        {"bAlarm", tn_leaf(a, "BOOL", NULL), "Alarme active"},
        {"eState", tn_enum(a, "INT", "E_State", STATES, 4), "État machine"},
        {"rTemperature", tn_leaf(a, "REAL", "°C"), "Température four"},
        {"rPression", tn_leaf(a, "REAL", "bar"), "Pression circuit"},
        {"rNiveau", tn_leaf(a, "REAL", "%"), "Niveau cuve"},
        {"rVitesse", tn_leaf(a, "REAL", "m/min"), "Vitesse convoyeur"},
        {"nCompteur", tn_leaf(a, "UDINT", NULL), "Pièces produites"},
        {"sMessage", tn_leaf(a, "STRING(80)", NULL), "Message opérateur"},
        {"aTemp", tn_array(a, tn_leaf(a, "REAL", "°C"), 1, 8), "Températures zones"},
        {"aAxes", tn_array(a, tn_struct(a, "ST_Axe", axe, 4), 0, 3), NULL},
        {"stCmd", tn_struct(a, "ST_Cmd", cmd, 5), NULL},
        {"wDefauts", tn_leaf(a, "WORD", NULL),
         "Mot de défauts (bit 0 alarme, 1 surchauffe, 2 niveau haut, 3 niveau bas, 4 surpression)"},
        {"bVanne", tn_leaf(a, "BOOL", NULL), "Vanne d'alimentation ouverte"},
        {"bPompe", tn_leaf(a, "BOOL", NULL), "Pompe de circulation en marche"},
        {"eMode", tn_enum(a, "INT", "E_Mode", MODES, 3), "Mode de marche"},
        {"stPresse", tn_struct(a, "ST_Presse", presse, 4), NULL},
    };
    roots_add(r, ROOT_PATH, tn_struct(a, "ST_Hmi", root, (int)(sizeof root / sizeof root[0])));
    return r;
}

/* ------------------------------------------------- accès aux variables simulées */

/** variable par chemin relatif à la racine (ex. "stCmd.bMarche") ; erreur de programmation si absente */
static const leaf_t *L(sim_t *s, const char *rel)
{
    char p[160];
    snprintf(p, sizeof p, ROOT_PATH ".%s", rel);
    for (int i = 0; i < s->leaves.n; i++)
        if (str_ieq(s->leaves.v[i].path, p)) return &s->leaves.v[i];
    log_error("simulateur : variable interne inconnue %s", p);
    abort();
}

static double G(sim_t *s, const char *rel) { return codec_num(L(s, rel), s->buf, s->len); }
static int B(sim_t *s, const char *rel) { return G(s, rel) != 0; }

static void S(sim_t *s, const char *rel, double v)
{
    const leaf_t *l = L(s, rel);
    uint8_t tmp[16];
    char err[160];
    int n = codec_encode_num(l, v, tmp, sizeof tmp, err, sizeof err);
    if (n > 0) codec_put(l, s->buf, s->len, tmp, n);
}

static void SS(sim_t *s, const char *rel, const char *text)
{
    const leaf_t *l = L(s, rel);
    uint8_t tmp[256];
    char err[160];
    int n = codec_encode_str(l, text, tmp, sizeof tmp, err, sizeof err);
    if (n > 0) codec_put(l, s->buf, s->len, tmp, n);
}

/** nombre pseudo-aléatoire dans [0, 1) (xorshift64*) */
static double rnd(sim_t *s)
{
    s->rng ^= s->rng >> 12;
    s->rng ^= s->rng << 25;
    s->rng ^= s->rng >> 27;
    return (double)((s->rng * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

static double clamp1(double x) { return x < 1 ? x : 1; }

/* ------------------------------------------------- un pas de simulation */

/** `dt` : durée depuis le pas précédent (s) ; `t` : temps depuis le démarrage (s). Verrou détenu. */
static void step(sim_t *s, double dt, double t)
{
    char rel[32];
    /* acquittement : efface l'alarme, puis l'ordre lui-même (comme un front montant traité par le PLC) */
    if (B(s, "stCmd.bAcquit")) {
        S(s, "bAlarm", 0);
        S(s, "stCmd.bAcquit", 0);
    }
    /* marche si commandée et sans alarme ; vitesse et température suivent leur consigne (premier ordre + bruit) */
    int run = B(s, "stCmd.bMarche") && !B(s, "bAlarm");
    S(s, "bRun", run);
    double csg_v = G(s, "stCmd.rConsigneVitesse"), csg_t = G(s, "stCmd.rConsigneTemp");
    double v = G(s, "rVitesse");
    v += ((run ? csg_v : 0) - v) * clamp1(dt * 1.5);
    S(s, "rVitesse", v);
    double temp = G(s, "rTemperature");
    temp += ((run ? csg_t : 20) - temp) * clamp1(dt * 0.15) + (rnd(s) - 0.5) * 0.4;
    S(s, "rTemperature", temp);
    S(s, "rPression", 1 + v / 20 + sin(t * 2) * 0.15 + (rnd(s) - 0.5) * 0.05);
    S(s, "rNiveau", 50 + 40 * sin(t / 8));
    for (int i = 1; i <= 8; i++) {
        snprintf(rel, sizeof rel, "aTemp[%d]", i);
        double x = G(s, rel);
        S(s, rel, x + ((temp - (i - 1) * 6) - x) * clamp1(dt * 0.3) + (rnd(s) - 0.5) * 0.3);
    }
    if (run) {
        s->acc += dt * v / 10;
        double n = G(s, "nCompteur");
        while (s->acc >= 1) { n++; s->acc -= 1; }
        S(s, "nCompteur", n);
    }
    for (int i = 0; i < 3; i++) {
        char rp[32], rv[32];
        snprintf(rp, sizeof rp, "aAxes[%d].rPos", i);
        snprintf(rv, sizeof rv, "aAxes[%d].rVel", i);
        double pos = G(s, rp);
        double next = run ? 250 + 200 * sin(t * (0.6 + i * 0.25)) : pos;
        S(s, rv, (next - pos) / (dt > 1e-3 ? dt : 1e-3));
        S(s, rp, next);
    }
    /* alarme aléatoire : en moyenne une toutes les 90 s de marche */
    if (run && rnd(s) < dt / 90) S(s, "bAlarm", 1);
    /* vanne, pompe, mot de défauts */
    S(s, "bVanne", run);
    double niv = G(s, "rNiveau");
    S(s, "bPompe", run && niv < 85);
    int w = (B(s, "bAlarm") ? 1 : 0) | (temp > 200 ? 2 : 0) | (niv > 85 ? 4 : 0) | (niv < 15 ? 8 : 0) |
            (G(s, "rPression") > 3 ? 16 : 0);
    S(s, "wDefauts", w);
    /* presse : un cycle de 3 s toutes les 6 s quand la machine tourne (descente 2 s, remontée 1 s) */
    double ph = fmod(t, 6);
    int cycle = run && ph < 3;
    double p_pos = !cycle ? 0 : ph < 2 ? 100 * ph / 2 : 100 * (3 - ph);
    double force = cycle && ph < 2 && p_pos > 40 ? 50 * pow((p_pos - 40) / 60, 1.6) + (rnd(s) - 0.5) * 1.2 : 0;
    if (cycle && !s->press_cycle) S(s, "stPresse.nCycles", G(s, "stPresse.nCycles") + 1);
    s->press_cycle = cycle;
    S(s, "stPresse.bCycle", cycle && ph < 2);
    S(s, "stPresse.rPosition", p_pos);
    S(s, "stPresse.rForce", force > 0 ? force : 0);
    int state = B(s, "bAlarm") ? 3 : run ? (v < csg_v * 0.95 ? 1 : 2) : 0;
    static const char *msgs[] = {"Machine à l'arrêt", "Montée en vitesse", "Production en cours", "DÉFAUT : acquitter"};
    S(s, "eState", state);
    SS(s, "sMessage", msgs[state]);
}

/* ------------------------------------------------- thread et interface source */

/** publie une copie du bloc (copie sous verrou, notification hors verrou) */
static void publish(sim_t *s)
{
    pm_lock(&s->lock);
    memcpy(s->pub, s->buf, s->len);
    pm_unlock(&s->lock);
    hub_source_data(s->hub, 0, s->pub, s->len);
}

static void sim_thread(void *arg)
{
    sim_t *s = arg;
    double t0 = mono_ms(), last = t0, next = t0;
    publish(s);
    while (pf_get(&s->running)) {
        next += s->cycle_ms;
        double now = mono_ms();
        if (next > now) sleep_ms((int)(next - now + 0.5));
        else next = now; /* retard (machine chargée) : pas de rattrapage en rafale */
        if (!pf_get(&s->running)) break;
        now = mono_ms();
        pm_lock(&s->lock);
        step(s, (now - last) / 1000.0, (now - t0) / 1000.0);
        pm_unlock(&s->lock);
        last = now;
        publish(s);
    }
}

static int sim_start(source_t *base, char *err, size_t cap)
{
    sim_t *s = (sim_t *)base;
    s->roots = build_roots();
    leaves_compile(s->roots, 1000, &s->leaves);
    s->len = (size_t)s->roots->v[0].node->size;
    s->buf = xcalloc(s->len, 1);
    s->pub = xcalloc(s->len, 1);
    S(s, "stCmd.rConsigneVitesse", 30);
    S(s, "stCmd.rConsigneTemp", 180);
    S(s, "stCmd.nMode", 1);
    S(s, "rTemperature", 20);
    S(s, "rPression", 1);
    char rel[32];
    for (int i = 1; i <= 8; i++) {
        snprintf(rel, sizeof rel, "aTemp[%d]", i);
        S(s, rel, 20);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(rel, sizeof rel, "aAxes[%d].bReady", i);
        S(s, rel, 1);
    }
    SS(s, "sMessage", "Machine à l'arrêt");
    S(s, "eMode", 1);

    /* le hub reçoit sa propre copie de la description (il en devient propriétaire) */
    hub_source_roots(s->hub, build_roots());
    hub_source_status(s->hub, ST_CONNECTED, "Simulateur actif");
    pf_set(&s->running, 1);
    if (pt_start(&s->thread, sim_thread, s) != 0) {
        pf_set(&s->running, 0);
        snprintf(err, cap, "Simulateur : impossible de démarrer le thread");
        return -1;
    }
    s->started = 1;
    return 0;
}

static void sim_stop(source_t *base)
{
    sim_t *s = (sim_t *)base;
    if (!s->started) return;
    pf_set(&s->running, 0);
    pt_join(s->thread);
    s->started = 0;
}

static int sim_write(source_t *base, const leaf_t *leaf, const uint8_t *data, int len, char *err, size_t cap)
{
    sim_t *s = (sim_t *)base;
    if (leaf->root != 0 || leaf->offset < 0 || (size_t)leaf->offset + (size_t)(leaf->size > 0 ? leaf->size : 1) > s->len) {
        snprintf(err, cap, "Variable hors de la mémoire simulée : %s", leaf->path);
        return -1;
    }
    /* écriture immédiate dans la mémoire simulée ; visible à la publication suivante */
    pm_lock(&s->lock);
    codec_put(leaf, s->buf, s->len, data, len);
    pm_unlock(&s->lock);
    return 0;
}

static void sim_destroy(source_t *base)
{
    sim_t *s = (sim_t *)base;
    sim_stop(base);
    leaves_free(&s->leaves);
    roots_free(s->roots);
    free(s->buf);
    free(s->pub);
    pm_destroy(&s->lock);
    free(s);
}

source_t *sim_new(hub_t *h, int cycle_ms)
{
    sim_t *s = xcalloc(1, sizeof *s);
    s->base.start = sim_start;
    s->base.stop = sim_stop;
    s->base.write = sim_write;
    s->base.destroy = sim_destroy;
    s->hub = h;
    s->cycle_ms = cycle_ms < 5 ? 5 : cycle_ms;
    s->rng = (uint64_t)wall_ms() * 6364136223846793005ULL + 1442695040888963407ULL;
    if (!s->rng) s->rng = 1;
    pm_init(&s->lock);
    pf_set(&s->running, 0);
    return &s->base;
}
