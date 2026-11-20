/* Flashpoint PS5 - the local game library (flashpoint.sqlite).
 * The search endpoint mirrors FlashpointProject/flashpoint-database-api:
 * every metadata field can be matched (comma separated values, LIKE
 * %value%), with "any" switching AND to OR and "filter" hiding extreme
 * entries. Paging/sorting are done here instead of in the browser, since
 * the PS5's browser has no JIT.
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
 * the base query built in lib_open (column aliases match these names). */
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

static const char *k_default_fields =
  "id,title,developer,publisher,platform,library,tags,originalDescription,dateAdded,"
  "dateModified,zipped,installed,lastPlayed,playtime,extreme";

static sqlite3 *g_db;
static char *g_base_sql;
static long long g_game_count = -1;

/* ------------------------------------------------------------ open */

static int has_column(const char *table, const char *col) {
  char sql[128];
  snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
  sqlite3_stmt *st;
  int found = 0;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(st, 1);
    if (name && !strcmp(name, col)) found = 1;
  }
  sqlite3_finalize(st);
  return found;
}

static int has_table(const char *table) {
  sqlite3_stmt *st;
  int found = 0;
  if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st,
                         NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

static void build_base_sql(void) {
  const char *ruffle = has_column("game", "ruffleSupport") ? "COALESCE(g.ruffleSupport,'')" : "''";
  const char *platform = has_column("game", "platformsStr")
                           ? "COALESCE(NULLIF(g.platformsStr,''), g.platformName, '')"
                           : "COALESCE(g.platformName,'')";
  const char *playtime = has_column("game", "playtime") ? "COALESCE(g.playtime,0)" : "0";
  const char *counter = has_column("game", "playCounter") ? "COALESCE(g.playCounter,0)" : "0";
  const char *last = has_column("game", "lastPlayed") ? "g.lastPlayed" : "NULL";
  const char *installed = has_column("game", "activeDataOnDisk") ? "g.activeDataOnDisk" : "0";
  sb_t sb;
  sb_init(&sb);
  sb_printf(&sb,
            "SELECT g.id AS id, g.title AS title, g.alternateTitles AS alternateTitles,"
            " g.series AS series, g.developer AS developer, g.publisher AS publisher,"
            " g.source AS source, g.library AS library, g.tagsStr AS tags, %s AS platform,"
            " g.playMode AS playMode, g.status AS status, g.version AS version,"
            " g.releaseDate AS releaseDate, g.language AS language, g.notes AS notes,"
            " g.originalDescription AS originalDescription, g.dateAdded AS dateAdded,"
            " g.dateModified AS dateModified,"
            " CASE WHEN gd.applicationPath IS NOT NULL AND gd.applicationPath != ''"
            "   THEN gd.applicationPath ELSE g.applicationPath END AS applicationPath,"
            " CASE WHEN gd.launchCommand IS NOT NULL AND gd.launchCommand != ''"
            "   THEN gd.launchCommand ELSE g.launchCommand END AS launchCommand,"
            " CASE WHEN g.activeDataId IS NULL THEN 'false' ELSE 'true' END AS zipped,"
            " %s AS ruffleSupport, g.extreme AS extreme, %s AS installed,"
            " %s AS lastPlayed, %s AS playtime, %s AS playCounter"
            " FROM game g LEFT JOIN game_data gd ON gd.id = g.activeDataId",
            platform, ruffle, installed, last, playtime, counter);
  free(g_base_sql);
  g_base_sql = sb_steal(&sb);
}

int lib_open(const char *db_path) {
  if (!is_file(db_path)) {
    LOG("library: no database at %s", db_path);
    return -1;
  }
  int rc = sqlite3_open_v2(db_path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK) {
    LOG("library: read-write open failed (%s), trying read-only", sqlite3_errstr(rc));
    if (g_db) sqlite3_close(g_db);
    rc = sqlite3_open_v2(db_path, &g_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
  }
  if (rc != SQLITE_OK) {
    LOG("library: can't open %s: %s", db_path, sqlite3_errstr(rc));
    if (g_db) sqlite3_close(g_db);
    g_db = NULL;
    return -1;
  }
  sqlite3_busy_timeout(g_db, 5000);
  /* WAL needs shared memory, which removable drives on the PS5 may not
   * give us; the plain rollback journal works everywhere. */
  sqlite3_exec(g_db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
  sqlite3_exec(g_db, "PRAGMA cache_size=-16384", NULL, NULL, NULL);
  if (!has_table("game")) {
    LOG("library: %s has no game table", db_path);
    sqlite3_close(g_db);
    g_db = NULL;
    return -1;
  }
  build_base_sql();
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM game", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) g_game_count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  LOG("library: opened %s (%lld entries)", db_path, g_game_count);
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

/* ------------------------------------------------------------ search */

static const field_t *find_field(const char *name) {
  for (int i = 0; k_fields[i].name; i++) {
    if (!strcmp(k_fields[i].name, name)) return &k_fields[i];
  }
  return NULL;
}

typedef struct binds {
  char *v[256];
  int n;
} binds_t;

static void bind_like(binds_t *b, sb_t *sql, const char *col, const char *value) {
  if (b->n >= 256) return;
  sb_t v;
  sb_init(&v);
  sb_append(&v, "%");
  for (const char *p = value; *p; p++) {
    if (*p == '^' || *p == '%' || *p == '_') sb_appendn(&v, "^", 1);
    sb_appendn(&v, p, 1);
  }
  sb_append(&v, "%");
  b->v[b->n++] = sb_steal(&v);
  sb_printf(sql, "%s LIKE ? ESCAPE '^'", col);
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
  char *val = malloc(4096);

  /* smartSearch: title, alternate titles, developer, publisher, series */
  if (val && http_param(req, "smartSearch", val, 4096) && val[0]) {
    char *save = NULL;
    for (char *tok = strtok_r(val, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      if (!*tok) continue;
      if (groups++) sb_append(&where, op);
      sb_append(&where, "(");
      static const char *cols[] = {"title", "alternateTitles", "developer", "publisher", "series"};
      for (int i = 0; i < 5; i++) {
        if (i) sb_append(&where, " OR ");
        bind_like(&b, &where, cols[i], tok);
      }
      sb_append(&where, ")");
    }
  }

  for (int f = 0; val && k_fields[f].name; f++) {
    if (!k_fields[f].searchable) continue;
    if (!http_param(req, k_fields[f].name, val, 4096) || !val[0]) continue;
    if (groups++) sb_append(&where, op);
    sb_append(&where, "(");
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(val, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      if (n++) sb_append(&where, op);
      bind_like(&b, &where, k_fields[f].name, tok);
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
  if (filter) sb_append(&filt, " AND extreme = 0");
  if (http_param_bool(req, "installed", 0)) sb_append(&filt, " AND installed = 1");
  if (http_param_bool(req, "played", 0)) sb_append(&filt, " AND lastPlayed IS NOT NULL AND lastPlayed != ''");
  if (http_param_bool(req, "ps5", 0)) {
    sb_append(&filt, " AND (platform LIKE '%Flash%' OR platform LIKE '%HTML5%')");
  }

  /* Output fields */
  const field_t *out_fields[64];
  int n_out = 0;
  char *fields = malloc(2048);
  if (fields && (!http_param(req, "fields", fields, 2048) || !fields[0]))
    str_lcpy(fields, k_default_fields, 2048);
  char *save = NULL;
  for (char *tok = fields ? strtok_r(fields, ",", &save) : NULL; tok && n_out < 64;
       tok = strtok_r(NULL, ",", &save)) {
    const field_t *f = find_field(tok);
    if (f) out_fields[n_out++] = f;
  }
  free(fields);
  if (!n_out) out_fields[n_out++] = find_field("id");

  /* Sorting and paging */
  char sort[32] = "title", dir[8] = "asc";
  http_param(req, "sort", sort, sizeof sort);
  http_param(req, "dir", dir, sizeof dir);
  const char *order = "title COLLATE NOCASE";
  if (!strcmp(sort, "dateAdded")) order = "dateAdded";
  else if (!strcmp(sort, "dateModified")) order = "dateModified";
  else if (!strcmp(sort, "releaseDate")) order = "releaseDate";
  else if (!strcmp(sort, "creator") || !strcmp(sort, "developer")) order = "developer COLLATE NOCASE";
  else if (!strcmp(sort, "publisher")) order = "publisher COLLATE NOCASE";
  else if (!strcmp(sort, "platform")) order = "platform COLLATE NOCASE";
  else if (!strcmp(sort, "lastPlayed")) order = "lastPlayed";
  else if (!strcmp(sort, "playtime")) order = "playtime";
  else if (!strcmp(sort, "random")) order = "RANDOM()";
  int desc = !strcasecmp(dir, "desc");
  long limit = http_param_long(req, "limit", 100);
  if (limit <= 0 || limit > 5000) limit = 5000;
  long offset = http_param_long(req, "offset", 0);
  if (offset < 0) offset = 0;

  sb_t cond;
  sb_init(&cond);
  sb_append(&cond, groups ? "(" : "(1");
  sb_append(&cond, where.buf);
  sb_append(&cond, ")");
  sb_append(&cond, filt.buf);

  /* total */
  sb_t sql;
  sb_init(&sql);
  sb_printf(&sql, "SELECT COUNT(*) FROM (%s) WHERE %s", g_base_sql, cond.buf);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(g_db, sql.buf, -1, &st, NULL) == SQLITE_OK) {
    for (int i = 0; i < b.n; i++) sqlite3_bind_text(st, i + 1, b.v[i], -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW && total_out) *total_out = sqlite3_column_int64(st, 0);
  } else {
    LOG("library: search count failed: %s", sqlite3_errmsg(g_db));
  }
  sqlite3_finalize(st);

  /* rows */
  sql.len = 0;
  sql.buf[0] = 0;
  sb_append(&sql, "SELECT ");
  for (int i = 0; i < n_out; i++) sb_printf(&sql, "%s%s", i ? "," : "", out_fields[i]->name);
  sb_printf(&sql, " FROM (%s) WHERE %s ORDER BY %s %s", g_base_sql, cond.buf, order,
            strcmp(order, "RANDOM()") ? (desc ? "DESC" : "ASC") : "");
  if (strcmp(order, "title COLLATE NOCASE")) sb_append(&sql, ", title COLLATE NOCASE");
  sb_printf(&sql, " LIMIT %ld OFFSET %ld", limit, offset);

  sb_t out;
  sb_init(&out);
  sb_append(&out, "[");
  st = NULL;
  if (sqlite3_prepare_v2(g_db, sql.buf, -1, &st, NULL) == SQLITE_OK) {
    for (int i = 0; i < b.n; i++) sqlite3_bind_text(st, i + 1, b.v[i], -1, SQLITE_TRANSIENT);
    int row = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
      sb_append(&out, row++ ? ",{" : "{");
      for (int i = 0; i < n_out; i++) {
        if (i) sb_append(&out, ",");
        sb_json_str(&out, out_fields[i]->name);
        sb_append(&out, ":");
        append_row_value(&out, st, i, out_fields[i]->type);
      }
      sb_append(&out, "}");
    }
  } else {
    LOG("library: search failed: %s", sqlite3_errmsg(g_db));
  }
  sqlite3_finalize(st);
  sb_append(&out, "]");

  for (int i = 0; i < b.n; i++) free(b.v[i]);
  free(val);
  sb_free(&where);
  sb_free(&filt);
  sb_free(&cond);
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
  sb_printf(&sql, "SELECT * FROM (%s) WHERE id = ?", g_base_sql);
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

static long long scalar(const char *sql) {
  sqlite3_stmt *st;
  long long v = 0;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  return v;
}

char *lib_stats_json(void) {
  if (!g_db) return str_dup("{\"libraryTotals\":[],\"formatTotals\":[],\"platformTotals\":[]}");
  sb_t out;
  sb_init(&out);
  sb_append(&out, "{");
  stats_query(&out, "libraryTotals",
              "SELECT library, COUNT(*) FROM game GROUP BY library ORDER BY library");
  sb_append(&out, ",");
  stats_query(&out, "formatTotals",
              "SELECT CASE WHEN activeDataId IS NULL THEN 'legacy' ELSE 'gameZip' END AS format,"
              " COUNT(*) FROM game GROUP BY format");
  sb_append(&out, ",");
  if (has_table("platform") && has_table("game_platforms_platform")) {
    stats_query(&out, "platformTotals",
                "SELECT pa.name, COUNT(*) FROM game_platforms_platform gpp"
                " JOIN platform p ON p.id = gpp.platformId"
                " JOIN platform_alias pa ON pa.id = p.primaryAliasId"
                " GROUP BY gpp.platformId ORDER BY 2 DESC");
  } else {
    stats_query(&out, "platformTotals",
                "SELECT platformName, COUNT(*) FROM game GROUP BY platformName ORDER BY 2 DESC");
  }
  sb_printf(&out, ",\"installedTotal\":%lld",
            has_column("game", "activeDataOnDisk")
              ? scalar("SELECT COUNT(*) FROM game WHERE activeDataOnDisk = 1")
              : 0);
  if (has_column("game", "playtime")) {
    sb_printf(&out, ",\"playedTotal\":%lld,\"playtimeTotal\":%lld",
              scalar("SELECT COUNT(*) FROM game WHERE lastPlayed IS NOT NULL AND lastPlayed != ''"),
              scalar("SELECT COALESCE(SUM(playtime),0) FROM game"));
  }
  sb_append(&out, "}");
  return sb_steal(&out);
}

/* ------------------------------------------------------------ launching */

static void col_copy(sqlite3_stmt *st, int c, char *dst, size_t size) {
  str_lcpy(dst, (const char *)sqlite3_column_text(st, c), size);
}

int lib_launch_info(const char *game_id, const char *add_app_id, launch_info_t *out) {
  memset(out, 0, sizeof *out);
  if (!g_db) return -1;
  const char *ruffle = has_column("game", "ruffleSupport") ? "COALESCE(g.ruffleSupport,'')" : "''";
  const char *logo = has_column("game", "logoPath") ? "COALESCE(g.logoPath,'')" : "''";
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
}

void lib_mark_data_present(long long game_data_id, const char *game_id, const char *path) {
  exec_bind("UPDATE game_data SET presentOnDisk = 1, path = ? WHERE id = ?", path, NULL, game_data_id);
  if (has_column("game", "activeDataOnDisk"))
    exec_bind("UPDATE game SET activeDataOnDisk = 1 WHERE id = ?", game_id, NULL, -1);
}

void lib_record_play(const char *game_id) {
  if (!has_column("game", "lastPlayed")) return;
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
