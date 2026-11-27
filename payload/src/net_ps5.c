/* Flashpoint PS5 - downloads on the console through the system's own
 * HTTP/TLS stack (libSceHttp + libSceSsl), the same way websrv does.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "util.h"
#include "version.h"

int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttpInit(int, int, size_t);
int sceHttpCreateTemplate(int, const char *, int, int);
int sceHttpDeleteTemplate(int);
int sceHttpsSetSslCallback(int, void *, void *);
int sceHttpSetResponseHeaderMaxSize(int, size_t);
int sceHttpSetAutoRedirect(int, int);
int sceHttpSetConnectTimeOut(int, uint32_t);
int sceHttpSetRecvTimeOut(int, uint32_t);
int sceHttpSetResolveTimeOut(int, uint32_t);
int sceHttpCreateConnectionWithURL(int, const char *, int);
int sceHttpDeleteConnection(int);
int sceHttpCreateRequestWithURL(int, int, const char *, uint64_t);
int sceHttpDeleteRequest(int);
int sceHttpSendRequest(int, const void *, size_t);
int sceHttpGetStatusCode(int, int *);
int sceHttpGetResponseContentLength(int, int *, uint64_t *);
int sceHttpReadData(int, void *, size_t);
int sceHttpAddRequestHeader(int, const char *, const char *, uint32_t);

#define HTTP_VERSION_1_1 2
#define HTTP_METHOD_GET 0
#define HTTP_HEADER_OVERWRITE 0

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_http = -1;

static int ssl_cb(void) { return 0; }

int net_init(void) {
  pthread_mutex_lock(&g_lock);
  if (g_http < 0) {
    sceNetInit();
    int pool = sceNetPoolCreate("fpps5_net", 256 * 1024, 0);
    int ssl = sceSslInit(512 * 1024);
    if (pool < 0 || ssl < 0) {
      LOG("net: init failed (pool %x, ssl %x)", pool, ssl);
    } else {
      g_http = sceHttpInit(pool, ssl, 512 * 1024);
      if (g_http < 0) LOG("net: sceHttpInit failed (%x)", g_http);
    }
  }
  int rc = g_http < 0 ? -1 : 0;
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int net_download(const char *url, const char *dest, net_progress_cb cb, void *ud, char *err,
                 size_t errlen) {
  return net_download_ref(url, dest, NULL, cb, ud, err, errlen);
}

int net_download_ref(const char *url, const char *dest, const char *referer, net_progress_cb cb, void *ud,
                     char *err, size_t errlen) {
  if (net_init()) {
    snprintf(err, errlen, "network is not available");
    return -1;
  }
  char part[FP_PATH_MAX];
  snprintf(part, sizeof part, "%s.part", dest);
  mkdirs_for_file(dest);

  int tmpl = -1, conn = -1, req = -1, fd = -1, rc = -1;
  char *buf = NULL;
  if ((tmpl = sceHttpCreateTemplate(g_http, FPPS5_USER_AGENT, HTTP_VERSION_1_1, 1)) < 0) {
    snprintf(err, errlen, "sceHttpCreateTemplate: %x", tmpl);
    goto out;
  }
  sceHttpSetResponseHeaderMaxSize(tmpl, 0x4000);
  sceHttpsSetSslCallback(tmpl, (void *)ssl_cb, NULL);
  sceHttpSetAutoRedirect(tmpl, 1);
  sceHttpSetResolveTimeOut(tmpl, 20 * 1000 * 1000);
  sceHttpSetConnectTimeOut(tmpl, 20 * 1000 * 1000);
  sceHttpSetRecvTimeOut(tmpl, 60 * 1000 * 1000);
  if ((conn = sceHttpCreateConnectionWithURL(tmpl, url, 1)) < 0) {
    snprintf(err, errlen, "bad URL (%x): %s", conn, url);
    goto out;
  }
  if ((req = sceHttpCreateRequestWithURL(conn, HTTP_METHOD_GET, url, 0)) < 0) {
    snprintf(err, errlen, "sceHttpCreateRequestWithURL: %x", req);
    goto out;
  }
  if (referer && *referer) {
    int h = sceHttpAddRequestHeader(req, "Referer", referer, HTTP_HEADER_OVERWRITE);
    if (h < 0) LOG("net: couldn't set the Referer (%x)", h);
  }
  int e;
  if ((e = sceHttpSendRequest(req, NULL, 0)) < 0) {
    snprintf(err, errlen, "can't reach %s (%x)", url, e);
    goto out;
  }
  int status = 0;
  sceHttpGetStatusCode(req, &status);
  if (status != 200) {
    snprintf(err, errlen, "HTTP %d for %s", status, url);
    goto out;
  }
  int has_len = -1;
  uint64_t len = 0;
  long long total = -1;
  if (!sceHttpGetResponseContentLength(req, &has_len, &len) && has_len == 0) total = (long long)len;

  if ((fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    snprintf(err, errlen, "can't write %s: %s", part, strerror(errno));
    goto out;
  }
  buf = malloc(1 << 16);
  long long done = 0;
  for (;;) {
    int n = sceHttpReadData(req, buf, 1 << 16);
    if (n < 0) {
      snprintf(err, errlen, "download interrupted (%x)", n);
      goto out;
    }
    if (n == 0) break;
    for (int off = 0; off < n;) {
      ssize_t w = write(fd, buf + off, (size_t)(n - off));
      if (w <= 0) {
        snprintf(err, errlen, "disk write failed: %s", strerror(errno));
        goto out;
      }
      off += (int)w;
    }
    done += n;
    if (cb && cb(ud, done, total)) {
      snprintf(err, errlen, "cancelled");
      goto out;
    }
  }
  if (total >= 0 && done != total) {
    snprintf(err, errlen, "download incomplete (%lld of %lld bytes)", done, total);
    goto out;
  }
  if (close(fd)) {
    fd = -1;
    snprintf(err, errlen, "disk write failed: %s", strerror(errno));
    goto out;
  }
  fd = -1;
  if (rename(part, dest)) {
    snprintf(err, errlen, "can't move download into place: %s", strerror(errno));
    goto out;
  }
  rc = 0;

out:
  free(buf);
  if (fd >= 0) close(fd);
  if (rc) unlink(part);
  if (req >= 0) sceHttpDeleteRequest(req);
  if (conn >= 0) sceHttpDeleteConnection(conn);
  if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
  return rc;
}
