#include "plat.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
/* rien de plus : uniquement l'API Win32 (pas de <direct.h>, absent de la bibliothèque de CVI) */
#else
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------- threads */

typedef struct {
    void (*fn)(void *);
    void *arg;
} thread_start;

#ifdef _WIN32

void pm_init(plat_mutex *m) { InitializeCriticalSection(m); }
void pm_lock(plat_mutex *m) { EnterCriticalSection(m); }
void pm_unlock(plat_mutex *m) { LeaveCriticalSection(m); }
void pm_destroy(plat_mutex *m) { DeleteCriticalSection(m); }

static DWORD WINAPI thread_main(LPVOID p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int pt_start(plat_thread *t, void (*fn)(void *), void *arg)
{
    thread_start *s = xmalloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, thread_main, s, 0, NULL);
    if (!*t) { free(s); return -1; }
    return 0;
}

void pt_join(plat_thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

long pf_get(plat_flag *f) { return InterlockedCompareExchange((LONG volatile *)f, 0, 0); }
void pf_set(plat_flag *f, long v) { InterlockedExchange((LONG volatile *)f, v); }

#else

void pm_init(plat_mutex *m) { pthread_mutex_init(m, NULL); }
void pm_lock(plat_mutex *m) { pthread_mutex_lock(m); }
void pm_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }
void pm_destroy(plat_mutex *m) { pthread_mutex_destroy(m); }

static void *thread_main(void *p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int pt_start(plat_thread *t, void (*fn)(void *), void *arg)
{
    thread_start *s = xmalloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, thread_main, s) != 0) { free(s); return -1; }
    return 0;
}

void pt_join(plat_thread t) { pthread_join(t, NULL); }

long pf_get(plat_flag *f) { return __atomic_load_n(f, __ATOMIC_SEQ_CST); }
void pf_set(plat_flag *f, long v) { __atomic_store_n(f, v, __ATOMIC_SEQ_CST); }

#endif

/* ------------------------------------------------------------- temps */

#ifdef _WIN32

double mono_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

int64_t wall_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns depuis 1601 */
    return (int64_t)(t / 10000 - 11644473600000ULL);
}

void sleep_ms(int ms) { Sleep(ms > 0 ? (DWORD)ms : 0); }

#else

double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms)
{
    if (ms <= 0) return;
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

#endif

void plat_now(plat_date *d, int local)
{
#ifdef _WIN32
    SYSTEMTIME st;
    if (local) GetLocalTime(&st); else GetSystemTime(&st);
    d->year = st.wYear; d->mon = st.wMonth; d->day = st.wDay;
    d->hour = st.wHour; d->min = st.wMinute; d->sec = st.wSecond; d->ms = st.wMilliseconds;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    struct tm tmv;
    if (local) localtime_r(&t, &tmv); else gmtime_r(&t, &tmv);
    d->year = tmv.tm_year + 1900; d->mon = tmv.tm_mon + 1; d->day = tmv.tm_mday;
    d->hour = tmv.tm_hour; d->min = tmv.tm_min; d->sec = tmv.tm_sec; d->ms = (int)(ts.tv_nsec / 1000000);
#endif
}

void iso_utc_now(char out[32])
{
    plat_date d;
    plat_now(&d, 0);
    /* modulos : bornes explicites pour -Wformat-truncation (GCC/MinGW), sans effet sur une date valide */
    snprintf(out, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", d.year % 10000, d.mon % 100, d.day % 100,
             d.hour % 100, d.min % 100, d.sec % 100, d.ms % 1000);
}

/* ------------------------------------------------------------- sockets */

#ifdef _WIN32

int net_init(void)
{
    WSADATA w;
    /* journal en UTF-8 : accents corrects dans la console */
    SetConsoleOutputCP(CP_UTF8);
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
}

static void (*g_quit_fn)(void);

static BOOL WINAPI on_console_event(DWORD ev)
{
    if (!g_quit_fn) return FALSE;
    g_quit_fn();
    /* fermeture de la fenêtre, de la session, arrêt : Windows termine le processus au retour ;
       on laisse à la boucle principale le temps de s'arrêter proprement (elle appelle ExitProcess) */
    if (ev == CTRL_CLOSE_EVENT || ev == CTRL_LOGOFF_EVENT || ev == CTRL_SHUTDOWN_EVENT) Sleep(3000);
    return TRUE;
}

void plat_on_quit(void (*fn)(void))
{
    g_quit_fn = fn;
    SetConsoleCtrlHandler(on_console_event, TRUE);
}
void sock_close(sock_t s) { closesocket(s); }
int sock_nonblock(sock_t s)
{
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0 ? 0 : -1;
}
int sock_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }

#else

int net_init(void)
{
    /* une écriture vers un client déconnecté ne doit pas tuer le processus */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}
