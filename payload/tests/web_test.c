/* Flashpoint PS5 - tests for the web source parsers and the SWF finder,
 * on sample pages shaped like each site's.
 *   cc -D_GNU_SOURCE -Isrc tests/web_test.c src/util.c third_party/cjson/cJSON.c \
 *      third_party/sha256/sha256.c -lpthread -o build/web_test && build/web_test
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/web.c"

#include <assert.h>
#include <utime.h>

/* stand-ins for the network and Ruffle */
static int offline;
static char last_referer[1024];
int net_download_ref(const char *url, const char *dest, const char *referer, net_progress_cb cb, void *ud,
                     char *err, size_t errlen) {
  (void)cb; (void)ud;
  const char *body = NULL;
  str_lcpy(last_referer, referer ? referer : "", sizeof last_referer);
  if (!strcmp(url, "https://piczogame.net/play/3-foot-ninja.html"))
    body = "<html><script>RufflePlayer.newest().createPlayer().load({url:\"/media/games/3footninja.swf\"});"
           "</script><embed src=\"/media/player.swf\"></html>";
  else if (!strcmp(url, "https://piczogame.net/media/games/3footninja.swf")) body = "CWS\x0a....";
  else if (!strcmp(url, "https://swfchan.com/54/269093/?18.swf"))
    body = "<a href=\"https://eye.swfchan.com/flash.asp?id=269093&amp;n=18.swf\">18.swf</a>";
  else if (!strcmp(url, "https://eye.swfchan.com/flash.asp?id=269093&n=18.swf"))
    body = "<object><param name=movie value=\"//eye.swfchan.com/flash/269093/18.swf\"></object>";
  else if (!strcmp(url, "https://eye.swfchan.com/flash/269093/18.swf")) body = "FWS\x09....";
  else if (!strcmp(url, "https://piczogame.net/content/games.html"))
    body = "<a href=\"https://piczogame.net/play/a.html\">Ninja Run</a>"
           "<a href=\"https://piczogame.net/content/games-page2.html\">2</a>";
  else if (!strcmp(url, "https://piczogame.net/content/games-page2.html"))
    body = "<a href=\"https://piczogame.net/play/b.html\">Space Ninja</a>"
           "<a href=\"https://piczogame.net/play/c.html\">Tetris</a>"
           "<a href=\"https://piczogame.net/content/games-page3.html\">3</a>";
  else if (!strcmp(url, "https://piczogame.net/content/games-page3.html"))
    body = "<a href=\"https://piczogame.net/play/d.html\">Ninja 3</a>";
  /* Flash Museum: a WordPress listing with lazy logos, a game page framing
   * its player, and the player page naming the file */
  else if (!strcmp(url, "https://flashmuseum.net/games/"))
    body = "<article><a href=\"https://flashmuseum.net/game/stick-war-4xx/\"><img src=\"data:image/svg+xml,x\" "
           "data-src=\"https://flashmuseum.net/wp-content/uploads/images/game-logo/s/stick-war-logo-4xx.webp\" "
           "alt=\"Stick War logo\"></a><h3><a href=\"https://flashmuseum.net/game/stick-war-4xx/\">Stick War</a></h3>"
           "</article><article><a href=\"/game/territory-war-sg3/\"><img alt=\"Territory War logo\"></a></article>"
           "<a href=\"https://flashmuseum.net/game/stick-war-4xx/#respond\">Comments</a>"
           "<a href=\"https://flashmuseum.net/genre/stick/\">Stick</a>"
           "<a href=\"https://flashmuseum.net/games/page/2/\">Next</a>";
  else if (!strcmp(url, "https://flashmuseum.net/game/stick-war-4xx/"))
    body = "<iframe class=\"fm-player-frame\" src=\"https://player.flashmuseum.net/game/stick-war-4xx/\"></iframe>";
  else if (!strcmp(url, "https://player.flashmuseum.net/game/stick-war-4xx/"))
    body = "<script src=\"https://unpkg.com/@ruffle-rs/ruffle\"></script><script>window.RufflePlayer.newest()"
           ".createPlayer().load({\"url\":\"https:\\/\\/files.flashmuseum.net\\/s\\/stick-war.swf\"});</script>";
  else if (!strcmp(url, "https://files.flashmuseum.net/s/stick-war.swf")) body = "CWS\x09....";
  /* files on Amazon S3: 404 unless asked for from a Flash Museum player page */
  else if (!strcmp(url, "https://flashmuseum.net/animation/crabs-fj7/"))
    body = "<iframe class=\"fm-player-frame\" src=\"https://player.flashmuseum.net/animation/crabs-fj7/\"></iframe>";
  else if (!strcmp(url, "https://player.flashmuseum.net/animation/crabs-fj7/"))
    body = "<script>window.RufflePlayer.newest().createPlayer().load({url:"
           "\"https://flashmuseum.s3.amazonaws.com/f79b2ae8-1125-43fc-738c-d72e98055de0.swf\"});</script>";
  else if (!strcmp(url, "https://flashmuseum.s3.amazonaws.com/f79b2ae8-1125-43fc-738c-d72e98055de0.swf") &&
           referer && !strcmp(referer, "https://player.flashmuseum.net/"))
    body = "FWS\x0a....";
  if (offline && strstr(url, "/content/")) body = NULL;
  if (!body) { snprintf(err, errlen, "404 %s", url); return -1; }
  mkdirs_for_file(dest);
  return write_text_file(dest, body);
}
int launch_swf(const char *key, const char *title, const char *swf, const char *source_url,
               char *msg, size_t mlen) {
  (void)key; (void)source_url;
  snprintf(msg, mlen, "started %s from %s", title, swf);
  return 0;
}
fp_config_t g_cfg;

