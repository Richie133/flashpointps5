/* Flashpoint PS5 - Flash from other websites: Dagobah, PiczoGame and
 * swfchan listings, and any .swf link, played in Ruffle Flash.
 *
 *   GET /api/web/sources                       sites and their lists
 *   GET /api/web/list?source=&list=&q=&page=   {"items":[...],"more":bool}
 *   GET /api/web/thumb?u=URL                   a list picture (cached)
 *   GET /api/web/play?url=&title=&source=      like /api/play: "pending"
 *        while the file is found and downloaded, then "launched"/"error"
 *
 * The lists are read from each site's ordinary pages, the way a browser
 * shows them, on request only, cached for ten minutes. Item pages are
 * searched for the .swf they embed (a few hops at most), and the file is
 * checked for the SWF signature before it is handed to Ruffle.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "web.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "launch.h"
#include "net.h"
#include "util.h"

/* ------------------------------------------------------------- helpers */

static unsigned long long fnv1a(const char *s) {
  unsigned long long h = 1469598103934665603ULL;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

static void hash_hex(const char *s, char out[17]) { snprintf(out, 17, "%016llx", fnv1a(s)); }

static int file_age_ok(const char *path, int max_seconds) {
  struct stat st;
  if (stat(path, &st) || st.st_size <= 0) return 0;
  return time(NULL) - st.st_mtime < max_seconds;
}

static void host_of(const char *url, char *host, size_t size) {
  host[0] = 0;
  const char *p = strstr(url, "://");
  if (!p) return;
  p += 3;
  size_t n = strcspn(p, "/:?#");
  if (n >= size) n = size - 1;
  memcpy(host, p, n);
  host[n] = 0;
  for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);
}

static int host_is(const char *host, const char *domain) {
  size_t h = strlen(host), d = strlen(domain);
  if (h < d || strcasecmp(host + h - d, domain)) return 0;
  return h == d || host[h - d - 1] == '.';
}

/* Tests point the sites at a local server: FPPS5_WEB_BASE=http://127.0.0.1:8771
 * makes https://dagobah.net/x into http://127.0.0.1:8771/dagobah.net/x. */
static const char *test_base(void) {
  const char *b = getenv("FPPS5_WEB_BASE");
  return b && b[0] ? b : NULL;
}

static void test_rewrite(char *url, size_t size) {
  const char *base = test_base();
  if (!base || strncmp(url, "https://", 8)) return;
  char tmp[2048];
  snprintf(tmp, sizeof tmp, "%s/%s", base, url + 8);
  str_lcpy(url, tmp, size);
}

static int allowed_host(const char *url) {
  if (test_base() && !strncmp(url, test_base(), strlen(test_base()))) return 1;
  char host[128];
  host_of(url, host, sizeof host);
  return host_is(host, "dagobah.net") || host_is(host, "piczogame.net") ||
         host_is(host, "swfchan.com") || host_is(host, "swfchan.net") ||
         host_is(host, "flashmuseum.net");
}

/* HTML entities that turn up in links and titles. */
static void decode_entities(char *s) {
  static const struct {
    const char *name;
    const char *text;
  } k[] = {{"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#039;", "'"}, {"&apos;", "'"},
           {"&lt;", "<"},  {"&gt;", ">"},    {"&nbsp;", " "}, {NULL, NULL}};
  char *w = s;
  for (char *r = s; *r;) {
    int hit = 0;
    if (*r == '&') {
      for (int i = 0; k[i].name; i++) {
        size_t n = strlen(k[i].name);
        if (!strncasecmp(r, k[i].name, n)) {
          *w++ = k[i].text[0];
          r += n;
          hit = 1;
          break;
        }
      }
    }
    if (!hit) *w++ = *r++;
  }
  *w = 0;
}

/* Resolves href against page URL into out. */
static void absolutize(const char *page, const char *href, char *out, size_t size) {
  char h[2048];
  str_lcpy(h, href, sizeof h);
  decode_entities(h);
  while (*h && isspace((unsigned char)h[0])) memmove(h, h + 1, strlen(h));
  if (!strncasecmp(h, "http://", 7) || !strncasecmp(h, "https://", 8)) {
    str_lcpy(out, h, size);
    return;
  }
  const char *scheme_end = strstr(page, "://");
  if (!scheme_end) {
    str_lcpy(out, h, size);
    return;
  }
  if (h[0] == '/' && h[1] == '/') {
    snprintf(out, size, "%.*s:%s", (int)(scheme_end - page), page, h);
    return;
  }
  const char *host_end = scheme_end + 3 + strcspn(scheme_end + 3, "/?#");
  if (h[0] == '/') {
    snprintf(out, size, "%.*s%s", (int)(host_end - page), page, h);
    return;
  }
  /* relative to the page's folder (or to the page itself for ?query) */
  size_t dir = strcspn(page, "?#");
  if (h[0] != '?') {
    while (dir > (size_t)(host_end - page) && page[dir - 1] != '/') dir--;
    if (dir == (size_t)(host_end - page)) {
      snprintf(out, size, "%.*s/%s", (int)dir, page, h);
      return;
    }
  }
  snprintf(out, size, "%.*s%s", (int)dir, page, h);
}

static void url_encode_q(sb_t *sb, const char *s, int plus_spaces) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (isalnum(*p) || strchr("-_.~", *p)) sb_appendn(sb, (const char *)p, 1);
    else if (*p == ' ' && plus_spaces) sb_append(sb, "+");
    else sb_printf(sb, "%%%02X", *p);
  }
}

/* net_download, but in tests every https address goes to the stand-in. */
static int web_download(const char *url, const char *referer, const char *dest, net_progress_cb cb, void *ud,
                        char *err, size_t errlen) {
  char u[2048];
  str_lcpy(u, url, sizeof u);
  test_rewrite(u, sizeof u);
  return net_download_ref(u, dest, referer, cb, ud, err, errlen);
}

