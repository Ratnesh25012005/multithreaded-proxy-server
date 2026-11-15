#define _POSIX_C_SOURCE 200809L
#include "net.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int valid_port(const char *text) {
    if (!text || !*text) return 0;
    for (const char *p = text; *p; ++p)
        if (!isdigit((unsigned char)*p)) return 0;
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return !errno && !*end && value >= 1 && value <= 65535;
}

int parse_http_url(const char *text, struct http_url *url, char *error, size_t size) {
    const char *why = "Invalid HTTP URL";
    memset(url, 0, sizeof(*url));
    if (strlen(text) >= URL_LIMIT) { why = "URL is too long"; goto fail; }
    for (const char *p = text; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127) goto fail;
    size_t scheme_length;
    if (!strncasecmp(text, "http://", 7)) {
        strcpy(url->scheme, "http"); scheme_length = 7;
    } else if (!strncasecmp(text, "https://", 8)) {
        strcpy(url->scheme, "https"); scheme_length = 8;
    } else {
        why = "Only http:// and https:// webpage URLs are supported";
        goto fail;
    }
    const char *start = text + scheme_length;
    size_t length = strcspn(start, "/?#");
    if (!length || length >= sizeof(url->authority)) goto fail;
    memcpy(url->authority, start, length);
    char hostport[320];
    memcpy(hostport, start, length);
    hostport[length] = '\0';
    if (strchr(hostport, '@') || strchr(hostport, '\\')) goto fail;
    char *host = hostport, *port = NULL;
    if (*host == '[') {
        char *bracket = strchr(++host, ']');
        if (!bracket) goto fail;
        *bracket = '\0';
        if (bracket[1]) {
            if (bracket[1] != ':') goto fail;
            port = bracket + 2;
        }
    } else {
        port = strchr(host, ':');
        if (port) *port++ = '\0';
    }
    if (!*host || strlen(host) >= sizeof(url->host)) goto fail;
    for (const char *p = host; *p; ++p)
        if (!isalnum((unsigned char)*p) && !strchr(".-:_", *p)) goto fail;
    if (port && (!valid_port(port) || strlen(port) >= sizeof(url->port))) goto fail;
    strcpy(url->host, host);
    strcpy(url->port, port ? port : (!strcmp(url->scheme, "https") ? "443" : "80"));
    const char *path = start + length;
    size_t path_length = strcspn(path, "#");
    size_t prefix = (*path != '/') ? 1 : 0;
    if (path_length + prefix >= sizeof(url->path)) goto fail;
    if (prefix) url->path[0] = '/';
    memcpy(url->path + prefix, path, path_length);
    return 0;
fail:
    snprintf(error, size, "%s", why);
    return -1;
}

int set_socket_timeout(int fd) {
    struct timeval timeout = {15, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/* Resolve the host, then give each connection attempt a finite timeout. */
int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int result = getaddrinfo(host, port, &hints, &addresses);
    if (result) { fprintf(stderr, "DNS: %s\n", gai_strerror(result)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) goto next;
        int status = connect(fd, a->ai_addr, a->ai_addrlen);
        if (status && errno != EINPROGRESS) goto next;
        if (status) {
            struct pollfd pending = {fd, POLLOUT, 0};
            do { status = poll(&pending, 1, 10000); } while (status < 0 && errno == EINTR);
            int socket_error = 0;
            socklen_t size = sizeof(socket_error);
            if (status <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size)
                || socket_error) goto next;
        }
        if (fcntl(fd, F_SETFL, flags) || set_socket_timeout(fd)) goto next;
        break;
next:
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

int send_all(int fd, const void *buffer, size_t length) {
    const unsigned char *bytes = buffer;
    while (length) {
        ssize_t sent = send(fd, bytes, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}
