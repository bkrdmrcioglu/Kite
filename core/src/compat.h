/* Socket portability between POSIX (macOS, Linux) and Windows (Winsock). */
#ifndef KITE_COMPAT_H
#define KITE_COMPAT_H

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define _CRT_RAND_S /* rand_s */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define poll WSAPoll
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#define SOCK_ERRNO WSAGetLastError()
#define SOCK_EINPROGRESS WSAEWOULDBLOCK
#define SOCK_EINTR WSAEINTR

static inline int sock_close(int fd) { return closesocket((SOCKET)fd); }
static inline void sock_nonblock(int fd, int on)
{
    u_long m = (u_long)on;
    ioctlsocket((SOCKET)fd, FIONBIO, &m);
}
static inline uint32_t kite_random32(void)
{
    unsigned int v = 0;
    rand_s(&v);
    return v;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SOCK_ERRNO errno
#define SOCK_EINPROGRESS EINPROGRESS
#define SOCK_EINTR EINTR

static inline int sock_close(int fd) { return close(fd); }
static inline void sock_nonblock(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
static inline uint32_t kite_random32(void) { return arc4random(); }
#endif

/* Winsock wants char pointers and int lengths; POSIX accepts these too. */
#define k_recv(fd, buf, n) recv((fd), (char *)(buf), (int)(n), 0)
#define k_send(fd, buf, n) send((fd), (const char *)(buf), (int)(n), 0)

#endif
