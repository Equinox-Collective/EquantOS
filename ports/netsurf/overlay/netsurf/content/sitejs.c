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
 * The engine is created the first time a HTML document is fetched.  Every
 * res/sitejs/ *.js file is evaluated in name order in one global context; the
 * scripts register themselves with registerSite() (see 00-lib.js), which
 * provides the two entry points used from here:
 *
 *   __siteWants(url)                  -> bool
 *   __siteTransform(url, html, code)  -> string | null
 *
 * Natives offered to the scripts (wrapped by 00-lib.js):
 *
 *   __native.http(url, method, ["Name: value", ...], body, timeout_ms)
 *        blocking HTTP(S) request -> {status, body, headers, url, error}
 *   __native.log(text)
 *   __native.readFile(name)           file next to the scripts, or null
 *   __native.loadState(key)           small persistent values kept in
 *   __native.saveState(key, text)     ~/.netsurf/sitejs-<key>
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <quickjs/quickjs.h>

#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "utils/useragent.h"

#include "content/sitejs.h"

/** longest a script may run before it is interrupted (seconds) */
#define SJ_TIME_LIMIT 40
/** largest HTTP response a script may receive */
#define SJ_MAX_RESPONSE (48 * 1024 * 1024)

static char *sj_dir;
static JSRuntime *sj_rt;
static JSContext *sj_ctx;
static bool sj_tried;
static CURL *sj_curl;
static time_t sj_deadline;

struct sj_buf {
	char *data;
	size_t len;
	size_t cap;
};

static bool sj_buf_add(struct sj_buf *b, const char *data, size_t len)
{
	if (b->len + len + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 16384;
		char *n;

		while (cap < b->len + len + 1)
			cap *= 2;
		if (cap > SJ_MAX_RESPONSE)
			return false;
		n = realloc(b->data, cap);
		if (n == NULL)
			return false;
		b->data = n;
		b->cap = cap;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
	return true;
}

static void sj_buf_adds(struct sj_buf *b, const char *s)
{
	sj_buf_add(b, s, strlen(s));
}

static void sj_buf_add_escaped(struct sj_buf *b, const char *s)
{
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '<': sj_buf_adds(b, "&lt;"); break;
		case '>': sj_buf_adds(b, "&gt;"); break;
		case '&': sj_buf_adds(b, "&amp;"); break;
		case '"': sj_buf_adds(b, "&quot;"); break;
		default: sj_buf_add(b, s, 1);
		}
	}
}

static char *sj_read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	struct sj_buf b = { NULL, 0, 0 };
	char chunk[8192];
	size_t n;

	if (f == NULL)
		return NULL;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		if (!sj_buf_add(&b, chunk, n)) {
			free(b.data);
			fclose(f);
			return NULL;
		}
	}
	fclose(f);
	if (b.data == NULL)
		b.data = calloc(1, 1);
	if (len != NULL)
		*len = b.len;
	return b.data;
}

static void sj_log(const char *what, const char *text)
{
	NSLOG(netsurf, INFO, "sitejs %s: %s", what, text);
	if (getenv("NETSURF_SITEJS_DEBUG") != NULL)
		fprintf(stderr, "sitejs %s: %s\n", what, text);
}

/** Describe the pending exception of the context (caller frees). */
static char *sj_exception(JSContext *ctx)
{
	JSValue exc = JS_GetException(ctx);
	JSValue stack;
	struct sj_buf b = { NULL, 0, 0 };
	const char *s;

	s = JS_ToCString(ctx, exc);
	sj_buf_adds(&b, s != NULL ? s : "unknown error");
	if (s != NULL)
		JS_FreeCString(ctx, s);

	stack = JS_GetPropertyStr(ctx, exc, "stack");
	if (JS_IsString(stack)) {
		s = JS_ToCString(ctx, stack);
		if (s != NULL) {
			sj_buf_adds(&b, "\n");
			sj_buf_adds(&b, s);
			JS_FreeCString(ctx, s);
		}
	}
	JS_FreeValue(ctx, stack);
	JS_FreeValue(ctx, exc);
	return b.data;
}

