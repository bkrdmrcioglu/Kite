/*
 * Local proxy: accepts HTTP CONNECT, plain HTTP and SOCKS5 clients, connects
 * to the real server and applies a desync strategy to the first payload.
 */
#include "compat.h" /* first: winsock2.h must precede windows.h */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"
#include "net.h"

#define KITE_VERSION "0.1.6"
#define BUF_SIZE 65536
#define HELLO_MAX (16384 + 5 + 4096)
#define IDLE_TIMEOUT_MS (10 * 60 * 1000)

kite_stats g_stats;
int g_verbose;

static kite_config g_cfg;
static pthread_t accept_thread;
static atomic_int running;

const char *kite_version(void) { return KITE_VERSION; }

void kite_stat_add(uint64_t *field, uint64_t n)
{
    __atomic_fetch_add(field, n, __ATOMIC_RELAXED);
}

void kite_get_stats(kite_stats *out)
{
    uint64_t *src = (uint64_t *)&g_stats, *dst = (uint64_t *)out;
    for (size_t i = 0; i < sizeof(kite_stats) / sizeof(uint64_t); i++)
        dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

void kite_log(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[kite] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void kite_config_default(kite_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = 8899;
    cfg->strategy = KITE_STRATEGY_TLSREC;
    cfg->tcp_split = 1;
}

void kite_set_resolver(kite_resolver_fn fn, void *ctx)
{
    kite_dns_set_resolver(fn, ctx);
}

/* ---- upstream ---------------------------------------------------------- */

static int connect_host(const char *host, uint16_t port)
{
    uint32_t addrs[8];
    int n = kite_dns_resolve(host, addrs, 8);
    for (int i = 0; i < n; i++) {
        uint8_t first = (uint8_t)(ntohl(addrs[i]) >> 24);
        if (first == 127 || first == 0)
            continue;
        int fd = kite_connect_timeout(addrs[i], htons(port), 5000);
        if (fd >= 0) {
            kite_set_nodelay(fd, 1);
            return fd;
        }
    }
    kite_log("connect: %s:%u failed", host, port);
    return -1;
}

/* ---- desync ------------------------------------------------------------ */

static int send_desync_tls(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0, hlen = 0;
    int has_sni = kite_tls_find_sni(buf, len, &off, &hlen) == 0;
    /* split two characters into the hostname, as verified against TR DPI */
    size_t cut = has_sni ? off + (hlen > 2 ? 2 : 1) : 6;

    switch (g_cfg.strategy) {
    case KITE_STRATEGY_TLSREC: {
        uint8_t *out = malloc(len + 5);
        size_t n = out ? kite_tls_split_record(buf, len, cut - 5, out) : 0;
        if (n == 0) {
            free(out);
            return kite_send_all(fd, buf, len);
        }
        int rc;
        if (g_cfg.tcp_split) {
            /* first record's header + payload up to the cut, then the rest */
            size_t first = cut;
            rc = kite_send_all(fd, out, first);
            if (rc == 0) {
                usleep(1000);
                rc = kite_send_all(fd, out + first, n - first);
            }
        } else {
            rc = kite_send_all(fd, out, n);
        }
        free(out);
        return rc;
    }
    case KITE_STRATEGY_BYTES: {
        size_t end = has_sni ? off + hlen : (len < 64 ? len : 64);
        for (size_t i = 0; i < end; i++) {
            if (kite_send_all(fd, buf + i, 1) < 0)
                return -1;
            usleep(1000);
        }
        return kite_send_all(fd, buf + end, len - end);
    }
    case KITE_STRATEGY_SPLIT:
        if (kite_send_all(fd, buf, cut) < 0)
            return -1;
        usleep(1000);
        return kite_send_all(fd, buf + cut, len - cut);
    default:
        return kite_send_all(fd, buf, len);
    }
}

static int send_desync_http(int fd, uint8_t *buf, size_t len)
{
    long host = kite_http_host_offset(buf, len);
    if (host < 0 || g_cfg.strategy == KITE_STRATEGY_NONE)
        return kite_send_all(fd, buf, len);
    size_t cut = (size_t)host + 1;
    if (kite_send_all(fd, buf, cut) < 0)
        return -1;
    usleep(1000);
    return kite_send_all(fd, buf + cut, len - cut);
}

/* ---- hosts that reject the desync ---------------------------------------
 * A few strict servers (e.g. some load balancers) close the connection when
 * the ClientHello arrives split across TLS records. Those hosts are not the
 * ones being censored, so they are remembered and sent the hello unmodified. */

#define PLAIN_HOSTS 256
static char plain_hosts[PLAIN_HOSTS][256];
static pthread_mutex_t plain_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned host_slot(const char *h)
{
    unsigned x = 5381;
    while (*h)
        x = x * 33 + (unsigned char)*h++;
    return x % PLAIN_HOSTS;
}

static int host_is_plain(const char *host)
{
    if (!host || strlen(host) >= 256)
        return 0;
    pthread_mutex_lock(&plain_lock);
    int hit = strcmp(plain_hosts[host_slot(host)], host) == 0;
    pthread_mutex_unlock(&plain_lock);
    return hit;
}

static void host_mark_plain(const char *host)
{
    if (!host || strlen(host) >= 256)
        return;
    pthread_mutex_lock(&plain_lock);
    strcpy(plain_hosts[host_slot(host)], host);
    pthread_mutex_unlock(&plain_lock);
}

/* True when the server closed or reset the connection before answering.
 * A TLS server always replies to a ClientHello, so silence is not rejection. */
static int server_rejected(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, 5000) != 1)
        return 0;
    char c;
    return recv(fd, &c, 1, MSG_PEEK) <= 0;
}

