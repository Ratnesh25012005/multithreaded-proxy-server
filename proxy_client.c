#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

struct reader {
    int socket;
    unsigned char buffer[8192];
    size_t position, available;
};
struct response {
    int status, chunked, has_length, html, encoded;
    uint64_t length;
    char final_url[URL_LIMIT];
    char proxy_error[256];
    unsigned redirects;
};

static ssize_t read_some(struct reader *r, void *output, size_t limit) {
    if (r->position == r->available) {
        ssize_t n;
        do { n = recv(r->socket, r->buffer, sizeof(r->buffer), 0); } while (n < 0 && errno == EINTR);
        if (n <= 0) return n;
        r->position = 0;
        r->available = (size_t)n;
    }
    size_t n = r->available - r->position;
    if (n > limit) n = limit;
    memcpy(output, r->buffer + r->position, n);
    r->position += n;
    return (ssize_t)n;
}

/* CRLF may be split across any two socket reads. */
static int read_line(struct reader *r, char *line, size_t capacity) {
    size_t n = 0;
    while (n + 1 < capacity) {
        unsigned char byte;
        if (read_some(r, &byte, 1) != 1 || !byte) return -1;
        line[n++] = (char)byte;
        if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') {
            line[n - 2] = '\0';
            return (int)n;
        }
    }
    return -1;
}

static int decimal_length(const char *value, uint64_t *length) {
    if (!*value) return -1;
    uint64_t n = 0;
    for (; *value; ++value) {
        unsigned digit = (unsigned)(*value - '0');
        if (!isdigit((unsigned char)*value) || n > (UINT64_MAX - digit) / 10) return -1;
        n = n * 10 + digit;
    }
    *length = n;
    return 0;
}

static int read_headers(struct reader *r, struct response *response) {
    char line[HEADER_LIMIT + 1];
    for (int interim = 0; interim < 6; ++interim) {
        memset(response, 0, sizeof(*response));
        int n = read_line(r, line, sizeof(line));
        size_t total = n > 0 ? (size_t)n : 0;
        if (n < 0 || (strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9))) return -1;
        if (strlen(line) < 12 || !isdigit((unsigned char)line[9])
            || !isdigit((unsigned char)line[10]) || !isdigit((unsigned char)line[11])
            || (line[12] && line[12] != ' ')) return -1;
        response->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
        if (response->status < 100 || response->status > 599) return -1;
        for (;;) {
            n = read_line(r, line, sizeof(line));
            if (n < 0 || (total += (size_t)n) > HEADER_LIMIT) return -1;
            if (!*line) break;
            char *value = strchr(line, ':');
            if (!value || value == line) return -1;
            *value++ = '\0';
            for (char *p = line; *p; ++p)
                if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p)) return -1;
            while (*value == ' ' || *value == '\t') ++value;
            char *end = value + strlen(value);
            while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            if (!strcasecmp(line, "Content-Length")) {
                uint64_t length;
                if (decimal_length(value, &length)
                    || (response->has_length && response->length != length)) return -1;
                response->has_length = 1;
                response->length = length;
            } else if (!strcasecmp(line, "Transfer-Encoding")) {
                if (response->chunked || strcasecmp(value, "chunked")) return -1;
                response->chunked = 1;
            } else if (!strcasecmp(line, "Content-Type")) {
                response->html = (!strncasecmp(value, "text/html", 9) && (!value[9] || value[9] == ';'))
                    || (!strncasecmp(value, "application/xhtml+xml", 21) && (!value[21] || value[21] == ';'));
            } else if (!strcasecmp(line, "Content-Encoding")) {
                response->encoded = strcasecmp(value, "identity") != 0;
            } else if (!strcasecmp(line, "X-Proxy-Final-URL")) {
                if (strlen(value) >= sizeof(response->final_url)) return -1;
                strcpy(response->final_url, value);
            } else if (!strcasecmp(line, "X-Proxy-Error")) {
                if (strlen(value) >= sizeof(response->proxy_error)) return -1;
                strcpy(response->proxy_error, value);
            } else if (!strcasecmp(line, "X-Proxy-Redirects")) {
                uint64_t redirects;
                if (decimal_length(value, &redirects) || redirects > 5) return -1;
                response->redirects = (unsigned)redirects;
            }
        }
        if (response->chunked && response->has_length) return -1;
        if (response->status >= 200) return 0;
        if (response->status == 101) return -1;
    }
    return -1;
}

