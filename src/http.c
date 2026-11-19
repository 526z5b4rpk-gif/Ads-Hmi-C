#include "http.h"
#include "plat.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX_HEADER 16384u
#define MAX_BODY (20u * 1024 * 1024)
#define MAX_FRAME (1u * 1024 * 1024)
#define MAX_CONNS 128
/** délai maximal pour recevoir une requête complète */
#define REQUEST_TIMEOUT_MS 30000
/** délai maximal de fermeture propre après la réponse */
#define DRAIN_TIMEOUT_MS 2000

enum {
    CS_HTTP,  /* lecture de la requête */
    CS_WS,    /* connexion WebSocket ouverte */
    CS_DONE,  /* réponse en cours d'envoi, entrée ignorée */
    CS_DRAIN, /* réponse envoyée, écriture fermée : attente de la fermeture par le client */
};

struct http_conn {
    sock_t fd;
    int state;
    sb_t in;
    sb_t out;
    size_t out_off;
    int close_when_flushed;
    int eof;
    int dead;
    int responded;
    int flag;
    int access;
    int peer_local;
    double created;
    double drain_deadline;
    /* message WebSocket fragmenté en cours */
    sb_t frag;
    int frag_op;
    http_server *srv;
};

struct http_server {
    sock_t lfd;
    int loopback;
    http_handlers h;
    http_conn **conns;
    int n, cap;
    struct pollfd *pfd;
    int pfd_cap;
};

/* ======================================================================= SHA-1 / base64 */

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t r = len - i;
    memcpy(tail, data + i, r);
    tail[r] = 0x80;
    size_t tl = r < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - (size_t)k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail);
    if (tl == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) {
        out[4 * k] = (uint8_t)(h[k] >> 24);
        out[4 * k + 1] = (uint8_t)(h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(h[k] >> 8);
        out[4 * k + 3] = (uint8_t)h[k];
    }
}

static void base64(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

void ws_accept_key(const char *client_key, char *out)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", client_key);
    uint8_t dig[20];
    sha1((const uint8_t *)buf, strlen(buf), dig);
    base64(dig, 20, out);
}

/* ======================================================================= écoute */

http_server *http_listen(const char *host, int port, const http_handlers *h, char *err, size_t cap)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int gai = getaddrinfo(host, ps, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, cap, "adresse d'écoute invalide %s:%d", host, port);
        return NULL;
    }
    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        snprintf(err, cap, "socket : %s", plat_strerror());
        freeaddrinfo(res);
        return NULL;
    }
#if defined(__APPLE__) || defined(__FreeBSD__)
    /* Sous BSD/macOS, SO_REUSEADDR permet de lier 127.0.0.1:port alors qu'un autre programme écoute
       déjà sur *:port : le service démarrerait sans recevoir les connexions. On teste donc le port. */
    {
        sock_t probe = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (probe != SOCK_INVALID) {
            int busy = connect(probe, res->ai_addr, (socklen_t)res->ai_addrlen) == 0;
            sock_close(probe);
            if (busy) {
                snprintf(err, cap, "écoute impossible sur %s:%d : port déjà utilisé par un autre programme", host, port);
                sock_close(fd);
                freeaddrinfo(res);
                return NULL;
            }
        }
    }
#endif
    int one = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (bind(fd, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(fd, 64) != 0 || sock_nonblock(fd) != 0) {
        snprintf(err, cap, "écoute impossible sur %s:%d (%s) — port déjà utilisé ?", host, port, plat_strerror());
        sock_close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    http_server *s = xcalloc(1, sizeof *s);
    s->lfd = fd;
    s->h = *h;
    if (res->ai_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)res->ai_addr;
        s->loopback = (ntohl(a->sin_addr.s_addr) >> 24) == 127;
    } else if (res->ai_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)res->ai_addr;
        s->loopback = IN6_IS_ADDR_LOOPBACK(&a->sin6_addr);
    }
    freeaddrinfo(res);
    return s;
}

static void conn_free(http_conn *c)
{
    sock_close(c->fd);
    sb_free(&c->in);
    sb_free(&c->out);
    sb_free(&c->frag);
    free(c);
}