/* The Referer a browser sends for url when page asked for it (its default
 * policy, strict-origin-when-cross-origin): the whole page address on the
 * same site, only the site's origin on another one. Flash Museum keeps its
 * files on Amazon S3, which only answers requests from its player pages. */
static void referer_for(const char *page, const char *url, char *out, size_t size) {
  out[0] = 0;
  if (!page || strncasecmp(page, "http", 4)) return;
  const char *ps = strstr(page, "://"), *us = strstr(url, "://");
  if (!ps || !us) return;
  size_t po = (size_t)(ps + 3 - page) + strcspn(ps + 3, "/?#"); /* scheme://host[:port] */
  size_t uo = (size_t)(us + 3 - url) + strcspn(us + 3, "/?#");
  if (po == uo && !strncasecmp(page, url, po)) {
    size_t n = strcspn(page, "#");
    snprintf(out, size, "%.*s", (int)n, page);
  } else if (!strncasecmp(page, "https:", 6) && !strncasecmp(url, "http:", 5)) {
    return; /* none from https to plain http */
  } else {
    snprintf(out, size, "%.*s/", (int)po, page);
  }
}

/* Download to a name of our own, then move it into place, so two
 * requests for the same URL never write one file at once. */
static int download_atomic(const char *url, const char *path, char *err, size_t errlen) {
  static unsigned counter;
  char tmp[FP_PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s.%u.tmp", path, __sync_fetch_and_add(&counter, 1));
  mkdirs_for_file(path);
  if (web_download(url, NULL, tmp, NULL, NULL, err, errlen)) {
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, path)) {
    snprintf(err, errlen, "can't store %s", path);
    unlink(tmp);
    return -1;
  }
  return 0;
}

static char *fetch_page(const char *url, size_t *len) {
  char key[17], path[FP_PATH_MAX], name[40], err[256];
  hash_hex(url, key);
  snprintf(name, sizeof name, "web/%s.html", key);
  path_join(path, sizeof path, g_cfg.cache_path, name);
  if (!file_age_ok(path, 600) && download_atomic(url, path, err, sizeof err)) {
    LOG("web: %s: %s", url, err);
    if (!is_file(path)) return NULL; /* an older copy is better than nothing */
  }
  return read_text_file(path, len);
}

/* Text of an element body: tags dropped, whitespace folded. */
static void strip_tags(const char *s, size_t n, char *out, size_t size) {
  size_t o = 0;
  int in_tag = 0, space = 0;
  for (size_t i = 0; i < n && o + 1 < size; i++) {
    char c = s[i];
    if (c == '<') in_tag = 1;
    else if (c == '>') in_tag = 0;
    else if (!in_tag) {
      if (isspace((unsigned char)c)) {
        space = o > 0;
      } else {
        if (space && o + 1 < size) out[o++] = ' ';
        space = 0;
        out[o++] = c;
      }
    }
  }
  out[o] = 0;
  decode_entities(out);
}

/* Each href=/src= attribute value, in order. */
typedef struct {
  const char *at;    /* start of the value */
  size_t len;
  int is_src;
} attr_t;

static int next_attr(const char *html, const char **pos, attr_t *a) {
  for (const char *p = *pos; *p; p++) {
    int src = 0;
    if (!strncasecmp(p, "href", 4)) src = 0;
    else if (!strncasecmp(p, "src", 3)) src = 1;
    else continue;
    const char *q = p + (src ? 3 : 4);
    while (*q == ' ') q++;
    if (*q != '=') continue;
    q++;
    while (*q == ' ') q++;
    char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
    const char *end = q;
    while (*end && (quote ? *end != quote : !isspace((unsigned char)*end) && *end != '>')) end++;
    a->at = q;
    a->len = (size_t)(end - q);
    a->is_src = src;
    *pos = *end ? end + 1 : end;
    (void)html;
    return 1;
  }
  return 0;
}

/* ---------------------------------------------------------- the sites */

typedef struct {
  const char *id, *name;
} list_def_t;

typedef struct {
  const char *id, *name, *home;
  const list_def_t *lists;
  int searchable;
} site_def_t;

static const list_def_t k_dagobah_lists[] = {
  {"new", "Newest"}, {"liked", "Most liked"}, {"games", "Games"},
  {"music", "Music videos"}, {"loops", "Loops"}, {NULL, NULL}};
static const list_def_t k_piczo_lists[] = {
  {"games", "Games"}, {"animations", "Animations"}, {"flash-loops", "Flash loops"},
  {"all", "Everything"}, {NULL, NULL}};
static const list_def_t k_swfchan_lists[] = {
  {"games", "Games"}, {"stories", "Stories"}, {"loops", "Music loops"}, {NULL, NULL}};
/* Flash Museum (WordPress): /games/, /animations/, /collection/hall-of-fame/
 * and /genre/SLUG/, each with /page/N/ after it. */
static const list_def_t k_fm_lists[] = {
  {"games", "Games"}, {"animations", "Animations"}, {"hall-of-fame", "Hall of Fame"},
  {"action", "Action"}, {"adventure", "Adventure"}, {"puzzle", "Puzzle"},
  {"platformer", "Platformer"}, {"shooter", "Shooter"}, {"strategy", "Strategy"},
  {"tower-defense", "Tower defense"}, {"escape-the-room", "Escape the room"},
  {"racing", "Racing"}, {"sports", "Sports"}, {"stick", "Stick"}, {NULL, NULL}};

static const site_def_t k_sites[] = {
  {"dagobah", "Dagobah", "https://dagobah.net/", k_dagobah_lists, 1},
  {"piczogame", "PiczoGame", "https://piczogame.net/", k_piczo_lists, 0},
  {"swfchan", "swfchan", "https://swfchan.com/", k_swfchan_lists, 0},
  {"flashmuseum", "Flash Museum", "https://flashmuseum.net/", k_fm_lists, 1},
  {NULL, NULL, NULL, NULL, 0}};