/* Reads the client's first payload and forwards it with the desync applied.
 * If the server rejects the reshaped hello, reconnects and sends it as is;
 * *server is replaced in that case (and is -1 if reconnecting failed). */
static int forward_first(int client, int *server, const char *host, uint16_t port,
                         const uint8_t *pre, size_t prelen)
{
    uint8_t *buf = malloc(HELLO_MAX);
    if (!buf)
        return -1;
    size_t len = 0;
    if (prelen > 0) {
        if (prelen > HELLO_MAX)
            prelen = HELLO_MAX;
        memcpy(buf, pre, prelen);
        len = prelen;
    } else {
        struct pollfd p = { .fd = client, .events = POLLIN };
        /* server-speaks-first protocols: nothing to reshape */
        if (poll(&p, 1, 3000) != 1) {
            free(buf);
            return 0;
        }
        ssize_t n = k_recv(client, buf, HELLO_MAX);
        if (n <= 0) {
            free(buf);
            return -1;
        }
        len = (size_t)n;
    }

    int rc;
    if (kite_is_tls_hello(buf, len)) {
        kite_stat_add(&g_stats.tls_seen, 1);
        /* large (post-quantum) hellos span several reads: collect the record */
        size_t want = kite_tls_record_len(buf, len);
        while (want <= HELLO_MAX && len < want) {
            struct pollfd p = { .fd = client, .events = POLLIN };
            if (poll(&p, 1, 2000) != 1)
                break;
            ssize_t n = k_recv(client, buf + len, HELLO_MAX - len);
            if (n <= 0)
                break;
            len += (size_t)n;
        }
        if (g_cfg.strategy == KITE_STRATEGY_NONE || host_is_plain(host)) {
            rc = kite_send_all(*server, buf, len);
        } else {
            rc = send_desync_tls(*server, buf, len);
            if (rc == 0 && host && server_rejected(*server)) {
                kite_log("%s rejected the desync, retrying unmodified", host);
                host_mark_plain(host);
                sock_close(*server);
                *server = connect_host(host, port);
                rc = *server >= 0 ? kite_send_all(*server, buf, len) : -1;
            } else if (rc == 0) {
                kite_stat_add(&g_stats.desynced, 1);
            }
        }
    } else if (kite_is_http_request(buf, len)) {
        if (g_cfg.strategy != KITE_STRATEGY_NONE)
            kite_stat_add(&g_stats.desynced, 1);
        rc = send_desync_http(*server, buf, len);
    } else {
        rc = kite_send_all(*server, buf, len);
    }
    kite_stat_add(&g_stats.bytes_up, len);
    free(buf);
    return rc;
}

