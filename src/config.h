/* Flashpoint PS5 - settings (fpps5.json) and Flashpoint folder layout.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "util.h"

#define FP_MAX_SOURCES 4

typedef struct fp_config {
  /* Flashpoint folder (the one holding Data/ and Legacy/), as on a PC. */
  char flashpoint_path[FP_PATH_MAX];
  char db_path[FP_PATH_MAX];       /* Data/flashpoint.sqlite */
  char games_path[FP_PATH_MAX];    /* Data/Games (GameZIPs) */
  char images_path[FP_PATH_MAX];   /* Data/Images */
  char htdocs_path[FP_PATH_MAX];   /* Legacy/htdocs */
  /* Where game files get unpacked for players (must be readable by Ruffle). */
  char run_path[FP_PATH_MAX];
  char www_path[FP_PATH_MAX];      /* optional on-disk UI override */
  char log_path[FP_PATH_MAX];
  char config_file[FP_PATH_MAX];   /* file this was loaded from, if any */

  int ui_port;
  int game_server_port;
  int open_browser;                /* launch the PS5 browser on start */
  int filter_extreme_default;
  int keep_run_dirs;               /* unpacked games kept around (LRU) */

  char game_data_sources[FP_MAX_SOURCES][512];
  int n_game_data_sources;
  char images_url[512];            /* on-demand logos/screenshots */
  char legacy_fallback_url[512];   /* on-demand Legacy htdocs files */
  char online_api[512];            /* flashpoint-database API (used by the UI) */

  /* Ruffle Flash PS5 (ZiZc3/Ruffle-Flash-PS5) hand-off. */
  char ruffle_title_id[16];
  char ruffle_dir[FP_PATH_MAX];    /* /data/ruffle */
  char ruffle_mode[16];            /* auto | direct | export */
} fp_config_t;

extern fp_config_t g_cfg;

/* Loads defaults, then the first fpps5.json found (or `explicit_path`),
 * then picks a Flashpoint folder. Returns 0 even without a database. */
int config_load(const char *explicit_path, const char *root_override);

/* Re-derives the Data/... paths from flashpoint_path. */
void config_derive_paths(void);

/* JSON for /api/config. Caller frees. */
char *config_to_json(void);