static const site_def_t *find_site(const char *id) {
  for (int i = 0; k_sites[i].id; i++) {
    if (!strcmp(k_sites[i].id, id)) return &k_sites[i];
  }
  return NULL;
}

char *web_sources_json(void) {
  sb_t sb;
  sb_init(&sb);
  sb_printf(&sb, "{\"adult\":%s,\"sources\":[", g_cfg.web_adult ? "true" : "false");
  for (int i = 0; k_sites[i].id; i++) {
    if (i) sb_append(&sb, ",");
    sb_append(&sb, "{\"id\":");
    sb_json_str(&sb, k_sites[i].id);
    sb_append(&sb, ",\"name\":");
    sb_json_str(&sb, k_sites[i].name);
    sb_append(&sb, ",\"home\":");
    sb_json_str(&sb, k_sites[i].home);
    sb_printf(&sb, ",\"search\":%s,\"lists\":[", k_sites[i].searchable ? "true" : "false");
    for (int j = 0; k_sites[i].lists[j].id; j++) {
      if (j) sb_append(&sb, ",");
      sb_append(&sb, "{\"id\":");
      sb_json_str(&sb, k_sites[i].lists[j].id);
      sb_append(&sb, ",\"name\":");
      sb_json_str(&sb, k_sites[i].lists[j].name);
      sb_append(&sb, "}");
    }
    sb_append(&sb, "]}");
  }
  sb_append(&sb, "]}");
  return sb_steal(&sb);
}

/* The listing URL for a site, list, search and page (1-based). */
static int list_url(const char *site, const char *list, const char *q, int page, char *out,
                    size_t size) {
  sb_t sb;
  sb_init(&sb);
  int ok = 1;
  if (!strcmp(site, "dagobah")) {
    sb_append(&sb, "https://dagobah.net/?");
    const char *search = q && q[0] ? q
                         : !strcmp(list, "games") ? "game"
                         : !strcmp(list, "music") ? "music video"
                         : !strcmp(list, "loops") ? "loop"
                         : NULL;
    if (search) {
      sb_append(&sb, "search=");
      url_encode_q(&sb, search, 1);
      sb_append(&sb, "&");
    } else if (!strcmp(list, "liked")) {
      sb_append(&sb, "o=r&");
    } else {
      sb_append(&sb, "o=id&");
    }
    sb_printf(&sb, "page=%d", page);
  } else if (!strcmp(site, "piczogame")) {
    const char *cat = list && list[0] ? list : "games";
    if (strcmp(cat, "games") && strcmp(cat, "animations") && strcmp(cat, "flash-loops") &&
        strcmp(cat, "all"))
      cat = "games";
    if (page <= 1) sb_printf(&sb, "https://piczogame.net/content/%s.html", cat);
    else sb_printf(&sb, "https://piczogame.net/content/%s-page%d.html", cat, page);
  } else if (!strcmp(site, "swfchan")) {
    const char *file = !strcmp(list, "stories") ? "categories-storycleannogames.shtml"
                       : !strcmp(list, "loops") ? "categories-loopmusic.shtml"
                                                : "categories-gameclean.shtml";
    if (page > 1) ok = 0; /* one page each */
    sb_printf(&sb, "https://swfchan.com/%s", file);
  } else if (!strcmp(site, "flashmuseum")) {
    if (q && q[0]) { /* WordPress's own search */
      sb_append(&sb, "https://flashmuseum.net/");
      if (page > 1) sb_printf(&sb, "page/%d/", page);
      sb_append(&sb, "?s=");
      url_encode_q(&sb, q, 1);
    } else {
      const char *l = list && list[0] ? list : "games";
      int known = 0;
      for (int i = 0; k_fm_lists[i].id; i++) known |= !strcmp(k_fm_lists[i].id, l);
      if (!known) l = "games";
      if (!strcmp(l, "games") || !strcmp(l, "animations"))
        sb_printf(&sb, "https://flashmuseum.net/%s/", l);
      else if (!strcmp(l, "hall-of-fame"))
        sb_append(&sb, "https://flashmuseum.net/collection/hall-of-fame/");
      else
        sb_printf(&sb, "https://flashmuseum.net/genre/%s/", l);
      if (page > 1) sb_printf(&sb, "page/%d/", page);
    }
  } else {
    ok = 0;
  }
  str_lcpy(out, sb.buf ? sb.buf : "", size);
  sb_free(&sb);
  test_rewrite(out, size);
  return ok ? 0 : -1;
}

typedef struct {
  char title[256];
  char page[1024];  /* where it came from */
  char swf[1024];   /* the file, when the list gives it */
  char thumb[1024];
  int adult;
} web_item_t;

typedef struct {
  web_item_t *items;
  int n, cap;
} items_t;

static web_item_t *items_add(items_t *it) {
  if (it->n == it->cap) {
    it->cap = it->cap ? it->cap * 2 : 64;
    it->items = realloc(it->items, (size_t)it->cap * sizeof *it->items);
  }
  web_item_t *w = &it->items[it->n++];
  memset(w, 0, sizeof *w);
  return w;
}

static int items_has(const items_t *it, const char *page) {
  for (int i = 0; i < it->n; i++) {
    if (!strcmp(it->items[i].page, page)) return 1;
  }
  return 0;
}

/* "Some_Flash%20Name.swf" -> "Some Flash Name" */
static void title_from_file(const char *file, size_t n, char *out, size_t size) {
  char tmp[512];
  if (n >= sizeof tmp) n = sizeof tmp - 1;
  memcpy(tmp, file, n);
  tmp[n] = 0;
  url_decode_inplace(tmp);
  char *dot = strrchr(tmp, '.');
  if (dot && !strcasecmp(dot, ".swf")) *dot = 0;
  for (char *c = tmp; *c; c++) {
    if (*c == '_' || *c == '+') *c = ' ';
  }
  str_lcpy(out, tmp, size);
}

