/*
 * Expressions des variables calculées : analyse une fois (texte -> programme en notation polonaise
 * inverse), puis évaluation rapide à chaque changement des variables sources.
 *
 * Syntaxe (inspirée du texte structuré IEC 61131-3 et du C, insensible à la casse) :
 *   nombres          12   3.5   1e-3   TRUE   FALSE   PI
 *   variables        SIM.stHmi.rTemperature   SIM.stHmi.aTemp[3]   SIM.stHmi.aAxes[0].rPos
 *                    nom court sans point = autre variable calculée définie AU-DESSUS (ex. rPuissance)
 *   arithmétique     + - * /   % ou MOD   ^ ou ** (puissance)
 *   comparaison      < <= > >=   = ou ==   <> ou !=
 *   logique          NOT ou !   AND ou &&   OR ou ||   XOR
 *   condition        c ? a : b        if(c, a, b)
 *   fonctions        abs sqrt exp ln log10 sin cos tan floor ceil trunc sign
 *                    round(x) round(x, décimales)  pow(a, b)  bit(mot, n)
 *                    min(…) max(…) sum(…) avg(…)  clamp(x, mini, maxi)  limit(mini, x, maxi)
 *
 * Valeurs : tout est nombre (double) ; booléen = 0/1, vrai si non nul ; énumération = sa valeur
 * numérique ; chaîne = nombre qu'elle contient. Une variable sans valeur (source déconnectée) ou un
 * calcul impossible (division par zéro…) donne une valeur invalide (NaN), propagée jusqu'au résultat
 * et affichée comme absente.
 */
#ifndef HMI_CALC_H
#define HMI_CALC_H

#include <stddef.h>

typedef struct calc_prog calc_prog;

/** analyse `expr` ; NULL et message dans err si la syntaxe est incorrecte */
calc_prog *calc_compile(const char *expr, char *err, size_t cap);
void calc_free(calc_prog *p);

/** le résultat est logique (comparaison, AND/OR/NOT, bit, TRUE/FALSE…) : type BOOL par défaut */
int calc_is_bool(const calc_prog *p);

/**
 * Variables référencées, sans doublon, telles qu'écrites (crochets sans espace).
 * Un nom court (sans point) désigne une variable calculée : il est renvoyé préfixé par « CALC. ».
 */
int calc_nrefs(const calc_prog *p);
const char *calc_ref(const calc_prog *p, int i);

/** évalue le programme ; get(ctx, i) donne la valeur de la référence i (NaN si inconnue) */
double calc_eval(const calc_prog *p, double (*get)(void *ctx, int i), void *ctx);

#endif
