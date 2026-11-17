/* Flashpoint PS5 - game data: on-demand GameZIP downloads (like Flashpoint
 * Infinity), unpacking for players, images and Legacy htdocs files.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gamedata.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../third_party/miniz/miniz.h"
#include "config.h"
#include "net.h"

#define MARKER ".fpps5-data"

enum { J_QUEUED, J_DOWNLOADING, J_VERIFYING, J_EXTRACTING, J_DONE, J_ERROR };
static const char *k_state_names[] = {"queued", "downloading", "verifying", "extracting", "done", "error"};

typedef struct job {
  launch_info_t li;
  int state;
  int cancel;
  long long done, total;
  char error[512];
  double updated;
  struct job *next;
} job_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static job_t *g_jobs;

/* ------------------------------------------------------------ paths */

void gd_run_dir(const char *game_id, char *out, size_t size) {
  path_join(out, size, g_cfg.run_path, game_id);
}

int gd_zip_path(const launch_info_t *li, char *out, size_t size) {
  if (!li->has_data) return -1;
  if (li->data.filename[0]) {
    path_join(out, size, g_cfg.games_path, li->data.filename);
    if (is_file(out)) return 0;
  }
  if (li->data.path[0]) {
    if (li->data.path[0] == '/' && is_file(li->data.path)) {
      str_lcpy(out, li->data.path, size);
      return 0;
    }
    /* launcher paths may be Windows ones: keep the file name */
    const char *base = li->data.path;
    for (const char *p = li->data.path; *p; p++) {
      if (*p == '/' || *p == '\\') base = p + 1;
    }
    path_join(out, size, g_cfg.games_path, base);
    if (is_file(out)) return 0;
  }
  return -1;
}

static int run_dir_current(const launch_info_t *li) {
  char dir[FP_PATH_MAX], marker[FP_PATH_MAX];
  gd_run_dir(li->game_id, dir, sizeof dir);
  path_join(marker, sizeof marker, dir, MARKER);
  char *text = read_text_file(marker, NULL);
  int ok = text && !strncasecmp(text, li->data.sha256, strlen(li->data.sha256)) &&
           strlen(text) >= strlen(li->data.sha256);
  free(text);
  return ok;
}

/* ------------------------------------------------------------ jobs */

static void job_set(job_t *j, int state, long long done, long long total) {
  pthread_mutex_lock(&g_lock);
  j->state = state;
  if (done >= 0) j->done = done;
  if (total >= 0) j->total = total;
  j->updated = now_seconds();
  pthread_mutex_unlock(&g_lock);
}

static void job_fail(job_t *j, const char *msg) {
  /* log first: once the state says "error" the job may be freed */
  LOG("gamedata: %s: %s", j->li.title, msg);
  pthread_mutex_lock(&g_lock);
  j->state = J_ERROR;
  str_lcpy(j->error, msg, sizeof j->error);
  j->updated = now_seconds();
  pthread_mutex_unlock(&g_lock);
}

static int dl_progress(void *ud, long long done, long long total) {
  job_t *j = ud;
  pthread_mutex_lock(&g_lock);
  j->done = done;
  if (total > 0) j->total = total;
  j->updated = now_seconds();
  int cancel = j->cancel;
  pthread_mutex_unlock(&g_lock);
  return cancel;
}

typedef struct run_entry {
  char path[FP_PATH_MAX];
  time_t mtime;
} run_entry_t;

static int cmp_run_desc(const void *a, const void *b) {
  const run_entry_t *x = a, *y = b;
  return (y->mtime > x->mtime) - (y->mtime < x->mtime);
}

static int list_run_dirs(run_entry_t *out, int max) {
  DIR *d = opendir(g_cfg.run_path);
  if (!d) return 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < max) {
    if (e->d_name[0] == '.') continue;
    char dir[FP_PATH_MAX], marker[FP_PATH_MAX];
    path_join(dir, sizeof dir, g_cfg.run_path, e->d_name);
    path_join(marker, sizeof marker, dir, MARKER);
    struct stat st;
    if (stat(marker, &st)) continue;
    str_lcpy(out[n].path, dir, sizeof out[n].path);
    out[n].mtime = st.st_mtime;
    n++;
  }
  closedir(d);
  qsort(out, (size_t)n, sizeof *out, cmp_run_desc);
  return n;
}

