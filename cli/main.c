#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kite.h"

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static void usage(void)
{
    fprintf(stderr,
            "kite %s\n"
            "usage: kite [-p port] [-t sni_port] [-s tlsrec|bytes|split|none] [-n] [-d dns,list] [-v]\n"
            "  -p  listen port (default 8899)\n"
            "  -t  transparent TLS port for hosts-file redirects (e.g. 443)\n"
            "  -s  strategy (default tlsrec)\n"
            "  -n  tlsrec without the extra TCP split\n"
            "  -d  TCP DNS servers (default 1.1.1.1,1.0.0.1,8.8.8.8,9.9.9.9)\n"
            "  -v  verbose\n",
            kite_version());
}

int main(int argc, char **argv)
{
    kite_config cfg;
    kite_config_default(&cfg);
    int opt;
    while ((opt = getopt(argc, argv, "p:t:s:nd:vh")) != -1) {
        switch (opt) {
        case 'p': cfg.listen_port = (uint16_t)atoi(optarg); break;
        case 't': cfg.sni_port = (uint16_t)atoi(optarg); break;
        case 's':
            if (!strcmp(optarg, "tlsrec")) cfg.strategy = KITE_STRATEGY_TLSREC;
            else if (!strcmp(optarg, "bytes")) cfg.strategy = KITE_STRATEGY_BYTES;
            else if (!strcmp(optarg, "split")) cfg.strategy = KITE_STRATEGY_SPLIT;
            else if (!strcmp(optarg, "none")) cfg.strategy = KITE_STRATEGY_NONE;
            else { usage(); return 1; }
            break;
        case 'n': cfg.tcp_split = 0; break;
        case 'd': cfg.dns_servers = optarg; break;
        case 'v': cfg.verbose = 1; break;
        default: usage(); return opt == 'h' ? 0 : 1;
        }
    }

    if (kite_start(&cfg) != 0) {
        fprintf(stderr, "kite: cannot listen on 127.0.0.1:%u\n", cfg.listen_port);
        return 1;
    }
    fprintf(stderr, "kite %s listening on 127.0.0.1:%u (HTTP + SOCKS5)\n", kite_version(), cfg.listen_port);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop)
        sleep(1);
    kite_stop();

    kite_stats s;
    kite_get_stats(&s);
    fprintf(stderr, "connections=%llu tls=%llu desynced=%llu up=%llu down=%llu errors=%llu\n",
            (unsigned long long)s.connections, (unsigned long long)s.tls_seen,
            (unsigned long long)s.desynced, (unsigned long long)s.bytes_up,
            (unsigned long long)s.bytes_down, (unsigned long long)s.errors);
    return 0;
}
