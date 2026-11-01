// The core's own tool, Decompress NSZ (Chimera's Tools > Core-Specific Tools):
// an .nsz made into the .nsp it was made from. Eden reads an NSZ whose
// compressed archives (.ncz) are in independent blocks, but not one
// compressed as a single stream ("solid"), while a game plays: such a game,
// update or DLC has to be decompressed first, and this does it.
//
// Chimera runs RunTool instead of Init, with no game: the .nsz is the mounted
// file "input" and the .nsp is written to "output", straight to the disk. An
// .nsz is an .nsp's container (PFS0) whose .nca files are .ncz: the NCA's
// first 0x4000 bytes as they are, a table of its sections (offset, size,
// encryption type, key and counter), and the rest decrypted and compressed
// with zstd - one stream, or blocks ("NCZBLOCK"). Each .ncz becomes its
// .nca again: decompressed, and each section encrypted again (AES-CTR, the
// counter's high half the section's, its low half the offset / 16), as
// Eden's own reader does (core/crypto/ncz_decompressor.cpp); every other file
// is copied. It reads and writes a few megabytes at a time, so a game of any
// size costs no more memory than that.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zstd.h>

#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"

#define ECL_EXPORT __attribute__((visibility("default")))
#define TEXT_ARG "\x1f"

namespace {

constexpr uint64_t kNcaHeader = 0x4000;
constexpr size_t kChunk = 4 << 20;

struct Section {
    uint64_t offset, size, type;
    Core::Crypto::Key128 key, counter;
};

struct Entry {
    std::string name;
    uint64_t offset, size;  // in the input, from its start
    bool ncz = false;
    // a .ncz's: its sections, where its compressed data starts, and its blocks
    std::vector<Section> sections;
    uint64_t payload = 0, nca_size = 0;
    uint32_t block_exponent = 0;
    std::vector<uint32_t> block_sizes;
    uint64_t block_total = 0;
};

uint64_t le(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = n; i-- > 0;)
        v = (v << 8) | p[i];
    return v;
}

bool read_at(FILE *f, uint64_t at, void *out, size_t n) {
    return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n;
}

std::string answer;
const char *say(const char *what, const std::string &text) {
    answer = std::string("{\"") + what + "\": \"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            answer += '\\', answer += static_cast<char>(c);
        else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", c);
            answer += u;
        } else
            answer += static_cast<char>(c);
    }
    answer += "\"}";
    return answer.c_str();
}

// the name the person's file had (the request's "inputs"), for the messages
std::string request_name(const std::string &request, const char *mount) {
    const std::string key = std::string("\"") + mount + "\":\"";
    const size_t at = request.find(key);
    if (at == std::string::npos)
        return mount;
    std::string out;
    for (size_t i = at + key.size(); i < request.size() && request[i] != '"'; i++) {
        if (request[i] == '\\' && i + 1 < request.size())
            i++;
        out += request[i];
    }
    return out;
}

// The bytes of an NCA from 0x4000 on, decrypted, encrypted again into place:
// the section a byte is in says how, a byte in none is as it is.
class Encrypter {
public:
    explicit Encrypter(const std::vector<Section> &sections) : sections_(sections) {}
    void apply(uint8_t *data, size_t n, uint64_t pos) {
        size_t done = 0;
        while (done < n) {
            const uint64_t at = pos + done;
            const Section *s = nullptr;
            uint64_t end = UINT64_MAX;
            for (const Section &x : sections_) {
                if (at >= x.offset && at < x.offset + x.size) {
                    s = &x;
                    break;
                }
                if (x.offset > at)
                    end = std::min(end, x.offset);
            }
            if (s != nullptr)
                end = s->offset + s->size;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(n - done, end - at));
            if (s != nullptr && (s->type == 3 || s->type == 4)) {
                // from the 16-byte block the piece starts in
                const size_t pad = static_cast<size_t>(at & 15);
                buffer_.assign(pad + take, 0);
                std::memcpy(buffer_.data() + pad, data + done, take);
                Core::Crypto::Key128 iv = s->counter;
                uint64_t block = (at - pad) >> 4;
                for (size_t i = 0; i < 8; i++, block >>= 8)
                    iv[15 - i] = static_cast<uint8_t>(block & 0xFF);
                Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(s->key, Core::Crypto::Mode::CTR);
                cipher.SetIV(iv);
                cipher.Transcode(buffer_.data(), buffer_.size(), buffer_.data(), Core::Crypto::Op::Encrypt);
                std::memcpy(data + done, buffer_.data() + pad, take);
            }
            done += take;
        }
    }

