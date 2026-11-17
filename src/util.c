/* Flashpoint PS5 - small helpers shared by every module.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../third_party/sha256/sha256.h"

static FILE *g_log_fp;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_set_file(const char *path) {
  pthread_mutex_lock(&g_log_lock);
  if (g_log_fp) fclose(g_log_fp);
  g_log_fp = NULL;
  if (path && *path) {
    mkdirs_for_file(path);
    g_log_fp = fopen(path, "a");
  }
  pthread_mutex_unlock(&g_log_lock);
}

void logf_(const char *fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);

  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char stamp[32];
  strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

  pthread_mutex_lock(&g_log_lock);
  fprintf(stdout, "[fpps5 %s] %s\n", stamp, line);
  fflush(stdout);
  if (g_log_fp) {
    fprintf(g_log_fp, "[%s] %s\n", stamp, line);
    fflush(g_log_fp);
  }
  pthread_mutex_unlock(&g_log_lock);
}

/* ---------------------------------------------------------------- sb */

void sb_init(sb_t *sb) {
  sb->cap = 256;
  sb->len = 0;
  sb->buf = malloc(sb->cap);
  if (sb->buf) sb->buf[0] = 0;
}

void sb_free(sb_t *sb) {
  free(sb->buf);
  sb->buf = NULL;
  sb->len = sb->cap = 0;
}

static int sb_reserve(sb_t *sb, size_t extra) {
  if (!sb->buf) return -1;
  if (sb->len + extra + 1 <= sb->cap) return 0;
  size_t ncap = sb->cap * 2;
  while (ncap < sb->len + extra + 1) ncap *= 2;
  char *n = realloc(sb->buf, ncap);
  if (!n) return -1;
  sb->buf = n;
  sb->cap = ncap;
  return 0;
}

void sb_appendn(sb_t *sb, const char *s, size_t n) {
  if (sb_reserve(sb, n)) return;
  memcpy(sb->buf + sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = 0;
}

void sb_append(sb_t *sb, const char *s) {
  if (s) sb_appendn(sb, s, strlen(s));
}

void sb_printf(sb_t *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n > 0 && !sb_reserve(sb, (size_t)n)) {
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
    sb->len += (size_t)n;
  }
  va_end(ap2);
}

void sb_json_str(sb_t *sb, const char *s) {
  sb_appendn(sb, "\"", 1);
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
      switch (*p) {
      case '"': sb_appendn(sb, "\\\"", 2); break;
      case '\\': sb_appendn(sb, "\\\\", 2); break;
      case '\n': sb_appendn(sb, "\\n", 2); break;
      case '\r': sb_appendn(sb, "\\r", 2); break;
      case '\t': sb_appendn(sb, "\\t", 2); break;
      default:
        if (*p < 0x20) {
          char esc[8];
          snprintf(esc, sizeof esc, "\\u%04x", *p);
          sb_append(sb, esc);
        } else {
          sb_appendn(sb, (const char *)p, 1);
        }
      }
    }
  }
  sb_appendn(sb, "\"", 1);
}

char *sb_steal(sb_t *sb) {
  char *b = sb->buf;
  sb->buf = NULL;
  sb->len = sb->cap = 0;
  return b;
}

/* ----------------------------------------------------------- strings */

int str_ends_with_ci(const char *s, const char *suffix) {
  if (!s || !suffix) return 0;
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcasecmp(s + a - b, suffix);
}

int str_starts_with_ci(const char *s, const char *prefix) {
  if (!s || !prefix) return 0;
  return !strncasecmp(s, prefix, strlen(prefix));
}

int str_contains_ci(const char *s, const char *needle) {
  if (!s || !needle) return 0;
  size_t n = strlen(needle);
  if (!n) return 1;
  for (; *s; s++) {
    if (!strncasecmp(s, needle, n)) return 1;
  }
  return 0;
}

