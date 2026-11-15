#define _POSIX_C_SOURCE 200809L
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_WORKERS 32
#define REQUEST_LIMIT 16384
static sem_t slots;
struct connection {
    int socket;
    unsigned long id;
};

static void reply_error(int client, int status, const char *reason, const char *message) {
    char body[512], header[512];
    int n = snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1><p>%s</p></body></html>\n",
                     status, reason, message);
    int h = snprintf(header, sizeof(header),
                     "HTTP/1.0 %d %s\r\nContent-Type: text/html\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", status, reason, n);
    if (!send_all(client, header, (size_t)h)) send_all(client, body, (size_t)n);
}

/* Each worker owns its client socket and upstream socket. No shared buffers. */
static void *serve_client(void *context) {
    struct connection connection = *(struct connection *)context;
    int client = connection.socket;
    free(context);
    int upstream = -1;
    size_t relayed = 0;
    const char *outcome = "client closed or request timed out";
    fprintf(stderr, "[request %lu] worker started\n", connection.id);
    char request[REQUEST_LIMIT + 1];
    size_t used = 0;
    while (used < REQUEST_LIMIT) {
        ssize_t n = recv(client, request + used, REQUEST_LIMIT - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto cleanup;
        if (memchr(request + used, '\0', (size_t)n)) {
            reply_error(client, 400, "Bad Request", "NUL bytes are not allowed in headers.");
            goto cleanup;
        }
        used += (size_t)n;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }
    if (!strstr(request, "\r\n\r\n")) {
        reply_error(client, 431, "Request Header Fields Too Large", "Request exceeds 16 KiB.");
        goto cleanup;
    }
    char *line_end = strstr(request, "\r\n");
    outcome = "request rejected";
    *line_end = '\0';
    char *url_text = strchr(request, ' ');
    if (!url_text) goto malformed;
    *url_text++ = '\0';
    char *version = strchr(url_text, ' ');
    if (!version) goto malformed;
    *version++ = '\0';
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) goto malformed;
    if (strcmp(request, "GET")) {
        reply_error(client, 405, "Method Not Allowed", "Only GET is supported.");
        goto cleanup;
    }
    struct http_url url;
    char error[160];
    if (parse_http_url(url_text, &url, error, sizeof(error))) {
        reply_error(client, 400, "Bad Request", error);
        goto cleanup;
    }
    upstream = connect_tcp(url.host, url.port);
    if (upstream < 0) {
        outcome = "destination connection failed";
        fprintf(stderr, "[request %lu] could not connect to %s:%s\n", connection.id, url.host, url.port);
        reply_error(client, 502, "Bad Gateway", "Could not connect to the destination.");
        goto cleanup;
    }
    char outbound[URL_LIMIT + 512];
    int length = snprintf(outbound, sizeof(outbound),
                          "GET %s HTTP/1.0\r\nHost: %s\r\n"
                          "User-Agent: OSProjectProxy/1.0\r\nAccept: text/html,application/xhtml+xml\r\n"
                          "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", url.path, url.authority);
    if (length < 0 || (size_t)length >= sizeof(outbound)
        || send_all(upstream, outbound, (size_t)length)) {
        outcome = "destination request send failed";
        reply_error(client, 502, "Bad Gateway", "Could not send the destination request.");
        goto cleanup;
    }
    fprintf(stderr, "[request %lu] fetching %s\n", connection.id, url_text);
    outcome = "forwarding failed";
    char buffer[8192];
    int started = 0;
    for (;;) {
        ssize_t n = recv(upstream, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!started) reply_error(client, 502, "Bad Gateway", "Destination returned no response.");
            outcome = n < 0 ? "upstream receive failed or timed out" : "upstream closed";
            break;
        }
        started = 1;
        if (send_all(client, buffer, (size_t)n)) { outcome = "client disconnected or send timed out"; break; }
        relayed += (size_t)n;
    }
    goto cleanup;
malformed:
    reply_error(client, 400, "Bad Request", "Expected GET followed by an absolute HTTP URL.");
cleanup:
    if (upstream >= 0) close(upstream);
    close(client);
    fprintf(stderr, "[request %lu] worker finished: %s; %zu response bytes relayed\n",
            connection.id, outcome, relayed);
    sem_post(&slots);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2 || !valid_port(argv[1])) {
        fprintf(stderr, "Usage: %s PORT\n", argv[0]); return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    if (sem_init(&slots, 0, MAX_WORKERS)) { perror("sem_init"); return EXIT_FAILURE; }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return EXIT_FAILURE; }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    /* Local-only by default: the demo does not need an internet-facing proxy. */
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)strtoul(argv[1], NULL, 10));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, MAX_WORKERS)) {
        perror("listen/bind"); close(listener); return EXIT_FAILURE;
    }
    fprintf(stderr, "HTTP proxy listening on 127.0.0.1:%s (%d workers maximum)\n", argv[1], MAX_WORKERS);
    unsigned long next_id = 1;
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        if (set_socket_timeout(client)) { close(client); continue; }
        unsigned long id = next_id++;
        if (sem_trywait(&slots)) {
            reply_error(client, 503, "Service Unavailable", "All workers are busy. Try again later.");
            fprintf(stderr, "[request %lu] rejected: all %d workers are busy\n", id, MAX_WORKERS);
            close(client); continue;
        }
        struct connection *context = malloc(sizeof(*context));
        if (!context) { close(client); sem_post(&slots); continue; }
        *context = (struct connection){.socket = client, .id = id};
        pthread_attr_t attributes;
        int status = pthread_attr_init(&attributes);
        if (!status) {
            status = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
            pthread_t thread;
            if (!status) status = pthread_create(&thread, &attributes, serve_client, context);
            pthread_attr_destroy(&attributes);
        }
        if (status) {
            fprintf(stderr, "Worker: %s\n", strerror(status));
            free(context); close(client); sem_post(&slots);
        }
    }
    close(listener);
    return EXIT_FAILURE;
}