void http_free(http_server *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) conn_free(s->conns[i]);
    free(s->conns);
    free(s->pfd);
    sock_close(s->lfd);
    free(s);
}

/* ======================================================================= envoi */

static void conn_flush(http_conn *c)
{
    while (!c->dead && c->out_off < c->out.len) {
        size_t left = c->out.len - c->out_off;
        int chunk = left > (1u << 30) ? (1 << 30) : (int)left;
        long n = (long)send(c->fd, c->out.p + c->out_off, chunk, MSG_NOSIGNAL);
        if (n > 0) c->out_off += (size_t)n;
        else if (n < 0 && sock_would_block()) break;
        else c->dead = 1;
    }
    if (c->dead) return;
    if (c->out_off == c->out.len) {
        c->out_off = 0;
        sb_clear(&c->out);
        if (c->close_when_flushed && c->state != CS_DRAIN) {
            /* fermeture en deux temps : le client lit toute la réponse avant la fermeture complète */
            shutdown(c->fd, SHUT_WR);
            c->state = CS_DRAIN;
            c->drain_deadline = mono_ms() + DRAIN_TIMEOUT_MS;
        }
    } else if (c->out_off > 65536 && c->out_off * 2 > c->out.len) {
        memmove(c->out.p, c->out.p + c->out_off, c->out.len - c->out_off);
        c->out.len -= c->out_off;
        c->out_off = 0;
    }
}

static const char *reason(int status)
{
    switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    }
    return "Unknown";
}

void http_respond(http_conn *c, int status, const char *ctype, const char *extra, const void *body, size_t len)
{
    if (c->responded) return;
    c->responded = 1;
    sb_printf(&c->out,
              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n"
              "X-Content-Type-Options: nosniff\r\n%s\r\n",
              status, reason(status), ctype, (unsigned long)len, extra ? extra : "");
    sb_put(&c->out, body, len);
}

/** réponse d'erreur JSON puis fermeture */
static void fail(http_conn *c, int status, const char *msg)
{
    sb_t b = {0};
    sb_puts(&b, "{\"error\":");
    sb_json_str(&b, msg, (size_t)-1);
    sb_putc(&b, '}');
    http_respond(c, status, "application/json; charset=utf-8", NULL, b.p, b.len);
    sb_free(&b);
    c->state = CS_DONE;
    c->close_when_flushed = 1;
    sb_clear(&c->in);
    conn_flush(c);
}

static void ws_frame(http_conn *c, int op, const void *data, size_t len)
{
    uint8_t h[10];
    size_t n;
    h[0] = (uint8_t)(0x80 | op);
    if (len < 126) {
        h[1] = (uint8_t)len;
        n = 2;
    } else if (len < 65536) {
        h[1] = 126;
        h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)len;
        n = 4;
    } else {
        h[1] = 127;
        for (int k = 0; k < 8; k++) h[2 + k] = (uint8_t)((uint64_t)len >> (56 - 8 * k));
        n = 10;
    }
    sb_put(&c->out, h, n);
    sb_put(&c->out, data, len);
}

void ws_send_text(http_conn *c, const char *data, size_t len)
{
    if (c->state != CS_WS || c->dead) return;
    ws_frame(c, 0x1, data, len);
    conn_flush(c);
}

static void ws_close(http_conn *c, int code)
{
    uint8_t p[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    ws_frame(c, 0x8, p, 2);
    c->state = CS_DONE;
    c->close_when_flushed = 1;
    conn_flush(c);
}

size_t http_conn_pending(const http_conn *c) { return c->out.len - c->out_off; }
int *http_conn_access(http_conn *c) { return &c->access; }
int *http_conn_flag(http_conn *c) { return &c->flag; }

void http_ws_foreach(http_server *s, void (*fn)(void *, http_conn *), void *ctx)
{
    for (int i = 0; i < s->n; i++)
        if (s->conns[i]->state == CS_WS && !s->conns[i]->dead) fn(ctx, s->conns[i]);
}

int http_ws_count(http_server *s)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) n += s->conns[i]->state == CS_WS && !s->conns[i]->dead;
    return n;
}

/* ======================================================================= réception */

