// The core's exports: what the frontend (and both runners of the gate) call.
// The same file in both flavours: natively they are called directly, in the
// sandbox through miniBox. A project names its game in "slots" ({"game":
// ["x.nsp"]}, mounted under that name); a file opened on its own arrives as
// what "rom.name" names. The settings are the JSON object in "settings", the
// console's keys and system software the firmware files "prod.keys",
// "title.keys" and "firmware.zip". Mounted files in the sandbox, files in
// the working directory natively.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_driver.h"
#include "memfs.h"

#include "gl-bridge.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef CHIMERA_GUEST
#include <emulibc.h>
#else
#define ECL_EXPORT __attribute__((visibility("default")))
#define ECL_INVISIBLE
#endif

namespace {

// The controls as waterbox.config declares them. Buttons, a player: Up,
// Down, Left, Right, A, B, X, Y, L, R, ZL, ZR, Plus, Minus, L Stick, R Stick
// (player 1 in bits 0..15 of the mask, player 2 in 16..31), then Touch (bit
// 32). Axes: a player's left stick X, Y and right stick X, Y (-32768..32767,
// right and up positive), player 1 then player 2, then Touch X, Touch Y
// (0..65535 across and down the screen).
constexpr int PLAYERS = 2, AXES = 10, TOUCH_BIT = 32, TOUCH_X = 8, TOUCH_Y = 9;
// the wire's button order in Eden's (Settings::NativeButton): A B X Y LStick
// RStick L R ZL ZR Plus Minus DLeft DUp DRight DDown
constexpr int EDEN_BUTTON[16] = { 13, 15, 12, 14, 0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5 };

std::string g_error;
std::vector<uint8_t> g_log;
int32_t g_axes[AXES] = { 0, 0, 0, 0, 0, 0, 0, 0, 32768, 32768 };
bool g_bridge = false;

std::string read_file(const char *name) {
    std::string out;
    if (FILE *f = fopen(name, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        fclose(f);
    }
    return out;
}

bool exists(const std::string &name) {
    if (FILE *f = fopen(name.c_str(), "rb")) {
        fclose(f);
        return true;
    }
    return false;
}

// The string value of a key in a flat JSON object, or "".
std::string json_string(const std::string &json, const char *key) {
    const std::string k = std::string("\"") + key + "\"";
    const auto at = json.find(k);
    if (at == std::string::npos)
        return "";
    size_t i = json.find(':', at + k.size());
    if (i == std::string::npos)
        return "";
    i = json.find('"', i);
    if (i == std::string::npos)
        return "";
    const size_t end = json.find('"', i + 1);
    return end == std::string::npos ? "" : json.substr(i + 1, end - i - 1);
}

// A number from a flat JSON object ("key": 123 or "key": "123"), or 0.
uint64_t json_number(const std::string &json, const char *key) {
    const std::string k = std::string("\"") + key + "\"";
    const auto at = json.find(k);
    if (at == std::string::npos)
        return 0;
    size_t i = json.find(':', at + k.size());
    if (i == std::string::npos)
        return 0;
    i++;
    while (i < json.size() && (json[i] == ' ' || json[i] == '"'))
        i++;
    return strtoull(json.c_str() + i, nullptr, 10);
}

// Every name in a slot of the project's slot map, in its order.
std::vector<std::string> slot_all(const std::string &slots, const char *id) {
    std::vector<std::string> out;
    const std::string k = std::string("\"") + id + "\"";
    const auto at = slots.find(k);
    if (at == std::string::npos)
        return out;
    size_t i = slots.find('[', at + k.size());
    if (i == std::string::npos)
        return out;
    for (;;) {
        i = slots.find_first_not_of(" \t\r\n,", i + 1);
        if (i == std::string::npos || slots[i] != '"')
            return out;
        std::string name;
        size_t j = i + 1;
        for (; j < slots.size() && slots[j] != '"'; j++) {
            if (slots[j] == '\\' && j + 1 < slots.size())
                j++;
            name += slots[j];
        }
        out.push_back(name);
        i = j;
    }
}

// The first name in a slot of the project's slot map, or "".
std::string slot_first(const std::string &slots, const char *id) {
    const std::vector<std::string> all = slot_all(slots, id);
    return all.empty() ? "" : all.front();
}

// A mounted file's name as it opens: a project mounts a slot's file under
// the name the map gives, a file opened on its own sits at "/<name>" too.
std::string mounted(const std::string &name) {
    if (name.empty() || name[0] == '/')
        return name;
    if (exists(name))
        return name;
    if (exists("/" + name))
        return "/" + name;
    return name;
}

} // namespace

extern "C" {

ECL_EXPORT const char *GetLoadError(void) {
    return g_error.c_str();
}

// The GPU bridge: the host's dispatcher, handed over before Init. Eden's
// OpenGL renderer draws on the machine's GPU through it.
ECL_EXPORT void SetGpuBridge(uint64_t addr) {
    g_bridge = chimera_gl_install(reinterpret_cast<chimera_gl_bridge_fn>(static_cast<uintptr_t>(addr)));
}

#ifndef CHIMERA_GUEST
// Natively the GL context is a real one held by one thread at a time.
ECL_EXPORT void SetGlThreadHooks(void (*bind)(), void (*release)()) {
    chimera_eden::set_gl_thread_hooks(bind, release);
}
#endif

ECL_EXPORT int Init(void) {
    const std::string slots = read_file("slots");
    std::string name = mounted(slot_first(slots, "game"));
    if (name.empty()) {
        name = read_file("rom.name");
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
            name.pop_back();
    }
    if (name.empty()) {
        g_error = "error.no_game";
        return 0;
    }
    const std::string settings = read_file("settings");
    chimera_eden::Options options;
    // The renderer: Eden's OpenGL one on the machine's GPU, named "-hw" so a
    // frontend knows to hand the GPU over; "null" draws nothing (the gate's).
    const std::string renderer = json_string(settings, "renderer");
    if (renderer.empty() || renderer == "opengl-hw") {
        options.renderer = "opengl";
        if (!g_bridge) {
            g_error = "error.no_gl_bridge";
            return 0;
        }
    } else if (renderer == "null") {
        options.renderer = "null";
    } else {
        g_error = std::string("error.bad_renderer" TEXT_ARG) + renderer;
        return 0;
    }
    const std::string mode = json_string(settings, "console_mode");
    if (!mode.empty() && mode != "handheld" && mode != "docked") {
        g_error = std::string("error.bad_console_mode" TEXT_ARG) + mode;
        return 0;
    }
    options.docked = mode == "docked";
    options.rtc_start = json_number(settings, "rtc_start");
    options.log_filter = json_string(settings, "log_filter");
    options.rebuild_every_frame = json_number(settings, "rebuild_every_frame") != 0;
    options.write_back = json_string(settings, "state_pictures") != "fast";
    const std::string resolution = json_string(settings, "resolution");
    if (!resolution.empty())
        options.resolution = resolution;
    if (exists("prod.keys"))
        options.prod_keys = "prod.keys";
    if (exists("title.keys"))
        options.title_keys = "title.keys";
    if (exists("firmware.zip"))
        options.firmware = "firmware.zip";
    options.savedata = mounted(slot_first(slots, "savedata"));
    for (const std::string &name : slot_all(slots, "content"))
        options.content.push_back(mounted(name));
    return chimera_eden::boot(name, options, g_error) ? 1 : 0;
}

// Turbo (the frontend's fast-forward and seeks): off, a present is not read
// back. Nothing else changes.
ECL_EXPORT void SetRenderingEnabled(int on) {
    chimera_eden::set_rendering(on != 0);
}

// The frontend is about to take a state, between frames. Above 1x what the
// renderer drew at the larger size goes into the core's memory first
// (kept_images.cpp); at 1x there is nothing to do.
ECL_EXPORT void StateSaving(void) {
    chimera_eden::state_saving();
}

// An axis of the next frame (the frontend sets every one before each frame).
ECL_EXPORT void SetAxis(int32_t index, int32_t value) {
    if (index >= 0 && index < AXES)
        g_axes[index] = value;
}

ECL_EXPORT void FrameAdvance(uint64_t buttons) {
    for (int p = 0; p < PLAYERS; p++) {
        uint64_t eden = 0;
        for (int b = 0; b < 16; b++)
            if (buttons & (1ull << (16 * p + b)))
                eden |= 1ull << EDEN_BUTTON[b];
        chimera_eden::set_pad(p, eden, g_axes + 4 * p);
    }
    chimera_eden::set_touch((buttons >> TOUCH_BIT) & 1, g_axes[TOUCH_X], g_axes[TOUCH_Y]);
    chimera_eden::frame();
}

ECL_EXPORT uint32_t *GetVideoBgra(void) {
    int w, h;
    return reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(chimera_eden::video(w, h)));
}
ECL_EXPORT int GetVideoWidth(void) {
    int w, h;
    chimera_eden::video(w, h);
    return w;
}
ECL_EXPORT int GetVideoHeight(void) {
    int w, h;
    chimera_eden::video(w, h);
    return h;
}

ECL_EXPORT int16_t *GetAudio(void) {
    int pairs;
    return const_cast<int16_t *>(chimera_eden::audio(pairs));
}
ECL_EXPORT int GetAudioSampleCount(void) {
    int pairs;
    chimera_eden::audio(pairs);
    return pairs;
}

// No memory domain yet: the Switch's memory is not one block at one address
// for a session (the kernel maps it as the game asks).
ECL_EXPORT int GetMemoryDomainCount(void) {
    return 0;
}
ECL_EXPORT const char *GetMemoryDomainName(int32_t) {
    return "";
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int32_t) {
    return nullptr;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int32_t) {
    return 0;
}
ECL_EXPORT int GetMemoryDomainWritable(int32_t) {
    return 0;
}

ECL_EXPORT uint64_t GetFrameCount(void) {
    return chimera_eden::frame_count();
}
ECL_EXPORT uint64_t GetExitedAt(void) {
    return chimera_eden::exited_at();
}
ECL_EXPORT uint64_t GetMachineTimeNs(void) {
    return chimera_eden::frame_ended_ns();
}
ECL_EXPORT uint64_t GetCpuTicks(void) {
    return chimera_eden::cpu_ticks();
}
// a digest of every page the game has mapped (the gate's)
ECL_EXPORT uint64_t GetRamDigest(void) {
    return chimera_eden::ram_digest();
}

// What the game has saved (Emulator > Export Save Data...): the count takes
// the picture the other three read.
ECL_EXPORT int32_t GetSaveDataFileCount(void) {
    return static_cast<int32_t>(chimera_eden::savedata_snapshot());
}
ECL_EXPORT const char *GetSaveDataFileName(int32_t i) {
    return chimera_eden::savedata_name(static_cast<size_t>(i)).c_str();
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i) {
    return static_cast<int64_t>(chimera_eden::savedata_bytes(static_cast<size_t>(i)).size());
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i) {
    return chimera_eden::savedata_bytes(static_cast<size_t>(i)).data();
}

// Every file in the machine's filesystem with its size, one a line: what a
// person debugging a boot wants first.
ECL_EXPORT const char *DebugListFiles(void) {
    static std::string listing;
    listing.clear();
    for (const auto &rel : chimera::memfs::list(chimera::memfs::ROOT)) {
        std::vector<uint8_t> data;
        const std::string path = std::string(chimera::memfs::ROOT) + "/" + rel;
        chimera::memfs::get(path, data);
        listing += path + " " + std::to_string(data.size()) + "\n";
    }
    return listing.c_str();
}

// The machine's log, for a person to read.
ECL_EXPORT int64_t GetLogSize(void) {
    g_log.clear();
    chimera_eden::log(g_log);
    return static_cast<int64_t>(g_log.size());
}
ECL_EXPORT const uint8_t *GetLogBuffer(void) {
    return g_log.data();
}

} // extern "C"
