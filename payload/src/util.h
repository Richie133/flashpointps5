/* Flashpoint PS5 - small helpers shared by every module.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define FP_PATH_MAX 1024

/* Logging: to stdout and, once set, to a log file. */
void log_set_file(const char *path);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

/* Growable string buffer (always NUL-terminated). */
typedef struct sb {
  char *buf;
  size_t len;
  size_t cap;
} sb_t;

void sb_init(sb_t *sb);
void sb_free(sb_t *sb);
void sb_append(sb_t *sb, const char *s);
void sb_appendn(sb_t *sb, const char *s, size_t n);
void sb_printf(sb_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(sb_t *sb, const char *s);   /* appends "escaped" (null -> "") */
char *sb_steal(sb_t *sb);                    /* returns buffer, caller frees */

/* Strings */
int str_ends_with_ci(const char *s, const char *suffix);
int str_starts_with_ci(const char *s, const char *prefix);
int str_contains_ci(const char *s, const char *needle);
void str_lcpy(char *dst, const char *src, size_t size);
char *str_dup(const char *s);  /* NULL-safe strdup */
void url_decode_inplace(char *s); /* %XX and (optionally) '+' */
void url_decode_form_inplace(char *s); /* %XX and '+' -> ' ' */

/* Filesystem */
int path_exists(const char *path);
int is_dir(const char *path);
int is_file(const char *path);
long long file_size(const char *path);
int mkdirs(const char *path);            /* mkdir -p */
int mkdirs_for_file(const char *path);   /* mkdir -p dirname(path) */
int copy_file(const char *src, const char *dst);
int copy_tree(const char *src, const char *dst, int recursive);
int remove_tree(const char *path);
int write_text_file(const char *path, const char *text);
char *read_text_file(const char *path, size_t *len_out);
void path_join(char *out, size_t size, const char *a, const char *b);

/* A relative path is "safe" when it has no "..", no NUL and doesn't start with '/'. */
int rel_path_is_safe(const char *rel);

/* Split an http(s) URL into host and decoded path (without leading '/').
 * Returns 0 on success. Query and fragment are dropped. */
int url_split(const char *url, char *host, size_t hostsz, char *path, size_t pathsz);

/* sha256 of a file as lowercase hex (65 bytes). Returns 0 on success. */
int sha256_file_hex(const char *path, char out[65]);

/* Monotonic-ish time in seconds. */
double now_seconds(void);
