#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <curl/curl.h>
#include "http_downloader.h"

typedef struct {
    char *url;
    char *path;
    unsigned long long start;
    unsigned long long end;
    _Atomic unsigned long long *current;
    bool success;
    char *state_path;
    void *all_tasks;
    size_t tasks_count;
    unsigned long long total_size;
    int fd;
    unsigned long long max_speed;
    const char *user_agent;
    const char *headers_str;
    const char *proxy;
} ThreadTask;

typedef struct {
    ThreadTask *tasks;
    size_t count;
    unsigned long long total_size;
    bool quiet;
    _Atomic bool done;
} ProgressInfo;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long long bytes_downloaded_this_session = 0;

static void save_state(const char *state_path, unsigned long long total_size, ThreadTask *tasks, size_t count) {
    pthread_mutex_lock(&state_mutex);
    FILE *out = fopen(state_path, "w");
    if (!out) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    fprintf(out, "%llu\n", total_size);
    for (size_t i = 0; i < count; i++) {
        unsigned long long curr = tasks[i].current ? atomic_load(tasks[i].current) : tasks[i].start;
        fprintf(out, "%llu,%llu,%llu\n", tasks[i].start, tasks[i].end, curr);
    }
    fclose(out);
    pthread_mutex_unlock(&state_mutex);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_bytes = size * nmemb;
    ThreadTask *task = (ThreadTask*)userp;
    unsigned long long curr = task->current ? atomic_load(task->current) : 0;
    ssize_t written = pwrite(task->fd, contents, total_bytes, curr);
    if (written < 0) {
        return 0;
    }
    if (task->current) {
        atomic_fetch_add(task->current, written);
    }
    atomic_fetch_add(&bytes_downloaded_this_session, written);
    if (task->all_tasks && task->state_path) {
        save_state(task->state_path, task->total_size, (ThreadTask*)task->all_tasks, task->tasks_count);
    }
    return written;
}

typedef struct {
    bool accept_ranges;
} HeaderData;

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    HeaderData *hd = (HeaderData*)userdata;
    char *line = malloc(total + 1);
    if (!line) {
        return total;
    }
    memcpy(line, buffer, total);
    line[total] = '\0';
    for (size_t i = 0; i < total; i++) {
        if (line[i] >= 'A' && line[i] <= 'Z') {
            line[i] += 32;
        }
    }
    if (strstr(line, "accept-ranges: bytes") != NULL) {
        hd->accept_ranges = true;
    }
    free(line);
    return total;
}

