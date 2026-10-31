/* The gate's harness, shared by run-native (the exports called directly) and
 * run-wbx (the exports through miniBox), so the two print the same lines from
 * the same loop and differ only by the sandbox.
 *
 * usage: <runner> <game> --work <dir> --firmware prod.keys=PATH
 *        [--firmware title.keys=PATH] [--firmware firmware.zip=PATH]
 *        [--frames N] [--timeout S] [--digest-every N] [--ram-every N]
 *        [--screenshot F=PATH]... [--rtc-start S] [--docked] [--renderer R]
 *        [--log-filter F] [--rerecord] [--save-state FILE] [--state FILE]
 *        (states: run-wbx only) [--input FILE] [--audio-out FILE]
 *        [--savedata-in ZIP] [--savedata-out DIR] [--turbo A:B]
 *        [--content NSP]... [--resolution R]
 *
 * --rerecord saves and loads a state before every frame; --save-state writes
 * the machine after the last frame, --state starts from one, so another
 * process carries on where the first stopped. Frames are numbered by the
 * machine's own count; a run started from a state first prints the picture
 * it was loaded with ("loaded frame F ...").
 *
 * --input FILE drives the controls: lines "F MASK A0 ... A9" (blank and #
 * lines ignored) give the packed button mask (a player: Up Down Left Right A
 * B X Y L R ZL ZR Plus Minus LStick RStick; player 1 in bits 0..15, player 2
 * in 16..31; Touch is bit 32) and the axes (player 1's left stick X Y, right
 * X Y, then player 2's, -32768..32767; then Touch X Y, 0..65535), from frame
 * F on, until a later line. Axes left out are 0.
 * Without it the controls rest. --audio-out writes the sound, S16 stereo at
 * 48 kHz. --firmware mounts a file under a firmware id, as the frontend
 * does. --turbo turns rendering off for frames A..B-1. --renderer is
 * opengl-hw (the default) or null. --content puts an update or DLC .nsp in
 * the project's "content" slot (any number, in order). --resolution is the
 * internal resolution setting (1x, the default, 1.5x, 2x or 3x).
 *
 * Lines: "frame F time_ns=T ticks=K video=H audio=H/N" every --digest-every
 * frames (T the machine's clock when the frame's end was noticed, K the
 * CPU's ticks, N pairs of sound), with " ram=H" every --ram-every frames (a
 * digest of every page the game has mapped), then "app=NAME frames=F
 * time_ns=T video=H audio=H WxH" (audio: the whole run's sound). The work
 * directory (made fresh; only one carrying the harness's mark is emptied)
 * receives the machine's log at the end.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HARNESS_MAX_SHOTS 64
#define HARNESS_AXES 10
#define HARNESS_MAX_INPUT 4096
#define HARNESS_MAX_CONTENT 16

struct harness_opts {
	const char *app;
	const char *work;
	uint64_t frames;
	int timeout;
	uint64_t digest_every;
	uint64_t rtc_start, ram_every;
	int rerecord, docked, rebuild_every_frame, fast_states;
	const char *renderer, *log_filter, *resolution;
	const char *save_state, *load_state;
	const char *input, *audio_out, *savedata_in, *savedata_out;
	int firmware_n;
	const char *firmware_id[4], *firmware_path[4];
	uint64_t turbo_from, turbo_to;
	int content_n;
	const char *content[HARNESS_MAX_CONTENT];
	int shots;
	uint64_t shot_frame[HARNESS_MAX_SHOTS];
	const char *shot_path[HARNESS_MAX_SHOTS];
};

struct harness_core {
	int (*init)(void);
	const char *(*load_error)(void);
	void (*frame_advance)(uint64_t);
	uint32_t *(*video)(void);
	int (*width)(void);
	int (*height)(void);
	uint64_t (*frame_count)(void);
	uint64_t (*exited_at)(void);
	uint64_t (*time_ns)(void);
	uint64_t (*ticks)(void);
	uint64_t (*ram_digest)(void);
	int64_t (*log_size)(void);
	const uint8_t *(*log_buffer)(void);
	/* states (null natively): the machine sealed after Init, saved into a
	 * buffer the callee allocates, loaded from one */
	int (*seal)(void);
	int (*save)(uint8_t **buf, size_t *len);
	int (*load)(const uint8_t *buf, size_t len);
	void (*set_axis)(int32_t, int32_t);
	int16_t *(*audio)(void);
	int (*audio_count)(void);
	int32_t (*savedata_count)(void);
	const char *(*savedata_name)(int32_t);
	int64_t (*savedata_size)(int32_t);
	const uint8_t *(*savedata_buffer)(int32_t);
	void (*set_rendering)(int);
};

