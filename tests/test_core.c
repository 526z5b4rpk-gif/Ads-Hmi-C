/*
 * Tests unitaires du cœur : texte/JSON, disposition mémoire, mise à plat, codec, hub, variables calculées, WebSocket.
 * Compilés avec AddressSanitizer et UBSan (make unit) : tout débordement ou fuite fait échouer le test.
 */
#include "calc.h"
#include "codec.h"
#include "http.h"
#include "hub.h"
#include "plat.h"
#include "store.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_count = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_count++;                                                                     \
        if (!(cond)) {                                                                 \
            g_fail++;                                                                  \
            fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        g_count++;                                                                     \
        const char *a_ = (a), *b_ = (b);                                               \
        if (strcmp(a_, b_) != 0) {                                                     \
            g_fail++;                                                                  \
            fprintf(stderr, "ÉCHEC %s:%d : « %s » au lieu de « %s »\n", __FILE__, __LINE__, a_, b_); \
        }                                                                              \
    } while (0)

static const char *num(double v, int single)
{
    static sb_t b;
    sb_clear(&b);
    sb_json_num(&b, v, single);
    return sb_cstr(&b);
}

static void test_json_text(void)
{
    CHECK_STR(num(1, 0), "1");
    CHECK_STR(num(-42, 0), "-42");
    CHECK_STR(num(0.1, 0), "0.1");
    CHECK_STR(num((float)20.4, 1), "20.4");
    CHECK_STR(num(1e20, 0), "1e+20");
    CHECK_STR(num(NAN, 0), "null");
    CHECK_STR(num(INFINITY, 1), "null");
    sb_t b = {0};
    sb_json_str(&b, "a\"b\\c\n\x01é", (size_t)-1);
    CHECK_STR(sb_cstr(&b), "\"a\\\"b\\\\c\\n\\u0001é\"");
    sb_free(&b);
    CHECK(parse_number_lenient(" 12,5 ") == 12.5);
    CHECK(parse_number_lenient("vrai") == 1);
    CHECK(parse_number_lenient("FAUX") == 0);
    CHECK(isnan(parse_number_lenient("12abc")));
    CHECK(isnan(parse_number_lenient("")));

    /* Latin-1 <-> UTF-8 */
    uint8_t lat[32];
    size_t n = latin1_from_utf8("Température °C €", lat, sizeof lat);
    CHECK(n == 16 && lat[4] == 0xE9 && lat[15] == '?');
    sb_t u = {0};
    utf8_from_latin1(&u, lat, 14);
    CHECK_STR(sb_cstr(&u), "Température °C");
    sb_free(&u);
    /* UTF-16 avec paire de substitution */
    uint8_t w[32];
    size_t wn = utf16le_from_utf8("é😀", w, sizeof w);
    CHECK(wn == 6);
    sb_t u2 = {0};
    utf8_from_utf16le(&u2, w, wn);
    CHECK_STR(sb_cstr(&u2), "é😀");
    sb_free(&u2);
    /* une paire n'est jamais coupée */
    CHECK(utf16le_from_utf8("a😀", w, 4) == 2);
}

static const leaf_t *find(const leaf_list *l, const char *path)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].path, path)) return &l->v[i];
    return NULL;
}