/* Dagobah: <a href="/flash/NAME.swf"><img src="/t100/NAME.jpg">...</a>
 * then "tags: Misc, NSFW" before the next entry. */
static void parse_dagobah(const char *page_url, const char *html, items_t *out) {
  const char *pos = html;
  attr_t a;
  const char *prev_at = NULL;
  web_item_t *prev = NULL;
  while (next_attr(html, &pos, &a)) {
    char href[1024], abs[1024];
    if (a.len >= sizeof href) continue;
    memcpy(href, a.at, a.len);
    href[a.len] = 0;
    absolutize(page_url, href, abs, sizeof abs);
    if (a.is_src && prev && !prev->thumb[0] && strstr(abs, "/t100/")) {
      str_lcpy(prev->thumb, abs, sizeof prev->thumb);
      continue;
    }
    const char *flash = strstr(abs, "/flash/");
    if (a.is_src || !flash || !str_ends_with_ci(abs, ".swf")) continue;
    if (items_has(out, abs)) continue;
    if (prev && prev_at) {
      /* the text between the previous entry and this one holds its tags */
      size_t span = (size_t)(a.at - prev_at);
      char *seg = malloc(span + 1);
      memcpy(seg, prev_at, span);
      seg[span] = 0;
      if (str_contains_ci(seg, "nsfw") || str_contains_ci(seg, "hentai")) prev->adult = 1;
      free(seg);
    }
    web_item_t *w = items_add(out);
    str_lcpy(w->page, abs, sizeof w->page);
    str_lcpy(w->swf, abs, sizeof w->swf);
    title_from_file(flash + 7, strlen(flash + 7), w->title, sizeof w->title);
    prev = w;
    prev_at = a.at;
  }
  if (prev && prev_at) {
    size_t span = strnlen(prev_at, 3000);
    char *seg = malloc(span + 1);
    memcpy(seg, prev_at, span);
    seg[span] = 0;
    if (str_contains_ci(seg, "nsfw") || str_contains_ci(seg, "hentai")) prev->adult = 1;
    free(seg);
  }
}

/* PiczoGame: <a href=".../play/SLUG.html"><img src=".../media/icons/X.jpg"
 * alt="Title"></a> <a href=".../play/SLUG.html">Title</a> */
static void parse_piczogame(const char *page_url, const char *html, items_t *out) {
  const char *pos = html;
  attr_t a;
  web_item_t *cur = NULL;
  char pending_thumb[1024] = "";
  while (next_attr(html, &pos, &a)) {
    char href[1024], abs[1024];
    if (a.len >= sizeof href) continue;
    memcpy(href, a.at, a.len);
    href[a.len] = 0;
    absolutize(page_url, href, abs, sizeof abs);
    if (a.is_src) {
      if (strstr(abs, "/media/icons/")) {
        if (cur && !cur->thumb[0]) str_lcpy(cur->thumb, abs, sizeof cur->thumb);
        else str_lcpy(pending_thumb, abs, sizeof pending_thumb);
        /* alt="..." right after the src names the item */
        const char *alt = strcasestr(pos, "alt=");
        const char *close = strchr(pos, '>');
        if (cur && !cur->title[0] && alt && close && alt < close) {
          alt += 4;
          char q = (*alt == '"' || *alt == '\'') ? *alt++ : 0;
          size_t n = q ? strcspn(alt, q == '"' ? "\"" : "'") : strcspn(alt, " >");
          strip_tags(alt, n, cur->title, sizeof cur->title);
        }
      }
      continue;
    }
    if (!strstr(abs, "/play/") || !str_ends_with_ci(abs, ".html")) continue;
    if (!cur || strcmp(cur->page, abs)) {
      if (items_has(out, abs)) {
        cur = NULL;
        for (int i = 0; i < out->n; i++) {
          if (!strcmp(out->items[i].page, abs)) cur = &out->items[i];
        }
      } else {
        cur = items_add(out);
        str_lcpy(cur->page, abs, sizeof cur->page);
        if (pending_thumb[0]) {
          str_lcpy(cur->thumb, pending_thumb, sizeof cur->thumb);
          pending_thumb[0] = 0;
        }
      }
    }
    /* the link's own text, when it has some */
    const char *body = pos;
    const char *end = strcasestr(body, "</a>");
    if (cur && end && end - body < 400) {
      const char *gt = strchr(body, '>');
      if (gt && gt < end) {
        char text[256];
        strip_tags(gt + 1, (size_t)(end - gt - 1), text, sizeof text);
        if (text[0] && !cur->title[0]) str_lcpy(cur->title, text, sizeof cur->title);
      }
    }
  }
  for (int i = 0; i < out->n; i++) {
    web_item_t *w = &out->items[i];
    if (!w->title[0]) {
      const char *slug = strrchr(w->page, '/');
      slug = slug ? slug + 1 : w->page;
      char t[256];
      title_from_file(slug, strcspn(slug, "."), t, sizeof t);
      for (char *c = t; *c; c++) {
        if (*c == '-') *c = ' ';
      }
      str_lcpy(w->title, t, sizeof w->title);
    }
  }
}

/* swfchan: .../BUCKET/ID/?NAME.swf, picture images1.swfchan.com/BUCKET/IDa.jpg */
static void parse_swfchan(const char *page_url, const char *html, items_t *out) {
  const char *pos = html;
  attr_t a;
  while (next_attr(html, &pos, &a)) {
    if (a.is_src) continue;
    char href[1024], abs[1024];
    if (a.len >= sizeof href) continue;
    memcpy(href, a.at, a.len);
    href[a.len] = 0;
    absolutize(page_url, href, abs, sizeof abs);
    char host[128];
    host_of(abs, host, sizeof host);
    if (strcmp(host, "swfchan.com") && strcmp(host, "www.swfchan.com")) continue;
    const char *path = strstr(abs, "://") + 3 + strlen(host);
    unsigned bucket = 0, id = 0;
    int used = 0;
    if (sscanf(path, "/%u/%u/?%n", &bucket, &id, &used) < 2 || !used) continue;
    const char *name = path + used;
    if (!str_ends_with_ci(name, ".swf") || items_has(out, abs)) continue;
    web_item_t *w = items_add(out);
    str_lcpy(w->page, abs, sizeof w->page);
    title_from_file(name, strlen(name), w->title, sizeof w->title);
    snprintf(w->thumb, sizeof w->thumb, "https://images1.swfchan.com/%u/%ua.jpg", bucket, id);
  }
}

