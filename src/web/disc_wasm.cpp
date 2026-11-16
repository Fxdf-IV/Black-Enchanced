#include "kernel/disc.h"

#include <emscripten.h>
#include <emscripten/threading_primitives.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace kernel {

namespace {

std::unique_ptr<Disc> g_mounted;
std::mutex g_file_lock;
constexpr intptr_t kVirtualVfsDisc = -2;
constexpr intptr_t kVirtualIsoDisc = -3;

uint32_t WebVfsRead(uint32_t file_id, uint64_t pos, void* out, uint32_t len) {
    if (!out || !len) return 0;
    uint32_t done = 0;
    uint32_t flag = 0;
    const uint32_t lo = static_cast<uint32_t>(pos & 0xFFFFFFFFull);
    const uint32_t hi = static_cast<uint32_t>(pos >> 32);
    MAIN_THREAD_EM_ASM(
        {
            const fileId = $0 >>> 0;
            const lo = $1 >>> 0;
            const hi = $2 >>> 0;
            const outPtr = $3 >>> 0;
            const len = $4 >>> 0;
            const donePtr = $5 >>> 0;
            const flagPtr = $6 >>> 0;
            const offset = hi * 4294967296 + lo;
            const getI32 = () => {
                if (typeof growMemViews === "function") growMemViews();
                return HEAP32;
            };
            if (!window.__blackVfs || typeof window.__blackVfs.readSlice !== "function") {
                const i32 = getI32();
                Atomics.store(i32, donePtr >> 2, 0);
                Atomics.store(i32, flagPtr >> 2, 2);
                Atomics.notify(i32, flagPtr >> 2, 1);
                return;
            }
            Promise.resolve(window.__blackVfs.readSlice(fileId, offset, outPtr, len))
                .then((n) => {
                    const i32 = getI32();
                    Atomics.store(i32, donePtr >> 2, (n || 0) | 0);
                    Atomics.store(i32, flagPtr >> 2, 1);
                    Atomics.notify(i32, flagPtr >> 2, 1);
                })
                .catch(() => {
                    const i32 = getI32();
                    Atomics.store(i32, donePtr >> 2, 0);
                    Atomics.store(i32, flagPtr >> 2, 2);
                    Atomics.notify(i32, flagPtr >> 2, 1);
                });
        },
        file_id, lo, hi, out, len, &done, &flag);

    while (__atomic_load_n(&flag, __ATOMIC_ACQUIRE) == 0) {
        emscripten_futex_wait(&flag, 0, 50.0);
    }
    return done;
}

uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t U64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ValidHeader(const uint8_t* buf) {
    return std::memcmp(buf, "MICROSOFT*XBOX*MEDIA", 20) == 0 &&
           std::memcmp(buf + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20) == 0;
}

}  // namespace

Disc::~Disc() {
    if (file_ != -1 && file_ != kVirtualVfsDisc && file_ != kVirtualIsoDisc) {
        std::fclose(reinterpret_cast<std::FILE*>(file_));
    }
    file_ = -1;
}

uint32_t Disc::ReadAt(uint64_t at, void* out, uint32_t len) const {
    if (file_ == -1 || !len) return 0;
    if (file_ == kVirtualIsoDisc) {
        return WebVfsRead(0u, at, out, len);
    }
    std::lock_guard<std::mutex> lock(g_file_lock);
    auto* fp = reinterpret_cast<std::FILE*>(file_);
    if (fseeko(fp, off_t(at), SEEK_SET) != 0) return 0;
    return static_cast<uint32_t>(std::fread(out, 1, len, fp));
}

bool Disc::ReadDir(Entry& dir, int depth) {
    if (depth > 32 || dir.size == 0) return true;
    std::vector<uint8_t> table(dir.size);
    if (ReadAt(dir.offset, table.data(), dir.size) != dir.size) return false;

    auto walk = [&](auto&& self, uint32_t off) -> bool {
        if (off == 0xFFFFu || off * 4u + 14u > dir.size) return true;
        const uint32_t b = off * 4u;
        const uint16_t left = U16(table.data() + b);
        const uint16_t right = U16(table.data() + b + 2);
        const uint32_t sector = U32(table.data() + b + 4);
        const uint32_t size = U32(table.data() + b + 8);
        const uint8_t attr = table[b + 12];
        const uint8_t nlen = table[b + 13];
        if (b + 14u + nlen > dir.size) return false;

        if (left && !self(self, left)) return false;

        Entry e;
        e.name.assign(reinterpret_cast<const char*>(table.data() + b + 14), nlen);
        e.offset = base_ + uint64_t(sector) * 2048ull;
        e.size = size;
        e.dir = (attr & 0x10u) != 0;
        if (e.dir && !ReadDir(e, depth + 1)) return false;
        dir.children.push_back(std::move(e));

        if (right && !self(self, right)) return false;
        return true;
    };
    if (!walk(walk, 0)) return false;
    std::sort(dir.children.begin(), dir.children.end(),
              [](const Entry& a, const Entry& b) { return Lower(a.name) < Lower(b.name); });
    return true;
}

