#include "calc.h"
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Seules des fonctions mathématiques C89 sont utilisées (floor, ceil, fmod, pow…) : trunc, round
   et fmin, apparus en C99, sont réécrits ici pour rester compilables avec toutes les bibliothèques C. */

#define MAX_STACK 64
#define MAX_NEST 64
#define MAX_ARGS 32

enum {
    OP_NUM, OP_VAR, OP_NEG, OP_NOT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_AND, OP_OR, OP_XOR, OP_SEL, OP_FN
};

enum {
    F_ABS, F_SQRT, F_EXP, F_LN, F_LOG10, F_SIN, F_COS, F_TAN, F_FLOOR, F_CEIL, F_TRUNC, F_SIGN,
    F_ROUND, F_POW, F_BIT, F_MIN, F_MAX, F_SUM, F_AVG, F_CLAMP, F_LIMIT, F_IF
};

static const struct {
    const char *name;
    int fn, amin, amax, is_bool;
} FUNCS[] = {
    {"abs", F_ABS, 1, 1, 0},     {"sqrt", F_SQRT, 1, 1, 0},       {"exp", F_EXP, 1, 1, 0},
    {"ln", F_LN, 1, 1, 0},       {"log10", F_LOG10, 1, 1, 0},     {"sin", F_SIN, 1, 1, 0},
    {"cos", F_COS, 1, 1, 0},     {"tan", F_TAN, 1, 1, 0},         {"floor", F_FLOOR, 1, 1, 0},
    {"ceil", F_CEIL, 1, 1, 0},   {"trunc", F_TRUNC, 1, 1, 0},     {"sign", F_SIGN, 1, 1, 0},
    {"round", F_ROUND, 1, 2, 0}, {"pow", F_POW, 2, 2, 0},         {"bit", F_BIT, 2, 2, 1},
    {"min", F_MIN, 1, MAX_ARGS, 0}, {"max", F_MAX, 1, MAX_ARGS, 0}, {"sum", F_SUM, 1, MAX_ARGS, 0},
    {"avg", F_AVG, 1, MAX_ARGS, 0}, {"clamp", F_CLAMP, 3, 3, 0},  {"limit", F_LIMIT, 3, 3, 0},
    {"if", F_IF, 3, 3, 0},
};

typedef struct {
    unsigned char op, fn, argc;
    int ref;
    double v;
} instr;

struct calc_prog {
    instr *code;
    int n, cap;
    char **refs;
    int nrefs;
    int is_bool;
};

/* ======================================================================= analyse */

typedef enum { T_END, T_NUM, T_NAME, T_PUNCT } tok_kind;

typedef struct {
    const char *src, *p, *tok;
    tok_kind kind;
    double num;
    char text[256];
    int dotted;
    calc_prog *prog;
    int nest, sp;
    char *err;
    size_t cap;
    int failed;
} parser;

static void perr(parser *P, const char *fmt, ...) HMI_PRINTF(2, 3);
static void perr(parser *P, const char *fmt, ...)
{
    if (P->failed) return;
    P->failed = 1;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(P->err, P->cap, "%s (position %d)", msg, (int)(P->tok - P->src) + 1);
}

static int ieq(const char *a, const char *b) { return str_ieq(a, b); }

