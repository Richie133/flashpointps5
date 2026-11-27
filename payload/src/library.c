/* Flashpoint PS5 - the local game library (flashpoint.sqlite).
 * The search endpoint mirrors FlashpointProject/flashpoint-database-api:
 * every metadata field can be matched (comma separated values, LIKE
 * %value%), with "any" switching AND to OR and "filter" hiding extreme
 * entries. Paging/sorting are done here instead of in the browser, since
 * the PS5's browser has no JIT.
 *
 * Speed: a whole library is ~240k entries and ~400 MB, and the PS5 reads
 * files slowly, so no search may read the game table itself. Narrow
 * indexes (fpps5_*, added once in the background) carry every column the
 * app filters and sorts on. A search walks one of them for the ordered
 * list of matching rows, keeps that list (the total and the next pages
 * then cost nothing), and reads only the rows of the page asked for.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "library.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../third_party/sqlite/sqlite3.h"
#include "config.h"
#include "util.h"

enum { T_STR, T_ARR, T_BOOL, T_NUM };

typedef struct field {
  const char *name;
  int type;
  int searchable;
} field_t;

/* Output/search fields, by their db-api names. The SQL for each comes from
 * build_exprs (it depends on the database's columns). */
static const field_t k_fields[] = {
  {"id", T_STR, 1},
  {"title", T_STR, 1},
  {"alternateTitles", T_STR, 1},
  {"series", T_STR, 1},
  {"developer", T_STR, 1},
  {"publisher", T_STR, 1},
  {"source", T_STR, 1},
  {"library", T_STR, 1},
  {"tags", T_ARR, 1},
  {"platform", T_STR, 1},
  {"playMode", T_STR, 1},
  {"status", T_STR, 1},
  {"version", T_STR, 1},
  {"releaseDate", T_STR, 1},
  {"language", T_STR, 1},
  {"notes", T_STR, 1},
  {"originalDescription", T_STR, 1},
  {"dateAdded", T_STR, 1},
  {"dateModified", T_STR, 1},
  {"applicationPath", T_STR, 1},
  {"launchCommand", T_STR, 1},
  {"zipped", T_BOOL, 1},
  {"ruffleSupport", T_STR, 1},
  /* launcher-side state */
  {"extreme", T_BOOL, 0},
  {"installed", T_BOOL, 0},
  {"lastPlayed", T_STR, 0},
  {"playtime", T_NUM, 0},
  {"playCounter", T_NUM, 0},
  {NULL, 0, 0},
};
#define N_FIELDS ((int)(sizeof k_fields / sizeof k_fields[0]) - 1)

static const char *k_default_fields =
  "id,title,developer,publisher,platform,library,tags,originalDescription,dateAdded,"
  "dateModified,zipped,installed,lastPlayed,playtime,extreme";

static sqlite3 *g_db;
static char *g_base_sql; /* every field, FROM game g LEFT JOIN game_data gd */
static long long g_game_count = -1;
static char *g_expr[N_FIELDS];       /* each field's SQL, over game g / game_data gd */
static unsigned char g_needs_gd[N_FIELDS];

/* Optional columns: copies from a PC differ by launcher version. */
static struct {
  int platforms_str, ruffle, playtime, counter, last_played, on_disk, extreme, logo, active_data;
} g_has;

/* Guards the cached lists and stats below; searches take it whole, which
 * also keeps two requests from building the same list at once. */
static pthread_mutex_t g_search_lock = PTHREAD_MUTEX_INITIALIZER;
/* One writer at a time among the index builder and the library download. */
static pthread_mutex_t g_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile long long g_local_writes; /* writes through g_db (plays, downloads) */

void lib_write_lock(void) { pthread_mutex_lock(&g_write_mutex); }
void lib_write_unlock(void) { pthread_mutex_unlock(&g_write_mutex); }

/* ------------------------------------------------------------ open */

static int has_column_in(sqlite3 *db, const char *table, const char *col) {
  char sql[128];
  snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
  sqlite3_stmt *st;
  int found = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(st, 1);
    if (name && !strcmp(name, col)) found = 1;
  }
  sqlite3_finalize(st);
  return found;
}

