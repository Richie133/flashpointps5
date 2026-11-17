/* Flashpoint PS5 - deciding how an entry can be played on the console and
 * handing it to a player.
 *
 * Flash  -> Ruffle Flash PS5 (github.com/ZiZc3/Ruffle-Flash-PS5).
 *           "direct": write /data/ruffle/launch.txt and start the app; a
 *           patched build (ruffle-ps5/ in this repo) starts that game at
 *           once and maps http:// requests onto the unpacked files.
 *           "export": copy the game into /data/ruffle/games/<Title>/ (and
 *           its logo as the cover) for the stock app's library.
 * HTML5  -> the PS5 browser, pointed at our game server.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "launch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "gamedata.h"
#include "platform.h"
#include "util.h"

static int url_path_ends_with(const char *url, const char *ext) {
  char tmp[2048];
  str_lcpy(tmp, url, sizeof tmp);
  tmp[strcspn(tmp, "?#")] = 0;
  return str_ends_with_ci(tmp, ext);
}

static int is_http(const char *url) {
  return str_starts_with_ci(url, "http://") || str_starts_with_ci(url, "https://");
}

static int is_browser_app(const char *ap) {
  static const char *k[] = {"basilisk", "fpnavigator", ":browser-mode:", "chrome", "firefox",
                            "waterfox", "palemoon", "browser", NULL};
  for (int i = 0; k[i]; i++) {
    if (str_contains_ci(ap, k[i])) return 1;
  }
  return 0;
}

launch_kind_t launch_kind(const launch_info_t *li, char *reason, size_t rlen) {
  const char *ap = li->application_path, *lc = li->launch_command;
  if (reason && rlen) reason[0] = 0;
  if (!strcmp(ap, ":message:")) return LK_MESSAGE;
  if (!strcmp(ap, ":extras:")) return LK_EXTRAS;
  if (is_http(lc) && (url_path_ends_with(lc, ".swf") || url_path_ends_with(lc, ".spl")))
    return LK_FLASH;
  if (str_contains_ci(li->platform, "HTML5") && is_http(lc)) {
    if (reason) {
      snprintf(reason, rlen,
               "Opens in the PS5 browser. It has no WebGL or Web Audio, so some games "
               "won't draw or will be silent.");
    }
    return LK_HTML5;
  }
  if (reason) {
    if (str_contains_ci(li->platform, "Flash") && is_browser_app(ap)) {
      snprintf(reason, rlen,
               "This Flash entry starts from a web page, which the PS5 can't run yet. "
               "Play it on a PC, or from this page on a phone/PC browser.");
    } else {
      snprintf(reason, rlen, "There's no %s player for the PS5 yet.",
               li->platform[0] ? li->platform : "player for this platform");
    }
  }
  return LK_UNSUPPORTED;
}

const char *launch_kind_name(launch_kind_t k) {
  switch (k) {
  case LK_FLASH: return "flash";
  case LK_HTML5: return "html5";
  case LK_MESSAGE: return "message";
  case LK_EXTRAS: return "extras";
  default: return "unsupported";
  }
}

int launch_ruffle_direct_supported(void) {
  char p[FP_PATH_MAX];
  path_join(p, sizeof p, g_cfg.ruffle_dir, ".flashpoint-launch");
  return is_file(p);
}

static void url_encode_path(sb_t *sb, const char *path) {
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    if (isalnum(*p) || strchr("/-_.~!$&'()*+,;=:@", *p)) sb_appendn(sb, (const char *)p, 1);
    else sb_printf(sb, "%%%02X", *p);
  }
}

/* "/host/path?query" as served by the game server. */
static void content_url(const launch_info_t *li, sb_t *out) {
  char host[256], path[2048];
  if (url_split(li->launch_command, host, sizeof host, path, sizeof path)) return;
  sb_append(out, "/");
  sb_append(out, host);
  sb_append(out, "/");
  url_encode_path(out, path);
  const char *q = strchr(li->launch_command, '?');
  if (q) {
    size_t n = strcspn(q, "#");
    sb_appendn(out, q, n);
  }
}