static void next(parser *P)
{
    const char *p = P->p;
    while (isspace((unsigned char)*p)) p++;
    P->tok = p;
    P->dotted = 0;
    P->text[0] = 0;
    if (!*p) { P->kind = T_END; P->p = p; return; }
    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        char *end;
        P->num = strtod(p, &end);
        P->kind = T_NUM;
        P->p = end;
        if (isalpha((unsigned char)*end) || *end == '_') perr(P, "nombre mal écrit");
        return;
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
        size_t n = 0;
        int over = 0;
#define PUT(ch) do { if (n < sizeof P->text - 1) P->text[n++] = (ch); else over = 1; } while (0)
        for (;;) {
            if (isalnum((unsigned char)*p) || *p == '_') { PUT(*p); p++; continue; }
            if (*p == '[') {
                P->dotted = 1;
                PUT('[');
                p++;
                while (*p && *p != ']') {
                    if (isdigit((unsigned char)*p) || *p == ',' || *p == '-') PUT(*p);
                    else if (!isspace((unsigned char)*p)) { P->p = p; P->tok = p; perr(P, "indice de tableau invalide"); return; }
                    p++;
                }
                if (*p != ']') { P->tok = p; perr(P, "crochet ] manquant"); P->p = p; return; }
                PUT(']');
                p++;
                continue;
            }
            if (*p == '.' && (isalpha((unsigned char)p[1]) || p[1] == '_')) { P->dotted = 1; PUT('.'); p++; continue; }
            break;
        }
#undef PUT
        P->text[n] = 0;
        P->kind = T_NAME;
        P->p = p;
        if (over) perr(P, "nom de variable trop long");
        return;
    }
    static const char *two[] = {"<=", ">=", "==", "!=", "<>", "&&", "||", "**"};
    for (size_t i = 0; i < sizeof two / sizeof two[0]; i++) {
        if (p[0] == two[i][0] && p[1] == two[i][1]) {
            memcpy(P->text, p, 2);
            P->text[2] = 0;
            P->kind = T_PUNCT;
            P->p = p + 2;
            return;
        }
    }
    if (strchr("+-*/%^<>=!(),?:", *p)) {
        P->text[0] = *p;
        P->text[1] = 0;
        P->kind = T_PUNCT;
        P->p = p + 1;
        return;
    }
    P->kind = T_END;
    P->p = p;
    perr(P, "caractère inattendu « %c »", isprint((unsigned char)*p) ? *p : '?');
}

static int is_punct(const parser *P, const char *s) { return P->kind == T_PUNCT && !strcmp(P->text, s); }
static int is_word(const parser *P, const char *s) { return P->kind == T_NAME && !P->dotted && ieq(P->text, s); }

static void emit(parser *P, int op, int fn, int argc, int ref, double v)
{
    if (P->failed) return;
    calc_prog *g = P->prog;
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 16;
        g->code = xrealloc(g->code, (size_t)g->cap * sizeof *g->code);
    }
    instr *in = &g->code[g->n++];
    in->op = (unsigned char)op;
    in->fn = (unsigned char)fn;
    in->argc = (unsigned char)argc;
    in->ref = ref;
    in->v = v;
    /* profondeur de pile à l'exécution, contrôlée ici une fois pour toutes */
    if (op == OP_NUM || op == OP_VAR) P->sp++;
    else if (op == OP_NEG || op == OP_NOT) {}
    else if (op == OP_SEL) P->sp -= 2;
    else if (op == OP_FN) P->sp -= argc - 1;
    else P->sp--;
    if (P->sp > MAX_STACK) perr(P, "expression trop complexe");
}

static int add_ref(parser *P, const char *name)
{
    calc_prog *g = P->prog;
    for (int i = 0; i < g->nrefs; i++)
        if (ieq(g->refs[i], name)) return i;
    g->refs = xrealloc(g->refs, (size_t)(g->nrefs + 1) * sizeof *g->refs);
    g->refs[g->nrefs] = xstrdup(name);
    return g->nrefs++;
}

static int p_ternary(parser *P);

