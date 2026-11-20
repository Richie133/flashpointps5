/* Flashpoint PS5 - the local game library: Flashpoint's own flashpoint.sqlite
 * (same schema as the Flashpoint Launcher / FPA-Rust), queried like the
 * Flashpoint Database API so the database frontend can browse it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "httpd.h"

int lib_open(const char *db_path);
int lib_is_open(void);
long long lib_game_count(void);
void lib_refresh_count(void); /* after the library changed */

/* db-api compatible JSON responses (strings are malloc'd, caller frees). */
char *lib_search_json(const http_req_t *req, long long *total_out);
char *lib_game_json(const char *id);
char *lib_addapps_json(const char *id);
char *lib_platforms_json(void);
char *lib_stats_json(void);

typedef struct game_data_info {
  long long id;
  char date_added[64];
  char sha256[80];
  long long size;
  int present;
  char path[1024];
  char parameters[256];
  char filename[128];     /* <gameId>-<ms>.zip, as the launcher names it */
} game_data_info_t;

typedef struct launch_info {
  char game_id[64];
  char title[512];
  char platform[128];
  char application_path[512];
  char launch_command[2048];
  char ruffle_support[32];
  char logo_path[512];          /* relative to Data/Images */
  int has_data;                 /* GameZIP (activeDataId set) */
  game_data_info_t data;
} launch_info_t;

/* Fills `out` for a game (or one of its additional apps when add_app_id is
 * set). Returns 0, or -1 when not found. */
int lib_launch_info(const char *game_id, const char *add_app_id, launch_info_t *out);

/* Bookkeeping the launcher does too. */
void lib_mark_data_present(long long game_data_id, const char *game_id, const char *path);
void lib_record_play(const char *game_id);

/* Builds the GameZIP file name the launcher uses: <gameId>-<epoch ms>.zip */
int lib_game_data_filename(const char *game_id, const char *date_added, char *out, size_t size);