static int copy_exact(struct reader *r, FILE *file, uint64_t count, uint64_t *total) {
    if (count > BODY_LIMIT - *total) return -1;
    unsigned char buffer[8192];
    while (count) {
        size_t wanted = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        ssize_t n = read_some(r, buffer, wanted);
        if (n <= 0 || fwrite(buffer, 1, (size_t)n, file) != (size_t)n) return -1;
        count -= (uint64_t)n;
        *total += (uint64_t)n;
    }
    return 0;
}

static int save_body(struct reader *r, const struct response *response, FILE *file, uint64_t *total) {
    *total = 0;
    if (response->status == 204 || response->status == 205) return 0;
    if (response->chunked) {
        char line[8192];
        for (;;) {
            if (read_line(r, line, sizeof(line)) < 0) return -1;
            char *extension = strchr(line, ';');
            if (extension) *extension = '\0';
            if (!*line) return -1;
            for (char *p = line; *p; ++p) if (!isxdigit((unsigned char)*p)) return -1;
            errno = 0;
            unsigned long long chunk = strtoull(line, NULL, 16);
            if (errno || chunk > BODY_LIMIT - *total) return -1;
            if (!chunk) {
                size_t trailers = 0;
                int n;
                do {
                    n = read_line(r, line, sizeof(line));
                    if (n < 0 || (trailers += (size_t)n) > HEADER_LIMIT) return -1;
                    if (*line && !strchr(line, ':')) return -1;
                } while (*line);
                return 0;
            }
            if (copy_exact(r, file, chunk, total) || read_line(r, line, sizeof(line)) != 2) return -1;
        }
    }
    if (response->has_length) return copy_exact(r, file, response->length, total);
    /* A close-delimited body cannot reveal an early clean EOF without a declared length. */
    unsigned char buffer[8192];
    for (;;) {
        ssize_t n = read_some(r, buffer, sizeof(buffer));
        if (n < 0) return -1;
        if (!n) return 0;
        if ((uint64_t)n > BODY_LIMIT - *total
            || fwrite(buffer, 1, (size_t)n, file) != (size_t)n) return -1;
        *total += (uint64_t)n;
    }
}

static void filename_base(const struct http_url *url, char *base, size_t capacity) {
    size_t used = 0;
    const char *pieces[] = {url->host, "_", url->path};
    for (size_t i = 0; i < 3; ++i) {
        for (const char *p = pieces[i]; *p && *p != '?' && used + 1 < capacity; ++p) {
            char c = (isalnum((unsigned char)*p) || *p == '-' || *p == '.') ? *p : '_';
            if (c == '_' && used && base[used - 1] == '_') continue;
            base[used++] = c;
        }
    }
    while (used && base[used - 1] == '_') --used;
    base[used] = '\0';
    if (used >= 5 && !strcasecmp(base + used - 5, ".html")) base[used - 5] = '\0';
    else if (used >= 4 && !strcasecmp(base + used - 4, ".htm")) base[used - 4] = '\0';
    if (!*base) strcpy(base, "page");
}

