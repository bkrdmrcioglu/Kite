#ifndef KITE_INTERNAL_H
#define KITE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "kite.h"

/* tls.c */
int kite_is_tls_hello(const uint8_t *buf, size_t len);
/* Total length (header included) of the TLS record starting at buf, or 0. */
size_t kite_tls_record_len(const uint8_t *buf, size_t len);
/* Finds the SNI hostname inside a ClientHello record. Returns 0 and sets the
 * offset (from buf start) and length of the hostname, or -1. */
int kite_tls_find_sni(const uint8_t *buf, size_t len, size_t *off, size_t *hlen);
/* Splits the first TLS record of buf into two records at payload offset p.
 * out must have room for len + 5 bytes. Returns the new length. */
size_t kite_tls_split_record(const uint8_t *buf, size_t len, size_t p, uint8_t *out);

/* http.c */
int kite_is_http_request(const uint8_t *buf, size_t len);
/* Offset of the Host header value in a plain HTTP request, or -1. */
long kite_http_host_offset(const uint8_t *buf, size_t len);

/* dns.c */
void kite_dns_init(const char *servers);
void kite_dns_set_resolver(kite_resolver_fn fn, void *ctx);
/* Resolves host to IPv4 addresses (network order). Returns count or -1. */
int kite_dns_resolve(const char *host, uint32_t *out, int max);

/* stats */
void kite_stat_add(uint64_t *field, uint64_t n);
extern kite_stats g_stats;

void kite_log(const char *fmt, ...);
extern int g_verbose;

#endif
