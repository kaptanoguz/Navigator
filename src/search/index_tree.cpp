// src/search/index_tree.cpp: in-memory index model, file codec (version 1) and atomic file I/O.
#include "index_tree.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/index.hpp>

namespace navigator::search::detail {

// ---------------------------------------------------------------------------------------------
// Tree
// ---------------------------------------------------------------------------------------------

void Tree::reset() {
    dirs.clear();
    free_dirs.clear();
    by_id.clear();
    dirs.emplace_back();
    dirs[0].alive = true;
    entries = 0;
    live_dirs = 1;
    scan_started_at = 0;
    complete = false;
    truncated = false;
}

std::uint32_t Tree::new_dir(std::uint32_t parent, std::string_view name) {
    std::uint32_t d = 0;
    if (!free_dirs.empty()) {
        d = free_dirs.back();
        free_dirs.pop_back();
        dirs[d] = Dir{};
    } else {
        d = static_cast<std::uint32_t>(dirs.size());
        dirs.emplace_back();
    }
    Dir& rec = dirs[d];
    rec.alive = true;
    rec.serial = ++last_serial;
    rec.parent = parent;
    rec.name.assign(name);
    ++live_dirs;
    return d;
}

void Tree::free_subtree(std::uint32_t d, std::vector<WatchHandle>& handles) {
    std::vector<std::uint32_t> stack{d};
    while (!stack.empty()) {
        const std::uint32_t cur = stack.back();
        stack.pop_back();
        Dir& rec = dirs[cur];
        if (!rec.alive) {
            continue;
        }
        for (const Child& c : rec.children) {
            if (c.dir != kNoIndex) {
                stack.push_back(c.dir);
            }
        }
        entries -= rec.children.size();
        if (rec.watch != kInvalidWatchHandle) {
            handles.push_back(rec.watch);
        }
        if (rec.device != 0 || rec.inode != 0) {
            const auto it = by_id.find(FileId{rec.device, rec.inode});
            if (it != by_id.end() && it->second == cur) {
                by_id.erase(it);
            }
        }
        rec = Dir{};
        free_dirs.push_back(cur);
        --live_dirs;
    }
}

std::uint32_t Tree::find_child(std::uint32_t d, std::string_view name) const noexcept {
    const Dir& rec = dirs[d];
    for (std::size_t i = 0; i < rec.children.size(); ++i) {
        if (raw_name(rec, rec.children[i]) == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoIndex;
}

std::string Tree::relative_path(std::uint32_t d, std::uint32_t ancestor) const {
    std::vector<std::string_view> parts;
    while (d != ancestor && d != kNoIndex) {
        parts.push_back(dirs[d].name);
        d = dirs[d].parent;
    }
    std::string out;
    for (const std::string_view part : std::views::reverse(parts)) {
        if (!out.empty()) {
            out += '/';
        }
        out.append(part);
    }
    return out;
}

Path Tree::path_of(std::uint32_t d, const Path& root) const {
    const std::string rel = relative_path(d, 0);
    if (rel.empty()) {
        return root;
    }
    return root / rel;
}

void Tree::set_identity(std::uint32_t d, std::uint64_t device, std::uint64_t inode) {
    Dir& rec = dirs[d];
    if (rec.device != 0 || rec.inode != 0) {
        const auto it = by_id.find(FileId{rec.device, rec.inode});
        if (it != by_id.end() && it->second == d) {
            by_id.erase(it);
        }
    }
    rec.device = device;
    rec.inode = inode;
    if (device != 0 || inode != 0) {
        by_id[FileId{device, inode}] = d;
    }
}

void compact_pool(Dir& d, std::string& fold_scratch) {
    std::string pool;
    pool.reserve(d.pool.size() - d.garbage);
    for (Child& c : d.children) {
        const std::string raw(Tree::raw_name(d, c));
        (void)append_name(pool, c, raw, fold_scratch);
    }
    d.pool = std::move(pool);
    d.garbage = 0;
}

bool append_name(std::string& pool, Child& c, std::string_view name, std::string& fold_scratch) {
    if (name.empty() || name.size() > kMaxIndexedName) {
        return false;
    }
    bool ascii = true;
    bool lower = true;
    for (const char ch : name) {
        const auto u = static_cast<unsigned char>(ch);
        ascii = ascii && u < 0x80;
        lower = lower && (u < 'A' || u > 'Z');
    }
    if (ascii) {
        fold_scratch.assign(name);
        if (!lower) {
            for (char& ch : fold_scratch) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
        }
    } else {
        fs::fold_name_into(name, fold_scratch);
    }
    if (fold_scratch.size() > 0xFFFFU ||
        pool.size() + name.size() + fold_scratch.size() > static_cast<std::size_t>(0xFFFFFFFFU)) {
        return false;
    }
    c.raw_off = static_cast<std::uint32_t>(pool.size());
    c.raw_len = static_cast<std::uint16_t>(name.size());
    pool.append(name);
    if (fold_scratch == name) {
        c.fold_off = c.raw_off; // share the bytes (ASCII lowercase names)
    } else {
        c.fold_off = static_cast<std::uint32_t>(pool.size());
        pool.append(fold_scratch);
    }
    c.fold_len = static_cast<std::uint16_t>(fold_scratch.size());
    return true;
}

// ---------------------------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kMagic("NAVIDX\0\n", 8);
constexpr std::size_t kHeaderBytes = 96;
constexpr std::size_t kDirRecordBytes = 48;
constexpr std::size_t kEntryRecordBytes = 16;

class Writer {
public:
    explicit Writer(std::string& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }
    void u16(std::uint16_t v) { put(v, 2); }
    void u32(std::uint32_t v) { put(v, 4); }
    void u64(std::uint64_t v) { put(v, 8); }
    void i64(std::int64_t v) { put(static_cast<std::uint64_t>(v), 8); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        out_.append(s);
    }
    void pad_to(std::size_t n) {
        while (out_.size() < n) {
            out_.push_back('\0');
        }
    }

private:
    void put(std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out_.push_back(static_cast<char>(v & 0xFFU));
            v >>= 8U;
        }
    }
    std::string& out_;
};

[[nodiscard]] std::uint64_t get(std::string_view b, std::size_t at, int bytes) noexcept {
    std::uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) {
        v = (v << 8U) | static_cast<unsigned char>(b[at + static_cast<std::size_t>(i)]);
    }
    return v;
}
[[nodiscard]] std::uint32_t get32(std::string_view b, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(get(b, at, 4));
}
[[nodiscard]] std::uint64_t get64(std::string_view b, std::size_t at) noexcept {
    return get(b, at, 8);
}

void put64_at(std::string& s, std::size_t at, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
        s[at + i] = static_cast<char>(v & 0xFFU);
        v >>= 8U;
    }
}

