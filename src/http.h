/*
 * Serveur HTTP/1.1 + WebSocket (RFC 6455) minimal, mono-thread, à base de poll() non bloquant.
 *
 * Volontairement simple et auditable :
 *  - une requête par connexion HTTP (réponse avec « Connection: close ») ;
 *  - corps de requête limité à 20 Mo, en-têtes à 16 Ko, trames WebSocket entrantes à 1 Mo ;
 *  - protections contre les pages web malveillantes ouvertes sur le même poste :
 *      · en écoute locale (127.0.0.1), l'en-tête Host doit désigner la machine locale (anti « DNS rebinding ») ;
 *      · toute requête modifiante et toute ouverture WebSocket portant un en-tête Origin doit venir
 *        de la même origine (une autre page ne peut ni écrire dans l'automate ni modifier les écrans).
 */
#ifndef HMI_HTTP_H
#define HMI_HTTP_H

#include <stddef.h>

typedef struct http_server http_server;
typedef struct http_conn http_conn;

/** Requête HTTP complète (chaînes valides pendant l'appel du gestionnaire uniquement). */
typedef struct {
    const char *method;
    /** chemin décodé (%xx), sans la chaîne de requête */
    const char *path;
    const char *query;
    const char *body;
    size_t body_len;
    /** en-têtes Cookie et Authorization (NULL si absents) */
    const char *cookie;
    const char *authorization;
    /** 1 si le client est sur cette machine (127.0.0.0/8, ::1) */
    int peer_local;
} http_req;

typedef struct {
    void *ctx;
    /** requête HTTP : le gestionnaire doit appeler http_respond une fois */
    void (*on_request)(void *ctx, http_conn *c, const http_req *req);
    /** connexion WebSocket ouverte sur /ws (requête d'ouverture : cookie, chaîne de requête…) */
    void (*on_ws_open)(void *ctx, http_conn *c, const http_req *req);
    /** message texte complet reçu */
    void (*on_ws_text)(void *ctx, http_conn *c, const char *data, size_t len);
} http_handlers;

/** écoute sur host:port ; NULL et message dans err en cas d'échec */
http_server *http_listen(const char *host, int port, const http_handlers *h, char *err, size_t cap);
/** traite les événements réseau, attend au plus `timeout_ms` */
void http_poll(http_server *s, int timeout_ms);
void http_free(http_server *s);

/** envoie la réponse ; `extra_headers` : lignes « Nom: valeur\r\n » supplémentaires ou NULL */
void http_respond(http_conn *c, int status, const char *content_type, const char *extra_headers, const void *body,
                  size_t len);

/** envoie un message texte WebSocket */
void ws_send_text(http_conn *c, const char *data, size_t len);
/** octets en attente d'envoi (client lent) */
size_t http_conn_pending(const http_conn *c);
/** indicateur libre à l'usage de l'application (ex. client à resynchroniser) */
int *http_conn_flag(http_conn *c);
/** second indicateur libre (ex. niveau d'accès de la connexion WebSocket) */
int *http_conn_access(http_conn *c);
/** appelle fn pour chaque connexion WebSocket ouverte */
void http_ws_foreach(http_server *s, void (*fn)(void *ctx, http_conn *c), void *ctx);
/** nombre de connexions WebSocket ouvertes */
int http_ws_count(http_server *s);

/** clé Sec-WebSocket-Accept pour une clé client (exposée pour les tests) ; out >= 29 octets */
void ws_accept_key(const char *client_key, char *out);

#endif
