#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <openssl/evp.h>
#include <curl/curl.h>
#include "http_downloader.h"

int torrent_downloader_download(const char *torrent_or_magnet, const char *output_dir, bool quiet);

static struct option long_options[] = {
    {"connections", required_argument, 0, 'n'},
    {"output",      required_argument, 0, 'o'},
    {"quiet",       no_argument,       0, 'q'},
    {"max-speed",   required_argument, 0, 's'},
    {"user-agent",  required_argument, 0, 'U'},
    {"resume",      no_argument,       0, 'c'},
    {"header",      required_argument, 0, 'H'},
    {"proxy",       required_argument, 0, 1001},
    {"checksum",    required_argument, 0, 1002},
    {0, 0, 0, 0}
};

static unsigned long long parse_speed(const char *speed_str) {
    if (!speed_str || strlen(speed_str) == 0) {
        return 0;
    }
    char *dup = strdup(speed_str);
    char *p = dup;
    while (*p) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
        p++;
    }
    size_t len = strlen(dup);
    unsigned long long unit = 1;
    if (len >= 2 && strcmp(dup + len - 2, "MB") == 0) {
        dup[len - 2] = '\0';
        unit = 1024 * 1024;
    } else if (len >= 1 && dup[len - 1] == 'M') {
        dup[len - 1] = '\0';
        unit = 1024 * 1024;
    } else if (len >= 2 && strcmp(dup + len - 2, "KB") == 0) {
        dup[len - 2] = '\0';
        unit = 1024;
    } else if (len >= 1 && dup[len - 1] == 'K') {
        dup[len - 1] = '\0';
        unit = 1024;
    }
    unsigned long long val = strtoull(dup, NULL, 10);
    free(dup);
    return val * unit;
}

static bool verify_checksum(const char *path, const char *checksum_arg) {
    char algo[64] = "sha256";
    char expected[256] = "";
    const char *colon = strchr(checksum_arg, ':');
    if (colon) {
        size_t algo_len = colon - checksum_arg;
        if (algo_len < sizeof(algo)) {
            strncpy(algo, checksum_arg, algo_len);
            algo[algo_len] = '\0';
        }
        strncpy(expected, colon + 1, sizeof(expected) - 1);
    } else {
        strncpy(expected, checksum_arg, sizeof(expected) - 1);
    }
    for (char *p = algo; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    for (char *p = expected; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    const EVP_MD *md = NULL;
    if (strcmp(algo, "sha256") == 0) {
        md = EVP_sha256();
    } else if (strcmp(algo, "sha512") == 0) {
        md = EVP_sha512();
    } else if (strcmp(algo, "md5") == 0) {
        md = EVP_md5();
    } else {
        fprintf(stderr, "Unsupported checksum algorithm: %s. Use sha256, sha512, or md5\n", algo);
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open file for checksum verification: %s\n", path);
        return false;
    }
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        fclose(file);
        return false;
    }
    if (EVP_DigestInit_ex(mdctx, md, NULL) != 1) {
        EVP_MD_CTX_free(mdctx);
        fclose(file);
        return false;
    }
    unsigned char buffer[65536];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (EVP_DigestUpdate(mdctx, buffer, bytes_read) != 1) {
            EVP_MD_CTX_free(mdctx);
            fclose(file);
            return false;
        }
    }
    fclose(file);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        return false;
    }
    EVP_MD_CTX_free(mdctx);
    char actual[512] = "";
    for (unsigned int i = 0; i < hash_len; i++) {
        snprintf(actual + (i * 2), 3, "%02x", hash[i]);
    }
    if (strcmp(actual, expected) == 0) {
        printf("\033[1;92mChecksum verification successful!\033[0m\n");
        return true;
    } else {
        fprintf(stderr, "\033[31mChecksum mismatch!\nExpected: %s\nActual:   %s\033[0m\n", expected, actual);
        return false;
    }
}

