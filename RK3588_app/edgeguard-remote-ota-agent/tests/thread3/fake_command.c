/* Test executable only. Controls live next to argv[0], never on the target:
 * .mode selects failure, .info supplies info text, .slot supplies current identity.
 * .log records each argv element as length:data on a separate line. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *sidecar(const char *binary, const char *suffix, const char *mode)
{
    char *path = NULL;
    if (asprintf(&path, "%s.%s", binary, suffix) < 0) return NULL;
    FILE *file = fopen(path, mode);
    free(path); return file;
}

static void emit(const char *binary, const char *suffix, const char *fallback)
{
    FILE *file = sidecar(binary, suffix, "r");
    if (!file) { fputs(fallback, stdout); return; }
    int c;
    while ((c = fgetc(file)) != EOF) putchar(c);
    fclose(file);
}

int main(int argc, char **argv)
{
    FILE *log = sidecar(argv[0], "log", "a");
    if (!log) return 98;
    fprintf(log, "argc=%d\n", argc);
    for (int i = 0; i < argc; ++i) fprintf(log, "%zu:%s\n", strlen(argv[i]), argv[i]);
    fclose(log);
    char mode[64] = "";
    FILE *control = sidecar(argv[0], "mode", "r");
    if (control) { if (!fgets(mode, sizeof(mode), control)) mode[0] = 0; fclose(control); }
    if (!strcmp(mode, "sleep")) { sleep(10); return 0; }
    if (!strcmp(mode, "signal")) { raise(SIGTERM); return 99; }
    if (!strcmp(mode, "nul")) { fwrite("a\0b", 1, 3, stdout); return 0; }
    if (!strcmp(mode, "flood")) {
        for (int i = 0; i < 300000; ++i) putchar('x');
        return 0;
    }
    if (!strcmp(mode, "fail")) return 42;
    if (argc == 1) return 0; /* optional hook with no argv */
    if (argc == 2 && !strcmp(argv[1], "get-current")) {
        emit(argv[0], "slot", "a\n"); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "info") &&
        !strcmp(argv[2], "--output-format=shell") && argv[3][0] == '/') {
        emit(argv[0], "info",
             "RAUC_MF_COMPATIBLE='EdgeGuard-ATK-DLRK3588-RK3588'\n"
             "RAUC_MF_VERSION='1.2.0'\nRAUC_MF_BUILD='candidate'\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "install") && argv[2][0] == '/')
        return !strcmp(mode, "install-fail") ? 23 : 0;
    if (argc == 2 && !strcmp(argv[1], "status")) return 0;
    if (argc == 3 && !strcmp(argv[1], "status") &&
        (!strcmp(argv[2], "mark-good") || !strcmp(argv[2], "mark-bad"))) return 0;
    return 97; /* reject every argv outside the fixture contract */
}
