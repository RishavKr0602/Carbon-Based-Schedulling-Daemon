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

#define CONFIG_FILE "/mnt/storage/osproject/tasks.json" //change this location
#define LOG_FILE "/tmp/scheduler.log"
#define PID_FILE "/var/run/green_scheduler.pid"
#define CARBON_API_URL "https://api.carbonintensity.org.uk/intensity"
#define MAX_TASKS 100

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

struct MemoryStruct {
    char *memory;
    size_t size;
};

Task *tasks = NULL;
int task_count = 0;

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

char* get_intensity_level(CURL *curl, FILE *logfp) {
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
        fprintf(logfp, "Carbon API request failed: %s\n", curl_easy_strerror(res));
        fflush(logfp);
        free(chunk.memory);
        return NULL;
    }
    
    struct json_object *root = json_tokener_parse(chunk.memory);
    free(chunk.memory);
    if (!root) {
        fprintf(logfp, "Failed to parse Carbon API JSON response\n");
        fflush(logfp);
        return NULL;
    }
    
    // Navigate JSON structure: data[0].intensity.index
    struct json_object *data_array;
    if (!json_object_object_get_ex(root, "data", &data_array)) {
        fprintf(logfp, "No 'data' field in Carbon API response\n");
        fflush(logfp);
        json_object_put(root);
        return NULL;
    }
    
    struct json_object *first_entry = json_object_array_get_idx(data_array, 0);
    if (!first_entry) {
        fprintf(logfp, "Empty data array in Carbon API response\n");
        fflush(logfp);
        json_object_put(root);
        return NULL;
    }
    
    struct json_object *intensity_obj;
    if (!json_object_object_get_ex(first_entry, "intensity", &intensity_obj)) {
        fprintf(logfp, "No 'intensity' field in Carbon API response\n");
        fflush(logfp);
        json_object_put(root);
        return NULL;
    }
    
    struct json_object *index_obj, *forecast_obj;
    json_object_object_get_ex(intensity_obj, "index", &index_obj);
    json_object_object_get_ex(intensity_obj, "forecast", &forecast_obj);
    
    const char *index = json_object_get_string(index_obj);
    int forecast = json_object_get_int(forecast_obj);
    
    fprintf(logfp, "Carbon Intensity: %s (forecast: %d gCO2/kWh)\n", 
            index ? index : "unknown", forecast);
    fflush(logfp);
    
    char *result = NULL;
    if (index) {
        result = strdup(index);
    }
    
    json_object_put(root);
    return result;
}

void daemonize() {
    pid_t pid, sid;

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    sid = setsid();
    if (sid < 0) exit(EXIT_FAILURE);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (chdir("/") < 0) exit(EXIT_FAILURE);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    FILE *pidf = fopen(PID_FILE, "w");
    if (pidf) {
        fprintf(pidf, "%d\n", getpid());
        fclose(pidf);
    }
}

void run_task(Task *task, FILE *logfp) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(logfp, "Fork failed for: %s\n", task->command);
        fflush(logfp);
        return;
    }
    if (pid == 0) {
        if (strcmp(task->urgency, "low") == 0) {
            nice(10);
        }
        char *args[64];
        int idx = 0;
        char *cmd_copy = strdup(task->command);
        char *token = strtok(cmd_copy, " ");
        while (token && idx < 63) {
            args[idx++] = token;
            token = strtok(NULL, " ");
        }
        args[idx] = NULL;
        execvp(args[0], args);
        exit(EXIT_FAILURE);
    } else {
        task->pid = pid;
        task->started = 1;
        time_t now = time(NULL);
        fprintf(logfp, "Started: %s | PID: %d | Time: %s%s",
                task->command, pid, ctime(&now), task->delayed ? " (delayed)\n" : "");
        fflush(logfp);
    }
}

void load_tasks(FILE *logfp) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        fprintf(logfp, "Could not open config file: %s\n", CONFIG_FILE);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *json_str = malloc(fsize + 1);
    fread(json_str, 1, fsize, fp);
    json_str[fsize] = 0;
    fclose(fp);

    struct json_object *root = json_tokener_parse(json_str);
    free(json_str);
    if (!root) return;

    task_count = json_object_array_length(root);
    if (tasks) {
        for (int i = 0; i < task_count; i++) {
            free(tasks[i].command);
            free(tasks[i].urgency);
        }
        free(tasks);
    }
    tasks = calloc(task_count, sizeof(Task));
    for (int i = 0; i < task_count; i++) {
        struct json_object *obj = json_object_array_get_idx(root, i);
        struct json_object *jcmd, *jurg, *jdl, *jsub;
        json_object_object_get_ex(obj, "command", &jcmd);
        json_object_object_get_ex(obj, "urgency", &jurg);
        json_object_object_get_ex(obj, "deadline_hours", &jdl);
        json_object_object_get_ex(obj, "submitted_at", &jsub);
        tasks[i].command = strdup(json_object_get_string(jcmd));
        tasks[i].urgency = strdup(json_object_get_string(jurg));
        tasks[i].deadline_hours = json_object_get_int(jdl);
        tasks[i].submitted_at = json_object_get_int64(jsub);
        tasks[i].deadline = tasks[i].submitted_at + tasks[i].deadline_hours * 3600;
        tasks[i].started = 0;
        tasks[i].delayed = 0;
    }
    json_object_put(root);
    fprintf(logfp, "Reloaded %d tasks from config\n", task_count);
    fflush(logfp);
}

int main(int argc, char *argv[]) {
    FILE *logfp = fopen(LOG_FILE, "a+");
    if (!logfp) return 1;
    
    // Only daemonize if not in foreground mode
    int foreground = 0;
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        foreground = 1;
        printf("Running in foreground mode...\n");
    } else {
        daemonize();
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) return 1;
    load_tasks(logfp);

    while (1) {
        load_tasks(logfp);
        char *intensity = get_intensity_level(curl, logfp);
        time_t now = time(NULL);
        
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].started) continue;
            
            if (strcmp(tasks[i].urgency, "high") == 0) {
                // High urgency tasks always run immediately
                run_task(&tasks[i], logfp);
            } else {
                // Check if carbon intensity is high or very high
                if (intensity && 
                    (strcmp(intensity, "high") == 0 || strcmp(intensity, "very high") == 0) && 
                    now < tasks[i].deadline) {
                    // Delay task if intensity is high and deadline not passed
                    tasks[i].delayed = 1;
                    fprintf(logfp, "Delaying task due to high carbon intensity: %s\n", 
                            tasks[i].command);
                    fflush(logfp);
                } else {
                    // Run task if intensity is low/moderate or deadline is approaching
                    run_task(&tasks[i], logfp);
                }
            }
        }
        
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < task_count; i++) {
                if (tasks[i].pid == pid) {
                    time_t end = time(NULL);
                    fprintf(logfp, "Completed: %s | PID: %d | Time: %s",
                            tasks[i].command, pid, ctime(&end));
                    fflush(logfp);
                }
            }
        }
        
        free(intensity);
        sleep(300); // Check every 5 minutes
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    fclose(logfp);
    return 0;
}