[[nodiscard]] constexpr std::size_t align8(std::size_t n) noexcept {
    return (n + 7U) & ~std::size_t{7};
}

[[nodiscard]] auto corrupt(std::string_view why) {
    return fail(ErrorCode::InvalidArgument, "index file corrupt: " + std::string(why));
}

} // namespace

Result<std::string> encode_index(const Tree& tree, const VolumeIdentity& identity,
                                 std::int64_t saved_at_ns) {
    // Breadth-first renumbering of the reachable live directories.
    std::vector<std::uint32_t> order;
    std::vector<std::uint32_t> renum(tree.dirs.size(), kNoIndex);
    std::vector<bool> hidden(tree.dirs.size(), false);
    order.reserve(tree.live_dirs);
    order.push_back(0);
    renum[0] = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t pool_bytes = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Dir& d = tree.dirs[order[k]];
        entry_count += d.children.size();
        for (const Child& c : d.children) {
            pool_bytes += c.raw_len;
            if (c.dir != kNoIndex && c.dir < tree.dirs.size() && tree.dirs[c.dir].alive &&
                renum[c.dir] == kNoIndex) {
                renum[c.dir] = static_cast<std::uint32_t>(order.size());
                hidden[c.dir] = hidden[order[k]] || (c.flags & kEntryHidden) != 0;
                order.push_back(c.dir);
            }
        }
    }
    if (pool_bytes > 0xFFFFFFFFULL || entry_count >= kNoIndex || order.size() >= kNoIndex) {
        return fail(ErrorCode::IoError, "index too large for the file format");
    }

    std::string vol;
    {
        Writer w(vol);
        w.str(index_stable_id(identity));
        w.str(identity.uuid);
        w.str(identity.device);
        w.str(identity.mount_point.native());
        w.str(identity.root);
    }
    const std::size_t dirs_offset = align8(kHeaderBytes + vol.size());
    const std::size_t entries_offset = dirs_offset + (kDirRecordBytes * order.size());
    const std::size_t pool_offset =
        entries_offset + (kEntryRecordBytes * static_cast<std::size_t>(entry_count));
    const std::size_t total = pool_offset + static_cast<std::size_t>(pool_bytes) + 8;
    if (total > kMaxIndexFileBytes) {
        return fail(ErrorCode::IoError, "index file would exceed kMaxIndexFileBytes");
    }

    std::string out;
    out.reserve(total);
    Writer w(out);
    out.append(kMagic);
    w.u32(kIndexFormatVersion);
    w.u32((tree.complete ? kFileComplete : 0U) | (tree.truncated ? kFileTruncated : 0U));
    w.i64(saved_at_ns);
    w.i64(tree.scan_started_at);
    w.u64(order.size());
    w.u64(entry_count);
    w.u64(pool_bytes);
    w.u64(dirs_offset);
    w.u64(entries_offset);
    w.u64(pool_offset);
    w.u32(static_cast<std::uint32_t>(vol.size()));
    w.u32(0);
    w.u64(0);
    out.append(vol);
    w.pad_to(dirs_offset);

    // self_entry of every directory.
    std::vector<std::uint32_t> self(order.size(), kNoIndex);
    {
        std::uint32_t e = 0;
        for (const std::uint32_t old : order) {
            for (const Child& c : tree.dirs[old].children) {
                if (c.dir != kNoIndex && c.dir < renum.size() && renum[c.dir] != kNoIndex) {
                    self[renum[c.dir]] = e;
                }
                ++e;
            }
        }
    }
    std::uint32_t first = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Dir& d = tree.dirs[order[k]];
        w.u32(k == 0 ? kNoIndex : renum[d.parent]);
        w.u32(self[k]);
        w.u32(first);
        w.u32(static_cast<std::uint32_t>(d.children.size()));
        w.i64(d.mtime_ns);
        w.u64(d.device);
        w.u64(d.inode);
        std::uint32_t flags = d.flags & (kDirScanned | kDirUnreadable | kDirOtherFs);
        if (hidden[order[k]]) {
            flags |= kDirHidden;
        }
        if (d.watch == kInvalidWatchHandle) {
            flags |= kDirUnwatched;
        }
        w.u32(flags);
        w.u32(0);
        first += static_cast<std::uint32_t>(d.children.size());
    }
    std::uint32_t name_off = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Dir& d = tree.dirs[order[k]];
        for (const Child& c : d.children) {
            w.u32(name_off);
            w.u16(c.raw_len);
            w.u8(static_cast<std::uint8_t>(c.kind));
            w.u8(static_cast<std::uint8_t>(c.flags & (kEntryHidden | kEntryLinkToDir)));
            w.u32(static_cast<std::uint32_t>(k));
            const bool linked =
                c.dir != kNoIndex && c.dir < renum.size() && renum[c.dir] != kNoIndex;
            w.u32(linked ? renum[c.dir] : kNoIndex);
            name_off += c.raw_len;
        }
    }
    for (const std::uint32_t old : order) {
        const Dir& d = tree.dirs[old];
        for (const Child& c : d.children) {
            out.append(Tree::raw_name(d, c));
        }
    }
    const std::uint64_t hash = fnv1a64(out);
    out.resize(out.size() + 8);
    put64_at(out, out.size() - 8, hash);
    return out;
}