private:
    const std::vector<Section> &sections_;
    std::vector<uint8_t> buffer_;
};

// what a .ncz holds, from its header; false when it is not one
bool read_ncz(FILE *f, Entry &e) {
    uint8_t head[16];
    if (e.size < kNcaHeader + 16 || !read_at(f, e.offset + kNcaHeader, head, 16) ||
        std::memcmp(head, "NCZSECTN", 8) != 0)
        return false;
    const uint64_t count = le(head + 8, 8);
    if (count == 0 || count > 0x10000 || count * 0x40 > e.size)
        return false;
    std::vector<uint8_t> table(count * 0x40);
    if (!read_at(f, e.offset + kNcaHeader + 16, table.data(), table.size()))
        return false;
    e.nca_size = kNcaHeader;
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *p = table.data() + i * 0x40;
        Section s;
        s.offset = le(p, 8);
        s.size = le(p + 8, 8);
        s.type = le(p + 16, 8);
        std::memcpy(s.key.data(), p + 32, 16);
        std::memcpy(s.counter.data(), p + 48, 16);
        e.sections.push_back(s);
        e.nca_size = std::max(e.nca_size, s.offset + s.size);
    }
    e.payload = e.offset + kNcaHeader + 16 + count * 0x40;
    uint8_t block[24];
    if (read_at(f, e.payload, block, 24) && std::memcmp(block, "NCZBLOCK", 8) == 0) {
        e.block_exponent = block[11];
        e.block_total = le(block + 16, 8);
        const uint32_t n = static_cast<uint32_t>(le(block + 12, 4));
        if (e.block_exponent < 14 || e.block_exponent > 32 || n == 0)
            return false;
        std::vector<uint8_t> sizes(static_cast<size_t>(n) * 4);
        if (!read_at(f, e.payload + 24, sizes.data(), sizes.size()))
            return false;
        for (uint32_t i = 0; i < n; i++)
            e.block_sizes.push_back(static_cast<uint32_t>(le(sizes.data() + i * 4, 4)));
        e.payload += 24 + sizes.size();
    }
    e.ncz = true;
    return true;
}

bool write_all(FILE *out, const uint8_t *data, size_t n) {
    return fwrite(data, 1, n, out) == n;
}

