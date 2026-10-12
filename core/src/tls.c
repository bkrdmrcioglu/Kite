#include <string.h>
#include "internal.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int kite_is_tls_hello(const uint8_t *buf, size_t len)
{
    /* record type handshake, major version 3, handshake type client_hello */
    return len >= 6 && buf[0] == 0x16 && buf[1] == 0x03 && buf[5] == 0x01;
}

size_t kite_tls_record_len(const uint8_t *buf, size_t len)
{
    if (len < 5 || buf[0] != 0x16 || buf[1] != 0x03)
        return 0;
    return 5 + (size_t)rd16(buf + 3);
}

int kite_tls_find_sni(const uint8_t *buf, size_t len, size_t *off, size_t *hlen)
{
    if (!kite_is_tls_hello(buf, len))
        return -1;
    size_t end = kite_tls_record_len(buf, len);
    if (end > len)
        end = len;

    /* record(5) + handshake type(1) + length(3) + version(2) + random(32) */
    size_t p = 5 + 4 + 2 + 32;
    if (p + 1 > end) return -1;
    p += 1 + buf[p];                         /* session id */
    if (p + 2 > end) return -1;
    p += 2 + rd16(buf + p);                  /* cipher suites */
    if (p + 1 > end) return -1;
    p += 1 + buf[p];                         /* compression methods */
    if (p + 2 > end) return -1;
    size_t ext_end = p + 2 + rd16(buf + p);
    p += 2;
    if (ext_end > end)
        ext_end = end;

    while (p + 4 <= ext_end) {
        uint16_t type = rd16(buf + p);
        uint16_t elen = rd16(buf + p + 2);
        p += 4;
        if (type == 0x0000) {
            /* server_name_list length(2), name type(1), name length(2) */
            if (p + 5 > ext_end) return -1;
            size_t nlen = rd16(buf + p + 3);
            if (buf[p + 2] != 0 || p + 5 + nlen > ext_end) return -1;
            *off = p + 5;
            *hlen = nlen;
            return 0;
        }
        p += elen;
    }
    return -1;
}

size_t kite_tls_split_record(const uint8_t *buf, size_t len, size_t p, uint8_t *out)
{
    size_t rec = kite_tls_record_len(buf, len);
    if (rec == 0 || rec > len)
        return 0;
    size_t body = rec - 5;
    if (p == 0 || p >= body)
        return 0;

    size_t a = p, b = body - p;
    uint8_t *o = out;
    o[0] = buf[0]; o[1] = buf[1]; o[2] = buf[2];
    o[3] = (uint8_t)(a >> 8); o[4] = (uint8_t)a;
    memcpy(o + 5, buf + 5, a);
    o += 5 + a;
    o[0] = buf[0]; o[1] = buf[1]; o[2] = buf[2];
    o[3] = (uint8_t)(b >> 8); o[4] = (uint8_t)b;
    memcpy(o + 5, buf + 5 + a, b);
    o += 5 + b;
    /* anything after the first record is passed through */
    memcpy(o, buf + rec, len - rec);
    return len + 5;
}