static void prune_run_dirs(const char *keep) {
  static run_entry_t entries[256];
  int n = list_run_dirs(entries, 256);
  int kept = 0;
  for (int i = 0; i < n; i++) {
    if (!strcmp(entries[i].path, keep) || kept < g_cfg.keep_run_dirs) {
      kept++;
      continue;
    }
    LOG("gamedata: removing old unpacked game %s", entries[i].path);
    remove_tree(entries[i].path);
  }
}

static int extract_content(job_t *j, const char *zip_path, const char *dir) {
  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
    job_fail(j, "the GameZIP can't be opened (damaged download?)");
    return -1;
  }
  remove_tree(dir);
  mkdirs(dir);
  mz_uint n = mz_zip_reader_get_num_files(&zip);
  long long total = 0, done = 0;
  for (mz_uint i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(&zip, i, &st)) total += (long long)st.m_uncomp_size;
  }
  job_set(j, J_EXTRACTING, 0, total);
  int rc = 0;
  for (mz_uint i = 0; i < n && !rc; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    if (strncmp(st.m_filename, "content/", 8)) continue;
    const char *rel = st.m_filename + 8;
    if (!*rel) continue;
    if (!rel_path_is_safe(rel)) {
      LOG("gamedata: skipping unsafe zip entry %s", st.m_filename);
      continue;
    }
    char dest[FP_PATH_MAX];
    path_join(dest, sizeof dest, dir, rel);
    if (mz_zip_reader_is_file_a_directory(&zip, i)) {
      mkdirs(dest);
      continue;
    }
    mkdirs_for_file(dest);
    if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
      char msg[600];
      snprintf(msg, sizeof msg, "can't unpack %s (disk full?)", rel);
      job_fail(j, msg);
      rc = -1;
      break;
    }
    done += (long long)st.m_uncomp_size;
    if (dl_progress(j, done, total)) {
      job_fail(j, "cancelled");
      rc = -1;
    }
  }
  mz_zip_reader_end(&zip);
  if (rc) {
    remove_tree(dir);
    return -1;
  }
  char marker[FP_PATH_MAX];
  path_join(marker, sizeof marker, dir, MARKER);
  char text[200];
  snprintf(text, sizeof text, "%s\n%s\n", j->li.data.sha256, j->li.data.filename);
  write_text_file(marker, text);
  prune_run_dirs(dir);
  return 0;
}

static void run_job(job_t *j) {
  launch_info_t *li = &j->li;
  char zip[FP_PATH_MAX];
  if (gd_zip_path(li, zip, sizeof zip)) {
    path_join(zip, sizeof zip, g_cfg.games_path, li->data.filename);
    char err[512] = "no game data sources are configured";
    int ok = 0;
    for (int s = 0; s < g_cfg.n_game_data_sources && !ok; s++) {
      char url[1024];
      const char *base = g_cfg.game_data_sources[s];
      snprintf(url, sizeof url, "%s%s%s", base, base[0] && base[strlen(base) - 1] == '/' ? "" : "/",
               li->data.filename);
      job_set(j, J_DOWNLOADING, 0, li->data.size);
      LOG("gamedata: downloading %s", url);
      if (!net_download(url, zip, dl_progress, j, err, sizeof err)) ok = 1;
    }
    if (!ok) {
      job_fail(j, err);
      return;
    }
    job_set(j, J_VERIFYING, -1, -1);
    if (li->data.sha256[0]) {
      char hex[65];
      if (sha256_file_hex(zip, hex) || strcasecmp(hex, li->data.sha256)) {
        unlink(zip);
        job_fail(j, "the download didn't match its checksum, try again");
        return;
      }
    }
    lib_mark_data_present(li->data.id, li->game_id, li->data.filename);
  }
  char dir[FP_PATH_MAX];
  gd_run_dir(li->game_id, dir, sizeof dir);
  if (!run_dir_current(li)) {
    LOG("gamedata: unpacking %s into %s", zip, dir);
    if (extract_content(j, zip, dir)) return;
  }
  LOG("gamedata: %s is ready", li->title);
  job_set(j, J_DONE, -1, -1); /* last touch: j may be freed after this */
}

