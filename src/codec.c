#include "codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- lecture little-endian */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/** nombre d'octets lus pour décoder (au moins 1, 1 pour un bit) */
static size_t span_len(const leaf_t *l)
{
    if (l->prim == PR_BIT) return 1;
    return l->size > 0 ? (size_t)l->size : 1;
}

static int in_block(const leaf_t *l, size_t len)
{
    return l->offset >= 0 && (size_t)l->offset + span_len(l) <= len;
}

/** décodage numérique ; 0 = valeur disponible, -1 = chaîne ou hors bloc ; `single` = issu d'un REAL */
static int decode_num(const leaf_t *l, const uint8_t *blk, size_t len, double *out, int *single)
{
    *single = 0;
    if (!blk || !in_block(l, len)) return -1;
    const uint8_t *s = blk + l->offset;
    switch (l->prim) {
    case PR_BOOL: *out = s[0] != 0; return 0;
    case PR_BIT: *out = (s[0] >> l->bit) & 1; return 0;
    case PR_I8: *out = (int8_t)s[0]; return 0;
    case PR_U8: *out = s[0]; return 0;
    case PR_I16: *out = (int16_t)rd16(s); return 0;
    case PR_U16: *out = rd16(s); return 0;
    case PR_I32: *out = (int32_t)rd32(s); return 0;
    case PR_U32: *out = rd32(s); return 0;
    case PR_I64: *out = (double)(int64_t)rd64(s); return 0;
    case PR_U64: *out = (double)rd64(s); return 0;
    case PR_F32: {
        uint32_t u = rd32(s);
        float f;
        memcpy(&f, &u, 4);
        *out = f;
        *single = 1;
        return 0;
    }
    case PR_F64: {
        uint64_t u = rd64(s);
        memcpy(out, &u, 8);
        return 0;
    }
    case PR_STRING:
    case PR_WSTRING:
        return -1;
    }
    return -1;
}

/** texte UTF-8 d'une variable chaîne, ajouté à `out` */
static void decode_str(const leaf_t *l, const uint8_t *s, sb_t *out)
{
    size_t n = (size_t)l->size;
    if (l->prim == PR_STRING) {
        const uint8_t *z = memchr(s, 0, n);
        utf8_from_latin1(out, s, z ? (size_t)(z - s) : n);
    } else {
        utf8_from_utf16le(out, s, n);
    }
}

void codec_json(const leaf_t *l, const uint8_t *blk, size_t len, sb_t *out)
{
    if (!blk || !in_block(l, len)) {
        sb_puts(out, "null");
        return;
    }
    if (l->prim == PR_STRING || l->prim == PR_WSTRING) {
        sb_t tmp = {0};
        decode_str(l, blk + l->offset, &tmp);
        sb_json_str(out, sb_cstr(&tmp), tmp.len);
        sb_free(&tmp);
        return;
    }
    double v;
    int single;
    decode_num(l, blk, len, &v, &single);
    if (l->prim == PR_BOOL || l->prim == PR_BIT) sb_puts(out, v != 0 ? "true" : "false");
    else sb_json_num(out, v, single);
}

double codec_num(const leaf_t *l, const uint8_t *blk, size_t len)
{
    double v;
    int single;
    if (decode_num(l, blk, len, &v, &single) == 0) return isfinite(v) ? v : NAN;
    if ((l->prim == PR_STRING || l->prim == PR_WSTRING) && blk && in_block(l, len)) {
        sb_t tmp = {0};
        decode_str(l, blk + l->offset, &tmp);
        v = parse_number_lenient(sb_cstr(&tmp));
        sb_free(&tmp);
        return v;
    }
    return NAN;
}

int codec_same(const leaf_t *l, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    size_t n = span_len(l);
    if (!a || !b || (size_t)l->offset + n > alen || (size_t)l->offset + n > blen) return 0;
    if (l->prim == PR_BIT) return ((a[l->offset] ^ b[l->offset]) & (1u << l->bit)) == 0;
    return memcmp(a + l->offset, b + l->offset, n) == 0;
}

/* ------------------------------------------------------------- encodage */

static int fail(char *err, size_t cap, const char *fmt, const leaf_t *l, const char *detail)
{
    snprintf(err, cap, fmt, l->path ? l->path : "?", l->type ? l->type : "?", detail);
    return -1;
}

/** arrondi au plus proche puis contrôle des bornes */
static int to_int(const leaf_t *l, double d, double min, double max, long long *out, char *err, size_t cap)
{
    double r = round(d);
    if (r < min || r > max) {
        char det[96];
        snprintf(det, sizeof det, "%.15g hors limites [%.0f ; %.0f]", d, min, max);
        return fail(err, cap, "Valeur invalide pour %s (%s) : %s", l, det);
    }
    *out = (long long)r;
    return 0;
}