static int p_primary(parser *P)
{
    if (P->failed) return 0;
    if (P->kind == T_NUM) {
        emit(P, OP_NUM, 0, 0, 0, P->num);
        next(P);
        return 0;
    }
    if (is_punct(P, "(")) {
        if (++P->nest > MAX_NEST) { perr(P, "trop de parenthèses imbriquées"); return 0; }
        next(P);
        int b = p_ternary(P);
        if (!is_punct(P, ")")) perr(P, "parenthèse ) attendue");
        P->nest--;
        next(P);
        return b;
    }
    if (P->kind != T_NAME) {
        if (P->kind == T_END) perr(P, "expression incomplète");
        else perr(P, "valeur ou variable attendue avant « %s »", P->text);
        return 0;
    }
    char name[256];
    str_copy(name, sizeof name, P->text);
    int dotted = P->dotted;
    if (!dotted) {
        if (ieq(name, "true")) { emit(P, OP_NUM, 0, 0, 0, 1); next(P); return 1; }
        if (ieq(name, "false")) { emit(P, OP_NUM, 0, 0, 0, 0); next(P); return 1; }
        if (ieq(name, "pi")) { emit(P, OP_NUM, 0, 0, 0, 3.14159265358979323846); next(P); return 0; }
        if (ieq(name, "and") || ieq(name, "or") || ieq(name, "xor") || ieq(name, "mod") || ieq(name, "not")) {
            perr(P, "valeur attendue avant « %s »", name);
            return 0;
        }
    }
    next(P);
    if (is_punct(P, "(")) {
        int f = -1;
        for (size_t i = 0; !dotted && i < sizeof FUNCS / sizeof FUNCS[0]; i++)
            if (ieq(FUNCS[i].name, name)) f = (int)i;
        if (f < 0) { perr(P, "fonction inconnue : %s", name); return 0; }
        if (++P->nest > MAX_NEST) { perr(P, "trop de parenthèses imbriquées"); return 0; }
        next(P);
        int argc = 0, bools[MAX_ARGS];
        if (!is_punct(P, ")")) {
            for (;;) {
                if (argc == MAX_ARGS) { perr(P, "trop d'arguments pour %s", FUNCS[f].name); return 0; }
                bools[argc++] = p_ternary(P);
                if (P->failed) return 0;
                if (is_punct(P, ",")) { next(P); continue; }
                break;
            }
        }
        if (!is_punct(P, ")")) { perr(P, "parenthèse ) ou virgule attendue"); return 0; }
        P->nest--;
        if (argc < FUNCS[f].amin || argc > FUNCS[f].amax) {
            if (FUNCS[f].amin == FUNCS[f].amax)
                perr(P, "%s attend %d argument%s", FUNCS[f].name, FUNCS[f].amin, FUNCS[f].amin > 1 ? "s" : "");
            else if (FUNCS[f].amax == MAX_ARGS)
                perr(P, "%s attend au moins %d argument", FUNCS[f].name, FUNCS[f].amin);
            else
                perr(P, "%s attend de %d à %d arguments", FUNCS[f].name, FUNCS[f].amin, FUNCS[f].amax);
            return 0;
        }
        next(P);
        if (FUNCS[f].fn == F_IF) {
            emit(P, OP_SEL, 0, 0, 0, 0);
            return bools[1] && bools[2];
        }
        emit(P, OP_FN, FUNCS[f].fn, argc, 0, 0);
        return FUNCS[f].is_bool;
    }
    /* nom court : autre variable calculée */
    char full[300];
    if (dotted) str_copy(full, sizeof full, name);
    else snprintf(full, sizeof full, "CALC.%s", name);
    emit(P, OP_VAR, 0, 0, add_ref(P, full), 0);
    return 0;
}

static int p_unary(parser *P);

static int p_pow(parser *P)
{
    int b = p_primary(P);
    if (is_punct(P, "^") || is_punct(P, "**")) {
        next(P);
        p_unary(P); /* associatif à droite : 2^-1, 2^3^2 = 2^9 */
        emit(P, OP_POW, 0, 0, 0, 0);
        return 0;
    }
    return b;
}

static int p_unary(parser *P)
{
    if (P->failed) return 0;
    if (is_punct(P, "-")) { next(P); p_unary(P); emit(P, OP_NEG, 0, 0, 0, 0); return 0; }
    if (is_punct(P, "+")) { next(P); return p_unary(P); }
    if (is_punct(P, "!") || is_word(P, "not")) { next(P); p_unary(P); emit(P, OP_NOT, 0, 0, 0, 0); return 1; }
    return p_pow(P);
}

static int p_mul(parser *P)
{
    int b = p_unary(P);
    for (;;) {
        int op = is_punct(P, "*") ? OP_MUL : is_punct(P, "/") ? OP_DIV : (is_punct(P, "%") || is_word(P, "mod")) ? OP_MOD : -1;
        if (op < 0 || P->failed) return b;
        next(P);
        p_unary(P);
        emit(P, op, 0, 0, 0, 0);
        b = 0;
    }
}

static int p_add(parser *P)
{
    int b = p_mul(P);
    for (;;) {
        int op = is_punct(P, "+") ? OP_ADD : is_punct(P, "-") ? OP_SUB : -1;
        if (op < 0 || P->failed) return b;
        next(P);
        p_mul(P);
        emit(P, op, 0, 0, 0, 0);
        b = 0;
    }
}

