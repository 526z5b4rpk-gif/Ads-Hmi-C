/*
 * Utilitaires communs : allocation, chaîne dynamique, arène, conversions de texte.
 *
 * Conventions du projet :
 *  - les fonctions x*alloc arrêtent le programme si la mémoire manque (pas de code d'erreur à propager) ;
 *  - tout texte manipulé est en UTF-8 ; les conversions Latin-1 (STRING TwinCAT) et UTF-16 (WSTRING)
 *    se font aux frontières (codec) ;
 *  - les fonctions renvoyant un int renvoient 0 en cas de succès, -1 en cas d'erreur, sauf mention contraire.
 */
#ifndef HMI_UTIL_H
#define HMI_UTIL_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* replis pour les bibliothèques C sans les macros C99 de <math.h> */
#ifndef isnan
#define isnan(x) ((x) != (x))
#endif
#ifndef isfinite
#define isfinite(x) (!isnan(x) && !isnan((x) - (x)))
#endif
#ifndef NAN
double hmi_nan(void);
#define NAN (hmi_nan())
#define HMI_NEED_NAN 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define HMI_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define HMI_PRINTF(f, a)
#endif

/* ------------------------------------------------------------------ allocation */

void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ------------------------------------------------------------- chaîne dynamique */

/** Tampon d'octets extensible, toujours terminé par un caractère nul (p peut être NULL tant que vide). */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
} sb_t;

/** garantit la place pour `extra` octets supplémentaires (plus le nul final) */
void sb_reserve(sb_t *b, size_t extra);
void sb_put(sb_t *b, const void *data, size_t n);
void sb_puts(sb_t *b, const char *s);
void sb_putc(sb_t *b, char c);
void sb_printf(sb_t *b, const char *fmt, ...) HMI_PRINTF(2, 3);
/** vide le tampon sans libérer la mémoire (réutilisation) */
void sb_clear(sb_t *b);
/** contenu sous forme de chaîne C (jamais NULL) */
const char *sb_cstr(sb_t *b);
void sb_free(sb_t *b);

/** Chaîne JSON entre guillemets, caractères spéciaux échappés. `n` = (size_t)-1 : chaîne terminée par nul. */
void sb_json_str(sb_t *b, const char *s, size_t n);

/**
 * Nombre JSON, écrit sous sa forme la plus courte qui se relit à l'identique.
 * `single` : la valeur provient d'un REAL (float) — la précision float suffit (20.4 et non 20.399999618530273).
 * Les valeurs non finies (NaN, infini) sont écrites `null`.
 */
void sb_json_num(sb_t *b, double v, int single);

/* ----------------------------------------------------------------------- arène */

/**
 * Allocateur par blocs : toutes les allocations sont libérées ensemble par arena_free.
 * Utilisé pour les arbres de types et les chemins des variables (durée de vie = celle d'un schéma).
 */
typedef struct arena arena_t;

arena_t *arena_new(void);
/** mémoire remise à zéro, alignée sur 16 octets */
void *arena_alloc(arena_t *a, size_t n);
/** copie d'une chaîne dans l'arène ; NULL donne NULL */
char *arena_strdup(arena_t *a, const char *s);
void arena_free(arena_t *a);

/* ------------------------------------------------------------------ chaînes */

/** égalité insensible à la casse (ASCII uniquement, comme les identifiants PLC) */
int str_ieq(const char *a, const char *b);
/** préfixe insensible à la casse (ASCII) */
int str_istarts(const char *s, const char *prefix);
/** hachage FNV-1a insensible à la casse ASCII */
uint32_t hash_ci(const char *s, size_t n);
/** copie bornée, toujours terminée par nul */
void str_copy(char *dst, size_t cap, const char *src);

/**
 * Conversion tolérante en nombre : « 12,5 » et « 12.5 » ; « true/vrai » = 1, « false/faux » = 0 ;
 * espaces de début et de fin ignorés. NaN si la chaîne n'est pas un nombre.
 */
double parse_number_lenient(const char *s);

/* ------------------------------------------------------- conversions de texte */

/** Latin-1 (Windows-1252 assimilé) -> UTF-8, ajouté à `b` */
void utf8_from_latin1(sb_t *b, const uint8_t *s, size_t n);
/** UTF-8 -> Latin-1 ; caractères non représentables remplacés par '?'. Renvoie le nombre d'octets écrits (<= cap). */
size_t latin1_from_utf8(const char *s, uint8_t *out, size_t cap);
/** UTF-16LE (s'arrête au premier caractère nul) -> UTF-8, ajouté à `b` */
void utf8_from_utf16le(sb_t *b, const uint8_t *s, size_t nbytes);
/** UTF-8 -> UTF-16LE ; renvoie le nombre d'octets écrits (pair, <= cap, jamais de paire de substitution coupée) */
size_t utf16le_from_utf8(const char *s, uint8_t *out, size_t cap);

/* ------------------------------------------------------------- journal */

/** message horodaté sur la sortie d'erreur ; `level` : "INFO", "ERREUR"… */
void log_msg(const char *level, const char *fmt, ...) HMI_PRINTF(2, 3);
#define log_info(...) log_msg("INFO", __VA_ARGS__)
#define log_error(...) log_msg("ERREUR", __VA_ARGS__)

#endif
