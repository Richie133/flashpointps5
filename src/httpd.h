/* Flashpoint PS5 - a small threaded HTTP/1.1 server (no dependencies).
 * Used twice: for the UI/API and for the Flashpoint-style game server.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

typedef struct http_req {
  int fd;
  int port;                 /* local port this request came in on */
  char method[12];
  char target[4096];        /* raw request-target (may be an absolute URI) */
  char path[4096];          /* decoded path, starts with '/', no query */
  char query[4096];         /* raw query string, without '?' */
  char host[256];           /* Host header without port */
  char host_header[300];    /* Host header as sent */
  char referer[2048];
  char range[128];
  char client_ip[64];
  long content_length;
  char *body;               /* request body (NUL-terminated), may be NULL */
  size_t body_len;
  int keep_alive;
  int is_head;
  int is_proxy;             /* target was an absolute http:// URI */
  int responded;
} http_req_t;

typedef void (*http_handler_t)(http_req_t *req, void *ctx);

/* Starts listening on 0.0.0.0:port in a background thread. 0 on success. */
int httpd_start(int port, http_handler_t handler, void *ctx);

/* Query/form helpers: copies the decoded value of `name` into buf.
 * Returns buf, or NULL when missing. */
const char *http_param(const http_req_t *req, const char *name, char *buf, size_t size);
int http_param_bool(const http_req_t *req, const char *name, int def);
long http_param_long(const http_req_t *req, const char *name, long def);

/* Responses */
void http_respond(http_req_t *req, int status, const char *ctype,
                  const void *body, size_t len, const char *extra_headers);
void http_respond_json(http_req_t *req, int status, const char *json);
void http_respond_text(http_req_t *req, int status, const char *text);
void http_redirect(http_req_t *req, const char *location);
/* Sends a file from disk (Range supported). Returns 0, or -1 if missing. */
int http_respond_file(http_req_t *req, const char *path, const char *ctype, const char *extra_headers);

const char *mime_for_path(const char *path);
