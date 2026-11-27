/* Flashpoint PS5 - downloads the Flashpoint library (flashpoint.sqlite)
 * from Flashpoint's metadata server (FPFSS), the way the Flashpoint
 * launcher's metadata sync and FPA-Rust's flashpoint-database-builder do:
 *   GET /api/games/updates?after=D   -> {"total": N}
 *   GET /api/platforms               -> [{id, name, ...}]
 *   GET /api/tags                    -> {"tags": [{id, name, ...}], ...}
 *   GET /api/games?broad=true&after=D[&afterId=ID]
 *       -> {games, add_apps, game_data, tag_relations, platform_relations}
 *          (pages until "games" is empty)
 *   GET /api/games/deleted           -> {"games": [{id}, ...]}
 * The result is a smaller database with the columns library.c reads, so
 * the PS5 needs no PC copy. Later runs only fetch what changed.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sync.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../third_party/cjson/cJSON.h"
#include "../third_party/sqlite/sqlite3.h"
#include "config.h"
#include "library.h"
#include "net.h"
#include "util.h"

/* Tags the online Flashpoint Database hides by default (db-api's
 * filteredTags): an entry with any of them is marked extreme. */
static const char *const k_extreme_tags[] = {
  "Adult", "Ambiguous Penetration", "Anal", "Anal Insertion", "Andromorph", "Anilingus", "BDSM",
  "Bestiality", "Breast Milking", "Cannibalism", "Cartoon Porn", "Cunnilingus", "Enema",
  "Fellatio", "Fingering", "Fisting", "Flatulence", "Footjob", "Frottage", "Futanari",
  "Gloryhole", "Gore", "Group", "Gynomorph", "Handjob", "Hentai", "Homophobia", "Hypnosis",
  "Incest", "Infantilism", "Inflation", "Intersex", "Interspecies", "Kabeshiri",
  "LEGACY-Extreme", "Macrophilia", "Male Futanari", "Masturbation", "Multiple Penises",
  "Necrophilia", "Obesity", "Oral", "Oviposition", "Paizuri", "Podophilia", "Porn", "Pregnancy",
  "Quicksand", "Racism", "Scat", "Sex Toys", "Sexual Content", "Sexual Violence", "Solo",
  "Spanking", "Stereotyping", "Strong Language", "Strong Violence", "Tentacles", "Tickling",
  "Touching", "Transphobia", "Tribadism", "Urination", "Vaginal", "Vaginal Insertion", "Vomit",
  "Vore", "Weight Gain", NULL};

static const char k_schema[] =
  "CREATE TABLE IF NOT EXISTS game ("
  " id TEXT PRIMARY KEY, library TEXT, title TEXT, alternateTitles TEXT, series TEXT,"
  " developer TEXT, publisher TEXT, platformName TEXT, platformsStr TEXT, dateAdded TEXT,"
  " dateModified TEXT, broken INTEGER DEFAULT 0, extreme INTEGER DEFAULT 0, playMode TEXT,"
  " status TEXT, notes TEXT, tagsStr TEXT, source TEXT, applicationPath TEXT,"
  " launchCommand TEXT, releaseDate TEXT, version TEXT, originalDescription TEXT,"
  " language TEXT, activeDataId INTEGER, activeDataOnDisk INTEGER DEFAULT 0,"
  " playtime INTEGER DEFAULT 0, playCounter INTEGER DEFAULT 0, lastPlayed TEXT,"
  " archiveState INTEGER, logoPath TEXT, screenshotPath TEXT, ruffleSupport TEXT);"
  "CREATE TABLE IF NOT EXISTS game_data ("
  " id INTEGER PRIMARY KEY AUTOINCREMENT, gameId TEXT, title TEXT, dateAdded TEXT,"
  " sha256 TEXT, crc32 INTEGER, presentOnDisk INTEGER DEFAULT 0, path TEXT, size INTEGER,"
  " parameters TEXT, applicationPath TEXT, launchCommand TEXT, UNIQUE(gameId, dateAdded));"
  "CREATE TABLE IF NOT EXISTS additional_app ("
  " id TEXT PRIMARY KEY, applicationPath TEXT, autoRunBefore INTEGER, launchCommand TEXT,"
  " name TEXT, waitForExit INTEGER, parentGameId TEXT);"
  "CREATE TABLE IF NOT EXISTS fpps5_sync (key TEXT PRIMARY KEY, value TEXT);"
  "CREATE INDEX IF NOT EXISTS game_data_game ON game_data(gameId);"
  "CREATE INDEX IF NOT EXISTS add_app_parent ON additional_app(parentGameId);";
