/* Flashpoint PS5 - the UI server: the merged frontend plus a JSON API.
 * /api/search, /api/addapps, /api/platforms, /api/stats and /api/files
 * answer like FlashpointProject/flashpoint-database-api, from the local
 * flashpoint.sqlite; the rest is the launcher side (play, downloads).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "assets.h"
#include "config.h"
#include "gamedata.h"
#include "launch.h"
#include "library.h"
#include "platform.h"
#include "sync.h"
#include "web.h"
#include "util.h"
#include "version.h"

volatile int g_quit_requested;

static void respond_owned(http_req_t *req, int status, char *json) {
  if (!json) {
    http_respond_json(req, 404, "{\"error\":\"not found\"}");
    return;
  }
  http_respond_json(req, status, json);
  free(json);
}

static void api_config(http_req_t *req) {
  char *cfg = config_to_json();
  char ip[64];
  plat_lan_ip(ip, sizeof ip);
  sb_t sb;
  sb_init(&sb);
  sb_appendn(&sb, cfg, strlen(cfg) - 1); /* reopen the object */
  sb_printf(&sb, ",\"dbOpen\":%s,\"gameCount\":%lld,\"lanIp\":", lib_is_open() ? "true" : "false",
            lib_game_count());
  sb_json_str(&sb, ip);
  sb_printf(&sb, ",\"ruffleDirect\":%s}", launch_ruffle_direct_supported() ? "true" : "false");
  free(cfg);
  respond_owned(req, 200, sb_steal(&sb));
}

static void api_search(http_req_t *req) {
  long long total = 0;
  char *json = lib_search_json(req, &total);
  char extra[160];
  snprintf(extra, sizeof extra,
           "X-Total-Count: %lld\r\nAccess-Control-Expose-Headers: X-Total-Count\r\n"
           "Cache-Control: no-store\r\n",
           total);
  if (http_param_bool(req, "envelope", 0)) {
    /* {"total":N,"results":[...]} for clients that can't read headers */
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"total\":%lld,\"results\":", total);
    sb_append(&sb, json);
    sb_append(&sb, "}");
    free(json);
    json = sb_steal(&sb);
  }
  http_respond(req, 200, "application/json; charset=utf-8", json, strlen(json), extra);
  free(json);
}

static int get_id(http_req_t *req, char *id, size_t size) {
  if (!http_param(req, "id", id, size) || !id[0]) {
    http_respond_json(req, 400, "{\"error\":\"missing id\"}");
    return -1;
  }
  return 0;
}

static void api_game(http_req_t *req) {
  char id[80];
  if (get_id(req, id, sizeof id)) return;
  char *game = lib_game_json(id);
  if (!game) {
    http_respond_json(req, 404, "{\"error\":\"no such entry in the local library\"}");
    return;
  }
  launch_info_t li;
  char *play = NULL;
  if (!lib_launch_info(id, NULL, &li)) play = launch_kind_json(&li);
  sb_t sb;
  sb_init(&sb);
  sb_printf(&sb, "{\"game\":%s,\"play\":%s}", game, play ? play : "null");
  free(game);
  free(play);
  respond_owned(req, 200, sb_steal(&sb));
}

static void api_play(http_req_t *req) {
  char id[80], add_app[80] = "";
  if (get_id(req, id, sizeof id)) return;
  http_param(req, "addApp", add_app, sizeof add_app);
  launch_info_t li;
  if (lib_launch_info(id, add_app, &li)) {
    http_respond_json(req, 404, "{\"status\":\"error\",\"message\":\"That entry isn't in the local library.\"}");
    return;
  }
  respond_owned(req, 200, launch_play(&li, req));
}

/* Download/unpack without launching anything (for playing on another device). */
static void api_prepare(http_req_t *req) {
  char id[80];
  if (get_id(req, id, sizeof id)) return;
  launch_info_t li;
  if (lib_launch_info(id, NULL, &li)) {
    http_respond_json(req, 404, "{\"status\":\"error\",\"message\":\"That entry isn't in the local library.\"}");
    return;
  }
  char err[512] = "";
  int rc = gd_prepare(&li, err, sizeof err);
  sb_t sb;
  sb_init(&sb);
  if (rc == GD_READY) {
    sb_append(&sb, "{\"status\":\"ready\"}");
  } else if (rc == GD_PENDING) {
    char *jobs = gd_jobs_json();
    sb_printf(&sb, "{\"status\":\"pending\",\"jobs\":%s}", jobs ? jobs : "[]");
    free(jobs);
  } else {
    sb_append(&sb, "{\"status\":\"error\",\"message\":");
    sb_json_str(&sb, err);
    sb_append(&sb, "}");
  }
  respond_owned(req, 200, sb_steal(&sb));
}

static void api_files(http_req_t *req) {
  char id[80];
  if (get_id(req, id, sizeof id)) return;
  launch_info_t li;
  if (lib_launch_info(id, NULL, &li)) {
    http_respond_json(req, 200, "[]");
    return;
  }
  respond_owned(req, 200, gd_files_json(&li));
}

static void api_sync(http_req_t *req) {
  if (!strcmp(req->method, "POST") || http_param_bool(req, "start", 0)) sync_start();
  respond_owned(req, 200, sync_status_json());
}