static void relay(int a, int b)
{
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf)
        return;
    int open_a = 1, open_b = 1;
    while (open_a || open_b) {
        struct pollfd p[2] = {
            { .fd = open_a ? a : -1, .events = POLLIN },
            { .fd = open_b ? b : -1, .events = POLLIN },
        };
        int r = poll(p, 2, IDLE_TIMEOUT_MS);
        if (r <= 0 || !running)
            break;
        for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int from = i == 0 ? a : b, to = i == 0 ? b : a;
            ssize_t n = k_recv(from, buf, BUF_SIZE);
            if (n <= 0) {
                shutdown(to, SHUT_WR);
                if (i == 0) open_a = 0; else open_b = 0;
                if (n < 0) {
                    open_a = open_b = 0;
                }
                continue;
            }
            if (kite_send_all(to, buf, (size_t)n) < 0) {
                open_a = open_b = 0;
                break;
            }
            kite_stat_add(i == 0 ? &g_stats.bytes_up : &g_stats.bytes_down, (uint64_t)n);
        }
    }
    free(buf);
}

/* ---- client protocols -------------------------------------------------- */

static int recv_until(int fd, uint8_t *buf, size_t cap, size_t *len, const char *marker)
{
    while (*len < cap - 1) {
        ssize_t n = k_recv(fd, buf + *len, cap - 1 - *len);
        if (n <= 0)
            return -1;
        *len += (size_t)n;
        buf[*len] = 0;
        if (strstr((char *)buf, marker))
            return 0;
    }
    return -1;
}

