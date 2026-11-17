/* Flashpoint PS5 - HTTPS downloads (sceHttp on PS5, curl on a PC).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

/* Return non-zero to abort. total is -1 when unknown. */
typedef int (*net_progress_cb)(void *ud, long long done, long long total);

int net_init(void);

/* Downloads url into dest (through dest.part, renamed when complete).
 * Returns 0 on success; on failure -1 and a message in err. */
int net_download(const char *url, const char *dest, net_progress_cb cb, void *ud, char *err,
                 size_t errlen);
