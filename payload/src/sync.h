/* Flashpoint PS5 - downloads the Flashpoint library from Flashpoint's
 * metadata server, so no PC is needed.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

enum { SYNC_IDLE, SYNC_RUNNING, SYNC_DONE, SYNC_ERROR };

/* Starts (or continues) a download in the background; 0 when started or
 * already running. A finished library only fetches what changed. */
int sync_start(void);

/* {"state", "running", "done", "total", "message", "source", "syncedAt",
 *  "entries"}. Caller frees. */
char *sync_status_json(void);
