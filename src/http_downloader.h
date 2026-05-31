#ifndef RUX_HTTP_DOWNLOADER_H
#define RUX_HTTP_DOWNLOADER_H

#include <stddef.h>
#include <stdbool.h>

int http_downloader_download(
    const char *urls,
    size_t connections,
    const char *output_path,
    bool quiet,
    unsigned long long max_speed,
    const char *user_agent,
    bool resume,
    const char *headers,
    const char *proxy,
    char *out_final_path,
    size_t out_final_path_len
);

#endif
