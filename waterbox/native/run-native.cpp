// run-native: the core without the sandbox, for the gate. The same harness
// as run-wbx (gate-harness.h), the same lines; the files a frontend would
// mount (the game, the keys, the firmware, settings, slots) sit in the work
// directory under the names they would be mounted as, and the GPU bridge's
// host half answers in this process.
//
// usage: eden-run-native <game> --work <dir> [options: see gate-harness.h]
// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include "../gate-harness.h"
}

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

extern "C" {
// the core's exports (driver/wbx-entry.cpp)
int Init(void);
const char *GetLoadError(void);
void SetGpuBridge(uint64_t addr);
void SetVulkanBridge(uint64_t addr);
void SetGlThreadHooks(void (*bind)(), void (*release)());
void FrameAdvance(uint64_t buttons);
uint32_t *GetVideoBgra(void);
int GetVideoWidth(void);
int GetVideoHeight(void);
uint64_t GetFrameCount(void);
uint64_t GetExitedAt(void);
uint64_t GetMachineTimeNs(void);
uint64_t GetCpuTicks(void);
uint64_t GetRamDigest(void);
int64_t GetLogSize(void);
const uint8_t *GetLogBuffer(void);
void SetAxis(int32_t index, int32_t value);
void SetRenderingEnabled(int on);
int32_t GetSaveDataFileCount(void);
const char *GetSaveDataFileName(int32_t i);
int64_t GetSaveDataFileSize(int32_t i);
const uint8_t *GetSaveDataFileBuffer(int32_t i);
int16_t *GetAudio(void);
int GetAudioSampleCount(void);
// the bridge's host half (gl-host.c)
int chimera_gl_host_init(char *err, int errlen);
const char *chimera_gl_host_description(void);
uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
int chimera_gl_host_bind_current(void);
void chimera_gl_host_release_current(void);
unsigned long chimera_gl_host_unhandled(long *last_op);
// the Vulkan bridge's host half (miniBox source/vk/vk-host.h)
int chimera_vk_host_init(char *err, int errlen);
uintptr_t chimera_vk_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
const char *chimera_vk_host_description(void);
unsigned long chimera_vk_host_refused(long *last_op);
}

// The Vulkan bridge's host half keeps its scratch in globals: in the sandbox
// every guest thread is the one host thread, here they are real threads, so
// one call at a time.
static std::recursive_mutex vk_calls;
static uintptr_t vk_dispatch_one_at_a_time(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    std::scoped_lock lock{vk_calls};
    return chimera_vk_host_dispatch(op, a, b, c, d, e);
}

static void bind_gl() {
    chimera_gl_host_bind_current();
}
static void release_gl() {
    chimera_gl_host_release_current();
}

static bool write_file(const std::string &path, const std::string &text) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fwrite(text.data(), 1, text.size(), f);
    return fclose(f) == 0;
}

int main(int argc, char **argv) {
    harness_opts o;
    if (!harness_parse(argc, argv, &o))
        return 2;
    if (!harness_fresh_work(o.work))
        return 2;
    char app[PATH_MAX];
    if (!realpath(o.app, app)) {
        perror(o.app);
        return 4;
    }
    char settings[512];
    harness_settings(&o, settings, sizeof settings);
    const std::string work = o.work;
    char slots[8192];
    for (int f = 0; f < o.content_n; f++) {
        char *full = realpath(o.content[f], nullptr);
        if (!full) {
            perror(o.content[f]);
            return 4;
        }
        o.content[f] = full;
    }
    for (int f = 0; f < o.mod_n; f++) {
        char *full = realpath(o.mod[f], nullptr);
        if (!full) {
            perror(o.mod[f]);
            return 4;
        }
        o.mod[f] = full;
    }
    harness_slots(&o, app, 1, slots, sizeof slots);
    if (!write_file(work + "/rom.name", app) || !write_file(work + "/settings", settings) || !write_file(work + "/slots", slots)) {
        fprintf(stderr, "cannot write the work directory\n");
        return 2;
    }
    for (int f = 0; f < o.firmware_n; f++) {
        // a copy, never a link: a link back to the owner's files is a way to
        // write to them
        const std::string cmd = "cp '" + std::string(o.firmware_path[f]) + "' '" + work + "/" + o.firmware_id[f] + "'";
        if (system(cmd.c_str()) != 0) {
            fprintf(stderr, "cannot copy %s\n", o.firmware_path[f]);
            return 2;
        }
    }
    if (o.savedata_in) {
        const std::string cmd = "cp '" + std::string(o.savedata_in) + "' '" + work + "/" + harness_base(o.savedata_in) + "'";
        if (system(cmd.c_str()) != 0) {
            fprintf(stderr, "cannot copy %s\n", o.savedata_in);
            return 2;
        }
    }
    harness_absolute(&o);
    char work_abs[PATH_MAX];
    if (!realpath(o.work, work_abs) || chdir(work_abs) != 0) {
        perror(o.work);
        return 2;
    }
    o.work = work_abs;

    const bool vulkan = o.renderer && std::string(o.renderer) == "vulkan-hw";
    if (vulkan) {
        char err[256] = "";
        if (chimera_vk_host_init(err, sizeof err) != 0) {
            fprintf(stderr, "vulkan bridge: %s\n", err);
            return 5;
        }
        fprintf(stderr, "vulkan bridge: %s\n", chimera_vk_host_description());
        SetVulkanBridge(reinterpret_cast<uintptr_t>(&vk_dispatch_one_at_a_time));
    } else if (!o.renderer || std::string(o.renderer) != "null") {
        char err[256] = "";
        if (chimera_gl_host_init(err, sizeof err) != 0) {
            fprintf(stderr, "gpu bridge: %s\n", err);
            return 5;
        }
        fprintf(stderr, "gpu bridge: %s\n", chimera_gl_host_description());
        // the context is made current by whoever draws
        chimera_gl_host_release_current();
        SetGpuBridge(reinterpret_cast<uintptr_t>(&chimera_gl_host_dispatch));
        SetGlThreadHooks(bind_gl, release_gl);
    }

    const harness_core c = { Init, GetLoadError, FrameAdvance, GetVideoBgra, GetVideoWidth, GetVideoHeight,
        GetFrameCount, GetExitedAt, GetMachineTimeNs, GetCpuTicks, GetRamDigest, GetLogSize, GetLogBuffer,
        nullptr, nullptr, nullptr, SetAxis, GetAudio, GetAudioSampleCount, GetSaveDataFileCount, GetSaveDataFileName,
        GetSaveDataFileSize, GetSaveDataFileBuffer, SetRenderingEnabled };
    const char *slash = strrchr(app, '/');
    const int rc = harness_run(&c, &o, slash ? slash + 1 : app);
    long last = 0;
    if (vulkan) {
        const unsigned long refused = chimera_vk_host_refused(&last);
        fprintf(stderr, "vulkan bridge: %lu calls refused (last opcode %ld)\n", refused, last);
    }
    const unsigned long unhandled = chimera_gl_host_unhandled(&last);
    if (unhandled)
        fprintf(stderr, "gpu bridge: %lu calls had no case (last opcode %ld)\n", unhandled, last);
    fflush(stdout);
    _Exit(rc);
}
