#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "stdbool.h"
#include "dbce.h"

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

static void list_databases() {
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
        FindClose(h);
    } else {
        printf("  (Hic veritabani bulunamadi)\n");
    }
#else
    DIR* d = opendir(".");
    if (d) {
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".dbce")) {
                printf("  - %s\n", dir->d_name);
            }
        }
        closedir(d);
    }
#endif
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

static void render_table(dbce_t* db) {
    printf("+----------------------+----------------------------------------------------+\n");
    printf("| %-20s | %-50s |\n", "KEY", "VALUE");
    printf("+----------------------+----------------------------------------------------+\n");

    table_ctx_t ctx = { .count = 0 };
    dbce_scan_all(db, print_table_row, &ctx);

    printf("+----------------------+----------------------------------------------------+\n");
    if (current_lang == LANG_TR) {
        printf("[+] Toplam aktif kayit: %llu\n", (unsigned long long)ctx.count);
    } else {
        printf("[+] Total active records: %llu\n", (unsigned long long)ctx.count);
    }
}

static void run_repl(const char* db_file) {
    char target[256];
    if (strstr(db_file, ".dbce")) {
        strncpy(target, db_file, sizeof(target) - 1);
    } else {
        snprintf(target, sizeof(target), "%s.dbce", db_file);
    }

    dbce_t* db = dbce_open(target);
    if (!db) {
        if (current_lang == LANG_TR) {
            printf("[-] Veritabani acilamadi: %s\n", target);
        } else {
            printf("[-] Failed to open database: %s\n", target);
        }
        return;
    }

    if (current_lang == LANG_TR) {
        printf("[+] DBCE Shell Baglandi: %s\n", target);
        printf("[i] Komutlar: add [key] [val] | get [key] | get table | del [key] | help | cls | exit\n");
    } else {
        printf("[+] DBCE Shell Connected: %s\n", target);
        printf("[i] Commands: add [key] [val] | get [key] | get table | del [key] | help | cls | exit\n");
    }

    char line[512];
    while (1) {
        printf("dbce(%s)> ", target);
        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strlen(line) == 0) continue;

        char cmd[32] = {0};
        char arg1[128] = {0};
        char arg2[256] = {0};

        int parsed = sscanf(line, "%31s %127s %255[^\n]", cmd, arg1, arg2);

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0) {
            show_shell_help();
        } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
            clear_screen();
        } else if (strcmp(cmd, "add") == 0) {
            if (parsed < 3) {
                printf(current_lang == LANG_TR ? "[-] Kullanim: add [key] [val]\n" : "[-] Usage: add [key] [val]\n");
            } else {
                dbce_set(db, arg1, arg2, strlen(arg2));
                printf("[OK] %s -> %s\n", current_lang == LANG_TR ? "Eklendi" : "Added", arg1);
            }
        } else if (strcmp(cmd, "get") == 0) {
            if (parsed < 2) {
                printf(current_lang == LANG_TR ? "[-] Kullanim: get [key] veya get table\n" : "[-] Usage: get [key] or get table\n");
            } else if (strcmp(arg1, "table") == 0) {
                render_table(db);
            } else {
                void* data = NULL;
                size_t out_len = 0;
                if (dbce_get(db, arg1, &data, &out_len)) {
                    render_single_table(arg1, (char*)data);
                    free(data);
                } else {
                    printf(current_lang == LANG_TR ? "[-] Bulunamadi: %s\n" : "[-] Not found: %s\n", arg1);
                }
            }
        } else if (strcmp(cmd, "del") == 0) {
            if (parsed < 2) {
                printf(current_lang == LANG_TR ? "[-] Kullanim: del [key]\n" : "[-] Usage: del [key]\n");
            } else {
                if (dbce_del(db, arg1)) {
                    printf("[OK] %s: %s\n", current_lang == LANG_TR ? "Silindi" : "Deleted", arg1);
                } else {
                    printf(current_lang == LANG_TR ? "[-] Kayit bulunamadi: %s\n" : "[-] Record not found: %s\n", arg1);
                }
            }
        } else {
            printf(current_lang == LANG_TR ? "[-] Bilinmeyen komut: %s. 'help' yazabilirsiniz.\n" : "[-] Unknown command: %s. Type 'help'.\n", cmd);
        }
    }

    dbce_close(db);
    printf(current_lang == LANG_TR ? "[+] Shell kapatildi.\n" : "[+] Shell closed.\n");
}

int main(int argc, char* argv[]) {
    setup_console_utf8();
    load_lang_config();

    if (argc < 2) {
        show_cli_help();
        return 0;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        show_cli_help();
        return 0;
    }

    if (strcmp(argv[1], "create") == 0 && argc >= 4 && strcmp(argv[2], "db") == 0) {
        char db_name[256];
        snprintf(db_name, sizeof(db_name), "%s.dbce", argv[3]);
        FILE* f = fopen(db_name, "a+b");
        if (f) {
            fclose(f);
            printf("[OK] %s veritabani olusturuldu.\n", db_name);
        } else {
            printf("[-] Veritabani olusturulamadi.\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "db") == 0 && argc >= 3 && strcmp(argv[2], "list") == 0) {
        list_databases();
        return 0;
    }

    if (strcmp(argv[1], "go") == 0 && argc >= 3) {
        run_repl(argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "add") == 0 && argc >= 5) {
        char target[256];
        snprintf(target, sizeof(target), "%s%s", argv[2], strstr(argv[2], ".dbce") ? "" : ".dbce");
        dbce_t* db = dbce_open(target);
        if (!db) {
            printf("[-] DB acilamadi.\n");
            return 1;
        }
        dbce_set(db, argv[3], argv[4], strlen(argv[4]));
        dbce_close(db);
        printf("[OK] Kaydedildi.\n");
        return 0;
    }

    if (strcmp(argv[1], "get") == 0 && argc >= 4) {
        char target[256];
        snprintf(target, sizeof(target), "%s%s", argv[2], strstr(argv[2], ".dbce") ? "" : ".dbce");
        dbce_t* db = dbce_open(target);
        if (!db) {
            printf("[-] DB acilamadi.\n");
            return 1;
        }
        if (strcmp(argv[3], "table") == 0) {
            render_table(db);
        } else {
            void* data = NULL;
            size_t len = 0;
            if (dbce_get(db, argv[3], &data, &len)) {
                render_single_table(argv[3], (char*)data);
                free(data);
            } else {
                printf("[-] Bulunamadi.\n");
            }
        }
        dbce_close(db);
        return 0;
    }

    if (strcmp(argv[1], "del") == 0 && argc >= 4) {
        char target[256];
        snprintf(target, sizeof(target), "%s%s", argv[2], strstr(argv[2], ".dbce") ? "" : ".dbce");
        dbce_t* db = dbce_open(target);
        if (!db) {
            printf("[-] DB acilamadi.\n");
            return 1;
        }
        if (dbce_del(db, argv[3])) {
            printf("[OK] Silindi.\n");
        } else {
            printf("[-] Bulunamadi.\n");
        }
        dbce_close(db);
        return 0;
    }

    show_cli_help();
    return 1;
}