// The machine's filesystem: see memfs.h.
// SPDX-License-Identifier: MIT
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "memfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#ifndef CHIMERA_GUEST
#include <dlfcn.h>
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1 // the kernel's value; musl's headers lack it
#endif

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace {

struct node {
    bool dir = false;
    uint64_t ino = 0;
    int64_t mtime = 0;
    std::vector<uint8_t> data; // a writable file
    int host_fd = -1; // a grafted file: read through the host
    uint64_t host_size = 0;
    const uint8_t *static_data = nullptr; // a file over built-in bytes
    size_t static_size = 0;
    std::map<std::string, std::shared_ptr<node>> children; // name order
};

bool read_only(const node &n) {
    return n.host_fd >= 0 || n.static_data;
}

uint64_t size_of(const node &n) {
    if (n.host_fd >= 0)
        return n.host_size;
    if (n.static_data)
        return n.static_size;
    return n.data.size();
}

// One lock for the tree: host threads (a logger, a GL driver) use the C
// library too, and the machine's own threads are many.
std::recursive_mutex g_lock;
std::shared_ptr<node> g_root;
uint64_t g_next_ino = 1;
int64_t (*g_clock)() = nullptr;

int64_t now() {
    return g_clock ? g_clock() : 0;
}

std::shared_ptr<node> make_node(bool dir) {
    auto n = std::make_shared<node>();
    n->dir = dir;
    n->ino = g_next_ino++;
    n->mtime = now();
    return n;
}

void ensure_root() {
    if (!g_root) {
        g_root = make_node(true);
        g_root->children["chimera"] = make_node(true);
    }
}

// A path under ROOT? Absolute, and ROOT followed by nothing or a slash.
bool ours(const char *path) {
    if (!path || path[0] != '/')
        return false;
    const size_t n = strlen(chimera::memfs::ROOT);
    return strncmp(path, chimera::memfs::ROOT, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

std::vector<std::string> split(const char *path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char *p = path;; p++) {
        if (*p == '/' || *p == '\0') {
            if (cur == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!cur.empty() && cur != ".")
                parts.push_back(cur);
            cur.clear();
            if (*p == '\0')
                break;
        } else
            cur += *p;
    }
    return parts;
}

// The node at a path, or null; `parent` and `leaf` name where it would go.
std::shared_ptr<node> lookup(const char *path, std::shared_ptr<node> *parent = nullptr, std::string *leaf = nullptr) {
    ensure_root();
    const auto parts = split(path);
    std::shared_ptr<node> cur = g_root, par;
    for (size_t i = 0; i < parts.size(); i++) {
        if (!cur || !cur->dir)
            return nullptr;
        par = cur;
        if (parent && i + 1 == parts.size()) {
            *parent = par;
            if (leaf)
                *leaf = parts[i];
        }
        auto it = cur->children.find(parts[i]);
        if (it == cur->children.end())
            return nullptr;
        cur = it->second;
    }
    return cur;
}

std::string normalised(const char *path) {
    std::string out;
    for (const auto &p : split(path))
        out += "/" + p;
    return out.empty() ? "/" : out;
}

// ---- open files ----

constexpr int FD_BASE = 1 << 24;

struct open_file {
    std::shared_ptr<node> n;
    uint64_t pos = 0;
    int flags = 0;
    std::string path; // for fdopendir and *at calls
};

std::map<int, open_file> g_files;

bool is_ours(int fd) {
    return fd >= FD_BASE;
}

open_file *file_of(int fd) {
    auto it = g_files.find(fd);
    return it == g_files.end() ? nullptr : &it->second;
}

int new_fd(open_file f) {
    int fd = FD_BASE;
    while (g_files.count(fd))
        fd++;
    g_files.emplace(fd, std::move(f));
    return fd;
}

int fail(int e) {
    errno = e;
    return -1;
}

// CHIMERA_MEMFS_TRACE=1: every call on a path of ours, to stderr
bool tracing() {
    static const int on = [] {
        const char *e = getenv("CHIMERA_MEMFS_TRACE");
        return e && *e && *e != '0';
    }();
    return on != 0;
}
#define TRACE(...)                                \
    do {                                          \
        if (tracing()) {                          \
            char buf_[600];                       \
            const int n_ = snprintf(buf_, sizeof buf_, __VA_ARGS__); \
            if (n_ > 0)                           \
                syscall(SYS_write, 2, buf_, static_cast<size_t>(n_ < (int)sizeof buf_ ? n_ : (int)sizeof buf_ - 1)); \
        }                                         \
    } while (0)

void fill_stat(const node &n, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_dev = 0x6368; // "ch"
    st->st_ino = n.ino;
    st->st_nlink = n.dir ? 2 : 1;
    st->st_mode = n.dir ? (S_IFDIR | 0755) : (S_IFREG | (read_only(n) ? 0444 : 0644));
    st->st_size = static_cast<off_t>(n.dir ? 4096 : size_of(n));
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = n.mtime;
}

ssize_t node_pread(node &n, void *buf, size_t count, uint64_t off) {
    const uint64_t size = size_of(n);
    if (off >= size)
        return 0;
    const size_t len = static_cast<size_t>(std::min<uint64_t>(count, size - off));
    if (n.host_fd >= 0)
        return static_cast<ssize_t>(syscall(SYS_pread64, n.host_fd, buf, len, static_cast<long>(off)));
    if (n.static_data)
        memcpy(buf, n.static_data + off, len);
    else
        memcpy(buf, n.data.data() + off, len);
    return static_cast<ssize_t>(len);
}

ssize_t node_pwrite(node &n, const void *buf, size_t count, uint64_t off) {
    if (read_only(n))
        return fail(EBADF);
    if (off + count > n.data.size())
        n.data.resize(static_cast<size_t>(off + count));
    memcpy(n.data.data() + off, buf, count);
    n.mtime = now();
    return static_cast<ssize_t>(count);
}

int mem_open(const char *path, int flags) {
    TRACE("memfs: open %s flags=%#x\n", path, flags);
    std::lock_guard lock(g_lock);
    std::shared_ptr<node> parent;
    std::string leaf;
    auto n = lookup(path, &parent, &leaf);
    if (n && (flags & O_CREAT) && (flags & O_EXCL))
        return fail(EEXIST);
    if (!n) {
        if (!(flags & O_CREAT))
            return fail(ENOENT);
        if (!parent || !parent->dir)
            return fail(ENOENT);
        n = make_node(false);
        parent->children[leaf] = n;
        parent->mtime = now();
    }
    if (n->dir && (flags & O_ACCMODE) != O_RDONLY)
        return fail(EISDIR);
    if ((flags & O_DIRECTORY) && !n->dir)
        return fail(ENOTDIR);
    if ((flags & O_ACCMODE) != O_RDONLY && read_only(*n))
        return fail(EROFS);
    if ((flags & O_TRUNC) && !n->dir && (flags & O_ACCMODE) != O_RDONLY) {
        n->data.clear();
        n->mtime = now();
    }
    open_file f;
    f.n = n;
    f.flags = flags;
    f.path = normalised(path);
    return new_fd(std::move(f));
}

// ---- the host's own versions, for paths and descriptors not ours ----

#ifndef CHIMERA_GUEST
template <typename F>
F host(const char *name) {
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}
// the host's own, looked up once
#define HOST(name) static const auto real_##name = host<decltype(&::name)>(#name)
#endif

int host_openat(int dirfd, const char *path, int flags, mode_t mode) {
    return static_cast<int>(syscall(SYS_openat, dirfd, path, flags, mode));
}

// ---- directory streams ----

struct mem_dir {
    std::vector<struct dirent> entries;
    size_t next = 0;
    int fd = -1;
};

std::set<void *> g_dirs;

mem_dir *dir_of(DIR *d) {
    return g_dirs.count(d) ? static_cast<mem_dir *>(static_cast<void *>(d)) : nullptr;
}

// A stream over a directory; `fd` >= 0 is a descriptor of it the stream
// takes over (fdopendir), otherwise it opens its own.
DIR *mem_opendir(const char *path, int fd = -1) {
    TRACE("memfs: opendir %s\n", path);
    std::lock_guard lock(g_lock);
    auto n = lookup(path);
    if (!n) {
        errno = ENOENT;
        return nullptr;
    }
    if (!n->dir) {
        errno = ENOTDIR;
        return nullptr;
    }
    auto *d = new mem_dir;
    auto add = [&](const std::string &name, const node &c) {
        struct dirent e;
        memset(&e, 0, sizeof e);
        e.d_ino = c.ino;
        e.d_type = c.dir ? DT_DIR : DT_REG;
        e.d_reclen = sizeof e;
        snprintf(e.d_name, sizeof e.d_name, "%s", name.c_str());
        d->entries.push_back(e);
    };
    add(".", *n);
    add("..", *n);
    for (const auto &[name, c] : n->children)
        add(name, *c);
    for (size_t i = 0; i < d->entries.size(); i++)
        d->entries[i].d_off = static_cast<off_t>(i + 1);
    if (fd >= 0)
        d->fd = fd;
    else {
        open_file f;
        f.n = n;
        f.flags = O_RDONLY | O_DIRECTORY;
        f.path = normalised(path);
        d->fd = new_fd(std::move(f));
    }
    g_dirs.insert(d);
    return static_cast<DIR *>(static_cast<void *>(d));
}

// ---- stdio streams over the tree ----

std::map<FILE *, int> g_streams;

ssize_t cookie_read(void *c, char *buf, size_t size) {
    return read(static_cast<int>(reinterpret_cast<intptr_t>(c)), buf, size);
}
ssize_t cookie_write(void *c, const char *buf, size_t size) {
    return write(static_cast<int>(reinterpret_cast<intptr_t>(c)), buf, size);
}
int cookie_seek(void *c, off_t *off, int whence) {
    const off_t r = lseek(static_cast<int>(reinterpret_cast<intptr_t>(c)), *off, whence);
    if (r < 0)
        return -1;
    *off = r;
    return 0;
}
// the stream is gone: its descriptor stops answering for it
int cookie_close(void *c) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(c));
    {
        std::lock_guard lock(g_lock);
        for (auto it = g_streams.begin(); it != g_streams.end(); ++it)
            if (it->second == fd) {
                g_streams.erase(it);
                break;
            }
    }
    return close(fd);
}