/* (the search indexes come from lib_create_indexes) */

/* ---------------------------------------------------------------- status */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running;
static int g_state = SYNC_IDLE;
static long long g_done, g_total;
static char g_message[256];
static char g_last_sync[64];

static void set_status(int state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(int state, const char *fmt, ...) {
  pthread_mutex_lock(&g_lock);
  g_state = state;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_message, sizeof g_message, fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&g_lock);
  LOG("sync: %s", g_message);
}

static void set_progress(long long done, long long total) {
  pthread_mutex_lock(&g_lock);
  g_done = done;
  if (total >= 0) g_total = total;
  pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------ id -> name */

typedef struct {
  long long id;
  char *name;
} named_t;

typedef struct {
  named_t *items;
  size_t n, cap;
} names_t;

static int cmp_named(const void *a, const void *b) {
  long long x = ((const named_t *)a)->id, y = ((const named_t *)b)->id;
  return x < y ? -1 : x > y;
}

static void names_free(names_t *m) {
  for (size_t i = 0; i < m->n; i++) free(m->items[i].name);
  free(m->items);
  memset(m, 0, sizeof *m);
}

static void names_add(names_t *m, long long id, const char *name) {
  if (m->n == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 1024;
    m->items = realloc(m->items, m->cap * sizeof *m->items);
  }
  m->items[m->n].id = id;
  m->items[m->n].name = str_dup(name);
  m->n++;
}

static const char *names_get(const names_t *m, long long id) {
  named_t key = {id, NULL};
  named_t *hit = bsearch(&key, m->items, m->n, sizeof *m->items, cmp_named);
  return hit ? hit->name : NULL;
}

static int is_extreme_tag(const char *name) {
  for (int i = 0; k_extreme_tags[i]; i++) {
    if (!strcmp(name, k_extreme_tags[i])) return 1;
  }
  return 0;
}

/* -------------------------------------------------------------- fetching */

static char g_base[256];

static long long json_id(const cJSON *v) {
  if (cJSON_IsNumber(v)) return (long long)v->valuedouble;
  if (cJSON_IsString(v) && v->valuestring) return atoll(v->valuestring);
  return -1;
}

static const char *json_str(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static double json_num(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(v)) return v->valuedouble;
  if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
  return 0;
}

static void encode_query(sb_t *sb, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
        strchr("-_.~:", *p))
      sb_appendn(sb, (const char *)p, 1);
    else
      sb_printf(sb, "%%%02X", *p);
  }
}

/* GETs base + path and parses it; NULL (and a logged reason) on failure. */
static cJSON *fetch_json(const char *base, const char *path) {
  char url[1024], tmp[FP_PATH_MAX], err[256];
  snprintf(url, sizeof url, "%s%s", base, path);
  snprintf(tmp, sizeof tmp, "%s/sync-%d.json", g_cfg.cache_path, (int)getpid());
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!net_download(url, tmp, NULL, NULL, err, sizeof err)) {
      size_t len = 0;
      char *text = read_text_file(tmp, &len);
      unlink(tmp);
      cJSON *root = text ? cJSON_ParseWithLength(text, len) : NULL;
      free(text);
      if (root) return root;
      LOG("sync: %s is not JSON", url);
      return NULL;
    }
    LOG("sync: %s: %s", url, err);
    sleep(2 + attempt * 3);
  }
  return NULL;
}

/* The first metadata server that answers /api/games/updates. */
static long long pick_server(const char *after) {
  for (int i = 0; i < g_cfg.n_metadata_sources; i++) {
    sb_t path;
    sb_init(&path);
    sb_append(&path, "/api/games/updates?after=");
    encode_query(&path, after);
    cJSON *root = fetch_json(g_cfg.metadata_sources[i], path.buf);
    sb_free(&path);
    if (!root) continue;
    long long total = (long long)json_num(root, "total");
    cJSON_Delete(root);
    str_lcpy(g_base, g_cfg.metadata_sources[i], sizeof g_base);
    return total;
  }
  return -1;
}

