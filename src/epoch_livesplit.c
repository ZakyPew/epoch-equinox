/* See epoch_livesplit.h. */
#include "epoch_livesplit.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET es_sock;
#define ES_INVALID INVALID_SOCKET
#define es_close closesocket
#define es_would_block() (WSAGetLastError() == WSAEWOULDBLOCK)
#define es_in_progress() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int es_sock;
#define ES_INVALID (-1)
#define es_close close
#define es_would_block() (errno == EAGAIN || errno == EWOULDBLOCK)
#define es_in_progress() (errno == EINPROGRESS)
#endif

/* LiveSplit Server's default. */
#define LS_DEFAULT_PORT 16834
/* Frames between reconnection attempts: two seconds at 60fps. Often
 * enough that starting LiveSplit mid-session picks up on its own,
 * seldom enough that a machine with no LiveSplit is not doing this
 * constantly. */
#define LS_RETRY_FRAMES 120

static bool    g_enabled = false;
static int     g_port = LS_DEFAULT_PORT;
static es_sock g_sock = ES_INVALID;
static bool    g_connecting = false;
static bool    g_ready = false;
static int     g_retry = 0;
static bool    g_said_hello = false;   /* one line per session, at most */

#ifdef _WIN32
static bool g_wsa = false;
static void es_startup(void) {
    if (!g_wsa) {
        WSADATA d;
        g_wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
}
#else
static void es_startup(void) {}
#endif

static void es_drop(void) {
    if (g_sock != ES_INVALID) es_close(g_sock);
    g_sock = ES_INVALID;
    g_connecting = false;
    g_ready = false;
}

static void es_set_nonblocking(es_sock s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Start a connection. Returns with g_connecting set when it is under
 * way; anything that fails outright just leaves us disconnected. */
static void es_begin(void) {
    es_startup();
    es_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == ES_INVALID) return;
    es_set_nonblocking(s);
    /* Splits are tiny and latency matters more than packing them. */
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    /* Loopback only. LiveSplit is on the same machine, and pointing this
     * at anything else would be a way to leak a run to the network. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == 0) {
        g_sock = s;
        g_ready = true;
        g_connecting = false;
    } else if (es_in_progress()) {
        g_sock = s;
        g_connecting = true;
    } else {
        es_close(s);
    }
}

/* Has an in-progress connect finished? Zero timeout: this is a poll. */
static void es_finish_connect(void) {
    fd_set w;
    FD_ZERO(&w);
#ifdef _WIN32
    FD_SET(g_sock, &w);
#else
    if (g_sock >= FD_SETSIZE) { es_drop(); return; }
    FD_SET(g_sock, &w);
#endif
    struct timeval zero = {0, 0};
    int rc = select((int)g_sock + 1, NULL, &w, NULL, &zero);
    if (rc <= 0) return;                    /* still pending */

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(g_sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err) {
        es_drop();
        return;
    }
    g_connecting = false;
    g_ready = true;
    if (!g_said_hello) {
        fprintf(stderr, "[livesplit] connected on port %d\n", g_port);
        g_said_hello = true;
    }
}

static void es_send(const char* cmd) {
    if (!g_enabled || !g_ready || g_sock == ES_INVALID) return;
    char line[64];
    int n = snprintf(line, sizeof(line), "%s\r\n", cmd);
    if (n <= 0 || (size_t)n >= sizeof(line)) return;
#ifdef _WIN32
    int sent = send(g_sock, line, n, 0);
#else
    /* No SIGPIPE: LiveSplit closing while we write must not kill the
     * player. MSG_NOSIGNAL where it exists, SO_NOSIGPIPE is set on the
     * platforms that want that instead. */
#ifdef MSG_NOSIGNAL
    int sent = (int)send(g_sock, line, (size_t)n, MSG_NOSIGNAL);
#else
    int sent = (int)send(g_sock, line, (size_t)n, 0);
#endif
#endif
    if (sent < 0 && !es_would_block()) es_drop();   /* gone; retry later */
}

void epoch_livesplit_enable(bool on, int port) {
    if (port > 0 && port < 65536) g_port = port;
    if (on == g_enabled) return;
    g_enabled = on;
    if (!on) es_drop();
    else g_retry = 0;               /* try immediately on the next tick */
}

bool epoch_livesplit_connected(void) { return g_enabled && g_ready; }

void epoch_livesplit_split(void) { es_send("startorsplit"); }
void epoch_livesplit_reset(void) { es_send("reset"); }
void epoch_livesplit_pause(bool paused) {
    es_send(paused ? "pause" : "resume");
}

void epoch_livesplit_tick(void) {
    if (!g_enabled) return;
    if (g_connecting) { es_finish_connect(); return; }
    if (g_ready) return;
    if (g_retry-- > 0) return;
    g_retry = LS_RETRY_FRAMES;
    es_begin();
}

void epoch_livesplit_shutdown(void) {
    es_drop();
    g_enabled = false;
}