static void* download_range(void *arg) {
    ThreadTask *task = (ThreadTask*)arg;
    task->success = false;
    unsigned long long curr = task->current ? atomic_load(task->current) : task->start;
    if (curr > task->end) {
        task->success = true;
        return NULL;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        return NULL;
    }
    int raw_fd = open(task->path, O_WRONLY);
    if (raw_fd < 0) {
        curl_easy_cleanup(curl);
        return NULL;
    }
    task->fd = raw_fd;
    char range_header[128];
    snprintf(range_header, sizeof(range_header), "%llu-%llu", curr, task->end);
    curl_easy_setopt(curl, CURLOPT_URL, task->url);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, task);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    if (task->max_speed > 0) {
        curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)task->max_speed);
    }
    if (task->user_agent && *task->user_agent) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, task->user_agent);
    }
    if (task->proxy && *task->proxy) {
        curl_easy_setopt(curl, CURLOPT_PROXY, task->proxy);
    }
    struct curl_slist *headers_list = NULL;
    if (task->headers_str && *task->headers_str) {
        char *h_copy = strdup(task->headers_str);
        char *tok = strtok(h_copy, "\n");
        while (tok) {
            if (strlen(tok) > 0) {
                headers_list = curl_slist_append(headers_list, tok);
            }
            tok = strtok(NULL, "\n");
        }
        free(h_copy);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
    }
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        task->success = true;
    }
    close(raw_fd);
    curl_easy_cleanup(curl);
    if (headers_list) {
        curl_slist_free_all(headers_list);
    }
    return NULL;
}

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void* progress_thread_func(void *arg) {
    ProgressInfo *pi = (ProgressInfo*)arg;
    double start_time = get_time_sec();
    double last_time = start_time;
    unsigned long long last_downloaded = 0;
    double speed = 0.0;
    while (!atomic_load(&pi->done)) {
        usleep(200000);
        double now = get_time_sec();
        unsigned long long current_downloaded = atomic_load(&bytes_downloaded_this_session);
        double dt = now - last_time;
        if (dt >= 0.5) {
            speed = (double)(current_downloaded - last_downloaded) / dt;
            last_downloaded = current_downloaded;
            last_time = now;
        }
        unsigned long long total_completed = 0;
        for (size_t i = 0; i < pi->count; ++i) {
            unsigned long long curr = pi->tasks[i].current ? atomic_load(pi->tasks[i].current) : pi->tasks[i].start;
            total_completed += (curr - pi->tasks[i].start);
        }
        if (pi->quiet) {
            continue;
        }
        double percent = 0.0;
        if (pi->total_size > 0) {
            percent = (double)total_completed / (double)pi->total_size * 100.0;
        }
        char speed_str[32];
        if (speed < 1024) {
            snprintf(speed_str, sizeof(speed_str), "%.1f B/s", speed);
        } else if (speed < 1024 * 1024) {
            snprintf(speed_str, sizeof(speed_str), "%.1f KB/s", speed / 1024.0);
        } else {
            snprintf(speed_str, sizeof(speed_str), "%.1f MB/s", speed / (1024.0 * 1024.0));
        }
        char size_str[64];
        if (pi->total_size > 0) {
            snprintf(size_str, sizeof(size_str), "%llu/%llu bytes", total_completed, pi->total_size);
        } else {
            snprintf(size_str, sizeof(size_str), "%llu bytes", total_completed);
        }
        char eta_str[32] = "N/A";
        if (pi->total_size > 0 && speed > 0) {
            double remaining_bytes = (double)(pi->total_size - total_completed);
            double eta_sec = remaining_bytes / speed;
            if (eta_sec < 60) {
                snprintf(eta_str, sizeof(eta_str), "%.0fs", eta_sec);
            } else if (eta_sec < 3600) {
                snprintf(eta_str, sizeof(eta_str), "%dm %ds", (int)eta_sec / 60, (int)eta_sec % 60);
            } else {
                snprintf(eta_str, sizeof(eta_str), "%dh %dm", (int)eta_sec / 3600, ((int)eta_sec % 3600) / 60);
            }
        }
        double elapsed = now - start_time;
        int el_h = (int)elapsed / 3600;
        int el_m = ((int)elapsed % 3600) / 60;
        int el_s = (int)elapsed % 60;
        fprintf(stderr, "\r\033[K");
        if (pi->total_size > 0) {
            int bar_width = 30;
            int pos = (int)(percent / 100.0 * bar_width);
            fprintf(stderr, "[%02d:%02d:%02d] [", el_h, el_m, el_s);
            for (int i = 0; i < bar_width; ++i) {
                if (i < pos) fprintf(stderr, "#");
                else if (i == pos) fprintf(stderr, ">");
                else fprintf(stderr, "-");
            }
            fprintf(stderr, "] %.1f%% | %s | %s | ETA: %s", percent, size_str, speed_str, eta_str);
        } else {
            fprintf(stderr, "[%02d:%02d:%02d] | %s | %s", el_h, el_m, el_s, size_str, speed_str);
        }
        fflush(stderr);
    }
    return NULL;
}

