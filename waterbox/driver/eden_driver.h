// The core's driver: Eden's libraries booted and stepped a frame at a time.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// A text a person reads is a key of languages/en.json, each of its
// arguments after a TEXT_ARG (Chimera's docs/translations.md). TEXT_ARG is a
// string of its own so the byte after it is never read as more of its hex.
#define TEXT_ARG "\x1f"

namespace chimera_eden {

struct Options {
    // null: no picture; opengl, vulkan: through the GPU bridge of that name
    std::string renderer = "opengl";
    // The console's keys and its system software, as the frontend provides
    // them: paths natively, mounted files' names in the sandbox. prod.keys
    // is required; title.keys and the firmware (a zip of its .nca files)
    // are not.
    std::string prod_keys, title_keys, firmware;
    // What the game already saved, a zip (optional): see install_savedata
    std::string savedata;
    // The game's updates and DLC (optional): .nsp files, read in place and
    // registered as Eden's own frontend registers what its game list finds
    std::vector<std::string> content;
    // The game's mods (optional): .pchtxt and .ips patches and zips of a
    // mod's folder, installed where Eden looks for them (see install_mods)
    std::vector<std::string> mods;
    // Unix seconds the machine's calendar starts at; 0: 2017-03-03 (the
    // console's launch day)
    uint64_t rtc_start = 0;
    // docked to a television (1920x1080 for the games that draw it) or in
    // the hand (1280x720)
    bool docked = false;
    // the gate's diagnostics: Eden's log filter ("*:Info")
    std::string log_filter;
    // What a state holds of the picture. True: at every frame's end
    // everything the GPU drew is written into the machine's memory, so a
    // state loaded later shows exactly what was there. False: nothing is
    // (upstream's way - the game still reads back whatever it asks for),
    // which costs nothing a frame; after a load the picture is wrong until
    // the game has drawn it again, a frame or two for what it draws every
    // frame and longer for what it drew once and kept. The game itself runs
    // the same either way.
    bool write_back = true;
    // the internal resolution: "1x" (the console's own), "1.5x", "2x", "3x"
    std::string resolution = "1x";
    // the gate's: the renderer is made again before every frame, as after a
    // state load, with no load - the rebuild alone must change no picture
    bool rebuild_every_frame = false;
};

// Boots the game (an .nsp/.xci/.nro: a path natively, a mounted file's name in
// the sandbox). False with `error` set on failure.
bool boot(const std::string &game, const Options &options, std::string &error);
// Runs the machine to the end of the next frame (a sixtieth of a second of
// its own time).
void frame();
uint64_t frame_count();
// the machine's clock when the last frame's end was noticed, and the CPU's
// tick counter now
uint64_t frame_ended_ns();
uint64_t cpu_ticks();
// A player's pad from now on: buttons by Eden's NativeButton order (A, B, X,
// Y, LStick, RStick, L, R, ZL, ZR, Plus, Minus, DLeft, DUp, DRight, DDown),
// the two sticks as -32768..32767 (right and up positive).
void set_pad(int player, uint64_t buttons, const int32_t axes[4]);
// The touch screen from now on: one finger, at 0..65535 across and down.
void set_touch(bool down, int32_t x, int32_t y);
// the newest presented picture: BGRA, top row first (black before the first)
const uint8_t *video(int &width, int &height);
// natively the GL context is a real one held by one thread at a time
void set_gl_thread_hooks(void (*bind)(), void (*release)());
// Turbo: off, a present is not read back (the GPU still draws).
void set_rendering(bool on);
// A state is about to be taken (the frontend's StateSaving): above 1x the
// images drawn at the larger size are copied into the core's memory.
void state_saving();
// the frame in which the game exited, or 0
uint64_t exited_at();
// the machine's log
void log(std::vector<uint8_t> &out);
// the last frame's sound: 48 kHz stereo pairs, interleaved
const int16_t *audio(int &pairs);
// a digest of every page the application has mapped
uint64_t ram_digest();
// pictures presented so far, and the machine's time
uint64_t frames();
uint64_t time_ns();
// What the game has saved, for the frontend's export: savedata_snapshot()
// collects every file of the save data area and returns how many; a name is
// "<TITLE ID>/<path>" for the user's saves and "device/<TITLE ID>/<path>"
// for the console's.
size_t savedata_snapshot();
const std::string &savedata_name(size_t i);
const std::vector<uint8_t> &savedata_bytes(size_t i);
void shutdown();
// every thread of the game: state, wait reason, pc (for debugging a stall)
void dump_threads(FILE *out);
// the log's writer stopped and its file complete
void flush_log();

} // namespace chimera_eden