static void conn_read(http_conn *c)
{
    for (;;) {
        if (c->state == CS_HTTP && c->in.len > MAX_HEADER + MAX_BODY) return; /* le traitement refusera */
        sb_reserve(&c->in, 16384);
        long n = (long)recv(c->fd, c->in.p + c->in.len, 16384, 0);
        if (n > 0) {
            if (c->state == CS_DONE || c->state == CS_DRAIN) continue; /* entrée ignorée */
            c->in.len += (size_t)n;
            c->in.p[c->in.len] = 0;
            continue;
        }
        if (n == 0) { c->eof = 1; return; }
        if (sock_would_block()) return;
        c->dead = 1;
        return;
    }
}

static size_t find_header_end(const char *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return i + 4;
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** décode les %xx ; -1 si invalide (dont %00) ou trop long */
static int url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int a = hexval((unsigned char)p[1]), b = a >= 0 ? hexval((unsigned char)p[2]) : -1;
            if (a < 0 || b < 0) return -1;
            c = a * 16 + b;
            if (c == 0) return -1;
            p += 2;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

/** en écoute locale, l'hôte demandé doit être la machine locale (protection contre le « DNS rebinding ») */
static int host_ok(const http_server *s, const char *host)
{
    if (!s->loopback) return 1;
    if (!host) return 0;
    char name[256];
    str_copy(name, sizeof name, host);
    if (name[0] == '[') {
        char *e = strchr(name, ']');
        if (!e) return 0;
        e[1] = 0;
        return str_ieq(name, "[::1]");
    }
    char *colon = strchr(name, ':');
    if (colon) *colon = 0;
    return str_ieq(name, "localhost") || strncmp(name, "127.", 4) == 0;
}

/** l'en-tête Origin (posé par les navigateurs) doit désigner ce même serveur */
static int origin_ok(const char *origin, const char *host)
{
    if (!origin) return 1; /* client non navigateur (outil, test) */
    if (!host) return 0;
    if (str_istarts(origin, "http://")) origin += 7;
    else if (str_istarts(origin, "https://")) origin += 8;
    else return 0;
    return str_ieq(origin, host);
}

static void process_ws(http_conn *c);

static void process_http(http_conn *c)
{
    http_server *s = c->srv;
    size_t hl = find_header_end(c->in.p, c->in.len);
    if (!hl) {
        if (c->in.len > MAX_HEADER) fail(c, 431, "En-têtes trop volumineux");
        return;
    }
    if (hl > MAX_HEADER) { fail(c, 431, "En-têtes trop volumineux"); return; }

    /* analyse sur une copie : le tampon reste intact tant que le corps n'est pas complet */
    char *hdr = xmalloc(hl + 1);
    memcpy(hdr, c->in.p, hl);
    hdr[hl] = 0;
    char *eol = strstr(hdr, "\r\n");
    *eol = 0;
    char *method = hdr, *target = strchr(hdr, ' ');
    char *version = target ? strchr(target + 1, ' ') : NULL;
    if (!target || !version) { free(hdr); fail(c, 400, "Requête invalide"); return; }
    *target++ = 0;
    *version++ = 0;
    const char *host = NULL, *origin = NULL, *upgrade = NULL, *key = NULL, *te = NULL;
    const char *cookie = NULL, *authz = NULL;
    unsigned long clen = 0;
    for (char *q = eol + 2;;) {
        char *e = strstr(q, "\r\n");
        if (!e || e == q) break;
        *e = 0;
        char *colon = strchr(q, ':');
        if (colon) {
            *colon = 0;
            char *v = trim(colon + 1);
            if (str_ieq(q, "host")) host = v;
            else if (str_ieq(q, "origin")) origin = v;
            else if (str_ieq(q, "upgrade")) upgrade = v;
            else if (str_ieq(q, "sec-websocket-key")) key = v;
            else if (str_ieq(q, "transfer-encoding")) te = v;
            else if (str_ieq(q, "cookie")) cookie = v;
            else if (str_ieq(q, "authorization")) authz = v;
            else if (str_ieq(q, "content-length")) {
                char *end;
                clen = strtoul(v, &end, 10);
                if (*end || *v == '-') { free(hdr); fail(c, 400, "Content-Length invalide"); return; }
            }
        }
        q = e + 2;
    }
    if (te) { free(hdr); fail(c, 501, "Transfer-Encoding non pris en charge"); return; }
    if (clen > MAX_BODY) { free(hdr); fail(c, 413, "Requête trop volumineuse"); return; }
    if (c->in.len < hl + clen) { free(hdr); return; } /* corps incomplet : attendre la suite */

    char *qm = strchr(target, '?');
    const char *query = "";
    if (qm) { *qm = 0; query = qm + 1; }
    char path[2048];
    if (target[0] != '/' || url_decode(target, path, sizeof path) != 0) { free(hdr); fail(c, 400, "Chemin invalide"); return; }
    if (!host_ok(s, host)) { free(hdr); fail(c, 403, "Hôte refusé"); return; }
    int is_ws = upgrade && str_ieq(upgrade, "websocket");
    int modifying = strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "OPTIONS");
    if ((is_ws || modifying) && !origin_ok(origin, host)) { free(hdr); fail(c, 403, "Origine refusée"); return; }

    if (is_ws) {
        if (strcmp(path, "/ws") != 0 || strcmp(method, "GET") != 0 || !key || strlen(key) > 64) {
            free(hdr);
            fail(c, 400, "Ouverture WebSocket invalide");
            return;
        }
        char acc[32];
        ws_accept_key(key, acc);
        sb_printf(&c->out,
                  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: %s\r\n\r\n",
                  acc);
        c->state = CS_WS;
        conn_flush(c);
        http_req wreq = {method, path, query, NULL, 0, cookie, authz, c->peer_local};
        if (s->h.on_ws_open) s->h.on_ws_open(s->h.ctx, c, &wreq);
        free(hdr);
        memmove(c->in.p, c->in.p + hl, c->in.len - hl);
        c->in.len -= hl;
        c->in.p[c->in.len] = 0;
        process_ws(c);
        return;
    }

    http_req req = {method, path, query, c->in.p + hl, clen, cookie, authz, c->peer_local};
    c->responded = 0;
    s->h.on_request(s->h.ctx, c, &req);
    free(hdr);
    if (!c->responded) fail(c, 500, "Aucune réponse");
    c->state = CS_DONE;
    c->close_when_flushed = 1;
    sb_clear(&c->in);
    conn_flush(c);
}

