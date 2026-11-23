// tests/search/index_test_util.hpp: generated trees, a brute-force reference search, a fake
// IWatcher and wait helpers for the name index tests.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>

#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/watch.hpp>

#include "temp_dir.hpp"

namespace navigator::test {

// ---------------------------------------------------------------------------------------------
// Generated trees
// ---------------------------------------------------------------------------------------------

struct TreeSpec {
    std::uint32_t seed = 1;
    std::size_t dirs = 60;         // directories to create below the root
    std::size_t files_per_dir = 6; // average
    std::size_t max_depth = 5;
    double hidden_dir_ratio = 0.08; // share of hidden directories
};

inline void touch(const std::filesystem::path& p) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ::close(fd);
    }
}

// Mixes ASCII, Turkish (İ ı ş ç ğ), NFD forms and assorted extensions. Returns the relative
// directory paths it created.
inline std::vector<std::string> generate_tree(const std::filesystem::path& root,
                                              const TreeSpec& spec) {
    static const std::vector<std::string> kStems = {
        "report", "Rapor", "İstanbul", "ıslak", "çiçek", "notes",     "e\xcc\x81te",
        "data",   "Foto",  "README",   "src",   "build", "Kayıt",     "photo2024",
        "myFile", "a_b-c", "Şehir",    "ğüz",   "index", "unreported"};
    static const std::vector<std::string> kExt = {".txt", ".md",     ".JPG", ".png",
                                                  "",     ".tar.gz", ".cpp", ".PDF"};
    std::mt19937 rng(spec.seed);
    const auto pick = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };
    std::vector<std::string> dirs{""};
    std::vector<std::size_t> depth{0};
    std::size_t counter = 0;
    while (dirs.size() < spec.dirs + 1) {
        std::size_t parent = pick(dirs.size());
        if (depth[parent] >= spec.max_depth) {
            continue;
        }
        std::string name = kStems[pick(kStems.size())] + "_d" + std::to_string(counter++);
        if (std::uniform_real_distribution<double>(0, 1)(rng) < spec.hidden_dir_ratio) {
            name.insert(0, ".");
        }
        const std::string rel = dirs[parent].empty() ? name : dirs[parent] + "/" + name;
        std::filesystem::create_directory(root / rel);
        dirs.push_back(rel);
        depth.push_back(depth[parent] + 1);
    }
    for (const std::string& d : dirs) {
        const std::size_t n = pick(spec.files_per_dir * 2 + 1);
        for (std::size_t i = 0; i < n; ++i) {
            std::string name =
                kStems[pick(kStems.size())] + std::to_string(counter++) + kExt[pick(kExt.size())];
            if (pick(12) == 0) {
                name.insert(0, ".");
            }
            touch(d.empty() ? root / name : root / d / name);
        }
        if (pick(10) == 0) {
            const std::filesystem::path base = d.empty() ? root : root / d;
            std::error_code ec;
            std::filesystem::create_symlink("..", base / ("up_link" + std::to_string(counter++)),
                                            ec);
        }
    }
    return dirs;
}

// Sets every directory's mtime (root included) to `age` in the past.
inline void age_directories(const std::filesystem::path& root, std::chrono::seconds age) {
    const auto set = [&](const std::filesystem::path& p) {
        struct timespec ts[2];
        ::clock_gettime(CLOCK_REALTIME, &ts[0]);
        ts[0].tv_sec -= static_cast<time_t>(age.count());
        ts[1] = ts[0];
        ::utimensat(AT_FDCWD, p.c_str(), ts, AT_SYMLINK_NOFOLLOW);
    };
    std::vector<std::filesystem::path> dirs{root};
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            dirs.push_back(it->path());
        }
    }
    // Deepest first is not needed: utimensat on a directory does not touch its parent.
    for (const auto& d : dirs) {
        set(d);
    }
}

// ---------------------------------------------------------------------------------------------
// Brute-force reference
// ---------------------------------------------------------------------------------------------