int mode_flags(const char *mode) {
    int flags = 0;
    const bool plus = strchr(mode, '+') != nullptr;
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    if (strchr(mode, 'x'))
        flags |= O_EXCL;
    return flags;
}

FILE *mem_fopen(const char *path, const char *mode) {
    const int flags = mode_flags(mode);
    if (flags < 0) {
        errno = EINVAL;
        return nullptr;
    }
    const int fd = mem_open(path, flags);
    if (fd < 0)
        return nullptr;
    cookie_io_functions_t io = { cookie_read, cookie_write, cookie_seek, cookie_close };
    FILE *f = fopencookie(reinterpret_cast<void *>(static_cast<intptr_t>(fd)), mode, io);
    if (!f) {
        close(fd);
        return nullptr;
    }
    std::lock_guard lock(g_lock);
    g_streams[f] = fd;
    return f;
}

// the path for an *at call: absolute stays, relative joins its directory
std::string at_path(int dirfd, const char *path, bool &mine) {
    mine = false;
    if (!path)
        return {};
    if (path[0] == '/') {
        mine = ours(path);
        return path;
    }
    if (is_ours(dirfd)) {
        std::lock_guard lock(g_lock);
        if (auto *f = file_of(dirfd)) {
            mine = true;
            return f->path + "/" + path;
        }
    }
    return path;
}

} // namespace