Result<Tree> decode_index(std::string_view b, const VolumeIdentity& identity) {
    const std::size_t size = b.size();
    if (size < kHeaderBytes + 8) {
        return corrupt("truncated header");
    }
    if (size > kMaxIndexFileBytes) {
        return corrupt("file too large");
    }
    if (b.substr(0, 8) != kMagic) {
        return corrupt("bad magic");
    }
    const std::uint32_t version = get32(b, 8);
    if (version != kIndexFormatVersion) {
        return fail(ErrorCode::InvalidArgument,
                    "index file version " + std::to_string(version) + " is not supported");
    }
    if (fnv1a64(b.substr(0, size - 8)) != get64(b, size - 8)) {
        return corrupt("checksum mismatch");
    }
    const std::uint32_t flags = get32(b, 12);
    if ((flags & ~(kFileComplete | kFileTruncated)) != 0) {
        return corrupt("unknown header flags");
    }
    const auto scan_started = static_cast<std::int64_t>(get64(b, 24));
    const std::uint64_t dir_count = get64(b, 32);
    const std::uint64_t entry_count = get64(b, 40);
    const std::uint64_t pool_bytes = get64(b, 48);
    const std::uint64_t dirs_offset = get64(b, 56);
    const std::uint64_t entries_offset = get64(b, 64);
    const std::uint64_t pool_offset = get64(b, 72);
    const std::uint64_t volume_bytes = get32(b, 80);

    // Bounds, all in u64 with sizes far below 2^63 (size <= 2 GiB).
    if (dir_count < 1 || dir_count > size / kDirRecordBytes ||
        entry_count > size / kEntryRecordBytes || pool_bytes > size || volume_bytes > size) {
        return corrupt("counts out of range");
    }
    // Each offset on its own first, so that the sums below cannot wrap.
    if (dirs_offset > size || entries_offset > size || pool_offset > size) {
        return corrupt("section offsets beyond the end of the file");
    }
    if (dirs_offset % 8 != 0 || entries_offset % 8 != 0 ||
        dirs_offset < kHeaderBytes + volume_bytes ||
        entries_offset < dirs_offset + (dir_count * kDirRecordBytes) ||
        pool_offset < entries_offset + (entry_count * kEntryRecordBytes) ||
        pool_offset + pool_bytes + 8 != size) {
        return corrupt("section offsets out of bounds");
    }
    if (dir_count >= kNoIndex || entry_count >= kNoIndex) {
        return corrupt("too many records");
    }

    // Volume block.
    {
        std::size_t at = kHeaderBytes;
        const std::size_t end = kHeaderBytes + static_cast<std::size_t>(volume_bytes);
        std::array<std::string_view, 5> fields;
        for (std::string_view& f : fields) {
            if (at + 4 > end) {
                return corrupt("volume block truncated");
            }
            const std::size_t len = get32(b, at);
            at += 4;
            if (len > end - at) {
                return corrupt("volume string out of bounds");
            }
            f = b.substr(at, len);
            at += len;
        }
        if (at != end) {
            return corrupt("volume block size mismatch");
        }
        if (fields[0] != index_stable_id(identity) || fields[1] != identity.uuid ||
            fields[2] != identity.device || fields[3] != identity.mount_point.native() ||
            fields[4] != identity.root) {
            return fail(ErrorCode::NotFound, "index file belongs to another volume");
        }
    }

    const auto dc = static_cast<std::size_t>(dir_count);
    const auto ec = static_cast<std::size_t>(entry_count);
    struct RawDir {
        std::uint32_t parent, self, first, count;
    };
    std::vector<RawDir> rd(dc);
    for (std::size_t d = 0; d < dc; ++d) {
        const std::size_t at = static_cast<std::size_t>(dirs_offset) + (d * kDirRecordBytes);
        RawDir r{get32(b, at), get32(b, at + 4), get32(b, at + 8), get32(b, at + 12)};
        if (d == 0) {
            if (r.parent != kNoIndex || r.self != kNoIndex) {
                return corrupt("bad root record");
            }
        } else if (r.parent >= dc || r.parent == d || r.self >= ec) {
            return corrupt("directory record out of bounds");
        }
        if (std::uint64_t{r.first} + r.count > ec) {
            return corrupt("child range out of bounds");
        }
        rd[d] = r;
    }
    struct RawEntry {
        std::uint32_t name_off;
        std::uint16_t name_len;
        std::uint8_t kind, flags;
        std::uint32_t parent, dir;
    };
    std::vector<RawEntry> re(ec);
    std::uint64_t name_bytes = 0;
    const std::string_view pool =
        b.substr(static_cast<std::size_t>(pool_offset), static_cast<std::size_t>(pool_bytes));
    for (std::size_t e = 0; e < ec; ++e) {
        const std::size_t at = static_cast<std::size_t>(entries_offset) + (e * kEntryRecordBytes);
        RawEntry r{get32(b, at),
                   static_cast<std::uint16_t>(get(b, at + 4, 2)),
                   static_cast<std::uint8_t>(b[at + 6]),
                   static_cast<std::uint8_t>(b[at + 7]),
                   get32(b, at + 8),
                   get32(b, at + 12)};
        if (r.name_len == 0 || r.name_len > kMaxIndexedName ||
            std::uint64_t{r.name_off} + r.name_len > pool_bytes) {
            return corrupt("name out of bounds");
        }
        const std::string_view name = pool.substr(r.name_off, r.name_len);
        if (name.find_first_of(std::string_view("/\0", 2)) != std::string_view::npos) {
            return corrupt("name contains '/' or NUL");
        }
        if (r.kind > static_cast<std::uint8_t>(fs::EntryKind::Other) || r.parent >= dc ||
            (r.dir != kNoIndex && (r.dir >= dc || r.dir == 0))) {
            return corrupt("entry record out of bounds");
        }
        // Names are concatenated, never shared: this bounds what loading can allocate.
        name_bytes += r.name_len;
        if (name_bytes > pool_bytes) {
            return corrupt("names overlap in the pool");
        }
        re[e] = r;
    }
    // Every entry lies in exactly one child range: its parent's.
    {
        std::vector<bool> covered(ec, false);
        for (std::size_t d = 0; d < dc; ++d) {
            for (std::size_t e = rd[d].first; e < std::size_t{rd[d].first} + rd[d].count; ++e) {
                if (covered[e] || re[e].parent != d) {
                    return corrupt("child ranges inconsistent");
                }
                covered[e] = true;
            }
        }
        if (std::find(covered.begin(), covered.end(), false) != covered.end()) {
            return corrupt("entry outside every child range");
        }
    }
    // Directory <-> entry links are a bijection and parents agree.
    for (std::size_t d = 1; d < dc; ++d) {
        const RawEntry& se = re[rd[d].self];
        if (se.dir != d || se.parent != rd[d].parent) {
            return corrupt("directory self entry mismatch");
        }
    }
    for (std::size_t e = 0; e < ec; ++e) {
        if (re[e].dir != kNoIndex && rd[re[e].dir].self != e) {
            return corrupt("entry directory link mismatch");
        }
    }
    // No cycles in parent links: every directory must reach the root.
    {
        std::vector<std::uint8_t> state(dc, 0); // 0 new, 1 on the current path, 2 reaches root
        state[0] = 2;
        std::vector<std::uint32_t> path;
        for (std::size_t d = 1; d < dc; ++d) {
            auto cur = static_cast<std::uint32_t>(d);
            path.clear();
            while (state[cur] == 0) {
                state[cur] = 1;
                path.push_back(cur);
                cur = rd[cur].parent;
            }
            if (state[cur] == 1) {
                return corrupt("cycle in parent links");
            }
            for (const std::uint32_t p : path) {
                state[p] = 2;
            }
        }
    }

    Tree tree;
    tree.dirs.resize(dc);
    tree.live_dirs = dc;
    tree.entries = ec;
    tree.scan_started_at = scan_started;
    tree.complete = (flags & kFileComplete) != 0;
    tree.truncated = (flags & kFileTruncated) != 0;
    std::string fold;
    for (std::size_t d = 0; d < dc; ++d) {
        const std::size_t at = static_cast<std::size_t>(dirs_offset) + (d * kDirRecordBytes);
        Dir& dir = tree.dirs[d];
        dir.alive = true;
        dir.parent = rd[d].parent;
        if (d != 0) {
            dir.name.assign(pool.substr(re[rd[d].self].name_off, re[rd[d].self].name_len));
        }
        dir.mtime_ns = static_cast<std::int64_t>(get64(b, at + 16));
        dir.device = get64(b, at + 24);
        dir.inode = get64(b, at + 32);
        dir.flags = get32(b, at + 40) & (kDirScanned | kDirUnreadable | kDirOtherFs);
        if (dir.device != 0 || dir.inode != 0) {
            tree.by_id.emplace(FileId{dir.device, dir.inode}, static_cast<std::uint32_t>(d));
        }
        dir.children.reserve(rd[d].count);
        for (std::size_t e = rd[d].first; e < std::size_t{rd[d].first} + rd[d].count; ++e) {
            const RawEntry& r = re[e];
            Child c;
            if (!append_name(dir.pool, c, pool.substr(r.name_off, r.name_len), fold)) {
                return corrupt("name cannot be indexed");
            }
            c.kind = static_cast<fs::EntryKind>(r.kind);
            c.flags = static_cast<std::uint8_t>(r.flags & (kEntryHidden | kEntryLinkToDir));
            c.dir = r.dir;
            dir.children.push_back(c);
        }
    }
    return tree;
}

