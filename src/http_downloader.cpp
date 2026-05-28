#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>

typedef void (*ProgressCallback)(void*, uint64_t);

struct SavedSegment {
    curl_off_t start;
    curl_off_t end;
    curl_off_t current;
};

struct CurlHandleGuard {
    CURL* curl;
    CurlHandleGuard() { curl = curl_easy_init(); }
    ~CurlHandleGuard() { if (curl) curl_easy_cleanup(curl); }
};

struct FileDescriptorGuard {
    int fd;
    FileDescriptorGuard(int f) : fd(f) {}
    ~FileDescriptorGuard() { if (fd >= 0) close(fd); }
};

struct ThreadTask {
    std::string url;
    std::string path;
    curl_off_t start;
    curl_off_t end;
    std::atomic<curl_off_t>* current;
    ProgressCallback callback;
    void* user_data;
    bool success;
    std::string state_path;
    const std::vector<ThreadTask>* all_tasks;
    curl_off_t total_size;
    int fd;
    curl_off_t max_speed;
    const char* user_agent;
};

static std::mutex state_mutex;

static void save_state(const std::string& state_path, curl_off_t total_size, const std::vector<ThreadTask>& tasks) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::ofstream out(state_path);
    if (!out) {
        return;
    }
    out << total_size << "\n";
    for (const auto& task : tasks) {
        out << task.start << "," << task.end << "," << (task.current ? task.current->load() : task.start) << "\n";
    }
}

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_bytes = size * nmemb;
    ThreadTask* task = static_cast<ThreadTask*>(userp);
    curl_off_t curr = task->current ? task->current->load() : 0;
    ssize_t written = pwrite(task->fd, contents, total_bytes, curr);
    if (written < 0) {
        return 0;
    }
    if (task->current) {
        task->current->fetch_add(written);
    }
    if (task->callback) {
        task->callback(task->user_data, written);
    }
    if (task->all_tasks && !task->state_path.empty()) {
        save_state(task->state_path, task->total_size, *task->all_tasks);
    }
    return written;
}

struct HeaderData {
    bool accept_ranges;
};

static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    HeaderData* hd = static_cast<HeaderData*>(userdata);
    std::string header(buffer, total);
    std::transform(header.begin(), header.end(), header.begin(), ::tolower);
    if (header.find("accept-ranges: bytes") != std::string::npos) {
        hd->accept_ranges = true;
    }
    return total;
}

static void download_range(ThreadTask* task) {
    task->success = false;
    curl_off_t curr = task->current ? task->current->load() : task->start;
    if (curr > task->end) {
        task->success = true;
        return;
    }
    CurlHandleGuard curl_guard;
    if (!curl_guard.curl) {
        return;
    }
    int raw_fd = open(task->path.c_str(), O_WRONLY);
    if (raw_fd < 0) {
        return;
    }
    FileDescriptorGuard fd_guard(raw_fd);
    task->fd = fd_guard.fd;
    std::string range_header = std::to_string(curr) + "-" + std::to_string(task->end);
    curl_easy_setopt(curl_guard.curl, CURLOPT_URL, task->url.c_str());
    curl_easy_setopt(curl_guard.curl, CURLOPT_RANGE, range_header.c_str());
    curl_easy_setopt(curl_guard.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_guard.curl, CURLOPT_WRITEDATA, task);
    curl_easy_setopt(curl_guard.curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_guard.curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_guard.curl, CURLOPT_CONNECTTIMEOUT, 30L);
    if (task->max_speed > 0) {
        curl_easy_setopt(curl_guard.curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)task->max_speed);
    }
    if (task->user_agent && *task->user_agent) {
        curl_easy_setopt(curl_guard.curl, CURLOPT_USERAGENT, task->user_agent);
    }
    CURLcode res = curl_easy_perform(curl_guard.curl);
    if (res == CURLE_OK) {
        task->success = true;
    }
}

