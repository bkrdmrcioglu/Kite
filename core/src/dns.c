/*
 * DNS resolver over TCP.
 *
 * Many ISPs (e.g. Turk Telekom) poison UDP DNS and intercept port 53/udp to
 * public resolvers, but leave DNS over TCP to 1.1.1.1 alone. Results are
 * cached and known block-page addresses are dropped.
 */
#include "compat.h" /* first: winsock2.h must precede windows.h */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "internal.h"
#include "net.h"

#define MAX_SERVERS 8
#define CACHE_SIZE 512
#define MAX_ADDRS 8

static uint32_t servers[MAX_SERVERS];
static int nservers;

static kite_resolver_fn custom_fn;
static void *custom_ctx;

/* ISP block-page answers that mean "censored", never a real server. */
static const char *blockpage_ips[] = {
    "195.175.254.2",    /* Turk Telekom */
    "193.192.98.41",    /* Turkcell Superonline */
    "195.175.255.10",
    NULL
};

struct cache_entry {
    char host[256];
    uint32_t addrs[MAX_ADDRS];
    int count;
    time_t expires;
};

static struct cache_entry cache[CACHE_SIZE];
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

void kite_dns_init(const char *list)
{
    nservers = 0;
    char buf[256];
    strncpy(buf, list && *list ? list : "1.1.1.1,1.0.0.1,8.8.8.8,9.9.9.9", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *tok = strtok(buf, ", "); tok && nservers < MAX_SERVERS; tok = strtok(NULL, ", ")) {
        struct in_addr a;
        if (inet_pton(AF_INET, tok, &a) == 1)
            servers[nservers++] = a.s_addr;
    }
}

void kite_dns_set_resolver(kite_resolver_fn fn, void *ctx)
{
    custom_fn = fn;
    custom_ctx = ctx;
}

static unsigned hash_host(const char *h)
{
    unsigned x = 5381;
    while (*h)
        x = x * 33 + (unsigned char)*h++;
    return x % CACHE_SIZE;
}

static int is_blockpage(uint32_t a)
{
    for (int i = 0; blockpage_ips[i]; i++) {
        struct in_addr b;
        inet_pton(AF_INET, blockpage_ips[i], &b);
        if (b.s_addr == a)
            return 1;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *buf, size_t n)
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

static size_t skip_name(const uint8_t *r, size_t len, size_t p)
{
    while (p < len) {
        if ((r[p] & 0xC0) == 0xC0)
            return p + 2;
        if (r[p] == 0)
            return p + 1;
        p += (size_t)r[p] + 1;
    }
    return len;
}

static int query_server(uint32_t server, const char *host, uint32_t *out, int max, uint32_t *ttl_out)
{
    uint8_t q[300];
    uint16_t id = (uint16_t)kite_random32();
    size_t n = 0;
    q[n++] = 0; q[n++] = 0;                 /* TCP length prefix, filled below */
    q[n++] = id >> 8; q[n++] = id & 0xff;
    q[n++] = 0x01; q[n++] = 0x00;           /* recursion desired */
    q[n++] = 0; q[n++] = 1;                 /* qdcount */
    memset(q + n, 0, 6); n += 6;

    const char *s = host;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63 || n + l + 6 > sizeof(q))
            return -1;
        q[n++] = (uint8_t)l;
        memcpy(q + n, s, l);
        n += l;
        s += l;
        if (*s == '.')
            s++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                 /* type A */
    q[n++] = 0; q[n++] = 1;                 /* class IN */
    q[0] = (uint8_t)((n - 2) >> 8);
    q[1] = (uint8_t)(n - 2);

    int fd = kite_connect_timeout(server, htons(53), 3000);
    if (fd < 0)
        return -1;
    kite_set_io_timeout(fd, 4000);

    int count = -1;
    uint8_t lenbuf[2];
    uint8_t *r = NULL;
    if (k_send(fd, q, n) != (ssize_t)n || recv_all(fd, lenbuf, 2) < 0)
        goto done;
    size_t rlen = (size_t)(lenbuf[0] << 8 | lenbuf[1]);
    if (rlen < 12 || !(r = malloc(rlen)) || recv_all(fd, r, rlen) < 0)
        goto done;
    if ((r[0] << 8 | r[1]) != id || (r[3] & 0x0f) != 0)
        goto done;

    int qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7];
    size_t p = 12;
    for (int i = 0; i < qd; i++)
        p = skip_name(r, rlen, p) + 4;

    count = 0;
    uint32_t min_ttl = 3600;
    for (int i = 0; i < an && p + 10 <= rlen; i++) {
        p = skip_name(r, rlen, p);
        if (p + 10 > rlen)
            break;
        uint16_t type = (uint16_t)(r[p] << 8 | r[p + 1]);
        uint32_t ttl = (uint32_t)r[p + 4] << 24 | r[p + 5] << 16 | r[p + 6] << 8 | r[p + 7];
        uint16_t dlen = (uint16_t)(r[p + 8] << 8 | r[p + 9]);
        p += 10;
        if (type == 1 && dlen == 4 && p + 4 <= rlen && count < max) {
            uint32_t a;
            memcpy(&a, r + p, 4);
            if (!is_blockpage(a)) {
                out[count++] = a;
                if (ttl < min_ttl)
                    min_ttl = ttl;
            }
        }
        p += dlen;
    }
    *ttl_out = min_ttl;

done:
    free(r);
    sock_close(fd);
    return count;
}

int kite_dns_resolve(const char *host, uint32_t *out, int max)
{
    struct in_addr lit;
    if (inet_pton(AF_INET, host, &lit) == 1) {
        out[0] = lit.s_addr;
        return 1;
    }
    if (strlen(host) >= sizeof(cache[0].host))
        return -1;

    unsigned h = hash_host(host);
    time_t now = time(NULL);
    pthread_mutex_lock(&cache_lock);
    if (cache[h].count > 0 && cache[h].expires > now && strcmp(cache[h].host, host) == 0) {
        int c = cache[h].count < max ? cache[h].count : max;
        memcpy(out, cache[h].addrs, sizeof(uint32_t) * (size_t)c);
        pthread_mutex_unlock(&cache_lock);
        return c;
    }
    pthread_mutex_unlock(&cache_lock);

    kite_stat_add(&g_stats.dns_queries, 1);
    uint32_t addrs[MAX_ADDRS];
    uint32_t ttl = 300;
    int count = -1;

    if (custom_fn)
        count = custom_fn(host, addrs, MAX_ADDRS, custom_ctx);
    for (int i = 0; count <= 0 && i < nservers; i++)
        count = query_server(servers[i], host, addrs, MAX_ADDRS, &ttl);
    if (count <= 0) {
        kite_log("dns: %s failed", host);
        return -1;
    }

    if (ttl < 60) ttl = 60;
    if (ttl > 1800) ttl = 1800;
    pthread_mutex_lock(&cache_lock);
    strcpy(cache[h].host, host);
    memcpy(cache[h].addrs, addrs, sizeof(uint32_t) * (size_t)count);
    cache[h].count = count;
    cache[h].expires = now + ttl;
    pthread_mutex_unlock(&cache_lock);

    int c = count < max ? count : max;
    memcpy(out, addrs, sizeof(uint32_t) * (size_t)c);
    return c;
}
