// The pictures the renderer draws above the console's resolution, kept where
// a savestate holds them.
//
// Above 1x an image the GPU draws into is a larger texture on the host's
// card, and the console's memory only gets it scaled down at every frame's
// end. A load makes the renderer again (the GL context is another one), and
// the new renderer would make its images from that memory as the game uses
// them: other pixels than the ones that were drawn, scaled up again; images
// that are not upscaled yet, since the cache upscales a render target only
// after it has been one for two frames; and none at all for the picture the
// screen shows first, which then comes from the console's memory at 1x. The
// run after a load would be another run.
//
// So before a state is taken (the StateSaving export) the texture cache hands
// over every image whose scaling a load would lose (patch 0009,
// texture_cache/chimera_kept.h), and this file copies what the cache knew of
// each - its description, where it is, its scale rating, its place in the
// cache's order - and, for the upscaled ones, the larger texture's pixels
// into a block of the core's own memory, which the state carries. Right after
// a load makes the renderer again, its cache makes those images again, in the
// same order, each as it was, before anything draws.
//
// The block is laid out to change little from one state to the next, because
// a state is stored as what changed: an image keeps its place in the block for
// as long as it lives, its pixels are read again only when the cache counted
// a change to them, and only the pages that differ are written.
//
// Nothing here allocates: taking a state must leave the machine as it would
// have been had none been taken, the heap included. The work lists live in a
// scratch block no state carries.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "kept_images.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "video_core/texture_cache/chimera_kept.h"

