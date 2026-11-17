/* Flashpoint PS5 - settings (fpps5.json) and Flashpoint folder layout.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/cjson/cJSON.h"
#include "version.h"

fp_config_t g_cfg;

#ifdef __PROSPERO__
static const char *k_config_candidates[] = {
  "fpps5.json", /* the homebrew folder: the Homebrew Launcher starts us there */
  "/data/fpps5/fpps5.json",
  "/data/homebrew/FlashpointPS5/fpps5.json",
  "/mnt/usb0/homebrew/FlashpointPS5/fpps5.json",
  "/mnt/usb1/homebrew/FlashpointPS5/fpps5.json",
  "/mnt/ext0/homebrew/FlashpointPS5/fpps5.json",
  NULL,
};
/* Where a copied Flashpoint folder is looked for, in order. */
static const char *k_root_candidates[] = {
  "/data/flashpoint",
  "/mnt/usb0/flashpoint", "/mnt/usb1/flashpoint", "/mnt/usb2/flashpoint",
  "/mnt/usb3/flashpoint", "/mnt/ext0/flashpoint", "/mnt/ext1/flashpoint",
  "/mnt/usb0/Flashpoint", "/mnt/usb1/Flashpoint", "/mnt/ext0/Flashpoint",
  NULL,
};
#else
static const char *k_config_candidates[] = {"./fpps5.json", NULL};
static const char *k_root_candidates[] = {"./flashpoint", "./Flashpoint", NULL};
#endif

static void defaults(void) {
  memset(&g_cfg, 0, sizeof g_cfg);
#ifdef __PROSPERO__
  str_lcpy(g_cfg.flashpoint_path, "/data/flashpoint", sizeof g_cfg.flashpoint_path);
  str_lcpy(g_cfg.run_path, "/data/ruffle/flashpoint", sizeof g_cfg.run_path);
  str_lcpy(g_cfg.log_path, "/data/fpps5/fpps5.log", sizeof g_cfg.log_path);
  g_cfg.open_browser = 1;
#else
  str_lcpy(g_cfg.flashpoint_path, "./flashpoint", sizeof g_cfg.flashpoint_path);
  str_lcpy(g_cfg.run_path, "./run", sizeof g_cfg.run_path);
  g_cfg.open_browser = 0;
#endif
  str_lcpy(g_cfg.ruffle_dir, "/data/ruffle", sizeof g_cfg.ruffle_dir);
  str_lcpy(g_cfg.ruffle_title_id, "PPSA68091", sizeof g_cfg.ruffle_title_id);
  str_lcpy(g_cfg.ruffle_mode, "auto", sizeof g_cfg.ruffle_mode);
  g_cfg.ui_port = 22600;
  g_cfg.game_server_port = 22500;
  g_cfg.filter_extreme_default = 1;
  g_cfg.keep_run_dirs = 4;
  /* Same defaults the Flashpoint launcher ships with. */
  str_lcpy(g_cfg.game_data_sources[0], "https://download.flashpointarchive.org/gib-roms/Games/",
           sizeof g_cfg.game_data_sources[0]);
  g_cfg.n_game_data_sources = 1;
  str_lcpy(g_cfg.images_url, "https://infinity.flashpointarchive.org/Flashpoint/Data/Images/",
           sizeof g_cfg.images_url);
  str_lcpy(g_cfg.legacy_fallback_url, "https://infinity.flashpointarchive.org/Flashpoint/Legacy/htdocs/",
           sizeof g_cfg.legacy_fallback_url);
  str_lcpy(g_cfg.online_api, "https://db-api.unstable.life", sizeof g_cfg.online_api);
}

void config_derive_paths(void) {
  char data[FP_PATH_MAX];
  path_join(data, sizeof data, g_cfg.flashpoint_path, "Data");
  path_join(g_cfg.db_path, sizeof g_cfg.db_path, data, "flashpoint.sqlite");
  path_join(g_cfg.games_path, sizeof g_cfg.games_path, data, "Games");
  path_join(g_cfg.images_path, sizeof g_cfg.images_path, data, "Images");
  path_join(g_cfg.htdocs_path, sizeof g_cfg.htdocs_path, g_cfg.flashpoint_path, "Legacy/htdocs");
}

static void get_str(cJSON *root, const char *key, char *dst, size_t size) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsString(v) && v->valuestring) str_lcpy(dst, v->valuestring, size);
}

static void get_int(cJSON *root, const char *key, int *dst) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsNumber(v)) *dst = v->valueint;
  else if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

