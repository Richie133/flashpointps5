/* Flashpoint PS5 - downloads for the PC test build: runs the curl tool.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net.h"
#include "util.h"
#include "version.h"

int net_init(void) { return 0; }

int net_download(const char *url, const char *dest, net_progress_cb cb, void *ud, char *err,
                 size_t errlen) {
  char part[FP_PATH_MAX];
  snprintf(part, sizeof part, "%s.part", dest);
  mkdirs_for_file(dest);
  unlink(part);

  pid_t pid = fork();
  if (pid < 0) {
    snprintf(err, errlen, "fork failed: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    execlp("curl", "curl", "-fsSL", "--retry", "2", "--connect-timeout", "20", "-A",
           FPPS5_USER_AGENT, "-o", part, url, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0 && errno != EINTR) break;
    if (cb && cb(ud, file_size(part), -1)) {
      kill(pid, SIGTERM);
      waitpid(pid, &status, 0);
      unlink(part);
      snprintf(err, errlen, "cancelled");
      return -1;
    }
    usleep(200 * 1000);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    unlink(part);
    snprintf(err, errlen, "download failed (curl exit %d): %s",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1, url);
    return -1;
  }
  if (cb) cb(ud, file_size(part), file_size(part));
  if (rename(part, dest)) {
    snprintf(err, errlen, "can't move download into place: %s", strerror(errno));
    unlink(part);
    return -1;
  }
  return 0;
}
