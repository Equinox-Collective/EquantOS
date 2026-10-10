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
 * <video>/<audio> player: libcurl transfer, ffmpeg demux/decode, OSS sound.
 *
 * The HTML box code turns a media element into an object whose source is a
 * small description (media URL, poster, autoplay...) of type
 * NSVIDEO_MIME_TYPE.  The media itself never goes through NetSurf's cache:
 * the player keeps its own libcurl transfer with a sliding window over the
 * file, so memory use is bounded and seeking is a new range request.
 *
 * Everything runs from NetSurf's scheduler on the main loop - there are no
 * threads.  Each tick moves bytes from the network, demuxes a few packets,
 * tops up the sound device and shows a picture when its time has come.  The
 * sound device is the clock: a picture is due when the samples that belong
 * to it are being played, so a slow machine drops pictures, not sound.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include <curl/curl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <nsutils/time.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/useragent.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "netsurf/misc.h"
#include "netsurf/mouse.h"
#include "netsurf/plotters.h"
#include "content/content_factory.h"
#include "content/content_protected.h"
#include "content/hlcache.h"
#include "content/llcache.h"
#include "desktop/gui_internal.h"

#include "image/video.h"

/** stream bytes kept in memory around the read position */
#define NSV_WINDOW (16 * 1024 * 1024)
/** bytes kept behind the read position (the demuxer steps back a little) */
#define NSV_KEEP_BEHIND (1024 * 1024)
/** a seek this far past the buffered data waits instead of reconnecting */
#define NSV_SKIP_AHEAD (768 * 1024)
/** bytes that must be buffered ahead before the demuxer is called */
#define NSV_READ_AHEAD (256 * 1024)
/** size of the ffmpeg I/O buffer */
#define NSV_IOBUF (32 * 1024)
/** scheduler period while playing (ms) */
#define NSV_TICK 10
/** height of the control bar (px) */
#define NSV_BAR 28
/** how long the control bar stays after the pointer stops moving (ms) */
#define NSV_BAR_LINGER 2500

enum nsv_state {
	NSV_IDLE,	/**< waiting for a click */
	NSV_OPENING,	/**< transfer started, format not known yet */
	NSV_PLAYING,
	NSV_PAUSED,
	NSV_ENDED,
	NSV_FAILED,
};

struct nsv_pkt {
	AVPacket *pkt;
	struct nsv_pkt *next;
};

struct nsv_queue {
	struct nsv_pkt *head;
	struct nsv_pkt *tail;
	int count;
	int64_t bytes;
};

/** Sound output: an OSS device, or a clock that pretends to be one. */
struct nsv_audio {
	bool open;
	int fd;			/**< OSS device, or -1 for the silent clock */
	int rate;		/**< frames per second the output wants */
	bool paused;
	int null_queued;	/**< silent clock: frames not "played" yet */
	uint64_t null_ms;	/**< silent clock: when null_queued was valid */
	int null_rem;		/**< silent clock: 1/1000 frames carried over */
	FILE *wav;		/**< NSVIDEO_WAV: copy of everything played */
};

typedef struct nsvideo_content {
	struct content base;

	/* what the media element asked for */
	char *url;
	char *poster_url;
	bool autoplay;
	bool loop;
	bool muted;
	double hint_duration;

	enum nsv_state state;
	char error[160];
	bool described;		/**< the element's description has been read */
	bool opened;		/**< shown in a browser window */

	/* stream transfer */
	CURLM *multi;
	CURL *easy;
	bool xfer_paused;	/**< window full, transfer held back */
	bool xfer_done;		/**< reached the end of the file */
	bool xfer_failed;
	int xfer_retries;
	size_t xfer_got;	/**< bytes received since the transfer (re)started */
	long xfer_status;	/**< HTTP status of the current response */
	int64_t xfer_skip;	/**< bytes to discard (server ignored Range) */
	uint8_t *buf;		/**< window over the file */
	size_t buf_len;
	size_t buf_cap;
	int64_t buf_base;	/**< file offset of buf[0] */
	int64_t total;		/**< file size, or -1 */
	int64_t pos;		/**< file offset the demuxer reads next */

	/* demuxer and decoders */
	AVFormatContext *fmt;
	AVIOContext *avio;
	AVStream *vst;
	AVStream *ast;
	AVCodecContext *vdec;
	AVCodecContext *adec;
	AVFrame *vframe;
	AVFrame *aframe;
	AVPacket *pkt;
	struct nsv_queue vq;
	struct nsv_queue aq;
	bool demux_eof;
	bool vdec_flushed, vdec_eof;
	bool adec_flushed, adec_eof;
	struct SwsContext *sws;
	struct SwrContext *swr;

	/* converted sound waiting for room in the device */
	int16_t *pcm;
	int pcm_cap;		/**< frames allocated */
	int pcm_len;		/**< frames converted */
	int pcm_off;		/**< frames already written */
	double pcm_pts_end;	/**< time at the end of the converted frames */
	struct nsv_audio audio;
	bool audio_clock;	/**< the sound device drives the clock */

	/* clock when there is no sound */
	uint64_t wall_ms;
	double wall_pts;

	/* picture */
	struct bitmap *bitmap;
	int bm_w, bm_h;
	int box_w, box_h;	/**< size the page shows the player at */
	bool have_picture;
	bool frame_pending;	/**< vframe holds a decoded picture */
	double frame_pts;
	double frame_period;

	double duration;
	double position;	/**< time shown in the control bar */
	bool seeking;		/**< dropping output until seek_target */
	double seek_target;

	/* keeping up on slow machines */
	int late_run;
	int skip_level;
	uint64_t skip_ms;	/**< when skip_level last changed */
	uint64_t late_ms;	/**< when a picture was last dropped as late */

	/* user interface */
	struct hlcache_handle *poster;
	uint64_t hover_ms;
	bool bar_shown;
	uint64_t bar_ms;	/**< last control bar repaint */

	/* statistics (NSVIDEO_DEBUG) */
	unsigned int shown, dropped;
	uint64_t stat_ms;
} nsvideo_content;

static const content_handler nsvideo_content_handler;

/** only one player makes sound at a time */
static nsvideo_content *nsv_active;

static void nsv_tick(void *p);
static void nsv_pause(nsvideo_content *v, bool pause);

static uint64_t nsv_now(void)
{
	uint64_t ms = 0;

	nsu_getmonotonic_ms(&ms);
	return ms;
}

static bool nsv_debug(void)
{
	static int debug = -1;

	if (debug < 0)
		debug = getenv("NSVIDEO_DEBUG") != NULL;
	return debug;
}

static void nsv_fail(nsvideo_content *v, const char *why)
{
	snprintf(v->error, sizeof(v->error), "%s", why);
	v->state = NSV_FAILED;
	NSLOG(netsurf, INFO, "video: %s", why);
	if (nsv_debug())
		fprintf(stderr, "nsvideo: failed: %s\n", why);
}

static void nsv_request_redraw(nsvideo_content *v)
{
	union content_msg_data data;

	data.redraw.x = 0;
	data.redraw.y = 0;
	data.redraw.width = v->base.width;
	data.redraw.height = v->base.height;
	content_broadcast(&v->base, CONTENT_MSG_REDRAW, &data);
}

/* ---- packet queues ---------------------------------------------------- */

static bool nsv_queue_put(struct nsv_queue *q, AVPacket *src)
{
	struct nsv_pkt *e = malloc(sizeof(*e));

	if (e == NULL)
		return false;
	e->pkt = av_packet_alloc();
	if (e->pkt == NULL) {
		free(e);
		return false;
	}
	av_packet_move_ref(e->pkt, src);
	e->next = NULL;
	if (q->tail != NULL)
		q->tail->next = e;
	else
		q->head = e;
	q->tail = e;
	q->count++;
	q->bytes += e->pkt->size;
	return true;
}

