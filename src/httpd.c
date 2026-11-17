/* Flashpoint PS5 - a small threaded HTTP/1.1 server (no dependencies).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "httpd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "util.h"
#include "version.h"

#define HDR_MAX (32 * 1024)
#define BODY_MAX (4 * 1024 * 1024)

typedef struct server {
  int fd;
  int port;
  http_handler_t handler;
  void *ctx;
} server_t;

typedef struct conn {
  server_t *srv;
  int fd;
  char ip[64];
} conn_t;

/* ------------------------------------------------------------ mime */

static const struct { const char *ext; const char *type; } k_mime[] = {
  {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
  {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"},
  {".css", "text/css; charset=utf-8"}, {".json", "application/json"},
  {".xml", "text/xml"}, {".txt", "text/plain; charset=utf-8"}, {".svg", "image/svg+xml"},
  {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
  {".gif", "image/gif"}, {".webp", "image/webp"}, {".ico", "image/x-icon"}, {".bmp", "image/bmp"},
  {".swf", "application/x-shockwave-flash"}, {".spl", "application/futuresplash"},
  {".mp3", "audio/mpeg"}, {".ogg", "audio/ogg"}, {".wav", "audio/wav"}, {".m4a", "audio/mp4"},
  {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".flv", "video/x-flv"},
  {".wasm", "application/wasm"}, {".ttf", "font/ttf"}, {".otf", "font/otf"},
  {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".zip", "application/zip"},
  {".dcr", "application/x-director"}, {".unity3d", "application/vnd.unity"},
  {NULL, NULL},
};

const char *mime_for_path(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  if (dot && (!slash || dot > slash)) {
    for (int i = 0; k_mime[i].ext; i++) {
      if (!strcasecmp(dot, k_mime[i].ext)) return k_mime[i].type;
    }
  }
  return "application/octet-stream";
}

/* ------------------------------------------------------------ io */

static int send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  while (len) {
#ifdef MSG_NOSIGNAL
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
#else
    ssize_t n = send(fd, p, len, 0);
#endif
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static const char *status_text(int s) {
  switch (s) {
  case 200: return "OK";
  case 204: return "No Content";
  case 206: return "Partial Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 416: return "Range Not Satisfiable";
  case 500: return "Internal Server Error";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  default: return "Unknown";
  }
}

static int send_head(http_req_t *req, int status, const char *ctype, long long len,
                     const char *extra) {
  char head[2048];
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 %d %s\r\n"
                   "Server: " FPPS5_USER_AGENT "\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %lld\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: %s\r\n"
                   "%s"
                   "\r\n",
                   status, status_text(status), ctype ? ctype : "application/octet-stream", len,
                   req->keep_alive ? "keep-alive" : "close", extra ? extra : "");
  if (n < 0 || (size_t)n >= sizeof head) return -1;
  req->responded = 1;
  return send_all(req->fd, head, (size_t)n);
}

void http_respond(http_req_t *req, int status, const char *ctype, const void *body,
                  size_t len, const char *extra_headers) {
  if (send_head(req, status, ctype, (long long)len, extra_headers)) {
    req->keep_alive = 0;
    return;
  }
  if (!req->is_head && len && send_all(req->fd, body, len)) req->keep_alive = 0;
}

void http_respond_json(http_req_t *req, int status, const char *json) {
  http_respond(req, status, "application/json; charset=utf-8", json, strlen(json),
               "Cache-Control: no-store\r\n");
}

void http_respond_text(http_req_t *req, int status, const char *text) {
  http_respond(req, status, "text/plain; charset=utf-8", text, strlen(text), NULL);
}

void http_redirect(http_req_t *req, const char *location) {
  char extra[2300];
  snprintf(extra, sizeof extra, "Location: %s\r\n", location);
  http_respond(req, 302, "text/plain", "", 0, extra);
}

int http_respond_file(http_req_t *req, const char *path, const char *ctype,
                      const char *extra_headers) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    return -1;
  }
  long long size = (long long)st.st_size, start = 0, end = size - 1;
  int status = 200;
  char extra[1024];
  snprintf(extra, sizeof extra, "Accept-Ranges: bytes\r\n%s", extra_headers ? extra_headers : "");

  if (req->range[0] && !strncasecmp(req->range, "bytes=", 6) && size > 0) {
    const char *r = req->range + 6;
    char *dash = strchr(r, '-');
    if (dash) {
      long long a = -1, b = -1;
      if (dash > r) a = atoll(r);
      if (dash[1]) b = atoll(dash + 1);
      if (a < 0 && b >= 0) { /* suffix range: last b bytes */
        a = size - b;
        b = size - 1;
      }
      if (b < 0 || b >= size) b = size - 1;
      if (a < 0) a = 0;
      if (a > b || a >= size) {
        close(fd);
        char cr[96];
        snprintf(cr, sizeof cr, "Content-Range: bytes */%lld\r\n", size);
        http_respond(req, 416, "text/plain", "", 0, cr);
        return 0;
      }
      start = a;
      end = b;
      status = 206;
      size_t l = strlen(extra);
      snprintf(extra + l, sizeof extra - l, "Content-Range: bytes %lld-%lld/%lld\r\n", start, end,
               size);
    }
  }

  long long len = size ? end - start + 1 : 0;
  if (send_head(req, status, ctype ? ctype : mime_for_path(path), len, extra)) {
    close(fd);
    req->keep_alive = 0;
    return 0;
  }
  if (!req->is_head && len > 0) {
    if (lseek(fd, (off_t)start, SEEK_SET) < 0) {
      close(fd);
      req->keep_alive = 0;
      return 0;
    }
    char *buf = malloc(1 << 16);
    long long left = len;
    while (buf && left > 0) {
      ssize_t n = read(fd, buf, (size_t)(left < (1 << 16) ? left : (1 << 16)));
      if (n <= 0 || send_all(req->fd, buf, (size_t)n)) {
        req->keep_alive = 0;
        break;
      }
      left -= n;
    }
    free(buf);
  }
  close(fd);
  return 0;
}

/* ------------------------------------------------------------ params */

const char *http_param(const http_req_t *req, const char *name, char *buf, size_t size) {
  const char *sources[2] = {req->query, NULL};
  /* form-encoded POST bodies count as parameters too */
  if (req->body && req->body_len && !strchr(req->body, '{')) sources[1] = req->body;
  size_t nl = strlen(name);
  for (int s = 0; s < 2; s++) {
    const char *q = sources[s];
    while (q && *q) {
      const char *amp = strchr(q, '&');
      size_t seglen = amp ? (size_t)(amp - q) : strlen(q);
      if (seglen >= nl && !strncmp(q, name, nl) && (seglen == nl || q[nl] == '=')) {
        const char *v = seglen == nl ? q + nl : q + nl + 1;
        size_t vl = seglen - (size_t)(v - q);
        if (vl >= size) vl = size - 1;
        memcpy(buf, v, vl);
        buf[vl] = 0;
        url_decode_form_inplace(buf);
        return buf;
      }
      q = amp ? amp + 1 : NULL;
    }
  }
  return NULL;
}

int http_param_bool(const http_req_t *req, const char *name, int def) {
  char b[16];
  if (!http_param(req, name, b, sizeof b)) return def;
  return !strcasecmp(b, "true") || !strcmp(b, "1") || !strcasecmp(b, "yes") || !b[0];
}

long http_param_long(const http_req_t *req, const char *name, long def) {
  char b[32];
  if (!http_param(req, name, b, sizeof b) || !b[0]) return def;
  return strtol(b, NULL, 10);
}

/* ------------------------------------------------------------ parsing */

static void trim(char *s) {
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  size_t i = 0;
  while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s + i, n - i + 1);
}

