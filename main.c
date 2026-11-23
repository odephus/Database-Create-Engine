#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "stdbool.h"
#include "dbce.h"
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include "windows.h"
#else
#include "dirent.h"
#endif

typedef enum {
    LANG_TR,
    LANG_EN
} dbce_lang_t;

static dbce_lang_t current_lang = LANG_TR;

static void setup_console_utf8() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
}

static void load_lang_config() {
    FILE* f = fopen("config.json", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* k = strstr(line, "\"lang\"");
        if (k) {
            if (strstr(k, "\"en\"")) {
                current_lang = LANG_EN;
            } else {
                current_lang = LANG_TR;
            }
            break;
        }
    }
    fclose(f);
}

static void show_cli_help() {
    if (current_lang == LANG_TR) {
        printf("\n================ [ DBCE Komut Kilavuzu ] ================\n\n");
        printf("[ Terminal (CLI) Komutlari ]:\n");
        printf("  dbce create db [isim]          : Yeni bir veritabani (.dbce) olusturur.\n");
        printf("  dbce db list                  : Klasordeki tum veritabanlarini listeler.\n");
        printf("  dbce go [db_adi]              : Veritabani icin interaktif shell acar.\n");
        printf("  dbce add [db] [key] [val]     : Hizli tekli kayit ekler.\n");
        printf("  dbce get [db] [key]           : Hizli tekli kayit okur (tablo formatinda).\n");
        printf("  dbce get [db] table           : Hizli tum tablo ciktisi verir.\n");
        printf("  dbce del [db] [key]           : Hizli tekli kayit siler.\n\n");
        printf("[ Shell (REPL) Komutlari ] (dbce go sonrasi):\n");
        printf("  add [key] [val]               : Kayit ekler veya gunceller.\n");
        printf("  get [key]                     : Anahtarin degerini tablo formatinda getirir.\n");
        printf("  get table                     : Tum aktif kayitlari listeler.\n");
        printf("  del [key]                     : Kaydi siler (tombstone).\n");
        printf("  cls / clear                   : Ekrani temizler.\n");
        printf("  help                          : Shell komutlarini gosterir.\n");
        printf("  exit / quit                   : Shell'den cikar.\n");
        printf("=========================================================\n\n");
    } else {
        printf("\n================ [ DBCE Command Guide ] ================\n\n");
        printf("[ Terminal (CLI) Commands ]:\n");
        printf("  dbce create db [name]         : Creates a new database (.dbce).\n");
        printf("  dbce db list                  : Lists all databases in directory.\n");
        printf("  dbce go [db_name]             : Enters interactive database shell.\n");
        printf("  dbce add [db] [key] [val]     : Quick single write.\n");
        printf("  dbce get [db] [key]           : Quick single read (table format).\n");
        printf("  dbce get [db] table           : Quick dump all as table.\n");
        printf("  dbce del [db] [key]           : Quick single delete.\n\n");
        printf("[ Shell (REPL) Commands ] (inside dbce go):\n");
        printf("  add [key] [val]               : Insert or update record.\n");
        printf("  get [key]                     : Fetch value in table format.\n");
        printf("  get table                     : Render all active rows.\n");
        printf("  del [key]                     : Delete record (tombstone).\n");
        printf("  cls / clear                   : Clear terminal.\n");
        printf("  help                          : Display shell help.\n");
        printf("  exit / quit                   : Exit shell.\n");
        printf("=========================================================\n\n");
    }
}

static void show_shell_help() {
    if (current_lang == LANG_TR) {
        printf("\n[ DBCE Shell Komutlari ]:\n");
        printf("  add [key] [val]   : Kayit ekler veya gunceller.\n");
        printf("  get [key]         : Anahtarin degerini tablo halinde basar.\n");
        printf("  get table         : Tum aktif kayitlari listeler.\n");
        printf("  del [key]         : Kaydi siler (tombstone).\n");
        printf("  cls / clear       : Ekrani temizler.\n");
        printf("  help              : Bu yardim listesini basar.\n");
        printf("  exit / quit       : Shell'den cikar.\n\n");
    } else {
        printf("\n[ DBCE Shell Commands ]:\n");
        printf("  add [key] [val]   : Insert or update record.\n");
        printf("  get [key]         : Fetch and display value in a table.\n");
        printf("  get table         : Render all active rows.\n");
        printf("  del [key]         : Delete record (tombstone).\n");
        printf("  cls / clear       : Clear screen.\n");
        printf("  help              : Display this help message.\n");
        printf("  exit / quit       : Exit shell session.\n\n");
    }
}

