/*
 * Persistance en fichiers JSON lisibles (et versionnables avec git), au même format que la version web :
 *   <data>/config.json            configuration de la source (AppConfig)
 *   <data>/dashboards/<id>.json   écrans
 * Toutes les écritures sont atomiques (fichier temporaire forcé sur disque puis renommé).
 */
#ifndef HMI_STORE_H
#define HMI_STORE_H

#include "../third_party/cjson/cJSON.h"
#include <stddef.h>

/** configuration par défaut (simulateur), identique à DEFAULT_CONFIG du protocole web */
cJSON *config_defaults(void);

/** configuration par défaut complétée par les clés de `raw` (sections ads et sim fusionnées clé par clé) */
cJSON *config_merge(const cJSON *raw);

/**
 * Lit <data>/config.json. Ne renvoie jamais NULL :
 *  - fichier absent : configuration par défaut, `err` vide ;
 *  - fichier illisible : configuration par défaut, message dans `err`, et copie de sauvegarde
 *    config.json.illisible-AAAAMMJJ-HHMMSS (l'appelant ne doit alors démarrer aucune source).
 */
cJSON *config_load(const char *data_dir, char *err, size_t cap);

/** écrit <data>/config.json ; 0 si succès */
int config_save(const char *data_dir, const cJSON *cfg, char *err, size_t cap);

/** identifiant d'écran valide : [A-Za-z0-9_-]{1,80} (pas de chemin, pas de « .. ») */
int store_valid_id(const char *id);

/** liste [{ id, name, updatedAt }] triée par nom (JSON alloué, à libérer avec free) */
char *store_list(const char *dash_dir);

/** contenu brut de l'écran (alloué) ; NULL si absent (`notfound` = 1) ou illisible */
char *store_get(const char *dash_dir, const char *id, size_t *len, int *notfound);

/** enregistre l'écran après avoir fixé ses champs id et updatedAt ; 0 si succès */
int store_put(const char *dash_dir, const char *id, cJSON *doc, char *err, size_t cap);

/** supprime l'écran ; 0 si succès, -1 (`notfound` = 1 si absent) */
int store_delete(const char *dash_dir, const char *id, int *notfound);

#endif
