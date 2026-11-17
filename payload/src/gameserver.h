/* Flashpoint PS5 - Flashpoint-style game server (proxy or /host/path).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "httpd.h"

void gameserver_handle(http_req_t *req, void *ctx);