void str_lcpy(char *dst, const char *src, size_t size) {
  if (!size) return;
  if (!src) src = "";
  size_t n = strlen(src);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

char *str_dup(const char *s) {
  return strdup(s ? s : "");
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void url_decode_impl(char *s, int plus) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '%' && p[1] && p[2] && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
      *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
      p += 2;
    } else if (plus && *p == '+') {
      *o++ = ' ';
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
}

void url_decode_inplace(char *s) { url_decode_impl(s, 0); }
void url_decode_form_inplace(char *s) { url_decode_impl(s, 1); }

/* -------------------------------------------------------- filesystem */

int path_exists(const char *path) {
  struct stat st;
  return path && !stat(path, &st);
}

int is_dir(const char *path) {
  struct stat st;
  return path && !stat(path, &st) && S_ISDIR(st.st_mode);
}

int is_file(const char *path) {
  struct stat st;
  return path && !stat(path, &st) && S_ISREG(st.st_mode);
}

long long file_size(const char *path) {
  struct stat st;
  if (!path || stat(path, &st)) return -1;
  return (long long)st.st_size;
}

int mkdirs(const char *path) {
  char tmp[FP_PATH_MAX];
  str_lcpy(tmp, path, sizeof tmp);
  size_t len = strlen(tmp);
  if (!len) return -1;
  if (tmp[len - 1] == '/') tmp[len - 1] = 0;
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
  return 0;
}

int mkdirs_for_file(const char *path) {
  char tmp[FP_PATH_MAX];
  str_lcpy(tmp, path, sizeof tmp);
  char *slash = strrchr(tmp, '/');
  if (!slash || slash == tmp) return 0;
  *slash = 0;
  return mkdirs(tmp);
}

int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  mkdirs_for_file(dst);
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) {
    close(in);
    return -1;
  }
  char *buf = malloc(1 << 16);
  int rc = buf ? 0 : -1;
  ssize_t n;
  while (!rc && (n = read(in, buf, 1 << 16)) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(out, buf + off, (size_t)(n - off));
      if (w <= 0) {
        rc = -1;
        break;
      }
      off += w;
    }
  }
  free(buf);
  close(in);
  if (close(out)) rc = -1;
  return rc;
}

int copy_tree(const char *src, const char *dst, int recursive) {
  DIR *d = opendir(src);
  if (!d) return -1;
  mkdirs(dst);
  int rc = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[FP_PATH_MAX], t[FP_PATH_MAX];
    path_join(s, sizeof s, src, e->d_name);
    path_join(t, sizeof t, dst, e->d_name);
    if (is_dir(s)) {
      if (recursive && copy_tree(s, t, 1)) rc = -1;
    } else if (copy_file(s, t)) {
      rc = -1;
    }
  }
  closedir(d);
  return rc;
}

int remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st)) return 0;
  if (!S_ISDIR(st.st_mode)) return unlink(path);
  DIR *d = opendir(path);
  if (!d) return -1;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[FP_PATH_MAX];
    path_join(s, sizeof s, path, e->d_name);
    remove_tree(s);
  }
  closedir(d);
  return rmdir(path);
}

int write_text_file(const char *path, const char *text) {
  mkdirs_for_file(path);
  char tmp[FP_PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  size_t n = strlen(text);
  int rc = fwrite(text, 1, n, f) == n ? 0 : -1;
  if (fclose(f)) rc = -1;
  if (!rc && rename(tmp, path)) rc = -1;
  if (rc) unlink(tmp);
  return rc;
}

char *read_text_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  sb_t sb;
  sb_init(&sb);
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_appendn(&sb, buf, n);
  fclose(f);
  if (len_out) *len_out = sb.len;
  return sb_steal(&sb);
}

void path_join(char *out, size_t size, const char *a, const char *b) {
  if (!a || !*a) {
    str_lcpy(out, b, size);
    return;
  }
  size_t la = strlen(a);
  while (b && *b == '/') b++;
  snprintf(out, size, "%s%s%s", a, (la && a[la - 1] == '/') ? "" : "/", b ? b : "");
}

int rel_path_is_safe(const char *rel) {
  if (!rel || rel[0] == '/' || rel[0] == '\\') return 0;
  const char *p = rel;
  while (*p) {
    const char *seg = p;
    while (*p && *p != '/' && *p != '\\') p++;
    size_t n = (size_t)(p - seg);
    if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0;
    if (*p) p++;
  }
  return 1;
}

int url_split(const char *url, char *host, size_t hostsz, char *path, size_t pathsz) {
  const char *p = strstr(url, "://");
  if (!p) return -1;
  p += 3;
  const char *h = p;
  while (*p && *p != '/' && *p != '?' && *p != '#') p++;
  size_t hl = (size_t)(p - h);
  /* drop credentials and port */
  const char *at = memchr(h, '@', hl);
  if (at) {
    hl -= (size_t)(at + 1 - h);
    h = at + 1;
  }
  const char *colon = memchr(h, ':', hl);
  if (colon) hl = (size_t)(colon - h);
  if (!hl || hl >= hostsz) return -1;
  memcpy(host, h, hl);
  host[hl] = 0;
  for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);

  if (*p == '/') p++;
  const char *e = p;
  while (*e && *e != '?' && *e != '#') e++;
  size_t pl = (size_t)(e - p);
  if (pl >= pathsz) return -1;
  memcpy(path, p, pl);
  path[pl] = 0;
  url_decode_inplace(path);
  return 0;
}

int sha256_file_hex(const char *path, char out[65]) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  SHA256_CTX ctx;
  sha256_init(&ctx);
  unsigned char *buf = malloc(1 << 16);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t n;
  while ((n = fread(buf, 1, 1 << 16, f)) > 0) sha256_update(&ctx, buf, n);
  free(buf);
  fclose(f);
  unsigned char hash[32];
  sha256_final(&ctx, hash);
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
  out[64] = 0;
  return 0;
}

double now_seconds(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + tv.tv_usec / 1e6;
}
