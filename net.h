#ifndef PROJECT_NET_H
#define PROJECT_NET_H

#include <stddef.h>

#define URL_LIMIT 4096
#define HEADER_LIMIT 65536
#define BODY_LIMIT (50ULL * 1024 * 1024)

struct http_url {
    char scheme[6];
    char host[256];
    char authority[320];
    char port[6];
    char path[URL_LIMIT];
};

int parse_http_url(const char *text, struct http_url *url, char *error, size_t size);
int connect_tcp(const char *host, const char *port);
int set_socket_timeout(int fd);
int send_all(int fd, const void *buffer, size_t length);
int valid_port(const char *text);

#endif
