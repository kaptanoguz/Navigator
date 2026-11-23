// tests/integration/search_support.hpp: Phase 5 helpers for navigator_browser_integration_tests
// (owner: qa). Real fs::Walker flattens, real create_name_index() indexes on fixture folders,
// and readers of the rows a BrowserModel shows. Nothing here touches anything outside the
// fixture directories the tests create (CLAUDE.md rule 9).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"

namespace navigator::integration_test {

inline constexpr std::chrono::milliseconds kSearchWait{15000};

// Names of the rows the view shows, in row order.
[[nodiscard]] inline std::vector<std::string> row_names(const ui::BrowserModel& m) {
    std::vector<std::string> out;
    out.reserve(m.row_count());
    for (std::size_t i = 0; i < m.row_count(); ++i) {
        out.push_back(m.row_entry(i).name);
    }
    return out;
}

// Relative path of a row ("parent/name", or "name" for children of the root).
[[nodiscard]] inline std::string row_rel(const ui::BrowserModel& m, std::size_t row) {
    const std::string parent(m.row_parent(row));
    return parent.empty() ? m.row_entry(row).name : parent + "/" + m.row_entry(row).name;
}

[[nodiscard]] inline std::vector<std::string> row_rels(const ui::BrowserModel& m) {
    std::vector<std::string> out;
    out.reserve(m.row_count());
    for (std::size_t i = 0; i < m.row_count(); ++i) {
        out.push_back(row_rel(m, i));
    }
    return out;
}

// Name of the row under the cursor ("" when none).
[[nodiscard]] inline std::string row_cursor_name(const ui::BrowserModel& m) {
    const fs::DirEntry* e = m.cursor_entry();
    return e != nullptr ? e->name : std::string();
}

[[nodiscard]] inline std::set<std::string> row_rel_set(const ui::BrowserModel& m) {
    const std::vector<std::string> rels = row_rels(m);
    return {rels.begin(), rels.end()};
}

// Pumps frame_start() until `done` or the timeout.
inline bool pump(ui::BrowserModel& m, const std::function<bool()>& done,
                 std::chrono::milliseconds timeout = kSearchWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        (void)m.frame_start();
        if (done()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Waits until `done(status)` or the timeout; the status is polled (any thread may call it).
inline bool wait_index(const search::IIndex& index,
                       const std::function<bool(const search::IndexStatus&)>& done,
                       std::chrono::milliseconds timeout = kSearchWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (done(index.status())) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

inline bool wait_ready(const search::IIndex& index,
                       std::chrono::milliseconds timeout = kSearchWait) {
    return wait_index(
        index, [](const search::IndexStatus& s) { return s.state == search::IndexState::Ready; },
        timeout);
}

// A name index of `root` (in memory unless `file` is given), no periodic passes.
[[nodiscard]] inline search::NameIndexOptions index_options(const Path& root, Path file = {},
                                                            WatcherFactory watchers = {}) {
    search::NameIndexOptions o;
    o.root = root;
    o.identity.uuid = "qa-fixture";
    o.identity.root = "/";
    o.identity.mount_point = root;
    o.file = std::move(file);
    o.watcher_factory = std::move(watchers);
    o.rescan_interval = std::chrono::seconds(0);
    o.background_priority = false;
    return o;
}

// Created, started and Ready (REQUIREd).
[[nodiscard]] inline std::shared_ptr<search::IIndex> ready_index(search::NameIndexOptions o) {
    Result<std::unique_ptr<search::IIndex>> made = search::create_name_index(std::move(o));
    REQUIRE(made.has_value());
    std::shared_ptr<search::IIndex> index = std::move(made).value();
    index->start();
    REQUIRE(wait_ready(*index));
    return index;
}

// Every hit of an index query (no limit), as relative paths, in result order.
[[nodiscard]] inline std::vector<std::string> query_rels(const search::IIndex& index,
                                                         const search::IndexQuery& q) {
    Result<search::IndexResult> r = index.query(q, {});
    REQUIRE(r.has_value());
    std::vector<std::string> out;
    const Path under = q.under.empty() ? index.root() : q.under;
    for (const search::IndexHit& hit : r.value().hits) {
        out.push_back(hit.path.lexically_relative(under).string());
    }
    return out;
}

// One headless BrowserModel on a fixture folder: recording launcher, optional watcher, the
// real fs::Walker unless a walk factory is given.
struct View {
    NoSpawnLauncher launcher;
    std::unique_ptr<ui::BrowserModel> model;

    explicit View(const Path& dir, ui::BrowserModel::Options o = {}) {
        o.launcher = &launcher;
        model = std::make_unique<ui::BrowserModel>(std::move(o));
        model->open_start(dir);
        REQUIRE(model->wait_listing(kBrowserWait));
        (void)model->frame_start();
        REQUIRE(model->loaded());
        REQUIRE(model->settle_search(kSearchWait));
    }
    [[nodiscard]] ui::BrowserModel& m() const { return *model; }
    // Flattens (walk, or index when given) and waits for the walk / query to finish.
    void flatten(const Path& root, std::shared_ptr<search::IIndex> index = nullptr,
                 bool volume = false) const {
        model->flatten(ui::FlattenRequest{root, volume || index != nullptr, std::move(index)});
        REQUIRE(model->flattened());
        REQUIRE(model->settle_search(kSearchWait));
    }
    // Sets the match mode of the current view.
    void mode(search::MatchMode want) const {
        if (model->match_mode() != want) {
            model->toggle_match_mode();
        }
        REQUIRE(model->match_mode() == want);
    }
    void query(std::string_view text) const {
        model->set_filter_text(text);
        REQUIRE(model->settle_search(kSearchWait));
    }
};

} // namespace navigator::integration_test
