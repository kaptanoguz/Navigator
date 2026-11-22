// Index file format version 1 (navigator/index.hpp "File format"): an independent writer built
// from the documented layout, encoder/decoder round trip, and hostile input.
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/index.hpp>

#include "index_test_util.hpp"
#include "index_tree.hpp"

using namespace navigator;
using namespace navigator::search;
using namespace navigator::search::detail;

namespace {

VolumeIdentity ident() {
    VolumeIdentity id;
    id.uuid = "AbC-123";
    id.root = "/@home";
    id.device = "/dev/nvme0n1p3";
    id.mount_point = "/home";
    return id;
}

// Little-endian writer for hand-made files (deliberately independent of index_tree.cpp).
struct Bytes {
    std::string b;
    void u8(std::uint64_t v) { b.push_back(static_cast<char>(v & 0xFF)); }
    void u16(std::uint64_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(std::uint64_t v) {
        u16(v);
        u16(v >> 16);
    }
    void u64(std::uint64_t v) {
        u32(v);
        u32(v >> 32);
    }
    void str(std::string_view s) {
        u32(s.size());
        b.append(s);
    }
};

struct RawDir {
    std::uint32_t parent, self, first, count;
    std::int64_t mtime;
    std::uint32_t flags;
};
struct RawEntry {
    std::string name;
    std::uint8_t kind, flags;
    std::uint32_t parent, dir;
};

std::string handmade(const VolumeIdentity& id, const std::vector<RawDir>& dirs,
                     const std::vector<RawEntry>& entries, std::uint32_t flags = 1,
                     std::uint32_t version = kIndexFormatVersion) {
    Bytes vol;
    vol.str(index_stable_id(id));
    vol.str(id.uuid);
    vol.str(id.device);
    vol.str(id.mount_point.native());
    vol.str(id.root);
    std::uint64_t pool = 0;
    for (const RawEntry& e : entries) {
        pool += e.name.size();
    }
    const std::uint64_t dirs_off = (96 + vol.b.size() + 7) / 8 * 8;
    const std::uint64_t entries_off = dirs_off + (48 * dirs.size());
    const std::uint64_t pool_off = entries_off + (16 * entries.size());
    Bytes f;
    f.b.append(std::string_view("NAVIDX\0\n", 8));
    f.u32(version);
    f.u32(flags);
    f.u64(1'700'000'000'000'000'000ULL);
    f.u64(1'690'000'000'000'000'000ULL);
    f.u64(dirs.size());
    f.u64(entries.size());
    f.u64(pool);
    f.u64(dirs_off);
    f.u64(entries_off);
    f.u64(pool_off);
    f.u32(vol.b.size());
    f.u32(0);
    f.u64(0);
    f.b += vol.b;
    while (f.b.size() < dirs_off) {
        f.u8(0);
    }
    for (const RawDir& d : dirs) {
        f.u32(d.parent);
        f.u32(d.self);
        f.u32(d.first);
        f.u32(d.count);
        f.u64(static_cast<std::uint64_t>(d.mtime));
        f.u64(42);
        f.u64(1000 + d.first);
        f.u32(d.flags);
        f.u32(0);
    }
    std::uint64_t off = 0;
    for (const RawEntry& e : entries) {
        f.u32(off);
        f.u16(e.name.size());
        f.u8(e.kind);
        f.u8(e.flags);
        f.u32(e.parent);
        f.u32(e.dir);
        off += e.name.size();
    }
    for (const RawEntry& e : entries) {
        f.b += e.name;
    }
    f.u64(fnv1a64(f.b));
    return f.b;
}

void refresh_checksum(std::string& file) {
    const std::uint64_t h = fnv1a64(std::string_view(file).substr(0, file.size() - 8));
    for (std::size_t i = 0; i < 8; ++i) {
        file[file.size() - 8 + i] = static_cast<char>((h >> (8 * i)) & 0xFF);
    }
}

constexpr std::uint32_t kNone = 0xFFFFFFFFU;

} // namespace

TEST_CASE("fnv1a64 and stable ids", "[search][index][file]") {
    CHECK(fnv1a64("") == 0xcbf29ce484222325ULL);
    CHECK(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
    CHECK(fnv1a64("foobar") == 0x85944171f73967e8ULL);
    CHECK(index_stable_id(ident()).rfind("u-abc-123-", 0) == 0);
    VolumeIdentity plain = ident();
    plain.root = "/";
    CHECK(index_stable_id(plain) == "u-abc-123");
    plain.uuid.clear();
    CHECK(index_stable_id(plain).rfind("d-", 0) == 0);
    CHECK(index_stable_id(plain).size() == 18);
    CHECK(index_file("/c", plain).native() == "/c/index/" + index_stable_id(plain) + ".idx");
}

TEST_CASE("a hand-made version 1 file decodes", "[search][index][file]") {
    // root: "a" (dir 1), "b.txt"; dir 1 "a": ".hid"
    const std::vector<RawDir> dirs = {{kNone, kNone, 0, 2, 5, 1}, {0, 0, 2, 1, 7, 1}};
    const std::vector<RawEntry> entries = {
        {"a", 2, 0, 0, 1}, {"b.txt", 1, 0, 0, kNone}, {".hid", 1, 1, 1, kNone}};
    const std::string file = handmade(ident(), dirs, entries);
    auto tree = decode_index(file, ident());
    REQUIRE(tree);
    CHECK(tree->complete);
    CHECK_FALSE(tree->truncated);
    CHECK(tree->entries == 3);
    REQUIRE(tree->dirs.size() == 2);
    CHECK(tree->dirs[1].name == "a");
    CHECK(tree->dirs[1].mtime_ns == 7);
    CHECK(tree->scan_started_at == 1'690'000'000'000'000'000LL);
    REQUIRE(tree->dirs[1].children.size() == 1);
    CHECK(Tree::raw_name(tree->dirs[1], tree->dirs[1].children[0]) == ".hid");
    CHECK(tree->dirs[0].children[0].dir == 1);

    // Re-encoding gives a file that decodes to the same tree.
    auto again = encode_index(*tree, ident(), 0);
    REQUIRE(again);
    CHECK(again->size() == file.size());
    auto tree2 = decode_index(*again, ident());
    REQUIRE(tree2);
    CHECK(tree2->entries == 3);
    CHECK(tree2->dirs[1].name == "a");

    // Wrong identity.
    VolumeIdentity other = ident();
    other.mount_point = "/mnt";
    CHECK_FALSE(decode_index(file, other));
}

TEST_CASE("structurally invalid files are rejected", "[search][index][file]") {
    const VolumeIdentity id = ident();
    const auto rejects = [&](const std::vector<RawDir>& d, const std::vector<RawEntry>& e) {
        const auto r = decode_index(handmade(id, d, e), id);
        return !r && r.error().code == ErrorCode::InvalidArgument;
    };
    // Cycle in parent links: dir 1 <-> dir 2, consistent otherwise.
    CHECK(rejects({{kNone, kNone, 0, 0, 0, 1}, {2, 0, 1, 1, 0, 1}, {1, 1, 0, 1, 0, 1}},
                  {{"x", 2, 0, 2, 1}, {"y", 2, 0, 1, 2}}));
    // Entry outside its parent's child range.
    CHECK(rejects({{kNone, kNone, 0, 1, 0, 1}}, {{"x", 1, 0, 0, kNone}, {"y", 1, 0, 0, kNone}}));
    // Name with '/'.
    CHECK(rejects({{kNone, kNone, 0, 1, 0, 1}}, {{"a/b", 1, 0, 0, kNone}}));
    // Kind out of range.
    CHECK(rejects({{kNone, kNone, 0, 1, 0, 1}}, {{"a", 9, 0, 0, kNone}}));
    // Directory link that does not point back.
    CHECK(rejects({{kNone, kNone, 0, 2, 0, 1}, {0, 0, 2, 0, 0, 1}},
                  {{"a", 2, 0, 0, 1}, {"b", 2, 0, 0, 1}}));
    // Root with a parent.
    CHECK(rejects({{0, kNone, 0, 0, 0, 1}}, {}));
    // Unknown header flag bits.
    {
        const auto r = decode_index(handmade(id, {{kNone, kNone, 0, 0, 0, 1}}, {}, 0x80), id);
        CHECK_FALSE(r);
    }
    // Version 2.
    {
        const auto r = decode_index(handmade(id, {{kNone, kNone, 0, 0, 0, 1}}, {}, 1, 2), id);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("version") != std::string::npos);
    }
    // Empty root is valid.
    CHECK(decode_index(handmade(id, {{kNone, kNone, 0, 0, 0, 1}}, {}), id));
}

TEST_CASE("random corruption never crashes the decoder", "[search][index][file]") {
    navigator::test::TempDir tmp;
    navigator::test::generate_tree(tmp.path(), navigator::test::TreeSpec{2, 25, 4, 3, 0.1});
    // Build a real tree through the index, then fuzz its encoding.
    NameIndexOptions o;
    o.root = tmp.path();
    o.identity = ident();
    o.background_priority = false;
    o.rescan_interval = std::chrono::seconds{0};
    auto index = create_name_index(o);
    REQUIRE(index);
    (*index)->start();
    REQUIRE(navigator::test::wait_ready(**index));
    // Save through a file to get the encoded bytes.
    navigator::test::TempDir cache;
    o.file = cache.path() / "x.idx";
    {
        auto with_file = create_name_index(o);
        REQUIRE(with_file);
        (*with_file)->start();
        REQUIRE(navigator::test::wait_ready(**with_file));
    }
    auto bytes = read_file(o.file);
    REQUIRE(bytes);
    const std::string good = *bytes;
    REQUIRE(decode_index(good, ident()));

    std::mt19937 rng(1234);
    std::size_t accepted = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string bad = good;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < flips; ++k) {
            const std::size_t at = rng() % (bad.size() - 8);
            bad[at] = static_cast<char>(rng() & 0xFF);
        }
        if (i % 3 == 0) {
            bad.resize(rng() % bad.size());
        } else {
            refresh_checksum(bad); // get past the checksum: exercise structural validation
        }
        auto r = decode_index(bad, ident());
        if (r) {
            ++accepted; // e.g. a flipped mtime or name byte: still a well-formed file
        }
    }
    CHECK(accepted < 3000);
}