static int p_cmp(parser *P)
{
    int b = p_add(P);
    for (;;) {
        int op = is_punct(P, "<") ? OP_LT : is_punct(P, "<=") ? OP_LE : is_punct(P, ">") ? OP_GT : is_punct(P, ">=") ? OP_GE : -1;
        if (op < 0 || P->failed) return b;
        next(P);
        p_add(P);
        emit(P, op, 0, 0, 0, 0);
        b = 1;
    }
}

static int p_eq(parser *P)
{
    int b = p_cmp(P);
    for (;;) {
        int op = (is_punct(P, "=") || is_punct(P, "==")) ? OP_EQ : (is_punct(P, "<>") || is_punct(P, "!=")) ? OP_NE : -1;
        if (op < 0 || P->failed) return b;
        next(P);
        p_cmp(P);
        emit(P, op, 0, 0, 0, 0);
        b = 1;
    }
}

static int p_and(parser *P)
{
    int b = p_eq(P);
    while (!P->failed && (is_punct(P, "&&") || is_word(P, "and"))) {
        next(P);
        p_eq(P);
        emit(P, OP_AND, 0, 0, 0, 0);
        b = 1;
    }
    return b;
}

static int p_or(parser *P)
{
    int b = p_and(P);
    for (;;) {
        int op = (is_punct(P, "||") || is_word(P, "or")) ? OP_OR : is_word(P, "xor") ? OP_XOR : -1;
        if (op < 0 || P->failed) return b;
        next(P);
        p_and(P);
        emit(P, op, 0, 0, 0, 0);
        b = 1;
    }
}

static int p_ternary(parser *P)
{
    int c = p_or(P);
    if (!is_punct(P, "?") || P->failed) return c;
    next(P);
    int a = p_ternary(P);
    if (!is_punct(P, ":")) { perr(P, "« : » attendu après « ? »"); return 0; }
    next(P);
    int b = p_ternary(P);
    emit(P, OP_SEL, 0, 0, 0, 0);
    return a && b;
}

calc_prog *calc_compile(const char *expr, char *err, size_t cap)
{
    if (err && cap) err[0] = 0;
    parser P;
    memset(&P, 0, sizeof P);
    char dummy[8];
    P.err = err && cap ? err : dummy;
    P.cap = err && cap ? cap : sizeof dummy;
    P.src = P.p = P.tok = expr ? expr : "";
    P.prog = xcalloc(1, sizeof *P.prog);
    next(&P);
    if (P.kind == T_END && !P.failed) perr(&P, "expression vide");
    int b = p_ternary(&P);
    if (!P.failed && P.kind != T_END) perr(&P, "opérateur attendu avant « %s »", P.text[0] ? P.text : "?");
    if (P.failed) {
        calc_free(P.prog);
        return NULL;
    }
    P.prog->is_bool = b;
    return P.prog;
}

void calc_free(calc_prog *p)
{
    if (!p) return;
    for (int i = 0; i < p->nrefs; i++) free(p->refs[i]);
    free(p->refs);
    free(p->code);
    free(p);
}

int calc_is_bool(const calc_prog *p) { return p->is_bool; }
int calc_nrefs(const calc_prog *p) { return p->nrefs; }
const char *calc_ref(const calc_prog *p, int i) { return i >= 0 && i < p->nrefs ? p->refs[i] : ""; }

/* ======================================================================= évaluation */

static double d_trunc(double x) { return x < 0 ? ceil(x) : floor(x); }
/* arrondi au plus proche, milieu loin de zéro (comme ROUND en texte structuré et Math.round pour x >= 0) */
static double d_round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