static int apply_file(const char *path) {
  char *text = read_text_file(path, NULL);
  if (!text) return -1;
  cJSON *root = cJSON_Parse(text);
  free(text);
  if (!root) {
    LOG("config: %s is not valid JSON, ignoring it", path);
    return -1;
  }
  get_str(root, "flashpointPath", g_cfg.flashpoint_path, sizeof g_cfg.flashpoint_path);
  get_str(root, "runPath", g_cfg.run_path, sizeof g_cfg.run_path);
  get_str(root, "wwwPath", g_cfg.www_path, sizeof g_cfg.www_path);
  get_str(root, "logPath", g_cfg.log_path, sizeof g_cfg.log_path);
  get_str(root, "imagesUrl", g_cfg.images_url, sizeof g_cfg.images_url);
  get_str(root, "legacyFallbackUrl", g_cfg.legacy_fallback_url, sizeof g_cfg.legacy_fallback_url);
  get_str(root, "onlineApi", g_cfg.online_api, sizeof g_cfg.online_api);
  get_str(root, "ruffleTitleId", g_cfg.ruffle_title_id, sizeof g_cfg.ruffle_title_id);
  get_str(root, "ruffleDir", g_cfg.ruffle_dir, sizeof g_cfg.ruffle_dir);
  get_str(root, "ruffleMode", g_cfg.ruffle_mode, sizeof g_cfg.ruffle_mode);
  get_int(root, "uiPort", &g_cfg.ui_port);
  get_int(root, "gameServerPort", &g_cfg.game_server_port);
  get_int(root, "openBrowser", &g_cfg.open_browser);
  get_int(root, "filterExtreme", &g_cfg.filter_extreme_default);
  get_int(root, "keepRunDirs", &g_cfg.keep_run_dirs);

  cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "gameDataSources");
  if (cJSON_IsArray(src)) {
    g_cfg.n_game_data_sources = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, src) {
      const char *url = NULL;
      if (cJSON_IsString(it)) url = it->valuestring;
      else if (cJSON_IsObject(it)) {
        /* launcher-style {"name":..,"arguments":["https://..."]} */
        cJSON *args = cJSON_GetObjectItemCaseSensitive(it, "arguments");
        cJSON *first = cJSON_IsArray(args) ? cJSON_GetArrayItem(args, 0) : NULL;
        if (cJSON_IsString(first)) url = first->valuestring;
      }
      if (url && g_cfg.n_game_data_sources < FP_MAX_SOURCES) {
        str_lcpy(g_cfg.game_data_sources[g_cfg.n_game_data_sources++], url, 512);
      }
    }
  }
  cJSON_Delete(root);
  str_lcpy(g_cfg.config_file, path, sizeof g_cfg.config_file);
  return 0;
}

static int has_db(const char *root) {
  char p[FP_PATH_MAX];
  path_join(p, sizeof p, root, "Data/flashpoint.sqlite");
  return is_file(p);
}

int config_load(const char *explicit_path, const char *root_override) {
  defaults();
  int explicit_root = 0;
  if (explicit_path) {
    if (apply_file(explicit_path)) LOG("config: can't read %s", explicit_path);
  } else {
    for (int i = 0; k_config_candidates[i]; i++) {
      if (!apply_file(k_config_candidates[i])) break;
    }
  }
  if (g_cfg.config_file[0]) {
    /* a configured path wins over auto-detection */
    char *text = read_text_file(g_cfg.config_file, NULL);
    explicit_root = text && strstr(text, "\"flashpointPath\"");
    free(text);
  }
  if (root_override) {
    str_lcpy(g_cfg.flashpoint_path, root_override, sizeof g_cfg.flashpoint_path);
    explicit_root = 1;
  }
  if (!explicit_root || !has_db(g_cfg.flashpoint_path)) {
    for (int i = 0; k_root_candidates[i]; i++) {
      if (has_db(k_root_candidates[i])) {
        str_lcpy(g_cfg.flashpoint_path, k_root_candidates[i], sizeof g_cfg.flashpoint_path);
        break;
      }
    }
  }
  config_derive_paths();
  return 0;
}

char *config_to_json(void) {
  sb_t sb;
  sb_init(&sb);
  sb_append(&sb, "{\"version\":");
  sb_json_str(&sb, FPPS5_VERSION);
#ifdef __PROSPERO__
  sb_append(&sb, ",\"ps5\":true");
#else
  sb_append(&sb, ",\"ps5\":false");
#endif
  sb_append(&sb, ",\"flashpointPath\":");
  sb_json_str(&sb, g_cfg.flashpoint_path);
  sb_append(&sb, ",\"dbPath\":");
  sb_json_str(&sb, g_cfg.db_path);
  sb_append(&sb, ",\"runPath\":");
  sb_json_str(&sb, g_cfg.run_path);
  sb_append(&sb, ",\"configFile\":");
  sb_json_str(&sb, g_cfg.config_file);
  sb_printf(&sb, ",\"uiPort\":%d,\"gameServerPort\":%d", g_cfg.ui_port, g_cfg.game_server_port);
  sb_printf(&sb, ",\"filterExtreme\":%s", g_cfg.filter_extreme_default ? "true" : "false");
  sb_append(&sb, ",\"onlineApi\":");
  sb_json_str(&sb, g_cfg.online_api);
  sb_append(&sb, ",\"imagesUrl\":");
  sb_json_str(&sb, g_cfg.images_url);
  sb_append(&sb, ",\"gameDataSources\":[");
  for (int i = 0; i < g_cfg.n_game_data_sources; i++) {
    if (i) sb_append(&sb, ",");
    sb_json_str(&sb, g_cfg.game_data_sources[i]);
  }
  sb_append(&sb, "],\"ruffleTitleId\":");
  sb_json_str(&sb, g_cfg.ruffle_title_id);
  sb_append(&sb, ",\"ruffleMode\":");
  sb_json_str(&sb, g_cfg.ruffle_mode);
  sb_append(&sb, "}");
  return sb_steal(&sb);
}
