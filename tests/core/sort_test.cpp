// sort_entries (navigator/fs.hpp, "Sorting").
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/fs.hpp>

namespace fs = navigator::fs;
using fs::DirEntry;
using fs::EntryKind;
using fs::SortKey;
using fs::SortOptions;

namespace {

DirEntry make(std::string name, EntryKind kind = EntryKind::File, std::uint64_t size = 0,
              std::int64_t mtime_s = 0) {
    DirEntry e;
    e.name = std::move(name);
    e.kind = kind;
    e.size = size;
    e.mtime = navigator::Timestamp{std::chrono::seconds{mtime_s}};
    return e;
}

DirEntry link_to(std::string name, EntryKind target) {
    DirEntry e = make(std::move(name), EntryKind::Symlink);
    e.target_kind = target;
    return e;
}

std::vector<std::string> names(const std::vector<DirEntry>& v) {
    std::vector<std::string> out;
    out.reserve(v.size());
    for (const DirEntry& e : v) {
        out.push_back(e.name);
    }
    return out;
}

std::vector<std::string> sorted_names(std::vector<DirEntry> v, const SortOptions& o) {
    fs::sort_entries(v, o);
    return names(v);
}

SortOptions by(SortKey key, bool descending = false, bool folders_first = true) {
    SortOptions o;
    o.key = key;
    o.descending = descending;
    o.folders_first = folders_first;
    return o;
}

using Names = std::vector<std::string>;

} // namespace

TEST_CASE("sort by name: natural, case-insensitive, folders first", "[sort]") {
    const std::vector<DirEntry> v = {
        make("file10.txt"),
        make("File2.txt"),
        make("file1.txt"),
        make("b", EntryKind::Directory),
        make("A", EntryKind::Directory),
        make("file.txt"),
        make("fileA.txt"),
        link_to("linkdir", EntryKind::Directory),
        link_to("linkfile", EntryKind::File),
    };
    CHECK(sorted_names(v, by(SortKey::Name)) == Names{"A", "b", "linkdir", "file.txt", "file1.txt",
                                                      "File2.txt", "file10.txt", "fileA.txt",
                                                      "linkfile"});
    CHECK(sorted_names(v, by(SortKey::Name, false, false)) ==
          Names{"A", "b", "file.txt", "file1.txt", "File2.txt", "file10.txt", "fileA.txt",
                "linkdir", "linkfile"});
}

TEST_CASE("sort descending reverses within groups, folders stay first", "[sort]") {
    const std::vector<DirEntry> v = {
        make("a.txt"),
        make("c.txt"),
        make("b.txt"),
        make("dir1", EntryKind::Directory),
        make("dir2", EntryKind::Directory),
        link_to("dangling", EntryKind::Unknown),
    };
    CHECK(sorted_names(v, by(SortKey::Name, true)) ==
          Names{"dir2", "dir1", "dangling", "c.txt", "b.txt", "a.txt"});
    // Descending is the exact reverse of ascending within each group.
    const Names asc = sorted_names(v, by(SortKey::Name, false, false));
    Names desc = sorted_names(v, by(SortKey::Name, true, false));
    std::reverse(desc.begin(), desc.end());
    CHECK(asc == desc);
}

TEST_CASE("sort by size, ties by name then raw bytes", "[sort]") {
    const std::vector<DirEntry> v = {
        make("big", EntryKind::File, 1000), make("small", EntryKind::File, 1),
        make("Mid", EntryKind::File, 50),   make("mid", EntryKind::File, 50),
        make("mid2", EntryKind::File, 50),  make("zdir", EntryKind::Directory, 0),
    };
    CHECK(sorted_names(v, by(SortKey::Size)) ==
          Names{"zdir", "small", "Mid", "mid", "mid2", "big"});
    CHECK(sorted_names(v, by(SortKey::Size, true)) ==
          Names{"zdir", "big", "mid2", "mid", "Mid", "small"});
}

TEST_CASE("sort by modified", "[sort]") {
    const std::vector<DirEntry> v = {
        make("new", EntryKind::File, 0, 300),
        make("old", EntryKind::File, 0, 100),
        make("mid", EntryKind::File, 0, 200),
        make("old2", EntryKind::File, 0, 100),
    };
    CHECK(sorted_names(v, by(SortKey::Modified)) == Names{"old", "old2", "mid", "new"});
    CHECK(sorted_names(v, by(SortKey::Modified, true)) == Names{"new", "mid", "old2", "old"});
}

