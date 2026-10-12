#include <string.h>
#include "compat.h"
#include "net.h"

int kite_connect_timeout(uint32_t addr, uint16_t port, int ms)
{
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
#endif
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = port;
    sa.sin_addr.s_addr = addr;

    sock_nonblock(fd, 1);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && SOCK_ERRNO == SOCK_EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        rc = poll(&pfd, 1, ms) == 1 ? 0 : -1;
        if (rc == 0) {
            int err = 0;
            socklen_t l = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &l);
            rc = err ? -1 : 0;
        }
    }
    if (rc < 0) {
        sock_close(fd);
        return -1;
    }
    sock_nonblock(fd, 0);
    return fd;
}

void kite_set_io_timeout(int fd, int ms)
{
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
#else
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
}

void kite_set_nodelay(int fd, int on)
{
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
}

int kite_send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = k_send(fd, buf, len);
        if (n < 0) {
            if (SOCK_ERRNO == SOCK_EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}
