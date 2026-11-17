/* Flashpoint PS5 - PC test build: the console calls only get logged.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "util.h"

void plat_init(void) {}

void plat_notify(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  LOG("notify: %s", msg);
}

int plat_open_browser(const char *url) {
  LOG("platform: would open the browser at %s", url);
  return 0;
}

int plat_launch_title(const char *title_id, const char *args) {
  LOG("platform: would launch app %s %s", title_id, args ? args : "");
  return 0;
}

int plat_lan_ip(char *buf, size_t size) {
  struct ifaddrs *ifa_list = NULL;
  str_lcpy(buf, "127.0.0.1", size);
  if (getifaddrs(&ifa_list)) return -1;
  for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, sizeof ip);
    if (!strncmp(ip, "127.", 4)) continue;
    str_lcpy(buf, ip, size);
    break;
  }
  freeifaddrs(ifa_list);
  return 0;
}