static int harness_parse(int argc, char **argv, struct harness_opts *o)
{
	memset(o, 0, sizeof *o);
	o->frames = 300;
	o->timeout = 120;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--work") && i + 1 < argc) o->work = argv[++i];
		else if (!strcmp(a, "--frames") && i + 1 < argc) o->frames = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(a, "--timeout") && i + 1 < argc) o->timeout = atoi(argv[++i]);
		else if (!strcmp(a, "--digest-every") && i + 1 < argc) o->digest_every = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(a, "--ram-every") && i + 1 < argc) o->ram_every = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(a, "--docked")) o->docked = 1;
		else if (!strcmp(a, "--rebuild-every-frame")) o->rebuild_every_frame = 1;
		else if (!strcmp(a, "--fast-states")) o->fast_states = 1;
		else if (!strcmp(a, "--renderer") && i + 1 < argc) o->renderer = argv[++i];
		else if (!strcmp(a, "--resolution") && i + 1 < argc) o->resolution = argv[++i];
		else if (!strcmp(a, "--log-filter") && i + 1 < argc) o->log_filter = argv[++i];
		else if (!strcmp(a, "--rtc-start") && i + 1 < argc) o->rtc_start = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(a, "--rerecord")) o->rerecord = 1;
		else if (!strcmp(a, "--input") && i + 1 < argc) o->input = argv[++i];
		else if (!strcmp(a, "--audio-out") && i + 1 < argc) o->audio_out = argv[++i];
		else if (!strcmp(a, "--savedata-in") && i + 1 < argc) o->savedata_in = argv[++i];
		else if (!strcmp(a, "--savedata-out") && i + 1 < argc) o->savedata_out = argv[++i];
		else if (!strcmp(a, "--content") && i + 1 < argc && o->content_n < HARNESS_MAX_CONTENT)
			o->content[o->content_n++] = argv[++i];
		else if (!strcmp(a, "--firmware") && i + 1 < argc && o->firmware_n < 4) {
			char *spec = argv[++i], *eq = strchr(spec, '=');
			if (!eq) { fprintf(stderr, "--firmware wants ID=PATH\n"); return 0; }
			*eq = '\0';
			o->firmware_id[o->firmware_n] = spec;
			o->firmware_path[o->firmware_n++] = eq + 1;
		}
		else if (!strcmp(a, "--turbo") && i + 1 < argc) {
			if (sscanf(argv[++i], "%" SCNu64 ":%" SCNu64, &o->turbo_from, &o->turbo_to) != 2) { fprintf(stderr, "--turbo wants A:B\n"); return 0; }
		}
		else if (!strcmp(a, "--save-state") && i + 1 < argc) o->save_state = argv[++i];
		else if (!strcmp(a, "--state") && i + 1 < argc) o->load_state = argv[++i];
		else if (!strcmp(a, "--screenshot") && i + 1 < argc && o->shots < HARNESS_MAX_SHOTS) {
			char *spec = argv[++i], *eq = strchr(spec, '=');
			if (!eq) { fprintf(stderr, "--screenshot wants F=PATH\n"); return 0; }
			*eq = '\0';
			o->shot_frame[o->shots] = strtoull(spec, NULL, 10);
			o->shot_path[o->shots++] = eq + 1;
		} else if (!o->app && a[0] != '-') o->app = a;
		else { fprintf(stderr, "unknown argument %s\n", a); return 0; }
	}
	if (!o->app || !o->work) {
		fprintf(stderr, "usage: %s <game> --work <dir> --firmware prod.keys=PATH [--firmware title.keys=PATH]"
			" [--firmware firmware.zip=PATH] [--frames N] [--timeout S] [--digest-every N] [--ram-every N]"
			" [--screenshot F=PATH]... [--rtc-start S] [--docked] [--renderer R] [--log-filter F] [--rerecord]"
			" [--save-state FILE] [--state FILE] [--input FILE] [--audio-out FILE] [--savedata-in ZIP]"
			" [--savedata-out DIR] [--turbo A:B] [--content NSP]... [--resolution R]\n", argv[0]);
		return 0;
	}
	return 1;
}

