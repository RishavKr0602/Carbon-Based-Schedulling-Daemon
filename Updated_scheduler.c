#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <microhttpd.h>

#define LOG_FILE "/tmp/scheduler.log"
#define PID_FILE "/var/run/green_scheduler.pid"
#define CARBON_API_URL "https://api.carbonintensity.org.uk/intensity"
#define HTTP_PORT 8080
#define POLL_INTERVAL 300
#define MAX_TASKS_INCREMENT 32

typedef struct Task {
    char *command;
    char *urgency;
    int deadline_hours;
    time_t submitted_at;
    time_t deadline;
    pid_t pid;
    int started;
    int delayed;
} Task;

struct MemoryStruct { char *memory; size_t size; };

static Task *tasks = NULL;
static int task_count = 0;
static int task_capacity = 0;
static pthread_mutex_t tasks_lock = PTHREAD_MUTEX_INITIALIZER;

static int completed_tasks = 0;
static double total_delay_seconds = 0.0;

static FILE *logfp_global = NULL;
static int running_foreground = 0;
static volatile sig_atomic_t exit_requested = 0;

static void timestamp_log(FILE *logfp) {
    time_t now = time(NULL);
    char timebuf[64];
    struct tm t;
    localtime_r(&now, &t);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &t);
    fprintf(logfp, "[%s] ", timebuf);
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

static char* fetch_carbon_index_with_curl() {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.size = 0;
    curl_easy_setopt(curl, CURLOPT_URL, CARBON_API_URL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (logfp_global) { timestamp_log(logfp_global); fprintf(logfp_global, "[ERROR] Carbon API request failed: %s\n", curl_easy_strerror(res)); fflush(logfp_global); }
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return NULL;
    }
    struct json_object *root = json_tokener_parse(chunk.memory);
    free(chunk.memory);
    if (!root) { curl_easy_cleanup(curl); return NULL; }
    struct json_object *data_array = NULL;
    if (!json_object_object_get_ex(root, "data", &data_array)) { json_object_put(root); curl_easy_cleanup(curl); return NULL; }
    struct json_object *first_entry = json_object_array_get_idx(data_array, 0);
    if (!first_entry) { json_object_put(root); curl_easy_cleanup(curl); return NULL; }
    struct json_object *intensity_obj = NULL;
    if (!json_object_object_get_ex(first_entry, "intensity", &intensity_obj)) { json_object_put(root); curl_easy_cleanup(curl); return NULL; }
    struct json_object *index_obj = NULL, *forecast_obj = NULL;
    json_object_object_get_ex(intensity_obj, "index", &index_obj);
    json_object_object_get_ex(intensity_obj, "forecast", &forecast_obj);
    const char *index = index_obj ? json_object_get_string(index_obj) : NULL;
    int forecast = forecast_obj ? json_object_get_int(forecast_obj) : -1;
    if (logfp_global) { timestamp_log(logfp_global); fprintf(logfp_global, "[INFO] Carbon Intensity Level: %s | Forecast: %d gCO2/kWh\n", index ? index : "unknown", forecast); fflush(logfp_global); }
    char *result = index ? strdup(index) : NULL;
    json_object_put(root);
    curl_easy_cleanup(curl);
    return result;
}

static void tasks_ensure_capacity(int need) {
    if (task_capacity >= need) return;
    int newcap = (task_capacity > 0) ? task_capacity * 2 : MAX_TASKS_INCREMENT;
    while (newcap < need) newcap *= 2;
    tasks = realloc(tasks, sizeof(Task) * newcap);
    for (int i = task_capacity; i < newcap; ++i) {
        tasks[i].command = NULL; tasks[i].urgency = NULL; tasks[i].started = 0; tasks[i].delayed = 0; tasks[i].pid = 0;
    }
    task_capacity = newcap;
}

static int tasks_append(Task t) { tasks_ensure_capacity(task_count + 1); tasks[task_count] = t; return task_count++; }

static int tasks_contains(const char *command, time_t submitted_at) {
    for (int i = 0; i < task_count; ++i)
        if (tasks[i].command && strcmp(tasks[i].command, command) == 0 && tasks[i].submitted_at == submitted_at) return 1;
    return 0;
}

static void run_task(Task *task) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (task->urgency && strcmp(task->urgency, "low") == 0) nice(10);
        char *cmdcopy = strdup(task->command);
        char *args[64];
        int idx = 0;
        char *tok = strtok(cmdcopy, " ");
        while (tok && idx < 63) { args[idx++] = tok; tok = strtok(NULL, " "); }
        args[idx] = NULL;
        execvp(args[0], args);
        _exit(127);
    } else {
        task->pid = pid;
        task->started = 1;
        timestamp_log(logfp_global);
        fprintf(logfp_global, "[TASK] Launched: %s | PID: %d | Delayed: %s\n", task->command, pid, task->delayed ? "yes" : "no");
        fflush(logfp_global);
    }
}

static int urgency_rank(const char *u) {
    if (!u) return 2;
    if (strcmp(u, "high") == 0) return 0;
    if (strcmp(u, "medium") == 0) return 1;
    return 2;
}