static void get_filename_from_url(const char *url, char *out, size_t max_len) {
    const char *last_slash = strrchr(url, '/');
    if (last_slash) {
        const char *name = last_slash + 1;
        const char *question_mark = strchr(name, '?');
        size_t len = question_mark ? (size_t)(question_mark - name) : strlen(name);
        if (len > 0) {
            if (len >= max_len) len = max_len - 1;
            strncpy(out, name, len);
            out[len] = '\0';
            return;
        }
    }
    strncpy(out, "downloaded_file", max_len - 1);
    out[max_len - 1] = '\0';
}

static bool is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return false;
}

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
) {
    char **url_list = NULL;
    size_t url_count = 0;
    char *urls_copy = strdup(urls);
    char *token = strtok(urls_copy, "\n");
    while (token) {
        if (strlen(token) > 0) {
            url_list = realloc(url_list, (url_count + 1) * sizeof(char*));
            url_list[url_count++] = strdup(token);
        }
        token = strtok(NULL, "\n");
    }
    free(urls_copy);
    if (url_count == 0) {
        return -9;
    }
    CURL *head_curl = curl_easy_init();
    if (!head_curl) {
        for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
        free(url_list);
        return -1;
    }
    HeaderData hd = { false };
    curl_easy_setopt(head_curl, CURLOPT_URL, url_list[0]);
    curl_easy_setopt(head_curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(head_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(head_curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(head_curl, CURLOPT_HEADERDATA, &hd);
    if (user_agent && *user_agent) {
        curl_easy_setopt(head_curl, CURLOPT_USERAGENT, user_agent);
    }
    if (proxy && *proxy) {
        curl_easy_setopt(head_curl, CURLOPT_PROXY, proxy);
    }
    struct curl_slist *head_headers = NULL;
    if (headers && *headers) {
        char *h_copy = strdup(headers);
        char *tok = strtok(h_copy, "\n");
        while (tok) {
            if (strlen(tok) > 0) {
                head_headers = curl_slist_append(head_headers, tok);
            }
            tok = strtok(NULL, "\n");
        }
        free(h_copy);
        curl_easy_setopt(head_curl, CURLOPT_HTTPHEADER, head_headers);
    }
    CURLcode res = curl_easy_perform(head_curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(head_curl);
        if (head_headers) curl_slist_free_all(head_headers);
        for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
        free(url_list);
        return -2;
    }
    curl_off_t total_size = -1;
    curl_easy_getinfo(head_curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total_size);
    curl_easy_cleanup(head_curl);
    if (head_headers) curl_slist_free_all(head_headers);
    if (total_size <= 0) {
        connections = 1;
    }
    char file_name[4096];
    if (output_path == NULL || strlen(output_path) == 0) {
        get_filename_from_url(url_list[0], file_name, sizeof(file_name));
    } else if (is_directory(output_path)) {
        char name[1024];
        get_filename_from_url(url_list[0], name, sizeof(name));
        snprintf(file_name, sizeof(file_name), "%s/%s", output_path, name);
    } else {
        strncpy(file_name, output_path, sizeof(file_name) - 1);
        file_name[sizeof(file_name) - 1] = '\0';
    }
    if (out_final_path && out_final_path_len > 0) {
        strncpy(out_final_path, file_name, out_final_path_len - 1);
        out_final_path[out_final_path_len - 1] = '\0';
    }
    if (!quiet) {
        printf("\033[1;36mDownloading:\033[0m \033[32m%s\033[0m\n", file_name);
    }
    char state_path[4096 + 8];
    snprintf(state_path, sizeof(state_path), "%s.rux", file_name);
    bool state_loaded = false;
    unsigned long long *saved_starts = NULL;
    unsigned long long *saved_ends = NULL;
    unsigned long long *saved_currents = NULL;
    struct stat st;
    bool file_exists = (stat(file_name, &st) == 0);
    if (resume && file_exists) {
        FILE *in = fopen(state_path, "r");
        if (in) {
            unsigned long long saved_total_size = 0;
            if (fscanf(in, "%llu\n", &saved_total_size) == 1 && saved_total_size == (unsigned long long)total_size) {
                saved_starts = malloc(connections * sizeof(unsigned long long));
                saved_ends = malloc(connections * sizeof(unsigned long long));
                saved_currents = malloc(connections * sizeof(unsigned long long));
                size_t loaded_count = 0;
                while (loaded_count < connections && 
                       fscanf(in, "%llu,%llu,%llu\n", &saved_starts[loaded_count], &saved_ends[loaded_count], &saved_currents[loaded_count]) == 3) {
                    loaded_count++;
                }
                if (loaded_count == connections) {
                    state_loaded = true;
                } else {
                    free(saved_starts);
                    free(saved_ends);
                    free(saved_currents);
                    saved_starts = NULL;
                    saved_ends = NULL;
                    saved_currents = NULL;
                }
            }
            fclose(in);
        }
    }
    if (connections <= 1 || !hd.accept_ranges || total_size <= 0) {
        unsigned long long resume_from = 0;
        int flags = O_WRONLY | O_CREAT | (resume ? 0 : O_TRUNC);
        if (resume && file_exists) {
            resume_from = st.st_size;
        }
        int raw_sfd = open(file_name, flags, 0666);
        if (raw_sfd < 0) {
            for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
            free(url_list);
            return -6;
        }
        _Atomic unsigned long long single_progress;
        atomic_init(&single_progress, resume_from);
        ThreadTask task;
        task.url = url_list[0];
        task.path = file_name;
        task.start = 0;
        task.end = total_size > 0 ? (total_size - 1) : 0;
        task.current = &single_progress;
        task.success = false;
        task.state_path = NULL;
        task.all_tasks = NULL;
        task.tasks_count = 0;
        task.total_size = total_size > 0 ? total_size : 0;
        task.fd = raw_sfd;
        task.max_speed = max_speed;
        task.user_agent = user_agent;
        task.headers_str = headers;
        task.proxy = proxy;
        CURL *single_curl = curl_easy_init();
        if (!single_curl) {
            close(raw_sfd);
            for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
            free(url_list);
            return -5;
        }
        curl_easy_setopt(single_curl, CURLOPT_URL, url_list[0]);
        curl_easy_setopt(single_curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(single_curl, CURLOPT_WRITEDATA, &task);
        curl_easy_setopt(single_curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(single_curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(single_curl, CURLOPT_CONNECTTIMEOUT, 30L);
        if (resume_from > 0) {
            curl_easy_setopt(single_curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
        }
        if (max_speed > 0) {
            curl_easy_setopt(single_curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)max_speed);
        }
        if (user_agent && *user_agent) {
            curl_easy_setopt(single_curl, CURLOPT_USERAGENT, user_agent);
        }
        if (proxy && *proxy) {
            curl_easy_setopt(single_curl, CURLOPT_PROXY, proxy);
        }
        struct curl_slist *single_headers = NULL;
        if (headers && *headers) {
            char *h_copy = strdup(headers);
            char *tok = strtok(h_copy, "\n");
            while (tok) {
                if (strlen(tok) > 0) {
                    single_headers = curl_slist_append(single_headers, tok);
                }
                tok = strtok(NULL, "\n");
            }
            free(h_copy);
            curl_easy_setopt(single_curl, CURLOPT_HTTPHEADER, single_headers);
        }
        ProgressInfo pi;
        pi.tasks = &task;
        pi.count = 1;
        pi.total_size = total_size > 0 ? total_size : 0;
        pi.quiet = quiet;
        atomic_init(&pi.done, false);
        atomic_init(&bytes_downloaded_this_session, 0);
        pthread_t prog_thread;
        bool prog_thread_started = false;
        if (!quiet) {
            if (pthread_create(&prog_thread, NULL, progress_thread_func, &pi) == 0) {
                prog_thread_started = true;
            }
        }
        CURLcode sres = curl_easy_perform(single_curl);
        close(raw_sfd);
        curl_easy_cleanup(single_curl);
        if (single_headers) curl_slist_free_all(single_headers);
        if (prog_thread_started) {
            atomic_store(&pi.done, true);
            pthread_join(prog_thread, NULL);
            if (!quiet) {
                fprintf(stderr, "\n");
            }
        }
        for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
        free(url_list);
        if (sres == CURLE_OK) {
            if (!quiet) {
                printf("\033[1;32mDownload complete\033[0m\n");
            }
            return 0;
        }
        return -7;
    }
    if (!state_loaded) {
        int raw_fd = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (raw_fd < 0) {
            for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
            free(url_list);
            return -3;
        }
        if (ftruncate(raw_fd, total_size) != 0) {
            close(raw_fd);
            for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
            free(url_list);
            return -4;
        }
        close(raw_fd);
    }
    _Atomic unsigned long long *progress_counters = malloc(connections * sizeof(_Atomic unsigned long long));
    for (size_t i = 0; i < connections; i++) {
        if (state_loaded) {
            atomic_init(&progress_counters[i], saved_currents[i]);
        } else {
            atomic_init(&progress_counters[i], i * (total_size / connections));
        }
    }
    unsigned long long chunk_size = total_size / connections;
    ThreadTask *tasks = malloc(connections * sizeof(ThreadTask));
    for (size_t i = 0; i < connections; i++) {
        tasks[i].url = url_list[i % url_count];
        tasks[i].path = file_name;
        if (state_loaded) {
            tasks[i].start = saved_starts[i];
            tasks[i].end = saved_ends[i];
        } else {
            tasks[i].start = i * chunk_size;
            tasks[i].end = (i == connections - 1) ? (unsigned long long)(total_size - 1) : ((i + 1) * chunk_size - 1);
        }
        tasks[i].current = &progress_counters[i];
        tasks[i].success = false;
        tasks[i].state_path = state_path;
        tasks[i].all_tasks = tasks;
        tasks[i].tasks_count = connections;
        tasks[i].total_size = total_size;
        tasks[i].max_speed = (max_speed > 0) ? (max_speed / connections) : 0;
        tasks[i].user_agent = user_agent;
        tasks[i].headers_str = headers;
        tasks[i].proxy = proxy;
    }
    if (state_loaded) {
        free(saved_starts);
        free(saved_ends);
        free(saved_currents);
    } else {
        save_state(state_path, total_size, tasks, connections);
    }
    ProgressInfo pi;
    pi.tasks = tasks;
    pi.count = connections;
    pi.total_size = total_size;
    pi.quiet = quiet;
    atomic_init(&pi.done, false);
    atomic_init(&bytes_downloaded_this_session, 0);
    pthread_t prog_thread;
    bool prog_thread_started = false;
    if (!quiet) {
        if (pthread_create(&prog_thread, NULL, progress_thread_func, &pi) == 0) {
            prog_thread_started = true;
        }
    }
    pthread_t *threads = malloc(connections * sizeof(pthread_t));
    for (size_t i = 0; i < connections; i++) {
        pthread_create(&threads[i], NULL, download_range, &tasks[i]);
    }
    bool all_success = true;
    for (size_t i = 0; i < connections; i++) {
        pthread_join(threads[i], NULL);
        if (!tasks[i].success) {
            all_success = false;
        }
    }
    if (prog_thread_started) {
        atomic_store(&pi.done, true);
        pthread_join(prog_thread, NULL);
        if (!quiet) {
            fprintf(stderr, "\n");
        }
    }
    free(threads);
    free(progress_counters);
    free(tasks);
    for (size_t i = 0; i < url_count; ++i) free(url_list[i]);
    free(url_list);
    if (all_success) {
        remove(state_path);
        if (!quiet) {
            printf("\033[1;32mDownload complete\033[0m\n");
        }
        return 0;
    }
    return -8;
}
