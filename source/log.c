#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#include "log.h"

static int s_nxlink_sock = -1;
// Journal sur la carte SD (version installée : pas de nxlink), recréé à chaque lancement
static FILE *s_file = NULL;

void log_init(void)
{
    // stdout reste sur la console à l'écran ; stderr part vers nxlink s'il est là
    if (R_SUCCEEDED(socketInitializeDefault()))
        s_nxlink_sock = nxlinkConnectToHost(false, true);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/nxgo", 0777);
    s_file = fopen("sdmc:/switch/nxgo/nxgo.log", "w");
}

void log_exit(void)
{
    if (s_nxlink_sock >= 0)
        close(s_nxlink_sock);
    if (s_file)
        fclose(s_file);
    s_file = NULL;
    socketExit();
}

void log_printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (s_nxlink_sock >= 0) {
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    }
    if (s_file) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        fprintf(s_file, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
        va_start(args, fmt);
        vfprintf(s_file, fmt, args);
        va_end(args);
        fflush(s_file);
    }
}