extern "C" {
    int download_http_cpp(
        const char* url,
        const char* output_path,
        int connections,
        bool quiet,
        ProgressCallback callback,
        void* user_data,
        curl_off_t max_speed,
        const char* user_agent,
        bool resume
    ) {
        (void)quiet;
        static std::once_flag curl_init_flag;
        std::call_once(curl_init_flag, []() { curl_global_init(CURL_GLOBAL_ALL); });
        CurlHandleGuard head_curl;
        if (!head_curl.curl) {
            return -1;
        }
        HeaderData hd = { false };
        curl_easy_setopt(head_curl.curl, CURLOPT_URL, url);
        curl_easy_setopt(head_curl.curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(head_curl.curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(head_curl.curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(head_curl.curl, CURLOPT_HEADERDATA, &hd);
        if (user_agent && *user_agent) {
            curl_easy_setopt(head_curl.curl, CURLOPT_USERAGENT, user_agent);
        }
        CURLcode res = curl_easy_perform(head_curl.curl);
        if (res != CURLE_OK) {
            return -2;
        }
        curl_off_t total_size = -1;
        curl_easy_getinfo(head_curl.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total_size);
        if (total_size <= 0) {
            connections = 1;
        }
        std::string state_path = std::string(output_path) + ".rux";
        bool state_loaded = false;
        std::vector<SavedSegment> segments;
        curl_off_t saved_total_size = -1;
        struct stat st;
        bool file_exists = (stat(output_path, &st) == 0);
        if (resume && file_exists) {
            std::ifstream in(state_path);
            if (in) {
                if (in >> saved_total_size && saved_total_size == total_size) {
                    SavedSegment seg;
                    char comma1, comma2;
                    while (in >> seg.start >> comma1 >> seg.end >> comma2 >> seg.current && comma1 == ',' && comma2 == ',') {
                        segments.push_back(seg);
                    }
                    if (segments.size() == static_cast<size_t>(connections)) {
                        state_loaded = true;
                    }
                }
            }
        }
        if (connections <= 1 || !hd.accept_ranges || total_size <= 0) {
            CurlHandleGuard single_curl;
            if (!single_curl.curl) {
                return -5;
            }
            curl_off_t resume_from = 0;
            int flags = O_WRONLY | O_CREAT;
            if (resume && file_exists) {
                resume_from = st.st_size;
                flags = O_WRONLY | O_CREAT | O_APPEND;
            } else {
                flags = O_WRONLY | O_CREAT | O_TRUNC;
            }
            int raw_sfd = open(output_path, flags, 0666);
            if (raw_sfd < 0) {
                return -6;
            }
            FileDescriptorGuard sfd_guard(raw_sfd);
            std::atomic<curl_off_t> single_progress(resume_from);
            ThreadTask task;
            task.url = url;
            task.path = output_path;
            task.start = 0;
            task.end = total_size - 1;
            task.current = &single_progress;
            task.callback = callback;
            task.user_data = user_data;
            task.success = false;
            task.state_path = "";
            task.all_tasks = nullptr;
            task.total_size = total_size;
            task.fd = sfd_guard.fd;
            task.max_speed = max_speed;
            task.user_agent = user_agent;
            curl_easy_setopt(single_curl.curl, CURLOPT_URL, url);
            curl_easy_setopt(single_curl.curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(single_curl.curl, CURLOPT_WRITEDATA, &task);
            curl_easy_setopt(single_curl.curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(single_curl.curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(single_curl.curl, CURLOPT_CONNECTTIMEOUT, 30L);
            if (resume_from > 0) {
                curl_easy_setopt(single_curl.curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
            }
            if (max_speed > 0) {
                curl_easy_setopt(single_curl.curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)max_speed);
            }
            if (user_agent && *user_agent) {
                curl_easy_setopt(single_curl.curl, CURLOPT_USERAGENT, user_agent);
            }
            CURLcode sres = curl_easy_perform(single_curl.curl);
            return (sres == CURLE_OK) ? 0 : -7;
        }
        if (!state_loaded) {
            int raw_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (raw_fd < 0) {
                return -3;
            }
            FileDescriptorGuard fd_guard(raw_fd);
            if (ftruncate(fd_guard.fd, total_size) != 0) {
                return -4;
            }
        }
        std::vector<std::atomic<curl_off_t>> progress_counters(connections);
        for (int i = 0; i < connections; ++i) {
            if (state_loaded) {
                progress_counters[i].store(segments[i].current);
            } else {
                progress_counters[i].store(i * (total_size / connections));
            }
        }
        curl_off_t chunk_size = total_size / connections;
        std::vector<ThreadTask> tasks(connections);
        for (int i = 0; i < connections; ++i) {
            tasks[i].url = url;
            tasks[i].path = output_path;
            if (state_loaded) {
                tasks[i].start = segments[i].start;
                tasks[i].end = segments[i].end;
            } else {
                tasks[i].start = i * chunk_size;
                tasks[i].end = (i == connections - 1) ? (total_size - 1) : ((i + 1) * chunk_size - 1);
            }
            tasks[i].current = &progress_counters[i];
            tasks[i].callback = callback;
            tasks[i].user_data = user_data;
            tasks[i].success = false;
            tasks[i].state_path = state_path;
            tasks[i].all_tasks = &tasks;
            tasks[i].total_size = total_size;
            tasks[i].max_speed = (max_speed > 0) ? (max_speed / connections) : 0;
            tasks[i].user_agent = user_agent;
        }
        if (!state_loaded) {
            save_state(state_path, total_size, tasks);
        }
        std::vector<std::thread> threads;
        for (int i = 0; i < connections; ++i) {
            threads.emplace_back(download_range, &tasks[i]);
        }
        bool all_success = true;
        for (int i = 0; i < connections; ++i) {
            if (threads[i].joinable()) {
                threads[i].join();
            }
            if (!tasks[i].success) {
                all_success = false;
            }
        }
        if (all_success) {
            std::remove(state_path.c_str());
            return 0;
        }
        return -8;
    }
}
