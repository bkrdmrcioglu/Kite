/*
 * kite-core: DPI bypass engine.
 *
 * A local HTTP CONNECT / SOCKS5 proxy that resolves names over TCP DNS
 * (bypassing poisoned ISP resolvers) and reshapes the first TLS ClientHello
 * of each connection so SNI-based DPI cannot read the hostname.
 */
#ifndef KITE_H
#define KITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KITE_STRATEGY_NONE = 0,     /* pass through unchanged */
    KITE_STRATEGY_TLSREC = 1,   /* split ClientHello into two TLS records at the SNI */
    KITE_STRATEGY_BYTES = 2,    /* send the head of the hello one byte per segment */
    KITE_STRATEGY_SPLIT = 3,    /* plain TCP split at the SNI */
} kite_strategy;

typedef struct {
    uint16_t listen_port;       /* default 8899 */
    int port_fallback;          /* if listen_port is busy, use the next free port */
    uint16_t sni_port;          /* transparent TLS listener for hosts-file redirects, 0 = off */
    kite_strategy strategy;     /* default KITE_STRATEGY_TLSREC */
    int tcp_split;              /* also send tlsrec records as separate segments */
    const char *dns_servers;    /* comma separated IPv4 list for TCP DNS, NULL = default */
    int verbose;
} kite_config;

typedef struct {
    uint64_t connections;       /* accepted client connections */
    uint64_t active;            /* currently open */
    uint64_t tls_seen;          /* connections whose first payload was a ClientHello */
    uint64_t desynced;          /* connections where a strategy was applied */
    uint64_t bytes_up;
    uint64_t bytes_down;
    uint64_t dns_queries;
    uint64_t errors;
} kite_stats;

/* Optional resolver override, e.g. DoH implemented by the host app.
 * Write up to max IPv4 addresses (network byte order) to out and return the
 * count, or a negative value to fall back to the built-in TCP DNS resolver. */
typedef int (*kite_resolver_fn)(const char *host, uint32_t *out, int max, void *ctx);

void kite_config_default(kite_config *cfg);

/* Starts the proxy on a background thread. Returns 0 on success. */
int kite_start(const kite_config *cfg);
void kite_stop(void);
int kite_running(void);
/* Port the proxy actually listens on (differs from listen_port after a fallback). */
uint16_t kite_listen_port(void);
/* 1 when the transparent TLS listener (sni_port) is accepting. */
int kite_sni_listening(void);

void kite_set_resolver(kite_resolver_fn fn, void *ctx);
void kite_get_stats(kite_stats *out);
const char *kite_version(void);

#ifdef __cplusplus
}
#endif
#endif
