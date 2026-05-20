#define _CRT_SECURE_NO_WARNINGS   
#include <stdio.h>                
#include <stdlib.h>               
#include <string.h>              
#include <time.h>                
#include <locale.h>               
#include <windows.h>              // Подключает Windows API (CreateThread, WaitForMultipleObjects, MessageBox и др.)

#define MAX_IP_LEN 256            // Максимальная длина строки с IP-адресом или именем хоста
#define MAX_LINE_LEN 512          // Максимальная длина строки при чтении конфигурационных файлов
#define RESULT_FILE "ping_results.txt"       // Имя файла для сохранения результатов ping
#define RESOURCE_FILE "resource_stats.txt"   // Имя файла для сохранения статистики ресурсов (CPU, RAM, диск)
#define ERROR_LOG "wmi_errors.log"           // Имя файла для лога ошибок выполнения команд WMIC
#define DEFAULT_INPUT_FILE "hosts.txt"       // Имя файла со списком хостов по умолчанию
#define CONFIG_FILE "config.txt"             // Имя файла конфигурации (интервалы проверок)
#define DEFAULT_PING_INTERVAL_SEC 60         // Интервал проверки ping по умолчанию (60 секунд)
#define DEFAULT_RESOURCE_INTERVAL_SEC 60     // Интервал сбора ресурсов по умолчанию (60 секунд)

// Структура для хранения информации об одном хосте
struct HostInfo {
    char host[MAX_IP_LEN];        // IP-адрес или доменное имя хоста
    char login[128];              // Логин для доступа к WMIC (если требуется)
    char password[128];           // Пароль для доступа к WMIC
};

// Структура для передачи данных в поток ping
struct PingThreadData {
    char host[MAX_IP_LEN];        // Копия адреса хоста
    int index;                    // Порядковый номер хоста в списке (для вывода)
};

// Структура для передачи данных в поток сбора ресурсов
struct ResourceThreadData {
    struct HostInfo hostInfo;     // Информация о хосте (адрес, логин, пароль)
    int index;                    // Индекс хоста в списке
    char result[2048];            // Буфер для строки с результатами сбора ресурсов
};

// Проверяет, является ли символ цифрой
int is_digit(char c) {
    return (c >= '0' && c <= '9'); // Возвращает 1, если c от '0' до '9', иначе 0
}

// Удаляет начальные и конечные пробельные символы (пробел, табуляция, \n, \r)
void trim(char* str) {
    char* start = str;
    char* end;
    while (*start == ' ' || *start == '\t') start++;
    if (*start == 0) { str[0] = '\0'; return; }
    end = start + strlen(start) - 1;
    // Удаляем пробелы, табуляции, \n, \r в конце
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = '\0';             // Завершаем строку нулём после последнего значимого символа
    if (start != str) memmove(str, start, strlen(start) + 1);
}

// Проверяет, является ли строка комментарием (#) или пустой
int is_ignored_line(const char* line) {
    return (line[0] == '#' || strlen(line) == 0);
}

// Читает интервалы проверок из config.txt
void read_intervals(int* ping_sec, int* resource_sec) {
    FILE* cfg;                     // Указатель на файл конфигурации
    char line[MAX_LINE_LEN];       // Буфер для чтения строки
    char key[64];                  // Название параметра (ping_interval или resource_interval)
    int val;                       // Значение параметра

    *ping_sec = DEFAULT_PING_INTERVAL_SEC;
    *resource_sec = DEFAULT_RESOURCE_INTERVAL_SEC;
    cfg = fopen(CONFIG_FILE, "r");
    if (!cfg) return;


    while (fgets(line, sizeof(line), cfg)) {
        line[strcspn(line, "\n")] = '\0'; // Удаляем символ перевода строки
        trim(line);                // Обрезаем пробелы
        if (is_ignored_line(line)) continue; // Пропускаем пустые строки и комментарии
        if (sscanf(line, "%s %d", key, &val) == 2 && val > 0) {
            if (strcmp(key, "ping_interval") == 0) *ping_sec = val;
            else if (strcmp(key, "resource_interval") == 0) *resource_sec = val;
        }
    }
    fclose(cfg);
}

