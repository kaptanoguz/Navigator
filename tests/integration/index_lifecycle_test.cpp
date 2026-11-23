// Phase 5 name-index lifecycle end to end (owner: qa): create_name_index() with a file in a
// temporary cache dir and the real inotify backend.
//   - cold scan -> stop (saved, 0700 dir / 0600 file, valid trailer) -> changes made while
//     stopped -> restart: Loading -> Verifying -> Ready from the file, and the changes show;
//   - corrupt, truncated, foreign-version and other-volume files are rejected and rebuilt;
//   - live inotify updates reach an index-backed flattened view (Ctrl+Shift+E).
// Temporary fixture directories only (tree and cache dir).
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"
#include "search_support.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using Rels = std::set<std::string>;

namespace {

std::vector<char> read_file(const Path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const Path& p, const std::vector<char>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Magic, version and the FNV-1a trailer of index.hpp "File format".
bool valid_index_file(const Path& p) {
    const std::vector<char> b = read_file(p);
    if (b.size() < 96 + 8 || std::string(b.data(), 8) != std::string("NAVIDX\0\n", 8)) {
        return false;
    }
    std::uint32_t version = 0;
    for (int i = 3; i >= 0; --i) {
        version = (version << 8U) | static_cast<std::uint8_t>(b[8 + static_cast<std::size_t>(i)]);
    }
    std::uint64_t trailer = 0;
    for (int i = 7; i >= 0; --i) {
        trailer = (trailer << 8U) |
                  static_cast<std::uint8_t>(b[b.size() - 8 + static_cast<std::size_t>(i)]);
    }
    return version == search::kIndexFormatVersion &&
           trailer == search::fnv1a64(std::string_view(b.data(), b.size() - 8));
}

mode_t mode_of(const Path& p) {
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

struct Fixture {
    TempDir tree;
    TempDir cache;
    Fixture() {
        for (const char* d : {"docs", "docs/old", "music", ".config"}) {
            std::filesystem::create_directories(tree / d);
        }
        for (const char* f :
             {"docs/a.txt", "docs/old/b.txt", "music/song.mp3", "top.md", ".config/rc"}) {
            REQUIRE(touch(tree / f));
        }
    }
    [[nodiscard]] Path file() const {
        return search::index_file(cache.path(), index_options(tree.path()).identity);
    }
    [[nodiscard]] search::NameIndexOptions options(bool watch = false) const {
        return index_options(tree.path(), file(),
                             watch ? WatcherFactory([] { return create_inotify_watcher(); })
                                   : WatcherFactory{});
    }
    [[nodiscard]] Rels all(const search::IIndex& index) const {
        search::IndexQuery q;
        q.show_hidden = true;
        q.limit = 0;
        const std::vector<std::string> rels = query_rels(index, q);
        return {rels.begin(), rels.end()};
    }
};

std::shared_ptr<search::IIndex> started(search::NameIndexOptions o) {
    Result<std::unique_ptr<search::IIndex>> made = search::create_name_index(std::move(o));
    REQUIRE(made.has_value());
    std::shared_ptr<search::IIndex> index = std::move(made).value();
    index->start();
    return index;
}

const Rels kInitial{"docs",           "docs/a.txt", "docs/old", "docs/old/b.txt", "music",
                    "music/song.mp3", "top.md",     ".config",  ".config/rc"};

} // namespace

TEST_CASE("Index: cold scan, save, restart from the file, verify picks up offline changes",
          "[index][lifecycle][e2e]") {
    const Fixture fx;
    {
        const std::shared_ptr<search::IIndex> index = started(fx.options());
        REQUIRE(wait_ready(*index));
        const search::IndexStatus s = index->status();
        CHECK_FALSE(s.loaded_from_file);
        CHECK(s.entries == kInitial.size());
        CHECK(fx.all(*index) == kInitial);
        index->stop();
        CHECK(index->status().state == search::IndexState::Stopped);
    }
    REQUIRE(std::filesystem::exists(fx.file()));
    CHECK(valid_index_file(fx.file()));
    CHECK(mode_of(fx.file()) == 0600);
    CHECK(mode_of(search::index_dir(fx.cache.path())) == 0700);
    // Only the index file, no temporaries.
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(search::index_dir(fx.cache.path()))) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);

    // While Navigator is not running: add, delete, rename a folder, create a nested folder.
    REQUIRE(touch(fx.tree / "docs/new.txt"));
    std::filesystem::remove(fx.tree / "top.md");
    std::filesystem::rename(fx.tree / "docs/old", fx.tree / "docs/archive");
    std::filesystem::create_directories(fx.tree / "music/live/2024");
    REQUIRE(touch(fx.tree / "music/live/2024/encore.flac"));
    const Rels changed{"docs",
                       "docs/a.txt",
                       "docs/new.txt",
                       "docs/archive",
                       "docs/archive/b.txt",
                       "music",
                       "music/song.mp3",
                       "music/live",
                       "music/live/2024",
                       "music/live/2024/encore.flac",
                       ".config",
                       ".config/rc"};

