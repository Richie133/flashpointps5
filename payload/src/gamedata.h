/* Flashpoint PS5 - game data: on-demand GameZIP downloads (like Flashpoint
 * Infinity), unpacking for players, images and Legacy htdocs files.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "library.h"
#include "util.h"

enum { GD_READY = 0, GD_PENDING = 1, GD_ERROR = -1 };

int gd_init(void);

/* Where a game's files get unpacked: <runPath>/<gameId> */
void gd_run_dir(const char *game_id, char *out, size_t size);
/* Path of the GameZIP if it's on disk. 0 when found. */
int gd_zip_path(const launch_info_t *li, char *out, size_t size);

/* Makes sure the game's data is downloaded, verified and unpacked.
 * Never blocks: GD_READY, GD_PENDING (a job is running) or GD_ERROR. */
int gd_prepare(const launch_info_t *li, char *err, size_t errlen);
char *gd_jobs_json(void);
int gd_cancel(const char *game_id);

/* Unpacked game folders, most recently played first (game server roots). */
int gd_recent_roots(char roots[][FP_PATH_MAX], int max);
void gd_touch_run_dir(const char *game_id);

/* Data/Images/<rel>, downloaded on demand. 0 when out_path is usable. */
int gd_ensure_image(const char *rel, char *out_path, size_t size);
/* Legacy/htdocs/<host>/<path>, downloaded on demand from the fallback. */
int gd_ensure_legacy_file(const char *host, const char *path, char *out_path, size_t size);

/* content/ listing of the active GameZIP, like the db-api /files endpoint. */
char *gd_files_json(const launch_info_t *li);
