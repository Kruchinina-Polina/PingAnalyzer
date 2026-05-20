#define _CRT_SECURE_NO_WARNINGS   
#include <stdio.h>                
#include <stdlib.h>               
#include <string.h>              
#include <time.h>                
#include <locale.h>               
#include <windows.h>              

#define MAX_IP_LEN 256            
#define MAX_LINE_LEN 512          
#define RESULT_FILE "ping_results.txt"
#define RESOURCE_FILE "resource_stats.txt"
#define ERROR_LOG "wmi_errors.log"
#define DEFAULT_INPUT_FILE "hosts.txt"
#define CONFIG_FILE "config.txt"
#define DEFAULT_PING_INTERVAL_SEC 60
#define DEFAULT_RESOURCE_INTERVAL_SEC 60

struct HostInfo {
    char host[MAX_IP_LEN];
    char login[128];
    char password[128];
};

struct PingThreadData {
    char host[MAX_IP_LEN];
    int index;
};

struct ResourceThreadData {
    struct HostInfo hostInfo;
    int index;
    char result[2048];
};

int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

void trim(char* str) {
    char* start = str;
    char* end;
    while (*start == ' ' || *start == '\t') start++;
    if (*start == 0) { str[0] = '\0'; return; }
    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = '\0';
    if (start != str) memmove(str, start, strlen(start) + 1);
}

int is_ignored_line(const char* line) {
    return (line[0] == '#' || strlen(line) == 0);
}

void read_intervals(int* ping_sec, int* resource_sec) {
    FILE* cfg;
    char line[MAX_LINE_LEN];
    char key[64];
    int val;
    *ping_sec = DEFAULT_PING_INTERVAL_SEC;
    *resource_sec = DEFAULT_RESOURCE_INTERVAL_SEC;
    cfg = fopen(CONFIG_FILE, "r");
    if (!cfg) return;
    while (fgets(line, sizeof(line), cfg)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (is_ignored_line(line)) continue;
        if (sscanf(line, "%s %d", key, &val) == 2 && val > 0) {
            if (strcmp(key, "ping_interval") == 0) *ping_sec = val;
            else if (strcmp(key, "resource_interval") == 0) *resource_sec = val;
        }
    }
    fclose(cfg);
}

int simple_ping(const char* host) {
    char command[512];
    sprintf(command, "ping -n 1 -w 1000 %s > nul 2>&1", host);
    return system(command);
}

DWORD WINAPI ping_worker(LPVOID arg) {
    struct PingThreadData* data = (struct PingThreadData*)arg;
    int res = simple_ping(data->host);
    printf("%s[%d] %s - %s\033[0m\n",
        res == 0 ? "\033[32m" : "\033[31m",
        data->index, data->host, res == 0 ? "AVAILABLE" : "UNAVAILABLE");
    free(data);
    return res;
}

void check_ping_parallel(struct HostInfo* hosts, int count, int iteration) {
    HANDLE* threads;
    int* results;
    int i;
    FILE* f;
    time_t now;
    struct tm* t;
    int fail;
    char msg[512];

    printf("\033[36m[PING] Итерация #%d\033[0m\n", iteration);
    threads = (HANDLE*)malloc(count * sizeof(HANDLE));
    results = (int*)calloc(count, sizeof(int));

    for (i = 0; i < count; i++) {
        struct PingThreadData* data = (struct PingThreadData*)malloc(sizeof(struct PingThreadData));
        strcpy(data->host, hosts[i].host);
        data->index = i + 1;
        threads[i] = CreateThread(NULL, 0, ping_worker, data, 0, NULL);
    }

    WaitForMultipleObjects(count, threads, TRUE, INFINITE);

    for (i = 0; i < count; i++) {
        DWORD exit_code;
        GetExitCodeThread(threads[i], &exit_code);
        results[i] = (int)exit_code;
        CloseHandle(threads[i]);
    }

    f = fopen(RESULT_FILE, "a");
    if (f) {
        now = time(NULL);
        t = localtime(&now);
        fprintf(f, "\n========================================\n");
        fprintf(f, "PING итерация #%d: %02d.%02d.%04d %02d:%02d:%02d\n",
            iteration, t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
            t->tm_hour, t->tm_min, t->tm_sec);
        fprintf(f, "========================================\n");
        for (i = 0; i < count; i++) {
            fprintf(f, "[%02d.%02d.%04d %02d:%02d:%02d] %s - %s\n",
                t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
                t->tm_hour, t->tm_min, t->tm_sec, hosts[i].host,
                results[i] == 0 ? "AVAILABLE" : "UNAVAILABLE");
        }
        fclose(f);
    }

    fail = 0;
    for (i = 0; i < count; i++) if (results[i] != 0) fail++;
    if (fail > 0) {
        sprintf(msg, "Итерация #%d: %d хостов недоступны.", iteration, fail);
        MessageBoxA(NULL, msg, "Мониторинг хостов", MB_OK | MB_ICONWARNING);
    }

    free(threads);
    free(results);
}

