/* Flashpoint PS5 - the web UI, compiled into the payload (see tools/embed.py).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

typedef struct asset {
  const char *path; /* "/index.html" */
  const unsigned char *data;
  size_t len;
} asset_t;

const asset_t *asset_find(const char *path);