static int download(const char *proxy_host, const char *proxy_port, const char *url_text,
                    const char *requested_output) {
    struct http_url url;
    char error[160];
    if (parse_http_url(url_text, &url, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); return EXIT_FAILURE;
    }
    int fd = connect_tcp(proxy_host, proxy_port);
    if (fd < 0) { fprintf(stderr, "Could not connect to proxy.\n"); return EXIT_FAILURE; }
    /* The proxy can spend up to 20 seconds fetching before returning headers. */
    struct timeval response_timeout = {30, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &response_timeout, sizeof(response_timeout))) {
        perror("Response timeout"); close(fd); return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    FILE *file = NULL;
    char temporary[PATH_MAX] = "", output[PATH_MAX], base[160];
    char request[URL_LIMIT + 512];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nAccept: text/html,application/xhtml+xml\r\n"
                     "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", url_text, url.authority);
    if (n < 0 || (size_t)n >= sizeof(request) || send_all(fd, request, (size_t)n)) {
        fprintf(stderr, "Could not send request to proxy.\n"); goto cleanup;
    }
    struct reader reader = {.socket = fd};
    struct response response;
    if (read_headers(&reader, &response)) {
        fprintf(stderr, "Incomplete, oversized, or unsupported HTTP response headers.\n"); goto cleanup;
    }
    printf("HTTP status: %d [%s]\n", response.status, url_text);
    if (*response.final_url)
        printf("Final URL: %s (%u redirects)\n", response.final_url, response.redirects);
    if (response.status < 200 || response.status >= 300) {
        if (*response.proxy_error)
            fprintf(stderr, "HTTP %d%s: %s Page not saved.\n", response.status,
                    response.status == 502 ? " Bad Gateway" : response.status == 504 ? " Gateway Timeout" : "",
                    response.proxy_error);
        else if (response.status >= 300 && response.status < 400)
            fprintf(stderr, "HTTP %d: destination did not provide a usable redirect; page not saved.\n", response.status);
        else if (response.status == 502)
            fprintf(stderr, "HTTP 502 Bad Gateway: destination fetch failed; page not saved. "
                            "Check the destination URL and that its server is running.\n");
        else
            fprintf(stderr, "HTTP %d: server returned an error; page not saved.\n", response.status);
        goto cleanup;
    }
    if (!response.html || response.encoded) {
        fprintf(stderr, "Expected HTML with identity content encoding; page not saved.\n"); goto cleanup;
    }
    if (response.has_length && response.length > BODY_LIMIT) {
        fprintf(stderr, "Response exceeds the 50 MiB download limit.\n"); goto cleanup;
    }
    if (requested_output) n = snprintf(output, sizeof(output), "%s", requested_output);
    else {
        if (mkdir("downloads", 0755) && errno != EEXIST) { perror("downloads"); goto cleanup; }
        filename_base(&url, base, sizeof(base));
        n = snprintf(output, sizeof(output), "downloads/%s.html", base);
    }
    if (n <= 0 || (size_t)n >= sizeof(output)) { fprintf(stderr, "Output path is too long.\n"); goto cleanup; }
    n = snprintf(temporary, sizeof(temporary), "%s.partXXXXXX", output);
    if (n < 0 || (size_t)n >= sizeof(temporary)) { *temporary = '\0'; goto cleanup; }
    int temporary_fd = mkstemp(temporary);
    if (temporary_fd < 0) { *temporary = '\0'; perror("Output file"); goto cleanup; }
    file = fdopen(temporary_fd, "wb");
    if (!file) { close(temporary_fd); perror("fdopen"); goto cleanup; }
    uint64_t total;
    if (save_body(&reader, &response, file, &total)) {
        fprintf(stderr, "Download failed: incomplete/invalid body, timeout, write error, or 50 MiB limit.\n");
        goto cleanup;
    }
    if (fclose(file)) { file = NULL; perror("Output write"); goto cleanup; }
    file = NULL;
    /* Publish the completed file atomically, without overwriting an existing name. */
    unsigned suffix = 0;
    while (link(temporary, output)) {
        if (errno != EEXIST || requested_output || ++suffix > 10000) {
            perror("Cannot finalize output (existing files are never overwritten)"); goto cleanup;
        }
        snprintf(output, sizeof(output), "downloads/%s_%u.html", base, suffix);
    }
    printf("Saved %llu bytes to %s\n", (unsigned long long)total, output);
    result = EXIT_SUCCESS;
cleanup:
    if (file) fclose(file);
    if (*temporary) unlink(temporary);
    close(fd);
    return result;
}

#define MAX_BATCH_URLS 1000
#define MAX_CLIENT_WORKERS 16
struct download_job {
    char url[URL_LIMIT];
    char output[PATH_MAX];
    int result;
};
struct batch {
    const char *host, *port;
    struct download_job *jobs;
    size_t count, next;
    pthread_mutex_t lock;
};

/* Only taking the next job needs a lock; every download owns its buffers and socket. */
static void *batch_worker(void *context) {
    struct batch *batch = context;
    for (;;) {
        pthread_mutex_lock(&batch->lock);
        size_t index = batch->next;
        if (index < batch->count) ++batch->next;
        pthread_mutex_unlock(&batch->lock);
        if (index >= batch->count) break;
        struct download_job *job = &batch->jobs[index];
        job->result = download(batch->host, batch->port, job->url, job->output);
        fprintf(stderr, "[job %zu] %s: %s\n", index + 1,
                job->result == EXIT_SUCCESS ? "saved" : "failed", job->url);
    }
    return NULL;
}