static int has_table_in(sqlite3 *db, const char *type, const char *name) {
  sqlite3_stmt *st;
  int found = 0;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type=? AND name=?", -1, &st, NULL) !=
      SQLITE_OK)
    return 0;
  sqlite3_bind_text(st, 1, type, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
  found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

static int has_table(const char *table) { return has_table_in(g_db, "table", table); }

static int field_index(const char *name) {
  for (int i = 0; i < N_FIELDS; i++) {
    if (!strcmp(k_fields[i].name, name)) return i;
  }
  return -1;
}

static void set_expr(const char *name, const char *sql, int gd) {
  int i = field_index(name);
  free(g_expr[i]);
  g_expr[i] = str_dup(sql);
  g_needs_gd[i] = (unsigned char)gd;
}

static void build_exprs(void) {
  g_has.platforms_str = has_column_in(g_db, "game", "platformsStr");
  g_has.ruffle = has_column_in(g_db, "game", "ruffleSupport");
  g_has.playtime = has_column_in(g_db, "game", "playtime");
  g_has.counter = has_column_in(g_db, "game", "playCounter");
  g_has.last_played = has_column_in(g_db, "game", "lastPlayed");
  g_has.on_disk = has_column_in(g_db, "game", "activeDataOnDisk");
  g_has.extreme = has_column_in(g_db, "game", "extreme");
  g_has.logo = has_column_in(g_db, "game", "logoPath");
  g_has.active_data = has_column_in(g_db, "game", "activeDataId");

  for (int i = 0; i < N_FIELDS; i++) {
    char sql[64];
    snprintf(sql, sizeof sql, "g.%s", k_fields[i].name);
    set_expr(k_fields[i].name, sql, 0);
  }
  set_expr("tags", "g.tagsStr", 0);
  set_expr("platform",
           g_has.platforms_str ? "COALESCE(NULLIF(g.platformsStr,''), g.platformName, '')"
                               : "COALESCE(g.platformName,'')",
           0);
  set_expr("applicationPath",
           "CASE WHEN gd.applicationPath IS NOT NULL AND gd.applicationPath != ''"
           " THEN gd.applicationPath ELSE g.applicationPath END",
           1);
  set_expr("launchCommand",
           "CASE WHEN gd.launchCommand IS NOT NULL AND gd.launchCommand != ''"
           " THEN gd.launchCommand ELSE g.launchCommand END",
           1);
  set_expr("zipped", g_has.active_data ? "CASE WHEN g.activeDataId IS NULL THEN 'false' ELSE 'true' END" : "'false'",
           0);
  set_expr("ruffleSupport", g_has.ruffle ? "COALESCE(g.ruffleSupport,'')" : "''", 0);
  set_expr("extreme", g_has.extreme ? "g.extreme" : "0", 0);
  set_expr("installed", g_has.on_disk ? "g.activeDataOnDisk" : "0", 0);
  set_expr("lastPlayed", g_has.last_played ? "g.lastPlayed" : "NULL", 0);
  set_expr("playtime", g_has.playtime ? "COALESCE(g.playtime,0)" : "0", 0);
  set_expr("playCounter", g_has.counter ? "COALESCE(g.playCounter,0)" : "0", 0);

  sb_t sb;
  sb_init(&sb);
  sb_append(&sb, "SELECT ");
  for (int i = 0; i < N_FIELDS; i++) sb_printf(&sb, "%s%s AS %s", i ? ", " : "", g_expr[i], k_fields[i].name);
  sb_append(&sb, " FROM game g LEFT JOIN game_data gd ON gd.id = g.activeDataId");
  free(g_base_sql);
  g_base_sql = sb_steal(&sb);
}

/* ------------------------------------------------------------ indexes */

enum { IX_TITLE = 1, IX_ADDED = 2, IX_TEXT = 4, IX_PLAYED = 8, IX_INSTALLED = 16, IX_ALL = 31 };
static const char *const k_ix_names[] = {"fpps5_title", "fpps5_added", "fpps5_text", "fpps5_played",
                                         "fpps5_installed"};
static int g_ix; /* the fpps5_* indexes the open database has */

typedef struct {
  const char *cols[16];
  int n;
  const char *where;
} ixcols_t;

static const char *ix_name(int ix) {
  for (int i = 0; i < 5; i++) {
    if (ix == 1 << i) return k_ix_names[i];
  }
  return NULL;
}

/* An index's columns for a database with db's columns; n = 0 when the
 * database lacks what it needs. Every browse index carries the filter
 * columns (extreme, platformName, platformsStr), so filtering never has to
 * read the game table. */
static void ix_columns(sqlite3 *db, int ix, ixcols_t *c) {
  memset(c, 0, sizeof *c);
  int extreme = has_column_in(db, "game", "extreme");
  int pstr = has_column_in(db, "game", "platformsStr");
#define ADD(s) (c->cols[c->n++] = (s))
#define FILTERS()               \
  do {                          \
    if (extreme) ADD("extreme"); \
    ADD("platformName");        \
    if (pstr) ADD("platformsStr"); \
  } while (0)
  switch (ix) {
  case IX_TITLE: /* the usual browse order; also the narrowest full scan */
    ADD("library");
    ADD("title COLLATE NOCASE");
    FILTERS();
    if (has_column_in(db, "game", "activeDataId")) ADD("activeDataId"); /* for stats */
    break;
  case IX_ADDED:
    ADD("library");
    ADD("dateAdded");
    FILTERS();
    break;
  case IX_TEXT: /* typed searches, and the sorts without an index of their own */
    ADD("library");
    FILTERS();
    ADD("title");
    ADD("alternateTitles");
    ADD("developer");
    ADD("publisher");
    ADD("series");
    ADD("releaseDate");
    break;
  case IX_PLAYED:
    if (!has_column_in(db, "game", "lastPlayed")) return;
    ADD("lastPlayed");
    ADD("library");
    if (extreme) ADD("extreme");
    if (has_column_in(db, "game", "playtime")) ADD("playtime");
    c->where = "lastPlayed IS NOT NULL AND lastPlayed != ''";
    break;
  case IX_INSTALLED:
    if (!has_column_in(db, "game", "activeDataOnDisk")) return;
    ADD("library");
    if (extreme) ADD("extreme");
    c->where = "activeDataOnDisk = 1";
    break;
  }
#undef FILTERS
#undef ADD
}

/* Adds the missing fpps5_* indexes; returns the ones that exist after. */
int lib_create_indexes(sqlite3 *db) {
  int have = 0;
  for (int ix = 1; ix <= IX_INSTALLED; ix <<= 1) {
    const char *name = ix_name(ix);
    if (has_table_in(db, "index", name)) {
      have |= ix;
      continue;
    }
    ixcols_t c;
    ix_columns(db, ix, &c);
    if (!c.n) continue;
    sb_t sql;
    sb_init(&sql);
    sb_printf(&sql, "CREATE INDEX IF NOT EXISTS %s ON game(", name);
    for (int i = 0; i < c.n; i++) sb_printf(&sql, "%s%s", i ? ", " : "", c.cols[i]);
    sb_append(&sql, ")");
    if (c.where) sb_printf(&sql, " WHERE %s", c.where);
    double t0 = now_seconds();
    char *msg = NULL;
    if (sqlite3_exec(db, sql.buf, NULL, NULL, &msg) == SQLITE_OK) {
      have |= ix;
      LOG("library: added index %s (%.1f s)", name, now_seconds() - t0);
    } else {
      LOG("library: couldn't add index %s: %s", name, msg ? msg : "?");
    }
    sqlite3_free(msg);
    sb_free(&sql);
  }
  return have;
}

/* A library downloaded by sync.c (ours to change), not a PC copy. */
static int is_synced_db(sqlite3 *db) {
  if (!has_table_in(db, "table", "fpps5_sync")) return 0;
  sqlite3_stmt *st;
  int ours = 0;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM fpps5_sync WHERE key='kind' AND value='fpfss'", -1, &st, NULL) ==
      SQLITE_OK) {
    ours = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
  }
  return ours;
}

/* Indexes of earlier versions, which the fpps5_* ones replace. */
static const char *const k_old_indexes[] = {"game_title", "game_library", "game_added", NULL};

static int scan_indexes(sqlite3 *db) {
  int have = 0;
  for (int ix = 1; ix <= IX_INSTALLED; ix <<= 1) {
    if (has_table_in(db, "index", ix_name(ix))) have |= ix;
  }
  return have;
}

static int indexes_wanted(sqlite3 *db) {
  int want = 0;
  for (int ix = 1; ix <= IX_INSTALLED; ix <<= 1) {
    ixcols_t c;
    ix_columns(db, ix, &c);
    if (c.n) want |= ix;
  }
  return want;
}