TEST_CASE("sort by extension (Type column)", "[sort]") {
    const std::vector<DirEntry> v = {
        make("b.txt"),    make("a.TXT"),   make("c.gz"),      make("noext"),    make(".bashrc"),
        make("x.tar.gz"), make("img.png"), make("img10.png"), make("img9.png"),
    };
    CHECK(sorted_names(v, by(SortKey::Extension)) == Names{".bashrc", "noext", "c.gz", "x.tar.gz",
                                                           "img.png", "img9.png", "img10.png",
                                                           "a.TXT", "b.txt"});
}

TEST_CASE("SortKey::Count currently sorts as Name", "[sort]") {
    const std::vector<DirEntry> v = {make("b"), make("a10"), make("a9"),
                                     make("d", EntryKind::Directory)};
    CHECK(sorted_names(v, by(SortKey::Count)) == sorted_names(v, by(SortKey::Name)));
    CHECK(sorted_names(v, by(SortKey::Count, true)) == sorted_names(v, by(SortKey::Name, true)));
}

TEST_CASE("raw byte tiebreak makes equivalent names deterministic", "[sort]") {
    // All equivalent under compare_names (case + Turkish I + leading zeros).
    const std::vector<DirEntry> v = {make("README"), make("readme"), make("ReadMe"), make("İx"),
                                     make("ix"),     make("Ix"),     make("ıx"),     make("a01"),
                                     make("a1"),     make("a001")};
    const Names expected = {"a001", "a01", "a1",     "Ix",     "ix",
                            "İx",   "ıx",  "README", "ReadMe", "readme"};
    CHECK(sorted_names(v, by(SortKey::Name)) == expected);
    Names rev = sorted_names(v, by(SortKey::Name, true));
    std::reverse(rev.begin(), rev.end());
    CHECK(rev == expected);
}

TEST_CASE("sort is a deterministic total order independent of input order", "[sort]") {
    std::vector<DirEntry> v;
    const char* bases[] = {"a", "A", "b", "İ", "ı", "I", "i", "z", "Ω", "ω", "é", "e\xCC\x81"};
    std::uint64_t n = 0;
    for (const char* base : bases) {
        for (const char* suffix : {"", "1", "01", "10", ".txt", "\xFF"}) {
            v.push_back(make(std::string(base) + suffix,
                             (n % 7 == 0) ? EntryKind::Directory : EntryKind::File, n % 5,
                             static_cast<std::int64_t>(n % 3)));
            ++n;
        }
    }
    for (const SortKey key :
         {SortKey::Name, SortKey::Size, SortKey::Modified, SortKey::Extension, SortKey::Count}) {
        for (const bool desc : {false, true}) {
            for (const bool ff : {false, true}) {
                for (const bool nat : {false, true}) {
                    for (const bool ci : {false, true}) {
                        SortOptions o = by(key, desc, ff);
                        o.natural = nat;
                        o.case_insensitive = ci;
                        const Names reference = sorted_names(v, o);
                        std::mt19937 rng(1234);
                        for (int round = 0; round < 5; ++round) {
                            std::vector<DirEntry> shuffled = v;
                            std::shuffle(shuffled.begin(), shuffled.end(), rng);
                            CHECK(sorted_names(shuffled, o) == reference);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("entries with metadata_error sort with default fields", "[sort]") {
    std::vector<DirEntry> v = {make("b", EntryKind::File, 10, 50), make("a", EntryKind::File, 5)};
    DirEntry broken;
    broken.name = "zz";
    broken.metadata_error = navigator::ErrorCode::PermissionDenied;
    v.push_back(broken);
    CHECK(sorted_names(v, by(SortKey::Size)) == Names{"zz", "a", "b"});
    CHECK(sorted_names(v, by(SortKey::Modified)) == Names{"a", "zz", "b"});
}

TEST_CASE("sort of empty and single spans", "[sort]") {
    std::vector<DirEntry> empty;
    fs::sort_entries(empty, SortOptions{});
    CHECK(empty.empty());
    std::vector<DirEntry> one = {make("x")};
    fs::sort_entries(one, SortOptions{});
    CHECK(one.front().name == "x");
}