static AVPacket *nsv_queue_get(struct nsv_queue *q)
{
	struct nsv_pkt *e = q->head;
	AVPacket *pkt;

	if (e == NULL)
		return NULL;
	q->head = e->next;
	if (q->head == NULL)
		q->tail = NULL;
	q->count--;
	q->bytes -= e->pkt->size;
	pkt = e->pkt;
	free(e);
	return pkt;
}

static void nsv_queue_clear(struct nsv_queue *q)
{
	AVPacket *pkt;

	while ((pkt = nsv_queue_get(q)) != NULL)
		av_packet_free(&pkt);
}

/* ---- sound output ----------------------------------------------------- */

static void nsv_wav_header(FILE *f, int rate, uint32_t bytes)
{
	uint8_t h[44] = "RIFF....WAVEfmt ";
	uint32_t v;

	v = bytes + 36; memcpy(h + 4, &v, 4);
	v = 16; memcpy(h + 16, &v, 4);
	h[20] = 1; h[21] = 0;			/* PCM */
	h[22] = 2; h[23] = 0;			/* stereo */
	v = rate; memcpy(h + 24, &v, 4);
	v = rate * 4; memcpy(h + 28, &v, 4);
	h[32] = 4; h[33] = 0;
	h[34] = 16; h[35] = 0;
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &bytes, 4);
	fwrite(h, 1, sizeof(h), f);
}

static void nsv_audio_open(struct nsv_audio *a, int rate)
{
	const char *dev = getenv("NSVIDEO_AUDIODEV");
	const char *wav = getenv("NSVIDEO_WAV");

	memset(a, 0, sizeof(*a));
	a->rate = rate;
	a->fd = open(dev != NULL ? dev : "/dev/dsp", O_WRONLY | O_NONBLOCK);
	if (a->fd >= 0) {
		int fmt = AFMT_S16_LE, ch = 2, r = rate;

		if (ioctl(a->fd, SNDCTL_DSP_SETFMT, &fmt) < 0 ||
		    ioctl(a->fd, SNDCTL_DSP_CHANNELS, &ch) < 0 ||
		    ioctl(a->fd, SNDCTL_DSP_SPEED, &r) < 0 ||
		    fmt != AFMT_S16_LE || ch != 2 || r <= 0) {
			close(a->fd);
			a->fd = -1;
		} else {
			a->rate = r;
		}
	}
	if (wav != NULL) {
		a->wav = fopen(wav, "wb");
		if (a->wav != NULL)
			nsv_wav_header(a->wav, a->rate, 0x7ffff000);
	}
	a->null_ms = nsv_now();
	a->open = true;
	if (nsv_debug())
		fprintf(stderr, "nsvideo: audio %s at %d Hz\n",
			a->fd >= 0 ? "device" : "silent clock", a->rate);
}

static void nsv_audio_close(struct nsv_audio *a)
{
	if (!a->open)
		return;
	if (a->fd >= 0) {
		ioctl(a->fd, SNDCTL_DSP_RESET, 0);
		close(a->fd);
	}
	if (a->wav != NULL) {
		long bytes = ftell(a->wav) - 44;

		if (bytes > 0 && fseek(a->wav, 0, SEEK_SET) == 0)
			nsv_wav_header(a->wav, a->rate, (uint32_t)bytes);
		fclose(a->wav);
	}
	a->open = false;
	a->fd = -1;
	a->wav = NULL;
}

/** silent clock: let the frames that would have been played by now go */
static void nsv_audio_null_run(struct nsv_audio *a)
{
	uint64_t now = nsv_now();
	int64_t units = (int64_t)(now - a->null_ms) * a->rate + a->null_rem;

	a->null_ms = now;
	if (a->paused)
		return;
	if (units / 1000 >= a->null_queued) {
		a->null_queued = 0;
		a->null_rem = 0;
	} else {
		a->null_queued -= (int)(units / 1000);
		a->null_rem = (int)(units % 1000);
	}
}

/** frames the output accepts right now */
static int nsv_audio_space(struct nsv_audio *a)
{
	if (a->fd >= 0) {
		audio_buf_info bi;

		if (ioctl(a->fd, SNDCTL_DSP_GETOSPACE, &bi) < 0)
			return 0;
		return bi.bytes / 4;
	}
	nsv_audio_null_run(a);
	return a->rate / 2 - a->null_queued;
}

/** frames written but not played yet */
static int nsv_audio_delay(struct nsv_audio *a)
{
	if (a->fd >= 0) {
		int bytes = 0;

		if (ioctl(a->fd, SNDCTL_DSP_GETODELAY, &bytes) < 0)
			return 0;
		return bytes / 4;
	}
	nsv_audio_null_run(a);
	return a->null_queued;
}

static int nsv_audio_write(struct nsv_audio *a, const int16_t *pcm, int frames)
{
	if (a->fd >= 0) {
		ssize_t n = write(a->fd, pcm, (size_t)frames * 4);

		if (n <= 0)
			return 0;
		frames = n / 4;
	} else {
		nsv_audio_null_run(a);
		a->null_queued += frames;
	}
	if (a->wav != NULL)
		fwrite(pcm, 4, frames, a->wav);
	return frames;
}

static void nsv_audio_pause(struct nsv_audio *a, bool pause)
{
	if (!a->open)
		return;
	if (a->fd >= 0) {
		int trigger = pause ? 0 : PCM_ENABLE_OUTPUT;

		ioctl(a->fd, SNDCTL_DSP_SETTRIGGER, &trigger);
	} else {
		nsv_audio_null_run(a);
	}
	a->paused = pause;
}

/** throw away what has been written but not played */
static void nsv_audio_reset(struct nsv_audio *a)
{
	if (!a->open)
		return;
	if (a->fd >= 0)
		ioctl(a->fd, SNDCTL_DSP_RESET, 0);
	a->null_queued = 0;
	a->null_ms = nsv_now();
}

/* ---- stream transfer -------------------------------------------------- */

/** drop the part of the window the demuxer has moved past */
static void nsv_window_trim(nsvideo_content *v)
{
	int64_t drop = v->pos - NSV_KEEP_BEHIND - v->buf_base;

	if (drop > (int64_t)v->buf_len)
		drop = v->buf_len;
	if (drop <= 0)
		return;
	memmove(v->buf, v->buf + drop, v->buf_len - drop);
	v->buf_len -= drop;
	v->buf_base += drop;
}

static size_t nsv_curl_write(char *ptr, size_t size, size_t nmemb, void *pw)
{
	nsvideo_content *v = pw;
	size_t n = size * nmemb;

	if (v->xfer_status >= 400)
		return n; /* error page, not media */

	if (v->xfer_skip > 0) {
		size_t skip = (int64_t)n < v->xfer_skip ? n : (size_t)v->xfer_skip;

		v->xfer_skip -= skip;
		ptr += skip;
		n -= skip;
		if (n == 0)
			return size * nmemb;
	}

	if (v->buf_len + n > v->buf_cap) {
		if (v->buf_cap < NSV_WINDOW) {
			size_t cap = v->buf_cap ? v->buf_cap * 2 : 1024 * 1024;
			uint8_t *nb;

			while (cap < v->buf_len + n)
				cap *= 2;
			if (cap > NSV_WINDOW)
				cap = NSV_WINDOW;
			nb = realloc(v->buf, cap);
			if (nb != NULL) {
				v->buf = nb;
				v->buf_cap = cap;
			}
		}
		if (v->buf_len + n > v->buf_cap)
			nsv_window_trim(v);
		if (v->buf_len + n > v->buf_cap) {
			if (size * nmemb != n)
				return 0; /* cannot hold back a partial block */
			v->xfer_paused = true;
			return CURL_WRITEFUNC_PAUSE;
		}
	}
	memcpy(v->buf + v->buf_len, ptr, n);
	v->buf_len += n;
	v->xfer_got += n;
	if (v->xfer_got > 1024 * 1024)
		v->xfer_retries = 0; /* the connection is doing fine */
	return size * nmemb;
}

