/*
 * Couche plateforme : seul fichier d'en-tête qui dépend du système (POSIX ou Windows).
 * Threads, verrous, temps, sockets et fichiers.
 */
#ifndef HMI_PLAT_H
#define HMI_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
/* WSAPoll et inet_pton : Windows 7 minimum (également requis par le SDK livré avec LabWindows/CVI) */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
typedef CRITICAL_SECTION plat_mutex;
typedef HANDLE plat_thread;
#define poll WSAPoll
#define SHUT_WR SD_SEND
#else
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
typedef pthread_mutex_t plat_mutex;
typedef pthread_t plat_thread;
#endif

/* ------------------------------------------------------------- threads */

void pm_init(plat_mutex *m);
void pm_lock(plat_mutex *m);
void pm_unlock(plat_mutex *m);
void pm_destroy(plat_mutex *m);

/** démarre un thread exécutant fn(arg) ; 0 si succès */
int pt_start(plat_thread *t, void (*fn)(void *), void *arg);
/** attend la fin du thread */
void pt_join(plat_thread t);

/**
 * Drapeau partagé entre threads (lecture/écriture atomiques).
 * Remplace <stdatomic.h>, absent de la bibliothèque C de LabWindows/CVI.
 */
typedef volatile long plat_flag;
long pf_get(plat_flag *f);
void pf_set(plat_flag *f, long v);

/* ------------------------------------------------------------- temps */

/** temps monotone en millisecondes (mesure de durées, cadencement) */
double mono_ms(void);
/** heure murale en millisecondes depuis 1970 (horodatage des messages, comme Date.now()) */
int64_t wall_ms(void);
void sleep_ms(int ms);
/** date et heure décomposées (année complète, mois 1-12) */
typedef struct {
    int year, mon, day, hour, min, sec, ms;
} plat_date;
/**
 * Date courante, locale ou UTC. N'utilise ni time_t ni localtime_r/localtime_s :
 * sous Windows elle vient directement de l'API Win32 (l'origine de time_t diffère selon les
 * bibliothèques C, notamment celle de LabWindows/CVI).
 */
void plat_now(plat_date *d, int local);
/** date UTC ISO 8601 avec millisecondes, ex. 2026-11-15T20:30:00.123Z (comme toISOString()) */
void iso_utc_now(char out[32]);

/* ------------------------------------------------------------- sockets */

/** initialisation réseau (WSAStartup sous Windows, SIGPIPE ignoré sous POSIX) */
int net_init(void);
/**
 * Installe fn, appelée quand l'arrêt est demandé : Ctrl+C, SIGTERM, ou sous Windows fermeture de la
 * console, fermeture de session et arrêt du système. fn doit seulement positionner un drapeau.
 */
void plat_on_quit(void (*fn)(void));
void sock_close(sock_t s);
int sock_nonblock(sock_t s);
/** la dernière opération a échoué parce qu'elle aurait bloqué (EAGAIN / WSAEWOULDBLOCK) */
int sock_would_block(void);
/**
 * Connexion TCP sortante (client), au plus `timeout_ms`. Socket renvoyée non bloquante, TCP_NODELAY.
 * SOCK_INVALID en cas d'échec, message dans err.
 */
sock_t tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t cap);

/* ------------------------------------------------------------- fichiers */

/** lit tout un fichier (alloué, terminé par nul) ; NULL si erreur. `notfound` (facultatif) = 1 si absent. */
char *file_read(const char *path, size_t *len, int *notfound);
/**
 * Écriture atomique : fichier temporaire écrit et forcé sur disque, puis renommé.
 * Une coupure de courant laisse soit l'ancien contenu, soit le nouveau, jamais un fichier tronqué.
 */
int file_write_atomic(const char *path, const void *data, size_t len);
int file_copy(const char *src, const char *dst);
int file_remove(const char *path);
/** crée le dossier et ses parents (mkdir -p) */
int dir_make(const char *path);
int path_is_dir(const char *path);
int path_is_file(const char *path);
/** appelle fn pour chaque entrée du dossier (hors . et ..) ; fn renvoie 0 pour continuer */
int dir_list(const char *dir, int (*fn)(const char *name, void *ctx), void *ctx);
/** message de la dernière erreur système (errno / GetLastError) */
const char *plat_strerror(void);

#endif