static void clear_screen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

static bool list_databases(void) {
    if (current_lang == LANG_TR) {
        printf("[+] Mevcut DBCE Veritabanlari:\n");
    } else {
        printf("[+] Available DBCE Databases:\n");
    }
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.dbce", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            printf("  - %s\n", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        DWORD error = GetLastError();
        if (!FindClose(h) || error != ERROR_NO_MORE_FILES) {
            errno = EIO;
            return false;
        }
    } else {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) { errno = EIO; return false; }
        printf("  (Hic veritabani bulunamadi)\n");
    }
#else
    DIR* d = opendir(".");
    if (!d) return false;
    {
        struct dirent* dir;
        for (;;) {
            errno = 0;
            dir = readdir(d);
            if (!dir) break;
            if (strstr(dir->d_name, ".dbce")) {
                printf("  - %s\n", dir->d_name);
            }
        }
        int error = errno;
        if (closedir(d) != 0) return false;
        if (error) { errno = error; return false; }
    }
#endif
    return true;
}

typedef struct {
    size_t count;
} table_ctx_t;

static void print_table_row(const char* key, const void* val, size_t val_len, void* udata) {
    (void)val_len;
    table_ctx_t* ctx = (table_ctx_t*)udata;
    ctx->count++;
    printf("| %-20s | %-50s |\n", key, (const char*)val);
}

static void render_single_table(const char* key, const char* val) {
    printf("+----------------------+----------------------------------------------------+\n");
    printf("| %-20s | %-50s |\n", "KEY", "VALUE");
    printf("+----------------------+----------------------------------------------------+\n");
    printf("| %-20s | %-50s |\n", key, val);
    printf("+----------------------+----------------------------------------------------+\n");
}

static bool render_table(dbce_t* db) {
    printf("+----------------------+----------------------------------------------------+\n");
    printf("| %-20s | %-50s |\n", "KEY", "VALUE");
    printf("+----------------------+----------------------------------------------------+\n");

    table_ctx_t ctx = { .count = 0 };
    if (!dbce_scan_all_checked(db, print_table_row, &ctx)) return false;

    printf("+----------------------+----------------------------------------------------+\n");
    if (current_lang == LANG_TR) {
        printf("[+] Toplam aktif kayit: %llu\n", (unsigned long long)ctx.count);
    } else {
        printf("[+] Total active records: %llu\n", (unsigned long long)ctx.count);
    }
    return true;
}

static bool db_path(const char* name, char target[256]) {
    size_t len = strlen(name);
    if (!len) { errno = EINVAL; return false; }
    bool suffix = len >= 5 && strcmp(name + len - 5, ".dbce") == 0;
    if (len >= 256u - (suffix ? 0u : 5u)) { errno = ENAMETOOLONG; return false; }
    strcpy(target, name);
    if (!suffix) strcat(target, ".dbce");
    return true;
}

static void report_error(const char* operation, const char* key) {
    const char* message;
    switch (errno) {
    case ENOENT:
        if (!key) { perror(operation); return; }
        fprintf(stderr, "%s: %s (%s)\n", operation,
                current_lang == LANG_TR ? "Anahtar bulunamadi" : "Key not found", key);
        return;
    case EILSEQ:
        message = current_lang == LANG_TR ? "Gecersiz veya eksik DBCE kaydi" :
                                           "Invalid or incomplete DBCE record";
        break;
    case ENOSPC:
        message = current_lang == LANG_TR ? "Indeks veya disk dolu" :
                                           "Index capacity or storage exhausted";
        break;
    default:
        perror(operation);
        return;
    }
    fprintf(stderr, "%s: %s\n", operation, message);
}

static bool show_value(dbce_t* db, const char* key) {
    if (strcmp(key, "table") == 0) return render_table(db);
    void* data;
    size_t len;
    if (!dbce_get(db, key, &data, &len)) return false;
    render_single_table(key, data);
    free(data);
    return true;
}

static char* skip_spaces(char* p) {
    while (isspace((unsigned char)*p)) ++p;
    return p;
}