// one .ncz as its .nca: false (and why) when it does not decompress
bool write_nca(FILE *in, FILE *out, const Entry &e, std::string &why) {
    std::vector<uint8_t> head(kNcaHeader);
    if (!read_at(in, e.offset, head.data(), head.size()) || !write_all(out, head.data(), head.size())) {
        why = "error.tool.read_write";
        return false;
    }
    Encrypter encrypt(e.sections);
    uint64_t pos = kNcaHeader;
    std::vector<uint8_t> src(kChunk), dst(kChunk);
    const uint64_t end = e.offset + e.size;
    auto put = [&](uint8_t *data, size_t n) {
        n = static_cast<size_t>(std::min<uint64_t>(n, e.nca_size - pos));
        encrypt.apply(data, n, pos);
        pos += n;
        return write_all(out, data, n);
    };
    if (e.block_sizes.empty()) {
        // one stream
        ZSTD_DStream *z = ZSTD_createDStream();
        ZSTD_DCtx_setParameter(z, ZSTD_d_windowLogMax, 31);
        uint64_t at = e.payload;
        bool ok = true;
        while (ok && pos < e.nca_size && at < end) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(src.size(), end - at));
            if (!read_at(in, at, src.data(), n)) {
                ok = false;
                why = "error.tool.read_write";
                break;
            }
            at += n;
            ZSTD_inBuffer input{src.data(), n, 0};
            while (ok && input.pos < input.size && pos < e.nca_size) {
                ZSTD_outBuffer output{dst.data(), dst.size(), 0};
                const size_t r = ZSTD_decompressStream(z, &output, &input);
                if (ZSTD_isError(r)) {
                    ok = false;
                    why = "error.tool.not_zstd";
                    break;
                }
                if (output.pos && !put(dst.data(), output.pos)) {
                    ok = false;
                    why = "error.tool.read_write";
                }
            }
        }
        ZSTD_freeDStream(z);
        if (!ok)
            return false;
    } else {
        // blocks, each its own: one no smaller compressed is stored as it is
        // (the rule of Eden's reader)
        const uint64_t block = 1ULL << e.block_exponent;
        uint64_t at = e.payload, left = e.block_total;
        std::vector<uint8_t> raw, plain(static_cast<size_t>(block));
        for (uint32_t size : e.block_sizes) {
            const uint64_t want = std::min(block, left);
            const size_t got = static_cast<size_t>(want);
            if (size < want) {
                raw.resize(size);
                if (!read_at(in, at, raw.data(), size)) {
                    why = "error.tool.read_write";
                    return false;
                }
                const size_t made = ZSTD_decompress(plain.data(), plain.size(), raw.data(), size);
                if (ZSTD_isError(made) || made != want) {
                    why = "error.tool.not_zstd";
                    return false;
                }
            } else if (!read_at(in, at, plain.data(), got)) {
                why = "error.tool.read_write";
                return false;
            }
            at += size;
            if (!put(plain.data(), got)) {
                why = "error.tool.read_write";
                return false;
            }
            left -= want;
        }
    }
    if (pos != e.nca_size) {
        why = "error.tool.cut_short";
        return false;
    }
    return true;
}

} // namespace