/* A runner that changes directory first makes every path it was given
 * absolute. */
static inline const char *harness_abs(const char *p)
{
	if (!p || p[0] == '/') return p;
	char cwd[4096];
	if (!getcwd(cwd, sizeof cwd)) return p;
	char *out = (char *)malloc(strlen(cwd) + strlen(p) + 2);
	sprintf(out, "%s/%s", cwd, p);
	return out;
}
static inline void harness_absolute(struct harness_opts *o)
{
	o->save_state = harness_abs(o->save_state);
	o->load_state = harness_abs(o->load_state);
	o->input = harness_abs(o->input);
	o->audio_out = harness_abs(o->audio_out);
	o->savedata_in = harness_abs(o->savedata_in);
	o->savedata_out = harness_abs(o->savedata_out);
	for (int f = 0; f < o->firmware_n; f++)
		o->firmware_path[f] = harness_abs(o->firmware_path[f]);
	for (int f = 0; f < o->content_n; f++)
		o->content[f] = harness_abs(o->content[f]);
	for (int s = 0; s < o->shots; s++)
		o->shot_path[s] = harness_abs(o->shot_path[s]);
}

/* The input script (--input): from frame `from` on, these buttons and axes. */
struct harness_input {
	uint64_t from, mask;
	int32_t axes[HARNESS_AXES];
};
static int harness_read_input(const char *path, struct harness_input *in, int *n)
{
	*n = 0;
	FILE *f = fopen(path, "r");
	if (!f) { fprintf(stderr, "cannot read %s\n", path); return 0; }
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0') continue;
		if (*n == HARNESS_MAX_INPUT) { fprintf(stderr, "%s: more than %d lines\n", path, HARNESS_MAX_INPUT); fclose(f); return 0; }
		struct harness_input *e = &in[(*n)++];
		char *end;
		e->from = strtoull(p, &end, 10);
		e->mask = strtoull(end, &end, 0);
		for (int a = 0; a < HARNESS_AXES; a++)
			e->axes[a] = (int32_t)strtol(end, &end, 10);
	}
	fclose(f);
	return 1;
}

/* the machine's save data export, into dir/<name> */
static int harness_savedata_out(const struct harness_core *c, const char *dir)
{
	if (!c->savedata_count) { fprintf(stderr, "this core exports no save data\n"); return 0; }
	const int32_t n = c->savedata_count();
	for (int32_t i = 0; i < n; i++) {
		const char *name = c->savedata_name(i);
		char path[4096], cmd[8300];
		snprintf(path, sizeof path, "%s/%s", dir, name);
		snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname '%s')\"", path);
		if (system(cmd) != 0) return 0;
		FILE *f = fopen(path, "wb");
		if (!f) { fprintf(stderr, "cannot write %s\n", path); return 0; }
		const int64_t size = c->savedata_size(i);
		if (size > 0) fwrite(c->savedata_buffer(i), 1, (size_t)size, f);
		fclose(f);
	}
	return 1;
}

/* The settings object both flavours read, as the frontend would mount it. */
static void harness_settings(const struct harness_opts *o, char *buf, size_t n)
{
	int k = snprintf(buf, n, "{\"rtc_start\": \"%" PRIu64 "\", \"console_mode\": \"%s\", \"renderer\": \"%s\"",
		o->rtc_start, o->docked ? "docked" : "handheld", o->renderer ? o->renderer : "opengl-hw");
	if (o->log_filter) k += snprintf(buf + k, n - k, ", \"log_filter\": \"%s\"", o->log_filter);
	if (o->rebuild_every_frame) k += snprintf(buf + k, n - k, ", \"rebuild_every_frame\": \"1\"");
	if (o->fast_states) k += snprintf(buf + k, n - k, ", \"state_pictures\": \"fast\"");
	if (o->resolution) k += snprintf(buf + k, n - k, ", \"resolution\": \"%s\"", o->resolution);
	snprintf(buf + k, n - k, "}");
}