static void *index_main(void *arg) {
  char *path = arg;
  lib_write_lock();
  sqlite3 *db = NULL;
  double t0 = now_seconds();
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
    sqlite3_busy_timeout(db, 60000);
    /* room for a whole index, so readers are only held up by the commit */
    sqlite3_exec(db, "PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    LOG("library: adding search indexes (once; searches are slow until it's done)");
    if (is_synced_db(db)) {
      for (int i = 0; k_old_indexes[i]; i++) {
        char sql[96];
        snprintf(sql, sizeof sql, "DROP INDEX IF EXISTS %s", k_old_indexes[i]);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
      }
    }
    int have = lib_create_indexes(db);
    LOG("library: search indexes ready (%.1f s, %d of 5)", now_seconds() - t0, __builtin_popcount(have));
  } else {
    LOG("library: can't add search indexes to %s (read-only?); searches stay slow", path);
  }
  if (db) sqlite3_close(db);
  lib_write_unlock();
  int have = scan_indexes(g_db);
  pthread_mutex_lock(&g_search_lock);
  g_ix = have;
  pthread_mutex_unlock(&g_search_lock);
  free(path);
  return NULL;
}

/* Builds missing indexes in the background, so the service answers at once. */
static void start_index_builder(const char *db_path) {
  int want = indexes_wanted(g_db);
  int need = (g_ix & want) != want;
  if (!need && is_synced_db(g_db)) {
    for (int i = 0; k_old_indexes[i]; i++) need |= has_table_in(g_db, "index", k_old_indexes[i]);
  }
  if (!need) return;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  char *path = str_dup(db_path);
  if (pthread_create(&th, &attr, index_main, path)) {
    LOG("library: couldn't start the index builder");
    free(path);
  }
  pthread_attr_destroy(&attr);
}

int lib_open(const char *db_path) {
  if (!is_file(db_path)) {
    LOG("library: no database at %s", db_path);
    return -1;
  }
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK) {
    LOG("library: read-write open failed (%s), trying read-only", sqlite3_errstr(rc));
    if (db) sqlite3_close(db);
    db = NULL;
    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
  }
  if (rc != SQLITE_OK) {
    LOG("library: can't open %s: %s", db_path, sqlite3_errstr(rc));
    if (db) sqlite3_close(db);
    return -1;
  }
  sqlite3_busy_timeout(db, 5000);
  /* WAL needs shared memory, which removable drives on the PS5 may not
   * give us; the plain rollback journal works everywhere. */
  sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA cache_size=-16384", NULL, NULL, NULL);
  /* sorts stay in memory: there may be nowhere writable for temp files */
  sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
  if (!has_table_in(db, "table", "game")) {
    LOG("library: %s has no game table", db_path);
    sqlite3_close(db);
    return -1;
  }
  g_db = db;
  build_exprs();
  g_ix = scan_indexes(g_db);
  lib_refresh_count();
  LOG("library: opened %s (%lld entries)", db_path, g_game_count);
  start_index_builder(db_path);
  return 0;
}

int lib_is_open(void) { return g_db != NULL; }

void lib_refresh_count(void) {
  if (!g_db) return;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM game", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) g_game_count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
}
long long lib_game_count(void) { return g_game_count; }

/* ------------------------------------------------------------ caching */

/* Ordered row lists of recent searches, by their SQL and values. */
#define RS_SLOTS 8
typedef struct {
  char *key;
  long long *ids;
  long n;
  unsigned long long used;
} rowlist_t;

static rowlist_t g_lists[RS_SLOTS];
static unsigned long long g_clock;
static long long g_seen_version = -1, g_seen_writes = -1;
static char *g_stats; /* the /api/stats answer */

/* Forgets lists and stats once anything changed the library: a commit by
 * another connection (the download, the index builder) moves
 * data_version, and our own writes count in g_local_writes. Call with
 * g_search_lock held. */
static void forget_if_changed(void) {
  long long v = -1;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, "PRAGMA data_version", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  long long w = g_local_writes;
  if (v == g_seen_version && w == g_seen_writes && v >= 0) return;
  g_seen_version = v;
  g_seen_writes = w;
  for (int i = 0; i < RS_SLOTS; i++) {
    free(g_lists[i].key);
    free(g_lists[i].ids);
    memset(&g_lists[i], 0, sizeof g_lists[i]);
  }
  free(g_stats);
  g_stats = NULL;
}

static rowlist_t *list_find(const char *key) {
  for (int i = 0; i < RS_SLOTS; i++) {
    if (g_lists[i].key && !strcmp(g_lists[i].key, key)) {
      g_lists[i].used = ++g_clock;
      return &g_lists[i];
    }
  }
  return NULL;
}

static rowlist_t *list_store(char *key, long long *ids, long n) {
  rowlist_t *slot = &g_lists[0];
  for (int i = 0; i < RS_SLOTS; i++) {
    if (!g_lists[i].key) {
      slot = &g_lists[i];
      break;
    }
    if (g_lists[i].used < slot->used) slot = &g_lists[i];
  }
  free(slot->key);
  free(slot->ids);
  slot->key = key;
  slot->ids = ids;
  slot->n = n;
  slot->used = ++g_clock;
  return slot;
}

/* ------------------------------------------------------------ search */

static const field_t *find_field(const char *name) {
  int i = field_index(name);
  return i < 0 ? NULL : &k_fields[i];
}

typedef struct binds {
  char *v[256];
  int n;
} binds_t;

static void bind_value(binds_t *b, sb_t *sql, const char *expr, const char *op, const char *value) {
  if (b->n >= 256) {
    sb_append(sql, "1");
    return;
  }
  b->v[b->n++] = str_dup(value);
  sb_printf(sql, "%s %s ?", expr, op);
}

static void bind_like(binds_t *b, sb_t *sql, const char *expr, const char *value) {
  if (b->n >= 256) {
    sb_append(sql, "1");
    return;
  }
  sb_t v;
  sb_init(&v);
  sb_append(&v, "%");
  for (const char *p = value; *p; p++) {
    if (*p == '^' || *p == '%' || *p == '_') sb_appendn(&v, "^", 1);
    sb_appendn(&v, p, 1);
  }
  sb_append(&v, "%");
  b->v[b->n++] = sb_steal(&v);
  sb_printf(sql, "%s LIKE ? ESCAPE '^'", expr);
}

static void append_row_value(sb_t *out, sqlite3_stmt *st, int col, int type) {
  switch (type) {
  case T_ARR: {
    const char *s = (const char *)sqlite3_column_text(st, col);
    sb_append(out, "[");
    int first = 1;
    while (s && *s) {
      const char *e = strstr(s, "; ");
      size_t n = e ? (size_t)(e - s) : strlen(s);
      char *item = malloc(n + 1);
      if (item) {
        memcpy(item, s, n);
        item[n] = 0;
        if (n) {
          if (!first) sb_append(out, ",");
          sb_json_str(out, item);
          first = 0;
        }
        free(item);
      }
      s = e ? e + 2 : NULL;
    }
    sb_append(out, "]");
    break;
  }
  case T_BOOL: {
    int v;
    if (sqlite3_column_type(st, col) == SQLITE_TEXT) {
      const char *s = (const char *)sqlite3_column_text(st, col);
      v = s && (!strcasecmp(s, "true") || !strcmp(s, "1"));
    } else {
      v = sqlite3_column_int(st, col) != 0;
    }
    sb_append(out, v ? "true" : "false");
    break;
  }
  case T_NUM:
    sb_printf(out, "%lld", (long long)sqlite3_column_int64(st, col));
    break;
  default:
    sb_json_str(out, (const char *)sqlite3_column_text(st, col));
  }
}