struct RefEntry {
    std::string parent; // relative to the search root
    std::string name;
    std::uint32_t depth = 1;
    bool hidden_chain = false; // the entry or a directory between it and the root is hidden
    bool directory = false;
    bool link_to_directory = false; // a symlink whose target is a folder: a Folder for types
};

inline std::vector<RefEntry> reference_entries(const std::filesystem::path& root) {
    std::vector<RefEntry> out;
    struct Item {
        std::filesystem::path path;
        std::string rel;
        std::uint32_t depth;
        bool hidden;
    };
    std::vector<Item> stack{{root, "", 0, false}};
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(it.path, ec)) {
            RefEntry e;
            e.parent = it.rel;
            e.name = de.path().filename().native();
            e.depth = it.depth + 1;
            e.hidden_chain = it.hidden || fs::is_hidden_name(e.name);
            std::error_code ec2;
            const bool link = de.is_symlink(ec2);
            e.directory = !link && de.is_directory(ec2);
            e.link_to_directory = link && de.is_directory(ec2); // follows the link

            if (e.directory) {
                stack.push_back(Item{de.path(), it.rel.empty() ? e.name : it.rel + "/" + e.name,
                                     e.depth, e.hidden_chain});
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

struct RefHit {
    std::string path; // absolute
    std::int32_t score = 0;
};

inline std::vector<RefHit> reference_query(const std::filesystem::path& root,
                                           const std::vector<RefEntry>& entries,
                                           const search::Query& query, bool show_hidden,
                                           const search::TypeFilter& types = {}) {
    struct Cand {
        const RefEntry* e;
        std::int32_t score;
    };
    std::vector<Cand> cands;
    search::Matcher m(query);
    for (const RefEntry& e : entries) {
        if (!show_hidden && e.hidden_chain) {
            continue;
        }
        fs::DirEntry de;
        de.name = e.name;
        de.kind = e.directory           ? fs::EntryKind::Directory
                  : e.link_to_directory ? fs::EntryKind::Symlink
                                        : fs::EntryKind::File;
        de.target_kind = e.link_to_directory ? fs::EntryKind::Directory : fs::EntryKind::Unknown;
        if (!search::allows(types, de)) {
            continue;
        }
        if (const auto s = m.score(e.name)) {
            cands.push_back(Cand{&e, *s});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return search::ranks_before(search::RankInfo{a.score, a.e->depth, a.e->name, a.e->parent},
                                    search::RankInfo{b.score, b.e->depth, b.e->name, b.e->parent});
    });
    std::vector<RefHit> out;
    for (const Cand& c : cands) {
        const std::filesystem::path p =
            c.e->parent.empty() ? root / c.e->name : root / c.e->parent / c.e->name;
        out.push_back(RefHit{p.native(), c.score});
    }
    return out;
}

inline std::vector<RefHit> hits_of(const search::IndexResult& r) {
    std::vector<RefHit> out;
    for (const search::IndexHit& h : r.hits) {
        out.push_back(RefHit{h.path.native(), h.match.score});
    }
    return out;
}

inline bool same_hits(const std::vector<RefHit>& a, const std::vector<RefHit>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path || a[i].score != b[i].score) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------------------------

inline bool wait_until(const std::function<bool()>& pred,
                       std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

inline bool wait_ready(const search::IIndex& index,
                       std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    return wait_until([&] { return index.status().state == search::IndexState::Ready; }, timeout);
}

inline search::IndexResult run_query(const search::IIndex& index, std::string_view text,
                                     search::MatchMode mode = search::MatchMode::Substring,
                                     bool show_hidden = false, Path under = {},
                                     search::TypeFilter types = {}) {
    search::IndexQuery q;
    q.query = search::compile_query(text, mode);
    q.show_hidden = show_hidden;
    q.limit = 0;
    q.under = std::move(under);
    q.types = std::move(types);
    auto r = index.query(q, {});
    REQUIRE(r);
    return std::move(*r);
}

inline bool index_has(const search::IIndex& index, const std::filesystem::path& p) {
    const search::IndexResult r =
        run_query(index, p.filename().native(), search::MatchMode::Substring, true);
    return std::any_of(r.hits.begin(), r.hits.end(),
                       [&](const search::IndexHit& h) { return h.path == p; });
}

// ---------------------------------------------------------------------------------------------
// Fake watcher
// ---------------------------------------------------------------------------------------------

struct FakeHub {
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, WatchHandle> by_path;
    std::map<WatchHandle, std::string> paths;
    std::vector<WatchEvent> queue;
    WatchHandle next = 1;
    std::size_t adds = 0;
    std::size_t fail_after = std::numeric_limits<std::size_t>::max();

    bool emit(const std::filesystem::path& dir, WatchEventKind kind, std::string name,
              std::string old_name = {}) {
        const std::lock_guard lock(mu);
        const auto it = by_path.find(dir.native());
        if (it == by_path.end()) {
            return false;
        }
        queue.push_back(WatchEvent{kind, it->second, std::move(name), std::move(old_name), false});
        cv.notify_all();
        return true;
    }
    void overflow() {
        const std::lock_guard lock(mu);
        queue.push_back(WatchEvent{WatchEventKind::Overflow, kInvalidWatchHandle, {}, {}, false});
        cv.notify_all();
    }
    bool watched(const std::filesystem::path& dir) {
        const std::lock_guard lock(mu);
        return by_path.contains(dir.native());
    }
    std::size_t watch_count() {
        const std::lock_guard lock(mu);
        return by_path.size();
    }
};

class FakeWatcher final : public IWatcher {
public:
    explicit FakeWatcher(std::shared_ptr<FakeHub> hub) : hub_(std::move(hub)) {}
    Result<WatchHandle> add(const Path& dir) override {
        const std::lock_guard lock(hub_->mu);
        if (hub_->adds >= hub_->fail_after) {
            return fail(ErrorCode::PlatformError, "fake: fs.inotify.max_user_watches reached");
        }
        ++hub_->adds;
        const auto it = hub_->by_path.find(dir.native());
        if (it != hub_->by_path.end()) {
            return it->second;
        }
        const WatchHandle h = hub_->next++;
        hub_->by_path.emplace(dir.native(), h);
        hub_->paths.emplace(h, dir.native());
        return h;
    }
    void remove(WatchHandle handle) noexcept override {
        const std::lock_guard lock(hub_->mu);
        const auto it = hub_->paths.find(handle);
        if (it != hub_->paths.end()) {
            hub_->by_path.erase(it->second);
            hub_->paths.erase(it);
        }
    }
    Result<std::size_t> poll(std::vector<WatchEvent>& out) override {
        const std::lock_guard lock(hub_->mu);
        const std::size_t n = hub_->queue.size();
        for (WatchEvent& e : hub_->queue) {
            out.push_back(std::move(e));
        }
        hub_->queue.clear();
        return n;
    }
    bool wait(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(hub_->mu);
        return hub_->cv.wait_for(lock, timeout, [&] { return !hub_->queue.empty(); });
    }
    int native_handle() const noexcept override { return -1; }

private:
    std::shared_ptr<FakeHub> hub_;
};

inline WatcherFactory fake_factory(const std::shared_ptr<FakeHub>& hub) {
    return [hub]() -> Result<std::unique_ptr<IWatcher>> {
        return std::unique_ptr<IWatcher>(std::make_unique<FakeWatcher>(hub));
    };
}

inline VolumeIdentity test_identity(const std::filesystem::path& root) {
    VolumeIdentity id;
    id.uuid = "1234-ABCD";
    id.root = "/";
    id.device = "/dev/test0";
    id.mount_point = root;
    return id;
}

} // namespace navigator::test