namespace chimera_eden::kept_images {

namespace {

namespace K = VideoCommon::ChimeraKept;

constexpr uint64_t kMagic = 0x32474D4950454B45ull; // "EKEPIMG2"
constexpr size_t kPage = 4096;
// the block: a header and the directory, then the pixels
constexpr size_t kDirectoryBytes = 1 << 20;
// the scratch block: the images found, the directory being written, the
// places taken, which to read, rows read from the card, and the console's
// memory read for a digest
constexpr size_t kMost = 2048;
constexpr size_t kRowsBytes = 4 << 20;
constexpr size_t kHashBytes = 1 << 20;

struct Header {
    uint64_t magic;
    // the renderer that wrote it (renderer_made_again counts them)
    uint64_t generation;
    // the cache's counters when the state was taken
    uint64_t frame_tick;
    uint64_t modification_tick;
    uint32_t count;
    uint32_t reserved;
    // images the block had no room for
    uint64_t left_out;
};

struct Entry {
    K::Record record;
    // of its pixels from the block's start (0: none kept), and how many
    uint64_t offset;
    uint64_t bytes;
};
static_assert(sizeof(Header) + kMost * sizeof(Entry) <= kDirectoryBytes);

struct Taken {
    uint64_t offset, bytes;
};

constexpr size_t kScratchBytes =
    kMost * sizeof(K::Found) + kDirectoryBytes + kMost * sizeof(Taken) + kMost + kRowsBytes + kHashBytes;

uint8_t *g_block = nullptr;
size_t g_block_bytes = 0;
K::Found *g_found = nullptr;
uint8_t *g_dir = nullptr;
Taken *g_taken = nullptr;
uint8_t *g_read = nullptr;
uint8_t *g_rows = nullptr;
// In the machine's memory, so a state carries it: a load makes the renderer
// again and counts one more, and the block then holds another renderer's.
uint64_t g_generation = 1;
bool g_said_full = false;
// CHIMERA_EDEN_KEPT_TRACE: what each state kept and each renderer took back
bool g_trace = false;

uint64_t page_up(uint64_t n) {
    return (n + kPage - 1) & ~static_cast<uint64_t>(kPage - 1);
}

// Writes only the pages that differ: a page written is a page in the next
// state's delta, whatever it was written with.
void put(uint8_t *to, const uint8_t *from, size_t n) {
    while (n) {
        const size_t chunk = std::min(n, kPage - (reinterpret_cast<uintptr_t>(to) & (kPage - 1)));
        if (std::memcmp(to, from, chunk) != 0)
            std::memcpy(to, from, chunk);
        to += chunk;
        from += chunk;
        n -= chunk;
    }
}

const Header &header() {
    return *reinterpret_cast<const Header *>(g_block);
}

const Entry *entries() {
    return reinterpret_cast<const Entry *>(g_block + sizeof(Header));
}

// the same image: where it is and what it is
bool same_image(const K::Record &a, const K::Record &b) {
    return a.gpu_addr == b.gpu_addr && a.cpu_addr == b.cpu_addr && a.channel == b.channel
        && a.info.format == b.info.format && a.info.type == b.info.type && a.info.size == b.info.size
        && a.info.resources.levels == b.info.resources.levels && a.info.resources.layers == b.info.resources.layers
        && a.info.num_samples == b.info.num_samples;
}

// The first gap between the places taken (sorted by offset) that holds
// `bytes`, which is then taken; 0: none.
uint64_t take(size_t &count, uint64_t bytes) {
    uint64_t at = kDirectoryBytes;
    size_t i = 0;
    for (; i < count; i++) {
        if (g_taken[i].offset - at >= bytes)
            break;
        at = g_taken[i].offset + g_taken[i].bytes;
    }
    if (i == count && g_block_bytes - at < bytes)
        return 0;
    std::memmove(g_taken + i + 1, g_taken + i, (count - i) * sizeof(Taken));
    g_taken[i] = { at, bytes };
    count++;
    return at;
}

void read_pixels(const K::Found &f, uint8_t *to) {
    for (int32_t level = 0; level < f.record.info.resources.levels; level++) {
        const uint32_t l = static_cast<uint32_t>(level);
        const size_t row = static_cast<size_t>(f.width(f.image, l)) * f.bytes_per_pixel;
        const uint32_t height = f.height(f.image, l);
        const uint32_t strip = static_cast<uint32_t>(std::max<size_t>(1, kRowsBytes / row));
        for (int32_t layer = 0; layer < f.record.info.resources.layers; layer++) {
            for (uint32_t y = 0; y < height; y += strip) {
                const uint32_t n = std::min(strip, height - y);
                f.read(f.image, l, static_cast<uint32_t>(layer), y, n, g_rows, n * row);
                put(to, g_rows, n * row);
                to += n * row;
            }
        }
    }
}

void keep_found(const K::Found *found, size_t count, size_t left_out, u64 frame_tick, u64 modification_tick) {
    const bool same_renderer = header().magic == kMagic && header().generation == g_generation;
    const Entry *old = entries();
    const uint32_t old_count = header().magic == kMagic ? header().count : 0;

    auto *out = reinterpret_cast<Entry *>(g_dir + sizeof(Header));
    size_t taken = 0;
    for (size_t i = 0; i < count; i++) {
        const K::Found &f = found[i];
        Entry &e = out[i];
        std::memset(&e, 0, sizeof e);
        e.record = f.record;
        e.bytes = f.bytes;
        g_read[i] = e.bytes != 0;
        if (!e.bytes)
            continue;
        // its place before, when the same image had one; its pixels are read
        // again unless the same renderer counted no change to them
        for (uint32_t j = 0; j < old_count; j++) {
            const Entry &o = old[j];
            if (o.offset && o.bytes == e.bytes && same_image(o.record, e.record)) {
                e.offset = o.offset;
                g_read[i] = !(same_renderer && o.record.modification_tick == e.record.modification_tick
                              && o.record.scaled_writes == e.record.scaled_writes
                              && o.record.guest_hash == e.record.guest_hash);
                g_taken[taken++] = { o.offset, page_up(o.bytes) };
                break;
            }
        }
    }

    // a place for each image that has none: the first gap it fits
    std::sort(g_taken, g_taken + taken, [](const Taken &a, const Taken &b) { return a.offset < b.offset; });
    bool fits = true;
    for (size_t i = 0; i < count && fits; i++) {
        if (out[i].bytes && !out[i].offset)
            fits = (out[i].offset = take(taken, page_up(out[i].bytes))) != 0;
    }
    if (!fits) {
        // no gap is large enough: every image goes up against the one before
        // it and is read again; what still has no room keeps no pixels
        uint64_t at = kDirectoryBytes;
        for (size_t i = 0; i < count; i++) {
            if (!out[i].bytes)
                continue;
            if (g_block_bytes - at < page_up(out[i].bytes)) {
                out[i].offset = 0;
                out[i].bytes = 0;
                g_read[i] = 0;
                left_out++;
                continue;
            }
            out[i].offset = at;
            at += page_up(out[i].bytes);
            g_read[i] = 1;
        }
    }

    for (size_t i = 0; i < count; i++)
        if (g_read[i])
            read_pixels(found[i], g_block + out[i].offset);

    auto *h = reinterpret_cast<Header *>(g_dir);
    std::memset(h, 0, sizeof *h);
    h->magic = kMagic;
    h->generation = g_generation;
    h->frame_tick = frame_tick;
    h->modification_tick = modification_tick;
    h->count = static_cast<uint32_t>(count);
    h->left_out = left_out;
    put(g_block, g_dir, sizeof(Header) + count * sizeof(Entry));

    if (g_trace) {
        size_t with = 0, read = 0;
        uint64_t bytes = 0;
        for (size_t i = 0; i < count; i++) {
            with += out[i].bytes != 0;
            read += g_read[i];
            bytes += g_read[i] ? out[i].bytes : 0;
        }
        fprintf(stderr, "kept gen=%llu images=%zu upscaled=%zu read=%zu bytes=%llu left_out=%zu\n",
                static_cast<unsigned long long>(g_generation), count, with, read,
                static_cast<unsigned long long>(bytes), left_out);
    }
    if (left_out && !g_said_full) {
        g_said_full = true;
        fprintf(stderr,
                "[eden] %llu upscaled images did not fit in the memory a state keeps them in: after a load "
                "they are drawn on from the console's memory, scaled up from its resolution\n",
                static_cast<unsigned long long>(left_out));
    }
}

bool kept(size_t *count, u64 *frame_tick, u64 *modification_tick) {
    // only what an earlier renderer had is made again: the one that wrote
    // the block still has its images
    if (header().magic != kMagic || header().generation == g_generation)
        return false;
    *count = header().count;
    *frame_tick = header().frame_tick;
    *modification_tick = header().modification_tick;
    if (g_trace)
        fprintf(stderr, "kept back gen=%llu images=%u\n", static_cast<unsigned long long>(g_generation),
                header().count);
    return true;
}

const K::Record *record(size_t i, const u8 **pixels, u64 *bytes) {
    const Entry &e = entries()[i];
    *pixels = e.bytes && e.offset ? g_block + e.offset : nullptr;
    *bytes = e.bytes;
    return &e.record;
}

} // namespace

void set_memory(void *block, size_t block_bytes, void *scratch, size_t scratch_bytes) {
    if (!block || !scratch || scratch_bytes < kScratchBytes || block_bytes < 2 * kDirectoryBytes)
        return;
    g_block = static_cast<uint8_t *>(block);
    g_block_bytes = block_bytes;
    uint8_t *p = static_cast<uint8_t *>(scratch);
    g_found = reinterpret_cast<K::Found *>(p);
    p += kMost * sizeof(K::Found);
    g_dir = p;
    p += kDirectoryBytes;
    g_taken = reinterpret_cast<Taken *>(p);
    p += kMost * sizeof(Taken);
    g_read = p;
    p += kMost;
    g_rows = p;
    p += kRowsBytes;
    auto &h = K::hooks;
    h.found = g_found;
    h.most = kMost;
    h.keep = keep_found;
    h.kept = kept;
    h.record = record;
    h.scratch = p;
    h.scratch_bytes = kHashBytes;
    g_trace = getenv("CHIMERA_EDEN_KEPT_TRACE") != nullptr;
}

size_t scratch_bytes() {
    return kScratchBytes;
}

bool active() {
    return g_block != nullptr;
}

void renderer_made_again() {
    g_generation++;
}

} // namespace chimera_eden::kept_images