/* Fields the browse indexes carry, so searching them needs no table reads. */
static int covered_field(const char *name, int *text) {
  static const char *const k_text[] = {"title", "alternateTitles", "developer", "publisher", "series", NULL};
  for (int i = 0; k_text[i]; i++) {
    if (!strcmp(name, k_text[i])) {
      *text = 1;
      return 1;
    }
  }
  return !strcmp(name, "library") || !strcmp(name, "platform");
}

static int is_library_name(const char *s) { return !strcasecmp(s, "arcade") || !strcasecmp(s, "theatre"); }

/* ORDER BY that walks index ix in its own order (after the library column),
 * so SQLite needs no sort. */
static void order_by_index(sb_t *order, int ix, const char *dir) {
  ixcols_t c;
  ix_columns(g_db, ix, &c);
  for (int i = 1; i < c.n; i++) sb_printf(order, "g.%s%s, ", c.cols[i], dir);
  sb_printf(order, "g.rowid%s", dir);
}

/* Fisher-Yates with a seeded xorshift: "random" stays the same order from
 * one page to the next. */
static void shuffle(long long *ids, long n, unsigned long long seed) {
  unsigned long long x = seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
  if (!x) x = 1;
  for (long i = n - 1; i > 0; i--) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    long j = (long)((x * 0x2545F4914F6CDD1Dull) % (unsigned long long)(i + 1));
    long long t = ids[i];
    ids[i] = ids[j];
    ids[j] = t;
  }
}

/* The ordered rowids matching a search, walking index ix (0: SQLite picks).
 * 0 when complete; -1 when the query failed or stopped part way. */
static int build_list(int ix, const char *where, int groups, const char *filt, int needs_gd, const char *key,
                      int by_title, int random, const char *D, const binds_t *b, long long **ids_out,
                      long *n_out) {
  sb_t order;
  sb_init(&order);
  if (random) sb_append(&order, "g.rowid"); /* shuffled afterwards */
  else if (ix == IX_TITLE && by_title) order_by_index(&order, IX_TITLE, D);
  else if (ix == IX_ADDED) order_by_index(&order, IX_ADDED, D);
  else {
    if (!strcmp(key, "platform")) sb_printf(&order, "%s COLLATE NOCASE%s", g_expr[field_index("platform")], D);
    else sb_printf(&order, "%s%s", key, D);
    if (!by_title) sb_append(&order, ", g.title COLLATE NOCASE");
    sb_printf(&order, ", g.rowid%s", D);
  }
  sb_t sql;
  sb_init(&sql);
  sb_append(&sql, "SELECT g.rowid FROM game g");
  if (ix) sb_printf(&sql, " INDEXED BY %s", ix_name(ix));
  if (needs_gd) sb_append(&sql, " LEFT JOIN game_data gd ON gd.id = g.activeDataId");
  sb_printf(&sql, " WHERE (%s%s)%s ORDER BY %s", groups ? "" : "1", where, filt, order.buf);
  sb_free(&order);

  long n = 0, cap = 1024;
  long long *ids = malloc((size_t)cap * sizeof *ids);
  int rc = SQLITE_NOMEM;
  sqlite3_stmt *st = NULL;
  if (ids && sqlite3_prepare_v2(g_db, sql.buf, -1, &st, NULL) == SQLITE_OK) {
    for (int i = 0; i < b->n; i++) sqlite3_bind_text(st, i + 1, b->v[i], -1, SQLITE_TRANSIENT);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      if (n == cap) {
        long long *more = realloc(ids, (size_t)(cap *= 2) * sizeof *ids);
        if (!more) {
          rc = SQLITE_NOMEM;
          break;
        }
        ids = more;
      }
      ids[n++] = sqlite3_column_int64(st, 0);
    }
  }
  if (rc != SQLITE_DONE) LOG("library: search failed: %s", sqlite3_errmsg(g_db));
  sqlite3_finalize(st);
  sb_free(&sql);
  *ids_out = ids;
  *n_out = ids ? n : 0;
  return rc == SQLITE_DONE ? 0 : -1;
}