/* ------------------------------------------------------------- database */

static int exec(sqlite3 *db, const char *sql) {
  char *msg = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
  if (rc != SQLITE_OK) {
    LOG("sync: SQL failed: %s", msg ? msg : sqlite3_errstr(rc));
    sqlite3_free(msg);
  }
  return rc;
}

static void get_state(sqlite3 *db, const char *key, char *out, size_t size) {
  out[0] = 0;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, "SELECT value FROM fpps5_sync WHERE key = ?", -1, &st, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
    str_lcpy(out, (const char *)sqlite3_column_text(st, 0), size);
  sqlite3_finalize(st);
}

static void put_state(sqlite3 *db, const char *key, const char *value) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO fpps5_sync (key, value) VALUES (?, ?)", -1, &st,
                         NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

/* Per-game strings built from the page's relations. */
typedef struct {
  const char *id;
  sb_t tags, platforms;
  int extreme;
} game_extra_t;

static int cmp_extra(const void *a, const void *b) {
  return strcmp(((const game_extra_t *)a)->id, ((const game_extra_t *)b)->id);
}

static game_extra_t *find_extra(game_extra_t *ex, size_t n, const char *id) {
  game_extra_t key = {.id = id};
  return bsearch(&key, ex, n, sizeof *ex, cmp_extra);
}

static void add_relations(game_extra_t *ex, size_t n, const cJSON *rels, const names_t *names,
                          int tags) {
  const cJSON *rel;
  cJSON_ArrayForEach(rel, rels) {
    const cJSON *gid = cJSON_GetArrayItem(rel, 0);
    const cJSON *oid = cJSON_GetArrayItem(rel, 1);
    if (!cJSON_IsString(gid)) continue;
    game_extra_t *e = find_extra(ex, n, gid->valuestring);
    const char *name = names_get(names, json_id(oid));
    if (!e || !name) continue;
    sb_t *sb = tags ? &e->tags : &e->platforms;
    if (sb->len) sb_append(sb, "; ");
    sb_append(sb, name);
    if (tags && is_extreme_tag(name)) e->extreme = 1;
  }
}

static void bind_s(sqlite3_stmt *st, int i, const char *s) { sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT); }

