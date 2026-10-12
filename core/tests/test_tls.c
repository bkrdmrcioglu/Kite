#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"

/* Builds a minimal ClientHello record carrying an SNI for host. */
static size_t build_hello(uint8_t *b, const char *host)
{
    size_t hl = strlen(host), n = 0;
    uint8_t body[512];
    size_t m = 0;
    body[m++] = 0x03; body[m++] = 0x03;          /* legacy version */
    memset(body + m, 0xAA, 32); m += 32;         /* random */
    body[m++] = 0;                               /* session id */
    body[m++] = 0; body[m++] = 2; body[m++] = 0x13; body[m++] = 0x01;
    body[m++] = 1; body[m++] = 0;                /* compression */
    size_t ext_len_pos = m; m += 2;
    /* a dummy extension before SNI */
    body[m++] = 0x00; body[m++] = 0x17; body[m++] = 0; body[m++] = 0;
    body[m++] = 0; body[m++] = 0;                /* server_name */
    body[m++] = 0; body[m++] = (uint8_t)(hl + 5);
    body[m++] = 0; body[m++] = (uint8_t)(hl + 3);
    body[m++] = 0;
    body[m++] = 0; body[m++] = (uint8_t)hl;
    memcpy(body + m, host, hl); m += hl;
    size_t ext_len = m - ext_len_pos - 2;
    body[ext_len_pos] = (uint8_t)(ext_len >> 8); body[ext_len_pos + 1] = (uint8_t)ext_len;

    b[n++] = 0x16; b[n++] = 0x03; b[n++] = 0x01;
    b[n++] = (uint8_t)((m + 4) >> 8); b[n++] = (uint8_t)(m + 4);
    b[n++] = 0x01; b[n++] = 0; b[n++] = (uint8_t)(m >> 8); b[n++] = (uint8_t)m;
    memcpy(b + n, body, m);
    return n + m;
}

int main(void)
{
    uint8_t hello[600], out[700];
    size_t len = build_hello(hello, "discord.com");

    assert(kite_is_tls_hello(hello, len));
    assert(kite_tls_record_len(hello, len) == len);

    size_t off, hl;
    assert(kite_tls_find_sni(hello, len, &off, &hl) == 0);
    assert(hl == 11 && memcmp(hello + off, "discord.com", 11) == 0);

    /* truncated input must not read past the buffer */
    for (size_t cut = 0; cut < len; cut++)
        kite_tls_find_sni(hello, cut, &off, &hl);

    size_t cutp = off + 2 - 5;
    size_t n = kite_tls_split_record(hello, len, cutp, out);
    assert(n == len + 5);
    size_t r1 = kite_tls_record_len(out, n);
    assert(r1 == 5 + cutp);
    size_t r2 = kite_tls_record_len(out + r1, n - r1);
    assert(r1 + r2 == n);
    /* payload is unchanged once the record headers are removed */
    assert(memcmp(out + 5, hello + 5, cutp) == 0);
    assert(memcmp(out + r1 + 5, hello + 5 + cutp, len - 5 - cutp) == 0);
    /* the hostname is no longer contiguous in one record */
    assert(memmem(out, n, "discord.com", 11) == NULL);

    const char *req = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    assert(kite_is_http_request((const uint8_t *)req, strlen(req)));
    assert(kite_http_host_offset((const uint8_t *)req, strlen(req)) == 22);

    puts("test_tls: ok");
    return 0;
}