static void test_layout(void)
{
    roots_t *r = roots_new();
    arena_t *a = r->arena;
    member_spec axe[] = {
        {"rPos", tn_leaf(a, "LREAL", "mm"), "Position"},
        {"rVel", tn_leaf(a, "LREAL", NULL), NULL},
        {"bReady", tn_leaf(a, "BOOL", NULL), NULL},
        {"bError", tn_leaf(a, "BOOL", NULL), NULL},
    };
    type_node *t_axe = tn_struct(a, "ST_Axe", axe, 4);
    CHECK(t_axe->size == 24);
    CHECK(t_axe->members[2].offset == 16 && t_axe->members[3].offset == 17);
    member_spec m[] = {
        {"b", tn_leaf(a, "BOOL", NULL), NULL},
        {"i", tn_leaf(a, "INT", NULL), NULL},
        {"r", tn_leaf(a, "REAL", NULL), NULL},
        {"s", tn_leaf(a, "STRING(80)", NULL), NULL},
        {"l", tn_leaf(a, "LREAL", NULL), NULL},
        {"ax", tn_array(a, t_axe, 0, 3), "Axes"},
        {"t", tn_array(a, tn_leaf(a, "REAL", "°C"), 1, 8), NULL},
    };
    type_node *root = tn_struct(a, "ST_T", m, 7);
    CHECK(root->members[1].offset == 2);
    CHECK(root->members[2].offset == 4);
    CHECK(root->members[3].offset == 8);
    CHECK(root->members[4].offset == 96); /* 8 + 81 = 89 -> aligné sur 8 */
    CHECK(root->members[5].offset == 104);
    CHECK(root->members[6].offset == 176);
    CHECK(root->size == 208);
    CHECK(tn_leaf(a, "INCONNU", NULL) == NULL);
    roots_add(r, "GVL.st", root);

    leaf_list l;
    leaves_compile(r, 1000, &l);
    CHECK(l.n == 5 + 3 * 4 + 8);
    const leaf_t *x = find(&l, "GVL.st.ax[2].bError");
    CHECK(x && x->offset == 104 + 2 * 24 + 17 && x->prim == PR_BOOL && x->kind == LK_BOOL);
    /* commentaire du membre parent hérité, attribut du type conservé */
    const leaf_t *p = find(&l, "GVL.st.ax[1].rPos");
    CHECK(p && p->comment && !strcmp(p->comment, "Position") && p->nattrs == 1 && !strcmp(p->attrs[0].value, "mm"));
    const leaf_t *t8 = find(&l, "GVL.st.t[8]");
    CHECK(t8 && t8->offset == 176 + 7 * 4 && t8->index == l.n - 1);
    leaves_free(&l);

    /* limite d'expansion des tableaux */
    leaves_compile(r, 2, &l);
    CHECK(l.n == 5 + 2 * 4 + 2);
    leaves_free(&l);
    roots_free(r);

    /* tableau à deux dimensions et membre BIT construits à la main (comme le fera la source ADS) */
    r = roots_new();
    a = r->arena;
    type_node *arr = tn_array(a, tn_leaf(a, "INT", NULL), 0, 6);
    arr->dims = arena_alloc(a, 2 * sizeof(array_dim));
    arr->dims[0] = (array_dim){1, 2};
    arr->dims[1] = (array_dim){0, 3};
    arr->ndims = 2;
    member_spec m2[] = {{"a", arr, NULL}, {"f", tn_leaf(a, "BOOL", NULL), NULL}};
    type_node *st = tn_struct(a, "ST_2", m2, 2);
    st->members[1].bit_offset = 12 * 8 + 5; /* bit 5 de l'octet 12 */
    roots_add(r, "R", st);
    leaves_compile(r, 100, &l);
    CHECK(l.n == 7);
    CHECK(!strcmp(l.v[0].path, "R.a[1,0]") && !strcmp(l.v[3].path, "R.a[2,0]") && l.v[3].offset == 6);
    CHECK(l.v[6].prim == PR_BIT && l.v[6].offset == 12 && l.v[6].bit == 5);
    leaves_free(&l);
    roots_free(r);
}

/** variable isolée de type `t` à l'offset 0 */
static leaf_t mk(arena_t *a, const char *t)
{
    type_node *n = tn_leaf(a, t, NULL);
    leaf_t l;
    memset(&l, 0, sizeof l);
    l.path = "X";
    l.type = n->type_name;
    l.kind = n->lkind;
    l.prim = n->prim;
    l.size = n->size;
    return l;
}

static const char *json_of(const leaf_t *l, const uint8_t *b, size_t n)
{
    static sb_t s;
    sb_clear(&s);
    codec_json(l, b, n, &s);
    return sb_cstr(&s);
}