static int recv_exact(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = k_recv(fd, buf + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int handle_socks5(int client, uint8_t *buf, size_t len)
{
    /* greeting: VER NMETHODS METHODS... */
    if (len < 2) {
        if (recv_exact(client, buf + len, 2 - len) < 0)
            return -1;
        len = 2;
    }
    size_t need = 2 + buf[1];
    if (len < need && recv_exact(client, buf + len, need - len) < 0)
        return -1;
    const uint8_t ok[2] = { 5, 0 };
    if (kite_send_all(client, ok, 2) < 0)
        return -1;

    uint8_t req[4];
    if (recv_exact(client, req, 4) < 0 || req[0] != 5 || req[1] != 1)
        return -1;
    char host[256];
    if (req[3] == 1) {
        uint8_t a[4];
        if (recv_exact(client, a, 4) < 0)
            return -1;
        inet_ntop(AF_INET, a, host, sizeof(host));
    } else if (req[3] == 3) {
        uint8_t l;
        if (recv_exact(client, &l, 1) < 0 || recv_exact(client, (uint8_t *)host, l) < 0)
            return -1;
        host[l] = 0;
    } else {
        const uint8_t unsup[10] = { 5, 8, 0, 1 };
        kite_send_all(client, unsup, 10);
        return -1;
    }
    uint8_t pb[2];
    if (recv_exact(client, pb, 2) < 0)
        return -1;
    uint16_t port = (uint16_t)(pb[0] << 8 | pb[1]);

    int server = connect_host(host, port);
    uint8_t rep[10] = { 5, server >= 0 ? 0 : 4, 0, 1 };
    kite_send_all(client, rep, 10);
    if (server < 0)
        return -1;
    kite_log("socks %s:%u", host, port);
    if (forward_first(client, &server, host, port, NULL, 0) == 0)
        relay(client, server);
    sock_close(server);
    return 0;
}

static int parse_hostport(const char *s, char *host, size_t hcap, uint16_t *port, uint16_t defport)
{
    const char *end = s + strcspn(s, " /\r\n");
    const char *colon = NULL;
    if (*s == '[')
        return -1; /* IPv6 literal: not supported yet */
    for (const char *p = s; p < end; p++)
        if (*p == ':')
            colon = p;
    size_t hl = (size_t)((colon ? colon : end) - s);
    if (hl == 0 || hl >= hcap)
        return -1;
    memcpy(host, s, hl);
    host[hl] = 0;
    *port = colon ? (uint16_t)atoi(colon + 1) : defport;
    return *port ? 0 : -1;
}

static int handle_http(int client, uint8_t *buf, size_t len, size_t cap)
{
    if (!strstr((char *)buf, "\r\n\r\n") && recv_until(client, buf, cap, &len, "\r\n\r\n") < 0)
        return -1;
    char *hdr_end = strstr((char *)buf, "\r\n\r\n") + 4;
    char host[256];
    uint16_t port;

    if (strncmp((char *)buf, "CONNECT ", 8) == 0) {
        if (parse_hostport((char *)buf + 8, host, sizeof(host), &port, 443) < 0)
            return -1;
        int server = connect_host(host, port);
        if (server < 0) {
            const char *bad = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            kite_send_all(client, (const uint8_t *)bad, strlen(bad));
            return -1;
        }
        const char *okmsg = "HTTP/1.1 200 Connection Established\r\n\r\n";
        kite_send_all(client, (const uint8_t *)okmsg, strlen(okmsg));
        kite_log("connect %s:%u", host, port);
        size_t extra = len - (size_t)((uint8_t *)hdr_end - buf);
        if (forward_first(client, &server, host, port, (uint8_t *)hdr_end, extra) == 0)
            relay(client, server);
        sock_close(server);
        return 0;
    }

    /* plain HTTP proxy request: METHOD http://host[:port]/path HTTP/1.1 */
    char *sp = strchr((char *)buf, ' ');
    if (!sp || strncasecmp(sp + 1, "http://", 7) != 0)
        return -1;
    char *hp = sp + 8;
    if (parse_hostport(hp, host, sizeof(host), &port, 80) < 0)
        return -1;
    char *path = hp + strcspn(hp, "/ ");
    if (*path != '/')
        path = "/";

    int server = connect_host(host, port);
    if (server < 0)
        return -1;
    kite_log("http %s:%u", host, port);

    /* rewrite to origin-form: METHOD /path ... */
    size_t mlen = (size_t)(sp - (char *)buf) + 1;
    size_t rest = len - (size_t)(path - (char *)buf);
    uint8_t *req = malloc(mlen + rest);
    if (!req) {
        sock_close(server);
        return -1;
    }
    memcpy(req, buf, mlen);
    memcpy(req + mlen, path, rest);
    if (forward_first(client, &server, host, port, req, mlen + rest) == 0)
        relay(client, server);
    free(req);
    sock_close(server);
    return 0;
}


/* Transparent TLS: the client was sent here by /etc/hosts and talks TLS
 * directly, so the destination comes from the ClientHello's SNI. */
static int handle_sni(int client, uint8_t *buf, size_t len, size_t cap)
{
    if (!kite_is_tls_hello(buf, len))
        return -1;
    size_t want = kite_tls_record_len(buf, len);
    while (want <= cap && len < want) {
        ssize_t n = k_recv(client, buf + len, cap - len);
        if (n <= 0)
            return -1;
        len += (size_t)n;
    }
    size_t off, hl;
    if (kite_tls_find_sni(buf, len, &off, &hl) < 0 || hl == 0 || hl > 253)
        return -1;
    char host[256];
    memcpy(host, buf + off, hl);
    host[hl] = 0;

    int server = connect_host(host, 443);
    if (server < 0)
        return -1;
    kite_log("sni %s", host);
    if (forward_first(client, &server, host, 443, buf, len) == 0)
        relay(client, server);
    sock_close(server);
    return 0;
}

enum { LISTEN_PROXY = 0, LISTEN_SNI = 1 };

static void *client_thread(void *arg)
{
    intptr_t v = (intptr_t)arg;
    int client = (int)(v >> 2), kind = (int)(v & 3);
    kite_stat_add(&g_stats.connections, 1);
    kite_stat_add(&g_stats.active, 1);
    kite_set_io_timeout(client, 15000);

    size_t cap = HELLO_MAX;
    uint8_t *buf = malloc(cap);
    ssize_t n = buf ? k_recv(client, buf, cap - 1) : -1;
    int rc = -1;
    if (n > 0) {
        buf[n] = 0;
        /* handshakes may block; the relay itself uses poll with its own timeout */
        if (kind == LISTEN_SNI)
            rc = handle_sni(client, buf, (size_t)n, cap);
        else if (buf[0] == 0x05)
            rc = handle_socks5(client, buf, (size_t)n);
        else
            rc = handle_http(client, buf, (size_t)n, cap);
    }
    if (rc < 0)
        kite_stat_add(&g_stats.errors, 1);
    free(buf);
    sock_close(client);
    __atomic_fetch_sub(&g_stats.active, 1, __ATOMIC_RELAXED);
    return NULL;
}

#define MAX_LISTENERS 3
static int listeners[MAX_LISTENERS];
static int listener_kind[MAX_LISTENERS];
static int nlisteners;
static int sni_ok;

static int is_loopback(const struct sockaddr_storage *ss)
{
    if (ss->ss_family == AF_INET)
        return (ntohl(((const struct sockaddr_in *)ss)->sin_addr.s_addr) >> 24) == 127;
    if (ss->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)ss)->sin6_addr;
        return IN6_IS_ADDR_LOOPBACK(a) ||
               (IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127);
    }
    return 0;
}

