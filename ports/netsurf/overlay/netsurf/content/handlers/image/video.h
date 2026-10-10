/*
 * Video player content for NetSurf (EquantOS port).
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

/**
 * \file
 * Interface to the <video>/<audio> player content handler.
 */

#ifndef NETSURF_IMAGE_VIDEO_H_
#define NETSURF_IMAGE_VIDEO_H_

#include <stdbool.h>

#include "utils/errors.h"

struct hlcache_handle;

/** MIME type of the player description a <video> element is turned into */
#define NSVIDEO_MIME_TYPE "video/x-nsvideo"

nserror nsvideo_init(void);

/**
 * Is this object a player?  Players get mouse clicks and movement passed on
 * by the HTML content they are embedded in.
 */
bool nsvideo_is_player(struct hlcache_handle *h);

#endif