// Выполняет одиночный ping (одна попытка, таймаут 1000 мс)
int simple_ping(const char* host) {
    char command[512];             // Буфер для команды ping
    // Формируем команду: ping -n 1 -w 1000 <хост> > nul 2>&1 (скрываем вывод)
    sprintf(command, "ping -n 1 -w 1000 %s > nul 2>&1", host);
    return system(command);        // Выполняем команду, возвращаем 0 при успехе
}

// Функция потока для проверки ping
DWORD WINAPI ping_worker(LPVOID arg) {
    struct PingThreadData* data = (struct PingThreadData*)arg; // Получаем переданные данные
    int res = simple_ping(data->host); // Выполняем ping
    // Выводим результат с цветом: зелёный если доступен, красный если нет
    printf("%s[%d] %s - %s\033[0m\n",
        res == 0 ? "\033[32m" : "\033[31m",
        data->index, data->host, res == 0 ? "AVAILABLE" : "UNAVAILABLE");
    free(data);
    return res;
}

// Параллельная проверка ping всех хостов
void check_ping_parallel(struct HostInfo* hosts, int count, int iteration) {
    HANDLE* threads;               // Массив дескрипторов потоков
    int* results;                  // Массив результатов ping для каждого хоста
    int i;
    FILE* f;
    time_t now;
    struct tm* t;
    int fail;
    char msg[512];

    printf("\033[36m[PING] Итерация #%d\033[0m\n", iteration);
    threads = (HANDLE*)malloc(count * sizeof(HANDLE));
    results = (int*)calloc(count, sizeof(int));

    // Создаём поток для каждого хоста
    for (i = 0; i < count; i++) {
        struct PingThreadData* data = (struct PingThreadData*)malloc(sizeof(struct PingThreadData));
        strcpy(data->host, hosts[i].host);
        data->index = i + 1;                 // Номер хоста (начиная с 1)
        threads[i] = CreateThread(NULL, 0, ping_worker, data, 0, NULL); // Создаём поток
    }

    // Ждём завершения всех потоков
    WaitForMultipleObjects(count, threads, TRUE, INFINITE);

    // Получаем коды возврата (результаты ping) и закрываем дескрипторы
    for (i = 0; i < count; i++) {
        DWORD exit_code;
        GetExitCodeThread(threads[i], &exit_code); // Получаем код выхода потока
        results[i] = (int)exit_code;               // Сохраняем результат
        CloseHandle(threads[i]);                   // Закрываем дескриптор
    }


    f = fopen(RESULT_FILE, "a");
    if (f) {
        now = time(NULL);
        t = localtime(&now);
        fprintf(f, "\n========================================\n");
        fprintf(f, "PING итерация #%d: %02d.%02d.%04d %02d:%02d:%02d\n",
            iteration,
            t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
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

    // Подсчитываем количество недоступных хостов
    fail = 0;
    for (i = 0; i < count; i++) if (results[i] != 0) fail++;
    if (fail > 0) {
        sprintf(msg, "Итерация #%d: %d хостов недоступны.", iteration, fail);
        MessageBoxA(NULL, msg, "Мониторинг хостов", MB_OK | MB_ICONWARNING);
    }

    free(threads);
    free(results);
}

// Определяет, является ли IP-адрес частным (RFC 1918) или localhost
int is_private_ip(const char* host) {
    unsigned int a, b, c, d;
    // Пытаемся распарсить IPv4 адрес
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0; // Не IPv4 – считаем публичным
    // Диапазоны частных адресов:
    if (a == 10) return 1;                         // 10.0.0.0/8
    if (a == 172 && b >= 16 && b <= 31) return 1;  // 172.16.0.0/12
    if (a == 192 && b == 168) return 1;            // 192.168.0.0/16
    if (a == 127) return 1;                        // 127.0.0.0/8 (localhost)
    return 0;                                      // Иначе публичный
}

// Выполняет WMIC-запрос, возвращает значение первого поля (CSV-формат)
int run_wmic_csv(int is_local, const char* host, const char* login, const char* password,
    const char* wql, char* out_value, unsigned int out_size)
{
    char cmd[2048];              // Буфер для команды
    char auth[256];              // Строка аутентификации (/user /password)
    FILE* pipe;                  // Канал для чтения вывода команды
    char line[1024];             // Буфер для строки вывода
    int found;                   // Флаг, найден ли результат
    char* first_comma, * value, * end;
    time_t now;
    struct tm* t;
    FILE* err;

    auth[0] = '\0';              // Инициализация пустой строкой

    // Формируем команду в зависимости от того, локальный хост или удалённый
    if (is_local) {
        sprintf(cmd, "wmic %s /format:csv 2>&1", wql); // Для локального хоста
    }
    else {
        if (login && login[0] && password && password[0]) {
            sprintf(auth, "/user:\"%s\" /password:\"%s\" ", login, password); // Аутентификация
        }
        sprintf(cmd, "wmic /node:\"%s\" /timeout:5000 %s %s /format:csv 2>&1", host, auth, wql);
    }

    pipe = _popen(cmd, "r");     // Открываем канал для чтения вывода
    if (!pipe) return -1;        // Ошибка открытия

    found = 0;
    // Читаем вывод построчно
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "Node", 4) == 0) continue; // Пропускаем строку заголовка
        if (strlen(line) == 0) continue;

        first_comma = strchr(line, ',');   // Ищем первую запятую (разделитель CSV)
        if (!first_comma) continue;

        value = first_comma + 1;           // Значение находится после запятой
        if (*value == '"') value++;        // Если значение в кавычках, пропускаем открывающую

        end = value + strlen(value) - 1;   // Указатель на последний символ значения
        if (*end == '"') *end = '\0';      // Удаляем закрывающую кавычку

        strncpy(out_value, value, out_size - 1); // Копируем значение в выходной буфер
        out_value[out_size - 1] = '\0';
        trim(out_value);
        found = 1;
        break;
    }
    _pclose(pipe);

    if (!found) {                          // Если результат не найден
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

// Получает загрузку CPU (проценты) через WMIC
int get_cpu_usage(int is_local, const char* host, const char* login, const char* password, int* cpu_percent) {
    char val[32];                          // Буфер для значения
    // Выполняем запрос "cpu get loadpercentage"
    if (run_wmic_csv(is_local, host, login, password, "cpu get loadpercentage", val, sizeof(val)) == 0) {
        if (strlen(val) > 0 && is_digit(val[0])) { // Проверяем, что результат – число
            *cpu_percent = atoi(val);      // Преобразуем в целое
            return 0;
        }
    }
    return -1;
}

// Получает использование RAM (проценты, использовано МБ, всего МБ)
int get_ram_usage(int is_local, const char* host, const char* login, const char* password,
    int* used_percent, unsigned long long* used_mb, unsigned long long* total_mb) {
    char combined[128];                    // Буфер для строки "свободно_КБ,всего_КБ"
    char* comma;
    char* free_str;
    char* total_str;
    unsigned long long free_kb, total_kb;

    // Запрашиваем FreePhysicalMemory и TotalVisibleMemorySize
    if (run_wmic_csv(is_local, host, login, password,
        "OS get FreePhysicalMemory,TotalVisibleMemorySize", combined, sizeof(combined)) == 0) {
        comma = strchr(combined, ',');     // Ищем разделитель запятую
        if (!comma) return -1;
        *comma = '\0';                     // Разделяем строку
        free_str = combined;
        total_str = comma + 1;
        trim(free_str);
        trim(total_str);
        free_kb = strtoull(free_str, NULL, 10);   // Свободная память в КБ
        total_kb = strtoull(total_str, NULL, 10); // Общая память в КБ
        if (total_kb == 0) return -1;
        *total_mb = total_kb / 1024;              // Переводим в МБ
        *used_mb = (total_kb - free_kb) / 1024;   // Использовано МБ
        *used_percent = (int)((*used_mb * 100) / (*total_mb));
        return 0;
    }
    return -1;
}

// Получает использование диска C: (проценты, использовано ГБ, всего ГБ)
int get_disk_usage(int is_local, const char* host, const char* login, const char* password,
    int* used_percent, double* used_gb, double* total_gb) {
    char combined[128];                    // Буфер для строки "свободно_байт,всего_байт"
    char* comma;
    char* free_str;
    char* total_str;
    unsigned long long free_bytes, total_bytes;

    // Запрос для диска C:
    if (run_wmic_csv(is_local, host, login, password,
        "logicaldisk where \"DeviceID='C:'\" get FreeSpace,Size", combined, sizeof(combined)) == 0) {
        comma = strchr(combined, ',');     // Ищем запятую
        if (!comma) return -1;
        *comma = '\0';
        free_str = combined;
        total_str = comma + 1;
        trim(free_str);
        trim(total_str);
        free_bytes = strtoull(free_str, NULL, 10);   // Свободное место в байтах
        total_bytes = strtoull(total_str, NULL, 10); // Общий размер в байтах
        if (total_bytes == 0) return -1;
        *total_gb = total_bytes / (1024.0 * 1024 * 1024); // Переводим в ГБ
        *used_gb = (total_bytes - free_bytes) / (1024.0 * 1024 * 1024);
        *used_percent = (int)((*used_gb * 100) / (*total_gb));
        return 0;
    }
    return -1;
}

// Основная функция сбора статистики ресурсов (CPU, RAM, диск C:) для хоста
void get_remote_resource_stats(const char* host, const char* login, const char* password,
    char* out_buf, unsigned int buf_size)
{
    char ping_cmd[512];          // Буфер для команды ping
    int is_local;                // Флаг локального хоста
    int cpu_test;                // Временная переменная для проверки доступности WMIC
    char tmp[256];               // Временный буфер для форматирования строк
    int has_error;               // Флаг наличия ошибок
    int cpu;                     // Загрузка CPU в процентах
    int ram_percent;             // Использование RAM в процентах
    unsigned long long used_mb, total_mb; // Использовано и всего RAM в МБ
    int disk_percent;            // Использование диска в процентах
    double used_gb, total_gb;    // Использовано и всего диска в ГБ
    FILE* err_log;               // Указатель на лог ошибок
    time_t now;
    struct tm* t;

    out_buf[0] = '\0';           // Очищаем выходной буфер

    // Для публичных IP ресурсы не собираем (только приватные и localhost)
    if (!is_private_ip(host)) {
        sprintf(out_buf, "PUBLIC_HOST (ресурсы не собираются)");
        return;
    }

    // Проверяем доступность хоста через ping (одна попытка)
    sprintf(ping_cmd, "ping -n 1 -w 1000 %s > nul 2>&1", host);
    if (system(ping_cmd) != 0) {
        sprintf(out_buf, "HOST UNREACHABLE");
        return;
    }

    // Определяем, является ли хост локальным (localhost или 127.0.0.1)
    is_local = (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0);

    // Если хост не локальный, пробуем подключиться к его WMIC с флагом is_local=1 (проверка доступности без аутентификации)
    if (!is_local) {
        cpu_test = -1;
        if (get_cpu_usage(1, host, login, password, &cpu_test) == 0 && cpu_test >= 0) {
            is_local = 1;        // Если удалось, считаем хост "локальным" для WMIC
        }
    }

    has_error = 0;

    // Получение загрузки CPU
    cpu = -1;
    if (get_cpu_usage(is_local, host, login, password, &cpu) == 0 && cpu >= 0) {
        sprintf(tmp, "CPU:%d%% ", cpu);
        strncat(out_buf, tmp, buf_size - strlen(out_buf) - 1); // Добавляем к выходной строке
    }
    else {
        strncat(out_buf, "CPU:ERROR ", buf_size - strlen(out_buf) - 1);
        has_error = 1;           // Отмечаем ошибку
    }

    // Получение использования RAM
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

    // Получение использования диска C:
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

    // Если были ошибки и строка результата короткая (менее 50 символов), записываем в лог
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

    if (strlen(out_buf) == 0) strcpy(out_buf, "UNKNOWN"); // Если буфер пуст, пишем UNKNOWN
}

// Извлекает процент загрузки диска из строки статистики (ищет "DISK_C:xx%")
int parse_disk_usage(const char* stats) {
    const char* disk_tag = "DISK_C:";  // Ищем этот тег
    char* p = strstr(stats, disk_tag);
    if (!p) return -1;                 // Тег не найден
    p += strlen(disk_tag);             // Перемещаем указатель после тега
    while (*p && !is_digit(*p)) p++;   // Пропускаем нецифровые символы
    if (!*p) return -1;                // Цифры не найдены
    return atoi(p);                    // Преобразуем число и возвращаем
}

// Функция потока для сбора ресурсов
DWORD WINAPI resource_worker(LPVOID arg) {
    struct ResourceThreadData* data = (struct ResourceThreadData*)arg; // Получаем данные
    // Вызываем основную функцию сбора статистики, результат пишем в data->result
    get_remote_resource_stats(data->hostInfo.host, data->hostInfo.login, data->hostInfo.password,
        data->result, sizeof(data->result));
    return 0;   // Поток завершается
}

// Параллельный сбор ресурсов для всех хостов
void check_resources_parallel(struct HostInfo* hosts, int count, int iteration) {
    struct ResourceThreadData* threadData; // Массив данных для потоков
    HANDLE* threads;                         // Массив дескрипторов потоков
    int i;
    FILE* f;
    time_t now;
    struct tm* t;
    char** overloaded_hosts;                 // Массив имён хостов с перегрузкой диска >80%
    int* overloaded_percents;                // Массив процентов загрузки диска
    int overload_count;                      // Количество перегруженных хостов
    char msg[2048];                          // Буфер для сообщения MessageBox
    char line[256];                          // Временная строка
    int disk_percent;

    printf("\033[35m[RESOURCE] Итерация #%d (параллельный сбор)\033[0m\n", iteration);

    threadData = (struct ResourceThreadData*)malloc(count * sizeof(struct ResourceThreadData));
    threads = (HANDLE*)malloc(count * sizeof(HANDLE));

    // Создаём потоки для каждого хоста
    for (i = 0; i < count; i++) {
        threadData[i].hostInfo = hosts[i];   // Копируем информацию о хосте
        threadData[i].index = i;
        threadData[i].result[0] = '\0';       // Очищаем буфер результата
        threads[i] = CreateThread(NULL, 0, resource_worker, &threadData[i], 0, NULL); // Создаём поток
        if (threads[i] == NULL) {
            printf("Ошибка создания потока для %s\n", hosts[i].host);
            strcpy(threadData[i].result, "THREAD ERROR");
        }
    }

    // Ждём завершения всех потоков
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
        fprintf(f, "[%s] %s\n", hosts[i].host, threadData[i].result); // Запись в файл
        printf("\033[35m  %s -> %s\033[0m\n", hosts[i].host, threadData[i].result); // Вывод на консоль

        disk_percent = parse_disk_usage(threadData[i].result); // Извлекаем процент загрузки диска
        if (disk_percent > 80) {                               // Если загрузка > 80%
            // Добавляем хост в список перегруженных
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

// Читает список хостов из файла (формат: хост [логин пароль])
struct HostInfo* read_hosts(const char* filename, int* count) {
    FILE* f;
    struct HostInfo* hosts;       // Указатель на массив структур
    char line[MAX_LINE_LEN];      // Буфер для чтения строки
    char host[MAX_IP_LEN];        // Временный буфер для хоста
    char login[128];              // Временный буфер для логина
    char password[128];           // Временный буфер для пароля
    int n;

    *count = 0;                   // Инициализируем счётчик
    f = fopen(filename, "r");
    if (!f) return NULL;
    hosts = NULL;                 // Изначально массив пуст

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (is_ignored_line(line)) continue;

        host[0] = login[0] = password[0] = '\0'; // Очищаем буферы
        n = sscanf(line, "%255s %127s %127s", host, login, password); // Парсим три поля
        if (n < 1) continue;              // Если нет хоста – пропускаем


        hosts = (struct HostInfo*)realloc(hosts, (*count + 1) * sizeof(struct HostInfo));
        strcpy(hosts[*count].host, host); // Копируем хост
        if (n >= 2) strcpy(hosts[*count].login, login); else hosts[*count].login[0] = '\0'; // Копируем логин, если есть
        if (n >= 3) strcpy(hosts[*count].password, password); else hosts[*count].password[0] = '\0'; // Копируем пароль
        (*count)++;                       // Увеличиваем счётчик
    }
    fclose(f);
    return hosts;
}


void free_hosts(struct HostInfo* hosts, int count) {
    (void)count;  // Подавляем предупреждение о неиспользуемом параметре (нужно для совместимости)
    free(hosts);
}


int main() {
    int ping_interval, resource_interval;
    time_t last_ping_time, last_resource_time; // Время последнего выполнения проверок
    int ping_iter, res_iter;                 // Номера итераций
    int old_ping, old_res;                   // Предыдущие значения интервалов (для отслеживания изменений)
    time_t now;
    int do_ping, do_resource;                // Флаги: нужно ли выполнять проверки сейчас
    int host_count;                          // Количество хостов
    struct HostInfo* hosts;                  // Массив хостов

    setlocale(LC_ALL, "Russian");
    printf("\033[33m=== Мониторинг хостов (ping + ресурсы) ===\033[0m\n");
    printf("Формат hosts.txt: хост [логин пароль]\n\n");

    read_intervals(&ping_interval, &resource_interval);
    printf("Текущие интервалы: ping = %d сек, ресурсы = %d сек\n", ping_interval, resource_interval);
    printf("Для изменения отредактируйте config.txt\n\n");

    last_ping_time = 0;       // Обнуляем время последнего ping (чтобы выполнился сразу)
    last_resource_time = 0;   // Обнуляем время последнего сбора ресурсов
    ping_iter = 0;            // Номер итерации ping
    res_iter = 0;             // Номер итерации ресурсов

    while (1) {
        now = time(NULL);
        old_ping = ping_interval;
        old_res = resource_interval;
        read_intervals(&ping_interval, &resource_interval);
        if (old_ping != ping_interval || old_res != resource_interval) {
            printf("\033[33m[!] Интервалы обновлены: ping = %d сек, ресурсы = %d сек\033[0m\n", ping_interval, resource_interval);
        }

        // Определяем, наступило ли время для выполнения проверок
        do_ping = (now - last_ping_time >= ping_interval);
        do_resource = (now - last_resource_time >= resource_interval);

        // Если пришло время – выполняем проверку ping
        if (do_ping) {
            hosts = read_hosts(DEFAULT_INPUT_FILE, &host_count); // Загружаем список хостов
            if (hosts && host_count > 0) {
                ping_iter++;
                check_ping_parallel(hosts, host_count, ping_iter); // Запускаем параллельную проверку
                free_hosts(hosts, host_count);
            }
            else {
                printf("Нет хостов для ping.\n");
            }
            last_ping_time = now;
            printf("Следующий ping через %d сек.\n\n", ping_interval);
        }

        // Если пришло время – выполняем сбор ресурсов
        if (do_resource) {
            hosts = read_hosts(DEFAULT_INPUT_FILE, &host_count); // Загружаем список хостов
            if (hosts && host_count > 0) {
                res_iter++;
                check_resources_parallel(hosts, host_count, res_iter); // Параллельный сбор ресурсов
                free_hosts(hosts, host_count);                  // Освобождаем память
            }
            else {
                printf("Нет хостов для ресурсов.\n");
            }
            last_resource_time = now;                           // Обновляем время последнего сбора
            printf("Следующий сбор ресурсов через %d сек.\n\n", resource_interval);
        }

        // Если ни одна проверка не выполнялась, ждём 1 секунду (чтобы не нагружать процессор)
        if (!do_ping && !do_resource) {
            Sleep(1000);
        }
    }
    return 0;
}