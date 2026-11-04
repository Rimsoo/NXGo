// NXGo: stubs for POSIX calls KataGo uses but Horizon does not provide.
#include <errno.h>
#include <string.h>
#include "include/switch_compat.h"

int gethostname(char *name, size_t len) {
  if (len < 7) { errno = ENAMETOOLONG; return -1; }
  strcpy(name, "switch");
  return 0;
}

ssize_t readlink(const char *path, char *buf, size_t len) {
  (void)path; (void)buf; (void)len;
  errno = ENOSYS;
  return -1;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  *result = NULL;
  return ENOENT;
}
