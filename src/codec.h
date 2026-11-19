/*
 * Décodage / encodage binaire des variables (format mémoire TwinCAT, little-endian),
 * indépendant de l'ordre des octets de la machine hôte.
 */
#ifndef HMI_CODEC_H
#define HMI_CODEC_H

#include "types.h"
#include "../third_party/cjson/cJSON.h"

/**
 * Valeur de la variable écrite en JSON dans `out` : true/false, nombre, "chaîne" (UTF-8),
 * ou null si la variable dépasse du bloc ou si le réel n'est pas fini.
 */
void codec_json(const leaf_t *l, const uint8_t *blk, size_t len, sb_t *out);

/** Valeur numérique (booléen = 0/1, chaîne convertie de façon tolérante) ; NaN si indisponible. */
double codec_num(const leaf_t *l, const uint8_t *blk, size_t len);

/** 1 si la zone mémoire de la variable est identique dans les deux blocs (détection de changement sans décodage). */
int codec_same(const leaf_t *l, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

/**
 * Encode une valeur JSON (booléen, nombre, chaîne, null) selon le type de la variable.
 * Renvoie le nombre d'octets écrits dans `out`, ou -1 avec un message dans `err`
 * (valeur non numérique, entier hors des bornes du type : jamais de débordement silencieux vers l'automate).
 * Pour une énumération, le nom d'une valeur est accepté (« Production »).
 */
int codec_encode(const leaf_t *l, const cJSON *v, uint8_t *out, size_t cap, char *err, size_t errcap);
/** idem à partir d'un nombre */
int codec_encode_num(const leaf_t *l, double d, uint8_t *out, size_t cap, char *err, size_t errcap);
/** idem à partir d'une chaîne UTF-8 (convertie en nombre pour une variable numérique) */
int codec_encode_str(const leaf_t *l, const char *s, uint8_t *out, size_t cap, char *err, size_t errcap);

/** Écrit une valeur encodée dans un bloc (simulateur) ; ignoré si elle dépasse du bloc. */
void codec_put(const leaf_t *l, uint8_t *blk, size_t len, const uint8_t *enc, int n);

#endif
