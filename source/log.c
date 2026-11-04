#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <switch.h>

#include "log.h"

static int s_nxlink_sock = -1;

void log_init(void)
{
    // stdout reste sur la console à l'écran ; stderr part vers nxlink s'il est là
    if (R_SUCCEEDED(socketInitializeDefault()))
        s_nxlink_sock = nxlinkConnectToHost(false, true);
}

void log_exit(void)
{
    if (s_nxlink_sock >= 0)
        close(s_nxlink_sock);
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
}
