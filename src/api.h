/* Flashpoint PS5 - UI server: merged frontend + JSON API.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "httpd.h"

extern volatile int g_quit_requested;

void api_handle(http_req_t *req, void *ctx);