// ---- the API ----

namespace chimera::memfs {

void set_clock(int64_t (*now_seconds)()) {
    std::lock_guard lock(g_lock);
    g_clock = now_seconds;
}

bool mkdirs(const std::string &path) {
    std::lock_guard lock(g_lock);
    ensure_root();
    std::shared_ptr<node> cur = g_root;
    for (const auto &p : split(path.c_str())) {
        auto &child = cur->children[p];
        if (!child)
            child = make_node(true);
        if (!child->dir)
            return false;
        cur = child;
    }
    return true;
}

static std::shared_ptr<node> new_file_at(const std::string &path) {
    const auto slash = path.rfind('/');
    if (!mkdirs(path.substr(0, slash)))
        return nullptr;
    std::shared_ptr<node> parent;
    std::string leaf;
    lookup(path.c_str(), &parent, &leaf);
    if (!parent)
        return nullptr;
    auto n = make_node(false);
    parent->children[leaf] = n;
    return n;
}

bool graft(const std::string &path, const std::string &host_name) {
    const int fd = host_openat(AT_FDCWD, host_name.c_str(), O_RDONLY, 0);
    if (fd < 0)
        return false;
    struct stat st;
    if (syscall(SYS_fstat, fd, &st) != 0) {
        syscall(SYS_close, fd);
        return false;
    }
    std::lock_guard lock(g_lock);
    auto n = new_file_at(path);
    if (!n)
        return false;
    n->host_fd = fd;
    n->host_size = static_cast<uint64_t>(st.st_size);
    return true;
}

bool graft_static(const std::string &path, const uint8_t *data, size_t size) {
    std::lock_guard lock(g_lock);
    auto n = new_file_at(path);
    if (!n)
        return false;
    n->static_data = data ? data : reinterpret_cast<const uint8_t *>("");
    n->static_size = size;
    return true;
}

bool put(const std::string &path, std::vector<uint8_t> data) {
    std::lock_guard lock(g_lock);
    auto n = new_file_at(path);
    if (!n)
        return false;
    n->data = std::move(data);
    return true;
}

bool get(const std::string &path, std::vector<uint8_t> &out) {
    std::lock_guard lock(g_lock);
    auto n = lookup(path.c_str());
    if (!n || n->dir)
        return false;
    out.resize(static_cast<size_t>(size_of(*n)));
    if (!out.empty())
        node_pread(*n, out.data(), out.size(), 0);
    return true;
}

std::vector<std::string> list(const std::string &dir) {
    std::lock_guard lock(g_lock);
    std::vector<std::string> out;
    auto n = lookup(dir.c_str());
    if (!n || !n->dir)
        return out;
    std::vector<std::pair<std::string, std::shared_ptr<node>>> stack = { { "", n } };
    while (!stack.empty()) {
        auto [prefix, cur] = stack.back();
        stack.pop_back();
        // reversed onto the stack, so they come off in name order
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it) {
            const std::string rel = prefix.empty() ? it->first : prefix + "/" + it->first;
            if (it->second->dir)
                stack.push_back({ rel, it->second });
            else
                out.push_back(rel);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace chimera::memfs

// ---- the C library ----
//
// Each serves a path or descriptor of ours from the tree and passes the rest
// to the host: natively the C library's own (dlsym RTLD_NEXT), in the sandbox
// the system call.

extern "C" {

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    bool mine;
    const std::string p = at_path(dirfd, path, mine);
    if (mine)
        return mem_open(p.c_str(), flags);
    return host_openat(dirfd, path, flags, mode);
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    if (ours(path))
        return mem_open(path, flags);
    return host_openat(AT_FDCWD, path, flags, mode);
}

#ifndef CHIMERA_GUEST
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return open(path, flags, mode);
}
#endif

#ifndef CHIMERA_GUEST
int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return openat(dirfd, path, flags, mode);
}
#endif

int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        return g_files.erase(fd) ? 0 : fail(EBADF);
    }
    return static_cast<int>(syscall(SYS_close, fd));
}