static int sj_interrupt(JSRuntime *rt, void *opaque)
{
	return time(NULL) > sj_deadline;
}

/* ---- natives ------------------------------------------------------------ */

static size_t sj_curl_write(char *ptr, size_t size, size_t nmemb, void *pw)
{
	if (!sj_buf_add(pw, ptr, size * nmemb))
		return 0;
	return size * nmemb;
}

static JSValue sj_native_http(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct sj_buf body = { NULL, 0, 0 }, head = { NULL, 0, 0 };
	struct curl_slist *hdrs = NULL;
	const char *url, *method = NULL, *post = NULL;
	size_t post_len = 0;
	int32_t timeout = 20000;
	long status = 0;
	char *eurl = NULL;
	CURLcode res;
	JSValue ret;

	if (argc < 1 || (url = JS_ToCString(ctx, argv[0])) == NULL)
		return JS_EXCEPTION;
	if (argc > 1 && JS_IsString(argv[1]))
		method = JS_ToCString(ctx, argv[1]);
	if (argc > 2 && JS_IsObject(argv[2])) {
		JSValue lenv = JS_GetPropertyStr(ctx, argv[2], "length");
		int32_t i, n = 0;

		JS_ToInt32(ctx, &n, lenv);
		JS_FreeValue(ctx, lenv);
		for (i = 0; i < n; i++) {
			JSValue hv = JS_GetPropertyUint32(ctx, argv[2], i);
			const char *h = JS_ToCString(ctx, hv);

			if (h != NULL) {
				hdrs = curl_slist_append(hdrs, h);
				JS_FreeCString(ctx, h);
			}
			JS_FreeValue(ctx, hv);
		}
	}
	if (argc > 3 && JS_IsString(argv[3]))
		post = JS_ToCStringLen(ctx, &post_len, argv[3]);
	if (argc > 4)
		JS_ToInt32(ctx, &timeout, argv[4]);

	if (sj_curl == NULL)
		sj_curl = curl_easy_init();
	else
		curl_easy_reset(sj_curl);
	if (sj_curl == NULL) {
		res = CURLE_FAILED_INIT;
	} else {
		curl_easy_setopt(sj_curl, CURLOPT_URL, url);
		curl_easy_setopt(sj_curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(sj_curl, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(sj_curl, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(sj_curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(sj_curl, CURLOPT_TIMEOUT_MS, (long)timeout);
		curl_easy_setopt(sj_curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
		curl_easy_setopt(sj_curl, CURLOPT_COOKIEFILE, "");
		curl_easy_setopt(sj_curl, CURLOPT_USERAGENT,
				user_agent_string());
		curl_easy_setopt(sj_curl, CURLOPT_WRITEFUNCTION, sj_curl_write);
		curl_easy_setopt(sj_curl, CURLOPT_WRITEDATA, &body);
		curl_easy_setopt(sj_curl, CURLOPT_HEADERFUNCTION, sj_curl_write);
		curl_easy_setopt(sj_curl, CURLOPT_HEADERDATA, &head);
		if (nsoption_charp(ca_bundle) != NULL)
			curl_easy_setopt(sj_curl, CURLOPT_CAINFO,
					nsoption_charp(ca_bundle));
		if (nsoption_charp(ca_path) != NULL)
			curl_easy_setopt(sj_curl, CURLOPT_CAPATH,
					nsoption_charp(ca_path));
		if (post != NULL) {
			curl_easy_setopt(sj_curl, CURLOPT_POSTFIELDSIZE_LARGE,
					(curl_off_t)post_len);
			curl_easy_setopt(sj_curl, CURLOPT_POSTFIELDS, post);
		}
		if (method != NULL && strcmp(method, "GET") != 0 &&
				!(post != NULL && strcmp(method, "POST") == 0))
			curl_easy_setopt(sj_curl, CURLOPT_CUSTOMREQUEST, method);
		if (hdrs != NULL)
			curl_easy_setopt(sj_curl, CURLOPT_HTTPHEADER, hdrs);

		res = curl_easy_perform(sj_curl);
		curl_easy_getinfo(sj_curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_getinfo(sj_curl, CURLINFO_EFFECTIVE_URL, &eurl);
	}

	ret = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, ret, "status", JS_NewInt32(ctx, (int32_t)status));
	JS_SetPropertyStr(ctx, ret, "body",
			JS_NewStringLen(ctx, body.data ? body.data : "", body.len));
	JS_SetPropertyStr(ctx, ret, "headers",
			JS_NewStringLen(ctx, head.data ? head.data : "", head.len));
	JS_SetPropertyStr(ctx, ret, "url", JS_NewString(ctx, eurl ? eurl : url));
	if (res != CURLE_OK)
		JS_SetPropertyStr(ctx, ret, "error",
				JS_NewString(ctx, curl_easy_strerror(res)));

	/* the next deadline starts after the network wait */
	sj_deadline = time(NULL) + SJ_TIME_LIMIT;

	curl_slist_free_all(hdrs);
	free(body.data);
	free(head.data);
	if (post != NULL)
		JS_FreeCString(ctx, post);
	if (method != NULL)
		JS_FreeCString(ctx, method);
	JS_FreeCString(ctx, url);
	return ret;
}

static JSValue sj_native_log(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *s;

	if (argc > 0 && (s = JS_ToCString(ctx, argv[0])) != NULL) {
		sj_log("log", s);
		JS_FreeCString(ctx, s);
	}
	return JS_UNDEFINED;
}

/** only plain file names: scripts cannot reach outside their directories */
static bool sj_name_ok(const char *name)
{
	const char *p;

	if (*name == '\0' || *name == '.')
		return false;
	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
				(*p >= '0' && *p <= '9') ||
				*p == '.' || *p == '-' || *p == '_'))
			return false;
	}
	return true;
}

static JSValue sj_native_read_file(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	char path[1024];
	const char *name;
	char *data;
	size_t len;
	JSValue ret = JS_NULL;

	if (argc < 1 || (name = JS_ToCString(ctx, argv[0])) == NULL)
		return JS_EXCEPTION;
	if (sj_dir != NULL && sj_name_ok(name)) {
		snprintf(path, sizeof(path), "%s/%s", sj_dir, name);
		data = sj_read_file(path, &len);
		if (data != NULL) {
			ret = JS_NewStringLen(ctx, data, len);
			free(data);
		}
	}
	JS_FreeCString(ctx, name);
	return ret;
}

static bool sj_state_path(char *path, size_t size, const char *key)
{
	const char *home = getenv("HOME");

	if (!sj_name_ok(key))
		return false;
	snprintf(path, size, "%s/.netsurf/sitejs-%s",
			home != NULL ? home : "", key);
	return true;
}

static JSValue sj_native_load_state(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	char path[1024];
	const char *key;
	char *data;
	size_t len;
	JSValue ret = JS_NULL;

	if (argc < 1 || (key = JS_ToCString(ctx, argv[0])) == NULL)
		return JS_EXCEPTION;
	if (sj_state_path(path, sizeof(path), key)) {
		data = sj_read_file(path, &len);
		if (data != NULL) {
			ret = JS_NewStringLen(ctx, data, len);
			free(data);
		}
	}
	JS_FreeCString(ctx, key);
	return ret;
}

static JSValue sj_native_save_state(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	char path[1024];
	const char *key, *value;
	size_t len;
	bool ok = false;
	FILE *f;

	if (argc < 2 || (key = JS_ToCString(ctx, argv[0])) == NULL)
		return JS_EXCEPTION;
	value = JS_ToCStringLen(ctx, &len, argv[1]);
	if (value != NULL && sj_state_path(path, sizeof(path), key)) {
		f = fopen(path, "wb");
		if (f != NULL) {
			ok = fwrite(value, 1, len, f) == len;
			fclose(f);
		}
	}
	if (value != NULL)
		JS_FreeCString(ctx, value);
	JS_FreeCString(ctx, key);
	return JS_NewBool(ctx, ok);
}

/* ---- engine ------------------------------------------------------------- */

static int sj_name_cmp(const void *a, const void *b)
{
	return strcmp(*(char * const *)a, *(char * const *)b);
}

static void sj_load_scripts(void)
{
	char path[1024];
	char *names[64];
	int n = 0, i;
	struct dirent *de;
	DIR *d;

	d = opendir(sj_dir);
	if (d == NULL) {
		sj_log("init", "script directory not found");
		return;
	}
	while ((de = readdir(d)) != NULL && n < 64) {
		size_t l = strlen(de->d_name);

		if (l > 3 && strcmp(de->d_name + l - 3, ".js") == 0)
			names[n++] = strdup(de->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(names[0]), sj_name_cmp);

	for (i = 0; i < n; i++) {
		size_t len;
		char *src;
		JSValue v;

		snprintf(path, sizeof(path), "%s/%s", sj_dir, names[i]);
		src = sj_read_file(path, &len);
		if (src != NULL) {
			sj_deadline = time(NULL) + SJ_TIME_LIMIT;
			v = JS_Eval(sj_ctx, src, len, names[i],
					JS_EVAL_TYPE_GLOBAL);
			if (JS_IsException(v)) {
				char *e = sj_exception(sj_ctx);

				sj_log(names[i], e != NULL ? e : "error");
				free(e);
			}
			JS_FreeValue(sj_ctx, v);
			free(src);
		}
		free(names[i]);
	}
}

static void sj_close_engine(void)
{
	if (sj_ctx != NULL)
		JS_FreeContext(sj_ctx);
	if (sj_rt != NULL)
		JS_FreeRuntime(sj_rt);
	sj_ctx = NULL;
	sj_rt = NULL;
	sj_tried = false;
}

static bool sj_open_engine(void)
{
	JSValue global, native;

	if (sj_ctx != NULL)
		return true;
	if (sj_tried || sj_dir == NULL)
		return false;
	sj_tried = true;

	sj_rt = JS_NewRuntime();
	if (sj_rt == NULL)
		return false;
	JS_SetMemoryLimit(sj_rt, 256 * 1024 * 1024);
	JS_SetMaxStackSize(sj_rt, 2 * 1024 * 1024);
	JS_SetInterruptHandler(sj_rt, sj_interrupt, NULL);

	sj_ctx = JS_NewContext(sj_rt);
	if (sj_ctx == NULL) {
		JS_FreeRuntime(sj_rt);
		sj_rt = NULL;
		return false;
	}

	global = JS_GetGlobalObject(sj_ctx);
	native = JS_NewObject(sj_ctx);
	JS_SetPropertyStr(sj_ctx, native, "http",
			JS_NewCFunction(sj_ctx, sj_native_http, "http", 5));
	JS_SetPropertyStr(sj_ctx, native, "log",
			JS_NewCFunction(sj_ctx, sj_native_log, "log", 1));
	JS_SetPropertyStr(sj_ctx, native, "readFile",
			JS_NewCFunction(sj_ctx, sj_native_read_file,
					"readFile", 1));
	JS_SetPropertyStr(sj_ctx, native, "loadState",
			JS_NewCFunction(sj_ctx, sj_native_load_state,
					"loadState", 1));
	JS_SetPropertyStr(sj_ctx, native, "saveState",
			JS_NewCFunction(sj_ctx, sj_native_save_state,
					"saveState", 2));
	JS_SetPropertyStr(sj_ctx, global, "__native", native);
	JS_FreeValue(sj_ctx, global);

	sj_load_scripts();
	return true;
}

/** Call a global function; the result must be freed by the caller. */
static JSValue sj_call(const char *name, int argc, JSValue *argv)
{
	JSValue global = JS_GetGlobalObject(sj_ctx);
	JSValue fn = JS_GetPropertyStr(sj_ctx, global, name);
	JSValue ret = JS_UNDEFINED;

	if (JS_IsFunction(sj_ctx, fn)) {
		sj_deadline = time(NULL) + SJ_TIME_LIMIT;
		ret = JS_Call(sj_ctx, fn, global, argc, argv);
	}
	JS_FreeValue(sj_ctx, fn);
	JS_FreeValue(sj_ctx, global);
	return ret;
}

/* exported interface documented in content/sitejs.h */
void sitejs_set_path(const char *dir)
{
	free(sj_dir);
	sj_dir = dir != NULL ? strdup(dir) : NULL;
}

/* exported interface documented in content/sitejs.h */
void sitejs_fini(void)
{
	sj_close_engine();
	if (sj_curl != NULL)
		curl_easy_cleanup(sj_curl);
	sj_curl = NULL;
	free(sj_dir);
	sj_dir = NULL;
}

/* exported interface documented in content/sitejs.h */
bool sitejs_wants(struct nsurl *url)
{
	JSValue arg, ret;
	bool wants;

	if (url == NULL || !sj_open_engine())
		return false;

	arg = JS_NewString(sj_ctx, nsurl_access(url));
	ret = sj_call("__siteWants", 1, &arg);
	if (JS_IsException(ret)) {
		char *e = sj_exception(sj_ctx);

		sj_log("wants", e != NULL ? e : "error");
		free(e);
		wants = false;
	} else {
		wants = JS_ToBool(sj_ctx, ret) > 0;
	}
	JS_FreeValue(sj_ctx, ret);
	JS_FreeValue(sj_ctx, arg);
	return wants;
}

/* exported interface documented in content/sitejs.h */
char *sitejs_transform(struct nsurl *url, const uint8_t *data, size_t len,
		long http_code, size_t *outlen)
{
	JSValue argv[3], ret;
	char *out = NULL;

	if (getenv("NETSURF_SITEJS_RELOAD") != NULL)
		sj_close_engine(); /* development: pick up edited scripts */
	if (url == NULL || !sj_open_engine())
		return NULL;

	argv[0] = JS_NewString(sj_ctx, nsurl_access(url));
	argv[1] = JS_NewStringLen(sj_ctx, (const char *)data, len);
	argv[2] = JS_NewInt32(sj_ctx, (int32_t)http_code);
	ret = sj_call("__siteTransform", 3, argv);

	if (JS_IsException(ret)) {
		struct sj_buf b = { NULL, 0, 0 };
		char *e = sj_exception(sj_ctx);

		sj_log("transform", e != NULL ? e : "error");
		sj_buf_adds(&b, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
				"<title>Site script error</title></head><body>"
				"<h2>Site script error</h2><p>");
		sj_buf_add_escaped(&b, nsurl_access(url));
		sj_buf_adds(&b, "</p><pre>");
		sj_buf_add_escaped(&b, e != NULL ? e : "error");
		sj_buf_adds(&b, "</pre></body></html>");
		free(e);
		out = b.data;
		if (out != NULL)
			*outlen = b.len;
	} else if (JS_IsString(ret)) {
		size_t l;
		const char *s = JS_ToCStringLen(sj_ctx, &l, ret);

		if (s != NULL) {
			out = malloc(l + 1);
			if (out != NULL) {
				memcpy(out, s, l);
				out[l] = '\0';
				*outlen = l;
			}
			JS_FreeCString(sj_ctx, s);
		}
	}

	JS_FreeValue(sj_ctx, ret);
	JS_FreeValue(sj_ctx, argv[0]);
	JS_FreeValue(sj_ctx, argv[1]);
	JS_FreeValue(sj_ctx, argv[2]);

	/* documents are large: do not let their garbage accumulate */
	JS_RunGC(sj_rt);

	return out;
}
