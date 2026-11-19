/*
 * Cœur temps réel : relie la source de données (simulateur, ADS) aux clients web.
 *
 *  - la source fournit la description des symboles racines (hub_source_roots), puis à chaque
 *    notification le bloc binaire complet d'un symbole racine (hub_source_data), sur son propre thread ;
 *  - le hub compare le bloc reçu au précédent variable par variable (comparaison d'octets, sans décodage)
 *    et marque les variables modifiées ;
 *  - la boucle principale appelle hub_poll toutes les ~10 ms : les variables marquées sont décodées
 *    depuis le dernier bloc et envoyées en un seul message delta { "t":"d", "v":[i, v, i, v…] }.
 *
 * Modèle de concurrence : un seul verrou protège tout l'état partagé. La source n'appelle le hub
 * que par les trois fonctions hub_source_* ; la boucle principale ne détient jamais le verrou pendant
 * un appel à la source (arrêt, écriture), ce qui exclut tout interblocage.
 */
#ifndef HMI_HUB_H
#define HMI_HUB_H

#include "types.h"
#include "../third_party/cjson/cJSON.h"

typedef enum { ST_STOPPED, ST_CONNECTING, ST_CONNECTED, ST_ERROR } conn_state;

typedef struct hub hub_t;

/** Interface d'une source de données. Toutes les fonctions sont appelées depuis la boucle principale. */
typedef struct source source_t;
struct source {
    /** démarre la source (connexion, thread…) ; 0 si succès, sinon message dans err */
    int (*start)(source_t *s, char *err, size_t cap);
    /** arrête la source : au retour, plus aucun appel hub_source_* ne sera fait */
    void (*stop)(source_t *s);
    /**
     * Écrit la valeur encodée d'une variable. Seuls path, type, root, offset, bit, size et prim
     * de `leaf` sont utilisables (copie faite par le hub). 0 si succès, sinon message dans err.
     */
    int (*write)(source_t *s, const leaf_t *leaf, const uint8_t *data, int len, char *err, size_t cap);
    void (*destroy)(source_t *s);
};

/** Fonction de diffusion appelée par hub_poll pour chaque message ; `is_delta` = message de valeurs. */
typedef void (*hub_emit_fn)(void *ctx, const char *json, size_t len, int is_delta);

hub_t *hub_new(void);
/** arrête la source et libère tout */
void hub_free(hub_t *h);

/**
 * Arrête la source courante puis démarre celle décrite par la configuration (format web : AppConfig).
 * Le schéma est vidé entre les deux : aucune valeur de l'ancienne source n'est affichée comme valide.
 */
void hub_apply(hub_t *h, const cJSON *config);

/** Signale une erreur empêchant tout démarrage de source (configuration illisible…). */
void hub_report_error(hub_t *h, const char *message);

/** À appeler régulièrement par la boucle principale : émet statut, schéma et deltas en attente. */
void hub_poll(hub_t *h, hub_emit_fn emit, void *ctx);

/** Message { "t":"schema", schema, values } complet (nouveau client, client à resynchroniser). */
void hub_schema_msg(hub_t *h, sb_t *out);
/** Message { "t":"status", status }. */
void hub_status_msg(hub_t *h, sb_t *out);

/**
 * Écrit une valeur JSON dans une variable (chemin insensible à la casse).
 * 0 si succès ; sinon -1 et message dans err (variable inconnue, valeur invalide, aucune source, erreur source).
 */
int hub_write(hub_t *h, const char *path, const cJSON *value, char *err, size_t cap);

/* ---- variables calculées (symbole racine virtuel CALC, voir calc.h pour la syntaxe) ---- */

/**
 * Remplace les variables calculées par la liste JSON [{name, expr, type?, unit?, comment?}].
 * Tout est contrôlé avant de modifier quoi que ce soit : nom, unicité, type, syntaxe, ordre
 * (une variable ne lit que celles définies au-dessus d'elle). 0 si succès, sinon -1 et message dans err.
 * Une variable source inconnue n'est pas une erreur (source déconnectée) : elle est signalée par hub_calcs_json.
 */
int hub_set_calcs(hub_t *h, const cJSON *vars, char *err, size_t cap);
/** {"variables":[{name, expr, type, unit?, comment?}]} ; avec with_values : path, plcType, value, error? en plus */
void hub_calcs_json(hub_t *h, int with_values, sb_t *out);
/** évalue une expression sur les valeurs courantes : {"value", "type", "error"?} ou {"error"} si syntaxe incorrecte */
void hub_calc_test(hub_t *h, const char *expr, sb_t *out);

/** nombre de variables du schéma courant */
int hub_leaf_count(hub_t *h);

/* ---- appelées par la source, depuis n'importe quel thread ---- */

/** nouveau schéma ; le hub devient propriétaire de `roots` */
void hub_source_roots(hub_t *h, roots_t *roots);
/** bloc binaire complet du symbole racine `root` */
void hub_source_data(hub_t *h, int root, const uint8_t *data, size_t len);
/** changement d'état de la source */
void hub_source_status(hub_t *h, conn_state state, const char *message);

/* ---- fabriques des sources ---- */

source_t *sim_new(hub_t *h, int cycle_ms);

#endif
