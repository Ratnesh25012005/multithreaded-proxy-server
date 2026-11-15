#define _POSIX_C_SOURCE 200809L
#include "fetch.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct transfer {
    struct fetch_result *result;
    size_t header_bytes;
    int has_length, has_transfer;
    unsigned long long declared_length;
    const char *failure;
};

int fetch_global_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

static size_t receive_body(char *data, size_t size, size_t count, void *context) {
    struct transfer *t = context;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes > BODY_LIMIT - t->result->length) {
        t->failure = "Destination response exceeds the 50 MiB decoded-body limit.";
        return 0;
    }
    size_t written = fwrite(data, 1, bytes, t->result->body);
    if (written != bytes) t->failure = "Could not write the temporary response file.";
    t->result->length += written;
    return written;
}

/* Validate each response block and discard any previous redirect/interim body. */
static size_t receive_header(char *data, size_t size, size_t count, void *context) {
    struct transfer *t = context;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes > HEADER_LIMIT || memchr(data, '\0', bytes)) goto invalid;
    if (bytes >= 5 && !memcmp(data, "HTTP/", 5)) {
        t->header_bytes = 0;
        t->has_length = t->has_transfer = 0;
        t->result->length = 0;
        t->result->content_type[0] = '\0';
        if (fflush(t->result->body) || ftruncate(fileno(t->result->body), 0)
            || fseek(t->result->body, 0, SEEK_SET)) {
            t->failure = "Could not reset the temporary response file."; return 0;
        }
    }
    if (bytes > HEADER_LIMIT - t->header_bytes) {
        t->failure = "Destination response headers exceed 64 KiB."; return 0;
    }
    t->header_bytes += bytes;
    char line[HEADER_LIMIT + 1];
    memcpy(line, data, bytes);
    line[bytes] = '\0';
    char *colon = strchr(line, ':');
    if (!colon) return bytes; /* status line or blank line */
    *colon++ = '\0';
    while (*colon == ' ' || *colon == '\t') ++colon;
    char *end = colon + strlen(colon);
    while (end > colon && strchr(" \t\r\n", end[-1])) *--end = '\0';
    if (!strcasecmp(line, "Content-Length")) {
        if (!*colon) goto invalid;
        unsigned long long value = 0;
        for (char *p = colon; *p; ++p) {
            unsigned digit = (unsigned)(*p - '0');
            if (!isdigit((unsigned char)*p) || value > (UINT64_MAX - digit) / 10) goto invalid;
            value = value * 10 + digit;
        }
        if (t->has_transfer || (t->has_length && value != t->declared_length)) goto invalid;
        t->has_length = 1;
        t->declared_length = value;
        if (value > BODY_LIMIT) {
            t->failure = "Destination declared a response larger than 50 MiB."; return 0;
        }
    } else if (!strcasecmp(line, "Transfer-Encoding")) {
        if (t->has_length || t->has_transfer || strcasecmp(colon, "chunked")) goto invalid;
        t->has_transfer = 1;
    } else if (!strcasecmp(line, "Content-Type")) {
        if (strlen(colon) >= sizeof(t->result->content_type)) goto invalid;
        for (char *p = colon; *p; ++p)
            if ((unsigned char)*p < 32 || (unsigned char)*p == 127) goto invalid;
        strcpy(t->result->content_type, colon);
    } else if (!strcasecmp(line, "Location") && strlen(colon) >= URL_LIMIT) {
        t->failure = "Redirect URL is too long."; return 0;
    }
    return bytes;
invalid:
    t->failure = "Destination returned invalid or ambiguous response headers.";
    return 0;
}

int fetch_page(const char *url, struct fetch_result *result) {
    memset(result, 0, sizeof(*result));
    result->error_status = 502;
    result->body = tmpfile();
    if (!result->body) {
        result->error_status = 500;
        snprintf(result->error, sizeof(result->error), "Could not create a temporary response file.");
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(result->error, sizeof(result->error), "Could not initialize the destination fetch.");
        return -1;
    }
    struct transfer transfer = {.result = result};
    CURLcode code;
#define SET(option, value) do { code = curl_easy_setopt(curl, option, value); if (code != CURLE_OK) goto finished; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROTOCOLS_STR, "http,https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    SET(CURLOPT_FOLLOWLOCATION, 1L);
    SET(CURLOPT_MAXREDIRS, 5L);
    SET(CURLOPT_CONNECTTIMEOUT, 8L);
    SET(CURLOPT_TIMEOUT, 20L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_PROXY, ""); /* Our worker connects directly, independent of shell proxy variables. */
    SET(CURLOPT_USERAGENT, "OSProjectProxy/1.0");
    SET(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_ACCEPT_ENCODING, ""); /* Decode the encodings supported by this libcurl build. */
    SET(CURLOPT_WRITEFUNCTION, receive_body);
    SET(CURLOPT_WRITEDATA, &transfer);
    SET(CURLOPT_HEADERFUNCTION, receive_header);
    SET(CURLOPT_HEADERDATA, &transfer);
    const char *ca_bundle = getenv("PROXY_CA_BUNDLE");
    if (ca_bundle && *ca_bundle) SET(CURLOPT_CAINFO, ca_bundle);
    code = curl_easy_perform(curl);
finished:
    if (code == CURLE_OK) {
        char *effective = NULL;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->status) != CURLE_OK
            || curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &result->redirects) != CURLE_OK
            || curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective) != CURLE_OK
            || !effective || strlen(effective) >= sizeof(result->final_url)
            || result->status < 200 || result->status > 599) {
            transfer.failure = "Destination returned an unsupported response.";
            code = CURLE_WEIRD_SERVER_REPLY;
        } else {
            for (const char *p = effective; *p; ++p)
                if ((unsigned char)*p < 32 || (unsigned char)*p == 127) code = CURLE_URL_MALFORMAT;
            if (code == CURLE_OK) strcpy(result->final_url, effective);
        }
        if (fflush(result->body) || fseek(result->body, 0, SEEK_SET)) {
            transfer.failure = "Could not read the temporary response file.";
            code = CURLE_WRITE_ERROR;
        }
    }
    if (code != CURLE_OK) {
        const char *message = transfer.failure;
        if (!message) {
            if (code == CURLE_PEER_FAILED_VERIFICATION) message = "Destination TLS certificate verification failed.";
            else if (code == CURLE_TOO_MANY_REDIRECTS) message = "Destination exceeded the limit of 5 redirects.";
            else if (code == CURLE_OPERATION_TIMEDOUT) message = "Destination fetch timed out after 20 seconds.";
            else if (code == CURLE_UNSUPPORTED_PROTOCOL) message = "Only HTTP and HTTPS destinations and redirects are allowed.";
            else if (code == CURLE_PARTIAL_FILE) message = "Destination response was incomplete.";
            else message = curl_easy_strerror(code);
        }
        if (code == CURLE_OPERATION_TIMEDOUT) result->error_status = 504;
        snprintf(result->error, sizeof(result->error), "%s", message);
    }
    curl_easy_cleanup(curl);
    return code == CURLE_OK ? 0 : -1;
#undef SET
}