char *lib_search_json(const http_req_t *req, long long *total_out) {
  if (total_out) *total_out = 0;
  if (!g_db) return str_dup("[]");

  char any_buf[8];
  int any = http_param(req, "any", any_buf, sizeof any_buf) && !strcasecmp(any_buf, "true");
  const char *op = any ? " OR " : " AND ";

  sb_t where;
  sb_init(&where);
  binds_t b = {.n = 0};
  int groups = 0;
  int covered = 1; /* every condition is on a column the browse indexes carry */
  int text = 0;    /* ...and some are typed text (title, developer, ...) */
  int needs_gd = 0;
  char *val = malloc(4096);

  /* smartSearch: title, alternate titles, developer, publisher, series */
  if (val && http_param(req, "smartSearch", val, 4096) && val[0]) {
    char *save = NULL;
    for (char *tok = strtok_r(val, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      if (!*tok) continue;
      if (groups++) sb_append(&where, op);
      sb_append(&where, "(");
      static const char *cols[] = {"g.title", "g.alternateTitles", "g.developer", "g.publisher", "g.series"};
      for (int i = 0; i < 5; i++) {
        if (i) sb_append(&where, " OR ");
        bind_like(&b, &where, cols[i], tok);
      }
      sb_append(&where, ")");
      text = 1;
    }
  }

  for (int f = 0; val && k_fields[f].name; f++) {
    if (!k_fields[f].searchable) continue;
    if (!http_param(req, k_fields[f].name, val, 4096) || !val[0]) continue;
    const char *name = k_fields[f].name;
    if (!covered_field(name, &text)) covered = 0;
    needs_gd |= g_needs_gd[f];
    if (groups++) sb_append(&where, op);
    sb_append(&where, "(");
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(val, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      if (n++) sb_append(&where, op);
      if (!strcmp(name, "library") && is_library_name(tok)) {
        /* the whole value, so the index finds it instead of reading every entry */
        char lib[16];
        str_lcpy(lib, tok, sizeof lib);
        for (char *c = lib; *c; c++) *c = (char)(*c | 0x20);
        bind_value(&b, &where, "g.library", "=", lib);
      } else {
        bind_like(&b, &where, g_expr[f], tok);
      }
    }
    if (!n) sb_append(&where, "1");
    sb_append(&where, ")");
  }

  /* Filters that always AND, whatever "any" says. */
  sb_t filt;
  sb_init(&filt);
  char fb[8];
  int filter = g_cfg.filter_extreme_default;
  if (http_param(req, "filter", fb, sizeof fb)) filter = !strcasecmp(fb, "true") || !strcmp(fb, "1");
  if (filter && g_has.extreme) sb_append(&filt, " AND g.extreme = 0");
  int installed = http_param_bool(req, "installed", 0) && g_has.on_disk;
  int played = http_param_bool(req, "played", 0) && g_has.last_played;
  if (installed) sb_append(&filt, " AND g.activeDataOnDisk = 1");
  if (played) sb_append(&filt, " AND g.lastPlayed IS NOT NULL AND g.lastPlayed != ''");
  if (http_param_bool(req, "ps5", 0)) {
    const char *p = g_expr[field_index("platform")];
    sb_printf(&filt, " AND (%s LIKE '%%Flash%%' OR %s LIKE '%%HTML5%%')", p, p);
  }

  /* Output fields */
  const field_t *out_fields[64];
  int n_out = 0, out_gd = 0;
  char *fields = malloc(2048);
  if (fields && (!http_param(req, "fields", fields, 2048) || !fields[0]))
    str_lcpy(fields, k_default_fields, 2048);
  char *save = NULL;
  for (char *tok = fields ? strtok_r(fields, ",", &save) : NULL; tok && n_out < 64;
       tok = strtok_r(NULL, ",", &save)) {
    const field_t *f = find_field(tok);
    if (f) {
      out_fields[n_out++] = f;
      out_gd |= g_needs_gd[f - k_fields];
    }
  }
  free(fields);
  if (!n_out) out_fields[n_out++] = find_field("id");

  /* Sorting and paging */
  char sort[32] = "title", dir[8] = "asc";
  http_param(req, "sort", sort, sizeof sort);
  http_param(req, "dir", dir, sizeof dir);
  const char *D = !strcasecmp(dir, "desc") ? " DESC" : "";
  int random = !strcmp(sort, "random");
  const char *key = "g.title COLLATE NOCASE";
  if (!strcmp(sort, "dateAdded")) key = "g.dateAdded";
  else if (!strcmp(sort, "dateModified")) key = "g.dateModified";
  else if (!strcmp(sort, "releaseDate")) key = "g.releaseDate";
  else if (!strcmp(sort, "creator") || !strcmp(sort, "developer")) key = "g.developer COLLATE NOCASE";
  else if (!strcmp(sort, "publisher")) key = "g.publisher COLLATE NOCASE";
  else if (!strcmp(sort, "platform")) key = "platform";
  else if (!strcmp(sort, "lastPlayed")) key = g_expr[field_index("lastPlayed")];
  else if (!strcmp(sort, "playtime")) key = g_expr[field_index("playtime")];
  int by_title = !strcmp(key, "g.title COLLATE NOCASE") && !random;
  long limit = http_param_long(req, "limit", 100);
  if (limit <= 0 || limit > 5000) limit = 5000;
  long offset = http_param_long(req, "offset", 0);
  if (offset < 0) offset = 0;
  unsigned long long seed = (unsigned long long)http_param_long(req, "seed", (long)(now_seconds() / 3600));

  pthread_mutex_lock(&g_search_lock);
  forget_if_changed();

  /* The index that answers this without reading the game table. */
  int ix = 0;
  if (covered && !any && !needs_gd) {
    if (played) ix = IX_PLAYED;
    else if (installed) ix = IX_INSTALLED;
    else if (text) ix = IX_TEXT;
    else if (by_title || random) ix = IX_TITLE;
    else if (!strcmp(key, "g.dateAdded")) ix = IX_ADDED;
    else if (!strcmp(key, "g.developer COLLATE NOCASE") || !strcmp(key, "g.publisher COLLATE NOCASE") ||
             !strcmp(key, "g.releaseDate") || !strcmp(key, "platform"))
      ix = IX_TEXT; /* carries these too, and is still far smaller than the table */
    if (!(g_ix & ix)) ix = 0;
  }

  /* cache key: the filters, the order, the values, and the shuffle */
  sb_t k;
  sb_init(&k);
  sb_printf(&k, "%d|%s|%s|%s|%d%s", any, where.buf, filt.buf, sort, needs_gd, D);
  for (int i = 0; i < b.n; i++) sb_printf(&k, "\x1f%s", b.v[i]);
  if (random) sb_printf(&k, "\x1fseed=%llu", seed);

  rowlist_t *list = list_find(k.buf);
  long long *ids = NULL;
  long n = 0;
  if (!list) {
    double t0 = now_seconds();
    int rc = build_list(ix, where.buf, groups, filt.buf, needs_gd, key, by_title, random, D, &b, &ids, &n);
    if (rc && ix) {
      LOG("library: %s didn't answer that search; trying without it", ix_name(ix));
      free(ids);
      ids = NULL;
      n = 0;
      rc = build_list(0, where.buf, groups, filt.buf, needs_gd, key, by_title, random, D, &b, &ids, &n);
    }
    if (random) shuffle(ids, n, seed);
    double dt = now_seconds() - t0;
    if (dt > 1.0) LOG("library: slow search (%.1f s, %ld entries, %s)", dt, n, ix ? ix_name(ix) : "no index");
    /* a list cut short (the library busy, out of memory) isn't kept */
    if (!rc) {
      list = list_store(sb_steal(&k), ids, n);
      ids = NULL;
    }
  }
  rowlist_t partial = {.ids = ids, .n = n};
  if (!list) list = &partial;
  sb_free(&k);
  if (total_out) *total_out = list->n;

  /* the page's rows */
  sb_t out, sql;
  sb_init(&out);
  sb_init(&sql);
  sb_append(&out, "[");
  sb_append(&sql, "SELECT ");
  for (int i = 0; i < n_out; i++) sb_printf(&sql, "%s%s", i ? "," : "", g_expr[out_fields[i] - k_fields]);
  sb_append(&sql, " FROM game g");
  if (out_gd) sb_append(&sql, " LEFT JOIN game_data gd ON gd.id = g.activeDataId");
  sb_append(&sql, " WHERE g.rowid = ?");
  sqlite3_stmt *st = NULL;
  if (offset < list->n && sqlite3_prepare_v2(g_db, sql.buf, -1, &st, NULL) == SQLITE_OK) {
    int row = 0;
    for (long i = offset; i < list->n && i < offset + limit; i++) {
      sqlite3_reset(st);
      sqlite3_bind_int64(st, 1, list->ids[i]);
      if (sqlite3_step(st) != SQLITE_ROW) continue; /* removed meanwhile */
      sb_append(&out, row++ ? ",{" : "{");
      for (int c = 0; c < n_out; c++) {
        if (c) sb_append(&out, ",");
        sb_json_str(&out, out_fields[c]->name);
        sb_append(&out, ":");
        append_row_value(&out, st, c, out_fields[c]->type);
      }
      sb_append(&out, "}");
    }
  }
  sqlite3_finalize(st);
  pthread_mutex_unlock(&g_search_lock);
  free(partial.ids);
  sb_append(&out, "]");

  for (int i = 0; i < b.n; i++) free(b.v[i]);
  free(val);
  sb_free(&where);
  sb_free(&filt);
  sb_free(&sql);
  return sb_steal(&out);
}

/* ------------------------------------------------------------ entries */

static void json_kv_str(sb_t *sb, const char *k, const char *v, int comma) {
  if (comma) sb_append(sb, ",");
  sb_json_str(sb, k);
  sb_append(sb, ":");
  sb_json_str(sb, v);
}

char *lib_addapps_json(const char *id) {
  sb_t out;
  sb_init(&out);
  sb_append(&out, "[");
  sqlite3_stmt *st;
  if (g_db && sqlite3_prepare_v2(g_db,
                                 "SELECT id, name, applicationPath, launchCommand, autoRunBefore "
                                 "FROM additional_app WHERE parentGameId = ? ORDER BY name",
                                 -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
      sb_append(&out, n++ ? ",{" : "{");
      json_kv_str(&out, "id", (const char *)sqlite3_column_text(st, 0), 0);
      json_kv_str(&out, "name", (const char *)sqlite3_column_text(st, 1), 1);
      json_kv_str(&out, "applicationPath", (const char *)sqlite3_column_text(st, 2), 1);
      json_kv_str(&out, "launchCommand", (const char *)sqlite3_column_text(st, 3), 1);
      sb_printf(&out, ",\"runBefore\":%s}", sqlite3_column_int(st, 4) ? "true" : "false");
    }
    sqlite3_finalize(st);
  }
  sb_append(&out, "]");
  return sb_steal(&out);
}

char *lib_game_json(const char *id) {
  if (!g_db) return NULL;
  sb_t sql;
  sb_init(&sql);
  sb_printf(&sql, "%s WHERE g.id = ?", g_base_sql);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, sql.buf, -1, &st, NULL) != SQLITE_OK) {
    sb_free(&sql);
    return NULL;
  }
  sb_free(&sql);
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    return NULL;
  }
  sb_t out;
  sb_init(&out);
  sb_append(&out, "{");
  int ncol = sqlite3_column_count(st);
  for (int c = 0; c < ncol; c++) {
    const char *name = sqlite3_column_name(st, c);
    const field_t *f = find_field(name);
    if (c) sb_append(&out, ",");
    sb_json_str(&out, name);
    sb_append(&out, ":");
    append_row_value(&out, st, c, f ? f->type : T_STR);
  }
  sqlite3_finalize(st);

  /* images, same relative layout as Data/Images */
  char rel[160];
  snprintf(rel, sizeof rel, "%.2s/%.2s/%s.png", id, strlen(id) > 2 ? id + 2 : "", id);
  sb_printf(&out, ",\"logoPath\":\"Logos/%s\",\"screenshotPath\":\"Screenshots/%s\"", rel, rel);

  /* game data (GameZIPs) */
  sb_append(&out, ",\"gameData\":[");
  if (sqlite3_prepare_v2(g_db,
                         "SELECT gd.id, gd.title, gd.dateAdded, gd.sha256, gd.size, gd.presentOnDisk,"
                         " gd.parameters, gd.applicationPath, gd.launchCommand,"
                         " (gd.id = g.activeDataId) FROM game_data gd JOIN game g ON g.id = gd.gameId"
                         " WHERE gd.gameId = ? ORDER BY gd.dateAdded DESC",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
      char fname[128] = "";
      lib_game_data_filename(id, (const char *)sqlite3_column_text(st, 2), fname, sizeof fname);
      sb_printf(&out, "%s{\"id\":%lld", n++ ? "," : "", (long long)sqlite3_column_int64(st, 0));
      json_kv_str(&out, "title", (const char *)sqlite3_column_text(st, 1), 1);
      json_kv_str(&out, "dateAdded", (const char *)sqlite3_column_text(st, 2), 1);
      json_kv_str(&out, "sha256", (const char *)sqlite3_column_text(st, 3), 1);
      sb_printf(&out, ",\"size\":%lld,\"presentOnDisk\":%s", (long long)sqlite3_column_int64(st, 4),
                sqlite3_column_int(st, 5) ? "true" : "false");
      json_kv_str(&out, "parameters", (const char *)sqlite3_column_text(st, 6), 1);
      json_kv_str(&out, "applicationPath", (const char *)sqlite3_column_text(st, 7), 1);
      json_kv_str(&out, "launchCommand", (const char *)sqlite3_column_text(st, 8), 1);
      json_kv_str(&out, "filename", fname, 1);
      sb_printf(&out, ",\"active\":%s}", sqlite3_column_int(st, 9) ? "true" : "false");
    }
    sqlite3_finalize(st);
  }
  sb_append(&out, "],\"addApps\":");
  char *aa = lib_addapps_json(id);
  sb_append(&out, aa ? aa : "[]");
  free(aa);
  sb_append(&out, "}");
  return sb_steal(&out);
}

