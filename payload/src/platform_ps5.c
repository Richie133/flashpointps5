/* Flashpoint PS5 - console side: notifications, browser, launching apps.
 * App launching follows ps5-payload-dev/websrv (sys_launch_title).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "platform.h"
#include "util.h"

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char *title_id, char **argv, app_launch_ctx_t *ctx);

void plat_init(void) {
  if (sceUserServiceInitialize(0)) LOG("platform: sceUserServiceInitialize failed");
}

void plat_notify(const char *fmt, ...) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  LOG("notify: %s", req.message);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

int plat_open_browser(const char *url) {
  int rc = sceSystemServiceLaunchWebBrowser(url, 0);
  if (rc) LOG("platform: sceSystemServiceLaunchWebBrowser(%s) = %x", url, rc);
  return rc ? -1 : 0;
}

int plat_launch_title(const char *title_id, const char *args) {
  app_launch_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  int err;
  if ((err = sceUserServiceGetForegroundUser(&ctx.user_id))) {
    LOG("platform: no foreground user (%x)", err);
    return -1;
  }
  /* Only one big app runs at a time; the running one makes room. */
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if (app_id > 0) sceSystemServiceKillApp(app_id, -1, 0, 0);

  char *argv[32] = {0};
  char *copy = args ? strdup(args) : NULL;
  int argc = 0;
  char *save = NULL;
  for (char *tok = copy ? strtok_r(copy, " ", &save) : NULL; tok && argc < 31;
       tok = strtok_r(NULL, " ", &save)) {
    argv[argc++] = tok;
  }
  err = sceSystemServiceLaunchApp(title_id, argv, &ctx);
  free(copy);
  if (err < 0) {
    LOG("platform: sceSystemServiceLaunchApp(%s) = %x", title_id, err);
    return -1;
  }
  return 0;
}

int plat_lan_ip(char *buf, size_t size) {
  struct ifaddrs *ifa_list = NULL;
  str_lcpy(buf, "127.0.0.1", size);
  if (getifaddrs(&ifa_list)) return -1;
  for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    struct sockaddr_in *in = (struct sockaddr_in *)ifa->ifa_addr;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
    if (!strncmp(ip, "127.", 4)) continue;
    str_lcpy(buf, ip, size);
    break;
  }
  freeifaddrs(ifa_list);
  return 0;
}