int is_private_ip(const char* host) {
    unsigned int a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 1;
    if (a == 192 && b == 168) return 1;
    if (a == 127) return 1;
    return 0;
}

int run_wmic_csv(int is_local, const char* host, const char* login, const char* password,
    const char* wql, char* out_value, unsigned int out_size)
{
    char cmd[2048];
    char auth[256];
    FILE* pipe;
    char line[1024];
    int found;
    char* first_comma, * value, * end;
    time_t now;
    struct tm* t;
    FILE* err;

    auth[0] = '\0';
    if (is_local) {
        sprintf(cmd, "wmic %s /format:csv 2>&1", wql);
    }
    else {
        if (login && login[0] && password && password[0]) {
            sprintf(auth, "/user:\"%s\" /password:\"%s\" ", login, password);
        }
        sprintf(cmd, "wmic /node:\"%s\" /timeout:5000 %s %s /format:csv 2>&1", host, auth, wql);
    }

    pipe = _popen(cmd, "r");
    if (!pipe) return -1;

    found = 0;
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "Node", 4) == 0) continue;
        if (strlen(line) == 0) continue;
        first_comma = strchr(line, ',');
        if (!first_comma) continue;
        value = first_comma + 1;
        if (*value == '"') value++;
        end = value + strlen(value) - 1;
        if (*end == '"') *end = '\0';
        strncpy(out_value, value, out_size - 1);
        out_value[out_size - 1] = '\0';
        trim(out_value);
        found = 1;
        break;
    }
    _pclose(pipe);

    if (!found) {
        err = fopen(ERROR_LOG, "a");
        if (err) {
            now = time(NULL);
            t = localtime(&now);
            fprintf(err, "[%02d.%02d.%04d %02d:%02d:%02d] Команда: %s\n",
                t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
                t->tm_hour, t->tm_min, t->tm_sec, cmd);
            fclose(err);
        }
        return -1;
    }
    return 0;
}

int get_cpu_usage(int is_local, const char* host, const char* login, const char* password, int* cpu_percent) {
    char val[32];
    if (run_wmic_csv(is_local, host, login, password, "cpu get loadpercentage", val, sizeof(val)) == 0) {
        if (strlen(val) > 0 && is_digit(val[0])) {
            *cpu_percent = atoi(val);
            return 0;
        }
    }
    return -1;
}

int get_ram_usage(int is_local, const char* host, const char* login, const char* password,
    int* used_percent, unsigned long long* used_mb, unsigned long long* total_mb) {
    char combined[128];
    char* comma;
    char* free_str;
    char* total_str;
    unsigned long long free_kb, total_kb;

    if (run_wmic_csv(is_local, host, login, password,
        "OS get FreePhysicalMemory,TotalVisibleMemorySize", combined, sizeof(combined)) == 0) {
        comma = strchr(combined, ',');
        if (!comma) return -1;
        *comma = '\0';
        free_str = combined;
        total_str = comma + 1;
        trim(free_str);
        trim(total_str);
        free_kb = strtoull(free_str, NULL, 10);
        total_kb = strtoull(total_str, NULL, 10);
        if (total_kb == 0) return -1;
        *total_mb = total_kb / 1024;
        *used_mb = (total_kb - free_kb) / 1024;
        *used_percent = (int)((*used_mb * 100) / (*total_mb));
        return 0;
    }
    return -1;
}

int get_disk_usage(int is_local, const char* host, const char* login, const char* password,
    int* used_percent, double* used_gb, double* total_gb) {
    char combined[128];
    char* comma;
    char* free_str;
    char* total_str;
    unsigned long long free_bytes, total_bytes;

    if (run_wmic_csv(is_local, host, login, password,
        "logicaldisk where \"DeviceID='C:'\" get FreeSpace,Size", combined, sizeof(combined)) == 0) {
        comma = strchr(combined, ',');
        if (!comma) return -1;
        *comma = '\0';
        free_str = combined;
        total_str = comma + 1;
        trim(free_str);
        trim(total_str);
        free_bytes = strtoull(free_str, NULL, 10);
        total_bytes = strtoull(total_str, NULL, 10);
        if (total_bytes == 0) return -1;
        *total_gb = total_bytes / (1024.0 * 1024 * 1024);
        *used_gb = (total_bytes - free_bytes) / (1024.0 * 1024 * 1024);
        *used_percent = (int)((*used_gb * 100) / (*total_gb));
        return 0;
    }
    return -1;
}