ssize_t read(int fd, void *buf, size_t count) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f)
            return fail(EBADF);
        if (f->n->dir)
            return fail(EISDIR);
        if ((f->flags & O_ACCMODE) == O_WRONLY)
            return fail(EBADF);
        const ssize_t r = node_pread(*f->n, buf, count, f->pos);
        if (r > 0)
            f->pos += static_cast<uint64_t>(r);
        return r;
    }
    return syscall(SYS_read, fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f || (f->flags & O_ACCMODE) == O_RDONLY)
            return fail(EBADF);
        if (f->flags & O_APPEND)
            f->pos = size_of(*f->n);
        const ssize_t r = node_pwrite(*f->n, buf, count, f->pos);
        if (r > 0)
            f->pos += static_cast<uint64_t>(r);
        return r;
    }
    return syscall(SYS_write, fd, buf, count);
}

// libstdc++'s file streams write a buffer and the next piece in one writev
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (is_ours(fd)) {
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (iov[i].iov_len == 0)
                continue;
            const ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
            if (r < 0)
                return total ? total : r;
            total += r;
        }
        return total;
    }
    return syscall(SYS_writev, fd, iov, iovcnt);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    if (is_ours(fd)) {
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            const ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
            if (r < 0)
                return total ? total : r;
            total += r;
            if (static_cast<size_t>(r) < iov[i].iov_len)
                break;
        }
        return total;
    }
    return syscall(SYS_readv, fd, iov, iovcnt);
}

ssize_t pread(int fd, void *buf, size_t count, off_t off) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f)
            return fail(EBADF);
        return node_pread(*f->n, buf, count, static_cast<uint64_t>(off));
    }
    return syscall(SYS_pread64, fd, buf, count, off);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t off) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f || (f->flags & O_ACCMODE) == O_RDONLY)
            return fail(EBADF);
        return node_pwrite(*f->n, buf, count, static_cast<uint64_t>(off));
    }
    return syscall(SYS_pwrite64, fd, buf, count, off);
}

#ifndef CHIMERA_GUEST
ssize_t pread64(int fd, void *buf, size_t count, off_t off) {
    return pread(fd, buf, count, off);
}
ssize_t pwrite64(int fd, const void *buf, size_t count, off_t off) {
    return pwrite(fd, buf, count, off);
}
#endif

off_t lseek(int fd, off_t off, int whence) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f)
            return fail(EBADF);
        int64_t base = 0;
        if (whence == SEEK_CUR)
            base = static_cast<int64_t>(f->pos);
        else if (whence == SEEK_END)
            base = static_cast<int64_t>(size_of(*f->n));
        else if (whence != SEEK_SET)
            return fail(EINVAL);
        if (base + off < 0)
            return fail(EINVAL);
        f->pos = static_cast<uint64_t>(base + off);
        return static_cast<off_t>(f->pos);
    }
    return static_cast<off_t>(syscall(SYS_lseek, fd, off, whence));
}

#ifndef CHIMERA_GUEST
off_t lseek64(int fd, off_t off, int whence) {
    return lseek(fd, off, whence);
}
#endif

int fstat(int fd, struct stat *st) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f)
            return fail(EBADF);
        fill_stat(*f->n, st);
        return 0;
    }
    return static_cast<int>(syscall(SYS_fstat, fd, st));
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    if (path && path[0] == '\0' && (flags & AT_EMPTY_PATH))
        return fstat(dirfd, st);
    bool mine;
    const std::string p = at_path(dirfd, path, mine);
    if (mine) {
        std::lock_guard lock(g_lock);
        auto n = lookup(p.c_str());
        TRACE("memfs: stat %s -> %s\n", p.c_str(), n ? (n->dir ? "dir" : "file") : "none");
        if (!n)
            return fail(ENOENT);
        fill_stat(*n, st);
        return 0;
    }
    return static_cast<int>(syscall(SYS_newfstatat, dirfd, path, st, flags));
}