static void api_web(http_req_t *req, const char *what) {
  char source[32] = "", list[48] = "", q[256] = "", url[2048] = "", title[256] = "";
  http_param(req, "source", source, sizeof source);
  if (!strcmp(what, "sources")) {
    respond_owned(req, 200, web_sources_json());
  } else if (!strcmp(what, "list")) {
    http_param(req, "list", list, sizeof list);
    http_param(req, "q", q, sizeof q);
    respond_owned(req, 200,
                  web_list_json(source, list, q, (int)http_param_long(req, "page", 1),
                                http_param_bool(req, "adult", g_cfg.web_adult)));
  } else if (!strcmp(what, "thumb")) {
    char path[FP_PATH_MAX];
    if (!http_param(req, "u", url, sizeof url) || web_thumb_path(url, path, sizeof path) ||
        http_respond_file(req, path, "image/jpeg", "Cache-Control: max-age=86400\r\n"))
      http_respond_text(req, 404, "no picture\n");
  } else if (!strcmp(what, "play")) {
    http_param(req, "url", url, sizeof url);
    http_param(req, "title", title, sizeof title);
    respond_owned(req, 200, web_play_json(url, title, source));
  } else {
    http_respond_json(req, 404, "{\"error\":\"unknown endpoint\"}");
  }
}

/* The end of the service's log, for the app's Logs page. */
static void api_log(http_req_t *req) {
  size_t len = 0;
  char *text = g_cfg.log_path[0] ? read_text_file(g_cfg.log_path, &len) : NULL;
  const char *start = text ? text : "";
  if (len > 24000) {
    start = text + len - 24000;
    const char *nl = strchr(start, '\n');
    if (nl) start = nl + 1;
  }
  http_respond(req, 200, "text/plain; charset=utf-8", start, strlen(start), "Cache-Control: no-store\r\n");
  free(text);
}

static void serve_image(http_req_t *req) {
  const char *rel = req->path + strlen("/images/");
  char path[FP_PATH_MAX];
  if (gd_ensure_image(rel, path, sizeof path) ||
      http_respond_file(req, path, NULL, "Cache-Control: max-age=86400\r\n"))
    http_respond_text(req, 404, "no image\n");
}

static void serve_static(http_req_t *req) {
  char rel[1024];
  str_lcpy(rel, req->path, sizeof rel);
  if (!strcmp(rel, "/") || !rel[0]) str_lcpy(rel, "/index.html", sizeof rel);
  if (!rel_path_is_safe(rel + 1)) {
    http_respond_text(req, 403, "forbidden\n");
    return;
  }
  if (g_cfg.www_path[0]) {
    char p[FP_PATH_MAX];
    path_join(p, sizeof p, g_cfg.www_path, rel + 1);
    if (!http_respond_file(req, p, NULL, "Cache-Control: no-cache\r\n")) return;
  }
  const asset_t *a = asset_find(rel);
  if (a) {
    http_respond(req, 200, mime_for_path(rel), a->data, a->len, "Cache-Control: no-cache\r\n");
    return;
  }
  /* single-page app: unknown paths without an extension get the UI */
  if (!strchr(strrchr(rel, '/'), '.') && (a = asset_find("/index.html"))) {
    http_respond(req, 200, "text/html; charset=utf-8", a->data, a->len, NULL);
    return;
  }
  http_respond_text(req, 404, "not found\n");
}

void api_handle(http_req_t *req, void *ctx) {
  (void)ctx;
  const char *p = req->path;
  if (!strncmp(p, "/api/", 5)) {
    p += 5;
    if (!strcmp(p, "config")) api_config(req);
    else if (!strcmp(p, "search")) api_search(req);
    else if (!strcmp(p, "game")) api_game(req);
    else if (!strcmp(p, "play")) api_play(req);
    else if (!strcmp(p, "files")) api_files(req);
    else if (!strcmp(p, "prepare")) api_prepare(req);
    else if (!strcmp(p, "addapps")) {
      char id[80];
      if (!get_id(req, id, sizeof id)) respond_owned(req, 200, lib_addapps_json(id));
    } else if (!strcmp(p, "platforms")) respond_owned(req, 200, lib_platforms_json());
    else if (!strcmp(p, "stats")) respond_owned(req, 200, lib_stats_json());
    else if (!strcmp(p, "jobs")) respond_owned(req, 200, gd_jobs_json());
    else if (!strcmp(p, "sync")) api_sync(req);
    else if (!strcmp(p, "log")) api_log(req);
    else if (!strncmp(p, "web/", 4)) api_web(req, p + 4);
    else if (!strcmp(p, "cancel")) {
      char id[80];
      if (!get_id(req, id, sizeof id))
        http_respond_json(req, 200, gd_cancel(id) ? "{\"ok\":false}" : "{\"ok\":true}");
    } else if (!strcmp(p, "quit")) {
      http_respond_json(req, 200, "{\"ok\":true}");
      g_quit_requested = 1;
    } else {
      http_respond_json(req, 404, "{\"error\":\"unknown endpoint\"}");
    }
    return;
  }
  if (!strncmp(p, "/images/", 8)) {
    serve_image(req);
    return;
  }
  serve_static(req);
}