static const char k_dagobah[] =
  "<div class=e><a href=\"/flash/Cursor_Invisible.swf\"><img src=\"/t100/Cursor_Invisible.jpg\"></a>"
  "<br>tags: Misc, Loop</div>"
  "<div class=e><a href=\"https://dagobah.net/flash/Oppai_Simulator.swf\"><img src=\"/img/swf.png\"></a>"
  " tags: Misc, NSFW</div>"
  "<div class=e><a href='/flash/Super%20Game_2.swf'><img src='/t100/Super%20Game_2.jpg'></a> tags: Game</div>"
  "<a href=\"?search=game&amp;page=2\">2</a>";
static const char k_piczo[] =
  "<li><a href=\"https://piczogame.net/play/03318.html\"><img src=\"https://piczogame.net/media/icons/03318.jpg\" alt=\"03318\"></a>"
  "<a href=\"https://piczogame.net/play/03318.html\">03318</a> 0 plays</li>"
  "<li><a href=\"https://piczogame.net/play/3-foot-ninja.html\"><img src=\"https://piczogame.net/media/icons/3footninja.jpg\"></a>"
  "<a href=\"https://piczogame.net/play/3-foot-ninja.html\">3 Foot Ninja</a> 483 plays</li>"
  "<a href=\"https://piczogame.net/content/games-page2.html\">Next &raquo;</a>";
static const char k_swfchan[] =
  "<a href=\"/54/269093/?18.swf\">18.swf</a> <img src=\"https://images1.swfchan.com/54/269093a.jpg\">"
  "<a href=\"https://swfchan.com/12/55555/?space_game.swf\">space_game.swf</a>"
  "<a href=\"/54/269093/info.shtml?18.swf\">[i]</a>";