char *lib_platforms_json(void) {
  sb_t out;
  sb_init(&out);
  sb_append(&out, "[");
  sqlite3_stmt *st;
  const char *sql = has_table("platform_alias")
                      ? "SELECT name FROM platform_alias ORDER BY name COLLATE NOCASE"
                      : "SELECT DISTINCT platformName FROM game WHERE platformName IS NOT NULL ORDER BY 1";
  if (g_db && sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
      if (n++) sb_append(&out, ",");
      sb_json_str(&out, (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
  }
  sb_append(&out, "]");
  return sb_steal(&out);
}

static void stats_query(sb_t *out, const char *key, const char *sql) {
  sb_printf(out, "\"%s\":[", key);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
      sb_append(out, n++ ? ",{\"name\":" : "{\"name\":");
      sb_json_str(out, (const char *)sqlite3_column_text(st, 0));
      sb_printf(out, ",\"count\":%lld}", (long long)sqlite3_column_int64(st, 1));
    }
    sqlite3_finalize(st);
  }
  sb_append(out, "]");
}

/* Totals by (library, platform, legacy or GameZIP), from one pass over the
 * narrowest index; every total in /api/stats is a sum of these. */
typedef struct {
  char lib[32], plat[96];
  int legacy;
  long long n;
} tally_t;

typedef struct {
  tally_t *v;
  int n, cap;
} tallies_t;