static int run_batch(const char *host, const char *port, const char *filename, size_t workers) {
    FILE *input = fopen(filename, "r");
    if (!input) { perror("URL list"); return EXIT_FAILURE; }
    struct download_job *jobs = calloc(MAX_BATCH_URLS, sizeof(*jobs));
    if (!jobs) { fclose(input); return EXIT_FAILURE; }
    char *line = NULL;
    size_t capacity = 0, count = 0, line_number = 0;
    ssize_t length;
    int result = EXIT_FAILURE;
    while ((length = getline(&line, &capacity, input)) >= 0) {
        ++line_number;
        if (memchr(line, '\0', (size_t)length)) {
            fprintf(stderr, "URL list line %zu contains a NUL byte.\n", line_number); goto cleanup;
        }
        while (length && isspace((unsigned char)line[length - 1])) line[--length] = '\0';
        char *start = line;
        while (*start == ' ' || *start == '\t') ++start;
        if (!*start || *start == '#') continue;
        struct http_url parsed;
        char error[160];
        if (count == MAX_BATCH_URLS || parse_http_url(start, &parsed, error, sizeof(error))) {
            fprintf(stderr, "Invalid URL list at line %zu (HTTP/HTTPS URLs only; maximum %d entries).\n",
                    line_number, MAX_BATCH_URLS); goto cleanup;
        }
        strcpy(jobs[count++].url, start);
    }
    if (ferror(input) || !count) { fprintf(stderr, "URL list is empty or unreadable.\n"); goto cleanup; }
    if (mkdir("downloads", 0755) && errno != EEXIST) { perror("downloads"); goto cleanup; }
    char directory[] = "downloads/batch_XXXXXX";
    if (!mkdtemp(directory)) { perror("Batch directory"); goto cleanup; }
    /* Reserve a unique directory and assign all names before any worker is started. */
    for (size_t i = 0; i < count; ++i) {
        struct http_url parsed;
        char error[160], base[160];
        parse_http_url(jobs[i].url, &parsed, error, sizeof(error));
        filename_base(&parsed, base, sizeof(base));
        snprintf(jobs[i].output, sizeof(jobs[i].output), "%s/%04zu_%s.html", directory, i + 1, base);
        jobs[i].result = EXIT_FAILURE;
    }
    if (workers > count) workers = count;
    struct batch batch = {.host = host, .port = port, .jobs = jobs, .count = count};
    int status = pthread_mutex_init(&batch.lock, NULL);
    if (status) { fprintf(stderr, "Batch mutex: %s\n", strerror(status)); goto cleanup; }
    pthread_t threads[MAX_CLIENT_WORKERS];
    size_t started = 0;
    printf("Batch: %zu URLs, %zu workers maximum, output %s\n", count, workers, directory);
    for (; started < workers; ++started) {
        status = pthread_create(&threads[started], NULL, batch_worker, &batch);
        if (status) {
            fprintf(stderr, "Worker creation: %s; using main thread for remaining work.\n", strerror(status));
            batch_worker(&batch);
            break;
        }
    }
    for (size_t i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    pthread_mutex_destroy(&batch.lock);
    size_t saved = 0;
    for (size_t i = 0; i < count; ++i) if (jobs[i].result == EXIT_SUCCESS) ++saved;
    printf("Batch complete: %zu saved, %zu failed. Output: %s\n", saved, count - saved, directory);
    result = saved == count ? EXIT_SUCCESS : EXIT_FAILURE;
cleanup:
    free(line);
    free(jobs);
    fclose(input);
    return result;
}

int main(int argc, char **argv) {
    if (argc >= 4 && valid_port(argv[2])) {
        if ((argc == 5 || argc == 7) && !strcmp(argv[3], "--batch")) {
            size_t workers = 4;
            if (argc == 7) {
                if (strcmp(argv[5], "--jobs") || !valid_port(argv[6])) goto usage;
                workers = (size_t)strtoul(argv[6], NULL, 10);
                if (workers > MAX_CLIENT_WORKERS) goto usage;
            }
            return run_batch(argv[1], argv[2], argv[4], workers);
        }
        if (argc == 4 || (argc == 6 && !strcmp(argv[4], "--output")))
            return download(argv[1], argv[2], argv[3], argc == 6 ? argv[5] : NULL);
    }
usage:
    fprintf(stderr, "Usage: %s PROXY_HOST PROXY_PORT HTTP_OR_HTTPS_URL [--output FILE.html]\n"
                    "       %s PROXY_HOST PROXY_PORT --batch URLS.txt [--jobs 1-16]\n", argv[0], argv[0]);
    return EXIT_FAILURE;
}