/* Applies one /api/games page; returns how many games it held. */
static int apply_page(sqlite3 *db, const cJSON *page, const names_t *tags, const names_t *plats,
                      char *last_id, size_t last_id_size, char *latest, size_t latest_size) {
  const cJSON *games = cJSON_GetObjectItemCaseSensitive(page, "games");
  int n = cJSON_GetArraySize(games);
  if (n <= 0) return 0;

  game_extra_t *ex = calloc((size_t)n, sizeof *ex);
  int k = 0;
  const cJSON *g;
  cJSON_ArrayForEach(g, games) {
    const char *id = json_str(g, "id");
    if (!id[0]) continue;
    ex[k].id = id;
    sb_init(&ex[k].tags);
    sb_init(&ex[k].platforms);
    k++;
    str_lcpy(last_id, id, last_id_size);
  }
  qsort(ex, (size_t)k, sizeof *ex, cmp_extra);
  add_relations(ex, (size_t)k, cJSON_GetObjectItemCaseSensitive(page, "tag_relations"), tags, 1);
  add_relations(ex, (size_t)k, cJSON_GetObjectItemCaseSensitive(page, "platform_relations"), plats, 0);

  exec(db, "BEGIN");
  sqlite3_stmt *ins = NULL, *del_apps = NULL, *ins_app = NULL, *ins_data = NULL, *active = NULL;
  sqlite3_prepare_v2(
    db,
    "INSERT INTO game (id, library, title, alternateTitles, series, developer, publisher,"
    " platformName, platformsStr, dateAdded, dateModified, extreme, playMode, status, notes,"
    " tagsStr, source, applicationPath, launchCommand, releaseDate, version,"
    " originalDescription, language, archiveState, logoPath, screenshotPath, ruffleSupport)"
    " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,"
    "?23,?24,?25,?26,?27)"
    " ON CONFLICT(id) DO UPDATE SET library=?2, title=?3, alternateTitles=?4, series=?5,"
    " developer=?6, publisher=?7, platformName=?8, platformsStr=?9, dateAdded=?10,"
    " dateModified=?11, extreme=?12, playMode=?13, status=?14, notes=?15, tagsStr=?16,"
    " source=?17, applicationPath=?18, launchCommand=?19, releaseDate=?20, version=?21,"
    " originalDescription=?22, language=?23, archiveState=?24, logoPath=?25,"
    " screenshotPath=?26, ruffleSupport=?27",
    -1, &ins, NULL);
  sqlite3_prepare_v2(db, "DELETE FROM additional_app WHERE parentGameId = ?", -1, &del_apps, NULL);
  sqlite3_prepare_v2(db,
                     "INSERT OR REPLACE INTO additional_app (id, applicationPath, autoRunBefore,"
                     " launchCommand, name, waitForExit, parentGameId) VALUES (?,?,?,?,?,?,?)",
                     -1, &ins_app, NULL);
  sqlite3_prepare_v2(db,
                     "INSERT INTO game_data (gameId, title, dateAdded, sha256, crc32, size,"
                     " parameters, applicationPath, launchCommand) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)"
                     " ON CONFLICT(gameId, dateAdded) DO UPDATE SET parameters=?7,"
                     " applicationPath=?8, launchCommand=?9, sha256=?4, size=?6",
                     -1, &ins_data, NULL);
  sqlite3_prepare_v2(db,
                     "UPDATE game SET activeDataId = (SELECT gd.id FROM game_data gd WHERE"
                     " gd.gameId = game.id ORDER BY gd.dateAdded DESC LIMIT 1),"
                     " activeDataOnDisk = COALESCE((SELECT gd.presentOnDisk FROM game_data gd"
                     " WHERE gd.gameId = game.id ORDER BY gd.dateAdded DESC LIMIT 1), 0)"
                     " WHERE id = ?",
                     -1, &active, NULL);
  if (!ins || !del_apps || !ins_app || !ins_data || !active) {
    LOG("sync: prepare failed: %s", sqlite3_errmsg(db));
    k = -1;
    goto done;
  }

  cJSON_ArrayForEach(g, games) {
    const char *id = json_str(g, "id");
    if (!id[0]) continue;
    game_extra_t *e = find_extra(ex, (size_t)k, id);
    const char *modified = json_str(g, "date_modified");
    if (strcmp(modified, latest) > 0) str_lcpy(latest, modified, latest_size);
    sqlite3_reset(ins);
    bind_s(ins, 1, id);
    bind_s(ins, 2, json_str(g, "library"));
    bind_s(ins, 3, json_str(g, "title"));
    bind_s(ins, 4, json_str(g, "alternate_titles"));
    bind_s(ins, 5, json_str(g, "series"));
    bind_s(ins, 6, json_str(g, "developer"));
    bind_s(ins, 7, json_str(g, "publisher"));
    bind_s(ins, 8, json_str(g, "platform_name"));
    bind_s(ins, 9, e && e->platforms.len ? e->platforms.buf : json_str(g, "platform_name"));
    bind_s(ins, 10, json_str(g, "date_added"));
    bind_s(ins, 11, modified);
    sqlite3_bind_int(ins, 12, e ? e->extreme : 0);
    bind_s(ins, 13, json_str(g, "play_mode"));
    bind_s(ins, 14, json_str(g, "status"));
    bind_s(ins, 15, json_str(g, "notes"));
    bind_s(ins, 16, e && e->tags.len ? e->tags.buf : "");
    bind_s(ins, 17, json_str(g, "source"));
    bind_s(ins, 18, json_str(g, "application_path"));
    bind_s(ins, 19, json_str(g, "launch_command"));
    bind_s(ins, 20, json_str(g, "release_date"));
    bind_s(ins, 21, json_str(g, "version"));
    bind_s(ins, 22, json_str(g, "original_description"));
    bind_s(ins, 23, json_str(g, "language"));
    sqlite3_bind_int(ins, 24, (int)json_num(g, "archive_state"));
    bind_s(ins, 25, json_str(g, "logo_path"));
    bind_s(ins, 26, json_str(g, "screenshot_path"));
    bind_s(ins, 27, json_str(g, "ruffle_support"));
    if (sqlite3_step(ins) != SQLITE_DONE) LOG("sync: game %s: %s", id, sqlite3_errmsg(db));
    sqlite3_reset(del_apps);
    bind_s(del_apps, 1, id);
    sqlite3_step(del_apps);
  }

  int app_no = 0;
  const cJSON *a;
  cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(page, "add_apps")) {
    char app_id[128];
    snprintf(app_id, sizeof app_id, "%s-%d", json_str(a, "parent_game_id"), app_no++);
    sqlite3_reset(ins_app);
    bind_s(ins_app, 1, app_id);
    bind_s(ins_app, 2, json_str(a, "application_path"));
    sqlite3_bind_int(ins_app, 3, (int)json_num(a, "auto_run_before"));
    bind_s(ins_app, 4, json_str(a, "launch_command"));
    bind_s(ins_app, 5, json_str(a, "name"));
    sqlite3_bind_int(ins_app, 6, (int)json_num(a, "wait_for_exit"));
    bind_s(ins_app, 7, json_str(a, "parent_game_id"));
    sqlite3_step(ins_app);
  }

  const cJSON *d;
  cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(page, "game_data")) {
    sqlite3_reset(ins_data);
    bind_s(ins_data, 1, json_str(d, "game_id"));
    bind_s(ins_data, 2, json_str(d, "title"));
    bind_s(ins_data, 3, json_str(d, "date_added"));
    bind_s(ins_data, 4, json_str(d, "sha_256"));
    sqlite3_bind_int64(ins_data, 5, (long long)json_num(d, "crc_32"));
    sqlite3_bind_int64(ins_data, 6, (long long)json_num(d, "size"));
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(d, "parameters");
    if (cJSON_IsString(params)) bind_s(ins_data, 7, params->valuestring);
    else sqlite3_bind_null(ins_data, 7);
    bind_s(ins_data, 8, json_str(d, "application_path"));
    bind_s(ins_data, 9, json_str(d, "launch_command"));
    if (sqlite3_step(ins_data) != SQLITE_DONE) LOG("sync: game data: %s", sqlite3_errmsg(db));
  }

  for (int i = 0; i < k; i++) {
    sqlite3_reset(active);
    bind_s(active, 1, ex[i].id);
    sqlite3_step(active);
  }

