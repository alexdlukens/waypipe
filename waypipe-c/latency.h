/*
 * Frame-path latency telemetry for the waypipe decoder (gdwaypipe_c).
 * Header-only so no new translation unit / build-file change is needed;
 * each including .c gets its own static accumulator and prints 1 Hz lines
 * to stderr with the prefix `[gdwaypipe-lat]`, gated by GDWAYPIPE_LATENCY=1.
 * Deltas use CLOCK_MONOTONIC within the process; see
 * panelspace/docs/frame-latency-telemetry-plan.md for the decision tree.
 *
 * Copyright © 2026 the gdwaypipe_c authors
 * SPDX-License-Identifier: MIT
 */
#ifndef GDWAYPIPE_LATENCY_H
#define GDWAYPIPE_LATENCY_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Cached one-time env read; GDWAYPIPE_LATENCY=1 turns telemetry on. */
static int wp_lat_enabled(void)
{
	static int once = 0;
	static int en = 0;
	if (!once) {
		once = 1;
		const char *e = getenv("GDWAYPIPE_LATENCY");
		en = (e && e[0] == '1') ? 1 : 0;
	}
	return en;
}

static int64_t wp_lat_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
}

/* Rolling one-second accumulator. idle = time blocked in poll (waiting on the
 * remote / ack coalescing); busy = time doing transfer work between polls;
 * wakeups = loop iterations where the channel (inbound) had an event. */
struct wp_lat_win {
	int64_t start_us;
	int64_t idle_us;
	int64_t busy_us;
	int64_t iters;
	int64_t wakeups;
};

static void wp_lat_win_init(struct wp_lat_win *w)
{
	w->start_us = wp_lat_now_us();
	w->idle_us = 0;
	w->busy_us = 0;
	w->iters = 0;
	w->wakeups = 0;
}

/* Record one loop iteration: idle_us spent in poll, busy_us spent on work,
 * chan_wake = whether the channel fd had inbound events this iteration.
 * Emits a 1 Hz aggregate line when the window elapses. */
static void wp_lat_win_record(struct wp_lat_win *w, int64_t idle_us,
		int64_t busy_us, int chan_wake)
{
	if (!wp_lat_enabled()) {
		return;
	}
	w->idle_us += idle_us;
	w->busy_us += busy_us;
	w->iters += 1;
	w->wakeups += chan_wake ? 1 : 0;

	int64_t now = wp_lat_now_us();
	int64_t dt = now - w->start_us;
	if (dt < 1000000) {
		return;
	}
	double idle_r = dt ? (double)w->idle_us / (double)dt : 0.0;
	double busy_r = dt ? (double)w->busy_us / (double)dt : 0.0;
	fprintf(stderr,
			"[gdwaypipe-lat] decoder idle_ratio=%.2f busy_ratio=%.2f "
			"iters/s=%.0f chan_wake/s=%.0f iter_busy_us_avg=%.1f\n",
			idle_r, busy_r,
			(double)w->iters * 1e6 / (double)dt,
			(double)w->wakeups * 1e6 / (double)dt,
			w->iters ? (double)w->busy_us / (double)w->iters : 0.0);
	w->start_us = now;
	w->idle_us = 0;
	w->busy_us = 0;
	w->iters = 0;
	w->wakeups = 0;
}

/* Video-decode frame counter/cost: accumulate per decoded frame, emit 1 Hz. */
struct wp_lat_video {
	int64_t start_us;
	int64_t frames;
	int64_t decode_us;
	int64_t decode_us_max;
};

static void wp_lat_video_init(struct wp_lat_video *w)
{
	w->start_us = wp_lat_now_us();
	w->frames = 0;
	w->decode_us = 0;
	w->decode_us_max = 0;
}

static void wp_lat_video_record(struct wp_lat_video *w, int64_t decode_us)
{
	if (!wp_lat_enabled()) {
		return;
	}
	w->frames += 1;
	w->decode_us += decode_us;
	if (decode_us > w->decode_us_max) {
		w->decode_us_max = decode_us;
	}
	int64_t now = wp_lat_now_us();
	int64_t dt = now - w->start_us;
	if (dt < 1000000) {
		return;
	}
	fprintf(stderr,
			"[gdwaypipe-lat] decode frames/s=%.0f decode_ms_avg=%.2f "
			"decode_ms_max=%.2f\n",
			(double)w->frames * 1e6 / (double)dt,
			w->frames ? (double)w->decode_us / (double)w->frames / 1000.0 : 0.0,
			(double)w->decode_us_max / 1000.0);
	w->start_us = now;
	w->frames = 0;
	w->decode_us = 0;
	w->decode_us_max = 0;
}

#endif /* GDWAYPIPE_LATENCY_H */
