#include <string.h>
#include <strings.h>
#include "internal.h"

int kite_is_http_request(const uint8_t *buf, size_t len)
{
    static const char *methods[] = { "GET ", "POST ", "HEAD ", "PUT ", "DELETE ",
                                     "OPTIONS ", "PATCH ", NULL };
    for (int i = 0; methods[i]; i++) {
        size_t n = strlen(methods[i]);
        if (len >= n && memcmp(buf, methods[i], n) == 0)
            return 1;
    }
    return 0;
}

long kite_http_host_offset(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 7 < len; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && strncasecmp((const char *)buf + i, "host:", 5) == 0) {
            size_t p = i + 5;
            while (p < len && buf[p] == ' ')
                p++;
            return (long)p;
        }
    }
    return -1;
}