int stat(const char *path, struct stat *st) {
    return fstatat(AT_FDCWD, path, st, 0);
}
int lstat(const char *path, struct stat *st) {
    return fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}
#ifndef CHIMERA_GUEST
// glibc's large-file names (musl's are macros for the plain ones); on x86-64
// struct stat64 is struct stat
static_assert(sizeof(struct stat64) == sizeof(struct stat), "stat64 and stat differ");
int fstat64(int fd, struct stat64 *st) {
    return fstat(fd, reinterpret_cast<struct stat *>(st));
}
int stat64(const char *path, struct stat64 *st) {
    return stat(path, reinterpret_cast<struct stat *>(st));
}
int lstat64(const char *path, struct stat64 *st) {
    return lstat(path, reinterpret_cast<struct stat *>(st));
}
int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags) {
    return fstatat(dirfd, path, reinterpret_cast<struct stat *>(st), flags);
}
#endif

#ifdef STATX_BASIC_STATS
// (glibc's; the sandbox's musl has no statx, and its Boost is built without)
int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *stx) {
    bool mine = false;
    std::string p;
    int fd = -1;
    if (path && path[0] == '\0' && (flags & AT_EMPTY_PATH)) {
        mine = is_ours(dirfd);
        fd = dirfd;
    } else
        p = at_path(dirfd, path, mine);
    if (mine) {
        struct stat st;
        const int r = fd >= 0 ? fstat(fd, &st) : fstatat(AT_FDCWD, p.c_str(), &st, 0);
        if (r != 0)
            return r;
        memset(stx, 0, sizeof *stx);
        stx->stx_mask = STATX_BASIC_STATS;
        stx->stx_blksize = static_cast<uint32_t>(st.st_blksize);
        stx->stx_nlink = static_cast<uint32_t>(st.st_nlink);
        stx->stx_mode = static_cast<uint16_t>(st.st_mode);
        stx->stx_ino = st.st_ino;
        stx->stx_size = static_cast<uint64_t>(st.st_size);
        stx->stx_blocks = static_cast<uint64_t>(st.st_blocks);
        stx->stx_atime.tv_sec = stx->stx_mtime.tv_sec = stx->stx_ctime.tv_sec = stx->stx_btime.tv_sec = st.st_mtim.tv_sec;
        stx->stx_dev_major = 0x63;
        stx->stx_dev_minor = 0x68;
        return 0;
    }
    (void)mask;
    return static_cast<int>(syscall(SYS_statx, dirfd, path, flags, mask, stx));
}
#endif

int faccessat(int dirfd, const char *path, int mode, int flags) {
    bool mine;
    const std::string p = at_path(dirfd, path, mine);
    if (mine) {
        std::lock_guard lock(g_lock);
        auto n = lookup(p.c_str());
        if (!n)
            return fail(ENOENT);
        if ((mode & W_OK) && read_only(*n))
            return fail(EROFS);
        return 0;
    }
    return static_cast<int>(syscall(SYS_faccessat, dirfd, path, mode, flags));
}

int access(const char *path, int mode) {
    return faccessat(AT_FDCWD, path, mode, 0);
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    bool mine;
    const std::string p = at_path(dirfd, path, mine);
    if (mine) {
        std::lock_guard lock(g_lock);
        std::shared_ptr<node> parent;
        std::string leaf;
        TRACE("memfs: mkdir %s\n", p.c_str());
        if (lookup(p.c_str(), &parent, &leaf))
            return fail(EEXIST);
        if (!parent || !parent->dir)
            return fail(ENOENT);
        parent->children[leaf] = make_node(true);
        parent->mtime = now();
        return 0;
    }
    return static_cast<int>(syscall(SYS_mkdirat, dirfd, path, mode));
}

int mkdir(const char *path, mode_t mode) {
    return mkdirat(AT_FDCWD, path, mode);
}

int unlinkat(int dirfd, const char *path, int flags) {
    bool mine;
    const std::string p = at_path(dirfd, path, mine);
    if (mine) {
        std::lock_guard lock(g_lock);
        std::shared_ptr<node> parent;
        std::string leaf;
        auto n = lookup(p.c_str(), &parent, &leaf);
        if (!n || !parent)
            return fail(ENOENT);
        if (flags & AT_REMOVEDIR) {
            if (!n->dir)
                return fail(ENOTDIR);
            if (!n->children.empty())
                return fail(ENOTEMPTY);
        } else if (n->dir)
            return fail(EISDIR);
        parent->children.erase(leaf);
        parent->mtime = now();
        return 0;
    }
    return static_cast<int>(syscall(SYS_unlinkat, dirfd, path, flags));
}