char *launch_kind_json(const launch_info_t *li) {
  char reason[512];
  launch_kind_t k = launch_kind(li, reason, sizeof reason);
  sb_t sb;
  sb_init(&sb);
  sb_printf(&sb, "{\"kind\":\"%s\",\"playable\":%s,\"reason\":", launch_kind_name(k),
            (k == LK_FLASH || k == LK_HTML5 || k == LK_MESSAGE) ? "true" : "false");
  sb_json_str(&sb, reason);
  sb_append(&sb, ",\"contentUrl\":");
  sb_t cu;
  sb_init(&cu);
  content_url(li, &cu);
  sb_json_str(&sb, cu.buf);
  sb_free(&cu);
  sb_printf(&sb, ",\"gameServerPort\":%d,\"ruffleMode\":\"%s\",\"ruffleSupport\":", g_cfg.game_server_port,
            launch_ruffle_direct_supported() ? "direct" : "export");
  sb_json_str(&sb, li->ruffle_support);
  sb_append(&sb, "}");
  return sb_steal(&sb);
}

/* Finds the entry's start file: unpacked GameZIP first, then Legacy htdocs
 * (fetched from the Infinity legacy server if it isn't there). */
static int resolve_start_file(const launch_info_t *li, char *out, size_t size) {
  char host[256], path[2048];
  if (url_split(li->launch_command, host, sizeof host, path, sizeof path)) return -1;
  char rel[2400];
  snprintf(rel, sizeof rel, "%s/%s", host, path);
  if (!rel_path_is_safe(rel)) return -1;
  const char *idx[] = {"", "index.html", "index.htm", NULL};
  for (int i = 0; idx[i]; i++) {
    if (i && path[0] && !str_ends_with_ci(path, "/")) break;
    char r[2600];
    snprintf(r, sizeof r, "%s%s", rel, idx[i]);
    if (li->has_data) {
      char dir[FP_PATH_MAX];
      gd_run_dir(li->game_id, dir, sizeof dir);
      path_join(out, size, dir, r);
      if (is_file(out)) return 0;
    }
    path_join(out, size, g_cfg.htdocs_path, r);
    if (is_file(out)) return 0;
  }
  return gd_ensure_legacy_file(host, path, out, size);
}

static void clean_line(char *s) {
  for (; *s; s++) {
    if (*s == '\n' || *s == '\r') *s = ' ';
  }
}

static void safe_title(const char *title, const char *fallback, char *out, size_t size) {
  size_t o = 0;
  int space = 0;
  for (const unsigned char *p = (const unsigned char *)title; *p && o + 1 < size && o < 64; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '\'' || *p == '(' || *p == ')') {
      if (space && o) out[o++] = ' ';
      space = 0;
      out[o++] = (char)*p;
    } else if (*p == ' ' || *p == ':' || *p == '/' || *p == '&' || *p == ',') {
      space = 1;
    }
  }
  while (o && (out[o - 1] == '.' || out[o - 1] == ' ')) o--;
  out[o] = 0;
  if (!o) str_lcpy(out, fallback, size);
}

/* Ruffle Flash PS5 keys covers by the file stem (covers.rs: game_key). */
static void ruffle_cover_key(const char *stem, char *out, size_t size) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)stem; *p && o + 1 < size; p++) {
    out[o++] = (isalnum(*p) || *p == '.' || *p == '_' || *p == '-') ? (char)*p : '_';
  }
  out[o] = 0;
}