int codec_encode_num(const leaf_t *l, double d, uint8_t *out, size_t cap, char *err, size_t errcap)
{
    if (l->prim == PR_STRING || l->prim == PR_WSTRING) {
        char buf[40];
        sb_t t = {0};
        sb_json_num(&t, d, 0);
        str_copy(buf, sizeof buf, sb_cstr(&t));
        sb_free(&t);
        return codec_encode_str(l, buf, out, cap, err, errcap);
    }
    if (!isfinite(d)) return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "nombre attendu");
    size_t need = span_len(l);
    if (cap < need) return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "taille");
    memset(out, 0, need);
    long long i = 0;
    switch (l->prim) {
    case PR_BOOL:
    case PR_BIT: out[0] = d != 0; return 1;
    case PR_I8: if (to_int(l, d, -128, 127, &i, err, errcap)) return -1; out[0] = (uint8_t)(int8_t)i; return 1;
    case PR_U8: if (to_int(l, d, 0, 255, &i, err, errcap)) return -1; out[0] = (uint8_t)i; return 1;
    case PR_I16: if (to_int(l, d, -32768, 32767, &i, err, errcap)) return -1; wr16(out, (uint16_t)(int16_t)i); return 2;
    case PR_U16: if (to_int(l, d, 0, 65535, &i, err, errcap)) return -1; wr16(out, (uint16_t)i); return 2;
    case PR_I32: if (to_int(l, d, -2147483648.0, 2147483647.0, &i, err, errcap)) return -1; wr32(out, (uint32_t)(int32_t)i); return 4;
    case PR_U32: if (to_int(l, d, 0, 4294967295.0, &i, err, errcap)) return -1; wr32(out, (uint32_t)i); return 4;
    case PR_I64: {
        double r = round(d);
        if (r < -9223372036854775808.0 || r >= 9223372036854775808.0)
            return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "hors limites");
        wr64(out, (uint64_t)(int64_t)r);
        return 8;
    }
    case PR_U64: {
        double r = round(d);
        if (r < 0 || r >= 18446744073709551616.0)
            return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "hors limites");
        wr64(out, (uint64_t)r);
        return 8;
    }
    case PR_F32: {
        float f = (float)d;
        if (!isfinite(f)) return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "hors limites");
        uint32_t u;
        memcpy(&u, &f, 4);
        wr32(out, u);
        return 4;
    }
    case PR_F64: {
        uint64_t u;
        memcpy(&u, &d, 8);
        wr64(out, u);
        return 8;
    }
    default: break;
    }
    return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "type non géré");
}

int codec_encode_str(const leaf_t *l, const char *s, uint8_t *out, size_t cap, char *err, size_t errcap)
{
    if (!s) s = "";
    if (l->prim == PR_STRING || l->prim == PR_WSTRING) {
        size_t size = (size_t)l->size;
        if (size == 0 || cap < size) return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "taille");
        memset(out, 0, size);
        /* tronquée si trop longue : la place du terminateur nul est toujours réservée */
        if (l->prim == PR_STRING) latin1_from_utf8(s, out, size - 1);
        else if (size >= 2) utf16le_from_utf8(s, out, size - 2);
        return (int)size;
    }
    if (l->kind == LK_ENUM) {
        for (int i = 0; i < l->nenums; i++)
            if (str_ieq(l->enums[i].name, s)) return codec_encode_num(l, l->enums[i].value, out, cap, err, errcap);
    }
    double d = parse_number_lenient(s);
    if (isnan(d)) return fail(err, errcap, "Valeur invalide pour %s (%s) : « %s »", l, s);
    return codec_encode_num(l, d, out, cap, err, errcap);
}

int codec_encode(const leaf_t *l, const cJSON *v, uint8_t *out, size_t cap, char *err, size_t errcap)
{
    if (cJSON_IsBool(v)) {
        if (l->prim == PR_STRING || l->prim == PR_WSTRING)
            return codec_encode_str(l, cJSON_IsTrue(v) ? "true" : "false", out, cap, err, errcap);
        return codec_encode_num(l, cJSON_IsTrue(v) ? 1 : 0, out, cap, err, errcap);
    }
    if (cJSON_IsNumber(v)) return codec_encode_num(l, v->valuedouble, out, cap, err, errcap);
    if (cJSON_IsString(v)) return codec_encode_str(l, v->valuestring, out, cap, err, errcap);
    if ((!v || cJSON_IsNull(v)) && (l->prim == PR_STRING || l->prim == PR_WSTRING))
        return codec_encode_str(l, "", out, cap, err, errcap);
    return fail(err, errcap, "Valeur invalide pour %s (%s) : %s", l, "booléen, nombre ou chaîne attendu");
}

void codec_put(const leaf_t *l, uint8_t *blk, size_t len, const uint8_t *enc, int n)
{
    if (!blk || !in_block(l, len) || n <= 0) return;
    if (l->prim == PR_BIT) {
        if (enc[0]) blk[l->offset] |= (uint8_t)(1u << l->bit);
        else blk[l->offset] &= (uint8_t)~(1u << l->bit);
        return;
    }
    size_t m = (size_t)n < (size_t)l->size ? (size_t)n : (size_t)l->size;
    memcpy(blk + l->offset, enc, m);
}