// ---------------------------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------------------------

namespace {

[[nodiscard]] auto io_error(std::string_view what, const Path& p, int err) {
    const ErrorCode code = err == ENOENT                   ? ErrorCode::NotFound
                           : err == EACCES || err == EPERM ? ErrorCode::PermissionDenied
                           : err == ENOMEM                 ? ErrorCode::OutOfMemory
                                                           : ErrorCode::IoError;
    return fail(code, std::string(what) + " " + p.native() + ": " + std::strerror(err));
}

Result<void> make_dirs(const Path& dir) {
    Path cur;
    for (const Path& part : dir) {
        cur /= part;
        if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
            return io_error("mkdir", cur, errno);
        }
    }
    return ok();
}

} // namespace

Result<void> write_file_atomic(const Path& file, std::string_view bytes) {
    const Path dir = file.parent_path();
    if (Result<void> r = make_dirs(dir); !r) {
        return r;
    }
    std::string tmpl = file.native() + ".tmp-XXXXXX";
    const int fd = ::mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd < 0) {
        return io_error("mkstemp", file, errno);
    }
    const auto cleanup = [&](int err) {
        ::close(fd);
        ::unlink(tmpl.c_str());
        return io_error("write", Path(tmpl), err);
    };
    if (::fchmod(fd, 0600) != 0) {
        return cleanup(errno);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return cleanup(errno);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        return cleanup(errno);
    }
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmpl.c_str());
        return io_error("close", Path(tmpl), err);
    }
    if (::rename(tmpl.c_str(), file.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmpl.c_str());
        return io_error("rename", file, err);
    }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return ok();
}

Result<std::string> read_file(const Path& file) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return io_error("open", file, errno);
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return io_error("fstat", file, err);
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<std::uint64_t>(st.st_size) > kMaxIndexFileBytes) {
        ::close(fd);
        return fail(ErrorCode::InvalidArgument,
                    "index file corrupt: not a regular file or too large");
    }
    std::string out(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            return io_error("read", file, err);
        }
        if (n == 0) {
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    out.resize(done);
    return out;
}

} // namespace navigator::search::detail
