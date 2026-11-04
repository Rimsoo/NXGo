// NXGo: force-included in every KataGo translation unit on Switch.
// Declares POSIX functions that newlib/libnx lack; stubs live in switch_compat.c.
#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
struct passwd;
int gethostname(char *name, size_t len);
ssize_t readlink(const char *path, char *buf, size_t len);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
#ifdef __cplusplus
}
#endif