static void test_codec(void)
{
    arena_t *a = arena_new();
    uint8_t buf[256], enc[256];
    char err[256];

    leaf_t i16 = mk(a, "INT");
    CHECK(codec_encode_num(&i16, -1234, enc, sizeof enc, err, sizeof err) == 2);
    memset(buf, 0, sizeof buf);
    codec_put(&i16, buf, sizeof buf, enc, 2);
    CHECK(buf[0] == 0x2E && buf[1] == 0xFB);
    CHECK_STR(json_of(&i16, buf, sizeof buf), "-1234");
    CHECK(codec_encode_num(&i16, 40000, enc, sizeof enc, err, sizeof err) == -1);
    CHECK(strstr(err, "hors limites") != NULL);
    CHECK(codec_encode_num(&i16, 12.6, enc, sizeof enc, err, sizeof err) == 2 && enc[0] == 13);

    leaf_t u8 = mk(a, "USINT");
    CHECK(codec_encode_num(&u8, -1, enc, sizeof enc, err, sizeof err) == -1);
    leaf_t u32 = mk(a, "UDINT");
    CHECK(codec_encode_num(&u32, 4294967295.0, enc, sizeof enc, err, sizeof err) == 4);
    codec_put(&u32, buf, sizeof buf, enc, 4);
    CHECK_STR(json_of(&u32, buf, sizeof buf), "4294967295");

    leaf_t r32 = mk(a, "REAL");
    cJSON *v = cJSON_Parse("\"20,4\"");
    CHECK(codec_encode(&r32, v, enc, sizeof enc, err, sizeof err) == 4);
    cJSON_Delete(v);
    codec_put(&r32, buf, sizeof buf, enc, 4);
    CHECK_STR(json_of(&r32, buf, sizeof buf), "20.4");
    CHECK(codec_encode_num(&r32, 1e300, enc, sizeof enc, err, sizeof err) == -1);

    leaf_t r64 = mk(a, "LREAL");
    CHECK(codec_encode_num(&r64, 0.1, enc, sizeof enc, err, sizeof err) == 8);
    codec_put(&r64, buf, sizeof buf, enc, 8);
    CHECK_STR(json_of(&r64, buf, sizeof buf), "0.1");
    /* NaN dans l'automate -> null */
    memset(buf, 0xFF, 8);
    CHECK_STR(json_of(&r64, buf, sizeof buf), "null");

    leaf_t b = mk(a, "BOOL");
    v = cJSON_Parse("true");
    CHECK(codec_encode(&b, v, enc, sizeof enc, err, sizeof err) == 1 && enc[0] == 1);
    cJSON_Delete(v);
    v = cJSON_Parse("\"vrai\"");
    CHECK(codec_encode(&b, v, enc, sizeof enc, err, sizeof err) == 1 && enc[0] == 1);
    cJSON_Delete(v);
    v = cJSON_Parse("{}");
    CHECK(codec_encode(&b, v, enc, sizeof enc, err, sizeof err) == -1);
    cJSON_Delete(v);

    /* bit : seul le bit visé change */
    leaf_t bit = b;
    bit.prim = PR_BIT;
    bit.offset = 3;
    bit.bit = 6;
    memset(buf, 0x0F, sizeof buf);
    enc[0] = 1;
    codec_put(&bit, buf, sizeof buf, enc, 1);
    CHECK(buf[3] == 0x4F && buf[2] == 0x0F);
    CHECK_STR(json_of(&bit, buf, sizeof buf), "true");

    /* chaîne : Latin-1, troncature avec terminateur conservé */
    leaf_t s = mk(a, "STRING(5)");
    CHECK(s.size == 6);
    CHECK(codec_encode_str(&s, "Été 2026", enc, sizeof enc, err, sizeof err) == 6);
    CHECK(enc[0] == 0xC9 && enc[5] == 0);
    memset(buf, 0, sizeof buf);
    codec_put(&s, buf, sizeof buf, enc, 6);
    CHECK_STR(json_of(&s, buf, sizeof buf), "\"Été 2\"");
    leaf_t ws = mk(a, "WSTRING(4)");
    CHECK(codec_encode_str(&ws, "aé😀b", enc, sizeof enc, err, sizeof err) == 10);
    codec_put(&ws, buf, sizeof buf, enc, 10);
    CHECK_STR(json_of(&ws, buf, sizeof buf), "\"aé😀\"");

    /* énumération : nom accepté */
    static const enum_entry E[] = {{"Arret", 0}, {"Production", 2}};
    type_node *en = tn_enum(a, "INT", "E_State", E, 2);
    leaf_t e = mk(a, "INT");
    e.kind = en->lkind;
    e.enums = en->enums;
    e.nenums = en->nenums;
    CHECK(codec_encode_str(&e, "production", enc, sizeof enc, err, sizeof err) == 2 && enc[0] == 2);
    CHECK(codec_encode_str(&e, "Inconnu", enc, sizeof enc, err, sizeof err) == -1);

    /* hors du bloc */
    leaf_t hors = i16; /* pas « far » : macro de windows.h */
    hors.offset = 255;
    CHECK_STR(json_of(&hors, buf, sizeof buf), "null");
    CHECK(!codec_same(&hors, buf, sizeof buf, buf, sizeof buf));
    CHECK(codec_same(&i16, buf, sizeof buf, buf, sizeof buf));
    arena_free(a);
}