struct http_cb_ctx { char *data; size_t size; };

static int add_tasks_post_reader(void *coninfo_cls, enum MHD_ValueKind kind, const char *key,
                                const char *filename, const char *content_type,
                                const char *transfer_encoding, const char *data, uint64_t off, size_t size) {
    struct http_cb_ctx *ctx = (struct http_cb_ctx *)coninfo_cls;
    if (!ctx->data) {
        ctx->data = malloc(size + 1);
        memcpy(ctx->data, data, size);
        ctx->size = size;
        ctx->data[ctx->size] = 0;
    } else {
        ctx->data = realloc(ctx->data, ctx->size + size + 1);
        memcpy(ctx->data + ctx->size, data, size);
        ctx->size += size;
        ctx->data[ctx->size] = 0;
    }
    return MHD_YES;
}

static int http_request_handler(void *cls, struct MHD_Connection *connection,
                                const char *url, const char *method, const char *version,
                                const char *upload_data, size_t *upload_data_size, void **con_cls) {
    if (*con_cls == NULL) {
        struct http_cb_ctx *ctx = malloc(sizeof(struct http_cb_ctx));
        ctx->data = NULL; ctx->size = 0;
        *con_cls = ctx;
        return MHD_YES;
    }
    struct http_cb_ctx *ctx = (struct http_cb_ctx *)*con_cls;
    if (strcmp(method, "POST") == 0 && strcmp(url, "/add_tasks") == 0) {
        if (*upload_data_size != 0) {
            add_tasks_post_reader(ctx, MHD_POSTDATA_KIND, NULL, NULL, NULL, NULL, upload_data, 0, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        } else {
            if (!ctx->data) {
                const char *msg = "No data received";
                struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(msg), (void*)msg, MHD_RESPMEM_PERSISTENT);
                int ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, resp);
                MHD_destroy_response(resp); free(ctx); *con_cls = NULL; return ret;
            }
            struct json_object *root = json_tokener_parse(ctx->data);
            if (!root || !json_object_is_type(root, json_type_array)) {
                const char *msg = "Expected JSON array";
                struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(msg), (void*)msg, MHD_RESPMEM_PERSISTENT);
                int ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, resp);
                MHD_destroy_response(resp);
                if (root) json_object_put(root);
                free(ctx->data); free(ctx); *con_cls = NULL; return ret;
            }
            int n = json_object_array_length(root);
            typedef struct TempTask { char *command; char *urgency; int deadline_hours; time_t submitted_at; int order; } TempTask;
            TempTask *arr = calloc(n, sizeof(TempTask));
            for (int i = 0; i < n; ++i) {
                struct json_object *obj = json_object_array_get_idx(root, i);
                struct json_object *jcmd = NULL, *jurg = NULL, *jdl = NULL, *jsub = NULL;
                json_object_object_get_ex(obj, "command", &jcmd);
                json_object_object_get_ex(obj, "urgency", &jurg);
                json_object_object_get_ex(obj, "deadline_hours", &jdl);
                json_object_object_get_ex(obj, "submitted_at", &jsub);
                const char *cmd = jcmd ? json_object_get_string(jcmd) : NULL;
                const char *urg = jurg ? json_object_get_string(jurg) : "low";
                int dl = jdl ? json_object_get_int(jdl) : 0;
                time_t sub = jsub ? (time_t)json_object_get_int64(jsub) : time(NULL);
                arr[i].command = cmd ? strdup(cmd) : strdup("");
                arr[i].urgency = urg ? strdup(urg) : strdup("low");
                arr[i].deadline_hours = dl;
                arr[i].submitted_at = sub;
                arr[i].order = i;
            }
            for (int i = 1; i < n; ++i) {
                TempTask key = arr[i];
                int j = i - 1;
                while (j >= 0) {
                    int rj = urgency_rank(arr[j].urgency);
                    int ri = urgency_rank(key.urgency);
                    if (rj > ri || (rj == ri && arr[j].order > key.order)) { arr[j+1] = arr[j]; j--; } else break;
                }
                arr[j+1] = key;
            }
            char *index_now = fetch_carbon_index_with_curl();
            pthread_mutex_lock(&tasks_lock);
            for (int i = 0; i < n; ++i) {
                Task t = {0};
                t.command = arr[i].command;
                t.urgency = arr[i].urgency;
                t.deadline_hours = arr[i].deadline_hours;
                t.submitted_at = arr[i].submitted_at;
                t.deadline = t.submitted_at + t.deadline_hours * 3600;
                t.started = 0; t.delayed = 0; t.pid = 0;
                if (!tasks_contains(t.command, t.submitted_at)) {
                    int idx = tasks_append(t);
                    int urgent = (strcmp(t.urgency, "high") == 0);
                    int high_carbon = (index_now && (strcmp(index_now, "high") == 0 || strcmp(index_now, "very high") == 0));
                    if (urgent) run_task(&tasks[idx]);
                    else {
                        if (high_carbon && time(NULL) < tasks[idx].deadline) {
                            tasks[idx].delayed = 1;
                            timestamp_log(logfp_global);
                            fprintf(logfp_global, "[INFO] Received and delayed (high carbon): %s | urgency=%s\n", tasks[idx].command, tasks[idx].urgency);
                            fflush(logfp_global);
                        } else run_task(&tasks[idx]);
                    }
                } else { free(arr[i].command); free(arr[i].urgency); }
            }
            pthread_mutex_unlock(&tasks_lock);
            if (index_now) free(index_now);
            free(arr);
            json_object_put(root);
            free(ctx->data); free(ctx);
            *con_cls = NULL;
            const char *msg = "Tasks accepted";
            struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(msg), (void*)msg, MHD_RESPMEM_PERSISTENT);
            int ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
            MHD_destroy_response(resp);
            return ret;
        }
    }
    const char *msg = "Not Found";
    struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(msg), (void*)msg, MHD_RESPMEM_PERSISTENT);
    int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, resp);
    MHD_destroy_response(resp);
    if (ctx) { if (ctx->data) free(ctx->data); free(ctx); *con_cls = NULL; }
    return ret;
}