static int ruffle_direct(const launch_info_t *li, const char *swf, char *msg, size_t mlen) {
  char title[512], url[2048];
  str_lcpy(title, li->title, sizeof title);
  clean_line(title);
  str_lcpy(url, li->launch_command, sizeof url);
  clean_line(url);
  sb_t sb;
  sb_init(&sb);
  sb_printf(&sb, "# written by Flashpoint PS5\ntitle=%s\nid=%s\nswf=%s\nurl=%s\n", title, li->game_id,
            swf, url);
  const char *q = strchr(url, '?');
  if (q) sb_printf(&sb, "query=%.*s\n", (int)strcspn(q + 1, "#"), q + 1);
  if (li->has_data) {
    char dir[FP_PATH_MAX];
    gd_run_dir(li->game_id, dir, sizeof dir);
    sb_printf(&sb, "root=%s\n", dir);
  }
  sb_printf(&sb, "root=%s\nexit=1\n", g_cfg.htdocs_path);
  char path[FP_PATH_MAX];
  path_join(path, sizeof path, g_cfg.ruffle_dir, "launch.txt");
  int rc = write_text_file(path, sb.buf);
  sb_free(&sb);
  if (rc) {
    snprintf(msg, mlen, "Can't write %s", path);
    return -1;
  }
  if (plat_launch_title(g_cfg.ruffle_title_id, NULL)) {
    unlink(path);
    snprintf(msg, mlen,
             "Couldn't start Ruffle Flash (%s). Is it installed and mounted "
             "(ShadowMountPlus + kstuff)?",
             g_cfg.ruffle_title_id);
    return -1;
  }
  snprintf(msg, mlen, "Starting %s in Ruffle Flash…", li->title);
  return 0;
}

static int ruffle_export(const launch_info_t *li, const char *swf, char *msg, size_t mlen,
                         char *exported, size_t elen) {
  char name[96];
  safe_title(li->title, li->game_id, name, sizeof name);
  char games[FP_PATH_MAX], dest[FP_PATH_MAX];
  path_join(games, sizeof games, g_cfg.ruffle_dir, "games");
  path_join(dest, sizeof dest, games, name);
  remove_tree(dest);

  char src_dir[FP_PATH_MAX];
  str_lcpy(src_dir, swf, sizeof src_dir);
  char *slash = strrchr(src_dir, '/');
  if (!slash) return -1;
  *slash = 0;
  const char *base = slash + 1;
  /* A GameZIP holds only this game, so take its whole folder; Legacy
   * htdocs folders are shared between games, so only the files beside it. */
  if (copy_tree(src_dir, dest, li->has_data)) {
    snprintf(msg, mlen, "Copying the game into %s failed (disk full?)", dest);
    remove_tree(dest);
    return -1;
  }
  char from[FP_PATH_MAX], to[FP_PATH_MAX], swf_name[128];
  snprintf(swf_name, sizeof swf_name, "%s.swf", name);
  path_join(from, sizeof from, dest, base);
  path_join(to, sizeof to, dest, swf_name);
  if (strcmp(base, swf_name) && !is_file(to)) rename(from, to);
  const char *final_path = is_file(to) ? to : from;
  str_lcpy(exported, final_path, elen);

  /* Flashpoint logo as the Ruffle cover, named after the file stem */
  char logo[FP_PATH_MAX];
  if (!gd_ensure_image(li->logo_path, logo, sizeof logo)) {
    char stem[128], key[128], covers[FP_PATH_MAX], cover[FP_PATH_MAX], file[160];
    str_lcpy(stem, strrchr(final_path, '/') + 1, sizeof stem);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    ruffle_cover_key(stem, key, sizeof key);
    path_join(covers, sizeof covers, g_cfg.ruffle_dir, "covers");
    snprintf(file, sizeof file, "%s.png", key);
    path_join(cover, sizeof cover, covers, file);
    copy_file(logo, cover);
  }
  if (plat_launch_title(g_cfg.ruffle_title_id, NULL)) {
    snprintf(msg, mlen,
             "Added to Ruffle Flash as \"%s\", but the app didn't start (%s). "
             "Open Ruffle Flash from the home screen.",
             name, g_cfg.ruffle_title_id);
    return 0;
  }
  snprintf(msg, mlen, "Added to Ruffle Flash as \"%s\". Pick it in Ruffle's library.", name);
  return 0;
}