int unlink(const char *path) {
    return unlinkat(AT_FDCWD, path, 0);
}
int rmdir(const char *path) {
    return unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

// remove() is the C library's "delete this, file or folder". The library's own
// version makes the unlink system call directly rather than calling unlink()
// above, so a file kept in this file system was asked of the sandbox instead -
// which has no such call, and stops the core. std::filesystem::remove goes
// through it, and so does every file a game deletes (the save a game replaces
// when it writes its settings: a game died there).
int remove(const char *path) {
    const int r = unlink(path);
    if (r != 0 && errno == EISDIR)
        return rmdir(path);
    return r;
}

int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags) {
    bool mine_old, mine_new;
    const std::string op = at_path(olddirfd, oldpath, mine_old);
    const std::string np = at_path(newdirfd, newpath, mine_new);
    if (mine_old || mine_new) {
        if (mine_old != mine_new)
            return fail(EXDEV);
        std::lock_guard lock(g_lock);
        std::shared_ptr<node> op_parent, np_parent;
        std::string op_leaf, np_leaf;
        auto n = lookup(op.c_str(), &op_parent, &op_leaf);
        if (!n || !op_parent)
            return fail(ENOENT);
        auto target = lookup(np.c_str(), &np_parent, &np_leaf);
        if (!np_parent || !np_parent->dir)
            return fail(ENOENT);
        if (target && (flags & RENAME_NOREPLACE))
            return fail(EEXIST);
        if (target && target->dir && !target->children.empty())
            return fail(ENOTEMPTY);
        op_parent->children.erase(op_leaf);
        np_parent->children[np_leaf] = n;
        op_parent->mtime = np_parent->mtime = now();
        return 0;
    }
    return static_cast<int>(syscall(SYS_renameat2, olddirfd, oldpath, newdirfd, newpath, flags));
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    return renameat2(olddirfd, oldpath, newdirfd, newpath, 0);
}
int rename(const char *oldpath, const char *newpath) {
    return renameat2(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

int ftruncate(int fd, off_t len) {
    if (is_ours(fd)) {
        std::lock_guard lock(g_lock);
        auto *f = file_of(fd);
        if (!f || read_only(*f->n) || f->n->dir)
            return fail(EBADF);
        f->n->data.resize(static_cast<size_t>(len));
        f->n->mtime = now();
        return 0;
    }
    return static_cast<int>(syscall(SYS_ftruncate, fd, len));
}

int truncate(const char *path, off_t len) {
    if (ours(path)) {
        const int fd = mem_open(path, O_WRONLY);
        if (fd < 0)
            return -1;
        const int r = ftruncate(fd, len);
        close(fd);
        return r;
    }
    return static_cast<int>(syscall(SYS_truncate, path, len));
}

#ifndef CHIMERA_GUEST
int ftruncate64(int fd, off_t len) {
    return ftruncate(fd, len);
}
int truncate64(const char *path, off_t len) {
    return truncate(path, len);
}
#endif

// libstdc++'s std::filesystem::copy_file copies between two descriptors
// with these; either of ours is a copy through read and write, which serve
// both kinds of descriptor
namespace {
ssize_t copy_fds(int in, off64_t *in_off, int out, off64_t *out_off, size_t count) {
    char buf[65536];
    size_t done = 0;
    while (done < count) {
        const size_t want = std::min(sizeof buf, count - done);
        const ssize_t got = in_off ? pread(in, buf, want, *in_off) : read(in, buf, want);
        if (got < 0)
            return done ? static_cast<ssize_t>(done) : -1;
        if (got == 0)
            break;
        for (ssize_t put = 0; put < got;) {
            const ssize_t w = out_off ? pwrite(out, buf + put, got - put, *out_off) : write(out, buf + put, got - put);
            if (w <= 0)
                return done ? static_cast<ssize_t>(done) : -1;
            put += w;
            if (out_off)
                *out_off += w;
        }
        if (in_off)
            *in_off += got;
        done += static_cast<size_t>(got);
    }
    return static_cast<ssize_t>(done);
}
} // namespace

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    if (is_ours(out_fd) || is_ours(in_fd)) {
        off64_t off = offset ? *offset : 0;
        const ssize_t n = copy_fds(in_fd, offset ? &off : nullptr, out_fd, nullptr, count);
        if (offset)
            *offset = static_cast<off_t>(off);
        return n;
    }
    return static_cast<ssize_t>(syscall(SYS_sendfile, out_fd, in_fd, offset, count));
}

#ifndef CHIMERA_GUEST
// glibc's name for the same call (musl's off_t is 64-bit already)
ssize_t sendfile64(int out_fd, int in_fd, off64_t *offset, size_t count) {
    if (is_ours(out_fd) || is_ours(in_fd))
        return copy_fds(in_fd, offset, out_fd, nullptr, count);
    return static_cast<ssize_t>(syscall(SYS_sendfile, out_fd, in_fd, offset, count));
}
#endif

ssize_t copy_file_range(int fd_in, off64_t *off_in, int fd_out, off64_t *off_out, size_t len, unsigned int flags) {
    if (is_ours(fd_in) || is_ours(fd_out))
        return copy_fds(fd_in, off_in, fd_out, off_out, len);
#ifdef SYS_copy_file_range
    return static_cast<ssize_t>(syscall(SYS_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags));
#else
    return fail(ENOSYS);
#endif
}

// Advice about how a file will be read (libstdc++'s copy_file gives it):
// nothing to act on in the tree, and the sandbox has no such call at all.
int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
#ifdef CHIMERA_GUEST
    return 0;
#else
    if (is_ours(fd))
        return 0;
    return syscall(SYS_fadvise64, fd, offset, len, advice) == 0 ? 0 : errno;
#endif
}