done:
  sqlite3_finalize(ins);
  sqlite3_finalize(del_apps);
  sqlite3_finalize(ins_app);
  sqlite3_finalize(ins_data);
  sqlite3_finalize(active);
  if (k >= 0) put_state(db, "lastId", last_id);
  exec(db, k >= 0 ? "COMMIT" : "ROLLBACK");
  for (int i = 0; i < n && ex[i].id; i++) {
    sb_free(&ex[i].tags);
    sb_free(&ex[i].platforms);
  }
  free(ex);
  return k;
}

static void apply_deleted(sqlite3 *db) {
  cJSON *root = fetch_json(g_base, "/api/games/deleted");
  if (!root) return;
  lib_write_lock();
  sqlite3_stmt *st[4] = {NULL};
  const char *sql[4] = {"DELETE FROM game WHERE id = ?", "DELETE FROM game_data WHERE gameId = ?",
                        "DELETE FROM additional_app WHERE parentGameId = ?", NULL};
  exec(db, "BEGIN");
  for (int i = 0; sql[i]; i++) sqlite3_prepare_v2(db, sql[i], -1, &st[i], NULL);
  int count = 0;
  const cJSON *g;
  cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(root, "games")) {
    const char *id = json_str(g, "id");
    if (!id[0]) continue;
    for (int i = 0; st[i]; i++) {
      sqlite3_reset(st[i]);
      bind_s(st[i], 1, id);
      sqlite3_step(st[i]);
    }
    count += sqlite3_changes(db) > 0;
  }
  for (int i = 0; i < 4; i++) sqlite3_finalize(st[i]);
  exec(db, "COMMIT");
  lib_write_unlock();
  cJSON_Delete(root);
  LOG("sync: removed %d deleted entries", count);
}