static void tally_add(tallies_t *t, const char *lib, const char *plat, int legacy, long long n) {
  for (int i = 0; i < t->n; i++) {
    if (!strcmp(t->v[i].lib, lib) && !strcmp(t->v[i].plat, plat) && t->v[i].legacy == legacy) {
      t->v[i].n += n;
      return;
    }
  }
  if (t->n == t->cap) {
    int cap = t->cap ? t->cap * 2 : 64;
    tally_t *v = realloc(t->v, (size_t)cap * sizeof *v);
    if (!v) return;
    t->v = v;
    t->cap = cap;
  }
  tally_t *e = &t->v[t->n++];
  str_lcpy(e->lib, lib, sizeof e->lib);
  str_lcpy(e->plat, plat, sizeof e->plat);
  e->legacy = legacy;
  e->n = n;
}

static int by_count_desc(const void *a, const void *b) {
  long long x = ((const tally_t *)a)->n, y = ((const tally_t *)b)->n;
  return x < y ? 1 : x > y ? -1 : 0;
}

static int by_lib(const void *a, const void *b) { return strcmp(((const tally_t *)a)->lib, ((const tally_t *)b)->lib); }

/* name_is_lib: the entries are libraries (libraryTotals); with_lib: each
 * also names its library (libraryPlatformTotals). */
static void tallies_json(sb_t *out, const char *key, tallies_t *t, int name_is_lib, int with_lib) {
  sb_printf(out, "\"%s\":[", key);
  for (int i = 0; i < t->n; i++) {
    sb_append(out, i ? ",{" : "{");
    if (with_lib) {
      sb_append(out, "\"library\":");
      sb_json_str(out, t->v[i].lib);
      sb_append(out, ",");
    }
    sb_append(out, "\"name\":");
    sb_json_str(out, name_is_lib ? t->v[i].lib : t->v[i].plat);
    sb_printf(out, ",\"count\":%lld}", t->v[i].n);
  }
  sb_append(out, "]");
}

static long long scalar(const char *sql, long long *second) {
  sqlite3_stmt *st;
  long long v = 0;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) {
      v = sqlite3_column_int64(st, 0);
      if (second) *second = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
  }
  return v;
}

static char *build_stats(void) {
  double t0 = now_seconds();
  tallies_t all = {0}, libs = {0}, formats = {0}, plats = {0}, lib_plats = {0};
  char sql[320];
  snprintf(sql, sizeof sql,
           "SELECT library, COALESCE(platformName,''), %s, COUNT(*) FROM game%s GROUP BY 1, 2, 3",
           g_has.active_data ? "activeDataId IS NULL" : "1", (g_ix & IX_TITLE) ? " INDEXED BY fpps5_title" : "");
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *lib = (const char *)sqlite3_column_text(st, 0);
      const char *plat = (const char *)sqlite3_column_text(st, 1);
      tally_add(&all, lib ? lib : "", plat ? plat : "", sqlite3_column_int(st, 2),
                sqlite3_column_int64(st, 3));
    }
    sqlite3_finalize(st);
  } else {
    LOG("library: stats failed: %s", sqlite3_errmsg(g_db));
  }
  for (int i = 0; i < all.n; i++) {
    const tally_t *e = &all.v[i];
    tally_add(&libs, e->lib, "", 0, e->n);
    tally_add(&formats, "", e->legacy ? "legacy" : "gameZip", 0, e->n);
    if (e->plat[0]) {
      tally_add(&plats, "", e->plat, 0, e->n);
      tally_add(&lib_plats, e->lib, e->plat, 0, e->n);
    }
  }
  qsort(libs.v, (size_t)libs.n, sizeof *libs.v, by_lib);
  qsort(formats.v, (size_t)formats.n, sizeof *formats.v, by_count_desc);
  qsort(plats.v, (size_t)plats.n, sizeof *plats.v, by_count_desc);
  qsort(lib_plats.v, (size_t)lib_plats.n, sizeof *lib_plats.v, by_count_desc);

  sb_t out;
  sb_init(&out);
  sb_append(&out, "{");
  tallies_json(&out, "libraryTotals", &libs, 1, 0);
  sb_append(&out, ",");
  tallies_json(&out, "formatTotals", &formats, 0, 0);
  sb_append(&out, ",");
  if (has_table("platform") && has_table("game_platforms_platform") && has_table("platform_alias")) {
    /* a PC library: count every platform an entry has, as the launcher does */
    stats_query(&out, "platformTotals",
                "SELECT pa.name, COUNT(*) FROM game_platforms_platform gpp"
                " JOIN platform p ON p.id = gpp.platformId"
                " JOIN platform_alias pa ON pa.id = p.primaryAliasId"
                " GROUP BY gpp.platformId ORDER BY 2 DESC");
  } else {
    tallies_json(&out, "platformTotals", &plats, 0, 0);
  }
  /* per library, for the sidebars (each counts only its own entries) */
  sb_append(&out, ",");
  tallies_json(&out, "libraryPlatformTotals", &lib_plats, 0, 1);
  sb_printf(&out, ",\"installedTotal\":%lld",
            g_has.on_disk ? scalar((g_ix & IX_INSTALLED)
                                     ? "SELECT COUNT(*) FROM game INDEXED BY fpps5_installed WHERE activeDataOnDisk = 1"
                                     : "SELECT COUNT(*) FROM game WHERE activeDataOnDisk = 1",
                                   NULL)
                          : 0);
  if (g_has.playtime && g_has.last_played) {
    long long playtime = 0;
    long long played = scalar((g_ix & IX_PLAYED)
                                 ? "SELECT COUNT(*), COALESCE(SUM(playtime),0) FROM game INDEXED BY fpps5_played"
                                   " WHERE lastPlayed IS NOT NULL AND lastPlayed != ''"
                                 : "SELECT COUNT(*), COALESCE(SUM(playtime),0) FROM game"
                                   " WHERE lastPlayed IS NOT NULL AND lastPlayed != ''",
                               &playtime);
    sb_printf(&out, ",\"playedTotal\":%lld,\"playtimeTotal\":%lld", played, playtime);
  }
  sb_append(&out, "}");
  free(all.v);
  free(libs.v);
  free(formats.v);
  free(plats.v);
  free(lib_plats.v);
  double dt = now_seconds() - t0;
  if (dt > 1.0) LOG("library: stats took %.1f s", dt);
  return sb_steal(&out);
}

char *lib_stats_json(void) {
  if (!g_db) return str_dup("{\"libraryTotals\":[],\"formatTotals\":[],\"platformTotals\":[]}");
  pthread_mutex_lock(&g_search_lock);
  forget_if_changed();
  if (!g_stats) g_stats = build_stats();
  char *copy = str_dup(g_stats);
  pthread_mutex_unlock(&g_search_lock);
  return copy;
}