#ifndef CHIMERA_GUEST
int posix_fadvise64(int fd, off64_t offset, off64_t len, int advice) {
    return posix_fadvise(fd, offset, len, advice);
}
#endif

int fsync(int fd) {
    return is_ours(fd) ? 0 : static_cast<int>(syscall(SYS_fsync, fd));
}
int fdatasync(int fd) {
    return is_ours(fd) ? 0 : static_cast<int>(syscall(SYS_fdatasync, fd));
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    bool mine;
    const std::string p = path ? at_path(dirfd, path, mine) : std::string();
    if (path ? mine : is_ours(dirfd))
        return 0; // a file's time is the machine's, when it is written
    return static_cast<int>(syscall(SYS_utimensat, dirfd, path, times, flags));
}

int futimens(int fd, const struct timespec times[2]) {
    return utimensat(fd, nullptr, times, 0);
}

int chmod(const char *path, mode_t mode) {
    return ours(path) ? 0 : static_cast<int>(syscall(SYS_chmod, path, mode));
}
int fchmod(int fd, mode_t mode) {
    return is_ours(fd) ? 0 : static_cast<int>(syscall(SYS_fchmod, fd, mode));
}

ssize_t readlink(const char *path, char *buf, size_t size) {
    if (ours(path))
        return fail(EINVAL); // nothing here is a link
    return syscall(SYS_readlink, path, buf, size);
}

char *realpath(const char *path, char *resolved) {
    if (ours(path)) {
        std::lock_guard lock(g_lock);
        if (!lookup(path)) {
            errno = ENOENT;
            return nullptr;
        }
        const std::string n = normalised(path);
        char *out = resolved ? resolved : static_cast<char *>(malloc(PATH_MAX));
        snprintf(out, PATH_MAX, "%s", n.c_str());
        return out;
    }
#ifndef CHIMERA_GUEST
    HOST(realpath);
    return real_realpath(path, resolved);
#else
    errno = ENOENT;
    return nullptr;
#endif
}

int statvfs(const char *path, struct statvfs *buf) {
    if (ours(path)) {
        // a memory card's worth of room, always
        memset(buf, 0, sizeof *buf);
        buf->f_bsize = buf->f_frsize = 4096;
        buf->f_blocks = buf->f_bfree = buf->f_bavail = (16ull << 30) / 4096;
        buf->f_files = buf->f_ffree = buf->f_favail = 1u << 20;
        buf->f_namemax = 255;
        return 0;
    }
#ifndef CHIMERA_GUEST
    HOST(statvfs);
    return real_statvfs(path, buf);
#else
    return fail(ENOSYS);
#endif
}

#ifdef CHIMERA_GUEST
// The C library keeps statvfs, fstatvfs, statfs and fstatfs in one object:
// whoever defines one defines the four (std::filesystem asks fstatvfs).
int fstatvfs(int, struct statvfs *buf) {
    memset(buf, 0, sizeof *buf);
    buf->f_bsize = buf->f_frsize = 4096;
    buf->f_blocks = buf->f_bfree = buf->f_bavail = (16ull << 30) / 4096;
    buf->f_files = buf->f_ffree = buf->f_favail = 1u << 20;
    buf->f_namemax = 255;
    return 0;
}
int statfs(const char *, struct statfs *) {
    return fail(ENOSYS);
}
int fstatfs(int, struct statfs *) {
    return fail(ENOSYS);
}
#endif

DIR *opendir(const char *path) {
    if (ours(path))
        return mem_opendir(path);
#ifndef CHIMERA_GUEST
    HOST(opendir);
    return real_opendir(path);
#else
    errno = ENOENT;
    return nullptr;
#endif
}

