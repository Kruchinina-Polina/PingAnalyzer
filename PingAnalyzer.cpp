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

// ---------- Определение частного IP ----------
int is_private_ip(const char* host) {
    unsigned int a, b, c, d;
    // Пытаемся распарсить IPv4 адрес
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    // Диапазоны частных адресов:
    if (a == 10) return 1;                         // 10.0.0.0/8
    if (a == 172 && b >= 16 && b <= 31) return 1;  // 172.16.0.0/12
    if (a == 192 && b == 168) return 1;            // 192.168.0.0/16
    if (a == 127) return 1;                        // 127.0.0.0/8 (localhost)
    return 0;
}
// -----------------------------------------

// Заглушки для остальных функций (ресурсная часть)
int run_wmic_csv(int is_local, const char* host, const char* login, const char* password,
    const char* wql, char* out_value, unsigned int out_size) {
    return -1;
}
int get_cpu_usage(int is_local, const char* host, const char* login, const char* password, int* cpu_percent) { return -1; }
int get_ram_usage(int is_local, const char* host, const char* login, const char* password,
    int* used_percent, unsigned long long* used_mb, unsigned long long* total_mb) {
    return -1;
}
int get_disk_usage(int is_local, const char* host, const char* login, const char* password,
    int* used_percent, double* used_gb, double* total_gb) {
    return -1;
}
void get_remote_resource_stats(const char* host, const char* login, const char* password,
    char* out_buf, unsigned int buf_size) {
    if (out_buf) out_buf[0] = '\0';
}
int parse_disk_usage(const char* stats) { return -1; }
DWORD WINAPI resource_worker(LPVOID arg) { return 0; }
void check_resources_parallel(struct HostInfo* hosts, int count, int iteration) {}
struct HostInfo* read_hosts(const char* filename, int* count) { *count = 0; return NULL; }
void free_hosts(struct HostInfo* hosts, int count) {}

int main() {
    // Небольшой тест для is_private_ip
    const char* ips[] = { "192.168.1.1", "10.0.0.5", "172.20.0.1", "8.8.8.8", "127.0.0.1" };
    for (int i = 0; i < 5; i++) {
        printf("%s -> %s\n", ips[i], is_private_ip(ips[i]) ? "private" : "public");
    }
    return 0;
}