/* ---- hub : messages capturés ---- */

typedef struct {
    sb_t all;
    int count, deltas;
} capture;

static void cap_emit(void *ctx, const char *json, size_t len, int is_delta)
{
    capture *c = ctx;
    sb_put(&c->all, json, len);
    sb_putc(&c->all, '\n');
    c->count++;
    c->deltas += is_delta;
}

static void test_hub(void)
{
    hub_t *h = hub_new();
    roots_t *r = roots_new();
    member_spec m[] = {
        {"bOn", tn_leaf(r->arena, "BOOL", NULL), "Marche"},
        {"nCnt", tn_leaf(r->arena, "UDINT", NULL), NULL},
        {"rVal", tn_leaf(r->arena, "REAL", "bar"), NULL},
    };
    roots_add(r, "GVL.st", tn_struct(r->arena, "ST", m, 3));
    hub_source_roots(h, r);
    CHECK(hub_leaf_count(h) == 3);

    capture c = {{0}, 0, 0};
    hub_poll(h, cap_emit, &c);
    CHECK(strstr(sb_cstr(&c.all), "\"t\":\"schema\"") != NULL);
    CHECK(strstr(c.all.p, "\"values\":[null,null,null]") != NULL);
    CHECK(strstr(c.all.p, "\"attrs\":{\"unit\":\"bar\"}") != NULL);
    CHECK(strstr(c.all.p, "\"comment\":\"Marche\"") != NULL);

    uint8_t blk[12] = {0};
    blk[0] = 1;
    blk[4] = 7;
    hub_source_data(h, 0, blk, sizeof blk);
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    CHECK(c.deltas == 1);
    CHECK(strstr(sb_cstr(&c.all), "\"v\":[0,true,1,7,2,0]") != NULL);

    /* seul nCnt change -> delta d'une seule variable */
    blk[4] = 8;
    hub_source_data(h, 0, blk, sizeof blk);
    hub_source_data(h, 0, blk, sizeof blk);
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    CHECK(strstr(sb_cstr(&c.all), "\"v\":[1,8]") != NULL);

    /* aucun changement -> aucun delta */
    hub_source_data(h, 0, blk, sizeof blk);
    int before = c.deltas;
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    CHECK(c.deltas == before);

    /* racine inconnue ignorée */
    hub_source_data(h, 5, blk, sizeof blk);

    char err[256];
    cJSON *v = cJSON_Parse("1");
    CHECK(hub_write(h, "GVL.st.inconnue", v, err, sizeof err) == -1 && strstr(err, "Variable inconnue"));
    CHECK(hub_write(h, "  gvl.ST.ncnt ", v, err, sizeof err) == -1 && !strcmp(err, "Aucune source active"));
    cJSON_Delete(v);
    v = cJSON_Parse("\"abc\"");
    CHECK(hub_write(h, "GVL.st.nCnt", v, err, sizeof err) == -1 && strstr(err, "Valeur invalide"));
    cJSON_Delete(v);

    /* configuration ADS (pas encore disponible) : schéma vidé, erreur explicite */
    cJSON *cfg = cJSON_Parse("{\"source\":\"ads\"}");
    hub_apply(h, cfg);
    cJSON_Delete(cfg);
    CHECK(hub_leaf_count(h) == 0);
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    CHECK(strstr(sb_cstr(&c.all), "\"state\":\"error\",\"source\":\"ads\"") != NULL);

    sb_free(&c.all);
    hub_free(h);
}

