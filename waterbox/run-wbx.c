/* run-wbx: the core in miniBox, for the gate. The same harness as run-native
 * (gate-harness.h), the same lines; the app is mounted under its basename
 * with rom.name naming it and the settings in "settings", the shape a
 * project mounts, and the GPU bridge's host half answers in this process.
 *
 * usage: run-wbx <core.wbx> <game> --work <dir> [options: see gate-harness.h]
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "minibox.h"
#include "gate-harness.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

int chimera_gl_host_init(char *err, int errlen);
const char *chimera_gl_host_description(void);
uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
unsigned long chimera_gl_host_unhandled(long *last_op);
void chimera_gl_host_state_loaded(void);

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t left = m->n - m->pos;
	if (s > left) s = left;
	memcpy(d, m->p + m->pos, s);
	m->pos += s;
	return (intptr_t)s;
}

static mb_host *g_host;

typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) {
		size_t cap = m->cap ? m->cap * 2 : (64u << 20);
		while (cap < m->len + n) cap *= 2;
		uint8_t *b = (uint8_t *)realloc(m->b, cap);
		if (!b) return -1;
		m->b = b; m->cap = cap;
	}
	memcpy(m->b + m->len, d, n);
	m->len += n;
	return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	size_t left = m->len - m->pos;
	if (n > left) n = left;
	memcpy(d, m->b + m->pos, n);
	m->pos += n;
	return (intptr_t)n;
}

/* the post-boot machine is the savestate baseline */
static int do_seal(void)
{
	mb_return r;
	wbx_deactivate_host(g_host, &r);
	wbx_seal(g_host, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 0; }
	wbx_activate_host(g_host, &r);
	return 1;
}
/* The engine tells the core before every state it takes (StateSaving);
 * RUN_WBX_NO_STATE_SAVING leaves it untold, the gate's negative control. */
static void (*g_state_saving)(void);
static int do_save(uint8_t **buf, size_t *len)
{
	membuf m = { 0 };
	mb_return r;
	const char *untold = getenv("RUN_WBX_NO_STATE_SAVING");
	if (g_state_saving && (!untold || !*untold))
		g_state_saving();
	wbx_save_state(g_host, mem_write, (uintptr_t)&m, &r);
	if (r.error_message[0]) { fprintf(stderr, "save: %s\n", r.error_message); free(m.b); return 0; }
	*buf = m.b;
	*len = m.len;
	return 1;
}
/* What Chimera's session does after a load (ce_gl_state_loaded): the host
 * mints a fresh context id, the renderer's cue that what it remembers is
 * another context's. Without it a load here would be an easier test than a
 * load in Chimera. */
static int do_load(const uint8_t *buf, size_t len)
{
	membuf m = { (uint8_t *)buf, len, len, 0 };
	mb_return r;
	wbx_load_state(g_host, mem_read, (uintptr_t)&m, &r);
	if (r.error_message[0]) { fprintf(stderr, "load: %s\n", r.error_message); return 0; }
	/* the gate's negative control: a load the renderer is not told about */
	const char *keep = getenv("RUN_WBX_KEEP_CONTEXT");
	if (!keep || !*keep)
		chimera_gl_host_state_loaded();
	return 1;
}

static uintptr_t proc(const char *n)
{
	mb_return r;
	wbx_get_proc_addr(g_host, n, &r);
	if (r.error_message[0] || !r.data) { fprintf(stderr, "missing export %s %s\n", n, r.error_message); exit(2); }
	return r.data;
}