DIR *fdopendir(int fd) {
    if (is_ours(fd)) {
        std::string path;
        {
            std::lock_guard lock(g_lock);
            auto *f = file_of(fd);
            if (!f) {
                errno = EBADF;
                return nullptr;
            }
            path = f->path;
        }
        // the stream takes the descriptor over, as POSIX has it: dirfd()
        // answers it, and closedir() closes it
        return mem_opendir(path.c_str(), fd);
    }
#ifndef CHIMERA_GUEST
    HOST(fdopendir);
    return real_fdopendir(fd);
#else
    errno = EBADF;
    return nullptr;
#endif
}

struct dirent *readdir(DIR *d) {
    {
        std::lock_guard lock(g_lock);
        if (auto *m = dir_of(d))
            return m->next < m->entries.size() ? &m->entries[m->next++] : nullptr;
    }
#ifndef CHIMERA_GUEST
    HOST(readdir);
    return real_readdir(d);
#else
    errno = EBADF;
    return nullptr;
#endif
}

#ifndef CHIMERA_GUEST
struct dirent64 *readdir64(DIR *d) {
    static_assert(sizeof(struct dirent) == sizeof(struct dirent64), "dirent and dirent64 differ");
    return reinterpret_cast<struct dirent64 *>(readdir(d));
}
#endif

int closedir(DIR *d) {
    {
        std::lock_guard lock(g_lock);
        if (auto *m = dir_of(d)) {
            g_files.erase(m->fd);
            g_dirs.erase(d);
            delete m;
            return 0;
        }
    }
#ifndef CHIMERA_GUEST
    HOST(closedir);
    return real_closedir(d);
#else
    return fail(EBADF);
#endif
}

void rewinddir(DIR *d) {
    {
        std::lock_guard lock(g_lock);
        if (auto *m = dir_of(d)) {
            m->next = 0;
            return;
        }
    }
#ifndef CHIMERA_GUEST
    HOST(rewinddir);
    real_rewinddir(d);
#endif
}

int dirfd(DIR *d) {
    {
        std::lock_guard lock(g_lock);
        if (auto *m = dir_of(d))
            return m->fd;
    }
#ifndef CHIMERA_GUEST
    HOST(dirfd);
    return real_dirfd(d);
#else
    return fail(EBADF);
#endif
}

FILE *fopen(const char *path, const char *mode) {
    if (ours(path))
        return mem_fopen(path, mode);
#ifndef CHIMERA_GUEST
    HOST(fopen);
    return real_fopen(path, mode);
#else
    const int flags = mode_flags(mode);
    if (flags < 0) {
        errno = EINVAL;
        return nullptr;
    }
    const int fd = host_openat(AT_FDCWD, path, flags, 0644);
    if (fd < 0)
        return nullptr;
    return fdopen(fd, mode);
#endif
}

#ifndef CHIMERA_GUEST
FILE *fopen64(const char *path, const char *mode) {
    return fopen(path, mode);
}
#endif

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (path && ours(path)) {
        fclose(stream);
        return mem_fopen(path, mode);
    }
#ifndef CHIMERA_GUEST
    HOST(freopen);
    return real_freopen(path, mode, stream);
#else
    errno = ENOSYS;
    return nullptr;
#endif
}

// libstdc++'s file streams fopen a file and then read and write its
// descriptor: a stream over the tree answers with the tree's descriptor.
int fileno(FILE *stream) {
    {
        std::lock_guard lock(g_lock);
        auto it = g_streams.find(stream);
        if (it != g_streams.end())
            return it->second;
    }
#ifndef CHIMERA_GUEST
    HOST(fileno);
    return real_fileno(stream);
#else
    if (stream == stdin)
        return 0;
    if (stream == stdout)
        return 1;
    if (stream == stderr)
        return 2;
    return fail(EBADF);
#endif
}

#ifndef CHIMERA_GUEST
// glibc's _FORTIFY_SOURCE entry points: code built with a distribution's
// default flags (Boost's b2 build) calls these in place of the plain names.
int __open_2(const char *path, int flags) {
    return open(path, flags);
}
int __open64_2(const char *path, int flags) {
    return open(path, flags);
}
int __openat_2(int dirfd, const char *path, int flags) {
    return openat(dirfd, path, flags);
}
int __openat64_2(int dirfd, const char *path, int flags) {
    return openat(dirfd, path, flags);
}
ssize_t __read_chk(int fd, void *buf, size_t count, size_t) {
    return read(fd, buf, count);
}
ssize_t __pread_chk(int fd, void *buf, size_t count, off_t off, size_t) {
    return pread(fd, buf, count, off);
}
ssize_t __pread64_chk(int fd, void *buf, size_t count, off_t off, size_t) {
    return pread(fd, buf, count, off);
}
ssize_t __readlink_chk(const char *path, char *buf, size_t len, size_t) {
    return readlink(path, buf, len);
}
char *__realpath_chk(const char *path, char *resolved, size_t) {
    return realpath(path, resolved);
}
#endif

} // extern "C"