/* ---- variables calculées : moteur d'expressions ---- */

static double calc_vals[4];
static double get_val(void *ctx, int i) { (void)ctx; return i >= 0 && i < 4 ? calc_vals[i] : NAN; }

/** évalue une expression sans variable (ou avec calc_vals) ; NaN si erreur de syntaxe */
static double ev(const char *expr)
{
    char err[200];
    calc_prog *p = calc_compile(expr, err, sizeof err);
    if (!p) return -12345.678;
    double v = calc_eval(p, get_val, NULL);
    calc_free(p);
    return v;
}

static int syntax_error(const char *expr, const char *expected)
{
    char err[200];
    calc_prog *p = calc_compile(expr, err, sizeof err);
    if (p) { calc_free(p); return 0; }
    if (!strstr(err, expected)) { fprintf(stderr, "  message : %s\n", err); return 0; }
    return 1;
}

static void test_calc(void)
{
    /* priorités et associativité */
    CHECK(ev("1 + 2 * 3") == 7);
    CHECK(ev("(1 + 2) * 3") == 9);
    CHECK(ev("2 ^ 3 ^ 2") == 512);
    CHECK(ev("-2 ^ 2") == -4);
    CHECK(ev("2 ** -1") == 0.5);
    CHECK(ev("7 mod 4 + 7 % 4") == 6);
    CHECK(ev("10 - 4 - 3") == 3);
    CHECK(ev("1e3 / 4") == 250);
    CHECK(ev(".5 + PI - pi") == 0.5);
    /* comparaisons et logique (syntaxe ST et C) */
    CHECK(ev("3 > 2 AND 2 >= 2 and not (1 = 2)") == 1);
    CHECK(ev("1 <> 1 || 2 != 2 OR 1 == 1") == 1);
    CHECK(ev("TRUE XOR TRUE") == 0);
    CHECK(ev("!0 && !FALSE") == 1);
    CHECK(ev("1 < 2 ? 10 : 20") == 10);
    CHECK(ev("0 ? 1 : 0 ? 2 : 3") == 3);
    /* fonctions */
    CHECK(ev("round(2.5) + round(-2.5)") == 0);
    CHECK(ev("round(3.14159, 2)") == 3.14);
    CHECK(ev("min(4, 2, 8) + max(4, 2, 8) + sum(1, 2, 3) + avg(2, 4)") == 19);
    CHECK(ev("clamp(150, 0, 100) + limit(0, -5, 100)") == 100);
    CHECK(ev("if(1, 5, 6) + abs(-3) + sign(-9) + trunc(-2.7) + floor(2.7) + ceil(2.1)") == 10);
    CHECK(ev("bit(4, 2) + bit(4, 1) + bit(65535, 15)") == 2);
    CHECK(ev("sqrt(16) + pow(2, 10) + log10(1000) + ln(1)") == 1031);
    /* valeurs invalides : propagées, sauf logique à trois états */
    CHECK(isnan(ev("1 / 0")));
    CHECK(isnan(ev("5 mod 0")));
    CHECK(isnan(ev("sqrt(-1)")));
    calc_vals[0] = NAN;
    calc_vals[1] = 2;
    CHECK(isnan(ev("A.x + 1")));
    CHECK(isnan(ev("A.x > 1")));
    CHECK(ev("FALSE AND A.x") == 0);
    CHECK(ev("TRUE OR A.x") == 1);
    CHECK(isnan(ev("TRUE AND A.x")));
    CHECK(isnan(ev("A.x ? 1 : 2")));
    /* références : chemins, tableaux, noms courts, sans doublon */
    char err[200];
    calc_prog *p = calc_compile("GVL.a[1, 2].b + gvl.A[1,2].B * rAutre + MAIN.x", err, sizeof err);
    CHECK(p != NULL);
    if (p) {
        CHECK(calc_nrefs(p) == 3);
        CHECK_STR(calc_ref(p, 0), "GVL.a[1,2].b");
        CHECK_STR(calc_ref(p, 1), "CALC.rAutre");
        CHECK_STR(calc_ref(p, 2), "MAIN.x");
        CHECK(!calc_is_bool(p));
        calc_vals[0] = 1; calc_vals[1] = 2; calc_vals[2] = 3;
        CHECK(calc_eval(p, get_val, NULL) == 6);
        calc_free(p);
    }
    p = calc_compile("GVL.a > 1 AND GVL.b", err, sizeof err);
    CHECK(p && calc_is_bool(p));
    calc_free(p);
    p = calc_compile("if(GVL.a, TRUE, 3 > 2)", err, sizeof err);
    CHECK(p && calc_is_bool(p));
    calc_free(p);
    /* erreurs de syntaxe, avec position */
    CHECK(syntax_error("", "expression vide"));
    CHECK(syntax_error("(1 + 2", "parenthèse ) attendue"));
    CHECK(syntax_error("1 +", "expression incomplète"));
    CHECK(syntax_error("1 2", "opérateur attendu"));
    CHECK(syntax_error("foo(1)", "fonction inconnue : foo"));
    CHECK(syntax_error("round(1, 2, 3)", "round attend de 1 à 2 arguments"));
    CHECK(syntax_error("clamp(1)", "clamp attend 3 arguments"));
    CHECK(syntax_error("1 ? 2", "« : » attendu"));
    CHECK(syntax_error("A.b[x]", "indice de tableau invalide"));
    CHECK(syntax_error("A.b[1", "crochet ] manquant"));
    CHECK(syntax_error("2 $ 3", "caractère inattendu « $ » (position 3)"));
    CHECK(syntax_error("12abc", "nombre mal écrit"));
    /* profondeur bornée : pas de débordement de pile, ni à l'analyse ni à l'évaluation */
    sb_t deep = {0};
    for (int i = 0; i < 200; i++) sb_putc(&deep, '(');
    sb_putc(&deep, '1');
    for (int i = 0; i < 200; i++) sb_putc(&deep, ')');
    CHECK(syntax_error(sb_cstr(&deep), "trop de parenthèses"));
    sb_clear(&deep);
    for (int i = 0; i < 100; i++) sb_puts(&deep, "1+(");
    sb_putc(&deep, '1');
    for (int i = 0; i < 100; i++) sb_putc(&deep, ')');
    CHECK(syntax_error(sb_cstr(&deep), "trop"));
    sb_free(&deep);
}