int main(void) {
  str_lcpy(g_cfg.cache_path, "/tmp/fpweb-test", sizeof g_cfg.cache_path);
  remove_tree(g_cfg.cache_path);
  char out[1024];
  absolutize("https://a.net/x/y.html?q=1", "z.swf", out, sizeof out);
  assert(!strcmp(out, "https://a.net/x/z.swf"));
  absolutize("https://a.net/x/y.html", "//b.net/c.swf", out, sizeof out);
  assert(!strcmp(out, "https://b.net/c.swf"));
  absolutize("https://a.net/x/y.html", "/c.swf", out, sizeof out);
  assert(!strcmp(out, "https://a.net/c.swf"));
  absolutize("https://a.net", "c.swf", out, sizeof out);
  assert(!strcmp(out, "https://a.net/c.swf"));

  items_t it = {0};
  parse_dagobah("https://dagobah.net/?search=game&page=1", k_dagobah, &it);
  assert(it.n == 3);
  assert(!strcmp(it.items[0].title, "Cursor Invisible") && !it.items[0].adult);
  assert(!strcmp(it.items[0].thumb, "https://dagobah.net/t100/Cursor_Invisible.jpg"));
  assert(it.items[1].adult && !it.items[1].thumb[0]);
  assert(!strcmp(it.items[2].title, "Super Game 2") && !it.items[2].adult);
  assert(!strcmp(it.items[2].swf, "https://dagobah.net/flash/Super%20Game_2.swf"));
  free(it.items);

  memset(&it, 0, sizeof it);
  parse_piczogame("https://piczogame.net/content/games.html", k_piczo, &it);
  assert(it.n == 2);
  assert(!strcmp(it.items[1].title, "3 Foot Ninja"));
  assert(!strcmp(it.items[1].thumb, "https://piczogame.net/media/icons/3footninja.jpg"));
  assert(!strcmp(it.items[0].page, "https://piczogame.net/play/03318.html"));
  free(it.items);

  memset(&it, 0, sizeof it);
  parse_swfchan("https://swfchan.com/categories-gameclean.shtml", k_swfchan, &it);
  assert(it.n == 2);
  assert(!strcmp(it.items[0].title, "18") && !strcmp(it.items[1].title, "space game"));
  assert(!strcmp(it.items[1].thumb, "https://images1.swfchan.com/12/55555a.jpg"));
  free(it.items);

  char err[256] = "", dest[1100];
  int budget = 6;
  snprintf(dest, sizeof dest, "%s/a.swf", g_cfg.cache_path);
  assert(!resolve_swf("https://piczogame.net/play/3-foot-ninja.html", NULL, dest, 0, &budget, err, sizeof err));
  assert(is_swf_file(dest));
  budget = 6;
  snprintf(dest, sizeof dest, "%s/b.swf", g_cfg.cache_path);
  assert(!resolve_swf("https://swfchan.com/54/269093/?18.swf", NULL, dest, 0, &budget, err, sizeof err));
  assert(is_swf_file(dest));
  budget = 6;
  err[0] = 0;
  assert(resolve_swf("https://piczogame.net/play/missing.html", NULL, dest, 0, &budget, err, sizeof err));
  /* Flash Museum: titles from link text or the logo's alt, logos from the
   * page or built from the slug, one entry per game */
  char *fm = web_list_json("flashmuseum", "games", "", 1, 0);
  assert(strstr(fm, "\"title\":\"Stick War\"") && strstr(fm, "\"title\":\"Territory War\""));
  assert(strstr(fm, "game-logo/s/stick-war-logo-4xx.webp"));
  assert(strstr(fm, "game-logo/t/territory-war-logo-sg3.webp"));
  assert(!strstr(fm, "#respond") && !strstr(fm, "genre/stick") && strstr(fm, "\"more\":true"));
  free(fm);
  /* its search page isn't confirmed: with nothing from it, titles are matched */
  fm = web_list_json("flashmuseum", "games", "territory", 1, 0);
  assert(strstr(fm, "Territory War") && !strstr(fm, "Stick War"));
  free(fm);
  /* playing: game page -> player frame -> the file it loads */
  {
    char d2[1200], e2[256] = "";
    snprintf(d2, sizeof d2, "%s/fm.swf", g_cfg.cache_path);
    int b2 = 8;
    assert(!resolve_swf("https://flashmuseum.net/game/stick-war-4xx/", NULL, d2, 0, &b2, e2, sizeof e2));
    /* the S3 file gets the player page's origin as its Referer, as from a browser */
    snprintf(d2, sizeof d2, "%s/fm2.swf", g_cfg.cache_path);
    b2 = 8;
    e2[0] = 0;
    assert(!resolve_swf("https://flashmuseum.net/animation/crabs-fj7/", NULL, d2, 0, &b2, e2, sizeof e2));
    assert(is_swf_file(d2) && !strcmp(last_referer, "https://player.flashmuseum.net/"));
  }
  /* Referer as a browser sends it */
  {
    char r[1024];
    referer_for("https://a.net/x/y.html?q=1#top", "https://a.net/z.swf", r, sizeof r);
    assert(!strcmp(r, "https://a.net/x/y.html?q=1"));
    referer_for("https://a.net/x/y.html", "https://cdn.b.com/z.swf", r, sizeof r);
    assert(!strcmp(r, "https://a.net/"));
    referer_for("https://a.net/x/", "http://a.net/z.swf", r, sizeof r);
    assert(!r[0]);
    referer_for(NULL, "https://a.net/z.swf", r, sizeof r);
    assert(!r[0]);
  }

  /* PiczoGame has no search page: its list pages are read and matched */
  char *json = web_list_json("piczogame", "games", "ninja", 1, 0);
  assert(strstr(json, "Ninja Run") && strstr(json, "Space Ninja") && strstr(json, "Ninja 3"));
  assert(!strstr(json, "Tetris") && strstr(json, "\"more\":false") && strstr(json, "\"next\":4"));
  free(json);
  json = web_list_json("piczogame", "games", "", 1, 0);
  assert(strstr(json, "Ninja Run") && !strstr(json, "Space Ninja") && strstr(json, "\"more\":true"));
  assert(strstr(json, "\"next\":2"));
  free(json);
  /* a site that stops answering still lists from the older copy */
  offline = 1;
  char cached[1100], key[17];
  hash_hex("https://piczogame.net/content/games.html", key);
  snprintf(cached, sizeof cached, "%s/web/%s.html", g_cfg.cache_path, key);
  struct utimbuf old = {time(NULL) - 3600, time(NULL) - 3600};
  assert(!utime(cached, &old));
  json = web_list_json("piczogame", "games", "", 1, 0);
  assert(strstr(json, "Ninja Run"));
  free(json);
  json = web_list_json("piczogame", "animations", "", 1, 0);
  assert(strstr(json, "didn't answer"));
  free(json);
  offline = 0;
  printf("web tests passed\n");
  return 0;
}