/* Reads one request head into buf (which may already hold `*have` bytes).
 * Returns header length (incl. CRLFCRLF) or -1. */
static int read_head(int fd, char *buf, size_t *have) {
  for (;;) {
    buf[*have] = 0;
    char *end = strstr(buf, "\r\n\r\n");
    if (end) return (int)(end - buf) + 4;
    if (*have >= HDR_MAX - 1) return -1;
    ssize_t n = recv(fd, buf + *have, HDR_MAX - 1 - *have, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    *have += (size_t)n;
  }
}

static int parse_head(http_req_t *req, char *head) {
  char *line_end = strstr(head, "\r\n");
  if (!line_end) return -1;
  *line_end = 0;
  char version[16] = "";
  if (sscanf(head, "%11s %4095s %15s", req->method, req->target, version) != 3) return -1;
  req->keep_alive = !strcmp(version, "HTTP/1.1");
  req->is_head = !strcmp(req->method, "HEAD");

  for (char *line = line_end + 2; *line;) {
    char *e = strstr(line, "\r\n");
    if (!e) break;
    *e = 0;
    char *colon = strchr(line, ':');
    if (colon) {
      *colon = 0;
      char *val = colon + 1;
      trim(val);
      if (!strcasecmp(line, "Host")) {
        str_lcpy(req->host_header, val, sizeof req->host_header);
        str_lcpy(req->host, val, sizeof req->host);
        char *pc = strrchr(req->host, ':');
        if (pc && !strchr(req->host, ']')) *pc = 0;
      } else if (!strcasecmp(line, "Referer")) {
        str_lcpy(req->referer, val, sizeof req->referer);
      } else if (!strcasecmp(line, "Range")) {
        str_lcpy(req->range, val, sizeof req->range);
      } else if (!strcasecmp(line, "Content-Length")) {
        req->content_length = atol(val);
      } else if (!strcasecmp(line, "Connection") || !strcasecmp(line, "Proxy-Connection")) {
        if (!strcasecmp(val, "close")) req->keep_alive = 0;
        else if (!strcasecmp(val, "keep-alive")) req->keep_alive = 1;
      }
    }
    line = e + 2;
  }

  /* Absolute URI => proxy request (how Flashpoint's game server is used). */
  const char *t = req->target;
  char tmp[4096];
  if (!strncasecmp(t, "http://", 7) || !strncasecmp(t, "https://", 8)) {
    req->is_proxy = 1;
    const char *hs = strstr(t, "://") + 3;
    const char *ps = strchr(hs, '/');
    size_t hl = ps ? (size_t)(ps - hs) : strlen(hs);
    if (hl >= sizeof req->host) hl = sizeof req->host - 1;
    memcpy(req->host, hs, hl);
    req->host[hl] = 0;
    char *pc = strchr(req->host, ':');
    if (pc) *pc = 0;
    str_lcpy(tmp, ps ? ps : "/", sizeof tmp);
  } else {
    str_lcpy(tmp, t, sizeof tmp);
  }
  for (char *c = req->host; *c; c++) *c = (char)tolower((unsigned char)*c);
  char *frag = strchr(tmp, '#');
  if (frag) *frag = 0;
  char *q = strchr(tmp, '?');
  if (q) {
    *q = 0;
    str_lcpy(req->query, q + 1, sizeof req->query);
  }
  url_decode_inplace(tmp);
  if (tmp[0] != '/') {
    req->path[0] = '/';
    str_lcpy(req->path + 1, tmp, sizeof req->path - 1);
  } else {
    str_lcpy(req->path, tmp, sizeof req->path);
  }
  return 0;
}

static void *conn_main(void *arg) {
  conn_t *c = arg;
  char *buf = malloc(HDR_MAX);
  size_t have = 0;
  int served = 0;
  while (buf) {
    int hl = read_head(c->fd, buf, &have);
    if (hl < 0) break;
    http_req_t *req = calloc(1, sizeof *req);
    if (!req) break;
    req->fd = c->fd;
    req->port = c->srv->port;
    str_lcpy(req->client_ip, c->ip, sizeof req->client_ip);

    char saved = buf[hl];
    buf[hl] = 0;
    int bad = parse_head(req, buf);
    buf[hl] = saved;
    size_t extra = have - (size_t)hl; /* bytes after the head */

    if (!bad && req->content_length > 0) {
      if (req->content_length > BODY_MAX) {
        bad = 1;
      } else {
        req->body = malloc((size_t)req->content_length + 1);
        size_t got = extra < (size_t)req->content_length ? extra : (size_t)req->content_length;
        if (req->body) memcpy(req->body, buf + hl, got);
        while (req->body && got < (size_t)req->content_length) {
          ssize_t n = recv(c->fd, req->body + got, (size_t)req->content_length - got, 0);
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0) break;
          got += (size_t)n;
        }
        if (!req->body || got < (size_t)req->content_length) bad = 1;
        else req->body[got] = 0, req->body_len = got;
        size_t used = (size_t)hl + (extra < (size_t)req->content_length ? extra : (size_t)req->content_length);
        memmove(buf, buf + used, have - used);
        have -= used;
      }
    } else {
      memmove(buf, buf + hl, extra);
      have = extra;
    }

    if (bad) {
      req->keep_alive = 0;
      http_respond_text(req, 400, "bad request\n");
    } else if (!strcmp(req->method, "OPTIONS")) {
      http_respond(req, 204, "text/plain", "", 0,
                   "Access-Control-Allow-Methods: GET, POST, HEAD, OPTIONS\r\n"
                   "Access-Control-Allow-Headers: *\r\n");
    } else {
      c->srv->handler(req, c->srv->ctx);
      if (!req->responded) http_respond_text(req, 404, "not found\n");
    }
    int keep = req->keep_alive;
    free(req->body);
    free(req);
    served++;
    if (!keep || served > 1000) break;
  }
  free(buf);
  close(c->fd);
  free(c);
  return NULL;
}

static void *accept_main(void *arg) {
  server_t *srv = arg;
  for (;;) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof addr;
    int fd = accept(srv->fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      LOG("httpd: accept failed on port %d (%s)", srv->port, strerror(errno));
      usleep(100 * 1000);
      continue;
    }
    struct timeval tv = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    conn_t *c = calloc(1, sizeof *c);
    if (!c) {
      close(fd);
      continue;
    }
    c->srv = srv;
    c->fd = fd;
    inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof c->ip);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&th, &attr, conn_main, c)) {
      close(fd);
      free(c);
    }
    pthread_attr_destroy(&attr);
  }
  return NULL;
}

int httpd_start(int port, http_handler_t handler, void *ctx) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 64)) {
    LOG("httpd: can't listen on port %d (%s)", port, strerror(errno));
    close(fd);
    return -1;
  }
  server_t *srv = calloc(1, sizeof *srv);
  srv->fd = fd;
  srv->port = port;
  srv->handler = handler;
  srv->ctx = ctx;
  pthread_t th;
  if (pthread_create(&th, NULL, accept_main, srv)) {
    close(fd);
    free(srv);
    return -1;
  }
  pthread_detach(th);
  LOG("httpd: listening on port %d", port);
  return 0;
}