/* ---- variables calculées dans le hub ---- */

static void test_hub_calc(void)
{
    hub_t *h = hub_new();
    char err[512];
    cJSON *defs = cJSON_Parse("[{\"name\":\"rDouble\",\"expr\":\"GVL.st.rVal * 2\",\"unit\":\"bar\",\"comment\":\"Double\"},"
                              "{\"name\":\"bHaut\",\"expr\":\"rDouble > 10\"},"
                              "{\"name\":\"nArrondi\",\"expr\":\"GVL.st.rVal\",\"type\":\"int\"}]");
    CHECK(hub_set_calcs(h, defs, err, sizeof err) == 0);
    cJSON_Delete(defs);
    CHECK(hub_leaf_count(h) == 3); /* aucune source : variables calculées seules */

    roots_t *r = roots_new();
    member_spec m[] = {
        {"bOn", tn_leaf(r->arena, "BOOL", NULL), NULL},
        {"rVal", tn_leaf(r->arena, "REAL", NULL), NULL},
    };
    roots_add(r, "GVL.st", tn_struct(r->arena, "ST", m, 2));
    hub_source_roots(h, r);
    CHECK(hub_leaf_count(h) == 5);

    capture c = {{0}, 0, 0};
    hub_poll(h, cap_emit, &c);
    const char *s = sb_cstr(&c.all);
    CHECK(strstr(s, "\"roots\":[\"GVL.st\",\"CALC\"]") != NULL);
    CHECK(strstr(s, "{\"i\":2,\"path\":\"CALC.rDouble\",\"type\":\"LREAL\",\"kind\":\"number\",\"comment\":\"Double\","
                    "\"attrs\":{\"unit\":\"bar\"}}") != NULL);
    CHECK(strstr(s, "\"path\":\"CALC.bHaut\",\"type\":\"BOOL\",\"kind\":\"bool\"") != NULL);
    CHECK(strstr(s, "\"path\":\"CALC.nArrondi\",\"type\":\"INT\"") != NULL);
    /* pas encore de données : réel invalide (null), BOOL FAUX, entier 0 */
    CHECK(strstr(s, "\"values\":[null,null,null,false,0]") != NULL);

    uint8_t blk[8] = {0};
    float f = 6.6f;
    memcpy(blk + 4, &f, 4);
    hub_source_data(h, 0, blk, sizeof blk);
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    s = sb_cstr(&c.all);
    CHECK(strstr(s, "\"v\":[0,false,1,6.6,2,13.199999809265137,3,true,4,7]") != NULL);

    /* une variable source inchangée : rien de calculé n'est renvoyé */
    blk[0] = 1;
    hub_source_data(h, 0, blk, sizeof blk);
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    CHECK(strstr(sb_cstr(&c.all), "\"v\":[0,true]") != NULL);

    /* lecture seule */
    cJSON *v = cJSON_Parse("1");
    CHECK(hub_write(h, "calc.rdouble", v, err, sizeof err) == -1 && strstr(err, "lecture seule"));
    cJSON_Delete(v);

    /* définitions refusées : rien n'est modifié */
    const char *bad[][2] = {
        {"[{\"name\":\"a\",\"expr\":\"b\"},{\"name\":\"b\",\"expr\":\"1\"}]", "définie plus bas"},
        {"[{\"name\":\"a\",\"expr\":\"a + 1\"}]", "sa propre valeur"},
        {"[{\"name\":\"a\",\"expr\":\"zz\"}]", "variable calculée inconnue : zz"},
        {"[{\"name\":\"a\",\"expr\":\"1\"},{\"name\":\"A\",\"expr\":\"2\"}]", "nom déjà utilisé"},
        {"[{\"name\":\"1a\",\"expr\":\"1\"}]", "nom invalide"},
        {"[{\"name\":\"and\",\"expr\":\"1\"}]", "nom invalide"},
        {"[{\"name\":\"a\",\"expr\":\"1 +\"}]", "a : expression incomplète"},
        {"[{\"name\":\"a\",\"expr\":\"1\",\"type\":\"STRING\"}]", "non pris en charge"},
        {"{}", "doit être une liste"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cJSON *d = cJSON_Parse(bad[i][0]);
        err[0] = 0;
        int rc = hub_set_calcs(h, d, err, sizeof err);
        CHECK(rc == -1 && strstr(err, bad[i][1]));
        if (rc != -1 || !strstr(err, bad[i][1])) fprintf(stderr, "  cas %d : %s\n", (int)i, err);
        cJSON_Delete(d);
    }
    CHECK(hub_leaf_count(h) == 5);

    /* changement des définitions : les valeurs de la source sont conservées (pas de nouveau bloc reçu) */
    defs = cJSON_Parse("[{\"name\":\"rTriple\",\"expr\":\"GVL.st.rVal * 3 + GVL.inconnue\"},"
                       "{\"name\":\"rVal3\",\"expr\":\"round(GVL.st.rVal * 3, 1)\"}]");
    CHECK(hub_set_calcs(h, defs, err, sizeof err) == 0);
    cJSON_Delete(defs);
    sb_clear(&c.all);
    hub_poll(h, cap_emit, &c);
    s = sb_cstr(&c.all);
    CHECK(strstr(s, "\"values\":[true,6.6,null,19.8]") != NULL);
    sb_t j = {0};
    hub_calcs_json(h, 1, &j);
    CHECK(strstr(sb_cstr(&j), "\"error\":\"variable inconnue : GVL.inconnue\"") != NULL);
    sb_clear(&j);
    hub_calcs_json(h, 0, &j);
    CHECK_STR(sb_cstr(&j), "{\"variables\":[{\"name\":\"rTriple\",\"expr\":\"GVL.st.rVal * 3 + GVL.inconnue\",\"type\":\"auto\"},"
                           "{\"name\":\"rVal3\",\"expr\":\"round(GVL.st.rVal * 3, 1)\",\"type\":\"auto\"}]}");
    sb_clear(&j);
    hub_calc_test(h, "GVL.st.bOn AND rVal3 > 19", &j);
    CHECK_STR(sb_cstr(&j), "{\"value\":true,\"type\":\"BOOL\"}");
    sb_clear(&j);
    hub_calc_test(h, "1 +", &j);
    CHECK(strstr(sb_cstr(&j), "{\"error\":\"expression incomplète") != NULL);
    sb_free(&j);

    /* liste vide : plus de symbole CALC */
    CHECK(hub_set_calcs(h, NULL, err, sizeof err) == 0);
    CHECK(hub_leaf_count(h) == 2);

    sb_free(&c.all);
    hub_free(h);
}

static void test_sim(void)
{
    hub_t *h = hub_new();
    cJSON *cfg = cJSON_Parse("{\"source\":\"sim\",\"sim\":{\"cycleTime\":10}}");
    hub_apply(h, cfg);
    cJSON_Delete(cfg);
    CHECK(hub_leaf_count(h) == 42);
    capture c = {{0}, 0, 0};
    for (int i = 0; i < 10; i++) {
        hub_poll(h, cap_emit, &c);
        sleep_ms(15);
    }
    CHECK(c.deltas >= 3);
    CHECK(strstr(sb_cstr(&c.all), "Machine à l'arrêt") != NULL);

    char err[256];
    cJSON *t = cJSON_Parse("true");
    CHECK(hub_write(h, "SIM.stHmi.stCmd.bMarche", t, err, sizeof err) == 0);
    cJSON_Delete(t);
    sb_clear(&c.all);
    for (int i = 0; i < 10; i++) {
        hub_poll(h, cap_emit, &c);
        sleep_ms(15);
    }
    /* bRun = indice 0 */
    CHECK(strstr(sb_cstr(&c.all), "[0,true") != NULL || strstr(c.all.p, ",0,true") != NULL);
    cJSON *big = cJSON_Parse("99999");
    CHECK(hub_write(h, "SIM.stHmi.stCmd.nMode", big, err, sizeof err) == -1 && strstr(err, "hors limites"));
    cJSON_Delete(big);
    sb_free(&c.all);
    hub_free(h);
}

static void test_ws_key(void)
{
    char acc[32];
    ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc); /* exemple de la RFC 6455 */
    CHECK_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_store(void)
{
    CHECK(store_valid_id("demo_1-a"));
    CHECK(!store_valid_id(""));
    CHECK(!store_valid_id("../x"));
    CHECK(!store_valid_id("a.b"));
    char longid[100];
    memset(longid, 'a', 81);
    longid[81] = 0;
    CHECK(!store_valid_id(longid));
    cJSON *raw = cJSON_Parse("{\"source\":\"ads\",\"ads\":{\"targetAdsPort\":852},\"extra\":1}");
    cJSON *m = config_merge(raw);
    const cJSON *ads = cJSON_GetObjectItem(m, "ads");
    CHECK(cJSON_GetObjectItem(ads, "targetAdsPort")->valuedouble == 852);
    CHECK(cJSON_GetObjectItem(ads, "cycleTime")->valuedouble == 20);
    CHECK(!strcmp(cJSON_GetObjectItem(m, "source")->valuestring, "ads"));
    CHECK(cJSON_GetObjectItem(m, "extra") != NULL);
    cJSON_Delete(raw);
    cJSON_Delete(m);
}

int main(void)
{
    test_json_text();
    test_layout();
    test_codec();
    test_hub();
    test_calc();
    test_hub_calc();
    test_sim();
    test_ws_key();
    test_store();
    printf("%d vérifications, %d échec(s)\n", g_count, g_fail);
    return g_fail ? 1 : 0;
}