static void signal_handler(int sig) { exit_requested = 1; if (logfp_global) { timestamp_log(logfp_global); fprintf(logfp_global, "[INFO] Signal %d received\n", sig); fflush(logfp_global); } }

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) running_foreground = 1;
    logfp_global = fopen(LOG_FILE, "a+");
    if (!logfp_global) return 1;
    if (!running_foreground) {
        pid_t pid = fork(); if (pid < 0) exit(EXIT_FAILURE); if (pid > 0) exit(EXIT_SUCCESS);
        umask(0); if (setsid() < 0) exit(EXIT_FAILURE);
        pid = fork(); if (pid < 0) exit(EXIT_FAILURE); if (pid > 0) exit(EXIT_SUCCESS);
        if (chdir("/") < 0) exit(EXIT_FAILURE);
        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
        FILE *pidf = fopen(PID_FILE, "w"); if (pidf) { fprintf(pidf, "%d\n", getpid()); fclose(pidf); }
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY | MHD_USE_THREAD_PER_CONNECTION,
                                                 HTTP_PORT, NULL, NULL, &http_request_handler, NULL, MHD_OPTION_END);
    if (!daemon) return 1;
    timestamp_log(logfp_global); fprintf(logfp_global, "[INFO] REST scheduler started on port %d\n", HTTP_PORT); fflush(logfp_global);
    while (!exit_requested) {
        char *index = fetch_carbon_index_with_curl();
        pthread_mutex_lock(&tasks_lock);
        time_t now = time(NULL);
        for (int i = 0; i < task_count; ++i) {
            if (tasks[i].started) continue;
            int urgent = (tasks[i].urgency && strcmp(tasks[i].urgency, "high") == 0);
            int high_carbon = (index && (strcmp(index, "high") == 0 || strcmp(index, "very high") == 0));
            if (urgent) run_task(&tasks[i]);
            else {
                if (high_carbon && now < tasks[i].deadline) {
                    tasks[i].delayed = 1;
                    timestamp_log(logfp_global);
                    fprintf(logfp_global, "[INFO] Deferred due to high carbon: %s\n", tasks[i].command);
                    fflush(logfp_global);
                } else run_task(&tasks[i]);
            }
        }
        pthread_mutex_unlock(&tasks_lock);
        int status; pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&tasks_lock);
            for (int i = 0; i < task_count; ++i)
                if (tasks[i].pid == pid) {
                    time_t end = time(NULL);
                    double delay = difftime(end, tasks[i].submitted_at);
                    total_delay_seconds += delay;
                    completed_tasks++;
                    timestamp_log(logfp_global);
                    fprintf(logfp_global, "[TASK] Completed: %s | PID: %d | Delay: %.0f sec\n", tasks[i].command, pid, delay);
                    fflush(logfp_global);
                    break;
                }
            pthread_mutex_unlock(&tasks_lock);
        }
        if (index) free(index);
        for (int s = 0; s < POLL_INTERVAL && !exit_requested; ++s) sleep(1);
    }
    MHD_stop_daemon(daemon);
    timestamp_log(logfp_global);
    fprintf(logfp_global, "[SUMMARY] Completed tasks: %d\n", completed_tasks);
    if (completed_tasks > 0) fprintf(logfp_global, "[SUMMARY] Average delay (sec): %.2f\n", total_delay_seconds / completed_tasks);
    else fprintf(logfp_global, "[SUMMARY] No completed tasks\n");
    fflush(logfp_global);
    curl_global_cleanup();
    fclose(logfp_global);
    pthread_mutex_lock(&tasks_lock);
    for (int i = 0; i < task_count; ++i) { free(tasks[i].command); free(tasks[i].urgency); }
    free(tasks);
    pthread_mutex_unlock(&tasks_lock);
    pthread_mutex_destroy(&tasks_lock);
    return 0;
}