static double fn_eval(int fn, const double *a, int n)
{
    for (int i = 0; i < n; i++)
        if (isnan(a[i])) return NAN;
    double r;
    switch (fn) {
    case F_ABS: return a[0] < 0 ? -a[0] : a[0];
    case F_SQRT: return a[0] < 0 ? NAN : sqrt(a[0]);
    case F_EXP: return exp(a[0]);
    case F_LN: return a[0] <= 0 ? NAN : log(a[0]);
    case F_LOG10: return a[0] <= 0 ? NAN : log10(a[0]);
    case F_SIN: return sin(a[0]);
    case F_COS: return cos(a[0]);
    case F_TAN: return tan(a[0]);
    case F_FLOOR: return floor(a[0]);
    case F_CEIL: return ceil(a[0]);
    case F_TRUNC: return d_trunc(a[0]);
    case F_SIGN: return a[0] > 0 ? 1 : a[0] < 0 ? -1 : 0;
    case F_ROUND: {
        if (n == 1) return d_round(a[0]);
        int d = (int)d_trunc(a[1]);
        if (d < -15) d = -15;
        if (d > 15) d = 15;
        double f = pow(10.0, d);
        return d_round(a[0] * f) / f;
    }
    case F_POW: return pow(a[0], a[1]);
    case F_BIT: {
        if (a[1] < 0 || a[1] > 63 || a[0] < -9.2e18 || a[0] > 1.8e19) return NAN;
        unsigned long long v = a[0] < 0 ? (unsigned long long)(long long)d_trunc(a[0]) : (unsigned long long)d_trunc(a[0]);
        return (double)((v >> (int)a[1]) & 1u);
    }
    case F_MIN: r = a[0]; for (int i = 1; i < n; i++) if (a[i] < r) r = a[i]; return r;
    case F_MAX: r = a[0]; for (int i = 1; i < n; i++) if (a[i] > r) r = a[i]; return r;
    case F_SUM: r = 0; for (int i = 0; i < n; i++) r += a[i]; return r;
    case F_AVG: r = 0; for (int i = 0; i < n; i++) r += a[i]; return r / n;
    case F_CLAMP: return a[0] < a[1] ? a[1] : a[0] > a[2] ? a[2] : a[0];
    case F_LIMIT: return a[1] < a[0] ? a[0] : a[1] > a[2] ? a[2] : a[1];
    }
    return NAN;
}

/* logique à trois états : FALSE AND (invalide) = FALSE, TRUE OR (invalide) = TRUE, sinon invalide propagé */
static double l_and(double a, double b)
{
    if (a == 0 || b == 0) return 0;
    return (isnan(a) || isnan(b)) ? NAN : 1;
}
static double l_or(double a, double b)
{
    if ((!isnan(a) && a != 0) || (!isnan(b) && b != 0)) return 1;
    return (isnan(a) || isnan(b)) ? NAN : 0;
}

#define CMP(expr) ((isnan(a) || isnan(b)) ? NAN : ((expr) ? 1.0 : 0.0))

double calc_eval(const calc_prog *p, double (*get)(void *ctx, int i), void *ctx)
{
    double st[MAX_STACK + 1];
    int sp = 0;
    for (int k = 0; k < p->n; k++) {
        const instr *in = &p->code[k];
        double a, b, c;
        switch (in->op) {
        case OP_NUM: st[sp++] = in->v; break;
        case OP_VAR: st[sp++] = get(ctx, in->ref); break;
        case OP_NEG: st[sp - 1] = -st[sp - 1]; break;
        case OP_NOT: a = st[sp - 1]; st[sp - 1] = isnan(a) ? NAN : (a == 0); break;
        case OP_SEL:
            b = st[--sp];
            a = st[--sp];
            c = st[sp - 1];
            st[sp - 1] = isnan(c) ? NAN : (c != 0 ? a : b);
            break;
        case OP_FN:
            sp -= in->argc;
            st[sp] = fn_eval(in->fn, &st[sp], in->argc);
            sp++;
            break;
        default:
            b = st[--sp];
            a = st[sp - 1];
            switch (in->op) {
            case OP_ADD: c = a + b; break;
            case OP_SUB: c = a - b; break;
            case OP_MUL: c = a * b; break;
            case OP_DIV: c = b == 0 ? NAN : a / b; break;
            case OP_MOD: c = b == 0 ? NAN : fmod(a, b); break;
            case OP_POW: c = pow(a, b); break;
            case OP_LT: c = CMP(a < b); break;
            case OP_LE: c = CMP(a <= b); break;
            case OP_GT: c = CMP(a > b); break;
            case OP_GE: c = CMP(a >= b); break;
            case OP_EQ: c = CMP(a == b); break;
            case OP_NE: c = CMP(a != b); break;
            case OP_AND: c = l_and(a, b); break;
            case OP_OR: c = l_or(a, b); break;
            case OP_XOR: c = (isnan(a) || isnan(b)) ? NAN : ((a != 0) != (b != 0)); break;
            default: c = NAN;
            }
            st[sp - 1] = c;
        }
    }
    double r = sp > 0 ? st[sp - 1] : NAN;
    return isfinite(r) ? r : NAN;
}