/* Flash Museum: links to /game/SLUG-ID/ and /animation/SLUG-ID/ (the link
 * text, or the logo's alt text, is the title). Logos are
 * /wp-content/uploads/images/game-logo/F/SLUG-logo-ID.webp: taken from the
 * page when it names one, otherwise built from the slug. */
static void parse_flashmuseum(const char *page_url, const char *html, items_t *out) {
  const char *pos = html;
  attr_t a;
  while (next_attr(html, &pos, &a)) {
    if (a.is_src) continue;
    char href[1024], abs[1024];
    if (a.len >= sizeof href) continue;
    memcpy(href, a.at, a.len);
    href[a.len] = 0;
    absolutize(page_url, href, abs, sizeof abs);
    char host[128];
    host_of(abs, host, sizeof host);
    if (!host_is(host, "flashmuseum.net") && !(test_base() && strstr(abs, "/flashmuseum.net/"))) continue;
    const char *kind = strstr(abs, "/game/");
    int anim = 0;
    if (!kind) {
      kind = strstr(abs, "/animation/");
      anim = kind != NULL;
    }
    if (!kind || strstr(abs, "player.flashmuseum.net") || strchr(abs, '#') || strchr(abs, '?')) continue;
    const char *slug = kind + (anim ? 11 : 6);
    size_t sl = strcspn(slug, "/");
    if (sl == 0 || slug[sl] != '/' || slug[sl + 1]) continue; /* exactly /game/SLUG/ */

    web_item_t *w = NULL;
    for (int i = 0; i < out->n; i++) {
      if (!strcmp(out->items[i].page, abs)) w = &out->items[i];
    }
    if (!w) {
      w = items_add(out);
      str_lcpy(w->page, abs, sizeof w->page);
    }
    /* the link's text, or an <img alt="... logo"> inside it */
    const char *gt = strchr(pos, '>');
    const char *end = strcasestr(pos, "</a>");
    if (gt && end && gt < end && end - gt < 2000) {
      char text[256] = "";
      strip_tags(gt + 1, (size_t)(end - gt - 1), text, sizeof text);
      const char *alt = strcasestr(gt, "alt=");
      if (!text[0] && alt && alt < end) {
        alt += 4;
        char qc = (*alt == '"' || *alt == '\'') ? *alt++ : 0;
        size_t n = qc ? strcspn(alt, qc == '"' ? "\"" : "'") : strcspn(alt, " >");
        strip_tags(alt, n, text, sizeof text);
      }
      size_t tl = strlen(text);
      if (tl > 5 && !strcasecmp(text + tl - 5, " logo")) text[tl - 5] = 0;
      if (text[0] && (!w->title[0] || strlen(text) > strlen(w->title))) str_lcpy(w->title, text, sizeof w->title);
    }
    if (!w->thumb[0]) {
      /* SLUG-ID: the logo is NAME-logo-ID.webp */
      char name[512];
      if (sl >= sizeof name) continue;
      memcpy(name, slug, sl);
      name[sl] = 0;
      char *dash = strrchr(name, '-');
      if (!dash) continue;
      *dash = 0;
      const char *id = dash + 1;
      char needle[600];
      snprintf(needle, sizeof needle, "%s-logo-%s.", name, id);
      const char *hit = strstr(html, needle);
      if (hit) {
        const char *s0 = hit;
        while (s0 > html && !strchr("\"' (,=\n", s0[-1])) s0--;
        size_t n = strcspn(hit, "\"' ),\n");
        size_t len = (size_t)(hit - s0) + n;
        char raw[1024];
        if (len < sizeof raw) {
          memcpy(raw, s0, len);
          raw[len] = 0;
          absolutize(page_url, raw, w->thumb, sizeof w->thumb);
        }
      } else if (isalnum((unsigned char)name[0])) {
        snprintf(w->thumb, sizeof w->thumb,
                 "https://flashmuseum.net/wp-content/uploads/images/%s-logo/%c/%s-logo-%s.webp",
                 anim ? "animation" : "game", tolower((unsigned char)name[0]), name, id);
        test_rewrite(w->thumb, sizeof w->thumb);
      }
    }
  }
  for (int i = 0; i < out->n; i++) {
    web_item_t *w = &out->items[i];
    if (w->title[0]) continue;
    const char *slug = strstr(w->page, "/game/");
    slug = slug ? slug + 6 : strstr(w->page, "/animation/") + 11;
    char t[256];
    title_from_file(slug, strcspn(slug, "/"), t, sizeof t);
    char *dash = strrchr(t, '-');
    if (dash) *dash = 0; /* the ID */
    for (char *c = t; *c; c++) {
      if (*c == '-') *c = ' ';
    }
    str_lcpy(w->title, t, sizeof w->title);
  }
}