    const std::shared_ptr<search::IIndex> again = started(fx.options());
    bool saw_verifying = false;
    REQUIRE(wait_index(*again, [&](const search::IndexStatus& s) {
        saw_verifying = saw_verifying || s.state == search::IndexState::Verifying;
        return s.state == search::IndexState::Ready;
    }));
    const search::IndexStatus s = again->status();
    CHECK(s.loaded_from_file);
    CHECK_FALSE(s.last_error.has_value());
    CHECK(fx.all(*again) == changed);
    CHECK(s.entries == changed.size());
    INFO("Verifying observed: " << saw_verifying);
    again->stop();
    CHECK(valid_index_file(fx.file()));
    // A third start loads the verified state and needs no change.
    const std::shared_ptr<search::IIndex> third = started(fx.options());
    REQUIRE(wait_ready(*third));
    CHECK(third->status().loaded_from_file);
    CHECK(fx.all(*third) == changed);
    third->stop();
}

TEST_CASE("Index: a damaged or foreign file is rejected and rebuilt", "[index][lifecycle][e2e]") {
    const Fixture fx;
    {
        const std::shared_ptr<search::IIndex> index = started(fx.options());
        REQUIRE(wait_ready(*index));
        index->stop();
    }
    const std::vector<char> good = read_file(fx.file());
    REQUIRE(good.size() > 200);
    struct Damage {
        const char* what;
        std::vector<char> bytes;
    };
    std::vector<Damage> damages;
    std::vector<char> b = good;
    b[good.size() / 2] = static_cast<char>(b[good.size() / 2] ^ 0x5A);
    damages.push_back({"one flipped byte (checksum)", b});
    damages.push_back({"truncated", std::vector<char>(good.begin(), good.begin() + 120)});
    damages.push_back({"empty", {}});
    damages.push_back({"garbage", std::vector<char>(4096, '\x7F')});
    b = good;
    b[8] = 2; // version 2
    damages.push_back({"other format version", b});
    b = good;
    b[0] = 'X';
    damages.push_back({"bad magic", b});
    for (const Damage& d : damages) {
        INFO(d.what);
        write_file(fx.file(), d.bytes);
        const std::shared_ptr<search::IIndex> index = started(fx.options());
        REQUIRE(wait_ready(*index));
        const search::IndexStatus s = index->status();
        CHECK_FALSE(s.loaded_from_file);
        CHECK(s.last_error.has_value());
        CHECK(fx.all(*index) == kInitial);
        index->stop();
        CHECK(valid_index_file(fx.file())); // rewritten after the rebuild
    }
    // The same file for another volume (identity differs): rebuilt, never mixed.
    search::NameIndexOptions other = fx.options();
    other.identity.uuid = "another-volume";
    other.file = fx.file(); // forced onto the same path
    const std::shared_ptr<search::IIndex> index = started(other);
    REQUIRE(wait_ready(*index));
    CHECK_FALSE(index->status().loaded_from_file);
    CHECK(fx.all(*index) == kInitial);
    index->stop();
}

TEST_CASE("Index: live inotify updates show in an index-backed flattened view",
          "[index][lifecycle][watch][e2e]") {
    const Fixture fx;
    const std::shared_ptr<search::IIndex> index = started(fx.options(true));
    REQUIRE(wait_ready(*index));
    REQUIRE(wait_index(*index, [](const search::IndexStatus& s) { return s.live; }));
    ui::BrowserModel::Options o;
    o.index_debounce = std::chrono::milliseconds(0);
    const View v(fx.tree.path(), std::move(o));
    v.flatten(fx.tree.path(), index);
    REQUIRE(v.m().flatten_progress().from_index);
    v.query("live");
    CHECK(v.m().row_count() == 0);

    const auto has = [&](const std::string& rel) { return row_rel_set(v.m()).contains(rel); };
    // Created file, created folder with a file inside (listed and watched), rename, delete.
    REQUIRE(touch(fx.tree / "docs/live_note.txt"));
    CHECK(pump(v.m(), [&] { return has("docs/live_note.txt"); }));
    std::filesystem::create_directories(fx.tree / "livedir");
    REQUIRE(touch(fx.tree / "livedir/inside_live.txt"));
    CHECK(pump(v.m(), [&] { return has("livedir") && has("livedir/inside_live.txt"); }));
    REQUIRE(touch(fx.tree / "livedir/second_live.txt")); // the new folder is watched too
    CHECK(pump(v.m(), [&] { return has("livedir/second_live.txt"); }));
    std::filesystem::rename(fx.tree / "docs/live_note.txt", fx.tree / "docs/live_renamed.txt");
    CHECK(pump(v.m(), [&] { return has("docs/live_renamed.txt") && !has("docs/live_note.txt"); }));
    std::filesystem::remove_all(fx.tree / "livedir");
    CHECK(pump(v.m(), [&] { return !has("livedir") && !has("livedir/inside_live.txt"); }));
    // A hidden file created live stays out unless show_hidden.
    REQUIRE(touch(fx.tree / ".live_hidden"));
    REQUIRE(wait_index(*index, [&](const search::IndexStatus&) {
        return fx.all(*index).contains(".live_hidden");
    }));
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK_FALSE(has(".live_hidden"));
    CHECK(row_rels(v.m()) == std::vector<std::string>{"docs/live_renamed.txt"});
    index->stop();
    // Saved on stop with the live changes.
    const std::shared_ptr<search::IIndex> again = started(fx.options());
    REQUIRE(wait_ready(*again));
    CHECK(fx.all(*again).contains("docs/live_renamed.txt"));
    CHECK_FALSE(fx.all(*again).contains("livedir"));
    again->stop();
}