void get_remote_resource_stats(const char* host, const char* login, const char* password,
    char* out_buf, unsigned int buf_size)
{
    char ping_cmd[512];
    int is_local;
    int cpu_test;
    char tmp[256];
    int has_error;
    int cpu;
    int ram_percent;
    unsigned long long used_mb, total_mb;
    int disk_percent;
    double used_gb, total_gb;
    FILE* err_log;
    time_t now;
    struct tm* t;

    out_buf[0] = '\0';

    if (!is_private_ip(host)) {
        sprintf(out_buf, "PUBLIC_HOST (ресурсы не собираются)");
        return;
    }

    sprintf(ping_cmd, "ping -n 1 -w 1000 %s > nul 2>&1", host);
    if (system(ping_cmd) != 0) {
        sprintf(out_buf, "HOST UNREACHABLE");
        return;
    }

    is_local = (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0);
    if (!is_local) {
        cpu_test = -1;
        if (get_cpu_usage(1, host, login, password, &cpu_test) == 0 && cpu_test >= 0) {
            is_local = 1;
        }
    }

    has_error = 0;
    cpu = -1;
    if (get_cpu_usage(is_local, host, login, password, &cpu) == 0 && cpu >= 0) {
        sprintf(tmp, "CPU:%d%% ", cpu);
        strncat(out_buf, tmp, buf_size - strlen(out_buf) - 1);
    }
    else {
        strncat(out_buf, "CPU:ERROR ", buf_size - strlen(out_buf) - 1);
        has_error = 1;
    }

    ram_percent = -1;
    used_mb = 0;
    total_mb = 0;
    if (get_ram_usage(is_local, host, login, password, &ram_percent, &used_mb, &total_mb) == 0 && ram_percent >= 0) {
        sprintf(tmp, "RAM:%d%% (%llu/%lluMB) ", ram_percent, used_mb, total_mb);
        strncat(out_buf, tmp, buf_size - strlen(out_buf) - 1);
    }
    else {
        strncat(out_buf, "RAM:ERROR ", buf_size - strlen(out_buf) - 1);
        has_error = 1;
    }

    disk_percent = -1;
    used_gb = 0;
    total_gb = 0;
    if (get_disk_usage(is_local, host, login, password, &disk_percent, &used_gb, &total_gb) == 0 && disk_percent >= 0) {
        sprintf(tmp, "DISK_C:%d%% (%.1f/%.1fGB)", disk_percent, used_gb, total_gb);
        strncat(out_buf, tmp, buf_size - strlen(out_buf) - 1);
    }
    else {
        strncat(out_buf, "DISK_C:ERROR", buf_size - strlen(out_buf) - 1);
        has_error = 1;
    }

    if (has_error && strlen(out_buf) < 50) {
        err_log = fopen(ERROR_LOG, "a");
        if (err_log) {
            now = time(NULL);
            t = localtime(&now);
            fprintf(err_log, "[%02d.%02d.%04d %02d:%02d:%02d] %s - %s\n",
                t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
                t->tm_hour, t->tm_min, t->tm_sec, host, out_buf);
            fclose(err_log);
        }
    }

    if (strlen(out_buf) == 0) strcpy(out_buf, "UNKNOWN");
}

int parse_disk_usage(const char* stats) {
    const char* disk_tag = "DISK_C:";
    char* p = strstr(stats, disk_tag);
    if (!p) return -1;
    p += strlen(disk_tag);
    while (*p && !is_digit(*p)) p++;
    if (!*p) return -1;
    return atoi(p);
}

DWORD WINAPI resource_worker(LPVOID arg) {
    struct ResourceThreadData* data = (struct ResourceThreadData*)arg;
    get_remote_resource_stats(data->hostInfo.host, data->hostInfo.login, data->hostInfo.password,
        data->result, sizeof(data->result));
    return 0;
}