static int load_platforms(names_t *plats) {
  cJSON *root = fetch_json(g_base, "/api/platforms");
  if (!root) return -1;
  const cJSON *p;
  cJSON_ArrayForEach(p, root) names_add(plats, json_id(cJSON_GetObjectItemCaseSensitive(p, "id")), json_str(p, "name"));
  cJSON_Delete(root);
  qsort(plats->items, plats->n, sizeof *plats->items, cmp_named);
  return 0;
}

static int load_tags(names_t *tags) {
  cJSON *root = fetch_json(g_base, "/api/tags");
  if (!root) return -1;
  const cJSON *list = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "tags");
  const cJSON *t;
  cJSON_ArrayForEach(t, list) names_add(tags, json_id(cJSON_GetObjectItemCaseSensitive(t, "id")), json_str(t, "name"));
  cJSON_Delete(root);
  qsort(tags->items, tags->n, sizeof *tags->items, cmp_named);
  return 0;
}

static void *sync_main(void *arg) {
  (void)arg;
  sqlite3 *db = NULL;
  names_t tags = {0}, plats = {0};
  char after[64] = "1970-01-01", last_id[128] = "", latest[64] = "";
  int fresh = 1;

  int locked = 0; /* lib_write_lock: the index builder may be busy with the same file */

  mkdirs(g_cfg.cache_path);
  mkdirs_for_file(g_cfg.db_path);
  set_progress(0, 0);
  set_status(SYNC_RUNNING, "Connecting to Flashpoint");
  lib_write_lock();
  locked = 1;
  if (sqlite3_open_v2(g_cfg.db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
    set_status(SYNC_ERROR, "Can't create %s", g_cfg.db_path);
    goto out;
  }
  sqlite3_busy_timeout(db, 10000);
  exec(db, "PRAGMA journal_mode=DELETE");
  exec(db, "PRAGMA synchronous=NORMAL");
  /* A page of entries fits in the cache, so it is written in one go at
   * COMMIT: searches can keep reading until then instead of waiting while
   * a spilled transaction holds the file. */
  exec(db, "PRAGMA cache_size=-65536");
  exec(db, "PRAGMA temp_store=MEMORY");
  char kind[32];
  sqlite3_stmt *probe;
  int has_game = 0;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name='game'", -1, &probe, NULL) == SQLITE_OK) {
    has_game = sqlite3_step(probe) == SQLITE_ROW;
    sqlite3_finalize(probe);
  }
  if (has_game) {
    kind[0] = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name='fpps5_sync'", -1, &probe,
                           NULL) == SQLITE_OK) {
      if (sqlite3_step(probe) == SQLITE_ROW) str_lcpy(kind, "?", sizeof kind);
      sqlite3_finalize(probe);
    }
    if (kind[0]) get_state(db, "kind", kind, sizeof kind);
    if (strcmp(kind, "fpfss")) {
      set_status(SYNC_ERROR, "This library came from a PC; update it with Flashpoint on the PC");
      goto out;
    }
    char state[64];
    get_state(db, "complete", state, sizeof state);
    if (state[0]) {
      get_state(db, "latest", after, sizeof after);
      fresh = 0;
    } else {
      get_state(db, "lastId", last_id, sizeof last_id); /* resume an interrupted download */
    }
  } else if (exec(db, k_schema) != SQLITE_OK) {
    set_status(SYNC_ERROR, "Can't create the library database");
    goto out;
  } else {
    lib_create_indexes(db); /* on the empty table: instant, then kept up as entries arrive */
  }
  put_state(db, "kind", "fpfss");
  lib_write_unlock();
  locked = 0;
  if (!after[0]) str_lcpy(after, "1970-01-01", sizeof after);
  str_lcpy(latest, after, sizeof latest);

  long long total = pick_server(after);
  if (total < 0) {
    set_status(SYNC_ERROR, "Flashpoint's metadata server didn't answer. Check the PS5's internet connection.");
    goto out;
  }
  LOG("sync: %s, %lld entries to fetch after %s", g_base, total, after);
  set_progress(0, total);
  if (load_platforms(&plats) || load_tags(&tags)) {
    set_status(SYNC_ERROR, "Couldn't read Flashpoint's platforms and tags");
    goto out;
  }

  long long done = 0;
  for (;;) {
    set_status(SYNC_RUNNING, fresh ? "Downloading the Flashpoint library" : "Updating the Flashpoint library");
    sb_t path;
    sb_init(&path);
    sb_append(&path, "/api/games?broad=true&after=");
    encode_query(&path, after);
    if (last_id[0]) {
      sb_append(&path, "&afterId=");
      encode_query(&path, last_id);
    }
    cJSON *page = fetch_json(g_base, path.buf);
    sb_free(&path);
    if (!page) {
      set_status(SYNC_ERROR, "The download stopped. It continues where it left off next time.");
      goto out;
    }
    lib_write_lock();
    int n = apply_page(db, page, &tags, &plats, last_id, sizeof last_id, latest, sizeof latest);
    lib_write_unlock();
    cJSON_Delete(page);
    if (n < 0) {
      set_status(SYNC_ERROR, "Writing the library failed (is the disk full?)");
      goto out;
    }
    if (n == 0) break;
    done += n;
    set_progress(done, total > done ? total : done);
    if (!lib_is_open()) lib_open(g_cfg.db_path); /* browse while the rest comes in */
  }
  if (!fresh) apply_deleted(db);
  lib_write_lock();
  put_state(db, "complete", "1");
  put_state(db, "latest", latest);
  put_state(db, "lastId", "");
  {
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%lld", (long long)now_seconds());
    put_state(db, "syncedAt", stamp);
    pthread_mutex_lock(&g_lock);
    str_lcpy(g_last_sync, stamp, sizeof g_last_sync);
    pthread_mutex_unlock(&g_lock);
  }
  lib_write_unlock();
  if (!lib_is_open()) lib_open(g_cfg.db_path);
  lib_refresh_count();
  set_status(SYNC_DONE, done ? "Library up to date (%lld entries added or changed)" : "Library up to date", done);