extern "C" ECL_EXPORT const char *RunTool(void) {
    std::string request;
    if (FILE *r = fopen("tool", "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, r)) > 0)
            request.append(buf, n);
        fclose(r);
    }
    if (request.find("\"tool\":\"decompress_nsz\"") == std::string::npos)
        return say("refused", "error.tool.unknown");
    const std::string shown = request_name(request, "input");

    FILE *in = fopen("input", "rb");
    if (in == nullptr)
        return say("refused", std::string("error.tool.unreadable" TEXT_ARG) + shown);
    uint8_t head[16];
    if (!read_at(in, 0, head, 16) || std::memcmp(head, "PFS0", 4) != 0) {
        fclose(in);
        return say("refused", std::string("error.tool.not_nsz" TEXT_ARG) + shown);
    }
    const uint32_t count = static_cast<uint32_t>(le(head + 4, 4)), names = static_cast<uint32_t>(le(head + 8, 4));
    std::vector<uint8_t> table(static_cast<size_t>(count) * 24 + names);
    if (count == 0 || count > 4096 || !read_at(in, 16, table.data(), table.size())) {
        fclose(in);
        return say("refused", std::string("error.tool.not_nsz" TEXT_ARG) + shown);
    }
    const uint64_t base = 16 + table.size();
    std::vector<Entry> entries;
    int nczs = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = table.data() + i * 24;
        Entry e;
        e.offset = base + le(p, 8);
        e.size = le(p + 8, 8);
        const uint32_t at = static_cast<uint32_t>(le(p + 16, 4));
        if (at >= names) {
            fclose(in);
            return say("refused", std::string("error.tool.not_nsz" TEXT_ARG) + shown);
        }
        const char *name = reinterpret_cast<const char *>(table.data() + count * 24 + at);
        e.name.assign(name, strnlen(name, names - at));
        if (e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".ncz") == 0) {
            if (!read_ncz(in, e)) {
                fclose(in);
                return say("refused", std::string("error.tool.bad_ncz" TEXT_ARG) + e.name);
            }
            e.name.replace(e.name.size() - 4, 4, ".nca");
            nczs++;
        }
        entries.push_back(std::move(e));
    }
    if (nczs == 0) {
        fclose(in);
        return say("refused", std::string("error.tool.nothing_compressed" TEXT_ARG) + shown);
    }

    // the .nsp's table: each file where the one before ends
    std::string strings;
    std::vector<uint8_t> out_table;
    uint64_t data_at = 0;
    for (const Entry &e : entries) {
        const uint64_t size = e.ncz ? e.nca_size : e.size;
        uint8_t row[24] = {};
        for (int i = 0; i < 8; i++) {
            row[i] = static_cast<uint8_t>(data_at >> (8 * i));
            row[8 + i] = static_cast<uint8_t>(size >> (8 * i));
        }
        const uint32_t at = static_cast<uint32_t>(strings.size());
        for (int i = 0; i < 4; i++)
            row[16 + i] = static_cast<uint8_t>(at >> (8 * i));
        out_table.insert(out_table.end(), row, row + 24);
        strings += e.name;
        strings += '\0';
        data_at += size;
    }
    while ((16 + out_table.size() + strings.size()) % 0x20)
        strings += '\0';
    uint8_t out_head[16] = {'P', 'F', 'S', '0'};
    for (int i = 0; i < 4; i++) {
        out_head[4 + i] = static_cast<uint8_t>(entries.size() >> (8 * i));
        out_head[8 + i] = static_cast<uint8_t>(strings.size() >> (8 * i));
    }

    FILE *out = fopen("output", "wb");
    std::string why;
    bool ok = out != nullptr && write_all(out, out_head, 16) && write_all(out, out_table.data(), out_table.size()) &&
              write_all(out, reinterpret_cast<const uint8_t *>(strings.data()), strings.size());
    if (!ok)
        why = "error.tool.read_write";
    std::vector<uint8_t> copy(kChunk);
    uint64_t written = 0;
    for (const Entry &e : entries) {
        if (!ok)
            break;
        if (e.ncz) {
            ok = write_nca(in, out, e, why);
            if (!ok && why.find(TEXT_ARG) == std::string::npos && why != "error.tool.read_write")
                why += TEXT_ARG + e.name;
            written += e.nca_size;
            continue;
        }
        for (uint64_t at = 0; ok && at < e.size;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(copy.size(), e.size - at));
            ok = read_at(in, e.offset + at, copy.data(), n) && write_all(out, copy.data(), n);
            if (!ok)
                why = "error.tool.read_write";
            at += n;
        }
        written += e.size;
    }
    fclose(in);
    if (out != nullptr && fclose(out) != 0 && ok) {
        ok = false;
        why = "error.tool.read_write";
    }
    if (!ok)
        return say("refused", why);
    char done[160];
    snprintf(done, sizeof done, "tool.decompress_nsz.done" TEXT_ARG "%d" TEXT_ARG "%.2f", nczs,
             static_cast<double>(16 + out_table.size() + strings.size() + written) / (1 << 30));
    return say("done", done);
}

// Whether a file is an NSZ that holds an archive compressed as one stream:
// Eden cannot read one while a game plays, and says only that the game has
// no program in it, so the core refuses it first, by name, saying what to do.
bool chimera_solid_nsz(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
        return false;
    bool solid = false;
    uint8_t head[16];
    if (read_at(f, 0, head, 16) && std::memcmp(head, "PFS0", 4) == 0) {
        const uint32_t count = static_cast<uint32_t>(le(head + 4, 4)), names = static_cast<uint32_t>(le(head + 8, 4));
        std::vector<uint8_t> table(static_cast<size_t>(count) * 24 + names);
        if (count > 0 && count <= 4096 && read_at(f, 16, table.data(), table.size())) {
            const uint64_t base = 16 + table.size();
            for (uint32_t i = 0; i < count && !solid; i++) {
                const uint8_t *p = table.data() + i * 24;
                const uint32_t at = static_cast<uint32_t>(le(p + 16, 4));
                if (at >= names)
                    break;
                const char *name = reinterpret_cast<const char *>(table.data() + count * 24 + at);
                const size_t len = strnlen(name, names - at);
                if (len < 4 || std::memcmp(name + len - 4, ".ncz", 4) != 0)
                    continue;
                Entry e;
                e.offset = base + le(p, 8);
                e.size = le(p + 8, 8);
                solid = read_ncz(f, e) && e.block_sizes.empty();
            }
        }
    }
    fclose(f);
    return solid;
}