void check_resources_parallel(struct HostInfo* hosts, int count, int iteration) {
    struct ResourceThreadData* threadData;
    HANDLE* threads;
    int i;
    FILE* f;
    time_t now;
    struct tm* t;
    char** overloaded_hosts;
    int* overloaded_percents;
    int overload_count;
    char msg[2048];
    char line[256];
    int disk_percent;

    printf("\033[35m[RESOURCE] Итерация #%d (параллельный сбор)\033[0m\n", iteration);

    threadData = (struct ResourceThreadData*)malloc(count * sizeof(struct ResourceThreadData));
    threads = (HANDLE*)malloc(count * sizeof(HANDLE));

    for (i = 0; i < count; i++) {
        threadData[i].hostInfo = hosts[i];
        threadData[i].index = i;
        threadData[i].result[0] = '\0';
        threads[i] = CreateThread(NULL, 0, resource_worker, &threadData[i], 0, NULL);
        if (threads[i] == NULL) {
            printf("Ошибка создания потока для %s\n", hosts[i].host);
            strcpy(threadData[i].result, "THREAD ERROR");
        }
    }

    WaitForMultipleObjects(count, threads, TRUE, INFINITE);

    for (i = 0; i < count; i++) {
        if (threads[i] != NULL) CloseHandle(threads[i]);
    }

    f = fopen(RESOURCE_FILE, "a");
    if (!f) {
        printf("Ошибка открытия %s\n", RESOURCE_FILE);
        free(threadData);
        free(threads);
        return;
    }

    now = time(NULL);
    t = localtime(&now);
    fprintf(f, "\n========================================\n");
    fprintf(f, "Сбор ресурсов: %02d.%02d.%04d %02d:%02d:%02d\n",
        t->tm_mday, t->tm_mon + 1, t->tm_year + 1900, t->tm_hour, t->tm_min, t->tm_sec);
    fprintf(f, "========================================\n");

    overloaded_hosts = NULL;
    overloaded_percents = NULL;
    overload_count = 0;

    for (i = 0; i < count; i++) {
        fprintf(f, "[%s] %s\n", hosts[i].host, threadData[i].result);
        printf("\033[35m  %s -> %s\033[0m\n", hosts[i].host, threadData[i].result);

        disk_percent = parse_disk_usage(threadData[i].result);
        if (disk_percent > 80) {
            overloaded_hosts = (char**)realloc(overloaded_hosts, (overload_count + 1) * sizeof(char*));
            overloaded_percents = (int*)realloc(overloaded_percents, (overload_count + 1) * sizeof(int));
            overloaded_hosts[overload_count] = (char*)malloc(strlen(hosts[i].host) + 1);
            strcpy(overloaded_hosts[overload_count], hosts[i].host);
            overloaded_percents[overload_count] = disk_percent;
            overload_count++;
        }
    }
    fclose(f);

    if (overload_count > 0) {
        sprintf(msg, "В итерации #%d обнаружены хосты с загрузкой диска C: > 80%%:\n\n", iteration);
        for (i = 0; i < overload_count && strlen(msg) < sizeof(msg) - 100; i++) {
            sprintf(line, "  %s — %d%%\n", overloaded_hosts[i], overloaded_percents[i]);
            strncat(msg, line, sizeof(msg) - strlen(msg) - 1);
        }
        MessageBoxA(NULL, msg, "Критическая загрузка диска", MB_OK | MB_ICONWARNING);
    }

    for (i = 0; i < overload_count; i++) free(overloaded_hosts[i]);
    free(overloaded_hosts);
    free(overloaded_percents);
    free(threadData);
    free(threads);
}

// ==================== НОВЫЕ ФУНКЦИИ ШАГА 12 ====================
struct HostInfo* read_hosts(const char* filename, int* count) {
    FILE* f;
    struct HostInfo* hosts;
    char line[MAX_LINE_LEN];
    char host[MAX_IP_LEN];
    char login[128];
    char password[128];
    int n;

    *count = 0;
    f = fopen(filename, "r");
    if (!f) return NULL;
    hosts = NULL;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (is_ignored_line(line)) continue;

        host[0] = login[0] = password[0] = '\0';
        n = sscanf(line, "%255s %127s %127s", host, login, password);
        if (n < 1) continue;

        hosts = (struct HostInfo*)realloc(hosts, (*count + 1) * sizeof(struct HostInfo));
        strcpy(hosts[*count].host, host);
        if (n >= 2) strcpy(hosts[*count].login, login);
        else hosts[*count].login[0] = '\0';
        if (n >= 3) strcpy(hosts[*count].password, password);
        else hosts[*count].password[0] = '\0';
        (*count)++;
    }
    fclose(f);
    return hosts;
}

void free_hosts(struct HostInfo* hosts, int count) {
    (void)count; // подавляем предупреждение о неиспользуемом параметре
    free(hosts);
}
// ===============================================================

int main() {
    int host_count;
    struct HostInfo* hosts = read_hosts("hosts.txt", &host_count);
    if (hosts && host_count > 0) {
        printf("Прочитано хостов: %d\n", host_count);
        for (int i = 0; i < host_count; i++) {
            printf("  %s\n", hosts[i].host);
        }
        free_hosts(hosts, host_count);
    }
    else {
        printf("Не удалось прочитать hosts.txt\n");
    }
    return 0;
}