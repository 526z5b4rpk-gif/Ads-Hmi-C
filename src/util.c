#include "util.h"
#include "plat.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HMI_NEED_NAN
double hmi_nan(void)
{
    static volatile double zero = 0.0;
    return zero / zero;
}
#endif

/* ------------------------------------------------------------------ allocation */

static void out_of_memory(void)
{
    fputs("ERREUR FATALE : mémoire insuffisante\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) out_of_memory();
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------- chaîne dynamique */

void sb_reserve(sb_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void sb_put(sb_t *b, const void *data, size_t n)
{
    sb_reserve(b, n);
    if (n) memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sb_puts(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }

void sb_putc(sb_t *b, char c)
{
    sb_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void sb_printf(sb_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_reserve(b, (size_t)n);
        vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

void sb_clear(sb_t *b)
{
    b->len = 0;
    if (b->p) b->p[0] = 0;
}

const char *sb_cstr(sb_t *b)
{
    if (!b->p) sb_reserve(b, 0), b->p[0] = 0;
    return b->p;
}

void sb_free(sb_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void sb_json_str(sb_t *b, const char *s, size_t n)
{
    if (!s) s = "";
    if (n == (size_t)-1) n = strlen(s);
    sb_reserve(b, n + 2);
    sb_putc(b, '"');
    size_t run = 0; /* début du segment sans caractère à échapper */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[8];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) {
                snprintf(ubuf, sizeof ubuf, "\\u%04x", c);
                esc = ubuf;
            }
        }
        if (esc) {
            sb_put(b, s + run, i - run);
            sb_puts(b, esc);
            run = i + 1;
        }
    }
    sb_put(b, s + run, n - run);
    sb_putc(b, '"');
}

void sb_json_num(sb_t *b, double v, int single)
{
    char buf[40];
    if (!isfinite(v)) {
        sb_puts(b, "null");
        return;
    }
    /* entiers exacts : sans partie décimale ni exposant (compteurs, mots d'état…) */
    if (v == floor(v) && fabs(v) < 1e15) {
        snprintf(buf, sizeof buf, "%.0f", v);
        sb_puts(b, buf);
        return;
    }
    if (single) {
        float f = (float)v;
        for (int prec = 6; prec <= 9; prec++) {
            snprintf(buf, sizeof buf, "%.*g", prec, (double)f);
            if (strtof(buf, NULL) == f) break;
        }
    } else {
        for (int prec = 15; prec <= 17; prec++) {
            snprintf(buf, sizeof buf, "%.*g", prec, v);
            if (strtod(buf, NULL) == v) break;
        }
    }
    sb_puts(b, buf);
}

/* ----------------------------------------------------------------------- arène */

#define ARENA_BLOCK 65536

typedef struct arena_block {
    struct arena_block *next;
    size_t used, cap;
    /* données après l'en-tête, alignées sur 8 octets (C99, sans _Alignas) */
    union { double d; long long l; void *p; } data[];
} arena_block;

struct arena {
    arena_block *head;
};

arena_t *arena_new(void) { return xcalloc(1, sizeof(arena_t)); }

void *arena_alloc(arena_t *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    arena_block *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        b = xmalloc(sizeof(arena_block) + cap);
        b->used = 0;
        b->cap = cap;
        b->next = a->head;
        a->head = b;
    }
    void *p = (unsigned char *)b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strdup(arena_t *a, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(a, n);
    memcpy(p, s, n);
    return p;
}

void arena_free(arena_t *a)
{
    if (!a) return;
    arena_block *b = a->head;
    while (b) {
        arena_block *n = b->next;
        free(b);
        b = n;
    }
    free(a);
}

/* ------------------------------------------------------------------ chaînes */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int str_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = lower((unsigned char)*a), cb = lower((unsigned char)*b);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

int str_istarts(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (lower((unsigned char)*s) != lower((unsigned char)*prefix)) return 0;
    return 1;
}

uint32_t hash_ci(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint32_t)lower((unsigned char)s[i]);
        h *= 16777619u;
    }
    return h;
}

void str_copy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

double parse_number_lenient(const char *s)
{
    if (!s) return NAN;
    while (isspace((unsigned char)*s)) s++;
    char buf[64];
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0 || n >= sizeof buf) return NAN;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (str_ieq(buf, "true") || str_ieq(buf, "vrai")) return 1;
    if (str_ieq(buf, "false") || str_ieq(buf, "faux")) return 0;
    for (char *p = buf; *p; p++)
        if (*p == ',') *p = '.';
    char *end;
    double v = strtod(buf, &end);
    if (*end) return NAN;
    return v;
}

/* ------------------------------------------------------- conversions de texte */

static void put_utf8(sb_t *b, uint32_t cp)
{
    char t[4];
    size_t n;
    if (cp < 0x80) {
        t[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_put(b, t, n);
}

/** décode un point de code UTF-8 ; renvoie le nombre d'octets consommés (>= 1) ; 0xFFFD si séquence invalide */
static size_t next_cp(const unsigned char *s, uint32_t *cp)
{
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n = (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 0;
    if (n == 0) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return (size_t)i; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return (size_t)n;
}

void utf8_from_latin1(sb_t *b, const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) put_utf8(b, s[i]);
}

size_t latin1_from_utf8(const char *s, uint8_t *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t o = 0;
    while (*p && o < cap) {
        uint32_t cp;
        p += next_cp(p, &cp);
        out[o++] = cp <= 0xFF ? (uint8_t)cp : (uint8_t)'?';
    }
    return o;
}

void utf8_from_utf16le(sb_t *b, const uint8_t *s, size_t nbytes)
{
    for (size_t i = 0; i + 1 < nbytes; i += 2) {
        uint32_t u = (uint32_t)s[i] | ((uint32_t)s[i + 1] << 8);
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < nbytes) {
            uint32_t lo = (uint32_t)s[i + 2] | ((uint32_t)s[i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                put_utf8(b, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (u >= 0xD800 && u < 0xE000) u = 0xFFFD; /* demi-paire isolée */
        put_utf8(b, u);
    }
}

size_t utf16le_from_utf8(const char *s, uint8_t *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t o = 0;
    while (*p) {
        uint32_t cp;
        p += next_cp(p, &cp);
        if (cp >= 0x10000) {
            if (o + 4 > cap) break;
            uint32_t v = cp - 0x10000, hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
            out[o++] = (uint8_t)hi; out[o++] = (uint8_t)(hi >> 8);
            out[o++] = (uint8_t)lo; out[o++] = (uint8_t)(lo >> 8);
        } else {
            if (o + 2 > cap) break;
            out[o++] = (uint8_t)cp; out[o++] = (uint8_t)(cp >> 8);
        }
    }
    return o;
}

/* ------------------------------------------------------------- journal */

void log_msg(const char *level, const char *fmt, ...)
{
    plat_date d;
    plat_now(&d, 1);
    char ts[32];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", d.year, d.mon, d.day, d.hour, d.min, d.sec);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s [%s] ", ts, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}