static size_t nsv_curl_header(char *ptr, size_t size, size_t nmemb, void *pw)
{
	nsvideo_content *v = pw;
	size_t n = size * nmemb;
	char line[256];
	long long a, b, t;

	if (n >= sizeof(line))
		return n;
	memcpy(line, ptr, n);
	line[n] = '\0';

	if (strncmp(line, "HTTP/", 5) == 0) {
		const char *sp = strchr(line, ' ');

		v->xfer_status = sp != NULL ? strtol(sp + 1, NULL, 10) : 0;
		if (nsv_debug())
			fprintf(stderr, "nsvideo: HTTP %ld\n", v->xfer_status);
		if (v->xfer_status == 200) {
			/* whole file from the start, whatever was asked for */
			v->xfer_skip = v->buf_base + v->buf_len;
		}
	} else if (strncasecmp(line, "Content-Range:", 14) == 0) {
		if (sscanf(line + 14, " bytes %lld-%lld/%lld", &a, &b, &t) == 3)
			v->total = t;
	} else if (strncasecmp(line, "Content-Length:", 15) == 0) {
		if (v->xfer_status == 200)
			v->total = strtoll(line + 15, NULL, 10);
	}
	return n;
}

static void nsv_xfer_stop(nsvideo_content *v)
{
	if (v->easy != NULL) {
		curl_multi_remove_handle(v->multi, v->easy);
		curl_easy_cleanup(v->easy);
		v->easy = NULL;
	}
	v->xfer_paused = false;
}

/**
 * Start fetching the file from \a offset.
 *
 * \param keep the window already ends at \a offset: add to it
 */