int main(int argc, char *argv[]) {
    int connections = 4;
    char *output = NULL;
    bool quiet = false;
    char *max_speed_str = NULL;
    char *user_agent = NULL;
    bool resume = false;
    char *headers = NULL;
    size_t headers_len = 0;
    char *proxy = NULL;
    char *checksum = NULL;
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "n:o:qs:U:cH:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'n':
                connections = atoi(optarg);
                if (connections <= 0) {
                    fprintf(stderr, "Error: Connections must be greater than 0.\n");
                    return 1;
                }
                break;
            case 'o':
                output = optarg;
                break;
            case 'q':
                quiet = true;
                break;
            case 's':
                max_speed_str = optarg;
                break;
            case 'U':
                user_agent = optarg;
                break;
            case 'c':
                resume = true;
                break;
            case 'H':
                if (headers == NULL) {
                    headers = strdup(optarg);
                    headers_len = strlen(headers);
                } else {
                    size_t new_len = headers_len + 1 + strlen(optarg) + 1;
                    headers = realloc(headers, new_len);
                    strcat(headers, "\n");
                    strcat(headers, optarg);
                    headers_len = new_len - 1;
                }
                break;
            case 1001:
                proxy = optarg;
                break;
            case 1002:
                checksum = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [options] <url>\n", argv[0]);
                return 1;
        }
    }
    int input_count = argc - optind;
    if (input_count <= 0) {
        fprintf(stderr, "No inputs provided.\n");
        if (headers) free(headers);
        return 1;
    }
    char **inputs = malloc(input_count * sizeof(char*));
    for (int i = 0; i < input_count; i++) {
        inputs[i] = argv[optind + i];
    }
    if (!quiet) {
        for (int i = 0; i < 60; i++) printf("\033[36m=\033[0m");
        printf("\n \033[1;92mRUX - High Performance Downloader\033[0m \n");
        for (int i = 0; i < 60; i++) printf("\033[36m=\033[0m");
        printf("\n");
        printf("\033[1;33m%-15s\033[0m \033[34m%s\033[0m\n", "Input:", inputs[0]);
        printf("\033[1;33m%-15s\033[0m \033[35m%d\033[0m\n", "Connections:", connections);
        if (output && strlen(output) > 0) {
            printf("\033[1;33m%-15s\033[0m \033[32m%s\033[0m\n", "Output:", output);
        }
        for (int i = 0; i < 60; i++) printf("\033[2m-\033[0m");
        printf("\n");
    }
    const char *primary_input = inputs[0];
    if (strncmp(primary_input, "magnet:", 7) == 0 || 
        (strlen(primary_input) >= 8 && strcmp(primary_input + strlen(primary_input) - 8, ".torrent") == 0)) {
        int status = torrent_downloader_download(primary_input, output, quiet);
        free(inputs);
        if (headers) free(headers);
        return status;
    }
    if (strncmp(primary_input, "http://", 7) != 0 && strncmp(primary_input, "https://", 8) != 0) {
        fprintf(stderr, "Unsupported input format. Please provide an HTTP/HTTPS URL.\n");
        free(inputs);
        if (headers) free(headers);
        return 1;
    }
    curl_global_init(CURL_GLOBAL_ALL);
    unsigned long long parsed_speed = parse_speed(max_speed_str);
    size_t total_len = 0;
    for (int i = 0; i < input_count; i++) {
        total_len += strlen(inputs[i]) + 1;
    }
    char *inputs_joined = malloc(total_len + 1);
    inputs_joined[0] = '\0';
    for (int i = 0; i < input_count; i++) {
        strcat(inputs_joined, inputs[i]);
        if (i < input_count - 1) {
            strcat(inputs_joined, "\n");
        }
    }
    char final_path[4096] = "";
    int status = http_downloader_download(
        inputs_joined,
        connections,
        output,
        quiet,
        parsed_speed,
        user_agent,
        resume,
        headers,
        proxy,
        final_path,
        sizeof(final_path)
    );
    free(inputs_joined);
    free(inputs);
    if (headers) free(headers);
    curl_global_cleanup();
    if (status != 0) {
        fprintf(stderr, "Download failed with error code: %d\n", status);
        return 1;
    }
    if (checksum && strlen(checksum) > 0) {
        if (!verify_checksum(final_path, checksum)) {
            return 1;
        }
    }
    return 0;
}