void sock_close(sock_t s) { close(s); }
int sock_nonblock(sock_t s)
{
    int fl = fcntl(s, F_GETFL, 0);
    return (fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0) ? 0 : -1;
}
int sock_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

static void (*g_quit_fn)(void);
static void on_signal(int sig) { (void)sig; if (g_quit_fn) g_quit_fn(); }

void plat_on_quit(void (*fn)(void))
{
    g_quit_fn = fn;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}

#endif

static int sock_errno(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

sock_t tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t cap)
{
    struct addrinfo hints, *res = NULL;
    char ps[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    int gai = getaddrinfo(host, ps, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, cap, "adresse inconnue : %s", host);
        return SOCK_INVALID;
    }
    sock_t fd = SOCK_INVALID;
    int last = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == SOCK_INVALID) { last = sock_errno(); continue; }
        if (sock_nonblock(fd) != 0) { last = sock_errno(); sock_close(fd); fd = SOCK_INVALID; continue; }
        int rc = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc != 0) {
            int e = sock_errno();
#ifdef _WIN32
            int pending = e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
            int pending = e == EINPROGRESS || e == EINTR;
#endif
            if (!pending) { last = e; sock_close(fd); fd = SOCK_INVALID; continue; }
            struct pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            int pr = poll(&p, 1, timeout_ms);
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (pr <= 0) { last = -1; sock_close(fd); fd = SOCK_INVALID; continue; }
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) != 0 || soerr != 0) {
                last = soerr ? soerr : sock_errno();
                sock_close(fd);
                fd = SOCK_INVALID;
                continue;
            }
        }
        break;
    }
    freeaddrinfo(res);
    if (fd == SOCK_INVALID) {
        if (last == -1) snprintf(err, cap, "pas de réponse de %s:%d (délai de %d ms dépassé)", host, port, timeout_ms);
        else {
#ifdef _WIN32
            snprintf(err, cap, "connexion à %s:%d impossible (erreur Winsock %d)", host, port, last);
#else
            snprintf(err, cap, "connexion à %s:%d impossible : %s", host, port, strerror(last));
#endif
        }
        return SOCK_INVALID;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return fd;
}

/* ------------------------------------------------------------- fichiers */

char *file_read(const char *path, size_t *len, int *notfound)
{
    if (notfound) *notfound = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (notfound && errno == ENOENT) *notfound = 1;
        return NULL;
    }
    sb_t b = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_put(&b, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err) { sb_free(&b); return NULL; }
    sb_cstr(&b);
    if (len) *len = b.len;
    return b.p;
}

#ifdef _WIN32

int file_write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const char *p = data;
    while (len > 0) {
        DWORD w = 0, chunk = len > 0x10000000 ? 0x10000000 : (DWORD)len;
        if (!WriteFile(h, p, chunk, &w, NULL)) { CloseHandle(h); DeleteFileA(tmp); return -1; }
        p += w;
        len -= w;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileA(tmp); return -1; }
    return 0;
}

int file_remove(const char *path) { return DeleteFileA(path) ? 0 : -1; }

int dir_make(const char *path)
{
    char buf[1024];
    str_copy(buf, sizeof buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = c;
        }
    }
    CreateDirectoryA(buf, NULL);
    return path_is_dir(path) ? 0 : -1;
}

int path_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int path_is_file(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_list(const char *dir, int (*fn)(const char *, void *), void *ctx)
{
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            if (fn(fd.cFileName, ctx)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

const char *plat_strerror(void)
{
    static char buf[256];
    DWORD e = GetLastError();
    if (!e) return strerror(errno);
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, buf, sizeof buf, NULL);
    return buf;
}

#else

int file_write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const char *p = data;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    /* données sur disque avant le renommage : sinon une coupure peut laisser un fichier vide */
    if (fsync(fd) != 0 || close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    /* le renommage lui-même est rendu durable en synchronisant le dossier */
    char dir[1024];
    str_copy(dir, sizeof dir, path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else str_copy(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return 0;
}

int file_remove(const char *path) { return unlink(path); }

int dir_make(const char *path)
{
    char buf[1024];
    str_copy(buf, sizeof buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
    return path_is_dir(path) ? 0 : -1;
}

int path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int path_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int dir_list(const char *dir, int (*fn)(const char *, void *), void *ctx)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (fn(e->d_name, ctx)) break;
    }
    closedir(d);
    return 0;
}

const char *plat_strerror(void) { return strerror(errno); }

#endif

int file_copy(const char *src, const char *dst)
{
    size_t len;
    char *data = file_read(src, &len, NULL);
    if (!data) return -1;
    int rc = file_write_atomic(dst, data, len);
    free(data);
    return rc;
}