out:
  if (locked) lib_write_unlock();
  names_free(&tags);
  names_free(&plats);
  if (db) sqlite3_close(db);
  pthread_mutex_lock(&g_lock);
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int sync_start(void) {
  pthread_mutex_lock(&g_lock);
  if (g_running) {
    pthread_mutex_unlock(&g_lock);
    return 0;
  }
  g_running = 1;
  pthread_mutex_unlock(&g_lock);
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1 << 20);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&th, &attr, sync_main, NULL);
  pthread_attr_destroy(&attr);
  if (rc) {
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  return 0;
}

/* What the current database is: "fpfss" (downloaded here), "pc" (copied
 * from a PC Flashpoint), or "" (none), and when it was last downloaded. */
static void db_kind(char *kind, size_t ksize, char *synced, size_t ssize) {
  kind[0] = synced[0] = 0;
  if (!is_file(g_cfg.db_path)) return;
  sqlite3 *db;
  if (sqlite3_open_v2(g_cfg.db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    sqlite3_close(db);
    return;
  }
  str_lcpy(kind, "pc", ksize);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, "SELECT key, value FROM fpps5_sync", -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *k = (const char *)sqlite3_column_text(st, 0);
      const char *v = (const char *)sqlite3_column_text(st, 1);
      if (!k || !v) continue;
      if (!strcmp(k, "kind")) str_lcpy(kind, v, ksize);
      if (!strcmp(k, "syncedAt")) str_lcpy(synced, v, ssize);
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
}

char *sync_status_json(void) {
  char kind[32], synced[64];
  db_kind(kind, sizeof kind, synced, sizeof synced);
  pthread_mutex_lock(&g_lock);
  static const char *const names[] = {"idle", "running", "done", "error"};
  sb_t sb;
  sb_init(&sb);
  sb_printf(&sb, "{\"state\":\"%s\",\"running\":%s,\"done\":%lld,\"total\":%lld,\"message\":",
            names[g_state], g_running ? "true" : "false", g_done, g_total);
  sb_json_str(&sb, g_message);
  sb_append(&sb, ",\"source\":");
  sb_json_str(&sb, kind);
  sb_printf(&sb, ",\"syncedAt\":%lld,\"entries\":%lld}", atoll(synced[0] ? synced : g_last_sync),
            lib_game_count());
  pthread_mutex_unlock(&g_lock);
  return sb_steal(&sb);
}
