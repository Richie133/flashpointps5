/* Flashpoint PS5 - Flashpoint Launcher + Flashpoint Database, as a PS5
 * homebrew payload.
 *
 * Runs two small web servers:
 *   :22600  the UI (the Flashpoint Database frontend, made for a TV and
 *           a controller) and its JSON API over the local flashpoint.sqlite
 *   :22500  a Flashpoint-style game server for HTML5 entries and players
 * then opens the PS5 browser on the UI. Flash entries are handed to the
 * native Ruffle Flash PS5 app, since the PS5 browser has no WebAssembly.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "config.h"
#include "gamedata.h"
#include "gameserver.h"
#include "httpd.h"
#include "library.h"
#include "net.h"
#include "platform.h"
#include "util.h"
#include "version.h"

static void usage(const char *argv0) {
  printf("Flashpoint PS5 %s\n"
         "usage: %s [--config fpps5.json] [--root <Flashpoint folder>] [--www <dir>]\n"
         "          [--ui-port N] [--game-port N] [--run <dir>] [--ruffle-dir <dir>]\n"
         "          [--no-browser]\n",
         FPPS5_VERSION, argv0);
}

int main(int argc, char **argv) {
  const char *cfg_path = NULL, *root = NULL, *www = NULL, *run = NULL, *ruffle = NULL;
  int ui_port = 0, game_port = 0, no_browser = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *next = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--config") && next) cfg_path = argv[++i];
    else if (!strcmp(a, "--root") && next) root = argv[++i];
    else if (!strcmp(a, "--www") && next) www = argv[++i];
    else if (!strcmp(a, "--run") && next) run = argv[++i];
    else if (!strcmp(a, "--ruffle-dir") && next) ruffle = argv[++i];
    else if (!strcmp(a, "--ui-port") && next) ui_port = atoi(argv[++i]);
    else if (!strcmp(a, "--game-port") && next) game_port = atoi(argv[++i]);
    else if (!strcmp(a, "--no-browser")) no_browser = 1;
    else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      usage(argv[0]);
      return 0;
    }
  }
  signal(SIGPIPE, SIG_IGN);
  plat_init();

  config_load(cfg_path, root);
  if (www) str_lcpy(g_cfg.www_path, www, sizeof g_cfg.www_path);
  if (run) str_lcpy(g_cfg.run_path, run, sizeof g_cfg.run_path);
  if (ruffle) str_lcpy(g_cfg.ruffle_dir, ruffle, sizeof g_cfg.ruffle_dir);
  if (ui_port) g_cfg.ui_port = ui_port;
  if (game_port) g_cfg.game_server_port = game_port;
  if (no_browser) g_cfg.open_browser = 0;
  if (g_cfg.log_path[0]) log_set_file(g_cfg.log_path);

  LOG("Flashpoint PS5 %s starting", FPPS5_VERSION);
  LOG("Flashpoint folder: %s%s", g_cfg.flashpoint_path,
      g_cfg.config_file[0] ? "" : " (no fpps5.json, using defaults)");

  char ui_url[128];
  snprintf(ui_url, sizeof ui_url, "http://127.0.0.1:%d/", g_cfg.ui_port);

  if (httpd_start(g_cfg.ui_port, api_handle, NULL)) {
    /* Most likely we're already running: just bring the UI up again. */
    LOG("UI port %d is taken - assuming Flashpoint PS5 is already running", g_cfg.ui_port);
    if (g_cfg.open_browser) plat_open_browser(ui_url);
    return 0;
  }
  if (httpd_start(g_cfg.game_server_port, gameserver_handle, NULL)) {
    LOG("game server port %d is taken; HTML5 entries won't load", g_cfg.game_server_port);
  }

  net_init();
  if (lib_open(g_cfg.db_path)) {
    LOG("no Flashpoint database yet - the UI explains how to add one");
  }
  gd_init();

  char ip[64];
  plat_lan_ip(ip, sizeof ip);
  plat_notify("Flashpoint PS5 is running\nhttp://%s:%d", ip, g_cfg.ui_port);
  if (g_cfg.open_browser) plat_open_browser(ui_url);

  while (!g_quit_requested) sleep(1);
  usleep(300 * 1000); /* let the reply to /api/quit go out */
  LOG("quitting");
  return 0;
}
