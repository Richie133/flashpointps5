/* Flashpoint PS5 - deciding how an entry can be played on the console and
 * handing it to a player: Ruffle Flash PS5 for Flash, the PS5 browser for
 * HTML5 (served by our game server).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "httpd.h"
#include "library.h"

typedef enum {
  LK_UNSUPPORTED = 0,
  LK_FLASH,
  LK_HTML5,
  LK_MESSAGE,
  LK_EXTRAS,
} launch_kind_t;

launch_kind_t launch_kind(const launch_info_t *li, char *reason, size_t rlen);
const char *launch_kind_name(launch_kind_t k);

/* {"kind":..,"playable":..,"reason":..} for the game viewer. */
char *launch_kind_json(const launch_info_t *li);

/* Runs a play request. Returns a JSON object (caller frees):
 *   {"status":"pending", ...job}     data still downloading/unpacking
 *   {"status":"launched", ...}       handed to Ruffle Flash PS5
 *   {"status":"open","url":...}      open this URL in the browser (HTML5)
 *   {"status":"message"|"unsupported"|"error","message":...} */
char *launch_play(const launch_info_t *li, const http_req_t *req);

/* True when the installed Ruffle Flash PS5 understands launch.txt. */
int launch_ruffle_direct_supported(void);