/* Every word of the query appears in the title. */
static int title_matches(const char *title, const char *q) {
  char words[256];
  str_lcpy(words, q, sizeof words);
  for (char *save = NULL, *w = strtok_r(words, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) {
    if (!str_contains_ci(title, w)) return 0;
  }
  return 1;
}

/* One listing page: its items, and whether a next page exists. 0, or -1
 * when the site didn't answer. */
static int list_page(const site_def_t *def, const char *list, const char *q, int page,
                     items_t *items, int *more) {
  char url[1024];
  *more = 0;
  if (list_url(def->id, list, q, page, url, sizeof url)) return 0; /* past the end */
  size_t len = 0;
  char *html = fetch_page(url, &len);
  if (!html) return -1;
  if (!strcmp(def->id, "dagobah")) parse_dagobah(url, html, items);
  else if (!strcmp(def->id, "piczogame")) parse_piczogame(url, html, items);
  else if (!strcmp(def->id, "flashmuseum")) parse_flashmuseum(url, html, items);
  else parse_swfchan(url, html, items);
  char next[64];
  if (!strcmp(def->id, "piczogame")) snprintf(next, sizeof next, "-page%d.html", page + 1);
  else if (!strcmp(def->id, "flashmuseum")) snprintf(next, sizeof next, "/page/%d/", page + 1);
  else snprintf(next, sizeof next, "page=%d", page + 1);
  if (strcmp(def->id, "swfchan")) *more = strstr(html, next) != NULL;
  free(html);
  return 0;
}

/* Sites with a search page are asked; the others are searched by reading
 * a few listing pages and keeping the titles that match, so "next" may
 * be several pages on. */
char *web_list_json(const char *site, const char *list, const char *q, int page, int adult) {
  const site_def_t *def = find_site(site ? site : "");
  if (!def) return str_dup("{\"error\":\"unknown source\",\"items\":[]}");
  if (page < 1) page = 1;
  if (!list) list = "";
  const int searching = q && q[0];
  int local = searching && !def->searchable;

  items_t items = {0};
  int more = 0, pg = page, failed = 0;
scan:
  for (int scanned = 0; scanned < (local ? 8 : 1); scanned++, pg++) {
    items_t got = {0};
    if (list_page(def, list, local ? NULL : q, pg, &got, &more)) {
      failed = scanned == 0;
      more = 0;
      free(got.items);
      break;
    }
    for (int i = 0; i < got.n; i++) {
      if (local && !title_matches(got.items[i].title, q)) continue;
      if (items_has(&items, got.items[i].page)) continue;
      *items_add(&items) = got.items[i];
    }
    free(got.items);
    if (!more || items.n >= 24) {
      pg++;
      break;
    }
  }
  /* Flash Museum's search page is WordPress's usual one, not confirmed:
   * when it gives nothing, match titles in the list instead */
  int fm_unmatched = 0;
  if (!local && searching && !strcmp(def->id, "flashmuseum") && items.n > 0) {
    fm_unmatched = 1; /* a page of entries none of which match looks like an ignored search */
    for (int i = 0; i < items.n && fm_unmatched; i++) fm_unmatched = !title_matches(items.items[i].title, q);
  }
  if (!local && searching && !strcmp(def->id, "flashmuseum") && (failed || items.n == 0 || fm_unmatched)) {
    LOG("web: %s search gave nothing; matching titles instead", def->name);
    local = 1;
    failed = 0;
    more = 0;
    pg = page;
    items.n = 0;
    goto scan;
  }
  if (failed) {
    free(items.items);
    sb_t sb;
    sb_init(&sb);
    sb_append(&sb, "{\"items\":[],\"more\":false,\"error\":");
    char m[300];
    snprintf(m, sizeof m, "%s didn't answer. Check the PS5's internet connection.", def->name);
    sb_json_str(&sb, m);
    sb_append(&sb, "}");
    return sb_steal(&sb);
  }

  sb_t sb;
  sb_init(&sb);
  sb_append(&sb, "{\"items\":[");
  int shown = 0, hidden = 0;
  for (int i = 0; i < items.n; i++) {
    web_item_t *w = &items.items[i];
    if (w->adult && !adult) {
      hidden++;
      continue;
    }
    if (shown++) sb_append(&sb, ",");
    sb_append(&sb, "{\"title\":");
    sb_json_str(&sb, w->title);
    sb_append(&sb, ",\"page\":");
    sb_json_str(&sb, w->page);
    sb_append(&sb, ",\"swf\":");
    sb_json_str(&sb, w->swf);
    sb_append(&sb, ",\"thumb\":");
    sb_json_str(&sb, w->thumb);
    sb_printf(&sb, ",\"adult\":%s}", w->adult ? "true" : "false");
  }
  sb_printf(&sb, "],\"more\":%s,\"next\":%d,\"hidden\":%d,\"source\":", more ? "true" : "false",
            pg, hidden);
  sb_json_str(&sb, def->name);
  sb_append(&sb, "}");
  free(items.items);
  return sb_steal(&sb);
}

int web_thumb_path(const char *url, char *out, size_t size) {
  if (!url || !allowed_host(url) || strncasecmp(url, "http", 4)) return -1;
  char key[17], name[48], err[256];
  hash_hex(url, key);
  snprintf(name, sizeof name, "thumbs/%s.img", key);
  path_join(out, size, g_cfg.cache_path, name);
  if (is_file(out)) return 0;
  char miss[FP_PATH_MAX];
  snprintf(miss, sizeof miss, "%s.miss", out);
  struct stat st;
  if (!stat(miss, &st) && time(NULL) - st.st_mtime < 600) return -1; /* failed lately */
  if (download_atomic(url, out, err, sizeof err)) {
    LOG("web: picture %s: %s", url, err);
    FILE *f = fopen(miss, "w");
    if (f) fclose(f);
    return -1;
  }
  return 0;
}

/* ---------------------------------------------------- finding the .swf */

static int is_swf_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  unsigned char m[3] = {0};
  size_t n = fread(m, 1, 3, f);
  fclose(f);
  return n == 3 && (m[0] == 'F' || m[0] == 'C' || m[0] == 'Z') && m[1] == 'W' && m[2] == 'S';
}

