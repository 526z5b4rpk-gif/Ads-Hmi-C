/*
 * Arbre de types neutre et mise à plat des variables.
 *
 * Une source (ADS ou simulateur) décrit chaque symbole racine par un arbre de type_node (structures,
 * tableaux, feuilles). leaves_compile aplatit ces arbres en une liste de leaf_t : une entrée par variable
 * élémentaire, avec son chemin complet (« GVL.st.aTab[2].rVal ») et son emplacement exact (offset, bit)
 * dans le bloc binaire du symbole racine. Le hub n'a ensuite qu'à lire ces offsets pour décoder.
 */
#ifndef HMI_TYPES_H
#define HMI_TYPES_H

#include "util.h"

/** Nature d'une variable pour l'interface (choix du widget, saisie, affichage). */
typedef enum { LK_BOOL, LK_NUMBER, LK_STRING, LK_ENUM } leaf_kind;

/** Représentation binaire d'un type élémentaire (mémoire PLC, little-endian). */
typedef enum {
    PR_BOOL, PR_BIT, PR_I8, PR_U8, PR_I16, PR_U16, PR_I32, PR_U32,
    PR_I64, PR_U64, PR_F32, PR_F64, PR_STRING, PR_WSTRING
} prim_t;

/** Valeur nommée d'une énumération PLC. */
typedef struct {
    const char *name;
    double value;
} enum_entry;

/** Attribut PLC ({attribute 'unit' := 'bar'} donne key = "unit", value = "bar"). */
typedef struct {
    const char *key;
    const char *value;
} attr_t;

typedef enum { TN_LEAF, TN_STRUCT, TN_ARRAY } tn_kind;

typedef struct type_node type_node;

/** Membre d'une structure. */
typedef struct {
    const char *name;
    /** offset en octets dans la structure */
    int offset;
    /** membres BIT : offset en bits depuis le début de la structure ; sinon -1 */
    int bit_offset;
    type_node *node;
    /** commentaire PLC du membre (libellé par défaut des widgets) */
    const char *comment;
    const attr_t *attrs;
    int nattrs;
} member_t;

/** Dimension de tableau : premier indice et nombre d'éléments. */
typedef struct {
    int start;
    int length;
} array_dim;

/** Nœud de l'arbre de types. Les champs utilisés dépendent de `kind`. */
struct type_node {
    tn_kind kind;
    /** nom du type PLC (REAL, ST_Axe, ARRAY [1..8] OF REAL…) */
    const char *type_name;
    /** taille en octets */
    int size;
    /* TN_LEAF */
    prim_t prim;
    leaf_kind lkind;
    const enum_entry *enums;
    int nenums;
    const attr_t *attrs;
    int nattrs;
    const char *comment;
    /* TN_STRUCT */
    member_t *members;
    int nmembers;
    /* TN_ARRAY : éléments contigus, dernier indice le plus rapide (ordre mémoire IEC) */
    array_dim *dims;
    int ndims;
    type_node *elem;
};

/** Symbole racine : un chemin PLC et son type. */
typedef struct {
    const char *path;
    type_node *node;
} root_def;

/**
 * Ensemble de symboles racines, propriétaire de tous ses nœuds (alloués dans `arena`).
 * Transmis par une source au hub, qui en devient propriétaire.
 */
typedef struct {
    arena_t *arena;
    root_def *v;
    int n, cap;
} roots_t;

roots_t *roots_new(void);
/** ajoute une racine ; `path` est copié dans l'arène, `node` doit y être alloué */
void roots_add(roots_t *r, const char *path, type_node *node);
void roots_free(roots_t *r);

/** Variable élémentaire mise à plat. */
typedef struct {
    /** position dans la liste (indice utilisé dans les messages WebSocket) */
    int index;
    /** chemin complet, identifiant utilisé par les écrans */
    const char *path;
    const char *type;
    leaf_kind kind;
    const char *comment;
    const enum_entry *enums;
    int nenums;
    /** attributs fusionnés (ceux du type puis ceux du membre, le membre l'emporte) */
    const attr_t *attrs;
    int nattrs;
    /** indice du symbole racine (bloc binaire) contenant la variable */
    int root;
    /** offset en octets dans le bloc */
    int offset;
    /** bit dans l'octet (0-7) pour PR_BIT */
    int bit;
    int size;
    prim_t prim;
} leaf_t;

/** Liste de variables ; chemins et attributs fusionnés sont alloués dans `arena`. */
typedef struct {
    leaf_t *v;
    int n, cap;
    arena_t *arena;
} leaf_list;

/**
 * Aplatit tous les symboles racines, dans l'ordre (les variables d'une même racine sont contiguës).
 * `max_array_expand` : nombre maximal d'éléments développés par tableau (les suivants sont ignorés).
 * Les nœuds de `roots` doivent rester vivants aussi longtemps que la liste (types, commentaires, énumérations).
 */
void leaves_compile(const roots_t *roots, int max_array_expand, leaf_list *out);
void leaves_free(leaf_list *l);

/** "bool", "number", "string", "enum" (valeurs du protocole web) */
const char *leaf_kind_name(leaf_kind k);

/**
 * Type PLC élémentaire -> représentation binaire et nature ; 0 si connu, -1 sinon.
 * Les types date/heure sont des entiers bruts ; un type inconnu de 1, 2, 4 ou 8 octets
 * est traité comme un entier non signé de cette taille (size = 0 : pas de repli).
 */
int plc_prim(const char *name, int size, prim_t *prim, leaf_kind *kind);

/* ------------------------------------------------------------------------------
 * Construction d'arbres avec calcul des offsets (alignement naturel plafonné à 8 octets,
 * comme TwinCAT 3 par défaut). Utilisée par le simulateur et les tests.
 */

/** Type élémentaire : BOOL, INT, REAL, LREAL, STRING(80)… ; `unit` facultatif. NULL si type inconnu. */
type_node *tn_leaf(arena_t *a, const char *plc_type, const char *unit);
/** Énumération stockée sur `base` (ex. "INT") et nommée `enum_type` ; `e` doit rester vivant. */
type_node *tn_enum(arena_t *a, const char *base, const char *enum_type, const enum_entry *e, int n);

typedef struct {
    const char *name;
    type_node *node;
    const char *comment;
} member_spec;

/** Structure : membres placés à l'offset aligné suivant, taille arrondie à l'alignement maximal. */
type_node *tn_struct(arena_t *a, const char *type_name, const member_spec *m, int n);
/** Tableau à une dimension ARRAY [start..start+length-1] OF elem. */
type_node *tn_array(arena_t *a, type_node *elem, int start, int length);

#endif
