/* Flashpoint PS5 - Flash from other websites (Dagobah, PiczoGame, swfchan)
 * and any .swf link, played in Ruffle Flash.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

/* The sites and their lists. Caller frees. */
char *web_sources_json(void);

/* One page of a site's list or search. Caller frees. */
/* adult: show entries the site marks NSFW. */
char *web_list_json(const char *site, const char *list, const char *q, int page, int adult);

/* The cached copy of a list picture from one of the sites; 0 when found. */
int web_thumb_path(const char *url, char *out, size_t size);

/* Finds, downloads and starts a Flash file; answers like /api/play
 * ("pending" until done). Caller frees. */
char *web_play_json(const char *url, const char *title, const char *source);
