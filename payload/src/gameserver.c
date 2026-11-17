/* Flashpoint PS5 - the game server: answers http://<host>/<path> requests
 * the way Flashpoint's game server does, from unpacked GameZIPs first and
 * then Legacy/htdocs (fetching missing Legacy files on demand).
 *
 * Two request styles work:
 *   proxy  GET http://www.example.com/game.swf HTTP/1.1   (Flashpoint style)
 *   path   GET /www.example.com/game.swf HTTP/1.1         (for a browser)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gameserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "gamedata.h"
#include "util.h"

#define MAX_ROOTS 9

static int try_roots(http_req_t *req, char roots[][FP_PATH_MAX], int n, const char *host,
                     const char *rel) {
  char key[FP_PATH_MAX * 2];
  snprintf(key, sizeof key, "%s/%s", host, rel);
  if (!host[0] || !rel_path_is_safe(key)) return -1;
  for (int i = 0; i < n; i++) {
    char p[FP_PATH_MAX];
    path_join(p, sizeof p, roots[i], key);
    if (is_dir(p)) {
      const char *idx[] = {"index.html", "index.htm", NULL};
      for (int k = 0; idx[k]; k++) {
        char q[FP_PATH_MAX];
        path_join(q, sizeof q, p, idx[k]);
        if (is_file(q) && !http_respond_file(req, q, NULL, "Cache-Control: no-cache\r\n")) return 0;
      }
      continue;
    }
    if (is_file(p) && !http_respond_file(req, p, NULL, "Cache-Control: no-cache\r\n")) return 0;
  }
  return -1;
}

static int split_host_path(const char *path, char *host, size_t hs, char *rel, size_t rs) {
  const char *p = path;
  while (*p == '/') p++;
  const char *slash = strchr(p, '/');
  size_t hl = slash ? (size_t)(slash - p) : strlen(p);
  if (!hl || hl >= hs) return -1;
  memcpy(host, p, hl);
  host[hl] = 0;
  for (char *c = host; *c; c++) {
    if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
  }
  str_lcpy(rel, slash ? slash + 1 : "", rs);
  return strchr(host, '.') ? 0 : -1; /* hosts have a dot (localhost is not a game host) */
}

void gameserver_handle(http_req_t *req, void *ctx) {
  (void)ctx;
  if (strcmp(req->method, "GET") && strcmp(req->method, "HEAD") && strcmp(req->method, "POST")) {
    http_respond_text(req, 405, "method not allowed\n");
    return;
  }
  char roots[MAX_ROOTS][FP_PATH_MAX];
  int n = gd_recent_roots(roots, MAX_ROOTS - 1);
  str_lcpy(roots[n++], g_cfg.htdocs_path, FP_PATH_MAX);

  char host[256], rel[4096];
  if (req->is_proxy) {
    str_lcpy(host, req->host, sizeof host);
    str_lcpy(rel, req->path[0] == '/' ? req->path + 1 : req->path, sizeof rel);
  } else if (split_host_path(req->path, host, sizeof host, rel, sizeof rel)) {
    host[0] = 0;
  }

  if (host[0] && !try_roots(req, roots, n, host, rel)) return;

  /* A root-relative link inside a game ("/img/a.png") arrives without its
   * host: take the host from the page that asked for it. */
  char rhost[256] = "";
  const char *full = req->path[0] == '/' ? req->path + 1 : req->path;
  if (!req->is_proxy && req->referer[0]) {
    const char *r = strstr(req->referer, "://");
    r = r ? strchr(r + 3, '/') : NULL;
    char rrel[2048];
    if (!r || split_host_path(r, rhost, sizeof rhost, rrel, sizeof rrel) || !strcmp(rhost, host))
      rhost[0] = 0;
    if (rhost[0] && !try_roots(req, roots, n, rhost, full)) return;
  }

  /* Not unpacked anywhere: Legacy entries fetch missing files on demand. */
  char out[FP_PATH_MAX];
  if (host[0] && !gd_ensure_legacy_file(host, rel, out, sizeof out) &&
      !http_respond_file(req, out, NULL, NULL))
    return;
  if (rhost[0] && !gd_ensure_legacy_file(rhost, full, out, sizeof out) &&
      !http_respond_file(req, out, NULL, NULL))
    return;

  if (!strcmp(req->path, "/") && !req->is_proxy) {
    http_respond_text(req, 200,
                      "Flashpoint PS5 game server.\n"
                      "Files are served as /<host>/<path> or through proxy requests.\n");
    return;
  }
  http_respond_text(req, 404, "not found in game data\n");
}