static bool nsv_xfer_start(nsvideo_content *v, int64_t offset, bool keep)
{
	char range[48];

	nsv_xfer_stop(v);
	if (v->multi == NULL)
		v->multi = curl_multi_init();
	v->easy = curl_easy_init();
	if (v->multi == NULL || v->easy == NULL) {
		v->xfer_failed = true;
		return false;
	}
	if (!keep) {
		v->buf_base = offset;
		v->buf_len = 0;
	}
	v->xfer_done = false;
	v->xfer_failed = false;
	v->xfer_status = 0;
	v->xfer_skip = 0;
	v->xfer_got = 0;

	snprintf(range, sizeof(range), "%lld-", (long long)offset);
	curl_easy_setopt(v->easy, CURLOPT_URL, v->url);
	curl_easy_setopt(v->easy, CURLOPT_RANGE, range);
	curl_easy_setopt(v->easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(v->easy, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(v->easy, CURLOPT_NOSIGNAL, 1L);
	/* a connection that stalls is dropped and picked up again where the
	 * window ends (a transfer held back by a full window is not stalled) */
	curl_easy_setopt(v->easy, CURLOPT_CONNECTTIMEOUT, 12L);
	curl_easy_setopt(v->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(v->easy, CURLOPT_LOW_SPEED_TIME, 12L);
	curl_easy_setopt(v->easy, CURLOPT_BUFFERSIZE, 128L * 1024);
	curl_easy_setopt(v->easy, CURLOPT_USERAGENT, user_agent_string());
	curl_easy_setopt(v->easy, CURLOPT_WRITEFUNCTION, nsv_curl_write);
	curl_easy_setopt(v->easy, CURLOPT_WRITEDATA, v);
	curl_easy_setopt(v->easy, CURLOPT_HEADERFUNCTION, nsv_curl_header);
	curl_easy_setopt(v->easy, CURLOPT_HEADERDATA, v);
	if (nsoption_charp(ca_bundle) != NULL)
		curl_easy_setopt(v->easy, CURLOPT_CAINFO,
				nsoption_charp(ca_bundle));
	if (nsoption_charp(ca_path) != NULL)
		curl_easy_setopt(v->easy, CURLOPT_CAPATH,
				nsoption_charp(ca_path));
	curl_multi_add_handle(v->multi, v->easy);
	if (nsv_debug())
		fprintf(stderr, "nsvideo: fetching %.60s... from byte %lld\n",
			v->url, (long long)offset);
	return true;
}

/**
 * Move bytes from the network into the window.
 *
 * \param wait_ms how long to wait for the network when nothing is ready
 */
static void nsv_xfer_pump(nsvideo_content *v, int wait_ms)
{
	CURLMsg *msg;
	int running, left;

	if (v->easy == NULL) {
		if (wait_ms > 0)
			usleep(wait_ms * 1000);
		return;
	}
	if (v->xfer_paused) {
		nsv_window_trim(v);
		if (v->buf_cap - v->buf_len >= 256 * 1024) {
			v->xfer_paused = false;
			curl_easy_pause(v->easy, CURLPAUSE_CONT);
		} else if (wait_ms > 0) {
			usleep(wait_ms * 1000);
			return;
		}
	}
	if (wait_ms > 0)
		curl_multi_poll(v->multi, NULL, 0, wait_ms, NULL);
	curl_multi_perform(v->multi, &running);

	while ((msg = curl_multi_info_read(v->multi, &left)) != NULL) {
		CURLcode res;
		long code = 0;

		if (msg->msg != CURLMSG_DONE || msg->easy_handle != v->easy)
			continue;
		res = msg->data.result;
		curl_easy_getinfo(v->easy, CURLINFO_RESPONSE_CODE, &code);
		nsv_xfer_stop(v);
		if (nsv_debug())
			fprintf(stderr, "nsvideo: transfer ended: %s (HTTP %ld), "
				"window %lld+%zu of %lld\n", curl_easy_strerror(res),
				code, (long long)v->buf_base, v->buf_len,
				(long long)v->total);

		if (code == 416 || (res == CURLE_OK && code < 400)) {
			/* 416: asked for bytes past the end */
			v->xfer_done = true;
			if (v->total < 0)
				v->total = v->buf_base + v->buf_len;
		} else if (code >= 400 || v->xfer_retries >=
				(v->state == NSV_OPENING && v->buf_len == 0 ? 2 : 6)) {
			/* refused, or it keeps failing (sooner when the server
			 * has not been reached even once) */
			v->xfer_failed = true;
			if (v->error[0] == '\0') {
				if (code >= 400)
					snprintf(v->error, sizeof(v->error),
						"The server refused the stream (HTTP %ld)", code);
				else
					snprintf(v->error, sizeof(v->error),
						"Cannot load the video: %s",
						curl_easy_strerror(res));
			}
		} else {
			/* connection lost: carry on where the window ends */
			v->xfer_retries++;
			nsv_xfer_start(v, v->buf_base + v->buf_len, true);
		}
		break;
	}
}

static int64_t nsv_buffered_ahead(nsvideo_content *v)
{
	return v->buf_base + (int64_t)v->buf_len - v->pos;
}

/** can the demuxer read a packet without waiting for the network? */
static bool nsv_data_ready(nsvideo_content *v)
{
	if (v->xfer_done || v->xfer_failed)
		return true;
	if (v->total > 0 && v->pos >= v->total)
		return true;
	return nsv_buffered_ahead(v) >= NSV_READ_AHEAD;
}

/* ffmpeg I/O: reads wait for the network (they are short: see above) */
static int nsv_io_read(void *opaque, uint8_t *out, int size)
{
	nsvideo_content *v = opaque;
	uint64_t start = nsv_now();

	for (;;) {
		int64_t end = v->buf_base + (int64_t)v->buf_len;

		if (v->pos >= v->buf_base && v->pos < end) {
			int64_t n = end - v->pos;

			if (n > size)
				n = size;
			memcpy(out, v->buf + (v->pos - v->buf_base), n);
			v->pos += n;
			return (int)n;
		}
		if (v->total > 0 && v->pos >= v->total)
			return AVERROR_EOF;
		if (v->xfer_failed)
			return AVERROR(EIO);
		if (v->xfer_done)
			return AVERROR_EOF;
		if (v->pos < v->buf_base || v->easy == NULL)
			nsv_xfer_start(v, v->pos, false);
		if (nsv_now() - start > 30000)
			return AVERROR(ETIMEDOUT);
		nsv_xfer_pump(v, 40);
	}
}

static int64_t nsv_io_seek(void *opaque, int64_t offset, int whence)
{
	nsvideo_content *v = opaque;
	int64_t target, end;

	if (whence & AVSEEK_SIZE)
		return v->total > 0 ? v->total : AVERROR(ENOSYS);
	switch (whence & ~AVSEEK_FORCE) {
	case SEEK_SET:
		target = offset;
		break;
	case SEEK_CUR:
		target = v->pos + offset;
		break;
	case SEEK_END:
		if (v->total <= 0)
			return AVERROR(ENOSYS);
		target = v->total + offset;
		break;
	default:
		return AVERROR(EINVAL);
	}
	if (target < 0)
		return AVERROR(EINVAL);

	end = v->buf_base + (int64_t)v->buf_len;
	if (target >= v->buf_base && target <= end) {
		v->pos = target;
	} else if (target > end && target <= end + NSV_SKIP_AHEAD &&
			v->easy != NULL) {
		v->pos = target; /* about to arrive */
	} else if (v->total > 0 && target >= v->total) {
		v->pos = target; /* reads report the end of the file */
	} else {
		v->pos = target;
		nsv_xfer_start(v, target, false);
	}
	return target;
}

/* ---- demuxer and decoders --------------------------------------------- */

static void nsv_close_media(nsvideo_content *v)
{
	nsv_queue_clear(&v->vq);
	nsv_queue_clear(&v->aq);
	if (v->vdec != NULL)
		avcodec_free_context(&v->vdec);
	if (v->adec != NULL)
		avcodec_free_context(&v->adec);
	if (v->fmt != NULL)
		avformat_close_input(&v->fmt);
	if (v->avio != NULL) {
		av_freep(&v->avio->buffer);
		avio_context_free(&v->avio);
	}
	if (v->sws != NULL) {
		sws_freeContext(v->sws);
		v->sws = NULL;
	}
	if (v->swr != NULL)
		swr_free(&v->swr);
	av_frame_free(&v->vframe);
	av_frame_free(&v->aframe);
	av_packet_free(&v->pkt);
	nsv_audio_close(&v->audio);
	v->vst = NULL;
	v->ast = NULL;
	v->frame_pending = false;
	v->pcm_len = v->pcm_off = 0;
}

static AVCodecContext *nsv_open_decoder(AVStream *st, const AVCodec *codec)
{
	AVCodecContext *ctx;

	if (codec == NULL)
		codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (codec == NULL)
		return NULL;
	ctx = avcodec_alloc_context3(codec);
	if (ctx == NULL)
		return NULL;
	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	ctx->pkt_timebase = st->time_base;
	ctx->thread_count = 1;
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	return ctx;
}

static bool nsv_open_media(nsvideo_content *v)
{
	const AVCodec *vcodec = NULL, *acodec = NULL;
	uint8_t *iobuf;
	unsigned int i;
	int vi, ai;

	iobuf = av_malloc(NSV_IOBUF);
	if (iobuf == NULL)
		return false;
	v->avio = avio_alloc_context(iobuf, NSV_IOBUF, 0, v,
			nsv_io_read, NULL, nsv_io_seek);
	v->fmt = avformat_alloc_context();
	v->vframe = av_frame_alloc();
	v->aframe = av_frame_alloc();
	v->pkt = av_packet_alloc();
	if (v->avio == NULL || v->fmt == NULL || v->vframe == NULL ||
			v->aframe == NULL || v->pkt == NULL) {
		if (v->avio == NULL)
			av_free(iobuf);
		nsv_fail(v, "Out of memory");
		return false;
	}
	v->fmt->pb = v->avio;
	v->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	v->fmt->probesize = 256 * 1024;
	v->fmt->max_analyze_duration = AV_TIME_BASE;

	if (avformat_open_input(&v->fmt, NULL, NULL, NULL) < 0) {
		nsv_fail(v, v->error[0] != '\0' ? v->error :
				"This media format is not supported");
		return false;
	}
	if (avformat_find_stream_info(v->fmt, NULL) < 0) {
		nsv_fail(v, "Cannot read the media streams");
		return false;
	}

	vi = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
	ai = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, vi, &acodec, 0);
	if (vi >= 0) {
		v->vdec = nsv_open_decoder(v->fmt->streams[vi], vcodec);
		if (v->vdec != NULL)
			v->vst = v->fmt->streams[vi];
	}
	if (ai >= 0) {
		v->adec = nsv_open_decoder(v->fmt->streams[ai], acodec);
		if (v->adec != NULL)
			v->ast = v->fmt->streams[ai];
	}
	if (v->vst == NULL && v->ast == NULL) {
		nsv_fail(v, "No decoder for this video (H.264, VP8/VP9, "
				"AAC, Opus, Vorbis and MP3 are supported)");
		return false;
	}
	for (i = 0; i < v->fmt->nb_streams; i++) {
		if (v->fmt->streams[i] != v->vst && v->fmt->streams[i] != v->ast)
			v->fmt->streams[i]->discard = AVDISCARD_ALL;
	}

	if (v->fmt->duration > 0)
		v->duration = (double)v->fmt->duration / AV_TIME_BASE;
	else
		v->duration = v->hint_duration;

	v->frame_period = 1.0 / 25;
	if (v->vst != NULL) {
		AVRational fr = av_guess_frame_rate(v->fmt, v->vst, NULL);

		if (fr.num > 0 && fr.den > 0)
			v->frame_period = av_q2d(av_inv_q(fr));
	}

	v->audio_clock = false;
	if (v->adec != NULL && v->adec->sample_rate > 0) {
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

		nsv_audio_open(&v->audio, v->adec->sample_rate);
		if (swr_alloc_set_opts2(&v->swr, &stereo, AV_SAMPLE_FMT_S16,
				v->audio.rate, &v->adec->ch_layout,
				v->adec->sample_fmt, v->adec->sample_rate,
				0, NULL) < 0 || swr_init(v->swr) < 0) {
			swr_free(&v->swr);
			nsv_audio_close(&v->audio);
			avcodec_free_context(&v->adec);
			v->ast = NULL;
		} else {
			v->audio_clock = true;
		}
	}

	if (nsv_debug()) {
		fprintf(stderr, "nsvideo: %s, %.1f s, video %s %dx%d %.2f fps, audio %s %d Hz\n",
			v->fmt->iformat->name, v->duration,
			v->vdec ? avcodec_get_name(v->vdec->codec_id) : "-",
			v->vdec ? v->vdec->width : 0, v->vdec ? v->vdec->height : 0,
			1.0 / v->frame_period,
			v->adec ? avcodec_get_name(v->adec->codec_id) : "-",
			v->adec ? v->adec->sample_rate : 0);
	}
	return true;
}

static void nsv_demux(nsvideo_content *v)
{
	int n;

	for (n = 0; n < 32 && !v->demux_eof; n++) {
		bool want_v = v->vst != NULL && v->vq.count < 24;
		bool want_a = v->ast != NULL && v->aq.count < 48;
		int ret;

		if (!want_v && !want_a)
			break;
		if (v->vq.bytes + v->aq.bytes > 12 * 1024 * 1024 ||
				v->vq.count + v->aq.count > 1200)
			break;
		if (!nsv_data_ready(v))
			break;

		ret = av_read_frame(v->fmt, v->pkt);
		if (ret < 0) {
			if (ret != AVERROR(EAGAIN))
				v->demux_eof = true;
			break;
		}
		if (v->vst != NULL && v->pkt->stream_index == v->vst->index)
			nsv_queue_put(&v->vq, v->pkt);
		else if (v->ast != NULL && v->pkt->stream_index == v->ast->index)
			nsv_queue_put(&v->aq, v->pkt);
		av_packet_unref(v->pkt);
	}
}

static double nsv_frame_time(AVFrame *f, AVStream *st, double fallback)
{
	int64_t ts = f->best_effort_timestamp;

	if (ts == AV_NOPTS_VALUE)
		ts = f->pts;
	if (ts == AV_NOPTS_VALUE)
		return fallback;
	if (st->start_time != AV_NOPTS_VALUE)
		ts -= st->start_time;
	return ts * av_q2d(st->time_base);
}

/** convert the decoded sound in aframe for the device */
static void nsv_convert_audio(nsvideo_content *v)
{
	AVFrame *f = v->aframe;
	double pts = nsv_frame_time(f, v->ast, v->pcm_pts_end);
	uint8_t *out;
	int max, got;

	max = swr_get_out_samples(v->swr, f->nb_samples);
	if (max > v->pcm_cap) {
		int16_t *n = realloc(v->pcm, (size_t)max * 4);

		if (n == NULL)
			return;
		v->pcm = n;
		v->pcm_cap = max;
	}
	out = (uint8_t *)v->pcm;
	got = swr_convert(v->swr, &out, max,
			(const uint8_t **)f->extended_data, f->nb_samples);
	if (got <= 0)
		return;
	if (v->muted)
		memset(v->pcm, 0, (size_t)got * 4);

	v->pcm_len = got;
	v->pcm_off = 0;
	v->pcm_pts_end = pts + (double)f->nb_samples / f->sample_rate;
	if (v->seeking && v->pcm_pts_end < v->seek_target)
		v->pcm_off = v->pcm_len; /* before the seek point */
}

/** keep the sound device supplied */
static void nsv_audio_step(nsvideo_content *v)
{
	int guard;

	if (v->adec == NULL)
		return;
	for (guard = 0; guard < 48; guard++) {
		AVPacket *pkt;
		int ret;

		if (v->pcm_off < v->pcm_len) {
			int space = nsv_audio_space(&v->audio);
			int n = v->pcm_len - v->pcm_off;

			if (v->seeking && v->vdec != NULL)
				return; /* starts with the picture at the seek point */
			if (space <= 0)
				return;
			if (n > space)
				n = space;
			n = nsv_audio_write(&v->audio,
					v->pcm + (size_t)v->pcm_off * 2, n);
			if (n <= 0)
				return;
			v->pcm_off += n;
			continue;
		}

		ret = avcodec_receive_frame(v->adec, v->aframe);
		if (ret == 0) {
			nsv_convert_audio(v);
			av_frame_unref(v->aframe);
			continue;
		}
		if (ret == AVERROR_EOF) {
			v->adec_eof = true;
			return;
		}

		pkt = nsv_queue_get(&v->aq);
		if (pkt == NULL) {
			if (!v->demux_eof || v->adec_flushed)
				return;
			avcodec_send_packet(v->adec, NULL);
			v->adec_flushed = true;
			continue;
		}
		avcodec_send_packet(v->adec, pkt);
		av_packet_free(&pkt);
	}
}

/** the time the picture on screen should belong to */
static double nsv_clock(nsvideo_content *v)
{
	if (v->audio_clock) {
		double end = v->pcm_pts_end -
			(double)(v->pcm_len - v->pcm_off) / v->audio.rate;

		if (v->adec_eof && v->pcm_off >= v->pcm_len &&
				nsv_audio_delay(&v->audio) == 0) {
			/* the sound ended first: carry on by the wall clock */
			v->audio_clock = false;
			v->wall_pts = end;
			v->wall_ms = nsv_now();
			return end;
		}
		return end - (double)nsv_audio_delay(&v->audio) / v->audio.rate;
	}
	if (v->state != NSV_PLAYING)
		return v->wall_pts;
	return v->wall_pts + (double)(nsv_now() - v->wall_ms) / 1000.0;
}

/** decode until vframe holds the next picture */
static bool nsv_decode_video(nsvideo_content *v)
{
	int guard;

	for (guard = 0; guard < 12; guard++) {
		AVPacket *pkt;
		int ret = avcodec_receive_frame(v->vdec, v->vframe);

		if (ret == 0) {
			v->frame_pts = nsv_frame_time(v->vframe, v->vst,
					v->frame_pts + v->frame_period);
			v->frame_pending = true;
			return true;
		}
		if (ret == AVERROR_EOF) {
			v->vdec_eof = true;
			return false;
		}
		pkt = nsv_queue_get(&v->vq);
		if (pkt == NULL) {
			if (!v->demux_eof || v->vdec_flushed)
				return false;
			avcodec_send_packet(v->vdec, NULL);
			v->vdec_flushed = true;
			continue;
		}
		avcodec_send_packet(v->vdec, pkt);
		av_packet_free(&pkt);
	}
	return false;
}

/** the picture area inside the box, keeping the aspect ratio */
static void nsv_picture_rect(nsvideo_content *v, int box_w, int box_h,
		int *x, int *y, int *w, int *h)
{
	int vw = v->base.width, vh = v->base.height;

	if (v->vdec != NULL && v->vdec->width > 0 && v->vdec->height > 0) {
		AVRational sar = v->vdec->sample_aspect_ratio;

		vw = v->vdec->width;
		vh = v->vdec->height;
		if (sar.num > 0 && sar.den > 0)
			vw = (int)((int64_t)vw * sar.num / sar.den);
	}
	if (vw <= 0 || vh <= 0) {
		vw = 16;
		vh = 9;
	}
	*w = box_w;
	*h = (int)((int64_t)box_w * vh / vw);
	if (*h > box_h) {
		*h = box_h;
		*w = (int)((int64_t)box_h * vw / vh);
	}
	if (*w < 2)
		*w = 2;
	if (*h < 2)
		*h = 2;
	*x = (box_w - *w) / 2;
	*y = (box_h - *h) / 2;
}

/** put the picture in vframe on screen */
static void nsv_show_frame(nsvideo_content *v)
{
	AVFrame *f = v->vframe;
	int x, y, w, h, stride;
	uint8_t *dst[4] = { NULL, NULL, NULL, NULL };
	int dst_stride[4] = { 0, 0, 0, 0 };

	nsv_picture_rect(v, v->box_w, v->box_h, &x, &y, &w, &h);
	if (v->bitmap == NULL || v->bm_w != w || v->bm_h != h) {
		if (v->bitmap != NULL)
			guit->bitmap->destroy(v->bitmap);
		v->bitmap = guit->bitmap->create(w, h, BITMAP_OPAQUE);
		v->bm_w = w;
		v->bm_h = h;
		v->have_picture = false;
		if (v->bitmap == NULL)
			return;
	}
	v->sws = sws_getCachedContext(v->sws, f->width, f->height, f->format,
			w, h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR,
			NULL, NULL, NULL);
	if (v->sws == NULL)
		return;

	stride = (int)guit->bitmap->get_rowstride(v->bitmap);
	dst[0] = guit->bitmap->get_buffer(v->bitmap);
	dst_stride[0] = stride;
	if (dst[0] == NULL)
		return;
	sws_scale(v->sws, (const uint8_t * const *)f->data, f->linesize,
			0, f->height, dst, dst_stride);
	guit->bitmap->modified(v->bitmap);
	v->have_picture = true;
	v->position = v->frame_pts;
	nsv_request_redraw(v);
}

/**
 * How much work the decoder leaves out: 0 nothing, 1 the deblocking filter,
 * 2 also pictures nothing else depends on, 3 everything but key pictures.
 */
static void nsv_set_skip_level(nsvideo_content *v, int level)
{
	v->skip_level = level;
	v->skip_ms = nsv_now();
	v->vdec->skip_loop_filter = level >= 1 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
	v->vdec->skip_frame = level >= 3 ? AVDISCARD_NONKEY :
			(level == 2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT);
	if (nsv_debug())
		fprintf(stderr, "nsvideo: skip level %d\n", level);
}

/** pictures keep arriving late: make the decoder do less */
static void nsv_fall_behind(nsvideo_content *v)
{
	uint64_t now = nsv_now();

	v->late_ms = now;
	if (++v->late_run < 12 || now - v->skip_ms < 4000 || v->skip_level >= 3)
		return;
	v->late_run = 0;
	nsv_set_skip_level(v, v->skip_level + 1);
}

/** show the next picture when its time has come */
static int nsv_video_step(nsvideo_content *v, double clock)
{
	int guard;

	if (v->vdec == NULL)
		return NSV_TICK;
	for (guard = 0; guard < (v->seeking ? 24 : 6); guard++) {
		double late;

		if (!v->frame_pending && !nsv_decode_video(v))
			return NSV_TICK;

		if (v->seeking) {
			if (v->frame_pts < v->seek_target - 0.001) {
				v->frame_pending = false; /* before the seek point */
				av_frame_unref(v->vframe);
				continue;
			}
			/* the picture at the seek point: the sound starts with it */
			nsv_show_frame(v);
			v->frame_pending = false;
			av_frame_unref(v->vframe);
			v->seeking = false;
			v->late_run = 0;
			return 1;
		}
		late = clock - v->frame_pts;
		if (late < -0.003) {
			int wait = (int)(-late * 1000);

			return wait < NSV_TICK ? wait : NSV_TICK;
		}
		if (late > 0.15 && v->vq.count > 0) {
			v->dropped++;
			v->frame_pending = false;
			av_frame_unref(v->vframe);
			nsv_fall_behind(v);
			continue;
		}
		if (late < 0.05)
			v->late_run = 0;
		nsv_show_frame(v);
		v->shown++;
		v->frame_pending = false;
		av_frame_unref(v->vframe);
		return 1;
	}
	return 1;
}

static bool nsv_finished(nsvideo_content *v)
{
	if (!v->demux_eof || v->vq.count > 0 || v->aq.count > 0)
		return false;
	if (v->vdec != NULL && (!v->vdec_eof || v->frame_pending))
		return false;
	if (v->adec != NULL) {
		if (!v->adec_eof || v->pcm_off < v->pcm_len)
			return false;
		if (v->audio_clock && nsv_audio_delay(&v->audio) > 0)
			return false;
	}
	return true;
}

/** restart the decoders at \a t seconds */
static void nsv_seek(nsvideo_content *v, double t)
{
	int64_t ts;

	if (v->fmt == NULL)
		return;
	if (v->duration > 0 && t > v->duration - 0.5)
		t = v->duration - 0.5;
	if (t < 0)
		t = 0;
	ts = (int64_t)(t * AV_TIME_BASE);
	if (v->fmt->start_time != AV_NOPTS_VALUE)
		ts += v->fmt->start_time;
	if (av_seek_frame(v->fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
		return;

	nsv_queue_clear(&v->vq);
	nsv_queue_clear(&v->aq);
	if (v->vdec != NULL)
		avcodec_flush_buffers(v->vdec);
	if (v->adec != NULL) {
		avcodec_flush_buffers(v->adec);
		nsv_audio_reset(&v->audio);
		v->audio_clock = true;
	}
	av_frame_unref(v->vframe);
	v->frame_pending = false;
	v->demux_eof = false;
	v->vdec_flushed = v->vdec_eof = false;
	v->adec_flushed = v->adec_eof = false;
	v->pcm_len = v->pcm_off = 0;
	v->pcm_pts_end = t;
	v->frame_pts = t;
	v->wall_pts = t;
	v->wall_ms = nsv_now();
	v->seeking = true;
	v->seek_target = t;
	v->position = t;
	v->late_run = 0;
	if (v->state == NSV_ENDED)
		v->state = NSV_PLAYING;
}

/** release everything a playing video holds; the player returns to idle */
static void nsv_stop(nsvideo_content *v)
{
	guit->misc->schedule(-1, nsv_tick, v);
	nsv_xfer_stop(v);
	nsv_close_media(v);
	free(v->buf);
	v->buf = NULL;
	v->buf_len = v->buf_cap = 0;
	v->buf_base = 0;
	v->pos = 0;
	v->total = -1;
	v->demux_eof = false;
	v->vdec_flushed = v->vdec_eof = false;
	v->adec_flushed = v->adec_eof = false;
	v->seeking = false;
	v->skip_level = 0;
	v->late_run = 0;
	v->state = NSV_IDLE;
	if (nsv_active == v)
		nsv_active = NULL;
}

static void nsv_play(nsvideo_content *v)
{
	if (v->url == NULL)
		return;
	if (nsv_active != NULL && nsv_active != v)
		nsv_pause(nsv_active, true);
	nsv_active = v;

	nsv_stop(v);
	nsv_active = v;
	v->error[0] = '\0';
	v->xfer_retries = 0;
	v->position = 0;
	v->pcm_pts_end = 0;
	v->frame_pts = 0;
	v->wall_pts = 0;
	v->shown = v->dropped = 0;
	v->stat_ms = nsv_now();
	if (!nsv_xfer_start(v, 0, false)) {
		nsv_fail(v, "Cannot start the network transfer");
		nsv_request_redraw(v);
		return;
	}
	v->state = NSV_OPENING;
	nsv_request_redraw(v);
	guit->misc->schedule(NSV_TICK, nsv_tick, v);
}

static void nsv_pause(nsvideo_content *v, bool pause)
{
	if (pause && v->state == NSV_PLAYING) {
		v->wall_pts = nsv_clock(v);
		v->state = NSV_PAUSED;
		nsv_audio_pause(&v->audio, true);
	} else if (!pause && v->state == NSV_PAUSED) {
		if (nsv_active != NULL && nsv_active != v)
			nsv_pause(nsv_active, true);
		nsv_active = v;
		v->wall_ms = nsv_now();
		v->state = NSV_PLAYING;
		nsv_audio_pause(&v->audio, false);
	}
	nsv_request_redraw(v);
}

static void nsv_tick(void *p)
{
	nsvideo_content *v = p;
	uint64_t now = nsv_now();
	int next = NSV_TICK;

	nsv_xfer_pump(v, 0);

	switch (v->state) {
	case NSV_OPENING:
		if (v->xfer_failed) {
			nsv_fail(v, v->error[0] != '\0' ? v->error :
					"Cannot load the video");
			nsv_request_redraw(v);
			return;
		}
		if (!v->xfer_done && v->buf_len < 192 * 1024)
			break;
		if (!nsv_open_media(v)) {
			nsv_xfer_stop(v);
			nsv_request_redraw(v);
			return;
		}
		v->state = NSV_PLAYING;
		v->wall_ms = nsv_now();
		nsv_request_redraw(v);
		break;

	case NSV_PLAYING:
		nsv_demux(v);
		nsv_audio_step(v);
		next = nsv_video_step(v, nsv_clock(v));
		if (v->seeking && (v->vdec == NULL ? nsv_clock(v) >= v->seek_target
				: v->vdec_eof))
			v->seeking = false; /* no picture to wait for */
		if (v->vdec == NULL)
			v->position = nsv_clock(v);
		if (v->skip_level > 0 && now - v->late_ms > 45000 &&
				now - v->skip_ms > 45000)
			nsv_set_skip_level(v, v->skip_level - 1); /* keeping up again */

		if (nsv_finished(v)) {
			if (v->loop) {
				nsv_seek(v, 0);
				v->seeking = false;
			} else {
				v->state = NSV_ENDED;
				nsv_audio_pause(&v->audio, true);
				nsv_request_redraw(v);
				next = 100;
			}
		} else if (v->xfer_failed && v->demux_eof &&
				v->vq.count == 0 && v->aq.count == 0) {
			nsv_fail(v, v->error[0] != '\0' ? v->error :
					"The connection was lost");
			nsv_request_redraw(v);
			return;
		}
		break;

	case NSV_PAUSED:
	case NSV_ENDED:
		nsv_demux(v); /* keep buffering */
		next = 50;
		break;

	default:
		return;
	}

	/* control bar: follows playback while shown, hides after a while */
	if (v->bar_shown && now - v->bar_ms >= 500) {
		v->bar_ms = now;
		nsv_request_redraw(v);
	}

	if (nsv_debug() && now - v->stat_ms >= 1000) {
		fprintf(stderr, "nsvideo: t=%.2f clock=%.2f shown=%u dropped=%u "
			"vq=%d aq=%d buf=%lldk ahead=%lldk skip=%d%s\n",
			v->position, nsv_clock(v), v->shown, v->dropped,
			v->vq.count, v->aq.count, (long long)v->buf_len / 1024,
			(long long)nsv_buffered_ahead(v) / 1024, v->skip_level,
			v->state == NSV_PAUSED ? " paused" : "");
		v->stat_ms = now;
	}

	guit->misc->schedule(next, nsv_tick, v);
}

/* ---- drawing ---------------------------------------------------------- */

static void nsv_fill(const struct redraw_context *ctx, int x0, int y0,
		int x1, int y1, colour c)
{
	plot_style_t style = {
		.stroke_type = PLOT_OP_TYPE_NONE,
		.fill_type = PLOT_OP_TYPE_SOLID,
		.fill_colour = c,
	};
	struct rect r = { .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };

	ctx->plot->rectangle(ctx, &style, &r);
}

static void nsv_text(const struct redraw_context *ctx, int x, int y,
		const char *text, colour fg, colour bg, int size)
{
	plot_font_style_t fstyle = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = size * PLOT_STYLE_SCALE,
		.weight = 400,
		.flags = FONTF_NONE,
		.background = bg,
		.foreground = fg,
	};

	ctx->plot->text(ctx, &fstyle, x, y, text, strlen(text));
}

static void nsv_triangle(const struct redraw_context *ctx, int x, int y,
		int size, colour c)
{
	plot_style_t style = {
		.stroke_type = PLOT_OP_TYPE_NONE,
		.fill_type = PLOT_OP_TYPE_SOLID,
		.fill_colour = c,
	};
	int p[6] = { x, y, x, y + size, x + size * 7 / 8, y + size / 2 };

	ctx->plot->polygon(ctx, &style, p, 3);
}

static void nsv_format_time(char *out, size_t size, double t)
{
	int s = t > 0 ? (int)t : 0;

	if (s >= 3600)
		snprintf(out, size, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	else
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

/** horizontal extent of the seek bar inside a box \a w wide */
static void nsv_seek_bar(int w, int *x0, int *x1)
{
	*x0 = 150;
	*x1 = w - 14;
	if (*x1 < *x0 + 20)
		*x1 = *x0 + 20;
}

static bool nsv_bar_wanted(nsvideo_content *v)
{
	if (v->state == NSV_PAUSED || v->state == NSV_ENDED)
		return true;
	if (v->state != NSV_PLAYING)
		return false;
	return nsv_now() - v->hover_ms < NSV_BAR_LINGER;
}

static bool nsvideo_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	nsvideo_content *v = (nsvideo_content *)c;
	int x = data->x, y = data->y, w = data->width, h = data->height;
	int px, py, pw, ph;
	const char *msg = NULL;

	if (w <= 0 || h <= 0)
		return true;
	v->box_w = w;
	v->box_h = h;

	nsv_fill(ctx, x, y, x + w, y + h, 0x000000);

	nsv_picture_rect(v, w, h, &px, &py, &pw, &ph);
	if (v->have_picture && v->bitmap != NULL && v->state != NSV_IDLE) {
		ctx->plot->bitmap(ctx, v->bitmap, x + px, y + py, pw, ph,
				0x000000, BITMAPF_NONE);
	} else if (v->poster != NULL &&
			content_get_status(v->poster) == CONTENT_STATUS_DONE) {
		struct content_redraw_data pd = *data;

		pd.repeat_x = false;
		pd.repeat_y = false;
		content_redraw(v->poster, &pd, clip, ctx);
	}

	switch (v->state) {
	case NSV_IDLE:
		nsv_fill(ctx, x + w / 2 - 36, y + h / 2 - 26,
				x + w / 2 + 36, y + h / 2 + 26, 0x0000e0);
		nsv_triangle(ctx, x + w / 2 - 10, y + h / 2 - 14, 28, 0xffffff);
		break;
	case NSV_OPENING:
		msg = "Loading...";
		break;
	case NSV_FAILED:
		msg = v->error[0] != '\0' ? v->error : "Cannot play this video";
		break;
	case NSV_ENDED:
		nsv_triangle(ctx, x + w / 2 - 10, y + h / 2 - 14, 28, 0xffffff);
		break;
	default:
		break;
	}
	if (msg != NULL) {
		int tw = (int)strlen(msg) * 7;

		nsv_fill(ctx, x + w / 2 - tw / 2 - 12, y + h / 2 - 16,
				x + w / 2 + tw / 2 + 12, y + h / 2 + 12, 0x202020);
		nsv_text(ctx, x + w / 2 - tw / 2, y + h / 2 + 3, msg,
				0xffffff, 0x202020, 11);
	}

	v->bar_shown = nsv_bar_wanted(v);
	if (v->bar_shown && h > NSV_BAR * 2) {
		int by = y + h - NSV_BAR;
		int sx0, sx1, knob;
		char now_s[16], dur_s[16], label[40];

		nsv_fill(ctx, x, by, x + w, y + h, 0x181818);
		if (v->state == NSV_PLAYING) {
			nsv_fill(ctx, x + 14, by + 8, x + 18, by + 20, 0xffffff);
			nsv_fill(ctx, x + 22, by + 8, x + 26, by + 20, 0xffffff);
		} else {
			nsv_triangle(ctx, x + 14, by + 7, 14, 0xffffff);
		}
		nsv_format_time(now_s, sizeof(now_s), v->position);
		nsv_format_time(dur_s, sizeof(dur_s), v->duration);
		snprintf(label, sizeof(label), "%s / %s", now_s, dur_s);
		nsv_text(ctx, x + 40, by + 19, label, 0xffffff, 0x181818, 10);

		nsv_seek_bar(w, &sx0, &sx1);
		nsv_fill(ctx, x + sx0, by + 12, x + sx1, by + 16, 0x606060);
		if (v->duration > 0) {
			double f = v->position / v->duration;

			if (f > 1)
				f = 1;
			if (f < 0)
				f = 0;
			knob = sx0 + (int)((sx1 - sx0) * f);
			nsv_fill(ctx, x + sx0, by + 12, x + knob, by + 16, 0x0000ff);
			nsv_fill(ctx, x + knob - 3, by + 8, x + knob + 3, by + 20,
					0x0000ff);
		}
	}
	return true;
}

/* ---- mouse ------------------------------------------------------------ */

static nserror nsvideo_mouse_track(struct content *c, struct browser_window *bw,
		browser_mouse_state mouse, int x, int y)
{
	nsvideo_content *v = (nsvideo_content *)c;

	v->hover_ms = nsv_now();
	if (v->state == NSV_PLAYING && !v->bar_shown) {
		v->bar_shown = true;
		v->bar_ms = v->hover_ms;
		nsv_request_redraw(v);
	}
	return NSERROR_OK;
}

static nserror nsvideo_mouse_action(struct content *c, struct browser_window *bw,
		browser_mouse_state mouse, int x, int y)
{
	nsvideo_content *v = (nsvideo_content *)c;
	int sx0, sx1;

	if (!(mouse & BROWSER_MOUSE_CLICK_1))
		return NSERROR_OK;
	v->hover_ms = nsv_now();

	switch (v->state) {
	case NSV_IDLE:
	case NSV_FAILED:
		nsv_play(v);
		break;
	case NSV_OPENING:
		break;
	default:
		nsv_seek_bar(v->box_w, &sx0, &sx1);
		if (v->bar_shown && y >= v->box_h - NSV_BAR && x >= sx0 - 6 &&
				v->duration > 0) {
			double f = (double)(x - sx0) / (sx1 - sx0);

			if (f < 0)
				f = 0;
			if (f > 1)
				f = 1;
			nsv_seek(v, f * v->duration);
			if (v->state == NSV_PAUSED)
				nsv_pause(v, false);
			guit->misc->schedule(NSV_TICK, nsv_tick, v);
		} else if (v->state == NSV_ENDED) {
			nsv_seek(v, 0);
			nsv_audio_pause(&v->audio, false);
			guit->misc->schedule(NSV_TICK, nsv_tick, v);
		} else {
			nsv_pause(v, v->state == NSV_PLAYING);
		}
		nsv_request_redraw(v);
		break;
	}
	return NSERROR_OK;
}

/* ---- content handler -------------------------------------------------- */

static nserror nsv_poster_callback(hlcache_handle *h, const hlcache_event *event,
		void *pw)
{
	nsvideo_content *v = pw;

	switch (event->type) {
	case CONTENT_MSG_READY:
	case CONTENT_MSG_DONE:
		if (!v->have_picture)
			nsv_request_redraw(v);
		break;
	case CONTENT_MSG_ERROR:
		hlcache_handle_release(h);
		v->poster = NULL;
		break;
	default:
		break;
	}
	return NSERROR_OK;
}

static nserror nsvideo_create(const content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	nsvideo_content *v;
	nserror error;

	v = calloc(1, sizeof(nsvideo_content));
	if (v == NULL)
		return NSERROR_NOMEM;

	error = content__init(&v->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(v);
		return error;
	}
	v->audio.fd = -1;
	v->total = -1;
	v->state = NSV_IDLE;
	*c = (struct content *)v;
	return NSERROR_OK;
}

/** the description is "key=value" lines written by box_video() */
static void nsv_parse_description(nsvideo_content *v, const char *text,
		size_t len)
{
	char *copy = malloc(len + 1), *line, *save = NULL;
	int w = 0, h = 0;

	if (copy == NULL)
		return;
	memcpy(copy, text, len);
	copy[len] = '\0';

	for (line = strtok_r(copy, "\n", &save); line != NULL;
			line = strtok_r(NULL, "\n", &save)) {
		char *value = strchr(line, '=');

		if (value == NULL)
			continue;
		*value++ = '\0';
		if (strcmp(line, "url") == 0) {
			free(v->url);
			v->url = strdup(value);
		} else if (strcmp(line, "poster") == 0) {
			free(v->poster_url);
			v->poster_url = strdup(value);
		} else if (strcmp(line, "autoplay") == 0) {
			v->autoplay = true;
		} else if (strcmp(line, "loop") == 0) {
			v->loop = true;
		} else if (strcmp(line, "muted") == 0) {
			v->muted = true;
		} else if (strcmp(line, "width") == 0) {
			w = atoi(value);
		} else if (strcmp(line, "height") == 0) {
			h = atoi(value);
		} else if (strcmp(line, "duration") == 0) {
			v->hint_duration = atof(value);
		}
	}
	free(copy);

	if (w <= 0 && h <= 0) {
		w = 640;
		h = 360;
	} else if (w <= 0) {
		w = h * 16 / 9;
	} else if (h <= 0) {
		h = w * 9 / 16;
	}
	v->base.width = w;
	v->base.height = h;
	v->box_w = w;
	v->box_h = h;
	v->duration = v->hint_duration;
}

static bool nsvideo_convert(struct content *c)
{
	nsvideo_content *v = (nsvideo_content *)c;
	const uint8_t *data;
	size_t size;

	data = content__get_source_data(c, &size);
	nsv_parse_description(v, (const char *)data, size);
	if (v->url == NULL) {
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	if (v->poster_url != NULL) {
		nsurl *url;

		if (nsurl_create(v->poster_url, &url) == NSERROR_OK) {
			hlcache_handle_retrieve(url, 0, NULL, NULL,
					nsv_poster_callback, v, NULL,
					CONTENT_IMAGE, &v->poster);
			nsurl_unref(url);
		}
	}

	v->described = true;
	content_set_ready(c);
	content_set_done(c);
	content_set_status(c, "");

	if (v->opened && v->autoplay && v->state == NSV_IDLE)
		nsv_play(v);
	return true;
}

static nserror nsvideo_open(struct content *c, struct browser_window *bw,
		struct content *page, struct object_params *params)
{
	nsvideo_content *v = (nsvideo_content *)c;

	/* the page may be shown before the description has arrived */
	v->opened = true;
	if (v->described && v->autoplay && v->state == NSV_IDLE)
		nsv_play(v);
	return NSERROR_OK;
}

static nserror nsvideo_close(struct content *c)
{
	nsvideo_content *v = (nsvideo_content *)c;

	v->opened = false;
	nsv_stop(v);
	return NSERROR_OK;
}

static void nsvideo_destroy(struct content *c)
{
	nsvideo_content *v = (nsvideo_content *)c;

	nsv_stop(v);
	if (v->multi != NULL)
		curl_multi_cleanup(v->multi);
	if (v->poster != NULL)
		hlcache_handle_release(v->poster);
	if (v->bitmap != NULL)
		guit->bitmap->destroy(v->bitmap);
	free(v->pcm);
	free(v->url);
	free(v->poster_url);
}

static nserror nsvideo_clone(const struct content *old, struct content **newc)
{
	return NSERROR_NOT_IMPLEMENTED;
}

static content_type nsvideo_type(void)
{
	return CONTENT_IMAGE;
}

static bool nsvideo_is_opaque(struct content *c)
{
	return true;
}

static const content_handler nsvideo_content_handler = {
	.create = nsvideo_create,
	.data_complete = nsvideo_convert,
	.destroy = nsvideo_destroy,
	.redraw = nsvideo_redraw,
	.mouse_track = nsvideo_mouse_track,
	.mouse_action = nsvideo_mouse_action,
	.open = nsvideo_open,
	.close = nsvideo_close,
	.clone = nsvideo_clone,
	.type = nsvideo_type,
	.is_opaque = nsvideo_is_opaque,
	.no_share = true,
};

static const char *nsvideo_types[] = {
	NSVIDEO_MIME_TYPE
};

CONTENT_FACTORY_REGISTER_TYPES(nsvideo, nsvideo_types, nsvideo_content_handler);

/* exported interface documented in image/video.h */
bool nsvideo_is_player(struct hlcache_handle *h)
{
	struct content *c = h != NULL ? hlcache_handle_get_content(h) : NULL;

	return c != NULL && c->handler == &nsvideo_content_handler;
}