static int looks_like_player(const char *url) {
  static const char *const k[] = {"player", "preload", "loader", "expressinstall", "mochi",
                                  "ruffle", NULL};
  const char *file = strrchr(url, '/');
  file = file ? file : url;
  for (int i = 0; k[i]; i++) {
    if (str_contains_ci(file, k[i])) return 1;
  }
  return 0;
}

/* Every URL in the page that mentions ".swf", then watch pages
 * (swfchan's flash.asp), then same-site iframes. */
static int collect_candidates(const char *page_url, const char *html, char cand[][1024], int max) {
  int n = 0;
  /* 1. ".swf" anywhere (attributes, flashvars, script strings) */
  for (const char *p = html; (p = strcasestr(p, ".swf")) && n < max; p += 4) {
    const char *s = p;
    while (s > html && !strchr("\"'<> \t\r\n=(,", s[-1])) s--;
    const char *e = p + 4;
    while (*e && !strchr("\"'<> \t\r\n&),", *e)) e++;
    if (e - s < 5 || e - s > 900) continue;
    char raw[1024], abs[1024];
    memcpy(raw, s, (size_t)(e - s));
    raw[e - s] = 0;
    if (strchr(raw, '\\')) { /* JS-escaped slashes */
      char *w = raw;
      for (char *r = raw; *r; r++) {
        if (*r != '\\') *w++ = *r;
      }
      *w = 0;
    }
    absolutize(page_url, raw, abs, sizeof abs);
    if (strncasecmp(abs, "http", 4)) continue;
    int dup = 0;
    for (int i = 0; i < n; i++) dup |= !strcmp(cand[i], abs);
    if (!dup) str_lcpy(cand[n++], abs, 1024);
  }
  /* players after real files */
  for (int i = 0, j = n - 1; i < j;) {
    if (looks_like_player(cand[i]) && !looks_like_player(cand[j])) {
      char t[1024];
      memcpy(t, cand[i], sizeof t);
      memcpy(cand[i], cand[j], sizeof t);
      memcpy(cand[j], t, sizeof t);
      i++;
      j--;
    } else if (!looks_like_player(cand[i])) {
      i++;
    } else {
      j--;
    }
  }
  /* 2. watch pages and frames */
  const char *pos = html;
  attr_t a;
  while (n < max && next_attr(html, &pos, &a)) {
    char raw[1024], abs[1024];
    if (a.len >= sizeof raw) continue;
    memcpy(raw, a.at, a.len);
    raw[a.len] = 0;
    absolutize(page_url, raw, abs, sizeof abs);
    /* Flash Museum's game page frames player.flashmuseum.net/game/SLUG/ */
    int watch = str_contains_ci(abs, "flash.asp") || str_contains_ci(abs, "/embed/") ||
                (a.is_src && allowed_host(abs) &&
                 (str_contains_ci(abs, ".htm") || str_contains_ci(abs, "player.flashmuseum.net/")));
    if (!watch || strncasecmp(abs, "http", 4)) continue;
    int dup = 0;
    for (int i = 0; i < n; i++) dup |= !strcmp(cand[i], abs);
    if (!dup) str_lcpy(cand[n++], abs, 1024);
  }
  return n;
}

typedef struct {
  pthread_mutex_t lock;
  int running, finished;
  char key[17];
  char url[2048], title[256], source[32];
  char stage[64];
  long long done, total;
  char status[16];
  char message[1024];
} web_job_t;

static web_job_t g_job = {.lock = PTHREAD_MUTEX_INITIALIZER};

static int job_progress(void *ud, long long done, long long total) {
  (void)ud;
  pthread_mutex_lock(&g_job.lock);
  g_job.done = done;
  g_job.total = total;
  pthread_mutex_unlock(&g_job.lock);
  return 0;
}

static void job_stage(const char *stage) {
  pthread_mutex_lock(&g_job.lock);
  str_lcpy(g_job.stage, stage, sizeof g_job.stage);
  g_job.done = 0;
  g_job.total = -1;
  pthread_mutex_unlock(&g_job.lock);
}

/* Downloads url (found on page `from`, NULL for the first) into dest; if
 * it is a page, follows what it embeds. */
static int resolve_swf(const char *url, const char *from, const char *dest, int depth, int *budget, char *err,
                       size_t errlen) {
  if (*budget <= 0) return -1;
  (*budget)--;
  char tmp[FP_PATH_MAX], referer[1024];
  snprintf(tmp, sizeof tmp, "%s.try%d", dest, *budget);
  unlink(tmp);
  referer_for(from, url, referer, sizeof referer);
  if (web_download(url, referer[0] ? referer : NULL, tmp, job_progress, NULL, err, errlen)) return -1;
  if (is_swf_file(tmp)) {
    unlink(dest);
    if (rename(tmp, dest)) {
      snprintf(err, errlen, "Can't write %s", dest);
      return -1;
    }
    return 0;
  }
  if (depth >= 3 || file_size(tmp) > 4 * 1024 * 1024) {
    unlink(tmp);
    snprintf(err, errlen, "No Flash file found on that page");
    return -1;
  }
  size_t len = 0;
  char *html = read_text_file(tmp, &len);
  unlink(tmp);
  if (!html) return -1;
  char (*cand)[1024] = calloc(12, 1024);
  int n = cand ? collect_candidates(url, html, cand, 12) : 0;
  if (n == 0) {
    /* for the log: how the page starts its player, to fix the next version */
    const char *hint = strcasestr(html, "RufflePlayer");
    if (!hint) hint = strcasestr(html, ".load(");
    if (hint) LOG("web: %s names no .swf; near the player: %.240s", url, hint);
    else LOG("web: %s names no .swf (%zu bytes)", url, len);
  }
  /* kept for the log in case none of the files downloads: how the page
   * names them (an address built in a script, a token, another host...) */
  char near[400] = "";
  const char *swf = strcasestr(html, ".swf");
  if (swf) {
    const char *from_p = swf - html > 240 ? swf - 240 : html;
    size_t k = 0;
    for (const char *c = from_p; *c && c < swf + 120 && k + 1 < sizeof near; c++)
      near[k++] = (*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c;
    near[k] = 0;
  }
  free(html);
  for (int i = 0; i < n; i++) {
    if (!strcmp(cand[i], url)) continue;
    LOG("web: trying %s", cand[i]);
    if (!resolve_swf(cand[i], url, dest, depth + 1, budget, err, errlen)) {
      free(cand);
      return 0;
    }
  }
  free(cand);
  if (n && near[0]) LOG("web: no file from %s downloaded; the page near .swf: %s", url, near);
  if (!err[0] || n == 0) snprintf(err, errlen, "No Flash file found on that page");
  return -1;
}

static void safe_name(const char *title, char *out, size_t size) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)title; *p && o + 1 < size && o < 60; p++) {
    out[o++] = (isalnum(*p) || *p == '-' || *p == '_' || *p == ' ' || *p == '(' || *p == ')')
                 ? (char)*p
                 : '_';
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = 0;
  if (!o) str_lcpy(out, "flash", size);
}

