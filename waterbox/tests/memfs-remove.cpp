// Deleting a file or an empty folder kept in the core's in-memory file system,
// through the C library's remove() and through std::filesystem::remove (what
// Eden's file layer calls when a game deletes a file).
//
// The C library's remove() makes the unlink system call itself instead of
// calling unlink(), so without memfs's own remove() the delete went to the
// host - or, in the sandbox, to a system call that does not exist, which
// stopped the core. Built and run natively by run-gate.sh.
// SPDX-License-Identifier: MIT
#include "../driver/memfs.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>

#define CHECK(what) \
    do { \
        if (!(what)) { \
            std::printf("memfs remove: FAILED at line %d: %s (errno %d)\n", __LINE__, #what, errno); \
            return 1; \
        } \
    } while (0)

int main() {
    using namespace chimera::memfs;
    CHECK(mkdirs("/chimera/save/folder"));
    CHECK(put("/chimera/save/a.bin", { 1, 2, 3 }));
    CHECK(put("/chimera/save/b.bin", { 4 }));
    struct stat st {};

    // the C library's remove(), on a file
    CHECK(std::remove("/chimera/save/a.bin") == 0);
    CHECK(stat("/chimera/save/a.bin", &st) != 0 && errno == ENOENT);

    // std::filesystem::remove, on a file
    std::error_code ec;
    CHECK(std::filesystem::remove("/chimera/save/b.bin", ec) && !ec);
    CHECK(!std::filesystem::exists("/chimera/save/b.bin"));

    // on an empty folder
    CHECK(std::remove("/chimera/save/folder") == 0);
    CHECK(stat("/chimera/save/folder", &st) != 0 && errno == ENOENT);

    // something that is not there: an error from remove(), "nothing removed" from std::filesystem
    CHECK(std::remove("/chimera/save/none") != 0 && errno == ENOENT);
    CHECK(!std::filesystem::remove("/chimera/save/none", ec) && !ec);

    // a folder that still holds a file is refused
    CHECK(put("/chimera/save/kept.bin", { 5 }));
    CHECK(std::remove("/chimera/save") != 0 && errno == ENOTEMPTY);
    std::vector<uint8_t> kept;
    CHECK(get("/chimera/save/kept.bin", kept) && kept.size() == 1);

    std::puts("memfs remove: ok");
    return 0;
}