static void process_ws(http_conn *c)
{
    http_server *s = c->srv;
    size_t pos = 0;
    while (c->state == CS_WS) {
        uint8_t *p = (uint8_t *)c->in.p + pos;
        size_t avail = c->in.len - pos;
        if (avail < 2) break;
        int fin = p[0] & 0x80, op = p[0] & 0x0F, masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t h = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = ((uint64_t)p[2] << 8) | p[3];
            h = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int k = 0; k < 8; k++) len = (len << 8) | p[2 + k];
            h = 10;
        }
        if (!masked) { ws_close(c, 1002); break; } /* les trames d'un client sont toujours masquées */
        if (len > MAX_FRAME) { ws_close(c, 1009); break; }
        if (avail < h + 4 + len) break;
        uint8_t *mask = p + h, *data = p + h + 4;
        for (size_t i = 0; i < len; i++) data[i] ^= mask[i & 3];
        pos += h + 4 + (size_t)len;
        switch (op) {
        case 0x8: ws_close(c, 1000); break;
        case 0x9: ws_frame(c, 0xA, data, (size_t)len); conn_flush(c); break;
        case 0xA: break;
        case 0x1:
        case 0x2:
            if (c->frag_op) { ws_close(c, 1002); break; }
            if (fin) {
                if (op == 0x1 && s->h.on_ws_text) s->h.on_ws_text(s->h.ctx, c, (const char *)data, (size_t)len);
            } else {
                c->frag_op = op;
                sb_clear(&c->frag);
                sb_put(&c->frag, data, (size_t)len);
            }
            break;
        case 0x0:
            if (!c->frag_op) { ws_close(c, 1002); break; }
            if (c->frag.len + len > MAX_FRAME) { ws_close(c, 1009); break; }
            sb_put(&c->frag, data, (size_t)len);
            if (fin) {
                int fop = c->frag_op;
                c->frag_op = 0;
                if (fop == 0x1 && s->h.on_ws_text) s->h.on_ws_text(s->h.ctx, c, sb_cstr(&c->frag), c->frag.len);
            }
            break;
        default: ws_close(c, 1002); break;
        }
    }
    if (c->state != CS_WS) {
        sb_clear(&c->in);
    } else if (pos) {
        memmove(c->in.p, c->in.p + pos, c->in.len - pos);
        c->in.len -= pos;
        c->in.p[c->in.len] = 0;
    }
}