static void *accept_loop(void *arg)
{
    (void)arg;
    while (running) {
        struct pollfd p[MAX_LISTENERS];
        for (int i = 0; i < nlisteners; i++)
            p[i] = (struct pollfd){ .fd = listeners[i], .events = POLLIN };
        /* short timeout so kite_stop() is noticed without a wake-up pipe */
        int r = poll(p, (unsigned)nlisteners, 500);
        if (r <= 0)
            continue;
        for (int i = 0; i < nlisteners; i++) {
            if (!(p[i].revents & POLLIN))
                continue;
            struct sockaddr_storage peer;
            socklen_t plen = sizeof(peer);
            int c = (int)accept(listeners[i], (struct sockaddr *)&peer, &plen);
            if (c < 0)
                continue;
            if (!is_loopback(&peer)) {
                sock_close(c);
                continue;
            }
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
#endif
            pthread_t t;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&attr, 256 * 1024);
            intptr_t v = (intptr_t)c << 2 | listener_kind[i];
            if (pthread_create(&t, &attr, client_thread, (void *)v) != 0)
                sock_close(c);
            pthread_attr_destroy(&attr);
        }
    }
    return NULL;
}

static int open_listener(int family, uint16_t port, int any)
{
    int fd = (int)socket(family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
#ifdef _WIN32
    /* SO_REUSEADDR on Windows allows port hijacking; use exclusive bind */
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#endif
    int rc;
    if (family == AF_INET6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&one, sizeof(one));
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(port);
        sa.sin6_addr = any ? in6addr_any : in6addr_loopback;
        rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    } else {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
        rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    }
    if (rc < 0 || listen(fd, 256) < 0) {
        sock_close(fd);
        return -1;
    }
    return fd;
}

static void close_listeners(void)
{
    for (int i = 0; i < nlisteners; i++)
        sock_close(listeners[i]);
    nlisteners = 0;
}

int kite_start(const kite_config *cfg)
{
    if (running)
        return -1;
    if (cfg)
        g_cfg = *cfg;
    else
        kite_config_default(&g_cfg);
    g_verbose = g_cfg.verbose;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    kite_dns_init(g_cfg.dns_servers);

    nlisteners = 0;
    sni_ok = 0;
    int fd = open_listener(AF_INET, g_cfg.listen_port, 0);
    if (fd < 0)
        return -1;
    listeners[nlisteners] = fd;
    listener_kind[nlisteners++] = LISTEN_PROXY;

    if (g_cfg.sni_port) {
        /* Optional: a busy port 443 must not stop the proxy itself.
         * macOS lets normal users bind ports below 1024 only on the wildcard
         * address, so bind that there and drop non-loopback peers on accept.
         * Windows allows loopback directly (and a wildcard bind would trigger
         * a firewall prompt). */
#ifdef __APPLE__
        int any = 1;
#else
        int any = 0;
#endif
        int families[2] = { AF_INET, AF_INET6 };
        for (int i = 0; i < 2; i++) {
            fd = open_listener(families[i], g_cfg.sni_port, any);
            if (fd < 0) {
                kite_log("sni listener on port %u failed", g_cfg.sni_port);
                continue;
            }
            listeners[nlisteners] = fd;
            listener_kind[nlisteners++] = LISTEN_SNI;
            if (families[i] == AF_INET)
                sni_ok = 1;
        }
    }

    running = 1;
    if (pthread_create(&accept_thread, NULL, accept_loop, NULL) != 0) {
        running = 0;
        close_listeners();
        return -1;
    }
    kite_log("listening on 127.0.0.1:%u, sni %s", g_cfg.listen_port, sni_ok ? "on" : "off");
    return 0;
}

void kite_stop(void)
{
    if (!running)
        return;
    running = 0;
    pthread_join(accept_thread, NULL);
    close_listeners();
}

int kite_running(void) { return running; }
int kite_sni_listening(void) { return running && sni_ok; }