/* ------------------------------------------------------------ launching */

static void col_copy(sqlite3_stmt *st, int c, char *dst, size_t size) {
  str_lcpy(dst, (const char *)sqlite3_column_text(st, c), size);
}

int lib_launch_info(const char *game_id, const char *add_app_id, launch_info_t *out) {
  memset(out, 0, sizeof *out);
  if (!g_db) return -1;
  const char *ruffle = g_has.ruffle ? "COALESCE(g.ruffleSupport,'')" : "''";
  const char *logo = g_has.logo ? "COALESCE(g.logoPath,'')" : "''";
  char sql[1400];
  snprintf(sql, sizeof sql,
           "SELECT g.id, g.title, COALESCE(g.platformName,''), g.applicationPath, g.launchCommand,"
           " %s, %s, g.activeDataId IS NOT NULL, gd.id, gd.dateAdded, gd.sha256, gd.size,"
           " gd.presentOnDisk, COALESCE(gd.path,''), COALESCE(gd.parameters,''),"
           " COALESCE(gd.applicationPath,''), COALESCE(gd.launchCommand,'')"
           " FROM game g LEFT JOIN game_data gd ON gd.id = g.activeDataId WHERE g.id = ?",
           ruffle, logo);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
    LOG("library: launch info query failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_text(st, 1, game_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    return -1;
  }
  col_copy(st, 0, out->game_id, sizeof out->game_id);
  col_copy(st, 1, out->title, sizeof out->title);
  col_copy(st, 2, out->platform, sizeof out->platform);
  col_copy(st, 3, out->application_path, sizeof out->application_path);
  col_copy(st, 4, out->launch_command, sizeof out->launch_command);
  col_copy(st, 5, out->ruffle_support, sizeof out->ruffle_support);
  col_copy(st, 6, out->logo_path, sizeof out->logo_path);
  out->has_data = sqlite3_column_int(st, 7);
  if (out->has_data) {
    out->data.id = sqlite3_column_int64(st, 8);
    col_copy(st, 9, out->data.date_added, sizeof out->data.date_added);
    col_copy(st, 10, out->data.sha256, sizeof out->data.sha256);
    out->data.size = sqlite3_column_int64(st, 11);
    out->data.present = sqlite3_column_int(st, 12);
    col_copy(st, 13, out->data.path, sizeof out->data.path);
    col_copy(st, 14, out->data.parameters, sizeof out->data.parameters);
    const char *gap = (const char *)sqlite3_column_text(st, 15);
    const char *glc = (const char *)sqlite3_column_text(st, 16);
    if (gap && *gap) str_lcpy(out->application_path, gap, sizeof out->application_path);
    if (glc && *glc) str_lcpy(out->launch_command, glc, sizeof out->launch_command);
    lib_game_data_filename(out->game_id, out->data.date_added, out->data.filename,
                           sizeof out->data.filename);
  }
  sqlite3_finalize(st);
  if (!out->logo_path[0]) {
    snprintf(out->logo_path, sizeof out->logo_path, "Logos/%.2s/%.2s/%s.png", out->game_id,
             out->game_id + 2, out->game_id);
  }

  if (add_app_id && *add_app_id) {
    if (sqlite3_prepare_v2(g_db,
                           "SELECT name, applicationPath, launchCommand FROM additional_app"
                           " WHERE id = ? AND parentGameId = ?",
                           -1, &st, NULL) != SQLITE_OK)
      return -1;
    sqlite3_bind_text(st, 1, add_app_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, game_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
      sqlite3_finalize(st);
      return -1;
    }
    char name[256];
    col_copy(st, 0, name, sizeof name);
    size_t tl = strlen(out->title);
    snprintf(out->title + tl, sizeof out->title - tl, " (%s)", name);
    col_copy(st, 1, out->application_path, sizeof out->application_path);
    col_copy(st, 2, out->launch_command, sizeof out->launch_command);
    sqlite3_finalize(st);
  }
  return 0;
}

static void exec_bind(const char *sql, const char *a, const char *b, long long n) {
  sqlite3_stmt *st;
  if (!g_db || sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return;
  int i = 1;
  if (a) sqlite3_bind_text(st, i++, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, i++, b, -1, SQLITE_TRANSIENT);
  if (n >= 0) sqlite3_bind_int64(st, i++, n);
  if (sqlite3_step(st) != SQLITE_DONE) LOG("library: update failed: %s", sqlite3_errmsg(g_db));
  sqlite3_finalize(st);
  __sync_fetch_and_add(&g_local_writes, 1); /* cached lists and stats are out of date */
}

void lib_mark_data_present(long long game_data_id, const char *game_id, const char *path) {
  exec_bind("UPDATE game_data SET presentOnDisk = 1, path = ? WHERE id = ?", path, NULL, game_data_id);
  if (g_has.on_disk) exec_bind("UPDATE game SET activeDataOnDisk = 1 WHERE id = ?", game_id, NULL, -1);
}

void lib_record_play(const char *game_id) {
  if (!g_has.last_played) return;
  exec_bind("UPDATE game SET lastPlayed = strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
            " playCounter = COALESCE(playCounter,0) + 1 WHERE id = ?",
            game_id, NULL, -1);
}

/* days_from_civil (H. Hinnant), for a UTC timestamp without timegm(). */
static long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

int lib_game_data_filename(const char *game_id, const char *date_added, char *out, size_t size) {
  if (!date_added || !*date_added) return -1;
  int Y = 0, M = 0, D = 0, h = 0, mi = 0, s = 0;
  if (sscanf(date_added, "%d-%d-%d%*1[ T]%d:%d:%d", &Y, &M, &D, &h, &mi, &s) < 3) return -1;
  long long ms = 0;
  const char *p = strchr(date_added, ':');
  p = p ? strchr(p + 1, ':') : NULL;
  if (p) {
    p++;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
      p++;
      int digits = 0;
      while (*p >= '0' && *p <= '9') {
        if (digits < 3) ms = ms * 10 + (*p - '0');
        digits++;
        p++;
      }
      for (; digits < 3; digits++) ms *= 10;
    }
  }
  long long offset_min = 0;
  if (p && (*p == '+' || *p == '-')) {
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%d:%d", &oh, &om) >= 1) offset_min = (oh * 60 + om) * (*p == '-' ? -1 : 1);
  }
  long long days = days_from_civil(Y, (unsigned)M, (unsigned)D);
  long long secs = days * 86400 + h * 3600 + mi * 60 + s - offset_min * 60;
  snprintf(out, size, "%s-%lld.zip", game_id, secs * 1000 + ms);
  return 0;
}