static char *status_json(const char *status, const char *message) {
  sb_t sb;
  sb_init(&sb);
  sb_append(&sb, "{\"status\":");
  sb_json_str(&sb, status);
  sb_append(&sb, ",\"message\":");
  sb_json_str(&sb, message);
  sb_append(&sb, "}");
  return sb_steal(&sb);
}

char *launch_play(const launch_info_t *li, const http_req_t *req) {
  char reason[512], err[512];
  launch_kind_t kind = launch_kind(li, reason, sizeof reason);
  if (kind == LK_MESSAGE) return status_json("message", li->launch_command);
  if (kind == LK_EXTRAS) {
    char m[1200];
    snprintf(m, sizeof m, "Extras for this entry are in %s/Extras/%s on a PC Flashpoint install.",
             g_cfg.flashpoint_path, li->launch_command);
    return status_json("message", m);
  }
  if (kind == LK_UNSUPPORTED) return status_json("unsupported", reason);

  int prep = gd_prepare(li, err, sizeof err);
  if (prep == GD_ERROR) return status_json("error", err);
  if (prep == GD_PENDING) {
    char *jobs = gd_jobs_json();
    sb_t sb;
    sb_init(&sb);
    sb_append(&sb, "{\"status\":\"pending\",\"gameId\":");
    sb_json_str(&sb, li->game_id);
    sb_printf(&sb, ",\"jobs\":%s}", jobs ? jobs : "[]");
    free(jobs);
    return sb_steal(&sb);
  }

  char start[FP_PATH_MAX];
  if (resolve_start_file(li, start, sizeof start)) {
    char m[2400];
    snprintf(m, sizeof m, "The start file for this entry (%s) isn't in its game data.",
             li->launch_command);
    return status_json("error", m);
  }
  lib_record_play(li->game_id);
  if (li->has_data) gd_touch_run_dir(li->game_id);

  sb_t sb;
  sb_init(&sb);
  if (kind == LK_HTML5) {
    sb_t cu;
    sb_init(&cu);
    content_url(li, &cu);
    sb_append(&sb, "{\"status\":\"open\",\"kind\":\"html5\",\"url\":");
    char url[4600];
    snprintf(url, sizeof url, "http://%s:%d%s", req->host[0] ? req->host : "127.0.0.1",
             g_cfg.game_server_port, cu.buf);
    sb_json_str(&sb, url);
    sb_append(&sb, "}");
    sb_free(&cu);
    LOG("launch: %s -> %s", li->title, url);
    return sb_steal(&sb);
  }

  /* Flash */
  int direct = !strcmp(g_cfg.ruffle_mode, "direct") ||
               (!strcmp(g_cfg.ruffle_mode, "auto") && launch_ruffle_direct_supported());
  char msg[1024], exported[FP_PATH_MAX] = "";
  int rc = direct ? ruffle_direct(li, start, msg, sizeof msg)
                  : ruffle_export(li, start, msg, sizeof msg, exported, sizeof exported);
  LOG("launch: %s (%s, %s): %s", li->title, start, direct ? "direct" : "export", msg);
  if (rc) {
    sb_free(&sb);
    return status_json("error", msg);
  }
  plat_notify("%s", msg);
  sb_append(&sb, "{\"status\":\"launched\",\"kind\":\"flash\",\"mode\":");
  sb_json_str(&sb, direct ? "direct" : "export");
  sb_append(&sb, ",\"message\":");
  sb_json_str(&sb, msg);
  sb_append(&sb, ",\"file\":");
  sb_json_str(&sb, direct ? start : exported);
  sb_append(&sb, "}");
  return sb_steal(&sb);
}