bool Disc::Open(const std::filesystem::path& path, std::string& error) {
    const std::string pstr = path.string();
    if (pstr.size() >= 17 && pstr.compare(pstr.size() - 17, 17, ".vfs_manifest.tsv") == 0) {
        std::ifstream in(path);
        if (!in) {
            error = "could not open VFS manifest";
            return false;
        }
        root_ = {};
        root_.dir = true;
        time_ = 127700000000000000ull;  // 2005-era FILETIME
        std::string line;
        bool iso_mode = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line == "#ISO") {
                iso_mode = true;
                break;
            }
            const size_t t1 = line.find('\t');
            const size_t t2 = (t1 != std::string::npos) ? line.find('\t', t1 + 1) : std::string::npos;
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            const uint32_t fid = static_cast<uint32_t>(std::strtoul(line.substr(0, t1).c_str(), nullptr, 10));
            const uint32_t fsz = static_cast<uint32_t>(std::strtoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10));
            std::string rel = line.substr(t2 + 1);
            std::replace(rel.begin(), rel.end(), '\\', '/');

            Entry* cur = &root_;
            size_t pos = 0;
            while (pos < rel.size()) {
                size_t slash = rel.find('/', pos);
                std::string seg = (slash == std::string::npos) ? rel.substr(pos) : rel.substr(pos, slash - pos);
                if (!seg.empty()) {
                    const bool is_leaf = (slash == std::string::npos);
                    if (is_leaf) {
                        Entry leaf;
                        leaf.name = std::move(seg);
                        leaf.offset = fid;
                        leaf.size = fsz;
                        leaf.dir = false;
                        cur->children.push_back(std::move(leaf));
                    } else {
                        const std::string low = Lower(seg);
                        auto it = std::find_if(cur->children.begin(), cur->children.end(),
                                               [&](const Entry& e) { return e.dir && Lower(e.name) == low; });
                        if (it == cur->children.end()) {
                            Entry sub;
                            sub.name = std::move(seg);
                            sub.dir = true;
                            cur->children.push_back(std::move(sub));
                            cur = &cur->children.back();
                        } else {
                            cur = &*it;
                        }
                    }
                }
                if (slash == std::string::npos) break;
                pos = slash + 1;
            }
        }
        auto sort_tree = [&](auto&& self, Entry& dir) -> void {
            std::sort(dir.children.begin(), dir.children.end(),
                      [](const Entry& a, const Entry& b) { return Lower(a.name) < Lower(b.name); });
            for (auto& ch : dir.children) {
                if (ch.dir) self(self, ch);
            }
        };
        sort_tree(sort_tree, root_);
        path_ = path;
        if (!iso_mode) {
            file_ = kVirtualVfsDisc;
            return true;
        }
        file_ = kVirtualIsoDisc;
    } else {
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) {
            error = "could not open ISO file";
            return false;
        }
        file_ = reinterpret_cast<intptr_t>(fp);
        path_ = path;
    }

    static constexpr uint64_t kCandidates[] = {0ull, 0xFD90000ull, 0x18300000ull};
    uint8_t sector[2048]{};
    bool found = false;
    for (uint64_t base : kCandidates) {
        if (ReadAt(base + 32ull * 2048ull, sector, 2048) == 2048 && ValidHeader(sector)) {
            base_ = base;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "not an Xbox ISO image";
        return false;
    }

    const uint32_t root_sector = U32(sector + 20);
    const uint32_t root_size = U32(sector + 24);
    time_ = U64(sector + 28);
    root_ = {};
    root_.dir = true;
    root_.offset = base_ + uint64_t(root_sector) * 2048ull;
    root_.size = root_size;
    if (!ReadDir(root_, 0)) {
        error = "corrupt XISO directory tree";
        return false;
    }
    return true;
}

const Disc::Entry* Disc::Find(const std::vector<std::string>& parts) const {
    const Entry* cur = &root_;
    for (const auto& part : parts) {
        if (!cur->dir || part.empty() || part == ".") continue;
        if (part == "..") continue;
        const std::string low = Lower(part);
        auto it = std::find_if(cur->children.begin(), cur->children.end(),
                               [&](const Entry& e) { return Lower(e.name) == low; });
        if (it == cur->children.end()) return nullptr;
        cur = &*it;
    }
    return cur;
}

const Disc::Entry* Disc::Find(std::string_view path) const {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        const size_t sep = path.find_first_of("\\/", start);
        const auto part = sep == std::string_view::npos ? path.substr(start) : path.substr(start, sep - start);
        if (!part.empty()) parts.emplace_back(part);
        if (sep == std::string_view::npos) break;
        start = sep + 1;
    }
    return Find(parts);
}

uint32_t Disc::Read(const Entry& file, uint64_t pos, void* out, uint32_t len) const {
    if (pos >= file.size) return 0;
    const uint32_t avail = static_cast<uint32_t>(std::min<uint64_t>(len, file.size - pos));
    if (file_ == kVirtualVfsDisc) {
        return WebVfsRead(static_cast<uint32_t>(file.offset), pos, out, avail);
    }
    return ReadAt(file.offset + pos, out, avail);
}

bool Disc::Load(std::string_view path, std::vector<uint8_t>& out) const {
    const Entry* e = Find(path);
    if (!e || e->dir) return false;
    out.resize(e->size);
    return Read(*e, 0, out.data(), e->size) == e->size;
}

void MountDisc(std::unique_ptr<Disc> disc) { g_mounted = std::move(disc); }
const Disc* MountedDisc() { return g_mounted.get(); }

}  // namespace kernel