static const char *harness_base(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* The slot map a project would mount: the app (named `game`, as the runner
 * mounts it), the save data zip under its own name, and the updates and DLC
 * under theirs (`paths`: as the paths given, for a runner that mounts
 * nothing). */
static void harness_slots(const struct harness_opts *o, const char *game, int paths, char *buf, size_t n)
{
	int k = snprintf(buf, n, "{\"game\": [\"%s\"]", game);
	if (o->savedata_in) k += snprintf(buf + k, n - k, ", \"savedata\": [\"%s\"]", harness_base(o->savedata_in));
	if (o->content_n) {
		k += snprintf(buf + k, n - k, ", \"content\": [");
		for (int f = 0; f < o->content_n; f++)
			k += snprintf(buf + k, n - k, "%s\"%s\"", f ? ", " : "", paths ? o->content[f] : harness_base(o->content[f]));
		k += snprintf(buf + k, n - k, "]");
	}
	snprintf(buf + k, n - k, "}");
}

/* A fresh work directory: made, or emptied when it carries our mark. */
static int harness_fresh_work(const char *work)
{
	char mark[4096], cmd[8300];
	snprintf(mark, sizeof mark, "%s/.eden-harness", work);
	struct stat st;
	if (stat(work, &st) == 0) {
		if (stat(mark, &st) != 0) {
			fprintf(stderr, "%s exists and is not a work directory of ours\n", work);
			return 0;
		}
		snprintf(cmd, sizeof cmd, "find '%s' -mindepth 1 ! -name .eden-harness -delete", work);
		if (system(cmd) != 0) return 0;
		return 1;
	}
	snprintf(cmd, sizeof cmd, "mkdir -p '%s' && touch '%s'", work, mark);
	return system(cmd) == 0;
}

static uint64_t harness_fnv1a(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
	return h;
}

/* uncompressed 32-bit TGA, top row first */
static int harness_tga(const char *path, const uint8_t *bgra, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	uint8_t hd[18] = { 0 };
	hd[2] = 2; hd[12] = w & 0xff; hd[13] = w >> 8; hd[14] = h & 0xff; hd[15] = h >> 8;
	hd[16] = 32; hd[17] = 0x28;
	fwrite(hd, 1, sizeof hd, f);
	fwrite(bgra, 1, (size_t)w * h * 4, f);
	return fclose(f) == 0;
}

/* the machine's log, into the work directory */
static void harness_save_log(const struct harness_core *c, const struct harness_opts *o)
{
	const int64_t n = c->log_size();
	if (n > 0) {
		char path[4096];
		snprintf(path, sizeof path, "%s/eden.log", o->work);
		FILE *f = fopen(path, "wb");
		if (f) { fwrite(c->log_buffer(), 1, (size_t)n, f); fclose(f); }
	}
}

/* Init, the frames, the lines, the log. Returns the process's exit status. */
static int harness_run(const struct harness_core *c, const struct harness_opts *o, const char *app_name)
{
	alarm((unsigned)o->timeout);
	if (!c->init()) {
		/* no load error at all: the guest died inside Init (miniBox said
		 * why), and nothing more may be asked of it */
		const char *why = c->load_error();
		fprintf(stderr, "Init failed: %s\n", why ? why : "the machine died (see above)");
		if (why)
			harness_save_log(c, o);
		return 6;
	}
	const int states = o->rerecord || o->save_state || o->load_state;
	if (states && (!c->seal || !c->save || !c->load)) {
		fprintf(stderr, "this runner has no states\n");
		return 2;
	}
	if (states && !c->seal()) {
		fprintf(stderr, "seal failed\n");
		return 6;
	}
	if (o->load_state) {
		FILE *f = fopen(o->load_state, "rb");
		if (!f) { fprintf(stderr, "cannot read %s\n", o->load_state); return 6; }
		fseek(f, 0, SEEK_END);
		const size_t len = (size_t)ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *buf = (uint8_t *)malloc(len);
		if (!buf || fread(buf, 1, len, f) != len || !c->load(buf, len)) { fprintf(stderr, "cannot load %s\n", o->load_state); return 6; }
		fclose(f);
		free(buf);
	}
	static struct harness_input script[HARNESS_MAX_INPUT];
	int script_n = 0;
	if (o->input && !harness_read_input(o->input, script, &script_n)) return 6;
	FILE *audio_out = NULL;
	if (o->audio_out && !(audio_out = fopen(o->audio_out, "wb"))) { fprintf(stderr, "cannot write %s\n", o->audio_out); return 6; }
	uint64_t run_audio = 0xcbf29ce484222325ull;

	const int w = c->width(), h = c->height();
	const size_t bytes = (size_t)w * h * 4;
	if (o->load_state)
		printf("loaded frame %" PRIu64 " time_ns=%" PRIu64 " video=%016" PRIx64 "\n", c->frame_count(), c->time_ns(),
			harness_fnv1a((const uint8_t *)c->video(), bytes));
	for (uint64_t i = 1; i <= o->frames; i++) {
		if (o->rerecord) {
			uint8_t *buf = NULL;
			size_t len = 0;
			if (!c->save(&buf, &len) || !c->load(buf, len)) { fprintf(stderr, "rerecord failed\n"); return 6; }
			free(buf);
		}
		/* the line of the script in force for the frame about to run */
		uint64_t mask = 0;
		const uint64_t next = c->frame_count() + 1;
		const struct harness_input *cur = NULL;
		for (int s = 0; s < script_n; s++)
			if (script[s].from <= next) cur = &script[s];
		if (cur) {
			mask = cur->mask;
			for (int a = 0; a < HARNESS_AXES; a++)
				c->set_axis(a, cur->axes[a]);
		}
		if (o->turbo_to > o->turbo_from)
			c->set_rendering(next >= o->turbo_from && next < o->turbo_to ? 0 : 1);
		c->frame_advance(mask);
		if (c->exited_at())
			break;
		const uint64_t f = c->frame_count();
		const uint8_t *pic = (const uint8_t *)c->video();
		if (!pic) {
			/* a machine that died answers nothing (miniBox said why) */
			fprintf(stderr, "the machine died in frame %" PRIu64 "\n", f + 1);
			return 7;
		}
		const int pairs = c->audio_count();
		const uint8_t *snd = (const uint8_t *)c->audio();
		const size_t snd_bytes = (size_t)pairs * 4;
		for (size_t b = 0; b < snd_bytes; b++) { run_audio ^= snd[b]; run_audio *= 0x100000001b3ull; }
		if (audio_out && snd_bytes) fwrite(snd, 1, snd_bytes, audio_out);
		if (o->digest_every && f % o->digest_every == 0)
		{
			printf("frame %" PRIu64 " time_ns=%" PRIu64 " ticks=%" PRIu64 " video=%016" PRIx64 " audio=%016" PRIx64 "/%d", f, c->time_ns(),
				c->ticks(), harness_fnv1a(pic, bytes), harness_fnv1a(snd, snd_bytes), pairs);
			if (o->ram_every && f % o->ram_every == 0)
				printf(" ram=%016" PRIx64, c->ram_digest());
			printf("\n");
			fflush(stdout);
		}
		for (int s = 0; s < o->shots; s++)
			if (o->shot_frame[s] == f && !harness_tga(o->shot_path[s], pic, w, h))
				fprintf(stderr, "could not write %s\n", o->shot_path[s]);
	}
	if (audio_out) fclose(audio_out);
	printf("app=%s frames=%" PRIu64 " time_ns=%" PRIu64 " video=%016" PRIx64 " audio=%016" PRIx64 " %dx%d\n",
		app_name, c->frame_count(), c->time_ns(),
		harness_fnv1a((const uint8_t *)c->video(), bytes), run_audio, w, h);
	fflush(stdout);
	harness_save_log(c, o);
	if (o->savedata_out && !harness_savedata_out(c, o->savedata_out)) return 6;
	if (o->save_state) {
		uint8_t *buf = NULL;
		size_t len = 0;
		FILE *f = fopen(o->save_state, "wb");
		if (!f || !c->save(&buf, &len) || fwrite(buf, 1, len, f) != len) { fprintf(stderr, "cannot write %s\n", o->save_state); return 6; }
		fclose(f);
		free(buf);
	}
	return 0;
}
