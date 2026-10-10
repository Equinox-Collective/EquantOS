/*
 * Site scripts for NetSurf (EquantOS port).
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

/**
 * \file
 * Per-site page scripts run by an embedded QuickJS engine.
 *
 * Sites such as YouTube or GitHub ship an application that needs a JS engine
 * and a layout engine far beyond what NetSurf has, but they also embed the
 * data of the page in the HTML they serve.  A site script (res/sitejs/ *.js)
 * receives the fetched document and returns HTML NetSurf can lay out.
 */

#ifndef NETSURF_CONTENT_SITEJS_H
#define NETSURF_CONTENT_SITEJS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nsurl;

/**
 * Set the directory the site scripts are loaded from.
 *
 * The scripts are only read when a page first asks for them.
 */
void sitejs_set_path(const char *dir);

/** Release the script engine. */
void sitejs_fini(void);

/**
 * Does a site script want to rewrite HTML documents fetched from this URL?
 */
bool sitejs_wants(struct nsurl *url);

/**
 * Let the site script for \a url rewrite a fetched document.
 *
 * \param url       URL of the document
 * \param data      document source as fetched
 * \param len       length of \a data
 * \param http_code HTTP status of the fetch
 * \param outlen    updated to the length of the result
 * \return UTF-8 HTML to parse instead of \a data (caller frees), or NULL to
 *         keep the document as it is.
 */
char *sitejs_transform(struct nsurl *url, const uint8_t *data, size_t len,
		long http_code, size_t *outlen);

#endif