/* ======================================================================= boucle */

/** client sur cette machine : 127.0.0.0/8, ::1, ou IPv4 locale encapsulée (::ffff:127.x.x.x) */
static int peer_is_local(const struct sockaddr_storage *a)
{
    if (a->ss_family == AF_INET) {
        const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in *)a)->sin_addr;
        return b[0] == 127;
    }
    if (a->ss_family == AF_INET6) {
        const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in6 *)a)->sin6_addr;
        static const uint8_t lo[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static const uint8_t v4[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, lo, 16) == 0) return 1;
        return memcmp(b, v4, 12) == 0 && b[12] == 127;
    }
    return 0;
}

static void accept_all(http_server *s)
{
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        memset(&peer, 0, sizeof peer);
        sock_t fd = accept(s->lfd, (struct sockaddr *)&peer, &plen);
        if (fd == SOCK_INVALID) return;
        if (s->n >= MAX_CONNS || sock_nonblock(fd) != 0) {
            sock_close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        http_conn *c = xcalloc(1, sizeof *c);
        c->fd = fd;
        c->state = CS_HTTP;
        c->created = mono_ms();
        c->srv = s;
        c->peer_local = peer_is_local(&peer);
        if (s->n == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 16;
            s->conns = xrealloc(s->conns, (size_t)s->cap * sizeof *s->conns);
        }
        s->conns[s->n++] = c;
    }
}

void http_poll(http_server *s, int timeout_ms)
{
    int n = s->n;
    if (n + 1 > s->pfd_cap) {
        s->pfd_cap = (n + 1) * 2;
        s->pfd = xrealloc(s->pfd, (size_t)s->pfd_cap * sizeof *s->pfd);
    }
    s->pfd[0].fd = s->lfd;
    s->pfd[0].events = POLLIN;
    s->pfd[0].revents = 0;
    for (int i = 0; i < n; i++) {
        http_conn *c = s->conns[i];
        short ev = 0;
        if (!c->eof) ev |= POLLIN;
        if (c->out_off < c->out.len) ev |= POLLOUT;
        s->pfd[i + 1].fd = c->fd;
        s->pfd[i + 1].events = ev;
        s->pfd[i + 1].revents = 0;
    }
    int r = poll(s->pfd, (unsigned)(n + 1), timeout_ms);
    if (r < 0) return; /* EINTR (signal) : la boucle principale vérifie l'arrêt */

    double now = mono_ms();
    for (int i = 0; i < n; i++) {
        http_conn *c = s->conns[i];
        short re = s->pfd[i + 1].revents;
        if (re & (POLLIN | POLLHUP | POLLERR)) {
            conn_read(c);
            if (!c->dead && c->state == CS_HTTP) process_http(c);
            else if (!c->dead && c->state == CS_WS) process_ws(c);
        }
        if (!c->dead && (re & POLLOUT)) conn_flush(c);
        if (c->eof && (c->state == CS_HTTP || c->state == CS_WS || c->state == CS_DRAIN || http_conn_pending(c) == 0))
            c->dead = 1;
        if (c->state == CS_DRAIN && now > c->drain_deadline) c->dead = 1;
        if (c->state == CS_HTTP && now - c->created > REQUEST_TIMEOUT_MS) c->dead = 1;
    }
    if (s->pfd[0].revents & POLLIN) accept_all(s);

    int k = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->conns[i]->dead) conn_free(s->conns[i]);
        else s->conns[k++] = s->conns[i];
    }
    s->n = k;
}