static void *job_main(void *arg) {
  (void)arg;
  char url[2048], title[256], key[17];
  pthread_mutex_lock(&g_job.lock);
  str_lcpy(url, g_job.url, sizeof url);
  str_lcpy(title, g_job.title, sizeof title);
  str_lcpy(key, g_job.key, sizeof key);
  pthread_mutex_unlock(&g_job.lock);

  /* Beside the unpacked Flashpoint games: Ruffle Flash (a title with its own
   * sandbox) can read there. */
  char dir[FP_PATH_MAX], sub[64], name[96], file[128], swf[FP_PATH_MAX], err[512] = "";
  snprintf(sub, sizeof sub, "web/%s", key);
  path_join(dir, sizeof dir, g_cfg.run_path, sub);
  safe_name(title, name, sizeof name);
  snprintf(file, sizeof file, "%s.swf", name);
  path_join(swf, sizeof swf, dir, file);
  mkdirs(dir);

  int rc = 0;
  if (!is_file(swf) || !is_swf_file(swf)) {
    job_stage(str_ends_with_ci(url, ".swf") ? "Downloading" : "Finding the Flash file");
    int budget = 8;
    rc = resolve_swf(url, NULL, swf, 0, &budget, err, sizeof err);
  }
  char msg[1024];
  const char *status = "launched";
  if (rc) {
    status = "error";
    snprintf(msg, sizeof msg, "Couldn't get the Flash file: %s", err[0] ? err : "unknown error");
  } else {
    job_stage("Starting Ruffle Flash");
    char tag[40];
    snprintf(tag, sizeof tag, "web-%s", key);
    if (launch_swf(tag, title, swf, url, msg, sizeof msg)) status = "error";
  }
  LOG("web: %s (%s): %s", title, url, msg);
  pthread_mutex_lock(&g_job.lock);
  g_job.running = 0;
  g_job.finished = 1;
  str_lcpy(g_job.status, status, sizeof g_job.status);
  str_lcpy(g_job.message, msg, sizeof g_job.message);
  pthread_mutex_unlock(&g_job.lock);
  return NULL;
}

char *web_play_json(const char *url, const char *title, const char *source) {
  if (!url || strncasecmp(url, "http", 4)) {
    return str_dup("{\"status\":\"error\",\"message\":\"That isn't a web address (it should start with http:// or https://).\"}");
  }
  char key[17];
  hash_hex(url, key);
  sb_t sb;
  sb_init(&sb);
  pthread_mutex_lock(&g_job.lock);
  if (g_job.finished && !strcmp(g_job.key, key)) {
    g_job.finished = 0;
    sb_append(&sb, "{\"status\":");
    sb_json_str(&sb, g_job.status);
    sb_append(&sb, ",\"message\":");
    sb_json_str(&sb, g_job.message);
    sb_append(&sb, "}");
    pthread_mutex_unlock(&g_job.lock);
    return sb_steal(&sb);
  }
  if (g_job.running && strcmp(g_job.key, key)) {
    pthread_mutex_unlock(&g_job.lock);
    sb_free(&sb);
    return str_dup("{\"status\":\"error\",\"message\":\"Another Flash file is still downloading. Try again in a moment.\"}");
  }
  if (!g_job.running) {
    memset(g_job.stage, 0, sizeof g_job.stage);
    str_lcpy(g_job.key, key, sizeof g_job.key);
    str_lcpy(g_job.url, url, sizeof g_job.url);
    char t[256];
    if (title && title[0]) {
      str_lcpy(t, title, sizeof t);
    } else {
      const char *f = strrchr(url, '/');
      f = f && f[1] ? f + 1 : url;
      title_from_file(f, strcspn(f, "?#"), t, sizeof t);
    }
    str_lcpy(g_job.title, t, sizeof g_job.title);
    str_lcpy(g_job.source, source ? source : "", sizeof g_job.source);
    str_lcpy(g_job.stage, "Starting", sizeof g_job.stage);
    g_job.done = 0;
    g_job.total = -1;
    g_job.running = 1;
    g_job.finished = 0;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, job_main, NULL)) {
      g_job.running = 0;
      pthread_attr_destroy(&attr);
      pthread_mutex_unlock(&g_job.lock);
      sb_free(&sb);
      return str_dup("{\"status\":\"error\",\"message\":\"Couldn't start the download.\"}");
    }
    pthread_attr_destroy(&attr);
  }
  sb_append(&sb, "{\"status\":\"pending\",\"stage\":");
  sb_json_str(&sb, g_job.stage);
  sb_printf(&sb, ",\"done\":%lld,\"total\":%lld,\"title\":", g_job.done, g_job.total);
  sb_json_str(&sb, g_job.title);
  sb_append(&sb, "}");
  pthread_mutex_unlock(&g_job.lock);
  return sb_steal(&sb);
}
