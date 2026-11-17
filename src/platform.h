/* Flashpoint PS5 - the few things that differ between the console and a PC.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

void plat_init(void);
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Opens the system web browser at url (PS5), or just logs it (PC). */
int plat_open_browser(const char *url);
/* Starts an installed app by title ID with optional space-separated args. */
int plat_launch_title(const char *title_id, const char *args);
/* First non-loopback IPv4 address, for "open this on your phone". */
int plat_lan_ip(char *buf, size_t size);
