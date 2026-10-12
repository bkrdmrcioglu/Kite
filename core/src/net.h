#ifndef KITE_NET_H
#define KITE_NET_H

#include <stdint.h>
#include <sys/types.h>

/* addr and port in network byte order. Returns fd or -1. */
int kite_connect_timeout(uint32_t addr, uint16_t port, int ms);
void kite_set_io_timeout(int fd, int ms);
void kite_set_nodelay(int fd, int on);
int kite_send_all(int fd, const uint8_t *buf, size_t len);

#endif