static void *worker_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_lock);
    job_t *next = NULL;
    while (!next) {
      for (job_t *j = g_jobs; j; j = j->next) {
        if (j->state == J_QUEUED) next = j; /* oldest queued is last-found */
      }
      if (!next) pthread_cond_wait(&g_cond, &g_lock);
    }
    next->state = J_DOWNLOADING;
    pthread_mutex_unlock(&g_lock);
    run_job(next);
  }
  return NULL;
}

int gd_init(void) {
  mkdirs(g_cfg.run_path);
  pthread_t th;
  if (pthread_create(&th, NULL, worker_main, NULL)) return -1;
  pthread_detach(th);
  return 0;
}

int gd_prepare(const launch_info_t *li, char *err, size_t errlen) {
  if (!li->has_data) return GD_READY; /* Legacy entry: files live in htdocs */
  char zip[FP_PATH_MAX];
  if (!gd_zip_path(li, zip, sizeof zip) && run_dir_current(li)) {
    gd_touch_run_dir(li->game_id);
    return GD_READY;
  }
  pthread_mutex_lock(&g_lock);
  job_t *found = NULL, **link = &g_jobs;
  for (job_t *j = g_jobs; j; link = &j->next, j = j->next) {
    if (!strcmp(j->li.game_id, li->game_id)) {
      found = j;
      break;
    }
  }
  int rc = GD_PENDING;
  if (found && found->state == J_ERROR) {
    str_lcpy(err, found->error, errlen);
    *link = found->next; /* forget it, so asking again retries */
    free(found);
    rc = GD_ERROR;
  } else if (found && found->state == J_DONE) {
    *link = found->next;
    free(found);
    rc = GD_READY;
  } else if (!found) {
    job_t *j = calloc(1, sizeof *j);
    if (!j) {
      str_lcpy(err, "out of memory", errlen);
      rc = GD_ERROR;
    } else {
      j->li = *li;
      j->state = J_QUEUED;
      j->total = li->data.size;
      j->updated = now_seconds();
      j->next = g_jobs;
      g_jobs = j;
      pthread_cond_signal(&g_cond);
    }
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int gd_cancel(const char *game_id) {
  int found = 0;
  pthread_mutex_lock(&g_lock);
  for (job_t *j = g_jobs; j; j = j->next) {
    if (!strcmp(j->li.game_id, game_id)) {
      j->cancel = 1;
      if (j->state == J_QUEUED) {
        j->state = J_ERROR;
        str_lcpy(j->error, "cancelled", sizeof j->error);
      }
      found = 1;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found ? 0 : -1;
}

char *gd_jobs_json(void) {
  sb_t sb;
  sb_init(&sb);
  sb_append(&sb, "[");
  pthread_mutex_lock(&g_lock);
  int n = 0;
  for (job_t *j = g_jobs; j; j = j->next) {
    sb_append(&sb, n++ ? ",{\"gameId\":" : "{\"gameId\":");
    sb_json_str(&sb, j->li.game_id);
    sb_append(&sb, ",\"title\":");
    sb_json_str(&sb, j->li.title);
    sb_printf(&sb, ",\"state\":\"%s\",\"done\":%lld,\"total\":%lld,\"error\":",
              k_state_names[j->state], j->done, j->total);
    sb_json_str(&sb, j->error);
    sb_append(&sb, "}");
  }
  pthread_mutex_unlock(&g_lock);
  sb_append(&sb, "]");
  return sb_steal(&sb);
}

int gd_recent_roots(char roots[][FP_PATH_MAX], int max) {
  static run_entry_t entries[64];
  int n = list_run_dirs(entries, 64);
  if (n > max) n = max;
  for (int i = 0; i < n; i++) str_lcpy(roots[i], entries[i].path, FP_PATH_MAX);
  return n;
}

void gd_touch_run_dir(const char *game_id) {
  char dir[FP_PATH_MAX], marker[FP_PATH_MAX];
  gd_run_dir(game_id, dir, sizeof dir);
  path_join(marker, sizeof marker, dir, MARKER);
  char *text = read_text_file(marker, NULL);
  if (text) {
    write_text_file(marker, text); /* rewriting bumps the mtime */
    free(text);
  }
}

/* ------------------------------------------------------------ on demand */

static pthread_mutex_t g_fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fetch_cond = PTHREAD_COND_INITIALIZER;
static int g_fetching;
#define MISS_SLOTS 512
static char g_miss[MISS_SLOTS][192];
static double g_miss_time[MISS_SLOTS];
static int g_miss_next;

static int recently_missed(const char *key) {
  int hit = 0;
  pthread_mutex_lock(&g_fetch_lock);
  for (int i = 0; i < MISS_SLOTS; i++) {
    if (g_miss[i][0] && !strncmp(g_miss[i], key, sizeof g_miss[i] - 1) &&
        now_seconds() - g_miss_time[i] < 600) {
      hit = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_fetch_lock);
  return hit;
}

static void remember_miss(const char *key) {
  pthread_mutex_lock(&g_fetch_lock);
  str_lcpy(g_miss[g_miss_next], key, sizeof g_miss[0]);
  g_miss_time[g_miss_next] = now_seconds();
  g_miss_next = (g_miss_next + 1) % MISS_SLOTS;
  pthread_mutex_unlock(&g_fetch_lock);
}

/* At most four downloads for images/htdocs at once. */
static int fetch_limited(const char *url, const char *dest) {
  pthread_mutex_lock(&g_fetch_lock);
  while (g_fetching >= 4) pthread_cond_wait(&g_fetch_cond, &g_fetch_lock);
  g_fetching++;
  pthread_mutex_unlock(&g_fetch_lock);
  char err[256];
  int rc = is_file(dest) ? 0 : net_download(url, dest, NULL, NULL, err, sizeof err);
  if (rc) LOG("gamedata: %s", err);
  pthread_mutex_lock(&g_fetch_lock);
  g_fetching--;
  pthread_cond_signal(&g_fetch_cond);
  pthread_mutex_unlock(&g_fetch_lock);
  return rc;
}

static void url_encode_path(sb_t *sb, const char *path) {
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    if (isalnum(*p) || strchr("/-_.~!$&'()*+,;=:@", *p)) {
      sb_appendn(sb, (const char *)p, 1);
    } else {
      sb_printf(sb, "%%%02X", *p);
    }
  }
}

int gd_ensure_image(const char *rel, char *out_path, size_t size) {
  if (!rel_path_is_safe(rel)) return -1;
  path_join(out_path, size, g_cfg.images_path, rel);
  if (is_file(out_path)) return 0;
  if (!g_cfg.images_url[0] || recently_missed(rel)) return -1;
  sb_t url;
  sb_init(&url);
  sb_append(&url, g_cfg.images_url);
  if (url.len && url.buf[url.len - 1] != '/') sb_append(&url, "/");
  url_encode_path(&url, rel);
  int rc = fetch_limited(url.buf, out_path);
  sb_free(&url);
  if (rc) remember_miss(rel);
  return rc;
}

int gd_ensure_legacy_file(const char *host, const char *path, char *out_path, size_t size) {
  char rel[FP_PATH_MAX];
  snprintf(rel, sizeof rel, "%s/%s", host, path);
  if (!rel_path_is_safe(rel)) return -1;
  path_join(out_path, size, g_cfg.htdocs_path, rel);
  if (is_file(out_path)) return 0;
  if (!g_cfg.legacy_fallback_url[0] || !path[0] || str_ends_with_ci(path, "/") ||
      recently_missed(rel))
    return -1;
  sb_t url;
  sb_init(&url);
  sb_append(&url, g_cfg.legacy_fallback_url);
  if (url.len && url.buf[url.len - 1] != '/') sb_append(&url, "/");
  url_encode_path(&url, rel);
  int rc = fetch_limited(url.buf, out_path);
  sb_free(&url);
  if (rc) remember_miss(rel);
  return rc;
}

char *gd_files_json(const launch_info_t *li) {
  sb_t sb;
  sb_init(&sb);
  sb_append(&sb, "[");
  char zip_path[FP_PATH_MAX];
  if (!gd_zip_path(li, zip_path, sizeof zip_path)) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (mz_zip_reader_init_file(&zip, zip_path, 0)) {
      mz_uint n = mz_zip_reader_get_num_files(&zip);
      int out = 0;
      for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || strncmp(st.m_filename, "content/", 8) ||
            !st.m_filename[8])
          continue;
        if (out++) sb_append(&sb, ",");
        sb_json_str(&sb, st.m_filename + 8);
      }
      mz_zip_reader_end(&zip);
    }
  }
  sb_append(&sb, "]");
  return sb_steal(&sb);
}
