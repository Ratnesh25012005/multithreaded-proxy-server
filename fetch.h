#ifndef PROJECT_FETCH_H
#define PROJECT_FETCH_H
#include "net.h"
#include <stdio.h>

struct fetch_result {
    FILE *body;
    size_t length;
    long status, redirects;
    char content_type[256];
    char final_url[URL_LIMIT];
    char error[256];
    int error_status;
};

int fetch_global_init(void);
int fetch_page(const char *url, struct fetch_result *result);

#endif