static char* next_token(char** cursor) {
    char* token = skip_spaces(*cursor);
    char* p = token;
    while (*p && !isspace((unsigned char)*p)) ++p;
    if (*p) *p++ = '\0';
    *cursor = p;
    return token;
}

static bool run_repl(dbce_t* db, const char* target) {
    if (current_lang == LANG_TR) {
        printf("[+] DBCE Shell Baglandi: %s\n", target);
        printf("[i] Komutlar: add [key] [val] | get [key] | get table | del [key] | help | cls | exit\n");
    } else {
        printf("[+] DBCE Shell Connected: %s\n", target);
        printf("[i] Commands: add [key] [val] | get [key] | get table | del [key] | help | cls | exit\n");
    }

    bool ok = true;
    char line[512];
    for (;;) {
        printf("dbce(%s)> ", target);
        if (!fgets(line, sizeof(line), stdin)) break;
        char* newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        else {
            int ch = getchar();
            if (ch != EOF && ch != '\n') {
                while (ch != EOF && ch != '\n') ch = getchar();
                fprintf(stderr, "[-] %s\n", current_lang == LANG_TR ? "Komut cok uzun." : "Command too long.");
                ok = false;
                continue;
            }
        }
        char* cursor = line;
        char* cmd = next_token(&cursor);
        char* key = next_token(&cursor);
        char* value = skip_spaces(cursor);
        if (!*cmd) continue;
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) break;
        if (strcmp(cmd, "help") == 0) { show_shell_help(); continue; }
        if (strcmp(cmd, "cls") == 0 || strcmp(cmd, "clear") == 0) { clear_screen(); continue; }
        bool success;
        if (strcmp(cmd, "add") == 0) {
            success = dbce_set(db, key, value, strlen(value));
            if (success) printf("[OK] %s -> %s\n", current_lang == LANG_TR ? "Eklendi" : "Added", key);
        } else if (strcmp(cmd, "get") == 0) {
            success = show_value(db, key);
        } else if (strcmp(cmd, "del") == 0) {
            success = dbce_del(db, key);
            if (success) printf("[OK] %s: %s\n", current_lang == LANG_TR ? "Silindi" : "Deleted", key);
        } else {
            show_shell_help();
            ok = false;
            continue;
        }
        if (!success) { report_error(cmd, key); ok = false; }
    }
    if (ferror(stdin)) { perror("stdin"); ok = false; }
    printf(current_lang == LANG_TR ? "[+] Shell kapatildi.\n" : "[+] Shell closed.\n");
    return ok;
}

int main(int argc, char* argv[]) {
    setup_console_utf8();
    load_lang_config();
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        show_cli_help();
        return 0;
    }

    const char* cmd = argv[1];
    if (argc >= 3 && strcmp(cmd, "db") == 0 && strcmp(argv[2], "list") == 0) {
        if (list_databases()) return 0;
        perror("db list");
        return 1;
    }
    bool create = argc >= 4 && strcmp(cmd, "create") == 0 && strcmp(argv[2], "db") == 0;
    bool repl = argc >= 3 && strcmp(cmd, "go") == 0;
    bool add = argc >= 5 && strcmp(cmd, "add") == 0;
    bool get = argc >= 4 && strcmp(cmd, "get") == 0;
    bool del = argc >= 4 && strcmp(cmd, "del") == 0;
    if (!(create || repl || add || get || del)) { show_cli_help(); return 1; }
    char target[256];
    if (!db_path(argv[create ? 3 : 2], target)) { perror(cmd); return 1; }
    dbce_t* db = dbce_open(target);
    if (!db) { report_error(target, NULL); return 1; }

    bool ok = true;
    if (repl) ok = run_repl(db, target);
    else if (add) ok = dbce_set(db, argv[3], argv[4], strlen(argv[4]));
    else if (get) ok = show_value(db, argv[3]);
    else if (del) ok = dbce_del(db, argv[3]);
    if (!ok && !repl) report_error(cmd, argv[3]);
    if (!dbce_close_checked(db)) {
        if (ok) report_error(target, NULL);
        ok = false;
    }
    if (!ok) return 1;
    if (create) printf("[OK] %s veritabani olusturuldu.\n", target);
    if (add) puts("[OK] Kaydedildi.");
    if (del) puts("[OK] Silindi.");
    return 0;
}
