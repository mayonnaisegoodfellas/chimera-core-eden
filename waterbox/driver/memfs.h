// The machine's filesystem: everything Eden keeps - the console's own
// NAND, its config, caches and logs - lives in an in-memory tree under
// /chimera, the same in both flavours of the core. The sandbox has no
// directories and lets a guest create nothing, and a host disk would hand the
// machine the host's timestamps and directory order; this tree has neither.
// A file's time is the machine's calendar, a listing is in name order, and an
// inode number is the order the file was made in.
//
// It sits under the C library: open, stat, opendir, fopen and the rest are
// defined here and serve every path under /chimera from the tree, passing any
// other path to the host. Files the frontend provides (the game) are grafted
// in read-only and read through one descriptor opened when they are grafted,
// before the machine is sealed.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chimera::memfs {

inline constexpr const char *ROOT = "/chimera";

// The tree's clock, in Unix seconds (the machine's calendar). Until set,
// every time is 0.
void set_clock(int64_t (*now_seconds)());

// Make a directory and its parents (an absolute path under ROOT).
bool mkdirs(const std::string &path);

// A read-only file read through the host: `host_name` is a path natively and
// a mounted file's name in the sandbox. Opened now.
bool graft(const std::string &path, const std::string &host_name);

// A read-only file over bytes that outlive the tree (data built into the
// core).
bool graft_static(const std::string &path, const uint8_t *data, size_t size);

// A writable file with these contents.
bool put(const std::string &path, std::vector<uint8_t> data);

// What a writable file holds now, or false if there is none.
bool get(const std::string &path, std::vector<uint8_t> &out);

// Every regular file under a directory, in name order (relative paths).
std::vector<std::string> list(const std::string &dir);

} // namespace chimera::memfs