int main(int argc, char **argv)
{
	if (getenv("MB_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, -1L, 0, 0, 0);
	if (argc < 2) { fprintf(stderr, "usage: run-wbx <core.wbx> <game> --work <dir> [options]\n"); return 2; }
	const char *core = argv[1];
	struct harness_opts o;
	if (!harness_parse(argc - 1, argv + 1, &o)) return 2;
	if (!harness_fresh_work(o.work)) return 2;

	FILE *wf = fopen(core, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", core); return 1; }
	/* The Switch's 4 GiB of RAM is one reservation, each guest core's JIT has
	 * its own code cache, and the GPU's shadows and the machine's filesystem
	 * (the installed system software) want a heap; Linux backs the block
	 * lazily. The invisible pool holds the picture (waterbox.config says
	 * how much of it). */
	mb_memory_layout_template layout = { (uintptr_t)2048 << 20, 16u << 20, 256u << 20, 256u << 20, (uintptr_t)24 << 30 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	g_host = (mb_host *)r.data;

	/* the app under its basename, and rom.name naming it: the frontend's shape */
	const char *base = strrchr(o.app, '/');
	base = base ? base + 1 : o.app;
	static char vfsname[512];
	snprintf(vfsname, sizeof vfsname, "/%s", base);
	wbx_mount_file_path(g_host, vfsname, o.app, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", vfsname, r.error_message); return 1; }
	memreader nr = { (const uint8_t *)vfsname, strlen(vfsname), 0 };
	wbx_mount_file(g_host, "rom.name", mem_reader, (uintptr_t)&nr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount rom.name: %s\n", r.error_message); return 1; }
	static char settings[512];
	harness_settings(&o, settings, sizeof settings);
	memreader sr = { (const uint8_t *)settings, strlen(settings), 0 };
	wbx_mount_file(g_host, "settings", mem_reader, (uintptr_t)&sr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }
	for (int f = 0; f < o.firmware_n; f++) {
		wbx_mount_file_path(g_host, o.firmware_id[f], o.firmware_path[f], &r);
		if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", o.firmware_id[f], r.error_message); return 1; }
	}
	/* the slot map a project mounts, and the save data under its own name */
	static char slots[8192], savename[1024];
	harness_slots(&o, base, 0, slots, sizeof slots);
	memreader slr = { (const uint8_t *)slots, strlen(slots), 0 };
	wbx_mount_file(g_host, "slots", mem_reader, (uintptr_t)&slr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount slots: %s\n", r.error_message); return 1; }
	if (o.savedata_in) {
		snprintf(savename, sizeof savename, "/%s", harness_base(o.savedata_in));
		wbx_mount_file_path(g_host, savename, o.savedata_in, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", savename, r.error_message); return 1; }
	}
	for (int f = 0; f < o.content_n; f++) {
		static char contentname[HARNESS_MAX_CONTENT][1024];
		snprintf(contentname[f], sizeof contentname[f], "/%s", harness_base(o.content[f]));
		wbx_mount_file_path(g_host, contentname[f], o.content[f], &r);
		if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", contentname[f], r.error_message); return 1; }
	}
	for (int f = 0; f < o.mod_n; f++) {
		static char modname[HARNESS_MAX_MODS][1024];
		snprintf(modname[f], sizeof modname[f], "/%s", harness_base(o.mod[f]));
		wbx_mount_file_path(g_host, modname[f], o.mod[f], &r);
		if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", modname[f], r.error_message); return 1; }
	}
	wbx_activate_host(g_host, &r);

	/* the GPU bridge, handed over before Init */
	char glerr[256] = "";
	if (!o.renderer || strcmp(o.renderer, "null") != 0) {
		if (chimera_gl_host_init(glerr, sizeof glerr) != 0) { fprintf(stderr, "gpu bridge: %s\n", glerr); return 5; }
		fprintf(stderr, "gpu bridge: %s\n", chimera_gl_host_description());
		void (*set_bridge)(uint64_t) = (void (*)(uint64_t))proc("SetGpuBridge");
		mb_return gr;
		wbx_get_callback_addr(g_host, (mb_external_callback)chimera_gl_host_dispatch, 0, &gr);
		if (!gr.data) { fprintf(stderr, "gpu bridge: could not register the callback\n"); return 5; }
		set_bridge((uint64_t)gr.data);
	}

	g_state_saving = (void (*)(void))proc("StateSaving");
	/* on Linux the guest's calling convention is the host's: the exports are
	 * called as plain functions */
	const struct harness_core c = {
		(int (*)(void))proc("Init"),
		(const char *(*)(void))proc("GetLoadError"),
		(void (*)(uint64_t))proc("FrameAdvance"),
		(uint32_t *(*)(void))proc("GetVideoBgra"),
		(int (*)(void))proc("GetVideoWidth"),
		(int (*)(void))proc("GetVideoHeight"),
		(uint64_t (*)(void))proc("GetFrameCount"),
		(uint64_t (*)(void))proc("GetExitedAt"),
		(uint64_t (*)(void))proc("GetMachineTimeNs"),
		(uint64_t (*)(void))proc("GetCpuTicks"),
		(uint64_t (*)(void))proc("GetRamDigest"),
		(int64_t (*)(void))proc("GetLogSize"),
		(const uint8_t *(*)(void))proc("GetLogBuffer"),
		do_seal, do_save, do_load,
		(void (*)(int32_t, int32_t))proc("SetAxis"),
		(int16_t *(*)(void))proc("GetAudio"),
		(int (*)(void))proc("GetAudioSampleCount"),
		(int32_t (*)(void))proc("GetSaveDataFileCount"),
		(const char *(*)(int32_t))proc("GetSaveDataFileName"),
		(int64_t (*)(int32_t))proc("GetSaveDataFileSize"),
		(const uint8_t *(*)(int32_t))proc("GetSaveDataFileBuffer"),
		(void (*)(int))proc("SetRenderingEnabled"),
	};
	const int rc = harness_run(&c, &o, base);
	if (getenv("CHIMERA_LIST_FILES"))
		fputs(((const char *(*)(void))proc("DebugListFiles"))(), stderr);
	long last = 0;
	const unsigned long unhandled = chimera_gl_host_unhandled(&last);
	if (unhandled)
		fprintf(stderr, "gpu bridge: %lu calls had no case (last opcode %ld)\n", unhandled, last);
	fflush(stdout);
	_Exit(rc);
}
